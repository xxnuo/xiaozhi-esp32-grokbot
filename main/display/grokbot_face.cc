#include "grokbot_face.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180;
constexpr int kContour = 64;
constexpr int kMaxPolygon = 192;
struct Key {
    float time, value[3], ease[4];
};
struct ExpressionKey {
    float time;
    int expression;
};
struct Recipe {
    const char* name;
    float duration, anchor;
    const Key* position;
    size_t position_count;
    const Key* rotation;
    size_t rotation_count;
    const Key* squash;
    size_t squash_count;
    const Key* zoom;
    size_t zoom_count;
    const Key* lids;
    size_t lids_count;
    const ExpressionKey* expression;
    size_t expression_count;
};
#include "grokbot_face_data.inc"

struct State {
    const char* name;
    uint8_t recipes[3];
    int expression;
};
constexpr State kStates[] = {
    {"idle", {0}, -1},     {"listening", {1}, 7}, {"streaming", {10, 11, 12}, -1},
    {"happy", {6}, -1},    {"laughing", {7}, -1}, {"excited", {8}, -1},
    {"proud", {9}, -1},    {"shy", {1}, 7},       {"sad", {4}, -1},
    {"sleeping", {0}, 5},  {"angry", {3}, 6},     {"surprised", {0}, 2},
    {"confused", {3}, -1}, {"scared", {1}, 2},    {"curious", {13}, 3},
    {"drowsy", {5}, -1},   {"humming", {0}, 7},   {"suspicious", {1}, 6},
    {"finished", {7}, 1},  {"waking", {0}, 2},    {"notifying", {7}, 2},
};
const State* FindState(const char* clip) {
    if (clip)
        for (const auto& state : kStates)
            if (std::strcmp(clip, state.name) == 0)
                return &state;
    return nullptr;
}
float Mix(float a, float b, float t) { return a + (b - a) * t; }
float Clamp(float t) { return std::clamp(t, 0.0f, 1.0f); }
float Smooth(float t) {
    t = Clamp(t);
    return t * t * (3 - 2 * t);
}
float Bezier(float t, float a, float b) {
    const float s = 1 - t;
    return 3 * s * s * t * a + 3 * s * t * t * b + t * t * t;
}
float Ease(float t, const float* ease) {
    float u = Clamp(t);
    for (int i = 0; i < 8; ++i) {
        const float error = Bezier(u, ease[0], ease[2]) - t;
        const float s = 1 - u;
        const float derivative =
            3 * s * s * ease[0] + 6 * s * u * (ease[2] - ease[0]) + 3 * u * u * (1 - ease[2]);
        if (std::fabs(error) < 1e-6f || std::fabs(derivative) < 1e-6f)
            break;
        u = Clamp(u - error / derivative);
    }
    return Bezier(u, ease[1], ease[3]);
}
void Track(const Key* keys, size_t count, float time, float* out, size_t dimensions) {
    if (!count)
        return;
    size_t i = 0;
    while (i + 1 < count && keys[i + 1].time <= time)
        ++i;
    if (time <= keys[0].time || i + 1 == count) {
        std::copy_n(keys[i].value, dimensions, out);
        return;
    }
    const float t = Ease((time - keys[i].time) / (keys[i + 1].time - keys[i].time), keys[i].ease);
    for (size_t j = 0; j < dimensions; ++j)
        out[j] = Mix(keys[i].value[j], keys[i + 1].value[j], t);
}
GrokBotFace::Pose RecipePose(int recipe, float time) {
    const auto& r = kRecipes[recipe];
    GrokBotFace::Pose pose;
    pose.anchor = r.anchor;
    Track(r.position, r.position_count, time, pose.position, 2);
    Track(r.rotation, r.rotation_count, time, pose.rotation, 3);
    Track(r.squash, r.squash_count, time, pose.squash, 2);
    Track(r.zoom, r.zoom_count, time, &pose.zoom, 1);
    return pose;
}
struct Point {
    float x, y;
};
struct Point3 {
    float x, y, z;
};
struct Polygon {
    Point points[kMaxPolygon];
    int count = 0;
    void Push(Point p) {
        if (count < kMaxPolygon)
            points[count++] = p;
    }
};
void Spine(const GrokBotFace::Eye& eye, Point* spine) {
    for (int i = 0; i < 21; ++i) {
        const int segment = i <= 10 ? 0 : 1;
        const float t = (i - segment * 10) * 0.1f, s = 1 - t;
        const float* a = eye.path + segment * 6;
        const float* b = a + 6;
        spine[i] = {s * s * s * a[0] + 3 * s * s * t * (a[0] + a[4]) +
                        3 * s * t * t * (b[0] + b[2]) + t * t * t * b[0],
                    s * s * s * a[1] + 3 * s * s * t * (a[1] + a[5]) +
                        3 * s * t * t * (b[1] + b[3]) + t * t * t * b[1]};
    }
}
struct Edge {
    float x, step;
    int first, last, winding;
};
struct Scratch {
    Polygon eyes[2];
    Point contour[kContour], spine[21];
    Point3 ring[kContour];
    Edge edges[2][kMaxPolygon];
    int edge_count[2] = {};
};
static_assert(sizeof(Scratch) <= GrokBotFace::kScratchBytes);
void EyeContour(const GrokBotFace::Eye& eye, Point* contour, Point* spine) {
    // The recipes use an open stroked spine or an ellipse; construct their boundary directly.
    static const struct Circle {
        Point points[kContour];
        Circle() {
            for (int i = 0; i < kContour; ++i) {
                const float angle = i * (2 * kPi / kContour);
                points[i] = {std::cos(angle), std::sin(angle)};
            }
        }
    } circle;
    Spine(eye, spine);
    if (eye.oval_mix > 0) {
        const float length = std::max(eye.wid, (eye.path[13] - eye.path[1]) / 2 + eye.wid);
        const float half = std::max(0.0f, length - eye.wid);
        for (int i = 0; i < kContour; ++i) {
            const auto direction = circle.points[i];
            const Point ellipse{eye.oval[0] * direction.x, eye.oval[1] * direction.y};
            if (eye.oval_mix >= 1) {
                contour[i] = ellipse;
                continue;
            }
            const float side = eye.wid / std::max(std::fabs(direction.x), 1e-6f);
            float radius = side;
            if (side * std::fabs(direction.y) > half) {
                const float w = half * std::fabs(direction.y);
                radius = w + std::sqrt(std::max(0.0f, w * w - half * half + eye.wid * eye.wid));
            }
            contour[i] = {Mix(radius * direction.x, ellipse.x, eye.oval_mix),
                          Mix(radius * direction.y, ellipse.y, eye.oval_mix)};
        }
        return;
    }
    float extent = 0;
    for (int i = 0; i < 21; ++i)
        extent = std::max(extent, std::hypot(spine[i].x - spine[0].x, spine[i].y - spine[0].y));
    if (extent < 1e-6f) {
        for (int i = 0; i < kContour; ++i)
            contour[i] = {spine[0].x + eye.wid * circle.points[i].x,
                          spine[0].y + eye.wid * circle.points[i].y};
        return;
    }
    Point first_normal{}, last_normal{};
    for (int i = 0; i < 21; ++i) {
        const auto a = spine[std::max(0, i - 1)], b = spine[std::min(20, i + 1)];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float length = std::hypot(dx, dy);
        const Point normal = length > 1e-8f ? Point{-dy / length, dx / length} : Point{-1, 0};
        contour[i] = {spine[i].x + eye.wid * normal.x, spine[i].y + eye.wid * normal.y};
        contour[52 - i] = {spine[i].x - eye.wid * normal.x, spine[i].y - eye.wid * normal.y};
        if (i == 0)
            first_normal = normal;
        if (i == 20)
            last_normal = normal;
    }
    constexpr float cosine[] = {
        0.965925826f,  0.866025404f, 0.707106781f,  0.5f,          0.258819045f, 0,
        -0.258819045f, -0.5f,        -0.707106781f, -0.866025404f, -0.965925826f};
    constexpr float sine[] = {0.258819045f, 0.5f, 0.707106781f, 0.866025404f,
                              0.965925826f, 1,    0.965925826f, 0.866025404f,
                              0.707106781f, 0.5f, 0.258819045f};
    for (int j = 0; j < 11; ++j) {
        const auto end = last_normal;
        const Point start{-first_normal.x, -first_normal.y};
        contour[21 + j] = {spine[20].x + eye.wid * (end.x * cosine[j] + end.y * sine[j]),
                           spine[20].y + eye.wid * (-end.x * sine[j] + end.y * cosine[j])};
        contour[53 + j] = {spine[0].x + eye.wid * (start.x * cosine[j] + start.y * sine[j]),
                           spine[0].y + eye.wid * (-start.x * sine[j] + start.y * cosine[j])};
    }
}
struct Capsule {
    float x, y, angle, length, width;
};
Capsule EyeCapsule(const GrokBotFace::Eye& e) {
    if (e.oval_mix >= 1) {
        const bool horizontal = e.oval[0] >= e.oval[1];
        return {e.off[0], e.off[1], horizontal ? kPi / 2 : 0, std::max(e.oval[0], e.oval[1]),
                std::min(e.oval[0], e.oval[1])};
    }
    return {e.off[0] + (e.path[0] + e.path[12]) / 2, e.off[1] + (e.path[1] + e.path[13]) / 2,
            std::atan2(e.path[12] - e.path[0], e.path[13] - e.path[1]) + e.tilt,
            std::hypot(e.path[12] - e.path[0], e.path[13] - e.path[1]) / 2 + e.wid, e.wid};
}
GrokBotFace::Eye MorphEye(const GrokBotFace::Eye& a, const GrokBotFace::Eye& b, float t) {
    if (t >= 1)
        return b;
    if (t <= 0)
        return a;
    GrokBotFace::Eye out = a;
    if (a.oval_mix > 0 || b.oval_mix > 0) {
        const auto from = EyeCapsule(a), to = EyeCapsule(b);
        float angle = to.angle - from.angle;
        angle -= kPi * std::round(angle / kPi);
        const float length = Mix(from.length, to.length, t), width = Mix(from.width, to.width, t);
        out.off[0] = Mix(from.x, to.x, t);
        out.off[1] = Mix(from.y, to.y, t);
        out.tilt = from.angle + angle * t;
        out.wid = width;
        out.oval[0] = width;
        out.oval[1] = length;
        out.oval_mix = Mix(a.oval_mix, b.oval_mix, t);
        std::fill_n(out.path, 18, 0);
        const float half = std::max(0.0f, length - width);
        out.path[1] = -half;
        out.path[5] = half / 3;
        out.path[9] = -half / 3;
        out.path[11] = half / 3;
        out.path[13] = half;
        out.path[15] = -half / 3;
    } else {
        for (int i = 0; i < 2; ++i)
            out.off[i] = Mix(a.off[i], b.off[i], t);
        out.tilt = Mix(a.tilt, b.tilt, t);
        out.wid = Mix(a.wid, b.wid, t);
        for (int i = 0; i < 18; ++i)
            out.path[i] = Mix(a.path[i], b.path[i], t);
    }
    out.lid_y = Mix(a.lid_y, b.lid_y, t);
    return out;
}
GrokBotFace::Eye BlinkEye(GrokBotFace::Eye eye, float amount) {
    if (amount <= 0)
        return eye;
    Point spine[21];
    Spine(eye, spine);
    const float cs = std::cos(eye.tilt), sn = std::sin(eye.tilt);
    float min_x = 1e6f, max_x = -1e6f, min_y = 1e6f, max_y = -1e6f;
    if (eye.oval_mix >= 1) {
        min_x = -eye.oval[0];
        max_x = eye.oval[0];
        min_y = -eye.oval[1];
        max_y = eye.oval[1];
    } else {
        for (auto p : spine) {
            const float x = cs * p.x + sn * p.y, y = -sn * p.x + cs * p.y;
            min_x = std::min(min_x, x - eye.wid);
            max_x = std::max(max_x, x + eye.wid);
            min_y = std::min(min_y, y - eye.wid);
            max_y = std::max(max_y, y + eye.wid);
        }
    }
    const float width = max_x - min_x;
    const float height = Mix(std::max(width, max_y - min_y), width * 0.42f, Clamp(amount));
    const bool horizontal = width > height;
    const float minor = std::min(width, height) / 2;
    const float half = std::max(width, height) / 2 - minor;
    eye.off[0] += (min_x + max_x) / 2;
    eye.off[1] += (min_y + max_y) / 2;
    eye.tilt = horizontal ? kPi / 2 : 0;
    eye.wid = minor;
    eye.oval[0] = eye.oval[1] = eye.oval_mix = 0;
    std::fill_n(eye.path, 18, 0);
    eye.path[1] = -half;
    eye.path[5] = half / 3;
    eye.path[9] = -half / 3;
    eye.path[11] = half / 3;
    eye.path[13] = half;
    eye.path[15] = -half / 3;
    return eye;
}
void Rotation(const float* degrees, float* m) {
    const float x = degrees[0] * kDeg, y = degrees[1] * kDeg, z = degrees[2] * kDeg;
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
    const float cz = std::cos(z), sz = std::sin(z);
    m[0] = cz * cy;
    m[1] = cz * sy * sx - sz * cx;
    m[2] = cz * sy * cx + sz * sx;
    m[3] = sz * cy;
    m[4] = sz * sy * sx + cz * cx;
    m[5] = sz * sy * cx - cz * sx;
    m[6] = -sy;
    m[7] = cy * sx;
    m[8] = cy * cx;
}
Point3 Transform(const float* m, const float* p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2], m[3] * p[0] + m[4] * p[1] + m[5] * p[2],
            m[6] * p[0] + m[7] * p[1] + m[8] * p[2]};
}
Point Horizon(Point3 a, Point3 b) {
    const float t = a.z / (a.z - b.z);
    Point p{Mix(a.x, b.x, t), Mix(a.y, b.y, t)};
    const float length = std::hypot(p.x, p.y);
    return {p.x / length, p.y / length};
}
float Wrap(float angle) { return angle - 2 * kPi * std::round(angle / (2 * kPi)); }
void HorizonArc(Polygon& polygon, Point a, Point b, float center) {
    const float start = std::atan2(a.y, a.x);
    const float sweep = Wrap(std::atan2(b.y, b.x) - center) - Wrap(start - center);
    const int count = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) * 48 / kPi)));
    for (int i = 1; i <= count; ++i) {
        const float angle = start + sweep * i / count;
        polygon.Push({std::cos(angle), std::sin(angle)});
    }
}
void ProjectEye(const GrokBotFace::Eye& eye, int index, const float* matrix, Polygon& polygon,
                Scratch& scratch) {
    auto* contour = scratch.contour;
    EyeContour(eye, contour, scratch.spine);
    const Point3 normal = Transform(matrix, kBasis[index][0]);
    const Point3 across = Transform(matrix, kBasis[index][1]);
    const Point3 up = Transform(matrix, kBasis[index][2]);
    const float cs = std::cos(eye.tilt), sn = std::sin(eye.tilt);
    auto* ring = scratch.ring;
    int first = -1;
    for (int j = 0; j < kContour; ++j) {
        const int i = index ? kContour - 1 - j : j;
        const float x = eye.off[0] + cs * contour[i].x + sn * contour[i].y * eye.lid_y;
        const float y = eye.off[1] - sn * contour[i].x + cs * contour[i].y * eye.lid_y;
        const float radius = std::hypot(x, y), a = std::cos(radius);
        const float b = radius > 1e-12f ? std::sin(radius) / radius : 1;
        ring[j] = {a * normal.x + b * (x * across.x + y * up.x),
                   a * normal.y + b * (x * across.y + y * up.y),
                   a * normal.z + b * (x * across.z + y * up.z)};
        if (ring[j].z > 0)
            first = j;
    }
    polygon.count = 0;
    if (first < 0)
        return;
    Point exit{};
    bool hidden = false;
    for (int i = 0; i < kContour; ++i) {
        const auto a = ring[(first + i) % kContour], b = ring[(first + i + 1) % kContour];
        if (a.z > 0 && b.z > 0)
            polygon.Push({b.x, b.y});
        else if (a.z > 0) {
            exit = Horizon(a, b);
            polygon.Push(exit);
            hidden = true;
        } else if (b.z > 0) {
            const auto entry = Horizon(a, b);
            if (hidden)
                HorizonArc(polygon, exit, entry, std::atan2(normal.y, normal.x));
            polygon.Push({b.x, b.y});
        }
    }
}
void BuildEdges(const Polygon& polygon, Edge* edges, int& count, int top, int bottom) {
    count = 0;
    for (int i = 0; i < polygon.count; ++i) {
        Point a = polygon.points[i], b = polygon.points[(i + 1) % polygon.count];
        if (a.y == b.y)
            continue;
        const int winding = a.y < b.y ? 1 : -1;
        if (a.y > b.y)
            std::swap(a, b);
        const int first = std::max(top * 4, static_cast<int>(std::ceil(a.y * 4 - 0.5f)));
        const int last = std::min(bottom * 4, static_cast<int>(std::ceil(b.y * 4 - 0.5f)));
        if (first >= last)
            continue;
        const float slope = (b.x - a.x) / (b.y - a.y);
        edges[count++] = {a.x + ((first + 0.5f) / 4 - a.y) * slope, slope / 4, first, last,
                          winding};
    }
    std::sort(edges, edges + count, [](const Edge& a, const Edge& b) { return a.first < b.first; });
}
void AddSpan(float left, float right, float* coverage, int width) {
    left = std::clamp(left, 0.0f, static_cast<float>(width));
    right = std::clamp(right, left, static_cast<float>(width));
    const int first = static_cast<int>(left), last = std::min(width - 1, static_cast<int>(right));
    if (first > last)
        return;
    if (first == last) {
        coverage[first] += right - left;
        return;
    }
    coverage[first] += first + 1 - left;
    for (int x = first + 1; x < last; ++x)
        coverage[x] += 1;
    coverage[last] += right - last;
}
}  // namespace

