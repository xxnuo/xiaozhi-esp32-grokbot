#include "grokbot_director.h"

#include <cstring>
#include <initializer_list>

namespace {
using Mode = GrokBotDirector::PlayMode;
constexpr const char* kClips[] = {
    "idle",     "listening",  "streaming", "loading",      "radar",   "waiting",   "searching",
    "waking",   "approving",  "uploading", "failed",       "happy",   "laughing",  "playful",
    "shy",      "proud",      "sad",       "sleeping",     "angry",   "surprised", "thinking",
    "confused", "scared",     "alerting",  "excited",      "curious", "drowsy",    "notifying",
    "humming",  "suspicious", "finished",  "celebrate",    "working", "turntable", "logo",
    "spawning", "writing",    "receiving", "powering-down"};
bool Equal(const char* a, const char* b) { return a == b || (a && b && std::strcmp(a, b) == 0); }
Mode OnceMode(const char* clip) {
    for (const char* forward :
         {"finished", "waking", "playful", "happy", "excited", "confused", "celebrate", "approving",
          "turntable", "spawning", "powering-down", "notifying"}) {
        if (Equal(clip, forward))
            return Mode::kOnceForward;
    }
    for (const char* pingpong :
         {"curious", "humming", "suspicious", "proud", "shy", "surprised", "drowsy", "logo"}) {
        if (Equal(clip, pingpong))
            return Mode::kOncePingPong;
    }
    return Mode::kLoopPingPong;
}
struct Choice {
    const char* clip;
    unsigned weight;
    uint16_t max_frames = 0;
};
struct Emotion {
    const char* names;
    Choice choices[4];
};
constexpr Emotion kEmotions[] = {
    {"relaxed", {{"humming", 60}, {"idle", 40}}},
    {"happy", {{"happy", 50}, {"excited", 25}, {"celebrate", 15}, {"humming", 10}}},
    {"loving", {{"happy", 45}, {"shy", 35}, {"humming", 20}}},
    {"delicious", {{"happy", 45}, {"playful", 35}, {"excited", 20}}},
    {"laughing", {{"laughing", 55}, {"excited", 25}, {"celebrate", 20}}},
    {"excited", {{"excited", 50}, {"celebrate", 35}, {"playful", 15}}},
    {"funny|silly", {{"playful", 55}, {"laughing", 25}, {"turntable", 20}}},
    {"winking|kissy", {{"playful", 55}, {"shy", 25}, {"happy", 20}}},
    {"confident|cool", {{"proud", 60}, {"turntable", 25}, {"celebrate", 15}}},
    {"embarrassed", {{"shy", 70}, {"confused", 30}}},
    {"curious", {{"curious", 65}, {"suspicious", 35}}},
    {"thinking", {{"curious", 45}, {"suspicious", 30}, {"thinking", 25}}},
    {"confused", {{"confused", 60}, {"suspicious", 40}}},
    {"suspicious", {{"suspicious", 75}, {"curious", 25}}},
    {"surprised|shocked", {{"surprised", 65}, {"scared", 25}, {"waking", 10}}},
    {"scared", {{"scared", 75}, {"surprised", 25}}},
    {"angry", {{"angry", 75}, {"suspicious", 25}}},
    {"sad|crying", {{"sad", 70}, {"shy", 30}}},
    {"sleepy", {{"sleeping", 70}, {"drowsy", 30}}},
    {"drowsy|low_battery", {{"drowsy", 100}}},
    {"warning", {{"alerting", 100}}},
    {"gear", {{"loading", 100}}},
    {"cloud_download|download", {{"receiving", 100}}},
    {"cancel|cloud_off", {{"failed", 100}}},
    {"link", {{"approving", 100}}},
    {"battery_connected|waking", {{"waking", 100}}},
    {"shy", {{"shy", 100}}},
    {"proud", {{"proud", 100}}},
    {"sleeping", {{"sleeping", 100}}},
    {"notifying", {{"notifying", 100}}}};
bool Matches(const char* names, const char* name) {
    const size_t length = std::strlen(name);
    for (const char* p = names; p;) {
        const char* end = std::strchr(p, '|');
        const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (n == length && std::strncmp(p, name, n) == 0)
            return true;
        p = end ? end + 1 : nullptr;
    }
    return false;
}
}  // namespace

