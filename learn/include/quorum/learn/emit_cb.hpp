/// \file
/// A learned casebook, written out as an ordinary `.cb` source file. HOST ONLY.
///
/// This is the point where learning stops. Everything downstream -- `cbc`'s resolve, lower, check, exhaustive
/// validation over every input state, the proven step bound, the device header -- is the same compiler that checks
/// a hand-written casebook, and it neither knows nor cares that a learner wrote this one. The output is text a
/// reviewer reads and may edit before anything is compiled.
///
/// ## The translation, and why it is exact
///
/// AA-CBR's framework for a query has a default argument, one argument per past case, and the new case. This file
/// maps each piece onto something the casebook language already has:
///
/// | AA-CBR                                         | casebook                                              |
/// |------------------------------------------------|-------------------------------------------------------|
/// | a feature                                      | `observe`                                             |
/// | case with the non-default outcome              | `claim ... needs <its features>`                      |
/// | case with the default outcome                  | `excuse ... needs <its features>`                     |
/// | new case attacks a case it lacks a feature of  | the `needs` gate: absent reading, gate In, case Out   |
/// | case attacks case (specificity, concision)     | `rebuts`                                              |
/// | prediction is non-default iff default is Out   | `action <assert> safe when any <default's attackers>` |
///
/// The default argument itself is not emitted. Its attackers are exactly the cases that attack it; it is Out iff one
/// of them is In; so "default Out" is the disjunction of its attackers, which is what `when any` means. Whether the
/// compiled casebook really decides as the learner does is not argued here -- `test_learned_casebook` checks it on
/// every input state.
///
/// No `exclusive` groups are emitted even for one-hot encodings, because cbc turns an exclusive group into an
/// instrument-fault doubt, which would change decisions on states the learner never saw. Equivalence is over ALL
/// states, so the translation adds nothing the learner did not decide.
#ifndef QUORUM_LEARN_EMIT_CB_HPP
#define QUORUM_LEARN_EMIT_CB_HPP

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "quorum/learn/aacbr.hpp"

namespace quorum {
namespace learn {

struct CasebookText {
    std::string source;
    /// observation index (bit in the device mask) -> feature bit in the learner's encoding
    std::vector<int> observation_feature;
    std::string assert_action;
    std::string idle_action;
    /// Cases left out because no chain of attacks connects them to the default, so they cannot change any decision.
    std::int32_t spikes_pruned = 0;
};

struct EmitOptions {
    std::string name;           ///< casebook identifier
    std::string document;       ///< provenance: what it was learned from
    std::string dated;          ///< YYYY-MM-DD
    std::string by;             ///< who or what produced it
    std::vector<std::string> notes;
    const char* const* feature_names = nullptr;  ///< 32 entries, valid casebook identifiers
    std::string asserts;        ///< name of the non-default outcome, e.g. "failure"
    std::string otherwise;      ///< name of the default outcome, e.g. "normal"
};

/// Write `model`, restricted to observations in `mask`, as casebook source. `model` must have been fitted on
/// features inside `mask`; a case with a feature outside it cannot be expressed and is reported by throwing.
inline CasebookText emit_casebook(const AaCbr& model, Features mask, const EmitOptions& opt) {
    CasebookText out;
    std::ostringstream cb;
    cb << "casebook " << opt.name << " v1\n";
    cb << "source \"" << opt.document << "\" dated " << opt.dated << " by \"" << opt.by << "\"\n";
    for (const std::string& n : opt.notes) { cb << "note \"" << n << "\"\n"; }
    cb << "note \"LEARNED, not written: AA-CBR over quorum's grounded solver. Review before compiling.\"\n";

    cb << "\n# --- observations: the device presence mask, in this order ---------------------\n";
    for (int f = 0; f < 32; ++f) {
        if (((mask >> f) & 1U) == 0U) { continue; }
        out.observation_feature.push_back(f);
        cb << "observe " << opt.feature_names[f] << " \"" << opt.feature_names[f] << "\"\n";
    }
    if (out.observation_feature.size() > 30U) {
        throw std::length_error("emit_casebook: a device casebook carries at most 30 observations");
    }

    const std::vector<Case>& cases = model.cases();
    const bool delta = model.default_outcome();
    auto case_name = [&](std::int32_t i) {
        return std::string(model.is_prior(i) ? "rule_" : "case_") + std::to_string(i);
    };
    auto features_of = [&](Features x) {
        std::string s;
        for (int f = 0; f < 32; ++f) {
            if (((x >> f) & 1U) != 0U) { s += std::string(" ") + opt.feature_names[f]; }
        }
        return s;
    };

    // A case with no features and the DEFAULT outcome attacks nothing (nothing is strictly less specific than the
    // empty set) and lies strictly between no pair, so it can change no decision -- a "spike" in Paulino-Passos &
    // Toni's terms (KR 2021, Theorem 20). It is omitted, with every edge into it. A featureless case with the OTHER
    // outcome is unconditional; it is emitted as a claim with no evidence.
    //
    // More generally, any case with no path of attacks to the default is a spike (ibid., Definition 19): whatever its
    // label, the default's cannot depend on it. cbc's validator reports such arguments as "cannot reach any action".
    // They are pruned here rather than shipped, because on a device every argument costs RAM and solve time, and a
    // reviewer should not have to read cases that do nothing. `test_learned_casebook` re-checks every state after
    // pruning, so this is verified rather than assumed.
    const std::int32_t n = static_cast<std::int32_t>(cases.size());
    std::vector<bool> reaches(static_cast<std::size_t>(n) + 1U, false);
    reaches[0] = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& e : model.base_attacks()) {
            if (reaches[static_cast<std::size_t>(e.second)] && !reaches[static_cast<std::size_t>(e.first)]) {
                reaches[static_cast<std::size_t>(e.first)] = true;
                grew = true;
            }
        }
    }
    auto omitted = [&](std::int32_t i) {
        const Case& c = cases[static_cast<std::size_t>(i - 1)];
        return (c.x == 0U && c.outcome == delta) || !reaches[static_cast<std::size_t>(i)];
    };
    for (std::int32_t i = 1; i <= n; ++i) { out.spikes_pruned += omitted(i) ? 1 : 0; }