GrokBotFace::GrokBotFace(uint32_t seed) : random_(seed ? seed : 1) {
    std::copy_n(kEyes[0], 2, eye_from_);
}
uint32_t GrokBotFace::Random() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
}
bool GrokBotFace::Supports(const char* clip) { return FindState(clip) != nullptr; }
bool GrokBotFace::AtlasBridgeSupported(const char* clip) {
    if (!clip)
        return false;
    for (const char* icon :
         {"thinking", "searching", "radar", "waiting", "loading", "uploading", "alerting",
          "working", "logo", "spawning", "writing", "receiving", "powering-down"})
        if (std::strcmp(icon, clip) == 0)
            return true;
    return false;
}
void GrokBotFace::BeginAtlasBridge(int64_t now_ms) {
    const auto current = Sample(now_ms);
    bridge_from_ = current.pose;
    for (int i = 0; i < 2; ++i)
        eye_from_[i] = BlinkEye(current.eyes[i], current.blink);
    expression_ = 0;
    expression_start_ = now_ms;
    recipe_start_ = now_ms;
    atlas_bridge_ = true;
    active_ = true;
}
void GrokBotFace::SampleEyes(int64_t now_ms, Eye* eyes) const {
    constexpr float ease[] = {0.4f, 0.06f, 0.18f, 1};
    const float t = Ease(Clamp(static_cast<float>(now_ms - expression_start_) / 280), ease);
    for (int i = 0; i < 2; ++i)
        eyes[i] = MorphEye(eye_from_[i], kEyes[expression_][i], t);
}
void GrokBotFace::SetExpression(int expression, int64_t now_ms) {
    if (expression == expression_)
        return;
    SampleEyes(now_ms, eye_from_);
    expression_ = expression;
    expression_start_ = now_ms;
}
void GrokBotFace::StartRecipe(int recipe, int64_t now_ms, bool bridge) {
    if (active_)
        bridge_from_ = SamplePose(now_ms);
    recipe_ = recipe;
    recipe_start_ = now_ms;
    bridge_ = bridge;
    const auto state = FindState(clip_);
    int expression = state->expression;
    if (expression < 0) {
        expression = 0;
        for (size_t i = 0; i < kRecipes[recipe].expression_count; ++i) {
            const auto& key = kRecipes[recipe].expression[i];
            if (key.time > 0)
                break;
            if (key.expression >= 0)
                expression = key.expression;
        }
    }
    SetExpression(expression, now_ms);
}
bool GrokBotFace::Select(const char* clip, uint32_t sequence, int64_t now_ms) {
    const auto state = FindState(clip);
    if (!state) {
        if (active_)
            Sample(now_ms);
        active_ = false;
        return false;
    }
    if (active_ && sequence_ == sequence && std::strcmp(clip_, clip) == 0)
        return true;
    Frame from;
    if (active_)
        from = Sample(now_ms);
    const bool bridge = active_;
    atlas_bridge_ = false;
    clip_ = state->name;
    sequence_ = sequence;
    variant_ = 0;
    StartRecipe(state->recipes[0], now_ms, bridge);
    if (bridge) {
        bridge_from_ = from.pose;
        for (int i = 0; i < 2; ++i)
            eye_from_[i] = BlinkEye(from.eyes[i], from.blink);
        expression_start_ = now_ms;
    }
    active_ = true;
    if (!next_blink_)
        next_blink_ = now_ms + 1700 + Random() % 2300;
    if (!next_gaze_)
        next_gaze_ = now_ms + 2100 + Random() % 3000;
    return true;
}
GrokBotFace::Pose GrokBotFace::SamplePose(int64_t now_ms) const {
    const float elapsed = std::max<int64_t>(0, now_ms - recipe_start_) / 1000.0f;
    const float time = std::max(0.0f, elapsed - (bridge_ ? 0.35f : 0));
    Pose pose = RecipePose(recipe_, time);
    if (std::strcmp(clip_, "idle") == 0 || std::strcmp(clip_, "sleeping") == 0 ||
        std::strcmp(clip_, "humming") == 0) {
        const float phase = now_ms * (2 * kPi / 4200);
        pose.squash[0] *= 1 + 0.006f * std::sin(phase);
        pose.squash[1] *= 1 - 0.006f * std::sin(phase);
        if (std::strcmp(clip_, "sleeping") != 0) {
            const float t = Smooth((now_ms - gaze_start_) / 500.0f);
            const float x = Mix(gaze_from_[0], gaze_[0], t), y = Mix(gaze_from_[1], gaze_[1], t);
            pose.rotation[0] -= 3.5f * y;
            pose.rotation[1] += 4.5f * x;
            pose.position[0] += 0.008f * x;
            pose.position[1] += 0.006f * y;
        }
    }
    if (bridge_ && elapsed < 0.35f) {
        constexpr float ease[] = {0.4f, 0, 0.2f, 1};
        const float t = Ease(elapsed / 0.35f, ease);
        for (int i = 0; i < 2; ++i) {
            pose.position[i] = Mix(bridge_from_.position[i], pose.position[i], t);
            pose.squash[i] = Mix(bridge_from_.squash[i], pose.squash[i], t);
        }
        for (int i = 0; i < 3; ++i) {
            const float to = pose.rotation[i] +
                             360 * std::round((bridge_from_.rotation[i] - pose.rotation[i]) / 360);
            pose.rotation[i] = Mix(bridge_from_.rotation[i], to, t);
        }
        pose.zoom = Mix(bridge_from_.zoom, pose.zoom, t);
        pose.anchor = Mix(bridge_from_.anchor, pose.anchor, t);
    }
    return pose;
}
GrokBotFace::Frame GrokBotFace::Sample(int64_t now_ms) {
    if (atlas_bridge_) {
        Frame frame;
        constexpr float ease[] = {0.4f, 0, 0.2f, 1};
        const float t = Ease(Clamp((now_ms - recipe_start_) / 350.0f), ease);
        for (int i = 0; i < 2; ++i) {
            frame.pose.position[i] = Mix(bridge_from_.position[i], 0, t);
            frame.pose.squash[i] = Mix(bridge_from_.squash[i], 1, t);
        }
        for (int i = 0; i < 3; ++i) {
            const float to =
                kAtlasStartRotation[i] +
                360 * std::round((bridge_from_.rotation[i] - kAtlasStartRotation[i]) / 360);
            frame.pose.rotation[i] = Mix(bridge_from_.rotation[i], to, t);
        }
        frame.pose.zoom = Mix(bridge_from_.zoom, 0.6f, t);
        frame.pose.anchor = Mix(bridge_from_.anchor, 0, t);
        SampleEyes(now_ms, frame.eyes);
        return frame;
    }
    const auto state = FindState(clip_);
    if (!active_ || !state)
        return ExpressionFrame("neutral");
    float time = std::max(0.0f, (now_ms - recipe_start_) / 1000.0f - (bridge_ ? 0.35f : 0));
    if (time >= kRecipes[recipe_].duration) {
        int next = state->recipes[0];
        if (std::strcmp(clip_, "idle") == 0) {
            constexpr uint8_t idle[] = {0, 0, 1, 0, 2};
            next = idle[++variant_ % 5];
        } else if (std::strcmp(clip_, "streaming") == 0) {
            next = state->recipes[++variant_ % 3];
        }
        StartRecipe(next, now_ms, true);
        time = 0;
    }
    const auto& recipe = kRecipes[recipe_];
    int expression = state->expression >= 0 ? state->expression : 0;
    float blink = 0;
    for (size_t i = 0; i < recipe.expression_count; ++i) {
        const auto& key = recipe.expression[i];
        if (key.time > time)
            break;
        if (key.expression >= 0 && state->expression < 0)
            expression = key.expression;
        else if (key.expression < 0 && time - key.time < 0.26f) {
            constexpr float ease[] = {0.42f, 0, 0.58f, 1};
            const float t = (time - key.time) / 0.26f;
            blink = t < 0.45f ? Ease(t / 0.45f, ease) : 1 - Ease((t - 0.45f) / 0.55f, ease);
        }
    }
    SetExpression(expression, now_ms);
    const bool allow_blink = expression != 1 && expression != 4 && expression != 5 &&
                             expression != 6 && expression != 7 && expression != 8 &&
                             now_ms - expression_start_ >= 280;
    if (now_ms >= next_blink_) {
        blink_start_ = now_ms;
        next_blink_ = now_ms + 2600 + Random() % 3600;
    }
    const int64_t blink_ms = now_ms - blink_start_;
    if (blink_ms >= 0 && blink_ms < 260 && allow_blink) {
        const float amount = blink_ms < 71    ? Smooth(blink_ms / 71.0f)
                             : blink_ms < 104 ? 1
                                              : 1 - Smooth((blink_ms - 104) / 156.0f);
        blink = std::max(blink, amount);
    }
    Track(recipe.lids, recipe.lids_count, time, &blink, 1);
    if (std::strcmp(clip_, "idle") == 0 || std::strcmp(clip_, "humming") == 0) {
        if (now_ms >= next_gaze_) {
            const float t = Smooth((now_ms - gaze_start_) / 500.0f);
            for (int i = 0; i < 2; ++i)
                gaze_from_[i] = Mix(gaze_from_[i], gaze_[i], t);
            gaze_[0] = (static_cast<int>(Random() % 2001) - 1000) / 1000.0f;
            gaze_[1] = (static_cast<int>(Random() % 2001) - 1000) / 1000.0f;
            gaze_start_ = now_ms;
            next_gaze_ = now_ms + 1600 + Random() % 3400;
        }
    }
    Frame frame;
    frame.pose = SamplePose(now_ms);
    SampleEyes(now_ms, frame.eyes);
    frame.blink = allow_blink ? Clamp(blink) : 0;
    return frame;
}
const char* GrokBotFace::recipe_name() const { return kRecipes[recipe_].name; }
GrokBotFace::Frame GrokBotFace::ExpressionFrame(const char* expression) {
    Frame frame;
    int index = 0;
    if (expression)
        for (int i = 0; i < 9; ++i)
            if (std::strcmp(expression, kExpressionNames[i]) == 0)
                index = i;
    std::copy_n(kEyes[index], 2, frame.eyes);
    return frame;
}
bool GrokBotFace::Render(uint8_t* packed, int width, int height, int64_t now_ms) {
    return Rasterize(Sample(now_ms), packed, width, height);
}
bool GrokBotFace::Render(uint8_t* packed, int width, int height, int64_t now_ms, void* scratch,
                         size_t scratch_size, uint32_t* rgb_pairs, const uint32_t* pair_lut) {
    return Rasterize(Sample(now_ms), packed, width, height, scratch, scratch_size, rgb_pairs,
                     pair_lut);
}
bool GrokBotFace::Rasterize(const Frame& frame, uint8_t* packed, int width, int height) {
    Scratch scratch;
    return Rasterize(frame, packed, width, height, &scratch, sizeof(scratch));
}
bool GrokBotFace::Rasterize(const Frame& frame, uint8_t* packed, int width, int height,
                            void* memory, size_t scratch_size, uint32_t* rgb_pairs,
                            const uint32_t* pair_lut) {
    if (!memory || scratch_size < sizeof(Scratch) ||
        reinterpret_cast<uintptr_t>(memory) % alignof(Scratch) || !packed || width <= 0 ||
        width > 512 || height <= 0 || height > 512 || width % 2 ||
        !std::isfinite(frame.pose.zoom) || frame.pose.zoom <= 0 || (rgb_pairs && !pair_lut))
        return false;
    const size_t row_size = width / 2;
    const auto clear_rows = [&](int first, int last) {
        const size_t offset = static_cast<size_t>(first) * row_size;
        const size_t count = static_cast<size_t>(last - first) * row_size;
        std::memset(packed + offset, 0, count);
        if (rgb_pairs) {
            if (pair_lut[0] == 0)
                std::memset(rgb_pairs + offset, 0, count * sizeof(uint32_t));
            else
                std::fill_n(rgb_pairs + offset, count, pair_lut[0]);
        }
    };
    float matrix[9];
    Rotation(frame.pose.rotation, matrix);
    auto& scratch = *static_cast<Scratch*>(memory);
    auto& eyes = scratch.eyes;
    const float radius = frame.pose.zoom * std::min(width, height) / 2;
    const float cx = width / 2.0f + radius * frame.pose.position[0];
    const float cy = height / 2.0f - radius * (frame.pose.anchor * (1 - frame.pose.squash[1]) +
                                               frame.pose.position[1]);
    const float rx = radius * std::fabs(frame.pose.squash[0]),
                ry = radius * std::fabs(frame.pose.squash[1]);
    if (rx <= 0 || ry <= 0) {
        clear_rows(0, height);
        return true;
    }
    const int top = std::clamp(static_cast<int>(std::floor(cy - ry)), 0, height);
    const int bottom = std::clamp(static_cast<int>(std::ceil(cy + ry)), 0, height);
    clear_rows(0, top);
    clear_rows(bottom, height);
    if (top == bottom)
        return true;
    for (int i = 0; i < 2; ++i) {
        ProjectEye(BlinkEye(frame.eyes[i], frame.blink), i, matrix, eyes[i], scratch);
        for (int j = 0; j < eyes[i].count; ++j) {
            eyes[i].points[j].x = cx + radius * eyes[i].points[j].x;
            eyes[i].points[j].y = cy - radius * eyes[i].points[j].y;
        }
    }
    // Keep only scanline hot data on the already allocated animation-task stack.
    float body[512], holes[512];
    alignas(uint32_t) uint8_t packed_row[256];
    uint32_t rgb_row[256];
    uint16_t active[2][kMaxPolygon];
    int active_count[2] = {}, next_edge[2] = {};
    for (int i = 0; i < 2; ++i)
        BuildEdges(eyes[i], scratch.edges[i], scratch.edge_count[i], top, bottom);
    const int first_x = std::max(0, static_cast<int>(std::floor(cx - rx))) & ~1;
    const int last_x = std::min(width, (static_cast<int>(std::ceil(cx + rx)) + 1) & ~1);
    for (int y = top; y < bottom; ++y) {
        std::memset(packed_row, 0, row_size);
        if (rgb_pairs)
            std::fill_n(rgb_row, row_size, pair_lut[0]);
        std::fill_n(body, width, 0);
        std::fill_n(holes, width, 0);
        float body_left[4], body_right[4];
        float inner_left = 0, inner_right = width;
        for (int sub = 0; sub < 4; ++sub) {
            const float scan = y + (sub + 0.5f) / 4;
            const float dy = (scan - cy) / ry;
            const float half = std::fabs(dy) < 1 ? rx * std::sqrt(1 - dy * dy) : 0;
            const float left = cx - half, right = cx + half;
            body_left[sub] = left;
            body_right[sub] = right;
            inner_left = std::max(inner_left, left);
            inner_right = std::min(inner_right, right);
            const int sample = y * 4 + sub;
            for (int eye = 0; eye < 2; ++eye) {
                auto* edges = scratch.edges[eye];
                auto* indices = active[eye];
                int& count = active_count[eye];
                for (int i = 0; i < count;) {
                    if (edges[indices[i]].last <= sample)
                        indices[i] = indices[--count];
                    else
                        ++i;
                }
                int& next = next_edge[eye];
                while (next < scratch.edge_count[eye] && edges[next].first <= sample) {
                    if (edges[next].last > sample)
                        indices[count++] = next;
                    ++next;
                }
                const auto crossing = [&](int index) {
                    const auto& edge = edges[index];
                    return edge.x + (sample - edge.first) * edge.step;
                };
                std::sort(indices, indices + count,
                          [&](int a, int b) { return crossing(a) < crossing(b); });
                int winding = 0;
                float start = 0;
                for (int i = 0; i < count; ++i) {
                    const float x = crossing(indices[i]);
                    if (winding == 0)
                        start = x;
                    winding += edges[indices[i]].winding;
                    if (winding == 0 && start < right && x > left)
                        AddSpan(std::max(left, start), std::min(right, x), holes, width);
                }
            }
        }
        const int full_first = std::clamp(static_cast<int>(std::ceil(inner_left)), 0, width);
        const int full_last =
            std::clamp(static_cast<int>(std::floor(inner_right)), full_first, width);
        std::fill(body + full_first, body + full_last, 4.0f);
        for (int sub = 0; sub < 4; ++sub) {
            AddSpan(body_left[sub], std::min(body_right[sub], static_cast<float>(full_first)), body,
                    width);
            AddSpan(std::max(body_left[sub], static_cast<float>(full_last)), body_right[sub], body,
                    width);
        }
        for (int x = first_x; x < last_x; x += 2) {
            const size_t offset = x / 2;
            if (body[x] == 4 && body[x + 1] == 4 && holes[x] == 0 && holes[x + 1] == 0) {
                packed_row[offset] = 255;
                if (rgb_pairs)
                    rgb_row[offset] = pair_lut[255];
                continue;
            }
            if (body[x] == 0 && body[x + 1] == 0)
                continue;
            const auto gray = [&](int at) {
                return static_cast<uint8_t>(Clamp((body[at] - holes[at]) / 4) * 15 + 0.5f);
            };
            const uint8_t value = (gray(x) << 4) | gray(x + 1);
            packed_row[offset] = value;
            if (rgb_pairs)
                rgb_row[offset] = pair_lut[value];
        }
        std::memcpy(packed + static_cast<size_t>(y) * row_size, packed_row, row_size);
        if (rgb_pairs)
            std::memcpy(rgb_pairs + static_cast<size_t>(y) * row_size, rgb_row,
                        row_size * sizeof(uint32_t));
    }
    return true;
}
