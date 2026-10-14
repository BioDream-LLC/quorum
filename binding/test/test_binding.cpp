// Tests for the binding layer.
//
// Organised around the failure modes rather than the methods, because the methods are trivial and the failure modes
// are what kill people. The first test is the most important one in the file.

#include <cstdint>
#include <cstdio>
#include <string>

#include "quorum/binding/binding.hpp"

using namespace quorum::binding;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

constexpr std::uint32_t bit(std::int32_t o) { return 1U << static_cast<std::uint32_t>(o); }

// A two-channel fixture: vibration in mm/s with two thresholds, suction pressure in bar with one.
//
//   observation 0  vib_high
//   observation 1  vib_extreme
//   observation 2  suction_low
//   observation 3  vib_unavailable
//   observation 4  vib_flatline
//   observation 5  suction_unavailable
enum : std::int32_t {
    o_vib_high = 0,
    o_vib_extreme = 1,
    o_suction_low = 2,
    o_vib_unavailable = 3,
    o_vib_flatline = 4,
    o_suction_unavailable = 5,
};
enum : std::int32_t { ch_vib = 0, ch_suction = 1 };

constexpr ChannelSpec kChannels[] = {
    // vibration: 0..50 mm/s, must be sampled every 5 s, flatlines after 60 s of not moving by >0.05 mm/s
    {0, 50000, 5000, 60000, 50, o_vib_unavailable, o_vib_flatline},
    // suction: -1..10 bar, must be sampled every 2 s, no flatline check
    {-1000, 10000, 2000, 0, 0, o_suction_unavailable, -1},
};

constexpr ThresholdSpec kThresholds[] = {
    {ch_vib, o_vib_high, Sense::Above, 7100, 6600},        // assert at 7.1, release at 6.6
    {ch_vib, o_vib_extreme, Sense::Above, 11000, 10500},   // assert at 11.0, release at 10.5
    {ch_suction, o_suction_low, Sense::Below, 800, 1000},  // assert at 0.8 falling, release at 1.0 rising
};

using Bind = Binding<4, 8>;

Bind fresh() {
    Bind b;
    const Status s = b.configure(kChannels, 2, kThresholds, 3);
    check(s == Status::Ok, "the fixture configures");
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────

/// THE test. A device that has booted but read nothing must say so, not report a quiet plant.
void test_boot_state_is_blind_not_quiet() {
    std::printf("at power-on, before any sample\n");
    Bind b = fresh();
    const std::uint32_t m = b.mask(0);

    check((m & bit(o_vib_unavailable)) != 0, "vibration reports unavailable");
    check((m & bit(o_suction_unavailable)) != 0, "suction reports unavailable");
    check((m & bit(o_vib_high)) == 0, "no vibration threshold is asserted");
    check((m & bit(o_vib_extreme)) == 0, "no extreme-vibration threshold is asserted");
    check((m & bit(o_suction_low)) == 0, "no suction threshold is asserted");
    check(b.health(ch_vib, 0) == Health::NeverSampled, "health says never sampled, not good");

    // The distinction that matters: the mask is not empty. An empty mask would be indistinguishable from a healthy
    // quiet machine, and the casebook would conclude the asset is fine.
    check(m != 0, "the mask is NOT empty -- blind is reported, not silence");
}

void test_threshold_and_hysteresis() {
    std::printf("threshold with hysteresis\n");
    Bind b = fresh();
    std::uint32_t t = 1000;

    check(b.sample(ch_vib, 5000, t) == Status::Ok, "a sample is accepted");
    check((b.mask(t) & bit(o_vib_high)) == 0, "5.0 mm/s does not assert vib_high");
    check(b.health(ch_vib, t) == Health::Good, "the channel is now good");

    t += 1000;
    check(b.sample(ch_vib, 7100, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_vib_high)) != 0, "7.1 mm/s asserts vib_high at the threshold");

    // Inside the deadband the latch must hold. Without this the observation chatters, the framework oscillates
    // between verdicts, and the operator gets the alarm flood this product exists to prevent.
    t += 1000;
    check(b.sample(ch_vib, 6800, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_vib_high)) != 0, "6.8 mm/s is inside the deadband, so vib_high holds");

    t += 1000;
    check(b.sample(ch_vib, 6600, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_vib_high)) == 0, "6.6 mm/s releases vib_high");

    t += 1000;
    check(b.sample(ch_vib, 12000, t) == Status::Ok, "");
    const std::uint32_t m = b.mask(t);
    check((m & bit(o_vib_high)) != 0 && (m & bit(o_vib_extreme)) != 0,
          "12.0 mm/s asserts both vibration thresholds");
}

