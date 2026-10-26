// cbc -- the casebook compiler.
//
// Six passes: parse, resolve, lower, check, validate, emit.
//
// Pass 5 (validate) runs HERE rather than in a separate step, because it can: the lowered form is enough to build
// a framework and solve it, so the entire input space is checked before a single line of C++ is written. This is
// the first place the competition solver's speed actually pays for itself -- exhaustive validation of KNOWLEDGE is
// only affordable because solving is nearly free.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/cbc/cbc.hpp"

namespace {

// Host-side capacity. Generous, because a workstation has memory to spare; the EMITTED header is sized exactly.
constexpr std::int32_t kHostMaxArgs = 512;
constexpr std::int32_t kHostMaxAttacks = 8192;

using HostFramework = quorum::solver::Framework<kHostMaxArgs, kHostMaxAttacks>;
using HostSolver = quorum::solver::Grounded<kHostMaxArgs>;
using HostPolicy = quorum::arbiter::Policy<32>;

using quorum::arbiter::Outcome;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

struct Options {
    std::vector<std::string> inputs;
    bool palette = false;
    std::string header_out;
    std::string af_out;
    std::string apx_out;
    std::string blob_out;
    bool validate = true;
    bool quiet = false;
    bool symbolic = false;
    std::string certificate_out;
    std::string commissioning_out;
};

[[nodiscard]] bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in) { return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

[[nodiscard]] bool write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out) { return false; }
    out << text;
    return out.good();
}

/// Binary, so a Windows build does not turn every 0x0A byte of a device image into 0x0D 0x0A.
[[nodiscard]] bool write_binary(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) { return false; }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

[[nodiscard]] Status build(HostFramework& fw, const quorum::cbc::Lowered& lo, std::uint32_t mask) {
    Status s = fw.declare(lo.count());
    if (s != Status::Ok) { return s; }
    for (const quorum::cbc::LoweredEdge& e : lo.edges) {
        s = fw.add_attack(Arg{e.from}, Arg{e.to});
        if (s != Status::Ok) { return s; }
    }
    for (std::int32_t a = 0; a < lo.observation_count; ++a) {
        const bool asserted = (mask & (1U << static_cast<std::uint32_t>(a))) != 0U;
        if (!asserted) {
            s = fw.add_attack(Arg{lo.closed_world}, Arg{a});
            if (s != Status::Ok) { return s; }
        }
    }
    return fw.finalize();
}

[[nodiscard]] Status make_policy(HostPolicy& p, const quorum::cbc::Lowered& lo) {
    for (const std::int32_t a : lo.actions) {
        const Status s = p.declare_action(Arg{a});
        if (s != Status::Ok) { return s; }
    }
    if (lo.safe_action < 0) { return Status::BadArgument; }
    return p.set_fallback(Arg{lo.safe_action});
}

/// Can this state occur, given the declared binding?
///
/// The binding layer guarantees two things, and they exclude combinations the framework otherwise has to consider:
/// an unbelievable channel asserts `unavailable` and NONE of its thresholds, and a flatline is only ever reported
/// for a channel that is otherwise good. So `xmtr_unavailable` together with `xmtr_high` cannot happen.
///
/// These states are REPORTED, never skipped. Two reasons. The guarantee holds only if the integrator actually uses
/// the binding layer, and an integrator supplying the mask by hand gets no such promise. And the same mistake was
/// already made once here, with `exclusive`: skipping states meant never testing the mechanism built to handle
/// them. So the conservative number stays the headline and reachability refines it.
[[nodiscard]] bool reachable_given_binding(const quorum::cbc::Lowered& lo, std::uint32_t mask) {
    auto asserted = [mask](std::int32_t obs) {
        return obs >= 0 && (mask & (1U << static_cast<std::uint32_t>(obs))) != 0U;
    };
    for (std::size_t c = 0; c < lo.channels.size(); ++c) {
        const quorum::cbc::LoweredChannel& ch = lo.channels[c];
        if (!asserted(ch.unavailable_obs)) { continue; }
        if (asserted(ch.flatline_obs)) { return false; }
        for (const quorum::cbc::LoweredThreshold& t : lo.thresholds) {
            if (t.channel != static_cast<std::int32_t>(c)) { continue; }
            if (asserted(t.observation)) { return false; }
        }
    }
    return true;
}

