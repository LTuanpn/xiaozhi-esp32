#include "notify_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <esp_log.h>

#include "board.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace {
constexpr int kHttpTimeoutMs = 5000;
constexpr size_t kHttpReadBufferSize = 1024;
constexpr uint32_t kNotifyTaskStackSize = 6144;
constexpr UBaseType_t kNotifyTaskPriority = 2;
const char* TAG = "NotifyPlayer";

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}
}  // namespace

NotifyPlayer::NotifyPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

NotifyPlayer::~NotifyPlayer() { Stop(); }

bool NotifyPlayer::Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                         uint32_t playback_id, SubtitleCallback subtitle_callback,
                         FinishedCallback finished_callback, std::string_view local_audio, bool music) {
    if (playback_id == 0 || (local_audio.empty() && !IsSupportedUrl(audio_url))) {
        return false;
    }

    std::stable_sort(subtitles.begin(), subtitles.end(),
                     [](const NotifySubtitle& left, const NotifySubtitle& right) {
                         return left.start_ms < right.start_ms;
                     });

    bool create_worker = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) {
            return false;
        }
        audio_url_ = std::move(audio_url);
        local_audio_ = local_audio;
        music_ = music;
        audio_generation_ = audio_service_.GetPlaybackGeneration();
        create_worker = !worker_running_;
        subtitles_ = std::move(subtitles);
        displayed_text_.clear();
        subtitle_callback_ = std::move(subtitle_callback);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        last_playback_position_ms_ = 0;
        underrun_count_ = 0;
        next_subtitle_index_ = 0;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        stream_started_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
    }

    if (!create_worker)
        return true;  // Existing worker will pick up the latest request after cleanup.
    BaseType_t created = xTaskCreate(WorkerEntry, "notify_http", kNotifyTaskStackSize, this,
                                     kNotifyTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create notification HTTP task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void NotifyPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    subtitles_.clear();
    subtitle_callback_ = nullptr;
    finished_callback_ = nullptr;
}

bool NotifyPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool NotifyPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

void NotifyPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    SubtitleCallback callback;
    std::string text;
    bool subtitle_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || playback_id != playback_id_) {
            return;
        }
        last_playback_position_ms_ = media_position_ms;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= media_position_ms) {
            text = subtitles_[next_subtitle_index_].text;
            ++next_subtitle_index_;
            subtitle_changed = true;
        }
        if (!subtitle_changed || text == displayed_text_) {
            return;
        }
        displayed_text_ = text;
        callback = subtitle_callback_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    uint32_t underrun_count = 0;
    uint32_t media_position_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        } else if (stream_started_) {
            underrun_count = ++underrun_count_;
            media_position_ms = last_playback_position_ms_;
        }
    }
    if (underrun_count != 0) {
        ESP_LOGW(TAG, "Notification playback underrun #%lu at %lu ms",
                 static_cast<unsigned long>(underrun_count),
                 static_cast<unsigned long>(media_position_ms));
    }
    if (callback) {
        callback(playback_id, true);
    }
}

void NotifyPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<NotifyPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void NotifyPlayer::WorkerTask() {
    while (true) {
        std::string audio_url;
        uint32_t playback_id = 0;
        uint32_t audio_generation = 0;
        std::string_view local_audio;
        bool music;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_url = audio_url_;
            local_audio = local_audio_;
            music = music_;
            audio_generation = audio_generation_;
            playback_id = playback_id_;
        }

        bool success = false;
        auto demuxer = std::make_unique<OggDemuxer>();
        uint32_t media_position_ms = 0;
        bool packet_error = false;
        const auto output_kind = !local_audio.empty() ? AudioOutputKind::System :
                                 music ? AudioOutputKind::Music : AudioOutputKind::Voice;
        demuxer->OnPacket([this, playback_id, audio_generation, output_kind, &media_position_ms, &packet_error](
                              const uint8_t* data, int rate, int duration_ms, size_t size) {
            if (packet_error || IsCancelled(playback_id)) {
                packet_error = true;
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = rate;
            packet->frame_duration = duration_ms;
            packet->playback_id = playback_id;
            packet->media_position_ms = media_position_ms;
            packet->output_kind = output_kind;
            packet->payload.assign(data, data + size);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_ || !active_ || playback_id != playback_id_) {
                    packet_error = true;
                    return;
                }
                playback_drained_ = false;
            }
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet), true,
                                                        audio_generation)) {
                packet_error = true;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (playback_id == playback_id_)
                    stream_started_ = true;
            }
            media_position_ms += duration_ms;
        });
        if (!local_audio.empty()) {
            for (size_t offset = 0;
                 offset < local_audio.size() && !packet_error && !IsCancelled(playback_id);) {
                const size_t count = std::min(kHttpReadBufferSize, local_audio.size() - offset);
                demuxer->Process(reinterpret_cast<const uint8_t*>(local_audio.data() + offset),
                                 count);
                offset += count;
                packet_error = packet_error || demuxer->HasError();
            }
            success = !packet_error && !IsCancelled(playback_id) && demuxer->Finish();
        } else if (!IsCancelled(playback_id)) {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            if (http) {
                http->SetTimeout(kHttpTimeoutMs);
                http->SetHeader("Accept", "audio/ogg, application/ogg");
                http->SetHeader("Accept-Encoding", "identity");
                auto opened = http->Open("GET", audio_url);
                if (opened) {
                    auto status = http->GetStatusCode();
                    if (status && *status >= 200 && *status < 300) {
                        std::array<char, kHttpReadBufferSize> buffer;
                        while (!packet_error && !IsCancelled(playback_id)) {
                            auto size = http->Read(buffer.data(), buffer.size());
                            if (!size) {
                                ESP_LOGW(TAG, "Playback HTTP read failed");
                                break;
                            }
                            if (*size == 0) {
                                success = demuxer->Finish();
                                break;
                            }
                            demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()),
                                             *size);
                            packet_error = packet_error || demuxer->HasError();
                        }
                    } else {
                        ESP_LOGW(TAG, "Playback HTTP rejected or missing status");
                    }
                } else {
                    ESP_LOGW(TAG, "Playback HTTP connection failed");
                }
                http->Close();
            }
        }

        FinishedCallback callback;
        uint32_t completed_playback_id = 0;
        bool report_success = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && playback_id != playback_id_)
                continue;
            worker_running_ = false;
            task_handle_ = nullptr;
            if (!cancelled_ && active_ && playback_id == playback_id_) {
                if (!success) {
                    callback = CompleteLocked(completed_playback_id);
                } else {
                    http_finished_ = true;
                    if (playback_drained_) {
                        callback = CompleteLocked(completed_playback_id);
                        report_success = true;
                    }
                }
            }
        }
        if (callback) {
            callback(completed_playback_id, report_success);
        }
        return;
    }
}
