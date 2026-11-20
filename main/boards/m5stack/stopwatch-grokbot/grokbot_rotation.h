#pragma once

#include <cstdint>
#include <cstdlib>
#include <utility>

// BMI270 specific force points up at rest; the screen uses +Y right and +X down.
inline int BootOrientation(int x, int y, int z) {
    const int64_t planar = int64_t(x) * x + int64_t(y) * y;
    const int64_t total = planar + int64_t(z) * z;
    // At +/-4 g, 8192 counts equal 1 g.
    if (total < 6554LL * 6554 || total > 9830LL * 9830 || planar < 2867LL * 2867)
        return 0;
    if (std::abs(x) >= std::abs(y))
        return x < 0 ? 0 : 2;
    return y > 0 ? 1 : 3;
}

inline void MapBootTouch(int orientation, int size, int& x, int& y) {
    const int last = size - 1;
    if (orientation >= 2)
        y = last - y;
    if (orientation == 1 || orientation == 2)
        x = last - x;
    if (orientation % 2 != 0)
        std::swap(x, y);
}
