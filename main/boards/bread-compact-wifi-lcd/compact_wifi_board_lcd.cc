#include <esp_app_desc.h>
#include <wifi_manager.h>
#include "activity_deadline.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "conversation_text.h"
#include "kcx_audio.h"
#include "lamp_controller.h"
#include "lan_control.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "pocket_lcd_display.h"
#include "settings.h"
#include "system_reset.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif

#define TAG "CompactWifiBoardLCD"

class CompactWifiBoardLCD : public WifiBoard {
private:
    Button boot_button_;
    LanControl lan_control_;
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    esp_timer_handle_t wifi_qr_timer_ = nullptr;
    int pending_wifi_qr_ms_ = 0;  // -1 cancels a provisioning request already queued.
    int64_t native_wifi_qr_until_us_ = 0;
#endif
#ifdef CONFIG_BREAD_COMPACT_LCD_KCX
    KcxAudio kcx_;
#endif
    PocketLcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // Timers & States for 7 MCP Tools
    esp_timer_handle_t guard_timer_ = nullptr;
    bool guard_mode_active_ = false;
    int64_t guard_arm_until_us_ = 0;
    int64_t countdown_deadline_us_ = 0;
    int64_t workout_deadline_us_ = 0;
    std::string countdown_label_;
    std::atomic_bool guard_poll_pending_{false};
    int guard_noise_level_ = 0;
    int guard_hits_ = 0;

    int workout_duration_ = 0;
    int workout_beep_interval_ = 0;
    int64_t guard_alert_us_ = 0;
    const char* last_alarm_ = "none";
    std::string alarm_label_;

    const char* GuardState() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        if (!guard_mode_active_)
            return guard_alert_us_ ? "pending_alarm" : "off";
        if (app.GetDeviceState() == kDeviceStateSpeaking ||
            app.GetDeviceState() == kDeviceStateNotifying || !audio.IsPlaybackIdle())
            return "paused_for_speaker";
        if (!audio.IsWakeWordRunning() && !audio.IsAudioProcessorRunning())
            return "paused_for_mic";
        return esp_timer_get_time() < guard_arm_until_us_ ? "arming" : "listening";
    }

    std::string GetDeviceStatusJson() override {
        CJsonUniquePtr root(cJSON_Parse(WifiBoard::GetDeviceStatusJson().c_str()));
        if (!root)
            return "{}";
        auto& app = Application::GetInstance();
        const auto now = esp_timer_get_time();
        const int timer = jarvis::SecondsLeft(countdown_deadline_us_, now);
        const int workout = jarvis::SecondsLeft(workout_deadline_us_, now);
        auto state = [&](int64_t deadline, int left, std::string_view kind) {
            if (deadline)
                return left ? "running" : "pending_alarm";
            return app.IsAlarmActive() && kind == last_alarm_ ? "ringing" : "idle";
        };
        cJSON_AddNumberToObject(root.get(), "timer_seconds", timer);
        cJSON_AddStringToObject(root.get(), "timer_label", countdown_label_.c_str());
        cJSON_AddStringToObject(root.get(), "timer_state",
                                state(countdown_deadline_us_, timer, "timer"));
        cJSON_AddNumberToObject(root.get(), "workout_remaining_seconds", workout);
        cJSON_AddNumberToObject(root.get(), "workout_elapsed_seconds",
                                workout_deadline_us_ ? workout_duration_ - workout : 0);
        cJSON_AddStringToObject(root.get(), "workout_state",
                                state(workout_deadline_us_, workout, "workout"));
        cJSON_AddBoolToObject(root.get(), "guard", guard_mode_active_);
        cJSON_AddStringToObject(root.get(), "guard_state", GuardState());
        cJSON_AddBoolToObject(root.get(), "alarm", app.IsAlarmActive());
        cJSON_AddStringToObject(root.get(), "last_alarm", last_alarm_);
        cJSON_AddStringToObject(root.get(), "last_alarm_label", alarm_label_.c_str());
        cJSON_AddStringToObject(root.get(), "music", app.MusicStatus());
        CJsonStringUniquePtr json(cJSON_PrintUnformatted(root.get()));
        return json ? json.get() : "{}";
    }

    cJSON* ControlStatus() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        CJsonUniquePtr status(cJSON_Parse(GetDeviceStatusJson().c_str()));
        if (!status)
            return nullptr;
        cJSON_AddStringToObject(status.get(), "version", esp_app_get_description()->version);
        cJSON_AddStringToObject(status.get(), "state",
                                DeviceStateMachine::GetStateName(app.GetDeviceState()));
        cJSON_AddBoolToObject(status.get(), "wake_word", audio.IsWakeWordRunning());
        cJSON_AddBoolToObject(status.get(), "standby", display_ && display_->IsStandbyClockActive());
        if (display_)
            cJSON_AddStringToObject(status.get(), "default_emotion", display_->DefaultEmotion().c_str());
#ifdef CONFIG_BREAD_COMPACT_LCD_KCX
        cJSON_AddItemToObject(status.get(), "bluetooth", kcx_.Status());
#endif
        cJSON_AddStringToObject(status.get(), "ota_url", Ota::GetCheckVersionUrl().c_str());
        Settings lan("lan_control");
        cJSON_AddStringToObject(status.get(), "https_origin", lan.GetString("https_origin").c_str());
        if (auto* emotions = cJSON_AddArrayToObject(status.get(), "emotions")) {
            for (const auto name : PocketLcdDisplay::ManualEmotions()) {
                if (auto* item = cJSON_CreateString(name.data()))
                    cJSON_AddItemToArray(emotions, item);
            }
        }
        return status.release();
    }

    bool ShowProvisioningQr(int duration_ms) override {
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        const int pending = std::exchange(pending_wifi_qr_ms_, 0);
        if (pending < 0) {
            ArmWifiQrTimeout(1);
            return true;
        }
        if (pending > 0)
            duration_ms = pending;
        if (pending > 0 || native_wifi_qr_until_us_ > 0)
            ArmWifiQrTimeout(duration_ms);
#endif
        auto& wifi = WifiManager::GetInstance();
        const auto ssid = wifi.GetApSsid();
        if (ssid.empty() || !display_) {
            return false;
        }

        // The provisioning AP is open. Encode its SSID, never saved station credentials.
        std::string payload = "WIFI:T:nopass;S:";
        for (char c : ssid) {
            if (c == '\\' || c == ';' || c == ':' || c == ',' || c == '"') {
                payload += '\\';
            }
            payload += c;
        }
        payload += ";;";
        return display_->TryShowQrCode(payload, ssid, duration_ms);
    }

    ToolResult CloseQr() {
        if (!display_)
            return std::unexpected("Màn hình chưa sẵn sàng.");
        display_->HideQrCode(true);
        lan_control_.CancelPairing();
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        if (pending_wifi_qr_ms_ > 0)
            pending_wifi_qr_ms_ = -1;
        if (native_wifi_qr_until_us_ > 0) {
            if (IsInWifiConfigMode() && xTaskCreate(
                                            [](void*) {
                                                WifiManager::GetInstance().StopConfigAp();
                                                vTaskDelete(nullptr);
                                            },
                                            "wifi_qr_close", 4096, nullptr, 2, nullptr) != pdPASS) {
                return std::unexpected("Không thể đóng hotspot Xiaozhi. Vui lòng thử lại.");
            }
            esp_timer_stop(wifi_qr_timer_);
            native_wifi_qr_until_us_ = 0;
        }
#endif
        return std::string("Đã tắt mã QR.");
    }

    ToolResult ShowLanConfigQr(int duration) {
        const auto ip = WifiManager::GetInstance().GetIpAddress();
        if (ip.empty() || ip == "0.0.0.0" || !display_)
            return std::unexpected("Robot chưa có IP hoặc màn hình chưa sẵn sàng.");
        CloseQr();
        Settings settings("lan_control");
        const auto url = lan_control_.Pair(
            ip, settings.GetString("https_origin"), [this]() { return ControlStatus(); }, duration);
        if (url.empty() || !display_->TryShowQrCode(url, "DIEU KHIEN JARVIS", duration * 1000))
            return std::unexpected("Không thể mở QR cấu hình Jarvis.");
        return std::string("Quét QR bằng điện thoại cùng Wi-Fi để mở cấu hình Jarvis.");
    }

