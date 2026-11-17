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
    else if (std::strcmp(argv[1], "retry") == 0)
        PresentationRetry();
    else
        CHECK(false);
}
