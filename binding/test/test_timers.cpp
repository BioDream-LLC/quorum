// Timed readings (binding::Timers): targeted cases for each kind, the configure-time refusals, and a comparison with
// a second implementation that keeps the whole history and applies the definitions in LANGUAGE.md literally.
//
// The second implementation is the point. Timers keeps a few timestamps per reading so it fits a microcontroller;
// the reference below keeps every step it has ever seen and asks the question in the words of the documentation
// ("did the source become set within T after the earlier reading became set?"). They share no code, and they are run
// side by side over random histories, including histories that cross the 32-bit millisecond wrap.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "quorum/binding/binding.hpp"

namespace {

using quorum::binding::kMaxTimerCount;
using quorum::binding::Status;
using quorum::binding::TimerKind;
using quorum::binding::Timers;
using quorum::binding::TimerSpec;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

constexpr std::uint32_t bit(int o) { return 1U << static_cast<std::uint32_t>(o); }

// Observations 0..3 are base readings; 4..6 are the timers' outputs.
constexpr int kStart = 0, kPressure = 1, kTrip = 2, kHot = 3, kDeadHead = 4, kRepeated = 5, kRanHot = 6;
constexpr int kObservations = 7;

const TimerSpec kSpecs[] = {
    {TimerKind::After, kDeadHead, kPressure, kStart, 10000U, 0},
    {TimerKind::Count, kRepeated, kTrip, -1, 3600000U, 3},
    {TimerKind::Recent, kRanHot, kHot, -1, 300000U, 0},
};

Timers<3> make() {
    Timers<3> t;
    check(t.configure(kSpecs, 3, kObservations) == Status::Ok, "the three-timer table configures");
    return t;
}

void test_after() {
    std::printf("A within T after B\n");
    Timers<3> t = make();
    std::uint32_t now = 1000;
    check((t.step(0, now) & bit(kDeadHead)) == 0, "nothing set: not set");
    check((t.step(bit(kStart), now += 100) & bit(kDeadHead)) == 0, "a start alone: not set");
    check((t.step(bit(kStart) | bit(kPressure), now += 9900) & bit(kDeadHead)) != 0,
          "pressure 10 s exactly after the start: set (the window is inclusive)");
    check((t.step(bit(kStart) | bit(kPressure), now += 60000) & bit(kDeadHead)) != 0,
          "stays set while the pressure stays high, however long");
    check((t.step(bit(kStart), now += 10) & bit(kDeadHead)) == 0, "clears when the pressure clears");
    check((t.step(bit(kStart) | bit(kPressure), now += 10) & bit(kDeadHead)) == 0,
          "pressure again, 70 s after the start: not set");

    Timers<3> u = make();
    now = 0;
    (void)u.step(bit(kStart), now);
    check((u.step(bit(kPressure), now += 10001) & bit(kDeadHead)) == 0, "10.001 s after the start: not set");

    Timers<3> v = make();
    now = 0;
    (void)v.step(bit(kPressure), now);
    check((v.step(bit(kPressure) | bit(kStart), now += 1000) & bit(kDeadHead)) == 0,
          "pressure first, then the start: not 'after', not set");

    Timers<3> w = make();
    check((w.step(bit(kPressure) | bit(kStart), 50) & bit(kDeadHead)) != 0,
          "both at the same step counts as after (the earlier reading is taken first)");

    Timers<3> x = make();
    now = 0;
    (void)x.step(bit(kStart), now);
    (void)x.step(0, now += 5000);
    check((x.step(bit(kPressure), now += 4000) & bit(kDeadHead)) != 0,
          "the start may already have ended: it is when it began that counts");
}

void test_recent() {
    std::printf("A within last T\n");
    Timers<3> t = make();
    std::uint32_t now = 0;
    check((t.step(0, now) & bit(kRanHot)) == 0, "never hot: not set");
    check((t.step(bit(kHot), now += 10) & bit(kRanHot)) != 0, "hot now: set");
    check((t.step(0, now += 300000) & bit(kRanHot)) != 0, "cooled exactly 5 minutes ago: still set");
    check((t.step(0, now += 1) & bit(kRanHot)) == 0, "5 minutes and 1 ms: clear");
    check((t.step(bit(kHot), now += 1) & bit(kRanHot)) != 0, "hot again: set again");
}

void test_count() {
    std::printf("A N times in T\n");
    Timers<3> t = make();
    std::uint32_t now = 0;
    const auto trip = [&](std::uint32_t gap) {
        (void)t.step(0, now += gap);
        return (t.step(bit(kTrip), now += 1) & bit(kRepeated)) != 0;
    };
    check(!trip(0), "one trip: not set");
    check(!trip(1000), "two trips: not set");
    check(trip(1000), "a third trip within the hour: set");
    check((t.step(bit(kTrip), now += 10) & bit(kRepeated)) != 0, "a relay held tripped is one trip, not many");
    check((t.step(0, now += 10) & bit(kRepeated)) != 0, "stays set after the relay resets");
    // The first of the three was at 1 ms; it leaves the window 3,600,000 ms later.
    (void)t.step(0, 3600001U);
    check((t.step(0, 3600001U) & bit(kRepeated)) != 0, "at exactly one hour after the first trip: still set");
    check((t.step(0, 3600002U) & bit(kRepeated)) == 0, "one millisecond later: clear (two trips in the window)");

    // More edges than the ring holds: only the newest matter, so wrapping the ring must not lose the count.
    Timers<3> u = make();
    now = 0;
    for (int i = 0; i < 3 * kMaxTimerCount; ++i) {
        (void)u.step(0, now += 2000000);
        (void)u.step(bit(kTrip), now += 1);
    }
    check((u.step(0, now) & bit(kRepeated)) == 0, "48 trips spread 33 minutes apart: never 3 in an hour");
    (void)u.step(0, now += 1);
    (void)u.step(bit(kTrip), now += 1);
    (void)u.step(0, now += 1);
    check((u.step(bit(kTrip), now += 1) & bit(kRepeated)) != 0, "then three within seconds: set");
}

void test_wraparound() {
    std::printf("the millisecond clock rolling through zero\n");
    Timers<3> t = make();
    std::uint32_t now = 0xFFFFFFFFU - 4000U;
    (void)t.step(bit(kStart), now);
    now += 8000U;  // wraps
    check((t.step(bit(kStart) | bit(kPressure), now) & bit(kDeadHead)) != 0, "8 s after a start, across the wrap: set");
    Timers<3> u = make();
    now = 0xFFFFFFFFU - 1000U;
    (void)u.step(bit(kHot), now);
    check((u.step(0, now + 200000U) & bit(kRanHot)) != 0, "a recent overheat survives the wrap");
    check((u.step(0, now + 400000U) & bit(kRanHot)) == 0, "and expires on time after it");
}

void test_long_quiet() {
    std::printf("an old event does not come back when the clock wraps 49.7 days later\n");
    // The device steps about every millisecond; stepping once a minute is enough to show it, and keeps the test fast.
    const std::uint32_t wrap_ms = 0xFFFFFFFFU;  // 2^32 - 1 ms, one turn of the clock
    Timers<3> t = make();
    std::uint32_t now = 1000U;
    (void)t.step(bit(kStart) | bit(kHot), now);
    for (int trip = 0; trip < 3; ++trip) {
        (void)t.step(bit(kStart) | bit(kTrip), now += 10);
        (void)t.step(bit(kStart), now += 10);
    }
    (void)t.step(0, now += 10);
    bool ever = false;
    const std::uint32_t end = now + wrap_ms + 600000U;  // ten minutes past one full turn
    for (std::uint64_t q = now; q < static_cast<std::uint64_t>(now) + wrap_ms + 600000U; q += 60000U) {
        const std::uint32_t at = static_cast<std::uint32_t>(q);
        if (static_cast<std::uint64_t>(q) - now > 3600010U && (t.step(0, at) & (bit(kRanHot) | bit(kRepeated))) != 0) {
            ever = true;
        } else {
            (void)t.step(0, at);
        }
    }
    (void)end;
    check(!ever, "an overheat and three trips 49.7 days ago stay forgotten through the wrap");
    // A start 49.7 days ago must not make a pressure rise now count as "within 10 s after".
    Timers<3> u = make();
    std::uint32_t at = 5000U;
    (void)u.step(bit(kStart), at);
    (void)u.step(0, at += 10);
    for (std::uint64_t q = 0; q + 60000U < wrap_ms; q += 60000U) { (void)u.step(0, at += 60000U); }
    at = 5000U + wrap_ms + 1U + 3000U;  // 3 s after the start, one full turn later
    check((u.step(bit(kPressure), at) & bit(kDeadHead)) == 0,
          "a pressure rise one clock turn after an old start is not 'within 10 s after' it");
}

void test_idempotent_and_outputs() {
    std::printf("stepping twice at one moment changes nothing\n");
    Timers<3> t = make();
    (void)t.step(bit(kStart), 0);
    const std::uint32_t a = t.step(bit(kStart) | bit(kPressure), 5);
    const std::uint32_t b = t.step(bit(kStart) | bit(kPressure), 5);
    check(a == b && (a & bit(kDeadHead)) != 0, "the same answer twice");
    check(t.outputs() == (bit(kDeadHead) | bit(kRepeated) | bit(kRanHot)), "outputs() names exactly the timer bits");
    check((t.step(0xFFFFFFFFU & ~t.outputs(), 6) & ~t.outputs()) == 0, "step() never sets a base reading");
}

void test_start_up() {
    std::printf("start-up\n");
    Timers<3> t = make();
    check((t.step(bit(kStart) | bit(kPressure), 0) & bit(kDeadHead)) != 0,
          "readings already set at the first step count as just set");
}

void test_configure_refuses() {
    std::printf("configure refuses tables that cannot mean anything\n");
    const auto refused = [](TimerSpec s, const char* what) {
        Timers<2> t;
        check(t.configure(&s, 1, kObservations) == Status::BadSpec, what);
    };
    refused({TimerKind::Recent, kRanHot, kHot, -1, 0U, 0}, "a zero window");
    refused({TimerKind::Recent, kHot, kHot, -1, 1000U, 0}, "a timer that watches itself");
    refused({TimerKind::Recent, 9, kHot, -1, 1000U, 0}, "an output beyond the observations");
    refused({TimerKind::Recent, kRanHot, 9, -1, 1000U, 0}, "a source beyond the observations");
    refused({TimerKind::After, kDeadHead, kPressure, -1, 1000U, 0}, "After with no earlier reading");
    refused({TimerKind::After, kDeadHead, kPressure, kPressure, 1000U, 0}, "After waiting for itself");
    refused({TimerKind::Recent, kRanHot, kHot, kStart, 1000U, 0}, "Recent with an earlier reading");
    refused({TimerKind::Count, kRepeated, kTrip, -1, 1000U, 1}, "a count of 1");
    refused({TimerKind::Count, kRepeated, kTrip, -1, 1000U, kMaxTimerCount + 1}, "a count above the ring");
    refused({static_cast<TimerKind>(7), kRanHot, kHot, -1, 1000U, 0}, "an unknown kind");
    const TimerSpec chained[] = {{TimerKind::Recent, kRanHot, kHot, -1, 1000U, 0},
                                 {TimerKind::Recent, kDeadHead, kRanHot, -1, 1000U, 0}};
    Timers<2> c;
    check(c.configure(chained, 2, kObservations) == Status::Ok, "a timer watching an earlier timer");
    const TimerSpec backwards[] = {{TimerKind::Recent, kDeadHead, kRanHot, -1, 1000U, 0},
                                   {TimerKind::Recent, kRanHot, kHot, -1, 1000U, 0}};
    Timers<2> c2;
    check(c2.configure(backwards, 2, kObservations) == Status::BadSpec, "a timer watching a LATER timer");
    const TimerSpec loop[] = {{TimerKind::Recent, kDeadHead, kRanHot, -1, 1000U, 0},
                              {TimerKind::Recent, kRanHot, kDeadHead, -1, 1000U, 0}};
    Timers<2> c3;
    check(c3.configure(loop, 2, kObservations) == Status::BadSpec, "two timers watching each other");
    const TimerSpec late_after[] = {{TimerKind::After, kDeadHead, kPressure, kRanHot, 1000U, 0},
                                    {TimerKind::Recent, kRanHot, kHot, -1, 1000U, 0}};
    Timers<2> c4;
    check(c4.configure(late_after, 2, kObservations) == Status::BadSpec, "an After whose earlier reading is a later timer");
    const TimerSpec twice[] = {{TimerKind::Recent, kRanHot, kHot, -1, 1000U, 0},
                               {TimerKind::Recent, kRanHot, kTrip, -1, 1000U, 0}};
    Timers<2> d;
    check(d.configure(twice, 2, kObservations) == Status::BadSpec, "two timers setting one reading");
    Timers<2> e;
    check(e.configure(kSpecs, 3, kObservations) == Status::CapacityExceeded, "more timers than slots");
}

// ── the reference: the whole history, and the definitions applied literally ──────────────────────────────────────

struct Step {
    std::uint64_t at;  ///< unwrapped time
    std::uint32_t mask;
};

bool has(std::uint32_t m, int o) { return (m & bit(o)) != 0U; }
bool rose(const std::vector<Step>& h, std::size_t i, int o) {
    return has(h[i].mask, o) && (i == 0 || !has(h[i - 1].mask, o));
}

// The reference for one timer, over a history of FULL masks (base readings and every timer output so far). A chain
// is handled by working out each step's full mask in table order, in reference_all below.
bool reference(const TimerSpec& s, const std::vector<Step>& h) {
    const std::size_t k = h.size() - 1;
    const std::uint64_t now = h[k].at;
    switch (s.kind) {
        case TimerKind::After: {
            if (!has(h[k].mask, s.source)) { return false; }
            std::size_t j = k;  // where the source's current run began
            while (j > 0 && has(h[j - 1].mask, s.source)) { --j; }
            for (std::size_t i = 0; i <= j; ++i) {
                if (rose(h, i, s.after) && h[j].at - h[i].at <= s.window_ms) { return true; }
            }
            return false;
        }
        case TimerKind::Recent:
            for (std::size_t i = 0; i <= k; ++i) {
                if (has(h[i].mask, s.source) && now - h[i].at <= s.window_ms) { return true; }
            }
            return false;
        case TimerKind::Count: {
            int n = 0;
            for (std::size_t i = 0; i <= k; ++i) {
                if (rose(h, i, s.source) && now - h[i].at <= s.window_ms) { ++n; }
            }
            return n >= s.times;
        }
    }
    return false;
}

// Append one step of base readings to a full-mask history, with the timer outputs the definitions give, in table
// order: a timer watching an earlier timer reads that timer's output from this same step.
std::uint32_t reference_all(const TimerSpec* specs, int n, std::vector<Step>& full, std::uint64_t at, std::uint32_t base) {
    full.push_back({at, base});
    for (int i = 0; i < n; ++i) {
        if (reference(specs[i], full)) { full.back().mask |= bit(specs[i].observation); }
    }
    return full.back().mask;
}

void test_chains() {
    std::printf("a timed reading that watches another\n");
    // ran_hot = hot within the last 300 s; dead_head (reused as "ran hot twice") = ran_hot 2 times in 1 h.
    const TimerSpec specs[] = {{TimerKind::Recent, kRanHot, kHot, -1, 300000U, 0},
                               {TimerKind::Count, kRepeated, kRanHot, -1, 3600000U, 2}};
    Timers<2> t;
    check(t.configure(specs, 2, kObservations) == Status::Ok, "the chained table configures");
    std::uint32_t now = 0;
    check((t.step(bit(kHot), now) & bit(kRepeated)) == 0, "hot once: not twice");
    check((t.step(bit(kHot), now += 1000) & bit(kRepeated)) == 0, "still hot: still once");
    check((t.step(0, now += 1000) & bit(kRanHot)) != 0, "cooled: ran_hot remembers");
    check((t.step(bit(kHot), now += 1000) & bit(kRepeated)) == 0, "hot again while ran_hot is still set: still once");
    check((t.step(0, now += 400000) & bit(kRanHot)) == 0, "ran_hot expires");
    check((t.step(bit(kHot), now += 1000) & bit(kRepeated)) != 0,
          "hot again after ran_hot expired: ran_hot rises a second time, in this same step: twice in the hour");
}

void test_against_reference() {
    std::printf("against a whole-history reference, random histories\n");
    std::mt19937 gen(20261025U);
    const auto rng = [&gen]() { return static_cast<std::uint32_t>(gen()); };
    long compared = 0, mismatches = 0, set_seen = 0;
    for (int trial = 0; trial < 400; ++trial) {
        // Random table: three timers over four base readings, with small windows so they fire and expire often. On
        // half the trials a timer may also watch the timers before it, so chains of up to three are compared too.
        TimerSpec specs[3];
        const bool chains = trial % 4 >= 2;
        for (int i = 0; i < 3; ++i) {
            const auto kind = static_cast<TimerKind>(rng() % 3U);
            // Readings it may watch: the four base readings, and with chains the outputs of timers 0..i-1 (4..3+i).
            const unsigned pool = chains ? 4U + static_cast<unsigned>(i) : 4U;
            const int src = static_cast<int>(rng() % pool);
            int after = -1;
            if (kind == TimerKind::After) {
                after = static_cast<int>((static_cast<unsigned>(src) + 1U + rng() % (pool - 1U)) % pool);
            }
            specs[i] = {kind, 4 + i, src, after, 1U + rng() % 400U, kind == TimerKind::Count ? 2 + static_cast<int>(rng() % 5U) : 0};
        }
        Timers<3> t;
        check(t.configure(specs, 3, kObservations) == Status::Ok, "a random table configures");
        std::vector<Step> h;
        // Start near the wrap on alternate trials.
        std::uint64_t at = trial % 2 == 0 ? 0ULL : 0xFFFFFFFFULL - 2000ULL;
        std::uint32_t base = 0;
        for (int step = 0; step < 300; ++step) {
            at += rng() % 60U;  // repeated moments included
            if (rng() % 3U == 0U) { base ^= bit(static_cast<int>(rng() % 4U)); }
            const std::uint32_t full = reference_all(specs, 3, h, at, base);
            const std::uint32_t got = t.step(base, static_cast<std::uint32_t>(at));
            for (int i = 0; i < 3; ++i) {
                const bool want = has(full, 4 + i);
                ++compared;
                if (want) { ++set_seen; }
                if (want != has(got, 4 + i)) {
                    if (mismatches < 5) {
                        std::printf("  mismatch: trial %d step %d timer %d kind %d\n", trial, step, i,
                                    static_cast<int>(specs[i].kind));
                    }
                    ++mismatches;
                }
            }
        }
    }
    std::printf("  %ld timer-steps compared, %ld of them set\n", compared, set_seen);
    check(mismatches == 0, "the device timers agree with the whole-history reference in every step");
    check(set_seen > compared / 10, "the random histories actually set the timers often");
}

}  // namespace

int main() {
    std::printf("timed readings\n\n");
    test_after();
    test_recent();
    test_count();
    test_wraparound();
    test_long_quiet();
    test_chains();
    test_idempotent_and_outputs();
    test_start_up();
    test_configure_refuses();
    test_against_reference();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
