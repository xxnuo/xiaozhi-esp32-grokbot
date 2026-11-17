#pragma once

#include <cstdint>
#include <cstdlib>

class GrokbotInputs {
public:
    enum class Reaction { kNone, kTap, kStroke, kShake, kTilt };

    Reaction Touch(bool down, int x, int y, int64_t now_ms) {
        if (down) {
            if (!touching_) {
                touch_start_ = now_ms;
                travel_ = 0;
                touch_x_ = x;
                touch_y_ = y;
            } else {
                const int distance = std::abs(x - touch_x_) + std::abs(y - touch_y_);
                if (distance >= 6) {
                    travel_ += distance;
                    touch_x_ = x;
                    touch_y_ = y;
                }
                if (travel_ > 4096)
                    travel_ = 4096;
            }
            touching_ = true;
            return Reaction::kNone;
        }
        if (!touching_)
            return Reaction::kNone;
        touching_ = false;
        const auto duration = now_ms - touch_start_;
        if (duration >= 100 && duration <= 4000 && travel_ >= 65)
            return Emit(Reaction::kStroke, now_ms);
        if (duration >= 50 && duration <= 600 && travel_ < 30)
            return Emit(Reaction::kTap, now_ms);
        return Reaction::kNone;
    }

    void CancelTouch() { touching_ = false; }

    // Acceleration is in mg; the board configures BMI270 to +/-4g before conversion.
    Reaction Motion(int x, int y, int z, int64_t now_ms) {
        const int delta =
            std::abs(x - previous_[0]) + std::abs(y - previous_[1]) + std::abs(z - previous_[2]);
        previous_[0] = x;
        previous_[1] = y;
        previous_[2] = z;
        if (!have_motion_) {
            have_motion_ = true;
            baseline_[0] = x;
            baseline_[1] = y;
            baseline_[2] = z;
            return Reaction::kNone;
        }
        if (delta >= 1400) {
            tilt_since_ = -1;
            if (shake_since_ >= 0 && now_ms - shake_since_ <= 350) {
                shake_since_ = -1;
                return Emit(Reaction::kShake, now_ms);
            }
            shake_since_ = now_ms;
        }
        const int64_t norm = int64_t(x) * x + int64_t(y) * y + int64_t(z) * z;
        const int64_t base_norm = int64_t(baseline_[0]) * baseline_[0] +
                                  int64_t(baseline_[1]) * baseline_[1] +
                                  int64_t(baseline_[2]) * baseline_[2];
        const int64_t dot =
            int64_t(x) * baseline_[0] + int64_t(y) * baseline_[1] + int64_t(z) * baseline_[2];
        const bool tilted = base_norm >= 600 * 600 && norm >= 800 * 800 && norm <= 1200 * 1200 &&
                            delta < 250 &&
                            (dot < 0 || double(dot) * dot < double(norm) * base_norm / 2);
        if (!tilted) {
            tilt_since_ = -1;
            // Do not calibrate a sensor that started moving or in free fall.
            if (base_norm < 600 * 600 && norm >= 800 * 800 && norm <= 1200 * 1200) {
                baseline_[0] = x;
                baseline_[1] = y;
                baseline_[2] = z;
            }
            return Reaction::kNone;
        }
        if (tilt_since_ < 0)
            tilt_since_ = now_ms;
        if (now_ms - tilt_since_ < 600)
            return Reaction::kNone;
        baseline_[0] = x;
        baseline_[1] = y;
        baseline_[2] = z;
        tilt_since_ = -1;
        return Emit(Reaction::kTilt, now_ms);
    }

private:
    Reaction Emit(Reaction reaction, int64_t now_ms) {
        if (now_ms - last_reaction_ < 1800)
            return Reaction::kNone;
        last_reaction_ = now_ms;
        return reaction;
    }

    bool touching_ = false;
    int touch_x_ = 0;
    int touch_y_ = 0;
    int travel_ = 0;
    int64_t touch_start_ = 0;
    bool have_motion_ = false;
    int previous_[3] = {};
    int baseline_[3] = {};
    int64_t shake_since_ = -1;
    int64_t tilt_since_ = -1;
    int64_t last_reaction_ = -1800;
};
