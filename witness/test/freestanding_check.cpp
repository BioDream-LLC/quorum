// Freestanding compile gate for the witness.
//
// Built with -ffreestanding -fno-exceptions -fno-rtti. `derivation.hpp` must survive this; `render.hpp` is
// deliberately NOT included, because it uses <cstdio> by design and the whole point of the split is that prose
// happens off-device.
//
// The exported entry point is what a device would expose over a diagnostic link: a controller asks "why did you
// say that?" and gets structure back, without the tick loop ever having paid for it.

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/witness/derivation.hpp"

namespace {

using quorum::solver::Arg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Status;
using quorum::witness::Derivation;
using quorum::witness::Step;

constexpr std::int32_t kMaxArgs = 64;
constexpr std::int32_t kMaxAttacks = 512;

// Statically allocated, as on the device.
Framework<kMaxArgs, kMaxAttacks> g_framework;
Grounded<kMaxArgs> g_solver;
Derivation<128, kMaxArgs> g_derivation;

}  // namespace

/// Reconstruct the derivation of one argument. Called on demand, never in the control loop.
extern "C" Status quorum_explain(std::int32_t query_one_based,
                                 std::int32_t closed_world_internal,
                                 std::int32_t* step_count_out,
                                 std::int32_t* depth_out,
                                 std::int32_t* assumed_steps_out) {
    Arg query{0};
    const Status s = quorum::solver::to_internal_checked(
        quorum::solver::FileArg{query_one_based}, g_framework.argument_count(), query);
    if (s != Status::Ok) { return s; }

    const Status built = g_derivation.build(g_framework, g_solver, query, Arg{closed_world_internal});
    if (built != Status::Ok) { return built; }

    *step_count_out = g_derivation.step_count();
    *depth_out = g_derivation.depth();
    *assumed_steps_out = g_derivation.assumed_steps();
    return Status::Ok;
}

/// Read one step out, for a caller that wants the whole chain over a link.
extern "C" Status quorum_explain_step(std::int32_t index,
                                     std::int32_t* subject_one_based_out,
                                     std::uint8_t* label_out,
                                     std::uint8_t* ground_out,
                                     std::int32_t* because_one_based_out,
                                     std::uint8_t* assumed_out,
                                     std::int32_t* depth_out) {
    if (index < 0 || index >= g_derivation.step_count()) { return Status::BadArgument; }
    const Step& st = g_derivation.step(index);
    *subject_one_based_out = quorum::solver::to_file(st.subject).get();
    *label_out = static_cast<std::uint8_t>(st.label);
    *ground_out = static_cast<std::uint8_t>(st.ground);
    *because_one_based_out = quorum::solver::to_file(st.because).get();
    *assumed_out = st.assumed ? 1U : 0U;
    *depth_out = st.depth;
    return Status::Ok;
}