/// Pass 5. Returns false if any invariant is violated.
[[nodiscard]] bool validate(quorum::cbc::Lowered& lo, bool quiet) {
    if (lo.max_args > kHostMaxArgs) {
        std::cerr << "error: " << lo.max_args << " arguments exceeds the compiler's host capacity of "
                  << kHostMaxArgs << "\n";
        return false;
    }
    if (lo.max_attacks > kHostMaxAttacks) {
        std::cerr << "error: " << lo.max_attacks << " attacks exceeds the compiler's host capacity of "
                  << kHostMaxAttacks << "\n";
        return false;
    }

    HostPolicy policy;
    if (make_policy(policy, lo) != Status::Ok) {
        std::cerr << "error: could not build an action policy\n";
        return false;
    }

    const std::uint32_t total = 1U << static_cast<std::uint32_t>(lo.observation_count);

    std::int32_t counts[5] = {0, 0, 0, 0, 0};
    std::int32_t max_steps = 0;
    // WHICH state is the worst case, not just how bad it is. A timing measurement taken on an average state tells
    // you nothing about worst-case execution time, and WCET is the entire certification argument. The mask is
    // emitted into the generated header so firmware can measure the state that actually matters.
    std::uint32_t max_steps_mask = 0;
    std::int32_t max_steps_edges = 0;
    std::int32_t conflicted = 0;
    std::int32_t unsatisfied = 0;
    std::int32_t faulted = 0;
    std::int32_t wcet_breaches = 0;
    std::int32_t undecided_doubts = 0;
    std::string undecided_doubt_example;

    // Ambiguity, partitioned by whether an exclusive group is fully present. Those states are instrument faults,
    // not genuine diagnostic ambiguity, and conflating the two would overstate how ambiguous the casebook is.
    std::int32_t indet_instrument = 0;
    std::int32_t indet_genuine = 0;
    std::vector<std::uint32_t> genuine_examples;

    std::int32_t unreachable = 0;
    std::int32_t indet_unreachable = 0;

    // States in which an `implies` consistency doubt fires, per doubt. A doubt that fires in no state cannot be
    // reached and is reported; one that fires is the casebook noticing an instrument that disagrees with physics.
    std::vector<std::int32_t> consistency_fired(lo.consistency_doubts.size(), 0);

    // How often each action is selected across the whole input space. An action selected in ZERO states is dead
    // code in a safety casebook -- usually a precedence chain that can never let it through, or conditions that
    // cannot co-occur. Caught here because pass 5 already visits every state, so the count is free.
    std::vector<std::int32_t> action_selected(lo.actions.size(), 0);
    std::vector<std::int32_t> action_selected_reachable(lo.actions.size(), 0);

    // ── pass 6: does every claim the author wrote actually do anything? ──────
    //
    // Added because a genuinely dangerous bug passed all of pass 5's checks. `vote_collapsed needs leg_frozen leg_blind`
    // over two existential claims reads as "two legs compromised" and means "at least one frozen and at least one
    // blind" -- it under-fired when two legs failed the same way, so a voted function could silently become 1oo1. The
    // casebook validated 100% Decided with every OK green.
    //
    // No mechanical check recovers intent. But two weaker checks catch whole families of the same mistake, and neither
    // existed:
    //
    //   * a claim never In in any REACHABLE state is dead logic -- a hazard modelled and then made unreachable
    //   * a claim In only in unreachable states is worse: it looks live and fires only on impossible plants
    //
    // pass 5 already visits every state, so the counts are free.
    std::vector<std::int32_t> claim_in_reachable(static_cast<std::size_t>(lo.count()), 0);
    std::vector<std::int32_t> claim_in_unreachable(static_cast<std::size_t>(lo.count()), 0);

    // Failures are only useful if they can be reproduced. "1 states" tells an author that something is wrong and
    // nothing about where to look, so the first example of each failure mode is kept along with the actions involved.
    struct Example {
        bool have = false;
        std::uint32_t mask = 0;
        std::vector<std::int32_t> actions;
    };
    Example conflict_example;
    Example unsatisfied_example;

    // Every state is independent of every other, so the states are split into contiguous ranges, one per hardware
    // thread, and the per-range results are merged IN RANGE ORDER. That order makes the result identical to a single
    // thread's: sums are sums, the worst case keeps the lowest state that reaches it (strictly-greater updates within
    // a range, earlier range wins a tie), and "first example" means the first in state order. The emitted header,
    // and so every fingerprint, does not depend on the number of threads. Measured 2026-10-14 on synthetic
    // rulebooks: one thread first checked about 370,000 states a second (24 readings: 46 s); reusing the framework
    // per thread and 16 threads brought it to about 1.2 million a
    // second per thread, and 30 readings (a billion states) to 2 minutes on a 16-thread PC. QUORUM_THREADS=<n> overrides the count (1 gives the old single-threaded walk).
    struct Acc {
        std::int32_t counts[5] = {0, 0, 0, 0, 0};
        std::int32_t max_steps = 0;
        std::uint32_t max_steps_mask = 0;
        std::int32_t max_steps_edges = 0;
        std::int32_t conflicted = 0, unsatisfied = 0, faulted = 0, wcet_breaches = 0, undecided_doubts = 0;
        std::string undecided_doubt_example;
        std::int32_t indet_instrument = 0, indet_genuine = 0;
        std::vector<std::uint32_t> genuine_examples;
        std::int32_t unreachable = 0, indet_unreachable = 0;
        std::vector<std::int32_t> consistency_fired, action_selected, action_selected_reachable;
        std::vector<std::int32_t> claim_in_reachable, claim_in_unreachable;
        Example conflict_example, unsatisfied_example;
        bool build_failed = false;
        std::uint32_t build_failed_mask = 0;
    };
    auto scan = [&](Acc& A, std::uint32_t first, std::uint32_t last) {
        A.consistency_fired.assign(lo.consistency_doubts.size(), 0);
        A.action_selected.assign(lo.actions.size(), 0);
        A.action_selected_reachable.assign(lo.actions.size(), 0);
        A.claim_in_reachable.assign(static_cast<std::size_t>(lo.count()), 0);
        A.claim_in_unreachable.assign(static_cast<std::size_t>(lo.count()), 0);
        // One framework and solver per thread, reset for each state, the way the firmware reuses its own every
        // cycle. Constructing them fresh zero-filled 100 KB per state, which was most of the time spent.
        const auto fw_box = std::make_unique<HostFramework>();
        const auto solver_box = std::make_unique<HostSolver>();
        HostFramework& fw = *fw_box;
        HostSolver& solver = *solver_box;
        for (std::uint32_t mask = first; mask < last; ++mask) {
            fw.reset();
            if (build(fw, lo, mask) != Status::Ok) {
                A.build_failed = true;
                A.build_failed_mask = mask;
                return;
            }

            const quorum::arbiter::Verdict v = quorum::arbiter::decide(fw, solver, policy);
            ++A.counts[static_cast<std::size_t>(v.outcome)];
            if (v.steps > A.max_steps) {
                A.max_steps = v.steps;
                A.max_steps_mask = mask;
                A.max_steps_edges = fw.attack_count();
            }

            const bool reachable = reachable_given_binding(lo, mask);
            if (!reachable) { ++A.unreachable; }

            for (std::int32_t a = 0; a < lo.count(); ++a) {
                if (solver.label(Arg{a}) != quorum::solver::Label::In) { continue; }
                if (reachable) {
                    ++A.claim_in_reachable[static_cast<std::size_t>(a)];
                } else {
                    ++A.claim_in_unreachable[static_cast<std::size_t>(a)];
                }
            }

            if (v.outcome == Outcome::Decided) {
                for (std::size_t k = 0; k < lo.actions.size(); ++k) {
                    if (v.action == Arg{lo.actions[k]}) {
                        ++A.action_selected[k];
                        // Separately over the states the declared binding can actually produce. Reporting only the whole
                        // input space is misleading, and badly so: the voting casebook showed `trip` at 88.3% of all
                        // states, which reads as a wildly trip-happy casebook, because most of those states require a
                        // transmitter to be simultaneously unavailable and reporting a value. An author would either panic
                        // or start weakening real safety logic to chase a number that describes impossible plants.
                        if (reachable) { ++A.action_selected_reachable[k]; }
                    }
                }
            }

            if (v.outcome == Outcome::Conflicted) {
                ++A.conflicted;
                if (!A.conflict_example.have) {
                    A.conflict_example.have = true;
                    A.conflict_example.mask = mask;
                    for (const std::int32_t a : lo.actions) {
                        if (solver.label(Arg{a}) == Label::In) { A.conflict_example.actions.push_back(a); }
                    }
                }
            }
            if (v.outcome == Outcome::Unsatisfied) {
                ++A.unsatisfied;
                if (!A.unsatisfied_example.have) {
                    A.unsatisfied_example.have = true;
                    A.unsatisfied_example.mask = mask;
                }
            }
            if (v.outcome == Outcome::Faulted) { ++A.faulted; }
            if (v.steps > HostSolver::step_bound(fw.argument_count(), fw.attack_count())) { ++A.wcet_breaches; }

            for (std::size_t k = 0; k < lo.consistency_doubts.size(); ++k) {
                if (solver.label(Arg{lo.consistency_doubts[k]}) == Label::In) { ++A.consistency_fired[k]; }
            }

            // A doubt that is Undecided means an odd cycle survived lowering -- the silent failure the raw/claim split
            // exists to prevent. Cheap to detect, and catastrophic to miss.
            for (std::int32_t i = 0; i < lo.count(); ++i) {
                if (lo.arguments[static_cast<std::size_t>(i)].role != quorum::cbc::Role::Doubt) { continue; }
                if (solver.label(Arg{i}) == Label::Undecided) {
                    ++A.undecided_doubts;
                    if (A.undecided_doubt_example.empty()) {
                        A.undecided_doubt_example = lo.arguments[static_cast<std::size_t>(i)].name;
                    }
                }
            }

            if (v.outcome == Outcome::Indeterminate) {
                bool instrument_fault = false;
                for (const auto& group : lo.exclusive_groups) {
                    bool all_present = true;
                    for (const std::int32_t m : group) {
                        if ((mask & (1U << static_cast<std::uint32_t>(m))) == 0U) { all_present = false; }
                    }
                    if (all_present) { instrument_fault = true; }
                }
                if (instrument_fault) {
                    ++A.indet_instrument;
                } else {
                    ++A.indet_genuine;
                    if (A.genuine_examples.size() < 3) { A.genuine_examples.push_back(mask); }
                }
                if (!reachable) { ++A.indet_unreachable; }
            }
        }
    };

    std::uint32_t threads = std::max(1U, std::thread::hardware_concurrency());
    if (const char* env = std::getenv("QUORUM_THREADS")) {
        const long n = std::strtol(env, nullptr, 10);
        if (n >= 1 && n <= 1024) { threads = static_cast<std::uint32_t>(n); }
    }
    if (total < (1U << 14)) { threads = 1; }  // a few thousand states: starting threads costs more than it saves
    threads = std::min(threads, total);
    if (!quiet && total >= (1U << 22)) {
        std::printf("\nchecking %u states on %u threads (roughly %u s)...\n", total, threads,
                    1U + total / 600000U / threads);
        std::fflush(stdout);
    }
    std::vector<Acc> parts(threads);
    {
        std::vector<std::thread> pool;
        const std::uint64_t per = (static_cast<std::uint64_t>(total) + threads - 1) / threads;
        for (std::uint32_t t = 0; t < threads; ++t) {
            const auto first = static_cast<std::uint32_t>(std::min<std::uint64_t>(t * per, total));
            const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>((t + 1) * per, total));
            if (t + 1 == threads) {
                scan(parts[t], first, last);  // the calling thread takes the last range
            } else {
                pool.emplace_back([&scan, &parts, t, first, last] { scan(parts[t], first, last); });
            }
        }
        for (std::thread& th : pool) { th.join(); }
    }
    for (const Acc& A : parts) {
        if (A.build_failed) {
            std::cerr << "error: framework construction failed at state " << A.build_failed_mask << "\n";
            return false;
        }
    }
    consistency_fired.assign(lo.consistency_doubts.size(), 0);
    action_selected.assign(lo.actions.size(), 0);
    action_selected_reachable.assign(lo.actions.size(), 0);
    claim_in_reachable.assign(static_cast<std::size_t>(lo.count()), 0);
    claim_in_unreachable.assign(static_cast<std::size_t>(lo.count()), 0);
    auto add = [](std::vector<std::int32_t>& into, const std::vector<std::int32_t>& from) {
        for (std::size_t i = 0; i < into.size(); ++i) { into[i] += from[i]; }
    };
    for (const Acc& A : parts) {  // in range order: see above
        for (int i = 0; i < 5; ++i) { counts[i] += A.counts[i]; }
        if (A.max_steps > max_steps) {
            max_steps = A.max_steps;
            max_steps_mask = A.max_steps_mask;
            max_steps_edges = A.max_steps_edges;
        }
        conflicted += A.conflicted;
        unsatisfied += A.unsatisfied;
        faulted += A.faulted;
        wcet_breaches += A.wcet_breaches;
        undecided_doubts += A.undecided_doubts;
        if (undecided_doubt_example.empty()) { undecided_doubt_example = A.undecided_doubt_example; }
        indet_instrument += A.indet_instrument;
        indet_genuine += A.indet_genuine;
        for (const std::uint32_t m : A.genuine_examples) {
            if (genuine_examples.size() < 3) { genuine_examples.push_back(m); }
        }
        unreachable += A.unreachable;
        indet_unreachable += A.indet_unreachable;
        add(consistency_fired, A.consistency_fired);
        add(action_selected, A.action_selected);
        add(action_selected_reachable, A.action_selected_reachable);
        add(claim_in_reachable, A.claim_in_reachable);
        add(claim_in_unreachable, A.claim_in_unreachable);
        if (!conflict_example.have) { conflict_example = A.conflict_example; }
        if (!unsatisfied_example.have) { unsatisfied_example = A.unsatisfied_example; }
    }

    if (!quiet) {
        std::printf("\nvalidation over all %u states\n", total);
        for (std::int32_t i = 0; i < 5; ++i) {
            std::printf("  %-14s %6d   %5.1f%%\n", outcome_name(static_cast<Outcome>(i)),
                        counts[static_cast<std::size_t>(i)],
                        100.0 * static_cast<double>(counts[static_cast<std::size_t>(i)]) /
                            static_cast<double>(total));
        }
        std::printf("  worst-case work %d steps of a %d bound, in state 0x%X (%d attacks live)\n", max_steps,
                    quorum::solver::Grounded<kHostMaxArgs>::step_bound(lo.count(), max_steps_edges),
                    max_steps_mask, max_steps_edges);
        std::printf("    that state asserts:");
        {
            bool any = false;
            for (std::int32_t a = 0; a < lo.observation_count; ++a) {
                if ((max_steps_mask & (1U << static_cast<std::uint32_t>(a))) != 0U) {
                    std::printf(" %s", lo.arguments[static_cast<std::size_t>(a)].name.c_str());
                    any = true;
                }
            }
            std::printf("%s\n", any ? "" : " nothing -- the quiet state is the worst case, because every"
                                            " closed-world edge is live");
        }

        if (!lo.consistency_doubts.empty()) {
            std::printf("\n  instrument consistency (from `implies`)\n");
            for (std::size_t k = 0; k < lo.consistency_doubts.size(); ++k) {
                std::printf("    %-40s fires in %6d states%s\n",
                            lo.arguments[static_cast<std::size_t>(lo.consistency_doubts[k])].name.c_str(),
                            consistency_fired[k], consistency_fired[k] == 0 ? "   NEVER -- check the readings" : "");
            }
        }

        if (!lo.channels.empty()) {
            std::printf("\n  reachability, given the declared binding\n"
                        "    physically reachable           %6d\n"
                        "    excluded by the binding layer  %6d   (an unavailable channel cannot also report)\n",
                        static_cast<std::int32_t>(total) - unreachable, unreachable);
            std::printf("    NOTE: unreachable states are still validated. The exclusion holds only where the\n"
                        "          integrator uses the binding layer; a hand-supplied mask gets no such promise.\n");
        }
        if (counts[static_cast<std::size_t>(Outcome::Indeterminate)] > 0) {
            std::printf("\n  ambiguity breakdown\n"
                        "    genuine diagnostic ambiguity   %6d\n"
                        "    instrument incoherence         %6d   (an exclusive group fully present)\n"
                        "    of which unreachable           %6d   (excluded by the declared binding)\n",
                        indet_genuine, indet_instrument, indet_unreachable);
            for (const std::uint32_t m : genuine_examples) {
                std::printf("    e.g.");
                bool any = false;
                for (std::int32_t a = 0; a < lo.observation_count; ++a) {
                    if ((m & (1U << static_cast<std::uint32_t>(a))) != 0U) {
                        std::printf(" %s", lo.arguments[static_cast<std::size_t>(a)].name.c_str());
                        any = true;
                    }
                }
                if (!any) { std::printf(" (nothing asserted)"); }
                std::printf("\n");
            }
        }
        std::printf("\n");
    }

    if (!quiet) {
        const std::int32_t reachable_total = static_cast<std::int32_t>(total) - unreachable;
        const bool has_binding = !lo.channels.empty();

        std::printf("  action selection%s\n", has_binding ? "        all states      reachable only" : "");
        for (std::size_t k = 0; k < lo.actions.size(); ++k) {
            const std::string& n = lo.arguments[static_cast<std::size_t>(lo.actions[k])].name;
            if (has_binding && reachable_total > 0) {
                std::printf("    %-20s %6d %5.1f%%   %6d %5.1f%%\n", n.c_str(), action_selected[k],
                            100.0 * static_cast<double>(action_selected[k]) / static_cast<double>(total),
                            action_selected_reachable[k],
                            100.0 * static_cast<double>(action_selected_reachable[k]) /
                                static_cast<double>(reachable_total));
            } else {
                std::printf("    %-20s %6d %5.1f%%\n", n.c_str(), action_selected[k],
                            100.0 * static_cast<double>(action_selected[k]) / static_cast<double>(total));
            }
        }
        if (has_binding) {
            std::printf("    The right-hand column is the one to reason about. The left includes states the\n"
                        "    declared binding cannot produce, such as a channel that is unavailable and\n"
                        "    reporting at the same time.\n");
        }
        std::printf("\n");
    }

    // ── pass 6 report ───────────────────────────────────────────────────────
    //
    // Can this claim reach an action at all? A claim with no directed attack path to any action provably cannot
    // influence a decision, whatever the state. That is a necessary condition, not a sufficient one -- a path may
    // always be defeated -- but it is sound in the direction that matters: no path means no influence, ever.
    //
    // In a safety casebook this is a serious omission rather than dead weight. It means a hazard was modelled and never
    // wired to a response, which reads in review as "this is handled".
    std::vector<bool> reaches_action(static_cast<std::size_t>(lo.count()), false);
    {
        std::vector<std::vector<std::int32_t>> incoming(static_cast<std::size_t>(lo.count()));
        for (const quorum::cbc::LoweredEdge& at : lo.edges) {
            incoming[static_cast<std::size_t>(at.to)].push_back(at.from);
        }
        // Walk backwards from every action: anything that can reach an action is an ancestor of one.
        std::vector<std::int32_t> stack;
        for (const std::int32_t a : lo.actions) {
            reaches_action[static_cast<std::size_t>(a)] = true;
            stack.push_back(a);
        }
        while (!stack.empty()) {
            const std::int32_t cur = stack.back();
            stack.pop_back();
            for (const std::int32_t from : incoming[static_cast<std::size_t>(cur)]) {
                if (reaches_action[static_cast<std::size_t>(from)]) { continue; }
                reaches_action[static_cast<std::size_t>(from)] = true;
                stack.push_back(from);
            }
        }
    }

    // ── is anything here actually DEFEASIBLE? ───────────────────────────────
    //
    // Added after importing a real FMEA table. The generated draft had no defeaters at all -- every claim was "sensor
    // says X, therefore Y" -- and it compiled to 100% Decided with all six OKs green. cbc could not tell a casebook from
    // an alarm list with extra steps.
    //
    // That matters because defeasibility IS the product. An indefeasible claim is a hard rule: nothing an operator knows,
    // no competing explanation, no recent maintenance can ever change it. A framework of nothing but hard rules does not
    // need argumentation semantics, and shipping one would be paying for a reasoner and getting a decision tree.
    //
    // ── and why this is an AGGREGATE check, not a per-claim one ──────────────
    //
    // The first version warned on every claim with no defeater. Run against the five real casebooks it flagged
    // `pair_ab` ("transmitters A and B both above setpoint"), `vote_collapsed`, `level_high` and eight others -- all
    // legitimate. Those are DERIVED FACTS, not diagnostic hypotheses: "A and B both read high" is a conjunction of
    // readings and there is nothing to argue with, and "two legs are compromised" is arithmetic on sensor health.
    //
    // A diagnostic hypothesis infers an unobserved cause from evidence and should be defeasible. A derived fact
    // combines observations and need not be. Structurally the two are IDENTICAL -- both are `claim X needs A B` -- so
    // cbc cannot tell them apart, and a per-claim warning would cry wolf on correct design. A diagnostic that fires on
    // good casebooks gets switched off, and then it is not protecting anything.
    //
    // What IS unambiguous is the extreme: a casebook where NOTHING is defeasible is not doing defeasible reasoning at
    // all. That is the FMEA draft's exact condition, it is mechanically detectable, and it is worth refusing to be quiet
    // about. The proportion is reported for everything else, as information rather than as a complaint.
    //
    // Author-declared attacks between presumptions are what make a claim defeasible. Generated plumbing does not count:
    // a gate is how "absent" is encoded, not a reason to doubt a conclusion.
    std::vector<std::int32_t> indefeasible;
    std::int32_t total_claims = 0;
    {
        std::vector<bool> has_defeater(static_cast<std::size_t>(lo.count()), false);
        for (const quorum::cbc::LoweredEdge& e : lo.edges) {
            const quorum::cbc::LoweredArgument& from = lo.arguments[static_cast<std::size_t>(e.from)];
            const quorum::cbc::LoweredArgument& to = lo.arguments[static_cast<std::size_t>(e.to)];
            if (from.role == quorum::cbc::Role::Gate || from.role == quorum::cbc::Role::ClosedWorld) { continue; }
            if (to.role == quorum::cbc::Role::Gate || to.role == quorum::cbc::Role::ClosedWorld) { continue; }
            if (to.role == quorum::cbc::Role::Action) { continue; }   // precedence, not doubt
            has_defeater[static_cast<std::size_t>(e.to)] = true;
        }
        for (std::int32_t a = 0; a < lo.count(); ++a) {
            const quorum::cbc::LoweredArgument& arg = lo.arguments[static_cast<std::size_t>(a)];
            if (arg.role != quorum::cbc::Role::Claim && arg.role != quorum::cbc::Role::UsableClaim) { continue; }
            if (arg.generated) { continue; }
            ++total_claims;
            if (!has_defeater[static_cast<std::size_t>(a)]) { indefeasible.push_back(a); }
        }
    }

    // ── claims that hold by DEFAULT, and therefore reverse the burden of proof ──
    //
    // Carneades (Gordon, Prakken & Walton 2007) models critical questions as premises of three kinds, separating
    // PRESUMPTIONS from EXCEPTIONS because they carry different burdens of proof: an assumption holds until questioned,
    // whereas with an exception the negation holds until the exception is positively supported.
    //
    // That is the formal name for what AUTHORING.md §7 argues on instinct, and the distinction is worth surfacing because
    // it is invisible in the source. A claim with no `needs` and no `when` is supported by nothing, so it holds unless
    // something defeats it -- the burden of proof sits on whoever wants it gone. Used deliberately that is the Piper
    // Alpha pattern and the strongest construct in the language. Used accidentally it is a claim nobody can switch off.
    //
    // Reported, never warned about: all three occurrences across the shipped casebooks (`no_measure`,
    // `work_status_unknown`, `condition_unestablished`) are correct, and a diagnostic that fires on correct design gets
    // switched off. See casebook/TAXONOMY.md §2a.
    // ── LOPA independence: does every route to a protective action share a sensor? ──
    //
    // From the CCPS criteria for an Independent Protection Layer, via the University of Michigan SAFEChE
    // LOPA tutorial (the same source family as this project's tank casebook):
    //
    //   "An IPL functions independently of the initiating event of the scenario and functions
    //    independently of all other layers that are used for that same scenario"
    //
    // and their worked counter-example: two level transmitters sharing one control logic are NOT
    // independent, because when the logic fails both transmitters fail together.
    //
    // That is a graph property, and nothing in this compiler checked it. For each action, walk backwards
    // through the gate edges from every claim that can select it, collect the observations each claim
    // depends on, and INTERSECT those sets. A non-empty intersection means every route to that action
    // passes through the same reading, so that reading is a common-cause failure for the whole action --
    // a single sensor whose silence disables the protection entirely.
    //
    // This is the CAPECO shape stated as a structural rule rather than as a story, and it is the question
    // a LOPA asks that an FMEA cannot: not "what can fail" but "do my protections share a dependency".
    //
    // Reported, not refused. One observation legitimately gating one action is normal and often correct --
    // a single pressure switch is a perfectly good trip. What must not happen is that nobody NOTICED.
    {
        // Backwards adjacency over support only: gate -> target means target needs gate's inputs.
        std::vector<std::vector<std::int32_t>> supports(static_cast<std::size_t>(lo.count()));
        for (const quorum::cbc::LoweredEdge& e : lo.edges) {
            if (lo.arguments[static_cast<std::size_t>(e.from)].role == quorum::cbc::Role::Gate) {
                supports[static_cast<std::size_t>(e.to)].push_back(e.from);
            }
        }
        // Gates are fed by the observations and claims they conjoin.
        std::vector<std::vector<std::int32_t>> gate_inputs(static_cast<std::size_t>(lo.count()));
        for (const quorum::cbc::LoweredEdge& e : lo.edges) {
            if (lo.arguments[static_cast<std::size_t>(e.to)].role == quorum::cbc::Role::Gate) {
                gate_inputs[static_cast<std::size_t>(e.to)].push_back(e.from);
            }
        }

        // Observations a single argument transitively depends on.
        const auto deps_of = [&](std::int32_t root) {
            std::set<std::int32_t> obs;
            std::vector<std::int32_t> stack{root};
            std::vector<bool> seen(static_cast<std::size_t>(lo.count()), false);
            while (!stack.empty()) {
                const std::int32_t cur = stack.back();
                stack.pop_back();
                if (seen[static_cast<std::size_t>(cur)]) { continue; }
                seen[static_cast<std::size_t>(cur)] = true;
                if (cur < lo.observation_count) { obs.insert(cur); continue; }
                for (const std::int32_t g : supports[static_cast<std::size_t>(cur)]) {
                    for (const std::int32_t in : gate_inputs[static_cast<std::size_t>(g)]) {
                        stack.push_back(in);
                    }
                }
            }
            return obs;
        };

        bool printed = false;
        bool sole_route_safe = false;
        for (const std::int32_t act : lo.actions) {
            // Claims that can select this action are the gate inputs feeding it.
            std::vector<std::int32_t> routes;
            for (const std::int32_t g : supports[static_cast<std::size_t>(act)]) {
                for (const std::int32_t in : gate_inputs[static_cast<std::size_t>(g)]) {
                    if (in >= lo.observation_count) { routes.push_back(in); }
                }
            }
            // A SINGLE route is not exempt -- it is the worst case, and this check used to skip it.
            //
            // The first version said "one route cannot be non-independent of itself" and moved on. That is a
            // definitional dodge. An action reachable by exactly one route depends entirely on that route's
            // observations, which is the single point of failure a LOPA exists to find.
            //
            // What it hid: pump.cb declares `action shutdown safe when bearing_wear`, and `bearing_wear needs
            // vib_high bearing_temp_high`. The SAFE STATE of the reference casebook is reachable only when both
            // of two sensors speak, and either going quiet disables it. The check reported the `maintain` action
            // as a win on its first run while saying nothing about the safe action in the same file.
            if (routes.empty()) { continue; }
            if (routes.size() == 1) {
                const std::set<std::int32_t> only = deps_of(routes[0]);
                if (only.empty()) { continue; }
                if (!quiet) {
                    if (!printed) {
                        std::printf("  protection-layer independence  (CCPS IPL criterion 2)\n");
                        printed = true;
                    }
                    const bool is_safe = (act == lo.safe_action);
                    std::printf("    %-14s %-22s reachable by ONE route (%s), depending on:",
                                is_safe ? "SOLE ROUTE*" : "SOLE ROUTE",
                                lo.arguments[static_cast<std::size_t>(act)].name.c_str(),
                                lo.arguments[static_cast<std::size_t>(routes[0])].name.c_str());
                    for (const std::int32_t o : only) {
                        std::printf(" %s", lo.arguments[static_cast<std::size_t>(o)].name.c_str());
                    }
                    std::printf("\n");
                    if (is_safe) { sole_route_safe = true; }
                }
                continue;
            }

            std::set<std::int32_t> shared = deps_of(routes[0]);
            for (std::size_t i = 1; i < routes.size() && !shared.empty(); ++i) {
                const std::set<std::int32_t> d = deps_of(routes[i]);
                std::set<std::int32_t> keep;
                for (const std::int32_t o : shared) {
                    if (d.count(o) != 0) { keep.insert(o); }
                }
                shared = keep;
            }
            if (shared.empty()) { continue; }

            if (!quiet) {
                if (!printed) {
                    std::printf("  protection-layer independence  (CCPS IPL criterion 2)\n");
                    printed = true;
                }
                std::printf("    COMMON CAUSE   %-22s all %zu routes depend on:",
                            lo.arguments[static_cast<std::size_t>(act)].name.c_str(), routes.size());
                for (const std::int32_t o : shared) {
                    std::printf(" %s", lo.arguments[static_cast<std::size_t>(o)].name.c_str());
                }
                std::printf("\n");
            }
        }
        if (sole_route_safe && !quiet) {
            std::printf("    * this is the SAFE action. Every route to the safe state runs through one argument, so\n");
            std::printf("      whatever silences the observations above removes the ability to reach safety at all.\n");
            std::printf("      Often legitimate -- one trip amplifier really can be the protection -- but it must be\n");
            std::printf("      a decision, not an accident, and it is the first thing a LOPA would question.\n");
        }
        if (printed && !quiet) {
            std::printf("    A LOPA asks whether protection layers are INDEPENDENT. Every route to the action above\n");
            std::printf("    passes through the same reading, so that one instrument going quiet disables the whole\n");
            std::printf("    protection -- the layers are not independent, they are one layer wearing several hats.\n");
            std::printf("    Legitimate when a single switch really is the protection; a defect when it was meant to\n");
            std::printf("    be redundant. See casebook/TAXONOMY.md section 3 on hidden failures.\n\n");
        }
    }

    std::vector<std::int32_t> by_default;
    {
        std::vector<bool> has_support(static_cast<std::size_t>(lo.count()), false);
        for (const quorum::cbc::LoweredEdge& e : lo.edges) {
            if (lo.arguments[static_cast<std::size_t>(e.from)].role == quorum::cbc::Role::Gate) {
                has_support[static_cast<std::size_t>(e.to)] = true;
            }
        }
        for (std::int32_t a = 0; a < lo.count(); ++a) {
            const quorum::cbc::LoweredArgument& arg = lo.arguments[static_cast<std::size_t>(a)];
            // Excuses and doubts are INCLUDED, and leaving them out was a defect.
            //
            // This check first looked only at claims, which missed the arguments MOST likely to be written
            // in assumption form. An excuse reads naturally as a bare assertion -- `excuse planned_drain
            // "a planned drain is in progress"` -- with nothing supporting it. So it holds in every state,
            // so it undercuts its target in every state, so the target can NEVER hold.
            //
            // Found by hand-encoding a real HAZOP to test the Deviation/Cause/Safeguards mapping this
            // compiler prints. Two claims came back NEVER HOLDS and the cause was exactly this: two bare
            // excuses. Claim reach reported the SYMPTOM while this report, which exists to name the CAUSE,
            // stayed silent. One instrument's blind spot found by using another -- and the first attempt at
            // this fix patched the defeasibility loop by mistake, because the two share a filter line.
            if (arg.role != quorum::cbc::Role::Claim && arg.role != quorum::cbc::Role::UsableClaim
                && arg.role != quorum::cbc::Role::Excuse && arg.role != quorum::cbc::Role::Doubt) { continue; }
            if (arg.generated) { continue; }
            if (!has_support[static_cast<std::size_t>(a)]) { by_default.push_back(a); }
        }
    }

    std::vector<std::int32_t> dead_claims;
    std::vector<std::int32_t> phantom_claims;
    std::vector<std::int32_t> inert_claims;
    for (std::int32_t a = 0; a < lo.count(); ++a) {
        const quorum::cbc::LoweredArgument& arg = lo.arguments[static_cast<std::size_t>(a)];
        // Only what the author wrote. Gates and the closed-world argument are plumbing, and observations are inputs
        // rather than conclusions.
        if (arg.role == quorum::cbc::Role::Gate || arg.role == quorum::cbc::Role::ClosedWorld ||
            arg.role == quorum::cbc::Role::Observation ||
            arg.role == quorum::cbc::Role::Action) {
            continue;
        }
        if (arg.generated) { continue; }

        const std::int32_t r = claim_in_reachable[static_cast<std::size_t>(a)];
        const std::int32_t u = claim_in_unreachable[static_cast<std::size_t>(a)];
        if (r == 0 && u == 0) {
            dead_claims.push_back(a);
        } else if (r == 0) {
            phantom_claims.push_back(a);
        }
        if (!reaches_action[static_cast<std::size_t>(a)]) { inert_claims.push_back(a); }
    }

    // ── what does this casebook do when exactly ONE thing is wrong? ─────────
    //
    // Derived from TAXONOMY.md section 1. ISO 13379-1's Failure Mode Symptoms Analysis works mode-by-mode, asking which
    // symptoms detect a given failure mode. It never asks the reverse question -- which symptoms warrant action on their
    // own -- and that reverse question is what `pump.cb` failed.
    //
    // Every claim in pump.cb was `needs A B`, because it was built for differential diagnosis: telling cavitation from
    // bearing wear from misalignment. The cost was that a single unambiguous reading produced nothing. A pump with a hot
    // bearing and nothing else asserted got `continue`, while the ordinary alarm system it would replace said `shutdown`.
    // All fourteen single symptoms were silent.
    //
    // That was found by hand, on one casebook, by comparing against an alarm system. It is cheap to compute for every
    // casebook, so it is computed here instead of waiting to be stumbled into again.
    //
    // Silence is often CORRECT: a single spectral feature identifies no fault, and instrument-health observations say
    // nothing about the machine. So this reports rather than fails. What it makes impossible is not NOTICING.
    if (!quiet && lo.observation_count > 0) {
        std::printf("  single-symptom response  (what happens when exactly one thing is asserted)\n");
        std::int32_t silent = 0;
        std::vector<std::int32_t> silent_names;
        for (std::int32_t b = 0; b < lo.observation_count; ++b) {
            HostFramework fw1;
            HostSolver solver1;
            const std::uint32_t m = 1U << static_cast<std::uint32_t>(b);
            if (build(fw1, lo, m) != Status::Ok) { continue; }
            const quorum::arbiter::Verdict v1 = quorum::arbiter::decide(fw1, solver1, policy);
            const bool is_idle = v1.outcome == Outcome::Decided && lo.idle_action >= 0 &&
                                 v1.action == Arg{lo.idle_action};
            if (is_idle) {
                ++silent;
                silent_names.push_back(b);
            }
        }
        std::printf("    %d of %d symptoms produce no response on their own\n", silent, lo.observation_count);
        if (silent > 0) {
            std::printf("   ");
            for (const std::int32_t b : silent_names) {
                std::printf(" %s", lo.arguments[static_cast<std::size_t>(b)].name.c_str());
            }
            std::printf("\n");
            std::printf("    For each, ask the question FMSA does not: is the state this symptom reports damaging BY\n");
            std::printf("    ITSELF? If so it needs its own claim -- a condition, not only a diagnosis -- because\n");
            std::printf("    waiting for a second symptom means waiting for the damage. If not, silence is correct.\n");
        }
        std::printf("\n");
    }

    if (!quiet && !by_default.empty()) {
        std::printf("  burden of proof\n");
        for (const std::int32_t a : by_default) {
            std::printf("    HOLDS BY DEFAULT  %-28s defeated only by a positive act\n",
                        lo.arguments[static_cast<std::size_t>(a)].name.c_str());
        }
        std::printf("    These are ASSUMPTIONS in the Carneades sense: nothing supports them, so they stand until\n");
        std::printf("    something argues them away, and the burden of proof is on whoever wants them gone. That is\n");
        std::printf("    the Piper Alpha pattern and it is deliberate here -- but check it IS deliberate, because a\n");
        std::printf("    claim supported by nothing cannot be switched off. See casebook/TAXONOMY.md section 2a.\n\n");
    }

    const bool nothing_defeasible = total_claims > 0 && indefeasible.size() == static_cast<std::size_t>(total_claims);
    if (!quiet && total_claims > 0) {
        std::printf("  defeasibility  %d of %d claims can be defeated by something the author declared\n",
                    total_claims - static_cast<std::int32_t>(indefeasible.size()), total_claims);
        if (nothing_defeasible) {
            std::printf("    NOTHING IS DEFEASIBLE. Every claim here is a hard rule: no competing explanation, no\n");
            std::printf("    recent maintenance, no operator knowledge can ever change a conclusion. A framework of\n");
            std::printf("    hard rules does not need argumentation semantics -- this is a decision tree, and\n");
            std::printf("    shipping it would mean paying for a reasoner and getting an alarm list.\n");
            std::printf("    An FMEA table produces exactly this, because it records what fails and not what would\n");
            std::printf("    explain the reading away. That second part is the engineer's contribution.\n");
        }
        std::printf("\n");
    }

    if (!quiet) {
        std::printf("  claim reach\n");
        if (dead_claims.empty() && phantom_claims.empty() && inert_claims.empty()) {
            std::printf("    every authored claim holds somewhere reachable and can reach an action\n\n");
        } else {
            for (const std::int32_t a : dead_claims) {
                std::printf("    NEVER HOLDS   %-32s no state at all makes this true\n",
                            lo.arguments[static_cast<std::size_t>(a)].name.c_str());
            }
            for (const std::int32_t a : phantom_claims) {
                std::printf("    ONLY IMPOSSIBLE %-30s holds in %d states, every one excluded by the binding\n",
                            lo.arguments[static_cast<std::size_t>(a)].name.c_str(),
                            claim_in_unreachable[static_cast<std::size_t>(a)]);
            }
            for (const std::int32_t a : inert_claims) {
                std::printf("    NO PATH       %-32s cannot reach any action, so it can never change a decision\n",
                            lo.arguments[static_cast<std::size_t>(a)].name.c_str());
            }
            std::printf("\n");
        }
    }

    bool ok = true;
    auto report = [&](bool good, const char* what, std::int32_t n) {
        if (good) {
            if (!quiet) { std::printf("  OK    %s\n", what); }
        } else {
            std::printf("  FAIL  %s  (%d states)\n", what, n);
            ok = false;
        }
    };
    /// Spell out a state as the list of observations asserted in it, so the author can reproduce it.
    auto describe = [&lo](std::uint32_t mask) {
        std::string out;
        for (std::int32_t a = 0; a < lo.observation_count; ++a) {
            if ((mask & (1U << static_cast<std::uint32_t>(a))) != 0U) {
                out += " " + lo.arguments[static_cast<std::size_t>(a)].name;
            }
        }
        return out.empty() ? std::string(" (nothing asserted)") : out;
    };

    report(conflicted == 0, "no state selects two actions at once", conflicted);
    if (conflict_example.have) {
        std::string names;
        for (const std::int32_t a : conflict_example.actions) {
            if (!names.empty()) { names += " and "; }
            names += "'" + lo.arguments[static_cast<std::size_t>(a)].name + "'";
        }
        std::printf("        %s are BOTH selected when%s\n", names.c_str(),
                    describe(conflict_example.mask).c_str());
        if (conflict_example.actions.size() == 2) {
            std::printf("        nothing orders them, so the framework cannot choose. Add `prefer %s over %s`\n"
                        "        (or the reverse) to say which wins, or narrow one of their conditions so they\n"
                        "        cannot both hold.\n",
                        lo.arguments[static_cast<std::size_t>(conflict_example.actions[0])].name.c_str(),
                        lo.arguments[static_cast<std::size_t>(conflict_example.actions[1])].name.c_str());
        }
    }

    report(unsatisfied == 0, "every state selects an action", unsatisfied);
    if (unsatisfied_example.have) {
        std::printf("        no action is selected when%s\n", describe(unsatisfied_example.mask).c_str());
        std::printf("        every declared action is decisively defeated there. Usually the idle action has\n"
                    "        conditions that exclude it too -- an idle action should have NO conditions, so it\n"
                    "        survives whenever nothing else fires.\n");
    }
    report(faulted == 0, "the solver completes in every state", faulted);
    report(wcet_breaches == 0, "work stays inside the 2n + 2m bound", wcet_breaches);

    // A casebook nothing can argue against is a decision tree. Refusing it is the point: an FMEA import produces exactly
    // this shape, it validates to 100% Decided, and accepting it would mean shipping an argumentation engine that has
    // nothing to argue about.
    report(!nothing_defeasible, "at least one claim can be argued against",
           static_cast<std::int32_t>(indefeasible.size()));

    // Dead actions. An action the casebook declares but can never choose is a defect, and a quiet one: the author
    // believes a response exists and it does not. Found by writing a third casebook whose `escalate_to_oim` was
    // selected in 0 of 1024 states because a higher-precedence action fired on the identical condition.
    std::int32_t dead = 0;
    std::string dead_names;
    for (std::size_t k = 0; k < lo.actions.size(); ++k) {
        if (action_selected[k] != 0) { continue; }
        ++dead;
        if (!dead_names.empty()) { dead_names += ", "; }
        dead_names += lo.arguments[static_cast<std::size_t>(lo.actions[k])].name;
    }
    if (dead != 0) {
        std::printf("  FAIL  every declared action is reachable  (%d unreachable: %s)\n"
                    "        an action that can never be selected is usually a precedence chain that cannot let it\n"
                    "        through, or conditions that cannot co-occur\n",
                    dead, dead_names.c_str());
        ok = false;
    } else if (!quiet) {
        std::printf("  OK    every declared action is selected in at least one state\n");
    }
    if (undecided_doubts != 0) {
        std::printf("  FAIL  no doubt is ever Undecided  (%d, e.g. '%s') -- an odd cycle survived lowering\n",
                    undecided_doubts, undecided_doubt_example.c_str());
        ok = false;
    } else if (!quiet) {
        std::printf("  OK    no doubt is ever Undecided (the raw/claim split held)\n");
    }

    lo.worst_case_mask = max_steps_mask;
    lo.worst_case_steps = max_steps;
    return ok;
}

