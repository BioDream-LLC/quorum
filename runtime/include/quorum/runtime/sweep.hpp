#ifndef QUORUM_RUNTIME_SWEEP_HPP
#define QUORUM_RUNTIME_SWEEP_HPP

// The boot-time self-report sweep, shared by the Pico 2 and Uno R4 runtime firmware so both decide, fingerprint and
// time states the same way. Device code: no heap, no exceptions. Needs quorum/bench/cycles.hpp from the firmware.

#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/bench/cycles.hpp"
#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/layout.hpp"

namespace quorum::runtime {

/// What one sweep found. `fnv` is FNV-1a over one byte per state: 1 safe action, 0 another action, 2 not Decided --
/// the same encoding qblob uses, so equal DEVICE lines mean equal decisions.
struct SweepResult {
    std::uint32_t states = 0;
    std::uint32_t fnv = 2166136261U;
    std::uint32_t asserts = 0;
    std::uint32_t not_decided = 0;
    std::uint32_t faults = 0;
    std::int32_t max_steps = 0;
    std::uint32_t worst_cycles = 0;
    std::uint32_t worst_mask = 0;
    bool full = false;  ///< every state was decided; false means the worst-case state alone was timed
};

namespace detail {
/// Written with every result so the compiler cannot drop the solve it is timing.
inline volatile std::int32_t g_sweep_sink = 0;

template <typename Instantiate>
void sweep_states(SweepResult& r, std::int32_t observations, std::uint32_t worst, std::int32_t shift,
                  std::int32_t safe, RtFramework& fw, RtSolver& solver, const RtPolicy& policy, Instantiate inst) {
    r.full = observations <= kSweepLimit;
    r.states = 1UL << static_cast<std::uint32_t>(observations);
    const std::uint32_t first = r.full ? 0UL : worst;
    const std::uint32_t last = r.full ? r.states : worst + 1UL;
    for (std::uint32_t local = first; local < last; ++local) {
        const std::uint32_t m = local << static_cast<std::uint32_t>(shift);
        arbiter::Verdict v{};
        bool built = true;
        // Four runs: the first is cold, which is the one an integrator must budget for.
        const auto probe = bench::measure(4U, [&] {
            built = inst(m) == solver::Status::Ok;
            if (!built) { return; }
            v = arbiter::decide(fw, solver, policy);
            g_sweep_sink = v.steps;
        });
        if (!built) {
            ++r.faults;
            continue;
        }
        const bool decided = v.outcome == arbiter::Outcome::Decided;
        const bool asserted = decided && v.action.valid() && v.action.get() == safe;
        r.fnv = (r.fnv ^ (!decided ? 2UL : (asserted ? 1UL : 0UL))) * 16777619UL;
        r.asserts += asserted ? 1U : 0U;
        r.not_decided += decided ? 0U : 1U;
        if (v.steps > r.max_steps) { r.max_steps = v.steps; }
        if (probe.valid && probe.max_cycles > r.worst_cycles) {
            r.worst_cycles = probe.max_cycles;
            r.worst_mask = m;
        }
    }
}
}  // namespace detail

/// Every state of a single casebook.
inline SweepResult sweep(const Casebook& cb, RtFramework& fw, RtSolver& solver, const RtPolicy& policy) noexcept {
    SweepResult r;
    detail::sweep_states(r, cb.observation_count, cb.worst_case_mask, 0, cb.safe_action, fw, solver, policy,
                         [&](std::uint32_t m) { return instantiate(fw, cb, m); });
    return r;
}

/// Every state of one palette member, the other members quiet. By the palette's non-interference argument the
/// result equals that member's casebook swept alone.
inline SweepResult sweep(const Palette& p, std::int32_t member, RtFramework& fw, RtSolver& solver,
                         const RtPolicy& policy) noexcept {
    SweepResult r;
    const Member& x = p.members[member];
    detail::sweep_states(r, x.observation_count, 0U, x.observation_offset, x.safe_action, fw, solver, policy,
                         [&](std::uint32_t m) { return instantiate(fw, p, m); });
    return r;
}

}  // namespace quorum::runtime

#endif  // QUORUM_RUNTIME_SWEEP_HPP