void test_below_sense() {
    std::printf("a falling threshold\n");
    Bind b = fresh();
    std::uint32_t t = 1000;

    check(b.sample(ch_suction, 2000, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_suction_low)) == 0, "2.0 bar does not assert suction_low");

    t += 500;
    check(b.sample(ch_suction, 800, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_suction_low)) != 0, "0.8 bar asserts suction_low");

    t += 500;
    check(b.sample(ch_suction, 900, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_suction_low)) != 0, "0.9 bar is inside the deadband, so it holds");

    t += 500;
    check(b.sample(ch_suction, 1000, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_suction_low)) == 0, "1.0 bar releases suction_low");
}

void test_staleness_suppresses_thresholds() {
    std::printf("a channel that stops reporting\n");
    Bind b = fresh();

    check(b.sample(ch_vib, 12000, 1000) == Status::Ok, "");
    check((b.mask(1000) & bit(o_vib_extreme)) != 0, "extreme vibration is asserted while fresh");

    // 5 s later the sample has expired. The latch still says "extreme vibration", but the channel can no longer be
    // believed, so the mask must not repeat the claim.
    const std::uint32_t later = 1000 + 5000;
    const std::uint32_t m = b.mask(later);
    check(b.health(ch_vib, later) == Health::Stale, "the channel reports stale");
    check((m & bit(o_vib_unavailable)) != 0, "unavailable is asserted");
    check((m & bit(o_vib_extreme)) == 0, "the latched threshold is suppressed");
    check((m & bit(o_vib_high)) == 0, "and so is the other one on the same channel");

    // Crucially, the OTHER channel is untouched. Losing one sensor must not blind the device to the rest.
    check((m & bit(o_suction_unavailable)) != 0, "suction is independently unavailable, never sampled");

    check(b.mask(1000 + 4999) == b.mask(1000), "the boundary is exclusive: 4999 ms is still fresh");
}

/// A device that has run for 49.7 days rolls the millisecond counter through zero. If age were computed as a signed
/// difference, every channel would look freshly sampled at precisely the moment the plant has been trusted longest.
void test_clock_wraparound() {
    std::printf("millisecond counter wrapping after 49.7 days\n");
    Bind b = fresh();

    const std::uint32_t before_wrap = 0xFFFFFF00U;
    check(b.sample(ch_vib, 12000, before_wrap) == Status::Ok, "");
    check(b.health(ch_vib, before_wrap) == Health::Good, "good when just sampled");

    // 0x100 ms later, having wrapped through zero. Still fresh: 256 ms < 5000 ms.
    const std::uint32_t just_after = 0x00000000U;
    check(b.health(ch_vib, just_after) == Health::Good, "still fresh 256 ms after the wrap");
    check((b.mask(just_after) & bit(o_vib_extreme)) != 0, "and the threshold still holds");

    // 6 s after the sample, having wrapped. Must be stale.
    const std::uint32_t well_after = 0x00001700U;  // 0xFFFFFF00 + 0x1800 = 6144 ms elapsed
    check(b.health(ch_vib, well_after) == Health::Stale, "stale 6 s after the sample, across the wrap");
    check((b.mask(well_after) & bit(o_vib_unavailable)) != 0, "and unavailable is asserted");
}