/// Validate a palette.
///
/// The affordable-and-honest approach, rather than the affordable-or-honest one. The full cross-product for pump +
/// tank is 2^29 = 536 million states, which is not something to run on every build. But exhaustive cross-product
/// validation is confirmation, not proof, and there is an exact argument available instead:
///
///   1. every member is already validated exhaustively on its own, in its own framework;
///   2. the merged framework contains NO cross-member edge;
///   3. each member's induced subgraph in the merged framework is EXACTLY its standalone framework.
///
/// Given (2) and (3), a member's grounded labelling in the palette is identical to its labelling alone, because the
/// grounded fixpoint propagates only along edges and there is no edge leaving the member. So (1) transfers. All three
/// are checked exactly and in linear time; none is sampled.
///
/// The cross-product sweep still exists in `test_palette.cpp`, where it ran over all 33.5 million pump+tank states
/// and found zero differences. That is the evidence that this argument is sound; this is the check that makes it
/// cheap enough to run every time.
[[nodiscard]] bool validate_palette(const quorum::cbc::Palette& p,
                                    const std::vector<quorum::cbc::Lowered>& members,
                                    bool quiet) {
    bool ok = true;

    if (!quiet) {
        std::printf("\npalette of %d casebooks\n", static_cast<int>(p.members.size()));
        for (std::size_t i = 0; i < p.members.size(); ++i) {
            std::printf("  %-24s %3d arguments, observations %2d..%-2d\n", p.members[i].casebook.c_str(),
                        members[i].count(), p.members[i].observation_offset,
                        p.members[i].observation_offset + p.members[i].observation_count - 1);
        }
        std::printf("  merged                   %3d arguments, %d attacks, capacity Framework<%d, %d>\n",
                    p.all.count(), static_cast<int>(p.all.edges.size()), p.all.max_args, p.all.max_attacks);
    }

    // Which member owns each combined argument.
    std::vector<int> owner(static_cast<std::size_t>(p.all.count()), -1);
    for (std::size_t i = 0; i < p.members.size(); ++i) {
        for (const std::int32_t a : p.members[i].map) {
            if (a >= 0) { owner[static_cast<std::size_t>(a)] = static_cast<int>(i); }
        }
    }

    // (2) No cross-member edge. This is the precondition under which ASPIC+'s non-interference and
    // crash-resistance postulates apply, so it is checked rather than assumed.
    std::int32_t cross = 0;
    std::string cross_example;
    for (const quorum::cbc::LoweredEdge& e : p.all.edges) {
        if (owner[static_cast<std::size_t>(e.from)] == owner[static_cast<std::size_t>(e.to)]) { continue; }
        ++cross;
        if (cross_example.empty()) {
            cross_example = p.all.arguments[static_cast<std::size_t>(e.from)].name + " -> " +
                            p.all.arguments[static_cast<std::size_t>(e.to)].name;
        }
    }
    if (cross == 0) {
        if (!quiet) { std::printf("\n  OK    no attack crosses between casebooks\n"); }
    } else {
        std::printf("\n  FAIL  %d attacks cross between casebooks (e.g. %s)\n"
                    "        casebooks on one device must be syntactically disjoint, or one can change another's\n"
                    "        verdicts and neither can be certified independently\n",
                    cross, cross_example.c_str());
        ok = false;
    }

    // (3) Each member's induced subgraph is exactly its standalone framework.
    for (std::size_t i = 0; i < p.members.size(); ++i) {
        const quorum::cbc::PaletteMember& m = p.members[i];
        const quorum::cbc::Lowered& alone = members[i];

        // Combined index -> member-local index.
        std::map<std::int32_t, std::int32_t> back;
        for (std::size_t k = 0; k < m.map.size(); ++k) {
            if (m.map[k] >= 0) { back[m.map[k]] = static_cast<std::int32_t>(k); }
        }

        std::set<std::pair<std::int32_t, std::int32_t>> induced;
        for (const quorum::cbc::LoweredEdge& e : p.all.edges) {
            if (owner[static_cast<std::size_t>(e.from)] != static_cast<int>(i)) { continue; }
            if (owner[static_cast<std::size_t>(e.to)] != static_cast<int>(i)) { continue; }
            induced.insert({back[e.from], back[e.to]});
        }
        std::set<std::pair<std::int32_t, std::int32_t>> standalone;
        for (const quorum::cbc::LoweredEdge& e : alone.edges) { standalone.insert({e.from, e.to}); }

        const bool same_args = static_cast<std::int32_t>(m.map.size()) == alone.count();
        const bool same_edges = induced == standalone;
        const bool same_obs = m.observation_count == alone.observation_count;

        if (same_args && same_edges && same_obs) {
            if (!quiet) {
                std::printf("  OK    %s is unchanged by the merge (%d arguments, %d attacks preserved exactly)\n",
                            m.casebook.c_str(), alone.count(), static_cast<int>(standalone.size()));
            }
        } else {
            std::printf("  FAIL  %s is NOT preserved by the merge", m.casebook.c_str());
            if (!same_args) { std::printf(" (argument count differs)"); }
            if (!same_edges) {
                std::printf(" (%d attacks standalone vs %d induced)", static_cast<int>(standalone.size()),
                            static_cast<int>(induced.size()));
            }
            if (!same_obs) { std::printf(" (observation count differs)"); }
            std::printf("\n        its exhaustive validation therefore does not transfer to the palette\n");
            ok = false;
        }
    }

    if (!quiet && ok) {
        std::printf("\n  each casebook was validated exhaustively on its own, and the two checks above show the\n"
                    "  merge preserves each one exactly -- so those results carry over without sweeping the\n"
                    "  %d-observation cross product (2^%d states).\n",
                    p.all.observation_count, p.all.observation_count);
    }
    return ok;
}

