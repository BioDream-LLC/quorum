#pragma once

#include <cstdint>
#include <cstdio>

#include "quorum/solver/ids.hpp"
#include "quorum/witness/derivation.hpp"

/// \file
/// Prose rendering of a derivation. **HOST ONLY.**
///
/// This header uses `<cstdio>` and is deliberately excluded from the freestanding gate. The split is the point: a
/// device emits structure -- a labelling, a casebook version, a `Derivation` -- and prose is produced where prose
/// is cheap. Grounded labelling is deterministic, so an off-device tool can reconstruct and render a byte-identical
/// explanation from the record alone.
///
/// Rendering discipline borrowed from the Kotlin line, where `ProofForest.explain` is *"deliberately byte-identical
/// to Proof.explain, and a test asserts it"*, on the grounds that *"a store that changed how a derivation reads
/// would make every recorded explanation in this project unverifiable against it."* If a second renderer ever
/// exists here, it must agree with this one and a test must say so.
///
/// Everything renders in **external one-based numbering** (`FileArg`), never internal `Arg`. A witness is read by
/// people against a casebook document, and the casebook numbers its arguments from one.

namespace quorum::witness {

/// Resolves an argument to the name a human wrote in the casebook.
using NameLookup = const char* (*)(std::int32_t internal_index);

[[nodiscard]] inline constexpr const char* label_word(solver::Label l) noexcept {
    switch (l) {
        case solver::Label::In:        return "holds";
        case solver::Label::Out:       return "defeated";
        case solver::Label::Undecided: return "undetermined";
    }
    return "?";
}

/// Write one derivation as an indented explanation.
///
/// The `ASSUMED` marker is in capitals for the reason `Solver.kt` gives: a derivation that cannot distinguish an
/// assumption from a fact is more persuasive than a guess and therefore worse than one. Here the assumption is
/// always the same one -- closed-world reasoning over a silent sensor -- and it is the difference between "no
/// vibration was reported" and "there is no vibration".
template <std::int32_t MaxSteps, std::int32_t MaxArgs>
void render(const Derivation<MaxSteps, MaxArgs>& d, NameLookup name) {
    for (std::int32_t i = 0; i < d.step_count(); ++i) {
        const Step& s = d.step(i);

        for (std::int32_t p = 0; p < s.depth; ++p) { std::printf("  "); }

        std::printf("%s (#%d) %s",
                    name(s.subject.get()),
                    solver::to_file(s.subject).get(),
                    label_word(s.label));

        switch (s.ground) {
            case Ground::Unattacked:
                std::printf(" -- nothing disputes it\n");
                break;
            case Ground::AllAttackersOut:
                std::printf(" -- everything that disputes it is defeated:\n");
                break;
            case Ground::DefeatedBy:
                if (s.assumed) {
                    std::printf(" -- ASSUMED ABSENT: no sensor asserted it. This is absence of evidence, "
                                "not evidence of absence; a dead sensor reads the same way.\n");
                } else {
                    std::printf(" -- defeated by %s (#%d):\n",
                                name(s.because.get()),
                                solver::to_file(s.because).get());
                }
                break;
            case Ground::Contested:
                std::printf(" -- contested, and nothing settles it:\n");
                break;
            case Ground::AlreadyShown:
                std::printf(" -- as established above\n");
                break;
        }
    }

    std::printf("\n  %d steps, depth %d", d.step_count(), d.depth());
    if (d.unconditional()) {
        std::printf(", resting on asserted readings alone\n");
    } else {
        std::printf(", of which %d rest on ASSUMED ABSENCE of a reading\n", d.assumed_steps());
    }
    if (d.truncated()) {
        std::printf("  WARNING: derivation truncated; this explanation is incomplete\n");
    }
}

/// The one-line summary a control system logs every cycle, as opposed to the full derivation it logs on demand.
template <std::int32_t MaxSteps, std::int32_t MaxArgs>
void render_summary(const Derivation<MaxSteps, MaxArgs>& d, NameLookup name) {
    if (d.step_count() == 0) {
        std::printf("(no derivation)\n");
        return;
    }
    const Step& root = d.step(0);
    std::printf("%s %s", name(root.subject.get()), label_word(root.label));
    if (root.ground == Ground::DefeatedBy && !root.assumed) {
        std::printf(" (by %s)", name(root.because.get()));
    }
    std::printf("; depth %d; %s\n",
                d.depth(),
                d.unconditional() ? "unconditional" : "CONDITIONAL on assumed absence");
}

}  // namespace quorum::witness
