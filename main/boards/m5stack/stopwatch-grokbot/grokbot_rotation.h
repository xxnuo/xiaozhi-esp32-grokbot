#pragma once

#include <cmath>
#include <cstdint>

class GrokbotRotation {
public:
    // The accelerometer reports specific force, which points up at rest.
    int Update(int right_mg, int down_mg, int normal_mg) {
        const int64_t planar = int64_t(right_mg) * right_mg + int64_t(down_mg) * down_mg;
        const int64_t total = planar + int64_t(normal_mg) * normal_mg;
        if (total < 800 * 800 || total > 1200 * 1200 ||
            planar < (tracking_ ? 250 * 250 : 350 * 350)) {
            tracking_ = false;
            return angle_;
        }

        tracking_ = true;
        constexpr float kDegreesPerRadian = 57.2957795f;
        const float target = std::atan2(float(right_mg), -float(down_mg)) * kDegreesPerRadian;
        const float delta = std::remainder(target - filtered_angle_, 360.0f);
        filtered_angle_ = std::remainder(filtered_angle_ + 0.28f * delta, 360.0f);
        if (std::abs(std::remainder(filtered_angle_ - angle_, 360.0f)) >= 1.5f) {
            angle_ = static_cast<int>(std::lround(filtered_angle_));
        }
        return angle_;
    }

private:
    bool tracking_ = false;
    float filtered_angle_ = 0;
    int angle_ = 0;
};