    cb << "\n# --- cases: rule_ from the reviewed casebook, case_ learned from records -------\n";
    for (std::int32_t i = 1; i <= static_cast<std::int32_t>(cases.size()); ++i) {
        const Case& c = cases[static_cast<std::size_t>(i - 1)];
        if ((c.x & ~mask) != 0U) { throw std::invalid_argument("emit_casebook: a case uses a feature outside the mask"); }
        if (omitted(i)) { continue; }
        const bool asserting = c.outcome != delta;
        cb << (asserting ? "claim  " : "excuse ") << case_name(i) << " \""
           << (model.is_prior(i) ? "reviewed rule: " : "learned: ") << (c.outcome ? opt.asserts : opt.otherwise)
           << (c.x == 0U ? " always" : " when" + features_of(c.x)) << "\"";
        if (c.x != 0U) { cb << " needs" << features_of(c.x); }
        cb << "\n";
    }

    cb << "\n# --- attacks: a more specific case with the other outcome ---------------------\n";
    std::vector<std::int32_t> default_attackers;
    for (const auto& e : model.base_attacks()) {
        if (e.second == 0) {
            default_attackers.push_back(e.first);
            continue;
        }
        if (omitted(e.second) || omitted(e.first)) { continue; }
        cb << case_name(e.first) << " rebuts " << case_name(e.second) << "\n";
    }

    // Name actions by the outcome they announce. The default outcome is the idle action: nothing fired.
    out.assert_action = (delta ? opt.otherwise : opt.asserts);
    out.idle_action = (delta ? opt.asserts : opt.otherwise);
    cb << "\n# --- decision: the default stands unless one of its attackers is In -----------\n";
    cb << "action " << out.idle_action << " idle\n";
    if (default_attackers.empty()) {
        throw std::invalid_argument("emit_casebook: the model never departs from its default; nothing to compile");
    }
    cb << "action " << out.assert_action << " safe when" << (default_attackers.size() > 1U ? " any" : "");
    for (std::int32_t a : default_attackers) { cb << " " << case_name(a); }
    cb << "\n";
    cb << "prefer " << out.assert_action << " over " << out.idle_action << "\n";
    out.source = cb.str();
    return out;
}

}  // namespace learn
}  // namespace quorum

#endif  // QUORUM_LEARN_EMIT_CB_HPP
