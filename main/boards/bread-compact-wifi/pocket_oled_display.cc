#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
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

int32_t NormalizeAngle(int32_t angle) {
    return ((angle % 3600) + 3600) % 3600;
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

    // 1. Base Eyes (White rounded squircles)
    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 8, 0);
    }

    // 2. Black Pupils (Pure deep black, moving smoothly with eyes)
    left_pupil_ = lv_obj_create(face_layer_);
    right_pupil_ = lv_obj_create(face_layer_);
    for (auto* pupil : {left_pupil_, right_pupil_}) {
        PrepareShape(pupil);
        lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pupil, 4, 0);
    }

    // 3. Crescent Masks (For TRUE curved sleeping eyes and laughing eyes)
    left_crescent_mask_ = lv_obj_create(face_layer_);
    right_crescent_mask_ = lv_obj_create(face_layer_);
    for (auto* mask : {left_crescent_mask_, right_crescent_mask_}) {
        PrepareShape(mask);
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mask, 14, 0);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. Eyelids (Angled top lids for angry / sad, lower lids for squinting)
    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);
    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 4, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // 5. Dual Teardrops for crying (both eyes)
    left_tear_ = lv_obj_create(face_layer_);
    right_tear_ = lv_obj_create(face_layer_);
    for (auto* tear : {left_tear_, right_tear_}) {
        PrepareShape(tear);
        lv_obj_set_style_bg_color(tear, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(tear, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tear, 3, 0);
        lv_obj_add_flag(tear, LV_OBJ_FLAG_HIDDEN);
    }

    // 6. Blushing marks
    left_blush_ = lv_label_create(face_layer_);
    right_blush_ = lv_label_create(face_layer_);
    for (auto* blush : {left_blush_, right_blush_}) {
        lv_label_set_text(blush, "///");
        lv_obj_set_style_text_color(blush, lv_color_white(), 0);
        lv_obj_set_style_text_font(blush, &lv_font_montserrat_14, 0);
        lv_obj_add_flag(blush, LV_OBJ_FLAG_HIDDEN);
    }

    // 7. Special Expression Elements: Hearts, Dizzy X, Sparkles, Laser, Smoke, Sweat
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
        lv_obj_set_style_radius(s, 2, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
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

    for (int i = 0; i < 4; ++i) {
        smoke_puff_[i] = lv_obj_create(face_layer_);
        PrepareShape(smoke_puff_[i]);
        lv_obj_set_style_bg_color(smoke_puff_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(smoke_puff_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(smoke_puff_[i], 1, 0);
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

    // 8. Chat / Status Line with Circular Marquee Scrolling
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
    last_active_tick_ = animation_tick_;
    temporary_emotion_until_tick_ = 0;
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

    has_chat_text_ = content != nullptr && content[0] != '\0';
    if (chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
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
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (face_mode_ == FaceMode::kSleeping) {
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
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (face_mode_ == FaceMode::kSleeping) {
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

    // 1. Temporary Emotion Timeout (returns to idle after test command or short emotion)
    if (temporary_emotion_until_tick_ > 0 && animation_tick_ >= temporary_emotion_until_tick_ && !is_speaking_) {
        temporary_emotion_until_tick_ = 0;
        emotion_mode_ = FaceMode::kNeutral;
        SetFaceMode(FaceMode::kNeutral);
    }

    // 2. Autonomous Sleep: Sleepy after 35s, Sleeping after 50s (NO random quirk cycling!)
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp && temporary_emotion_until_tick_ == 0) {
        if (animation_tick_ >= last_active_tick_ + kAutoSleepTimeoutTicks) {
            SetFaceMode(FaceMode::kSleeping);
            is_speaking_ = false;
            if (chat_line_ != nullptr && !has_chat_text_) {
                lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_label_set_text(chat_line_, "zZz...");
            }
        } else if (animation_tick_ >= last_active_tick_ + kSleepyTimeoutTicks && face_mode_ == FaceMode::kNeutral) {
            SetFaceMode(FaceMode::kSleepy);
        }
    }

    // 3. Waking Up Transition
    if (face_mode_ == FaceMode::kWakingUp) {
        if (animation_tick_ >= wake_up_until_tick_) {
            SetFaceMode(FaceMode::kNeutral);
        }
    }

    // 4. Natural Blinking (Smooth squash)
    bool blink = false;
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink && face_mode_ != FaceMode::kEcstaticLaugh) {
        if (animation_tick_ >= next_blink_tick_) {
            blink_until_tick_ = animation_tick_ + 3;  // 120ms blink
            next_blink_tick_ = animation_tick_ + 70 + (NextRandom() % 100);
        }
        blink = animation_tick_ < blink_until_tick_;
    }

    // 5. Look Around (Autonomous curious eye shifts)
    if (animation_tick_ >= next_idle_tick_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kSleepy) && !is_speaking_) {
        target_look_x_ = static_cast<int>(NextRandom() % (max_look_x * 2 + 1)) - max_look_x;
        target_look_y_ = static_cast<int>(NextRandom() % (max_look_y * 2 + 1)) - max_look_y;
        next_idle_tick_ = animation_tick_ + 50 + (NextRandom() % 80);
    }

    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kDizzy ||
        face_mode_ == FaceMode::kScared || face_mode_ == FaceMode::kLoadingBuffer || face_mode_ == FaceMode::kRebooting ||
        face_mode_ == FaceMode::kZenMeditation || face_mode_ == FaceMode::kGameOver || face_mode_ == FaceMode::kHypnotized) {
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
    } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        target_look_x_ = 0;
        target_look_y_ = 3;  // Eyes looking down with sadness
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kFurious) {
        target_look_x_ = 0;
        target_look_y_ = 2;  // Eyes lowered slightly for focused angry furrow
    } else if (face_mode_ == FaceMode::kSmug) {
        target_look_x_ = 5;  // Looking up-right with pride
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kSideEye) {
        target_look_x_ = 7;  // Extreme bombastic side-eye
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kScheming) {
        target_look_x_ = ((animation_tick_ / 15) % 2 == 0) ? -4 : 4;  // Sly shifty eyes
        target_look_y_ = 1;
    } else if (face_mode_ == FaceMode::kWhispering) {
        target_look_x_ = -5;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kParanoid) {
        target_look_x_ = (animation_tick_ % 8 < 4) ? -6 : 6;
        target_look_y_ = (animation_tick_ % 6 < 3) ? -3 : 3;
    } else if (face_mode_ == FaceMode::kConfusedQuestion) {
        target_look_x_ = 3;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kIdeaEureka) {
        target_look_x_ = 0;
        target_look_y_ = -3;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_look_x_ = 0;
        target_look_y_ = 4;
    } else if (face_mode_ == FaceMode::kDrunk) {
        target_look_x_ = ((animation_tick_ / 20) % 2 == 0) ? -3 : 3;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kBored) {
        int bored_phase = (animation_tick_ / 25) % 3;
        if (bored_phase == 0) {
            target_look_x_ = -6; target_look_y_ = 2;
        } else if (bored_phase == 1) {
            target_look_x_ = 6; target_look_y_ = 2;
        } else {
            target_look_x_ = 0; target_look_y_ = -4;  // Eye roll upwards
        }
    } else if (face_mode_ == FaceMode::kScanning) {
        target_look_x_ = ((animation_tick_ / 6) % 2 == 0) ? -6 : 6;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kSkeptical) {
        target_look_x_ = ((animation_tick_ / 25) % 2 == 0) ? -2 : 3;
        target_look_y_ = -1;
    } else if (face_mode_ == FaceMode::kPout) {
        target_look_x_ = 0;
        target_look_y_ = 2;  // Looking slightly up pleadingly
    } else if (face_mode_ == FaceMode::kMusic) {
        target_look_x_ = 0;
        target_look_y_ = 1;
    } else if (is_speaking_ || face_mode_ == FaceMode::kWakingUp || face_mode_ == FaceMode::kSmileGentle ||
               face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPlayfulWink ||
               face_mode_ == FaceMode::kExcited) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    // 6. Multi-tier Eye Heights
    int target_left_height = eye_height;
    int target_right_height = eye_height;
    int target_left_y_offset = 0;
    int target_right_y_offset = 0;

    if (face_mode_ == FaceMode::kSleeping) {
        target_left_height = 22;
        target_right_height = 22;
        target_left_y_offset = 6;
        target_right_y_offset = 6;
    } else if (blink) {
        target_left_height = 2;
        target_right_height = 2;
    } else if (face_mode_ == FaceMode::kLove) {
        target_left_height = 0;
        target_right_height = 0;
    } else if (face_mode_ == FaceMode::kLoadingBuffer || face_mode_ == FaceMode::kRebooting) {
        target_left_height = target_right_height = 4;
    } else if (face_mode_ == FaceMode::kLowBattery) {
        target_left_height = target_right_height = 14;
        target_left_y_offset = target_right_y_offset = 8;
    } else if (face_mode_ == FaceMode::kFacepalm) {
        target_left_height = 0;
        target_right_height = 20;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kInnocent || face_mode_ == FaceMode::kShockedJawdrop) {
        target_left_height = target_right_height = 42;
        target_left_y_offset = target_right_y_offset = -2;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_left_height = target_right_height = 10;
        target_left_y_offset = target_right_y_offset = 12;
    } else if (face_mode_ == FaceMode::kStealthNinja) {
        target_left_height = target_right_height = 8;
    } else if (face_mode_ == FaceMode::kZenMeditation) {
        target_left_height = target_right_height = 18;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        int glitch_squash = (animation_tick_ % 4) * 3;
        target_left_height = eye_height - glitch_squash;
        target_right_height = eye_height - (9 - glitch_squash);
    } else if (face_mode_ == FaceMode::kSleepy || face_mode_ == FaceMode::kBored) {
        target_left_height = eye_height;
        target_right_height = eye_height;
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        target_left_height = eye_height;
        target_right_height = 22;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kConfusedQuestion || face_mode_ == FaceMode::kDrunk) {
        target_left_height = eye_height + 4;
        target_left_y_offset = -2;
        target_right_height = 20;
        target_right_y_offset = 5;
    } else if (face_mode_ == FaceMode::kSmug) {
        target_left_height = eye_height;
        target_right_height = eye_height - 2;
        target_right_y_offset = -2;
    } else if (face_mode_ == FaceMode::kSurprised || face_mode_ == FaceMode::kWakingUp) {
        target_left_height = 42;
        target_right_height = 42;
    } else if (face_mode_ == FaceMode::kScanning) {
        target_left_height = 26;
        target_right_height = 26;
    } else if (face_mode_ == FaceMode::kScared) {
        target_left_height = 30;
        target_right_height = 30;
    } else if (face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kPout) {
        target_left_height = eye_height + 2;
        target_right_height = eye_height + 2;
    } else if (face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kMusic) {
        target_left_height = eye_height;
        target_right_height = eye_height;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kCryingStream) {
        target_left_height = eye_height - 4;
        target_right_height = eye_height - 4;
        target_left_y_offset = 2;
        target_right_y_offset = 2;
    } else if (face_mode_ == FaceMode::kShy) {
        target_left_height = eye_height - 4;
        target_right_height = eye_height - 4;
        target_left_y_offset = 3;
        target_left_y_offset = 3;
        target_right_y_offset = 3;
    }

    current_left_height_ = Ease(current_left_height_, target_left_height);
    current_right_height_ = Ease(current_right_height_, target_right_height);
    current_left_y_offset_ = Ease(current_left_y_offset_, target_left_y_offset);
    current_right_y_offset_ = Ease(current_right_y_offset_, target_right_y_offset);

    // 7. Animations: Laugh bounce, Sobbing tremble, Speaking bounce, Rage vibration, Panic shiver, Music groove
    int animation_x = 0;
    int animation_y = 0;
    if (is_speaking_) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kSadCrying) {
        animation_y = (animation_tick_ % 4 < 2) ? -1 : 0;  // Sobbing tremble
    } else if (face_mode_ == FaceMode::kExcited) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -3 : 0;  // Star jump bounce
    } else if (face_mode_ == FaceMode::kMusic) {
        animation_x = ((animation_tick_ / 10) % 2 == 0) ? -3 : 3;  // Groove sway
        animation_y = ((animation_tick_ / 5) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kSmug) {
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -1 : 0;  // Confident subtle nod
    } else if (face_mode_ == FaceMode::kDizzy) {
        animation_x = ((animation_tick_ / 4) % 4 < 2) ? -2 : 2;  // Dizzy wobble
        animation_y = ((animation_tick_ / 6) % 4 < 2) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kPout) {
        animation_y = ((animation_tick_ / 8) % 2 == 0) ? -1 : 0;
    } else if (face_mode_ == FaceMode::kFreezing) {
        animation_x = (animation_tick_ % 2 == 0) ? -1 : 1;
        animation_y = (animation_tick_ % 2 == 0) ? 1 : -1;
    } else if (face_mode_ == FaceMode::kHeadache || face_mode_ == FaceMode::kOverheat) {
        animation_x = (animation_tick_ % 4 < 2) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kThunderShock) {
        animation_x = (animation_tick_ % 2 == 0) ? -3 : 3;
        animation_y = (animation_tick_ % 3 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kPartyCelebrate) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -3 : 1;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        animation_x = (animation_tick_ % 3 == 0) ? -3 : 2;
    } else if (face_mode_ == FaceMode::kZenMeditation) {
        animation_y = ((animation_tick_ / 20) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kDrunk) {
        animation_x = ((animation_tick_ / 15) % 2 == 0) ? -2 : 2;
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kSneezing) {
        int ph = animation_tick_ % 30;
        if (ph > 26) animation_x = 3;
    }

    if (face_mode_ == FaceMode::kThinking) {
        animation_x = ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kSurprised) {
        animation_x = (animation_tick_ % 2 == 0) ? -1 : 1;  // Shock vibration
    } else if (face_mode_ == FaceMode::kFurious) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;  // Furious rage shake
        animation_y = (animation_tick_ % 3 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kScared) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;  // Panic shiver
        animation_y = (animation_tick_ % 2 == 0) ? 1 : -1;
    }

    const int left_x = base_x + current_look_x_ + animation_x;
    const int right_x = left_x + eye_width + eye_gap;
    const int left_y =
        base_y + (eye_height - current_left_height_) / 2 + current_look_y_ + animation_y + current_left_y_offset_;
    const int right_y =
        base_y + (eye_height - current_right_height_) / 2 + current_look_y_ + animation_y + current_right_y_offset_;

    if (face_mode_ == FaceMode::kLove) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_eye_, left_x, left_y);
        lv_obj_set_size(left_eye_, eye_width, current_left_height_);
        lv_obj_set_pos(right_eye_, right_x, right_y);
        lv_obj_set_size(right_eye_, eye_width, current_right_height_);
    }

    // Base eye radius (14 for curved sleep, 8 for normal crisp Huy Vector eyes)
    int left_radius = 8;
    int right_radius = 8;
    if (face_mode_ == FaceMode::kSleeping) {
        left_radius = 14;
        right_radius = 14;
    }
    lv_obj_set_style_radius(left_eye_, left_radius, 0);
    lv_obj_set_style_radius(right_eye_, right_radius, 0);

    // Keep body coordinates aligned without full eye rotation to avoid distortions
    lv_obj_set_style_transform_rotation(left_eye_, 0, 0);
    lv_obj_set_style_transform_rotation(right_eye_, 0, 0);

    // 8. Black Pupils (Deep expressive black pupils moving smoothly with eyes)
    const bool show_left_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kLove &&
                                 face_mode_ != FaceMode::kDizzy && face_mode_ != FaceMode::kLoadingBuffer &&
                                 face_mode_ != FaceMode::kRebooting && face_mode_ != FaceMode::kGameOver &&
                                 face_mode_ != FaceMode::kFacepalm && face_mode_ != FaceMode::kZenMeditation;
    const bool show_right_pupil = !blink && face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kPlayfulWink &&
                                  face_mode_ != FaceMode::kLove && face_mode_ != FaceMode::kDizzy &&
                                  face_mode_ != FaceMode::kLoadingBuffer && face_mode_ != FaceMode::kRebooting &&
                                  face_mode_ != FaceMode::kGameOver && face_mode_ != FaceMode::kTongueOut &&
                                  face_mode_ != FaceMode::kZenMeditation;

    int left_pupil_w = 18;
    int left_pupil_h = 22;
    int right_pupil_w = 18;
    int right_pupil_h = 22;

    // Pupil Shock Dilation & Expression Custom Sizes
    if (face_mode_ == FaceMode::kSurprised) {
        uint32_t elapsed = (animation_tick_ > surprised_start_tick_) ? (animation_tick_ - surprised_start_tick_) : 0;
        if (elapsed < 18) {
            left_pupil_w = right_pupil_w = 8;
            left_pupil_h = right_pupil_h = 10;
        } else {
            left_pupil_w = right_pupil_w = 24;
            left_pupil_h = right_pupil_h = 26;
        }
    } else if (face_mode_ == FaceMode::kFurious) {
        left_pupil_w = right_pupil_w = 12;
        left_pupil_h = right_pupil_h = 14;
    } else if (face_mode_ == FaceMode::kScared) {
        left_pupil_w = right_pupil_w = 8;
        left_pupil_h = right_pupil_h = 8;
    } else if (face_mode_ == FaceMode::kPout) {
        left_pupil_w = right_pupil_w = 26;
        left_pupil_h = right_pupil_h = 28;
    } else if (face_mode_ == FaceMode::kExcited) {
        left_pupil_w = right_pupil_w = 22;
        left_pupil_h = right_pupil_h = 24;
    } else if (face_mode_ == FaceMode::kSkeptical) {
        left_pupil_w = 18;
        left_pupil_h = 22;
        right_pupil_w = 16;
        right_pupil_h = 12;
    } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
        left_pupil_w = right_pupil_w = 18;
        left_pupil_h = right_pupil_h = 20;
    } else if (face_mode_ == FaceMode::kShockedJawdrop) {
        left_pupil_w = right_pupil_w = 8;
        left_pupil_h = right_pupil_h = 8;
    } else if (face_mode_ == FaceMode::kInnocent) {
        left_pupil_w = right_pupil_w = 22;
        left_pupil_h = right_pupil_h = 24;
    } else if (face_mode_ == FaceMode::kParanoid) {
        left_pupil_w = right_pupil_w = 10;
        left_pupil_h = right_pupil_h = 10;
    } else if (face_mode_ == FaceMode::kSleepwalking) {
        left_pupil_w = right_pupil_w = 4;
        left_pupil_h = right_pupil_h = 4;
    }

    int left_pupil_pos_x = 0;
    int left_pupil_pos_y = 0;
    int right_pupil_pos_x = 0;
    int right_pupil_pos_y = 0;

    if (show_left_pupil) {
        lv_obj_remove_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
        left_pupil_pos_x = left_x + (eye_width - left_pupil_w) / 2 + current_look_x_ * 3 / 4;
        left_pupil_pos_y = left_y + (current_left_height_ - left_pupil_h) / 2 + current_look_y_ * 3 / 4;
        lv_obj_set_pos(left_pupil_, left_pupil_pos_x, left_pupil_pos_y);
        lv_obj_set_size(left_pupil_, left_pupil_w, left_pupil_h);
    } else {
        lv_obj_add_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_right_pupil) {
        lv_obj_remove_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
        right_pupil_pos_x = right_x + (eye_width - right_pupil_w) / 2 + current_look_x_ * 3 / 4;
        right_pupil_pos_y = right_y + (current_right_height_ - right_pupil_h) / 2 + current_look_y_ * 3 / 4;
        lv_obj_set_pos(right_pupil_, right_pupil_pos_x, right_pupil_pos_y);
        lv_obj_set_size(right_pupil_, right_pupil_w, right_pupil_h);
    } else {
        lv_obj_add_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
    }

    // 9. Crescent Masks (ONLY for TRUE curved sleeping eyes and playful wink)
    lv_obj_add_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);

    if (face_mode_ == FaceMode::kSleeping) {
        const int breath_offset = ((animation_tick_ / 25) % 2 == 0) ? -4 : -3;
        lv_obj_remove_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_crescent_mask_, left_x, left_y + breath_offset);
        lv_obj_set_size(left_crescent_mask_, eye_width, current_left_height_);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + breath_offset);
        lv_obj_set_size(right_crescent_mask_, eye_width, current_right_height_);
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + 4);
        lv_obj_set_size(right_crescent_mask_, eye_width, current_right_height_);
    }

    // 10. Dual Teardrops for crying (falling smoothly under BOTH eyes)
    lv_obj_add_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
    if (face_mode_ == FaceMode::kSadCrying && !blink) {
        lv_obj_remove_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
        int tear_slide_y = (animation_tick_ % 14);
        lv_obj_set_pos(left_tear_, left_x + 19, left_y + current_left_height_ + tear_slide_y - 2);
        lv_obj_set_size(left_tear_, 6, 8);
        lv_obj_set_pos(right_tear_, right_x + 19, right_y + current_right_height_ + tear_slide_y - 2);
        lv_obj_set_size(right_tear_, 6, 8);
    }

    // 11. Blushing marks when shy, love, or pout
    lv_obj_add_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
    if ((face_mode_ == FaceMode::kShy || face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kPout) && !blink) {
        lv_obj_remove_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        int blush_y = (face_mode_ == FaceMode::kLove) ? (base_y + eye_height - 2) : (left_y + current_left_height_ - 2);
        lv_obj_set_pos(left_blush_, left_x + 8, blush_y);
        lv_obj_set_pos(right_blush_, right_x + 8, blush_y);
    }

    // Hide special expression objects by default
    for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
        if (h) lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
        if (d) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto* s : {left_sparkle_a_, left_sparkle_b_, right_sparkle_a_, right_sparkle_b_}) {
        if (s) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
    if (sweat_drop_) lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
    if (laser_line_) lv_obj_add_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; ++i) {
        if (smoke_puff_[i]) lv_obj_add_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; ++i) {
        if (music_note_[i]) lv_obj_add_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 6; ++i) {
        if (extra_shape_[i]) lv_obj_add_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 12. Mode-Specific Visual Elements
    if (face_mode_ == FaceMode::kLove && !blink) {
        // Dual beating cartoon hearts
        int pulse = (animation_tick_ % 14 < 7) ? 2 : -1;
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
        if (left_heart_a_) {
            lv_obj_set_pos(left_heart_a_, l_cx - hw / 2, l_cy - hh / 2);
            lv_obj_set_style_transform_rotation(left_heart_a_, NormalizeAngle(450), 0);
        }
        if (left_heart_b_) {
            lv_obj_set_pos(left_heart_b_, l_cx - hw / 2, l_cy - hh / 2);
            lv_obj_set_style_transform_rotation(left_heart_b_, NormalizeAngle(-450), 0);
        }
        if (right_heart_a_) {
            lv_obj_set_pos(right_heart_a_, r_cx - hw / 2, r_cy - hh / 2);
            lv_obj_set_style_transform_rotation(right_heart_a_, NormalizeAngle(450), 0);
        }
        if (right_heart_b_) {
            lv_obj_set_pos(right_heart_b_, r_cx - hw / 2, r_cy - hh / 2);
            lv_obj_set_style_transform_rotation(right_heart_b_, NormalizeAngle(-450), 0);
        }
    } else if ((face_mode_ == FaceMode::kDizzy || face_mode_ == FaceMode::kLoadingBuffer || face_mode_ == FaceMode::kHypnotized) && !blink) {
        // Spinning X or spiral in both eyes
        int dw = 20;
        int dh = 4;
        int speed = (face_mode_ == FaceMode::kHypnotized) ? 180 : 120;
        int dizzy_angle = (animation_tick_ * speed) % 3600;
        int l_cx = left_x + eye_width / 2;
        int l_cy = left_y + current_left_height_ / 2;
        int r_cx = right_x + eye_width / 2;
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
    } else if (face_mode_ == FaceMode::kGameOver && !blink) {
        // Fixed X X dead eyes sinking
        int dw = 20, dh = 4;
        int l_cx = left_x + eye_width / 2;
        int l_cy = left_y + current_left_height_ / 2;
        int r_cx = right_x + eye_width / 2;
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
    } else if (face_mode_ == FaceMode::kScared && !blink) {
        // Sweat drop running down the temple
        if (sweat_drop_) {
            lv_obj_remove_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
            int sweat_x = right_x + eye_width + 4;
            int sweat_y = 6 + ((animation_tick_ * 2) % 24);
            lv_obj_set_pos(sweat_drop_, sweat_x, sweat_y);
            lv_obj_set_size(sweat_drop_, 5, 7);
        }
    } else if ((face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kOverheat || face_mode_ == FaceMode::kFireFury) && !blink) {
        // Floating smoke/heat/fire puffs rising
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                int phase = (animation_tick_ + i * 4) % 16;
                int smoke_y = left_y - 2 - phase;
                if (smoke_y >= 0) {
                    lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                    int smoke_x = (i < 2) ? (left_x + 8 + i * 20) : (right_x + 8 + (i - 2) * 20);
                    int s_size = 3 + (phase / 5);
                    lv_obj_set_pos(smoke_puff_[i], smoke_x, smoke_y);
                    lv_obj_set_size(smoke_puff_[i], s_size, s_size);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kPartyCelebrate && !blink) {
        // Confetti falling around
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                int cy = ((animation_tick_ * 2 + i * 10) % 46);
                int cx = 10 + i * 32;
                lv_obj_set_pos(smoke_puff_[i], cx, cy);
                lv_obj_set_size(smoke_puff_[i], 3, 3);
            }
        }
    } else if ((face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kMoneyEyes || face_mode_ == FaceMode::kSunnyDazzle) && !blink) {
        // Star cross sparkles inside dilated pupils or dollar/sun
        int star_len = ((animation_tick_ / 4) % 2 == 0) ? 14 : 10;
        int star_thick = 3;
        int l_pcx = left_pupil_pos_x + left_pupil_w / 2;
        int l_pcy = left_pupil_pos_y + left_pupil_h / 2;
        int r_pcx = right_pupil_pos_x + right_pupil_w / 2;
        int r_pcy = right_pupil_pos_y + right_pupil_h / 2;

        if (left_sparkle_a_ && left_sparkle_b_ && right_sparkle_a_ && right_sparkle_b_) {
            lv_obj_remove_flag(left_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(left_sparkle_b_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_b_, LV_OBJ_FLAG_HIDDEN);

            // Cross: A is vertical, B is horizontal
            lv_obj_set_pos(left_sparkle_a_, l_pcx - star_thick / 2, l_pcy - star_len / 2);
            lv_obj_set_size(left_sparkle_a_, star_thick, star_len);
            lv_obj_set_pos(left_sparkle_b_, l_pcx - star_len / 2, l_pcy - star_thick / 2);
            lv_obj_set_size(left_sparkle_b_, star_len, star_thick);

            lv_obj_set_pos(right_sparkle_a_, r_pcx - star_thick / 2, r_pcy - star_len / 2);
            lv_obj_set_size(right_sparkle_a_, star_thick, star_len);
            lv_obj_set_pos(right_sparkle_b_, r_pcx - star_len / 2, r_pcy - star_thick / 2);
            lv_obj_set_size(right_sparkle_b_, star_len, star_thick);
        }
    } else if (face_mode_ == FaceMode::kPout && !blink) {
        // Two glossy highlights inside huge anime pupils
        if (left_sparkle_a_ && left_sparkle_b_ && right_sparkle_a_ && right_sparkle_b_) {
            lv_obj_remove_flag(left_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(left_sparkle_b_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_sparkle_b_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_sparkle_a_, left_pupil_pos_x + 3, left_pupil_pos_y + 3);
            lv_obj_set_size(left_sparkle_a_, 6, 6);
            lv_obj_set_style_radius(left_sparkle_a_, 3, 0);

            lv_obj_set_pos(left_sparkle_b_, left_pupil_pos_x + left_pupil_w - 7, left_pupil_pos_y + left_pupil_h - 7);
            lv_obj_set_size(left_sparkle_b_, 3, 3);
            lv_obj_set_style_radius(left_sparkle_b_, 1, 0);

            lv_obj_set_pos(right_sparkle_a_, right_pupil_pos_x + 3, right_pupil_pos_y + 3);
            lv_obj_set_size(right_sparkle_a_, 6, 6);
            lv_obj_set_style_radius(right_sparkle_a_, 3, 0);

            lv_obj_set_pos(right_sparkle_b_, right_pupil_pos_x + right_pupil_w - 7, right_pupil_pos_y + right_pupil_h - 7);
            lv_obj_set_size(right_sparkle_b_, 3, 3);
            lv_obj_set_style_radius(right_sparkle_b_, 1, 0);
        }
    } else if ((face_mode_ == FaceMode::kMusic || face_mode_ == FaceMode::kConfusedQuestion) && !blink) {
        // Floating dancing music notes or Question mark
        for (int i = 0; i < 2; ++i) {
            if (music_note_[i]) {
                lv_obj_remove_flag(music_note_[i], LV_OBJ_FLAG_HIDDEN);
                int ny = 4 + (((animation_tick_ / 6) + i * 3) % 2 == 0 ? -2 : 1);
                int nx = (i == 0) ? (left_x + 8) : (right_x + eye_width - 12);
                lv_obj_set_pos(music_note_[i], nx, ny);
                lv_obj_set_size(music_note_[i], 6, 7);
            }
        }
    } else if ((face_mode_ == FaceMode::kScanning || face_mode_ == FaceMode::kThunderShock || face_mode_ == FaceMode::kRebooting) && !blink) {
        // High-tech laser sweep line / thunder line / CRT line
        if (laser_line_) {
            lv_obj_remove_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
            int scan_offset = (face_mode_ == FaceMode::kRebooting) ? 12 : ((animation_tick_ * 3) % (current_left_height_ + 6));
            lv_obj_set_pos(laser_line_, base_x - 4, left_y - 2 + scan_offset);
            lv_obj_set_size(laser_line_, pair_width + 8, 2);
        }
    }

    // Special Extra Shapes for V7.0
    if (face_mode_ == FaceMode::kLowBattery && !blink) {
        if (extra_shape_[0] && (animation_tick_ % 14 < 8)) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 56, 4);
            lv_obj_set_size(extra_shape_[0], 16, 8);
            lv_obj_set_style_radius(extra_shape_[0], 2, 0);
        }
    } else if (face_mode_ == FaceMode::kInnocent && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 48, 4);
            lv_obj_set_size(extra_shape_[0], 32, 6);
            lv_obj_set_style_radius(extra_shape_[0], 3, 0);
        }
    } else if (face_mode_ == FaceMode::kCryingStream && !blink) {
        // Anime waterfall tears
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int sx = (i == 0) ? (left_x + 6) : (i == 1) ? (left_x + 28) : (i == 2) ? (right_x + 6) : (right_x + 28);
                lv_obj_set_pos(extra_shape_[i], sx, left_y + current_left_height_);
                lv_obj_set_size(extra_shape_[i], 6, 48 - (left_y + current_left_height_));
                lv_obj_set_style_radius(extra_shape_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kShockedJawdrop && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 52, left_y + current_left_height_ + 1);
            lv_obj_set_size(extra_shape_[0], 24, 10);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
    } else if (face_mode_ == FaceMode::kTongueOut && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 58, left_y + current_left_height_ - 2);
            lv_obj_set_size(extra_shape_[0], 12, 10);
            lv_obj_set_style_radius(extra_shape_[0], 5, 0);
        }
    } else if (face_mode_ == FaceMode::kRainyGloomy && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 44, 4);
            lv_obj_set_size(extra_shape_[0], 40, 8);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                int ry = 14 + ((animation_tick_ * 2 + i * 7) % 24);
                lv_obj_set_pos(smoke_puff_[i], 48 + i * 9, ry);
                lv_obj_set_size(smoke_puff_[i], 2, 4);
            }
        }
    } else if (face_mode_ == FaceMode::kFacepalm && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], left_x - 2, base_y);
            lv_obj_set_size(extra_shape_[0], eye_width + 4, eye_height);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);
        }
    } else if (face_mode_ == FaceMode::kAngelBlessing && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 48, 4);
            lv_obj_set_size(extra_shape_[0], 32, 6);
            lv_obj_set_style_radius(extra_shape_[0], 3, 0);
        }
    }

    // 13. Eyelids (Top & Bottom)
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kLove && !blink) {
        if (face_mode_ == FaceMode::kAngry) {
            // Huy Vector 14-degree clean furrowed eyebrow cut - perfectly symmetrical
            constexpr int lid_w = 60;
            constexpr int lid_h = 30;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            // Left lid
            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 27);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(140), 0);

            // Right lid
            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 27);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-140), 0);

        } else if (face_mode_ == FaceMode::kFurious) {
            // Furious 18-degree steep angry furrow + lower lid squint
            constexpr int lid_w = 64;
            constexpr int lid_h = 32;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 10, left_y - 26);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(180), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 10, right_y - 26);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-180), 0);

            // Lower squint lids
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - 6);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, 10);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - 6);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, 10);

        } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
            // Droopy sad eyelids slanting down outwards - perfectly symmetrical
            constexpr int lid_w = 60;
            constexpr int lid_h = 30;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            // Left lid
            lv_obj_set_pos(left_top_lid_, left_x - 8, left_y - 27);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-140), 0);

            // Right lid
            lv_obj_set_pos(right_top_lid_, right_x - 8, right_y - 27);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(140), 0);

        } else if (face_mode_ == FaceMode::kSleepy) {
            // Sleepy ptosis: ONLY top lid drops horizontally covering top 1/3 (12px), bottom lid remains hidden!
            constexpr int sleepy_cover = 12;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 2, left_y - 2);
            lv_obj_set_size(left_top_lid_, eye_width + 4, sleepy_cover + 2);
            lv_obj_set_style_transform_rotation(left_top_lid_, 0, 0);

            lv_obj_set_pos(right_top_lid_, right_x - 2, right_y - 2);
            lv_obj_set_size(right_top_lid_, eye_width + 4, sleepy_cover + 2);
            lv_obj_set_style_transform_rotation(right_top_lid_, 0, 0);

        } else if (face_mode_ == FaceMode::kBored) {
            // Bored: top lid drops horizontally covering 50% (18px)
            constexpr int bored_cover = 18;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 2, left_y - 2);
            lv_obj_set_size(left_top_lid_, eye_width + 4, bored_cover + 2);
            lv_obj_set_style_transform_rotation(left_top_lid_, 0, 0);

            lv_obj_set_pos(right_top_lid_, right_x - 2, right_y - 2);
            lv_obj_set_size(right_top_lid_, eye_width + 4, bored_cover + 2);
            lv_obj_set_style_transform_rotation(right_top_lid_, 0, 0);

        } else if (face_mode_ == FaceMode::kSideEye) {
            // Bombastic side-eye slanted eyelids
            constexpr int lid_w = 60;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 6, left_y - 18);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(100), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 6, right_y - 18);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(100), 0);

        } else if (face_mode_ == FaceMode::kJealous) {
            // Jealous slanted triangular eyelids
            constexpr int lid_w = 60;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 6, left_y - 18);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-120), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 6, right_y - 18);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(120), 0);

        } else if (face_mode_ == FaceMode::kScheming) {
            // Scheming sharp squint
            constexpr int lid_w = 60;
            constexpr int lid_h = 24;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 6, left_y - 16);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(150), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 6, right_y - 16);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-150), 0);

        } else if (face_mode_ == FaceMode::kSkeptical) {
            // Skeptical: Right eye squinting top lid
            constexpr int lid_w = 50;
            constexpr int lid_h = 20;
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_top_lid_, right_x - 4, right_y - 14);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-60), 0);

        } else if (face_mode_ == FaceMode::kSmug) {
            // Smug: Right cheek raised high (14px) for asymmetrical smirk
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - 6);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, 10);

            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - 14);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, 18);
        }

        // Cheerful Laugh / Gentle Smile / Music / Speaking: Lower eyelid cheek bump
        if (face_mode_ == FaceMode::kEcstaticLaugh) {
            // Subtle smiling cheek curve at bottom (only 5px)
            constexpr int cover_height = 8;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - cover_height + 4);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - cover_height + 4);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
        } else if (face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kMusic || face_mode_ == FaceMode::kZenMeditation ||
            (is_speaking_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening))) {
            constexpr int cover_height = 14;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 2, left_y + current_left_height_ - cover_height / 2);
            lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 2, right_y + current_right_height_ - cover_height / 2);
            lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
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

PocketOledDisplay::FaceMode PocketOledDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
    }
    // 1. Crying / Sadness
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
    // 2. Laugh / Happiness / Playful
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
    // 3. Anger / Rage
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
    // 4. Cyber & Tech (Group 1)
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
    // 5. Personality & Sass (Group 2)
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
    // 6. Physical & Bodily (Group 3)
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
    // 7. Mental & Spiritual (Group 4)
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
    // 8. Playful & Gaming (Group 5)
    if (Contains(emotion, "money") || Contains(emotion, "dollar") || Contains(emotion, "tien") || Contains(emotion, "giau")) {
        return FaceMode::kMoneyEyes;
    }
    if (Contains(emotion, "stealth") || Contains(emotion, "ninja") || Contains(emotion, "an_nap") || Contains(emotion, "bong_dem")) {
        return FaceMode::kStealthNinja;
    }
    if (Contains(emotion, "game_over") || Contains(emotion, "dead") || Contains(emotion, "die") || Contains(emotion, "chet") || Contains(emotion, "thua")) {
        return FaceMode::kGameOver;
    }
    // 9. Elemental & Powers (Group 6)
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
    // 10. Existing Dynamic Emotions (V6)
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
    return random_state_;
}
