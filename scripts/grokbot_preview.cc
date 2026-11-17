#include "grokbot_director.h"
#include "grokbot_face.h"
#include "grokbot_player.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t kGlobalHeaderSize = 16;
constexpr size_t kClipHeaderSize = 32;

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

struct ClipMeta {
    std::string name;
    uint16_t frames = 0;
    uint16_t start_frame = 0;
};

struct AtlasMeta {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t fps = 30;
    std::vector<ClipMeta> clips;

    const ClipMeta* Find(const char* name) const {
        if (!name)
            return nullptr;
        for (const auto& c : clips) {
            if (c.name == name)
                return &c;
        }
        return nullptr;
    }
};

bool LoadAtlasMeta(const std::string& path, AtlasMeta* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    uint8_t global_hdr[kGlobalHeaderSize];
    if (!f.read(reinterpret_cast<char*>(global_hdr), kGlobalHeaderSize))
        return false;
    if (std::memcmp(global_hdr, "GBZ1", 4) != 0 || ReadU16(global_hdr + 4) != 1)
        return false;
    out->width = ReadU16(global_hdr + 6);
    out->height = ReadU16(global_hdr + 8);
    out->fps = global_hdr[10];
    uint8_t clip_count = global_hdr[11];
    out->clips.clear();
    for (uint8_t i = 0; i < clip_count; ++i) {
        uint8_t clip_hdr[kClipHeaderSize];
        if (!f.read(reinterpret_cast<char*>(clip_hdr), kClipHeaderSize))
            return false;
        char name_buf[17] = {};
        std::memcpy(name_buf, clip_hdr, 16);
        ClipMeta cm;
        cm.name = name_buf;
        cm.frames = ReadU16(clip_hdr + 16);
        cm.start_frame = ReadU16(clip_hdr + 18);
        out->clips.push_back(cm);
    }
    return true;
}

int RenderFaceFrames(const AtlasMeta& atlas) {
    if (!atlas.width || !atlas.height || atlas.width > 512 || atlas.height > 512 ||
        atlas.width % 2) {
        std::fprintf(stderr, "Invalid face render dimensions\n");
        return 1;
    }
    GrokBotFace face;  // Match the firmware's default face seed; --seed controls the director.
    std::vector<uint8_t> packed(static_cast<size_t>(atlas.width) * atlas.height / 2);
    int64_t now_ms;
    uint32_t sequence, previous_sequence = 0;
    std::string clip;
    int mode, present;
    while (std::cin >> now_ms >> sequence >> clip >> mode >> present) {
        if (now_ms < 0 || clip.size() > 16 || mode < 0 || mode > 2 || present < 0 || present > 1)
            return 1;
        if (sequence != previous_sequence) {
            if (mode == 2)
                face.BeginAtlasBridge(now_ms);
            else
                face.Select(clip.c_str(), sequence, now_ms);
            previous_sequence = sequence;
        }
        if (mode && present && !face.Render(packed.data(), atlas.width, atlas.height, now_ms))
            return 1;
        if (std::fputc(mode ? 1 : 0, stdout) == EOF ||
            (mode && std::fwrite(packed.data(), 1, packed.size(), stdout) != packed.size()) ||
            std::fflush(stdout) != 0)
            return 1;
    }
    return std::cin.eof() ? 0 : 1;
}

struct Action {
    int64_t at_ms;
    enum class Type {
        kStatus,
        kEmotion,
        kUserSpeech,
        kPowerSave,
        kNotification,
        kInteraction
    } type;
    GrokBotDirector::StatusId status = GrokBotDirector::StatusId::kIdle;
    const char* str_arg = nullptr;
    bool bool_arg = false;
    int64_t until_ms = 0;
    const char* interaction_label = nullptr;
};

class Simulator {
public:
    Simulator(const AtlasMeta& atlas, uint32_t seed) : atlas_(atlas), director_(seed) {
        std::vector<const char*> names;
        names.reserve(atlas_.clips.size());
        for (const auto& c : atlas_.clips) {
            names.push_back(c.name.c_str());
        }
        director_.SetAvailable(names.data(), names.size());
    }

