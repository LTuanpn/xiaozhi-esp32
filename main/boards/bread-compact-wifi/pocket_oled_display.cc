#include "pocket_oled_display.h"

#include "application.h"
#include "assets/lang_config.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kAutoSleepTimeoutTicks = 50000 / kAnimationPeriodMs;  // 50s = 1250 ticks
constexpr uint32_t kSleepyTimeoutTicks = 35000 / kAnimationPeriodMs;     // 35s = 875 ticks

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

// Eyelids close fast and open a little slower, like a real blink.
int EaseLid(int current, int target) {
    if (current == target) {
        return current;
    }
    const int delta = target - current;
    if (delta < 0) {
        return current + std::min(-1, (delta * 3) / 4);
    }
    return current + std::max(1, (delta * 3) / 5);
}

// Rounded-rectangle eye: corner radius follows the shorter side so the eye stays soft
int EyeRadius(int width, int height) {
    return std::clamp(static_cast<int>(std::round(std::min(width, height) * 8.0f / 38.0f)), 2, 8);
}

// Khóa radius ở 2..3 để con ngươi luôn có form chữ nhật bo góc vuông sắc nét
int PupilRadius(int width, int height) {
    const int shorter = std::min(width, height);
    if (shorter <= 7) return 2;
    if (shorter <= 11) return 3;
    if (shorter <= 15) return 5;
    return shorter / 2;
}

