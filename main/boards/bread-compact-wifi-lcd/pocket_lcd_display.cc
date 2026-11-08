#include <vector>
#include "pocket_lcd_display.h"
#include "application.h"
#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "qrcodegen.h"
#include <ctime>
#include <wifi_manager.h>
#include <esp_wifi.h>
#include "board.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "PocketLcdDisplay"

#if LV_FONT_MONTSERRAT_20
LV_FONT_DECLARE(lv_font_montserrat_20);
#endif
#if LV_FONT_MONTSERRAT_32
LV_FONT_DECLARE(lv_font_montserrat_32);
#endif

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
    return std::clamp(static_cast<int>(std::round(std::min(width, height) * 17.0f / 76.0f)), 4, 17);
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

inline int32_t NormalizeAngle(int32_t angle) {
    return ((angle % 3600) + 3600) % 3600;
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

    // 2. Face Layer (240x190, centered on upper canvas)
    face_layer_ = lv_obj_create(scr);
    PrepareShape(face_layer_);
    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 240, 190);
    lv_obj_set_style_bg_color(face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_layer_, LV_OPA_COVER, 0);

    // 3. Base Eyes (W=76, H=72, Radius=16)
    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 17, 0);
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

    // 6. Sparkles & Dizzy X (Drawn inside/over eyes, underneath eyelids)
    left_dizzy_a_ = lv_obj_create(face_layer_);
    left_dizzy_b_ = lv_obj_create(face_layer_);
    right_dizzy_a_ = lv_obj_create(face_layer_);
    right_dizzy_b_ = lv_obj_create(face_layer_);
    for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
        PrepareShape(d);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(d, 4, 0);
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

    // 7. Crescent Masks (For curved sleeping eyes and winking eyes)
    left_crescent_mask_ = lv_obj_create(face_layer_);
    right_crescent_mask_ = lv_obj_create(face_layer_);
    for (auto* mask : {left_crescent_mask_, right_crescent_mask_}) {
        PrepareShape(mask);
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mask, 24, 0);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
    }

    // 8. Eyelids (Angled top lids for angry / sad, lower lids for squinting)
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

    // 9. Dual Teardrops for crying
    left_tear_ = lv_obj_create(face_layer_);
    right_tear_ = lv_obj_create(face_layer_);
    for (auto* tear : {left_tear_, right_tear_}) {
        PrepareShape(tear);
        lv_obj_set_style_bg_color(tear, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(tear, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tear, 6, 0);
        lv_obj_add_flag(tear, LV_OBJ_FLAG_HIDDEN);
    }

    // 10. Blushing marks & FX Labels
    left_blush_ = lv_label_create(face_layer_);
    right_blush_ = lv_label_create(face_layer_);
    for (auto* blush : {left_blush_, right_blush_}) {
        lv_label_set_text(blush, "///");
        lv_obj_set_style_text_color(blush, lv_color_hex(0xFF80AB), 0);
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

    // 11. Foreground Special Expression Elements (Hearts, Sweat, Laser, Smoke, Notes, Extra Shapes)
    left_heart_a_ = lv_obj_create(face_layer_);
    left_heart_b_ = lv_obj_create(face_layer_);
    right_heart_a_ = lv_obj_create(face_layer_);
    right_heart_b_ = lv_obj_create(face_layer_);
    for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
        PrepareShape(h);
        lv_obj_set_style_bg_color(h, lv_color_hex(0xFF1493), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(h, 12, 0);
        lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    }

    sweat_drop_ = lv_obj_create(face_layer_);
    PrepareShape(sweat_drop_);
    lv_obj_set_style_bg_color(sweat_drop_, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(sweat_drop_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sweat_drop_, 6, 0);
    lv_obj_add_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);

    laser_line_ = lv_obj_create(face_layer_);
    PrepareShape(laser_line_);
    lv_obj_set_style_bg_color(laser_line_, lv_color_hex(0xFFFF00), 0);
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
        lv_obj_set_style_radius(music_note_[i], 4, 0);
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

    // 12. Subtitle / Chat Line
    chat_line_ = lv_label_create(scr);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(chat_line_, 230);
    lv_obj_set_pos(chat_line_, 5, 202);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(chat_line_, lvgl_theme->text_font()->font(), 0);
    }
    lv_label_set_text(chat_line_, "");

    ApplyLayout();

    // 13. Start Animation Frame Timer (20 FPS)
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
        wake_up_until_tick_ = animation_tick_ + 30;  // 1.5s wake up animation
        SetFaceMode(FaceMode::kWakingUp);
        if (chat_line_ != nullptr && !has_chat_text_) {
            lv_label_set_text(chat_line_, "");
        }
    }
    idle_quirk_ = IdleQuirk::kNone;
    temporary_emotion_until_tick_ = 0;
}

void PocketLcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (!lock) return;

    if (status == nullptr) return;
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
        }
    }

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
}

void PocketLcdDisplay::SetFaceMode(FaceMode mode) {
    if (face_mode_ == mode) return;
    face_mode_ = mode;
    last_active_tick_ = animation_tick_;
    if (mode == FaceMode::kSurprised) {
        surprised_start_tick_ = animation_tick_;
    }
    if (mode != FaceMode::kNeutral && mode != FaceMode::kSleeping) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }
}

void PocketLcdDisplay::ApplyLayout() {
    if (face_layer_ == nullptr) return;
    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 240, 190);
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

