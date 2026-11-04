#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <cstring>

namespace {
// An original LVGL implementation inspired by the visual behavior documented by
// FluxGarage RoboEyes: https://github.com/FluxGarage/RoboEyes
constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kAutoSleepTimeoutTicks = 45000 / kAnimationPeriodMs;  // 45s = 1125 ticks

void PrepareShape(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

bool Contains(const char* text, const char* value) {
    return text != nullptr && value != nullptr && std::strstr(text, value) != nullptr;
}

int Ease(int current, int target) {
    if (current == target) {
        return current;
    }
    const int delta = target - current;
    if (delta > 0) {
        return current + std::max(1, delta / 3);
    }
    return current + std::min(-1, delta / 3);
}
}  // namespace

PocketOledDisplay::PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel, int width, int height,
                                     bool mirror_x, bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

PocketOledDisplay::~PocketOledDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_del(animation_timer_);
        animation_timer_ = nullptr;
    }
    if (chat_line_ != nullptr) {
        lv_obj_delete(chat_line_);
        chat_line_ = nullptr;
    }
    if (face_layer_ != nullptr) {
        lv_obj_delete(face_layer_);
        face_layer_ = nullptr;
    }
}

void PocketOledDisplay::SetupUI() {
    OledDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto* screen = lv_screen_active();

    face_layer_ = lv_obj_create(screen);
    PrepareShape(face_layer_);
    lv_obj_set_style_bg_color(face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face_layer_, 0, 0);
    lv_obj_set_style_radius(face_layer_, 0, 0);
    lv_obj_set_style_pad_all(face_layer_, 0, 0);

    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);
    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);
    teardrop_ = lv_obj_create(face_layer_);
    left_blush_ = lv_label_create(face_layer_);
    right_blush_ = lv_label_create(face_layer_);

    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 8, 0);
    }
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 6, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // Teardrop shape for crying
    PrepareShape(teardrop_);
    lv_obj_set_style_bg_color(teardrop_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(teardrop_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(teardrop_, 3, 0);
    lv_obj_add_flag(teardrop_, LV_OBJ_FLAG_HIDDEN);

    // Blushing marks
    for (auto* blush : {left_blush_, right_blush_}) {
        lv_label_set_text(blush, "///");
        lv_obj_set_style_text_color(blush, lv_color_white(), 0);
        lv_obj_set_style_text_font(blush, &lv_font_montserrat_14, 0);
        lv_obj_add_flag(blush, LV_OBJ_FLAG_HIDDEN);
    }

    chat_line_ = lv_label_create(screen);
    lv_obj_set_pos(chat_line_, 0, 48);
    lv_obj_set_size(chat_line_, 128, 16);
    lv_obj_set_style_bg_color(chat_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(chat_line_, 2, 0);
    lv_obj_set_style_pad_right(chat_line_, 2, 0);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_line_, "");

    ApplyLayout();
    lv_obj_move_foreground(chat_line_);
    animation_timer_ = lv_timer_create(AnimationTimerCallback, kAnimationPeriodMs, this);
}

void PocketOledDisplay::WakeUp() {
    if (face_mode_ == FaceMode::kSleeping) {
        wake_up_until_tick_ = animation_tick_ + 25;
        SetFaceMode(FaceMode::kWakingUp);
        if (chat_line_ != nullptr && !has_chat_text_) {
            lv_label_set_text(chat_line_, "");
        }
    }
    last_active_tick_ = animation_tick_;
}

void PocketOledDisplay::SetStatus(const char* status) {
    OledDisplay::SetStatus(status);
    DisplayLockGuard lock(this);

    if (status == nullptr) {
        return;
    }

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
        std::strcmp(status, Lang::Strings::SPEAKING) == 0 ||
        std::strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
        Contains(status, "Wi-Fi") || Contains(status, "Config")) {
        WakeUp();
    }

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        is_speaking_ = false;
        SetFaceMode(FaceMode::kListening);
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        is_speaking_ = true;
        last_active_tick_ = animation_tick_;
        if (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening) {
            SetFaceMode(emotion_mode_ != FaceMode::kNeutral ? emotion_mode_ : FaceMode::kSmileGentle);
        }
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 || Contains(status, "Đang") ||
               Contains(status, "dang")) {
        is_speaking_ = false;
        SetFaceMode(FaceMode::kThinking);
    } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        is_speaking_ = false;
        last_active_tick_ = animation_tick_;
        if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
            SetFaceMode(FaceMode::kNeutral);
            emotion_mode_ = FaceMode::kNeutral;
        }
    }

    if (!has_chat_text_ && chat_line_ != nullptr) {
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, status);
        }
    }
}

bool PocketOledDisplay::SetEmotion(const char* emotion) {
    OledDisplay::SetEmotion(emotion);
    DisplayLockGuard lock(this);
    WakeUp();
    emotion_mode_ = FaceModeFromEmotion(emotion);
    SetFaceMode(emotion_mode_);
    return true;
}

void PocketOledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    WakeUp();

    has_chat_text_ = content != nullptr && content[0] != '\0';
    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, has_chat_text_ ? content : "");
    }
    if (has_chat_text_ && role != nullptr && std::strcmp(role, "assistant") == 0) {
        is_speaking_ = true;
        last_active_tick_ = animation_tick_;
        if (face_mode_ == FaceMode::kNeutral) {
            SetFaceMode(emotion_mode_ != FaceMode::kNeutral ? emotion_mode_ : FaceMode::kSmileGentle);
        }
    }
}

void PocketOledDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    has_chat_text_ = false;
    is_speaking_ = false;
    last_active_tick_ = animation_tick_;
    if (chat_line_ != nullptr) {
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, "");
        }
    }
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
        SetFaceMode(FaceMode::kNeutral);
        emotion_mode_ = FaceMode::kNeutral;
    }
}

void PocketOledDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    has_chat_text_ = false;
    is_speaking_ = false;
    last_active_tick_ = animation_tick_;
    if (chat_line_ != nullptr) {
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, "");
        }
    }
}

void PocketOledDisplay::AnimationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<PocketOledDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        display->UpdateAnimationFrame();
    }
}

void PocketOledDisplay::ApplyLayout() {
    if (face_layer_ == nullptr) {
        return;
    }

    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 128, 48);
    lv_obj_set_style_radius(left_eye_, 8, 0);
    lv_obj_set_style_radius(right_eye_, 8, 0);
    current_left_height_ = std::min(current_left_height_, 36);
    current_right_height_ = std::min(current_right_height_, 36);
    lv_obj_move_foreground(face_layer_);
    if (chat_line_ != nullptr) {
        lv_obj_move_foreground(chat_line_);
    }
    UpdateAnimationFrame();
}

