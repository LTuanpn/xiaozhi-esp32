#include "wifi_board.h"
#include <wifi_manager.h>
#include <ssid_manager.h>
#include "codecs/no_audio_codec.h"
#include "pocket_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <vector>
#include <string>

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
    int workout_remaining_sec_ = 0;

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
        io_config.pclk_hz = 40 * 1000 * 1000;
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
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel_));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel_));
#endif
        
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new PocketLcdDisplay(panel_io, panel_,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (display_ != nullptr) { display_->HideStandbyClock(); display_->HideQrCode(); }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
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
                                auto* disp = board->GetDisplay();
                                if (disp != nullptr) {
                                    disp->SetEmotion("thunder");
                                    disp->ShowNotification("[BAO THUC] HET GIO ROI DAI CA!", 15000);
                                }
                                Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
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

                if (seconds <= 0) {
                    if (display_ != nullptr) {
                        display_->ShowNotification("Đã hủy hẹn giờ", 3000);
                    }
                    return "Đã hủy bộ đếm ngược";
                }

                esp_timer_start_once(countdown_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);

                if (display_ != nullptr) {
                    std::string notify_msg = "[HEN GIO] " + std::to_string(seconds) + "s: " + label;
                    display_->ShowNotification(notify_msg.c_str(), 5000);
                    display_->SetEmotion("target");
                }
                return "Đã bật đếm ngược " + std::to_string(seconds) + " giây cho: " + label;
            }
        );

        // 2. TOOL GHIM GHI CHÚ / HIỂN THỊ IP, WIFI, MẬT KHẨU, MÃ QR
        mcp_server.AddTool(
            "self.screen.pin_memo",
            "Ghim ghi chú lên màn hình. ĐẶC BIỆT: Khi người dùng hỏi hoặc muốn xem IP, WiFi, mật khẩu hoặc mã QR cấu hình, hãy gọi tool này với text='ip', 'wifi' hoặc 'qr' để phần cứng tự động trích xuất thông tin thật và hiển thị lên màn hình đồng thời trả về kết quả cho bạn.",
            PropertyList({
                Property("text", kPropertyTypeString, ""),
                Property("duration_seconds", kPropertyTypeInteger, 600, 10, 3600)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string text = properties["text"].value<std::string>();
                int duration = properties["duration_seconds"].value<int>();

                // Chuẩn hóa text sang chữ thường để phát hiện từ khóa
                std::string lower_text = text;
                std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);

                // TRƯỜNG HỢP 0: Yêu cầu chuyển sang Màn hình chờ (Standby Clock)
                if (lower_text == "standby" || lower_text == "dong_ho" || lower_text == "clock" ||
                    lower_text.find("man hinh cho") != std::string::npos ||
                    lower_text.find("màn hình chờ") != std::string::npos ||
                    lower_text.find("che do cho") != std::string::npos ||
                    lower_text.find("chế độ chờ") != std::string::npos ||
                    lower_text.find("dong ho") != std::string::npos ||
                    lower_text.find("đồng hồ") != std::string::npos) {
                    if (display_ != nullptr) {
                        display_->ShowStandbyClock();
                    }
                    return "Đã chuyển sang màn hình chờ đồng hồ. Bạn có thể gọi 'Jarvis' bất kỳ lúc nào để đánh thức tôi.";
                }

                // TRƯỜNG HỢP 1: Yêu cầu hiển thị Mã QR
                if (lower_text.find("qr") != std::string::npos) {
                    auto& wifi = WifiManager::GetInstance();
                    std::string url;
                    if (wifi.IsConfigMode()) {
                        url = "http://192.168.4.1";
                    } else {
                        std::string ip = wifi.GetIpAddress();
                        url = ip.empty() ? "http://192.168.4.1" : ("http://" + ip);
                    }
                    if (display_ != nullptr) {
                        display_->ShowQrCode(url, "QUET MA CAU HINH", duration * 1000);
                    }
                    return "Đã hiển thị mã QR cấu hình lên màn hình: " + url + ". Người dùng có thể quét bằng điện thoại.";
                }

                // TRƯỜNG HỢP 2: Yêu cầu thông tin IP / WiFi / Mật khẩu
                if (lower_text.find("ip") != std::string::npos || lower_text.find("wifi") != std::string::npos ||
                    lower_text.find("mat_khau") != std::string::npos || lower_text.find("mật khẩu") != std::string::npos ||
                    lower_text.find("password") != std::string::npos || lower_text.find("mang") != std::string::npos ||
                    lower_text.find("mạng") != std::string::npos || text.empty()) {
                    auto& wifi = WifiManager::GetInstance();
                    std::string ssid = wifi.GetSsid();
                    std::string ip = wifi.GetIpAddress();
                    int rssi = wifi.GetRssi();
                    std::string pwd = "(Khong co mat khau)";

                    auto& ssid_mgr = SsidManager::GetInstance();
                    for (const auto& item : ssid_mgr.GetSsidList()) {
                        if (item.ssid == ssid && !item.password.empty()) {
                            pwd = item.password;
                            break;
                        }
                    }

                    if (display_ != nullptr) {
                        std::string info_disp = "[WIFI] IP: " + (ip.empty() ? "Chua co IP" : ip) +
                                                " | WF: " + (ssid.empty() ? "Chua ket noi" : ssid) +
                                                " | MK: " + pwd;
                        display_->ShowNotification(info_disp.c_str(), duration * 1000);
                        display_->SetEmotion("idea");
                    }

                    return "Thông tin mạng phần cứng hiện tại: WiFi = " + (ssid.empty() ? "Chưa kết nối" : ssid) +
                           ", Mật khẩu = " + pwd +
                           ", Địa chỉ IP = " + (ip.empty() ? "Chưa nhận IP" : ip) +
                           ", Cường độ sóng = " + std::to_string(rssi) + "dBm. Đã ghim thông tin lên màn hình.";
                }

                // TRƯỜNG HỢP 3: Ghim ghi chú văn bản thông thường
                if (display_ != nullptr) {
                    std::string pinned = "[GHI CHU] " + text;
                    display_->ShowNotification(pinned.c_str(), duration * 1000);
                    display_->SetEmotion("idea");
                }
                return "Đã ghim nội dung lên màn hình: " + text;
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

        // 4. TOOL CANH TRỘM PHÒNG BẰNG CẢM BIẾN ÂM THANH MIC (SENTRY MODE)
        mcp_server.AddTool(
            "self.security.guard_mode",
            "Bật hoặc tắt chế độ canh gác phòng (Sentry Mode). Khi bật, robot sẽ quét âm thanh trong phòng, nếu phát hiện tiếng động lạ sẽ hú còi báo động.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ReturnValue {
                guard_mode_active_ = properties["enable"].value<bool>();
                if (guard_timer_ != nullptr) {
                    esp_timer_stop(guard_timer_);
                }

                if (!guard_mode_active_) {
                    if (display_ != nullptr) {
                        display_->SetEmotion("neutral");
                        display_->ShowNotification("[CANH GAC] Da tat canh phong", 3000);
                    }
                    return "Đã tắt chế độ canh phòng";
                }

                // Chuyển bot sang Idle ngay lập tức để không kẹt ở Listening
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);

                if (display_ != nullptr) {
                    display_->SetEmotion("scanning");
                    display_->ShowNotification("[CANH GAC] SENTRY MODE: ON", 60000);
                }

                if (guard_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                            auto& app = Application::GetInstance();
                            if (board->guard_mode_active_) {
                                uint8_t mic_lvl = app.GetAudioService().GetInputLevel();
                                if (mic_lvl > 25) { // Bắt xung tiếng vỗ tay / bước chân nhạy bén!
                                    board->guard_mode_active_ = false;
                                    esp_timer_stop(board->guard_timer_);
                                    app.Schedule([board, &app]() {
                                        if (board->GetDisplay() != nullptr) {
                                            board->GetDisplay()->SetEmotion("furious");
                                            board->GetDisplay()->ShowNotification("[CANH BAO] PHAT HIEN XAM NHAP!", 10000);
                                        }
                                        // Còi hú báo động reo to dồn dập
                                        app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                                    });
                                }
                            }
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
            }
        );

        // 5. TOOL ĐẾM GIÂY ĐU XÀ / WORKOUT (BÍP MỖI 10 GIÂY + CẢNH BÁO TIẾN ĐỘ)
        mcp_server.AddTool(
            "self.fitness.start_workout",
            "Bắt đầu bộ đếm thời gian tập thể dục, đu xà đơn, plank (tính bằng giây). Robot sẽ hiển thị tiến trình và phát tiếng bíp mỗi 10 giây để báo tiến độ.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 45, 5, 600)}),
            [this](const PropertyList& properties) -> ReturnValue {
                workout_remaining_sec_ = properties["seconds"].value<int>();
                if (workout_timer_ != nullptr) {
                    esp_timer_stop(workout_timer_);
                }

                if (workout_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoardLCD*>(arg);
                            auto& app = Application::GetInstance();
                            board->workout_remaining_sec_--;
                            int rem = board->workout_remaining_sec_;

                            if (rem <= 0) {
                                esp_timer_stop(board->workout_timer_);
                                app.Schedule([board, &app]() {
                                    if (board->GetDisplay() != nullptr) {
                                        board->GetDisplay()->SetEmotion("party");
                                        board->GetDisplay()->ShowNotification("[CHIEN THANG] HOAN THANH XUAT SAC!", 10000);
                                    }
                                    app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                                });
                                return;
                            }

                            app.Schedule([board, &app, rem]() {
                                if (board->GetDisplay() != nullptr) {
                                    std::string msg = "[TAP LUYEN] Con " + std::to_string(rem) + "s";
                                    board->GetDisplay()->ShowNotification(msg.c_str(), 1500);
                                    if (rem <= 5) {
                                        board->GetDisplay()->SetEmotion("fire");
                                    }
                                }
                                if (rem % 10 == 0 || rem <= 3) {
                                    app.PlaySound(Lang::Sounds::OGG_POPUP);
                                }
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
                return "Đã bắt đầu đếm hiệp tập " + std::to_string(workout_remaining_sec_) + " giây";
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
                return "Kết quả bốc thăm ngẫu nhiên từ phần cứng là: " + chosen;
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
                    if (display_ != nullptr) {
                        display_->SetEmotion("dizzy");
                        display_->ShowNotification("[DONG XU] Dang quay...", 1500);
                    }
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    bool is_heads = (esp_random() % 2) == 1;
                    std::string outcome = is_heads ? "[DONG XU] NGUA (Heads)" : "[DONG XU] SAP (Tails)";
                    if (display_ != nullptr) {
                        display_->SetEmotion(is_heads ? "excited" : "smug");
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
                    return "Kết quả tung đồng xu: " + outcome;
                } else {
                    if (display_ != nullptr) {
                        display_->SetEmotion("dizzy");
                        display_->ShowNotification("[XUC XAC] Dang lac...", 1500);
                    }
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                    vTaskDelay(pdMS_TO_TICKS(1200));

                    int roll = static_cast<int>((esp_random() % sides) + 1);
                    std::string outcome = "[XUC XAC] " + std::to_string(roll) + " / d" + std::to_string(sides);
                    if (display_ != nullptr) {
                        display_->SetEmotion(roll == sides ? "party" : (roll == 1 ? "facepalm" : "smug"));
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(roll == sides ? Lang::Sounds::OGG_SUCCESS : Lang::Sounds::OGG_POPUP);
                    return "Kết quả đổ xúc xắc " + std::to_string(sides) + " mặt: " + outcome;
                }
            }
        );

        // CÁC TOOL MÀN HÌNH
        // TOOL CHUYỂN ĐỔI MÀN HÌNH CHỜ ĐỒNG HỒ
        mcp_server.AddTool(
            "self.screen.set_standby",
            "Chuyển robot sang chế độ Màn hình chờ đồng hồ (Standby Clock) hoặc thoát khỏi màn hình chờ về khuôn mặt robot. enable=true để bật màn hình chờ, false để đánh thức robot.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
                if (display_ != nullptr) {
                    if (enable) {
                        display_->ShowStandbyClock();
                    } else {
                        display_->HideStandbyClock();
                    }
                }
                return enable ? "Đã bật màn hình chờ đồng hồ" : "Đã đánh thức robot trở lại trạng thái sẵn sàng";
            }
        );

        mcp_server.AddTool("self.screen.set_emotion", "Set emotion of robot display. Supports 60 emotions: happy, sad, angry, laugh, cry, wink, sleepy, surprised, skeptical, smug, furious, love, dizzy, scared, excited, bored, pout, music, scanning, thinking, shy, neutral, matrix, loading, battery, overheat, reboot, target, side_eye, facepalm, innocent, jealous, scheming, disgusted, whisper, freeze, sneeze, waterfall, headache, drunk, exhausted, zen, jawdrop, paranoid, question, idea, hypno, money, heartbroken, tongue, party, ninja, game_over, fire, thunder, rain, sunny, sleepwalk, angel",
            PropertyList({Property("emotion", kPropertyTypeString, "happy")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string emotion = properties["emotion"].value<std::string>();
                if (display_ != nullptr) {
                    display_->SetEmotion(emotion.c_str());
                }
                return true;
            });
    }

    void InitializeSerialCli() {
        xTaskCreate([](void* arg) {
            auto* display = static_cast<Display*>(arg);
            char buf[64];
            int idx = 0;
            while (true) {
                int c = getchar();
                if (c != EOF && c > 0) {
                    if (c == '\r' || c == '\n') {
                        if (idx > 0) {
                            buf[idx] = '\0';
                            ESP_LOGI(TAG, "Serial CLI command: %s", buf);
                            if (display != nullptr) {
                                const char* cmd = buf;
                                if (std::strncmp(cmd, "face ", 5) == 0) {
                                    cmd += 5;
                                }
                                display->SetEmotion(cmd);
                            }
                            idx = 0;
                        }
                    } else if (idx < static_cast<int>(sizeof(buf)) - 1) {
                        buf[idx++] = static_cast<char>(c);
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }, "serial_cli_lcd", 3072, display_, 1, nullptr);
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