/// Write a proof certificate: for every state, the grounded labelling WITH the round in which each argument was
/// labelled, plus the outcome and the action. `tools/qcert` checks it with no code in common with this compiler.
///
/// Why rounds: a labelling alone cannot be checked for being the GROUNDED one cheaply, but a labelling with rounds can,
/// locally. Every In argument's attackers are all Out at an earlier round; every Out argument has an In attacker at an
/// earlier round; and every unlabelled argument has an attacker that is not Out and none that is In. The first two
/// show the labelled part is contained in the grounded labelling, the third shows it is a fixed point, and the grounded
/// labelling is the least fixed point, so the two are equal.
///
/// The rounds are computed here by synchronous iteration, independently of the device solver, and the result is
/// compared against `solver::Grounded` argument by argument before anything is written.
/// One sheet row, as the timed bench script needs it.
struct SheetRow {
    std::int32_t bit = 0;
    std::uint32_t mask = 0;
    std::uint32_t with = 0;  ///< the other readings with which it matters, 0 if alone or none
    std::string with_names;
    std::string alone, then, without;
};

/// The bench script for a timed rulebook, appended to the commissioning sheet:
///
///     scene   <quiet ms>                     write no readings, then let this much time pass
///     set     <ms into the scene>  <mask>    write the readings
///     expect  <ms into the scene>  <action>  <label>
///
/// The board works out its timed readings from the history of the others, so a bench test is a script through time
/// rather than a list of states. Every expected answer comes from replaying the WHOLE script, from the first scene on,
/// through the definitions in LANGUAGE.md applied to the full history -- not through the board's timer code -- and
/// deciding with the timed readings that gives. The replay steps where the board steps: just before and after each
/// write, at each answer, and at the last moment and the first moment after a timed reading expires on its own.
///
/// Each timed reading is tested just inside and just outside its window. A timed reading that watches another is
/// driven through the readings at the bottom of the chain. Each scene checks, in the replay, that the reading under
/// test really is set or clear as its label says; when the readings it is built from share inputs in a way that
/// makes that impossible to arrange, the scene keeps its writes but drops its answers, and cbc says so.
[[nodiscard]] bool write_timed_script(const quorum::cbc::Lowered& lo, const std::vector<SheetRow>& rows,
                                      const std::function<bool(std::uint32_t, std::string&, std::string&)>& decide,
                                      const std::function<std::uint32_t(std::uint32_t)>& immediate, std::ofstream& out) {
    using Timer = quorum::cbc::LoweredTimer;
    struct Step {
        std::uint64_t at;
        std::uint32_t full;  ///< base readings and the timed readings the definitions give at this step
    };
    auto has = [](std::uint32_t m, std::int32_t o) { return o >= 0 && (m & (1U << static_cast<std::uint32_t>(o))) != 0U; };
    auto bit = [](std::int32_t o) { return o >= 0 ? 1U << static_cast<std::uint32_t>(o) : 0U; };
    std::uint32_t timed_bits = 0;
    for (const Timer& t : lo.timers) { timed_bits |= bit(t.observation); }

    // ── the replay ──
    std::vector<Step> h;
    std::uint64_t now = 0;
    std::uint32_t base = 0;
    auto rose = [&](std::size_t i, std::int32_t o) { return has(h[i].full, o) && (i == 0 || !has(h[i - 1].full, o)); };
    // The definitions for one timer at the last step, whose earlier timers are already in h.back().full.
    auto defined = [&](const Timer& t) {
        const std::size_t k = h.size() - 1;
        if (t.kind == 0) {
            if (!has(h[k].full, t.source)) { return false; }
            std::size_t j = k;
            while (j > 0 && has(h[j - 1].full, t.source)) { --j; }
            for (std::size_t i = 0; i <= j; ++i) {
                if (rose(i, t.after) && h[j].at - h[i].at <= t.window_ms) { return true; }
            }
            return false;
        }
        if (t.kind == 1) {
            for (std::size_t i = k + 1; i-- > 0;) {
                if (h[k].at - h[i].at > t.window_ms) { break; }
                if (has(h[i].full, t.source)) { return true; }
            }
            return false;
        }
        int n = 0;
        for (std::size_t i = k + 1; i-- > 0;) {
            if (h[k].at - h[i].at > t.window_ms) { break; }
            n += rose(i, t.source) ? 1 : 0;
        }
        return n >= t.times;
    };
    auto push = [&](std::uint64_t at, std::uint32_t b) {
        h.push_back({at, b});
        for (const Timer& t : lo.timers) {  // table order: a timer sees the earlier timers of this same step
            if (defined(t)) { h.back().full |= bit(t.observation); }
        }
    };
    // The next moment a timed reading that is set now expires with the readings unchanged, as on the board.
    auto next_expiry = [&](std::uint64_t& at) {
        bool any = false;
        const std::size_t k = h.size() - 1;
        for (const Timer& t : lo.timers) {
            if (!has(h[k].full, t.observation) || t.kind == 0) { continue; }
            std::uint64_t from = 0;
            if (t.kind == 1) {
                for (std::size_t i = k + 1; i-- > 0;) {
                    if (has(h[i].full, t.source)) { from = h[i].at; break; }
                }
            } else {
                int n = 0;
                for (std::size_t i = k + 1; i-- > 0;) {
                    if (rose(i, t.source) && ++n == t.times) { from = h[i].at; break; }
                }
            }
            const std::uint64_t e = from + t.window_ms + 1U;
            if (e > now && (!any || e < at)) { at = e; any = true; }
        }
        return any;
    };
    auto advance = [&](std::uint64_t to) {
        std::uint64_t e = 0;
        while (next_expiry(e) && e <= to) {
            if (e - 1U > now) { push(e - 1U, base); }
            push(e, base);
            now = e;
        }
        now = std::max(now, to);
    };
    auto write = [&](std::uint64_t at, std::uint32_t b) {
        advance(at);
        push(at, base);  // the board steps just before applying a write ...
        base = b;
        push(at, base);  // ... and just after
    };
    auto look = [&](std::uint64_t at) {
        advance(at);
        push(at, base);
        return h.back().full;
    };
    push(0, 0);

    // ── how to make a reading rise at a given moment, through the base readings ──
    struct Edge {
        std::uint64_t at;
        std::int32_t obs;
        bool on;
    };
    constexpr std::uint64_t kPulse = 300U;  // how long a base reading is held set
    constexpr std::uint64_t kSettle = 2000U;  // after a timed reading expires, before the next scene begins
    const auto timer_of = [&](std::int32_t o) -> const Timer* {
        for (const Timer& t : lo.timers) {
            if (t.observation == o) { return &t; }
        }
        return nullptr;
    };
    // hold: how long the reading stays set after it rises; lead: how long before its rise its drive starts.
    std::function<std::uint64_t(std::int32_t)> hold;
    std::function<std::uint64_t(std::int32_t)> lead;
    std::function<std::uint64_t(const Timer&)> gap;
    gap = [&](const Timer& t) { return hold(t.source) + kPulse; };  // between the rises a Count needs
    hold = [&](std::int32_t o) -> std::uint64_t {
        const Timer* t = timer_of(o);
        if (t == nullptr) { return kPulse; }
        if (t->kind == 0) { return hold(t->source); }
        if (t->kind == 1) { return hold(t->source) + t->window_ms; }
        const std::uint64_t spread = static_cast<std::uint64_t>(t->times - 1) * gap(*t);
        return t->window_ms > spread ? t->window_ms - spread : 1U;
    };
    lead = [&](std::int32_t o) -> std::uint64_t {
        const Timer* t = timer_of(o);
        if (t == nullptr) { return 0U; }
        if (t->kind == 0) { return std::max(lead(t->source), lead(t->after)); }
        if (t->kind == 1) { return lead(t->source); }
        return static_cast<std::uint64_t>(t->times - 1) * gap(*t) + lead(t->source);
    };
    std::function<void(std::int32_t, std::uint64_t, std::vector<Edge>&)> rise;
    rise = [&](std::int32_t o, std::uint64_t at, std::vector<Edge>& e) {
        const Timer* t = timer_of(o);
        if (t == nullptr) {
            e.push_back({at, o, true});
            e.push_back({at + kPulse, o, false});
        } else if (t->kind == 0) {
            rise(t->after, at, e);  // together counts as "after"
            rise(t->source, at, e);
        } else if (t->kind == 1) {
            rise(t->source, at, e);
        } else {
            const std::uint64_t g = gap(*t);
            for (std::int32_t k = 0; k < t->times; ++k) {
                rise(t->source, at - static_cast<std::uint64_t>(t->times - 1 - k) * g, e);
            }
        }
    };

    // ── writing scenes ──
    auto margin = [](std::uint32_t w) {
        const std::uint32_t m = std::clamp<std::uint32_t>(w / 10U, 100U, 5000U);
        return std::min(m, w / 4U);
    };
    auto dur = [](std::uint64_t ms) {
        char b[48];
        const auto tidy = [](std::string t, const std::string& zero) {
            return t.size() > zero.size() && t.substr(t.size() - zero.size()) == zero ? t.substr(0, t.size() - zero.size()) : t;
        };
        if (ms >= 3600000U && ms % 60000U == 0U) {
            std::snprintf(b, sizeof b, "%llu h %llu min", static_cast<unsigned long long>(ms / 3600000U),
                          static_cast<unsigned long long>(ms % 3600000U / 60000U));
            return tidy(b, " 0 min");
        }
        if (ms >= 60000U && ms % 1000U == 0U) {
            std::snprintf(b, sizeof b, "%llu min %llu s", static_cast<unsigned long long>(ms / 60000U),
                          static_cast<unsigned long long>(ms % 60000U / 1000U));
            return tidy(b, " 0 s");
        }
        if (ms % 1000U == 0U) { return std::to_string(ms / 1000U) + " s"; }
        std::snprintf(b, sizeof b, "%.1f s", static_cast<double>(ms) / 1000.0);
        return std::string(b);
    };
    const auto name = [&](std::int32_t o) { return lo.arguments[static_cast<std::size_t>(o)].name; };
    const auto names = [&](std::uint32_t m) {
        std::string r;
        for (std::int32_t o = 0; o < 32; ++o) {
            if (has(m, o)) { r += (r.empty() ? "" : " + ") + name(o); }
        }
        return r;
    };
    // Quiet: write nothing, and let time pass until no timed reading remembers anything, as the board would.
    auto memory_until = [&](const Timer& t) {
        std::uint64_t last = 0;
        bool any = false;
        for (std::size_t i = h.size(); i-- > 0;) {
            const bool event = t.kind == 0 ? rose(i, t.after) : (t.kind == 1 ? has(h[i].full, t.source) : rose(i, t.source));
            if (event) { last = h[i].at; any = true; break; }
        }
        return any ? last + t.window_ms + margin(t.window_ms) : 0U;
    };
    // A scene's edges are written in time order, interleaved with its answers: run_until(T) writes those up to T.
    std::vector<Edge> pending;
    std::size_t next_edge = 0;
    std::map<std::int32_t, int> level;
    std::uint32_t scene_ctx = 0;
    std::ostringstream buf;  // one scene, kept back until its answers are known to mean what they say
    std::uint64_t start = 0;
    // The base readings each timed reading is built from, through any chain.
    std::function<std::uint32_t(std::int32_t)> inputs;
    inputs = [&](std::int32_t o) -> std::uint32_t {
        const Timer* t = timer_of(o);
        if (t == nullptr) { return bit(o); }
        return inputs(t->source) | (t->after >= 0 ? inputs(t->after) : 0U);
    };
    // Quiet before a scene that will write the base readings in `uses`: long enough that every timed reading built
    // from any of them has forgotten earlier scenes, and that no timed reading is still set. A timed reading the
    // scene cannot reach keeps its history; it stays clear through the scene, which the replay confirms either way.
    auto scene = [&](std::uint32_t uses) {
        pending.clear();
        next_edge = 0;
        write(now, 0);
        const std::uint64_t from = now;
        for (int round = 0; round < 64; ++round) {  // a chain releases one link per round
            std::uint64_t until = now;
            for (const Timer& t : lo.timers) {
                if ((inputs(t.observation) & uses) != 0U) { until = std::max(until, memory_until(t)); }
            }
            if (until <= now && (look(now) & timed_bits) == 0U) { break; }
            if (until <= now) {
                // Wait for the next expiry, and then a margin: a bench in real time writes a few ms late, so a scene
                // must not begin at the very millisecond an earlier timed reading lets go.
                std::uint64_t e = 0;
                if (!next_expiry(e)) { break; }
                until = e + kSettle;
            }
            advance(until);
        }
        buf << "scene\t" << now - from << "\n";
        start = now;
    };
    auto play = [&](std::vector<Edge> edges, std::uint32_t ctx) {
        std::stable_sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.at < b.at; });
        pending = std::move(edges);
        next_edge = 0;
        level.clear();
        scene_ctx = ctx;
    };
    auto run_until = [&](std::uint64_t until) {
        // Each moment's mask after every edge at that moment: a reading pulsed twice at once stays set.
        while (next_edge < pending.size() && pending[next_edge].at <= until) {
            const std::uint64_t at = pending[next_edge].at;
            for (; next_edge < pending.size() && pending[next_edge].at == at; ++next_edge) {
                level[pending[next_edge].obs] += pending[next_edge].on ? 1 : -1;
            }
            std::uint32_t m = scene_ctx;
            for (const auto& [o, n] : level) {
                if (n > 0) { m |= bit(o); }
            }
            write(start + at, m);
            buf << "set\t" << at << "\t0x" << std::hex << m << std::dec << "\n";
        }
    };
    bool ok_scene = true;
    auto expect = [&](std::uint64_t at, const std::string& label, std::int32_t subject, bool want_set,
                      std::string* got = nullptr) {
        run_until(at);
        const std::uint32_t full = look(start + at);
        if (subject >= 0 && has(full, subject) != want_set) { ok_scene = false; }
        std::string a, o;
        if (!decide(full, a, o)) { return false; }
        buf << "expect\t" << at << "\t" << a << "\t" << label << "\n";
        if (got != nullptr) { *got = a; }
        return true;
    };
    std::vector<std::string> skipped;
    auto finish = [&](const std::string& what) {
        run_until(~0ULL);
        if (ok_scene) {
            out << buf.str();
        } else {
            // Keep the writes, so the board's history matches the replay's; drop the answers.
            std::istringstream in(buf.str());
            for (std::string l; std::getline(in, l);) {
                if (l.rfind("expect\t", 0) != 0) { out << l << "\n"; }
            }
            skipped.push_back(what);
        }
        buf.str("");
        buf.clear();
        ok_scene = true;
    };
    // The readings with which a timed reading changes the decision, minus the readings that drive it.
    auto context = [&](const Timer& t) {
        std::uint32_t ctx = 0;
        for (const SheetRow& r : rows) {
            if (r.bit == t.observation) { ctx = r.with; }
        }
        std::vector<Edge> drive;
        rise(t.observation, lead(t.observation) + 1U, drive);
        for (const Edge& e : drive) { ctx &= ~bit(e.obs); }
        return ctx & ~timed_bits;
    };

    scene(0);
    if (!expect(0, "nothing set", -1, false)) { return false; }
    finish("nothing set");

    // Each timed reading on both sides of its window, with the readings that make it matter, shortest first.
    std::vector<const Timer*> order;
    for (const Timer& t : lo.timers) { order.push_back(&t); }
    std::stable_sort(order.begin(), order.end(), [](const Timer* a, const Timer* b) { return a->window_ms < b->window_ms; });
    for (const Timer* tp : order) {
        const Timer& t = *tp;
        const std::uint32_t ctx = context(t);
        const std::string in_ctx = ctx != 0U ? ", with " + names(ctx) : "";
        const std::uint32_t w = t.window_ms;
        const std::uint32_t m = margin(w);
        const std::string tn = name(t.observation) + ": ";
        if (t.kind == 0) {
            for (const std::uint32_t g : {w - m, w + m}) {
                scene(inputs(t.observation) | ctx);
                const std::uint64_t t0 = std::max(lead(t.after), lead(t.source) > g ? lead(t.source) - g : 0U);
                std::vector<Edge> e;
                rise(t.after, t0, e);
                rise(t.source, t0 + g, e);
                play(e, ctx);
                if (!expect(t0 + g, tn + name(t.source) + " " + dur(g) + " after " + name(t.after) + in_ctx,
                            t.observation, g <= w)) {
                    return false;
                }
                finish(name(t.observation));
            }
        } else if (t.kind == 1) {
            scene(inputs(t.observation) | ctx);
            const std::uint64_t t0 = lead(t.source);
            std::vector<Edge> e;
            rise(t.source, t0, e);
            play(e, ctx);
            const std::uint64_t gone = t0 + hold(t.source);  // the last moment the source is set
            if (!expect(gone + w - m, tn + name(t.source) + " last set " + dur(w - m) + " ago" + in_ctx, t.observation,
                        true) ||
                !expect(gone + w + m, tn + name(t.source) + " last set " + dur(w + m) + " ago" + in_ctx, t.observation,
                        false)) {
                return false;
            }
            finish(name(t.observation));
        } else {
            scene(inputs(t.observation) | ctx);
            const std::uint64_t g = gap(t);
            const std::uint64_t t0 = lead(t.source);
            std::vector<Edge> e;
            for (std::int32_t k = 0; k < t.times; ++k) { rise(t.source, t0 + static_cast<std::uint64_t>(k) * g, e); }
            play(e, ctx);
            const std::uint64_t last = t0 + static_cast<std::uint64_t>(t.times - 1) * g;
            // Read it once the base readings that drove it have cleared again, if the window allows, so the answer
            // shows the timed reading and not the readings it came from.
            std::uint64_t read_at = last + 1U;
            for (const Edge& x : pending) {
                if (!x.on) { read_at = std::max(read_at, x.at + 1U); }
            }
            if (read_at > t0 + w - m) { read_at = last + 1U; }
            if (!expect(read_at, tn + std::to_string(t.times) + " times " + name(t.source) + " within " +
                                     dur(last - t0) + ", read " + dur(read_at - t0) + " after the first" + in_ctx,
                        t.observation, true) ||
                !expect(t0 + w + m, tn + "the first of those " + dur(w + m) + " ago, so " +
                                        std::to_string(t.times - 1) + " in the window" + in_ctx,
                        t.observation, false)) {
                return false;
            }
            finish(name(t.observation));
        }
    }

    // Then the sheet's own lines, each after a quiet spell: they must come out exactly as the sheet says, which
    // checks the sheet's quick rule for what appears at once against the full replay. They come last because they
    // set readings that long windows would otherwise have to wait to forget.
    std::string got;
    const auto line = [&](std::uint32_t mask, const std::string& label, const std::string& want) {
        scene(mask);
        write(start, mask);
        buf << "set\t0\t0x" << std::hex << mask << std::dec << "\n";
        if (!expect(0, label, -1, false, &got)) { return false; }
        finish(label);
        if (got != want) {
            std::cerr << "error: internal: the sheet says " << want << " for '" << label << "' but replaying it gives "
                      << got << " (timed readings at once: " << names(immediate(mask) & timed_bits) << ")\n";
            return false;
        }
        return true;
    };
    for (const SheetRow& r : rows) {
        if (has(timed_bits, r.bit)) { continue; }  // a timed reading is driven by the scenes above
        if (!line(r.mask, name(r.bit) + " alone", r.alone)) { return false; }
        if (r.with != 0U) {
            if (!line(r.with | r.mask, name(r.bit) + " with " + r.with_names, r.then)) { return false; }
            if (!line(r.with, r.with_names + " without " + name(r.bit), r.without)) { return false; }
        }
    }
    for (const std::string& s : skipped) {
        std::cerr << "note: the bench script cannot drive '" << s << "' across its window on its own (the readings it "
                  << "is built from share inputs); its answers are left out of the bench test, so check it on the "
                  << "plant\n";
    }
    return true;
}

