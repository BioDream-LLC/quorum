// See ../pico/stdlib.h for why these stubs exist.
#pragma once

#include <cstdint>

extern "C" {
enum clock_index { clk_sys = 0 };
/// 150 MHz is the RP2350 default system clock.
inline std::uint32_t clock_get_hz(clock_index) { return 150000000U; }
}
