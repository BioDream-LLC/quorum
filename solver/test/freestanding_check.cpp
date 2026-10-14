// Freestanding compile gate.
//
// Built with -ffreestanding -fno-exceptions -fno-rtti. Nothing here is executed; the point is that it must
// COMPILE, which it only does if every header under include/ stays clean of exceptions, RTTI, iostreams, the
// allocator, and anything else the hosted standard library drags in.
//
// A test that cannot fail for the reason it names is not a test. This one fails the build the moment somebody
// adds `#include <vector>` to a device header -- which is the failure it exists to catch, and it catches it on a
// workstation rather than on a Cortex-M.

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"

namespace {

using quorum::solver::Arg;
using quorum::solver::FileArg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Status;

// Statically allocated, exactly as it would be on the device: in .bss, not on the heap and not on the stack.
constexpr std::int32_t kMaxArgs = 64;
constexpr std::int32_t kMaxAttacks = 512;

Framework<kMaxArgs, kMaxAttacks> g_framework;
Grounded<kMaxArgs> g_solver;

}  // namespace

/// Install a casebook. Every status is returned, never thrown.
extern "C" Status quorum_install(const std::int32_t* pairs, std::int32_t pair_count, std::int32_t arg_count) {
    Status s = g_framework.declare(arg_count);
    if (s != Status::Ok) { return s; }

    for (std::int32_t i = 0; i < pair_count; ++i) {
        s = g_framework.add_attack(Arg{pairs[2 * i]}, Arg{pairs[2 * i + 1]});
        if (s != Status::Ok) { return s; }
    }
    return g_framework.finalize();
}

/// One control cycle: re-derive the grounded extension and report whether a given claim survives.
extern "C" Status quorum_tick(std::int32_t query_one_based, bool* accepted_out, std::int32_t* steps_out) {
    Arg query{0};
    Status s = quorum::solver::to_internal_checked(FileArg{query_one_based},
                                                    g_framework.argument_count(),
                                                    query);
    if (s != Status::Ok) { return s; }

    s = g_solver.solve(g_framework);
    if (s != Status::Ok) { return s; }

    *accepted_out = g_solver.accepted(query);
    *steps_out = g_solver.steps();
    return Status::Ok;
}

/// The worst case this build can take, for the timing budget. Constant-folded at compile time.
extern "C" std::int32_t quorum_step_bound(void) {
    return Grounded<kMaxArgs>::step_bound(kMaxArgs, kMaxAttacks);
}
