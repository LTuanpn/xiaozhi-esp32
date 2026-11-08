#include "pocket_lcd_display.h"
#include "application.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "qrcodegen.h"
#include <ctime>
#include <wifi_manager.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "PocketLcdDisplay"

namespace {

constexpr uint32_t kAnimationPeriodMs = 50;  // 20 FPS
constexpr uint32_t kAutoSleepTimeoutTicks = 3600;  // ~3 minutes

inline int Ease(int current, int target) {
    int diff = target - current;
    if (diff == 0) return current;
    int step = diff / 3;
    if (step == 0) step = (diff > 0) ? 1 : -1;
    return current + step;
}

inline int EaseLid(int current, int target) {
    int diff = target - current;
    if (diff == 0) return current;
    int step = diff / 2;
    if (step == 0) step = (diff > 0) ? 1 : -1;
    return current + step;
}

inline int EyeRadius(int width, int height) {
    return std::clamp(static_cast<int>(std::round(std::min(width, height) * 16.0f / 72.0f)), 4, 16);
}

inline int PupilRadius(int width, int height) {
    const int shorter = std::min(width, height);
    if (shorter <= 14) return 4;
    if (shorter <= 22) return 6;
    if (shorter <= 30) return 8;
    return shorter / 2;
}

void PrepareShape(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

// Multi-tier round pixel-art disc renderer for 240x240 LCD (anti-diamond)
void PlaceDisc(lv_obj_t* wide, lv_obj_t* tall, lv_obj_t* mid, int x, int y, int diameter) {
    if (wide == nullptr) return;
    if (diameter <= 0) {
        lv_obj_add_flag(wide, LV_OBJ_FLAG_HIDDEN);
        if (tall) lv_obj_add_flag(tall, LV_OBJ_FLAG_HIDDEN);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(wide, LV_OBJ_FLAG_HIDDEN);

    if (diameter <= 4 || tall == nullptr) {
        lv_obj_set_pos(wide, x, y);
        lv_obj_set_size(wide, diameter, diameter);
        if (tall) lv_obj_add_flag(tall, LV_OBJ_FLAG_HIDDEN);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (diameter <= 8 || mid == nullptr) {
        int chamfer = (diameter >= 7) ? 2 : 1;
        lv_obj_set_pos(wide, x, y + chamfer);
        lv_obj_set_size(wide, diameter, diameter - 2 * chamfer);
        lv_obj_remove_flag(tall, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tall, x + chamfer, y);
        lv_obj_set_size(tall, diameter - 2 * chamfer, diameter);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 3-tier round pixel circle for diameter >= 9px
    int c1 = diameter / 4;
    int c2 = c1 / 2 + 1;
    lv_obj_set_pos(wide, x, y + c1);
    lv_obj_set_size(wide, diameter, diameter - 2 * c1);

    lv_obj_remove_flag(tall, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(tall, x + c1, y);
    lv_obj_set_size(tall, diameter - 2 * c1, diameter);

    lv_obj_remove_flag(mid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(mid, x + c2, y + c2);
    lv_obj_set_size(mid, diameter - 2 * c2, diameter - 2 * c2);
}

inline bool Contains(const char* haystack, const char* needle) {
    if (haystack == nullptr || needle == nullptr) return false;
    return strstr(haystack, needle) != nullptr;
}

} // namespace

PocketLcdDisplay::PocketLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y,
                                   bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
}

PocketLcdDisplay::~PocketLcdDisplay() {
    if (frame_timer_ != nullptr) {
        esp_timer_stop(frame_timer_);
        esp_timer_delete(frame_timer_);
        frame_timer_ = nullptr;
    }
}

void PocketLcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();  // Mark SetupUI as called

    DisplayLockGuard lock(this);
    if (!lock) return;

    // 1. Full Black Screen Background
    auto scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Face Layer (240x180, centered on upper canvas)
    face_layer_ = lv_obj_create(scr);
    PrepareShape(face_layer_);
    lv_obj_set_pos(face_layer_, 0, 10);
    lv_obj_set_size(face_layer_, 240, 180);
    lv_obj_set_style_bg_color(face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_layer_, LV_OPA_COVER, 0);

    // 3. Base Eyes (W=76, H=72, Radius=16)
    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 16, 0);
    }

    // 4. Black Pupils (Default: Natural Black, W=28, H=32, Radius=8)
    left_pupil_ = lv_obj_create(face_layer_);
    right_pupil_ = lv_obj_create(face_layer_);
    for (auto* pupil : {left_pupil_, right_pupil_}) {
        PrepareShape(pupil);
        lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pupil, 8, 0);
    }

    // 5. Catchlights (Multi-tier round discs)
    for (int eye_idx = 0; eye_idx < 2; ++eye_idx) {
        for (int i = 0; i < 4; ++i) {
            catchlight_[eye_idx][i] = lv_obj_create(face_layer_);
            PrepareShape(catchlight_[eye_idx][i]);
            lv_obj_set_style_bg_color(catchlight_[eye_idx][i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(catchlight_[eye_idx][i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(catchlight_[eye_idx][i], 0, 0);
            lv_obj_add_flag(catchlight_[eye_idx][i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 6. Visual Effects & Accessories (Laser, Sweat drop, Smoke puffs, Music notes, Extra shapes)
    sweat_drop_ = lv_obj_create(face_layer_);
    PrepareShape(sweat_drop_);
    lv_obj_set_style_bg_color(sweat_drop_, lv_color_hex(0x40C4FF), 0);
    lv_obj_set_style_bg_opa(sweat_drop_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sweat_drop_, 3, 0);
    lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);

    laser_line_ = lv_obj_create(face_layer_);
    PrepareShape(laser_line_);
    lv_obj_set_style_bg_color(laser_line_, lv_color_hex(0xFF1744), 0);
    lv_obj_set_style_bg_opa(laser_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(laser_line_, 2, 0);
    lv_obj_add_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 6; ++i) {
        smoke_puff_[i] = lv_obj_create(face_layer_);
        PrepareShape(smoke_puff_[i]);
        lv_obj_set_style_bg_color(smoke_puff_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(smoke_puff_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(smoke_puff_[i], 0, 0);
        lv_obj_add_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 2; ++i) {
        music_note_[i] = lv_obj_create(face_layer_);
        PrepareShape(music_note_[i]);
        lv_obj_set_style_bg_color(music_note_[i], (i == 0 ? lv_color_hex(0x00E5FF) : lv_color_hex(0xFF4081)), 0);
        lv_obj_set_style_bg_opa(music_note_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(music_note_[i], 2, 0);
        lv_obj_add_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 6; ++i) {
        extra_shape_[i] = lv_obj_create(face_layer_);
        PrepareShape(extra_shape_[i]);
        lv_obj_set_style_bg_color(extra_shape_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(extra_shape_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(extra_shape_[i], 4, 0);
        lv_obj_add_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 7. Subtitle / Chat Line (Positioned below the eyes, Marquee / Circular scrolling)
    chat_line_ = lv_label_create(scr);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(chat_line_, 230);
    lv_obj_set_pos(chat_line_, 5, 200);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);

    // Apply font from theme if available (supports Vietnamese UTF-8 accents)
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(chat_line_, lvgl_theme->text_font()->font(), 0);
    }
    lv_label_set_text(chat_line_, "");

    ApplyLayout();

    // 8. Start Animation Frame Timer (20 FPS)
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<PocketLcdDisplay*>(arg);
            self->UpdateAnimationFrame();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pocket_lcd_anim",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &frame_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(frame_timer_, kAnimationPeriodMs * 1000));
}

void PocketLcdDisplay::WakeUp() {
    last_active_tick_ = animation_tick_;
    if (standby_active_) {
        HideStandbyClock();
    }
    HideQrCode();
    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kSleepy) {
        SetFaceMode(FaceMode::kWakingUp);
    }
}

void PocketLcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    WakeUp();
    last_status_text_ = (status != nullptr) ? status : "";
    if (chat_line_ != nullptr && !has_chat_text_ && !hide_subtitle_) {
        lv_label_set_text(chat_line_, last_status_text_.c_str());
    }
}

void PocketLcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    WakeUp();
    if (chat_line_ != nullptr && !hide_subtitle_) {
        lv_label_set_text(chat_line_, (notification != nullptr) ? notification : "");
        notification_until_tick_ = animation_tick_ + (duration_ms / kAnimationPeriodMs);
    }
}

void PocketLcdDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

bool PocketLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (!lock) return false;
    WakeUp();
    emotion_mode_ = FaceModeFromEmotion(emotion);
    SetFaceMode(emotion_mode_);
    temporary_emotion_until_tick_ = animation_tick_ + (5000 / kAnimationPeriodMs);
    return true;
}

void PocketLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    WakeUp();

    if (content == nullptr || content[0] == '\0') {
        has_chat_text_ = false;
        if (chat_line_ != nullptr) {
            lv_label_set_text(chat_line_, "");
        }
        return;
    }

    std::string raw_text = content;
    std::string clean_text;
    std::string target_emotion;
    clean_text.reserve(raw_text.size());

    // Extract inline tag [emotion]
    size_t cursor = 0;
    while (cursor < raw_text.size()) {
        size_t open_tag = raw_text.find('[', cursor);
        if (open_tag == std::string::npos) {
            clean_text += raw_text.substr(cursor);
            break;
        }

        clean_text += raw_text.substr(cursor, open_tag - cursor);
        size_t close_tag = raw_text.find(']', open_tag + 1);
        if (close_tag == std::string::npos) {
            clean_text += raw_text.substr(open_tag);
            break;
        }

        std::string tag = raw_text.substr(open_tag + 1, close_tag - open_tag - 1);
        tag.erase(std::remove_if(tag.begin(), tag.end(), ::isspace), tag.end());
        if (!tag.empty() && target_emotion.empty()) {
            target_emotion = tag;
        }
        cursor = close_tag + 1;
    }

    size_t first_char = clean_text.find_first_not_of(" \t\r\n");
    if (first_char != std::string::npos) {
        clean_text = clean_text.substr(first_char);
    }

    if (!target_emotion.empty()) {
        FaceMode emo = FaceModeFromEmotion(target_emotion.c_str());
        SetFaceMode(emo);
        temporary_emotion_until_tick_ = animation_tick_ + (6000 / kAnimationPeriodMs);
    }

    has_chat_text_ = !clean_text.empty();
    if (chat_line_ != nullptr && !hide_subtitle_) {
        lv_label_set_text(chat_line_, clean_text.c_str());
        lv_obj_move_foreground(chat_line_);
    }
}

void PocketLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!lock) return;
    has_chat_text_ = false;
    if (chat_line_ != nullptr) {
        if (!last_status_text_.empty() && !hide_subtitle_) {
            lv_label_set_text(chat_line_, last_status_text_.c_str());
        } else {
            lv_label_set_text(chat_line_, "");
        }
    }
}

void PocketLcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    ClearChatMessages();
}

void PocketLcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    current_theme_ = theme;
    if (chat_line_ != nullptr && current_theme_ != nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr) {
            lv_obj_set_style_text_font(chat_line_, lvgl_theme->text_font()->font(), 0);
        }
    }
}

void PocketLcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    if (chat_line_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(chat_line_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(chat_line_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void PocketLcdDisplay::UpdateStatusBar(bool update_all) {
    // PocketLcdDisplay uses streamlined minimalist UI; status is presented in chat_line_
}

void PocketLcdDisplay::SetFaceMode(FaceMode mode) {
    if (face_mode_ == mode) return;
    face_mode_ = mode;
    last_active_tick_ = animation_tick_;
    if (mode == FaceMode::kSurprised) {
        surprised_start_tick_ = animation_tick_;
    }
}

void PocketLcdDisplay::ApplyLayout() {
    if (face_layer_ == nullptr) return;
    lv_obj_set_pos(face_layer_, 0, 10);
    lv_obj_set_size(face_layer_, 240, 180);
    current_left_width_ = std::min(current_left_width_, 76);
    current_right_width_ = std::min(current_right_width_, 76);
    current_left_height_ = std::min(current_left_height_, 72);
    current_right_height_ = std::min(current_right_height_, 72);
    lv_obj_set_style_radius(left_eye_, EyeRadius(current_left_width_, current_left_height_), 0);
    lv_obj_set_style_radius(right_eye_, EyeRadius(current_right_width_, current_right_height_), 0);
    lv_obj_move_foreground(face_layer_);
    if (chat_line_ != nullptr) {
        lv_obj_move_foreground(chat_line_);
    }
    UpdateAnimationFrame();
}

void PocketLcdDisplay::UpdateAnimationFrame() {
    if (face_layer_ == nullptr) return;
    DisplayLockGuard lock(this);
    if (!lock) return;
    ++animation_tick_;

    auto& app = Application::GetInstance();
    auto dev_state = app.GetDeviceState();

    // TỰ ĐỘNG THOÁT NGAY LẬP TỨC MÀN HÌNH CHỜ & QR KHI CÓ TƯƠNG TÁC (Listening, Speaking, Connecting...)
    if (dev_state != kDeviceStateIdle) {
        if (standby_active_) {
            HideStandbyClock();
        }
        HideQrCode();
        last_active_tick_ = animation_tick_;
    } else {
        // TỰ ĐỘNG VÀO MÀN HÌNH CHỜ NẾU NHÀN RỖI 30 GIÂY (600 ticks * 50ms = 30s) HOẶC ĐANG NGỦ
        if (!standby_active_ && (animation_tick_ - last_active_tick_ >= 600 || face_mode_ == FaceMode::kSleeping)) {
            ShowStandbyClock();
            return;
        }
    }

    if (standby_active_) {
        if (animation_tick_ - last_clock_tick_ >= 20) {
            last_clock_tick_ = animation_tick_;
            UpdateStandbyClock();
        }
        if (height_ <= 240) {
            return; // Màn vuông 240x240 return để tối ưu CPU
        }
    }

    // Restore chat line after notification timeout
        // Restore QR code display after timeout
    if (qr_until_tick_ > 0 && animation_tick_ >= qr_until_tick_) {
        HideQrCode();
    }

    if (notification_until_tick_ > 0 && animation_tick_ >= notification_until_tick_) {
        notification_until_tick_ = 0;
        if (chat_line_ != nullptr && !has_chat_text_ && !hide_subtitle_) {
            if (face_mode_ == FaceMode::kSleeping) {
                lv_label_set_text(chat_line_, "zZz...");
            } else {
                lv_label_set_text(chat_line_, last_status_text_.c_str());
            }
        }
    }

    const uint8_t mic_level = app.GetAudioService().GetInputLevel();

    // 240x240 Geometry: eye_width = 76, eye_height = 72, gap = 24
    constexpr int face_width = 240;
    constexpr int eye_width = 76;
    constexpr int eye_height = 72;
    constexpr int eye_gap = 24;
    const int pair_width = eye_width * 2 + eye_gap;
    const int base_x = (face_width - pair_width) / 2;     // 32px
    const int base_y = (180 - eye_height) / 2;             // 54px
    constexpr int max_look_x = 12;
    constexpr int max_look_y = 10;

    // State machine updates
    dev_state = app.GetDeviceState();
    if (dev_state == kDeviceStateListening) {
        SetFaceMode(FaceMode::kListening);
    } else if (dev_state == kDeviceStateConnecting) {
        SetFaceMode(FaceMode::kThinking);
    } else if (face_mode_ == FaceMode::kWakingUp) {
        if (animation_tick_ > last_active_tick_ + 14) {
            SetFaceMode(FaceMode::kNeutral);
        }
    } else if (face_mode_ == FaceMode::kSleeping) {
        // Sleep state
    } else if (temporary_emotion_until_tick_ > 0 && animation_tick_ >= temporary_emotion_until_tick_) {
        temporary_emotion_until_tick_ = 0;
        SetFaceMode(FaceMode::kNeutral);
    } else if (animation_tick_ > last_active_tick_ + kAutoSleepTimeoutTicks && face_mode_ == FaceMode::kNeutral) {
        SetFaceMode(FaceMode::kSleeping);
    }

    // Natural Blinking
    bool blink = false;
    if (animation_tick_ >= next_blink_tick_) {
        uint32_t blink_phase = animation_tick_ - next_blink_tick_;
        if (blink_phase < 3) {
            blink = true;
        } else {
            next_blink_tick_ = animation_tick_ + 40 + (NextRandom() % 80);
        }
    }

    // Target geometry initialization
    int target_left_width = eye_width;
    int target_right_width = eye_width;
    int target_left_height = eye_height;
    int target_right_height = eye_height;
    int target_left_y_offset = 0;
    int target_right_y_offset = 0;

    // Colors: Default is pure natural White Sclera, Black Pupil, White Catchlight
    lv_color_t eye_color = lv_color_white();
    lv_color_t pupil_color = lv_color_black();

    // 1. Dynamic Emotion Colors (Giải Pháp 2: Chuẩn màu Anime GIF rực rỡ)
    if (face_mode_ == FaceMode::kFireFury || face_mode_ == FaceMode::kFurious) {
        eye_color = lv_color_hex(0xFF3D00);       // Lửa Siêu Saiyan đỏ cam bốc cháy
    } else if (face_mode_ == FaceMode::kAngry) {
        eye_color = lv_color_hex(0xFF5722);       // Hổ phách giận dữ
    } else if (face_mode_ == FaceMode::kLove) {
        eye_color = lv_color_hex(0xFF1493);       // Hồng Ruby Doki-Doki Waifu
    } else if (face_mode_ == FaceMode::kSharingan) {
        eye_color = lv_color_hex(0xD50000);       // Đỏ máu Sharingan Uchiha
    } else if (face_mode_ == FaceMode::kAnimeStarlight) {
        eye_color = lv_color_hex(0x2A0845);       // Tím ngân hà Oshi no Ko
    } else if (face_mode_ == FaceMode::kHeartbroken) {
        eye_color = lv_color_hex(0xAB47BC);       // Tím thương đau
    } else if (face_mode_ == FaceMode::kFreezing) {
        eye_color = lv_color_hex(0x00E5FF);       // Băng tuyết Cyan
    } else if (face_mode_ == FaceMode::kMoneyEyes) {
        eye_color = lv_color_hex(0xFFD700);       // Vàng kim hoàng gia
    } else if (face_mode_ == FaceMode::kIdeaEureka) {
        eye_color = lv_color_hex(0xFFEB3B);       // Vàng sáng kiến
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        eye_color = lv_color_hex(0x00E676);       // Xanh hacker Matrix
    } else if (face_mode_ == FaceMode::kThunderShock) {
        eye_color = lv_color_hex(0x00E5FF);       // Xanh điện quang Zenitsu
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        eye_color = lv_color_hex(0x01579B);       // Xanh thẫm nhắm mắt Chibi Waterfall
    } else if (face_mode_ == FaceMode::kRainyGloomy) {
        eye_color = lv_color_hex(0xB0BEC5);       // Xám mưa ảm đạm
    } else if (face_mode_ == FaceMode::kZenMeditation || face_mode_ == FaceMode::kAngelBlessing) {
        eye_color = lv_color_hex(0xFFF9C4);       // Vàng thiên thần
    }

    // Target Emotion Shapes
    if (blink || face_mode_ == FaceMode::kSleeping) {
        target_left_height = target_right_height = 8;
        target_left_y_offset = target_right_y_offset = 20;
    } else if (face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kEcstaticLaugh ||
               face_mode_ == FaceMode::kPartyCelebrate) {
        target_left_height = target_right_height = 40;
        target_left_y_offset = target_right_y_offset = 12;
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        target_left_height = 72;
        target_right_height = 8;
        target_right_y_offset = 20;
    } else if (face_mode_ == FaceMode::kTongueOut) {
        target_left_height = target_right_height = 42;
        target_left_width = target_right_width = 72;
        target_left_y_offset = target_right_y_offset = 6;
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        // Siêu Saiyan bùng cháy: Nheo mắt sắc bén dốc chéo
        target_left_height = target_right_height = 40;
        target_left_y_offset = target_right_y_offset = 14;
    } else if (face_mode_ == FaceMode::kThunderShock) {
        // Lôi Độn sấm chớp: Nheo mỏng như lưỡi kiếm
        target_left_height = target_right_height = 14;
        target_left_y_offset = target_right_y_offset = 18;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        // Thác lệ Chibi: Mắt nhắm tịt run rẩy
        target_left_height = target_right_height = 10;
        target_left_y_offset = target_right_y_offset = 20;
    } else if (face_mode_ == FaceMode::kAnimeStarlight || face_mode_ == FaceMode::kSharingan) {
        target_left_height = target_right_height = 72;
        target_left_width = target_right_width = 76;
    } else if (face_mode_ == FaceMode::kSurprised || face_mode_ == FaceMode::kShockedJawdrop) {
        target_left_height = target_right_height = 78;
        target_left_width = target_right_width = 80;
    } else if (face_mode_ == FaceMode::kSadEmpathy) {
        target_left_height = target_right_height = 42;
        target_left_y_offset = target_right_y_offset = 14;
    } else if (face_mode_ == FaceMode::kSideEye) {
        target_left_width = target_right_width = 68;
        target_left_height = target_right_height = 40;
    }

    // Easing dimensions
    current_left_width_ = Ease(current_left_width_, target_left_width);
    current_right_width_ = Ease(current_right_width_, target_right_width);
    current_left_height_ = EaseLid(current_left_height_, target_left_height);
    current_right_height_ = EaseLid(current_right_height_, target_right_height);
    current_left_y_offset_ = Ease(current_left_y_offset_, target_left_y_offset);
    current_right_y_offset_ = Ease(current_right_y_offset_, target_right_y_offset);

    // Apply eye colors
    lv_obj_set_style_bg_color(left_eye_, eye_color, 0);
    lv_obj_set_style_bg_color(right_eye_, eye_color, 0);
    lv_obj_set_style_radius(left_eye_, EyeRadius(current_left_width_, current_left_height_), 0);
    lv_obj_set_style_radius(right_eye_, EyeRadius(current_right_width_, current_right_height_), 0);

    const int left_x = base_x + (eye_width - current_left_width_) / 2;
    const int right_x = base_x + eye_width + eye_gap + (eye_width - current_right_width_) / 2;
    const int left_y = base_y + current_left_y_offset_ + (eye_height - current_left_height_) / 2;
    const int right_y = base_y + current_right_y_offset_ + (eye_height - current_right_height_) / 2;

    lv_obj_set_pos(left_eye_, left_x, left_y);
    lv_obj_set_size(left_eye_, current_left_width_, current_left_height_);
    lv_obj_set_pos(right_eye_, right_x, right_y);
    lv_obj_set_size(right_eye_, current_right_width_, current_right_height_);

    // Look coordinates
    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);
    const int look_x = std::clamp(current_look_x_, -max_look_x, max_look_x);
    const int look_y = std::clamp(current_look_y_, -max_look_y, max_look_y);

    // Pupil Visibility
    const bool show_left_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kLove &&
                                 face_mode_ != FaceMode::kHeartbroken && face_mode_ != FaceMode::kDizzy &&
                                 face_mode_ != FaceMode::kRebooting && face_mode_ != FaceMode::kGameOver &&
                                 face_mode_ != FaceMode::kZenMeditation && face_mode_ != FaceMode::kSadCrying &&
                                 face_mode_ != FaceMode::kCryingStream && face_mode_ != FaceMode::kThunderShock;
    const bool show_right_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink &&
                                  face_mode_ != FaceMode::kLove && face_mode_ != FaceMode::kHeartbroken &&
                                  face_mode_ != FaceMode::kDizzy && face_mode_ != FaceMode::kRebooting &&
                                  face_mode_ != FaceMode::kGameOver && face_mode_ != FaceMode::kZenMeditation &&
                                  face_mode_ != FaceMode::kSadCrying && face_mode_ != FaceMode::kCryingStream &&
                                  face_mode_ != FaceMode::kThunderShock;

    // Pupil Geometry: Proportional 28x32
    int left_pupil_w = 28;
    int left_pupil_h = 32;
    int right_pupil_w = 28;
    int right_pupil_h = 32;

    if (face_mode_ == FaceMode::kListening && mic_level > 10) {
        int boost = (mic_level > 60) ? 4 : 2;
        left_pupil_w = right_pupil_w = 28 + boost;
        left_pupil_h = right_pupil_h = 32 + boost;
    } else if (face_mode_ == FaceMode::kSurprised) {
        left_pupil_w = right_pupil_w = 34;
        left_pupil_h = right_pupil_h = 38;
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kFurious) {
        left_pupil_w = right_pupil_w = 24;
        left_pupil_h = right_pupil_h = 26;
    }

    smooth_pupil_w_[0] = Ease(smooth_pupil_w_[0], left_pupil_w);
    smooth_pupil_h_[0] = Ease(smooth_pupil_h_[0], left_pupil_h);
    smooth_pupil_w_[1] = Ease(smooth_pupil_w_[1], right_pupil_w);
    smooth_pupil_h_[1] = Ease(smooth_pupil_h_[1], right_pupil_h);

    left_pupil_w = smooth_pupil_w_[0];
    left_pupil_h = smooth_pupil_h_[0];
    right_pupil_w = smooth_pupil_w_[1];
    right_pupil_h = smooth_pupil_h_[1];

    auto place_pupil = [&](int eye_x, int eye_y, int eye_w, int eye_h, int pw, int ph, int& ox, int& oy) {
        const float cx = eye_x + (eye_w - pw) / 2.0f;
        const float cy = eye_y + (eye_h - ph) / 2.0f;
        ox = static_cast<int>(std::lround(cx + look_x));
        oy = static_cast<int>(std::lround(cy + look_y));
        ox = std::clamp(ox, eye_x + 4, std::max(eye_x + 4, eye_x + eye_w - pw - 4));
        oy = std::clamp(oy, eye_y + 3, std::max(eye_y + 3, eye_y + eye_h - ph - 3));
    };

    int left_px = 0, left_py = 0, right_px = 0, right_py = 0;
    if (show_left_pupil) {
        lv_obj_remove_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
        place_pupil(left_x, left_y, current_left_width_, current_left_height_, left_pupil_w, left_pupil_h, left_px, left_py);
        lv_obj_set_pos(left_pupil_, left_px, left_py);
        lv_obj_set_size(left_pupil_, left_pupil_w, left_pupil_h);
        lv_obj_set_style_radius(left_pupil_, PupilRadius(left_pupil_w, left_pupil_h), 0);
        lv_obj_set_style_bg_color(left_pupil_, pupil_color, 0);
    } else {
        lv_obj_add_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_right_pupil) {
        lv_obj_remove_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
        place_pupil(right_x, right_y, current_right_width_, current_right_height_, right_pupil_w, right_pupil_h, right_px, right_py);
        lv_obj_set_pos(right_pupil_, right_px, right_py);
        lv_obj_set_size(right_pupil_, right_pupil_w, right_pupil_h);
        lv_obj_set_style_radius(right_pupil_, PupilRadius(right_pupil_w, right_pupil_h), 0);
        lv_obj_set_style_bg_color(right_pupil_, pupil_color, 0);
    } else {
        lv_obj_add_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    // Dynamic Catchlights (Anime glint)
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            if (catchlight_[e][i]) lv_obj_add_flag(catchlight_[e][i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!blink && current_left_height_ >= 28 && current_right_height_ >= 28) {
        // Left Eye: 6px round dot hugged tight against middle-right edge
        if (show_left_pupil && left_pupil_w >= 14 && catchlight_[0][0] != nullptr) {
            int ld = 6;
            int lx = left_px + left_pupil_w - ld - 2;
            int ly = left_py + (left_pupil_h - ld) / 2;
            PlaceDisc(catchlight_[0][0], catchlight_[0][1], catchlight_[0][2], lx, ly, ld);
        }
        // Right Eye: 12px 3-tier round pixel circle hugged at top-right corner
        if (show_right_pupil && right_pupil_w >= 16 && catchlight_[1][0] != nullptr) {
            int rd = 12;
            int rx = right_px + right_pupil_w - rd - 2;
            int ry = right_py + 2;
            PlaceDisc(catchlight_[1][0], catchlight_[1][1], catchlight_[1][2], rx, ry, rd);

            // Secondary tiny glint (4x4) at bottom-left corner
            if (catchlight_[1][3] != nullptr) {
                lv_obj_remove_flag(catchlight_[1][3], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(catchlight_[1][3], right_px + 4, right_py + right_pupil_h - 7);
                lv_obj_set_size(catchlight_[1][3], 4, 4);
                lv_obj_set_style_radius(catchlight_[1][3], 1, 0);
            }
        }
    }

    // Reset accessories
    if (sweat_drop_) lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
    if (laser_line_) lv_obj_add_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; ++i) {
        if (smoke_puff_[i]) lv_obj_add_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
        if (extra_shape_[i]) lv_obj_add_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; ++i) {
        if (music_note_[i]) lv_obj_add_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Sleeping Bubbles (Scaled to 10px and 14px 3-tier round pixel circles!)
    if (face_mode_ == FaceMode::kSleeping) {
        // Bong bóng 1 (10px):
        int b1_prog = (animation_tick_ / 3) % 24;
        int b1_x = 110 + b1_prog;
        int b1_y = 35 - b1_prog;
        PlaceDisc(smoke_puff_[0], smoke_puff_[1], smoke_puff_[2], b1_x, b1_y, 10);

        // Bong bóng 2 (14px):
        int b2_prog = (animation_tick_ / 3 + 12) % 24;
        int b2_x = 125 + b2_prog;
        int b2_y = 35 - b2_prog;
        PlaceDisc(smoke_puff_[3], smoke_puff_[4], smoke_puff_[5], b2_x, b2_y, 14);
    } else if (face_mode_ == FaceMode::kMusic) {
        for (int i = 0; i < 2; ++i) {
            if (music_note_[i]) {
                lv_obj_remove_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
                int ny = 20 - ((animation_tick_ / 3 + i * 8) % 18);
                int nx = (i == 0) ? (left_x + 10) : (right_x + current_right_width_ - 18);
                lv_obj_set_pos(music_note_[i], nx, ny);
                lv_obj_set_size(music_note_[i], 8, 8);
            }
        }
    } else if (face_mode_ == FaceMode::kShy) {
        // Blushing pink cheeks below eyes
        for (int i = 0; i < 2; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_hex(0xFF80AB), 0);
                lv_obj_set_size(extra_shape_[i], 16, 6);
                lv_obj_set_style_radius(extra_shape_[i], 3, 0);
                int cx = (i == 0) ? (left_x + 8) : (right_x + current_right_width_ - 24);
                lv_obj_set_pos(extra_shape_[i], cx, left_y + current_left_height_ + 4);
            }
        }
    } else if (face_mode_ == FaceMode::kTongueOut && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            int wiggle = ((animation_tick_ / 4) % 2 == 0) ? -2 : 2;
            int tongue_w = 20;
            int tongue_h = 22;
            int tx = 120 - tongue_w / 2 + wiggle;
            int ty = base_y + 44;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[0], lv_color_hex(0xFF4081), 0);
            lv_obj_set_pos(extra_shape_[0], tx, ty);
            lv_obj_set_size(extra_shape_[0], tongue_w, tongue_h);
            lv_obj_set_style_radius(extra_shape_[0], 8, 0);

            // Midline
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[1], lv_color_hex(0xC2185B), 0);
            lv_obj_set_pos(extra_shape_[1], tx + tongue_w / 2 - 1, ty + 2);
            lv_obj_set_size(extra_shape_[1], 2, tongue_h - 6);
            lv_obj_set_style_radius(extra_shape_[1], 1, 0);
        }
    } else if (face_mode_ == FaceMode::kPartyCelebrate) {
        // Confetti burst
        const lv_color_t confetti_colors[4] = {
            lv_color_hex(0xFFD700),
            lv_color_hex(0x00E5FF),
            lv_color_hex(0xFF4081),
            lv_color_hex(0x76FF03)
        };
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], confetti_colors[i], 0);
                int cy = 15 + ((animation_tick_ * 3 + i * 20) % 65);
                int cx = 25 + i * 55 + ((animation_tick_ + i * 5) % 8 - 4);
                lv_obj_set_pos(extra_shape_[i], cx, cy);
                lv_obj_set_size(extra_shape_[i], 6, 6);
                lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kLove) {
        // Doki-Doki Waifu Heartbeat (Nhịp đập 75 BPM + bong bóng bay + má hồng)
        int pulse = (animation_tick_ % 14 < 7) ? 4 : -2;
        lv_obj_set_style_bg_color(left_eye_, lv_color_hex(0xFF1493), 0);
        lv_obj_set_style_bg_color(right_eye_, lv_color_hex(0xFF1493), 0);
        lv_obj_set_size(left_eye_, current_left_width_ + pulse, current_left_height_ + pulse);
        lv_obj_set_size(right_eye_, current_right_width_ + pulse, current_right_height_ + pulse);

        // Má hồng đào
        for (int i = 0; i < 2; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_hex(0xFF80AB), 0);
                lv_obj_set_size(extra_shape_[i], 20, 8);
                lv_obj_set_style_radius(extra_shape_[i], 4, 0);
                int cx = (i == 0) ? (left_x + 6) : (right_x + current_right_width_ - 26);
                lv_obj_set_pos(extra_shape_[i], cx, left_y + current_left_height_ + 6);
            }
        }
        // Bong bóng trái tim bay lên
        for (int i = 2; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_hex(0xFF4081), 0);
                int prog = ((animation_tick_ * 2 + i * 16) % 60);
                int bx = (i == 2 ? 65 : 155) + ((prog % 8 < 4) ? -2 : 2);
                int by = 130 - prog;
                lv_obj_set_pos(extra_shape_[i], bx, by);
                lv_obj_set_size(extra_shape_[i], 8, 8);
                lv_obj_set_style_radius(extra_shape_[i], 4, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        // Thác Lệ Chibi Xanh Pha Lê (Waterfall Tears) tuôn trào cuồn cuộn
        for (int i = 0; i < 2; ++i) {
            // Má hồng ngượng ngùng nấc nở
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_hex(0xFF80AB), 0);
                lv_obj_set_size(extra_shape_[i], 18, 6);
                lv_obj_set_style_radius(extra_shape_[i], 3, 0);
                int cx = (i == 0) ? (left_x + 6) : (right_x + current_right_width_ - 24);
                lv_obj_set_pos(extra_shape_[i], cx, left_y + 14);
            }
        }
        // Dòng thác nước 2 bên mắt
        for (int i = 0; i < 2; ++i) {
            if (smoke_puff_[i * 3]) {
                lv_obj_remove_flag(smoke_puff_[i * 3], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(smoke_puff_[i * 3], lv_color_hex(0x00E5FF), 0); // Thác xanh cyan
                int wx = (i == 0) ? (left_x + 18) : (right_x + 36);
                lv_obj_set_pos(smoke_puff_[i * 3], wx, left_y + 12);
                lv_obj_set_size(smoke_puff_[i * 3], 14, 110);
                lv_obj_set_style_radius(smoke_puff_[i * 3], 6, 0);
            }
            // Bọt tuyết trượt xuống
            if (smoke_puff_[i * 3 + 1]) {
                lv_obj_remove_flag(smoke_puff_[i * 3 + 1], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(smoke_puff_[i * 3 + 1], lv_color_hex(0xE0F7FA), 0);
                int wx = (i == 0) ? (left_x + 20) : (right_x + 38);
                int wy = left_y + 15 + ((animation_tick_ * 6 + i * 20) % 95);
                lv_obj_set_pos(smoke_puff_[i * 3 + 1], wx, wy);
                lv_obj_set_size(smoke_puff_[i * 3 + 1], 10, 8);
                lv_obj_set_style_radius(smoke_puff_[i * 3 + 1], 3, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kFireFury || face_mode_ == FaceMode::kFurious) {
        // Hào quang Siêu Saiyan 3 tầng lửa bốc lên cuồn cuộn
        const lv_color_t flame_colors[3] = {lv_color_hex(0xFFEA00), lv_color_hex(0xFF6D00), lv_color_hex(0xD50000)};
        for (int i = 0; i < 6; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(smoke_puff_[i], flame_colors[i % 3], 0);
                int fx = 30 + i * 32 + ((animation_tick_ + i * 4) % 6 - 3);
                int fy = base_y - 6 - ((animation_tick_ * 4 + i * 12) % 36);
                lv_obj_set_pos(smoke_puff_[i], fx, fy);
                lv_obj_set_size(smoke_puff_[i], 12, 18);
                lv_obj_set_style_radius(smoke_puff_[i], 6, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kThunderShock) {
        // Zenitsu Thunder Flash (Lôi Độn sấm sét neon zic-zac)
        if (laser_line_) {
            lv_obj_remove_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(laser_line_, lv_color_hex(0xFFFF00), 0); // Vàng điện
            int flash_w = (animation_tick_ % 3 == 0) ? 220 : 160;
            lv_obj_set_pos(laser_line_, (240 - flash_w) / 2, base_y + 30);
            lv_obj_set_size(laser_line_, flash_w, 4);
            lv_obj_set_style_radius(laser_line_, 2, 0);
        }
    } else if (face_mode_ == FaceMode::kAnimeStarlight) {
        // Oshi no Ko Starlight (Ngôi sao vàng kim xoay trong đồng tử tím)
        for (int e = 0; e < 2; ++e) {
            if (catchlight_[e][0]) {
                lv_obj_remove_flag(catchlight_[e][0], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(catchlight_[e][0], lv_color_hex(0xFFD700), 0);
                int star_sz = 18 + ((animation_tick_ % 16 < 8) ? 4 : 0);
                int ex = (e == 0 ? left_x : right_x) + (current_left_width_ - star_sz) / 2;
                int ey = left_y + (current_left_height_ - star_sz) / 2;
                lv_obj_set_pos(catchlight_[e][0], ex, ey);
                lv_obj_set_size(catchlight_[e][0], star_sz, star_sz);
                lv_obj_set_style_radius(catchlight_[e][0], 3, 0);
            }
        }
        // Bụi sao lấp lánh khóe mắt
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_white(), 0);
                int sx = (i < 2 ? left_x - 12 : right_x + current_right_width_ + 4) + (i % 2) * 8;
                int sy = base_y + 10 + (i % 2) * 35;
                lv_obj_set_pos(extra_shape_[i], sx, sy);
                lv_obj_set_size(extra_shape_[i], 4, 4);
                lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kSharingan) {
        // Sharingan Uchiha (3 câu ngọc Tomoe đen xoay 120 độ quanh đồng tử)
        for (int e = 0; e < 2; ++e) {
            int cx = (e == 0 ? left_x : right_x) + current_left_width_ / 2;
            int cy = left_y + current_left_height_ / 2;
            float angle_base = (animation_tick_ * 12.0f) * 3.14159f / 180.0f;
            for (int t = 0; t < 3; ++t) {
                if (catchlight_[e][t]) {
                    lv_obj_remove_flag(catchlight_[e][t], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_bg_color(catchlight_[e][t], lv_color_black(), 0);
                    float a = angle_base + t * (2.0f * 3.14159f / 3.0f);
                    int tx = static_cast<int>(cx + 18.0f * cos(a) - 4);
                    int ty = static_cast<int>(cy + 18.0f * sin(a) - 4);
                    lv_obj_set_pos(catchlight_[e][t], tx, ty);
                    lv_obj_set_size(catchlight_[e][t], 8, 8);
                    lv_obj_set_style_radius(catchlight_[e][t], 4, 0);
                }
            }
        }
    }
}

PocketLcdDisplay::FaceMode PocketLcdDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
    }
    // Group 7: Anime Special FX (GIF-like Animations)
    if (Contains(emotion, "starlight") || Contains(emotion, "idol") || Contains(emotion, "ngoi_sao") ||
        Contains(emotion, "oshi_no_ko") || Contains(emotion, "toa_sang") || Contains(emotion, "sieu_sao")) {
        return FaceMode::kAnimeStarlight;
    }
    if (Contains(emotion, "sharingan") || Contains(emotion, "uchiha") || Contains(emotion, "nhan_thuat") ||
        Contains(emotion, "cau_ngoc") || Contains(emotion, "nhin_thau")) {
        return FaceMode::kSharingan;
    }
    if (Contains(emotion, "cry_stream") || Contains(emotion, "waterfall") || Contains(emotion, "thac_nuoc")) {
        return FaceMode::kCryingStream;
    }
    if (Contains(emotion, "cry") || Contains(emotion, "tear") || Contains(emotion, "grief") ||
        Contains(emotion, "weep") || Contains(emotion, "khoc")) {
        return FaceMode::kSadCrying;
    }
    if (Contains(emotion, "heartbreak") || Contains(emotion, "heartbroken") || Contains(emotion, "that_tinh") || Contains(emotion, "tan_nat")) {
        return FaceMode::kHeartbroken;
    }
    if (Contains(emotion, "sad") || Contains(emotion, "sympath") || Contains(emotion, "comfort") ||
        Contains(emotion, "sorrow") || Contains(emotion, "buon")) {
        return FaceMode::kSadEmpathy;
    }
    if (Contains(emotion, "party") || Contains(emotion, "celebrate") || Contains(emotion, "tiec") || Contains(emotion, "an_mung")) {
        return FaceMode::kPartyCelebrate;
    }
    if (Contains(emotion, "tongue") || Contains(emotion, "le_luoi") || Contains(emotion, "bleh")) {
        return FaceMode::kTongueOut;
    }
    if (Contains(emotion, "laugh") || Contains(emotion, "hilarious") || Contains(emotion, "lol") ||
        Contains(emotion, "ecstatic") || Contains(emotion, "joy") || Contains(emotion, "cuoi")) {
        return FaceMode::kEcstaticLaugh;
    }
    if (Contains(emotion, "wink") || Contains(emotion, "playful") || Contains(emotion, "cheeky") ||
        Contains(emotion, "joke") || Contains(emotion, "tease") || Contains(emotion, "nhay_mat")) {
        return FaceMode::kPlayfulWink;
    }
    if (Contains(emotion, "happy") || Contains(emotion, "smile") || Contains(emotion, "grin") ||
        Contains(emotion, "cheerful") || Contains(emotion, "vui")) {
        return FaceMode::kSmileGentle;
    }
    if (Contains(emotion, "fire") || Contains(emotion, "lua") || Contains(emotion, "saiyan")) {
        return FaceMode::kFireFury;
    }
    if (Contains(emotion, "furious") || Contains(emotion, "steaming") || Contains(emotion, "thinh_no") ||
        Contains(emotion, "dien_tiet") || Contains(emotion, "cuc_gian") || Contains(emotion, "outraged")) {
        return FaceMode::kFurious;
    }
    if (Contains(emotion, "angry") || Contains(emotion, "rage") || Contains(emotion, "mad") ||
        Contains(emotion, "tuc_gian") || Contains(emotion, "gian")) {
        return FaceMode::kAngry;
    }
    if (Contains(emotion, "matrix") || Contains(emotion, "glitch") || Contains(emotion, "loi")) {
        return FaceMode::kMatrixGlitch;
    }
    if (Contains(emotion, "loading") || Contains(emotion, "buffer") || Contains(emotion, "spinner") || Contains(emotion, "tai")) {
        return FaceMode::kLoadingBuffer;
    }
    if (Contains(emotion, "battery") || Contains(emotion, "pin") || Contains(emotion, "low_battery")) {
        return FaceMode::kLowBattery;
    }
    if (Contains(emotion, "overheat") || Contains(emotion, "nong") || Contains(emotion, "qua_nhiet")) {
        return FaceMode::kOverheat;
    }
    if (Contains(emotion, "reboot") || Contains(emotion, "restart") || Contains(emotion, "khoi_dong")) {
        return FaceMode::kRebooting;
    }
    if (Contains(emotion, "target") || Contains(emotion, "sniper") || Contains(emotion, "khoa_muc_tieu") || Contains(emotion, "lock")) {
        return FaceMode::kTargetLocked;
    }
    if (Contains(emotion, "side_eye") || Contains(emotion, "liec_xeo") || Contains(emotion, "phan_xet")) {
        return FaceMode::kSideEye;
    }
    if (Contains(emotion, "facepalm") || Contains(emotion, "bat_luc") || Contains(emotion, "can_loi")) {
        return FaceMode::kFacepalm;
    }
    if (Contains(emotion, "innocent") || Contains(emotion, "ngay_tho") || Contains(emotion, "halo")) {
        return FaceMode::kInnocent;
    }
    if (Contains(emotion, "jealous") || Contains(emotion, "ghen") || Contains(emotion, "do_ky")) {
        return FaceMode::kJealous;
    }
    if (Contains(emotion, "scheming") || Contains(emotion, "muu_mo") || Contains(emotion, "gian_xao") || Contains(emotion, "toan_tinh")) {
        return FaceMode::kScheming;
    }
    if (Contains(emotion, "disgust") || Contains(emotion, "kinh_tom") || Contains(emotion, "oe")) {
        return FaceMode::kDisgusted;
    }
    if (Contains(emotion, "whisper") || Contains(emotion, "thi_tham") || Contains(emotion, "bi_mat")) {
        return FaceMode::kWhispering;
    }
    if (Contains(emotion, "freeze") || Contains(emotion, "freezing") || Contains(emotion, "cold") || Contains(emotion, "lanh") || Contains(emotion, "bang")) {
        return FaceMode::kFreezing;
    }
    if (Contains(emotion, "sneeze") || Contains(emotion, "sneezing") || Contains(emotion, "hat_xi")) {
        return FaceMode::kSneezing;
    }
    if (Contains(emotion, "headache") || Contains(emotion, "dau_dau") || Contains(emotion, "nhuc_oc")) {
        return FaceMode::kHeadache;
    }
    if (Contains(emotion, "drunk") || Contains(emotion, "say_xin") || Contains(emotion, "say_ruou") || Contains(emotion, "beer")) {
        return FaceMode::kDrunk;
    }
    if (Contains(emotion, "exhaust") || Contains(emotion, "kiet_suc") || Contains(emotion, "guc_nga")) {
        return FaceMode::kExhausted;
    }
    if (Contains(emotion, "zen") || Contains(emotion, "meditat") || Contains(emotion, "thien") || Contains(emotion, "binh_yen")) {
        return FaceMode::kZenMeditation;
    }
    if (Contains(emotion, "jawdrop") || Contains(emotion, "shocked") || Contains(emotion, "rot_ham") || Contains(emotion, "bang_hoang")) {
        return FaceMode::kShockedJawdrop;
    }
    if (Contains(emotion, "paranoid") || Contains(emotion, "hoang_tuong") || Contains(emotion, "so_theo_doi")) {
        return FaceMode::kParanoid;
    }
    if (Contains(emotion, "confused") || Contains(emotion, "question") || Contains(emotion, "ngo_ngac") || Contains(emotion, "kho_hieu")) {
        return FaceMode::kConfusedQuestion;
    }
    if (Contains(emotion, "idea") || Contains(emotion, "eureka") || Contains(emotion, "bong_den") || Contains(emotion, "sang_kien")) {
        return FaceMode::kIdeaEureka;
    }
    if (Contains(emotion, "hypno") || Contains(emotion, "hypnotized") || Contains(emotion, "thoi_mien") || Contains(emotion, "lu_lan")) {
        return FaceMode::kHypnotized;
    }
    if (Contains(emotion, "money") || Contains(emotion, "dollar") || Contains(emotion, "tien") || Contains(emotion, "giau")) {
        return FaceMode::kMoneyEyes;
    }
    if (Contains(emotion, "stealth") || Contains(emotion, "ninja") || Contains(emotion, "an_nap") || Contains(emotion, "bong_dem")) {
        return FaceMode::kStealthNinja;
    }
    if (Contains(emotion, "game_over") || Contains(emotion, "dead") || Contains(emotion, "die") || Contains(emotion, "chet") || Contains(emotion, "thua")) {
        return FaceMode::kGameOver;
    }
    if (Contains(emotion, "thunder") || Contains(emotion, "lightning") || Contains(emotion, "set") || Contains(emotion, "te_liet")) {
        return FaceMode::kThunderShock;
    }
    if (Contains(emotion, "rain") || Contains(emotion, "gloomy") || Contains(emotion, "mua") || Contains(emotion, "am_dam")) {
        return FaceMode::kRainyGloomy;
    }
    if (Contains(emotion, "sunny") || Contains(emotion, "dazzle") || Contains(emotion, "nang") || Contains(emotion, "choi_chang")) {
        return FaceMode::kSunnyDazzle;
    }
    if (Contains(emotion, "sleepwalk") || Contains(emotion, "mong_du") || Contains(emotion, "zombie")) {
        return FaceMode::kSleepwalking;
    }
    if (Contains(emotion, "angel") || Contains(emotion, "bless") || Contains(emotion, "thanh_thien") || Contains(emotion, "ban_phuoc")) {
        return FaceMode::kAngelBlessing;
    }
    if (Contains(emotion, "love") || Contains(emotion, "heart") || Contains(emotion, "yeu") ||
        Contains(emotion, "thich") || Contains(emotion, "crush") || Contains(emotion, "adore")) {
        return FaceMode::kLove;
    }
    if (Contains(emotion, "dizzy") || Contains(emotion, "spin") || Contains(emotion, "chong_mat") ||
        Contains(emotion, "hoa_mat") || Contains(emotion, "say") || Contains(emotion, "faint")) {
        return FaceMode::kDizzy;
    }
    if (Contains(emotion, "scared") || Contains(emotion, "fear") || Contains(emotion, "afraid") ||
        Contains(emotion, "hoang_so") || Contains(emotion, "panic") || Contains(emotion, "terrified")) {
        return FaceMode::kScared;
    }
    if (Contains(emotion, "excited") || Contains(emotion, "star") || Contains(emotion, "hao_hung") ||
        Contains(emotion, "phan_khich") || Contains(emotion, "wow") || Contains(emotion, "sparkle")) {
        return FaceMode::kExcited;
    }
    if (Contains(emotion, "smug") || Contains(emotion, "pride") || Contains(emotion, "proud") ||
        Contains(emotion, "dac_y") || Contains(emotion, "kieu_ngao") || Contains(emotion, "cuoi_khey")) {
        return FaceMode::kSmug;
    }
    if (Contains(emotion, "bored") || Contains(emotion, "boring") || Contains(emotion, "chan") ||
        Contains(emotion, "te_nhat") || Contains(emotion, "meh") || Contains(emotion, "uninterested")) {
        return FaceMode::kBored;
    }
    if (Contains(emotion, "pout") || Contains(emotion, "plead") || Contains(emotion, "cute") ||
        Contains(emotion, "nong_niu") || Contains(emotion, "phung_phinh") || Contains(emotion, "puppy")) {
        return FaceMode::kPout;
    }
    if (Contains(emotion, "music") || Contains(emotion, "sing") || Contains(emotion, "song") ||
        Contains(emotion, "nhac") || Contains(emotion, "hat") || Contains(emotion, "melody") ||
        Contains(emotion, "dance") || Contains(emotion, "chill")) {
        return FaceMode::kMusic;
    }
    if (Contains(emotion, "scan") || Contains(emotion, "radar") || Contains(emotion, "quet") ||
        Contains(emotion, "phan_tich") || Contains(emotion, "analyze") || Contains(emotion, "reading")) {
        return FaceMode::kScanning;
    }
    if (Contains(emotion, "skeptic") || Contains(emotion, "doubt") || Contains(emotion, "suspicio") ||
        Contains(emotion, "disbelief") || Contains(emotion, "hoai_nghi") || Contains(emotion, "to_mo") ||
        Contains(emotion, "curious")) {
        return FaceMode::kSkeptical;
    }
    if (Contains(emotion, "shy") || Contains(emotion, "blush") || Contains(emotion, "embarrass") ||
        Contains(emotion, "ngai") || Contains(emotion, "xau_ho")) {
        return FaceMode::kShy;
    }
    if (Contains(emotion, "surpris") || Contains(emotion, "shock") || Contains(emotion, "amaz") ||
        Contains(emotion, "astonish") || Contains(emotion, "ngac_nhien") || Contains(emotion, "bat_ngo")) {
        return FaceMode::kSurprised;
    }
    if (Contains(emotion, "think") || Contains(emotion, "ponder") || Contains(emotion, "suy_nghi")) {
        return FaceMode::kThinking;
    }
    if (Contains(emotion, "sleepy") || Contains(emotion, "tired") || Contains(emotion, "yawn") ||
        Contains(emotion, "buon_ngu")) {
        return FaceMode::kSleepy;
    }
    if (Contains(emotion, "sleeping") || Contains(emotion, "sleep") || Contains(emotion, "ngu")) {
        return FaceMode::kSleeping;
    }
    return FaceMode::kNeutral;
}

uint32_t PocketLcdDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_ >> 8;
}

void PocketLcdDisplay::ShowQrCode(const std::string& text, const std::string& title, int duration_ms) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    WakeUp();

    if (qr_container_ == nullptr) {
        auto scr = lv_screen_active();
        qr_container_ = lv_obj_create(scr);
        lv_obj_remove_style_all(qr_container_);
        lv_obj_set_size(qr_container_, 240, 240);
        lv_obj_set_pos(qr_container_, 0, 0);
        lv_obj_set_style_bg_color(qr_container_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_container_, LV_OPA_COVER, 0);

        qr_title_label_ = lv_label_create(qr_container_);
        lv_obj_set_style_text_color(qr_title_label_, lv_color_black(), 0);
        lv_obj_set_style_text_align(qr_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(qr_title_label_, 220);
        lv_obj_set_pos(qr_title_label_, 10, 8);

        qr_canvas_ = lv_canvas_create(qr_container_);
        lv_obj_set_pos(qr_canvas_, 40, 32);

        qr_url_label_ = lv_label_create(qr_container_);
        lv_obj_set_style_text_color(qr_url_label_, lv_color_hex(0x0055AA), 0);
        lv_obj_set_style_text_align(qr_url_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(qr_url_label_, 220);
        lv_obj_set_pos(qr_url_label_, 10, 204);
    }

    lv_obj_remove_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qr_container_);

    std::string disp_title = title.empty() ? "QUET MA DE CAU HINH" : title;
    lv_label_set_text(qr_title_label_, disp_title.c_str());
    lv_label_set_text(qr_url_label_, text.c_str());

    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(text.c_str(), tempBuffer, qrcode, qrcodegen_Ecc_LOW, 1, 8, qrcodegen_Mask_AUTO, true);
    if (ok) {
        int qr_size = qrcodegen_getSize(qrcode);
        int scale = 160 / qr_size;
        if (scale < 1) scale = 1;
        int render_w = qr_size * scale;
        int offset_x = (160 - render_w) / 2;
        int offset_y = (160 - render_w) / 2;

        static lv_color_t s_qr_cbuf[160 * 160];
        lv_canvas_set_buffer(qr_canvas_, s_qr_cbuf, 160, 160, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(qr_canvas_, lv_color_white(), LV_OPA_COVER);

        for (int y = 0; y < qr_size; y++) {
            for (int x = 0; x < qr_size; x++) {
                if (qrcodegen_getModule(qrcode, x, y)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            lv_canvas_set_px(qr_canvas_, offset_x + x * scale + dx, offset_y + y * scale + dy, lv_color_black(), LV_OPA_COVER);
                        }
                    }
                }
            }
        }
    }

    qr_until_tick_ = animation_tick_ + (duration_ms / kAnimationPeriodMs);
}

void PocketLcdDisplay::HideQrCode() {
    DisplayLockGuard lock(this);
    if (!lock) return;
    qr_until_tick_ = 0;
    if (qr_container_ != nullptr) {
        lv_obj_add_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PocketLcdDisplay::ShowStandbyClock() {
    DisplayLockGuard lock(this);
    if (!lock) return;

    standby_active_ = true;
    last_clock_tick_ = animation_tick_;

    if (standby_container_ == nullptr) {
        auto scr = lv_screen_active();
        standby_container_ = lv_obj_create(scr);
        lv_obj_remove_style_all(standby_container_);
        if (height_ <= 240) {
            lv_obj_set_size(standby_container_, 240, 240);
            lv_obj_set_pos(standby_container_, 0, 0);
        } else {
            // Màn hình 2.4" 240x320: Đồng hồ đặt ở nửa dưới (y=100..320), nửa trên hở cho mắt robot
            lv_obj_set_size(standby_container_, 240, 220);
            lv_obj_set_pos(standby_container_, 0, 100);
        }
        lv_obj_set_style_bg_color(standby_container_, lv_color_hex(0x070B14), 0);
        lv_obj_set_style_bg_opa(standby_container_, LV_OPA_COVER, 0);

        if (height_ <= 240) {
            // ================= LAYOUT MÀN HÌNH VUÔNG 240x240 =================
            // Viền Bezel vuông bo góc kép Neon
            lv_obj_t* bezel = lv_obj_create(standby_container_);
            lv_obj_remove_style_all(bezel);
            lv_obj_set_size(bezel, 230, 230);
            lv_obj_set_pos(bezel, 5, 5);
            lv_obj_set_style_border_width(bezel, 2, 0);
            lv_obj_set_style_border_color(bezel, lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_border_opa(bezel, LV_OPA_70, 0);
            lv_obj_set_style_radius(bezel, 14, 0);

            // Status bar trên cùng
            clock_wifi_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_wifi_label_, lv_color_hex(0x80D8FF), 0);
            lv_obj_set_pos(clock_wifi_label_, 14, 12);

            clock_mini_robot_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_mini_robot_, lv_color_hex(0xFFD700), 0);
            lv_obj_set_pos(clock_mini_robot_, 180, 12);
            lv_label_set_text(clock_mini_robot_, "(•) zZz");

            // Ngày tháng
            clock_date_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_date_label_, 220);
            lv_obj_set_pos(clock_date_label_, 10, 42);

            // Giờ : Phút khổng lồ
            clock_time_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_time_label_, lv_color_white(), 0);
            lv_obj_set_style_text_align(clock_time_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_time_label_, 220);
            lv_obj_set_pos(clock_time_label_, 10, 78);

            // Giây
            clock_sec_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_sec_label_, lv_color_hex(0xFFB300), 0);
            lv_obj_set_style_text_align(clock_sec_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_sec_label_, 220);
            lv_obj_set_pos(clock_sec_label_, 10, 142);

            // Footer IP & Tips
            clock_ip_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_ip_label_, lv_color_hex(0x90CAF9), 0);
            lv_obj_set_style_text_align(clock_ip_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_ip_label_, 220);
            lv_obj_set_pos(clock_ip_label_, 10, 185);

            clock_tips_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_tips_label_, lv_color_hex(0x64748B), 0);
            lv_obj_set_style_text_align(clock_tips_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_tips_label_, 220);
            lv_obj_set_pos(clock_tips_label_, 10, 208);
            lv_label_set_text(clock_tips_label_, "Noi \"Jarvis\" de danh thuc");
        } else {
            // ================= LAYOUT MÀN HÌNH 2.4 INCH 240x320 =================
            // Nửa trên (y=0..100) đã có đôi mắt robot Wall-E sống động đang thở
            // Nửa dưới (container y=100..320) bố trí đồng hồ và thẻ thông tin
            clock_date_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_date_label_, 220);
            lv_obj_set_pos(clock_date_label_, 10, 6);

            // Giờ : Phút cỡ lớn
            clock_time_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_time_label_, lv_color_white(), 0);
            lv_obj_set_style_text_align(clock_time_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_time_label_, 220);
            lv_obj_set_pos(clock_time_label_, 10, 30);

            // Giây
            clock_sec_label_ = lv_label_create(standby_container_);
            lv_obj_set_style_text_color(clock_sec_label_, lv_color_hex(0xFFB300), 0);
            lv_obj_set_style_text_align(clock_sec_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(clock_sec_label_, 220);
            lv_obj_set_pos(clock_sec_label_, 10, 78);

            // Hộp thẻ kính mờ (Frosted Glass Card)
            clock_card_ = lv_obj_create(standby_container_);
            lv_obj_remove_style_all(clock_card_);
            lv_obj_set_size(clock_card_, 220, 105);
            lv_obj_set_pos(clock_card_, 10, 105);
            lv_obj_set_style_bg_color(clock_card_, lv_color_hex(0x101726), 0);
            lv_obj_set_style_bg_opa(clock_card_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(clock_card_, 1, 0);
            lv_obj_set_style_border_color(clock_card_, lv_color_hex(0x1E293B), 0);
            lv_obj_set_style_radius(clock_card_, 10, 0);

            clock_wifi_label_ = lv_label_create(clock_card_);
            lv_obj_set_style_text_color(clock_wifi_label_, lv_color_hex(0x80D8FF), 0);
            lv_obj_set_pos(clock_wifi_label_, 10, 10);

            clock_ip_label_ = lv_label_create(clock_card_);
            lv_obj_set_style_text_color(clock_ip_label_, lv_color_hex(0x90CAF9), 0);
            lv_obj_set_pos(clock_ip_label_, 10, 36);

            clock_tips_label_ = lv_label_create(clock_card_);
            lv_obj_set_style_text_color(clock_tips_label_, lv_color_hex(0xFBBF24), 0);
            lv_obj_set_pos(clock_tips_label_, 10, 68);
            lv_label_set_text(clock_tips_label_, "Noi \"Jarvis\" de danh thuc!");
        }
    }

    lv_obj_remove_flag(standby_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(standby_container_);
    UpdateStandbyClock();
}

