#include "grokbot_director.h"
#include "grokbot_player.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                   \
        }                                                                                   \
    } while (0)

using Director = GrokBotDirector;
using Status = Director::StatusId;
using Event = Director::ClipEvent;
using Mode = Director::PlayMode;

bool Is(const Director::Directive& d, const char* name) {
    return d.clip && std::strcmp(d.clip, name) == 0;
}
void Boot(Director& d) {
    auto pre = d.Step(0);
    CHECK(Is(pre, "loading") && pre.mode == Mode::kLoopPingPong);
    d.OnStatus(Status::kIdle);
    auto first = d.Step(0);
    CHECK(Is(first, "logo") && first.max_frames == 57 && first.mode == Mode::kOncePingPong);
    d.OnClipEvent(Event::kOnceDone);
    auto idle = d.Step(0);
    CHECK(Is(idle, "idle") && idle.max_frames == 0);
}
void Stages() {
    Director d;
    Boot(d);
    for (int64_t t = 20000; t <= 200000; t += 20000) {
        d.SetNow(t);
        d.OnStatus(Status::kIdle);  // Includes the minute clock's SetStatus("HH:MM").
        auto cur = d.Step(t);
        CHECK(Is(cur, "idle"));
    }
    auto s1 = d.Step(219999);
    CHECK(Is(s1, "idle"));
    auto s2 = d.Step(220000);
    CHECK(Is(s2, "idle"));  // Must wait for kCycleBoundary before entering drowsy.
    d.OnClipEvent(Event::kCycleBoundary);
    auto s3 = d.Step(220000);
    CHECK(Is(s3, "drowsy"));
    auto s4 = d.Step(280000);
    CHECK(Is(s4, "drowsy"));
    d.OnClipEvent(Event::kCycleBoundary);
    auto s5 = d.Step(280001);
    CHECK(Is(s5, "sleeping"));
    d.OnUserSpeech();
    auto s6 = d.Step(280001);
    CHECK(Is(s6, "idle"));
    auto s7 = d.Step(520000);
    CHECK(Is(s7, "idle"));
    auto s8 = d.Step(520001);
    CHECK(Is(s8, "idle"));
    d.OnClipEvent(Event::kCycleBoundary);
    auto s9 = d.Step(520001);
    CHECK(Is(s9, "drowsy"));

    // Crossing 240s while a fidget is playing must not interrupt the fidget.
    Director f(1);
    Boot(f);
    f.Step(219000);
    f.OnClipEvent(Event::kCycleBoundary);
    auto fidget = f.Step(219000);
    CHECK(!Is(fidget, "idle"));
    auto during = f.Step(220500);
    CHECK(std::strcmp(during.clip, fidget.clip) == 0);
    f.OnClipEvent(Event::kOnceDone);
    auto after_fidget = f.Step(220500);
    CHECK(Is(after_fidget, "idle"));
    f.OnClipEvent(Event::kCycleBoundary);
    auto after_boundary = f.Step(220500);
    CHECK(Is(after_boundary, "drowsy"));
}
void IdleBreathing();
void Fidgets() {
    Director a(123), b(123);
    Boot(a);
    Boot(b);
    const char* recent[2] = {};
    int count = 0;
    int64_t last_fidget = -18000;
    for (int64_t t = 0; t < 95000; t += 100) {
        auto first = a.Step(t), second = b.Step(t);
        CHECK(first.seq == second.seq && std::strcmp(first.clip, second.clip) == 0);
        CHECK(Is(first, "idle"));  // Wall time alone cannot start a fidget.
        a.OnClipEvent(Event::kCycleBoundary);
        b.OnClipEvent(Event::kCycleBoundary);
        first = a.Step(t);
        second = b.Step(t);
        CHECK(first.seq == second.seq && std::strcmp(first.clip, second.clip) == 0);
        if (Is(first, "idle"))
            continue;
        CHECK(t - last_fidget >= 10000);
        CHECK(!recent[0] || std::strcmp(first.clip, recent[0]) != 0);
        CHECK(!recent[1] || std::strcmp(first.clip, recent[1]) != 0);
        if (Is(first, "suspicious") || Is(first, "shy"))
            CHECK(first.max_frames == 90);
        if (Is(first, "proud"))
            CHECK(first.max_frames == 75);
        if (Is(first, "logo"))
            CHECK(first.max_frames == 57);
        recent[1] = recent[0];
        recent[0] = first.clip;
        ++count;
        last_fidget = t;
        a.OnClipEvent(Event::kOnceDone);
        b.OnClipEvent(Event::kOnceDone);
        auto next_a = a.Step(t), next_b = b.Step(t);
        CHECK(Is(next_a, "idle"));
        CHECK(Is(next_b, "idle"));
    }
    CHECK(count >= 5);
    IdleBreathing();
    bool saw_late_shy = false;
    for (uint32_t seed = 1; seed <= 10; ++seed) {
        Director late(seed);
        Boot(late);
        int64_t last_done = -1;
        for (int64_t t = 100000; t <= 180000; t += 400) {
            late.Step(t);
            late.OnClipEvent(Event::kCycleBoundary);
            const auto clip = late.Step(t);
            if (Is(clip, "idle"))
                continue;
            CHECK(Is(clip, "drowsy") || Is(clip, "humming") || Is(clip, "shy"));
            if (last_done >= 0)
                CHECK(t - last_done >= 8000 && t - last_done <= 15400);
            if (Is(clip, "shy")) {
                CHECK(clip.max_frames == 90);
                saw_late_shy = true;
            }
            late.OnClipEvent(Event::kOnceDone);
            late.Step(t);
            last_done = t;
        }
    }
    CHECK(saw_late_shy);
    Director early(7);
    Boot(early);
    early.OnUserSpeech();
    for (int64_t t = 0; t <= 11000; t += 100) {
        early.OnClipEvent(Event::kCycleBoundary);
        auto clip = early.Step(t);
        if (!Is(clip, "idle")) {
            CHECK(t >= 6000);
            CHECK(Is(clip, "humming") || Is(clip, "curious") || Is(clip, "happy") ||
                  Is(clip, "playful"));
            return;
        }
    }
    CHECK(false);
}
void IdleBreathing() {
    bool saw_pause = false, saw_varied_pause = false;
    int first_pause_frames = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        Director d(seed);
        const char* available[] = {"idle"};
        d.SetAvailable(available, 1);
        d.OnStatus(Status::kIdle);
        Director::Directive current{nullptr, Mode::kLoopPingPong, 0, 0};
        int index = 2, breath_frames = 0, pause_frames = 0, pause_length = 0;
        bool reverse = false, pending = false;
        for (int frame = 0; frame < 6000; ++frame) {
            d.SetNow(frame * 1000LL / 30);
            if (pending)
                d.OnClipEvent(Event::kCycleBoundary);
            const auto next = d.Step(frame * 1000LL / 30);
            CHECK(Is(next, "idle"));
            if (next.seq != current.seq) {
                if (current.clip)
                    CHECK(pending);  // Both pause entry and exit must wait for a seam.
                if (current.max_frames == 15) {
                    CHECK(pause_length >= 13 && pause_length <= 85);
                    if (!first_pause_frames)
                        first_pause_frames = pause_length;
                    else if (pause_length != first_pause_frames)
                        saw_varied_pause = true;
                    pause_length = 0;
                }
                current = next;
                index = 2;
                reverse = false;
            }
            if (current.max_frames == 15) {
                saw_pause = true;
                ++pause_frames;
                ++pause_length;
            } else {
                ++breath_frames;
            }
            const auto step = GrokBotNextFrame(
                index, reverse, 2, GrokBotFrameUpper(180, current.max_frames), true, false);
            index = step.index;
            reverse = step.reverse;
            pending = step.cycle_boundary;
        }
        CHECK(breath_frames * 100 >= (breath_frames + pause_frames) * 70);
    }
    CHECK(saw_pause && saw_varied_pause);
}
void RareCooldowns() {
    int seen[3] = {};
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        Director d(seed);
        Boot(d);
        int64_t last[3] = {-90000, -120000, -180000};
        for (int64_t t = 0; t < 2000000; t += 1000) {
            if (t % 100000 == 0) {
                d.SetNow(t);
                d.OnUserSpeech();
            }
            d.Step(t);
            d.OnClipEvent(Event::kCycleBoundary);
            auto clip = d.Step(t);
            for (int i = 0; i < 3; ++i) {
                if (!Is(clip, i == 0 ? "turntable" : i == 1 ? "celebrate" : "logo"))
                    continue;
                CHECK(t - last[i] >= (i == 0 ? 90000 : i == 1 ? 120000 : 180000));
                last[i] = t;
                ++seen[i];
            }
            if (clip.mode != Mode::kLoopPingPong) {
                d.OnClipEvent(Event::kOnceDone);
                d.Step(t);
            }
        }
    }
    CHECK(seen[0] && seen[1] && seen[2]);
}
void ListeningEcho() {
    Director d;
    Boot(d);
    d.SetNow(1000);
    d.OnStatus(Status::kLoading);
    CHECK(Is(d.Step(1000), "loading"));
    d.SetNow(1200);
    d.OnStatus(Status::kListening);
    const auto listening = d.Step(1200);
    CHECK(Is(listening, "listening"));
    for (int64_t t : {1200, 1400, 2699}) {
        d.SetNow(t);
        d.OnStatus(Status::kListening);  // Same-status refreshes must not extend the window.
        d.OnUserSpeech();
        CHECK(d.Step(t).seq == listening.seq);
    }
    d.SetNow(2700);
    d.OnUserSpeech();
    CHECK(Is(d.Step(2700), "thinking"));
    d.OnStatus(Status::kStreaming);
    d.Step(2700);
    d.SetNow(4000);
    d.OnStatus(Status::kListening);
    d.Step(4000);
    d.SetNow(5499);
    d.OnUserSpeech();
    CHECK(Is(d.Step(5499), "listening"));
    d.SetNow(5500);
    d.OnUserSpeech();
    CHECK(Is(d.Step(5500), "thinking"));

    Director greeting;
    Boot(greeting);
    greeting.OnStatus(Status::kListening);
    greeting.Step(0);
    greeting.SetNow(200);
    greeting.OnStatus(Status::kStreaming);
    const bool accepted = greeting.OnEmotion("happy");
    CHECK(accepted);
    const auto emotion = greeting.Step(200);
    greeting.SetNow(300);
    greeting.OnUserSpeech();  // A late echo must also preserve the greeting emotion.
    CHECK(greeting.Step(300).seq == emotion.seq);
}
void Thinking() {
    ListeningEcho();
    Director d;
    Boot(d);
    d.OnStatus(Status::kListening);
    auto s0 = d.Step(0);
    CHECK(Is(s0, "listening"));
    d.SetNow(1500);
    d.OnUserSpeech();
    auto s1 = d.Step(1500);
    CHECK(Is(s1, "thinking") && s1.max_frames == 31 && s1.from_frame_zero &&
          s1.mode == Mode::kOncePingPong);
    d.OnClipEvent(Event::kCycleBoundary);
    CHECK(Is(d.Step(3499), "thinking"));
    CHECK(Is(d.Step(3500), "thinking"));  // A deadline alone cannot cut an active clip.
    d.OnClipEvent(Event::kOnceDone);
    auto s2 = d.Step(3533);
    CHECK(Is(s2, "working") && s2.max_frames == 31 && s2.from_frame_zero &&
          s2.mode == Mode::kOncePingPong);
    CHECK(Is(d.Step(5500), "working"));
    d.OnClipEvent(Event::kOnceDone);
    auto s3 = d.Step(5566);
    CHECK(Is(s3, "searching") && s3.max_frames == 0 && s3.from_frame_zero);
    for (const char* name : {"writing", "working", "searching"}) {
        d.OnClipEvent(Event::kOnceDone);
        auto cur = d.Step(8100);
        CHECK(Is(cur, name));
    }
    bool ok = d.OnEmotion("happy");
    CHECK(ok);
    auto s4 = d.Step(8100);
    CHECK(Is(s4, "searching"));
    d.OnStatus(Status::kStreaming);
    auto s5 = d.Step(8100);
    CHECK(!Is(s5, "searching"));
}
void Emotions() {
    Director d(77);
    Boot(d);
    d.OnStatus(Status::kStreaming);
    auto s0 = d.Step(0);
    CHECK(Is(s0, "streaming"));
    bool ok = d.OnEmotion("happy");
    CHECK(ok);
    const auto first = d.Step(0);
    CHECK(first.mode == Mode::kLoopPingPong && !Is(first, "streaming"));
    d.SetNow(1999);
    ok = d.OnEmotion("happy");
    CHECK(ok);
    auto merged = d.Step(1999);
    CHECK(merged.seq == first.seq);
    // Merge at 1999 refreshed emotion_until_ to 9999.
    auto refreshed_hold = d.Step(8500);
    CHECK(refreshed_hold.seq == first.seq);
    d.SetNow(9000);
    ok = d.OnEmotion("happy");
    CHECK(ok);
    const auto second = d.Step(9000);
    CHECK(std::strcmp(first.clip, second.clip) != 0);
    // B12: After 8s (at 17000), streaming emotion waits for kCycleBoundary (up to 2s grace).
    d.OnClipEvent(Event::kCycleBoundary);
    auto before_expiry = d.Step(16999);
    CHECK(!Is(before_expiry, "streaming"));
    auto grace_hold = d.Step(17000);
    CHECK(!Is(grace_hold, "streaming"));
    d.OnClipEvent(Event::kCycleBoundary);
    auto after_boundary = d.Step(17500);
    CHECK(Is(after_boundary, "streaming"));
    // Without kCycleBoundary, streaming emotion expires at 8s + 2s = 10s.
    d.SetNow(20000);
    ok = d.OnEmotion("happy");
    CHECK(ok);
    auto third = d.Step(20000);
    CHECK(!Is(third, "streaming"));
    auto grace_end = d.Step(29999);
    CHECK(!Is(grace_end, "streaming"));
    auto hard_cap = d.Step(30000);
    CHECK(Is(hard_cap, "streaming"));

    // B7: relaxed during streaming must never close mouth (idle falls back to streaming).
    bool saw_humming = false, saw_streaming = false;
    for (int i = 0; i < 12; ++i) {
        int64_t t = 35000 + i * 2500;
        d.SetNow(t);
        ok = d.OnEmotion("relaxed");
        CHECK(ok);
        auto cur = d.Step(t);
        CHECK(!Is(cur, "idle"));
        if (Is(cur, "humming"))
            saw_humming = true;
        if (Is(cur, "streaming"))
            saw_streaming = true;
    }
    CHECK(saw_humming && saw_streaming);

    bool bad1 = d.OnEmotion("unknown");
    bool bad2 = d.OnEmotion(nullptr);
    CHECK(!bad1 && !bad2);
    d.SetNow(70000);
    ok = d.OnEmotion(std::string("waking").c_str());
    CHECK(ok);
    auto waking_em = d.Step(70000);
    CHECK(Is(waking_em, "waking"));
    ok = d.OnEmotion("neutral");
    CHECK(ok);
    auto back_stream = d.Step(70000);
    CHECK(Is(back_stream, "streaming"));
    d.OnStatus(Status::kIdle);
    d.Step(70000);
    d.OnClipEvent(Event::kOnceDone);
    d.Step(70000);
    // B8: kOnceDone clears emotion_group_ so same emotion within 2s is not swallowed.
    d.SetNow(71000);
    ok = d.OnEmotion("happy");
    CHECK(ok);
    auto idle_em1 = d.Step(71000);
    CHECK(!Is(idle_em1, "idle"));
    d.OnClipEvent(Event::kOnceDone);
    auto idle_after_once = d.Step(71500);
    CHECK(Is(idle_after_once, "idle"));
    d.SetNow(71600);
    ok = d.OnEmotion("happy");
    CHECK(ok);
    auto idle_em2 = d.Step(71600);
    CHECK(!Is(idle_em2, "idle"));
    d.OnClipEvent(Event::kOnceDone);
    d.Step(71700);

    for (int i = 0; i < 12; ++i) {
        d.SetNow(72000 + i * 100);
        ok = d.OnEmotion("low_battery");
        CHECK(ok && Is(d.Step(72000 + i * 100), "drowsy"));
    }
    // B1: sleeping/shy/proud/notifying time out after 8s in idle; warning is system emotion.
    d.SetNow(80000);
    ok = d.OnEmotion("sleeping");
    CHECK(ok);
    auto sl1 = d.Step(87999);
    CHECK(Is(sl1, "sleeping"));
    auto sl2 = d.Step(88000);
    CHECK(Is(sl2, "idle"));

    d.SetNow(90000);
    ok = d.OnEmotion("loving");
    CHECK(ok);
    auto lv1 = d.Step(97999);
    CHECK(!Is(lv1, "idle"));
    auto lv2 = d.Step(98000);
    CHECK(Is(lv2, "idle"));

    d.SetNow(100000);
    ok = d.OnEmotion("warning");
    CHECK(ok);
    auto wn = d.Step(115000);
    CHECK(Is(wn, "alerting"));
    ok = d.OnEmotion("robot_2");
    CHECK(ok);
    auto cl = d.Step(115000);
    CHECK(Is(cl, "idle"));
}
void Transitions() {
    Director d;
    Boot(d);
    d.OnStatus(Status::kStreaming);
    d.Step(0);
    d.OnStatus(Status::kIdle);
    auto finished = d.Step(0);
    CHECK(Is(finished, "finished") && finished.mode == Mode::kOnceForward &&
          finished.max_frames == 30);
    d.OnClipEvent(Event::kOnceDone);
    auto s0 = d.Step(0);
    CHECK(Is(s0, "idle"));

    // B4: Auto mode tts stop -> listening -> quick idle (<1.5s) still plays finished.
    d.OnStatus(Status::kStreaming);
    d.Step(100);
    d.SetNow(200);
    d.OnStatus(Status::kListening);
    auto auto_listen = d.Step(200);
    CHECK(Is(auto_listen, "listening"));
    d.SetNow(400);
    d.OnStatus(Status::kIdle);
    auto auto_fin = d.Step(400);
    CHECK(Is(auto_fin, "finished") && auto_fin.mode == Mode::kOnceForward);
    d.OnClipEvent(Event::kOnceDone);
    auto auto_idle = d.Step(400);
    CHECK(Is(auto_idle, "idle"));

    // Short listening without prior streaming does not play finished.
    d.SetNow(500);
    d.OnStatus(Status::kListening);
    d.Step(1999);
    d.OnStatus(Status::kIdle);
    auto short_listen = d.Step(1999);
    CHECK(Is(short_listen, "idle"));
    d.OnStatus(Status::kListening);
    d.SetNow(3499);
    d.OnStatus(Status::kIdle);
    auto long_listen = d.Step(3499);
    CHECK(Is(long_listen, "finished"));
    d.OnStatus(Status::kFailed);
    auto fail_trans = d.Step(3499);
    CHECK(Is(fail_trans, "confused"));
    d.OnClipEvent(Event::kOnceDone);
    auto fail_loop = d.Step(3499);
    CHECK(Is(fail_loop, "failed") && fail_loop.mode == Mode::kLoopPingPong);
    d.OnStatus(Status::kIdle);
    auto after_fail = d.Step(3499);
    CHECK(Is(after_fail, "idle"));

    // B5 & B6: kReceiving and persistent base statuses use kLoopPingPong.
    d.OnStatus(Status::kReceiving);
    auto recv = d.Step(4000);
    CHECK(Is(recv, "receiving") && recv.mode == Mode::kLoopPingPong);
    d.OnStatus(Status::kWaking);
    auto act = d.Step(4000);
    CHECK(Is(act, "waking") && act.mode == Mode::kLoopPingPong);
    d.OnStatus(Status::kApproving);
    auto app = d.Step(4000);
    CHECK(Is(app, "approving") && app.mode == Mode::kLoopPingPong);
    d.OnStatus(Status::kHappy);
    auto hap = d.Step(4000);
    CHECK(Is(hap, "happy") && hap.mode == Mode::kLoopPingPong);
}
void WakeInterruption() {
    for (int response : {0, 1, 2}) {
        const bool greeting = response == 1;
        Director d;
        Boot(d);
        d.OnPowerSave(true);
        d.Step(0);
        d.OnClipEvent(Event::kOnceDone);
        d.Step(0);
        d.SetNow(10000);
        d.OnStatus(Status::kLoading);
        const auto wake = d.Step(10000);
        CHECK(Is(wake, "waking"));
        d.SetNow(10300);
        d.OnStatus(Status::kListening);
        d.OnUserSpeech();  // Wake-word echo must not request thinking.
        CHECK(d.Step(10300).seq == wake.seq);
        d.SetNow(11799);
        d.OnUserSpeech();
        CHECK(d.Step(11799).seq == wake.seq);
        if (greeting) {
            d.OnStatus(Status::kStreaming);
            const bool accepted = d.OnEmotion("happy");
            CHECK(accepted);
        }
        CHECK(d.Step(11800).seq == wake.seq);
        if (response == 2) {
            d.OnUserSpeech();  // At 1.5s a real transcript is retained for after waking.
            CHECK(d.Step(11800).seq == wake.seq);
        }
        d.SetNow(12367);
        d.OnClipEvent(Event::kOnceDone);
        const auto after = d.Step(12367);
        if (greeting)
            CHECK(!Is(after, "waking") && !Is(after, "streaming") && !Is(after, "thinking"));
        else
            CHECK(Is(after, response == 2 ? "thinking" : "listening"));
    }
    for (Status interrupt : {Status::kFailed, Status::kReceiving, Status::kRadar}) {
        Director d;
        Boot(d);
        d.OnPowerSave(true);
        d.Step(0);
        d.OnPowerSave(false);
        CHECK(Is(d.Step(0), "waking"));
        d.OnStatus(interrupt);
        const auto after = d.Step(0);
        CHECK(Is(after, interrupt == Status::kFailed      ? "confused"
                        : interrupt == Status::kReceiving ? "receiving"
                                                          : "radar"));
    }
}
void Wake() {
    WakeInterruption();
    Director d;
    Boot(d);
    d.Step(220000);
    d.OnStatus(Status::kListening);
    auto wake = d.Step(220000);
    CHECK(Is(wake, "waking") && wake.max_frames == 73);
    d.OnUserSpeech();
    auto th = d.Step(220000);
    CHECK(Is(th, "waking") && th.seq == wake.seq);
    d.OnClipEvent(Event::kOnceDone);
    CHECK(Is(d.Step(220000), "listening"));
    d.OnStatus(Status::kIdle);
    d.Step(220000);
    d.OnPowerSave(true);
    auto down = d.Step(220000);
    CHECK(Is(down, "drowsy") && down.max_frames == 42 && down.mode == Mode::kOncePingPong);
    d.OnClipEvent(Event::kOnceDone);
    auto sl = d.Step(220000);
    CHECK(Is(sl, "sleeping"));
    d.OnPowerSave(false);
    wake = d.Step(220000);
    CHECK(Is(wake, "waking") && wake.max_frames == 73);
    d.OnClipEvent(Event::kOnceDone);
    auto id1 = d.Step(220000);
    CHECK(Is(id1, "idle"));

    // B2: waking preserved across kLoading -> kListening -> kIdle.
    d.OnPowerSave(true);
    d.Step(220000);
    d.OnClipEvent(Event::kOnceDone);
    d.Step(220000);
    d.OnStatus(Status::kLoading);
    auto w_load = d.Step(220000);
    CHECK(Is(w_load, "waking") && w_load.max_frames == 73);
    d.OnStatus(Status::kListening);
    auto w_listen = d.Step(220000);
    CHECK(Is(w_listen, "waking") && w_listen.max_frames == 73 && w_listen.seq == w_load.seq);
    d.OnClipEvent(Event::kOnceDone);
    auto after_w = d.Step(220000);
    CHECK(Is(after_w, "listening"));

    d.OnStatus(Status::kIdle);
    d.Step(220000);
    d.OnPowerSave(true);
    d.Step(220000);
    d.OnStatus(Status::kStreaming);
    auto st = d.Step(220000);
    CHECK(Is(st, "streaming"));
    d.OnStatus(Status::kIdle);
    auto fn = d.Step(220000);
    CHECK(Is(fn, "finished"));
    d.OnClipEvent(Event::kOnceDone);
    auto id2 = d.Step(220000);
    CHECK(Is(id2, "idle"));
    bool ok = d.OnEmotion("sleeping");
    CHECK(ok);
    auto em_sl = d.Step(220000);
    CHECK(Is(em_sl, "sleeping"));
    d.OnStatus(Status::kListening);
    auto w2 = d.Step(220000);
    CHECK(Is(w2, "waking"));
    d.OnStatus(Status::kIdle);
    auto w3 = d.Step(220000);
    CHECK(Is(w3, "waking"));
    d.OnClipEvent(Event::kOnceDone);
    auto id3 = d.Step(220000);
    CHECK(Is(id3, "idle"));
}
void Legacy() {
    const char* old[] = {"idle",      "listening", "streaming", "loading",   "radar",  "waiting",
                         "searching", "waking",    "approving", "uploading", "failed", "happy",
                         "laughing",  "playful",   "shy",       "proud",     "sad",    "sleeping",
                         "angry",     "surprised", "thinking",  "confused",  "scared", "alerting",
                         "excited",   "curious",   "drowsy",    "notifying"};
    Director refreshing;
    refreshing.OnStatus(Status::kIdle);
    auto sp = refreshing.Step(0);
    CHECK(Is(sp, "logo"));
    const uint64_t mask = Director::AvailabilityMask(old, 28);
    refreshing.SetAvailable(mask);
    auto refreshed = refreshing.Step(0);
    CHECK(Is(refreshed, "idle") && refreshed.mode == Mode::kLoopPingPong);
    Director d(99);
    d.SetAvailable(old, 28);
    d.OnStatus(Status::kIdle);
    auto s0 = d.Step(0);
    CHECK(Is(s0, "idle"));
    for (int64_t t = 0; t < 300000; t += 1000) {
        d.OnClipEvent(Event::kCycleBoundary);
        auto clip = d.Step(t);
        bool found = false;
        for (const char* name : old)
            if (Is(clip, name))
                found = true;
        CHECK(found);
        if (clip.mode != Mode::kLoopPingPong)
            d.OnClipEvent(Event::kOnceDone);
    }
    d.OnUserSpeech();
    d.OnStatus(Status::kListening);
    CHECK(Is(d.Step(299000), "waking"));
    d.OnClipEvent(Event::kOnceDone);
    CHECK(Is(d.Step(301500), "listening"));
    d.OnUserSpeech();
    auto th = d.Step(302000);
    CHECK(Is(th, "thinking"));
    d.OnClipEvent(Event::kOnceDone);
    auto work_fallback = d.Step(304033);
    CHECK(Is(work_fallback, "thinking") && work_fallback.seq != th.seq);
    d.OnClipEvent(Event::kOnceDone);
    CHECK(Is(d.Step(306066), "searching"));
    d.OnStatus(Status::kIdle);
    bool ok = d.OnEmotion("download");
    CHECK(ok);
    auto up = d.Step(302000);
    CHECK(Is(up, "uploading"));
    d.OnStatus(Status::kReceiving);
    auto recv_fb = d.Step(302000);
    CHECK(Is(recv_fb, "uploading") && recv_fb.mode == Mode::kLoopPingPong);
    d.SetAvailable(nullptr, 0);
    auto none = d.Step(302000);
    CHECK(none.clip == nullptr);
}
void Notifications() {
    // B3: Boot notification does not preempt the boot logo, and OnceDone clears
    // notification_until_.
    Director boot_d;
    boot_d.OnNotification(5000);
    boot_d.OnStatus(Status::kIdle);
    auto sp = boot_d.Step(0);
    CHECK(Is(sp, "logo"));
    boot_d.OnClipEvent(Event::kOnceDone);
    auto nt = boot_d.Step(1000);
    CHECK(Is(nt, "notifying") && nt.mode == Mode::kOnceForward);
    boot_d.OnClipEvent(Event::kOnceDone);
    auto after_nt = boot_d.Step(1000);
    CHECK(Is(after_nt, "idle"));

    Director d;
    Boot(d);
    d.OnNotification(3000);
    auto n1 = d.Step(2999);
    CHECK(Is(n1, "notifying"));
    auto n2 = d.Step(3000);
    CHECK(Is(n2, "idle"));
    // A6: radar notification clip.
    d.OnNotification(5000, "radar");
    auto rd = d.Step(4000);
    CHECK(Is(rd, "radar") && rd.mode == Mode::kLoopPingPong);
    auto rd_end = d.Step(5000);
    CHECK(Is(rd_end, "idle"));
    d.OnNotification(6000);
    d.OnStatus(Status::kListening);
    auto ls = d.Step(5500);
    CHECK(Is(ls, "listening"));
    d.OnNotification(9000);  // Updating an active notification cannot be lost to expiry.
    d.OnStatus(Status::kIdle);
    auto n3 = d.Step(8999);
    CHECK(Is(n3, "notifying"));
    auto n4 = d.Step(9000);
    CHECK(!Is(n4, "notifying"));
    d.OnStatus(Status::kLoading);
    d.OnNotification(15000);
    auto ld = d.Step(10000);
    CHECK(Is(ld, "loading"));
    d.OnStatus(Status::kStreaming);
    auto st = d.Step(10000);
    CHECK(Is(st, "streaming"));
    d.SetNow(15001);
    d.OnStatus(Status::kIdle);
    auto n5 = d.Step(15001);
    CHECK(!Is(n5, "notifying"));
}
void Interactions() {
    Director d;
    Boot(d);
    d.Step(240000);
    d.OnClipEvent(Event::kCycleBoundary);
    CHECK(Is(d.Step(240000), "drowsy"));
    d.Step(300000);
    d.OnClipEvent(Event::kCycleBoundary);
    CHECK(Is(d.Step(300000), "sleeping"));
    CHECK(d.OnInteraction("curious"));
    CHECK(!Is(d.Step(300000), "sleeping"));
    d.OnClipEvent(Event::kOnceDone);
    CHECK(Is(d.Step(303000), "idle"));
    d.OnClipEvent(Event::kCycleBoundary);
    CHECK(!Is(d.Step(303000), "drowsy"));
    d.OnStatus(Status::kListening);
    CHECK(!d.OnInteraction("happy"));
    CHECK(Is(d.Step(303000), "listening"));
    d.OnStatus(Status::kStreaming);
    CHECK(!d.OnInteraction("confused"));
    CHECK(Is(d.Step(303000), "streaming"));

    Director newer;
    Boot(newer);
    CHECK(!newer.OnInteraction("unknown"));
    newer.SetNow(100);
    CHECK(newer.OnInteraction("curious"));
    newer.SetNow(1000);
    CHECK(newer.OnEmotion("angry"));
    CHECK(Is(newer.Step(2700), "angry") || Is(newer.Step(2700), "suspicious"));
    newer.OnNotification(5000);
    CHECK(!newer.OnInteraction("happy"));
    newer.Step(6000);
    newer.OnPowerSave(true);
    CHECK(!newer.OnInteraction("happy"));
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string test = argv[1];
    if (test == "stages")
        Stages();
    else if (test == "fidgets")
        Fidgets();
    else if (test == "rare")
        RareCooldowns();
    else if (test == "thinking")
        Thinking();
    else if (test == "emotions")
        Emotions();
    else if (test == "transitions")
        Transitions();
    else if (test == "wake")
        Wake();
    else if (test == "legacy")
        Legacy();
    else if (test == "notifications")
        Notifications();
    else if (test == "interactions")
        Interactions();
    else
        CHECK(false);
}