PocketLcdDisplay::FaceMode PocketLcdDisplay::InferEmotionFromText(const char* text) const {
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

PocketLcdDisplay::FaceMode PocketLcdDisplay::FaceModeFromEmotion(const char* emotion) const {
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

void PocketLcdDisplay::UpdateAnimationFrame() {
    if (face_layer_ == nullptr) return;
    DisplayLockGuard lock(this);
    if (!lock) return;
    ++animation_tick_;

    auto& app = Application::GetInstance();
    auto dev_state = app.GetDeviceState();

    if (dev_state != kDeviceStateIdle) {
        if (standby_active_) {
            HideStandbyClock();
        }
        HideQrCode();
        last_active_tick_ = animation_tick_;
    } else {
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
            return;
        }
    }

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
    const uint8_t spk_level = app.GetAudioService().GetOutputLevel();

    // 240x240 Geometry: eye_width = 76, eye_height = 72, gap = 24
    constexpr int face_width = 240;
    constexpr int eye_width = 78;
    constexpr int eye_height = 76;
    constexpr int eye_gap = 22;
    const int pair_width = eye_width * 2 + eye_gap;      // 178px
    const int base_x = (face_width - pair_width) / 2;      // 31px
    const int base_y = (180 - eye_height) / 2;              // 52px
    constexpr int max_look_x = 12;
    constexpr int max_look_y = 8;

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
        } else if (animation_tick_ >= last_active_tick_ + (kAutoSleepTimeoutTicks / 2) && face_mode_ == FaceMode::kNeutral) {
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
            bug_x = 24 + static_cast<int>((qt * 192) / 75);
            int wave = static_cast<int>(qt % 20) - 10;
            bug_y = 44 + (wave < 0 ? -wave : wave) * 2 - 10;
            target_look_x_ = (bug_x - 120) / 9;
            target_look_y_ = (bug_y - 50) / 5;
        } else if (idle_quirk_ == IdleQuirk::kPeekABoo) {
            if (qt < 42) {
                target_look_x_ = -10;
                target_look_y_ = 4;
            } else {
                target_look_x_ = 0;
                target_look_y_ = 0;
            }
        } else if (idle_quirk_ == IdleQuirk::kEyeRoll) {
            int phase = (qt / 12) % 4;
            if (phase == 0) { target_look_x_ = -10; target_look_y_ = 0; }
            else if (phase == 1) { target_look_x_ = 0; target_look_y_ = -8; }
            else if (phase == 2) { target_look_x_ = 10; target_look_y_ = 0; }
            else { target_look_x_ = 0; target_look_y_ = 8; }
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
        target_look_x_ = ((animation_tick_ / 6) % 2 == 0) ? -4 : 4;
        target_look_y_ = -8;
    } else if (face_mode_ == FaceMode::kInnocent) {
        target_look_x_ = ((animation_tick_ / 22) % 2 == 0) ? -5 : 5;
        target_look_y_ = -6;
    } else if (face_mode_ == FaceMode::kThinking) {
        int read_cycle = animation_tick_ % 36;
        if (read_cycle < 20) {
            target_look_x_ = -10 + (read_cycle * 20) / 20;
            target_look_y_ = -4;
        } else {
            target_look_x_ = max_look_x;
            target_look_y_ = -max_look_y;
        }
    } else if (face_mode_ == FaceMode::kListening) {
        target_look_x_ = 0;
        target_look_y_ = (mic_level > 15) ? 0 : (((animation_tick_ / 8) % 2 == 0) ? -4 : 2);
    } else if (face_mode_ == FaceMode::kShy) {
        target_look_x_ = -7;
        target_look_y_ = 6;
    } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kCryingStream) {
        target_look_x_ = 0;
        target_look_y_ = 6;
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kJealous) {
        target_look_x_ = 0;
        target_look_y_ = -6;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        target_look_x_ = 0;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kSmug) {
        target_look_x_ = 8;
        target_look_y_ = -6;
    } else if (face_mode_ == FaceMode::kSideEye) {
        target_look_x_ = 12;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kScheming) {
        target_look_x_ = ((animation_tick_ / 15) % 2 == 0) ? -8 : 8;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kWhispering) {
        target_look_x_ = -9;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kDisgusted) {
        target_look_x_ = -6;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kParanoid) {
        target_look_x_ = (animation_tick_ % 10 < 5) ? -12 : 12;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kConfusedQuestion) {
        target_look_x_ = 6;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kIdeaEureka) {
        target_look_x_ = 0;
        target_look_y_ = -6;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_look_x_ = 0;
        target_look_y_ = 6;
    } else if (face_mode_ == FaceMode::kDrunk) {
        target_look_x_ = ((animation_tick_ / 20) % 2 == 0) ? -8 : 8;
        target_look_y_ = 4;
    } else if (face_mode_ == FaceMode::kBored) {
        int bored_phase = (animation_tick_ / 25) % 3;
        if (bored_phase == 0) {
            target_look_x_ = -10; target_look_y_ = 4;
        } else if (bored_phase == 1) {
            target_look_x_ = 10; target_look_y_ = 4;
        } else {
            target_look_x_ = 0; target_look_y_ = -8;
        }
    } else if (face_mode_ == FaceMode::kScanning) {
        target_look_x_ = ((animation_tick_ / 6) % 2 == 0) ? -10 : 10;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kSkeptical) {
        target_look_x_ = ((animation_tick_ / 25) % 2 == 0) ? -4 : 6;
        target_look_y_ = -2;
    } else if (face_mode_ == FaceMode::kPout) {
        target_look_x_ = 0;
        target_look_y_ = 4;
    } else if (face_mode_ == FaceMode::kSunnyDazzle) {
        target_look_x_ = 0;
        target_look_y_ = -4;
    } else if (face_mode_ == FaceMode::kMusic) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (is_speaking_ || face_mode_ == FaceMode::kWakingUp || face_mode_ == FaceMode::kSmileGentle ||
               face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPlayfulWink ||
               face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kTongueOut) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    // 6. Multi-tier Eye Heights & Squash-and-Stretch Widths (Scaled to 76x72 TFT)
    int target_left_height = eye_height;
    int target_right_height = eye_height;
    int target_left_width = eye_width;
    int target_right_width = eye_width;
    int target_left_y_offset = 0;
    int target_right_y_offset = 0;

    if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kZenMeditation ||
        face_mode_ == FaceMode::kAngelBlessing) {
        target_left_height = 38;
        target_right_height = 38;
        target_left_y_offset = 8;
        target_right_y_offset = 8;
    } else if (blink) {
        target_left_height = 4;
        target_right_height = 4;
        target_left_width = 72;
        target_right_width = 72;
    } else if (face_mode_ == FaceMode::kListening) {
        int boost = (mic_level > 15) ? std::min(10, static_cast<int>(mic_level / 9)) : -4;
        target_left_height = target_right_height = eye_height + boost;
        target_left_width = target_right_width = eye_width - (boost > 4 ? 2 : 0);
    } else if (face_mode_ == FaceMode::kNeutral && idle_quirk_ == IdleQuirk::kPeekABoo) {
        uint32_t qt = animation_tick_ - quirk_start_tick_;
        if (qt < 42) {
            target_left_height = target_right_height = 42;
            target_left_width = target_right_width = 68;
        } else {
            target_left_height = target_right_height = 80;
            target_left_width = target_right_width = 62;
        }
    } else if (face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) {
        target_left_height = 0;
        target_right_height = 0;
    } else if (face_mode_ == FaceMode::kRebooting) {
        int ph = animation_tick_ % 30;
        if (ph < 10) {
            target_left_height = target_right_height = 4;
            target_left_width = target_right_width = 76;
        } else if (ph < 20) {
            target_left_height = target_right_height = 4;
            target_left_width = target_right_width = 16;
        } else {
            target_left_height = target_right_height = eye_height;
            target_left_width = target_right_width = eye_width;
        }
    } else if (face_mode_ == FaceMode::kLoadingBuffer) {
        target_left_height = target_right_height = 54;
        target_left_width = target_right_width = 72;
        target_left_y_offset = target_right_y_offset = 10;
    } else if (face_mode_ == FaceMode::kLowBattery) {
        target_left_height = target_right_height = 32;
        target_left_width = target_right_width = 68;
        target_left_y_offset = target_right_y_offset = 16;
    } else if (face_mode_ == FaceMode::kFacepalm) {
        target_left_height = 16;
        target_right_height = 28;
        target_left_y_offset = 12;
        target_right_y_offset = 12;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        target_left_height = 32;
        target_left_width = 68;
        target_left_y_offset = 4;
        target_right_height = 76;
        target_right_width = 64;
    } else if (face_mode_ == FaceMode::kDisgusted) {
        target_left_height = 32;
        target_right_height = 46;
        target_left_width = 68;
        target_left_y_offset = 10;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kWhispering) {
        target_left_height = target_right_height = 46;
        target_left_y_offset = target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kSneezing) {
        int ph = animation_tick_ % 35;
        if (ph < 22) {
            target_left_height = target_right_height = 72 + (ph / 2);
            target_left_width = target_right_width = 64;
            target_left_y_offset = target_right_y_offset = -(ph / 2);
        } else {
            target_left_height = target_right_height = 8;
            target_left_width = target_right_width = 76;
            target_left_y_offset = target_right_y_offset = 16;
        }
    } else if (face_mode_ == FaceMode::kInnocent || face_mode_ == FaceMode::kShockedJawdrop || face_mode_ == FaceMode::kIdeaEureka) {
        target_left_height = target_right_height = 80;
        target_left_width = target_right_width = 68;
        target_left_y_offset = target_right_y_offset = (face_mode_ == FaceMode::kShockedJawdrop) ? -6 : 2;
    } else if (face_mode_ == FaceMode::kExhausted) {
        target_left_height = target_right_height = 24;
        target_left_width = target_right_width = 72;
        target_left_y_offset = target_right_y_offset = 20;
    } else if (face_mode_ == FaceMode::kParanoid) {
        target_left_height = target_right_height = 32;
        target_left_width = target_right_width = 76;
        target_left_y_offset = target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kStealthNinja) {
        target_left_height = target_right_height = 20;
        target_left_width = target_right_width = 72;
        target_left_y_offset = target_right_y_offset = 0;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        int glitch_squash = (animation_tick_ % 5) * 8;
        target_left_height = eye_height - glitch_squash;
        target_right_height = 38 + glitch_squash;
        target_left_y_offset = (animation_tick_ % 2 == 0) ? -6 : 6;
        target_right_y_offset = -target_left_y_offset;
    } else if (face_mode_ == FaceMode::kHeadache) {
        target_left_height = 28;
        target_right_height = 46;
        target_left_y_offset = 8;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kSunnyDazzle) {
        target_left_height = target_right_height = 50;
        target_left_y_offset = target_right_y_offset = 8;
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        target_left_height = eye_height;
        target_right_height = 38;
        target_right_width = 68;
        target_right_y_offset = 8;
    } else if (face_mode_ == FaceMode::kTongueOut) {
        target_left_height = target_right_height = 46;
        target_left_width = target_right_width = 72;
        target_left_y_offset = target_right_y_offset = -4;
    } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kConfusedQuestion || face_mode_ == FaceMode::kDrunk) {
        target_left_height = eye_height + 8;
        target_left_width = 64;
        target_left_y_offset = -4;
        target_right_height = 42;
        target_right_width = 68;
        target_right_y_offset = 10;
    } else if (face_mode_ == FaceMode::kSmug) {
        target_left_height = eye_height;
        target_right_height = eye_height - 4;
        target_right_y_offset = -4;
    } else if (face_mode_ == FaceMode::kSurprised || face_mode_ == FaceMode::kWakingUp) {
        target_left_height = 84;
        target_right_height = 84;
        target_left_width = 64;
        target_right_width = 64;
    } else if (face_mode_ == FaceMode::kScanning) {
        target_left_height = 54;
        target_right_height = 54;
        target_left_width = 68;
        target_right_width = 68;
    } else if (face_mode_ == FaceMode::kScared) {
        target_left_height = 72;
        target_right_height = 72;
        target_left_width = 64;
        target_right_width = 64;
    } else if (face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kPout) {
        target_left_height = eye_height + 4;
        target_right_height = eye_height + 4;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kCryingStream) {
        target_left_height = eye_height - 8;
        target_right_height = eye_height - 8;
        target_left_y_offset = 4;
        target_right_y_offset = 4;
    } else if (face_mode_ == FaceMode::kShy) {
        target_left_height = eye_height - 8;
        target_right_height = eye_height - 8;
        target_left_y_offset = 6;
        target_right_y_offset = 6;
    }

    // Breathing effect
    if (!blink && !is_speaking_ &&
        (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening ||
         face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kSleepy)) {
        const int phase = static_cast<int>(animation_tick_ % 120);
        const int triangle = (phase < 60) ? phase : (120 - phase);
        const int breath = (triangle * 6) / 61;
        target_left_height += breath;
        target_right_height += breath;
    }

    const bool lid_snap = animation_tick_ < std::max(blink_until_tick_, second_blink_until_tick_) + 6;
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
        int amp_bounce = 2 + (spk_level / 16);
        animation_y = ((animation_tick_ / 2) % 2 == 0) ? -amp_bounce : (amp_bounce / 2);
    } else if (face_mode_ == FaceMode::kSleeping) {
        uint32_t sleep_ticks = (animation_tick_ > last_active_tick_ + kAutoSleepTimeoutTicks)
                                   ? (animation_tick_ - (last_active_tick_ + kAutoSleepTimeoutTicks))
                                   : 0;
        if (sleep_ticks > 200 && (sleep_ticks % 300) < 16) {
            animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;
        }
    } else if (face_mode_ == FaceMode::kEcstaticLaugh) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -4 : 2;
    } else if (face_mode_ == FaceMode::kSadCrying || face_mode_ == FaceMode::kHeartbroken) {
        animation_y = (animation_tick_ % 4 < 2) ? -2 : 0;
    } else if (face_mode_ == FaceMode::kExcited) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -6 : 0;
    } else if (face_mode_ == FaceMode::kMusic) {
        animation_x = ((animation_tick_ / 10) % 2 == 0) ? -6 : 6;
        animation_y = ((animation_tick_ / 5) % 2 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kSmug) {
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -2 : 0;
    } else if (face_mode_ == FaceMode::kDizzy) {
        animation_x = ((animation_tick_ / 4) % 4 < 2) ? -4 : 4;
        animation_y = ((animation_tick_ / 6) % 4 < 2) ? -4 : 4;
    } else if (face_mode_ == FaceMode::kPout) {
        animation_y = ((animation_tick_ / 8) % 2 == 0) ? -2 : 0;
    } else if (face_mode_ == FaceMode::kFreezing) {
        animation_x = (animation_tick_ % 2 == 0) ? -4 : 4;
    } else if (face_mode_ == FaceMode::kHeadache || face_mode_ == FaceMode::kOverheat) {
        animation_x = (animation_tick_ % 4 < 2) ? -4 : 4;
    } else if (face_mode_ == FaceMode::kThunderShock) {
        animation_x = (animation_tick_ % 2 == 0) ? -6 : 6;
        animation_y = (animation_tick_ % 3 == 0) ? -4 : 4;
    } else if (face_mode_ == FaceMode::kPartyCelebrate) {
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -6 : 2;
    } else if (face_mode_ == FaceMode::kMatrixGlitch) {
        animation_x = (animation_tick_ % 3 == 0) ? -8 : 6;
    } else if (face_mode_ == FaceMode::kZenMeditation || face_mode_ == FaceMode::kAngelBlessing) {
        animation_y = ((animation_tick_ / 20) % 2 == 0) ? -4 : 2;
    } else if (face_mode_ == FaceMode::kDrunk) {
        animation_x = ((animation_tick_ / 15) % 2 == 0) ? -4 : 4;
        animation_y = ((animation_tick_ / 10) % 2 == 0) ? -4 : 4;
    } else if (face_mode_ == FaceMode::kSneezing) {
        int ph = animation_tick_ % 35;
        if (ph >= 22 && ph < 28) {
            animation_x = (ph % 2 == 0) ? -6 : 6;
            animation_y = 8;
        }
    }

    if (face_mode_ == FaceMode::kThinking) {
        animation_y = ((animation_tick_ / 6) % 2 == 0) ? -2 : 0;
    } else if (face_mode_ == FaceMode::kSurprised) {
        animation_x = (animation_tick_ % 2 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        animation_x = (animation_tick_ % 2 == 0) ? -4 : 4;
        animation_y = (animation_tick_ % 3 == 0) ? -2 : 2;
    } else if (face_mode_ == FaceMode::kScared) {
        animation_x = (animation_tick_ % 2 == 0) ? -4 : 4;
        animation_y = (animation_tick_ % 2 == 0) ? 2 : -2;
    }

    // Micro-saccades
    if (animation_tick_ >= next_micro_tick_) {
        micro_look_x_ = static_cast<int>(NextRandom() % 5) - 2;
        micro_look_y_ = (NextRandom() % 4 == 0) ? static_cast<int>(NextRandom() % 5) - 2 : 0;
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
    const int left_y = base_y + (eye_height - current_left_height_) / 2 + look_y + animation_y + current_left_y_offset_;
    const int right_y = base_y + (eye_height - current_right_height_) / 2 + look_y + animation_y + current_right_y_offset_;

    if (face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        int left_radius = EyeRadius(current_left_width_, current_left_height_);
        int right_radius = EyeRadius(current_right_width_, current_right_height_);
        if (face_mode_ == FaceMode::kSleeping || face_mode_ == FaceMode::kZenMeditation ||
            face_mode_ == FaceMode::kAngelBlessing) {
            left_radius = 24;
            right_radius = 24;
        }
        lv_obj_set_pos(left_eye_, left_x, left_y);
        lv_obj_set_size(left_eye_, current_left_width_, current_left_height_);
        lv_obj_set_style_radius(left_eye_, left_radius, 0);

        lv_obj_set_pos(right_eye_, right_x, right_y);
        lv_obj_set_size(right_eye_, current_right_width_, current_right_height_);
        lv_obj_set_style_radius(right_eye_, right_radius, 0);
    }

    // 8. Black Pupils (Default W=28, H=32)
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

    int left_pupil_w = 28;
    int left_pupil_h = 32;
    int right_pupil_w = 28;
    int right_pupil_h = 32;

    if (face_mode_ == FaceMode::kListening && mic_level > 10) {
        int boost = (mic_level > 60) ? 4 : 2;
        left_pupil_w = right_pupil_w = 28 + boost;
        left_pupil_h = right_pupil_h = 32 + boost;
    } else if (face_mode_ == FaceMode::kSurprised) {
        uint32_t elapsed = (animation_tick_ > surprised_start_tick_) ? (animation_tick_ - surprised_start_tick_) : 0;
        if (elapsed < 15) {
            left_pupil_w = right_pupil_w = 20;
            left_pupil_h = right_pupil_h = 24;
        } else {
            left_pupil_w = right_pupil_w = 36;
            left_pupil_h = right_pupil_h = 40;
        }
    } else if (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kScheming ||
               face_mode_ == FaceMode::kSideEye || face_mode_ == FaceMode::kJealous) {
        left_pupil_w = right_pupil_w = 24;
        left_pupil_h = right_pupil_h = 28;
    } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury) {
        left_pupil_w = right_pupil_w = 24;
        left_pupil_h = right_pupil_h = 26;
    } else if (face_mode_ == FaceMode::kScared || face_mode_ == FaceMode::kThunderShock ||
               face_mode_ == FaceMode::kShockedJawdrop) {
        left_pupil_w = right_pupil_w = 20;
        left_pupil_h = right_pupil_h = 22;
    } else if (face_mode_ == FaceMode::kTargetLocked) {
        left_pupil_w = 24;
        left_pupil_h = 22;
        right_pupil_w = 18;
        right_pupil_h = 18;
    } else if (face_mode_ == FaceMode::kPout || face_mode_ == FaceMode::kInnocent) {
        left_pupil_w = right_pupil_w = 36;
        left_pupil_h = right_pupil_h = 40;
    } else if (face_mode_ == FaceMode::kExcited || face_mode_ == FaceMode::kMoneyEyes ||
               face_mode_ == FaceMode::kSunnyDazzle) {
        left_pupil_w = right_pupil_w = 32;
        left_pupil_h = right_pupil_h = 36;
    } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kDisgusted) {
        left_pupil_w = 28;
        left_pupil_h = 32;
        right_pupil_w = 24;
        right_pupil_h = 24;
    } else if (face_mode_ == FaceMode::kParanoid) {
        left_pupil_w = right_pupil_w = 20;
        left_pupil_h = right_pupil_h = 22;
    } else if (face_mode_ == FaceMode::kSleepwalking) {
        left_pupil_w = right_pupil_w = 16;
        left_pupil_h = right_pupil_h = 18;
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
        const float ax = std::max(1.0f, (eye_w - pw) / 2.0f - 4.0f);
        const float ay = std::max(1.0f, (eye_h - ph) / 2.0f - 4.0f);
        const float k = (dx / ax) * (dx / ax) + (dy / ay) * (dy / ay);
        if (k > 1.0f) {
            const float s = 1.0f / std::sqrt(k);
            dx *= s;
            dy *= s;
        }
        out_x = static_cast<int>(std::lround(cx + dx));
        out_y = static_cast<int>(std::lround(cy + dy));
        out_x = std::clamp(out_x, eye_x + 4, std::max(eye_x + 4, eye_x + eye_w - pw - 4));
        out_y = std::clamp(out_y, eye_y + 3, std::max(eye_y + 3, eye_y + eye_h - ph - 3));
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
        const int breath_offset = ((animation_tick_ / 25) % 2 == 0) ? -8 : -6;
        lv_obj_remove_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_crescent_mask_, left_x, left_y + breath_offset);
        lv_obj_set_size(left_crescent_mask_, current_left_width_, current_left_height_);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + breath_offset);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    } else if (face_mode_ == FaceMode::kAngelBlessing) {
        lv_obj_remove_flag(left_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_crescent_mask_, left_x, left_y + 8);
        lv_obj_set_size(left_crescent_mask_, current_left_width_, current_left_height_);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + 8);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    } else if (face_mode_ == FaceMode::kPlayfulWink) {
        lv_obj_remove_flag(right_crescent_mask_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(right_crescent_mask_, right_x, right_y + 8);
        lv_obj_set_size(right_crescent_mask_, current_right_width_, current_right_height_);
    }

    // 10. Dual Teardrops for crying
    lv_obj_add_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
    if (face_mode_ == FaceMode::kSadCrying && !blink) {
        lv_obj_remove_flag(left_tear_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
        int tear_slide_y = (animation_tick_ % 14) * 2;
        lv_obj_set_pos(left_tear_, left_x + 28, left_y + current_left_height_ + tear_slide_y - 4);
        lv_obj_set_size(left_tear_, 12, 16);
        lv_obj_set_pos(right_tear_, right_x + 28, right_y + current_right_height_ + tear_slide_y - 4);
        lv_obj_set_size(right_tear_, 12, 16);
    }

    // 11. Blushing marks
    lv_obj_add_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
    if ((face_mode_ == FaceMode::kShy || face_mode_ == FaceMode::kLove ||
         face_mode_ == FaceMode::kPout || face_mode_ == FaceMode::kDrunk ||
         face_mode_ == FaceMode::kInnocent) && !blink) {
        lv_obj_remove_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        int blush_y = (face_mode_ == FaceMode::kLove) ? (base_y + eye_height - 8) : (left_y + current_left_height_ - 8);
        lv_obj_set_pos(left_blush_, left_x + 12, blush_y);
        lv_obj_set_pos(right_blush_, right_x + 12, blush_y);
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

    // Dynamic Catchlights (Asymmetric round pixel discs)
    const bool is_special_sparkle_mode = (face_mode_ == FaceMode::kExcited ||
                                          face_mode_ == FaceMode::kMoneyEyes ||
                                          face_mode_ == FaceMode::kTargetLocked);
    if (!is_special_sparkle_mode) {
        const int total_look_x = current_look_x_ + micro_look_x_;
        const int total_look_y = current_look_y_ + micro_look_y_;
        const int gaze_shift_x = std::clamp(-total_look_x / 4, -2, 2);
        const int gaze_shift_y = std::clamp(-total_look_y / 4, -2, 2);

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
            if (show_left_pupil) {
                PlaceDisc(catchlight_[0][0], catchlight_[0][1], catchlight_[0][2],
                          left_pupil_pos_x + (left_pupil_w - 12) / 2,
                          left_pupil_pos_y + (left_pupil_h - 12) / 2, 12);
            }
            if (show_right_pupil) {
                PlaceDisc(catchlight_[1][0], catchlight_[1][1], catchlight_[1][2],
                          right_pupil_pos_x + (right_pupil_w - 12) / 2,
                          right_pupil_pos_y + (right_pupil_h - 12) / 2, 12);
            }
        } else {
            // Left Eye: 6px solid dot
            if (show_left_pupil && left_pupil_w >= 12 && catchlight_[0][0] != nullptr) {
                int left_diam = (is_happy_mode && left_pupil_w >= 30) ? 6 : 5;
                int left_dot_x = left_pupil_pos_x + left_pupil_w - left_diam - 2 + gaze_shift_x;
                int left_dot_y = left_pupil_pos_y + (left_pupil_h - left_diam) / 2 + gaze_shift_y;
                left_dot_x = std::clamp(left_dot_x, left_pupil_pos_x + 2, left_pupil_pos_x + left_pupil_w - left_diam - 2);
                left_dot_y = std::clamp(left_dot_y, left_pupil_pos_y + 2, left_pupil_pos_y + left_pupil_h - left_diam - 2);
                PlaceDisc(catchlight_[0][0], catchlight_[0][1], catchlight_[0][2], left_dot_x, left_dot_y, left_diam);
            }

            // Right Eye: 12px 3-tier round pixel circle hugged at top-right corner
            if (show_right_pupil && right_pupil_w >= 14 && catchlight_[1][0] != nullptr) {
                int right_diam = 12;
                if (is_happy_mode && right_pupil_w >= 30) {
                    right_diam = 14;
                } else if (right_pupil_w <= 20 || is_angry_mode || is_gloomy_mode) {
                    right_diam = 10;
                }
                int right_dot_x = right_pupil_pos_x + right_pupil_w - right_diam - 2 + gaze_shift_x;
                int right_dot_y = right_pupil_pos_y + 2 + gaze_shift_y;
                right_dot_x = std::clamp(right_dot_x, right_pupil_pos_x + 2, right_pupil_pos_x + right_pupil_w - right_diam - 2);
                right_dot_y = std::clamp(right_dot_y, right_pupil_pos_y + 2, right_pupil_pos_y + right_pupil_h - right_diam - 2);
                PlaceDisc(catchlight_[1][0], catchlight_[1][1], catchlight_[1][2], right_dot_x, right_dot_y, right_diam);

                // Secondary tiny glint (4x4) at bottom-left corner
                if (right_pupil_w >= 24 && (is_happy_mode || face_mode_ == FaceMode::kNeutral) && catchlight_[1][3] != nullptr) {
                    lv_obj_remove_flag(catchlight_[1][3], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(catchlight_[1][3], right_pupil_pos_x + 4, right_pupil_pos_y + right_pupil_h - 8);
                    lv_obj_set_size(catchlight_[1][3], 4, 4);
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
            lv_obj_set_style_radius(extra_shape_[i], 4, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[i], 0, 0);
        }
    }

    // 12. Mode-Specific Distinct Visual Elements (Full 60 Wall-E Emotions)
    if (face_mode_ == FaceMode::kNeutral && idle_quirk_ == IdleQuirk::kBugTracking && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(extra_shape_[0], 6, 6);
            lv_obj_set_pos(extra_shape_[0], bug_x, bug_y);
        }
    } else if (face_mode_ == FaceMode::kSleeping) {
        uint32_t sleep_ticks = (animation_tick_ > last_active_tick_ + kAutoSleepTimeoutTicks)
                                   ? (animation_tick_ - (last_active_tick_ + kAutoSleepTimeoutTicks))
                                   : 0;
        if (sleep_ticks > 60 || face_mode_ == FaceMode::kSleeping) {
            int b1_prog = (animation_tick_ / 3) % 25;
            int b1_x = 110 + b1_prog;
            int b1_y = 35 - b1_prog;
            PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, b1_x, b1_y, 10);

            int b2_prog = (animation_tick_ / 3 + 12) % 25;
            int b2_x = 125 + b2_prog;
            int b2_y = 35 - b2_prog;
            PlaceDisc(smoke_puff_[2], smoke_puff_[3], nullptr, b2_x, b2_y, 14);
        }
    } else if (face_mode_ == FaceMode::kListening && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int wave = ((animation_tick_ + i * 2) % 4) * 4;
                int bar_h = 4 + (mic_level * (i % 2 == 0 ? 28 : 20)) / 100 + (mic_level > 8 ? wave : 0);
                bar_h = std::min(36, bar_h);
                lv_obj_set_size(extra_shape_[i], 4, bar_h);
                lv_obj_set_pos(extra_shape_[i], 112 + i * 6, 90 - bar_h);
            }
        }
    } else if (face_mode_ == FaceMode::kThinking && !blink) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int bounce = (((animation_tick_ / 3) + i) % 3 == 0) ? -6 : 0;
                lv_obj_set_size(extra_shape_[i], 6, 6);
                lv_obj_set_pos(extra_shape_[i], 108 + i * 12, 10 + bounce);
            }
        }
    } else if ((face_mode_ == FaceMode::kLove || face_mode_ == FaceMode::kHeartbroken) && !blink) {
        int pulse = (face_mode_ == FaceMode::kLove) ? ((animation_tick_ % 14 < 7) ? 4 : -2) : 0;
        int hw = 24 + pulse;
        int hh = 44 + pulse * 2;
        int l_cx = left_x + eye_width / 2;
        int l_cy = base_y + eye_height / 2;
        int r_cx = right_x + eye_width / 2;
        int r_cy = base_y + eye_height / 2;

        for (auto* h : {left_heart_a_, left_heart_b_, right_heart_a_, right_heart_b_}) {
            if (h) {
                lv_obj_remove_flag(h, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(h, hw, hh);
                lv_obj_set_style_radius(h, 12, 0);
                lv_obj_set_style_transform_pivot_x(h, hw / 2, 0);
                lv_obj_set_style_transform_pivot_y(h, hh / 2, 0);
            }
        }
        int split = (face_mode_ == FaceMode::kHeartbroken) ? 4 : 0;
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
                    lv_obj_set_size(extra_shape_[i], 6, 54);
                    lv_obj_set_pos(extra_shape_[i], cx - 3, l_cy - 27);
                    lv_obj_set_style_transform_pivot_x(extra_shape_[i], 3, 0);
                    lv_obj_set_style_transform_pivot_y(extra_shape_[i], 27, 0);
                    lv_obj_set_style_transform_rotation(extra_shape_[i], NormalizeAngle(120), 0);
                }
            }
        }
    } else if ((face_mode_ == FaceMode::kDizzy || face_mode_ == FaceMode::kHypnotized) && !blink) {
        int dw = 36;
        int dh = 8;
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
                lv_obj_set_style_radius(d, 4, 0);
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
                    lv_obj_set_style_border_width(extra_shape_[i], 3, 0);
                    lv_obj_set_style_border_color(extra_shape_[i], lv_color_black(), 0);
                    int pulse_r = ((animation_tick_ / 3) % 2 == 0) ? 48 : 32;
                    int cx = (i == 0) ? l_cx : r_cx;
                    int cy = (i == 0) ? l_cy : r_cy;
                    lv_obj_set_size(extra_shape_[i], pulse_r, pulse_r);
                    lv_obj_set_style_radius(extra_shape_[i], 10, 0);
                    lv_obj_set_pos(extra_shape_[i], cx - pulse_r / 2, cy - pulse_r / 2);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kLoadingBuffer && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 2, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 70, 4);
            lv_obj_set_size(extra_shape_[0], 100, 12);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);

            int fill_w = 10 + ((animation_tick_ * 5) % 80);
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 74, 7);
            lv_obj_set_size(extra_shape_[1], fill_w, 6);
            lv_obj_set_style_radius(extra_shape_[1], 0, 0);
        }
    } else if (face_mode_ == FaceMode::kGameOver && !blink) {
        int dw = 36, dh = 8;
        int l_cx = left_x + current_left_width_ / 2;
        int l_cy = left_y + current_left_height_ / 2;
        int r_cx = right_x + current_right_width_ / 2;
        int r_cy = right_y + current_right_height_ / 2;
        for (auto* d : {left_dizzy_a_, left_dizzy_b_, right_dizzy_a_, right_dizzy_b_}) {
            if (d) {
                lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(d, dw, dh);
                lv_obj_set_style_radius(d, 4, 0);
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
            int sweat_x = right_x + current_right_width_ + 6;
            int sweat_y = 12 + ((animation_tick_ * 3) % 40);
            lv_obj_set_pos(sweat_drop_, sweat_x, sweat_y);
            lv_obj_set_size(sweat_drop_, 10, 16);
        }
    } else if ((face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kOverheat) && !blink) {
        for (int i = 0; i < 3; ++i) {
            int phase = (animation_tick_ + i * 5) % 24;
            int smoke_y = left_y - 6 - phase;
            if (smoke_y >= 0) {
                int smoke_x = (i == 0) ? (left_x + 20) : (i == 1) ? 115 : (right_x + 36);
                int s_diam = (phase > 12) ? 12 : 10;
                PlaceDisc(smoke_puff_[i * 2], smoke_puff_[i * 2 + 1], nullptr, smoke_x, smoke_y, s_diam);
            }
        }
        if (face_mode_ == FaceMode::kOverheat && fx_label_a_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_WARNING);
            lv_obj_set_pos(fx_label_a_, 112, 4);
        }
    } else if (face_mode_ == FaceMode::kFireFury && !blink) {
        for (int i = 0; i < 6; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int flame_h = 16 + ((animation_tick_ * 4 + i * 8) % 28);
                int fx_pos = (i < 3) ? (left_x - 6 + i * 32) : (right_x - 4 + (i - 3) * 32);
                int fy_pos = std::max(0, left_y + 10 - flame_h);
                lv_obj_set_size(extra_shape_[i], 8, flame_h);
                lv_obj_set_pos(extra_shape_[i], fx_pos, fy_pos);
                lv_obj_set_style_radius(extra_shape_[i], 4, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kPartyCelebrate && !blink) {
        int p_cycle = (animation_tick_ / 2) % 12;
        if (p_cycle < 9) {
            int radius = 4 + p_cycle * 4;
            int spark_sz = (p_cycle >= 6) ? 4 : 6;
            const int centers[2][2] = {{30, 30}, {210, 30}};

            for (int cluster = 0; cluster < 2; ++cluster) {
                int cx = centers[cluster][0];
                int cy = centers[cluster][1];
                int base_idx = cluster * 3;

                if (smoke_puff_[base_idx]) {
                    lv_obj_remove_flag(smoke_puff_[base_idx], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(smoke_puff_[base_idx], cx, cy - radius);
                    lv_obj_set_size(smoke_puff_[base_idx], spark_sz, spark_sz);
                }
                if (smoke_puff_[base_idx + 1]) {
                    lv_obj_remove_flag(smoke_puff_[base_idx + 1], LV_OBJ_FLAG_HIDDEN);
                    int offset = (radius * 7) / 10;
                    lv_obj_set_pos(smoke_puff_[base_idx + 1], cx + (cluster == 0 ? offset : -offset), cy - offset);
                    lv_obj_set_size(smoke_puff_[base_idx + 1], spark_sz, spark_sz);
                }
                if (smoke_puff_[base_idx + 2]) {
                    lv_obj_remove_flag(smoke_puff_[base_idx + 2], LV_OBJ_FLAG_HIDDEN);
                    int offset = (radius * 7) / 10;
                    lv_obj_set_pos(smoke_puff_[base_idx + 2], cx + (cluster == 0 ? offset : -offset), cy + offset);
                    lv_obj_set_size(smoke_puff_[base_idx + 2], spark_sz, spark_sz);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kMoneyEyes && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, "$");
            lv_label_set_text(fx_label_b_, "$");
            int bounce = ((animation_tick_ / 4) % 2 == 0) ? -2 : 2;
            lv_obj_set_pos(fx_label_a_, left_pupil_pos_x + 8, left_pupil_pos_y + 4 + bounce);
            lv_obj_set_pos(fx_label_b_, right_pupil_pos_x + 8, right_pupil_pos_y + 4 + bounce);
        }
    } else if (face_mode_ == FaceMode::kExcited && !blink) {
        int star_len = ((animation_tick_ / 4) % 2 == 0) ? 28 : 20;
        int star_thick = 4;
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
        PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, 113, 2, 14);
        int ray_ext = ((animation_tick_ / 5) % 2 == 0) ? 4 : 0;
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 98 - ray_ext, 8);
            lv_obj_set_size(extra_shape_[1], 10 + ray_ext, 4);
        }
        if (extra_shape_[2]) {
            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[2], 132, 8);
            lv_obj_set_size(extra_shape_[2], 10 + ray_ext, 4);
        }
        if (extra_shape_[3]) {
            lv_obj_remove_flag(extra_shape_[3], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[3], 106, 20);
            lv_obj_set_size(extra_shape_[3], 4, 6 + ray_ext);
        }
        if (extra_shape_[4]) {
            lv_obj_remove_flag(extra_shape_[4], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[4], 130, 20);
            lv_obj_set_size(extra_shape_[4], 4, 6 + ray_ext);
        }
    } else if (face_mode_ == FaceMode::kMusic && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_AUDIO);
            lv_label_set_text(fx_label_b_, LV_SYMBOL_AUDIO);
            int ny0 = 4 + (((animation_tick_ / 6)) % 2 == 0 ? -4 : 4);
            int ny1 = 4 + (((animation_tick_ / 6) + 1) % 2 == 0 ? -4 : 4);
            lv_obj_set_pos(fx_label_a_, left_x - 24, ny0);
            lv_obj_set_pos(fx_label_b_, right_x + current_right_width_ + 4, ny1);
        }
    } else if (face_mode_ == FaceMode::kConfusedQuestion && !blink) {
        if (fx_label_a_ && fx_label_b_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(fx_label_b_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, "?");
            lv_label_set_text(fx_label_b_, "?");
            int ny = 2 + ((animation_tick_ / 6) % 2 == 0 ? -2 : 4);
            lv_obj_set_pos(fx_label_a_, 112, ny);
            lv_obj_set_pos(fx_label_b_, right_x + current_right_width_ + 6, ny + 4);
        }
    } else if ((face_mode_ == FaceMode::kScanning || face_mode_ == FaceMode::kThunderShock) && !blink) {
        if (laser_line_) {
            lv_obj_remove_flag(laser_line_, LV_OBJ_FLAG_HIDDEN);
            int scan_offset = ((animation_tick_ * 4) % (current_left_height_ + 10));
            lv_obj_set_pos(laser_line_, base_x - 8, left_y - 4 + scan_offset);
            lv_obj_set_size(laser_line_, pair_width + 16, 4);
        }
        if (face_mode_ == FaceMode::kThunderShock && fx_label_a_) {
            lv_obj_remove_flag(fx_label_a_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_label_a_, LV_SYMBOL_CHARGE);
            lv_obj_set_pos(fx_label_a_, 112, 4);
        }
    } else if (face_mode_ == FaceMode::kMatrixGlitch && !blink) {
        for (int i = 0; i < 5; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], (i % 2 == 0) ? lv_color_black() : lv_color_white(), 0);
                int gx = 18 + ((animation_tick_ * 29 + i * 41) % 180);
                int gy = 12 + ((animation_tick_ * 13 + i * 19) % 72);
                int gw = 28 + ((animation_tick_ * 9 + i * 23) % 48);
                lv_obj_set_pos(extra_shape_[i], gx, gy);
                lv_obj_set_size(extra_shape_[i], gw, 5);
                lv_obj_set_style_radius(extra_shape_[i], 0, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kLowBattery && !blink) {
        if ((animation_tick_ % 14 < 9) && extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 3, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 100, 4);
            lv_obj_set_size(extra_shape_[0], 38, 18);
            lv_obj_set_style_radius(extra_shape_[0], 4, 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 138, 9);
            lv_obj_set_size(extra_shape_[1], 4, 8);

            if (extra_shape_[2]) {
                lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(extra_shape_[2], 106, 9);
                lv_obj_set_size(extra_shape_[2], 8, 8);
            }
        }
    } else if (face_mode_ == FaceMode::kAngelBlessing && !blink) {
        if (extra_shape_[0]) {
            int float_y = ((animation_tick_ / 8) % 2 == 0) ? 0 : 4;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 3, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 75, float_y);
            lv_obj_set_size(extra_shape_[0], 90, 14);
            lv_obj_set_style_radius(extra_shape_[0], 8, 0);
        }
    } else if (face_mode_ == FaceMode::kInnocent && !blink) {
        for (int i = 0; i < 2; ++i) {
            int sy = ((animation_tick_ / 5 + i) % 2 == 0) ? 4 : 10;
            int sx = (i == 0) ? (left_x - 12) : (right_x + current_right_width_ + 4);
            PlaceDisc(smoke_puff_[i * 2], smoke_puff_[i * 2 + 1], nullptr, sx, sy, 10);
        }
    } else if (face_mode_ == FaceMode::kTargetLocked && !blink) {
        int cx = right_x + current_right_width_ / 2;
        int cy = right_y + current_right_height_ / 2;
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 3, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[0], 48, 48);
            lv_obj_set_style_radius(extra_shape_[0], 12, 0);
            lv_obj_set_pos(extra_shape_[0], cx - 24, cy - 24);
        }
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[1], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[1], 64, 4);
            lv_obj_set_pos(extra_shape_[1], cx - 32, cy - 2);
        }
        if (extra_shape_[2]) {
            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[2], lv_color_black(), 0);
            lv_obj_set_size(extra_shape_[2], 4, 64);
            lv_obj_set_pos(extra_shape_[2], cx - 2, cy - 32);
        }
    } else if (face_mode_ == FaceMode::kIdeaEureka && !blink) {
        PlaceDisc(smoke_puff_[0], smoke_puff_[1], nullptr, 113, 2, 14);
        if (extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 115, 18);
            lv_obj_set_size(extra_shape_[1], 10, 6);
            lv_obj_set_style_radius(extra_shape_[1], 2, 0);
        }
        if ((animation_tick_ / 4) % 2 == 0) {
            for (int i = 2; i < 4; ++i) {
                if (extra_shape_[i]) {
                    lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_size(extra_shape_[i], 8, 4);
                    lv_obj_set_pos(extra_shape_[i], (i == 2) ? 100 : 132, 10);
                }
            }
        }
    } else if (face_mode_ == FaceMode::kWhispering && !blink) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int float_dy = ((animation_tick_ / 5 + i) % 2 == 0) ? -2 : 2;
                lv_obj_set_size(extra_shape_[i], 4, 4);
                lv_obj_set_pos(extra_shape_[i], 110 + i * 10, 32 + float_dy);
            }
        }
    } else if (face_mode_ == FaceMode::kSneezing && (animation_tick_ % 35) >= 22) {
        for (int i = 0; i < 3; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int dist = ((animation_tick_ % 35) - 22) * 4;
                lv_obj_set_size(extra_shape_[i], 6, 6);
                lv_obj_set_pos(extra_shape_[i], 100 + i * 20, 60 + dist);
            }
        }
    } else if (face_mode_ == FaceMode::kHeadache && !blink) {
        if (extra_shape_[0] && extra_shape_[1] && extra_shape_[2]) {
            int pulse = ((animation_tick_ / 3) % 2 == 0) ? 4 : 0;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 110, 2);
            lv_obj_set_size(extra_shape_[0], 8, 18 + pulse);
            lv_obj_set_style_transform_rotation(extra_shape_[0], NormalizeAngle(250), 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], 122, 2);
            lv_obj_set_size(extra_shape_[1], 8, 18 + pulse);
            lv_obj_set_style_transform_rotation(extra_shape_[1], NormalizeAngle(-250), 0);

            lv_obj_remove_flag(extra_shape_[2], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[2], 104, 12);
            lv_obj_set_size(extra_shape_[2], 32, 6);
        }
    } else if (face_mode_ == FaceMode::kFreezing && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                int ix = (i < 2) ? (left_x + 16 + i * 32) : (right_x + 16 + (i - 2) * 32);
                int ih = (i % 2 == 0) ? 18 : 12;
                lv_obj_set_pos(extra_shape_[i], ix, left_y + current_left_height_ - 2);
                lv_obj_set_size(extra_shape_[i], 6, ih);
            }
        }
        if (extra_shape_[4] && extra_shape_[5]) {
            int chatter = (animation_tick_ % 2 == 0) ? 0 : 2;
            lv_obj_remove_flag(extra_shape_[4], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[4], 100, 80 - chatter);
            lv_obj_set_size(extra_shape_[4], 40, 4);

            lv_obj_remove_flag(extra_shape_[5], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[5], 100, 86 + chatter);
            lv_obj_set_size(extra_shape_[5], 40, 4);
        }
    } else if (face_mode_ == FaceMode::kCryingStream && !blink) {
        for (int i = 0; i < 4; ++i) {
            if (extra_shape_[i]) {
                lv_obj_remove_flag(extra_shape_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(extra_shape_[i], lv_color_hex(0x00E5FF), 0);
                int sx = (i == 0) ? (left_x + 10) : (i == 1) ? (left_x + 44) : (i == 2) ? (right_x + 10) : (right_x + 44);
                int top_y = left_y + current_left_height_ - 4;
                lv_obj_set_pos(extra_shape_[i], sx, top_y);
                lv_obj_set_size(extra_shape_[i], 14, std::max(4, 180 - top_y));
                lv_obj_set_style_radius(extra_shape_[i], 4, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kShockedJawdrop && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(extra_shape_[0], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(extra_shape_[0], 4, 0);
            lv_obj_set_style_border_color(extra_shape_[0], lv_color_white(), 0);
            lv_obj_set_pos(extra_shape_[0], 100, 76);
            lv_obj_set_size(extra_shape_[0], 40, 20);
            lv_obj_set_style_radius(extra_shape_[0], 8, 0);
        }
    } else if (face_mode_ == FaceMode::kTongueOut && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            int wiggle = ((animation_tick_ / 4) % 2 == 0) ? -4 : 4;
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], 88, 72);
            lv_obj_set_size(extra_shape_[0], 64, 4);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[1], lv_color_hex(0xFF4081), 0);
            lv_obj_set_pos(extra_shape_[1], 104 + wiggle, 74);
            lv_obj_set_size(extra_shape_[1], 32, 20);
            lv_obj_set_style_radius(extra_shape_[1], 10, 0);
        }
    } else if (face_mode_ == FaceMode::kRainyGloomy && !blink) {
        if (extra_shape_[0]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(extra_shape_[0], lv_color_hex(0x90A4AE), 0);
            lv_obj_set_pos(extra_shape_[0], 80, 4);
            lv_obj_set_size(extra_shape_[0], 80, 14);
            lv_obj_set_style_radius(extra_shape_[0], 8, 0);
        }
        for (int i = 0; i < 4; ++i) {
            if (smoke_puff_[i]) {
                lv_obj_remove_flag(smoke_puff_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(smoke_puff_[i], lv_color_hex(0x00E5FF), 0);
                int ry = 22 + ((animation_tick_ * 6 + i * 14) % 64);
                lv_obj_set_pos(smoke_puff_[i], 90 + i * 20, ry);
                lv_obj_set_size(smoke_puff_[i], 4, 10);
                lv_obj_set_style_radius(smoke_puff_[i], 2, 0);
            }
        }
    } else if (face_mode_ == FaceMode::kFacepalm && !blink) {
        if (extra_shape_[0] && extra_shape_[1]) {
            lv_obj_remove_flag(extra_shape_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[0], left_x + 8, 8);
            lv_obj_set_size(extra_shape_[0], 76, 28);
            lv_obj_set_style_radius(extra_shape_[0], 8, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[0], NormalizeAngle(-120), 0);

            lv_obj_remove_flag(extra_shape_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(extra_shape_[1], left_x + 24, 32);
            lv_obj_set_size(extra_shape_[1], 64, 20);
            lv_obj_set_style_radius(extra_shape_[1], 6, 0);
            lv_obj_set_style_transform_rotation(extra_shape_[1], NormalizeAngle(-80), 0);
        }
        if (sweat_drop_) {
            lv_obj_remove_flag(sweat_drop_, LV_OBJ_FLAG_HIDDEN);
            int sy = 12 + ((animation_tick_ * 4) % 40);
            lv_obj_set_pos(sweat_drop_, right_x + current_right_width_ + 6, sy);
            lv_obj_set_size(sweat_drop_, 10, 16);
        }
    }

    // 13. Eyelids (Dynamic Angled Top & Bottom Lids)
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kZenMeditation &&
        face_mode_ != FaceMode::kAngelBlessing && face_mode_ != FaceMode::kLove &&
        face_mode_ != FaceMode::kHeartbroken && !blink) {
        if (face_mode_ == FaceMode::kAngry) {
            constexpr int lid_w = 100;
            constexpr int lid_h = 54;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 12, left_y - 36);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(140), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 12, right_y - 36);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-140), 0);

        } else if (face_mode_ == FaceMode::kFurious || face_mode_ == FaceMode::kFireFury || face_mode_ == FaceMode::kHeadache) {
            constexpr int lid_w = 108;
            constexpr int lid_h = 58;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 16, left_y - 38);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(180), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 16, right_y - 38);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-180), 0);

            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 4, left_y + current_left_height_ - 12);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 8, 20);
            lv_obj_set_pos(right_bottom_lid_, right_x - 4, right_y + current_right_height_ - 12);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 8, 20);

        } else if (face_mode_ == FaceMode::kSadEmpathy || face_mode_ == FaceMode::kSadCrying ||
                   face_mode_ == FaceMode::kCryingStream || face_mode_ == FaceMode::kRainyGloomy) {
            constexpr int lid_w = 100;
            constexpr int lid_h = 54;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(left_top_lid_, left_x - 12, left_y - 36);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-140), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 12, right_y - 36);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(140), 0);

        } else if (face_mode_ == FaceMode::kSleepy) {
            constexpr int sleepy_cover = 26;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 4, left_y - 4);
            lv_obj_set_size(left_top_lid_, current_left_width_ + 8, sleepy_cover + 4);
            lv_obj_set_pos(right_top_lid_, right_x - 4, right_y - 4);
            lv_obj_set_size(right_top_lid_, current_right_width_ + 8, sleepy_cover + 4);

        } else if (face_mode_ == FaceMode::kBored || face_mode_ == FaceMode::kDisgusted) {
            constexpr int bored_cover = 34;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 4, left_y - 4);
            lv_obj_set_size(left_top_lid_, current_left_width_ + 8, bored_cover + 4);
            lv_obj_set_pos(right_top_lid_, right_x - 4, right_y - 4);
            lv_obj_set_size(right_top_lid_, current_right_width_ + 8, bored_cover + 4);

            if (face_mode_ == FaceMode::kDisgusted) {
                lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(left_bottom_lid_, left_x - 4, left_y + current_left_height_ - 12);
                lv_obj_set_size(left_bottom_lid_, current_left_width_ + 8, 16);
            }

        } else if (face_mode_ == FaceMode::kSideEye) {
            constexpr int lid_w = 100;
            constexpr int lid_h = 48;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 12, left_y - 30);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(100), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 12, right_y - 30);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(100), 0);

        } else if (face_mode_ == FaceMode::kJealous) {
            constexpr int lid_w = 100;
            constexpr int lid_h = 48;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 12, left_y - 30);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(-120), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 12, right_y - 30);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(120), 0);

        } else if (face_mode_ == FaceMode::kScheming) {
            constexpr int lid_w = 100;
            constexpr int lid_h = 48;
            lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_top_lid_, left_x - 12, left_y - 28);
            lv_obj_set_size(left_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(left_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(left_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(left_top_lid_, NormalizeAngle(150), 0);

            lv_obj_set_pos(right_top_lid_, right_x - 12, right_y - 28);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-150), 0);

        } else if (face_mode_ == FaceMode::kSkeptical || face_mode_ == FaceMode::kConfusedQuestion) {
            constexpr int lid_w = 90;
            constexpr int lid_h = 40;
            lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_top_lid_, right_x - 10, right_y - 24);
            lv_obj_set_size(right_top_lid_, lid_w, lid_h);
            lv_obj_set_style_transform_pivot_x(right_top_lid_, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(right_top_lid_, lid_h / 2, 0);
            lv_obj_set_style_transform_rotation(right_top_lid_, NormalizeAngle(-60), 0);

        } else if (face_mode_ == FaceMode::kSmug) {
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 4, left_y + current_left_height_ - 12);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 8, 20);

            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(right_bottom_lid_, right_x - 4, right_y + current_right_height_ - 28);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 8, 36);
        }

        // Laugh / Smile / TongueOut / SunnyDazzle / Speaking Cheek Bump
        if (face_mode_ == FaceMode::kEcstaticLaugh || face_mode_ == FaceMode::kPartyCelebrate ||
            face_mode_ == FaceMode::kTongueOut || face_mode_ == FaceMode::kSunnyDazzle) {
            constexpr int cover_height = 18;
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 4, left_y + current_left_height_ - cover_height + 8);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 8, cover_height);
            lv_obj_set_pos(right_bottom_lid_, right_x - 4, right_y + current_right_height_ - cover_height + 8);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 8, cover_height);
        } else if (face_mode_ == FaceMode::kSmileGentle || face_mode_ == FaceMode::kMusic ||
                   (is_speaking_ && (face_mode_ == FaceMode::kNeutral || face_mode_ == FaceMode::kListening))) {
            int dynamic_cheek = 24 + (is_speaking_ ? (spk_level / 10) : 4);
            lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(left_bottom_lid_, left_x - 4, left_y + current_left_height_ - dynamic_cheek / 2);
            lv_obj_set_size(left_bottom_lid_, current_left_width_ + 8, dynamic_cheek);
            lv_obj_set_pos(right_bottom_lid_, right_x - 4, right_y + current_right_height_ - dynamic_cheek / 2);
            lv_obj_set_size(right_bottom_lid_, current_right_width_ + 8, dynamic_cheek);
        }
    }
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

    std::vector<uint8_t> qrcode(qrcodegen_BUFFER_LEN_MAX);
    std::vector<uint8_t> tempBuffer(qrcodegen_BUFFER_LEN_MAX);
    bool ok = qrcodegen_encodeText(text.c_str(), tempBuffer.data(), qrcode.data(), qrcodegen_Ecc_LOW, 1, 8, qrcodegen_Mask_AUTO, true);
    if (ok) {
        int qr_size = qrcodegen_getSize(qrcode.data());
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
                if (qrcodegen_getModule(qrcode.data(), x, y)) {
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

void PocketLcdDisplay::SetPinnedMemo(const std::string& memo) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    if (memo.empty()) {
        pinned_memo_ = "READY • SAY WAKE WORD";
    } else {
        pinned_memo_ = "PIN: " + memo;
    }
    if (pinned_memo_label_ != nullptr) {
        lv_label_set_text(pinned_memo_label_, pinned_memo_.c_str());
    }
}

void PocketLcdDisplay::SetCountdownTimer(int seconds, const std::string& label) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    countdown_remaining_sec_ = std::max(0, seconds);
    countdown_label_ = label.empty() ? "FOCUS" : label;
    if (countdown_remaining_sec_ == 0) {
        countdown_label_ = "IDLE";
    }
    if (timer_val_label_ != nullptr) {
        char timer_buf[16];
        snprintf(timer_buf, sizeof(timer_buf), "%02d:%02d",
                 countdown_remaining_sec_ / 60, countdown_remaining_sec_ % 60);
        lv_label_set_text(timer_val_label_, timer_buf);
        lv_obj_set_style_text_color(timer_val_label_,
            (countdown_remaining_sec_ > 0) ? lv_color_hex(0xFF5500) : lv_color_hex(0x8E8E93), 0);
    }
    if (timer_tag_label_ != nullptr) {
        lv_label_set_text(timer_tag_label_, countdown_label_.c_str());
    }
}

