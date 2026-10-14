// Minimal stubs for the Pico SDK, so the firmware's own code can be cross-compiled and checked for Cortex-M33
// without the SDK installed.
//
// This verifies everything quorum contributes: register access, the cycle-counting harness, the casebook headers, the
// arbiter, the witness, and all the integer formatting. What it does NOT verify is SDK integration -- boot, clocks,
// UART -- which is precisely the part that cannot be verified without hardware anyway.
//
// The point of drawing the line here is that when the board arrives, a failure is attributable. If the firmware does
// not run, the cause is in the SDK layer, because everything on this side of the line already compiles for the target.

#pragma once

#include <cstdint>

extern "C" {
inline void stdio_init_all() {}
inline void sleep_ms(std::uint32_t) {}
inline void tight_loop_contents() {}
}