/// The commissioning sheet: what the device must do with each reading on its own, for a person to confirm on the real
/// plant. Exhaustive validation proves the logic over every combination; it cannot prove that the wire labelled
/// "overspeed" is the overspeed switch, that a threshold is set where the document says, or that the controller writes
/// the right bit. That is what a loop check is for, so this writes one line per reading -- set it alone, every other
/// reading at rest, and this is the action that must follow -- plus the at-rest line itself. Many readings change
/// nothing alone (a rule needs two of them), so each line also gives the fewest other readings with which this one
/// DOES change the decision, and the action with and without it. "-" if no set of up to three others does.
/// Tab-separated, so the studio and a spreadsheet can both read it:
///
///     quorum-commissioning 1
///     casebook <name> v<n>
///     fingerprint 0x........
///     actions  <name> ...          (in the order the device numbers them)
///     channels <count>
///     rest  <action>  <outcome>
///     row   <bit>  <name>  <how it is set>  <action alone>  <outcome alone>  <with: a+b, "alone" or ->
///           <action with them and this reading>  <action with them only>  <description>
[[nodiscard]] bool write_commissioning(const quorum::cbc::Lowered& lo, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write '" << path << "'\n";
        return false;
    }
    HostPolicy policy;
    if (make_policy(policy, lo) != Status::Ok) { return false; }
    const auto fw = std::make_unique<HostFramework>();
    const auto solver = std::make_unique<HostSolver>();
    auto decide = [&](std::uint32_t mask, std::string& action, std::string& outcome) {
        fw->reset();
        if (build(*fw, lo, mask) != Status::Ok) { return false; }
        const quorum::arbiter::Verdict v = quorum::arbiter::decide(*fw, *solver, policy);
        if (!v.action.valid()) { return false; }
        action = lo.arguments[static_cast<std::size_t>(v.action.get())].name;
        outcome = outcome_name(v.outcome);
        return true;
    };
    auto clean = [](std::string t) {
        for (char& c : t) {
            if (c == '\t' || c == '\n' || c == '\r') { c = ' '; }
        }
        return t;
    };
    auto milli = [](std::int32_t v) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", static_cast<double>(v) / 1000.0);
        return std::string(b);
    };
    // A timed rulebook: setting readings on the plant also sets the timed readings that react at once (a "within
    // last" reading of one of them, an "after" reading whose two sources appear together), so the sheet's answers
    // include those. Readings appear after a quiet spell. Without timed readings this is zero and nothing changes.
    std::uint32_t timed_bits = 0;
    for (const quorum::cbc::LoweredTimer& t : lo.timers) { timed_bits |= 1U << static_cast<std::uint32_t>(t.observation); }
    auto has = [](std::uint32_t m, std::int32_t o) { return o >= 0 && (m & (1U << static_cast<std::uint32_t>(o))) != 0U; };
    // In table order, so a timed reading watching another sees it: "within last" of a reading that is set is set,
    // "after" of two that appear together is set, and a count needs several rises, so never at once.
    auto immediate = [&](std::uint32_t m) {
        std::uint32_t r = m;
        for (const quorum::cbc::LoweredTimer& t : lo.timers) {
            const bool on = (t.kind == 1 && has(r, t.source)) || (t.kind == 0 && has(r, t.source) && has(r, t.after));
            if (on) { r |= 1U << static_cast<std::uint32_t>(t.observation); }
        }
        return r & timed_bits;
    };
    auto decide_plant = [&](std::uint32_t m, std::string& a, std::string& o) {
        return decide(m | immediate(m & ~timed_bits), a, o);
    };
    char fp[16];
    std::snprintf(fp, sizeof fp, "0x%08x", quorum::cbc::fingerprint(lo));
    out << "quorum-commissioning 1\ncasebook " << lo.casebook << " v" << lo.version << "\nfingerprint " << fp << "\n";
    out << "actions";  // in the order the device numbers them (Modbus reports an action as its position here)
    for (const std::int32_t a : lo.actions) { out << "\t" << lo.arguments[static_cast<std::size_t>(a)].name; }
    out << "\nchannels\t" << lo.channels.size() << "\n";
    std::string action;
    std::string outcome;
    if (!decide(0, action, outcome)) { return false; }
    out << "rest\t" << action << "\t" << outcome << "\n";
    std::vector<SheetRow> rows;
    for (std::int32_t b = 0; b < lo.observation_count; ++b) {
        const quorum::cbc::LoweredArgument& a = lo.arguments[static_cast<std::size_t>(b)];
        std::string how = "presence bit " + std::to_string(b);
        for (const quorum::cbc::LoweredThreshold& t : lo.thresholds) {
            if (t.observation != b) { continue; }
            const quorum::cbc::LoweredChannel& ch = lo.channels[static_cast<std::size_t>(t.channel)];
            how = "channel " + ch.name + (t.above ? " above " : " below ") + milli(t.on) + " " + ch.unit +
                  " (clears at " + milli(t.off) + ")";
            if (t.hold_ms > 0) { how += " for " + std::to_string(t.hold_ms / 1000U) + " s"; }
        }
        for (const quorum::cbc::LoweredChannel& ch : lo.channels) {
            if (ch.unavailable_obs == b) {
                how = "channel " + ch.name + " unavailable (out of range " + milli(ch.valid_lo) + " to " +
                      milli(ch.valid_hi) + " " + ch.unit + ", or no sample for " +
                      std::to_string(ch.stale_after_ms / 1000U) + " s)";
            }
            if (ch.flatline_obs == b) {
                how = "channel " + ch.name + " flatline (moves less than " + milli(ch.flatline_epsilon) + " " +
                      ch.unit + " in " + std::to_string(ch.flatline_after_ms / 60000U) + " min)";
            }
        }
        for (const quorum::cbc::LoweredTimer& t : lo.timers) {
            if (t.observation != b) { continue; }
            const auto dur = [](std::uint32_t ms) {
                if (ms % 3600000U == 0U) { return std::to_string(ms / 3600000U) + " h"; }
                if (ms % 60000U == 0U) { return std::to_string(ms / 60000U) + " min"; }
                if (ms % 1000U == 0U) { return std::to_string(ms / 1000U) + " s"; }
                return std::to_string(ms) + " ms";
            };
            const std::string src = lo.arguments[static_cast<std::size_t>(t.source)].name;
            // "timed:" is how the studio's bench test recognises a reading it cannot set by writing a bit.
            if (t.kind == 0) {
                how = "timed: " + src + " within " + dur(t.window_ms) + " after " +
                      lo.arguments[static_cast<std::size_t>(t.after)].name;
            } else if (t.kind == 1) {
                how = "timed: " + src + " within the last " + dur(t.window_ms);
            } else {
                how = "timed: " + src + " " + std::to_string(t.times) + " times in " + dur(t.window_ms);
            }
        }
        const std::uint32_t bit = 1U << static_cast<std::uint32_t>(b);
        if (!decide_plant(bit, action, outcome)) { return false; }
        // The smallest set of other readings with which this one changes the decision: setting it then shows, on the
        // real plant, that it arrives and matters. Up to three others, fewest first, then lowest bits first.
        std::string with = "-";
        std::string then_action = "-";
        std::string without_action = "-";
        const std::int32_t n = lo.observation_count;
        std::vector<std::int32_t> others;
        for (std::int32_t k = 0; k < n; ++k) {
            if (k != b && !has(timed_bits, k)) { others.push_back(k); }  // a timed reading cannot be set by hand
        }
        bool found = false;
        std::uint32_t with_mask = 0;
        for (std::int32_t size = 0; size <= 3 && !found && size <= static_cast<std::int32_t>(others.size()); ++size) {
            std::vector<std::int32_t> pick(static_cast<std::size_t>(size));
            for (std::int32_t i = 0; i < size; ++i) { pick[static_cast<std::size_t>(i)] = i; }
            while (!found) {
                std::uint32_t m = 0;
                for (const std::int32_t i : pick) {
                    m |= 1U << static_cast<std::uint32_t>(others[static_cast<std::size_t>(i)]);
                }
                std::string a1, o1, a0, o0;
                if (!decide_plant(m | bit, a1, o1) || !decide_plant(m, a0, o0)) { return false; }
                if (a1 != a0) {
                    found = true;
                    with_mask = m;
                    then_action = a1;
                    without_action = a0;
                    with.clear();
                    for (const std::int32_t i : pick) {
                        if (!with.empty()) { with += "+"; }
                        with += lo.arguments[static_cast<std::size_t>(others[static_cast<std::size_t>(i)])].name;
                    }
                    if (with.empty()) { with = "alone"; }
                    break;
                }
                // next combination of `size` out of others.size()
                std::int32_t i = size - 1;
                const auto total_others = static_cast<std::int32_t>(others.size());
                while (i >= 0 && pick[static_cast<std::size_t>(i)] == total_others - size + i) { --i; }
                if (i < 0) { break; }
                ++pick[static_cast<std::size_t>(i)];
                for (std::int32_t j = i + 1; j < size; ++j) {
                    pick[static_cast<std::size_t>(j)] = pick[static_cast<std::size_t>(j - 1)] + 1;
                }
            }
        }
        out << "row\t" << b << "\t" << a.name << "\t" << clean(how) << "\t" << action << "\t" << outcome << "\t"
            << with << "\t" << then_action << "\t" << without_action << "\t" << clean(a.description) << "\n";
        rows.push_back({b, bit, with == "alone" || with == "-" ? 0U : with_mask, with, action, then_action, without_action});
    }
    if (!lo.timers.empty() && !write_timed_script(lo, rows, decide, immediate, out)) { return false; }
    return out.good();
}

