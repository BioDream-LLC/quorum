#pragma once

// What a human is told when the machine will not decide.
//
// This is the product's most important output path and it was the least developed. `Indeterminate` is the correct
// verdict when a plant is genuinely ambiguous, but as a thing to show an operator it is nearly useless -- it says the
// machine gave up without saying what about. The operator is then in exactly the position the product exists to
// prevent: staring at conflicting instruments with no structure to the conflict.
//
// The advisory tier can do better, because it can enumerate the competing readings exactly. So instead of
//
//     INDETERMINATE
//
// this renders
//
//     Two coherent readings of this plant, and they disagree on what to do.
//       Reading A -> maintain    because it accepts misalignment and rejects sensor_drift
//       Reading B -> shutdown    because it accepts sensor_drift and rejects misalignment
//     Check `vib_2x`: asserting it resolves the ambiguity.
//
// The last line is the part that matters. A disagreement an operator cannot act on is barely better than silence;
// naming the single observation that would settle it turns "I don't know" into a work instruction.
//
// HOST ONLY. Allocates, and depends on the advisory tier.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "quorum/advisory/semantics.hpp"

namespace quorum::advisory {

/// One self-consistent reading of the plant: a preferred extension, described by what makes it different from its
/// rivals rather than by its full contents. An extension of 20 arguments where 18 are common ground is not 20 facts an
/// operator needs; it is 2.
struct Reading {
    /// The action this reading selects, or `-1` if it selects none.
    std::int32_t action = -1;
    /// Arguments this reading accepts that at least one rival does not.
    std::vector<std::int32_t> distinctive_accepts;
    /// Arguments this reading rejects that at least one rival accepts.
    std::vector<std::int32_t> distinctive_rejects;
};

struct Explanation {
    /// True when the readings disagree about what to DO. Readings can differ on arguments while agreeing on the action,
    /// and that case is not worth an operator's attention -- the disagreement is immaterial.
    bool actions_disagree = false;

    std::vector<Reading> readings;

    /// Arguments on which the readings differ. The substance of the disagreement.
    std::vector<std::int32_t> contested;

    /// Observations which, if asserted, collapse the disagreement to a single action. The actionable output: go and
    /// measure one of these.
    std::vector<std::int32_t> discriminators;

