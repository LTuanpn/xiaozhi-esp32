#include "kcx_audio.h"

#ifdef CONFIG_BREAD_COMPACT_LCD_KCX

#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#include <algorithm>
#include <cctype>

#include "config.h"
#include "settings.h"

namespace {
constexpr auto kUart = UART_NUM_1;
constexpr int kProbeRates[] = {115200, 9600, 115200, 9600};

std::string Mac(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X"))
        text.remove_prefix(2);
    std::string mac;
    for (unsigned char c : text) {
        if (c == ':' || c == '-')
            continue;
        if (!std::isxdigit(c) || mac.size() == 12)
            return {};
        mac += static_cast<char>(std::tolower(c));
    }
    return mac.size() == 12 ? mac : std::string{};
}

std::string_view Trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return {};
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

std::string ExtractMac(std::string_view text) {
    const auto hex = text.find("0x");
    if (hex != std::string_view::npos)
        return Mac(text.substr(hex, 14));
    const auto colon = text.find(':');
    if (colon != std::string_view::npos)
        return Mac(Trim(text.substr(colon + 1)).substr(0, 12));
    return {};
}
}  // namespace

bool KcxAudio::EnsureWorker() {
    if (worker_)
        return true;
    return xTaskCreate([](void* self) { static_cast<KcxAudio*>(self)->Run(); }, "kcx_audio", 4096,
                       this, 3, &worker_) == pdPASS;
}

void KcxAudio::Restore() {
    Settings settings("kcx_audio");
    saved_ = Mac(settings.GetString("speaker"));
    voice_ = settings.GetBool("voice");
    music_ = settings.GetBool("music");
}

bool KcxAudio::Power(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled && !EnsureWorker())
        return false;
    enabled_ = enabled;
    if (!enabled) {
        linked_ = false;
        pending_.clear();
        state_ = "off";
    } else if (!active_power_) {
        state_ = "starting";
    }
    return true;
}

bool KcxAudio::Request(std::string_view action, std::string_view target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !active_power_ || !present_ || !pending_.empty())
        return false;
    if (action == "disconnect") {
        linked_ = false;
        pending_ = "disconnect";
        return true;
    }
    if (state_ == "scanning" || state_ == "connecting")
        return false;
    if (action == "scan") {
        pending_ = "scan";
        return true;
    }
    if (action != "connect")
        return false;
    std::string mac = target.empty() ? saved_ : Mac(target);
    if (mac.empty()) {
        for (const auto& device : devices_) {
            if (device.name != target)
                continue;
            if (!mac.empty())
                return false;
            mac = device.mac;
        }
    }
    if (mac.empty())
        return false;
    wanted_ = std::move(mac);
    pending_ = "connect";
    return true;
}

void KcxAudio::SetRoutes(bool voice, bool music) {
    Settings settings("kcx_audio", true);
    if (voice_.exchange(voice) != voice)
        settings.SetBool("voice", voice);
    if (music_.exchange(music) != music)
        settings.SetBool("music", music);
}

bool KcxAudio::UseBluetooth(AudioOutputKind kind) const {
    return enabled_ && linked_ && gpio_get_level(KCX_LINK_GPIO) &&
           (kind == AudioOutputKind::Voice ? voice_.load()
                                           : kind == AudioOutputKind::Music && music_.load());
}

cJSON* KcxAudio::Status() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* json = cJSON_CreateObject();
    if (!json)
        return nullptr;
    cJSON_AddBoolToObject(json, "enabled", enabled_);
    cJSON_AddBoolToObject(json, "connected", linked_ && gpio_get_level(KCX_LINK_GPIO));
    cJSON_AddBoolToObject(json, "module_ready", present_);
    cJSON_AddBoolToObject(json, "voice", voice_);
    cJSON_AddBoolToObject(json, "music", music_);
    cJSON_AddStringToObject(json, "state", state_.c_str());
    cJSON_AddStringToObject(json, "mac", linked_ ? confirmed_.c_str() : "");
    cJSON_AddStringToObject(json, "saved", saved_.c_str());
    auto* items = cJSON_AddArrayToObject(json, "devices");
    for (const auto& device : devices_) {
        auto* row = cJSON_CreateObject();
        if (!row)
            break;
        cJSON_AddStringToObject(row, "mac", device.mac.c_str());
        cJSON_AddStringToObject(row, "name", device.name.c_str());
        cJSON_AddItemToArray(items, row);
    }
    return json;
}

