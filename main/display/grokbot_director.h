#pragma once

#include <cstddef>
#include <cstdint>

class GrokBotDirector {
public:
    enum class StatusId {
        kIdle,
        kListening,
        kStreaming,
        kLoading,
        kRadar,
        kWaiting,
        kSearching,
        kWaking,
        kApproving,
        kUploading,
        kReceiving,
        kFailed,
        kHappy
    };
    enum class PlayMode { kLoopPingPong, kOnceForward, kOncePingPong };
    enum class ClipEvent { kCycleBoundary, kOnceDone };
    struct Directive {
        const char* clip;
        PlayMode mode;
        uint16_t max_frames;
        uint32_t seq;
        bool from_frame_zero = false;
    };

    explicit GrokBotDirector(uint32_t seed = 1);
    static uint64_t AvailabilityMask(const char* const* names, size_t count);
    void SetAvailable(uint64_t mask);
    void SetAvailable(const char* const* names, size_t count);
    void SetNow(int64_t now_ms) {
        if (now_ms > now_)
            now_ = now_ms;
    }
    void OnStatus(StatusId status);
    bool OnEmotion(const char* emotion);
    void OnUserSpeech();
    void OnNotification(int64_t until_ms, const char* clip = "notifying");
    void OnPowerSave(bool on);
    void OnClipEvent(ClipEvent event);
    Directive Step(int64_t now_ms);

private:
    enum class Layer { kBase, kTransition, kFidget, kEmotion, kNotification, kThinking };
    uint32_t Random();
    bool Available(const char* clip) const;
    const char* Fallback(const char* clip) const;
    Directive Select(const char* clip, PlayMode mode, uint16_t max_frames, Layer layer,
                     bool from_frame_zero = false);
    void StartTransition(const char* clip, uint16_t max_frames = 0);
    void ResetIdle(bool after_conversation);
    void ScheduleFidget();
    const char* PickFidget();

    uint64_t available_ = UINT64_MAX;
    uint32_t random_;
    int64_t now_ = 0;
    int64_t idle_since_ = -20000;
    int64_t listening_since_ = 0;
    int64_t speech_since_ = -1;
    int64_t wake_echo_until_ = 0;
    int64_t next_fidget_ = 0;
    int64_t idle_pause_until_ = 0;
    int64_t notification_until_ = 0;
    int64_t emotion_until_ = 0;
    int64_t emotion_received_ = -2000;
    int64_t rare_last_[3] = {-90000, -120000, -180000};
    int emotion_group_ = -1;
    const char* emotion_ = nullptr;
    const char* previous_variant_ = nullptr;
    const char* transition_ = nullptr;
    const char* fidget_ = nullptr;
    const char* notification_clip_ = "notifying";
    const char* recent_[2] = {};
    uint16_t transition_max_ = 0;
    StatusId status_ = StatusId::kLoading;
    bool ready_ = false;
    bool emotion_system_ = false;
    bool session_spoke_ = false;
    bool power_save_ = false;
    bool drowsy_ = false;
    bool sleeping_ = false;
    bool cycle_ = false;
    unsigned thinking_cycle_ = 0;
    unsigned idle_breaths_ = 0;
    Layer layer_ = Layer::kBase;
    Directive current_{nullptr, PlayMode::kLoopPingPong, 0, 0};
};