[[nodiscard]] bool write_certificate(const quorum::cbc::Lowered& lo, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write '" << path << "'\n";
        return false;
    }
    HostPolicy policy;
    if (make_policy(policy, lo) != Status::Ok) { return false; }
    char buf[64];
    std::snprintf(buf, sizeof buf, "0x%08x", quorum::cbc::fingerprint(lo));
    out << "quorum-certificate 2\n"
        << "casebook " << lo.casebook << " v" << lo.version << "\n"
        << "fingerprint " << buf << "\n"
        << "arguments " << lo.count() << "\n"
        << "observations " << lo.observation_count << "\n"
        << "capacity " << lo.max_args << " " << lo.max_attacks << "\n"
        << "closed_world " << lo.closed_world << "\n"
        << "edges " << lo.edges.size() << "\n";
    for (const quorum::cbc::LoweredEdge& e : lo.edges) { out << "e " << e.from << " " << e.to << "\n"; }
    out << "actions " << lo.actions.size();
    for (const std::int32_t a : lo.actions) { out << " " << a; }
    out << "\nfallback " << lo.safe_action << "\n"
        << "idle " << lo.idle_action << "\n";
    // The binding, so the checker can recompute the whole fingerprint. qcert hashes these numbers; it does not
    // check what they mean -- whether 90.0 is the right setpoint is a question for the rulebook's approver.
    out << "binding " << lo.channels.size() << " " << lo.thresholds.size() << "\n";
    for (const quorum::cbc::LoweredChannel& c : lo.channels) {
        out << "c " << c.valid_lo << " " << c.valid_hi << " " << c.stale_after_ms << " " << c.flatline_after_ms << " "
            << c.flatline_epsilon << " " << c.unavailable_obs << " " << c.flatline_obs << "\n";
    }
    for (const quorum::cbc::LoweredThreshold& t : lo.thresholds) {
        out << "t " << t.channel << " " << t.observation << " " << (t.above ? 1 : 0) << " " << t.on << " " << t.off << " "
            << t.hold_ms << "\n";
    }
    // Timed readings, only when there are any, so a certificate without them is byte-identical to before.
    if (!lo.timers.empty()) {
        out << "timers " << lo.timers.size() << "\n";
        for (const quorum::cbc::LoweredTimer& t : lo.timers) {
            out << "m " << t.kind << " " << t.observation << " " << t.source << " " << t.after << " " << t.window_ms << " "
                << t.times << "\n";
        }
    }

    const auto n = static_cast<std::size_t>(lo.count());
    std::vector<std::vector<std::int32_t>> attackers(n);
    for (const quorum::cbc::LoweredEdge& e : lo.edges) { attackers[static_cast<std::size_t>(e.to)].push_back(e.from); }
    static HostFramework fw;
    static HostSolver solver;
    const std::uint32_t total = 1U << static_cast<std::uint32_t>(lo.observation_count);
    out << "states " << total << "\n";
    std::vector<int> label(n);
    std::vector<int> round(n);
    for (std::uint32_t mask = 0; mask < total; ++mask) {
        std::fill(label.begin(), label.end(), 0);
        std::fill(round.begin(), round.end(), 0);
        auto attacked_by_cw = [&](std::size_t a) {
            return static_cast<std::int32_t>(a) < lo.observation_count && (mask >> a & 1U) == 0U;
        };
        // Synchronous rounds: decide round r from labels of rounds < r only.
        for (int r = 1;; ++r) {
            std::vector<std::pair<std::size_t, int>> fresh;
            for (std::size_t a = 0; a < n; ++a) {
                if (label[a] != 0) { continue; }
                bool all_out = true;
                bool some_in = false;
                auto see = [&](std::size_t b) {
                    if (!(label[b] == -1 && round[b] < r)) { all_out = false; }
                    if (label[b] == 1 && round[b] < r) { some_in = true; }
                };
                for (const std::int32_t b : attackers[a]) { see(static_cast<std::size_t>(b)); }
                if (attacked_by_cw(a)) { see(static_cast<std::size_t>(lo.closed_world)); }
                if (all_out) { fresh.emplace_back(a, 1); }
                else if (some_in) { fresh.emplace_back(a, -1); }
            }
            if (fresh.empty()) { break; }
            for (const auto& [a, l] : fresh) {
                label[a] = l;
                round[a] = r;
            }
        }
        fw.reset();
        if (build(fw, lo, mask) != Status::Ok || solver.solve(fw) != Status::Ok) { return false; }
        for (std::size_t a = 0; a < n; ++a) {
            const Label l = solver.label(Arg{static_cast<std::int32_t>(a)});
            const int want = l == Label::In ? 1 : (l == Label::Out ? -1 : 0);
            if (want != label[a]) {
                std::cerr << "error: certificate labelling disagrees with the device solver in state " << mask
                          << " at '" << lo.arguments[a].name << "'; no certificate written\n";
                return false;
            }
        }
        const quorum::arbiter::Verdict v = quorum::arbiter::decide(fw, solver, policy);
        out << "s " << mask << " " << static_cast<int>(v.outcome) << " " << v.action.get();
        for (std::size_t a = 0; a < n; ++a) {
            out << ' ' << (label[a] == 1 ? 'I' : (label[a] == -1 ? 'O' : 'U'));
            if (label[a] != 0) { out << round[a]; }
        }
        out << "\n";
    }
    out << "end\n";
    return out.good();
}

