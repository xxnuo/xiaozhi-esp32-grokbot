#include "grokbot_display.h"

#include "assets.h"
#include "assets/lang_config.h"
#include "lvgl_theme.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <miniz.h>

namespace {
constexpr const char* TAG = "GrokbotDisplay";
constexpr const char* kAtlasName = "grokbot.atlas";
constexpr int kRoundEdgeInset = 56;
constexpr int kRoundStatusTop = 56;
constexpr int kRoundChatBottom = 66;
constexpr int kNormalFps = 30;
constexpr int kPowerSaveFps = 5;
constexpr uint32_t kPanelTeHz = 60;

GrokBotDirector::StatusId MapStatus(const char* status) {
    if (status == nullptr) {
        return GrokBotDirector::StatusId::kIdle;
    }
    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        return GrokBotDirector::StatusId::kIdle;
    }
    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        return GrokBotDirector::StatusId::kListening;
    }
    if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        return GrokBotDirector::StatusId::kStreaming;
    }
    if (strcmp(status, Lang::Strings::CONNECTING) == 0 ||
        strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0 ||
        strcmp(status, Lang::Strings::INITIALIZING) == 0 ||
        strcmp(status, Lang::Strings::LOADING_ASSETS) == 0) {
        return GrokBotDirector::StatusId::kLoading;
    }
    if (strcmp(status, Lang::Strings::SCANNING_WIFI) == 0) {
        return GrokBotDirector::StatusId::kRadar;
    }
    if (strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0 ||
        strcmp(status, Lang::Strings::SERVER_NOT_FOUND) == 0) {
        return GrokBotDirector::StatusId::kWaiting;
    }
    if (strcmp(status, Lang::Strings::DETECTING_MODULE) == 0 ||
        strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0) {
        return GrokBotDirector::StatusId::kSearching;
    }
    if (strcmp(status, Lang::Strings::ACTIVATION) == 0) {
        return GrokBotDirector::StatusId::kWaking;
    }
    if (strcmp(status, Lang::Strings::CONNECTION_SUCCESSFUL) == 0) {
        return GrokBotDirector::StatusId::kApproving;
    }
    if (strcmp(status, Lang::Strings::UPGRADING) == 0 ||
        strcmp(status, Lang::Strings::OTA_UPGRADE) == 0) {
        return GrokBotDirector::StatusId::kReceiving;
    }
    if (strcmp(status, Lang::Strings::ERROR) == 0 ||
        strcmp(status, Lang::Strings::SERVER_ERROR) == 0 ||
        strcmp(status, Lang::Strings::SERVER_TIMEOUT) == 0 ||
        strcmp(status, Lang::Strings::SERVER_NOT_CONNECTED) == 0 ||
        strcmp(status, Lang::Strings::UPGRADE_FAILED) == 0 ||
        strcmp(status, Lang::Strings::DOWNLOAD_ASSETS_FAILED) == 0) {
        return GrokBotDirector::StatusId::kFailed;
    }
    if (strcmp(status, Lang::Strings::HELLO_MY_FRIEND) == 0) {
        return GrokBotDirector::StatusId::kHappy;
    }
    if (strcmp(status, Lang::Strings::PLEASE_WAIT) == 0) {
        return GrokBotDirector::StatusId::kWaiting;
    }
    // Application::Alert() accepts a human-readable status string while the
    // animation is selected by the accompanying emotion. Do not let an unknown
    // status hide a valid emotion.
    return GrokBotDirector::StatusId::kIdle;
}
}  // namespace

GrokbotDisplay::GrokbotDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy, gpio_num_t te_pin)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    80),
      director_(static_cast<uint32_t>(esp_timer_get_time())),
      te_pin_(te_pin) {}

