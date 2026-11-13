#include "wifi_board.h"
#include <wifi_manager.h>
#include "codecs/no_audio_codec.h"
#include "pocket_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

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
#include <string>
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
    PocketLcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // Timers & States for 7 MCP Tools
    esp_timer_handle_t countdown_timer_ = nullptr;
    esp_timer_handle_t guard_timer_ = nullptr;
    esp_timer_handle_t workout_timer_ = nullptr;
    bool guard_mode_active_ = false;
    int64_t guard_arm_until_us_ = 0;
    int64_t countdown_deadline_us_ = 0;
    int64_t workout_deadline_us_ = 0;
    std::string countdown_label_;
    std::atomic_bool guard_poll_pending_{false};
    std::atomic_bool workout_poll_pending_{false};
    int guard_noise_level_ = 0;
    int guard_hits_ = 0;
    int workout_remaining_sec_ = 0;

    ToolResult ShowConfigQr(int duration) {
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConfigMode()) {
            if (!display_ || !display_->TryShowQrCode(wifi.GetApWebUrl(), wifi.GetApSsid(),
                                                     duration * 1000)) {
                return std::unexpected("Không thể hiển thị QR cấu hình.");
            }
            return std::string("Kết nối Wi-Fi của robot rồi quét QR để mở trang cấu hình.");
        }

        auto& app = Application::GetInstance();
        if (app.IsAlarmActive())
            return std::unexpected("Gọi Jarvis để xác nhận báo động trước khi mở cấu hình.");
        app.Schedule([this]() { EnterWifiConfigMode(); });
        return std::string("Robot sẽ mở Wi-Fi Xiaozhi. Kết nối mạng hiện trên màn hình rồi quét QR.");
#else
        return std::unexpected("Bản firmware này không hỗ trợ cấu hình Wi-Fi bằng hotspot.");