void test_out_of_range_never_latches() {
    std::printf("an instrument returning nonsense\n");
    Bind b = fresh();

    check(b.sample(ch_vib, 5000, 1000) == Status::Ok, "a good low sample first");
    check((b.mask(1000) & bit(o_vib_high)) == 0, "nothing asserted");

    // 900 mm/s is not a vibration reading, it is a broken ADC. It must not latch a shutdown.
    check(b.sample(ch_vib, 900000, 2000) == Status::Ok, "the out-of-range sample is accepted for bookkeeping");
    const std::uint32_t m = b.mask(2000);
    check(b.health(ch_vib, 2000) == Health::OutOfRange, "health reports out of range");
    check((m & bit(o_vib_unavailable)) != 0, "unavailable is asserted");
    check((m & bit(o_vib_high)) == 0, "the garbage value did NOT latch vib_high");
    check((m & bit(o_vib_extreme)) == 0, "nor vib_extreme");

    // And recovery works: a good sample restores the channel.
    check(b.sample(ch_vib, 5000, 3000) == Status::Ok, "");
    check(b.health(ch_vib, 3000) == Health::Good, "a good sample restores health");
    check((b.mask(3000) & bit(o_vib_unavailable)) == 0, "unavailable clears");
}

void test_read_fault() {
    std::printf("a read that fails outright\n");
    Bind b = fresh();
    check(b.sample(ch_vib, 12000, 1000) == Status::Ok, "");
    check(b.fault(ch_vib, 2000) == Status::Ok, "a fault is recorded");

    const std::uint32_t m = b.mask(2000);
    check(b.health(ch_vib, 2000) == Health::Faulted, "health distinguishes a failed read from a stale one");
    check((m & bit(o_vib_unavailable)) != 0, "unavailable is asserted");
    check((m & bit(o_vib_extreme)) == 0, "the previously latched threshold is suppressed");
}

/// The CAPECO condition. The transmitter was not silent and not out of range; it delivered a perfectly plausible
/// constant value, and the fill time was calculated by hand around it.
void test_flatline_is_not_staleness() {
    std::printf("a transmitter delivering fresh samples that never move\n");
    Bind b = fresh();

    std::uint32_t t = 1000;
    for (int i = 0; i < 30; ++i) {
        check(b.sample(ch_vib, 3000, t) == Status::Ok, "");
        t += 1000;
    }
    // 30 s of identical samples: fresh, in range, believable -- but not moving, and not yet past the 60 s window.
    check(b.health(ch_vib, t) == Health::Good, "the channel is good: fresh and in range");
    check((b.mask(t) & bit(o_vib_flatline)) == 0, "30 s unchanged is not yet a flatline");
    check((b.mask(t) & bit(o_vib_unavailable)) == 0, "and it is certainly not unavailable");

    for (int i = 0; i < 35; ++i) {
        check(b.sample(ch_vib, 3000, t) == Status::Ok, "");
        t += 1000;
    }
    check(b.health(ch_vib, t) == Health::Good, "still fresh and in range -- this is the dangerous part");
    check((b.mask(t) & bit(o_vib_flatline)) != 0, "past 60 s unchanged, the flatline observation is asserted");

    // Movement resets it.
    check(b.sample(ch_vib, 4000, t) == Status::Ok, "");
    check((b.mask(t) & bit(o_vib_flatline)) == 0, "a real change clears the flatline");
}

void test_flatline_epsilon_ignores_dither() {
    std::printf("dither is not movement\n");
    Bind b = fresh();
    std::uint32_t t = 1000;

    // Alternating by 0.04 mm/s, under the 0.05 epsilon. A failed transmitter with a noisy ADC looks exactly like
    // this, and treating the noise as liveness would hide the failure forever.
    for (int i = 0; i < 70; ++i) {
        check(b.sample(ch_vib, 3000 + (i % 2) * 40, t) == Status::Ok, "");
        t += 1000;
    }
    check(b.health(ch_vib, t) == Health::Good, "fresh and in range");
    check((b.mask(t) & bit(o_vib_flatline)) != 0, "sub-epsilon dither still counts as flatlined");

    // A change larger than epsilon does count.
    Bind c = fresh();
    t = 1000;
    for (int i = 0; i < 70; ++i) {
        check(c.sample(ch_vib, 3000 + (i % 2) * 200, t) == Status::Ok, "");
        t += 1000;
    }
    check((c.mask(t) & bit(o_vib_flatline)) == 0, "movement above epsilon is liveness");
}

