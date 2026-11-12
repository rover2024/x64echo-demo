// SPDX-License-Identifier: MIT
#include "work.h"

#include <cstdint>

extern "C" unsigned quick_mix(unsigned value)
{
    std::uint32_t state = value;
    for (unsigned i = 0; i < 4096; ++i)
        state = state * 1664525u + 1013904223u;
    return state;
}
