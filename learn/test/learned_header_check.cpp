// The device header for a learned casebook, run on the host: does the code a microcontroller would run decide every
// state exactly as the learner did?
//
//   learned_header_check <name>.expected
//
// The header is the one cbc emitted for firmware/pico2/learned.cpp, compiled here for the host CPU instead. Same
// generated tables, same instantiate(), same arbiter::decide(); the expected line was written by
// test_learned_casebook from the learner itself. This is the silicon check minus the silicon, so it runs on every
// build, and the board run (RUNNING_ON_HARDWARE.md) only has to show the same line.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include "quorum/arbiter/verdict.hpp"

#include "monks_learned_generated.hpp"

namespace cb = quorum::casebook::gen::monks_1_knows_2;
using quorum::arbiter::Outcome;
using quorum::solver::Status;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: learned_header_check <expected file>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    std::string expected;
    std::getline(in, expected);

    static cb::CaseFramework fw;
    static cb::CaseSolver solver;
    static cb::CasePolicy policy;
    if (!cb::verify_casebook() || cb::make_policy(policy) != Status::Ok) {
        std::printf("FAIL  fingerprint or policy\n");
        return 1;
    }
    std::uint32_t fnv = 2166136261U, asserts = 0;
    std::int32_t max_steps = 0;
    const std::uint32_t states = 1U << cb::kObservationCount;
    for (std::uint32_t m = 0; m < states; ++m) {
        if (cb::instantiate(fw, m) != Status::Ok) {
            std::printf("FAIL  instantiate at state 0x%x\n", m);
            return 1;
        }
        const auto v = quorum::arbiter::decide(fw, solver, policy);
        const bool asserted = v.outcome == Outcome::Decided && v.action.valid() && v.action.get() == cb::kSafeAction;
        fnv = (fnv ^ (v.outcome != Outcome::Decided ? 2U : (asserted ? 1U : 0U))) * 16777619U;
        asserts += asserted ? 1U : 0U;
        if (v.steps > max_steps) { max_steps = v.steps; }
    }
    char line[160];
    std::snprintf(line, sizeof line, "%s states %u asserts %u fnv1a 0x%08x", cb::kCasebookName, states, asserts, fnv);
    std::printf("header   %s\nlearner  %s\nsteps    max %d, proven bound %d\n", line, expected.c_str(), max_steps,
                cb::kStepBound);
    if (expected != line) {
        std::printf("FAIL  the device header does not decide as the learner did\n");
        return 1;
    }
    if (max_steps > cb::kStepBound) {
        std::printf("FAIL  a solve exceeded the proven bound\n");
        return 1;
    }
    std::printf("learned_header_check: the device header decides every state as the learner did\n");
    return 0;
}