GrokBotDirector::GrokBotDirector(uint32_t seed) : random_(seed ? seed : 1) { ScheduleFidget(); }
uint32_t GrokBotDirector::Random() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
}
uint64_t GrokBotDirector::AvailabilityMask(const char* const* names, size_t count) {
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(kClips) / sizeof(*kClips); ++i)
        for (size_t j = 0; j < count; ++j)
            if (Equal(kClips[i], names[j]))
                mask |= uint64_t{1} << i;
    return mask;
}
void GrokBotDirector::SetAvailable(uint64_t mask) {
    available_ = mask;
    if (transition_ && !Available(transition_))
        transition_ = nullptr;
    if (fidget_ && !Available(fidget_)) {
        fidget_ = nullptr;
        ScheduleFidget();
    }
    if (emotion_)
        emotion_ = Fallback(emotion_);
}
void GrokBotDirector::SetAvailable(const char* const* names, size_t count) {
    SetAvailable(AvailabilityMask(names, count));
}
bool GrokBotDirector::Available(const char* clip) const {
    for (size_t i = 0; i < sizeof(kClips) / sizeof(*kClips); ++i)
        if (Equal(kClips[i], clip))
            return (available_ & (uint64_t{1} << i)) != 0;
    return false;
}
const char* GrokBotDirector::Fallback(const char* clip) const {
    if (Available(clip))
        return clip;
    if (Equal(clip, "working") || Equal(clip, "writing")) {
        if (Available("thinking"))
            return "thinking";
    }
    if (Equal(clip, "receiving") && Available("uploading"))
        return "uploading";
    if (Available("idle"))
        return "idle";
    for (const char* name : kClips)
        if (Available(name))
            return name;
    return nullptr;
}
GrokBotDirector::Directive GrokBotDirector::Select(const char* clip, PlayMode mode,
                                                   uint16_t max_frames, Layer layer,
                                                   bool from_frame_zero) {
    clip = Fallback(clip);
    if (!Equal(clip, current_.clip) || mode != current_.mode || max_frames != current_.max_frames ||
        layer != layer_ || from_frame_zero != current_.from_frame_zero) {
        current_ = {clip, mode, max_frames, current_.seq + 1, from_frame_zero};
        layer_ = layer;
    }
    return current_;
}
void GrokBotDirector::StartTransition(const char* clip, uint16_t max_frames) {
    if (Equal(clip, "waking") && Available(clip) && !Equal(transition_, "waking"))
        wake_echo_until_ = now_ + 1500;
    transition_ = Available(clip) ? clip : nullptr;
    transition_max_ = max_frames;
    cycle_ = false;
}
void GrokBotDirector::ResetIdle(bool after_conversation) {
    idle_since_ = now_ - (after_conversation ? 0 : 20000);
    drowsy_ = false;
    sleeping_ = false;
    idle_pause_until_ = 0;
    idle_breaths_ = 0;
    ScheduleFidget();
}
void GrokBotDirector::ScheduleFidget() {
    const int64_t age = now_ - idle_since_;
    const int low = age < 20000 ? 6000 : age < 120000 ? 10000 : 8000;
    const int span = age < 20000 ? 5000 : age < 120000 ? 8000 : 7000;
    next_fidget_ = now_ + low + Random() % (span + 1);
}
void GrokBotDirector::OnStatus(StatusId status) {
    if (status_ == status && (ready_ || status != StatusId::kIdle))
        return;
    const bool first_idle = !ready_ && status == StatusId::kIdle;
    if (status == StatusId::kIdle)
        ready_ = true;
    const bool keep_waking = Equal(transition_, "waking");
    const bool wake = keep_waking || sleeping_ || power_save_ || Equal(current_.clip, "sleeping") ||
                      (status_ == StatusId::kIdle && now_ - idle_since_ >= 120000);
    const bool finished = status == StatusId::kIdle &&
                          (session_spoke_ || status_ == StatusId::kStreaming ||
                           (status_ == StatusId::kListening && now_ - listening_since_ >= 1500));
    status_ = status;
    transition_ = nullptr;
    fidget_ = nullptr;
    cycle_ = false;
    if (status != StatusId::kStreaming) {
        emotion_ = nullptr;
        emotion_group_ = -1;
    }
    speech_since_ = -1;
    if (status == StatusId::kListening) {
        listening_since_ = now_;
        wake_echo_until_ = now_ + 1500;
    }
    if (status == StatusId::kStreaming) {
        session_spoke_ = true;
        power_save_ = false;
        drowsy_ = false;
        sleeping_ = false;
    } else if (status == StatusId::kIdle || status == StatusId::kFailed) {
        session_spoke_ = false;
    }
    if (keep_waking && (status == StatusId::kIdle || status == StatusId::kListening ||
                        status == StatusId::kStreaming || status == StatusId::kLoading)) {
        transition_ = "waking";
        if (status == StatusId::kIdle)
            ResetIdle(!first_idle);
        return;
    }
    if (status == StatusId::kIdle) {
        ResetIdle(!first_idle);
        if (first_idle)
            StartTransition("logo", 57);
        else if (wake)
            StartTransition("waking", 73);
        else if (finished)
            StartTransition("finished", 30);
    } else if (wake && (status == StatusId::kListening || status == StatusId::kLoading)) {
        power_save_ = false;
        drowsy_ = false;
        sleeping_ = false;
        StartTransition("waking", 73);
    } else if (status == StatusId::kFailed)
        StartTransition("confused");
}
bool GrokBotDirector::OnEmotion(const char* emotion) {
    if (!emotion)
        return false;
    if (Equal(emotion, "neutral") || Equal(emotion, "robot_2") || Equal(emotion, "idle")) {
        emotion_ = nullptr;
        emotion_group_ = -1;
        return true;
    }
    for (size_t i = 0; i < sizeof(kEmotions) / sizeof(*kEmotions); ++i) {
        if (!Matches(kEmotions[i].names, emotion))
            continue;
        if (emotion_ && emotion_group_ == static_cast<int>(i) && now_ - emotion_received_ < 2000) {
            emotion_until_ = now_ + 8000;
            return true;
        }
        unsigned total = 0, candidates = 0;
        for (const auto& c : kEmotions[i].choices)
            if (c.clip && Available(c.clip))
                ++candidates;
        for (const auto& c : kEmotions[i].choices)
            if (c.clip && Available(c.clip) &&
                (candidates <= 1 || !Equal(c.clip, previous_variant_)))
                total += c.weight;
        unsigned roll = total ? Random() % total : 0;
        emotion_ = Fallback(kEmotions[i].choices[0].clip);
        for (const auto& c : kEmotions[i].choices) {
            if (!c.clip || !Available(c.clip) ||
                (candidates > 1 && Equal(c.clip, previous_variant_)))
                continue;
            if (roll < c.weight) {
                emotion_ = c.clip;
                break;
            }
            roll -= c.weight;
        }
        previous_variant_ = emotion_;
        emotion_group_ = static_cast<int>(i);
        emotion_system_ = i >= 20 && i <= 24;
        emotion_received_ = now_;
        emotion_until_ = now_ + 8000;
        if (status_ == StatusId::kIdle) {
            ResetIdle(true);
            fidget_ = nullptr;
        }
        return true;
    }
    return false;
}
bool GrokBotDirector::OnInteraction(const char* emotion) {
    if (status_ != StatusId::kIdle || power_save_ || notification_until_ > now_ ||
        !OnEmotion(emotion)) {
        return false;
    }
    ResetIdle(true);
    fidget_ = nullptr;
    transition_ = nullptr;
    emotion_until_ = now_ + 2500;
    return true;
}
void GrokBotDirector::OnUserSpeech() {
    // The wake-word transcript is echoed as a user message before the greeting.
    if (now_ < wake_echo_until_)
        return;
    if (!Equal(transition_, "waking"))
        transition_ = nullptr;
    fidget_ = nullptr;
    emotion_ = nullptr;
    emotion_group_ = -1;
    if (status_ == StatusId::kListening) {
        speech_since_ = now_;
        thinking_cycle_ = 0;
    }
    if (status_ == StatusId::kIdle)
        ResetIdle(true);
    drowsy_ = false;
    sleeping_ = false;
    power_save_ = false;
}
void GrokBotDirector::OnNotification(int64_t until_ms, const char* clip) {
    notification_until_ = until_ms;
    notification_clip_ = "notifying";
    if (clip) {
        for (const char* name : kClips)
            if (Equal(name, clip))
                notification_clip_ = name;
    }
    if (layer_ == Layer::kNotification)
        ++current_.seq;
}
void GrokBotDirector::OnPowerSave(bool on) {
    if (power_save_ == on)
        return;
    power_save_ = on;
    fidget_ = nullptr;
    emotion_ = nullptr;
    emotion_group_ = -1;
    drowsy_ = false;
    if (on) {
        StartTransition("drowsy", 42);
        sleeping_ = !transition_;
    } else {
        sleeping_ = false;
        ResetIdle(true);
        StartTransition("waking", 73);
    }
}
void GrokBotDirector::OnClipEvent(ClipEvent event) {
    if (event == ClipEvent::kOnceDone) {
        if (layer_ == Layer::kTransition && Equal(current_.clip, transition_)) {
            if (power_save_)
                sleeping_ = true;
            transition_ = nullptr;
        } else if (layer_ == Layer::kThinking) {
            ++thinking_cycle_;
            ++current_.seq;  // Older atlases can map consecutive stages to the same clip.
        } else if (layer_ == Layer::kNotification) {
            notification_until_ = 0;
        } else if (layer_ == Layer::kEmotion && Equal(current_.clip, emotion_)) {
            emotion_ = nullptr;
            emotion_group_ = -1;
        } else if (layer_ == Layer::kFidget && Equal(current_.clip, fidget_)) {
            fidget_ = nullptr;
            idle_pause_until_ = 0;
            idle_breaths_ = 0;
            ScheduleFidget();
        }
    } else {
        cycle_ = true;
    }
}
const char* GrokBotDirector::PickFidget() {
    static constexpr Choice early[] = {
        {"humming", 35}, {"curious", 30}, {"happy", 20}, {"playful", 15}};
    static constexpr Choice normal[] = {{"curious", 24},        {"humming", 22},  {"playful", 16},
                                        {"suspicious", 14, 90}, {"proud", 9, 75}, {"shy", 6, 90},
                                        {"turntable", 5},       {"celebrate", 3}, {"logo", 1, 57}};
    static constexpr Choice late[] = {{"drowsy", 55}, {"humming", 30}, {"shy", 15, 90}};
    const int64_t age = now_ - idle_since_;
    const Choice* choices = age < 20000 ? early : age < 120000 ? normal : late;
    const size_t count = age < 20000 ? 4 : age < 120000 ? 9 : 3;
    auto eligible = [&](const Choice& c) {
        if (!Available(c.clip))
            return false;
        if (Equal(c.clip, "turntable"))
            return now_ - rare_last_[0] >= 90000;
        if (Equal(c.clip, "celebrate"))
            return now_ - rare_last_[1] >= 120000;
        if (Equal(c.clip, "logo"))
            return now_ - rare_last_[2] >= 180000;
        return true;
    };
    size_t candidates = 0;
    for (size_t i = 0; i < count; ++i)
        if (eligible(choices[i]))
            ++candidates;
    unsigned total = 0;
    for (size_t i = 0; i < count; ++i)
        if (eligible(choices[i]) && (candidates <= 2 || (!Equal(choices[i].clip, recent_[0]) &&
                                                         !Equal(choices[i].clip, recent_[1]))))
            total += choices[i].weight;
    if (!total)
        return nullptr;
    unsigned roll = Random() % total;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = choices[i];
        if (!eligible(c) ||
            (candidates > 2 && (Equal(c.clip, recent_[0]) || Equal(c.clip, recent_[1]))))
            continue;
        if (roll >= c.weight) {
            roll -= c.weight;
            continue;
        }
        recent_[1] = recent_[0];
        recent_[0] = c.clip;
        for (int r = 0; r < 3; ++r)
            if (Equal(c.clip, r == 0 ? "turntable" : r == 1 ? "celebrate" : "logo"))
                rare_last_[r] = now_;
        transition_max_ = c.max_frames;
        return c.clip;
    }
    return nullptr;
}
GrokBotDirector::Directive GrokBotDirector::Step(int64_t now_ms) {
    SetNow(now_ms);
    const bool boundary = cycle_;
    cycle_ = false;
    if (emotion_ && now_ >= emotion_until_) {
        if (status_ == StatusId::kStreaming) {
            if (boundary || now_ >= emotion_until_ + 2000) {
                emotion_ = nullptr;
                emotion_group_ = -1;
            }
        } else if (!emotion_system_) {
            emotion_ = nullptr;
            emotion_group_ = -1;
        }
    }
    const bool wake_transition = Equal(transition_, "waking");
    const bool failure_transition = status_ == StatusId::kFailed && Equal(transition_, "confused");
    if (failure_transition || wake_transition)
        return Select(transition_, OnceMode(transition_), transition_max_, Layer::kTransition);
    const char* business = nullptr;
    switch (status_) {
        case StatusId::kLoading:
            business = "loading";
            break;
        case StatusId::kRadar:
            business = "radar";
            break;
        case StatusId::kWaiting:
            business = "waiting";
            break;
        case StatusId::kSearching:
            business = "searching";
            break;
        case StatusId::kWaking:
            business = "waking";
            break;
        case StatusId::kApproving:
            business = "approving";
            break;
        case StatusId::kUploading:
            business = "uploading";
            break;
        case StatusId::kReceiving:
            business = "receiving";
            break;
        case StatusId::kFailed:
            business = "failed";
            break;
        case StatusId::kHappy:
            business = "happy";
            break;
        default:
            break;
    }
    if (business)
        return Select(business, PlayMode::kLoopPingPong, 0, Layer::kBase);
    if (status_ == StatusId::kListening) {
        if (speech_since_ < 0)
            return Select("listening", PlayMode::kLoopPingPong, 0, Layer::kBase);
        const char* clip = thinking_cycle_ == 0             ? "thinking"
                           : thinking_cycle_ == 1           ? "working"
                           : (thinking_cycle_ - 2) % 3 == 0 ? "searching"
                           : (thinking_cycle_ - 2) % 3 == 1 ? "writing"
                                                            : "working";
        // Returning to the shared frame zero joins thought clips without a brightness jump.
        return Select(clip, PlayMode::kOncePingPong, thinking_cycle_ < 2 ? 31 : 0, Layer::kThinking,
                      true);
    }
    if (status_ == StatusId::kStreaming) {
        const char* clip = (emotion_ && !Equal(emotion_, "idle")) ? emotion_ : "streaming";
        return Select(clip, PlayMode::kLoopPingPong, 0, Layer::kBase);
    }
    if (transition_ && !Equal(transition_, "finished"))
        return Select(transition_, OnceMode(transition_), transition_max_, Layer::kTransition);
    if (notification_until_ > now_)
        return Select(notification_clip_, OnceMode(notification_clip_), 0, Layer::kNotification);
    if (emotion_)
        return Select(emotion_, OnceMode(emotion_), 0, Layer::kEmotion);
    if (transition_)
        return Select(transition_, OnceMode(transition_), transition_max_, Layer::kTransition);
    if (power_save_ || sleeping_)
        return Select("sleeping", PlayMode::kLoopPingPong, 0, Layer::kBase);
    if (fidget_)
        return Select(fidget_, OnceMode(fidget_), transition_max_, Layer::kFidget);
    const int64_t age = now_ - idle_since_;
    if (age >= 240000) {
        if (boundary && layer_ == Layer::kBase) {
            if (age >= 300000 && drowsy_)
                sleeping_ = true;
            drowsy_ = true;
        }
        if (sleeping_ || drowsy_)
            return Select(sleeping_ ? "sleeping" : "drowsy", PlayMode::kLoopPingPong, 0,
                          Layer::kBase);
    } else if (boundary && layer_ == Layer::kBase && now_ >= next_fidget_) {
        fidget_ = PickFidget();
        if (!fidget_)
            ScheduleFidget();
        else
            return Select(fidget_, OnceMode(fidget_), transition_max_, Layer::kFidget);
    }
    if (layer_ != Layer::kBase || !Equal(current_.clip, "idle")) {
        idle_pause_until_ = 0;
        idle_breaths_ = 0;
    } else if (boundary) {
        if (idle_pause_until_) {
            if (now_ >= idle_pause_until_)
                idle_pause_until_ = 0;
        } else if (++idle_breaths_ >= 2 && Random() % 3 == 0) {
            // Two full breaths before a 0.4-2.8s pause keep breathing above 80% of base time.
            idle_pause_until_ = now_ + (1 + Random() % 7) * 400;
            idle_breaths_ = 0;
        }
    }
    return Select("idle", PlayMode::kLoopPingPong, idle_pause_until_ ? 15 : 0, Layer::kBase);
}
