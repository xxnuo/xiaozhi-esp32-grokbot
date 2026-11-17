#include "grokbot_player.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CHECK(expression)                                                         \
    do {                                                                          \
        if (!(expression)) {                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
            std::exit(1);                                                         \
        }                                                                         \
    } while (false)

namespace {
struct Frame {
    uint8_t value;
    bool delta;
};
struct Renderer {
    Frame frames[4] = {{0x10, false}, {0x20, true}, {0x40, true}, {0x08, true}};
    int packed_index = -1;
    uint8_t packed = 0;
    int decodes = 0;

    GrokBotFrameAction Render(int target, bool reverse) {
        const int source = reverse ? packed_index : target;
        const bool delta = source >= 0 && frames[source].delta;
        const auto action = GrokBotPlanFrame(packed_index, target, reverse, delta);
        if (action == GrokBotFrameAction::kDecode) {
            Decode(source);
        } else if (action == GrokBotFrameAction::kRebuild) {
            int key = target;
            while (key > 0 && frames[key].delta)
                --key;
            CHECK(!frames[key].delta);
            for (int i = key; i <= target; ++i)
                Decode(i);
        }
        packed_index = target;
        return action;
    }
    void Decode(int source) {
        packed = frames[source].delta ? packed ^ frames[source].value : frames[source].value;
        ++decodes;
    }
};

void SingleDeltaFrame() {
    Renderer renderer;
    CHECK(GrokBotPlanFrame(-1, 0, false, true) == GrokBotFrameAction::kRebuild);
    const int upper = GrokBotFrameUpper(3, 0);
    const int lower = GrokBotFrameLower(2, upper);
    CHECK(lower == 2 && upper == 2);
    CHECK(renderer.Render(lower, false) == GrokBotFrameAction::kRebuild);
    CHECK(renderer.packed == 0x70);
    for (int i = 0; i < 10; ++i) {
        const auto step = GrokBotNextFrame(lower, false, lower, upper, true, false);
        CHECK(step.index == 2 && !step.reverse && step.cycle_boundary && !step.once_done);
        CHECK(renderer.Render(step.index, step.reverse) == GrokBotFrameAction::kPresent);
        CHECK(renderer.packed == 0x70 && renderer.decodes == 3);
    }
}

void TwoFramePingPong() {
    Renderer renderer;
    int target = 0;
    bool reverse = false;
    for (int expected : {0x10, 0x30, 0x10, 0x30, 0x10}) {
        CHECK(renderer.Render(target, reverse) == GrokBotFrameAction::kDecode);
        CHECK(renderer.packed == expected);
        const auto step = GrokBotNextFrame(target, reverse, 0, 1, true, false);
        CHECK(step.cycle_boundary == (target == 1 || reverse));
        CHECK(!step.once_done);
        target = step.index;
        reverse = step.reverse;
    }
    const auto upper = GrokBotNextFrame(1, false, 0, 1, false, false);
    CHECK(upper.index == 0 && upper.reverse && !upper.cycle_boundary && !upper.once_done);
    const auto lower = GrokBotNextFrame(0, true, 0, 1, false, false);
    CHECK(lower.once_done && !lower.cycle_boundary);
    const auto forward = GrokBotNextFrame(1, false, 0, 1, false, true);
    CHECK(forward.once_done && forward.index == 1);
}

void MiddleKeyFrame() {
    Renderer renderer;
    renderer.frames[2] = {0x44, false};
    CHECK(renderer.Render(3, false) == GrokBotFrameAction::kRebuild);
    CHECK(renderer.packed == 0x4c && renderer.decodes == 2);
    CHECK(renderer.Render(2, true) == GrokBotFrameAction::kDecode);
    CHECK(renderer.packed == 0x44);
    CHECK(renderer.Render(1, true) == GrokBotFrameAction::kRebuild);
    CHECK(renderer.packed == 0x30);
    CHECK(renderer.Render(2, false) == GrokBotFrameAction::kDecode);
    CHECK(renderer.packed == 0x44);
    CHECK(renderer.Render(0, false) == GrokBotFrameAction::kDecode);
    CHECK(renderer.packed == 0x10);
    CHECK(renderer.Render(3, false) == GrokBotFrameAction::kRebuild);
    CHECK(renderer.packed == 0x4c);
}

void FrameLimit() {
    CHECK(GrokBotFrameLower(2, 30, true) == 0);
    const auto intro_done = GrokBotNextFrame(0, true, 0, 30, false, false);
    CHECK(intro_done.once_done && !intro_done.cycle_boundary);
    int intro_target = 0;
    bool intro_reverse = false;
    int intro_frames = 0;
    GrokBotFrameStep intro_step{};
    do {
        intro_step = GrokBotNextFrame(intro_target, intro_reverse, 0, 30, false, false);
        intro_target = intro_step.index;
        intro_reverse = intro_step.reverse;
        ++intro_frames;
    } while (!intro_step.once_done && intro_frames <= 61);
    CHECK(intro_step.once_done && intro_target == 1 && intro_frames == 61);
    CHECK(GrokBotFrameUpper(3, 10) == 2);
    const int upper = GrokBotFrameUpper(3, 2);
    const int lower = GrokBotFrameLower(2, upper);
    CHECK(upper == 1 && lower == 1);
    const auto once = GrokBotNextFrame(lower, false, lower, upper, false, true);
    CHECK(once.index == 1 && once.once_done);
    const auto loop = GrokBotNextFrame(lower, false, lower, upper, true, false);
    CHECK(loop.index == 1 && loop.cycle_boundary);
    int target = 0;
    bool reverse = false;
    for (int i = 0; i < 10; ++i) {
        const auto step = GrokBotNextFrame(target, reverse, 0, upper, true, false);
        CHECK(step.index >= 0 && step.index <= 1);
        target = step.index;
        reverse = step.reverse;
    }
}

void PresentationRetry() {
    Renderer renderer;
    renderer.Render(0, false);
    CHECK(renderer.Render(1, false) == GrokBotFrameAction::kDecode);
    const int decodes = renderer.decodes;
    // The presentation failed: keep the same target and decoded packed frame.
    CHECK(renderer.Render(1, false) == GrokBotFrameAction::kPresent);
    CHECK(renderer.decodes == decodes && renderer.packed == 0x30);
    CHECK(renderer.Render(0, true) == GrokBotFrameAction::kDecode);
    CHECK(renderer.Render(0, true) == GrokBotFrameAction::kPresent);
    CHECK(renderer.packed == 0x10);
    // A failed decode leaves the old packed index, so the same delta is retried once.
    CHECK(GrokBotPlanFrame(0, 1, false, true) == GrokBotFrameAction::kDecode);
    CHECK(GrokBotPlanFrame(0, 1, false, true) == GrokBotFrameAction::kDecode);
}
void Blend() {
    CHECK(GrokBotDisplayGray(0) == 0);
    CHECK(GrokBotDisplayGray(15) == 255);
    CHECK(GrokBotDisplayGray(7) == 180);
    for (unsigned level = 1; level < 16; ++level)
        CHECK(GrokBotDisplayGray(level) > GrokBotDisplayGray(level - 1));
    uint8_t lut[256];
    for (unsigned step = 0; step <= 6; ++step) {
        for (unsigned pair = 0; pair < 256; ++pair)
            lut[pair] = GrokBotBlendGray(pair >> 4, pair & 15, step);
        for (unsigned from = 0; from < 256; ++from)
            for (unsigned to = 0; to < 256; ++to) {
                const uint8_t mixed = GrokBotBlendPacked(from, to, lut);
                CHECK((mixed >> 4) == GrokBotBlendGray(from >> 4, to >> 4, step));
                CHECK((mixed & 15) == GrokBotBlendGray(from & 15, to & 15, step));
            }
    }
    for (uint8_t from = 0; from < 16; ++from)
        for (uint8_t to = 0; to < 16; ++to) {
            CHECK(GrokBotBlendGray(from, to, 0) == from);
            CHECK(GrokBotBlendGray(from, to, 6) == to);
            for (unsigned step = 0; step <= 6; ++step) {
                const auto mixed = GrokBotBlendGray(from, to, step);
                CHECK(mixed <= 15);
                CHECK(mixed >= (from < to ? from : to));
                CHECK(mixed <= (from > to ? from : to));
                CHECK(mixed == (from * (6 - step) + to * step + 3) / 6);
            }
        }
}

void Pipeline() {
    GrokBotFramePipeline pipeline;
    CHECK(pipeline.Submit() == 0);
    CHECK(pipeline.write == 1);      // Decode frame N+1 into the other buffer during refresh N.
    CHECK(pipeline.Submit() == -1);  // Never replace a source LVGL is still reading.
    CHECK(pipeline.write == 1);
    pipeline.Complete();
    CHECK(pipeline.Submit() == 1);
    CHECK(pipeline.write == 0);
    CHECK(GrokBotNextTeEdge(100, 101, 2) == 102);
    CHECK(GrokBotNextTeEdge(100, 103, 2) == 104);
    CHECK(GrokBotNextTeEdge(100, 105, 2) == 106);  // A late decode preserves even TE phase.
    CHECK(GrokBotNextTeEdge(100, 101, 12) == 112);
    CHECK(GrokBotNextTeEdge(0xfffffffe, 0xffffffff, 2) == 0);
    CHECK(GrokBotNextTeEdge(0xfffffffe, 1, 2) == 2);
}
void FusedExpand() {
    uint32_t lut[256];
    for (unsigned i = 0; i < 256; ++i)
        lut[i] = 0x10001 * i;
    for (size_t size = 0; size <= 9; ++size) {
        alignas(4) uint8_t packed[12];
        alignas(4) uint8_t delta[12];
        uint32_t pixels[12];
        for (unsigned i = 0; i < 12; ++i) {
            packed[i] = 0x10 + i;
            delta[i] = 0xf0 - i;
            pixels[i] = 0xdeadbeef;
        }
        GrokBotExpandDelta(packed, delta, pixels, size, lut, true);
        for (size_t i = 0; i < 12; ++i) {
            CHECK(packed[i] == (i < size ? ((0x10 + i) ^ (0xf0 - i)) : 0x10 + i));
            CHECK(pixels[i] == (i < size ? lut[packed[i]] : 0xdeadbeef));
        }
        for (size_t i = 0; i < size; ++i) {
            CHECK(delta[i] == packed[i]);
            delta[i] = 0xf0 - i;
        }
        GrokBotExpandDelta(packed, delta, pixels, size, lut, false);
        for (size_t i = 0; i < size; ++i) {
            CHECK(packed[i] == delta[i]);
            CHECK(pixels[i] == lut[delta[i]]);
        }
    }
}

void CachedSwitch() {
    const uint8_t cache[] = {0x10, 0x30, 0x70};
    Renderer renderer;
    renderer.Render(0, false);
    const int before = renderer.decodes;
    for (int clip = 0; clip < 3; ++clip) {
        const auto* frame = GrokBotCachedStart(cache, clip, 2, 2, 1);
        CHECK(frame && *frame == cache[clip]);
        renderer.packed = *frame;
        renderer.packed_index = 2;
        CHECK(renderer.Render(2, false) == GrokBotFrameAction::kPresent);
        CHECK(renderer.decodes == before);
    }
    CHECK(GrokBotCachedStart(cache, 0, 0, 2, 1) == nullptr);    // Thinking starts at frame zero.
    CHECK(GrokBotCachedStart(cache, 0, 1, 2, 1) == nullptr);    // A short max_frames limit.
    CHECK(GrokBotCachedStart(nullptr, 0, 2, 2, 1) == nullptr);  // Allocation fallback.
    CHECK(GrokBotCachedStart(cache, -1, 2, 2, 1) == nullptr);
    const auto* zero = GrokBotCachedStart(cache, 1, 0, 0, 1);
    CHECK(zero == cache + 1);  // Older atlases may use frame zero as their start.
    const auto once = GrokBotNextFrame(2, false, 2, 2, false, true);
    CHECK(once.once_done && !once.cycle_boundary);
    const auto loop = GrokBotNextFrame(2, false, 2, 2, true, false);
    CHECK(loop.cycle_boundary && !loop.once_done);
}

void FailedSwitch() {
    GrokBotFramePipeline pipeline;
    uint8_t displayed = 0x11;
    uint8_t candidate = 0xee;
    CHECK(pipeline.Submit() == 0);
    // Refresh is delayed: decoding the other buffer must leave the shown snapshot alone.
    candidate = 0x88;
    CHECK(pipeline.Submit() == -1);
    CHECK(displayed == 0x11);
    // A timeout or failed display lock does not commit candidate, including while paused.
    for (int i = 0; i < 3; ++i)
        CHECK(pipeline.pending && displayed == 0x11);
    const uint8_t fade_source = displayed;
    CHECK(fade_source == 0x11 && fade_source != candidate);
    pipeline.Complete();
    CHECK(pipeline.Submit() == 1);
    displayed = candidate;
    CHECK(displayed == 0x88);
    pipeline.Complete();
    CHECK(pipeline.Submit() == 0);
}
}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (std::strcmp(argv[1], "single") == 0)
        SingleDeltaFrame();
    else if (std::strcmp(argv[1], "pingpong") == 0)
        TwoFramePingPong();
    else if (std::strcmp(argv[1], "keyframe") == 0)
        MiddleKeyFrame();
    else if (std::strcmp(argv[1], "limit") == 0)
        FrameLimit();
    else if (std::strcmp(argv[1], "blend") == 0)
        Blend();
    else if (std::strcmp(argv[1], "pipeline") == 0)
        Pipeline();
    else if (std::strcmp(argv[1], "fused") == 0)
        FusedExpand();
    else if (std::strcmp(argv[1], "cached-switch") == 0)
        CachedSwitch();
    else if (std::strcmp(argv[1], "failed-switch") == 0)
        FailedSwitch();
    else if (std::strcmp(argv[1], "retry") == 0)
        PresentationRetry();
    else
        CHECK(false);
}
