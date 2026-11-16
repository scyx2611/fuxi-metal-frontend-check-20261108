#ifndef FUXI_TORCH_LEVELS_H
#define FUXI_TORCH_LEVELS_H

#include <stdbool.h>

static inline bool fuxi_torch_ui_decode(float value, unsigned *level)
{
    if (level == 0 || !(value >= 0.0f && value <= 1.0f)) return false;
    *level = value == 0.0f ? 0u : value <= 0.25f ? 1u :
        value <= 0.5f ? 2u : value <= 0.85f ? 3u : 4u;
    return true;
}

static inline float fuxi_torch_ui_encode(unsigned level)
{
    static const float values[] = {0.0f, 0.25f, 0.5f, 0.85f, 1.0f};
    return level <= 4 ? values[level] : 0.0f;
}

#endif
