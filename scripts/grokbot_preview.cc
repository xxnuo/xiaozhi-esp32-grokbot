#include "grokbot_director.h"
#include "grokbot_player.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

struct Action {
    int64_t at_ms;
    enum class Type { kStatus, kEmotion, kUserSpeech, kPowerSave, kNotification } type;
    GrokBotDirector::StatusId status = GrokBotDirector::StatusId::kIdle;
    const char* str_arg = nullptr;
    bool bool_arg = false;
    int64_t until_ms = 0;
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
                if (has_selected_)
                    fade_step_ = 0;
                current_clip_ = cm;
                directive_ = next;
                const int upper = GrokBotFrameUpper(current_clip_->frames, directive_.max_frames);
                current_index_ = GrokBotFrameLower(current_clip_->start_frame, upper,
                                                   directive_.from_frame_zero);
                reverse_ = false;
                has_selected_ = true;
                once_done_ = false;
                held_frames_ = 0;
            }

            std::printf("%lld %s %d %u\n", static_cast<long long>(now_ms), directive_.clip,
                        current_index_, directive_.seq);

            const int repeats = power_save_ && std::strcmp(directive_.clip, "sleeping") == 0 &&
                                        fade_step_ >= kGrokBotFadeFrames
                                    ? 6
                                    : 1;
            if (fade_step_ < kGrokBotFadeFrames)
                ++fade_step_;
            if (once_done_ || ++held_frames_ < repeats) {
                continue;
            }
            held_frames_ = 0;

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
    int held_frames_ = 0;
    unsigned fade_step_ = kGrokBotFadeFrames;
};

}  // namespace

int main(int argc, char** argv) {
    std::string scenario = "boot_idle";
    std::string atlas_path = "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas";
    uint32_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--atlas" && i + 1 < argc) {
            atlas_path = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--scenario" && i + 1 < argc) {
            scenario = argv[++i];
        } else if (arg[0] != '-') {
            scenario = arg;
        } else {
            std::fprintf(
                stderr,
                "Usage: %s [boot_idle|conversation|wake_from_sleep] [--atlas path] [--seed N]\n",
                argv[0]);
            return 1;
        }
    }

    AtlasMeta atlas;
    if (!LoadAtlasMeta(atlas_path, &atlas)) {
        std::fprintf(stderr, "Failed to load atlas: %s\n", atlas_path.c_str());
        return 1;
    }

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
    } else {
        std::fprintf(stderr, "Unknown scenario: %s\n", scenario.c_str());
        return 1;
    }

    Simulator sim(atlas, seed);
    sim.Run(actions, duration_ms);
    return 0;
}
