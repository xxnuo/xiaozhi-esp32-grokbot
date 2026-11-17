#pragma once

#include <cmath>
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

inline int GrokBotFrameLower(int start_frame, int upper, bool from_frame_zero = false) {
    return from_frame_zero ? 0 : start_frame < upper ? start_frame : upper;
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

inline uint8_t GrokBotDisplayGray(uint8_t coverage) {
    return static_cast<uint8_t>(std::lround(255 * std::pow(coverage / 15.0, 1.0 / 2.2)));
}

constexpr unsigned kGrokBotFadeFrames = 6;

inline uint8_t GrokBotBlendGray(uint8_t from, uint8_t to, unsigned step) {
    if (step >= kGrokBotFadeFrames)
        return to;
    return (from * (kGrokBotFadeFrames - step) + to * step + kGrokBotFadeFrames / 2) /
           kGrokBotFadeFrames;
}

inline uint8_t GrokBotBlendPacked(uint8_t from, uint8_t to, const uint8_t* lut) {
    return (lut[(from & 0xf0) | (to >> 4)] << 4) | lut[((from & 15) << 4) | (to & 15)];
}

// Keep the original TE phase even after a missed deadline or counter wrap.
inline uint32_t GrokBotNextTeEdge(uint32_t previous, uint32_t current, uint32_t stride) {
    const uint32_t elapsed = current - previous;
    return previous + (elapsed / stride + 1) * stride;
}

inline void GrokBotExpandDelta(uint8_t* packed, uint8_t* delta, uint32_t* pixels, size_t size,
                               const uint32_t* lut, bool xor_delta) {
    auto* words = reinterpret_cast<uint32_t*>(packed);
    auto* deltas = reinterpret_cast<uint32_t*>(delta);
    const size_t count = size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = xor_delta ? words[i] ^ deltas[i] : deltas[i];
        words[i] = value;
        deltas[i] = value;
        pixels[4 * i] = lut[value & 255];
        pixels[4 * i + 1] = lut[(value >> 8) & 255];
        pixels[4 * i + 2] = lut[(value >> 16) & 255];
        pixels[4 * i + 3] = lut[value >> 24];
    }
    for (size_t i = count * sizeof(uint32_t); i < size; ++i) {
        packed[i] = xor_delta ? packed[i] ^ delta[i] : delta[i];
        delta[i] = packed[i];
        pixels[i] = lut[packed[i]];
    }
}

inline const uint8_t* GrokBotCachedStart(const uint8_t* cache, int selected, int target,
                                         int start_frame, size_t frame_size) {
    return cache && selected >= 0 && target == start_frame ? cache + selected * frame_size
                                                           : nullptr;
}

struct GrokBotFramePipeline {
    int write = 0;
    bool pending = false;

    int Submit() {
        if (pending)
            return -1;
        const int submitted = write;
        write ^= 1;
        pending = true;
        return submitted;
    }
    void Complete() { pending = false; }
};

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
