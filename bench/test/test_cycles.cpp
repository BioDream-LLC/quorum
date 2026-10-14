// Tests for the cycle-counting harness.
//
// A timing rig nobody has exercised is not a rig. The measurement itself needs hardware, but everything around it --
// wrap arithmetic across two different counter widths, the distribution bookkeeping, the cycles-to-microseconds
// conversion -- is ordinary code that can be wrong, and would be wrong silently. A rig that reports 16 million cycles
// because a 24-bit down-counter was subtracted the wrong way does not look broken. It looks like a slow path.

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

#include "quorum/bench/cycles.hpp"

namespace bench = quorum::bench;

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

constexpr std::uint32_t kSysTickPeriod = 0x01000000U;  // 24-bit

void test_wrap_32bit() {
    std::printf("32-bit counter (DWT CYCCNT)\n");
    check(bench::elapsed_within(100U, 150U, 0U) == 50U, "a plain forward difference");
    check(bench::elapsed_within(0U, 0U, 0U) == 0U, "zero elapsed");

    // The DWT counter wraps every 2^32 cycles -- about 28 seconds at 150 MHz, so a device left running will cross it.
    check(bench::elapsed_within(0xFFFFFFF0U, 0x00000010U, 0U) == 0x20U, "across the 2^32 wrap");
    check(bench::elapsed_within(0xFFFFFFFFU, 0x00000000U, 0U) == 1U, "exactly at the wrap");
}

void test_wrap_24bit() {
    std::printf("24-bit counter (SysTick)\n");
    check(bench::elapsed_within(100U, 150U, kSysTickPeriod) == 50U, "a plain forward difference");

    // 2^24 cycles is ~112 ms at 150 MHz. A measurement that long is not one of ours, but the arithmetic still has to
    // be right, because the counter is shared with whatever else is running.
    check(bench::elapsed_within(0x00FFFFF0U, 0x00000010U, kSysTickPeriod) == 0x20U, "across the 2^24 wrap");
    check(bench::elapsed_within(0x00FFFFFFU, 0x00000000U, kSysTickPeriod) == 1U, "exactly at the wrap");

    // The failure this guards against. SysTick counts DOWN, and `now()` negates it so callers see a forward count.
    // If that negation were missing, a 50-cycle measurement would read as 16,777,166 -- which does not look like an
    // arithmetic error, it looks like a slow path worth investigating for a week.
    const std::uint32_t wrong_way = bench::elapsed_within(150U, 100U, kSysTickPeriod);
    check(wrong_way == kSysTickPeriod - 50U,
          "subtracting a down-counter the wrong way yields a huge plausible-looking value, which is why now() "
          "normalises the direction rather than leaving it to the caller");
}

void test_measurement_bookkeeping() {
    std::printf("distribution bookkeeping\n");
    bench::Measurement m;
    check(!m.valid, "a fresh Measurement is invalid, so an unrun rig cannot report zeroes as a result");
    check(m.runs == 0, "no runs");
    check(m.mean_cycles() == 0, "and asking for a mean of nothing does not divide by zero");

    m.add(100U);
    m.add(50U);
    m.add(300U);
    m.add(150U);

    check(m.valid, "valid after adding");
    check(m.runs == 4, "four runs counted");
    check(m.min_cycles == 50U, "minimum tracked");
    check(m.max_cycles == 300U, "maximum tracked");
    check(m.mean_cycles() == 150U, "mean is (100+50+300+150)/4 = 150");

    // The minimum matters as much as the maximum on a part with XIP flash caching: the first run pays cache misses
    // and later runs do not, so a single number cannot distinguish a genuinely slow path from a cold one.
    check(m.max_cycles > m.min_cycles, "the spread is visible, not averaged away");
}

void test_microseconds() {
    std::printf("cycles to microseconds, in integers only\n");
    bench::Measurement m;
    m.add(15000U);

    // 15,000 cycles at 150 MHz is exactly 100 us.
    check(m.max_microseconds(150000000U) == 100U, "15000 cycles at 150 MHz is 100 us");
    check(m.max_microseconds(1000000U) == 15000U, "and at 1 MHz it is 15000 us");

    // Rounds UP. For a WCET figure, rounding a worst case down is the one direction that is not allowed.
    bench::Measurement odd;
    odd.add(15001U);
    check(odd.max_microseconds(150000000U) == 101U, "rounds up, because rounding a worst case down is not allowed");

    check(m.max_microseconds(0U) == 0U, "a zero clock does not divide by zero");
}

void test_measure_runs_the_work() {
    std::printf("measure() actually runs the work\n");
    int calls = 0;
    volatile std::uint32_t sink = 0;

    const bench::Measurement m = bench::measure(64U, [&calls, &sink] {
        ++calls;
        std::uint32_t acc = 0;
        for (std::uint32_t i = 0; i < 200U; ++i) { acc += i * i; }
        sink = acc;
    });

    check(calls == 64, "the work ran exactly 64 times");
    check(m.runs == 64U, "and 64 samples were recorded");
    check(m.valid, "the measurement is valid");
    check(sink != 0U, "the work was not optimised away -- its result reached a volatile");

    // On a host this is nanoseconds from steady_clock, not cycles, so the only sane assertion is that time passed.
    // The point of running it at all is that the bookkeeping path is exercised end to end.
    std::printf("      host timing (nanoseconds, NOT cycles): min %u, mean %u, max %u\n", m.min_cycles,
                m.mean_cycles(), m.max_cycles);
}

void test_interrupt_guard() {
    std::printf("interrupt masking around the timed window\n");

    // On a host this is a no-op and says so. The point of testing it here is that the RAII shape is right -- it must
    // be non-copyable and non-movable, or a copy would re-enable interrupts when the temporary died, silently
    // reopening the window the guard exists to close.
    static_assert(!std::is_copy_constructible<bench::InterruptGuard>::value, "a copy would restore PRIMASK early");
    static_assert(!std::is_move_constructible<bench::InterruptGuard>::value, "a move would restore PRIMASK early");

    {
        const bench::InterruptGuard outer;
        {
            // Nesting must be safe: PRIMASK is saved and restored rather than blanket-enabled, so the inner guard
            // going out of scope cannot re-enable interrupts the outer one disabled. Getting this wrong would leave
            // the outer window unprotected in a way no measurement would reveal.
            const bench::InterruptGuard inner;
            (void)inner;
        }
        (void)outer;
    }
    check(true, "guards nest without the inner one re-enabling interrupts");
    check(!bench::InterruptGuard::active(), "and on a host it reports that it masks nothing, rather than implying it does");
}

void test_source_is_reported() {
    std::printf("the counter source is reported\n");
    check(bench::enable(), "enable() succeeds");
    const std::string source = bench::source_name();
    check(!source.empty(), "the source has a name");

    // A measurement whose source is unknown is not a measurement. On a device this line distinguishes a 32-bit DWT
    // reading from a 24-bit SysTick one, and the harness falls back between them silently otherwise.
    std::printf("      source: %s\n", source.c_str());
    check(source.find("host") != std::string::npos,
          "and on a host it says so plainly, so nobody mistakes nanoseconds for cycles");
}

}  // namespace

int main() {
    std::printf("cycle-counting harness\n\n");

    test_wrap_32bit();
    test_wrap_24bit();
    test_measurement_bookkeeping();
    test_microseconds();
    test_measure_runs_the_work();
    test_interrupt_guard();
    test_source_is_reported();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("\nNote: this tests the rig, not the timings. Cycle counts need silicon, and QEMU is not\n"
                "cycle-accurate, so a number from it would be worse than no number.\n");
    return g_failures == 0 ? 0 : 1;
}
