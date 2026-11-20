#include "grokbot_inputs.h"
#include "grokbot_rotation.h"

#include <cassert>

using Reaction = GrokbotInputs::Reaction;

static void TestRotation() {
    GrokbotRotation rotation;
    assert(rotation.Update(0, 0, 1000) == 0);
    assert(rotation.Update(-1000, 0, 0) == 0);
    for (int i = 0; i < 30; ++i)
        rotation.Update(-707, -707, 0);
    assert(std::abs(rotation.Update(-707, -707, 0) + 45) <= 2);
    const int held = rotation.Update(0, 0, 1000);
    assert(std::abs(held + 45) <= 2);
    assert(rotation.Update(0, 1900, 1000) == held);
    for (int i = 0; i < 30; ++i)
        rotation.Update(999, -17, 0);
    const int before_wrap = rotation.Update(999, -17, 0);
    assert(before_wrap <= -177);
    assert(rotation.Update(999, 17, 0) <= -177);
    for (int i = 0; i < 30; ++i)
        rotation.Update(0, 1000, 0);
    assert(std::abs(rotation.Update(0, 1000, 0) - 90) <= 2);
}

int main() {
    TestRotation();
    GrokbotInputs touch;
    assert(touch.Touch(true, 100, 100, 0) == Reaction::kNone);
    assert(touch.Touch(false, 0, 0, 100) == Reaction::kTap);
    assert(touch.Touch(false, 0, 0, 150) == Reaction::kNone);
    touch.Touch(true, 100, 100, 500);
    assert(touch.Touch(false, 0, 0, 600) == Reaction::kNone);  // Cooldown.
    touch.Touch(true, 100, 100, 2000);
    touch.Touch(true, 135, 100, 2100);
    touch.Touch(true, 170, 100, 2200);
    assert(touch.Touch(false, 0, 0, 2300) == Reaction::kStroke);
    touch.Touch(true, 100, 100, 5000);
    assert(touch.Touch(false, 0, 0, 5020) == Reaction::kNone);  // Bounce.
    touch.Touch(true, 100, 100, 6000);
    assert(touch.Touch(false, 0, 0, 7000) == Reaction::kNone);  // Long stationary press.
    touch.Touch(true, 100, 100, 8000);
    touch.CancelTouch();
    assert(touch.Touch(false, 0, 0, 8100) == Reaction::kNone);

    GrokbotInputs slow_stroke;
    slow_stroke.Touch(true, 100, 100, 0);
    for (int t = 50; t <= 1000; t += 50)
        assert(slow_stroke.Touch(true, 100 + t / 10, 100, t) == Reaction::kNone);
    assert(slow_stroke.Touch(false, 0, 0, 1050) == Reaction::kStroke);

    GrokbotInputs shake;
    assert(shake.Motion(0, 0, 1000, 0) == Reaction::kNone);
    assert(shake.Motion(1800, 0, 1000, 50) == Reaction::kNone);
    assert(shake.Motion(-1800, 0, 1000, 100) == Reaction::kShake);
    assert(shake.Motion(1800, 0, 1000, 150) == Reaction::kNone);
    assert(shake.Motion(-1800, 0, 1000, 200) == Reaction::kNone);
    GrokbotInputs isolated;
    isolated.Motion(0, 0, 1000, 0);
    assert(isolated.Motion(1800, 0, 1000, 50) == Reaction::kNone);
    assert(isolated.Motion(-1800, 0, 1000, 1000) == Reaction::kNone);

    GrokbotInputs tilt;
    tilt.Motion(0, 0, 1000, 0);
    assert(tilt.Motion(900, 0, 435, 50) == Reaction::kNone);
    for (int t = 100; t < 700; t += 50)
        assert(tilt.Motion(900, 0, 435, t) == Reaction::kNone);
    assert(tilt.Motion(900, 0, 435, 700) == Reaction::kTilt);
    for (int t = 750; t < 4000; t += 50)
        assert(tilt.Motion(900, 0, 435, t) == Reaction::kNone);  // No repeat while held.
    assert(tilt.Motion(0, 0, 0, 4000) == Reaction::kNone);       // Free fall is not tilt.

    GrokbotInputs invalid_start;
    invalid_start.Motion(0, 0, 0, 0);
    invalid_start.Motion(0, 0, 1000, 1000);
    for (int t = 1050; t < 2000; t += 50)
        assert(invalid_start.Motion(0, 0, 1000, t) == Reaction::kNone);

    GrokbotInputs shared_cooldown;
    shared_cooldown.Touch(true, 100, 100, 0);
    shared_cooldown.Touch(false, 0, 0, 100);
    shared_cooldown.Motion(0, 0, 1000, 200);
    shared_cooldown.Motion(1800, 0, 1000, 250);
    assert(shared_cooldown.Motion(-1800, 0, 1000, 300) == Reaction::kNone);
}