    void Run(const std::vector<Action>& actions, int64_t duration_ms) {
        size_t action_idx = 0;
        const int total_frames = static_cast<int>((duration_ms * 30) / 1000);

        for (int frame = 0; frame <= total_frames; ++frame) {
            const int64_t now_ms = (static_cast<int64_t>(frame) * 1000) / 30;

            while (action_idx < actions.size() && actions[action_idx].at_ms <= now_ms) {
                const auto& act = actions[action_idx++];
                director_.SetNow(now_ms);
                switch (act.type) {
                    case Action::Type::kStatus:
                        if (business_status_ != act.status)
                            presentation_budget_ = 30;
                        business_status_ = act.status;
                        director_.OnStatus(act.status);
                        break;
                    case Action::Type::kEmotion:
                        director_.OnEmotion(act.str_arg);
                        break;
                    case Action::Type::kUserSpeech:
                        director_.OnUserSpeech();
                        break;
                    case Action::Type::kPowerSave:
                        power_save_ = act.bool_arg;
                        director_.OnPowerSave(act.bool_arg);
                        break;
                    case Action::Type::kNotification:
                        director_.OnNotification(act.until_ms, act.str_arg);
                        break;
                    case Action::Type::kInteraction:
                        std::printf("# interaction %lld %s %s %d\n", static_cast<long long>(now_ms),
                                    act.interaction_label, act.str_arg,
                                    director_.OnInteraction(act.str_arg));
                        break;
                }
            }

            director_.SetNow(now_ms);
            if (has_selected_ && event_pending_) {
                director_.OnClipEvent(pending_event_);
            }
            const auto next = director_.Step(now_ms);
            event_pending_ = false;

            if (!has_selected_ || next.seq != directive_.seq) {
                const ClipMeta* cm = atlas_.Find(next.clip);
                if (!cm) {
                    std::fprintf(stderr, "Missing clip in atlas: %s\n",
                                 next.clip ? next.clip : "(null)");
                    std::exit(1);
                }
                const bool previous_geometry =
                    GrokBotFace::Supports(directive_.clip) || face_exit_until_ > now_ms;
                const bool bridge_to_icon = has_selected_ && previous_geometry &&
                                            GrokBotFace::AtlasBridgeSupported(next.clip);
                if (has_selected_ && !bridge_to_icon &&
                    !(previous_geometry && GrokBotFace::Supports(next.clip)))
                    fade_step_ = 0;
                else
                    fade_step_ = kGrokBotFadeFrames;
                face_exit_until_ = bridge_to_icon ? now_ms + 350 : 0;
                current_clip_ = cm;
                directive_ = next;
                if (bridge_to_icon)
                    directive_.from_frame_zero = true;
                const int upper = GrokBotFrameUpper(current_clip_->frames, directive_.max_frames);
                current_index_ = GrokBotFrameLower(current_clip_->start_frame, upper,
                                                   directive_.from_frame_zero);
                reverse_ = false;
                has_selected_ = true;
                once_done_ = false;
                presentation_budget_ = 30;
            }

            const bool face_exiting = now_ms < face_exit_until_;
            const int face_mode = face_exiting ? 2 : GrokBotFace::Supports(directive_.clip) ? 1 : 0;
            const bool voice_active = business_status_ == GrokBotDirector::StatusId::kListening ||
                                      business_status_ == GrokBotDirector::StatusId::kStreaming;
            const int fps = power_save_ && std::strcmp(directive_.clip, "sleeping") == 0 &&
                                    fade_step_ >= kGrokBotFadeFrames
                                ? 5
                            : voice_active ? 20
                                           : 30;
            if (fps != previous_fps_)
                presentation_budget_ = 30;
            previous_fps_ = fps;
            const bool present = !once_done_ && presentation_budget_ >= 30;
            if (present) {
                presentation_budget_ -= 30;
                presented_index_ = current_index_;
                presented_fade_step_ = fade_step_;
                presented_face_mode_ = face_mode;
            }
            std::printf("%lld %s %d %u %u %d %d\n", static_cast<long long>(now_ms), directive_.clip,
                        presented_index_, directive_.seq, presented_fade_step_,
                        presented_face_mode_, present);
            // 30Hz output quantizes the 20fps cadence to alternating 33/67ms gaps (~16ms error).
            presentation_budget_ += fps;

            if (present && fade_step_ < kGrokBotFadeFrames)
                ++fade_step_;
            if (!present || face_exiting)
                continue;

            const int target = current_index_;
            const int upper = GrokBotFrameUpper(current_clip_->frames, directive_.max_frames);
            const int lower =
                GrokBotFrameLower(current_clip_->start_frame, upper, directive_.from_frame_zero);
            const auto step =
                GrokBotNextFrame(target, reverse_, lower, upper,
                                 directive_.mode == GrokBotDirector::PlayMode::kLoopPingPong,
                                 directive_.mode == GrokBotDirector::PlayMode::kOnceForward);
            if (step.cycle_boundary || step.once_done) {
                pending_event_ = step.once_done ? GrokBotDirector::ClipEvent::kOnceDone
                                                : GrokBotDirector::ClipEvent::kCycleBoundary;
                event_pending_ = true;
            }
            once_done_ = step.once_done;
            reverse_ = step.reverse;
            current_index_ = step.index;
        }
    }

private:
    const AtlasMeta& atlas_;
    GrokBotDirector director_;
    GrokBotDirector::Directive directive_{nullptr, GrokBotDirector::PlayMode::kLoopPingPong, 0, 0};
    GrokBotDirector::ClipEvent pending_event_ = GrokBotDirector::ClipEvent::kCycleBoundary;
    const ClipMeta* current_clip_ = nullptr;
    int current_index_ = 0;
    bool reverse_ = false;
    bool has_selected_ = false;
    bool event_pending_ = false;
    bool once_done_ = false;
    bool power_save_ = false;
    GrokBotDirector::StatusId business_status_ = GrokBotDirector::StatusId::kIdle;
    int presentation_budget_ = 30;
    int previous_fps_ = 30;
    int presented_index_ = 0;
    unsigned presented_fade_step_ = kGrokBotFadeFrames;
    int presented_face_mode_ = 0;
    unsigned fade_step_ = kGrokBotFadeFrames;
    int64_t face_exit_until_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string scenario = "boot_idle";
    std::string atlas_path = "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas";
    uint32_t seed = 42;
    bool face_raw = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--face-raw") {
            face_raw = true;
        } else if (arg == "--atlas" && i + 1 < argc) {
            atlas_path = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--scenario" && i + 1 < argc) {
            scenario = argv[++i];
        } else if (arg[0] != '-') {
            scenario = arg;
        } else {
            std::fprintf(stderr,
                         "Usage: %s [boot_idle|conversation|wake_from_sleep|interactions] "
                         "[--atlas path] [--seed N]\n",
                         argv[0]);
            return 1;
        }
    }

    AtlasMeta atlas;
    if (!LoadAtlasMeta(atlas_path, &atlas)) {
        std::fprintf(stderr, "Failed to load atlas: %s\n", atlas_path.c_str());
        return 1;
    }
    if (face_raw)
        return RenderFaceFrames(atlas);

    using Status = GrokBotDirector::StatusId;
    std::vector<Action> actions;
    int64_t duration_ms = 0;

    if (scenario == "boot_idle") {
        // Boot into idle at 0ms, then stay idle for 330s with minute-clock status refreshes.
        actions.push_back({0, Action::Type::kStatus, Status::kIdle});
        for (int64_t t = 60000; t <= 300000; t += 60000) {
            actions.push_back({t, Action::Type::kStatus, Status::kIdle});
        }
        duration_ms = 330000;
    } else if (scenario == "conversation") {
        // Boot -> idle -> wake/listening -> user speech -> thinking chain (6s+) ->
        // streaming with happy & curious emotions -> tts stop -> listening -> idle + 40s idle.
        actions.push_back({0, Action::Type::kStatus, Status::kIdle});
        actions.push_back({3500, Action::Type::kStatus, Status::kListening});
        actions.push_back({5500, Action::Type::kUserSpeech});
        // 6s thinking chain (5500ms -> 11500ms)
        actions.push_back({11500, Action::Type::kStatus, Status::kStreaming});
        actions.push_back({12500, Action::Type::kEmotion, Status::kIdle, "happy"});
        actions.push_back({16500, Action::Type::kEmotion, Status::kIdle, "curious"});
        // TTS stop -> auto-listening -> session end idle -> 40s idle
        actions.push_back({21500, Action::Type::kStatus, Status::kListening});
        actions.push_back({24000, Action::Type::kStatus, Status::kIdle});
        duration_ms = 64000;
    } else if (scenario == "wake_from_sleep") {
        // Sleep -> wake/listen/echo/greeting; then sleep -> wake/echo/real user transcript.
        actions.push_back({0, Action::Type::kStatus, Status::kIdle});
        actions.push_back({3500, Action::Type::kPowerSave, Status::kIdle, nullptr, true});
        actions.push_back({10000, Action::Type::kPowerSave, Status::kIdle, nullptr, false});
        actions.push_back({10300, Action::Type::kStatus, Status::kListening});
        actions.push_back({10300, Action::Type::kUserSpeech});
        actions.push_back({10350, Action::Type::kStatus, Status::kStreaming});
        actions.push_back({10400, Action::Type::kEmotion, Status::kIdle, "happy"});
        actions.push_back({14000, Action::Type::kStatus, Status::kIdle});
        actions.push_back({15000, Action::Type::kPowerSave, Status::kIdle, nullptr, true});
        actions.push_back({21000, Action::Type::kStatus, Status::kLoading});
        actions.push_back({21300, Action::Type::kStatus, Status::kListening});
        actions.push_back({21300, Action::Type::kUserSpeech});
        actions.push_back({22900, Action::Type::kUserSpeech});
        actions.push_back({28000, Action::Type::kStatus, Status::kIdle});
        duration_ms = 30000;
    } else if (scenario == "interactions") {
        // Simulated inputs use the board's emotion mapping, not recorded sensor readings.
        actions.push_back({0, Action::Type::kStatus, Status::kIdle});
        actions.push_back(
            {4000, Action::Type::kInteraction, Status::kIdle, "happy", false, 0, "tap"});
        actions.push_back(
            {10000, Action::Type::kInteraction, Status::kIdle, "loving", false, 0, "stroke"});
        actions.push_back(
            {16000, Action::Type::kInteraction, Status::kIdle, "confused", false, 0, "shake"});
        actions.push_back(
            {22000, Action::Type::kInteraction, Status::kIdle, "curious", false, 0, "tilt"});
        actions.push_back({28000, Action::Type::kStatus, Status::kListening});
        actions.push_back(
            {28500, Action::Type::kInteraction, Status::kIdle, "happy", false, 0, "tap"});
        actions.push_back({31000, Action::Type::kStatus, Status::kIdle});
        duration_ms = 35000;
    } else {
        std::fprintf(stderr, "Unknown scenario: %s\n", scenario.c_str());
        return 1;
    }

    Simulator sim(atlas, seed);
    sim.Run(actions, duration_ms);
    return 0;
}
