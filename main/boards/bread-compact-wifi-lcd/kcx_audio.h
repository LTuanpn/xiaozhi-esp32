#pragma once

#ifdef CONFIG_BREAD_COMPACT_LCD_KCX

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "codecs/no_audio_codec.h"

class KcxAudio {
public:
    explicit KcxAudio() = default;
    void Restore();
    bool Power(bool enabled);
    bool Request(std::string_view action, std::string_view target = {});
    void SetRoutes(bool voice, bool music);
    bool UseBluetooth(AudioOutputKind kind) const;
    cJSON* Status();

private:
    struct Device {
        std::string mac, name;
    };
    void Run();
    void Transmit(const std::string& command);
    void ReadLine(std::string_view line);
    void ConfigurePower(bool enabled);
    bool EnsureWorker();

    std::atomic_bool enabled_{false}, linked_{false}, voice_{false}, music_{false};
    std::mutex mutex_;
    TaskHandle_t worker_ = nullptr;
    bool uart_ready_ = false;
    std::string pending_, wanted_, confirmed_, discovered_mac_, saved_;
    std::string state_ = "off";
    std::vector<Device> devices_;
    int64_t deadline_ = 0;
    bool active_power_ = false;
    bool saw_link_down_ = false;
    bool present_ = false;
    bool reconnect_on_start_ = false;
    int baud_ = 115200;
};

class KcxCodec : public NoAudioCodecSimplex {
public:
    explicit KcxCodec(KcxAudio& audio);
    void SelectOutput(AudioOutputKind kind) override;

private:
    KcxAudio& audio_;
    bool bluetooth_ = false;
};

#endif
