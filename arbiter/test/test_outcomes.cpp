// Every Verdict outcome, reached deliberately.
//
// Written because of something the palette test taught. That test contaminated the pump casebook with five
// pathologies and reported zero differences, which looked like a clean pass. It was not: the pump reaches
// `Unsatisfied` in 0 of its 16,384 states, so it was structurally incapable of exposing the defect that was actually
// there. The bug only appeared once a casebook was built for the sole purpose of reaching the outcome the real
// casebooks never reach.
//
// The general lesson: testing with realistic data mostly proves that the realistic data does not trip the bug. Both
// casebooks here are overwhelmingly `Decided` -- 99.5% and 100% -- which is exactly what a good casebook should be,
// and exactly why they cannot test the other four outcomes.
//
// So this file abandons realism. Each fixture is the smallest framework that reaches one outcome, and `Faulted` gets
// the most attention because it had never been reached at all. `Faulted` means the reasoner did not complete and the
// device must surrender to its hardwired interlock -- the most safety-critical path in the arbiter, and until now
// the only one with no test.

#include <cstdint>
#include <cstdio>
#include <string>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

using quorum::arbiter::Outcome;
using quorum::arbiter::Policy;
using quorum::arbiter::Verdict;
using quorum::arbiter::decide;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Label;
using quorum::solver::Status;

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

using Fw = Framework<16, 32>;
using Solver = Grounded<16>;
using Pol = Policy<4>;

// ─────────────────────────────────────────────────────────────────────────────
// Decided: exactly one action In.

void test_decided() {
    std::printf("Decided -- exactly one action survives\n");
    Fw fw;
    Solver solver;
    Pol policy;

    // 0 = reason, 1 = act, 2 = idle. `reason` is unattacked, so it fells nothing and `act` stands; `act` displaces
    // `idle`.
    check(fw.declare(3) == Status::Ok, "declare");
    check(fw.add_attack(Arg{1}, Arg{2}) == Status::Ok, "act displaces idle");
    check(fw.finalize() == Status::Ok, "finalize");
    check(policy.declare_action(Arg{1}) == Status::Ok, "declare act");
    check(policy.declare_action(Arg{2}) == Status::Ok, "declare idle");
    check(policy.set_fallback(Arg{2}) == Status::Ok, "fallback");

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Decided, "outcome is Decided");
    check(v.decided(), "decided() agrees");
    check(v.action == Arg{1}, "the surviving action is chosen");
    check(!v.is_fallback, "and it is not reported as a fallback");
    check(v.actions_in == 1, "exactly one action In");
    check(v.actions_undecided == 0, "no action Undecided");
    check(v.status == Status::Ok, "status Ok");
}

// ─────────────────────────────────────────────────────────────────────────────
// Indeterminate: no action In, but an action is Undecided. The plant is ambiguous; escalate to a human.

void test_indeterminate() {
    std::printf("Indeterminate -- the evidence is genuinely ambiguous\n");
    Fw fw;
    Solver solver;
    Pol policy;

    // 0 <-> 1 is an even cycle, so both are Undecided under grounded semantics. Each attacks one action, leaving
    // both actions Undecided rather than defeated.
    check(fw.declare(4) == Status::Ok, "declare");
    check(fw.add_attack(Arg{0}, Arg{1}) == Status::Ok, "");
    check(fw.add_attack(Arg{1}, Arg{0}) == Status::Ok, "");
    check(fw.add_attack(Arg{0}, Arg{2}) == Status::Ok, "");
    check(fw.add_attack(Arg{1}, Arg{3}) == Status::Ok, "");
    check(fw.finalize() == Status::Ok, "finalize");
    check(policy.declare_action(Arg{2}) == Status::Ok, "");
    check(policy.declare_action(Arg{3}) == Status::Ok, "");
    check(policy.set_fallback(Arg{3}) == Status::Ok, "");

    const Verdict v = decide(fw, solver, policy);
    check(solver.label(Arg{0}) == Label::Undecided, "the cycle is Undecided");
    check(v.outcome == Outcome::Indeterminate, "outcome is Indeterminate");
    check(v.actions_in == 0, "no action is In");
    check(v.actions_undecided == 2, "both actions are Undecided");
    check(v.action == Arg{3}, "the fallback is returned");
    check(v.is_fallback, "and it is reported AS a fallback, so the caller knows");
}

// ─────────────────────────────────────────────────────────────────────────────
// Unsatisfied: every action decisively defeated. The casebook is broken, not the plant.