/// Symbolic validation (pass 5s): every state at once. Returns false if any invariant is violated.
///
/// For a single casebook `policies` has one entry; for a palette, one per member, all over the merged framework, and
/// then non-interference is checked by asking which presence bits each member's outcome depends on -- over the WHOLE
/// combined space, not a sample of it.
[[nodiscard]] bool validate_symbolic(const quorum::cbc::Lowered& lo, const std::vector<quorum::cbc::SymbolicPolicy>& pols,
                                     const std::vector<std::pair<std::int32_t, std::int32_t>>& own_bits, bool quiet) {
    const quorum::cbc::SymbolicCheck sym(lo, pols);
    bool ok = true;
    if (!quiet) {
        std::printf("\nsymbolic validation over all %s states at once (%d rounds, %zu BDD nodes, %.3f s)\n",
                    sym.states().c_str(), sym.rounds(), sym.nodes(), sym.seconds());
    }
    for (std::size_t p = 0; p < pols.size(); ++p) {
        if (!quiet) {
            if (pols.size() > 1) { std::printf("  %s\n", pols[p].name.c_str()); }
            for (int o = 0; o < 4; ++o) {
                std::printf("    %-14s %s\n", outcome_name(static_cast<Outcome>(o)), sym.count(p, o).c_str());
            }
        }
        if (!sym.empty(p, static_cast<int>(Outcome::Conflicted))) {
            std::printf("  FAIL  %s: %s states select two actions at once\n", pols[p].name.c_str(),
                        sym.count(p, static_cast<int>(Outcome::Conflicted)).c_str());
            ok = false;
        }
        if (!sym.empty(p, static_cast<int>(Outcome::Unsatisfied))) {
            std::printf("  FAIL  %s: %s states reject every action\n", pols[p].name.c_str(),
                        sym.count(p, static_cast<int>(Outcome::Unsatisfied)).c_str());
            ok = false;
        }
        for (const std::int32_t a : pols[p].actions) {
            const std::string n = sym.selected(p, a);
            if (!quiet) { std::printf("    selects %-22s %s\n", lo.arguments[static_cast<std::size_t>(a)].name.c_str(), n.c_str()); }
            if (n == "0") {
                std::printf("  FAIL  action '%s' is selected in no state\n",
                            lo.arguments[static_cast<std::size_t>(a)].name.c_str());
                ok = false;
            }
        }
        if (p < own_bits.size()) {
            const std::vector<bool> dep = sym.depends_on(p);
            std::int32_t foreign = 0;
            for (std::size_t bit = 0; bit < dep.size(); ++bit) {
                const auto b = static_cast<std::int32_t>(bit);
                if (dep[bit] && (b < own_bits[p].first || b >= own_bits[p].first + own_bits[p].second)) { ++foreign; }
            }
            if (foreign > 0) {
                std::printf("  FAIL  %s: its outcome depends on %d readings belonging to other casebooks\n",
                            pols[p].name.c_str(), foreign);
                ok = false;
            } else if (!quiet) {
                std::printf("    PROVED  its outcome depends on no other casebook's readings, in every combined state\n");
            }
        }
    }
    if (sym.undecided_doubt_possible()) {
        std::printf("  FAIL  a doubt is Undecided in %s states (an odd cycle survived lowering)\n",
                    sym.undecided_doubt_states().c_str());
        ok = false;
    }
    if (!quiet) {
        std::printf("  %s  symbolic invariants%s\n", ok ? "OK  " : "FAIL",
                    ok ? ": no conflict, nothing unsatisfied, every action reachable, no undecided doubt" : "");
        std::printf("  NOTE  this pass does not compute per-state work; the timing bound and the worst-case state\n"
                    "        come from exhaustive validation, which device images still require.\n");
    }
    return ok;
}