// Vẽ đĩa tròn pixel-art: Cấm cắt chữ thập ở 3px và cấm cắt 2 tầng ở 6-7px (Xóa sổ hình thoi!)
void PlaceDisc(lv_obj_t* wide, lv_obj_t* tall, lv_obj_t* mid, int x, int y, int diameter) {
    if (wide == nullptr) return;
    if (diameter <= 0) {
        lv_obj_add_flag(wide, LV_OBJ_FLAG_HIDDEN);
        if (tall) lv_obj_add_flag(tall, LV_OBJ_FLAG_HIDDEN);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(wide, LV_OBJ_FLAG_HIDDEN);

    // Chấm nhỏ 1..3px: giữ nguyên khối đặc, không cắt góc chữ thập
    if (diameter <= 3 || tall == nullptr) {
        lv_obj_set_pos(wide, x, y);
        lv_obj_set_size(wide, diameter, diameter);
        if (tall) lv_obj_add_flag(tall, LV_OBJ_FLAG_HIDDEN);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 4px cắt góc sẽ thành hình thoi -> tự động nâng lên 5px tròn bầu
    if (diameter == 4) diameter = 5;

    // Đĩa 5..7px: chỉ vát đúng 1px ở 4 góc -> đỉnh phẳng 3..5px tròn căng!
    if (diameter <= 7 || mid == nullptr) {
        lv_obj_set_pos(wide, x, y + 1);
        lv_obj_set_size(wide, diameter, diameter - 2);
        lv_obj_remove_flag(tall, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tall, x + 1, y);
        lv_obj_set_size(tall, diameter - 2, diameter);
        if (mid) lv_obj_add_flag(mid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Đĩa đại 8..10px: vát 2 tầng
    lv_obj_set_pos(wide, x, y + 2);
    lv_obj_set_size(wide, diameter, diameter - 4);
    lv_obj_remove_flag(tall, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(tall, x + 2, y);
    lv_obj_set_size(tall, diameter - 4, diameter);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(mid, x + 1, y + 1);
    lv_obj_set_size(mid, diameter - 2, diameter - 2);
}

int32_t NormalizeAngle(int32_t angle) {
    return ((angle % 3600) + 3600) % 3600;
}
}  // namespace

PocketOledDisplay::PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel, int width, int height,
                                     bool mirror_x, bool mirror_x_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_x_y) {}

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

    // 1. Base Eyes (Rectangular: W=44, H=36, radius=9)
    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 8, 0);
    }

    // 2. Black Pupils (Proportional rounded rectangle: W=14, H=16, radius=4)
    left_pupil_ = lv_obj_create(face_layer_);
    right_pupil_ = lv_obj_create(face_layer_);
    for (auto* pupil : {left_pupil_, right_pupil_}) {
        PrepareShape(pupil);
        lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pupil, 6, 0);
    }

    // 2b. Catchlights (3-tier pixel matrix per eye + secondary glint)
    for (auto& eye_glints : catchlight_) {
        for (auto*& glint : eye_glints) {
            glint = lv_obj_create(face_layer_);
            PrepareShape(glint);
            lv_obj_set_style_bg_color(glint, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(glint, 0, 0);
            lv_obj_add_flag(glint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 3. Sparkles & Dizzy X (Drawn inside eyes, underneath eyelids)
    left_dizzy_a_ = lv_obj_create(face_layer_);
    left_dizzy_b_ = lv_obj_create(face_layer_);
    right_dizzy_a_ = lv_obj_create(face_layer_);
    right_dizzy_b_ = lv_obj_create(face_layer_);
    for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
        PrepareShape(d);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(d, 2, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }

    left_sparkle_a_ = lv_obj_create(face_layer_);
    left_sparkle_b_ = lv_obj_create(face_layer_);
    right_sparkle_a_ = lv_obj_create(face_layer_);
    right_sparkle_b_ = lv_obj_create(face_layer_);
    for (auto* s : {left_sparkle_a_, left_sparkle_b_, right_sparkle_a_, right_sparkle_b_}) {
        PrepareShape(s);
        lv_obj_set_style_bg_color(s, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. Crescent Masks (For curved sleeping eyes and winking eyes)
    left_crescent_mask_ = lv_obj_create(face_layer_);
    right_crescent_mask_ = lv_obj_create(face_layer_);
    for (auto* mask : {left_crescent_mask_, right_crescent_mask_}) {
        PrepareShape(mask);
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mask, 12, 0);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
    }

    // 5. Eyelids (Angled top lids for angry / sad, lower lids for squinting)
    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);
    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 0, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // 6. Dual Teardrops for crying
    left_tear_ = lv_obj_create(face_layer_);
    right_tear_ = lv_obj_create(face_layer_);
    for (auto* tear : {left_tear_, right_tear_}) {
        PrepareShape(tear);
        lv_obj_set_style_bg_color(tear, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(tear, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tear, 3, 0);
        lv_obj_add_flag(tear, LV_OBJ_FLAG_HIDDEN);
    }

    // 7. Blushing marks & FX Labels
    left_blush_ = lv_label_create(face_layer_);
    right_blush_ = lv_label_create(face_layer_);
    for (auto* blush : {left_blush_, right_blush_}) {
        lv_label_set_text(blush, "///");
        lv_obj_set_style_text_color(blush, lv_color_white(), 0);
        lv_obj_set_style_text_font(blush, &lv_font_montserrat_14, 0);
        lv_obj_add_flag(blush, LV_OBJ_FLAG_HIDDEN);
    }

    fx_label_a_ = lv_label_create(face_layer_);
    fx_label_b_ = lv_label_create(face_layer_);
    for (auto* lbl : {fx_label_a_, fx_label_b_}) {
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // 8. Foreground Special Expression Elements
    left_heart_a_ = lv_obj_create(face_layer_);
    left_heart_b_ = lv_obj_create(face_layer_);
    right_heart_a_ = lv_obj_create(face_layer_);
    right_heart_b_ = lv_obj_create(face_layer_);
    for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
        PrepareShape(h);
        lv_obj_set_style_bg_color(h, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(h, 8, 0);
        lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    }

    sweat_drop_ = lv_obj_create(face_layer_);
    PrepareShape(sweat_drop_);
    lv_obj_set_style_bg_color(sweat_drop_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sweat_drop_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sweat_drop_, 3, 0);
    lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);

    laser_line_ = lv_obj_create(face_layer_);
    PrepareShape(laser_line_);
    lv_obj_set_style_bg_color(laser_line_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(laser_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(laser_line_, 1, 0);
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
        lv_obj_set_style_bg_color(music_note_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(music_note_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(music_note_[i], 2, 0);
        lv_obj_add_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 6; ++i) {
        extra_shape_[i] = lv_obj_create(face_layer_);
        PrepareShape(extra_shape_[i]);
        lv_obj_set_style_bg_color(extra_shape_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(extra_shape_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(extra_shape_[i], 2, 0);
        lv_obj_add_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 9. Chat / Status Line with Circular Marquee Scrolling
    chat_line_ = lv_label_create(screen);
    lv_obj_set_pos(chat_line_, 0, 48);
    lv_obj_set_size(chat_line_, 128, 16);
    lv_obj_set_style_bg_color(chat_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(chat_line_, 2, 0);
    lv_obj_set_style_pad_right(chat_line_, 2, 0);
    lv_obj_set_width(chat_line_, 128);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(chat_line_, 4000, 0);
    lv_label_set_text(chat_line_, "");

    ApplyLayout();
    lv_obj_move_foreground(chat_line_);
    animation_timer_ = lv_timer_create(AnimationTimerCallback, kAnimationPeriodMs, this);
}

void PocketOledDisplay::WakeUp() {
    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kSleepy) {
        wake_up_until_tick_ = animation_tick_ + 30;  // 1.2s wake up animation
        SetFaceMode(FaceMode::kWakingUp);
        if (chat_line_ != nullptr && !has_chat_text_) {
            lv_label_set_text(chat_line_, "");
        }
    }
    idle_quirk_ = IdleQuirk::kNone;
    last_active_tick_ = animation_tick_;
    temporary_emotion_until_tick_ = 0;
}

void PocketOledDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void PocketOledDisplay::ShowNotification(const char* notification, int duration_ms) {
    OledDisplay::ShowNotification(notification, duration_ms);
    DisplayLockGuard lock(this);
    if (notification == nullptr) {
        return;
    }
    if (chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_text(chat_line_, notification);
        notification_until_tick_ =
            animation_tick_ + std::max<uint32_t>(1, duration_ms / kAnimationPeriodMs);
    }
}

void PocketOledDisplay::SetStatus(const char* status) {
    OledDisplay::SetStatus(status);
    DisplayLockGuard lock(this);

    if (status == nullptr) {
        return;
    }
    last_status_text_ = status;

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

    if (!has_chat_text_ && chat_line_ != nullptr && animation_tick_ >= notification_until_tick_) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (face_mode_ == FaceMode::kSleeping) {
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
    temporary_emotion_until_tick_ = animation_tick_ + (5000 / kAnimationPeriodMs);
    return true;
}

void PocketOledDisplay::SetChatMessage(const char* role, const char* content) {
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
    } else {
        clean_text.clear();
    }

    has_chat_text_ = !clean_text.empty();

    if (chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_text(chat_line_, clean_text.c_str());
    }

    if (role != nullptr && std::strcmp(role, "assistant") == 0) {
        is_speaking_ = true;
        last_active_tick_ = animation_tick_;

        if (!target_emotion.empty()) {
            emotion_mode_ = FaceModeFromEmotion(target_emotion.c_str());
            SetFaceMode(emotion_mode_);
        } else if (emotion_mode_ == FaceMode::kNeutral) {
            FaceMode inferred = InferEmotionFromText(clean_text.c_str());
            if (inferred != FaceMode::kNeutral) {
                emotion_mode_ = inferred;
                SetFaceMode(inferred);
            } else if (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening) {
                SetFaceMode(FaceMode::kSmileGentle);
            }
        } else if (face_mode_ == FaceMode::kNeutral) {
            SetFaceMode(emotion_mode_);
        }
    }
}

void PocketOledDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    has_chat_text_ = false;
    is_speaking_ = false;
    last_active_tick_ = animation_tick_;
    if (chat_line_ != nullptr && animation_tick_ >= notification_until_tick_) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, last_status_text_.c_str());
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
    if (chat_line_ != nullptr && animation_tick_ >= notification_until_tick_) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, last_status_text_.c_str());
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
    lv_obj_set_style_radius(left_eye_, EyeRadius(current_left_width_, current_left_height_), 0);
    lv_obj_set_style_radius(right_eye_, EyeRadius(current_right_width_, current_right_height_), 0);
    current_left_width_ = std::min(current_left_width_, 40);
    current_right_width_ = std::min(current_right_width_, 40);
    current_left_height_ = std::min(current_left_height_, 38);
    current_right_height_ = std::min(current_right_height_, 38);
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

    // Restore chat_line_ after temporary notification expires
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
    const uint8_t spk_level = app.GetAudioService().GetOutputLevel();

    // RECTANGULAR EYE GEOMETRY (W=44, H=36, radius=9, Gap=12)
    constexpr int face_width = 128;
    constexpr int eye_width = 40;   // Rộng 40 (thay vì 44)
    constexpr int eye_height = 38;  // Cao 38 (thay vì 36)
    constexpr int eye_gap = 14;     // Cách nhau 14 (thay vì 12)
    const int pair_width = eye_width * 2 + eye_gap;
    const int base_x = (face_width - pair_width) / 2;
    const int base_y = (48 - eye_height) / 2;
    constexpr int max_look_x = 6;
    constexpr int max_look_y = 4;

    // 1. Temporary Emotion Timeout
    if (temporary_emotion_until_tick_ > 0 && animation_tick_ >= temporary_emotion_until_tick_ && !is_speaking_) {
        temporary_emotion_until_tick_ = 0;
        emotion_mode_ = FaceMode::kNeutral;
        SetFaceMode(FaceMode::kNeutral);
    }

    // 2. Autonomous Sleep & Mini Idle Quirks
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp && temporary_emotion_until_tick_ == 0) {
        if (animation_tick_ >= last_active_tick_ + kAutoSleepTimeoutTicks) {
            idle_quirk_ = IdleQuirk::kNone;
            SetFaceMode(FaceMode::kSleeping);
            is_speaking_ = false;
            if (chat_line_ != nullptr && !has_chat_text_) {
                lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_label_set_text(chat_line_, "zZz...");
            }
        } else if (animation_tick_ >= last_active_tick_ + kSleepyTimeoutTicks && face_mode_ == FaceMode::kNeutral) {
            idle_quirk_ = IdleQuirk::kNone;
            SetFaceMode(FaceMode::kSleepy);
        } else if (face_mode_ == FaceMode::kNeutral && !is_speaking_) {
            if (idle_quirk_ != IdleQuirk::kNone && animation_tick_ >= quirk_until_tick_) {
                idle_quirk_ = IdleQuirk::kNone;
                next_quirk_tick_ = animation_tick_ + 300 + (NextRandom() % 200);
            } else if (idle_quirk_ == IdleQuirk::kNone && animation_tick_ >= next_quirk_tick_) {
                uint32_t pick = NextRandom() % 3;
                quirk_start_tick_ = animation_tick_;
                if (pick == 0) {
                    idle_quirk_ = IdleQuirk::kBugTracking;
                    quirk_until_tick_ = animation_tick_ + 75;
                } else if (pick == 1) {
                    idle_quirk_ = IdleQuirk::kPeekABoo;
                    quirk_until_tick_ = animation_tick_ + 60;
                } else {
                    idle_quirk_ = IdleQuirk::kEyeRoll;
                    quirk_until_tick_ = animation_tick_ + 48;
                }
            }
        } else {
            idle_quirk_ = IdleQuirk::kNone;
        }
    }

    // 3. Waking Up Transition
    if (face_mode_ == FaceMode::kWakingUp) {
        if (animation_tick_ >= wake_up_until_tick_) {
            SetFaceMode(FaceMode::kNeutral);
        }
    }

    // 4. Natural Blinking with Double-Blink Realism
    bool blink = false;
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink &&
        face_mode_ != FaceMode::kEcstaticLaugh && face_mode_ != FaceMode::kHeartbroken &&
        face_mode_ != FaceMode::kLove && face_mode_ != FaceMode::kSneezing &&
        face_mode_ != FaceMode::kRebooting && face_mode_ != FaceMode::kGameOver &&
        face_mode_ != FaceMode::kZenMeditation && face_mode_ != FaceMode::kAngelBlessing) {
        if (animation_tick_ >= next_blink_tick_) {
            blink_until_tick_ = animation_tick_ + 2;
            if (face_mode_ == FaceMode::kInnocent || (NextRandom() % 100) < 35) {
                second_blink_start_tick_ = animation_tick_ + 5;
                second_blink_until_tick_ = animation_tick_ + 7;
            } else {
                second_blink_start_tick_ = 0;
                second_blink_until_tick_ = 0;
            }
            next_blink_tick_ = (face_mode_ == FaceMode::kInnocent)
                                   ? (animation_tick_ + 28 + (NextRandom() % 25))
                                   : (animation_tick_ + 70 + (NextRandom() % 100));
        }
        blink = (animation_tick_ < blink_until_tick_) ||
                (second_blink_start_tick_ > 0 && animation_tick_ >= second_blink_start_tick_ &&
                 animation_tick_ < second_blink_until_tick_);
    }

    // 5. Look Around & Quirk Gaze Tracking
    int bug_x = 0;
    int bug_y = 0;
    if (face_mode_ == FaceMode::kNeutral && idle_quirk_ != IdleQuirk::kNone && !is_speaking_) {
        uint32_t qt = animation_tick_ - quirk_start_tick_;
        if (idle_quirk_ == IdleQuirk::kBugTracking) {
            bug_x = 12 + static_cast<int>((qt * 104) / 75);
            int wave = static_cast<int>(qt % 20) - 10;
            bug_y = 22 + (wave < 0 ? -wave : wave) - 5;
            target_look_x_ = (bug_x - 64) / 9;
            target_look_y_ = (bug_y - 24) / 4;
        } else if (idle_quirk_ == IdleQuirk::kPeekABoo) {
            if (qt < 42) {
                target_look_x_ = -6;
                target_look_y_ = 2;
            } else {
                target_look_x_ = 0;
                target_look_y_ = 0;
            }
        } else if (idle_quirk_ == IdleQuirk::kEyeRoll) {
            int phase = (qt / 12) % 4;
            if (phase == 0) { target_look_x_ = -5; target_look_y_ = 0; }
            else if (phase == 1) { target_look_x_ = 0; target_look_y_ = -4; }
            else if (phase == 2) { target_look_x_ = 5; target_look_y_ = 0; }
            else { target_look_x_ = 0; target_look_y_ = 4; }
        }
    } else if (animation_tick_ >= next_idle_tick_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kSleepy) && !is_speaking_) {
        target_look_x_ = static_cast<int>(NextRandom() % (max_look_x * 2 + 1)) - max_look_x;
        target_look_y_ = static_cast<int>(NextRandom() % (max_look_y * 2 + 1)) - max_look_y;
        next_idle_tick_ = animation_tick_ + 50 + (NextRandom() % 80);
    }

    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken ||
        face_mode_ == FaceMode::kDizzy || face_mode_ == FaceMode::kScared ||
        face_mode_ == FaceMode::kRebooting || face_mode_ == FaceMode::kZenMeditation ||
        face_mode_ == FaceMode::kGameOver || face_mode_ == FaceMode::kHypnotized ||
        face_mode_ == FaceMode::kAngelBlessing) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kLoadingBuffer) {
        target_look_x_ = ((animation_tick_ / 6) % 2 == 0) ? -2 : 2;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kInnocent) {
        target_look_x_ = ((animation_tick_ / 22) % 2 == 0) ? -3 : 3;
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kThinking) {
        int read_cycle = animation_tick_ % 36;
        if (read_cycle < 20) {
            target_look_x_ = -5 + (read_cycle * 10) / 20;
            target_look_y_ = -2;
        } else {
            target_look_x_ = max_look_x;
            target_look_y_ = -max_look_y;
        }
    } else if (face_mode_ == FaceMode::kListening) {
        target_look_x_ = 0;
        target_look_y_ = (mic_level > 15) ? 0 : (((animation_tick_ / 8) % 2 == 0) ? -2 : 1);
    } else if (face_mode_ == FaceMode::kShy) {
        target_look_x_ = -4;
        target_look_y_ = 3;
    } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        target_look_x_ = 0;
        target_look_y_ = 3;
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kJealous) {
        target_look_x_ = 0;
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        target_look_x_ = 0;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kSmug) {
        target_look_x_ = 4;
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kSideEye) {
        target_look_x_ = 6;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kScheming) {
        target_look_x_ = ((animation_tick_ / 15) % 2 == 0) ? -4 : 4;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kWhispering) {
        target_look_x_ = -5;
        target_look_y_ = 1;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kDisgusted) {
        target_look_x_ = -3;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kParanoid) {
        target_look_x_ = (animation_tick_ % 10 < 5) ? -6 : 6;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kConfusedQuestion) {
        target_look_x_ = 3;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kIdeaEureka) {
        target_look_x_ = 0;
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_look_x_ = 0;
        target_look_y_ = 3;
    } else if (face_mode_ == FaceMode::kDrunk) {
        target_look_x_ = ((animation_tick_ / 20) % 2 == 0) ? -4 : 4;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kBored) {
        int bored_phase = (animation_tick_ / 25) % 3;
        if (bored_phase == 0) {
            target_look_x_ = -5; target_look_y_ = 2;
        } else if (bored_phase == 1) {
            target_look_x_ = 5; target_look_y_ = 2;
        } else {
            target_look_x_ = 0; target_look_y_ = -4;
        }
    } else if (face_mode_ == FaceMode::kScanning) {
        target_look_x_ = ((animation_tick_ / 6) % 2 == 0) ? -5 : 5;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kSkeptical) {
        target_look_x_ = ((animation_tick_ / 25) % 2 == 0) ? -2 : 3;
        target_look_y_ = -1;
    } else if (face_mode_ == FaceMode::kPout) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kSunnyDazzle) {
        target_look_x_ = 0;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kMusic) {
        target_look_x_ = 0;
        target_look_y_ = 1;
    } else if (is_speaking_ || face_mode_ == FaceMode::kWakingUp || face_mode_ == FaceMode::kSmileGentle ||
               face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPlayfulWink ||
               face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kTongueOut) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    // 6. Multi-tier Eye Heights & Squash-and-Stretch Widths
    int target_left_height = eye_height;
    int target_right_height = eye_height;
    int target_left_width = eye_width;
    int target_right_width = eye_width;
    int target_left_y_offset = 0;
    int target_right_y_offset = 0;

    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kZenMeditation ||
        face_mode_ == FaceMode::kAngelBlessing) {
        target_left_height = 20;
        target_right_height = 20;
        target_left_y_offset = 4;
        target_right_y_offset = 4;
    } else if (blink) {
        target_left_height = 2;
        target_right_height = 2;
        target_left_width = 38;
        target_right_width = 38;
    } else if (face_mode_ == FaceMode::kListening) {
        int boost = (mic_level > 15) ? std::min(5, static_cast<int>(mic_level / 18)) : -2;
        target_left_height = target_right_height = eye_height + boost;
        target_left_width = target_right_width = eye_width - (boost > 2 ? 1 : 0);
    } else if (face_mode_ == FaceMode::kNeutral && idle_quirk_ == IdleQuirk::kPeekABoo) {
        uint32_t qt = animation_tick_ - quirk_start_tick_;
        if (qt < 42) {
            target_left_height = target_right_height = 22;
            target_left_width = target_right_width = 36;
        } else {
            target_left_height = target_right_height = 42;
            target_left_width = target_right_width = 33;
        }
    } else if (face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) {
        target_left_height = 0;
        target_right_height = 0;
    } else if (face_mode_ == FaceMode::kRebooting) {
        int ph = animation_tick_ % 30;
        if (ph < 10) {
            target_left_height = target_right_height = 2;
            target_left_width = target_right_width = 40;
        } else if (ph < 20) {
            target_left_height = target_right_height = 2;
            target_left_width = target_right_width = 8;
        } else {
            target_left_height = target_right_height = eye_height;
            target_left_width = target_right_width = eye_width;
        }
    } else if (face_mode_ == FaceMode::kLoadingBuffer) {
        target_left_height = target_right_height = 28;
        target_left_width = target_right_width = 38;
        target_left_y_offset = target_right_y_offset = 5;
    } else if (face_mode_ == FaceMode::kLowBattery) {
        target_left_height = target_right_height = 16;
        target_left_width = target_right_width = 36;
        target_left_y_offset = target_right_y_offset = 8;
    } else if (face_mode_ == FaceMode::kFacepalm) {
        target_left_height = 8;
        target_right_height = 14;
        target_left_y_offset = 6;
        target_right_y_offset = 6;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        target_left_height = 16;
        target_left_width = 36;
        target_left_y_offset = 2;
        target_right_height = 40;
        target_right_width = 34;
    } else if (face_mode_ == FaceMode::kDisgusted) {
        target_left_height = 16;
        target_right_height = 24;
        target_left_width = 36;
        target_left_y_offset = 5;
        target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kWhispering) {
        target_left_height = target_right_height = 24;
        target_left_y_offset = target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kSneezing) {
        int ph = animation_tick_ % 35;
        if (ph < 22) {
            target_left_height = target_right_height = 38 + (ph / 4);
            target_left_width = target_right_width = 33;
            target_left_y_offset = target_right_y_offset = -(ph / 5);
        } else {
            target_left_height = target_right_height = 4;
            target_left_width = target_right_width = 40;
            target_left_y_offset = target_right_y_offset = 8;
        }
    } else if (face_mode_ == FaceMode::kInnocent || face_mode_ == FaceMode::kShockedJawdrop || face_mode_ == FaceMode::kIdeaEureka) {
        target_left_height = target_right_height = 42;
        target_left_width = target_right_width = 36;
        target_left_y_offset = target_right_y_offset = (face_mode_ == FaceMode::kShockedJawdrop) ? -3 : 1;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_left_height = target_right_height = 12;
        target_left_width = target_right_width = 38;
        target_left_y_offset = target_right_y_offset = 10;
    } else if (face_mode_ == FaceMode::kParanoid) {
        target_left_height = target_right_height = 16;
        target_left_width = target_right_width = 40;
        target_left_y_offset = target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kStealthNinja) {
        target_left_height = target_right_height = 10;
        target_left_width = target_right_width = 38;
        target_left_y_offset = target_right_y_offset = 0;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        int glitch_squash = (animation_tick_ % 5) * 4;
        target_left_height = eye_height - glitch_squash;
        target_right_height = 20 + glitch_squash;
        target_left_y_offset = (animation_tick_ % 2 == 0) ? -3 : 3;
        target_right_y_offset = -target_left_y_offset;
    } else if (face_mode_ == FaceMode::kHeadache) {
        target_left_height = 14;
        target_right_height = 24;
        target_left_y_offset = 4;
        target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kSunnyDazzle) {
        target_left_height = target_right_height = 26;
        target_left_y_offset = target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        target_left_height = eye_height;
        target_right_height = 20;
        target_right_width = 36;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kTongueOut) {
        target_left_height = target_right_height = 24;
        target_left_width = target_right_width = 38;
        target_left_y_offset = target_right_y_offset = -2;
    } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kConfusedQuestion || face_mode_ == FaceMode::kDrunk) {
        target_left_height = eye_height + 4;
        target_left_width = 34;
        target_left_y_offset = -2;
        target_right_height = 22;
        target_right_width = 36;
        target_right_y_offset = 5;
    } else if (face_mode_ == FaceMode::kSmug) {
        target_left_height = eye_height;
        target_right_height = eye_height - 2;
        target_right_y_offset = -2;
    } else if (face_mode_ == FaceMode::kSurprised || face_mode_ == FaceMode::kWakingUp) {
        target_left_height = 44;
        target_right_height = 44;
        target_left_width = 34;
        target_right_width = 34;
    } else if (face_mode_ == FaceMode::kScanning) {
        target_left_height = 28;
        target_right_height = 28;
        target_left_width = 36;
        target_right_width = 36;
    } else if (face_mode_ == FaceMode::kScared) {
        target_left_height = 38;
        target_right_height = 38;
        target_left_width = 34;
        target_right_width = 34;
    } else if (face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kPout) {
        target_left_height = eye_height + 2;
        target_right_height = eye_height + 2;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kCryingStream) {
        target_left_height = eye_height - 4;
        target_right_height = eye_height - 4;
        target_left_y_offset = 2;
        target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kShy) {
        target_left_height = eye_height - 4;
        target_right_height = eye_height - 4;
        target_left_y_offset = 3;
        target_right_y_offset = 3;
    }

    // Breathing effect
    if (!blink && !is_speaking_ &&
        (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening ||
         face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kSleepy)) {
        const int phase = static_cast<int>(animation_tick_ % 120);
        const int triangle = (phase < 60) ? phase : (120 - phase);
        const int breath = (triangle * 3) / 61;
        target_left_height += breath;
        target_right_height += breath;
    }

    const bool lid_snap =
        animation_tick_ < std::max(blink_until_tick_, second_blink_until_tick_) + 6;
    if (lid_snap) {
        current_left_height_ = EaseLid(current_left_height_, target_left_height);
        current_right_height_ = EaseLid(current_right_height_, target_right_height);
    } else {
        current_left_height_ = Ease(current_left_height_, target_left_height);
        current_right_height_ = Ease(current_right_height_, target_right_height);
    }
    current_left_width_ = Ease(current_left_width_, target_left_width);
    current_right_width_ = Ease(current_right_width_, target_right_width);
    current_left_y_offset_ = Ease(current_left_y_offset_, target_left_y_offset);
    current_right_y_offset_ = Ease(current_right_y_offset_, target_right_y_offset);

    // 7. Animations: Audio-reactive speaking bounce, Laugh bounce, Rage shake
    int animation_x = 0;
    int animation_y = 0;
    if (is_speaking_) {
        int amp_bounce = 1 + (spk_level / 32);
        animation_y = ((animation_tick_ / 2) % 2 == 0) ? -amp_bounce : (amp_bounce / 2);
    } else if (face_mode_ == FaceMode::kSleeping) {
        uint32_t sleep_ticks = (animation_tick_ > last_active_tick_ + kAutoSleepTimeoutTicks)
                                   ? (animation_tick_ - (last_active_tick_ + kAutoSleepTimeoutTicks))
                                   : 0;
        if (sleep_ticks > 200 && (sleep_ticks % 300) < 16) {
            animation_x = (animation_tick_ % 2 == 0) ? -1 : 1;
        }
    } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kHeartbroken) {
        animation_y = (animation_tick_ % 4 < 2) ? -1 : 0;
    } else if (face_mode_ == FaceMode::kExcited) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -3 : 0;
    } else if (face_mode_ == FaceMode::kMusic) {
        animation_x = ((animation_tick_ / 10) % 2 == 0) ? -3 : 3;
        animation_y = ((animation_tick_ / 5) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kSmug) {
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -1 : 0;
    } else if (face_mode_ == FaceMode::kDizzy) {
        animation_x = ((animation_tick_ / 4) % 4 < 2) ? -2 : 2;
        animation_y = ((animation_tick_ / 6) % 4 < 2) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kPout) {
        animation_y = ((animation_tick_ / 8) % 2 == 0) ? -1 : 0;
    } else if (face_mode_ == FaceMode::kFreezing) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kHeadache || face_mode_ == FaceMode::kOverheat) {
        animation_x = (animation_tick_ % 4 < 2) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kThunderShock) {
        animation_x = (animation_tick_ % 2 == 0) ? -3 : 3;
        animation_y = (animation_tick_ % 3 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kPartyCelebrate) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -3 : 1;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        animation_x = (animation_tick_ % 3 == 0) ? -4 : 3;
    } else if (face_mode_ == FaceMode::kZenMeditation || face_mode_ == FaceMode::kAngelBlessing) {
        animation_y = ((animation_tick_ / 20) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kDrunk) {
        animation_x = ((animation_tick_ / 15) % 2 == 0) ? -2 : 2;
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kSneezing) {
        int ph = animation_tick_ % 35;
        if (ph >= 22 && ph < 28) {
            animation_x = (ph % 2 == 0) ? -3 : 3;
            animation_y = 4;
        }
    }

    if (face_mode_ == FaceMode::kThinking) {
        animation_y = ((animation_tick_ / 6) % 2 == 0) ? -1 : 0;
    } else if (face_mode_ == FaceMode::kSurprised) {
        animation_x = (animation_tick_ % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;
        animation_y = (animation_tick_ % 3 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kScared) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;
        animation_y = (animation_tick_ % 2 == 0) ? 1 : -1;
    }

    // Micro-saccades
    if (animation_tick_ >= next_micro_tick_) {
        micro_look_x_ = static_cast<int>(NextRandom() % 3) - 1;
        micro_look_y_ = (NextRandom() % 4 == 0) ? static_cast<int>(NextRandom() % 3) - 1 : 0;
        micro_until_tick_ = animation_tick_ + 3 + (NextRandom() % 4);
        next_micro_tick_ = animation_tick_ + 20 + (NextRandom() % 45);
    }
    if (animation_tick_ >= micro_until_tick_) {
        micro_look_x_ = 0;
        micro_look_y_ = 0;
    }
    const bool micro_enabled =
        !blink && !is_speaking_ && idle_quirk_ == IdleQuirk::kNone &&
        (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening ||
         face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kSleepy);
    const int look_x = current_look_x_ + (micro_enabled ? micro_look_x_ : 0);
    const int look_y = current_look_y_ + (micro_enabled ? micro_look_y_ : 0);

    const int left_x = base_x + (eye_width - current_left_width_) / 2 + look_x + animation_x;
    const int right_x = base_x + eye_width + eye_gap + (eye_width - current_right_width_) / 2 + look_x + animation_x;
    const int left_y =
        base_y + (eye_height - current_left_height_) / 2 + look_y + animation_y + current_left_y_offset_;
    const int right_y =
        base_y + (eye_height - current_right_height_) / 2 + look_y + animation_y + current_right_y_offset_;

    if (face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_eye_, left_x, left_y);
        lv_obj_set_size(left_eye_, current_left_width_, current_left_height_);
        lv_obj_set_pos(right_eye_, right_x, right_y);
        lv_obj_set_size(right_eye_, current_right_width_, current_right_height_);
    }

    int left_radius = EyeRadius(current_left_width_, current_left_height_);
    int right_radius = EyeRadius(current_right_width_, current_right_height_);
    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kZenMeditation ||
        face_mode_ == FaceMode::kAngelBlessing) {
        left_radius = 12;
        right_radius = 12;
    }
    lv_obj_set_style_radius(left_eye_, left_radius, 0);
    lv_obj_set_style_radius(right_eye_, right_radius, 0);

    lv_obj_set_style_transform_rotation(left_eye_, 0, 0);
    lv_obj_set_style_transform_rotation(right_eye_, 0, 0);

    // 8. Black Pupils (W=14, H=16 standard: plump oval, room for 2px left dot and 6px round right disc)
    const bool show_left_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kLove &&
                                 face_mode_ != FaceMode::kHeartbroken && face_mode_ != FaceMode::kDizzy &&
                                 face_mode_ != FaceMode::kRebooting &&
                                 face_mode_ != FaceMode::kGameOver && face_mode_ != FaceMode::kFacepalm &&
                                 face_mode_ != FaceMode::kZenMeditation && face_mode_ != FaceMode::kAngelBlessing &&
                                 face_mode_ != FaceMode::kHypnotized &&
                                 !(face_mode_ == FaceMode::kSneezing && (animation_tick_ % 35) >= 22);
    const bool show_right_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink &&
                                  face_mode_ != FaceMode::kLove && face_mode_ != FaceMode::kHeartbroken &&
                                  face_mode_ != FaceMode::kDizzy &&
                                  face_mode_ != FaceMode::kRebooting && face_mode_ != FaceMode::kGameOver &&
                                  face_mode_ != FaceMode::kZenMeditation && face_mode_ != FaceMode::kAngelBlessing &&
                                  face_mode_ != FaceMode::kHypnotized &&
                                  !(face_mode_ == FaceMode::kSneezing && (animation_tick_ % 35) >= 22);

    int left_pupil_w = 14;
    int left_pupil_h = 16;
    int right_pupil_w = 14;
    int right_pupil_h = 16;

    if (face_mode_ == FaceMode::kListening && mic_level > 10) {
        int boost = (mic_level > 60) ? 2 : 1;
        left_pupil_w = right_pupil_w = 14 + boost;
        left_pupil_h = right_pupil_h = 16 + boost;
    } else if (face_mode_ == FaceMode::kSurprised) {
        uint32_t elapsed = (animation_tick_ > surprised_start_tick_) ? (animation_tick_ - surprised_start_tick_) : 0;
        if (elapsed < 15) {
            left_pupil_w = right_pupil_w = 10;
            left_pupil_h = right_pupil_h = 12;
        } else {
            left_pupil_w = right_pupil_w = 18;
            left_pupil_h = right_pupil_h = 20;
        }
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kScheming ||
               face_mode_ == FaceMode::kSideEye || face_mode_ == FaceMode::kJealous) {
        left_pupil_w = right_pupil_w = 12;
        left_pupil_h = right_pupil_h = 14;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        left_pupil_w = right_pupil_w = 12;
        left_pupil_h = right_pupil_h = 13;
    } else if (face_mode_ == FaceMode::kScared || face_mode_ == FaceMode::kThunderShock ||
               face_mode_ == FaceMode::kShockedJawdrop) {
        left_pupil_w = right_pupil_w = 10;
        left_pupil_h = right_pupil_h = 11;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        left_pupil_w = 12;
        left_pupil_h = 11;
        right_pupil_w = 9;
        right_pupil_h = 9;
    } else if (face_mode_ == FaceMode::kPout || face_mode_ == FaceMode::kInnocent) {
        left_pupil_w = right_pupil_w = 18;
        left_pupil_h = right_pupil_h = 20;
    } else if (face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kMoneyEyes ||
               face_mode_ == FaceMode::kSunnyDazzle) {
        left_pupil_w = right_pupil_w = 16;
        left_pupil_h = right_pupil_h = 18;
    } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kDisgusted) {
        left_pupil_w = 14;
        left_pupil_h = 16;
        right_pupil_w = 12;
        right_pupil_h = 12;
    } else if (face_mode_ == FaceMode::kParanoid) {
        left_pupil_w = right_pupil_w = 10;
        left_pupil_h = right_pupil_h = 11;
    } else if (face_mode_ == FaceMode::kSleepwalking) {
        left_pupil_w = right_pupil_w = 8;
        left_pupil_h = right_pupil_h = 9;
    }

    smooth_pupil_w_[0] = Ease(smooth_pupil_w_[0], left_pupil_w);
    smooth_pupil_h_[0] = Ease(smooth_pupil_h_[0], left_pupil_h);
    smooth_pupil_w_[1] = Ease(smooth_pupil_w_[1], right_pupil_w);
    smooth_pupil_h_[1] = Ease(smooth_pupil_h_[1], right_pupil_h);
    left_pupil_w = smooth_pupil_w_[0];
    left_pupil_h = smooth_pupil_h_[0];
    right_pupil_w = smooth_pupil_w_[1];
    right_pupil_h = smooth_pupil_h_[1];

    int left_pupil_pos_x = 0;
    int left_pupil_pos_y = 0;
    int right_pupil_pos_x = 0;
    int right_pupil_pos_y = 0;

    auto place_pupil = [&](int eye_x, int eye_y, int eye_w, int eye_h, int pw, int ph,
                           int& out_x, int& out_y) {
        const float cx = eye_x + (eye_w - pw) / 2.0f;
        const float cy = eye_y + (eye_h - ph) / 2.0f;
        float dx = look_x * 5 / 4.0f;
        float dy = look_y * 5 / 4.0f;
        const float ax = std::max(1.0f, (eye_w - pw) / 2.0f - 2.5f);
        const float ay = std::max(1.0f, (eye_h - ph) / 2.0f - 2.0f);
        const float k = (dx / ax) * (dx / ax) + (dy / ay) * (dy / ay);
        if (k > 1.0f) {
            const float s = 1.0f / std::sqrt(k);
            dx *= s;
            dy *= s;
        }
        out_x = static_cast<int>(std::lround(cx + dx));
        out_y = static_cast<int>(std::lround(cy + dy));
        out_x = std::clamp(out_x, eye_x + 2, std::max(eye_x + 2, eye_x + eye_w - pw - 2));
        out_y = std::clamp(out_y, eye_y + 1, std::max(eye_y + 1, eye_y + eye_h - ph - 2));
    };

    if (show_left_pupil) {
        lv_obj_remove_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
        place_pupil(left_x, left_y, current_left_width_, current_left_height_, left_pupil_w,
                    left_pupil_h, left_pupil_pos_x, left_pupil_pos_y);
        lv_obj_set_pos(left_pupil_, left_pupil_pos_x, left_pupil_pos_y);
        lv_obj_set_size(left_pupil_, left_pupil_w, left_pupil_h);
        lv_obj_set_style_radius(left_pupil_, PupilRadius(left_pupil_w, left_pupil_h), 0);
    } else {
        lv_obj_add_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_right_pupil) {
        lv_obj_remove_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
        place_pupil(right_x, right_y, current_right_width_, current_right_height_, right_pupil_w,
                    right_pupil_h, right_pupil_pos_x, right_pupil_pos_y);
        lv_obj_set_pos(right_pupil_, right_pupil_pos_x, right_pupil_pos_y);
        lv_obj_set_size(right_pupil_, right_pupil_w, right_pupil_h);
        lv_obj_set_style_radius(right_pupil_, PupilRadius(right_pupil_w, right_pupil_h), 0);
    } else {
        lv_obj_add_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    // 9. Crescent Masks (Curved sleeping / Zen / Angel / PlayfulWink)
    lv_obj_add_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);

    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kZenMeditation) {
        const int breath_offset = ((animation_tick_ / 25) % 2 == 0) ? -4 : -3;
        lv_obj_remove_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_crescent_mask_, left_x, left_y + breath_offset);
        lv_obj_set_size(left_crescent_mask_, current_left_width_, current_left_height_);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + breath_offset);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    } else if (face_mode_ == FaceMode::kAngelBlessing) {
        lv_obj_remove_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_crescent_mask_, left_x, left_y + 4);
        lv_obj_set_size(left_crescent_mask_, current_left_width_, current_left_height_);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + 4);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + 4);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    }

    // 10. Dual Teardrops for crying
    lv_obj_add_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
    if (face_mode_ == FaceMode::kSadCrying && !blink) {
        lv_obj_remove_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
        int tear_slide_y = (animation_tick_ % 12);
        lv_obj_set_pos(left_tear_, left_x + 14, left_y + current_left_height_ + tear_slide_y - 2);
        lv_obj_set_size(left_tear_, 6, 8);
        lv_obj_set_pos(right_tear_, right_x + 14, right_y + current_right_height_ + tear_slide_y - 2);
        lv_obj_set_size(right_tear_, 6, 8);
    }

    // 11. Blushing marks
    lv_obj_add_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
    if ((face_mode_ == FaceMode::kShy || face_mode_ == FaceMode::kLove ||
         face_mode_ == FaceMode::kPout || face_mode_ == FaceMode::kDrunk ||
         face_mode_ == FaceMode::kInnocent) && !blink) {
        lv_obj_remove_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        int blush_y = (face_mode_ == FaceMode::kLove) ? (base_y + eye_height - 4) : (left_y + current_left_height_ - 4);
        lv_obj_set_pos(left_blush_, left_x + 6, blush_y);
        lv_obj_set_pos(right_blush_, right_x + 6, blush_y);
    }

    // Hide special expression objects by default
    for (auto* lbl : {fx_label_a_, fx_label_b_}) {
        if (lbl) {
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        }
    }
    for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
        if (h) lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
        if (d) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto* s : {left_sparkle_a_, left_sparkle_b_, right_sparkle_a_, right_sparkle_b_}) {
        if (s) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto& eye_glints : catchlight_) {
        for (auto* glint : eye_glints) {
            if (glint) lv_obj_add_flag(glint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // =========================================================================
    // ASYMMETRIC ROUND PIXEL-MATRIX CATCHLIGHTS (Zero-Diamond!)
    // - Left Eye:  Small crisp dot (2..3px solid), hugged at MIDDLE-RIGHT edge of pupil.
    // - Right Eye: 2x Larger round disc (6..7px with 1px corner chamfer -> 4..5px flat top/bottom),
    //              hugged at TOP-RIGHT corner of pupil.
    // =========================================================================
    const bool is_special_sparkle_mode = (face_mode_ == FaceMode::kExcited ||
                                          face_mode_ == FaceMode::kMoneyEyes ||
                                          face_mode_ == FaceMode::kTargetLocked);
    if (!is_special_sparkle_mode) {
        const int total_look_x = current_look_x_ + micro_look_x_;
        const int total_look_y = current_look_y_ + micro_look_y_;
        const int gaze_shift_x = std::clamp(-total_look_x / 4, -1, 1);
        const int gaze_shift_y = std::clamp(-total_look_y / 4, -1, 1);

        const bool is_happy_mode = (face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kPlayfulWink ||
                                    face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPartyCelebrate ||
                                    face_mode_ == FaceMode::kInnocent || face_mode_ == FaceMode::kPout ||
                                    face_mode_ == FaceMode::kSunnyDazzle || face_mode_ == FaceMode::kIdeaEureka);
        const bool is_angry_mode = (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kFurious ||
                                    face_mode_ == FaceMode::kScheming || face_mode_ == FaceMode::kSideEye ||
                                    face_mode_ == FaceMode::kJealous);
        const bool is_gloomy_mode = (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying ||
                                     face_mode_ == FaceMode::kCryingStream || face_mode_ == FaceMode::kSleepy ||
                                     face_mode_ == FaceMode::kExhausted || face_mode_ == FaceMode::kRainyGloomy);

        if (face_mode_ == FaceMode::kFireFury) {
            // Super Saiyan glowing round white core in both pupils
            if (show_left_pupil) {
                PlaceDisc(catchlight_[0][0], catchlight_[0][1], catchlight_[0][2],
                          left_pupil_pos_x + (left_pupil_w - 6) / 2,
                          left_pupil_pos_y + (left_pupil_h - 6) / 2, 6);
            }
            if (show_right_pupil) {
                PlaceDisc(catchlight_[1][0], catchlight_[1][1], catchlight_[1][2],
                          right_pupil_pos_x + (right_pupil_w - 6) / 2,
                          right_pupil_pos_y + (right_pupil_h - 6) / 2, 6);
            }
        } else {
            // 1. LEFT EYE: Small dot (2px normal, 3px happy), hugged tight against MIDDLE-RIGHT edge of pupil
            if (show_left_pupil && left_pupil_w >= 6 && catchlight_[0][0] != nullptr) {
                int left_diam = (is_happy_mode && left_pupil_w >= 15) ? 3 : 2;

                int left_dot_x = left_pupil_pos_x + left_pupil_w - left_diam - 1 + gaze_shift_x;
                int left_dot_y = left_pupil_pos_y + (left_pupil_h - left_diam) / 2 + gaze_shift_y;

                left_dot_x = std::clamp(left_dot_x, left_pupil_pos_x + 1, left_pupil_pos_x + left_pupil_w - left_diam - 1);
                left_dot_y = std::clamp(left_dot_y, left_pupil_pos_y + 1, left_pupil_pos_y + left_pupil_h - left_diam - 1);

                PlaceDisc(catchlight_[0][0], catchlight_[0][1], catchlight_[0][2], left_dot_x, left_dot_y, left_diam);
            }

            // 2. RIGHT EYE: Giant 2x round disc (6px normal, 7px happy, 5px angry/small), hugged at TOP-RIGHT corner
            if (show_right_pupil && right_pupil_w >= 7 && catchlight_[1][0] != nullptr) {
                int right_diam = 6;  // 6x6 with 1px corner chamfer -> plump round circle!
                if (is_happy_mode && right_pupil_w >= 15) {
                    right_diam = 7;
                } else if (right_pupil_w <= 10 || is_angry_mode || is_gloomy_mode) {
                    right_diam = 5;  // 5x5 with 1px corner chamfer -> 3px flat edges, never a diamond!
                }

                int right_dot_x = right_pupil_pos_x + right_pupil_w - right_diam - 1 + gaze_shift_x;
                int right_dot_y = right_pupil_pos_y + 1 + gaze_shift_y;

                right_dot_x = std::clamp(right_dot_x, right_pupil_pos_x + 1, right_pupil_pos_x + right_pupil_w - right_diam - 1);
                right_dot_y = std::clamp(right_dot_y, right_pupil_pos_y + 1, right_pupil_pos_y + right_pupil_h - right_diam - 1);

                PlaceDisc(catchlight_[1][0], catchlight_[1][1], catchlight_[1][2], right_dot_x, right_dot_y, right_diam);

                // Secondary tiny lower-left glint in right pupil
                if (right_pupil_w >= 12 && (is_happy_mode || face_mode_ == FaceMode::kNeutral) && catchlight_[1][3] != nullptr) {
                    lv_obj_remove_flag(catchlight_[1][3], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(catchlight_[1][3], right_pupil_pos_x + 2, right_pupil_pos_y + right_pupil_h - 4);
                    lv_obj_set_size(catchlight_[1][3], 2, 2);
                }
            }
        }
    }

    if (sweat_drop_) lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
    if (laser_line_) lv_obj_add_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; ++i) {
        if (smoke_puff_[i]) {
            lv_obj_add_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_radius(smoke_puff_[i], 0, 0);
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (music_note_[i]) lv_obj_add_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 6; ++i) {
        if (extra_shape_[i]) {
            lv_obj_add_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(extra_shape_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(extra_shape_[i], 0, 0);
            lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[i], 0, 0);
        }
    }

    // 12. Mode-Specific Distinct Visual Elements (Zero Clones, Zero Diamonds!)
    if (face_mode_ == FaceMode::kNeutral && idle_quirk_ == IdleQuirk::kBugTracking && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(extra_shape_[0], 3, 3);
            lv_obj_set_pos(extra_shape_[0], bug_x, bug_y);
        }
    } else if (face_mode_ == FaceMode::kSleeping) {
        uint32_t sleep_ticks = (animation_tick_ > last_active_tick_ + kAutoSleepTimeoutTicks)
                                   ? (animation_tick_ - (last_active_tick_ + kAutoSleepTimeoutTicks))
                                   : 0;
        if (sleep_ticks > 60 || face_mode_ == FaceMode::kSleeping) {
            // Bong bóng 1 (5px tròn bầu vát 1px góc - không bao giờ bị hình thoi!):
            int b1_prog = (animation_tick_ / 3) % 15;
            int b1_x = 58 + b1_prog / 2;
            int b1_y = 16 - b1_prog;
            PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, b1_x, b1_y, 5);

            // Bong bóng 2 (7px tròn căng vát 1px góc):
            int b2_prog = (animation_tick_ / 3 + 7) % 15;
            int b2_x = 66 + b2_prog / 2;
            int b2_y = 16 - b2_prog;
            PlaceDisc(smoke_puff_[2], smoke_puff_[3], nullptr, b2_x, b2_y, 7);
        }
    } else if (face_mode_ == FaceMode::kListening && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int wave = ((animation_tick_ + i * 2) % 4) * 2;
                int bar_h = 2 + (mic_level * (i % 2 == 0 ? 14 : 10)) / 100 + (mic_level > 8 ? wave : 0);
                bar_h = std::min(18, bar_h);
                lv_obj_set_size(extra_shape_[i], 2, bar_h);
                lv_obj_set_pos(extra_shape_[i], 59 + i * 3, 44 - bar_h);
            }
        }
    } else if (face_mode_ == FaceMode::kThinking && !blink) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int bounce = (((animation_tick_ / 3) + i) % 3 == 0) ? -3 : 0;
                lv_obj_set_size(extra_shape_[i], 3, 3);
                lv_obj_set_pos(extra_shape_[i], 57 + i * 6, 4 + bounce);
            }
        }
    } else if ((face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) && !blink) {
        int pulse = (face_mode_ == FaceMode::kLove) ? ((animation_tick_ % 14 < 7) ? 2 : -1) : 0;
        int hw = 12 + pulse;
        int hh = 22 + pulse * 2;
        int l_cx = left_x + eye_width / 2;
        int l_cy = base_y + eye_height / 2;
        int r_cx = right_x + eye_width / 2;
        int r_cy = base_y + eye_height / 2;

        for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
            if (h) {
                lv_obj_remove_flag(h, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(h, hw, hh);
                lv_obj_set_style_radius(h, 6, 0);
                lv_obj_set_style_transform_pivot_x(h, hw / 2, 0);
                lv_obj_set_style_transform_pivot_y(h, hh / 2, 0);
            }
        }
        int split = (face_mode_ == FaceMode::kHeartbroken) ? 2 : 0;
        if (left_heart_a_) {
            lv_obj_set_pos(left_heart_a_, l_cx - hw / 2 - split, l_cy - hh / 2);
            lv_obj_set_style_transform_rotation(left_heart_a_, NormalizeAngle(450), 0);
        }
        if (left_heart_b_) {
            lv_obj_set_pos(left_heart_b_, l_cx - hw / 2 + split, l_cy - hh / 2);
            lv_obj_set_style_transform_rotation(left_heart_b_, NormalizeAngle(-450), 0);
        }
        if (right_heart_a_) {
            lv_obj_set_pos(right_heart_a_, r_cx - hw / 2 - split, r_cy - hh / 2);
            lv_obj_set_style_transform_rotation(right_heart_a_, NormalizeAngle(450), 0);
        }
        if (right_heart_b_) {
            lv_obj_set_pos(right_heart_b_, r_cx - hw / 2 + split, r_cy - hh / 2);
            lv_obj_set_style_transform_rotation(right_heart_b_, NormalizeAngle(-450), 0);
        }
        if (face_mode_ == FaceMode::kHeartbroken) {
            for (int i = 0; i < 2; ++i) {
                if (extra_shape_[i]) {
                    lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_bg_color(extra_shape_[i], lv_color_black(), 0);
                    int cx = (i == 0) ? l_cx : r_cx;
                    lv_obj_set_size(extra_shape_[i], 4, 28);
                    lv_obj_set_pos(extra_shape_[i], cx - 2, l_cy - 14);
                    lv_obj_set_style_transform_pivot_x(extra_shape_[i], 2, 0);
                    lv_obj_set_style_transform_pivot_y(extra_shape_[i], 14, 0);
                    lv_obj_set_style_transform_rotation(extra_shape_[i], NormalizeAngle(120), 0);
                }
            }
        }
    } else if ((face_mode_ == FaceMode::kDizzy || face_mode_ == FaceMode::kHypnotized) && !blink) {
        int dw = 18;
        int dh = 4;
        int speed = (face_mode_ == FaceMode::kHypnotized) ? 180 : 120;
        int dizzy_angle = (animation_tick_ * speed) % 3600;
        int l_cx = left_x + current_left_width_ / 2;
        int l_cy = left_y + current_left_height_ / 2;
        int r_cx = right_x + current_right_width_ / 2;
        int r_cy = right_y + current_right_height_ / 2;

        for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
            if (d) {
                lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(d, dw, dh);
                lv_obj_set_style_radius(d, 2, 0);
                lv_obj_set_style_transform_pivot_x(d, dw / 2, 0);
                lv_obj_set_style_transform_pivot_y(d, dh / 2, 0);
            }
        }
        if (left_dizzy_a_) {
            lv_obj_set_pos(left_dizzy_a_, l_cx - dw / 2, l_cy - dh / 2);
            lv_obj_set_style_transform_rotation(left_dizzy_a_, NormalizeAngle(450 + dizzy_angle), 0);
        }
        if (left_dizzy_b_) {
            lv_obj_set_pos(left_dizzy_b_, l_cx - dw / 2, l_cy - dh / 2);
            lv_obj_set_style_transform_rotation(left_dizzy_b_, NormalizeAngle(1350 + dizzy_angle), 0);
        }
        if (right_dizzy_a_) {
            lv_obj_set_pos(right_dizzy_a_, r_cx - dw / 2, r_cy - dh / 2);
            lv_obj_set_style_transform_rotation(right_dizzy_a_, NormalizeAngle(450 + dizzy_angle), 0);
        }
        if (right_dizzy_b_) {
            lv_obj_set_pos(right_dizzy_b_, r_cx - dw / 2, r_cy - dh / 2);
            lv_obj_set_style_transform_rotation(right_dizzy_b_, NormalizeAngle(1350 + dizzy_angle), 0);
        }
        if (face_mode_ == FaceMode::kHypnotized) {
            for (int i = 0; i < 2; ++i) {
                if (extra_shape_[i]) {
                    lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_bg_opa(extra_shape_[i], LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(extra_shape_[i], 2, 0);
                    lv_obj_set_style_border_color(extra_shape_[i], lv_color_black(), 0);
                    int pulse_r = ((animation_tick_ / 3) % 2 == 0) ? 24 : 16;
                    int cx = (i == 0) ? l_cx : r_cx;
                    int cy = (i == 0) ? l_cy : r_cy;
                    lv_obj_set_size(extra_shape_[i], pulse_r, pulse_r);
                    lv_obj_set_style_radius(extra_shape_[i], 5, 0);
                    lv_obj_set_pos(extra_shape_[i], cx - pulse_r / 2, cy - pulse_r / 2);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kLoadingBuffer && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 1, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 38, 1);
            lv_obj_set_size(extra_shape_[0], 52, 7);
            lv_obj_set_style_radius(extra_shape_[0], 2, 0);

            int fill_w = 6 + ((animation_tick_ * 3) % 42);
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 40, 3);
            lv_obj_set_size(extra_shape_[1], fill_w, 3);
            lv_obj_set_style_radius(extra_shape_[1], 0, 0);
        }
    } else if (face_mode_ == FaceMode::kGameOver && !blink) {
        int dw = 18, dh = 4;
        int l_cx = left_x + current_left_width_ / 2;
        int l_cy = left_y + current_left_height_ / 2;
        int r_cx = right_x + current_right_width_ / 2;
        int r_cy = right_y + current_right_height_ / 2;
        for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
            if (d) {
                lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(d, dw, dh);
                lv_obj_set_style_radius(d, 2, 0);
                lv_obj_set_style_transform_pivot_x(d, dw / 2, 0);
                lv_obj_set_style_transform_pivot_y(d, dh / 2, 0);
            }
        }
        if (left_dizzy_a_) {
            lv_obj_set_pos(left_dizzy_a_, l_cx - dw / 2, l_cy - dh / 2);
            lv_obj_set_style_transform_rotation(left_dizzy_a_, NormalizeAngle(450), 0);
        }
        if (left_dizzy_b_) {
            lv_obj_set_pos(left_dizzy_b_, l_cx - dw / 2, l_cy - dh / 2);
            lv_obj_set_style_transform_rotation(left_dizzy_b_, NormalizeAngle(-450), 0);
        }
        if (right_dizzy_a_) {
            lv_obj_set_pos(right_dizzy_a_, r_cx - dw / 2, r_cy - dh / 2);
            lv_obj_set_style_transform_rotation(right_dizzy_a_, NormalizeAngle(450), 0);
        }
        if (right_dizzy_b_) {
            lv_obj_set_pos(right_dizzy_b_, r_cx - dw / 2, r_cy - dh / 2);
            lv_obj_set_style_transform_rotation(right_dizzy_b_, NormalizeAngle(-450), 0);
        }
    } else if ((face_mode_ == FaceMode::kScared || face_mode_ == FaceMode::kExhausted) && !blink) {
        if (sweat_drop_) {
            lv_obj_remove_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
            int sweat_x = right_x + current_right_width_ + 3;
            int sweat_y = 6 + ((animation_tick_ * 2) % 24);
            lv_obj_set_pos(sweat_drop_, sweat_x, sweat_y);
            lv_obj_set_size(sweat_drop_, 5, 8);
        }
    } else if ((face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kOverheat) && !blink) {
        // Use pairs of smoke_puff_ via PlaceDisc so steam puffs are round instead of squares!
        for (int i = 0; i < 3; ++i) {
            int phase = (animation_tick_ + i * 5) % 16;
            int smoke_y = left_y - 3 - phase;
            if (smoke_y >= 0) {
                int smoke_x = (i == 0) ? (left_x + 12) : (i == 1) ? 61 : (right_x + 18);
                int s_diam = (phase > 8) ? 6 : 5;
                PlaceDisc(smoke_puff_[i * 2], smoke_puff_[i * 2 + 1], nullptr, smoke_x, smoke_y, s_diam);
            }
        }
        if (face_mode_ == FaceMode::kOverheat && fx_label_a_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_WARNING);
            lv_obj_set_pos(fx_label_a_, 57, 1);
        }
    } else if (face_mode_ == FaceMode::kFireFury && !blink) {
        for (int i = 0; i < 6; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int flame_h = 8 + ((animation_tick_ * 3 + i * 5) % 14);
                int fx_pos = (i < 3) ? (left_x - 4 + i * 18) : (right_x - 2 + (i - 3) * 18);
                int fy_pos = std::max(0, left_y + 6 - flame_h);
                lv_obj_set_size(extra_shape_[i], 4, flame_h);
                lv_obj_set_pos(extra_shape_[i], fx_pos, fy_pos);
                lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kPartyCelebrate && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                int cy = ((animation_tick_ * 2 + i * 10) % 46);
                int cx = 10 + i * 32;
                lv_obj_set_pos(smoke_puff_[i], cx, cy);
                lv_obj_set_size(smoke_puff_[i], 3, 3);
            }
        }
    } else if (face_mode_ == FaceMode::kMoneyEyes && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, "$");
            lv_label_set_text(fx_label_b_, "$");
            int bounce = ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
            lv_obj_set_pos(fx_label_a_, left_pupil_pos_x + 4, left_pupil_pos_y + 2 + bounce);
            lv_obj_set_pos(fx_label_b_, right_pupil_pos_x + 4, right_pupil_pos_y + 2 + bounce);
        }
    } else if (face_mode_ == FaceMode::kExcited && !blink) {
        int star_len = ((animation_tick_ / 4) % 2 == 0) ? 14 : 10;
        int star_thick = 2;
        int l_pcx = left_pupil_pos_x + left_pupil_w / 2;
        int l_pcy = left_pupil_pos_y + left_pupil_h / 2;
        int r_pcx = right_pupil_pos_x + right_pupil_w / 2;
        int r_pcy = right_pupil_pos_y + right_pupil_h / 2;

        if (left_sparkle_a_ && left_sparkle_b_ && right_sparkle_a_ && right_sparkle_b_) {
            lv_obj_remove_flag(left_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(left_sparkle_b_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_b_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_sparkle_a_, l_pcx - star_thick / 2, l_pcy - star_len / 2);
            lv_obj_set_size(left_sparkle_a_, star_thick, star_len);
            lv_obj_set_pos(left_sparkle_b_, l_pcx - star_len / 2, l_pcy - star_thick / 2);
            lv_obj_set_size(left_sparkle_b_, star_len, star_thick);

            lv_obj_set_pos(right_sparkle_a_, r_pcx - star_thick / 2, r_pcy - star_len / 2);
            lv_obj_set_size(right_sparkle_a_, star_thick, star_len);
            lv_obj_set_pos(right_sparkle_b_, r_pcx - star_len / 2, r_pcy - star_thick / 2);
            lv_obj_set_size(right_sparkle_b_, star_len, star_thick);
        }
    } else if (face_mode_ == FaceMode::kSunnyDazzle && !blink) {
        // Round sun core using PlaceDisc (7x7 round) + rays
        PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, 60, 1, 7);
        int ray_ext = ((animation_tick_ / 5) % 2 == 0) ? 2 : 0;
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 53 - ray_ext, 4);
            lv_obj_set_size(extra_shape_[1], 5 + ray_ext, 2);
        }
        if (extra_shape_[2]) {
            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[2], 69, 4);
            lv_obj_set_size(extra_shape_[2], 5 + ray_ext, 2);
        }
        if (extra_shape_[3]) {
            lv_obj_remove_flag(extra_shape_[3], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[3], 56, 10);
            lv_obj_set_size(extra_shape_[3], 2, 3 + ray_ext);
        }
        if (extra_shape_[4]) {
            lv_obj_remove_flag(extra_shape_[4], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[4], 69, 10);
            lv_obj_set_size(extra_shape_[4], 2, 3 + ray_ext);
        }
    } else if (face_mode_ == FaceMode::kMusic && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_AUDIO);
            lv_label_set_text(fx_label_b_, LV_SYMBOL_AUDIO);
            int ny0 = 2 + (((animation_tick_ / 6)) % 2 == 0 ? -2 : 2);
            int ny1 = 2 + (((animation_tick_ / 6) + 1) % 2 == 0 ? -2 : 2);
            lv_obj_set_pos(fx_label_a_, left_x - 12, ny0);
            lv_obj_set_pos(fx_label_b_, right_x + current_right_width_ + 2, ny1);
        }
    } else if (face_mode_ == FaceMode::kConfusedQuestion && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, "?");
            lv_label_set_text(fx_label_b_, "?");
            int ny = 1 + ((animation_tick_ / 6) % 2 == 0 ? -1 : 2);
            lv_obj_set_pos(fx_label_a_, 58, ny);
            lv_obj_set_pos(fx_label_b_, right_x + current_right_width_ + 2, ny + 2);
        }
    } else if ((face_mode_ == FaceMode::kScanning || face_mode_ == FaceMode::kThunderShock) && !blink) {
        if (laser_line_) {
            lv_obj_remove_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
            int scan_offset = ((animation_tick_ * 3) % (current_left_height_ + 6));
            lv_obj_set_pos(laser_line_, base_x - 4, left_y - 2 + scan_offset);
            lv_obj_set_size(laser_line_, pair_width + 8, 2);
        }
        if (face_mode_ == FaceMode::kThunderShock && fx_label_a_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_CHARGE);
            lv_obj_set_pos(fx_label_a_, 60, 2);
        }
    } else if (face_mode_ == FaceMode::kMatrixGlitch && !blink) {
        for (int i = 0; i < 5; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], (i % 2 == 0) ? lv_color_black() : lv_color_white(), 0);
                int gx = 10 + ((animation_tick_ * 17 + i * 23) % 90);
                int gy = 6 + ((animation_tick_ * 7 + i * 9) % 36);
                int gw = 14 + ((animation_tick_ * 5 + i * 11) % 24);
                lv_obj_set_pos(extra_shape_[i], gx, gy);
                lv_obj_set_size(extra_shape_[i], gw, 3);
                lv_obj_set_style_radius(extra_shape_[i], 0, 0);
            }
        }
    }

    // Special Extra Shapes (Polished 1-bit Monochrome Graphics)
    if (face_mode_ == FaceMode::kLowBattery && !blink) {
        if ((animation_tick_ % 14 < 9) && extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 2, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 52, 2);
            lv_obj_set_size(extra_shape_[0], 20, 10);
            lv_obj_set_style_radius(extra_shape_[0], 2, 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 72, 5);
            lv_obj_set_size(extra_shape_[1], 2, 4);

            if (extra_shape_[2]) {
                lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(extra_shape_[2], 55, 5);
                lv_obj_set_size(extra_shape_[2], 4, 4);
            }
        }
    } else if (face_mode_ == FaceMode::kAngelBlessing && !blink) {
        if (extra_shape_[0]) {
            int float_y = ((animation_tick_ / 8) % 2 == 0) ? 0 : 2;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 2, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 40, float_y);
            lv_obj_set_size(extra_shape_[0], 48, 7);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
    } else if (face_mode_ == FaceMode::kInnocent && !blink) {
        for (int i = 0; i < 2; ++i) {
            int sy = ((animation_tick_ / 5 + i) % 2 == 0) ? 2 : 5;
            int sx = (i == 0) ? (left_x - 6) : (right_x + current_right_width_ + 2);
            PlaceDisc(smoke_puff_[i * 2], smoke_puff_[i * 2 + 1], nullptr, sx, sy, 5);
        }
    } else if (face_mode_ == FaceMode::kTargetLocked && !blink) {
        int cx = right_x + current_right_width_ / 2;
        int cy = right_y + current_right_height_ / 2;
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 2, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[0], 24, 24);
            lv_obj_set_style_radius(extra_shape_[0], 6, 0);
            lv_obj_set_pos(extra_shape_[0], cx - 12, cy - 12);
        }
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[1], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[1], 32, 2);
            lv_obj_set_pos(extra_shape_[1], cx - 16, cy - 1);
        }
        if (extra_shape_[2]) {
            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[2], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[2], 2, 32);
            lv_obj_set_pos(extra_shape_[2], cx - 1, cy - 16);
        }
    } else if (face_mode_ == FaceMode::kIdeaEureka && !blink) {
        // Round bulb head using PlaceDisc (7x7 round) + base
        PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, 60, 2, 7);
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 61, 10);
            lv_obj_set_size(extra_shape_[1], 5, 3);
            lv_obj_set_style_radius(extra_shape_[1], 1, 0);
        }
        if ((animation_tick_ / 4) % 2 == 0) {
            for (int i = 2; i < 4; ++i) {
                if (extra_shape_[i]) {
                    lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_size(extra_shape_[i], 4, 2);
                    lv_obj_set_pos(extra_shape_[i], (i == 2) ? 53 : 70, 5);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kWhispering && !blink) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int float_dy = ((animation_tick_ / 5 + i) % 2 == 0) ? -1 : 1;
                lv_obj_set_size(extra_shape_[i], 2, 2);
                lv_obj_set_pos(extra_shape_[i], 57 + i * 5, 16 + float_dy);
            }
        }
    } else if (face_mode_ == FaceMode::kSneezing && (animation_tick_ % 35) >= 22) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int dist = ((animation_tick_ % 35) - 22) * 2;
                lv_obj_set_size(extra_shape_[i], 3, 3);
                lv_obj_set_pos(extra_shape_[i], 52 + i * 10, 32 + dist);
            }
        }
    } else if (face_mode_ == FaceMode::kHeadache && !blink) {
        if (extra_shape_[0] && extra_shape_[1] && extra_shape_[2]) {
            int pulse = ((animation_tick_ / 3) % 2 == 0) ? 2 : 0;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 58, 1);
            lv_obj_set_size(extra_shape_[0], 4, 9 + pulse);
            lv_obj_set_style_transform_rotation(extra_shape_[0], NormalizeAngle(250), 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 63, 1);
            lv_obj_set_size(extra_shape_[1], 4, 9 + pulse);
            lv_obj_set_style_transform_rotation(extra_shape_[1], NormalizeAngle(-250), 0);

            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[2], 56, 6);
            lv_obj_set_size(extra_shape_[2], 16, 3);
        }
    } else if (face_mode_ == FaceMode::kFreezing && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int ix = (i < 2) ? (left_x + 8 + i * 16) : (right_x + 8 + (i - 2) * 16);
                int ih = (i % 2 == 0) ? 9 : 6;
                lv_obj_set_pos(extra_shape_[i], ix, left_y + current_left_height_ - 1);
                lv_obj_set_size(extra_shape_[i], 3, ih);
            }
        }
        if (extra_shape_[4] && extra_shape_[5]) {
            int chatter = (animation_tick_ % 2 == 0) ? 0 : 1;
            lv_obj_remove_flag(extra_shape_[4], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[4], 54, 41 - chatter);
            lv_obj_set_size(extra_shape_[4], 20, 2);

            lv_obj_remove_flag(extra_shape_[5], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[5], 54, 44 + chatter);
            lv_obj_set_size(extra_shape_[5], 20, 2);
        }
    } else if (face_mode_ == FaceMode::kCryingStream && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int sx = (i == 0) ? (left_x + 5) : (i == 1) ? (left_x + 22) : (i == 2) ? (right_x + 5) : (right_x + 22);
                int top_y = left_y + current_left_height_ - 2;
                lv_obj_set_pos(extra_shape_[i], sx, top_y);
                lv_obj_set_size(extra_shape_[i], 7, std::max(2, 48 - top_y));
                lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kShockedJawdrop && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 2, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 54, 38);
            lv_obj_set_size(extra_shape_[0], 20, 10);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
    } else if (face_mode_ == FaceMode::kTongueOut && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            int wiggle = ((animation_tick_ / 4) % 2 == 0) ? -2 : 2;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 48, 36);
            lv_obj_set_size(extra_shape_[0], 32, 2);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 56 + wiggle, 37);
            lv_obj_set_size(extra_shape_[1], 16, 10);
            lv_obj_set_style_radius(extra_shape_[1], 5, 0);
        }
    } else if (face_mode_ == FaceMode::kRainyGloomy && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 44, 2);
            lv_obj_set_size(extra_shape_[0], 40, 7);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                int ry = 11 + ((animation_tick_ * 3 + i * 7) % 32);
                lv_obj_set_pos(smoke_puff_[i], 48 + i * 10, ry);
                lv_obj_set_size(smoke_puff_[i], 2, 5);
            }
        }
    } else if (face_mode_ == FaceMode::kFacepalm && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], left_x + 4, 4);
            lv_obj_set_size(extra_shape_[0], 38, 14);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[0], NormalizeAngle(-120), 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], left_x + 12, 16);
            lv_obj_set_size(extra_shape_[1], 32, 10);
            lv_obj_set_style_radius(extra_shape_[1], 3, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[1], NormalizeAngle(-80), 0);
        }
        if (sweat_drop_) {
            lv_obj_remove_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
            int sy = 6 + ((animation_tick_ * 2) % 20);
            lv_obj_set_pos(sweat_drop_, right_x + current_right_width_ + 3, sy);
            lv_obj_set_size(sweat_drop_, 5, 8);
        }
    }

    // 13. Eyelids (Top & Bottom)
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kZenMeditation &&
        face_mode_ != FaceMode::kAngelBlessing && face_mode_ != FaceMode::kLove &&
        face_mode_ != FaceMode::kHeartbroken && !blink) {
        if (face_mode_ == FaceMode::kAngry) {
            constexpr int lid_w = 52;
            constexpr int lid_h = 28;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 20);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(140), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 20);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-140), 0);

        } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury || face_mode_ == FaceMode::kHeadache) {
            constexpr int lid_w = 56;
            constexpr int lid_h = 30;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 10, left_y - 20);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(180), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 10, right_y - 20);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-180), 0);

            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - 6);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 4, 10);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - 6);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 4, 10);

        } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying ||
                   face_mode_ == FaceMode::kCryingStream || face_mode_ == FaceMode::kRainyGloomy) {
            constexpr int lid_w = 52;
            constexpr int lid_h = 28;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 20);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-140), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 20);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(140), 0);

        } else if (face_mode_ == FaceMode::kSleepy) {
            constexpr int sleepy_cover = 13;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 2, left_y - 2);
            lv_obj_set_size(left_top_lid_, current_left_width_ + 4, sleepy_cover + 2);

            lv_obj_set_pos(right_top_lid_, right_x - 2, right_y - 2);
            lv_obj_set_size(right_top_lid_, current_right_width_ + 4, sleepy_cover + 2);

        } else if (face_mode_ == FaceMode::kBored || face_mode_ == FaceMode::kDisgusted) {
            constexpr int bored_cover = 17;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 2, left_y - 2);
            lv_obj_set_size(left_top_lid_, current_left_width_ + 4, bored_cover + 2);

            lv_obj_set_pos(right_top_lid_, right_x - 2, right_y - 2);
            lv_obj_set_size(right_top_lid_, current_right_width_ + 4, bored_cover + 2);

            if (face_mode_ == FaceMode::kDisgusted) {
                lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - 6);
                lv_obj_set_size(left_bottom_lid_, current_left_width_ + 4, 8);
            }

        } else if (face_mode_ == FaceMode::kSideEye) {
            constexpr int lid_w = 52;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 16);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(100), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 16);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(100), 0);

        } else if (face_mode_ == FaceMode::kJealous) {
            constexpr int lid_w = 52;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 16);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-120), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 16);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(120), 0);

        } else if (face_mode_ == FaceMode::kScheming) {
            constexpr int lid_w = 52;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 15);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(150), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 15);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-150), 0);

        } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kConfusedQuestion) {
            constexpr int lid_w = 46;
            constexpr int lid_h = 20;
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_top_lid_, right_x - 6, right_y - 13);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-60), 0);

        } else if (face_mode_ == FaceMode::kSmug) {
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - 6);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 4, 10);

            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - 14);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 4, 18);
        }

        // Cheerful Laugh / Gentle Smile / TongueOut / SunnyDazzle / Audio-Reactive Speaking Cheek Bump
        if (face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPartyCelebrate ||
            face_mode_ == FaceMode::kTongueOut || face_mode_ == FaceMode::kSunnyDazzle) {
            constexpr int cover_height = 9;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - cover_height + 4);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 4, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - cover_height + 4);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 4, cover_height);
        } else if (face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kMusic ||
                   (is_speaking_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening))) {
            int dynamic_cheek = 12 + (is_speaking_ ? (spk_level / 20) : 2);
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - dynamic_cheek / 2);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 4, dynamic_cheek);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - dynamic_cheek / 2);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 4, dynamic_cheek);
        }
    }
}

