#include "quorum/cbc/cbc.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace quorum::cbc {

std::int32_t Lowered::index_of(const std::string& name) const {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i].name == name) { return static_cast<std::int32_t>(i); }
    }
    return -1;
}

namespace {

/// Everything the lowering pass needs to know about a name, resolved once.
struct Symbols {
    std::map<std::string, const Observe*> observations;
    std::map<std::string, const Presumption*> presumptions;
    std::map<std::string, const Action*> actions;

    [[nodiscard]] bool is_observation(const std::string& n) const { return observations.count(n) != 0; }
    [[nodiscard]] bool is_presumption(const std::string& n) const { return presumptions.count(n) != 0; }
    [[nodiscard]] bool is_action(const std::string& n) const { return actions.count(n) != 0; }
    [[nodiscard]] bool known(const std::string& n) const {
        return is_observation(n) || is_presumption(n) || is_action(n);
    }
};

std::string gate_name(const std::string& gated) { return "gate_no_" + gated; }
std::string usable_name(const std::string& raw) { return "claim_" + raw; }

std::string mismatch_name(const Implies& im) { return "mismatch__" + im.antecedent + "__" + im.consequent; }

std::string incoherence_name(const std::vector<std::string>& members) {
    std::string n = "incoherent";
    for (const std::string& m : members) { n += "__" + m; }
    return n;
}

}  // namespace

/// Milli-units back to the decimal the author actually typed.
///
/// The compiler scales to integers immediately, and every message that quoted a setpoint was quoting `90000` at an
/// engineer who had written `90.0`. Correct, and useless: they cannot find `90000` by searching their own file.
std::string as_written(std::int32_t milli) {
    const bool negative = milli < 0;
    const std::int64_t magnitude = negative ? -static_cast<std::int64_t>(milli) : milli;
    const std::int64_t whole = magnitude / 1000;
    const std::int64_t frac = magnitude % 1000;

    std::string out = (negative ? "-" : "") + std::to_string(whole);
    if (frac != 0) {
        std::string f = std::to_string(frac);
        while (f.size() < 3) { f = "0" + f; }
        while (!f.empty() && f.back() == '0') { f.pop_back(); }
        out += "." + f;
    }
    return out;
}

void desugar_votes(Casebook& cb, Diagnostics& diag) {
    std::vector<Presumption> generated;

    for (Presumption& p : cb.presumptions) {
        if (p.vote_threshold == 0) { continue; }

        const int n = static_cast<int>(p.vote_items.size());
        const int k = p.vote_threshold;

        // C(n,k) combinations. Guard the count: 2oo3 is 3 claims, 3oo5 is 10, but someone will eventually write
        // `vote 5 of` over fifteen sensors and get 3003 generated arguments, which would blow the device capacity with
        // no indication of why.
        long long combinations = 1;
        for (int i = 0; i < k; ++i) {
            combinations = combinations * (n - i) / (i + 1);
        }
        if (combinations > 64) {
            diag.error(p.line, "'vote " + std::to_string(k) + " of' over " + std::to_string(n) + " items expands to " +
                                   std::to_string(combinations) +
                                   " arguments, which will not fit a microcontroller. Group the items into "
                                   "intermediate claims and vote over those.");
            continue;
        }

        // Each combination becomes a conjunctive claim; the original becomes the disjunction of them. Generated names
        // carry the parent so a witness reads intelligibly: `level_high_voted__vote_ab` is recognisable, `gen_17` is
        // not.
        std::vector<std::string> branch_names;
        std::vector<int> idx(static_cast<std::size_t>(k));
        for (int i = 0; i < k; ++i) { idx[static_cast<std::size_t>(i)] = i; }

        while (true) {
            Presumption branch;
            branch.kind = p.kind;
            branch.generated = true;
            branch.line = p.line;
            branch.name = p.name + "__vote";
            branch.description = "at least " + std::to_string(k) + " of " + std::to_string(n) + " hold:";
            for (int i = 0; i < k; ++i) {
                const std::string& item = p.vote_items[static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])];
                branch.name += "_" + item;
                branch.description += " " + item;
                branch.needs.push_back(item);
            }
            // The severity of a branch is the parent's: a branch is not a separate hypothesis, it is one way of
            // establishing the same one, so it must not lose a preference the author declared.
            branch.severity = p.severity;
            branch_names.push_back(branch.name);
            generated.push_back(std::move(branch));

            // next combination in lexicographic order
            int i = k - 1;
            while (i >= 0 && idx[static_cast<std::size_t>(i)] == n - k + i) { --i; }
            if (i < 0) { break; }
            ++idx[static_cast<std::size_t>(i)];
            for (int j = i + 1; j < k; ++j) {
                idx[static_cast<std::size_t>(j)] = idx[static_cast<std::size_t>(j - 1)] + 1;
            }
        }

        for (const std::string& b : branch_names) { p.when.push_back(b); }
        p.vote_threshold = 0;
        p.vote_items.clear();
    }

    // Appended, not inserted: a generated branch references only the author's items, never another branch, so order
    // does not matter and appending keeps the author's declarations first in every report.
    for (Presumption& g : generated) { cb.presumptions.push_back(std::move(g)); }
}