void usage() {
    std::cerr << "usage: cbc <input.cb> [more.cb ... --palette] [options]\n"
                 "  --palette         merge several casebooks onto one device\n"
                 "  --header <path>   emit the device C++ header\n"
                 "  --af <path>       emit ICCMA .af interchange\n"
                 "  --apx <path>      emit ASPARTIX apx interchange\n"
                 "  --blob <path>     emit the data image for the prebuilt runtime firmware (needs validation)\n"
                 "  --no-validate     skip exhaustive validation\n"
                 "  --certificate <p> write a proof certificate of every state, checkable by tools/qcert alone\n"
                 "  --commissioning <p> write the loop-check sheet: the action each reading must cause on its own\n"
                 "  --symbolic        also validate every state at once with decision diagrams (fast for large\n"
                 "                    casebooks and whole palettes; proves palette non-interference)\n"
                 "  --quiet           only report problems\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << what << " needs a path\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--header") {
            opt.header_out = next("--header");
        } else if (a == "--af") {
            opt.af_out = next("--af");
        } else if (a == "--blob") {
            opt.blob_out = next("--blob");
        } else if (a == "--apx") {
            opt.apx_out = next("--apx");
        } else if (a == "--palette") {
            opt.palette = true;
        } else if (a == "--no-validate") {
            opt.validate = false;
        } else if (a == "--certificate") {
            opt.certificate_out = next("--certificate");
        } else if (a == "--commissioning") {
            opt.commissioning_out = next("--commissioning");
        } else if (a == "--symbolic") {
            opt.symbolic = true;
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n";
            usage();
            return 2;
        } else {
            opt.inputs.push_back(a);
        }
    }

    if (opt.inputs.empty()) {
        usage();
        return 2;
    }
    if (opt.inputs.size() > 1 && !opt.palette) {
        std::cerr << "error: several input files need --palette, which merges them onto one device\n";
        return 2;
    }

    // Compile every input independently first. A palette is a union of separately valid casebooks, and compiling
    // them together would blur which file an error came from.
    std::vector<quorum::cbc::Casebook> books;
    std::vector<quorum::cbc::Lowered> lowered;
    int errors = 0;

    for (const std::string& path : opt.inputs) {
        std::string text;
        if (!read_file(path, text)) {
            std::cerr << "error: cannot read '" << path << "'\n";
            return 2;
        }

        quorum::cbc::Diagnostics diag;
        quorum::cbc::Casebook cb = quorum::cbc::parse(text, diag);
        quorum::cbc::Lowered lo;
        if (!diag.has_errors()) {
            // Expand `vote k of ...` before lowering, so the generated combination claims resolve like any others.
            quorum::cbc::desugar_votes(cb, diag);
        }
        if (!diag.has_errors()) {
            lo = quorum::cbc::lower(cb, diag);
            if (!diag.has_errors()) { quorum::cbc::check(lo, diag); }
        }
        for (const quorum::cbc::Diagnostic& d : diag.items()) {
            const bool is_error = d.severity == quorum::cbc::Diagnostic::Severity::Error;
            if (is_error) { ++errors; }
            std::cerr << path;
            if (d.line > 0) { std::cerr << ":" << d.line; }
            std::cerr << ": " << (is_error ? "error: " : "warning: ") << d.message << "\n";
        }
        books.push_back(cb);
        lowered.push_back(std::move(lo));
    }

    if (errors > 0) {
        std::cerr << errors << " error" << (errors == 1 ? "" : "s") << "\n";
        return 1;
    }

    const quorum::cbc::Lowered& lo = lowered.front();

    // Every member is validated exhaustively on its own, palette or not. That is the result the palette checks
    // then show carries over unchanged.
    for (std::size_t i = 0; i < lowered.size(); ++i) {
        if (!opt.quiet) { std::cout << quorum::cbc::emit_report(books[i], lowered[i]); }
        if (opt.validate && !validate(lowered[i], opt.quiet)) {
            std::cerr << "\nvalidation failed for " << opt.inputs[i] << "; no output written\n";
            return 1;
        }
        if (opt.symbolic && !validate_symbolic(lowered[i], {{lowered[i].casebook, lowered[i].actions}}, {}, opt.quiet)) {
            std::cerr << "\nsymbolic validation failed for " << opt.inputs[i] << "; no output written\n";
            return 1;
        }
    }

    if (opt.palette && !opt.commissioning_out.empty()) {
        std::cerr << "error: --commissioning covers one casebook; write a sheet for each member\n";
        return 2;
    }
    if (opt.palette && !opt.certificate_out.empty()) {
        std::cerr << "error: --certificate covers one casebook; certify each member separately (the palette checks "
                     "show the merge preserves each exactly)\n";
        return 2;
    }
    if (opt.palette) {
        quorum::cbc::Diagnostics pdiag;
        const quorum::cbc::Palette palette = quorum::cbc::merge(lowered, pdiag);
        for (const quorum::cbc::Diagnostic& d : pdiag.items()) {
            std::cerr << "palette: " << d.message << "\n";
        }
        if (pdiag.has_errors()) { return 1; }

        if (opt.validate && !validate_palette(palette, lowered, opt.quiet)) {
            std::cerr << "\npalette validation failed; no output written\n";
            return 1;
        }
        if (opt.symbolic) {
            std::vector<quorum::cbc::SymbolicPolicy> pols;
            std::vector<std::pair<std::int32_t, std::int32_t>> bits;
            for (const quorum::cbc::PaletteMember& m : palette.members) {
                pols.push_back({m.casebook, m.actions});
                bits.emplace_back(m.observation_offset, m.observation_count);
            }
            if (!validate_symbolic(palette.all, pols, bits, opt.quiet)) {
                std::cerr << "\nsymbolic palette validation failed; no output written\n";
                return 1;
            }
        }
        if (!opt.blob_out.empty()) {
            if (!opt.validate) {
                std::cerr << "error: --blob refuses --no-validate: a device image must come from a validated casebook\n";
                return 2;
            }
            if (!write_binary(opt.blob_out, quorum::cbc::emit_palette_blob(palette))) {
                std::cerr << "error: cannot write '" << opt.blob_out << "'\n";
                return 2;
            }
        }
        if (!opt.header_out.empty() &&
            !write_file(opt.header_out, quorum::cbc::emit_palette_header(palette))) {
            std::cerr << "error: cannot write '" << opt.header_out << "'\n";
            return 2;
        }
        if (!opt.af_out.empty() && !write_file(opt.af_out, quorum::cbc::emit_af(palette.all))) {
            std::cerr << "error: cannot write '" << opt.af_out << "'\n";
            return 2;
        }
        if (!opt.apx_out.empty() && !write_file(opt.apx_out, quorum::cbc::emit_apx(palette.all))) {
            std::cerr << "error: cannot write '" << opt.apx_out << "'\n";
            return 2;
        }
        return 0;
    }

    if (!opt.certificate_out.empty()) {
        if (!opt.validate) {
            std::cerr << "error: --certificate refuses --no-validate: it certifies a validated casebook\n";
            return 2;
        }
        if (!write_certificate(lo, opt.certificate_out)) { return 2; }
    }

    if (!opt.commissioning_out.empty()) {
        if (!opt.validate) {
            std::cerr << "error: --commissioning refuses --no-validate: the sheet is for a validated casebook\n";
            return 2;
        }
        if (!write_commissioning(lo, opt.commissioning_out)) { return 2; }
    }

    if (!opt.blob_out.empty()) {
        // The runtime firmware trusts a blob the way header firmware trusts its header: the fingerprint proves the
        // tables are the ones cbc emitted, not that anyone validated them. So an unvalidated blob is never written.
        if (!opt.validate) {
            std::cerr << "error: --blob refuses --no-validate: a device image must come from a validated casebook\n";
            return 2;
        }
        if (!write_binary(opt.blob_out, quorum::cbc::emit_blob(lo))) {
            std::cerr << "error: cannot write '" << opt.blob_out << "'\n";
            return 2;
        }
    }
    if (!opt.header_out.empty() && !write_file(opt.header_out, quorum::cbc::emit_header(lo))) {
        std::cerr << "error: cannot write '" << opt.header_out << "'\n";
        return 2;
    }
    if (!opt.af_out.empty() && !write_file(opt.af_out, quorum::cbc::emit_af(lo))) {
        std::cerr << "error: cannot write '" << opt.af_out << "'\n";
        return 2;
    }
    if (!opt.apx_out.empty() && !write_file(opt.apx_out, quorum::cbc::emit_apx(lo))) {
        std::cerr << "error: cannot write '" << opt.apx_out << "'\n";
        return 2;
    }

    return 0;
}
