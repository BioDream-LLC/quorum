// The AI guard: each reason on a hand-built ranking, then the property over every state of a real image.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/guard.hpp"
#include "quorum/runtime/layout.hpp"

namespace rt = quorum::runtime;
using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Arg;

namespace {
int g_checks = 0;
int g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

Verdict decided(std::int32_t a) {
    Verdict v;
    v.outcome = Outcome::Decided;
    v.action = Arg{a};
    v.is_fallback = false;
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    std::printf("each guard reason, on a hand-built ranking\n");
    // Actions 10 (trip) > 11 (inspect) > 12 (carry on, idle); 13 is an action nothing ranks against 11.
    const rt::Edge edges[] = {{10, 11}, {10, 12}, {11, 12}, {13, 12}, {10, 13}, {0, 10}};
    const std::int32_t actions[] = {10, 11, 12, 13};
    const auto g = [&](const Verdict& v, std::int32_t p) { return rt::guard(edges, 6, actions, 4, v, p); };

    rt::GuardResult r = g(decided(11), 11);
    check(r.accepted && r.output == 11 && r.reason == rt::GuardReason::Agrees, "agreeing proposal is accepted");
    r = g(decided(11), 10);
    check(r.accepted && r.output == 10 && r.reason == rt::GuardReason::MoreCautious, "a more cautious one is accepted");
    r = g(decided(11), 12);
    check(!r.accepted && r.output == 11 && r.reason == rt::GuardReason::LessCautious, "a less cautious one is replaced");
    r = g(decided(11), 13);
    check(!r.accepted && r.output == 11 && r.reason == rt::GuardReason::Incomparable,
          "an unranked one is replaced: nothing shows it is safe");
    r = g(decided(11), 0);
    check(!r.accepted && r.output == 11 && r.reason == rt::GuardReason::NotAnAction,
          "a non-action is replaced, even though it 'attacks' the trip");
    r = g(decided(11), -7);
    check(!r.accepted && r.output == 11 && r.reason == rt::GuardReason::NotAnAction, "garbage is replaced");

    Verdict undecided;
    undecided.outcome = Outcome::Indeterminate;
    undecided.action = Arg{11};  // pretend the fallback is inspect
    r = g(undecided, 12);
    check(!r.accepted && r.output == 11 && r.reason == rt::GuardReason::NoDecision, "undecided: fallback stands");
    r = g(undecided, 10);
    check(r.accepted && r.output == 10 && r.reason == rt::GuardReason::MoreCautious,
          "undecided: something above the fallback is still accepted");
    r = g(undecided, 11);
    check(r.accepted && r.output == 11 && r.reason == rt::GuardReason::NoDecision, "undecided: the fallback itself");

    if (argc > 1) {
        std::printf("the guard over every state and every proposal of %s\n", argv[1]);
        std::ifstream in(argv[1], std::ios::binary);
        const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static rt::Casebook cb;
        check(rt::load(blob.data(), blob.size(), cb) == rt::BlobStatus::Ok, "the image loads");
        static rt::RtFramework fw;
        static rt::RtSolver solver;
        static rt::RtPolicy policy;
        check(rt::make_policy(policy, cb) == quorum::solver::Status::Ok, "the policy builds");
        const rt::GuardSweep s = rt::guard_sweep(cb, fw, solver, policy);
        check(s.faults == 0U, "every state instantiates");
        check(s.weaker == 0U, "the output is never outranked by the rulebook's decision");
        check(s.pairs == (1U << static_cast<std::uint32_t>(cb.observation_count)) *
                             static_cast<std::uint32_t>(cb.action_count),
              "every state x every proposal");
        // Whether proposing the safe action is ALWAYS accepted depends on the rulebook, and is reported, not assumed.
        // It is in the pump. It is not in the tank: `prefer stop_and_evacuate over stop_transfer`, so a proposal to
        // merely stop the transfer while the rulebook says stop and evacuate is replaced. The action above the safe one
        // includes stopping the transfer, so what the guard accepts there is never less than the safe action.
        bool safe_on_top = true;
        for (std::int32_t k = 0; k < cb.action_count; ++k) {
            if (cb.actions[k] != cb.safe_action && !rt::outranks(cb.edges, cb.edge_count, cb.safe_action, cb.actions[k])) {
                safe_on_top = false;
            }
        }
        std::printf("  the safe action %s every other action\n", safe_on_top ? "outranks" : "does NOT outrank");
        std::printf("  %u pairs: %u accepted, %u replaced, %u weaker\n", s.pairs, s.accepted, s.replaced, s.weaker);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
