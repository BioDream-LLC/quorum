#pragma once

// Cycle counting, for the one claim that cannot be verified without silicon.
//
// Everything else about this product has been measured: 2,640 bytes of flash, 1,780 bytes of RAM, a 332-byte stack
// ceiling, 140 worst-case steps against a proven bound of 248. Timing has not, and it is the claim the certification
// argument actually rests on. A polynomial algorithm with a proven step bound is necessary for a safety function and
// not sufficient: somebody has to say how long a step takes on the part.
//
// QEMU cannot answer that. It is a functional emulator, not a cycle-accurate one, and a number from it would be worse
// than no number because it would look like evidence. So this reads the hardware's own counter.
//
// On Cortex-M33 (RP2350, Raspberry Pi Pico 2) the Data Watchpoint and Trace unit provides CYCCNT, a free-running
// 32-bit count of core clock cycles. It is the same mechanism a certification body would expect to see used, it
// requires no external equipment, and it costs two register writes to enable.
//
// The host fallback exists so the harness itself can be tested. A timing rig nobody has exercised is not a rig.

#include <cstdint>

// Two independent questions, and conflating them was a bug.
//
// A NOTE ON THE SPELLING `Systick`, WHICH LOOKS LIKE A TYPO AND IS NOT
//
// ARM's CMSIS headers contain `#define SysTick ((SysTick_Type *) SysTick_BASE)`. An enumerator spelled `SysTick`
// therefore does not survive contact with any vendor BSP that includes CMSIS -- the preprocessor rewrites it into a
// cast expression and the compiler reports the error against CMSIS's own header, tens of thousands of lines away
// from the cause.
//
// This was not caught by the portability gate, which compiles the staged sketch against a STUB Arduino.h. The stub
// had no CMSIS in it, so it could not collide. It was caught by the first real vendor toolchain the code ever met.
//
//   QUORUM_BENCH_ARM  -- is this a Cortex-M at all? SysTick is part of the core on every one of them, back to
//                        ARMv6-M, so any Cortex-M can be measured in cycles.
//   QUORUM_BENCH_DWT  -- is the DWT worth trying? It appears from ARMv7-M onwards, and even there the Cortex-M33
//                        Technical Reference Manual calls the cycle counter implementation-dependent.
//
// The first draft tested only `__ARM_ARCH >= 7`, which silently routed a Cortex-M0+ (ARMv6-M) to the HOST fallback --
// so a measurement on an RP2040 would have reported host nanoseconds from a device that has a perfectly good cycle
// counter. Found by writing down that an RP2040 was the obvious second target.
#if defined(__ARM_ARCH) && defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
#define QUORUM_BENCH_ARM 1
// Only when the compiler does not say which profile it targets. An A-profile core -- a Raspberry Pi 5's Cortex-A76,
// any 64-bit Arm -- has no SysTick, DWT or PRIMASK, and the Cortex-M instructions do not even assemble there. The
// first native build on a Pi 5 (GCC 14, aarch64) failed on exactly that: `cpsid i`, `mrs x0, primask`.
#elif defined(__ARM_ARCH) && (__ARM_ARCH >= 6) && !defined(__ARM_ARCH_PROFILE) && !defined(__aarch64__)
#define QUORUM_BENCH_ARM 1
#else
#define QUORUM_BENCH_ARM 0
#endif

#if QUORUM_BENCH_ARM && (__ARM_ARCH >= 7)
#define QUORUM_BENCH_DWT 1
#else
#define QUORUM_BENCH_DWT 0
#endif

#if !QUORUM_BENCH_ARM
#include <chrono>
#endif