void test_unsatisfied() {
    std::printf("Unsatisfied -- the casebook rejects every action it declared\n");
    Fw fw;
    Solver solver;
    Pol policy;

    // 0 is unattacked and defeats both actions. Nothing is Undecided anywhere.
    check(fw.declare(3) == Status::Ok, "declare");
    check(fw.add_attack(Arg{0}, Arg{1}) == Status::Ok, "");
    check(fw.add_attack(Arg{0}, Arg{2}) == Status::Ok, "");
    check(fw.finalize() == Status::Ok, "finalize");
    check(policy.declare_action(Arg{1}) == Status::Ok, "");
    check(policy.declare_action(Arg{2}) == Status::Ok, "");
    check(policy.set_fallback(Arg{2}) == Status::Ok, "");

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Unsatisfied, "outcome is Unsatisfied");
    check(v.undecided == 0, "nothing in the framework is Undecided");
    check(v.actions_undecided == 0, "and no action is Undecided");
    check(v.action == Arg{2}, "the fallback is still returned -- there is always an executable action");
    check(v.is_fallback, "flagged as a fallback");

    // The distinction from Indeterminate is the whole point, and it is not cosmetic: Indeterminate means ask a
    // human, Unsatisfied means the FMEA is wrong. Confusing them sends an engineer to the wrong problem.
    check(Outcome::Unsatisfied != Outcome::Indeterminate, "the two are distinct outcomes, deliberately");
}

// ─────────────────────────────────────────────────────────────────────────────
// Conflicted: more than one action In. A casebook defect in the dangerous direction.

void test_conflicted() {
    std::printf("Conflicted -- two actions at once\n");
    Fw fw;
    Solver solver;
    Pol policy;

    // Two unattacked actions. Both In.
    check(fw.declare(2) == Status::Ok, "declare");
    check(fw.finalize() == Status::Ok, "finalize");
    check(policy.declare_action(Arg{0}) == Status::Ok, "");
    check(policy.declare_action(Arg{1}) == Status::Ok, "");
    check(policy.set_fallback(Arg{1}) == Status::Ok, "");

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Conflicted, "outcome is Conflicted");
    check(v.actions_in == 2, "both actions are In");

    // The important part. With two actions In there is a temptation to just pick one -- and that is precisely what
    // must not happen, because "open the valve" and "close the valve" can both be In and picking either is a guess
    // dressed as a decision. The fallback is returned instead.
    check(v.action == Arg{1}, "the fallback is returned, NOT an arbitrary winner");
    check(v.is_fallback, "flagged as a fallback");
}

// ─────────────────────────────────────────────────────────────────────────────
// Faulted. The reasoner did not complete. Until this file, never reached by any test.

void test_faulted_unfinalized_framework() {
    std::printf("Faulted -- the framework was never finalized\n");
    Fw fw;
    Solver solver;
    Pol policy;

    check(fw.declare(3) == Status::Ok, "declare");
    check(fw.add_attack(Arg{0}, Arg{1}) == Status::Ok, "");
    // finalize() deliberately omitted: the CSR adjacency was never built.
    check(policy.declare_action(Arg{1}) == Status::Ok, "");
    check(policy.set_fallback(Arg{1}) == Status::Ok, "");

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Faulted, "outcome is Faulted");
    check(v.status == Status::NotFinalized, "and the status says exactly why");
    check(v.steps == 0, "no work was reported");
    check(v.is_fallback, "flagged as a fallback");
}

void test_faulted_policy_with_no_actions() {
    std::printf("Faulted -- a policy declaring no actions\n");
    Fw fw;
    Solver solver;
    Pol policy;  // never given an action or a fallback

    check(fw.declare(2) == Status::Ok, "declare");
    check(fw.finalize() == Status::Ok, "finalize");

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Faulted, "outcome is Faulted");
    check(v.status == Status::BadArgument, "status explains the policy is unusable");
    check(v.is_fallback, "flagged as a fallback");

    // THE POINT OF THIS TEST. There is no fallback to fall back to, so there is no meaningful action to return.
    // A caller that checks `outcome` behaves correctly. A caller that skips the check and executes `v.action`
    // must not be handed something that looks plausible.
    check(!v.action.valid(),
          "action is INVALID, so a caller that ignores the outcome fails loudly instead of "
          "executing argument 0 -- which in a real casebook is a sensor reading, not an action");
}

void test_faulted_policy_with_actions_but_no_fallback() {
    std::printf("Faulted -- actions declared but no fallback chosen\n");
    Fw fw;
    Solver solver;
    Pol policy;

    check(fw.declare(2) == Status::Ok, "declare");
    check(fw.finalize() == Status::Ok, "finalize");
    check(policy.declare_action(Arg{0}) == Status::Ok, "an action is declared");
    // set_fallback deliberately omitted. A casebook that never says what to do when reasoning fails is incomplete,
    // and the arbiter refuses to invent a safe state on its behalf: a pump stops, a tank stops filling, a drone
    // must keep flying, and no default is right for all three.

    const Verdict v = decide(fw, solver, policy);
    check(v.outcome == Outcome::Faulted, "outcome is Faulted");
    check(v.status == Status::BadArgument, "status explains why");
    check(!v.action.valid(), "and no action is offered");
}