    /// True when no single observation resolves it, so the ambiguity needs more than one new fact.
    [[nodiscard]] bool needs_more_than_one_check() const { return actions_disagree && discriminators.empty(); }
};

/// Build an explanation from an analysis.
///
/// `is_authored` comes from the generated casebook header and excludes the compiler's encoding machinery -- gates and
/// the closed-world argument. Those have no referent in the plant, so naming them to an operator is worse than saying
/// nothing: it invites someone to go and look for a thing that does not exist.
///
/// Actions are excluded too, but for a different reason: a reading already states its action as its conclusion, so
/// repeating it under "rejects" is noise.
template <typename IsAuthored>
[[nodiscard]] inline Explanation explain(const Analysis& a, const std::int32_t* actions, std::int32_t action_count,
                                         std::int32_t argument_count, IsAuthored is_authored) {
    Explanation e;
    if (a.status != Status::Ok || a.preferred.size() < 2) { return e; }

    for (const Extension& ext : a.preferred) {
        Reading r;
        for (std::int32_t i = 0; i < action_count; ++i) {
            if (ext.contains(Arg{actions[i]})) {
                r.action = actions[i];
                break;
            }
        }
        e.readings.push_back(std::move(r));
    }

    // Do the readings actually disagree about what to do?
    for (std::size_t i = 1; i < e.readings.size(); ++i) {
        if (e.readings[i].action != e.readings[0].action) { e.actions_disagree = true; }
    }

    // Which arguments differ across readings? Only these are worth naming, and only if a human could act on them.
    for (std::int32_t arg = 0; arg < argument_count; ++arg) {
        if (!is_authored(arg)) { continue; }
        bool is_action = false;
        for (std::int32_t i = 0; i < action_count; ++i) {
            if (actions[i] == arg) { is_action = true; }
        }
        if (is_action) { continue; }

        bool any = false;
        bool all = true;
        for (const Extension& ext : a.preferred) {
            if (ext.contains(Arg{arg})) {
                any = true;
            } else {
                all = false;
            }
        }
        if (any && !all) { e.contested.push_back(arg); }
    }

    for (std::size_t i = 0; i < a.preferred.size(); ++i) {
        for (const std::int32_t arg : e.contested) {
            if (a.preferred[i].contains(Arg{arg})) {
                e.readings[i].distinctive_accepts.push_back(arg);
            } else {
                e.readings[i].distinctive_rejects.push_back(arg);
            }
        }
    }

    return e;
}

/// Find the observations which, asserted one at a time, would collapse the disagreement.
///
/// Brute force over the unasserted observations, re-solving each. That is `observations` extra analyses, which is
/// nothing on a host and impossible on the device -- which is a clean illustration of what the two tiers are actually
/// for: the device decides, the host explains.
template <std::int32_t MA, std::int32_t MT, std::int32_t SA, typename Instantiate, typename IsAuthored>
inline void find_discriminators(Explanation& e, solver::Framework<MA, MT>& fw, solver::Grounded<SA>& solver,
                                std::uint32_t current_mask, std::int32_t observation_count,
                                const std::int32_t* actions, std::int32_t action_count, Instantiate instantiate,
                                IsAuthored is_authored) {
    if (!e.actions_disagree) { return; }

    for (std::int32_t bit = 0; bit < observation_count; ++bit) {
        const std::uint32_t probe = 1U << static_cast<std::uint32_t>(bit);
        if ((current_mask & probe) != 0U) { continue; }  // already asserted; nothing to go and check

        if (instantiate(fw, current_mask | probe) != solver::Status::Ok) { continue; }
        const Analysis probed = analyse(fw, solver);
        if (probed.status != Status::Ok) { continue; }

        const Explanation pe = explain(probed, actions, action_count, fw.argument_count(), is_authored);
        // Resolved means: either one reading remains, or the remaining readings agree on the action.
        if (!pe.actions_disagree) { e.discriminators.push_back(bit); }
    }
}

/// Render an explanation in the author's vocabulary.
///
/// `name_of` must return the name the casebook author wrote, not an escaped identifier -- an operator reading this has
/// to recognise their own plant.
template <typename NameOf>
[[nodiscard]] inline std::string render(const Explanation& e, NameOf name_of) {
    std::string out;
    if (e.readings.size() < 2) { return out; }

    if (!e.actions_disagree) {
        out += "AMBIGUOUS, but immaterially: there are ";
        out += std::to_string(e.readings.size());
        out += " coherent readings of this plant and they all imply the same action (";
        out += e.readings[0].action >= 0 ? name_of(e.readings[0].action) : "no action";
        out += "). No decision is blocked.\n";
        return out;
    }

    out += "AMBIGUOUS: ";
    out += std::to_string(e.readings.size());
    out += " coherent readings of this plant, and they disagree on what to do.\n\n";

    char tag = 'A';
    for (const Reading& r : e.readings) {
        out += "  Reading ";
        out += tag;
        out += " -> ";
        out += r.action >= 0 ? name_of(r.action) : "no action selected";
        out += "\n";
        if (!r.distinctive_accepts.empty()) {
            out += "      accepts: ";
            for (std::size_t i = 0; i < r.distinctive_accepts.size(); ++i) {
                if (i != 0) { out += ", "; }
                out += name_of(r.distinctive_accepts[i]);
            }
            out += "\n";
        }
        if (!r.distinctive_rejects.empty()) {
            out += "      rejects: ";
            for (std::size_t i = 0; i < r.distinctive_rejects.size(); ++i) {
                if (i != 0) { out += ", "; }
                out += name_of(r.distinctive_rejects[i]);
            }
            out += "\n";
        }
        ++tag;
    }

    out += "\n";
    if (!e.discriminators.empty()) {
        out += "  TO RESOLVE, check any one of: ";
        for (std::size_t i = 0; i < e.discriminators.size(); ++i) {
            if (i != 0) { out += ", "; }
            out += name_of(e.discriminators[i]);
        }
        out += "\n";
    } else {
        // Honest about the harder case rather than silent. A single measurement is not always enough, and saying so is
        // more useful than implying one exists.
        out += "  No single additional observation resolves this. It needs more than one new fact,\n";
        out += "  or a judgement the casebook does not encode.\n";
    }
    return out;
}

}  // namespace quorum::advisory
