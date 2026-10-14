// CAPECO, Tank 409, 23 October 2009 -- driven from raw instrument numbers.
//
// Everything before this ran on presence masks: someone had already decided which propositions were asserted. This
// program starts where a device actually starts, with milliamps converted to percent, and a clock. The casebook,
// the binding and the thresholds are all generated from tank.cb; nothing below chooses when the transmitter stops
// being believable.
//
// The accident, from the CSB report: the main tank was full, so a shipment was routed to four smaller tanks by a
// largely manual process. Tank 409's level transmitter was broken. Fill time was calculated by hand and
// overestimated. Nobody walked out to read the local gauge. The tank overflowed, the vapour cloud found an ignition
// source, and the explosion damaged around 300 homes and businesses up to 1.25 miles away. There were no
// fatalities. The CSB's key finding was that there were no redundant or independent safeguards against overfilling.
//
// The question this program asks is narrow and answerable: fed the same numbers, in the same order, would the
// machine have stopped the transfer? And -- the part that matters more -- would it have said WHY in terms an
// operator could have acted on?

#include <cstdint>
#include <cstdio>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/binding/binding.hpp"
#include "quorum/witness/derivation.hpp"
#include "quorum/witness/render.hpp"
#include "tank_generated.hpp"

namespace gen = quorum::casebook::gen::tank_409;
namespace bind = quorum::binding;

using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    std::printf("  %s  %s\n", condition ? "OK  " : "FAIL", what);
    if (!condition) { ++g_failures; }
}

constexpr std::int32_t milli(double v) { return static_cast<std::int32_t>(v * 1000.0 + 0.5); }

/// Observations no instrument supplies: a gauge somebody walked out and read, a calculation somebody did on paper.
struct ByHand {
    bool gauge_normal = false;
    bool gauge_high = false;
    bool fill_calc_room = false;
    bool hi_level_alarm = false;
    bool dike_liquid = false;

    [[nodiscard]] std::uint32_t mask() const {
        std::uint32_t m = 0;
        auto set = [&m](bool on, std::int32_t o) {
            if (on) { m |= 1U << static_cast<std::uint32_t>(o); }
        };
        set(gauge_normal, gen::gauge_normal);
        set(gauge_high, gen::gauge_high);
        set(fill_calc_room, gen::fill_calc_room);
        set(hi_level_alarm, gen::hi_level_alarm);
        set(dike_liquid, gen::dike_liquid);
        return m;
    }
};

struct Rig {
    gen::CaseBinding binding;
    gen::CasePolicy policy;

    Rig() {
        if (gen::make_binding(binding) != bind::Status::Ok) {
            std::printf("FAIL  binding configuration\n");
            ++g_failures;
        }
        if (gen::make_policy(policy) != Status::Ok) {
            std::printf("FAIL  policy configuration\n");
            ++g_failures;
        }
    }

    [[nodiscard]] Verdict run(std::uint32_t now_ms, const ByHand& by_hand, std::uint32_t* out_mask = nullptr) {
        const std::uint32_t mask = binding.mask(now_ms) | by_hand.mask();
        if (out_mask != nullptr) { *out_mask = mask; }

        gen::CaseFramework fw;
        gen::CaseSolver solver;
        if (gen::instantiate(fw, mask) != Status::Ok) {
            ++g_failures;
            return Verdict{};
        }
        return quorum::arbiter::decide(fw, solver, policy);
    }
};

void show(const char* when, const Verdict& v, std::uint32_t mask) {
    std::printf("    %-34s %-13s -> %s\n", when, outcome_name(v.outcome), gen::name_of(v.action.get()));
    std::printf("      asserted:");
    bool any = false;
    for (std::int32_t o = 0; o < gen::kObservationCount; ++o) {
        if ((mask & (1U << static_cast<std::uint32_t>(o))) != 0U) {
            std::printf(" %s", gen::name_of(o));
            any = true;
        }
    }
    std::printf("%s\n", any ? "" : " (nothing)");
}

// ─────────────────────────────────────────────────────────────────────────────