#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    bool PrepareWifiQrTimeout() {
        if (wifi_qr_timer_)
            return true;
        esp_timer_create_args_t timeout{
            .callback =
                [](void* arg) {
                    auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                    Application::GetInstance().Schedule([board]() {
                        if (board->native_wifi_qr_until_us_ != 0 &&
                            esp_timer_get_time() >= board->native_wifi_qr_until_us_ &&
                            !board->CloseQr()) {
                            esp_timer_start_once(board->wifi_qr_timer_, 1000000);
                        }
                    });
                },
            .arg = this,
            .name = "wifi_qr_timeout",
        };
        return esp_timer_create(&timeout, &wifi_qr_timer_) == ESP_OK;
    }

    void ArmWifiQrTimeout(int duration_ms) {
        esp_timer_stop(wifi_qr_timer_);
        const int64_t delay_us = duration_ms * 1000LL;
        native_wifi_qr_until_us_ = esp_timer_get_time() + delay_us;
        esp_timer_start_once(wifi_qr_timer_, delay_us);
    }
#endif

    ToolResult ShowWifiQr(int duration) {
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        auto& app = Application::GetInstance();
        if (!display_ || app.IsAlarmActive() || pending_wifi_qr_ms_ != 0)
            return std::unexpected("Màn hình đang bận.");
        if (IsInWifiConfigMode()) {
            if (!ShowProvisioningQr(duration * 1000))
                return std::unexpected("Không thể hiện mã QR Wi-Fi Xiaozhi.");
        } else {
            const auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle && state != kDeviceStateListening &&
                state != kDeviceStateSpeaking && state != kDeviceStateNotifying)
                return std::unexpected("Robot chưa sẵn sàng mở cấu hình Wi-Fi.");
            if (!PrepareWifiQrTimeout() || !CloseQr())
                return std::unexpected("Không thể mở cấu hình Wi-Fi.");
            pending_wifi_qr_ms_ = duration * 1000;
            app.Schedule([this, &app]() {
                if (pending_wifi_qr_ms_ < 0 || app.IsAlarmActive()) {
                    pending_wifi_qr_ms_ = 0;
                    return;
                }
                app.EndConversation();
                if (!EnterWifiConfigMode()) {
                    pending_wifi_qr_ms_ = 0;
                    display_->ShowNotification("KHONG MO DUOC CAU HINH WI-FI");
                }
            });
        }
        return std::string(
            "Quét QR để vào trang cấu hình gốc của Xiaozhi. "
            "Jarvis tạm ngắt trong lúc cấu hình. Bấm BOOT để quay lại Wi-Fi đã lưu.");
#else
        return std::unexpected("Bản firmware này không hỗ trợ cấu hình Wi-Fi bằng hotspot.");
#endif
    }

    void OnClockTick() override {
        auto& app = Application::GetInstance();
        const auto now = esp_timer_get_time();
        if (app.IsAlarmActive() || app.GetDeviceState() == kDeviceStateUpgrading ||
            app.GetDeviceState() == kDeviceStateWifiConfiguring)
            return;
        // A due deadline doubles as a bounded pending alarm until playback accepts it.
        auto* first = jarvis::FirstDue(
            now, {&countdown_deadline_us_, &workout_deadline_us_, &guard_alert_us_});
        if (!first) {
            if (workout_deadline_us_) {
                const int left = jarvis::SecondsLeft(workout_deadline_us_, now);
                const int interval = (workout_duration_ - left) / 10;
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    if (display_)
                        display_->ShowNotification("[TAP LUYEN] Con " + std::to_string(left) + "s",
                                                   1100);
                    if (interval > workout_beep_interval_ && app.GetAudioService().IsPlaybackIdle())
                        app.PlaySound(Lang::Sounds::OGG_POPUP);
                }
                workout_beep_interval_ = interval;
            }
            return;
        }
        const bool guard = first == &guard_alert_us_;
        const bool timer = first == &countdown_deadline_us_;
        const std::string label = guard ? "[CANH BAO] TIENG DONG BAT THUONG"
                                        : (timer ? countdown_label_ : "[TAP LUYEN] HOAN THANH");
        if (RaiseAlarm(guard, label)) {
            *first = 0;
            last_alarm_ = guard ? "guard" : (timer ? "timer" : "workout");
            alarm_label_ = label;
            if (timer && display_)
                display_->SetCountdownTimer(0, "IDLE");
        }
    }

    bool RaiseAlarm(bool guard, const std::string& label) {
        auto& app = Application::GetInstance();
        const bool started = guard
            ? app.StartLocalAlarm({Lang::Sounds::OGG_GUARD_ALERT_VI,
                                   Lang::Sounds::OGG_GUARD_REMINDER_2_VI,
                                   Lang::Sounds::OGG_GUARD_REMINDER_3_VI})
            : app.StartLocalAlarm({Lang::Sounds::OGG_TIMER_ALERT_VI,
                                   Lang::Sounds::OGG_TIMER_REMINDER_2_VI,
                                   Lang::Sounds::OGG_TIMER_REMINDER_3_VI});
        if (started && display_) {
            display_->SetAlarmEmotion(guard);
            display_->ShowNotification(label, 15000);
        }
        ESP_LOGI(TAG, "Local alarm kind=%s accepted=%d", guard ? "guard" : "timer", started);
        return started;
    }

    ToolResult ShowStandby() {
        auto& app = Application::GetInstance();
        if (!display_ || app.IsAlarmActive() || !display_->ShowStandbyClock(true)) {
            return std::unexpected("Màn hình đang bận.");
        }
        // The MCP callback runs on the application task. Send the result before ending the turn.
        app.Schedule([&app]() { app.EndConversation(); });
        return std::string("Đã hiển thị đồng hồ. Gọi Jarvis để trò chuyện tiếp.");
    }

    void GuardTick() {
        if (!guard_mode_active_)
            return;
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        // Ignore all local playback, including chimes, not just cloud TTS.
        if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
            !app.GetAudioService().IsPlaybackIdle()) {
            guard_hits_ = 0;
            guard_arm_until_us_ = esp_timer_get_time() + 1500000;
            return;
        }
        if (!app.GetAudioService().IsWakeWordRunning() &&
            !app.GetAudioService().IsAudioProcessorRunning())
            return;
        const int level = app.GetAudioService().GetInputLevel();
        if (esp_timer_get_time() < guard_arm_until_us_) {
            guard_noise_level_ = (guard_noise_level_ * 7 + level) / 8;
            return;
        }
        const int threshold = std::min(90, std::max(25, guard_noise_level_ + 15));
        guard_hits_ = level > threshold ? guard_hits_ + 1 : 0;
        if (guard_hits_ >= 3 || level >= std::min(100, threshold + 35)) {
            guard_mode_active_ = false;
            esp_timer_stop(guard_timer_);
            if (display_)
                display_->SetGuardMode(false);
            guard_alert_us_ = esp_timer_get_time();
            OnClockTick();
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // Khởi tạo LCD Driver ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel_));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
        panel_config.vendor_config = &gc9107_vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel_));