void PocketOledDisplay::UpdateAnimationFrame() {
    if (face_layer_ == nullptr) {
        return;
    }
    ++animation_tick_;

    constexpr int face_width = 128;
    constexpr int eye_width = 44;
    constexpr int eye_height = 36;
    constexpr int eye_gap = 12;
    const int pair_width = eye_width * 2 + eye_gap;
    const int base_x = (face_width - pair_width) / 2;
    const int base_y = (48 - eye_height) / 2;
    constexpr int max_look_x = 7;
    constexpr int max_look_y = 4;

    // 1. Tự động đi ngủ sau 45s không tương tác
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
        if (animation_tick_ >= last_active_tick_ + kAutoSleepTimeoutTicks) {
            SetFaceMode(FaceMode::kSleeping);
            is_speaking_ = false;
            if (chat_line_ != nullptr && !has_chat_text_) {
                lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
                lv_label_set_text(chat_line_, "zZz...");
            }
        }
    }

    // 2. Trạng thái thức dậy kWakingUp
    if (face_mode_ == FaceMode::kWakingUp) {
        if (animation_tick_ >= wake_up_until_tick_) {
            SetFaceMode(FaceMode::kNeutral);
        }
    }

    // 3. Logic Chớp mắt (Blink) - Đang ngủ hoặc đang nháy mắt thì KHÔNG chớp mắt
    bool blink = false;
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink) {
        if (animation_tick_ >= next_blink_tick_) {
            blink_until_tick_ = animation_tick_ + 3;
            next_blink_tick_ = animation_tick_ + 70 + (NextRandom() % 100);
        }
        blink = animation_tick_ < blink_until_tick_;
    }

    // 4. Liếc nhìn nhàn rỗi (Look around)
    if (animation_tick_ >= next_idle_tick_ && face_mode_ == FaceMode::kNeutral && !is_speaking_) {
        target_look_x_ = static_cast<int>(NextRandom() % (max_look_x * 2 + 1)) - max_look_x;
        target_look_y_ = static_cast<int>(NextRandom() % (max_look_y * 2 + 1)) - max_look_y;
        next_idle_tick_ = animation_tick_ + 45 + (NextRandom() % 75);
    }

    if (face_mode_ == FaceMode::kSleeping) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kThinking) {
        target_look_x_ = max_look_x;
        target_look_y_ = -max_look_y;
    } else if (face_mode_ == FaceMode::kListening) {
        target_look_x_ = 0;
        target_look_y_ = ((animation_tick_ / 8) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kShy) {
        target_look_x_ = -5;
        target_look_y_ = 3;
    } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kAngry) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (is_speaking_ || face_mode_ == FaceMode::kWakingUp || face_mode_ == FaceMode::kSmileGentle ||
               face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPlayfulWink) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    // 5. Chiều cao và độ lệch Y của từng mắt (Multi-tier Eye Heights)
    int target_left_height = eye_height;
    int target_right_height = eye_height;
    int target_left_y_offset = 0;
    int target_right_y_offset = 0;

    if (face_mode_ == FaceMode::kSleeping) {
        // Nhịp thở êm ái khi ngủ: 2px <-> 3px
        int breath = ((animation_tick_ / 25) % 2 == 0) ? 2 : 3;
        target_left_height = breath;
        target_right_height = breath;
    } else if (blink) {
        target_left_height = 2;
        target_right_height = 2;
    } else if (face_mode_ == FaceMode::kSleepy) {
        target_left_height = 7;
        target_right_height = 7;
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        target_left_height = eye_height;       // Mắt trái mở to rực rỡ
        target_right_height = 4;                // Mắt phải nháy híp vòng cung
        target_right_y_offset = 6;
    } else if (face_mode_ == FaceMode::kSkeptical) {
        target_left_height = eye_height + 4;   // Mắt trái nhướn to
        target_left_y_offset = -3;
        target_right_height = 18;              // Mắt phải nheo nhỏ
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kSurprised) {
        target_left_height = eye_height + 4;
        target_right_height = eye_height + 4;
    } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
        target_left_height = 14;
        target_right_height = 14;
        target_left_y_offset = 4;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kSadCrying) {
        target_left_height = eye_height - 6;
        target_right_height = eye_height - 6;
        target_left_y_offset = 3;
        target_right_y_offset = 3;
    } else if (face_mode_ == FaceMode::kSadEmpathy) {
        target_left_height = eye_height - 2;
        target_right_height = eye_height - 2;
        target_left_y_offset = 2;
        target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kShy) {
        target_left_height = eye_height - 4;
        target_right_height = eye_height - 4;
        target_left_y_offset = 3;
        target_right_y_offset = 3;
    }

    current_left_height_ = Ease(current_left_height_, target_left_height);
    current_right_height_ = Ease(current_right_height_, target_right_height);
    current_left_y_offset_ = Ease(current_left_y_offset_, target_left_y_offset);
    current_right_y_offset_ = Ease(current_right_y_offset_, target_right_y_offset);

    // 6. Hiệu ứng nhún nhảy khi nói hoặc rung lắc khi suy nghĩ
    int animation_x = 0;
    int animation_y = 0;
    if (is_speaking_) {
        // Nhún nhảy nhẹ theo nhịp nói
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -1 : 1;
    }
    if (face_mode_ == FaceMode::kThinking) {
        animation_x = ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
    }

    const int left_x = base_x + current_look_x_ + animation_x;
    const int right_x = left_x + eye_width + eye_gap;
    const int left_y =
        base_y + (eye_height - current_left_height_) / 2 + current_look_y_ + animation_y + current_left_y_offset_;
    const int right_y =
        base_y + (eye_height - current_right_height_) / 2 + current_look_y_ + animation_y + current_right_y_offset_;

    lv_obj_set_pos(left_eye_, left_x, left_y);
    lv_obj_set_size(left_eye_, eye_width, current_left_height_);
    lv_obj_set_pos(right_eye_, right_x, right_y);
    lv_obj_set_size(right_eye_, eye_width, current_right_height_);

    // Bo góc mắt
    const int left_radius = (face_mode_ == FaceMode::kSleeping) ? 1 : 8;
    int right_radius = (face_mode_ == FaceMode::kSleeping) ? 1 : 8;
    if (face_mode_ == FaceMode::kPlayfulWink) {
        right_radius = 2;
    }
    lv_obj_set_style_radius(left_eye_, left_radius, 0);
    lv_obj_set_style_radius(right_eye_, right_radius, 0);

    // 7. Giọt nước mắt (Teardrop) khi khóc
    lv_obj_add_flag(teardrop_, LV_OBJ_FLAG_HIDDEN);
    if (face_mode_ == FaceMode::kSadCrying && !blink) {
        lv_obj_remove_flag(teardrop_, LV_OBJ_FLAG_HIDDEN);
        int tear_slide_y = (animation_tick_ % 14);
        lv_obj_set_pos(teardrop_, right_x + eye_width + 2, right_y + 8 + tear_slide_y);
        lv_obj_set_size(teardrop_, 6, 8);
    }

    // 8. Má hồng thẹn thùng khi Shy
    lv_obj_add_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
    if (face_mode_ == FaceMode::kShy && !blink) {
        lv_obj_remove_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_blush_, left_x + 8, left_y + current_left_height_ - 2);
        lv_obj_set_pos(right_blush_, right_x + 8, right_y + current_right_height_ - 2);
    }

    // 9. Mí mắt trên và dưới
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    constexpr int lid_height = 10;
    if (face_mode_ != FaceMode::kSleeping && !blink) {
        // Mí trên: Angry / SadEmpathy / SadCrying / Skeptical
        if (face_mode_ == FaceMode::kAngry) {
            const int lid_width = eye_width + 8;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 4, left_y - 2);
            lv_obj_set_size(left_top_lid_, lid_width, lid_height);
            lv_obj_set_pos(right_top_lid_, right_x - 4, right_y - 2);
            lv_obj_set_size(right_top_lid_, lid_width, lid_height);
            lv_obj_set_style_transform_rotation(left_top_lid_, -150, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, 150, 0);
        } else if (face_mode_ == FaceMode::kSadEmpathy) {
            const int lid_width = eye_width + 6;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 3, left_y - 2);
            lv_obj_set_size(left_top_lid_, lid_width, lid_height);
            lv_obj_set_pos(right_top_lid_, right_x - 3, right_y - 2);
            lv_obj_set_size(right_top_lid_, lid_width, lid_height);
            lv_obj_set_style_transform_rotation(left_top_lid_, 100, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, -100, 0);
        } else if (face_mode_ == FaceMode::kSadCrying) {
            const int lid_width = eye_width + 8;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 4, left_y - 2);
            lv_obj_set_size(left_top_lid_, lid_width, lid_height);
            lv_obj_set_pos(right_top_lid_, right_x - 4, right_y - 2);
            lv_obj_set_size(right_top_lid_, lid_width, lid_height);
            lv_obj_set_style_transform_rotation(left_top_lid_, 180, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, -180, 0);
        }

        // Mí dưới: SmileGentle / EcstaticLaugh / Speaking
        if (face_mode_ == FaceMode::kSmileGentle ||
            (is_speaking_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening))) {
            constexpr int cover_height = 14;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2,
                           left_y + current_left_height_ - cover_height / 2);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2,
                           right_y + current_right_height_ - cover_height / 2);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
        } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
            constexpr int cover_height = 18;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2,
                           left_y + current_left_height_ - cover_height / 2);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2,
                           right_y + current_right_height_ - cover_height / 2);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
        }
    }
}