void PocketOledDisplay::SetFaceMode(FaceMode mode) {
    face_mode_ = mode;
    if (mode == FaceMode::kSurprised) {
        surprised_start_tick_ = animation_tick_;
    }
    if (mode != FaceMode::kNeutral && mode != FaceMode::kSleeping) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }
    UpdateAnimationFrame();
}

PocketOledDisplay::FaceMode PocketOledDisplay::InferEmotionFromText(const char* text) const {
    if (text == nullptr) {
        return FaceMode::kNeutral;
    }
    if (Contains(text, "haha") || Contains(text, "hihi") || Contains(text, "hehe") ||
        Contains(text, "buồn cười") || Contains(text, " mắc cười")) {
        return FaceMode::kEcstaticLaugh;
    }
    if (Contains(text, "yêu ") || Contains(text, "thả tim") || Contains(text, "đáng yêu") ||
        Contains(text, "cưng quá") || Contains(text, "<3")) {
        return FaceMode::kLove;
    }
    if (Contains(text, "xin lỗi") || Contains(text, "tiếc quá") || Contains(text, "buồn") ||
        Contains(text, "chia buồn")) {
        return FaceMode::kSadEmpathy;
    }
    if (Contains(text, "tuyệt vời") || Contains(text, "xuất sắc") || Contains(text, "wow") ||
        Contains(text, "chúc mừng")) {
        return FaceMode::kExcited;
    }
    if (Contains(text, "bí mật") || Contains(text, "suỵt") || Contains(text, "thì thầm")) {
        return FaceMode::kWhispering;
    }
    if (Contains(text, "bất ngờ") || Contains(text, "ngạc nhiên") || Contains(text, "thật sao")) {
        return FaceMode::kSurprised;
    }
    if (Contains(text, "ý tưởng") || Contains(text, "nghĩ ra rồi") || Contains(text, "mẹo")) {
        return FaceMode::kIdeaEureka;
    }
    return FaceMode::kNeutral;
}

PocketOledDisplay::FaceMode PocketOledDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
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

uint32_t PocketOledDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_ >> 8;
}