#endif
        
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new PocketLcdDisplay(panel_io, panel_,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.Schedule([this, &app]() {
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
                if (pending_wifi_qr_ms_ != 0 ||
                    (native_wifi_qr_until_us_ != 0 && IsInWifiConfigMode())) {
                    CloseQr();
                    return;
                }
#endif
                if (display_) {
                    display_->HideStandbyClock(true);
                    display_->HideQrCode(true);
                }
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                if (app.IsAlarmActive()) {
                    app.StopNotification();
                    return;
                }
                app.ToggleChatState();
            });
        });
    }

    void InitializeTools() {
#if CONFIG_BREAD_LCD_LAMP_ENABLED
        static LampController lamp(LAMP_GPIO);
#endif
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.network.check",
            "Kiểm tra kết nối thật của robot. 'kiểm tra Wi-Fi', 'kiểm tra OTA', 'kiểm tra MCP "
            "Music' "
            "gọi tool này với service lần lượt là wifi, ota, music. Dùng all khi hỏi chung. "
            "Đọc kết quả một lần, không suy đoán hay cài bản cập nhật.",
            PropertyList({Property("service", kPropertyTypeString, "all")}),
            [this](const PropertyList& properties) -> ToolResult {
                return CheckConnections(properties["service"].value<std::string>());
            },
            true);
        mcp_server.AddTool(
            "self.audio.prepare_music",
            "Báo một lần rằng robot đang chuẩn bị nhạc. Gọi trước play_song nếu người dùng "
            "muốn phát nhạc, rồi tiếp tục tìm bài. Robot tự nói câu chuẩn bị, không nói lặp.",
            PropertyList(), [](const PropertyList&) -> ToolResult {
                const int id = Application::GetInstance().PrepareMusicRequest();
                if (!id)
                    return std::unexpected("Robot đang bận, chưa thể chuẩn bị nhạc.");
                return "Gọi play_song ngay với request_id=" + std::to_string(id) +
                       ". Không đọc mã này.";
            });
        mcp_server.AddTool(
            "self.audio.finish_music_request",
            "Kết thúc lượt tìm không phát được hoặc cần hỏi chọn. Chỉ đọc user_message nếu kết quả "
            "là accepted.",
            PropertyList({Property("request_id", kPropertyTypeInteger, 1, 2147483647)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.FinishMusicRequest(p["request_id"].value<int>()))
                    return "stale: kết thúc im lặng";
                return "accepted";
            });

        mcp_server.AddTool(
            "self.timer.set_countdown",
            "Đặt hoặc hủy (seconds=0) timer. Nếu đang chạy, hỏi trước khi thay rồi truyền "
            "replace=true. Hỏi thời gian còn lại dùng self.get_device_status.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 60, 0, 86400),
                          Property("label", kPropertyTypeString, "Hết giờ rồi!"),
                          Property("replace", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& p) -> ToolResult {
                const int seconds = p["seconds"].value<int>();
                if (seconds && countdown_deadline_us_ && !p["replace"].value<bool>())
                    return std::unexpected("Đã có timer " + countdown_label_ +
                                           ". Bạn muốn thay timer này không?");
                if (p["label"].value<std::string>().size() > 240)
                    return std::unexpected("Nhãn hẹn giờ quá dài.");
                auto& app = Application::GetInstance();
                if (!seconds && !countdown_deadline_us_ && app.IsAlarmActive() &&
                    std::string_view(last_alarm_) == "timer")
                    app.StopNotification();
                countdown_deadline_us_ =
                    seconds ? esp_timer_get_time() + int64_t(seconds) * 1000000 : 0;
                countdown_label_ = seconds ? p["label"].value<std::string>() : "";
                if (display_)
                    display_->SetCountdownTimer(seconds, seconds ? countdown_label_ : "IDLE");
                return seconds ? "Đã đặt " + countdown_label_ + " trong " +
                                     std::to_string(seconds) + " giây."
                               : std::string("Đã hủy bộ đếm ngược.");
            });
        mcp_server.AddTool("self.alarm.dismiss", "Tắt chuông đang reo, giữ các hẹn giờ khác.",
                           PropertyList(), [](const PropertyList&) -> ReturnValue {
                               auto& app = Application::GetInstance();
                               if (!app.IsAlarmActive())
                                   return "Không có chuông đang reo.";
                               app.StopNotification();
                               return "Đã tắt chuông.";
                           });
        mcp_server.AddTool("self.timer.extend",
                           "Cộng số giây vào timer đang chạy. Giữ nguyên nhãn.",
                           PropertyList({Property("seconds", kPropertyTypeInteger, 30, 1, 86400)}),
                           [this](const PropertyList& p) -> ToolResult {
                               const auto now = esp_timer_get_time();
                               const int left = jarvis::SecondsLeft(countdown_deadline_us_, now);
                               if (!left)
                                   return std::unexpected("Không có timer đang đếm để gia hạn.");
                               const int extra = p["seconds"].value<int>();
                               if (left + extra > 86400)
                                   return std::unexpected("Thời gian còn lại tối đa 24 giờ.");
                               countdown_deadline_us_ += int64_t(extra) * 1000000;
                               if (display_)
                                   display_->SetCountdownTimer(left + extra, countdown_label_);
                               return "Timer " + countdown_label_ + " còn " +
                                      std::to_string(left + extra) + " giây.";
                           });

        // 2. TOOL GHIM GHI CHÚ / HIỂN THỊ IP, WIFI, MÃ QR
        mcp_server.AddTool(
            "self.screen.pin_memo",
            "Ghim ghi chú hoặc hiển thị IP (text='ip') lên màn hình. "
            "Để kiểm tra kết nối Wi-Fi, OTA hoặc MCP Music, dùng self.network.check. "
            "Để mở QR, dùng các tool QR chuyên dụng.",
            PropertyList({
                Property("text", kPropertyTypeString, ""),
                Property("duration_seconds", kPropertyTypeInteger, 600, 10, 3600)
            }),
            [this](const PropertyList& properties) -> ToolResult {
                std::string text = properties["text"].value<std::string>();
                int duration = properties["duration_seconds"].value<int>();

                // Chuẩn hóa text sang chữ thường để phát hiện từ khóa
                std::string lower_text = text;
                std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                // TRƯỜNG HỢP 0: Yêu cầu chuyển sang Màn hình chờ (Standby Clock)
                if (lower_text == "standby" || lower_text == "dong_ho" || lower_text == "clock" ||
                    lower_text == "home" || lower_text == "home screen" ||
                    lower_text == "trang chủ" || lower_text == "trang chu" ||
                    lower_text.find("man hinh chinh") != std::string::npos ||
                    lower_text.find("màn hình chính") != std::string::npos ||
                    lower_text.find("man hinh cho") != std::string::npos ||
                    lower_text.find("màn hình chờ") != std::string::npos ||
                    lower_text.find("che do cho") != std::string::npos ||
                    lower_text.find("chế độ chờ") != std::string::npos ||
                    lower_text.find("dong ho") != std::string::npos ||
                    lower_text.find("đồng hồ") != std::string::npos) {
                    return ShowStandby();
                }

                if (lower_text.find("qr") != std::string::npos) {
                    if (lower_text.find("tắt") != std::string::npos ||
                        lower_text.find("đóng") != std::string::npos ||
                        lower_text.find("ẩn") != std::string::npos ||
                        lower_text.starts_with("tat ") || lower_text.starts_with("dong ") ||
                        lower_text.starts_with("an "))
                        return CloseQr();
                    if (lower_text.find("điều khiển") != std::string::npos ||
                        lower_text.find("dieu khien") != std::string::npos)
                        return ShowLanConfigQr(duration);
                    if (lower_text.find("wifi") != std::string::npos ||
                        lower_text.find("wi-fi") != std::string::npos ||
                        lower_text.find("cấu hình") != std::string::npos ||
                        lower_text.find("cau hinh") != std::string::npos)
                        return ShowWifiQr(duration);
                    return std::unexpected(
                        "Bạn muốn QR Wi-Fi, QR trang điều khiển hay QR của đường dẫn nào?");
                }

                // TRƯỜNG HỢP 2: Yêu cầu thông tin IP / WiFi
                if (lower_text.find("ip") != std::string::npos || lower_text.find("wifi") != std::string::npos ||
                    lower_text.find("mang") != std::string::npos ||
                    lower_text.find("mạng") != std::string::npos || text.empty()) {
                    auto& wifi = WifiManager::GetInstance();
                    std::string ssid = wifi.GetSsid();
                    std::string ip = wifi.GetIpAddress();
                    int rssi = wifi.GetRssi();
                    if (display_ != nullptr) {
                        std::string info_disp = "[WIFI] IP: " + (ip.empty() ? "Chua co IP" : ip) +
                                                " | WF: " + (ssid.empty() ? "Chua ket noi" : ssid);
                        display_->ShowNotification(info_disp.c_str(), duration * 1000);
                        display_->SetEmotion("idea");
                    }

                    return "Thông tin mạng phần cứng hiện tại: WiFi = " +
                           (ssid.empty() ? "Chưa kết nối" : ssid) +
                           ", Địa chỉ IP = " + (ip.empty() ? "Chưa nhận IP" : ip) +
                           ", Cường độ sóng = " + std::to_string(rssi) +
                           "dBm. Đã ghim thông tin lên màn hình.";
                }

                // TRƯỜNG HỢP 3: Ghim ghi chú văn bản thông thường
                if (display_ != nullptr) {
                    display_->SetPinnedMemo(text);
                    std::string pinned = "[GHI CHU] " + text;
                    display_->ShowNotification(pinned.c_str(), duration * 1000);
                    display_->SetEmotion("idea");
                }
                return "Đã ghim nội dung lên màn hình: " + text + "";
            }
        );

        // 3. TOOL BẬT/TẮT CHẾ ĐỘ NGẮT LỜI (REALTIME FULL-DUPLEX)
        mcp_server.AddTool(
            "self.audio.set_realtime_mode",
            "Bật hoặc tắt chế độ trò chuyện rảnh tay liên tục (cho phép người dùng nói chen ngang ngắt lời robot khi robot đang nói). enable = true để bật, false để tắt.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
                Application::GetInstance().SetAecMode(enable ? kAecOnServerSide : kAecOff);
                return enable ? "Đã bật chế độ Realtime (cho phép ngắt lời)"
                              : "Đã chuyển về chế độ đàm thoại tiêu chuẩn";
            }
        );

        // 3.1. TOOL PHÁT ÂM THANH / NHẠC TỪ URL CHO ROBOT (OGG OPUS)
        mcp_server.AddTool(
            "self.audio.play_url",
            "Phát âm thanh/nhạc trực tiếp từ một đường dẫn URL (HTTP/HTTPS luồng Ogg Opus). "
            "Khi cần phát bài hát hoặc âm thanh từ server nhạc, hãy gọi tool này với tham số url.",
            PropertyList({Property("url", kPropertyTypeString, ""),
                          Property("request_id", kPropertyTypeInteger, 0, 0, 2147483647)}),
            [](const PropertyList& properties) -> ToolResult {
                std::string url = properties["url"].value<std::string>();
                if (url.empty())
                    return std::unexpected("Chưa có đường dẫn phát nhạc.");
                auto& app = Application::GetInstance();
                const int id = properties["request_id"].value<int>();
                const int url_id = MusicUrlTurn(url);
                if (url_id < 0 || (url_id && id && url_id != id) ||
                    ((id || url_id) && !app.AcceptsMusicRequest(id ? id : url_id)))
                    return std::string("Lượt nhạc đã kết thúc. Không phát và không nhắc lại.");
                if (!app.StartNotification(std::move(url), {}, true)) {
                    app.CancelMusicRequest();
                    return std::unexpected("Robot chưa thể mở luồng nhạc.");
                }
                return "Robot đang mở luồng. Kết thúc lượt, không nói đè nhạc.";
            });

        mcp_server.AddUserOnlyTool(
            "self.audio.get_mic_status", "Inspect the current microphone and wake word state.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                auto& audio = app.GetAudioService();
                auto* status = cJSON_CreateObject();
                cJSON_AddNumberToObject(status, "state", app.GetDeviceState());
                cJSON_AddBoolToObject(status, "wake_word", audio.IsWakeWordRunning());
                cJSON_AddBoolToObject(status, "voice_processing", audio.IsAudioProcessorRunning());
                cJSON_AddNumberToObject(status, "input_level", audio.GetInputLevel());
                cJSON_AddBoolToObject(status, "alarm", app.IsAlarmActive());
                return status;
            });

        // 3.2. TOOL DỪNG PHÁT ÂM THANH / NHẠC TRÊN ROBOT
        mcp_server.AddTool(
            "self.audio.stop_playback",
            "Dừng phát nhạc hoặc âm thanh đang phát từ URL trên robot.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI("Board", "self.audio.stop_playback");
                auto& app = Application::GetInstance();
                if (app.IsAlarmActive())
                    return "Robot đang báo động. Có thể xác nhận chuông bằng nút BOOT.";
                app.CancelMusicRequest();
                if (app.GetDeviceState() == kDeviceStateNotifying)
                    app.StopNotification();
                return "Đã dừng phát âm thanh";
            }
        );

        // 4. TOOL CANH TRỘM PHÒNG BẰNG CẢM BIẾN ÂM THANH MIC (SENTRY MODE)
        mcp_server.AddTool(
            "self.security.guard_mode",
            "Bật hoặc tắt chế độ canh gác phòng (Sentry Mode). Khi bật, robot sẽ quét âm thanh "
            "trong phòng, sau 6 giây ổn định sẽ theo dõi tiếng động; khi vượt ngưỡng sẽ báo bằng "
            "còi và lời nhắc lặp lại đến khi người dùng gọi Jarvis.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ReturnValue {
                guard_mode_active_ = properties["enable"].value<bool>();
                if (guard_timer_ != nullptr) {
                    esp_timer_stop(guard_timer_);
                }

                if (!guard_mode_active_) {
                    if (display_ != nullptr) {
                        display_->SetGuardMode(false);
                        display_->SetEmotion("neutral");
                        display_->ShowNotification("[CANH GAC] Da tat canh phong", 3000);
                    }
                    return "Đã tắt chế độ canh phòng";
                }

                if (guard_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                            if (board->guard_poll_pending_.exchange(true))
                                return;
                            Application::GetInstance().Schedule([board]() {
                                board->GuardTick();
                                board->guard_poll_pending_.store(false);
                            });
                        },
                        .arg = this,
                        .dispatch_method = ESP_TIMER_TASK,
                        .name = "guard_timer",
                        .skip_unhandled_events = true
                    };
                    if (esp_timer_create(&timer_args, &guard_timer_) != ESP_OK) {
                        guard_mode_active_ = false;
                        return "Không tạo được bộ theo dõi GUARD.";
                    }
                }
                if (esp_timer_start_periodic(guard_timer_, 50000) != ESP_OK) {
                    guard_mode_active_ = false;
                    if (display_)
                        display_->SetGuardMode(false);
                    return "Không thể khởi động GUARD.";
                }
                // Chuyển bot sang Idle và ngắt phiên thu âm ngay lập tức
                guard_arm_until_us_ = esp_timer_get_time() + 6000000;
                guard_noise_level_ = 0;
                guard_hits_ = 0;
                Application::GetInstance().Schedule(
                    []() { Application::GetInstance().EndConversation(); });

                if (display_ != nullptr) {
                    auto* p_disp = static_cast<PocketLcdDisplay*>(display_);
                    p_disp->SetGuardMode(true);
                    p_disp->ShowStandbyClock(true /* manual_lock */);
                    p_disp->ShowNotification("[CANH GAC] SENTRY MODE: ON", 5000);
                }

                return "Đã kích hoạt chế độ canh gác phòng bằng cảm biến âm thanh";
            });

        mcp_server.AddTool(
            "self.fitness.start_workout",
            "Bắt đầu đếm hiệp tập, hiện tiến độ và báo khi xong. Hỏi tiến độ dùng "
            "self.get_device_status.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 45, 5, 600)}),
            [this](const PropertyList& p) -> ToolResult {
                if (workout_deadline_us_)
                    return std::unexpected("Đã có một hiệp tập đang chạy hoặc chờ báo.");
                workout_beep_interval_ = 0;
                workout_duration_ = p["seconds"].value<int>();
                workout_deadline_us_ = esp_timer_get_time() + int64_t(workout_duration_) * 1000000;
                if (display_)
                    display_->SetEmotion("excited");
                return "Đã bắt đầu hiệp tập " + std::to_string(workout_duration_) + " giây.";
            });
        mcp_server.AddTool("self.fitness.stop_workout", "Hủy hiệp tập đang đếm.", PropertyList(),
                           [this](const PropertyList&) -> ToolResult {
                               if (!workout_deadline_us_)
                                   return std::string("Không có hiệp tập đang đếm.");
                               workout_deadline_us_ = 0;
                               return std::string("Đã hủy hiệp tập.");
                           });
        // 6. TOOL VÒNG QUAY QUYẾT ĐỊNH SỐ PHẬN (DECISION ROULETTE)
        mcp_server.AddTool(
            "self.fun.pick_random",
            "Quay số ngẫu nhiên để chốt lựa chọn giúp người dùng (ví dụ: ăn gì, làm gì, mua gì). Truyền các lựa chọn cách nhau bởi dấu phẩy vào options.",
            PropertyList({Property("options", kPropertyTypeString, "Có, Không")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string raw = properties["options"].value<std::string>();
                std::vector<std::string> items;
                size_t start = 0;
                while (start < raw.size()) {
                    size_t comma = raw.find(',', start);
                    std::string token = (comma == std::string::npos) ? raw.substr(start) : raw.substr(start, comma - start);
                    size_t first = token.find_first_not_of(" \t");
                    if (first != std::string::npos) {
                        items.push_back(token.substr(first, token.find_last_not_of(" \t") - first + 1));
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }

                if (items.empty()) {
                    return "Không có lựa chọn nào để bốc thăm";
                }

                uint32_t idx = esp_random() % items.size();
                std::string chosen = items[idx];

                if (display_ != nullptr) {
                    display_->SetEmotion("smug");
                    std::string result_msg = "[CHOT] " + chosen;
                    display_->ShowNotification(result_msg.c_str(), 10000);
                }
                return "Kết quả bốc thăm ngẫu nhiên từ phần cứng là: " + chosen + "";
            }
        );

        // 7. TOOL ĐỔ XÚC XẮC / TUNG ĐỒNG XU THỰC TẾ ẢO (ROLL RNG)
        mcp_server.AddTool(
            "self.fun.roll_rng",
            "Đổ xúc xắc (xúc xắc 6 mặt d6, 20 mặt d20...) hoặc tung đồng xu sấp ngửa bằng bộ tạo số ngẫu nhiên phần cứng ESP32. type='dice' hoặc 'coin'. sides là số mặt của xúc xắc (mặc định 6).",
            PropertyList({
                Property("type", kPropertyTypeString, "dice"),
                Property("sides", kPropertyTypeInteger, 6, 2, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string type = properties["type"].value<std::string>();
                int sides = properties["sides"].value<int>();

                if (type == "coin" || sides == 2) {
                    bool is_heads = (esp_random() % 2) == 1;
                    std::string outcome = is_heads ? "[DONG XU] NGUA (Heads)" : "[DONG XU] SAP (Tails)";
                    if (display_ != nullptr) {
                        display_->SetEmotion(is_heads ? "excited" : "smug");
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
                    return "Kết quả tung đồng xu: " + outcome + "";
                } else {
                    int roll = static_cast<int>((esp_random() % sides) + 1);
                    std::string outcome = "[XUC XAC] " + std::to_string(roll) + " / d" + std::to_string(sides);
                    if (display_ != nullptr) {
                        display_->SetEmotion(roll == sides ? "party" : (roll == 1 ? "facepalm" : "smug"));
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(roll == sides ? Lang::Sounds::OGG_SUCCESS : Lang::Sounds::OGG_POPUP);
                    return "Kết quả đổ xúc xắc " + std::to_string(sides) + " mặt: " + outcome + "";
                }
            }
        );

        // CÁC TOOL MÀN HÌNH
        // TOOL CHUYỂN ĐỔI MÀN HÌNH CHỜ ĐỒNG HỒ
        mcp_server.AddTool(
            "self.screen.set_standby",
            "Hiện đồng hồ khi người dùng yêu cầu màn hình chính, màn hình chờ, trang chủ, standby "
            "hoặc đồng hồ: enable=true. Chỉ dùng enable=false khi yêu cầu hiện khuôn mặt robot.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ToolResult {
                if (!display_)
                    return std::unexpected("Màn hình chưa sẵn sàng.");
                bool enable = properties["enable"].value<bool>();
                if (enable) {
                    return ShowStandby();
                }
                if (!display_->ShowFace())
                    return std::unexpected("Màn hình đang bận.");
                return std::string("Đã hiển thị khuôn mặt.");
            });

        mcp_server.AddTool(
            "self.screen.open_lan_control",
            "Mở QR ghép cặp bảng điều khiển Jarvis qua Wi-Fi hiện tại. Dùng khi người dùng nói "
            "'mở trang điều khiển'. Đọc kết quả, không đọc mã ghép cặp.",
            PropertyList(), [this](const PropertyList&) { return ShowLanConfigQr(120); });

        mcp_server.AddUserOnlyTool("self.wifi.start_provisioning",
                                   "Open the original Xiaozhi Wi-Fi setup hotspot.", PropertyList(),
                                   [this](const PropertyList&) { return ShowWifiQr(180); });

        mcp_server.AddUserOnlyTool(
            "self.lan.set_https_origin", "Set the HTTPS reverse proxy origin used by the control QR.",
            PropertyList({Property("origin", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ToolResult {
                const auto origin = properties["origin"].value<std::string>();
                if (!origin.empty()) {
                    if (!origin.starts_with("https://") || origin.size() > 100 ||
                        origin.size() <= 8 ||
                        !std::all_of(origin.begin() + 8, origin.end(), [](unsigned char c) {
                            return std::isalnum(c) || c == '.' || c == '-' || c == ':';
                        }))
                        return std::unexpected("Nhập HTTPS origin, ví dụ https://jarvis.example.vn.");
                }
                Settings settings("lan_control", true);
                settings.SetString("https_origin", origin);
                return true;
            });

        mcp_server.AddUserOnlyTool(
            "self.lan.start_listening", "Start listening from the built-in microphone.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                auto& app = Application::GetInstance();
                if (guard_mode_active_ || !app.StartControlListening())
                    return std::unexpected("Robot đang bận.");
                if (display_)
                    display_->HideStandbyClock(true);
                return true;
            });

        mcp_server.AddTool(
            "self.screen.show_wifi_qr",
            "Mở một mã QR cho trang cấu hình Wi-Fi gốc của Xiaozhi. Jarvis tạm ngắt. Dùng cho 'mã "
            "QR Wi-Fi', "
            "'mã QR trang cấu hình Wi-Fi', 'mở hotspot' hoặc 'mở trang cấu hình'.",
            PropertyList({Property("duration_seconds", kPropertyTypeInteger, 180, 10, 600)}),
            [this](const PropertyList& properties) {
                return ShowWifiQr(properties["duration_seconds"].value<int>());
            });

        mcp_server.AddTool(
            "self.screen.close_qr",
            "Đóng mã QR đang hiện và dừng hotspot tạm. Dùng khi người dùng bảo 'tắt mã QR', "
            "'đóng mã QR' hoặc 'ẩn mã QR'.",
            PropertyList(), [this](const PropertyList&) { return CloseQr(); });

        mcp_server.AddTool(
            "self.screen.show_qr",
            "Chỉ tạo QR cho URL HTTP hoặc HTTPS cụ thể do người dùng đưa. Nếu họ chỉ nói 'mở mã "
            "QR', "
            "hãy hỏi muốn QR Wi-Fi, QR trang điều khiển hay QR của đường dẫn nào.",
            PropertyList({Property("url", kPropertyTypeString, ""),
                          Property("duration_seconds", kPropertyTypeInteger, 180, 10, 600)}),
            [this](const PropertyList& properties) -> ToolResult {
                const auto url = properties["url"].value<std::string>();
                const int duration = properties["duration_seconds"].value<int>();
                if (url.empty())
                    return std::unexpected(
                        "Bạn muốn QR Wi-Fi, QR trang điều khiển hay QR của đường dẫn nào?");
                if (!url.starts_with("https://") && !url.starts_with("http://"))
                    return std::unexpected("QR trang web cần địa chỉ HTTP hoặc HTTPS.");
                if (!CloseQr() || !display_->TryShowQrCode(url, "QUET MA QR", duration * 1000))
                    return std::unexpected("Không thể hiển thị QR.");
                return std::string("Đã hiển thị QR trang web.");
            });
        mcp_server.AddTool(
            "self.screen.set_emotion",
            "Đổi biểu cảm TFT khi người dùng trực tiếp yêu cầu. Không gọi mỗi lượt trò chuyện. "
            "Tên thường dùng: happy, sad, angry, furious, side_eye, facepalm, smug, love, wink, "
            "neutral. Dùng shy cho ngại hoặc ngượng ngùng, zen cho thiền hoặc tĩnh tâm.",
            PropertyList({Property("emotion", kPropertyTypeString),
                          Property("duration_seconds", kPropertyTypeInteger, 8, 1, 15)}),
            [this](const PropertyList& properties) -> ToolResult {
                if (!display_ || !display_->SetManualEmotion(
                                     properties["emotion"].value<std::string>(),
                                     properties["duration_seconds"].value<int>() * 1000)) {
                    return std::unexpected("Biểu cảm không hợp lệ hoặc màn hình chưa sẵn sàng");
                }
                return ReturnValue(true);
            });
        mcp_server.AddTool("self.screen.set_default_emotion",
            "Lưu biểu cảm mặc định khi robot chờ hoặc lắng nghe. Dùng tên biểu cảm như happy, sad, shy, neutral. "
            "Khi người dùng yêu cầu hủy mặt mặc định hoặc về chế độ bình thường, truyền emotion=auto.",
            PropertyList({Property("emotion", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ToolResult {
                if (!display_ || !display_->SetDefaultEmotion(properties["emotion"].value<std::string>()))
                    return std::unexpected("Chọn tên biểu cảm hợp lệ hoặc auto.");
                return ReturnValue(true);
            });
    }

    void InitializeSerialCli() {
        // The polling console must deliver short lines without waiting for stdio's
        // input buffer to fill with a later command.
        setvbuf(stdin, nullptr, _IONBF, 0);
        xTaskCreate(
            [](void* arg) {
                auto* display = static_cast<PocketLcdDisplay*>(arg);
                char buf[1024];
                int idx = 0;
                bool overflow = false;
                while (true) {
                    int c = getchar();
                    if (c != EOF && c > 0) {
                        if (c == '\r' || c == '\n') {
                            if (overflow) {
                                idx = 0;
                                overflow = false;
                                ESP_LOGW(TAG, "Serial CLI input too long; discarded");
                                continue;
                            }
                            if (idx > 0) {
                                buf[idx] = '\0';
                                if (buf[0] == '{') {
                                    // Exercise the same validated dispatcher as Cloud, with replies
                                    // sent only to USB serial. DoToolCall schedules mutations on
                                    // main.
                                    McpServer::GetInstance().ParseMessage(
                                        std::string(buf), [](const std::string& response) {
                                            printf("MCPCLI %s\n", response.c_str());
                                            fflush(stdout);
                                        });
                                } else if (display != nullptr) {
                                    ESP_LOGI(TAG, "Serial CLI display command");
                                    if (std::strncmp(buf, "qr", 2) == 0) {
                                        std::string url = (std::strlen(buf) > 3)
                                                              ? (buf + 3)
                                                              : "http://192.168.1.100";
                                        display->ShowQrCode(url, "TEST QR CODE", 30000);
                                    } else if (std::strcmp(buf, "standby") == 0) {
                                        display->ShowStandbyClock(true);
                                    } else if (std::strcmp(buf, "wake") == 0) {
                                        display->HideStandbyClock(true);
                                    } else if (std::strcmp(buf, "face_perf") == 0) {
                                        display->LogFacePerformance();
                                    } else if (std::strncmp(buf, "play ", 5) == 0 ||
                                               std::strncmp(buf, "speak ", 6) == 0) {
                                        const bool music = buf[0] == 'p';
                                        std::string play_url = buf + (music ? 5 : 6);
                                        if (music)
                                            display->SetEmotion("music");
                                        auto& app = Application::GetInstance();
                                        app.Schedule([&app, play_url, music]() {
                                            app.StartNotification(play_url, {}, music);
                                        });
                                    } else {
                                        const char* cmd = buf;
                                        if (std::strncmp(cmd, "face ", 5) == 0) {
                                            cmd += 5;
                                        }
                                        if (!display->SetManualEmotion(cmd)) {
                                            ESP_LOGW(TAG, "Serial CLI unsupported emotion");
                                        }
                                    }
                                }
                                idx = 0;
                            }
                        } else if (idx < static_cast<int>(sizeof(buf)) - 1) {
                            buf[idx++] = static_cast<char>(c);
                        } else {
                            overflow = true;
                        }
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
            },
            "serial_cli_lcd", 4096, display_, 1, nullptr);
    }

    ServiceStatus CheckMusicStatus(std::string url) {
        const auto path = url.find("/fleet/ota/");
        if (path == std::string::npos || url.find_first_of("?#", path) != std::string::npos)
            return ServiceStatus::Unverified;
        while (url.ends_with('/'))
            url.pop_back();
        auto http = GetNetwork()->CreateHttp(0);
        http->SetTimeout(3000);
        http->SetHeader("Client-Id", GetUuid());
        auto& app = Application::GetInstance();
        const auto report = app.MusicReport();
        http->SetHeader("X-Music-Report", std::to_string(report));
        http->SetHeader("X-Music-Choice-Ack", "1");
        if (!http->Open("GET", url + "/health"))
            return ServiceStatus::Unverified;
        const auto code = http->GetStatusCode();
        const auto size = http->GetBodyLength();
        const auto result =
            code && *code == 200 && size > 0 && size <= 15 ? http->ReadAll() : std::string();
        http->Close();
        if (result == "online-choice") {
            app.Schedule([id = static_cast<int>(report >> 4)]() {
                Application::GetInstance().AwaitMusicChoice(id);
            });
            return ServiceStatus::Online;
        }
        if (result == "online")
            return ServiceStatus::Online;
        if (result == "connecting")
            return ServiceStatus::Connecting;
        if (result == "offline")
            return ServiceStatus::Offline;
        return ServiceStatus::Unverified;
    }

    ToolResult CheckConnections(const std::string& service) {
        if (service != "wifi" && service != "ota" && service != "music" && service != "all")
            return std::unexpected("Chọn wifi, ota, music hoặc all.");
        auto& wifi = WifiManager::GetInstance();
        const auto ip = wifi.IsConnected() ? wifi.GetIpAddress() : std::string();
        const bool connected = !ip.empty() && ip != "0.0.0.0";
        if (!connected)
            return std::string("Robot chưa có kết nối Wi-Fi, nên chưa thể kiểm tra các máy chủ.");
        std::string reply;
        if (service == "wifi" || service == "all") {
            reply = "Wi-Fi đã kết nối, địa chỉ IP " + ip + ", tín hiệu " +
                    std::to_string(wifi.GetRssi()) + " dBm.";
        }
        if (service == "ota" || service == "all") {
            Ota check;
            auto result = check.CheckVersion(Ota::GetCheckVersionUrl(), false);
            display_->SetServiceStatus(DisplayService::Ota,
                                       result ? ServiceStatus::Online : ServiceStatus::Offline);
            if (!reply.empty())
                reply += ' ';
            if (!result)
                reply += "Không nhận được phản hồi OTA hợp lệ.";
            else if (check.HasNewVersion())
                reply += "OTA phản hồi, có bản " + check.GetFirmwareVersion() + ".";
            else
                reply += "OTA phản hồi, hiện không có bản mới.";
        }
        if (service == "music" || service == "all") {
            const auto state = CheckMusicStatus(Ota::GetCheckVersionUrl());
            display_->SetServiceStatus(DisplayService::MusicMcp, state);
            if (!reply.empty())
                reply += ' ';
            switch (state) {
                case ServiceStatus::Online:
                    reply += "Fleet xác nhận MCP Music đang kết nối.";
                    break;
                case ServiceStatus::Connecting:
                    reply += "MCP Music đang kết nối lại.";
                    break;
                case ServiceStatus::Offline:
                    reply += "MCP Music đang mất kết nối với Xiaozhi Cloud.";
                    break;
                default:
                    reply += "Chưa xác minh được MCP Music qua Fleet.";
                    break;
            }
        }
        return reply;
    }

    void StartMusicStatusMonitor() {
        if (xTaskCreate(
                [](void* arg) {
                    auto& board = *static_cast<CompactWifiBoardLCD*>(arg);
                    int64_t last_probe_us = esp_timer_get_time();
                    while (true) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        if (!WifiManager::GetInstance().IsConnected())
                            continue;
                        const auto now = esp_timer_get_time();
                        const auto phase = Application::GetInstance().ReportedMusicPhase();
                        const auto interval = phase == MusicPhase::Searching ? 2000000 : 15000000;
                        if (now - last_probe_us < interval)
                            continue;
                        last_probe_us = now;
                        board.display_->SetServiceStatus(
                            DisplayService::MusicMcp,
                            board.CheckMusicStatus(Ota::GetCheckVersionUrl()));
                    }
                },
                "mcp_health", 4096, this, 1, nullptr) != pdPASS) {
            display_->SetServiceStatus(DisplayService::MusicMcp, ServiceStatus::Unverified);
        }
    }

public:
    bool SupportsMusicRequests() const override { return true; }

    size_t ReservedAssetsTail() const override {
        return display_ && display_->width() == 240 && display_->height() == 240
                   ? ExpressionPack::kReserved : 0;
    }

    std::string GetBoardJson() override {
        auto json = WifiBoard::GetBoardJson();
        if (ReservedAssetsTail() && ExpressionPack::Partition()) {
            json.pop_back();
            json += ",\"expression_protocol\":2,\"expression_digest\":\"" +
                    display_->ExpressionDigest() + "\"}";
        }
        return json;
    }

    void ApplyFleetExpressions(const std::string& url, const std::string& digest) override {
        if (!ReservedAssetsTail() || !ExpressionPack::Partition() ||
            digest == display_->ExpressionDigest())
            return;
        display_->ReloadExpressions(false);
        const bool installed = ExpressionPack::Download(url, digest);
        display_->ReloadExpressions(true);
        if (!installed)
            ESP_LOGW(TAG, "Expression asset download failed");
    }

    CompactWifiBoardLCD() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
#ifdef CONFIG_BREAD_COMPACT_LCD_KCX
        GetAudioCodec();  // Initialize power, LINK and the saved preferences before tools are used.
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.bluetooth.status",
                    "Đọc trạng thái loa Bluetooth và danh sách thiết bị mới quét. Chỉ xác nhận kết nối khi connected=true. Kiểm tra lại khi thao tác trước đang chạy.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        return kcx_.Status();
                    });
        mcp.AddTool("self.bluetooth.power", "Bật hoặc tắt nguồn bộ phát Bluetooth KCX.",
                    PropertyList({Property("enable", kPropertyTypeBoolean)}),
                    [this](const PropertyList& args) -> ToolResult {
                        if (!kcx_.Power(args["enable"].value<bool>()))
                            return std::unexpected("Không khởi động được worker Bluetooth.");
                        return std::string("Đã nhận lệnh nguồn. Kiểm tra self.bluetooth.status để xác nhận.");
                    });
        mcp.AddTool("self.bluetooth.device",
                    "Điều khiển loa Bluetooth. action là scan, connect hoặc disconnect. target là MAC, tên trùng duy nhất trong kết quả quét, hoặc rỗng để nối lại loa từng xác nhận. Chỉ báo đã kết nối sau khi status.connected=true.",
                    PropertyList({Property("action", kPropertyTypeString),
                                  Property("target", kPropertyTypeString, "")}),
                    [this](const PropertyList& args) -> ToolResult {
                        if (!kcx_.Request(args["action"].value<std::string>(),
                                          args["target"].value<std::string>()))
                            return std::unexpected("Kiểm tra trạng thái Bluetooth hoặc chọn đúng tên hay MAC của loa.");
                        return std::string("Đã nhận yêu cầu. Dùng self.bluetooth.status để xem kết quả.");
                    });
        mcp.AddTool("self.bluetooth.route",
                    "Chọn loa Bluetooth riêng cho giọng robot hoặc nhạc. Truyền voice và music, mỗi mục là bluetooth, local hoặc keep. Khi mất loa Bluetooth, robot dùng loa tích hợp.",
                    PropertyList({Property("voice", kPropertyTypeString, "keep"),
                                  Property("music", kPropertyTypeString, "keep")}),
                    [this](const PropertyList& args) -> ToolResult {
                        const auto voice = args["voice"].value<std::string>();
                        const auto music = args["music"].value<std::string>();
                        const auto valid = [](const std::string& value) {
                            return value == "local" || value == "bluetooth" || value == "keep";
                        };
                        if (!valid(voice) || !valid(music))
                            return std::unexpected("Chọn local, bluetooth hoặc keep.");
                        CJsonUniquePtr state(kcx_.Status());
                        kcx_.SetRoutes(voice == "bluetooth" || (voice == "keep" &&
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "voice"))),
                            music == "bluetooth" || (music == "keep" &&
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "music"))));
                        return std::string("Đã lưu đường âm thanh. Kiểm tra trạng thái để biết loa đang kết nối.");
                    });
#endif
        InitializeSerialCli();
        StartMusicStatusMonitor();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef CONFIG_BREAD_COMPACT_LCD_KCX
        static KcxCodec audio_codec(kcx_);
#elif defined(AUDIO_I2S_METHOD_SIMPLEX)
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(CompactWifiBoardLCD);
