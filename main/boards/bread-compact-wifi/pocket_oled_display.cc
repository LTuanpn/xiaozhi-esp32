#include "pocket_oled_display.h"
#include "assets/lang_config.h"

#include <algorithm>
#include <cstring>
#include <esp_log.h>

namespace {
constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kAutoSleepTimeoutTicks = 45000 / kAnimationPeriodMs; // 45s = 1125 ticks

void PrepareShape(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

int Ease(int current, int target) {
    if (current == target) {
        return current;
    }
    const int step = (target - current) / 2;
    if (step == 0) {
        return target > current ? current + 1 : current - 1;
    }
    return current + step;
}

bool Contains(const char* text, const char* pattern) {
    return text != nullptr && pattern != nullptr && std::strstr(text, pattern) != nullptr;
}
}  // namespace

PocketOledDisplay::PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel,
                                     int width,
                                     int height,
                                     bool mirror_x,
                                     bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {
}

PocketOledDisplay::~PocketOledDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_del(animation_timer_);
        animation_timer_ = nullptr;
    }
}

void PocketOledDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    // Chỉ gọi Display::SetupUI(), tuyệt đối KHÔNG gọi OledDisplay::SetupUI() để tránh icon/đồng hồ cũ
    Display::SetupUI();

    DisplayLockGuard lock(this);
    auto* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Face layer bao phủ toàn bộ màn hình (128x64) nền đen sâu tuyệt đối
    face_layer_ = lv_obj_create(screen);
    PrepareShape(face_layer_);
    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 128, 64);
    lv_obj_set_style_bg_color(face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_layer_, LV_OPA_COVER, 0);

    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);
    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);

    // Khởi tạo đôi mắt to tròn (màu trắng hiển thị thành màu xanh dương ở vùng y: 16-63)
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 16, 0);
    }
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 8, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // Dòng chữ phụ đề/status: Tọa độ y = 50, cao 14 (nằm hoàn toàn trong dải xanh y: 16-63)
    // DÙNG LV_LABEL_LONG_CLIP: ĐỨNG YÊN HOÀN TOÀN, TUYỆT ĐỐI KHÔNG CUỘN DỌC
    chat_line_ = lv_label_create(face_layer_);
    lv_obj_set_pos(chat_line_, 0, 50);
    lv_obj_set_size(chat_line_, 128, 14);
    lv_obj_set_style_bg_color(chat_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(chat_line_, 0, 0);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(chat_line_, "zZz...");

    // Mặc định KHỞI ĐỘNG LÀ NHẮM MẮT NGỦ SAY
    face_mode_ = FaceMode::kSleeping;
    current_eye_height_ = 2;
    last_active_tick_ = 0;
    wake_up_until_tick_ = 0;

    ApplyLayout();
    animation_timer_ = lv_timer_create(AnimationTimerCallback, kAnimationPeriodMs, this);
}

void PocketOledDisplay::AnimationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<PocketOledDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        display->UpdateAnimationFrame();
    }
}

void PocketOledDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);

    if (status == nullptr) {
        return;
    }

    // Nếu đang ngủ: CHỈ thức giấc khi có tương tác nghe (LISTENING) hoặc nói (SPEAKING)
    if (face_mode_ == FaceMode::kSleeping) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
            std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            last_active_tick_ = animation_tick_;
            wake_up_until_tick_ = animation_tick_ + 25; // 1 giây thức giấc mở to mắt
            SetFaceMode(FaceMode::kWakingUp);
            return;
        }
        // Các trạng thái hệ thống khác khi đang ngủ thì vẫn giữ ngủ say
        return;
    }

    last_active_tick_ = animation_tick_;

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        SetFaceMode(FaceMode::kListening);
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        SetFaceMode(FaceMode::kSpeaking);
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 || Contains(status, "Đang") ||
               Contains(status, "đang")) {
        SetFaceMode(FaceMode::kThinking);
    } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
            SetFaceMode(FaceMode::kNeutral);
        }
    }

    if (!has_chat_text_ && chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, status);
        }
    }
}

bool PocketOledDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (emotion == nullptr) {
        return false;
    }

    last_active_tick_ = animation_tick_;

    if (face_mode_ == FaceMode::kSleeping) {
        wake_up_until_tick_ = animation_tick_ + 25;
        SetFaceMode(FaceMode::kWakingUp);
        return true;
    }

    SetFaceMode(FaceModeFromEmotion(emotion));
    return true;
}

void PocketOledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);

    has_chat_text_ = content != nullptr && content[0] != '\0';
    if (has_chat_text_) {
        last_active_tick_ = animation_tick_;
        if (face_mode_ == FaceMode::kSleeping) {
            wake_up_until_tick_ = animation_tick_ + 25;
            SetFaceMode(FaceMode::kWakingUp);
        }
    }

    if (chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
        if (has_chat_text_) {
            lv_label_set_text(chat_line_, content);
        } else {
            if (face_mode_ == FaceMode::kSleeping) {
                lv_label_set_text(chat_line_, "zZz...");
            } else {
                lv_label_set_text(chat_line_, "");
            }
        }
    }

    if (has_chat_text_ && role != nullptr && std::strcmp(role, "assistant") == 0) {
        if (face_mode_ != FaceMode::kSpeaking) {
            SetFaceMode(FaceMode::kSpeaking);
        }
    }
}

void PocketOledDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    has_chat_text_ = false;
    if (chat_line_ != nullptr) {
        lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
        if (face_mode_ == FaceMode::kSleeping) {
            lv_label_set_text(chat_line_, "zZz...");
        } else {
            lv_label_set_text(chat_line_, "");
        }
    }
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
        SetFaceMode(FaceMode::kNeutral);
    }
}

void PocketOledDisplay::ApplyLayout() {
    if (face_layer_ == nullptr) {
        return;
    }

    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 128, 64);
    UpdateAnimationFrame();
}

