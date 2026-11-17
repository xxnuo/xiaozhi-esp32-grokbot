#pragma once

#include "grokbot_director.h"
#include "grokbot_face.h"
#include "grokbot_player.h"
#include "lcd_display.h"

#include <atomic>
#include <cstdint>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <miniz.h>

class GrokbotDisplay : public SpiLcdDisplay {
public:
    static constexpr uint16_t kTeScanline = 128;

    GrokbotDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy, gpio_num_t te_pin = GPIO_NUM_NC);
    ~GrokbotDisplay() override;

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    bool OnInteraction(const char* emotion);
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetPowerSaveMode(bool on) override;

private:
    static void RounderEventCb(lv_event_t* event);
    static void AnimationTaskEntry(void* arg);
    static void RefreshTaskEntry(void* arg);
    static void TeIsr(void* arg);
    static void RefreshEventCb(lv_event_t* event);
    static bool TransferDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* event,
                             void* arg);
    void AnimationTask();
    void InitializeTe();
    bool WaitForTeFrame(int fps, bool pace_animation = true);
    void CheckStripTransfer(uint32_t completed_us);

    bool EnsurePlayer();
    bool DecodeFrame(const GrokBotPlayer::FrameInfo& frame, bool expand);
    bool RebuildCurrentFrame();
    bool CacheStartFrames();
    void ExpandPackedFrame();
    bool PresentFrame();
    int64_t NowUs() const;

    portMUX_TYPE mailbox_mux_ = portMUX_INITIALIZER_UNLOCKED;
    GrokBotDirector director_;
    GrokBotFace face_;
    // Protected by the display lock, like the status label.
    const char* expression_caption_ = "Hello there!";
    bool show_expression_caption_ = false;
    int64_t face_exit_until_ = 0;
    bool assets_update_in_progress_ = false;
    bool atlas_refresh_required_ = false;
    bool power_save_ = false;
    GrokBotPlayer player_;
    const uint8_t* atlas_data_ = nullptr;
    size_t atlas_size_ = 0;
    bool player_ready_ = false;
    int64_t next_atlas_check_us_ = 0;

    uint8_t* start_frames_ = nullptr;
    int start_frame_slots_[GrokBotPlayer::kMaxClips] = {};
    uint8_t* packed_ = nullptr;
    uint8_t* delta_ = nullptr;
    tinfl_decompressor* decompressor_ = nullptr;
    uint16_t* image_buffers_[2] = {};
    lv_image_dsc_t image_descs_[2] = {};
    uint32_t pair_lut_[256] = {};
    int frame_width_ = 0;
    int frame_height_ = 0;
    size_t packed_size_ = 0;
    size_t image_size_ = 0;
    GrokBotFramePipeline pipeline_;
    SemaphoreHandle_t refresh_ready_ = nullptr;
    std::atomic<bool> refresh_pending_{false};
    bool refreshing_frame_ = false;
    bool awaiting_te_ = false;
    int refresh_fps_ = 30;
    std::atomic<uint32_t> te_timestamp_{0};
    uint16_t* retired_image_ = nullptr;
    lv_image_dsc_t retired_desc_ = {};
    uint8_t* displayed_packed_ = nullptr;
    uint8_t* fade_from_ = nullptr;
    uint8_t* blend_lut_ = nullptr;
    unsigned fade_step_ = kGrokBotFadeFrames;
    bool have_displayed_frame_ = false;
    std::atomic<int64_t> last_present_us_{0};
    uint32_t frame_intervals_[256] = {};
    unsigned interval_count_ = 0;
    unsigned slow_frames_ = 0;
    uint32_t te_lag_max_us_ = 0;
    lv_area_t strip_area_ = {};
    uint32_t strip_start_us_ = 0;
    std::atomic<uint32_t> frame_transfer_start_us_{0};
    std::atomic<uint32_t> frame_transfer_us_total_{0};
    std::atomic<uint32_t> frame_transfer_count_{0};
    std::atomic<uint32_t> frame_transfer_us_max_{0};
    bool strip_last_ = false;
    uint32_t refresh_te_us_ = 0;
    uint32_t refresh_period_us_ = 16667;
    int refresh_scan_cycle_ = 0;
    bool refresh_tear_risk_ = false;
    std::atomic<uint32_t> te_period_us_{16667};
    std::atomic<unsigned> tear_risk_{0};

    bool reverse_ = false;
    bool has_selected_ = false;
    int packed_index_ = -1;
    gpio_num_t te_pin_ = GPIO_NUM_NC;
    SemaphoreHandle_t te_semaphore_ = nullptr;
    bool te_isr_installed_ = false;
    std::atomic<uint32_t> te_timeout_count_{0};
    std::atomic<uint32_t> te_edge_count_{0};
    std::atomic<uint32_t> te_wait_count_{0};
    std::atomic<int64_t> te_wait_us_total_{0};
    uint32_t last_te_present_count_ = 0;

    std::atomic<int> target_fps_{30};
    std::atomic<bool> running_{false};
    int64_t decode_us_total_ = 0;
    int64_t face_us_total_ = 0;
    uint32_t face_us_max_ = 0;
    uint32_t face_frame_count_ = 0;
    int64_t present_us_total_ = 0;
    TaskHandle_t task_handle_ = nullptr;
    TaskHandle_t refresh_task_ = nullptr;
};
