// Freestanding compile gate for the arbiter.
//
// Built with -ffreestanding -fno-exceptions -fno-rtti. Nothing here runs; the point is that it must COMPILE,
// which it only does if `verdict.hpp` stays clean of exceptions, RTTI, iostreams and the allocator.
//
// It also sketches the device-side C ABI, which is the shape the real arbiter runtime will take: install a
// casebook once, then tick, and every tick returns an executable action.

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

namespace {

using quorum::arbiter::Outcome;
using quorum::arbiter::Policy;
using quorum::arbiter::Verdict;
using quorum::solver::Arg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Status;

constexpr std::int32_t kMaxArgs = 64;
constexpr std::int32_t kMaxAttacks = 512;
constexpr std::int32_t kMaxActions = 8;

// Statically allocated, exactly as on the device: .bss, not heap and not stack.
Framework<kMaxArgs, kMaxAttacks> g_framework;
Grounded<kMaxArgs> g_solver;
Policy<kMaxActions> g_policy;

}  // namespace

/// Install a casebook: the attack list, plus the action set and the asset's safe state.
extern "C" Status quorum_install(const std::int32_t* pairs,
                                 std::int32_t pair_count,
                                 std::int32_t arg_count,
                                 const std::int32_t* actions,
                                 std::int32_t action_count,
                                 std::int32_t fallback) {
    Status s = g_framework.declare(arg_count);
    if (s != Status::Ok) { return s; }

    for (std::int32_t i = 0; i < pair_count; ++i) {
        s = g_framework.add_attack(Arg{pairs[2 * i]}, Arg{pairs[2 * i + 1]});
        if (s != Status::Ok) { return s; }
    }

    for (std::int32_t i = 0; i < action_count; ++i) {
        s = g_policy.declare_action(Arg{actions[i]});
        if (s != Status::Ok) { return s; }
    }

    s = g_policy.set_fallback(Arg{fallback});
    if (s != Status::Ok) { return s; }

    return g_framework.finalize();
}

/// One control cycle. Always yields an action; `outcome` says how it was reached.
extern "C" Status quorum_tick(std::int32_t* action_out,
                              std::uint8_t* outcome_out,
                              std::int32_t* steps_out) {
    const Verdict v = quorum::arbiter::decide(g_framework, g_solver, g_policy);
    *action_out = v.action.get();
    *outcome_out = static_cast<std::uint8_t>(v.outcome);
    *steps_out = v.steps;
    return v.status;
}

/// The worst case this build can take, for the timing budget. Constant-folded.
extern "C" std::int32_t quorum_step_bound(void) {
    return Grounded<kMaxArgs>::step_bound(kMaxArgs, kMaxAttacks);
}