void KcxAudio::Transmit(const std::string& command) {
    uart_write_bytes(kUart, command.data(), command.size());
    if (uart_crlf_)
        uart_write_bytes(kUart, "\r\n", 2);
}

void KcxAudio::ConfigurePower(bool enabled) {
    if (enabled) {
        uart_set_pin(kUart, KCX_UART_TX_GPIO, KCX_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
        gpio_set_level(KCX_POWER_EN_GPIO, 1);
    } else {
        gpio_set_direction(KCX_UART_TX_GPIO, GPIO_MODE_INPUT);
        gpio_set_level(KCX_POWER_EN_GPIO, 0);
    }
}

void KcxAudio::ReadLine(std::string_view line) {
    line = Trim(line);
    if (line == "OK+" || line.starts_with("OK+VERS")) {
        const bool first = !present_;
        present_ = true;
        if (state_ == "starting" || state_ == "no_module")
            state_ = "ready";
        if (first && uart_crlf_)
            Transmit("AT+BT_MODE?");
        return;
    }
    if (line.starts_with("OK+BT_EMITTER")) {
        Transmit("AT+CHANNEL=1");
        return;
    }
    if (line.starts_with("OK+BT_RECEIVER")) {
        present_ = false;
        linked_ = false;
        state_ = "receiver_mode";
        return;
    }
    if (state_ == "scanning") {
        auto address_at = line.find("MacAdd:");
        if (address_at == std::string_view::npos)
            address_at = line.find("MacDdd:");
        if (address_at == std::string_view::npos)
            address_at = line.find("MACADD:");
        if (address_at != std::string_view::npos) {
            discovered_mac_ = ExtractMac(line.substr(address_at));
            auto name_at = line.find("Name:", address_at);
            if (name_at == std::string_view::npos)
                name_at = line.find("NAME:", address_at);
            if (name_at == std::string_view::npos)
                return;
            line = line.substr(name_at);
        }
        if ((line.starts_with("Name:") || line.starts_with("NAME:")) && !discovered_mac_.empty()) {
            const auto name = Trim(line.substr(5)).substr(0, 70);
            auto existing = std::find_if(devices_.begin(), devices_.end(), [this](const Device& d) {
                return d.mac == discovered_mac_;
            });
            if (existing != devices_.end())
                existing->name = name;
            else if (devices_.size() < 12)
                devices_.push_back({discovered_mac_, std::string(name)});
            discovered_mac_.clear();
        }
        if (line.starts_with("ALL Devices"))
            state_ = "ready";
    }
    if (state_ == "connecting" && (line.starts_with("CON:") || line.starts_with("CONNECTED") ||
                                   line.starts_with("OK+CONNECTED"))) {
        const auto mac = ExtractMac(line);
        if (mac == wanted_ && !mac.empty())
            confirmed_ = mac;
    }
    if (line.starts_with("DISCONNECT") || line.starts_with("OK+DISCON"))
        linked_ = false;
    if (line.starts_with("ERROR") || line.starts_with("ERR") || line.starts_with("Wrong")) {
        if (state_ == "scanning" || state_ == "connecting")
            state_ = "command_unsupported";
    }
}

void KcxAudio::Run() {
    const uart_config_t config{.baud_rate = 115200,
                               .data_bits = UART_DATA_8_BITS,
                               .parity = UART_PARITY_DISABLE,
                               .stop_bits = UART_STOP_BITS_1,
                               .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                               .source_clk = UART_SCLK_DEFAULT};
    const bool uart_ok = uart_param_config(kUart, &config) == ESP_OK &&
                         uart_driver_install(kUart, 1024, 0, 0, nullptr, 0) == ESP_OK;
    std::string line;
    line.reserve(180);
    int64_t probe = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = esp_timer_get_time();
            if (!uart_ok) {
                state_ = "uart_error";
            } else if (enabled_ != active_power_) {
                active_power_ = enabled_;
                ConfigurePower(active_power_);
                present_ = false;
                linked_ = false;
                reconnect_on_start_ = active_power_ && !saved_.empty();
                probe_profile_ = 0;
                line.clear();
                uart_flush_input(kUart);
                deadline_ = now + 5600000;
                probe = now + 650000;
            }
            if (uart_ok && active_power_) {
                if (!present_ && state_ != "receiver_mode" && now >= probe) {
                    uart_set_baudrate(kUart, kProbeRates[probe_profile_]);
                    uart_crlf_ = probe_profile_ < 2;
                    probe_profile_ = (probe_profile_ + 1) % std::size(kProbeRates);
                    Transmit("AT+");
                    probe = now + 1200000;
                    if (now >= deadline_)
                        state_ = "no_module";
                }
                if (present_ && state_ == "ready" && reconnect_on_start_ && now >= probe) {
                    reconnect_on_start_ = false;
                    if (pending_.empty()) {
                        wanted_ = saved_;
                        pending_ = "connect";
                    }
                }
                if (!pending_.empty()) {
                    reconnect_on_start_ = false;
                    const auto action = std::move(pending_);
                    pending_.clear();
                    linked_ = false;
                    confirmed_.clear();
                    if (action == "scan") {
                        devices_.clear();
                        discovered_mac_.clear();
                        Transmit("AT+SCAN");
                        state_ = "scanning";
                        deadline_ = now + 14000000;
                    } else if (action == "connect") {
                        saw_link_down_ = !gpio_get_level(KCX_LINK_GPIO);
                        if (!saw_link_down_)
                            Transmit("AT+DISCON");
                        else
                            Transmit("AT+CONADD=0x" + wanted_);
                        state_ = "connecting";
                        deadline_ = now + 45000000;
                    } else {
                        Transmit("AT+DISCON");
                        state_ = "ready";
                    }
                }
                if (state_ == "connecting" && !saw_link_down_ && !gpio_get_level(KCX_LINK_GPIO)) {
                    saw_link_down_ = true;
                    Transmit("AT+CONADD=0x" + wanted_);
                }
                if (state_ == "connecting" && saw_link_down_ && confirmed_ == wanted_ &&
                    gpio_get_level(KCX_LINK_GPIO)) {
                    linked_ = true;
                    state_ = "connected";
                    if (saved_ != wanted_) {
                        saved_ = wanted_;
                        Settings settings("kcx_audio", true);
                        settings.SetString("speaker", saved_);
                    }
                }
                if (linked_ && !gpio_get_level(KCX_LINK_GPIO)) {
                    linked_ = false;
                    state_ = "ready";
                }
                if (now >= deadline_ && state_ == "scanning")
                    state_ = devices_.empty() ? "no_devices" : "ready";
                if (now >= deadline_ && state_ == "connecting")
                    state_ = "not_confirmed";
            }
        }

        char bytes[128];
        const auto count =
            uart_ok ? uart_read_bytes(kUart, bytes, sizeof(bytes), pdMS_TO_TICKS(60)) : 0;
        if (!uart_ok)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (count <= 0 || !enabled_)
            continue;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < count; ++i) {
            const unsigned char c = bytes[i];
            if (c == '\r' || c == '\n') {
                if (!line.empty())
                    ReadLine(line);
                line.clear();
            } else if (c >= 32 && c < 127 && line.size() < 180) {
                line += static_cast<char>(c);
            }
        }
    }
}

