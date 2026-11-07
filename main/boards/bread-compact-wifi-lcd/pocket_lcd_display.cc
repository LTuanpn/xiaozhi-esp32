#include "pocket_lcd_display.h"
#include "application.h"

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
    DisplayLockGuard lock(this);

    // 1. Full Black Screen Background
    lv_obj_t* scr = lv_scr_act();
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
    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kSleepy) {
        SetFaceMode(FaceMode::kWakingUp);
    }
}

void PocketLcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    WakeUp();
    last_status_text_ = (status != nullptr) ? status : "";
    if (chat_line_ != nullptr && !has_chat_text_) {
        lv_label_set_text(chat_line_, last_status_text_.c_str());
    }
}

void PocketLcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    WakeUp();
    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, (notification != nullptr) ? notification : "");
        notification_until_tick_ = animation_tick_ + (duration_ms / kAnimationPeriodMs);
    }
}

void PocketLcdDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

bool PocketLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    WakeUp();
    emotion_mode_ = FaceModeFromEmotion(emotion);
    SetFaceMode(emotion_mode_);
    temporary_emotion_until_tick_ = animation_tick_ + (5000 / kAnimationPeriodMs);
    return true;
}

void PocketLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
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
    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, clean_text.c_str());
        lv_obj_move_foreground(chat_line_);
    }
}

void PocketLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    has_chat_text_ = false;
    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, "");
    }
}

void PocketLcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    ClearChatMessages();
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
    ++animation_tick_;

    // Restore chat line after notification timeout
    if (notification_until_tick_ > 0 && animation_tick_ >= notification_until_tick_) {
        notification_until_tick_ = 0;
        if (chat_line_ != nullptr && !has_chat_text_) {
            if (face_mode_ == FaceMode::kSleeping) {
                lv_label_set_text(chat_line_, "zZz...");
            } else {
                lv_label_set_text(chat_line_, last_status_text_.c_str());
            }
        }
    }

    auto& app = Application::GetInstance();
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
    auto dev_state = app.GetDeviceState();
    if (dev_state == kDeviceStateListening) {
        SetFaceMode(FaceMode::kListening);
    } else if (dev_state == kDeviceStateThinking) {
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
    lv_color_t catchlight_color = lv_color_white();

    // 1. Dynamic Emotion Colors
    if (face_mode_ == FaceMode::kFireFury || face_mode_ == FaceMode::kFurious) {
        eye_color = lv_color_hex(0xFF3300);       // Fiery crimson
    } else if (face_mode_ == FaceMode::kAngry) {
        eye_color = lv_color_hex(0xFF5722);       // Angry flame amber
    } else if (face_mode_ == FaceMode::kLove) {
        eye_color = lv_color_hex(0xFF2D55);       // Passionate rose pink
    } else if (face_mode_ == FaceMode::kHeartbroken) {
        eye_color = lv_color_hex(0xAB47BC);       // Wounded purple
    } else if (face_mode_ == FaceMode::kFreezing) {
        eye_color = lv_color_hex(0x00E5FF);       // Icy cyber cyan
    } else if (face_mode_ == FaceMode::kMoneyEyes) {
        eye_color = lv_color_hex(0xFFD700);       // Pure gold
    } else if (face_mode_ == FaceMode::kIdeaEureka) {
        eye_color = lv_color_hex(0xFFEB3B);       // Warm bright yellow
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        eye_color = lv_color_hex(0x00E676);       // Hacker cyber green
    } else if (face_mode_ == FaceMode::kThunderShock) {
        eye_color = lv_color_hex(0xFFFF00);       // Electric yellow
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream || face_mode_ == FaceMode::kRainyGloomy) {
        eye_color = lv_color_hex(0xE1F5FE);       // Pale tear tint
    } else if (face_mode_ == FaceMode::kZenMeditation || face_mode_ == FaceMode::kAngelBlessing) {
        eye_color = lv_color_hex(0xFFF9C4);       // Divine celestial gold
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
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        target_left_height = target_right_height = 44;
        target_left_y_offset = target_right_y_offset = 12;
    } else if (face_mode_ == FaceMode::kSurprised || face_mode_ == FaceMode::kShockedJawdrop) {
        target_left_height = target_right_height = 78;
        target_left_width = target_right_width = 80;
    } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying) {
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
                                 face_mode_ != FaceMode::kZenMeditation;
    const bool show_right_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink &&
                                  face_mode_ != FaceMode::kLove && face_mode_ != FaceMode::kHeartbroken &&
                                  face_mode_ != FaceMode::kDizzy && face_mode_ != FaceMode::kRebooting &&
                                  face_mode_ != FaceMode::kGameOver && face_mode_ != FaceMode::kZenMeditation;

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
    } else if (face_mode_ == FaceMode::kLove) {
        // Heart eyes
        lv_obj_set_style_bg_color(left_eye_, lv_color_hex(0xFF2D55), 0);
        lv_obj_set_style_bg_color(right_eye_, lv_color_hex(0xFF2D55), 0);
    }
}

