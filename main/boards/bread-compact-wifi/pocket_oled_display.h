#ifndef POCKET_OLED_DISPLAY_H
#define POCKET_OLED_DISPLAY_H

#include "display/oled_display.h"

#include <lvgl.h>

class PocketOledDisplay : public OledDisplay {
public:
    PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, bool mirror_x, bool mirror_y);
    ~PocketOledDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void RemoveChatMessage(const char* role, const char* content) override;

private:
    enum class FaceMode {
        kNeutral,
        kListening,
        kThinking,
        kSmileGentle,
        kPlayfulWink,
        kEcstaticLaugh,
        kSadEmpathy,
        kSadCrying,
        kAngry,
        kSurprised,
        kSkeptical,
        kShy,
        kSleepy,
        kSleeping,
        kWakingUp,
    };

    lv_obj_t* face_layer_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;
    lv_obj_t* left_catchlight_ = nullptr;
    lv_obj_t* right_catchlight_ = nullptr;
    lv_obj_t* left_top_lid_ = nullptr;
    lv_obj_t* right_top_lid_ = nullptr;
    lv_obj_t* left_bottom_lid_ = nullptr;
    lv_obj_t* right_bottom_lid_ = nullptr;
    lv_obj_t* left_crescent_mask_ = nullptr;
    lv_obj_t* right_crescent_mask_ = nullptr;
    lv_obj_t* teardrop_ = nullptr;
    lv_obj_t* left_blush_ = nullptr;
    lv_obj_t* right_blush_ = nullptr;
    lv_obj_t* chat_line_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;

    FaceMode face_mode_ = FaceMode::kNeutral;
    FaceMode emotion_mode_ = FaceMode::kNeutral;
    bool is_speaking_ = false;
    uint32_t animation_tick_ = 0;
    uint32_t last_active_tick_ = 0;
    uint32_t wake_up_until_tick_ = 0;
    uint32_t temporary_emotion_until_tick_ = 0;
    uint32_t random_state_ = 0x57414C4C;
    uint32_t next_blink_tick_ = 45;
    uint32_t blink_until_tick_ = 0;
    uint32_t next_idle_tick_ = 30;
    uint32_t next_quirk_tick_ = 160;

    int current_look_x_ = 0;
    int current_look_y_ = 0;
    int target_look_x_ = 0;
    int target_look_y_ = 0;
    int current_left_height_ = 36;
    int current_right_height_ = 36;
    int current_left_y_offset_ = 0;
    int current_right_y_offset_ = 0;
    bool has_chat_text_ = false;

    static void AnimationTimerCallback(lv_timer_t* timer);
    void UpdateAnimationFrame();
    void ApplyLayout();
    void SetFaceMode(FaceMode mode);
    FaceMode FaceModeFromEmotion(const char* emotion) const;
    uint32_t NextRandom();
    void WakeUp();
};

#endif  // POCKET_OLED_DISPLAY_H