/// Before any instrument has been read. The device must not mistake ignorance for an all-clear.
void test_cold_start() {
    std::printf("\ncold start, before any instrument has been read\n");
    Rig rig;
    std::uint32_t mask = 0;
    const Verdict v = rig.run(0, ByHand{}, &mask);
    show("t = 0, nothing sampled", v, mask);

    expect(v.outcome == quorum::arbiter::Outcome::Decided, "the framework decides");
    expect(v.action == Arg{gen::stop_transfer},
           "a device that has read nothing stops the transfer, rather than idling");
    expect((mask & (1U << gen::tank_level_unavailable)) != 0,
           "the level channel reports unavailable, not a normal level");
}

/// Normal operation. A healthy transfer must be allowed to run, or the product is a nuisance and gets bypassed --
/// which is how safety systems actually fail in the field.
void test_healthy_transfer() {
    std::printf("\na healthy transfer, transmitter working\n");
    Rig rig;
    ByHand by_hand;
    std::uint32_t t = 1000;
    std::uint32_t mask = 0;

    // A level that moves as the tank fills, and a live inlet meter.
    double level = 40.0;
    Verdict v{};
    for (int i = 0; i < 20; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, milli(level), t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.sample(gen::ch_gas_lel, milli(0.0), t);
        level += 0.5;
        t += 5000;
        v = rig.run(t, by_hand, &mask);
    }
    show("100 s in, level rising 40 -> 50 %", v, mask);
    expect(v.action == Arg{gen::continue_transfer}, "a measurable transfer is allowed to continue");
}

/// The accident. The transmitter is not silent and not out of range; it reports a plausible, unchanging value while
/// the tank fills underneath it, and the fill time was worked out on paper around that number.
void test_the_accident() {
    std::printf("\nthe CAPECO condition: a transmitter frozen at a plausible value\n");
    Rig rig;
    ByHand by_hand;
    by_hand.fill_calc_room = true;  // the hand calculation said there was room. It was wrong.
    // Nobody walked out to read the local gauge, so gauge_normal and gauge_high stay false.

    std::uint32_t t = 1000;
    std::uint32_t mask = 0;
    Verdict v{};

    // The transmitter froze at 45 %. Every sample is fresh, in range, and entirely plausible.
    constexpr std::int32_t frozen = milli(45.0);

    // Twenty minutes in. Not yet past the thirty-minute flatline window.
    for (int i = 0; i < 240; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, frozen, t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.sample(gen::ch_gas_lel, milli(0.0), t);
        t += 5000;
    }
    v = rig.run(t, by_hand, &mask);
    show("20 min, frozen at 45 %", v, mask);
    expect(v.action == Arg{gen::continue_transfer},
           "before the flatline window elapses the transfer continues -- the device is not clairvoyant");
    expect(rig.binding.health(gen::ch_tank_level, t) == bind::Health::Good,
           "and the channel is genuinely 'good': fresh, in range, plausible");

    // Past thirty minutes of a value that has not moved.
    for (int i = 0; i < 180; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, frozen, t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.sample(gen::ch_gas_lel, milli(0.0), t);
        t += 5000;
    }
    v = rig.run(t, by_hand, &mask);
    show("35 min, still frozen at 45 %", v, mask);

    expect((mask & (1U << gen::tank_level_flatline)) != 0,
           "the flatline is detected from the numbers alone, with no hand-authored observation");
    expect(rig.binding.health(gen::ch_tank_level, t) == bind::Health::Good,
           "the channel is STILL 'good' -- this is why staleness alone would have missed it");
    expect((mask & (1U << gen::xmtr_normal)) != 0,
           "the transmitter is still asserting a normal level, because that is what it reports");
    expect(v.action == Arg{gen::stop_transfer},
           "the transfer CAPECO continued is stopped");

    // The reason has to be legible, not just correct. An operator who cannot see why will bypass it.
    std::printf("\n  why it stopped:\n");
    {
        gen::CaseFramework fw;
        gen::CaseSolver solver;
        (void)gen::instantiate(fw, mask);
        (void)solver.solve(fw);

        quorum::witness::Derivation<256, gen::kMaxArgs> d;
        (void)d.build(fw, solver, Arg{gen::stop_transfer}, Arg{gen::kClosedWorld});
        quorum::witness::render(d, gen::name_of);
    }

    // The counterfactual the CSB asked about: one independent measurement.
    std::printf("\n  the counterfactual -- somebody walks out and reads the gauge:\n");
    by_hand.gauge_high = true;
    v = rig.run(t, by_hand, &mask);
    show("gauge read by hand: HIGH", v, mask);
    expect(v.action == Arg{gen::stop_transfer}, "an independent reading of 'high' also stops the transfer");

    by_hand.gauge_high = false;
    by_hand.gauge_normal = true;
    v = rig.run(t, by_hand, &mask);
    show("gauge read by hand: normal", v, mask);
    expect(v.action == Arg{gen::continue_transfer},
           "an independent reading of 'normal' restores the measurement and permits the transfer");
}