void test_faulted_capacity_exceeded() {
    std::printf("Faulted -- more attacks than the framework can hold\n");
    Framework<8, 4> small;
    Grounded<8> solver;
    Pol policy;

    check(small.declare(5) == Status::Ok, "declare");
    check(small.add_attack(Arg{0}, Arg{1}) == Status::Ok, "1");
    check(small.add_attack(Arg{0}, Arg{2}) == Status::Ok, "2");
    check(small.add_attack(Arg{0}, Arg{3}) == Status::Ok, "3");
    check(small.add_attack(Arg{0}, Arg{4}) == Status::Ok, "4");
    check(small.add_attack(Arg{1}, Arg{2}) == Status::CapacityExceeded,
          "the fifth attack is refused rather than silently dropped");

    // The failure this guards against is the worst one available. A caller that ignores the rejection and finalizes
    // anyway would get a framework that solves perfectly cleanly and returns a confident verdict computed from a
    // casebook with one of its rules missing -- internally consistent, and wrong. Nothing downstream can detect it,
    // because the labelling is valid for the framework that was actually built.
    //
    // So the rejection is sticky. `finalize` refuses, `solve` refuses, and the verdict is Faulted.
    check(small.finalize() == Status::CapacityExceeded,
          "finalize REFUSES after a rejected attack, rather than solving an incomplete casebook");
    check(!small.finalized(), "and the framework is not finalized");

    check(policy.declare_action(Arg{4}) == Status::Ok, "");
    check(policy.set_fallback(Arg{4}) == Status::Ok, "");
    const Verdict v = decide(small, solver, policy);
    check(v.outcome == Outcome::Faulted, "the verdict is Faulted, not a confident wrong answer");
    check(v.action.valid() && v.action == Arg{4},
          "and the declared safe state is still named, so the device has somewhere to go");

    // reset() clears it, because reusing the object across control cycles is the intended pattern.
    small.reset();
    check(small.declare(3) == Status::Ok, "after reset, declare works");
    check(small.add_attack(Arg{0}, Arg{1}) == Status::Ok, "");
    check(small.finalize() == Status::Ok, "and finalize succeeds -- the flag is per-construction, not permanent");
}

// ─────────────────────────────────────────────────────────────────────────────

/// Whatever happens, the caller gets something it can act on, or an unmistakable refusal.
void test_always_actionable() {
    std::printf("property: every outcome yields either a valid action or an explicit refusal\n");

    struct Case {
        const char* name;
        Outcome outcome;
        bool expect_valid_action;
    };

    // Faulted with a usable policy still returns the declared fallback; Faulted with a BROKEN policy cannot.
    Fw fw;
    Solver solver;
    check(fw.declare(2) == Status::Ok, "");
    check(fw.finalize() == Status::Ok, "");

    {
        Pol good;
        check(good.declare_action(Arg{0}) == Status::Ok, "");
        check(good.set_fallback(Arg{0}) == Status::Ok, "");
        Fw unfinalized;
        check(unfinalized.declare(2) == Status::Ok, "");
        const Verdict v = decide(unfinalized, solver, good);
        check(v.outcome == Outcome::Faulted, "Faulted with a good policy");
        check(v.action.valid() && v.action == Arg{0},
              "the casebook's declared safe state is returned even when reasoning failed -- this is the "
              "hardwired-interlock path and it must still name a state");
    }
    {
        Pol broken;
        const Verdict v = decide(fw, solver, broken);
        check(v.outcome == Outcome::Faulted, "Faulted with a broken policy");
        check(!v.action.valid(), "no action is invented when the casebook never declared one");
    }
}

void test_outcome_names_are_distinct() {
    std::printf("every outcome has a distinct name, for logs a human will read at 3 a.m.\n");
    const Outcome all[] = {Outcome::Decided, Outcome::Indeterminate, Outcome::Unsatisfied, Outcome::Conflicted,
                           Outcome::Faulted};
    for (std::size_t i = 0; i < 5; ++i) {
        const std::string a = outcome_name(all[i]);
        check(!a.empty() && a != "unknown", "outcome " + std::to_string(i) + " has a name");
        for (std::size_t j = i + 1; j < 5; ++j) {
            check(a != std::string(outcome_name(all[j])), a + " is distinct from " + outcome_name(all[j]));
        }
    }
}

}  // namespace

int main() {
    std::printf("arbiter: every Verdict outcome, reached deliberately\n\n");

    test_decided();
    test_indeterminate();
    test_unsatisfied();
    test_conflicted();
    test_faulted_unfinalized_framework();
    test_faulted_policy_with_no_actions();
    test_faulted_policy_with_actions_but_no_fallback();
    test_faulted_capacity_exceeded();
    test_always_actionable();
    test_outcome_names_are_distinct();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