Lowered lower(const Casebook& cb, Diagnostics& diag) {
    Lowered lo;
    lo.casebook = cb.name;
    lo.version = cb.version;
    lo.provenance = cb.provenance;

    // ── Step 1: generate incoherence doubts from `exclusive` ────────────────
    //
    // An exclusive group says the readings cannot physically co-occur. If they do, the INSTRUMENT is at fault --
    // a real, reachable condition, not an impossible one. So one declaration becomes a doubt that fires exactly
    // when every member is present and discards all of them. Because its `needs` and `undermines` sets are
    // identical, this automatically trips the raw/claim split below, which is the only thing that keeps it
    // acyclic.
    std::vector<Presumption> presumptions = cb.presumptions;
    for (const Exclusive& ex : cb.exclusives) {
        Presumption p;
        p.kind = PresumptionKind::Doubt;
        p.name = incoherence_name(ex.members);
        p.description = "readings that cannot co-occur are all present, so the instrument is untrustworthy";
        p.needs = ex.members;
        p.undermines = ex.members;
        p.generated = true;
        p.line = ex.line;
        presumptions.push_back(std::move(p));
    }

    // ── Step 1a: generate consistency doubts from `implies` ─────────────────
    //
    // "A is present and B is not." The doubt needs the raw antecedent and is cancelled by the raw consequent, and it
    // discards the antecedent. Because it needs and undermines the same reading, the antecedent is split into raw and
    // usable forms below, exactly as for `exclusive`, which is what keeps it acyclic.
    for (const Implies& im : cb.implications) {
        Presumption p;
        p.kind = PresumptionKind::Doubt;
        p.name = mismatch_name(im);
        p.description = im.antecedent + " is present without " + im.consequent +
                        (im.description.empty() ? std::string(", which it physically requires")
                                                : " (" + im.description + ")") +
                        ", so one of the two instruments is wrong";
        p.needs = {im.antecedent};
        p.undermines = {im.antecedent};
        p.unless = {im.consequent};
        p.generated = true;
        p.line = im.line;
        presumptions.push_back(std::move(p));
    }

    // ── Step 1b: generate the sensor-health observations for each channel ───
    //
    // Every channel gains an `_unavailable` observation, and one `_flatline` observation if it asks for the check.
    // These are GENERATED rather than left to the author because the alternative has a body count: under closed
    // world an absent observation is Out, so a channel that has died reads exactly like a channel reporting
    // nothing, and "the sensor is gone" silently becomes positive evidence of health. That is the CAPECO failure in
    // one line -- a frozen level transmitter read as "the tank is not full".
    //
    // Generating them means the casebook CAN reason about sensor failure. It does not mean it does: an author who
    // ignores `xmtr_unavailable` still gets the dangerous behaviour. The compiler warns about that in `check`,
    // which is the most it can honestly do -- whether a blind sensor is tolerable is an engineering judgement.
    std::vector<Observe> observes = cb.observes;
    for (const Channel& ch : cb.channels) {
        Observe u;
        u.name = ch.name + "_unavailable";
        u.description = "channel '" + ch.name + "' cannot be believed: never sampled, faulted, out of range, or "
                                                "stale. NOT the same as reporting nothing.";
        u.line = ch.line;
        observes.push_back(std::move(u));

        if (ch.flatline_after_ms != 0) {
            Observe f;
            f.name = ch.name + "_flatline";
            f.description = "channel '" + ch.name +
                            "' is delivering fresh, in-range samples that never move -- a failed transmitter, not "
                            "a steady process";
            f.line = ch.line;
            observes.push_back(std::move(f));
        }
    }

    // ── Step 1c: timed readings are observations too ────────────────────────
    //
    // The device sets them from other readings over time (binding.hpp, Timers); to the framework each is one more
    // presence bit, so every rule that uses one is still checked in every state. They come after the channel
    // readings, so a casebook without them numbers its observations exactly as before.
    for (const TimedReading& r : cb.timed) {
        Observe o;
        o.name = r.name;
        o.description = r.description;
        o.line = r.line;
        observes.push_back(std::move(o));
    }

    // ── Step 2: resolve symbols, and reject duplicates ──────────────────────
    Symbols sym;
    std::map<std::string, int> first_seen;

    auto claim_name = [&](const std::string& n, int line, const char* what) {
        auto it = first_seen.find(n);
        if (it != first_seen.end()) {
            diag.error(line, "'" + n + "' already declared on line " + std::to_string(it->second));
            return false;
        }
        first_seen[n] = line;
        if (n.empty()) {
            diag.error(line, std::string(what) + " needs a name");
            return false;
        }
        if (n.rfind("gate_no_", 0) == 0 || n.rfind("claim_", 0) == 0 || n == "closed_world") {
            diag.error(line, "'" + n + "' uses a name the compiler generates for itself. Pick a different one: "
                             "the prefixes gate_no_ and claim_, and the name closed_world, are reserved because "
                             "lowering creates arguments with those names and a collision would silently rewire "
                             "the framework");
            return false;
        }
        return true;
    };

    for (const Observe& o : observes) {
        if (claim_name(o.name, o.line, "observe")) { sym.observations[o.name] = &o; }
    }
    for (const Presumption& p : presumptions) {
        if (claim_name(p.name, p.line, kind_name(p.kind))) { sym.presumptions[p.name] = &p; }
    }
    for (const Action& a : cb.actions) {
        if (claim_name(a.name, a.line, "action")) { sym.actions[a.name] = &a; }
    }

    // Reference and type checking, before anything is generated from bad input.
    auto require_observation = [&](const std::string& n, int line, const char* ctx) {
        if (!sym.known(n)) {
            diag.error(line, "undefined name '" + n + "' in " + ctx + " -- declare it, or check the spelling");
        } else if (!sym.is_observation(n)) {
            // Almost always a confusion between the ASPIC+ attack forms rather than a typo, so name the one the
            // author probably wanted instead of only reporting what is wrong.
            const std::string hint =
                std::string(ctx) == "undermines"
                    ? ". 'undermines' attacks a PREMISE, so its target must be an observation; to attack a "
                      "conclusion write `rebuts` instead"
                    : "";
            diag.error(line, "'" + n + "' is not an observation, but " + ctx + " requires one" + hint);
        }
    };
    auto require_presumption = [&](const std::string& n, int line, const char* ctx) {
        if (!sym.known(n)) {
            diag.error(line, "undefined name '" + n + "' in " + ctx);
        } else if (!sym.is_presumption(n)) {
            const std::string hint =
                std::string(ctx) == "rebuts" || std::string(ctx) == "undercuts"
                    ? ". '" + std::string(ctx) +
                          "' attacks a conclusion, so its target must be a claim, excuse or doubt; to discard a "
                          "reading write `undermines` instead"
                    : "";
            diag.error(line, "'" + n + "' is not a claim, excuse or doubt, but " + ctx + " requires one" + hint);
        }
    };
    /// Evidence may be a reading or another presumption; it may not be an action, because an action is a
    /// conclusion of the framework and not evidence for anything.
    auto require_evidence = [&](const std::string& n, int line, const char* ctx) {
        if (!sym.known(n)) {
            diag.error(line, "undefined name '" + n + "' in " + ctx);
        } else if (sym.is_action(n)) {
            diag.error(line, "'" + n + "' is an action, which cannot serve as evidence in " + ctx);
        }
    };

    for (const Exclusive& ex : cb.exclusives) {
        for (const std::string& m : ex.members) { require_observation(m, ex.line, "exclusive"); }
    }
    for (const Implies& im : cb.implications) {
        require_observation(im.antecedent, im.line, "implies");
        require_observation(im.consequent, im.line, "implies");
    }
    for (const Presumption& p : presumptions) {
        for (const std::string& n : p.needs) { require_evidence(n, p.line, "needs"); }
        for (const std::string& n : p.when) { require_evidence(n, p.line, "when"); }
        for (const std::string& n : p.undermines) { require_observation(n, p.line, "undermines"); }
        if (p.kind == PresumptionKind::Doubt) {
            for (const std::string& n : p.needs) {
                if (sym.known(n) && !sym.is_observation(n)) {
                    diag.error(p.line, "a doubt must gate on raw readings, so '" + n +
                                           "' cannot appear in its 'needs'");
                }
            }
        }
    }
    for (const AttackDecl& a : cb.attacks) {
        if (!sym.known(a.from)) { diag.error(a.line, "undefined name '" + a.from + "' on the left of an attack"); }
        for (const std::string& t : a.to) {
            if (a.kind == AttackKind::Undermines) {
                require_observation(t, a.line, "undermines");
            } else {
                require_presumption(t, a.line, attack_kind_name(a.kind));
            }
        }
    }
    for (const Action& a : cb.actions) {
        for (const std::string& w : a.when) { require_evidence(w, a.line, "when"); }
        for (const std::string& w : a.needs) { require_evidence(w, a.line, "needs"); }
    }
    for (const Prefer& pr : cb.prefers) {
        for (const std::string& n : pr.order) {
            if (!sym.is_action(n)) { diag.error(pr.line, "'" + n + "' is not an action, but prefer requires one"); }
        }
    }

    if (diag.has_errors()) { return lo; }

    // ── Step 3: decide which observations need the raw/claim split ──────────
    //
    // Any doubt that discards a reading it also depends on is self-referential: `doubt -> obs -> gate -> doubt`
    // is a three-cycle, and every argument in an odd cycle goes Undecided. The entire channel would silently
    // become unknowable -- a correct labelling that means nothing. The fix is to separate the RAW reading, which
    // triggers the doubt, from the USABLE claim, which carries the discrimination and is what the doubt attacks.
    std::set<std::string> split;
    for (const Presumption& p : presumptions) {
        if (p.kind != PresumptionKind::Doubt) { continue; }
        for (const std::string& u : p.undermines) {
            if (std::find(p.needs.begin(), p.needs.end(), u) != p.needs.end()) { split.insert(u); }
        }
    }
    // And whenever a reading that something discards is also read RAW by a doubt (its needs or when) or by a
    // consistency doubt's `unless`. Without the split the discarding attack lands on the raw reading itself, so
    // every doubt that promised to read it raw would see the discarded value instead -- and whether it did depended
    // on whether some unrelated doubt happened to need and undermine the same reading, and on whether `undermines`
    // was written in the doubt's declaration or on its own line. Found 2026-10-14 by tools/refcheck.py, a second
    // implementation written from the documentation: on random rulebooks a doubt that discarded the consequent of
    // an `implies` made the mismatch fire and the safe action go unchosen. Readings no doubt reads raw are not
    // split, so every rulebook without that shape lowers exactly as before.
    {
        std::set<std::string> undermined;
        for (const Presumption& p : presumptions) {
            undermined.insert(p.undermines.begin(), p.undermines.end());
        }
        for (const AttackDecl& a : cb.attacks) {
            if (a.kind == AttackKind::Undermines) { undermined.insert(a.to.begin(), a.to.end()); }
        }
        for (const Presumption& p : presumptions) {
            std::vector<std::string> raw = p.unless;
            if (p.kind == PresumptionKind::Doubt) {
                raw.insert(raw.end(), p.needs.begin(), p.needs.end());
                raw.insert(raw.end(), p.when.begin(), p.when.end());
            }
            for (const std::string& r : raw) {
                if (undermined.count(r) != 0) { split.insert(r); }
            }
        }
    }

    // ── Step 4: lay out arguments. Observations must be a contiguous prefix. ─
    auto add = [&lo](std::string name, std::string desc, Role role, bool generated,
                     std::string relates = {}, int src_line = 0) -> std::int32_t {
        LoweredArgument a;
        a.name = std::move(name);
        a.description = std::move(desc);
        a.role = role;
        a.generated = generated;
        a.relates_to = std::move(relates);
        a.line = src_line;
        lo.arguments.push_back(std::move(a));
        return static_cast<std::int32_t>(lo.arguments.size() - 1);
    };

    std::map<std::string, std::int32_t> idx;

    for (const Observe& o : observes) {
        idx[o.name] = add(o.name, o.description, Role::Observation, false, {}, o.line);
    }
    lo.observation_count = static_cast<std::int32_t>(lo.arguments.size());

    // ── Step 4b: resolve the binding ────────────────────────────────────────
    //
    // Channels are resolved here, after the observation layout, because the tables the device uses are indices into
    // exactly this numbering. The physical constants were already converted to milli-units by the parser, on the
    // host, so the device never performs floating-point arithmetic.
    for (const Channel& ch : cb.channels) {
        LoweredChannel lc;
        lc.name = ch.name;
        lc.description = ch.description;
        lc.unit = ch.unit;
        lc.valid_lo = ch.valid_lo;
        lc.valid_hi = ch.valid_hi;
        lc.stale_after_ms = ch.stale_after_ms;
        lc.flatline_after_ms = ch.flatline_after_ms;
        lc.flatline_epsilon = ch.flatline_epsilon;
        lc.line = ch.line;

        const auto u = idx.find(ch.name + "_unavailable");
        lc.unavailable_obs = u != idx.end() ? u->second : -1;
        if (ch.flatline_after_ms != 0) {
            const auto f = idx.find(ch.name + "_flatline");
            lc.flatline_obs = f != idx.end() ? f->second : -1;
        }

        if (!ch.has_range) {
            diag.error(ch.line, "channel '" + ch.name +
                                    "' needs a 'range', so an implausible reading can be told from a real one");
        }
        if (ch.stale_after_ms == 0) {
            diag.error(ch.line,
                       "channel '" + ch.name +
                           "' needs a 'stale_after'. A channel with no freshness requirement can never be reported "
                           "dead, which defeats the purpose of declaring it");
        }
        if (ch.thresholds.empty()) {
            diag.warn(ch.line, "channel '" + ch.name +
                                   "' asserts no observation, so it only ever reports its own health");
        }

        const std::int32_t channel_index = static_cast<std::int32_t>(lo.channels.size());
        for (const Threshold& th : ch.thresholds) {
            LoweredThreshold lt;
            lt.channel = channel_index;
            lt.above = th.above;
            lt.on = th.on;
            lt.off = th.off;
            lt.hold_ms = th.hold_ms;

            const auto o = idx.find(th.observation);
            if (o == idx.end()) {
                diag.error(th.line, "channel '" + ch.name + "' asserts undefined observation '" + th.observation +
                                        "'");
                continue;
            }
            if (!sym.is_observation(th.observation)) {
                diag.error(th.line, "'" + th.observation + "' is not an observation, so a channel cannot assert it");
                continue;
            }
            lt.observation = o->second;

            // A deadband that runs the wrong way is positive feedback, not hysteresis.
            if (th.above && th.off > th.on) {
                diag.error(th.line, "threshold '" + th.observation + "' asserts above " + as_written(th.on) +
                                        " but releases at " + as_written(th.off) +
                                        ", which is higher -- so once asserted it could never clear. For a rising "
                                        "threshold the release value must be BELOW the assert value; write something "
                                        "like `above " + as_written(th.on) + " release " +
                                        as_written(th.on - (th.on / 20)) + "`.");
            }
            if (!th.above && th.off < th.on) {
                diag.error(th.line, "threshold '" + th.observation + "' asserts below " + as_written(th.on) +
                                        " but releases at " + as_written(th.off) +
                                        ", which is lower -- so once asserted it could never clear. For a falling "
                                        "threshold the release value must be ABOVE the assert value; write something "
                                        "like `below " + as_written(th.on) + " release " +
                                        as_written(th.on + (th.on / 20)) + "`.");
            }
            // A setpoint outside the valid range can never be reached, so the observation is dead on arrival. Worth
            // an error rather than a warning: the author believes they have a detector and they do not.
            if (ch.has_range && (th.on < ch.valid_lo || th.on > ch.valid_hi)) {
                diag.error(th.line, "threshold '" + th.observation + "' asserts at " + as_written(th.on) +
                                        ", outside channel '" + ch.name + "' valid range of " +
                                        as_written(ch.valid_lo) + " to " + as_written(ch.valid_hi) +
                                        " -- so it can never be asserted, and the observation is dead. Either widen "
                                        "the range or move the setpoint inside it.");
            }
            lo.thresholds.push_back(std::move(lt));
        }
        lo.channels.push_back(std::move(lc));
    }

    // ── Step 4c: resolve the timed readings ─────────────────────────────────
    //
    // A timer watches readings: an `observe`, a reading a channel sets, or another timed reading. Never a
    // conclusion: that would feed the rules' own answers back into their inputs, and exhaustive validation would no
    // longer cover what the device does. The table is ordered so that each timer comes after those it watches (the
    // device steps them in table order), and a loop of timed readings watching each other is refused.
    {
        const auto base_reading = [&](const std::string& name, const TimedReading& r, const char* what) -> std::int32_t {
            const auto it = idx.find(name);
            bool conclusion = false;
            for (const Presumption& p : cb.presumptions) { conclusion = conclusion || p.name == name; }
            for (const Action& a : cb.actions) { conclusion = conclusion || a.name == name; }
            if (it == idx.end() && !conclusion) {
                diag.error(r.line, "reading '" + r.name + "' watches undefined reading '" + name + "'" + what +
                                       "; declare it with `observe`, or check the spelling");
                return -1;
            }
            if (conclusion || it->second >= lo.observation_count) {
                diag.error(r.line, "reading '" + r.name + "' watches '" + name +
                                       "', which is a conclusion, not a reading. A timed reading can watch only an "
                                       "`observe` or a reading a channel sets: watch the readings the conclusion "
                                       "needs instead");
                return -1;
            }
            if (name == r.name) {
                diag.error(r.line, "reading '" + r.name + "' watches itself; watch the reading it is built from instead");
                return -1;
            }
            return it->second;
        };
        for (const TimedReading& r : cb.timed) {
            LoweredTimer lt;
            lt.line = r.line;
            lt.kind = r.kind == TimedReading::Kind::After ? 0 : (r.kind == TimedReading::Kind::Recent ? 1 : 2);
            lt.observation = idx.count(r.name) != 0 ? idx[r.name] : -1;
            lt.source = base_reading(r.source, r, "");
            if (r.kind == TimedReading::Kind::After) {
                lt.after = base_reading(r.after, r, " after 'after'");
                if (r.after == r.source) {
                    diag.error(r.line, "reading '" + r.name + "' waits for '" + r.source +
                                           "' after itself; write two different readings");
                }
            }
            lt.window_ms = r.window_ms;
            lt.times = r.kind == TimedReading::Kind::Count ? r.times : 0;
            if (r.window_ms > 0x7FFFFFFFU) {
                diag.error(r.line, "reading '" + r.name + "' has a time over 24 days, longer than the device's "
                                   "millisecond clock can compare safely");
            }
            lo.timers.push_back(lt);
        }
        // Order: declaration order, except that a timer moves after any timer it watches. Without chains this keeps
        // the declaration order exactly, so tables (and fingerprints) of earlier rulebooks do not change.
        {
            std::vector<LoweredTimer> ordered;
            std::vector<bool> placed(lo.timers.size(), false);
            std::set<std::int32_t> done;  // observations of the timers already placed
            std::set<std::int32_t> outputs;
            for (const LoweredTimer& t : lo.timers) { outputs.insert(t.observation); }
            const auto ready = [&](const LoweredTimer& t) {
                for (const std::int32_t w : {t.source, t.after}) {
                    if (w >= 0 && outputs.count(w) != 0 && done.count(w) == 0) { return false; }
                }
                return true;
            };
            while (ordered.size() < lo.timers.size()) {
                bool progress = false;
                for (std::size_t i = 0; i < lo.timers.size(); ++i) {
                    if (placed[i] || !ready(lo.timers[i])) { continue; }
                    placed[i] = true;
                    done.insert(lo.timers[i].observation);
                    ordered.push_back(lo.timers[i]);
                    progress = true;
                    break;  // restart from the first declared, so the order is declaration order where it can be
                }
                if (!progress) {
                    std::string names;
                    int line = 0;
                    for (std::size_t i = 0; i < lo.timers.size(); ++i) {
                        if (placed[i]) { continue; }
                        names += (names.empty() ? "'" : ", '") + cb.timed[i].name + "'";
                        if (line == 0) { line = cb.timed[i].line; }
                    }
                    diag.error(line, "timed readings " + names + " watch each other in a loop, so none of them can "
                                     "be worked out first; break the loop by watching a base reading instead");
                    break;
                }
            }
            if (ordered.size() == lo.timers.size()) { lo.timers = std::move(ordered); }
        }
        // A doubt that discards a reading does not reach a timed reading built from it: the device works the timed
        // reading out from the raw bits. Usually the author wants both discarded, so say so when only one is.
        std::map<std::string, std::set<std::string>> discards;  // doubt -> readings it undermines
        for (const Presumption& p : cb.presumptions) {
            for (const std::string& u : p.undermines) { discards[p.name].insert(u); }
        }
        for (const AttackDecl& a : cb.attacks) {
            if (a.kind != AttackKind::Undermines) { continue; }
            for (const std::string& u : a.to) { discards[a.from].insert(u); }
        }
        for (const TimedReading& r : cb.timed) {
            for (const auto& [doubt, readings] : discards) {
                for (const std::string& from : {r.source, r.after}) {
                    if (from.empty() || readings.count(from) == 0 || readings.count(r.name) != 0) { continue; }
                    diag.warn(r.line, "'" + doubt + "' discards '" + from + "' but not '" + r.name +
                                          "', which the device works out from it. If a suspect " + from +
                                          " should not set " + r.name + " either, add " + r.name + " to what " +
                                          doubt + " undermines");
                }
            }
        }
        if (lo.timers.size() > 8U) {
            diag.error(cb.timed[8].line, "more than 8 timed readings; the board runtime has room for 8");
        }
    }

    // The usable counterpart of each split reading.
    std::map<std::string, std::int32_t> usable;
    for (const Observe& o : observes) {
        if (split.count(o.name) == 0) { continue; }
        const std::string n = usable_name(o.name);
        usable[o.name] = add(n, "trustworthy form of " + o.name, Role::UsableClaim, true, o.name, o.line);
        idx[n] = usable[o.name];
    }

    for (const Presumption& p : presumptions) {
        if (p.kind == PresumptionKind::Doubt) { continue; }
        idx[p.name] = add(p.name, p.description,
                          p.kind == PresumptionKind::Claim ? Role::Claim : Role::Excuse, p.generated, {},
                          p.line);
        lo.arguments[static_cast<std::size_t>(idx[p.name])].severity = p.severity;
    }
    for (const Presumption& p : presumptions) {
        if (p.kind != PresumptionKind::Doubt) { continue; }
        idx[p.name] = add(p.name, p.description, Role::Doubt, p.generated, {}, p.line);
        lo.arguments[static_cast<std::size_t>(idx[p.name])].severity = p.severity;
    }
    for (const Action& a : cb.actions) {
        idx[a.name] = add(a.name, "", Role::Action, false, {}, a.line);
        lo.actions.push_back(idx[a.name]);
        // Duplicates are caught HERE rather than in `check`, because by then the information is gone: `check` only
        // sees a single `idle_action` index and counted how many actions equalled it, which is at most one by
        // construction. That check was vacuous from the day it was written, and it took documenting the intended
        // behaviour in the authoring guide to notice -- the guide promised a diagnostic that did not exist.
        if (a.idle) {
            if (lo.idle_action >= 0) {
                diag.error(a.line, "action '" + a.name + "' is also marked 'idle', but '" +
                                       lo.arguments[static_cast<std::size_t>(lo.idle_action)].name +
                                       "' already is. Exactly one action means 'carry on, nothing is wrong'; remove "
                                       "'idle' from one of them.");
            }
            lo.idle_action = idx[a.name];
        }
        if (a.safe) {
            if (lo.safe_action >= 0) {
                diag.error(a.line, "action '" + a.name + "' is also marked 'safe', but '" +
                                       lo.arguments[static_cast<std::size_t>(lo.safe_action)].name +
                                       "' already is. An asset has one safe state, and the arbiter needs to know "
                                       "which; remove 'safe' from one of them.");
            }
            lo.safe_action = idx[a.name];
        }
    }

    // Gates are appended as they are needed, memoised so they are shared. Sharing is what makes conjunction work:
    // a presumption needing two readings is attacked by two gates and survives only when both are Out.
    //
    // `gated_name` is taken BY VALUE, deliberately. Taking it by reference is a use-after-free: callers naturally
    // write `gate_of(lo.arguments[i].name)`, and `add()` push_backs onto that same vector, reallocating it and
    // dangling the reference mid-call. That bug cost a debugger session; the copy is cheap and the comment is
    // cheaper.
    std::map<std::string, std::int32_t> gates;
    auto gate_of = [&](std::string gated_name) -> std::int32_t {
        auto it = gates.find(gated_name);
        if (it != gates.end()) { return it->second; }
        const std::string n = gate_name(gated_name);
        const std::int32_t g = add(n, "fires when " + gated_name + " is absent", Role::Gate, true, gated_name);
        gates[std::move(gated_name)] = g;
        idx[n] = g;
        return g;
    };

    // ── Step 5: edges ───────────────────────────────────────────────────────
    std::set<std::pair<std::int32_t, std::int32_t>> seen_edges;
    auto edge = [&](std::int32_t from, std::int32_t to, std::string reason, bool pref_dependent = false) {
        if (from < 0 || to < 0) { return; }
        if (!seen_edges.insert({from, to}).second) { return; }  // dedupe
        lo.edges.push_back({from, to, std::move(reason), pref_dependent});
    };

    /// The argument that carries a reading's discriminating power. For a split reading that is the usable claim,
    /// never the raw one. For anything that is not a reading, itself.
    auto effective = [&](const std::string& name) -> std::int32_t {
        auto it = usable.find(name);
        return it != usable.end() ? it->second : idx[name];
    };

    // CONJUNCTIVE evidence. One gate per requirement, each defeated by the requirement itself, so the presumption
    // survives only when every gate is Out -- which requires every requirement to hold.
    //
    // A claim or excuse gates on the EFFECTIVE form, so a discarded reading cannot enable it. A doubt gates on the
    // RAW reading, because a doubt must be able to fire from evidence it is about to discard; that asymmetry is
    // what keeps the raw/claim split acyclic.
    for (const Presumption& p : presumptions) {
        for (const std::string& o : p.needs) {
            const std::int32_t source = p.kind == PresumptionKind::Doubt ? idx[o] : effective(o);
            const std::int32_t g = gate_of(lo.arguments[static_cast<std::size_t>(source)].name);
            edge(source, g, "requirement present, so the gate falls");
            edge(g, idx[p.name], std::string(kind_name(p.kind)) + " '" + p.name + "' needs " + o);
        }
    }

    // DISJUNCTIVE evidence. One shared gate attacked by every alternative, so any single alternative fells it.
    for (const Presumption& p : presumptions) {
        if (p.when.empty()) { continue; }
        const std::string gname = p.name + "_evidence";
        const std::int32_t g = add(gate_name(gname), "fires when nothing supports " + p.name, Role::Gate, true,
                                   p.name);
        idx[gate_name(gname)] = g;
        for (const std::string& w : p.when) {
            const std::int32_t source = p.kind == PresumptionKind::Doubt ? idx[w] : effective(w);
            edge(source, g, w + " is enough to support " + p.name);
        }
        edge(g, idx[p.name], std::string(kind_name(p.kind)) + " '" + p.name + "' needs some evidence");
    }

    // A usable claim requires its raw reading.
    for (const auto& [raw, claim_idx] : usable) {
        const std::int32_t g = gate_of(raw);
        edge(idx[raw], g, "raw reading present, so the gate falls");
        edge(g, claim_idx, "the usable claim requires its raw reading");
    }

    // A doubt discards what it undermines -- the usable claim if split, the raw reading otherwise.
    for (const Presumption& p : presumptions) {
        if (p.kind != PresumptionKind::Doubt) { continue; }
        for (const std::string& o : p.undermines) {
            const bool is_split = usable.count(o) != 0;
            const std::int32_t target = is_split ? usable[o] : idx[o];
            edge(idx[p.name], target,
                 "doubt '" + p.name + "' undermines " + o + (is_split ? " (via its usable claim)" : ""));
        }
    }

    // A generated consistency doubt is cancelled by the RAW consequent: the question is whether the instruments agree.
    for (const Presumption& p : presumptions) {
        for (const std::string& u : p.unless) {
            edge(idx[u], idx[p.name], u + " is present, so '" + p.name + "' has nothing to explain");
        }
    }

    // Declared attacks. `undermines` targets the raw reading; the other two target a presumption. The source is
    // the effective reading, so a split reading's discriminating power comes from its usable claim.
    for (const AttackDecl& a : cb.attacks) {
        const std::int32_t from = sym.is_observation(a.from) ? effective(a.from) : idx[a.from];
        // Rebutting and undermining are preference-DEPENDENT in ASPIC+ and so can be cancelled by severity.
        // Undercutting is not: it says the inference does not apply at all, which is not a claim that can be
        // outranked by a more serious one.
        const bool pref_dependent = a.kind != AttackKind::Undercuts;
        for (const std::string& t : a.to) {
            // A discarding attack on a split reading lands on its usable claim, as a doubt's own `undermines` does.
            const bool via_usable = a.kind == AttackKind::Undermines && usable.count(t) != 0;
            edge(from, via_usable ? usable[t] : idx[t],
                 std::string(a.from) + " " + attack_kind_name(a.kind) + " " + t + (via_usable ? " (via its usable claim)" : ""),
                 pref_dependent);
        }
    }

    // An action's CONJUNCTIVE conditions: one gate per requirement, so the action survives only when every one
    // holds. Written `when all ...` or `needs ...`.
    for (const Action& a : cb.actions) {
        for (const std::string& n : a.needs) {
            const std::int32_t source = effective(n);
            const std::int32_t g = gate_of(lo.arguments[static_cast<std::size_t>(source)].name);
            edge(source, g, "condition present, so the gate falls");
            edge(g, idx[a.name], "action '" + a.name + "' requires " + n);
        }
    }

    // An action's `when` list is a DISJUNCTION, exactly as for a presumption.
    for (const Action& a : cb.actions) {
        if (a.when.empty()) { continue; }
        const std::string gname = a.name + "_reason";
        const std::int32_t g = add(gate_name(gname), "fires when nothing justifies " + a.name, Role::Gate, true,
                                   a.name);
        idx[gate_name(gname)] = g;
        for (const std::string& w : a.when) {
            edge(effective(w), g, w + " justifies " + a.name);
        }
        edge(g, idx[a.name], "action '" + a.name + "' needs a reason");
    }

    // Precedence. A chain a > b > c yields every pair, so the ordering is a genuine total order over the chain
    // rather than a sequence of local preferences.
    for (const Prefer& pr : cb.prefers) {
        for (std::size_t i = 0; i < pr.order.size(); ++i) {
            for (std::size_t j = i + 1; j < pr.order.size(); ++j) {
                edge(idx[pr.order[i]], idx[pr.order[j]],
                     pr.order[i] + " is preferred over " + pr.order[j]);
            }
        }
    }

    // The idle action is whatever is left when nothing else applies.
    if (lo.idle_action >= 0) {
        for (const std::int32_t a : lo.actions) {
            if (a == lo.idle_action) { continue; }
            edge(a, lo.idle_action, "any active action displaces the idle one");
        }
    }

    // ── Step 5b: resolve preference-dependent attacks by severity ───────────
    //
    // The one column of an FMEA that translates into the framework. ASPIC+ says a preference-dependent attack
    // succeeds as a DEFEAT only if the attacker is not strictly less preferred than its target; so an attack from a
    // less severe claim on a more severe one is dropped, and the more severe claim survives.
    //
    // This is the conservative direction, and that is the whole justification. When the evidence cannot distinguish
    // two hypotheses, the framework previously reported ambiguity and escalated to a human -- correct, but it
    // escalates on every such pair, and an alarm that fires on every ambiguity is the alarm flood again. Severity
    // says: act on the worse reading. You cannot talk the machine out of a serious hypothesis using a mild one.
    //
    // Deliberately opt-in. A casebook that declares no severities gets no edges dropped and behaves exactly as
    // before. Every drop is reported, because silently deleting an attack an author wrote would be indefensible.
    {
        std::vector<LoweredEdge> kept;
        kept.reserve(lo.edges.size());
        for (const LoweredEdge& e : lo.edges) {
            if (!e.preference_dependent) {
                kept.push_back(e);
                continue;
            }
            const int attacker = lo.arguments[static_cast<std::size_t>(e.from)].severity;
            const int target = lo.arguments[static_cast<std::size_t>(e.to)].severity;
            // Both must be declared. An undeclared severity is not "zero" -- it is unknown, and treating unknown as
            // the weakest possible value would silently strip attacks from every claim the author had not annotated.
            if (attacker == 0 || target == 0 || attacker >= target) {
                kept.push_back(e);
                continue;
            }
            lo.preference_resolutions.push_back(
                {lo.arguments[static_cast<std::size_t>(e.from)].name, attacker,
                 lo.arguments[static_cast<std::size_t>(e.to)].name, target});
        }
        lo.edges = std::move(kept);
    }

    // ── Step 6: the closed-world argument ───────────────────────────────────
    //
    // Unattacked, therefore always In. Its edges are added at INSTANTIATION time, one per absent reading, because
    // which readings are absent changes every cycle. An absent reading must be decisively Out: self-attack would
    // make it Undecided, and an Undecided attacker blocks its target from ever being In, so the whole framework
    // would go Undecided on a quiet sensor.
    lo.closed_world = add("closed_world", "readings not asserted this cycle are taken to be false",
                          Role::ClosedWorld, true);

    // ── Step 7: metadata ────────────────────────────────────────────────────
    for (const Exclusive& ex : cb.exclusives) {
        std::vector<std::int32_t> group;
        for (const std::string& m : ex.members) { group.push_back(idx[m]); }
        lo.exclusive_groups.push_back(std::move(group));
    }
    for (const Implies& im : cb.implications) { lo.consistency_doubts.push_back(idx[mismatch_name(im)]); }

    lo.max_args = lo.count();
    lo.max_attacks = static_cast<std::int32_t>(lo.edges.size()) + lo.observation_count;

    return lo;
}

