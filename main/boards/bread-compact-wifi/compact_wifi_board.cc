#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "pocket_oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    int64_t last_touch_down_us_ = 0;
    uint8_t rapid_touch_count_ = 0;
    esp_timer_handle_t countdown_timer_ = nullptr;
    esp_timer_handle_t guard_timer_ = nullptr;
    esp_timer_handle_t workout_timer_ = nullptr;
    bool guard_mode_active_ = false;
    int workout_remaining_sec_ = 0;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new PocketOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnDoubleClick([this]() {
            if (display_ != nullptr) {
                display_->SetEmotion("wink");
            }
        });
        boot_button_.OnMultipleClick([this]() {
            if (display_ != nullptr) {
                display_->SetEmotion("furious");
                display_->ShowNotification("Đừng chọc nữa! >:(", 2000);
            }
        }, 3);
        boot_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });

        touch_button_.OnPressDown([this]() {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_touch_down_us_ < 650000) {
                ++rapid_touch_count_;
            } else {
                rapid_touch_count_ = 1;
            }
            last_touch_down_us_ = now_us;

            // Rapid touch poking/petting reaction (>= 3 taps quickly)
            if (rapid_touch_count_ >= 3 && display_ != nullptr) {
                if (rapid_touch_count_ == 3) {
                    display_->SetEmotion("love");
                    display_->ShowNotification("Nhột quá! <3", 1500);
                } else if (rapid_touch_count_ >= 5) {
                    display_->SetEmotion("dizzy");
                    display_->ShowNotification("Chóng mặt quá @_@", 2000);
                }
                return;
            }
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            if (rapid_touch_count_ >= 3) {
                return;
            }
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            if (volume >= 100) {
                GetDisplay()->SetEmotion("excited");
            }
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            GetDisplay()->SetEmotion("sunny");
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            if (volume == 0) {
                GetDisplay()->SetEmotion("whisper");
            }
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            GetDisplay()->SetEmotion("whisper");
        });
    }

    // Khởi tạo các công cụ tương tác MCP (Model Context Protocol) cho AI
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

                // Khởi tạo timer lần đầu nếu chưa có
                if (countdown_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoard*>(arg);
                            Application::GetInstance().Schedule([board]() {
                                auto* disp = board->GetDisplay();
                                if (disp != nullptr) {
                                    disp->SetEmotion("thunder");
                                    disp->ShowNotification("⏰ HẾT GIỜ RỒI ĐẠI CA!", 15000);
                                }
                                Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
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

                // Bắt đầu đếm ngược (đổi ra micro-giây)
                esp_timer_start_once(countdown_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);

                if (display_ != nullptr) {
                    std::string notify_msg = "⏳ Hẹn " + std::to_string(seconds) + "s: " + label;
                    display_->ShowNotification(notify_msg.c_str(), 5000);
                    display_->SetEmotion("target");
                }
                return "Đã bật đếm ngược " + std::to_string(seconds) + " giây cho: " + label;
            }
        );

        // 2. TOOL GHIM GHI CHÚ / NHẮC VIỆC LÊN MÀN HÌNH OLED (STICKY MEMO)
        mcp_server.AddTool(
            "self.screen.pin_memo",
            "Ghim một dòng ghi chú, địa chỉ IP, mật khẩu, hoặc việc cần làm lên màn hình OLED trong thời gian dài (mặc định 10 phút) để người dùng nhìn vào làm việc.",
            PropertyList({
                Property("text", kPropertyTypeString, ""),
                Property("duration_seconds", kPropertyTypeInteger, 600, 10, 3600)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string text = properties["text"].value<std::string>();
                int duration = properties["duration_seconds"].value<int>();

                if (display_ != nullptr) {
                    if (text.empty()) {
                        display_->ShowNotification("", 100);
                        return "Đã xóa ghi chú trên màn hình";
                    }
                    std::string pinned = "📌 " + text;
                    display_->ShowNotification(pinned.c_str(), duration * 1000);
                    display_->SetEmotion("idea");
                }
                return "Đã ghim nội dung lên màn hình OLED: " + text;
            }
        );

        // 3. TOOL BẬT/TẮT CHẾ ĐỘ NGẮT LỜI (REALTIME FULL-DUPLEX)
        mcp_server.AddTool(
            "self.audio.set_realtime_mode",
            "Bật hoặc tắt chế độ trò chuyện rảnh tay liên tục (cho phép người dùng nói chen ngang ngắt lời robot khi robot đang nói). enable = true để bật, false để tắt.",
            PropertyList({Property("enable", kPropertyTypeBoolean, true)}),
            [](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
                Application::GetInstance().SetAecMode(enable ? Application::kAecOnServerSide : Application::kAecOff);
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
                        display_->ShowNotification("🛡️ Đã tắt canh gác", 3000);
                    }
                    return "Đã tắt chế độ canh phòng";
                }

                if (display_ != nullptr) {
                    display_->SetEmotion("scanning");
                    display_->ShowNotification("🛡️ SENTRY MODE: ON", 60000);
                }

                if (guard_timer_ == nullptr) {
                    esp_timer_create_args_t timer_args = {
                        .callback = [](void* arg) {
                            auto* board = static_cast<CompactWifiBoard*>(arg);
                            auto& app = Application::GetInstance();
                            // Chỉ bắt trộm khi bot đang ở trạng thái Idle (đã nói xong)
                            if (board->guard_mode_active_ && app.GetDeviceState() == kDeviceStateIdle) {
                                uint8_t mic_lvl = app.GetAudioService().GetInputLevel();
                                if (mic_lvl > 35) { // Phát hiện tiếng động lớn bất thường!
                                    board->guard_mode_active_ = false;
                                    esp_timer_stop(board->guard_timer_);
                                    app.Schedule([board, &app]() {
                                        if (board->GetDisplay() != nullptr) {
                                            board->GetDisplay()->SetEmotion("furious");
                                            board->GetDisplay()->ShowNotification("🚨 PHÁT HIỆN XÂM NHẬP!", 10000);
                                        }
                                        app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
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
                esp_timer_start_periodic(guard_timer_, 200 * 1000); // Kiểm tra mỗi 200ms
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
                            auto* board = static_cast<CompactWifiBoard*>(arg);
                            auto& app = Application::GetInstance();
                            board->workout_remaining_sec_--;
                            int rem = board->workout_remaining_sec_;

                            if (rem <= 0) {
                                esp_timer_stop(board->workout_timer_);
                                app.Schedule([board, &app]() {
                                    if (board->GetDisplay() != nullptr) {
                                        board->GetDisplay()->SetEmotion("party");
                                        board->GetDisplay()->ShowNotification("🏆 HOÀN THÀNH HIỆP TẬP!", 8000);
                                    }
                                    app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                                });
                                return;
                            }

                            app.Schedule([board, &app, rem]() {
                                if (board->GetDisplay() != nullptr) {
                                    std::string msg = "💪 Hiệp tập: còn " + std::to_string(rem) + "s";
                                    board->GetDisplay()->ShowNotification(msg.c_str(), 1500);
                                    if (rem <= 5) {
                                        board->GetDisplay()->SetEmotion("fire");
                                    }
                                }
                                // Phát tiếng bíp mỗi 10 giây hoặc trong 3 giây cuối
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
                esp_timer_start_periodic(workout_timer_, 1000 * 1000); // 1 giây
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
                    std::string result_msg = "🎯 Chốt: " + chosen;
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
                    bool is_heads = (esp_random() % 2) == 1;
                    std::string outcome = is_heads ? "🪙 NGỬA (Heads)" : "🪙 SẤP (Tails)";
                    if (display_ != nullptr) {
                        display_->SetEmotion("dizzy");
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                    return "Kết quả tung đồng xu: " + outcome;
                } else {
                    int roll = static_cast<int>((esp_random() % sides) + 1);
                    std::string outcome = "🎲 [ " + std::to_string(roll) + " / d" + std::to_string(sides) + " ]";
                    if (display_ != nullptr) {
                        display_->SetEmotion(roll == sides ? "party" : (roll == 1 ? "facepalm" : "smug"));
                        display_->ShowNotification(outcome.c_str(), 8000);
                    }
                    Application::GetInstance().PlaySound(roll == sides ? Lang::Sounds::OGG_SUCCESS : Lang::Sounds::OGG_POPUP);
                    return "Kết quả đổ xúc xắc " + std::to_string(sides) + " mặt: " + std::to_string(roll);
                }
            }
        );

        // --- CÁC TOOL MÀN HÌNH CŨ ---
        mcp_server.AddTool("self.screen.flip", "Flip or rotate the OLED screen 180 degrees",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                static bool current_flip = false;
                current_flip = !current_flip;
                esp_lcd_panel_mirror(panel_, current_flip, current_flip);
                return true;
            });
        mcp_server.AddTool("self.screen.set_rotation", "Set rotation degree of the screen, e.g. 0 or 180",
            PropertyList({Property("degree", kPropertyTypeInteger, 0, 0, 180)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int degree = properties["degree"].value<int>();
                bool mirror = (degree == 180);
                esp_lcd_panel_mirror(panel_, mirror, mirror);
                return true;
            });
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
        }, "serial_cli", 3072, display_, 1, nullptr);
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
        InitializeSerialCli();
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
};

DECLARE_BOARD(CompactWifiBoard);
