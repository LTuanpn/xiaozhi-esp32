#ifndef POCKET_LCD_DISPLAY_H
#define POCKET_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "expression_pack.h"

#include <lvgl.h>
#include <span>
#include <string>
#include <string_view>

extern const lv_font_t lv_font_ultra_digits_64;

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
    static std::span<const std::string_view> ManualEmotions();
    bool SetManualEmotion(const std::string& emotion, int duration_ms = 8000);
    bool TryShowQrCode(const std::string& text, const std::string& title, int duration_ms);
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void RemoveChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void SetHideSubtitle(bool hide) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowQrCode(const std::string& text, const std::string& title = "", int duration_ms = 30000) override;
    void HideQrCode() override;
    void HideQrCode(bool force);
    void ShowStandbyClock() override;
    void ShowStandbyClock(bool manual_lock);
    void HideStandbyClock() override;
    void HideStandbyClock(bool force);
    bool ShowFace();
    void ReloadExpressions(bool load);
    std::string ExpressionDigest();
    std::string_view ActiveExpressionName() const;
    void LogFacePerformance();
    bool IsStandbyClockActive() const override { return standby_active_; }
    void UnlockBootStandby() { boot_standby_locked_ = false; manual_standby_locked_ = false; }
    bool IsBootStandbyLocked() const { return boot_standby_locked_ || manual_standby_locked_; }
    void SetPinnedMemo(const std::string& memo);
    void SetCountdownTimer(int seconds, const std::string& label = "FOCUS");
    void SetGuardMode(bool enable);
    bool IsGuardModeActive() const { return guard_mode_active_; }
    int GetCountdownRemaining() const { return countdown_remaining_sec_; }

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
    };

    enum class IdleQuirk {
        kNone,
        kBugTracking,
        kPeekABoo,
        kEyeRoll,
    };

    void UpdateAnimationFrame();
    void SetFaceMode(FaceMode mode);
    FaceMode FaceModeFromEmotion(const char* emotion) const;
    FaceMode InferEmotionFromText(const char* text) const;
    void ApplyLayout();
    void WakeUp();
    uint32_t NextRandom();

    lv_obj_t* face_layer_ = nullptr;
    lv_obj_t* speaking_mouth_ = nullptr;
    ExpressionPack expression_pack_;
    std::string expression_name_ = "neutral";
    lv_obj_t* chat_line_ = nullptr;

    // Eye elements
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_iris_ = nullptr;
    lv_obj_t* right_iris_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;

    // Dynamic Eyelids & Crescent Masks
    lv_obj_t* left_top_lid_ = nullptr;
    lv_obj_t* right_top_lid_ = nullptr;
    lv_obj_t* left_bottom_lid_ = nullptr;
    lv_obj_t* right_bottom_lid_ = nullptr;
    lv_obj_t* left_crescent_mask_ = nullptr;
    lv_obj_t* right_crescent_mask_ = nullptr;

    // Teardrops & Blush
    lv_obj_t* left_tear_ = nullptr;
    lv_obj_t* right_tear_ = nullptr;
    lv_obj_t* left_blush_ = nullptr;
    lv_obj_t* right_blush_ = nullptr;

    // FX Labels
    lv_obj_t* fx_label_a_ = nullptr;
    lv_obj_t* fx_label_b_ = nullptr;

    // Hearts, Dizzy X & Sparkles
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

    // Dynamic 3-tier catchlights
    lv_obj_t* catchlight_[2][4] = {
        {nullptr, nullptr, nullptr, nullptr},
        {nullptr, nullptr, nullptr, nullptr}
    };

    // Accessories & Visual Elements
    lv_obj_t* laser_line_ = nullptr;
    lv_obj_t* smoke_puff_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* music_note_[2] = {nullptr, nullptr};
    lv_obj_t* sweat_drop_ = nullptr;
    lv_obj_t* extra_shape_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    FaceMode face_mode_ = FaceMode::kWakingUp;
    FaceMode emotion_mode_ = FaceMode::kNeutral;
    IdleQuirk idle_quirk_ = IdleQuirk::kNone;
    bool is_speaking_ = false;

    lv_timer_t* frame_timer_ = nullptr;
    lv_timer_t* visual_timer_ = nullptr;
    static void OnDisplayFlush(lv_event_t* event);
    uint32_t face_refreshes_ = 0;
    bool expression_visible_ = false;
    bool fast_refresh_ = false;
    uint32_t visual_period_ms_ = 50;
    int mouth_width_ = 0;
    int mouth_height_ = 0;
    void UpdateVisualFrame();
    void UpdateSpeakingMouth(uint16_t motion);
    int64_t manual_emotion_until_us_ = 0;
    FaceMode manual_emotion_mode_ = FaceMode::kNeutral;
    int64_t countdown_deadline_us_ = 0;
    uint32_t animation_tick_ = 0;
    uint32_t last_active_tick_ = 0;
    uint32_t wake_up_until_tick_ = 0;
    uint32_t surprised_start_tick_ = 0;
    uint32_t notification_until_tick_ = 0;
    uint32_t temporary_emotion_until_tick_ = 0;
    uint32_t quirk_start_tick_ = 0;
    uint32_t quirk_until_tick_ = 0;
    uint32_t next_quirk_tick_ = 300;
    uint32_t next_blink_tick_ = 45;
    uint32_t blink_until_tick_ = 0;
    uint32_t second_blink_start_tick_ = 0;
    uint32_t second_blink_until_tick_ = 0;
    uint32_t next_idle_tick_ = 30;

    // Smoothed pupil size (left=0, right=1)
    int smooth_pupil_w_[2] = {30, 30};
    int smooth_pupil_h_[2] = {34, 34};
    // Micro-saccades
    int micro_look_x_ = 0;
    int micro_look_y_ = 0;
    uint32_t next_micro_tick_ = 40;
    uint32_t micro_until_tick_ = 0;

    int current_look_x_ = 0;
    int current_look_y_ = 0;
    int target_look_x_ = 0;
    int target_look_y_ = 0;
    int current_left_width_ = 82;
    int current_right_width_ = 82;
    int current_left_height_ = 80;
    int current_right_height_ = 80;
    int current_left_y_offset_ = 0;
    int current_right_y_offset_ = 0;
    bool has_chat_text_ = false;
    std::string last_status_text_;
    uint32_t random_state_ = 0x57414C4C;

    // QR Code UI elements
    lv_obj_t* qr_container_ = nullptr;
    lv_obj_t* qr_canvas_ = nullptr;
    uint16_t* qr_cbuf_ = nullptr;
    lv_obj_t* qr_title_label_ = nullptr;
    lv_obj_t* qr_url_label_ = nullptr;
    uint32_t qr_until_tick_ = 0;
    uint8_t saved_backlight_brightness_ = 80;
    bool qr_backlight_dimmed_ = false;

    // Standby Clock UI elements (Apple Watch Modular Ultra Bento Grid)
    void UpdateStandbyClock();
    bool standby_active_ = false;
    bool boot_standby_locked_ = true;
    bool manual_standby_locked_ = false;
    uint32_t standby_lock_until_tick_ = 0;
    uint32_t last_clock_tick_ = 0;
    lv_obj_t* standby_container_ = nullptr;

    // Top Status Bar (h: 28)
    lv_obj_t* status_ready_label_ = nullptr;
    lv_obj_t* pill_container_ = nullptr;
    lv_obj_t* pill_label_ = nullptr;
    lv_obj_t* wifi_rssi_label_ = nullptr;

    // Hero Clock & Memo Card (w: 228, h: 114)
    lv_obj_t* hero_card_ = nullptr;
    lv_obj_t* hero_accent_bar_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* sec_label_ = nullptr;
    lv_obj_t* pinned_memo_label_ = nullptr;

    // Bottom Bento 3 Widgets (h: 78)
    // Box 1: Timer
    lv_obj_t* timer_card_ = nullptr;
    lv_obj_t* timer_title_label_ = nullptr;
    lv_obj_t* timer_val_label_ = nullptr;
    lv_obj_t* timer_tag_label_ = nullptr;

    // Box 2: Volume
    lv_obj_t* vol_card_ = nullptr;
    lv_obj_t* vol_title_label_ = nullptr;
    lv_obj_t* vol_arc_ = nullptr;
    lv_obj_t* vol_val_label_ = nullptr;

    // Dynamic Emotion Timeline Queue (Multi-emotion per sentence)
    struct EmotionSegment {
        FaceMode mode;
        uint32_t duration_ticks;
    };
    std::vector<EmotionSegment> emotion_timeline_;
    size_t current_emotion_idx_ = 0;
    uint32_t next_emotion_tick_ = 0;

    // Box 3: Guard (Knight Crest Shield Canvas 28x32)
    void DrawShield(bool is_active);
    lv_obj_t* guard_card_ = nullptr;
    lv_obj_t* guard_title_label_ = nullptr;
    lv_obj_t* guard_canvas_ = nullptr;
    uint8_t guard_cbuf_[28 * 32 * 2];
    lv_obj_t* guard_status_label_ = nullptr;

    // State Variables (No Fake Sensors)
    std::string pinned_memo_ = "READY • SAY WAKE WORD";
    int countdown_remaining_sec_ = 0;
    std::string countdown_label_ = "FOCUS";
    bool guard_mode_active_ = false;
    uint32_t last_countdown_tick_ = 0;
    uint32_t guard_alarm_cooldown_tick_ = 0;
};

#endif  // POCKET_LCD_DISPLAY_H