GrokbotDisplay::~GrokbotDisplay() {
    running_.store(false);
    if (te_isr_installed_) {
        gpio_isr_handler_remove(te_pin_);
        te_isr_installed_ = false;
    }
    if (te_semaphore_ != nullptr) {
        vSemaphoreDelete(te_semaphore_);
        te_semaphore_ = nullptr;
    }
    if (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    heap_caps_free(packed_);
    heap_caps_free(delta_);
    heap_caps_free(decompressor_);
    for (auto*& buffer : image_buffers_) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
}

void GrokbotDisplay::RounderEventCb(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    if (area == nullptr) {
        return;
    }
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

void GrokbotDisplay::InitializeTe() {
    if (te_pin_ == GPIO_NUM_NC) {
        return;
    }
    te_semaphore_ = xSemaphoreCreateBinary();
    if (te_semaphore_ == nullptr) {
        ESP_LOGW(TAG, "Failed to create TE semaphore");
        return;
    }

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << te_pin_;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_POSEDGE;
    if (gpio_config(&config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure TE GPIO %d", te_pin_);
        return;
    }

    esp_err_t isr_service = gpio_install_isr_service(0);
    if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(isr_service));
        return;
    }
    if (gpio_isr_handler_add(te_pin_, TeIsr, this) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add TE ISR for GPIO %d", te_pin_);
        return;
    }
    te_isr_installed_ = true;
    ESP_LOGI(TAG, "TE synchronization enabled on GPIO %d", te_pin_);
}

void GrokbotDisplay::TeIsr(void* arg) {
    auto* display = static_cast<GrokbotDisplay*>(arg);
    if (display->te_semaphore_ == nullptr) {
        return;
    }
    display->te_edge_count_.fetch_add(1, std::memory_order_relaxed);
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(display->te_semaphore_, &higher_priority_task_woken);
    if (higher_priority_task_woken != pdFALSE) {
        portYIELD_FROM_ISR();
    }
}

bool GrokbotDisplay::WaitForTeFrame(int fps) {
    if (!te_isr_installed_ || te_semaphore_ == nullptr) {
        return true;
    }

    const int64_t start_us = NowUs();
    uint32_t edges_per_frame =
        (kPanelTeHz + static_cast<uint32_t>(fps) / 2) / static_cast<uint32_t>(fps);
    edges_per_frame = std::max<uint32_t>(1, edges_per_frame);
    const uint32_t target_edge = last_te_present_count_ + edges_per_frame;
    uint32_t current_edge = te_edge_count_.load(std::memory_order_acquire);

    while (current_edge < target_edge) {
        if (xSemaphoreTake(te_semaphore_, pdMS_TO_TICKS(25)) != pdTRUE) {
            te_timeout_count_.fetch_add(1, std::memory_order_relaxed);
            te_wait_us_total_.fetch_add(NowUs() - start_us, std::memory_order_relaxed);
            te_wait_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        current_edge = te_edge_count_.load(std::memory_order_acquire);
    }

    last_te_present_count_ = current_edge;
    te_wait_us_total_.fetch_add(NowUs() - start_us, std::memory_order_relaxed);
    te_wait_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void GrokbotDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    auto* dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark_theme != nullptr && current_theme_ != dark_theme) {
        SetTheme(dark_theme);
    }

    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }

    lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.2f, 0);
    lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.2f, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, kRoundStatusTop);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.6f);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.6f);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundChatBottom);
        lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75f);
    }
    if (top_bar_ != nullptr) {
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kRoundEdgeInset / 2);
        lv_obj_set_style_pad_top(top_bar_, kRoundEdgeInset / 4, 0);
    }

    if (emoji_box_ != nullptr) {
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_display_add_event_cb(display_, RounderEventCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    InitializeTe();

    if (!running_.exchange(true)) {
        BaseType_t created = xTaskCreatePinnedToCore(AnimationTaskEntry, "grokbot_anim", 12 * 1024,
                                                     this, 1, &task_handle_, 0);
        if (created != pdPASS) {
            running_.store(false);
            task_handle_ = nullptr;
            ESP_LOGE(TAG, "Failed to create animation task");
        }
    }
}

bool GrokbotDisplay::SetEmotion(const char* emotion) {
    const int64_t now_ms = NowUs() / 1000;
    portENTER_CRITICAL(&mailbox_mux_);
    director_.SetNow(now_ms);
    const bool accepted = director_.OnEmotion(emotion);
    portEXIT_CRITICAL(&mailbox_mux_);
    return accepted;
}

void GrokbotDisplay::SetStatus(const char* status) {
    const auto mapped = MapStatus(status);
    const bool loading_assets = status && strcmp(status, Lang::Strings::LOADING_ASSETS) == 0;
    const int64_t now_ms = NowUs() / 1000;
    portENTER_CRITICAL(&mailbox_mux_);
    director_.SetNow(now_ms);
    director_.OnStatus(mapped);
    if (loading_assets)
        atlas_refresh_required_ = true;
    assets_update_in_progress_ = loading_assets;
    portEXIT_CRITICAL(&mailbox_mux_);
    LvglDisplay::SetStatus(status);
}

void GrokbotDisplay::SetChatMessage(const char* role, const char* content) {
    if (role && strcmp(role, "user") == 0 && content && *content) {
        const int64_t now_ms = NowUs() / 1000;
        portENTER_CRITICAL(&mailbox_mux_);
        director_.SetNow(now_ms);
        director_.OnUserSpeech();
        portEXIT_CRITICAL(&mailbox_mux_);
    }
    SpiLcdDisplay::SetChatMessage(role, content);
}

void GrokbotDisplay::ShowNotification(const char* notification, int duration_ms) {
    const int64_t now_ms = NowUs() / 1000;
    const int64_t until_ms = now_ms + std::clamp(duration_ms, 0, 3000);
    const char* clip = notification && strcmp(notification, Lang::Strings::SCANNING_WIFI) == 0
                           ? "radar"
                           : "notifying";
    portENTER_CRITICAL(&mailbox_mux_);
    director_.SetNow(now_ms);
    director_.OnNotification(until_ms, clip);
    portEXIT_CRITICAL(&mailbox_mux_);
    LvglDisplay::ShowNotification(notification, duration_ms);
}

void GrokbotDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void GrokbotDisplay::SetPowerSaveMode(bool on) {
    const int64_t now_ms = NowUs() / 1000;
    portENTER_CRITICAL(&mailbox_mux_);
    director_.SetNow(now_ms);
    director_.OnPowerSave(on);
    power_save_ = on;
    portEXIT_CRITICAL(&mailbox_mux_);
    // The base implementation sets sleepy/neutral and would mask the director transition.
    SpiLcdDisplay::SetChatMessage("system", "");
}

int64_t GrokbotDisplay::NowUs() const { return esp_timer_get_time(); }

bool GrokbotDisplay::EnsurePlayer() {
    const int64_t now = NowUs();
    portENTER_CRITICAL(&mailbox_mux_);
    const bool force_refresh = atlas_refresh_required_;
    atlas_refresh_required_ = false;
    portEXIT_CRITICAL(&mailbox_mux_);
    if (force_refresh) {
        player_ready_ = false;
        atlas_data_ = nullptr;
        atlas_size_ = 0;
        has_selected_ = false;
    }
    if (!force_refresh && player_ready_ && now < next_atlas_check_us_) {
        return true;
    }
    if (!force_refresh && !player_ready_ && now < next_atlas_check_us_) {
        return false;
    }
    next_atlas_check_us_ = now + 1000000;

    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(kAtlasName, ptr, size)) {
        player_ready_ = false;
        atlas_data_ = nullptr;
        atlas_size_ = 0;
        has_selected_ = false;
        ESP_LOGW(TAG, "Waiting for %s in assets partition", kAtlasName);
        return false;
    }

    const auto* data = static_cast<const uint8_t*>(ptr);
    if (force_refresh || !player_ready_ || data != atlas_data_ || size != atlas_size_) {
        player_ready_ = false;
        atlas_data_ = nullptr;
        atlas_size_ = 0;
        has_selected_ = false;
        if (!player_.init(data, size)) {
            return false;
        }
        const char* names[GrokBotPlayer::kMaxClips];
        for (int i = 0; i < player_.clip_count(); ++i)
            names[i] = player_.clip_name(i);
        const uint64_t available = GrokBotDirector::AvailabilityMask(names, player_.clip_count());
        portENTER_CRITICAL(&mailbox_mux_);
        director_.SetAvailable(available);
        portEXIT_CRITICAL(&mailbox_mux_);
        const int new_packed_size = player_.width() * player_.height() / 2;
        const size_t new_image_size = static_cast<size_t>(player_.width()) * player_.height() * 2;
        if (new_packed_size <= 0 || new_image_size == 0) {
            player_ready_ = false;
            return false;
        }

        if (frame_width_ != player_.width() || frame_height_ != player_.height() ||
            packed_ == nullptr || delta_ == nullptr || decompressor_ == nullptr ||
            image_buffers_[0] == nullptr || image_buffers_[1] == nullptr) {
            heap_caps_free(packed_);
            heap_caps_free(delta_);
            heap_caps_free(decompressor_);
            for (auto*& buffer : image_buffers_) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
            packed_ = nullptr;
            delta_ = nullptr;
            decompressor_ = nullptr;

            frame_width_ = player_.width();
            frame_height_ = player_.height();
            packed_size_ = static_cast<size_t>(new_packed_size);
            image_size_ = new_image_size;
            packed_ = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(64, packed_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            delta_ = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(64, packed_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            decompressor_ = static_cast<tinfl_decompressor*>(heap_caps_malloc(
                sizeof(tinfl_decompressor), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (decompressor_ == nullptr) {
                decompressor_ = static_cast<tinfl_decompressor*>(heap_caps_malloc(
                    sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
            image_buffers_[0] = static_cast<uint16_t*>(
                heap_caps_aligned_alloc(64, image_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            image_buffers_[1] = static_cast<uint16_t*>(
                heap_caps_aligned_alloc(64, image_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (packed_ == nullptr || delta_ == nullptr || decompressor_ == nullptr ||
                image_buffers_[0] == nullptr || image_buffers_[1] == nullptr) {
                ESP_LOGE(TAG,
                         "PSRAM allocation failed: packed=%p delta=%p decompressor=%p images=%p/%p",
                         packed_, delta_, decompressor_, image_buffers_[0], image_buffers_[1]);
                heap_caps_free(packed_);
                heap_caps_free(delta_);
                heap_caps_free(decompressor_);
                heap_caps_free(image_buffers_[0]);
                heap_caps_free(image_buffers_[1]);
                packed_ = nullptr;
                delta_ = nullptr;
                decompressor_ = nullptr;
                image_buffers_[0] = nullptr;
                image_buffers_[1] = nullptr;
                frame_width_ = 0;
                frame_height_ = 0;
                packed_size_ = 0;
                image_size_ = 0;
                player_ready_ = false;
                return false;
            }
            memset(packed_, 0, packed_size_);
            memset(delta_, 0, packed_size_);
            memset(image_buffers_[0], 0, image_size_);
            memset(image_buffers_[1], 0, image_size_);

            uint16_t gray_lut[16] = {};
            for (int i = 0; i < 16; ++i) {
                const uint8_t value = static_cast<uint8_t>(i * 17);
                gray_lut[i] = static_cast<uint16_t>(((value & 0xF8) << 8) | ((value & 0xFC) << 3) |
                                                    (value >> 3));
            }
            for (int i = 0; i < 256; ++i) {
                const uint16_t low = gray_lut[i & 0x0F];
                const uint16_t high = gray_lut[i >> 4];
                pair_lut_[i] = static_cast<uint32_t>(high) | (static_cast<uint32_t>(low) << 16);
            }

            for (int i = 0; i < 2; ++i) {
                image_descs_[i] = {};
                image_descs_[i].header.magic = LV_IMAGE_HEADER_MAGIC;
                image_descs_[i].header.cf = LV_COLOR_FORMAT_RGB565;
                image_descs_[i].header.w = frame_width_;
                image_descs_[i].header.h = frame_height_;
                image_descs_[i].header.stride = frame_width_ * sizeof(uint16_t);
                image_descs_[i].data_size = image_size_;
                image_descs_[i].data = reinterpret_cast<const uint8_t*>(image_buffers_[i]);
            }
        }
        atlas_data_ = data;
        atlas_size_ = size;
        player_ready_ = true;
        ESP_LOGI(TAG, "Atlas ready: %dx%d, %d states, %d fps, packed=%u image=%u", frame_width_,
                 frame_height_, player_.clip_count(), player_.fps(),
                 static_cast<unsigned>(packed_size_), static_cast<unsigned>(image_size_));
        ESP_LOGI(TAG, "Heap after PSRAM buffers: internal=%u, psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    return player_ready_;
}

bool GrokbotDisplay::DecodeFrame(const GrokBotPlayer::FrameInfo& frame, bool expand) {
    if (frame.data == nullptr || frame.compressed_size == 0 || packed_ == nullptr ||
        delta_ == nullptr || decompressor_ == nullptr) {
        return false;
    }
    const int64_t start_us = NowUs();
    tinfl_init(decompressor_);
    size_t input_size = frame.compressed_size;
    size_t output_size = packed_size_;
    const tinfl_status status =
        tinfl_decompress(decompressor_, frame.data, &input_size, delta_, delta_, &output_size,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || output_size != packed_size_ ||
        input_size != frame.compressed_size) {
        ESP_LOGE(TAG, "Inflate failed: status=%d in=%u/%u out=%u/%u", status,
                 static_cast<unsigned>(input_size), static_cast<unsigned>(frame.compressed_size),
                 static_cast<unsigned>(output_size), static_cast<unsigned>(packed_size_));
        return false;
    }
    if (!frame.delta) {
        memcpy(packed_, delta_, packed_size_);
    } else {
        auto* packed_words = reinterpret_cast<uint32_t*>(packed_);
        auto* delta_words = reinterpret_cast<uint32_t*>(delta_);
        const size_t word_count = packed_size_ / sizeof(uint32_t);
        for (size_t i = 0; i < word_count; ++i) {
            packed_words[i] ^= delta_words[i];
        }
        for (size_t i = word_count * sizeof(uint32_t); i < packed_size_; ++i) {
            packed_[i] ^= delta_[i];
        }
    }
    if (expand) {
        ExpandPackedFrame();
    }
    decode_us_total_ += NowUs() - start_us;
    return true;
}

void GrokbotDisplay::ExpandPackedFrame() {
    auto* pixels = image_buffers_[write_buffer_];
    if (pixels == nullptr || packed_ == nullptr) {
        return;
    }
    auto* pixels32 = reinterpret_cast<uint32_t*>(pixels);
    const size_t pixel_pairs = packed_size_;
    for (size_t i = 0; i < pixel_pairs; ++i) {
        pixels32[i] = pair_lut_[packed_[i]];
    }
}

bool GrokbotDisplay::PresentFrame() {
    if (emoji_image_ == nullptr || image_buffers_[write_buffer_] == nullptr) {
        return false;
    }
    WaitForTeFrame(std::max(1, target_fps_.load()));
    const int64_t start_us = NowUs();
    const int presented = write_buffer_;
    DisplayLockGuard lock(this);
    if (!lock) {
        return false;
    }
    write_buffer_ ^= 1;
    lv_image_set_src(emoji_image_, &image_descs_[presented]);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    present_us_total_ += NowUs() - start_us;
    return true;
}

bool GrokbotDisplay::RebuildCurrentFrame() {
    const int target = player_.current_index();
    GrokBotPlayer::FrameInfo frame = {};
    int key = target;
    while (key > 0) {
        if (!player_.frame_info(key, &frame))
            return false;
        if (!frame.delta)
            break;
        --key;
    }
    packed_index_ = -1;
    for (int i = key; i <= target; ++i) {
        if (!player_.frame_info(i, &frame) || (i == key && frame.delta) ||
            !DecodeFrame(frame, i == target))
            return false;
        packed_index_ = i;
    }
    return PresentFrame();
}

void GrokbotDisplay::AnimationTaskEntry(void* arg) {
    static_cast<GrokbotDisplay*>(arg)->AnimationTask();
    vTaskDelete(nullptr);
}

void GrokbotDisplay::AnimationTask() {
    int64_t next_frame_us = NowUs();
    int64_t report_start_us = next_frame_us;
    int64_t render_total_us = 0;
    uint32_t last_report_te_edges = te_edge_count_.load(std::memory_order_relaxed);
    uint32_t rendered_frames = 0;
    GrokBotDirector::Directive directive{nullptr, GrokBotDirector::PlayMode::kLoopPingPong, 0, 0};
    GrokBotDirector::ClipEvent pending_event = GrokBotDirector::ClipEvent::kCycleBoundary;
    bool event_pending = false;
    bool once_done = false;
    while (running_.load()) {
        portENTER_CRITICAL(&mailbox_mux_);
        bool paused = assets_update_in_progress_;
        portEXIT_CRITICAL(&mailbox_mux_);
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!EnsurePlayer()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        const int64_t now_ms = NowUs() / 1000;
        portENTER_CRITICAL(&mailbox_mux_);
        paused = assets_update_in_progress_;
        director_.SetNow(now_ms);
        if (!paused && has_selected_ && event_pending)
            director_.OnClipEvent(pending_event);
        const auto next = paused ? directive : director_.Step(now_ms);
        const bool power_save = power_save_;
        portEXIT_CRITICAL(&mailbox_mux_);
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        event_pending = false;
        if (!has_selected_ || next.seq != directive.seq) {
            if (!next.clip || !player_.select_name(next.clip)) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            directive = next;
            const int upper =
                GrokBotFrameUpper(player_.selected_frame_count(), directive.max_frames);
            player_.seek(GrokBotFrameLower(player_.start_frame(), upper));
            reverse_ = false;
            has_selected_ = true;
            packed_index_ = -1;
            once_done = false;
            ESP_LOGI(TAG, "Animation -> %s", directive.clip);
        }
        const int fps =
            power_save && strcmp(directive.clip, "sleeping") == 0 ? kPowerSaveFps : kNormalFps;
        target_fps_.store(fps);
        const int64_t frame_us = 1000000LL / fps;
        if (once_done) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int64_t render_start_us = NowUs();
        const int target = player_.current_index();
        GrokBotPlayer::FrameInfo frame = {};
        // D[i] reverses P[i] to P[i-1]; a reverse key frame needs reconstruction.
        const int source = reverse_ ? packed_index_ : target;
        const bool source_valid = player_.frame_info(source, &frame);
        auto action = GrokBotPlanFrame(packed_index_, target, reverse_, frame.delta);
        if (action != GrokBotFrameAction::kPresent && !source_valid)
            action = GrokBotFrameAction::kRebuild;
        bool rendered = false;
        switch (action) {
            case GrokBotFrameAction::kPresent:
                ExpandPackedFrame();
                rendered = PresentFrame();
                break;
            case GrokBotFrameAction::kDecode:
                if (DecodeFrame(frame, true)) {
                    packed_index_ = target;
                    rendered = PresentFrame();
                }
                break;
            case GrokBotFrameAction::kRebuild:
                rendered = RebuildCurrentFrame();
                break;
        }
        if (!rendered) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const int upper = GrokBotFrameUpper(player_.selected_frame_count(), directive.max_frames);
        const int lower = GrokBotFrameLower(player_.start_frame(), upper);
        const auto step =
            GrokBotNextFrame(target, reverse_, lower, upper,
                             directive.mode == GrokBotDirector::PlayMode::kLoopPingPong,
                             directive.mode == GrokBotDirector::PlayMode::kOnceForward);
        if (step.cycle_boundary || step.once_done) {
            pending_event = step.once_done ? GrokBotDirector::ClipEvent::kOnceDone
                                           : GrokBotDirector::ClipEvent::kCycleBoundary;
            event_pending = true;
        }
        once_done = step.once_done;
        reverse_ = step.reverse;
        player_.seek(step.index);
        render_total_us += NowUs() - render_start_us;
        ++rendered_frames;

        next_frame_us += frame_us;
        const int64_t now = NowUs();
        if (next_frame_us > now) {
            const int64_t wait_us = next_frame_us - now;
            if (wait_us >= 2000) {
                vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
            } else {
                taskYIELD();
            }
        } else if (now - next_frame_us > frame_us) {
            next_frame_us = now;
        }

        if (now - report_start_us >= 5000000) {
            const int64_t elapsed_us = now - report_start_us;
            const uint32_t fps_hundredths = static_cast<uint32_t>(
                (static_cast<int64_t>(rendered_frames) * 100000000 + elapsed_us / 2) / elapsed_us);
            const uint32_t avg_render_us =
                rendered_frames > 0 ? static_cast<uint32_t>(render_total_us / rendered_frames) : 0;
            const uint32_t avg_decode_us =
                rendered_frames > 0 ? static_cast<uint32_t>(decode_us_total_ / rendered_frames) : 0;
            const uint32_t avg_present_us =
                rendered_frames > 0 ? static_cast<uint32_t>(present_us_total_ / rendered_frames)
                                    : 0;
            const uint32_t current_te_edges = te_edge_count_.load(std::memory_order_relaxed);
            const uint32_t te_edges = current_te_edges - last_report_te_edges;
            last_report_te_edges = current_te_edges;
            const uint32_t te_timeouts = te_timeout_count_.exchange(0, std::memory_order_relaxed);
            const uint32_t te_waits = te_wait_count_.exchange(0, std::memory_order_relaxed);
            const int64_t te_wait_us_total =
                te_wait_us_total_.exchange(0, std::memory_order_relaxed);
            const uint32_t avg_te_wait_us =
                te_waits > 0 ? static_cast<uint32_t>(te_wait_us_total / te_waits) : 0;
            ESP_LOGI(TAG,
                     "Animation fps=%u.%02u render=%uus decode=%uus present=%uus "
                     "te_wait=%uus te_hz=%u te_timeout=%u state=%s",
                     fps_hundredths / 100, fps_hundredths % 100,
                     static_cast<unsigned>(avg_render_us), static_cast<unsigned>(avg_decode_us),
                     static_cast<unsigned>(avg_present_us), static_cast<unsigned>(avg_te_wait_us),
                     static_cast<unsigned>(
                         (static_cast<uint64_t>(te_edges) * 1000000 + elapsed_us / 2) / elapsed_us),
                     static_cast<unsigned>(te_timeouts), player_.selected_name());
            report_start_us = now;
            render_total_us = 0;
            decode_us_total_ = 0;
            present_us_total_ = 0;
            rendered_frames = 0;
        }
    }
}