/// Staleness would not have caught CAPECO, and it is worth demonstrating that it catches a different failure.
void test_transmitter_goes_silent() {
    std::printf("\nthe other failure: a transmitter that stops reporting\n");
    Rig rig;
    ByHand by_hand;
    std::uint32_t t = 1000;
    std::uint32_t mask = 0;

    double level = 40.0;
    for (int i = 0; i < 20; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, milli(level), t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.sample(gen::ch_gas_lel, milli(0.0), t);
        level += 0.5;
        t += 5000;
    }
    Verdict v = rig.run(t, by_hand, &mask);
    expect(v.action == Arg{gen::continue_transfer}, "running normally first");

    // The bus goes quiet. Eleven seconds is past the ten-second freshness limit.
    t += 11000;
    v = rig.run(t, by_hand, &mask);
    show("11 s with no sample", v, mask);
    expect(rig.binding.health(gen::ch_tank_level, t) == bind::Health::Stale, "the channel reports stale");
    expect((mask & (1U << gen::xmtr_normal)) == 0,
           "the last known level is NOT repeated as though it were current");
    expect(v.action == Arg{gen::stop_transfer}, "and the transfer stops");
}

/// A release detector that has failed takes the last safeguard with it.
void test_blind_to_release() {
    std::printf("\nthe last safeguard fails: LEL detector offline\n");
    Rig rig;
    ByHand by_hand;
    std::uint32_t t = 1000;
    std::uint32_t mask = 0;

    double level = 40.0;
    for (int i = 0; i < 20; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, milli(level), t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.sample(gen::ch_gas_lel, milli(0.0), t);
        level += 0.5;
        t += 5000;
    }
    expect(rig.run(t, by_hand).action == Arg{gen::continue_transfer}, "running normally first");

    // Keep the level channel alive but let the gas detector fail its read.
    for (int i = 0; i < 12; ++i) {
        (void)rig.binding.sample(gen::ch_tank_level, milli(level), t);
        (void)rig.binding.sample(gen::ch_inlet_rate, milli(400.0), t);
        (void)rig.binding.sample(gen::ch_outlet_rate, milli(0.0), t);
        (void)rig.binding.fault(gen::ch_gas_lel, t);
        level += 0.5;
        t += 5000;
    }
    const Verdict v = rig.run(t, by_hand, &mask);
    show("gas detector faulted, level fine", v, mask);
    expect(rig.binding.health(gen::ch_gas_lel, t) == bind::Health::Faulted, "the fault is distinguished from stale");
    expect((mask & (1U << gen::tank_level_unavailable)) == 0,
           "losing one channel does not blind the device to the others");
    expect(v.action == Arg{gen::stop_transfer},
           "a transfer whose last safeguard is gone is not permitted to continue");
}

}  // namespace

int main() {
    std::printf("Tank 409, CAPECO -- from raw instrument numbers\n");
    std::printf("casebook %s v%d, from %s\n", gen::kCasebookName, static_cast<int>(gen::kCasebookVersion),
                gen::kSourceDocument);
    std::printf("%d channels, %d thresholds, %d observations, %d arguments\n", gen::kChannelCount,
                gen::kThresholdCount, gen::kObservationCount, static_cast<int>(gen::argument_count));

    test_cold_start();
    test_healthy_transfer();
    test_the_accident();
    test_transmitter_goes_silent();
    test_blind_to_release();

    std::printf("\n%s\n", g_failures == 0 ? "all scenarios behaved as recorded"
                                          : "SCENARIO FAILURE");
    return g_failures == 0 ? 0 : 1;
}
