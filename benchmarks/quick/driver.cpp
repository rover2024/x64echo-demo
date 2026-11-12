// SPDX-License-Identifier: MIT
#include "work.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

int main(int argc, char **argv)
{
    unsigned count = 100'000u;
    if (argc > 1)
        count = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));

    const auto begin = std::chrono::steady_clock::now();
    std::uint32_t checksum = 0;
    for (unsigned i = 0; i < count; ++i)
        checksum ^= quick_mix(i);
    const auto end = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration<double>(end - begin).count();
    std::printf("calls=%u checksum=%08x seconds=%.6f\n",
                count, checksum, seconds);
    return 0;
}