#endif
    }

    void RaiseAlarm(bool guard) {
        auto& app = Application::GetInstance();
        const bool started = guard
            ? app.StartLocalAlarm({Lang::Sounds::OGG_GUARD_ALERT_VI,
                                   Lang::Sounds::OGG_GUARD_REMINDER_2_VI,
                                   Lang::Sounds::OGG_GUARD_REMINDER_3_VI})
            : app.StartLocalAlarm({Lang::Sounds::OGG_TIMER_ALERT_VI,
                                   Lang::Sounds::OGG_TIMER_REMINDER_2_VI,
                                   Lang::Sounds::OGG_TIMER_REMINDER_3_VI});
        if (display_) {
            display_->SetManualEmotion(guard ? "furious" : "thunder", 15000);
            display_->ShowNotification(
                guard ? "[CANH BAO] TIENG DONG BAT THUONG" : countdown_label_, 15000);
        }
        ESP_LOGI(TAG, "Local alarm kind=%s accepted=%d", guard ? "guard" : "timer", started);
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
            RaiseAlarm(true);
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
            if (display_ != nullptr) {
                auto* p_disp = static_cast<PocketLcdDisplay*>(display_);
                p_disp->HideStandbyClock(true);
                p_disp->HideQrCode(true);
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
#if CONFIG_BREAD_LCD_LAMP_ENABLED
        static LampController lamp(LAMP_GPIO);
#endif
        auto& mcp_server = McpServer::GetInstance();

        // 1. TOOL ĐẾM NGƯỢC / BÁO THỨC POMODORO TRỰC TIẾP TRÊN MẠCH
        mcp_server.AddTool(
            "self.timer.set_countdown",
            "Đặt bộ đếm ngược thời gian hoặc hẹn giờ báo thức (tính bằng giây). Khi hết giờ, robot sẽ tự động phát âm thanh báo động và đổi biểu cảm trên màn hình. Truyền seconds = 0 nếu muốn hủy hẹn giờ.",
            PropertyList({
                Property("seconds", kPropertyTypeInteger, 60, 0, 86400),
                Property("label", kPropertyTypeString, "Hết giờ rồi!")
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int seconds = properties["seconds"].value<int>();
                std::string label = properties["label"].value<std::string>();

                if (countdown_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                            Application::GetInstance().Schedule([board]() {
                                if (board->countdown_deadline_us_ == 0 ||
                                    esp_timer_get_time() < board->countdown_deadline_us_)
                                    return;
                                board->countdown_deadline_us_ = 0;
                                if (board->display_)
                                    board->display_->SetCountdownTimer(0, "IDLE");
                                board->RaiseAlarm(false);
                            });
                        },
                        .arg = this,
                        .dispatch_method = ESP_TIMER_TASK,
                        .name = "pomodoro_timer",
                        .skip_unhandled_events = true
                    };
                    esp_timer_create(&timer_args, &countdown_timer_);
                } else {
                    esp_timer_stop(countdown_timer_);
                }

                countdown_label_ = label;
                countdown_deadline_us_ =
                    seconds > 0 ? esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000
                                : 0;
                if (seconds <= 0) {
                    if (display_ != nullptr) {
                        display_->SetCountdownTimer(0, "IDLE");
                        display_->ShowNotification("Đã hủy hẹn giờ", 3000);
                    }
                    return "Đã hủy bộ đếm ngược";
                }

                esp_timer_start_once(countdown_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);

                if (display_ != nullptr) {
                    display_->SetCountdownTimer(seconds, label);
                    std::string notify_msg = "[HEN GIO] " + std::to_string(seconds) + "s: " + label;
                    display_->ShowNotification(notify_msg.c_str(), 5000);
                    display_->SetEmotion("target");
                }
                return "Đã bật đếm ngược " + std::to_string(seconds) + " giây cho: " + label + "";
            }
        );

        // 2. TOOL GHIM GHI CHÚ / HIỂN THỊ IP, WIFI, MÃ QR
        mcp_server.AddTool(
            "self.screen.pin_memo",
            "Ghim ghi chú lên màn hình. ĐẶC BIỆT: Khi người dùng hỏi hoặc muốn xem IP, WiFi hoặc mã QR cấu hình, hãy gọi tool này với text='ip', 'wifi' hoặc 'qr' để phần cứng tự động trích xuất thông tin thật và hiển thị lên màn hình đồng thời trả về kết quả cho bạn.",
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
                    lower_text.find("man hinh cho") != std::string::npos ||
                    lower_text.find("màn hình chờ") != std::string::npos ||
                    lower_text.find("che do cho") != std::string::npos ||
                    lower_text.find("chế độ chờ") != std::string::npos ||
                    lower_text.find("dong ho") != std::string::npos ||
                    lower_text.find("đồng hồ") != std::string::npos) {
                    Application::GetInstance().Schedule(
                        []() { Application::GetInstance().EndConversation(); });
                    if (display_ != nullptr) {
                        display_->ShowStandbyClock(true /* manual_lock */);
                    }
                    return "Đã hiển thị đồng hồ. Gọi Jarvis để trò chuyện tiếp.";
                }

                // TRƯỜNG HỢP 1: Yêu cầu hiển thị Mã QR
                if (lower_text.find("qr") != std::string::npos) {
                    return ShowConfigQr(duration);
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
            PropertyList({Property("url", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string url = properties["url"].value<std::string>();
                if (url.empty()) {
                    return "Lỗi: url không được để trống";
                }
                auto& app = Application::GetInstance();
                const bool accepted = app.StartNotification(url, {});
                if (accepted && display_)
                    display_->SetEmotion("music");
                return accepted ? "Đã tiếp nhận luồng nhạc; đang kết nối."
                                : "Không thể bắt đầu luồng nhạc.";
            }
        );

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
                app.Schedule([this, &app]() {
                    if (app.IsAlarmActive())
                        return;
                    app.StopNotification();
                    if (display_ != nullptr) {
                        display_->SetEmotion("neutral");
                    }
                });
                return "Đã dừng phát âm thanh";
            }
        );

        // 4. TOOL CANH TRỘM PHÒNG BẰNG CẢM BIẾN ÂM THANH MIC (SENTRY MODE)
        mcp_server.AddTool(
            "self.security.guard_mode",
            "Bật hoặc tắt chế độ canh gác phòng (Sentry Mode). Khi bật, robot sẽ quét âm thanh "
            "trong phòng, sau 6 giây ổn định sẽ theo dõi tiếng động; khi vượt ngưỡng sẽ báo bằng "
            "còi và lời nói rồi tự tắt.",
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
                    esp_timer_create(&timer_args, &guard_timer_);
                }
                esp_timer_start_periodic(guard_timer_, 50 * 1000); // Kiểm tra mỗi 50ms bắt trúng đỉnh xung vỗ tay!
                return "Đã kích hoạt chế độ canh gác phòng bằng cảm biến âm thanh";
            });

        // 5. TOOL ĐẾM GIÂY ĐU XÀ / WORKOUT (BÍP MỖI 10 GIÂY + CẢNH BÁO TIẾN ĐỘ)
        mcp_server.AddTool(
            "self.fitness.start_workout",
            "Bắt đầu bộ đếm thời gian tập thể dục, đu xà đơn, plank (tính bằng giây). Robot sẽ hiển thị tiến trình và phát tiếng bíp mỗi 10 giây để báo tiến độ.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 45, 5, 600)}),
            [this](const PropertyList& properties) -> ReturnValue {
                workout_remaining_sec_ = properties["seconds"].value<int>();
                workout_deadline_us_ =
                    esp_timer_get_time() + static_cast<int64_t>(workout_remaining_sec_) * 1000000;
                if (workout_timer_ != nullptr) {
                    esp_timer_stop(workout_timer_);
                }

                if (workout_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                            if (board->workout_poll_pending_.exchange(true))
                                return;
                            Application::GetInstance().Schedule([board]() {
                                const int rem = static_cast<int>(std::max<int64_t>(
                                    0,
                                    (board->workout_deadline_us_ - esp_timer_get_time() + 999999) /
                                        1000000));
                                board->workout_remaining_sec_ = rem;
                                if (rem == 0) {
                                    esp_timer_stop(board->workout_timer_);
                                    board->countdown_label_ = "[TAP LUYEN] HOAN THANH";
                                    board->RaiseAlarm(false);
                                } else if (board->display_) {
                                    board->display_->ShowNotification(
                                        "[TAP LUYEN] Con " + std::to_string(rem) + "s", 1500);
                                }
                                board->workout_poll_pending_.store(false);
                            });
                        },
                        .arg = this,
                        .dispatch_method = ESP_TIMER_TASK,
                        .name = "workout_timer",
                        .skip_unhandled_events = true
                    };
                    esp_timer_create(&timer_args, &workout_timer_);
                }
                esp_timer_start_periodic(workout_timer_, 1000 * 1000);
                if (display_ != nullptr) {
                    display_->SetEmotion("excited");
                }
                return "Đã bắt đầu đếm hiệp tập " + std::to_string(workout_remaining_sec_) +
                       " giây";
            }
        );

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
            "Chuyển robot sang chế độ Màn hình chờ đồng hồ (Standby Clock) hoặc thoát khỏi màn hình chờ về khuôn mặt robot. Khi người dùng nói 'quay về màn hình chờ', 'vào màn hình chờ', 'màn hình chờ đi', 'đồng hồ', hãy gọi tool này với enable=true.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
                if (enable) {
                    Application::GetInstance().Schedule(
                        []() { Application::GetInstance().EndConversation(); });
                }
                if (display_ != nullptr) {
                    auto* p_disp = static_cast<PocketLcdDisplay*>(display_);
                    if (enable) {
                        p_disp->ShowStandbyClock(true /* manual_lock */);
                    } else {
                        p_disp->HideStandbyClock(true /* force */);
                    }
                }
                return enable ? "Đã hiển thị đồng hồ. Gọi Jarvis để trò chuyện tiếp."
                              : "Đã đánh thức robot trở lại trạng thái sẵn sàng";
            }
        );

        mcp_server.AddTool(
            "self.screen.show_qr",
            "Mở hotspot cấu hình Wi-Fi và hiển thị QR dẫn tới trang cấu hình nếu url rỗng. "
            "Chỉ truyền url khi người dùng đưa một địa chỉ web cụ thể.",
            PropertyList({
                Property("url", kPropertyTypeString, ""),
                Property("duration_seconds", kPropertyTypeInteger, 60, 10, 600)
            }),
            [this](const PropertyList& properties) -> ToolResult {
                const auto url = properties["url"].value<std::string>();
                const int duration = properties["duration_seconds"].value<int>();
                if (url.empty() || url == "config" || url == "wifi" || url == "qr" ||
                    url == "http://192.168.4.1" || url == "http://192.168.4.1/") {
                    return ShowConfigQr(duration);
                }
                if (!url.starts_with("https://") && !url.starts_with("http://")) {
                    return std::unexpected("QR trang web cần địa chỉ HTTP hoặc HTTPS.");
                }
                if (!display_ || !display_->TryShowQrCode(url, "QUET MA QR", duration * 1000)) {
                    return std::unexpected("Không thể hiển thị QR.");
                }
                return std::string("Đã hiển thị QR trang web.");
            }
        );
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
                                    } else if (std::strncmp(buf, "play ", 5) == 0) {
                                        std::string play_url = buf + 5;
                                        display->SetEmotion("music");
                                        auto& app = Application::GetInstance();
                                        app.Schedule([&app, play_url]() {
                                            app.StartNotification(play_url, {});
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

public:
    CompactWifiBoardLCD() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        InitializeSerialCli();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
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