void PocketOledDisplay::UpdateAnimationFrame() {
    if (face_layer_ == nullptr || left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }
    ++animation_tick_;

    // TỌA ĐỘ VÀ KÍCH THƯỚC CHUẨN XÁC TRONG DẢI MÀU XANH:
    // Dải vàng: y = 0 -> 15 (TẮT ĐEN 100%, TUYỆT ĐỐI KHÔNG CÓ PIXEL NÀO SÁNG)
    // Dải xanh: y = 16 -> 63
    constexpr int eye_width = 40;
    constexpr int eye_height = 32; // Mắt cao 32px, to tròn tuyệt đẹp
    constexpr int eye_gap = 12;
    const int pair_width = eye_width * 2 + eye_gap;
    const int base_x = (128 - pair_width) / 2; // = 18
    const int base_y = 17; // Tọa độ y trung tâm trong vùng xanh
    constexpr int max_look_x = 6;
    constexpr int max_look_y = 2;

    // 1. Tự động đi ngủ sau 45s không có tương tác
    if (face_mode_ != FaceMode::kSleeping && face_mode_ != FaceMode::kWakingUp) {
        if (animation_tick_ >= last_active_tick_ + kAutoSleepTimeoutTicks) {
            SetFaceMode(FaceMode::kSleeping);
            if (chat_line_ != nullptr && !has_chat_text_) {
                lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_CLIP);
                lv_label_set_text(chat_line_, "zZz...");
            }
        }
    }

    // 2. Thức giấc (Waking up)
    if (face_mode_ == FaceMode::kWakingUp) {
        if (animation_tick_ >= wake_up_until_tick_) {
            SetFaceMode(FaceMode::kListening);
        }
    }

    // 3. Logic Chớp mắt (Blink) - Đang ngủ thì KHÔNG chớp mắt
    bool blink = false;
    if (face_mode_ != FaceMode::kSleeping) {
        if (animation_tick_ >= next_blink_tick_) {
            blink = true;
            blink_until_tick_ = animation_tick_ + 3; // chớp mắt 3 ticks = 120ms
            next_blink_tick_ = animation_tick_ + 45 + (NextRandom() % 60);
        } else if (animation_tick_ < blink_until_tick_) {
            blink = true;
        }
    }

    // 4. Hướng nhìn (Look Target)
    if (face_mode_ == FaceMode::kSleeping) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    } else if (face_mode_ == FaceMode::kWakingUp) {
        target_look_x_ = 0;
        target_look_y_ = -1;
    } else if (face_mode_ == FaceMode::kNeutral) {
        if (animation_tick_ >= next_idle_tick_) {
            target_look_x_ = static_cast<int>(NextRandom() % (max_look_x * 2 + 1)) - max_look_x;
            target_look_y_ = static_cast<int>(NextRandom() % (max_look_y * 2 + 1)) - max_look_y;
            next_idle_tick_ = animation_tick_ + 30 + (NextRandom() % 40);
        }
    } else if (face_mode_ == FaceMode::kThinking) {
        target_look_x_ = 4;
        target_look_y_ = -1;
    } else {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    // 5. Chiều cao mắt (Eye Height)
    int target_height = eye_height;
    if (face_mode_ == FaceMode::kSleeping) {
        target_height = 2; // Nhắm tịt mắt nằm ngủ tĩnh lặng
    } else if (blink) {
        target_height = 2; // Chớp mắt
    } else if (face_mode_ == FaceMode::kWakingUp || face_mode_ == FaceMode::kSurprised) {
        target_height = eye_height;
    } else if (face_mode_ == FaceMode::kListening) {
        target_height = eye_height;
    } else if (face_mode_ == FaceMode::kSleepy) {
        target_height = 6; // Lim dim buồn ngủ
    }
    current_eye_height_ = Ease(current_eye_height_, target_height);

    // 6. Hiệu ứng rung nhẹ khi nói / suy nghĩ
    int animation_x = 0;
    int animation_y = 0;
    if (face_mode_ == FaceMode::kSpeaking) {
        animation_y = ((animation_tick_ % 4) < 2) ? 1 : 0;
    } else if (face_mode_ == FaceMode::kThinking) {
        animation_x = ((animation_tick_ % 6) < 3) ? 1 : 0;
    }

    const int left_x = base_x + current_look_x_ + animation_x;
    const int right_x = base_x + eye_width + eye_gap + current_look_x_ + animation_x;
    
    // Tọa độ y của mắt: Giới hạn tuyệt đối để không bao giờ chạm dải vàng (y < 16) và dòng chữ (y > 49)
    int eye_y = base_y + (eye_height - current_eye_height_) / 2 + current_look_y_ + animation_y;
    if (eye_y < 16) {
        eye_y = 16;
    }
    if (eye_y + current_eye_height_ > 50) {
        eye_y = 50 - current_eye_height_;
    }

    // Radius tạo độ cong tròn xoe (khi mắt mở to radius = 16 trên chiều cao 32 => 2 đầu tròn cong hoàn hảo)
    int current_radius = std::min(16, current_eye_height_ / 2);
    lv_obj_set_style_radius(left_eye_, current_radius, 0);
    lv_obj_set_style_radius(right_eye_, current_radius, 0);

    lv_obj_set_pos(left_eye_, left_x, eye_y);
    lv_obj_set_size(left_eye_, eye_width, current_eye_height_);
    lv_obj_set_pos(right_eye_, right_x, eye_y);
    lv_obj_set_size(right_eye_, eye_width, current_eye_height_);

    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    // 7. Mí mắt biểu cảm (Angry / Sad / Happy / Speaking)
    constexpr int lid_height = 10;
    if (face_mode_ != FaceMode::kSleeping && !blink && (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kSad)) {
        const bool angry = face_mode_ == FaceMode::kAngry;
        const int lid_width = eye_width + 8;
        int lid_y = eye_y - 2;
        if (lid_y < 16) lid_y = 16;
        lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_top_lid_, left_x - 4, lid_y);
        lv_obj_set_size(left_top_lid_, lid_width, lid_height);
        lv_obj_set_pos(right_top_lid_, right_x - 4, lid_y);
        lv_obj_set_size(right_top_lid_, lid_width, lid_height);
        lv_obj_set_style_transform_rotation(left_top_lid_, angry ? -140 : 140, 0);
        lv_obj_set_style_transform_rotation(right_top_lid_, angry ? 140 : -140, 0);
    }

    if (face_mode_ != FaceMode::kSleeping && !blink && (face_mode_ == FaceMode::kHappy || face_mode_ == FaceMode::kSpeaking)) {
        constexpr int cover_height = 12;
        int lid_y = eye_y + current_eye_height_ - cover_height / 2;
        if (lid_y + cover_height > 50) lid_y = 50 - cover_height;
        lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_bottom_lid_, left_x - 2, lid_y);
        lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
        lv_obj_set_pos(right_bottom_lid_, right_x - 2, lid_y);
        lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
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
    if (Contains(emotion, "happy") || Contains(emotion, "laugh") || Contains(emotion, "lov") ||
        Contains(emotion, "wink") || Contains(emotion, "confident")) {
        return FaceMode::kHappy;
    }
    if (Contains(emotion, "sad") || Contains(emotion, "cry") || Contains(emotion, "sorrow")) {
        return FaceMode::kSad;
    }
    if (Contains(emotion, "angry") || Contains(emotion, "rage") || Contains(emotion, "annoy")) {
        return FaceMode::kAngry;
    }
    if (Contains(emotion, "sleep") || Contains(emotion, "tired") || Contains(emotion, "yawn")) {
        return FaceMode::kSleeping;
    }
    if (Contains(emotion, "think") || Contains(emotion, "confused")) {
        return FaceMode::kThinking;
    }
    if (Contains(emotion, "surpris") || Contains(emotion, "shock")) {
        return FaceMode::kSurprised;
    }
    return FaceMode::kNeutral;
}

uint32_t PocketOledDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}
