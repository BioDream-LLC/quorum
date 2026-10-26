// Freestanding compile gate for the binding layer.
//
// Same purpose as the other gates: prove the header compiles with no hosted runtime, no exceptions, no RTTI and no
// thread-safe statics, and double as a sketch of the C ABI a device integrator actually calls.
//
// Also proves the arithmetic is integer-only. There is no libm here to link against, so any accidental use of a
// floating-point operation would show up as a link failure rather than as a surprise on a part without an FPU.

#include <cstdint>

#include "quorum/binding/binding.hpp"

namespace {

using namespace quorum::binding;

enum : std::int32_t {
    o_level_high = 0,
    o_level_unavailable = 1,
    o_level_flatline = 2,
};

constexpr ChannelSpec kChannels[] = {
    {0, 12000, 2000, 30000, 20, o_level_unavailable, o_level_flatline},
};
constexpr ThresholdSpec kThresholds[] = {
    {0, o_level_high, Sense::Above, 10000, 9500},
};

constexpr TimerSpec kTimers[] = {
    {TimerKind::Recent, 3, o_level_high, -1, 60000U, 0},
};

// Static storage, so the device integrator can see there is no allocation anywhere in the path.
Binding<1, 1> g_binding;
Timers<1> g_timers;

}  // namespace

extern "C" {

/// Install the generated tables. Returns 0 on success.
int quorum_binding_configure(void) {
    return g_binding.configure(kChannels, 1, kThresholds, 1) == Status::Ok ? 0 : -1;
}

/// Feed one reading, in milli-units of the channel's declared unit.
int quorum_binding_sample(int channel, int milli_value, unsigned long now_ms) {
    return g_binding.sample(static_cast<std::int32_t>(channel),
                            static_cast<std::int32_t>(milli_value),
                            static_cast<std::uint32_t>(now_ms)) == Status::Ok
               ? 0
               : -1;
}

/// Report that a read failed, as distinct from returning an implausible value.
int quorum_binding_fault(int channel, unsigned long now_ms) {
    return g_binding.fault(static_cast<std::int32_t>(channel), static_cast<std::uint32_t>(now_ms)) == Status::Ok
               ? 0
               : -1;
}

/// The presence mask to hand to the casebook. Never reports silence when it means blindness.
unsigned long quorum_binding_mask(unsigned long now_ms) {
    return g_binding.mask(static_cast<std::uint32_t>(now_ms));
}

/// Why a channel is or is not believable, for the witness.
int quorum_binding_health(int channel, unsigned long now_ms) {
    return static_cast<int>(g_binding.health(static_cast<std::int32_t>(channel),
                                             static_cast<std::uint32_t>(now_ms)));
}

/// Install the timed-reading table (4 observations: the three above and one timed reading). Returns 0 on success.
int quorum_timers_configure(void) { return g_timers.configure(kTimers, 1, 4) == Status::Ok ? 0 : -1; }

/// Advance the timed readings; returns the timer bits to OR into the mask.
unsigned long quorum_timers_step(unsigned long base_mask, unsigned long now_ms) {
    return g_timers.step(static_cast<std::uint32_t>(base_mask), static_cast<std::uint32_t>(now_ms));
}
}
