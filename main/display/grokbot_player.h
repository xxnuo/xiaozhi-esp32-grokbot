#pragma once

#include <cstddef>
#include <cstdint>

enum class GrokBotFrameAction { kPresent, kDecode, kRebuild };

inline GrokBotFrameAction GrokBotPlanFrame(int packed_index, int target, bool reverse,
                                           bool source_delta) {
    if (packed_index == target) {
        return GrokBotFrameAction::kPresent;
    }
    if (reverse ? packed_index == target + 1 && source_delta
                : !source_delta || (packed_index >= 0 && packed_index == target - 1)) {
        return GrokBotFrameAction::kDecode;
    }
    return GrokBotFrameAction::kRebuild;
}

inline int GrokBotFrameUpper(int frame_count, uint16_t max_frames) {
    return (max_frames && max_frames < frame_count ? max_frames : frame_count) - 1;
}

inline int GrokBotFrameLower(int start_frame, int upper) {
    return start_frame < upper ? start_frame : upper;
}

struct GrokBotFrameStep {
    int index;
    bool reverse;
    bool cycle_boundary = false;
    bool once_done = false;
};

inline GrokBotFrameStep GrokBotNextFrame(int target, bool reverse, int lower, int upper, bool loop,
                                         bool forward_only) {
    if (forward_only && target >= upper) {
        return {target, reverse, false, true};
    }
    if (upper == lower || (reverse && target <= lower)) {
        return {lower < upper ? lower + 1 : lower, false, loop, !loop};
    }
    if (!reverse && target >= upper) {
        return {target - 1, true, loop, false};
    }
    return {target + (reverse ? -1 : 1), reverse};
}

class GrokBotPlayer {
public:
    static constexpr int kMaxClips = 64;

    struct FrameInfo {
        const uint8_t* data = nullptr;
        uint32_t compressed_size = 0;
        bool delta = false;
    };

    bool init(const uint8_t* data, size_t size);
    bool select(int index);
    bool select_name(const char* name);
    void restart();
    bool seek(int frame_index);
    bool advance();
    bool retreat();

    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    int clip_count() const { return clip_count_; }
    int selected_index() const { return selected_; }
    int selected_frame_count() const;
    int current_index() const { return current_index_; }
    int start_frame() const;
    size_t packed_frame_size() const { return packed_frame_size_; }
    bool frame_info(int frame_index, FrameInfo* frame) const;
    bool current_frame(FrameInfo* frame) const;
    const char* clip_name(int index) const;
    const char* selected_name() const;

private:
    struct ClipInfo {
        const char* name = nullptr;
        uint16_t frames = 0;
        uint16_t start_frame = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t packed_frame_size = 0;
    };

    const uint8_t* data_ = nullptr;
    size_t data_size_ = 0;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int clip_count_ = 0;
    int selected_ = -1;
    int current_index_ = 0;
    size_t packed_frame_size_ = 0;
    ClipInfo clips_[kMaxClips] = {};
};