void PocketLcdDisplay::SetGuardMode(bool enable) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    guard_mode_active_ = enable;
    if (guard_icon_ != nullptr) {
        if (guard_mode_active_) {
            lv_obj_set_style_bg_color(guard_icon_, lv_color_hex(0x32D74B), 0);
            lv_obj_set_style_border_width(guard_icon_, 0, 0);
        } else {
            lv_obj_set_style_bg_color(guard_icon_, lv_color_hex(0x262629), 0);
            lv_obj_set_style_border_width(guard_icon_, 1, 0);
            lv_obj_set_style_border_color(guard_icon_, lv_color_hex(0x8E8E93), 0);
        }
    }
    if (guard_status_label_ != nullptr) {
        lv_label_set_text(guard_status_label_, guard_mode_active_ ? "ON" : "OFF");
        lv_obj_set_style_text_color(guard_status_label_,
            guard_mode_active_ ? lv_color_hex(0x32D74B) : lv_color_hex(0x8E8E93), 0);
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
        lv_obj_set_size(standby_container_, 240, 240);
        lv_obj_set_pos(standby_container_, 0, 0);
        lv_obj_set_style_bg_color(standby_container_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(standby_container_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(standby_container_, LV_OBJ_FLAG_SCROLLABLE);

        // 1. TOP STATUS BAR (x: 6, y: 4, w: 228, h: 28)
        status_ready_label_ = lv_label_create(standby_container_);
        lv_obj_set_pos(status_ready_label_, 6, 8);
        lv_obj_set_style_text_color(status_ready_label_, lv_color_hex(0x32D74B), 0);
        lv_label_set_text(status_ready_label_, "READY");

        pill_container_ = lv_obj_create(standby_container_);
        lv_obj_remove_style_all(pill_container_);
        lv_obj_set_size(pill_container_, 68, 20);
        lv_obj_set_pos(pill_container_, 86, 6);
        lv_obj_set_style_bg_color(pill_container_, lv_color_hex(0xFF5500), 0);
        lv_obj_set_style_bg_opa(pill_container_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill_container_, 8, 0);

        pill_label_ = lv_label_create(pill_container_);
        lv_obj_center(pill_label_);
        lv_obj_set_style_text_color(pill_label_, lv_color_hex(0x000000), 0);
        lv_label_set_text(pill_label_, "XIAOZHI");

        wifi_rssi_label_ = lv_label_create(standby_container_);
        lv_obj_set_pos(wifi_rssi_label_, 140, 8);
        lv_obj_set_width(wifi_rssi_label_, 94);
        lv_obj_set_style_text_align(wifi_rssi_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(wifi_rssi_label_, lv_color_hex(0x0A84FF), 0);
        lv_label_set_text(wifi_rssi_label_, "WIFI --dB");

        // 2. HERO CLOCK & MEMO CARD (x: 6, y: 34, w: 228, h: 114)
        hero_card_ = lv_obj_create(standby_container_);
        lv_obj_remove_style_all(hero_card_);
        lv_obj_set_size(hero_card_, 228, 114);
        lv_obj_set_pos(hero_card_, 6, 34);
        lv_obj_set_style_bg_color(hero_card_, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(hero_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hero_card_, 12, 0);
        lv_obj_set_style_border_width(hero_card_, 1, 0);
        lv_obj_set_style_border_color(hero_card_, lv_color_hex(0x262629), 0);
        lv_obj_clear_flag(hero_card_, LV_OBJ_FLAG_SCROLLABLE);

        hero_accent_bar_ = lv_obj_create(hero_card_);
        lv_obj_remove_style_all(hero_accent_bar_);
        lv_obj_set_size(hero_accent_bar_, 3, 90);
        lv_obj_set_pos(hero_accent_bar_, 4, 12);
        lv_obj_set_style_bg_color(hero_accent_bar_, lv_color_hex(0xFF5500), 0);
        lv_obj_set_style_bg_opa(hero_accent_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hero_accent_bar_, 2, 0);

        date_label_ = lv_label_create(hero_card_);
        lv_obj_set_pos(date_label_, 16, 8);
        lv_obj_set_style_text_color(date_label_, lv_color_hex(0xFF5500), 0);
        lv_label_set_text(date_label_, "MON NOV 9");

        time_label_ = lv_label_create(hero_card_);
        lv_obj_set_pos(time_label_, 14, 26);
        lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_32
        lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_32, 0);
#elif defined(BUILTIN_TEXT_FONT)
        lv_obj_set_style_text_font(time_label_, &BUILTIN_TEXT_FONT, 0);
#endif
        lv_label_set_text(time_label_, "10:09");

        sec_label_ = lv_label_create(hero_card_);
        lv_obj_set_pos(sec_label_, 152, 42);
        lv_obj_set_style_text_color(sec_label_, lv_color_hex(0xFF5500), 0);
#if LV_FONT_MONTSERRAT_20
        lv_obj_set_style_text_font(sec_label_, &lv_font_montserrat_20, 0);
#endif
        lv_label_set_text(sec_label_, ":35");

        pinned_memo_label_ = lv_label_create(hero_card_);
        lv_obj_set_pos(pinned_memo_label_, 16, 88);
        lv_obj_set_width(pinned_memo_label_, 200);
        lv_label_set_long_mode(pinned_memo_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_color(pinned_memo_label_, lv_color_hex(0xFFD60A), 0);
        lv_label_set_text(pinned_memo_label_, pinned_memo_.c_str());

        // 3. BOTTOM 3 BENTO WIDGETS (y: 154, h: 80)
        // Box 1: TIMER (x: 6, w: 72, h: 80)
        timer_card_ = lv_obj_create(standby_container_);
        lv_obj_remove_style_all(timer_card_);
        lv_obj_set_size(timer_card_, 72, 80);
        lv_obj_set_pos(timer_card_, 6, 154);
        lv_obj_set_style_bg_color(timer_card_, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(timer_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(timer_card_, 12, 0);
        lv_obj_set_style_border_width(timer_card_, 1, 0);
        lv_obj_set_style_border_color(timer_card_, lv_color_hex(0x262629), 0);
        lv_obj_clear_flag(timer_card_, LV_OBJ_FLAG_SCROLLABLE);

        timer_title_label_ = lv_label_create(timer_card_);
        lv_obj_set_pos(timer_title_label_, 0, 6);
        lv_obj_set_width(timer_title_label_, 72);
        lv_obj_set_style_text_align(timer_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(timer_title_label_, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(timer_title_label_, "TIMER");

        timer_val_label_ = lv_label_create(timer_card_);
        lv_obj_set_pos(timer_val_label_, 0, 26);
        lv_obj_set_width(timer_val_label_, 72);
        lv_obj_set_style_text_align(timer_val_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(timer_val_label_, lv_color_hex(0x8E8E93), 0);
#if LV_FONT_MONTSERRAT_20
        lv_obj_set_style_text_font(timer_val_label_, &lv_font_montserrat_20, 0);
#endif
        lv_label_set_text(timer_val_label_, "00:00");

        timer_tag_label_ = lv_label_create(timer_card_);
        lv_obj_set_pos(timer_tag_label_, 0, 56);
        lv_obj_set_width(timer_tag_label_, 72);
        lv_obj_set_style_text_align(timer_tag_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(timer_tag_label_, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(timer_tag_label_, "IDLE");

        // Box 2: VOL (x: 84, w: 72, h: 80)
        vol_card_ = lv_obj_create(standby_container_);
        lv_obj_remove_style_all(vol_card_);
        lv_obj_set_size(vol_card_, 72, 80);
        lv_obj_set_pos(vol_card_, 84, 154);
        lv_obj_set_style_bg_color(vol_card_, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(vol_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(vol_card_, 12, 0);
        lv_obj_set_style_border_width(vol_card_, 1, 0);
        lv_obj_set_style_border_color(vol_card_, lv_color_hex(0x262629), 0);
        lv_obj_clear_flag(vol_card_, LV_OBJ_FLAG_SCROLLABLE);

        vol_title_label_ = lv_label_create(vol_card_);
        lv_obj_set_pos(vol_title_label_, 0, 6);
        lv_obj_set_width(vol_title_label_, 72);
        lv_obj_set_style_text_align(vol_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vol_title_label_, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(vol_title_label_, "VOL");

        vol_arc_ = lv_arc_create(vol_card_);
        lv_obj_set_size(vol_arc_, 48, 48);
        lv_obj_set_pos(vol_arc_, 12, 24);
        lv_arc_set_rotation(vol_arc_, 135);
        lv_arc_set_bg_angles(vol_arc_, 0, 270);
        lv_arc_set_range(vol_arc_, 0, 100);
        lv_obj_remove_style(vol_arc_, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(vol_arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(vol_arc_, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(vol_arc_, lv_color_hex(0x262629), 0);
        lv_obj_set_style_arc_width(vol_arc_, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(vol_arc_, lv_color_hex(0x0A84FF), LV_PART_INDICATOR);

        vol_val_label_ = lv_label_create(vol_card_);
        lv_obj_set_pos(vol_val_label_, 0, 38);
        lv_obj_set_width(vol_val_label_, 72);
        lv_obj_set_style_text_align(vol_val_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vol_val_label_, lv_color_hex(0x0A84FF), 0);
        lv_label_set_text(vol_val_label_, "80%");

        // Box 3: GUARD (x: 162, w: 72, h: 80)
        guard_card_ = lv_obj_create(standby_container_);
        lv_obj_remove_style_all(guard_card_);
        lv_obj_set_size(guard_card_, 72, 80);
        lv_obj_set_pos(guard_card_, 162, 154);
        lv_obj_set_style_bg_color(guard_card_, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(guard_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(guard_card_, 12, 0);
        lv_obj_set_style_border_width(guard_card_, 1, 0);
        lv_obj_set_style_border_color(guard_card_, lv_color_hex(0x262629), 0);
        lv_obj_clear_flag(guard_card_, LV_OBJ_FLAG_SCROLLABLE);

        guard_title_label_ = lv_label_create(guard_card_);
        lv_obj_set_pos(guard_title_label_, 0, 6);
        lv_obj_set_width(guard_title_label_, 72);
        lv_obj_set_style_text_align(guard_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(guard_title_label_, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(guard_title_label_, "GUARD");

        guard_icon_ = lv_obj_create(guard_card_);
        lv_obj_remove_style_all(guard_icon_);
        lv_obj_set_size(guard_icon_, 24, 24);
        lv_obj_set_pos(guard_icon_, 24, 28);
        lv_obj_set_style_radius(guard_icon_, 6, 0);
        lv_obj_set_style_bg_color(guard_icon_, lv_color_hex(0x262629), 0);
        lv_obj_set_style_bg_opa(guard_icon_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(guard_icon_, 1, 0);
        lv_obj_set_style_border_color(guard_icon_, lv_color_hex(0x8E8E93), 0);

        guard_status_label_ = lv_label_create(guard_card_);
        lv_obj_set_pos(guard_status_label_, 0, 56);
        lv_obj_set_width(guard_status_label_, 72);
        lv_obj_set_style_text_align(guard_status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(guard_status_label_, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(guard_status_label_, "OFF");
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

    // 1. System Time & Date
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char time_buf[16];
    char sec_buf[16];
    char date_buf[64];

    if (tm_now.tm_year >= (2025 - 1900)) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        snprintf(sec_buf, sizeof(sec_buf), ":%02d", tm_now.tm_sec);
        const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        snprintf(date_buf, sizeof(date_buf), "%s %s %d",
                 days[tm_now.tm_wday % 7], months[tm_now.tm_mon % 12], tm_now.tm_mday);
    } else {
        uint32_t uptime_sec = animation_tick_ / 20;
        uint32_t hrs = uptime_sec / 3600;
        uint32_t mins = (uptime_sec % 3600) / 60;
        uint32_t secs = uptime_sec % 60;
        snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu", (unsigned long)hrs, (unsigned long)mins);
        snprintf(sec_buf, sizeof(sec_buf), ":%02lu", (unsigned long)secs);
        snprintf(date_buf, sizeof(date_buf), "CONNECTING...");
    }

    if (time_label_) lv_label_set_text(time_label_, time_buf);
    if (sec_label_) lv_label_set_text(sec_label_, sec_buf);
    if (date_label_) lv_label_set_text(date_label_, date_buf);

    // 2. Real Wi-Fi RSSI via esp_wifi_sta_get_ap_info
    wifi_ap_record_t ap_info;
    int rssi = -99;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    char wifi_buf[32];
    if (rssi > -100 && rssi < 0) {
        snprintf(wifi_buf, sizeof(wifi_buf), "WIFI %ddB", rssi);
    } else {
        snprintf(wifi_buf, sizeof(wifi_buf), "WIFI DISCONN");
    }
    if (wifi_rssi_label_) lv_label_set_text(wifi_rssi_label_, wifi_buf);

    // 3. Status label
    if (status_ready_label_) {
        auto& app = Application::GetInstance();
        DeviceState st = app.GetDeviceState();
        if (st == kDeviceStateListening) {
            lv_label_set_text(status_ready_label_, "LISTENING");
            lv_obj_set_style_text_color(status_ready_label_, lv_color_hex(0x0A84FF), 0);
        } else if (st == kDeviceStateSpeaking) {
            lv_label_set_text(status_ready_label_, "TALKING");
            lv_obj_set_style_text_color(status_ready_label_, lv_color_hex(0xFFD60A), 0);
        } else {
            lv_label_set_text(status_ready_label_, "READY");
            lv_obj_set_style_text_color(status_ready_label_, lv_color_hex(0x32D74B), 0);
        }
    }

    // 4. Volume Widget (Real volume from AudioCodec)
    auto codec = Board::GetInstance().GetAudioCodec();
    int vol = (codec != nullptr) ? codec->output_volume() : 70;
    if (vol_arc_) lv_arc_set_value(vol_arc_, vol);
    if (vol_val_label_) {
        char vol_buf[16];
        snprintf(vol_buf, sizeof(vol_buf), "%d%%", vol);
        lv_label_set_text(vol_val_label_, vol_buf);
    }

    // 5. Countdown / Pomodoro Timer
    if (animation_tick_ >= last_countdown_tick_ + 20) {
        last_countdown_tick_ = animation_tick_;
        if (countdown_remaining_sec_ > 0) {
            countdown_remaining_sec_--;
            if (countdown_remaining_sec_ == 0) {
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
                countdown_label_ = "IDLE";
            }
        }
    }
    if (timer_val_label_) {
        char timer_buf[16];
        snprintf(timer_buf, sizeof(timer_buf), "%02d:%02d",
                 countdown_remaining_sec_ / 60, countdown_remaining_sec_ % 60);
        lv_label_set_text(timer_val_label_, timer_buf);
        lv_obj_set_style_text_color(timer_val_label_,
            (countdown_remaining_sec_ > 0) ? lv_color_hex(0xFF5500) : lv_color_hex(0x8E8E93), 0);
    }
    if (timer_tag_label_) {
        lv_label_set_text(timer_tag_label_, countdown_label_.c_str());
    }

    // 6. Guard Mode Widget & Acoustic Monitoring
    if (guard_icon_) {
        if (guard_mode_active_) {
            lv_obj_set_style_bg_color(guard_icon_, lv_color_hex(0x32D74B), 0);
            lv_obj_set_style_border_width(guard_icon_, 0, 0);
        } else {
            lv_obj_set_style_bg_color(guard_icon_, lv_color_hex(0x262629), 0);
            lv_obj_set_style_border_width(guard_icon_, 1, 0);
            lv_obj_set_style_border_color(guard_icon_, lv_color_hex(0x8E8E93), 0);
        }
    }
    if (guard_status_label_) {
        lv_label_set_text(guard_status_label_, guard_mode_active_ ? "ON" : "OFF");
        lv_obj_set_style_text_color(guard_status_label_,
            guard_mode_active_ ? lv_color_hex(0x32D74B) : lv_color_hex(0x8E8E93), 0);
    }
    if (guard_mode_active_) {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) {
            uint8_t mic_lvl = app.GetAudioService().GetInputLevel();
            if (mic_lvl > 35 && animation_tick_ >= guard_alarm_cooldown_tick_) {
                guard_alarm_cooldown_tick_ = animation_tick_ + 100; // 5s cooldown
                app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            }
        }
    }
}