KcxCodec::KcxCodec(KcxAudio& audio)
    : NoAudioCodecSimplex(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                          AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
                          AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN),
      audio_(audio) {
    gpio_set_direction(KCX_DAC_DATA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(KCX_DAC_DATA_GPIO, 0);
    gpio_set_direction(KCX_LINK_GPIO, GPIO_MODE_INPUT);
    gpio_pulldown_en(KCX_LINK_GPIO);
    gpio_set_level(KCX_POWER_EN_GPIO, 0);
    gpio_set_direction(KCX_POWER_EN_GPIO, GPIO_MODE_OUTPUT);
    audio_.Restore();
}

void KcxCodec::SelectOutput(AudioOutputKind kind) {
    const bool bluetooth = audio_.UseBluetooth(kind);
    if (bluetooth == bluetooth_)
        return;
    // Push at least one DMA ring of silence before changing the physical sink.
    // Subsequent samples for the previous output cannot spill into the next sink.
    static constexpr int16_t silence[AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM]{};
    Write(silence, std::size(silence));
    const auto old_pin = bluetooth_ ? KCX_DAC_DATA_GPIO : AUDIO_I2S_SPK_GPIO_DOUT;
    const auto new_pin = bluetooth ? KCX_DAC_DATA_GPIO : AUDIO_I2S_SPK_GPIO_DOUT;
    gpio_set_level(old_pin, 0);
    esp_rom_gpio_connect_out_signal(old_pin, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(new_pin, I2S0O_SD_OUT_IDX, false, false);
    bluetooth_ = bluetooth;
}

#endif