void test_configure_rejects_unsafe_specs() {
    std::printf("configure refuses specifications that cannot be safe\n");

    {
        // No freshness requirement means the channel can never be reported dead.
        constexpr ChannelSpec bad[] = {{0, 1000, 0, 0, 0, 0, -1}};
        Bind b;
        check(b.configure(bad, 1, kThresholds, 1) == Status::BadSpec,
              "a channel with stale_after_ms == 0 is rejected");
    }
    {
        constexpr ChannelSpec bad[] = {{1000, 0, 5000, 0, 0, 0, -1}};
        Bind b;
        check(b.configure(bad, 1, kThresholds, 1) == Status::BadSpec, "an inverted valid range is rejected");
    }
    {
        constexpr ChannelSpec bad[] = {{0, 1000, 5000, 0, 0, -1, -1}};
        Bind b;
        check(b.configure(bad, 1, kThresholds, 1) == Status::BadSpec,
              "a channel with no unavailable observation is rejected");
    }
    {
        // Hysteresis in the wrong direction is positive feedback, not a deadband.
        constexpr ThresholdSpec bad[] = {{0, 0, Sense::Above, 7100, 7600}};
        Bind b;
        check(b.configure(kChannels, 1, bad, 1) == Status::BadSpec,
              "an Above threshold releasing above its assert point is rejected");
    }
    {
        constexpr ThresholdSpec bad[] = {{0, 0, Sense::Below, 800, 600}};
        Bind b;
        check(b.configure(kChannels, 1, bad, 1) == Status::BadSpec,
              "a Below threshold releasing below its assert point is rejected");
    }
    {
        constexpr ThresholdSpec bad[] = {{7, 0, Sense::Above, 100, 50}};
        Bind b;
        check(b.configure(kChannels, 1, bad, 1) == Status::BadSpec, "a threshold on a nonexistent channel is rejected");
    }
    {
        Bind b;
        check(b.configure(kChannels, 99, kThresholds, 1) == Status::CapacityExceeded,
              "exceeding channel capacity is reported, not silently truncated");
    }
    {
        Bind b;
        check(b.sample(0, 0, 0) == Status::NotConfigured, "sampling before configure is refused");
        check(b.mask(0) == 0, "an unconfigured binding asserts nothing");
    }
}

/// The contract, stated as a property and checked over a long pseudo-random run rather than by inspection.
/// "Above 7.1 for 5 s": the observation asserts only once the value has STAYED past `on` for the hold.
void test_hold_requires_a_sustained_condition() {
    std::printf("a held threshold asserts only after the condition is sustained\n");
    constexpr ThresholdSpec held[] = {{ch_vib, o_vib_high, Sense::Above, 7100, 6600, 5000}};
    Bind b;
    check(b.configure(kChannels, 1, held, 1) == Status::Ok, "a held threshold configures");
    const std::uint32_t hi = 1U << o_vib_high;

    // A spike: past `on` for 3 s, then back below. Never asserts, and the hold restarts.
    (void)b.sample(ch_vib, 8000, 1000);
    check((b.mask(1000) & hi) == 0U, "not asserted the moment it crosses");
    (void)b.sample(ch_vib, 8000, 4000);
    check((b.mask(4000) & hi) == 0U, "not asserted after 3 s of a 5 s hold");
    (void)b.sample(ch_vib, 7000, 4500);  // below on, above off: the condition was not sustained
    (void)b.sample(ch_vib, 8000, 5000);
    check((b.mask(9000) & hi) == 0U, "a dip below `on` restarts the hold: 4 s since the restart is not enough");

    // Sustained: matures exactly at 5 s after the restart, even with no new sample at that moment.
    check((b.mask(9999) & hi) == 0U, "one millisecond short of the hold");
    (void)b.sample(ch_vib, 8100, 9500);  // keep the channel fresh (stale_after is 5 s)
    check((b.mask(10000) & hi) != 0U, "asserted exactly when the hold elapses, without waiting for a sample");

    // Once asserted it behaves as ordinary hysteresis: the deadband holds it, `off` releases it at once.
    (void)b.sample(ch_vib, 7000, 11000);
    check((b.mask(11000) & hi) != 0U, "the deadband holds an asserted, held threshold");
    (void)b.sample(ch_vib, 6500, 12000);
    check((b.mask(12000) & hi) == 0U, "release at `off` is immediate -- delaying a release is the dangerous side");

    // And it starts again from nothing.
    (void)b.sample(ch_vib, 9000, 13000);
    check((b.mask(14000) & hi) == 0U, "after release, a new hold is needed");

    // A dead channel still asserts nothing, hold or no hold.
    check((b.mask(30000) & hi) == 0U, "a stale channel suppresses even a matured hold");
    check((b.mask(30000) & (1U << o_vib_unavailable)) != 0U, "and reports itself unavailable");
}