namespace quorum::bench {

#if QUORUM_BENCH_ARM

// ARMv7-M and ARMv8-M debug and core registers. Addresses are architectural, not vendor-specific.
namespace regs {
constexpr std::uintptr_t kDemcr = 0xE000EDFCU;      ///< Debug Exception and Monitor Control
constexpr std::uintptr_t kDwtCtrl = 0xE0001000U;    ///< DWT Control
constexpr std::uintptr_t kDwtCyccnt = 0xE0001004U;  ///< DWT Cycle Count
constexpr std::uintptr_t kDwtLar = 0xE0001FB0U;     ///< DWT Lock Access (ARMv8-M)

constexpr std::uint32_t kTrcEna = 1U << 24;     ///< DEMCR: enable the trace/debug blocks
constexpr std::uint32_t kCyccntEna = 1U << 0;   ///< DWT_CTRL: enable the cycle counter
constexpr std::uint32_t kNoCyccnt = 1U << 25;   ///< DWT_CTRL: set when CYCCNT is NOT implemented
constexpr std::uint32_t kLarUnlock = 0xC5ACCE55U;

// SysTick. Part of the core and architecturally mandated on Cortex-M33, unlike the DWT cycle counter, which the
// Technical Reference Manual explicitly describes as implementation-dependent. This is the fallback that means a
// board arriving and the DWT being absent does not leave the measurement with nowhere to go.
constexpr std::uintptr_t kSystCsr = 0xE000E010U;
constexpr std::uintptr_t kSystRvr = 0xE000E014U;
constexpr std::uintptr_t kSystCvr = 0xE000E018U;

constexpr std::uint32_t kSystEnable = 1U << 0;
constexpr std::uint32_t kSystClkCore = 1U << 2;  ///< count core clock, not the external reference
constexpr std::uint32_t kSystMax = 0x00FFFFFFU;  ///< 24-bit; 2^24 cycles is ~112 ms at 150 MHz

inline volatile std::uint32_t& at(std::uintptr_t address) {
    return *reinterpret_cast<volatile std::uint32_t*>(address);
}
}  // namespace regs

/// Which counter is in use. Reported, because a measurement whose source is unknown is not a measurement.
enum class Source : std::uint8_t {
    None = 0,
    Dwt,      ///< DWT CYCCNT: 32-bit, counts up, cycle resolution
    Systick,  ///< SysTick CVR: 24-bit, counts DOWN, cycle resolution. NOT `SysTick`: CMSIS #defines that.
};

inline Source g_source = Source::None;

[[nodiscard]] inline const char* source_name() noexcept {
    switch (g_source) {
        case Source::Dwt: return "DWT CYCCNT (32-bit)";
        case Source::Systick: return "SysTick (24-bit, core clock)";
        case Source::None: return "none";
    }
    return "?";
}

/// Enable a cycle counter, preferring the DWT and falling back to SysTick.
///
/// Returns false only if neither starts. That distinction matters: a silently-zero counter would be read as an
/// impossibly fast result rather than as a broken rig, which is the worst failure available to a measurement tool.
[[nodiscard]] inline bool enable() noexcept {
#if QUORUM_BENCH_DWT
    // ── first choice: DWT CYCCNT ────────────────────────────────────────────
    // Compiled out entirely on ARMv6-M, where these addresses are not merely unimplemented but would fault.
    //
    // ARMv8-M gates the DWT behind a lock register; the write is harmless on ARMv7-M where the address is reserved.
    regs::at(regs::kDwtLar) = regs::kLarUnlock;
    regs::at(regs::kDemcr) |= regs::kTrcEna;

    // The architecture advertises absence. Ask before assuming.
    if ((regs::at(regs::kDwtCtrl) & regs::kNoCyccnt) == 0U) {
        regs::at(regs::kDwtCyccnt) = 0U;
        regs::at(regs::kDwtCtrl) |= regs::kCyccntEna;

        const std::uint32_t first = regs::at(regs::kDwtCyccnt);
        for (volatile int i = 0; i < 16; ++i) {}
        if (regs::at(regs::kDwtCyccnt) != first) {
            g_source = Source::Dwt;
            return true;
        }
    }
#endif

    // ── fallback: SysTick as a free-running down-counter ────────────────────
    // No interrupt, maximum reload, core clock. Note the Pico SDK does not use SysTick for timekeeping -- it uses the
    // timer peripheral -- so this does not fight the SDK. An RTOS would, and an integrator adding one must know.
    regs::at(regs::kSystCsr) = 0U;
    regs::at(regs::kSystRvr) = regs::kSystMax;
    regs::at(regs::kSystCvr) = 0U;
    regs::at(regs::kSystCsr) = regs::kSystEnable | regs::kSystClkCore;

    const std::uint32_t first = regs::at(regs::kSystCvr);
    for (volatile int i = 0; i < 16; ++i) {}
    if (regs::at(regs::kSystCvr) != first) {
        g_source = Source::Systick;
        return true;
    }

    g_source = Source::None;
    return false;
}

/// The current counter reading. Normalised so that elapsed() is always a forward difference, which means negating
/// SysTick -- it counts DOWN, and getting that backwards yields enormous nonsense values that look like real results.
[[nodiscard]] inline std::uint32_t now() noexcept {
    if (g_source == Source::Systick) {
        return (regs::kSystMax - (regs::at(regs::kSystCvr) & regs::kSystMax)) & regs::kSystMax;
    }
#if QUORUM_BENCH_DWT
    return regs::at(regs::kDwtCyccnt);
#else
    return 0U;
#endif
}

/// The counter's modulus, for wrap-correct subtraction.
[[nodiscard]] inline std::uint32_t modulus() noexcept {
    return g_source == Source::Systick ? (regs::kSystMax + 1U) : 0U;  // 0 means the full 2^32
}

#else

enum class Source : std::uint8_t { None = 0, Dwt, Systick, Host };
inline Source g_source = Source::Host;

[[nodiscard]] inline const char* source_name() noexcept { return "host steady_clock (nanoseconds, NOT cycles)"; }

/// Host fallback. Nanoseconds, not cycles, so the numbers are not comparable to a device run -- but the harness is
/// exercisable, and a rig nobody has run is not a rig.
[[nodiscard]] inline bool enable() noexcept { return true; }

[[nodiscard]] inline std::uint32_t now() noexcept {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

[[nodiscard]] inline std::uint32_t modulus() noexcept { return 0U; }

#endif

/// Disables interrupts for its lifetime, restoring the previous state on exit.
///
/// A WCET figure is a maximum over many runs, so it is exactly the statistic an interrupt corrupts: one timer tick or
/// one USB transfer landing inside the timed window becomes "the worst case" and is reported as a property of the
/// software. The first version of this harness dealt with that by forbidding USB serial and requiring a UART adapter,
/// which is the wrong fix -- it adds a hardware requirement, and it only removes the interrupts you thought of.
///
/// Masking instead measures the code rather than the platform. Nested use is safe: PRIMASK is saved and restored, not
/// unconditionally cleared, so an inner guard cannot re-enable interrupts an outer one disabled.
///
/// This deliberately does NOT model interrupt latency. A real integration must add its own worst-case interrupt load
/// to these numbers; what is measured here is quorum's contribution, which is the only part quorum can be responsible
/// for.
class InterruptGuard {
public:
    InterruptGuard() noexcept {
#if QUORUM_BENCH_ARM
        __asm volatile("mrs %0, primask" : "=r"(saved_)::"memory");
        __asm volatile("cpsid i" ::: "memory");
#endif
    }

    ~InterruptGuard() noexcept {
#if QUORUM_BENCH_ARM
        // Restore, never blanket-enable: this runs inside whatever critical section the caller already had.
        if ((saved_ & 1U) == 0U) { __asm volatile("cpsie i" ::: "memory"); }
#endif
    }

    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
    InterruptGuard(InterruptGuard&&) = delete;
    InterruptGuard& operator=(InterruptGuard&&) = delete;

    /// True when this guard actually masks anything, so a report can say whether the numbers are interrupt-free.
    [[nodiscard]] static constexpr bool active() noexcept { return QUORUM_BENCH_ARM != 0; }

private:
#if QUORUM_BENCH_ARM
    std::uint32_t saved_ = 0U;
#endif
};


///
/// Split out from `elapsed()` so it can be tested on a host, where neither counter exists. The SysTick path is the
/// one that needs testing: it is 24-bit and counts DOWN, and getting either of those wrong produces enormous values
/// that look like real slow paths rather than like arithmetic errors.
///
/// \param modulus the counter's period, or 0 for a full 32-bit counter.
[[nodiscard]] inline constexpr std::uint32_t elapsed_within(std::uint32_t from,
                                                            std::uint32_t to,
                                                            std::uint32_t modulus) noexcept {
    if (modulus == 0U) { return static_cast<std::uint32_t>(to - from); }
    return static_cast<std::uint32_t>(to - from) & (modulus - 1U);
}

/// Unsigned difference, correct across the counter's wrap and for either counter width.
[[nodiscard]] inline std::uint32_t elapsed(std::uint32_t from, std::uint32_t to) noexcept {
    return elapsed_within(from, to, modulus());
}

/// The measured cost of a repeated operation.
///
/// Reports the MINIMUM as well as the maximum, deliberately. On a part with flash caching and no operating system the
/// minimum is the clean-path cost and the maximum includes first-run cache misses; reporting only a mean would hide
/// both. For a WCET argument the maximum is the number that counts, and it must be taken over many runs rather than
/// one, because a single run can miss the slow path entirely.
struct Measurement {
    std::uint32_t min_cycles = 0xFFFFFFFFU;
    std::uint32_t max_cycles = 0;
    std::uint64_t total_cycles = 0;
    std::uint32_t runs = 0;
    bool valid = false;  ///< false when the counter never advanced; see enable()

    void add(std::uint32_t cycles) noexcept {
        if (cycles < min_cycles) { min_cycles = cycles; }
        if (cycles > max_cycles) { max_cycles = cycles; }
        total_cycles += cycles;
        ++runs;
        valid = true;
    }

    [[nodiscard]] std::uint32_t mean_cycles() const noexcept {
        if (runs == 0) { return 0; }
        return static_cast<std::uint32_t>(total_cycles / runs);
    }

    /// Microseconds at a given core clock, rounded up. Integer arithmetic only, so this is usable on device.
    [[nodiscard]] std::uint32_t max_microseconds(std::uint32_t clock_hz) const noexcept {
        if (clock_hz == 0U) { return 0U; }
        const std::uint64_t per_mhz = clock_hz / 1000000U;
        if (per_mhz == 0U) { return 0U; }
        return static_cast<std::uint32_t>((max_cycles + per_mhz - 1U) / per_mhz);
    }
};

/// Time `work` `runs` times and report the distribution.
///
/// `work` must return something, and the caller must consume it, or the optimiser is entitled to delete the entire
/// measurement. Every call site here writes the result through a volatile.
///
/// Interrupts are masked around each individual run, not around the whole loop: masking the loop would hold them off
/// for thousands of runs, which on a real board means dropped USB, a stalled scheduler, and possibly a watchdog reset.
/// Per-run masking keeps every timed window interrupt-free while leaving gaps for the system to breathe.
template <typename Work>
[[nodiscard]] Measurement measure(std::uint32_t runs, Work&& work) noexcept {
    Measurement m;
    for (std::uint32_t i = 0; i < runs; ++i) {
        std::uint32_t delta = 0U;
        {
            const InterruptGuard no_irq;
            const std::uint32_t start = now();
            work();
            const std::uint32_t stop = now();
            delta = elapsed(start, stop);
        }
        m.add(delta);
    }
    return m;
}

}  // namespace quorum::bench