void PocketOledDisplay::SetFaceMode(FaceMode mode) {
    face_mode_ = mode;
    if (mode != FaceMode::kNeutral && mode != FaceMode::kSleeping) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }
    UpdateAnimationFrame();
}

PocketOledDisplay::FaceMode PocketOledDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
    }
    // 1. Crying / Sadness
    if (Contains(emotion, "cry") || Contains(emotion, "tear") || Contains(emotion, "grief") || Contains(emotion, "weep")) {
        return FaceMode::kSadCrying;
    }
    if (Contains(emotion, "sad") || Contains(emotion, "sympath") || Contains(emotion, "comfort") || Contains(emotion, "sorrow")) {
        return FaceMode::kSadEmpathy;
    }
    // 2. Laugh / Happiness
    if (Contains(emotion, "laugh") || Contains(emotion, "hilarious") || Contains(emotion, "lol") || Contains(emotion, "ecstatic") || Contains(emotion, "joy")) {
        return FaceMode::kEcstaticLaugh;
    }
    if (Contains(emotion, "wink") || Contains(emotion, "playful") || Contains(emotion, "cheeky") || Contains(emotion, "joke") || Contains(emotion, "tease")) {
        return FaceMode::kPlayfulWink;
    }
    if (Contains(emotion, "happy") || Contains(emotion, "smile") || Contains(emotion, "grin") || Contains(emotion, "cheerful") || Contains(emotion, "confident")) {
        return FaceMode::kSmileGentle;
    }
    // 3. Special Emotions
    if (Contains(emotion, "skeptic") || Contains(emotion, "doubt") || Contains(emotion, "suspicio") || Contains(emotion, "disbelief")) {
        return FaceMode::kSkeptical;
    }
    if (Contains(emotion, "shy") || Contains(emotion, "blush") || Contains(emotion, "embarrass")) {
        return FaceMode::kShy;
    }
    if (Contains(emotion, "angry") || Contains(emotion, "rage") || Contains(emotion, "furious") || Contains(emotion, "mad")) {
        return FaceMode::kAngry;
    }
    if (Contains(emotion, "surpris") || Contains(emotion, "shock") || Contains(emotion, "amaz") || Contains(emotion, "astonish")) {
        return FaceMode::kSurprised;
    }
    if (Contains(emotion, "think") || Contains(emotion, "confused") || Contains(emotion, "ponder") || Contains(emotion, "curious")) {
        return FaceMode::kThinking;
    }
    if (Contains(emotion, "sleepy") || Contains(emotion, "tired") || Contains(emotion, "yawn") || Contains(emotion, "exhaust")) {
        return FaceMode::kSleepy;
    }
    if (Contains(emotion, "sleeping") || Contains(emotion, "sleep")) {
        return FaceMode::kSleeping;
    }
    return FaceMode::kNeutral;
}

uint32_t PocketOledDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}