void test_zero_hold_is_unchanged() {
    std::printf("a zero hold behaves exactly as a threshold without one\n");
    constexpr ThresholdSpec zero[] = {{ch_vib, o_vib_high, Sense::Above, 7100, 6600, 0}};
    Bind a;
    Bind b;
    check(a.configure(kChannels, 1, kThresholds, 1) == Status::Ok && b.configure(kChannels, 1, zero, 1) == Status::Ok,
          "both configure");
    const std::int32_t trace[] = {5000, 7200, 7000, 6500, 6700, 7100, 9000, 6600, 7099, 7101};
    bool same = true;
    for (std::uint32_t i = 0; i < sizeof trace / sizeof trace[0]; ++i) {
        (void)a.sample(ch_vib, trace[i], 100 * i);
        (void)b.sample(ch_vib, trace[i], 100 * i);
        same = same && a.mask(100 * i) == b.mask(100 * i);
    }
    check(same, "identical masks over a trace crossing both setpoints");
}

void test_invariant_unavailable_excludes_thresholds() {
    std::printf("property: an unavailable channel never asserts a threshold\n");
    Bind b = fresh();

    std::uint32_t t = 0;
    std::uint32_t rng = 0x12345678U;
    auto next = [&rng]() {
        rng = rng * 1664525U + 1013904223U;
        return rng;
    };

    int violations = 0;
    int unavailable_seen = 0;
    int good_seen = 0;

    for (int step = 0; step < 200000; ++step) {
        const std::uint32_t r = next();
        t += (r % 3000U);
        const std::int32_t c = static_cast<std::int32_t>((r >> 16) % 2U);

        switch ((r >> 8) % 4U) {
            case 0: (void)b.fault(c, t); break;
            case 1: (void)b.sample(c, static_cast<std::int32_t>((r >> 4) % 60000U) - 2000, t); break;
            default: (void)b.sample(c, static_cast<std::int32_t>((r >> 4) % 15000U), t); break;
        }

        const std::uint32_t m = b.mask(t);
        for (std::int32_t chan = 0; chan < 2; ++chan) {
            const bool unavailable = (m & bit(kChannels[chan].unavailable_obs)) != 0;
            if (unavailable) { ++unavailable_seen; } else { ++good_seen; }

            for (std::int32_t k = 0; k < 3; ++k) {
                if (kThresholds[k].channel != chan) { continue; }
                const bool asserted = (m & bit(kThresholds[k].observation)) != 0;
                if (unavailable && asserted) { ++violations; }
            }
            // And a flatline is only ever reported for a believable channel: "the sensor is stuck" and "the sensor
            // is gone" are different findings and must not be reported together.
            if (unavailable && kChannels[chan].flatline_obs >= 0) {
                if ((m & bit(kChannels[chan].flatline_obs)) != 0) { ++violations; }
            }
        }
    }

    check(violations == 0, "no violation across 200,000 random steps");
    check(unavailable_seen > 1000, "the run actually exercised unavailable channels");
    check(good_seen > 1000, "and healthy ones");
}

}  // namespace

int main() {
    test_hold_requires_a_sustained_condition();
    test_zero_hold_is_unchanged();
    std::printf("binding layer tests\n\n");

    test_boot_state_is_blind_not_quiet();
    test_threshold_and_hysteresis();
    test_below_sense();
    test_staleness_suppresses_thresholds();
    test_clock_wraparound();
    test_out_of_range_never_latches();
    test_read_fault();
    test_flatline_is_not_staleness();
    test_flatline_epsilon_ignores_dither();
    test_configure_rejects_unsafe_specs();
    test_invariant_unavailable_excludes_thresholds();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
