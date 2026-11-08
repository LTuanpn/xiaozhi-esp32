#ifndef POCKET_LCD_DISPLAY_H
#define POCKET_LCD_DISPLAY_H

#include "display/lcd_display.h"

#include <lvgl.h>
#include <string>

class PocketLcdDisplay : public SpiLcdDisplay {
public:
    PocketLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);
    ~PocketLcdDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void RemoveChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void SetHideSubtitle(bool hide) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowQrCode(const std::string& text, const std::string& title = "", int duration_ms = 30000) override;
    void HideQrCode() override;

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
        kStealthNinja,
        kGameOver,
        kPartyCelebrate,
        kTongueOut,
        // Group 6: Elemental & Powers
        kThunderShock,
        kRainyGloomy,
        kSunnyDazzle,
        kSleepwalking,
        kAngelBlessing,
        kFireFury,
        kCryingStream,
        kHeartbroken,
        // Group 7: Anime Special FX (GIF-like Animations)
        kAnimeStarlight,
        kSharingan,
    };

    enum class IdleQuirk {
        kNone,
        kCuriousHeadTilt,
        kSaccadeLeft,
        kSaccadeRight,
        kSquintFocus,
        kBugTracking,
        kWideLookAround,
        kLazyHalfBlink,
    };

    void UpdateAnimationFrame();
    void SetFaceMode(FaceMode mode);
    FaceMode FaceModeFromEmotion(const char* emotion) const;
    void ApplyLayout();
    void WakeUp();
    uint32_t NextRandom();

    lv_obj_t* face_layer_ = nullptr;
    lv_obj_t* chat_line_ = nullptr;

    // Eye elements
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;

    // Dynamic 3-tier catchlights
    lv_obj_t* catchlight_[2][4] = {
        {nullptr, nullptr, nullptr, nullptr},
        {nullptr, nullptr, nullptr, nullptr}
    };

    // Accessories & Visual Elements
    lv_obj_t* left_sparkle_a_ = nullptr;
    lv_obj_t* left_sparkle_b_ = nullptr;
    lv_obj_t* right_sparkle_a_ = nullptr;
    lv_obj_t* right_sparkle_b_ = nullptr;
    lv_obj_t* laser_line_ = nullptr;
    lv_obj_t* smoke_puff_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* music_note_[2] = {nullptr, nullptr};
    lv_obj_t* sweat_drop_ = nullptr;
    lv_obj_t* extra_shape_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    FaceMode face_mode_ = FaceMode::kWakingUp;
    FaceMode emotion_mode_ = FaceMode::kNeutral;
    IdleQuirk idle_quirk_ = IdleQuirk::kNone;

    esp_timer_handle_t frame_timer_ = nullptr;
    uint32_t animation_tick_ = 0;
    uint32_t last_active_tick_ = 0;
    uint32_t surprised_start_tick_ = 0;
    uint32_t notification_until_tick_ = 0;
    uint32_t temporary_emotion_until_tick_ = 0;
    uint32_t next_blink_tick_ = 40;
    uint32_t next_idle_tick_ = 30;

    // Smoothed pupil size (left=0, right=1)
    int smooth_pupil_w_[2] = {28, 28};
    int smooth_pupil_h_[2] = {32, 32};
    // Micro-saccades
    int micro_look_x_ = 0;
    int micro_look_y_ = 0;
    uint32_t next_micro_saccade_tick_ = 10;
    int current_look_x_ = 0;
    int current_look_y_ = 0;
    int target_look_x_ = 0;
    int target_look_y_ = 0;
    int current_left_width_ = 76;
    int current_right_width_ = 76;
    int current_left_height_ = 72;
    int current_right_height_ = 72;
    int current_left_y_offset_ = 0;
    int current_right_y_offset_ = 0;
    bool has_chat_text_ = false;
    std::string last_status_text_;
    uint32_t random_state_ = 0x12345678;

    // QR Code UI elements
    lv_obj_t* qr_container_ = nullptr;
    lv_obj_t* qr_canvas_ = nullptr;
    lv_obj_t* qr_title_label_ = nullptr;
    lv_obj_t* qr_url_label_ = nullptr;
    uint32_t qr_until_tick_ = 0;
};

#endif  // POCKET_LCD_DISPLAY_H