PocketLcdDisplay::FaceMode PocketLcdDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) return FaceMode::kNeutral;
    if (Contains(emotion, "cry_stream") || Contains(emotion, "waterfall")) return FaceMode::kCryingStream;
    if (Contains(emotion, "cry") || Contains(emotion, "tear")) return FaceMode::kSadCrying;
    if (Contains(emotion, "heartbreak")) return FaceMode::kHeartbroken;
    if (Contains(emotion, "sad")) return FaceMode::kSadEmpathy;
    if (Contains(emotion, "party") || Contains(emotion, "celebrate")) return FaceMode::kPartyCelebrate;
    if (Contains(emotion, "tongue")) return FaceMode::kTongueOut;
    if (Contains(emotion, "laugh") || Contains(emotion, "lol")) return FaceMode::kEcstaticLaugh;
    if (Contains(emotion, "wink")) return FaceMode::kPlayfulWink;
    if (Contains(emotion, "happy") || Contains(emotion, "smile")) return FaceMode::kSmileGentle;
    if (Contains(emotion, "fire") || Contains(emotion, "saiyan")) return FaceMode::kFireFury;
    if (Contains(emotion, "furious")) return FaceMode::kFurious;
    if (Contains(emotion, "angry")) return FaceMode::kAngry;
    if (Contains(emotion, "matrix") || Contains(emotion, "glitch")) return FaceMode::kMatrixGlitch;
    if (Contains(emotion, "loading")) return FaceMode::kLoadingBuffer;
    if (Contains(emotion, "battery") || Contains(emotion, "low_battery")) return FaceMode::kLowBattery;
    if (Contains(emotion, "overheat")) return FaceMode::kOverheat;
    if (Contains(emotion, "reboot")) return FaceMode::kRebooting;
    if (Contains(emotion, "target") || Contains(emotion, "sniper")) return FaceMode::kTargetLocked;
    if (Contains(emotion, "side_eye")) return FaceMode::kSideEye;
    if (Contains(emotion, "facepalm")) return FaceMode::kFacepalm;
    if (Contains(emotion, "innocent")) return FaceMode::kInnocent;
    if (Contains(emotion, "jealous")) return FaceMode::kJealous;
    if (Contains(emotion, "scheming")) return FaceMode::kScheming;
    if (Contains(emotion, "disgust")) return FaceMode::kDisgusted;
    if (Contains(emotion, "whisper")) return FaceMode::kWhispering;
    if (Contains(emotion, "freeze") || Contains(emotion, "cold")) return FaceMode::kFreezing;
    if (Contains(emotion, "sneeze")) return FaceMode::kSneezing;
    if (Contains(emotion, "headache")) return FaceMode::kHeadache;
    if (Contains(emotion, "drunk")) return FaceMode::kDrunk;
    if (Contains(emotion, "exhaust")) return FaceMode::kExhausted;
    if (Contains(emotion, "zen") || Contains(emotion, "meditat")) return FaceMode::kZenMeditation;
    if (Contains(emotion, "jawdrop") || Contains(emotion, "shocked")) return FaceMode::kShockedJawdrop;
    if (Contains(emotion, "paranoid")) return FaceMode::kParanoid;
    if (Contains(emotion, "confused")) return FaceMode::kConfusedQuestion;
    if (Contains(emotion, "idea") || Contains(emotion, "eureka")) return FaceMode::kIdeaEureka;
    if (Contains(emotion, "hypno")) return FaceMode::kHypnotized;
    if (Contains(emotion, "money") || Contains(emotion, "dollar")) return FaceMode::kMoneyEyes;
    if (Contains(emotion, "stealth") || Contains(emotion, "ninja")) return FaceMode::kStealthNinja;
    if (Contains(emotion, "game_over") || Contains(emotion, "dead")) return FaceMode::kGameOver;
    if (Contains(emotion, "thunder") || Contains(emotion, "lightning")) return FaceMode::kThunderShock;
    if (Contains(emotion, "rain") || Contains(emotion, "gloomy")) return FaceMode::kRainyGloomy;
    if (Contains(emotion, "sunny") || Contains(emotion, "dazzle")) return FaceMode::kSunnyDazzle;
    if (Contains(emotion, "sleepwalk")) return FaceMode::kSleepwalking;
    if (Contains(emotion, "angel") || Contains(emotion, "bless")) return FaceMode::kAngelBlessing;
    if (Contains(emotion, "love") || Contains(emotion, "heart")) return FaceMode::kLove;
    if (Contains(emotion, "dizzy") || Contains(emotion, "spin")) return FaceMode::kDizzy;
    if (Contains(emotion, "scared") || Contains(emotion, "fear")) return FaceMode::kScared;
    if (Contains(emotion, "excited") || Contains(emotion, "wow")) return FaceMode::kExcited;
    if (Contains(emotion, "smug") || Contains(emotion, "proud")) return FaceMode::kSmug;
    if (Contains(emotion, "bored")) return FaceMode::kBored;
    if (Contains(emotion, "pout")) return FaceMode::kPout;
    if (Contains(emotion, "music") || Contains(emotion, "sing")) return FaceMode::kMusic;
    if (Contains(emotion, "scan") || Contains(emotion, "radar")) return FaceMode::kScanning;
    if (Contains(emotion, "skeptic")) return FaceMode::kSkeptical;
    if (Contains(emotion, "shy")) return FaceMode::kShy;
    if (Contains(emotion, "surpris")) return FaceMode::kSurprised;
    if (Contains(emotion, "think")) return FaceMode::kThinking;
    if (Contains(emotion, "sleepy")) return FaceMode::kSleepy;
    if (Contains(emotion, "sleeping") || Contains(emotion, "sleep")) return FaceMode::kSleeping;
    return FaceMode::kNeutral;
}

uint32_t PocketLcdDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_ >> 8;
}