void check(const Lowered& lo, Diagnostics& diag) {
    const int line = lo.provenance.line;

    /// The line of an argument, for diagnostics. Structural errors are reported against the construct that caused
    /// them; falling back to the provenance line only when nothing better is known.
    auto line_of = [&lo](std::int32_t a) {
        if (a < 0 || a >= lo.count()) { return 0; }
        return lo.arguments[static_cast<std::size_t>(a)].line;
    };
    /// Where to point when the whole action set is at fault: the last action declared, since that is where the
    /// author was working and where the missing keyword has to be added.
    const int action_line = lo.actions.empty() ? line : line_of(lo.actions.back());
    const std::string last_action =
        lo.actions.empty() ? std::string() : lo.arguments[static_cast<std::size_t>(lo.actions.back())].name;

    if (lo.actions.empty()) {
        diag.error(line, "a casebook needs at least one action; declare one, and mark it both 'idle' and 'safe' if "
                         "the asset has only one state");
    }

    if (lo.idle_action < 0 && !lo.actions.empty()) {
        diag.error(action_line,
                   "no action is marked 'idle'. Write `action " + last_action +
                       " idle` on whichever action means 'carry on, nothing is wrong'. Without one, a healthy quiet "
                       "asset decisively rejects every action and the arbiter reports Unsatisfied -- a defect on "
                       "the most common operating state.");
    }
    if (lo.safe_action < 0 && !lo.actions.empty()) {
        diag.error(action_line,
                   "no action is marked 'safe'. Write `action " + last_action +
                       " safe` on whichever action is this asset's safe state. The arbiter needs a fallback for "
                       "when the framework will not choose, and the safe state is asset-specific -- a pump stops, a "
                       "tank stops filling, a drone must keep flying -- so it cannot be assumed.");
    }

    // Duplicate 'idle' and 'safe' flags are detected during lowering, where the declarations are still visible.
    // Counting them here cannot work: `lo` carries one index for each, so any count is at most one.

    if (lo.observation_count == 0) { diag.warn(line, "casebook has no observations, so nothing can vary"); }
    if (lo.observation_count > 30) {
        diag.error(line, "more than 30 observations (" + std::to_string(lo.observation_count) +
                             ") -- the instantiation mask is 32 bits and exhaustive validation is 2^n");
    } else if (lo.observation_count > 20) {
        diag.warn(line, std::to_string(lo.observation_count) + " observations means " +
                            std::to_string(1L << lo.observation_count) +
                            " states; exhaustive validation takes about " +
                            std::to_string(1L + (1L << lo.observation_count) / 600000L / 8L) +
                            " s on an 8-thread PC, and doubles with each reading. `cbc --symbolic` checks the main " +
                            "invariants of every state at once");
    }

    // A gate with no attacker can never fall, so whatever it gates is dead. A gate that attacks nothing is waste.
    std::vector<int> in_degree(static_cast<std::size_t>(lo.count()), 0);
    std::vector<int> out_degree(static_cast<std::size_t>(lo.count()), 0);
    for (const LoweredEdge& e : lo.edges) {
        ++in_degree[static_cast<std::size_t>(e.to)];
        ++out_degree[static_cast<std::size_t>(e.from)];
    }
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        const LoweredArgument& a = lo.arguments[static_cast<std::size_t>(i)];
        if (a.role == Role::Gate && in_degree[static_cast<std::size_t>(i)] == 0) {
            diag.error(line, "gate '" + a.name + "' can never fall, so what it gates is unreachable");
        }
        if (a.role == Role::Gate && out_degree[static_cast<std::size_t>(i)] == 0) {
            diag.warn(line, "gate '" + a.name + "' gates nothing");
        }
        if (a.role == Role::Claim && in_degree[static_cast<std::size_t>(i)] == 0) {
            diag.warn(line, "claim '" + a.name + "' has no evidence requirement, so it always holds");
        }
    }

    // The closed-world argument must be unattacked, or absence stops being decisive.
    if (lo.closed_world >= 0 && in_degree[static_cast<std::size_t>(lo.closed_world)] != 0) {
        diag.error(line, "the closed-world argument is attacked; absent readings would become Undecided");
    }

    // The split must have worked: no doubt may both need and undermine the same raw reading after lowering.
    for (const LoweredEdge& e : lo.edges) {
        const LoweredArgument& from = lo.arguments[static_cast<std::size_t>(e.from)];
        const LoweredArgument& to = lo.arguments[static_cast<std::size_t>(e.to)];
        if (from.role == Role::Doubt && to.role == Role::Observation) {
            // Legal only when that doubt does not also gate on this reading.
            for (const LoweredEdge& g : lo.edges) {
                if (g.to != e.from) { continue; }
                const LoweredArgument& gate = lo.arguments[static_cast<std::size_t>(g.from)];
                if (gate.role == Role::Gate && gate.relates_to == to.name) {
                    diag.error(line, "doubt '" + from.name + "' both needs and undermines raw reading '" +
                                         to.name + "'; the raw/claim split was not applied");
                }
            }
        }
    }

    // ── Can the casebook notice its own blindness? ──────────────────────────
    //
    // A generated `_unavailable` observation that nothing reasons about is the dangerous case. The binding layer
    // will dutifully assert it, the framework will label it In, and not one argument will care -- so the casebook
    // behaves exactly as if the sensor were healthy and reporting nothing. That is the CAPECO failure reintroduced
    // through inattention.
    //
    // A warning rather than an error, deliberately. Whether a blind channel is tolerable is an engineering
    // judgement: a redundant sensor on a non-critical trend genuinely may not need to be argued about. The compiler
    // can say "you are not using this", not "you are wrong".
    for (const LoweredChannel& ch : lo.channels) {
        auto unused = [&lo](std::int32_t obs) {
            if (obs < 0) { return false; }
            for (const LoweredEdge& edge : lo.edges) {
                if (edge.from == obs) { return false; }
            }
            return true;
        };
        if (unused(ch.unavailable_obs)) {
            diag.warn(ch.line, "nothing reasons about '" + ch.name +
                                "_unavailable', so this casebook cannot tell a dead " + ch.name +
                                " sensor from one reporting nothing");
        }
        if (unused(ch.flatline_obs)) {
            diag.warn(ch.line, "nothing reasons about '" + ch.name +
                                "_flatline', so a stuck " + ch.name + " transmitter will be believed");
        }
    }

    // Every threshold must land on a distinct observation per channel, or two setpoints on one signal fight over
    // one proposition and the later one silently wins.
    for (std::size_t i = 0; i < lo.thresholds.size(); ++i) {
        for (std::size_t j = i + 1; j < lo.thresholds.size(); ++j) {
            if (lo.thresholds[i].channel != lo.thresholds[j].channel) { continue; }
            if (lo.thresholds[i].observation != lo.thresholds[j].observation) { continue; }
            diag.error(lo.channels[static_cast<std::size_t>(lo.thresholds[i].channel)].line,
                       "channel '" + lo.channels[static_cast<std::size_t>(lo.thresholds[i].channel)].name +
                           "' has two thresholds asserting the same observation; remove one, or give the second "
                           "its own observation");
        }
    }
}

}  // namespace quorum::cbc
