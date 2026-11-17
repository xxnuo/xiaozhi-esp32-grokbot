#include "grokbot_face.h"
#include "grokbot_player.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using Face = GrokBotFace;

static void Near(float a, float b, float epsilon = 0.00002f) {
    assert(std::isfinite(a) && std::isfinite(b));
    assert(std::fabs(a - b) < epsilon);
}
static void SamePose(const Face::Pose& a, const Face::Pose& b) {
    for (int i = 0; i < 2; ++i) {
        Near(a.position[i], b.position[i]);
        Near(a.squash[i], b.squash[i]);
    }
    for (int i = 0; i < 3; ++i)
        Near(std::remainder(a.rotation[i] - b.rotation[i], 360.0f), 0);
    Near(a.zoom, b.zoom);
    Near(a.anchor, b.anchor);
}
static void SameEyes(const Face::Frame& a, const Face::Frame& b) {
    for (int i = 0; i < 2; ++i) {
        Near(a.eyes[i].wid, b.eyes[i].wid);
        Near(a.eyes[i].tilt, b.eyes[i].tilt);
        Near(a.eyes[i].oval_mix, b.eyes[i].oval_mix);
        for (int j = 0; j < 2; ++j) {
            Near(a.eyes[i].off[j], b.eyes[i].off[j]);
            Near(a.eyes[i].oval[j], b.eyes[i].oval[j]);
        }
        for (int j = 0; j < 18; ++j)
            Near(a.eyes[i].path[j], b.eyes[i].path[j]);
    }
}
static void Continuity() {
    Face face(1);
    assert(face.Select("idle", 1, 0));
    for (int t = 0; t < 7000; t += 20)
        face.Sample(t);
    auto current = face.Sample(7000);
    assert(face.Select("happy", 2, 7000));
    SamePose(current.pose, face.Sample(7000).pose);
    SameEyes(current, face.Sample(7000));
    face.Sample(7140);
    current = face.Sample(7160);
    assert(face.Select("sad", 3, 7160));
    SamePose(current.pose, face.Sample(7160).pose);
    SameEyes(current, face.Sample(7160));
    current = face.Sample(7290);
    assert(face.Select("idle", 4, 7290));
    SamePose(current.pose, face.Sample(7290).pose);
    SameEyes(current, face.Sample(7290));

    // The geometry at an interrupted blink must also be the actual displayed eye.
    Face blinking(1);
    blinking.Select("idle", 1, 0);
    for (int t = 0; t <= 3060; t += 20)
        current = blinking.Sample(t);
    current = blinking.Sample(3060);
    std::vector<uint8_t> before(360 * 360 / 2), after(before.size());
    Face::Rasterize(current, before.data(), 360, 360);
    blinking.Select("surprised", 2, 3060);
    Face::Rasterize(blinking.Sample(3060), after.data(), 360, 360);
    assert(before == after);
}
static void IdleAndBlink() {
    Face face(1);
    face.Select("idle", 1, 0);
    std::set<std::string> recipes;
    bool blink = false;
    float min_zoom = 1, max_zoom = 0;
    for (int t = 0; t < 30000; t += 20) {
        const auto frame = face.Sample(t);
        recipes.emplace(face.recipe_name());
        blink |= frame.blink > 0.9f;
        min_zoom = std::min(min_zoom, frame.pose.zoom);
        max_zoom = std::max(max_zoom, frame.pose.zoom);
    }
    assert(recipes == std::set<std::string>({"Idle_A", "Idle_B", "Idle_D"}));
    assert(blink && max_zoom - min_zoom > 0.035f);
    for (const char* clip : {"laughing", "angry", "sad", "sleeping", "shy", "suspicious"}) {
        Face blocked(1);
        blocked.Select(clip, 1, 0);
        for (int t = 300; t < 20000; t += 20)
            assert(blocked.Sample(t).blink == 0);
    }
    Face morphing(1);
    morphing.Select("idle", 1, 0);
    morphing.Sample(2980);
    morphing.Select("surprised", 2, 2980);
    for (int t = 2980; t < 3260; t += 20)
        assert(morphing.Sample(t).blink == 0);
}
static void Geometry() {
    Face face;
    face.Select("idle", 1, 0);
    auto first = face.Sample(1000);
    face.Select("surprised", 2, 1000);
    auto middle = face.Sample(1140);
    const auto last = face.Sample(1280);
    assert(middle.eyes[0].wid > first.eyes[0].wid && middle.eyes[0].wid < last.eyes[0].wid);
    assert(middle.eyes[0].path[1] < first.eyes[0].path[1]);
    std::vector<uint8_t> a(360 * 360 / 2), b(a.size()), c(a.size());
    // Hold the same body pose, so this proves that eye geometry, not fading, changes.
    middle.pose = first.pose;
    auto end = last;
    end.pose = first.pose;
    Face::Rasterize(first, a.data(), 360, 360);
    Face::Rasterize(middle, b.data(), 360, 360);
    Face::Rasterize(end, c.data(), 360, 360);
    size_t differs_from_pixel_blend = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int hi = ((a[i] >> 4) + (c[i] >> 4) + 1) / 2;
        const int lo = ((a[i] & 15) + (c[i] & 15) + 1) / 2;
        differs_from_pixel_blend += b[i] != ((hi << 4) | lo);
    }
    assert(differs_from_pixel_blend > 100);
    for (const char* expression : {"neutral", "happy", "surprised", "inspecting", "sad", "sleepy",
                                   "squinted", "calm", "confused"}) {
        const auto frame = Face::ExpressionFrame(expression);
        assert(Face::Rasterize(frame, a.data(), 360, 360));
        assert(std::count(a.begin(), a.end(), 255) > 10000);
        assert(std::count(a.begin(), a.end(), 0) > 10000);
    }
}
static void MorphTopology() {
    Face face(1);
    face.Select("idle", 1, 0);
    face.Sample(1000);
    face.Select("finished", 2, 1000);
    std::vector<uint8_t> packed(360 * 360 / 2);
    constexpr float normal[] = {-0.315345757f, 0.000413547f, 0.948976755f};
    constexpr float across[] = {-0.948976051f, -0.001431051f, -0.315344900f};
    constexpr float up[] = {-0.001227625f, 0.999998891f, -0.000843722f};
    for (int time = 20; time < 280; time += 20) {
        auto frame = face.Sample(1000 + time);
        frame.pose = Face::Pose{};
        frame.blink = 0;
        assert(Face::Rasterize(frame, packed.data(), 360, 360));
        const auto& eye = frame.eyes[0];
        float spine[21][2];
        for (int i = 0; i < 21; ++i) {
            const int segment = i <= 10 ? 0 : 1;
            const float t = (i - segment * 10) * 0.1f, u = 1 - t;
            const auto* a = eye.path + segment * 6;
            const auto* b = a + 6;
            for (int axis = 0; axis < 2; ++axis)
                spine[i][axis] = u * u * u * a[axis] + 3 * u * u * t * (a[axis] + a[axis + 4]) +
                                 3 * u * t * t * (b[axis] + b[axis + 2]) + t * t * t * b[axis];
        }
        // Independent source SDF oracle: every point well inside the stroke must remain a hole.
        for (int y = -55; y < 55; y += 2) {
            for (int x = -55; x < 55; x += 2) {
                const float px = x / 108.0f, py = y / 108.0f;
                float distance = 1e6f;
                for (int i = 0; i < 20; ++i) {
                    const float dx = spine[i + 1][0] - spine[i][0];
                    const float dy = spine[i + 1][1] - spine[i][1];
                    const float length = dx * dx + dy * dy;
                    const float t =
                        length > 0
                            ? std::clamp(
                                  ((px - spine[i][0]) * dx + (py - spine[i][1]) * dy) / length,
                                  0.0f, 1.0f)
                            : 0;
                    const float ax = px - spine[i][0] - t * dx, ay = py - spine[i][1] - t * dy;
                    distance = std::min(distance, std::sqrt(ax * ax + ay * ay));
                }
                if (distance > eye.wid - 0.025f)
                    continue;
                const float a = eye.off[0] + px, b = eye.off[1] + py;
                const float r = std::hypot(a, b), cs = std::cos(r),
                            sn = r > 0 ? std::sin(r) / r : 1;
                const int sx = static_cast<int>(
                    180 + 108 * (cs * normal[0] + sn * (a * across[0] + b * up[0])));
                const int sy = static_cast<int>(
                    180 - 108 * (cs * normal[1] + sn * (a * across[1] + b * up[1])));
                const size_t index = static_cast<size_t>(sy) * 360 + sx;
                const int gray = index % 2 ? packed[index / 2] & 15 : packed[index / 2] >> 4;
                assert(gray <= 1);
            }
        }
    }
}
static void AtlasBridge() {
    assert(!Face::Supports("thinking") && !Face::Supports("working"));
    assert(Face::AtlasBridgeSupported("thinking") && Face::AtlasBridgeSupported("working"));
    assert(!Face::AtlasBridgeSupported("playful"));
    Face face(1);
    face.Select("idle", 1, 0);
    for (int t = 0; t < 7000; t += 20)
        face.Sample(t);
    const auto from = face.Sample(7000);
    face.BeginAtlasBridge(7000);
    SamePose(from.pose, face.Sample(7000).pose);
    SameEyes(from, face.Sample(7000));
    const auto settled = face.Sample(7350);
    Near(settled.pose.zoom, 0.6f);
    Near(settled.pose.position[0], 0);
    Near(settled.pose.position[1], 0);
    Near(settled.pose.rotation[0], -15.896271f);
    Near(settled.pose.rotation[1], 33.293156f);
    Near(settled.pose.rotation[2], 25.384801f);
    const auto target = Face::ExpressionFrame("neutral");
    SameEyes(settled, target);
    const auto interrupted = face.Sample(7150);
    face.Select("shy", 2, 7150);
    SamePose(interrupted.pose, face.Sample(7150).pose);
    SameEyes(interrupted, face.Sample(7150));
}
static void ScratchAndBounds() {
    const auto frame = Face::ExpressionFrame("happy");
    std::vector<uint8_t> a(360 * 360 / 2), b(a.size());
    std::vector<uint32_t> scratch(Face::kScratchBytes / sizeof(uint32_t));
    assert(Face::Rasterize(frame, a.data(), 360, 360));
    assert(Face::Rasterize(frame, b.data(), 360, 360, scratch.data(), Face::kScratchBytes));
    assert(a == b);
    assert(!Face::Rasterize(frame, b.data(), 360, 360, scratch.data(), 16));
    assert(!Face::Rasterize(frame, b.data(), 359, 360));
    assert(!Face::Rasterize(frame, nullptr, 360, 360));
    assert(!Face::Rasterize(frame, b.data(), 1024, 360));
    for (int angle = 0; angle <= 360; angle += 15) {
        auto turned = frame;
        turned.pose.rotation[1] = angle;
        assert(Face::Rasterize(turned, b.data(), 360, 360, scratch.data(), Face::kScratchBytes));
    }
}
static void FusedRgbAndSnapshots() {
    constexpr int width = 360, height = 360;
    constexpr size_t size = width * height / 2;
    std::vector<uint8_t> plain(size), fused(size);
    std::vector<uint32_t> scratch(Face::kScratchBytes / sizeof(uint32_t)), rgb(size);
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = 0x11220000u | (static_cast<uint32_t>(i) * 0x010101u);
    for (const char* expression : {"neutral", "happy", "surprised", "inspecting", "sad", "sleepy",
                                   "squinted", "calm", "confused"}) {
        for (int yaw : {0, 90, 180}) {
            auto frame = Face::ExpressionFrame(expression);
            frame.pose.rotation[1] = yaw;
            assert(Face::Rasterize(frame, plain.data(), width, height));
            assert(Face::Rasterize(frame, fused.data(), width, height, scratch.data(),
                                   Face::kScratchBytes, rgb.data(), lut));
            assert(plain == fused);
            for (size_t i = 0; i < size; ++i)
                assert(rgb[i] == lut[plain[i]]);
        }
    }
    const auto zero = Face::ExpressionFrame("neutral");
    assert(!Face::Rasterize(zero, fused.data(), width, height, scratch.data(), Face::kScratchBytes,
                            rgb.data(), nullptr));
    auto empty = zero;
    empty.pose.squash[0] = 0;
    assert(Face::Rasterize(empty, fused.data(), width, height, scratch.data(), Face::kScratchBytes,
                           rgb.data(), lut));
    assert(std::all_of(rgb.begin(), rgb.end(), [&](uint32_t pixel) { return pixel == lut[0]; }));

    std::vector<uint8_t> buffers[3] = {std::vector<uint8_t>(size), std::vector<uint8_t>(size),
                                       std::vector<uint8_t>(size, 0x33)};
    auto* packed = buffers[0].data();
    auto* candidate = buffers[1].data();
    auto* displayed = buffers[2].data();
    const auto previous = buffers[2];
    Face face(1);
    face.Select("idle", 1, 0);
    assert(face.Render(candidate, width, height, 1000, packed, size, rgb.data(), lut));
    // A failed presentation leaves candidate ownership and the displayed snapshot untouched.
    GrokBotFramePipeline pipeline;
    pipeline.pending = true;
    assert(pipeline.Submit() == -1);
    assert(std::equal(previous.begin(), previous.end(), displayed));
    assert(face.Render(candidate, width, height, 1200, packed, size, rgb.data(), lut));
    const std::vector<uint8_t> expected(candidate, candidate + size);
    pipeline.Complete();
    assert(pipeline.Submit() == 0);
    std::swap(candidate, displayed);
    assert(std::equal(expected.begin(), expected.end(), displayed));
    assert(face.Render(candidate, width, height, 1400, packed, size, rgb.data(), lut));
    assert(std::equal(expected.begin(), expected.end(), displayed));
    for (size_t i = 0; i < size; ++i)
        assert(rgb[i] == lut[candidate[i]]);
}
static void Benchmark() {
    std::vector<uint8_t> packed(360 * 360 / 2);
    std::vector<uint32_t> scratch(Face::kScratchBytes / sizeof(uint32_t));
    Face face(1);
    face.Select("idle", 1, 0);
    std::vector<double> samples;
    for (int i = 0; i < 900; ++i) {
        const auto start = std::chrono::steady_clock::now();
        assert(face.Render(packed.data(), 360, 360, i * 1000 / 30, scratch.data(),
                           Face::kScratchBytes));
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    double total = 0;
    for (double sample : samples)
        total += sample;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "360x360 geometry 900 frames mean=%.1fus p50=%.1fus p95=%.1fus max=%.1fus scratch=%zuB "
        "object=%zuB\n",
        total / samples.size(), samples[450], samples[855], samples.back(), Face::kScratchBytes,
        sizeof(Face));
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    if (name == "continuity")
        Continuity();
    else if (name == "idle")
        IdleAndBlink();
    else if (name == "geometry")
        Geometry();
    else if (name == "topology")
        MorphTopology();
    else if (name == "atlas")
        AtlasBridge();
    else if (name == "scratch")
        ScratchAndBounds();
    else if (name == "fused")
        FusedRgbAndSnapshots();
    else if (name == "benchmark")
        Benchmark();
    else
        assert(false);
}