void PocketLcdDisplay::HideStandbyClock() {
    DisplayLockGuard lock(this);
    if (!lock) return;

    standby_active_ = false;
    if (standby_container_ != nullptr) {
        lv_obj_add_flag(standby_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PocketLcdDisplay::UpdateStandbyClock() {
    if (!standby_active_ || standby_container_ == nullptr) return;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char time_buf[16];
    char sec_buf[16];
    char date_buf[64];

    if (tm_now.tm_year >= (2025 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        snprintf(sec_buf, sizeof(sec_buf), ": %02ds", tm_now.tm_sec);

        const char* days[] = {"CN", "THU 2", "THU 3", "THU 4", "THU 5", "THU 6", "THU 7"};
        snprintf(date_buf, sizeof(date_buf), "%s  *  %02d/%02d/%04d",
                 days[tm_now.tm_wday % 7], tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900);
    } else {
        // Chưa đồng bộ NTP qua WiFi
        uint32_t uptime_sec = animation_tick_ / 20;
        uint32_t hrs = uptime_sec / 3600;
        uint32_t mins = (uptime_sec % 3600) / 60;
        uint32_t secs = uptime_sec % 60;
        snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu", (unsigned long)hrs, (unsigned long)mins);
        snprintf(sec_buf, sizeof(sec_buf), ": %02lus", (unsigned long)secs);
        snprintf(date_buf, sizeof(date_buf), "DANG KET NOI WIFI...");
    }

    if (clock_time_label_) lv_label_set_text(clock_time_label_, time_buf);
    if (clock_sec_label_) lv_label_set_text(clock_sec_label_, sec_buf);
    if (clock_date_label_) lv_label_set_text(clock_date_label_, date_buf);

    // Cập nhật thông tin WiFi & IP
    auto& wifi = WifiManager::GetInstance();
    std::string ssid = wifi.GetSsid();
    std::string ip = wifi.GetIpAddress();
    int rssi = wifi.GetRssi();

    if (clock_wifi_label_) {
        std::string wf_str = "WF: " + (ssid.empty() ? "Offline" : (ssid + " (" + std::to_string(rssi) + "dB)"));
        lv_label_set_text(clock_wifi_label_, wf_str.c_str());
    }
    if (clock_ip_label_) {
        std::string ip_str = "IP: " + (ip.empty() ? "192.168.4.1 (AP)" : ip);
        lv_label_set_text(clock_ip_label_, ip_str.c_str());
    }
}

