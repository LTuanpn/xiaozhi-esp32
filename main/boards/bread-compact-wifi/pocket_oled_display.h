#ifndef POCKET_OLED_DISPLAY_H
#define POCKET_OLED_DISPLAY_H

#include "display/oled_display.h"

#include <lvgl.h>
#include <string>

class PocketOledDisplay : public OledDisplay {
public:
    PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, bool mirror_x, bool mirror_y);
    ~PocketOledDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
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
        kLove,
        kDizzy,
        kScared,
        kExcited,
        kSmug,
        kBored,
        kFurious,
        kPout,
        kMusic,
        kScanning,
        // Group 1: Cyber & Tech
        kMatrixGlitch,
        kLoadingBuffer,
        kLowBattery,
        kOverheat,
        kRebooting,
        kTargetLocked,
        // Group 2: Personality & Sass
        kSideEye,
        kFacepalm,
        kInnocent,
        kJealous,
        kScheming,
        kDisgusted,
        kWhispering,
        // Group 3: Physical & Bodily
        kFreezing,
        kSneezing,
        kCryingStream,
        kHeadache,
        kDrunk,
        kExhausted,
        // Group 4: Mental & Spiritual
        kZenMeditation,
        kShockedJawdrop,
        kParanoid,
        kConfusedQuestion,
        kIdeaEureka,
        kHypnotized,
        // Group 5: Playful & Gaming
        kMoneyEyes,
        kHeartbroken,
        kTongueOut,
        kPartyCelebrate,
        kStealthNinja,
        kGameOver,
        // Group 6: Elemental & Powers
        kFireFury,
        kThunderShock,
        kRainyGloomy,
        kSunnyDazzle,
        kSleepwalking,
        kAngelBlessing,
    };

    enum class IdleQuirk {
        kNone,
        kBugTracking,
        kPeekABoo,
        kEyeRoll,
    };

    lv_obj_t* face_layer_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* left_eye_tall_ = nullptr;
    lv_obj_t* left_eye_mid_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* right_eye_tall_ = nullptr;
    lv_obj_t* right_eye_mid_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;
    lv_obj_t* left_top_lid_ = nullptr;
    lv_obj_t* right_top_lid_ = nullptr;
    lv_obj_t* left_bottom_lid_ = nullptr;
    lv_obj_t* right_bottom_lid_ = nullptr;
    lv_obj_t* left_crescent_mask_ = nullptr;
    lv_obj_t* right_crescent_mask_ = nullptr;
    lv_obj_t* left_tear_ = nullptr;
    lv_obj_t* right_tear_ = nullptr;
    lv_obj_t* left_blush_ = nullptr;
    lv_obj_t* right_blush_ = nullptr;
    lv_obj_t* fx_label_a_ = nullptr;
    lv_obj_t* fx_label_b_ = nullptr;
    lv_obj_t* left_heart_a_ = nullptr;
    lv_obj_t* left_heart_b_ = nullptr;
    lv_obj_t* right_heart_a_ = nullptr;
    lv_obj_t* right_heart_b_ = nullptr;
    lv_obj_t* left_dizzy_a_ = nullptr;
    lv_obj_t* left_dizzy_b_ = nullptr;
    lv_obj_t* right_dizzy_a_ = nullptr;
    lv_obj_t* right_dizzy_b_ = nullptr;
    lv_obj_t* left_sparkle_a_ = nullptr;
    lv_obj_t* left_sparkle_b_ = nullptr;
    lv_obj_t* right_sparkle_a_ = nullptr;
    lv_obj_t* right_sparkle_b_ = nullptr;
    lv_obj_t* laser_line_ = nullptr;
    lv_obj_t* smoke_puff_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* music_note_[2] = {nullptr, nullptr};
    lv_obj_t* sweat_drop_ = nullptr;
    lv_obj_t* extra_shape_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    // 3-tier pixel-matrix catchlight per eye: [eye][0]=wide, [1]=tall, [2]=mid-core, [3]=secondary glint
    lv_obj_t* catchlight_[2][4] = {{nullptr, nullptr, nullptr, nullptr},
                                   {nullptr, nullptr, nullptr, nullptr}};
    lv_obj_t* chat_line_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;

    FaceMode face_mode_ = FaceMode::kNeutral;
    FaceMode emotion_mode_ = FaceMode::kNeutral;
    IdleQuirk idle_quirk_ = IdleQuirk::kNone;
    bool is_speaking_ = false;
    uint32_t animation_tick_ = 0;
    uint32_t last_active_tick_ = 0;
    uint32_t wake_up_until_tick_ = 0;
    uint32_t temporary_emotion_until_tick_ = 0;
    uint32_t notification_until_tick_ = 0;
    uint32_t surprised_start_tick_ = 0;
    uint32_t quirk_start_tick_ = 0;
    uint32_t quirk_until_tick_ = 0;
    uint32_t next_quirk_tick_ = 300;
    uint32_t random_state_ = 0x57414C4C;
    uint32_t next_blink_tick_ = 45;
    uint32_t blink_until_tick_ = 0;
    uint32_t second_blink_start_tick_ = 0;
    uint32_t second_blink_until_tick_ = 0;
    uint32_t next_idle_tick_ = 30;

    // Smoothed pupil size (left=0, right=1)
    int smooth_pupil_w_[2] = {14, 14};
    int smooth_pupil_h_[2] = {16, 16};
    // Micro-saccades: tiny gaze jitter while idle
    int micro_look_x_ = 0;
    int micro_look_y_ = 0;
    uint32_t next_micro_tick_ = 40;
    uint32_t micro_until_tick_ = 0;

    int current_look_x_ = 0;
    int current_look_y_ = 0;
    int target_look_x_ = 0;
    int target_look_y_ = 0;
    int current_left_width_ = 40;
    int current_right_width_ = 40;
    int current_left_height_ = 38;
    int current_right_height_ = 38;
    int current_left_y_offset_ = 0;
    int current_right_y_offset_ = 0;
    bool has_chat_text_ = false;
    std::string last_status_text_;

    static void AnimationTimerCallback(lv_timer_t* timer);
    void UpdateAnimationFrame();
    void ApplyLayout();
    void SetFaceMode(FaceMode mode);
    FaceMode FaceModeFromEmotion(const char* emotion) const;
    FaceMode InferEmotionFromText(const char* text) const;
    uint32_t NextRandom();
    void WakeUp();
};

#endif  // POCKET_OLED_DISPLAY_H