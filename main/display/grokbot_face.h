#pragma once

#include <cstddef>
#include <cstdint>

// Ordinary sphere faces share one geometry renderer on the device and in host previews.
class GrokBotFace {
public:
    struct Eye {
        float off[2], tilt, wid, lid_y, oval[2], path[18];
        float oval_mix = 0;
    };
    struct Pose {
        float position[2] = {}, rotation[3] = {}, squash[2] = {1, 1};
        float zoom = 0.6f, anchor = 0;
    };
    struct Frame {
        Pose pose;
        Eye eyes[2];
        float blink = 0;
    };

    explicit GrokBotFace(uint32_t seed = 1);
    static constexpr size_t kScratchBytes = 12 * 1024;
    static bool Supports(const char* clip);
    static bool AtlasBridgeSupported(const char* clip);
    void BeginAtlasBridge(int64_t now_ms);
    bool Select(const char* clip, uint32_t sequence, int64_t now_ms);
    Frame Sample(int64_t now_ms);
    bool Render(uint8_t* packed, int width, int height, int64_t now_ms);
    bool Render(uint8_t* packed, int width, int height, int64_t now_ms, void* scratch,
                size_t scratch_size, uint32_t* rgb_pairs = nullptr,
                const uint32_t* pair_lut = nullptr);
    static bool Rasterize(const Frame& frame, uint8_t* packed, int width, int height);
    static bool Rasterize(const Frame& frame, uint8_t* packed, int width, int height, void* scratch,
                          size_t scratch_size, uint32_t* rgb_pairs = nullptr,
                          const uint32_t* pair_lut = nullptr);
    static Frame ExpressionFrame(const char* expression);
    const char* recipe_name() const;

private:
    uint32_t Random();
    void StartRecipe(int recipe, int64_t now_ms, bool bridge);
    void SetExpression(int expression, int64_t now_ms);
    void SampleEyes(int64_t now_ms, Eye* eyes) const;
    Pose SamplePose(int64_t now_ms) const;

    const char* clip_ = nullptr;
    uint32_t sequence_ = 0, random_;
    int recipe_ = 0, expression_ = 0, variant_ = 0;
    int64_t recipe_start_ = 0, expression_start_ = 0, next_blink_ = 0, blink_start_ = -1000;
    int64_t next_gaze_ = 0, gaze_start_ = 0;
    float gaze_[2] = {}, gaze_from_[2] = {};
    Pose bridge_from_;
    Eye eye_from_[2] = {};
    bool bridge_ = false, active_ = false, atlas_bridge_ = false;
};
