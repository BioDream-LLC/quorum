#include <cstdlib>

#include "quorum/cbc/cbc.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace quorum::cbc {

const char* kind_name(PresumptionKind k) {
    switch (k) {
        case PresumptionKind::Claim:  return "claim";
        case PresumptionKind::Excuse: return "excuse";
        case PresumptionKind::Doubt:  return "doubt";
    }
    return "?";
}

const char* attack_kind_name(AttackKind k) {
    switch (k) {
        case AttackKind::Undermines: return "undermines";
        case AttackKind::Rebuts:     return "rebuts";
        case AttackKind::Undercuts:  return "undercuts";
    }
    return "?";
}

const char* role_name(Role r) {
    switch (r) {
        case Role::Observation: return "observation";
        case Role::UsableClaim: return "usable-claim";
        case Role::Claim:       return "claim";
        case Role::Excuse:      return "excuse";
        case Role::Doubt:       return "doubt";
        case Role::Gate:        return "gate";
        case Role::Action:      return "action";
        case Role::ClosedWorld: return "closed-world";
    }
    return "?";
}

namespace {

struct Token {
    std::string value;
    bool quoted = false;
};

struct LogicalLine {
    int line = 0;  ///< the physical line the declaration starts on
    std::vector<Token> tokens;
};

/// Split one physical line into tokens, treating "..." as a single token and dropping `#` comments.
///
/// A `#` inside quotes is literal; a `#` outside quotes starts a comment. Getting that wrong would silently
/// truncate any description containing a hash.
std::vector<Token> tokenize(const std::string& line, int line_no, Diagnostics& diag) {
    std::vector<Token> out;
    std::size_t i = 0;
    const std::size_t n = line.size();

    while (i < n) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        if (c == '#') { break; }

        if (c == '"') {
            ++i;
            std::string value;
            bool closed = false;
            while (i < n) {
                if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                value.push_back(line[i]);
                ++i;
            }
            if (!closed) { diag.error(line_no, "unterminated quoted string: a \" was opened and never closed on this line. Descriptions must fit on "
                             "one line; use a following indented line for extra clauses."); }
            out.push_back({value, true});
            continue;
        }

        std::string value;
        while (i < n && std::isspace(static_cast<unsigned char>(line[i])) == 0 && line[i] != '#' && line[i] != '"') {
            value.push_back(line[i]);
            ++i;
        }
        out.push_back({value, false});
    }
    return out;
}

/// Assemble physical lines into logical ones. An indented non-empty line continues the previous declaration --
/// which is what lets a `doubt` put its `undermines` list on its own line without inventing a continuation
/// character.
std::vector<LogicalLine> logical_lines(const std::string& text, Diagnostics& diag) {
    std::vector<LogicalLine> out;
    std::istringstream in(text);
    std::string raw;
    int line_no = 0;

    while (std::getline(in, raw)) {
        ++line_no;
        const bool indented = !raw.empty() && (raw[0] == ' ' || raw[0] == '\t');
        std::vector<Token> toks = tokenize(raw, line_no, diag);
        if (toks.empty()) { continue; }

        if (indented && !out.empty()) {
            out.back().tokens.insert(out.back().tokens.end(), toks.begin(), toks.end());
        } else {
            out.push_back({line_no, std::move(toks)});
        }
    }
    return out;
}

[[nodiscard]] bool is_section_keyword(const Token& t) {
    if (t.quoted) { return false; }
    return t.value == "needs" || t.value == "undermines" || t.value == "rebuts" ||
           t.value == "undercuts" || t.value == "when" || t.value == "over" || t.value == "severity" ||
           t.value == "vote";
}

/// Collect the token list following each section keyword, starting at `from`.
std::map<std::string, std::vector<std::string>> sections(const std::vector<Token>& toks,
                                                        std::size_t from,
                                                        Diagnostics& diag,
                                                        int line,
                                                        std::vector<std::string>* bare_flags = nullptr) {
    std::map<std::string, std::vector<std::string>> out;
    std::string current;
    for (std::size_t i = from; i < toks.size(); ++i) {
        if (is_section_keyword(toks[i])) {
            current = toks[i].value;
            out.emplace(current, std::vector<std::string>{});
            continue;
        }
        if (current.empty()) {
            if (bare_flags != nullptr && !toks[i].quoted) {
                bare_flags->push_back(toks[i].value);
            } else {
                diag.error(line, "unexpected token '" + toks[i].value + "'");
            }
            continue;
        }
        out[current].push_back(toks[i].value);
    }
    return out;
}

[[nodiscard]] int parse_version(const std::string& tok, int line, Diagnostics& diag) {
    std::string digits = tok;
    if (!digits.empty() && (digits[0] == 'v' || digits[0] == 'V')) { digits.erase(0, 1); }
    if (digits.empty()) {
        diag.error(line, "expected a version like 'v3'");
        return 0;
    }
    for (char c : digits) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            diag.error(line, "version '" + tok + "' is not a number");
            return 0;
        }
    }
    return std::stoi(digits);
}

}  // namespace

/// Decimal engineering value to milli-units. `7.1` becomes 7100.
///
/// Floating point is used HERE, on the host, at build time, and nowhere else. The device compares integers only,
/// which is what lets the binding layer run on a part with no FPU and makes its arithmetic trivially reviewable.
std::int32_t parse_milli(const std::string& text, Diagnostics& diag, int line) {
    if (text.empty()) {
        diag.error(line, "expected a number");
        return 0;
    }
    try {
        std::size_t used = 0;
        const double v = std::stod(text, &used);
        if (used != text.size()) {
            diag.error(line, "'" + text + "' is not a plain number; units belong in the channel's 'unit' field");
            return 0;
        }
        const double scaled = v * 1000.0;
        if (scaled > 2147483.0 * 1000.0 || scaled < -2147483.0 * 1000.0) {
            diag.error(line, "'" + text + "' is out of range for milli-unit integers");
            return 0;
        }
        return static_cast<std::int32_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
    } catch (const std::exception&) {
        diag.error(line, "'" + text + "' is not a number");
        return 0;
    }
}

/// `5s`, `500ms`, `2m` to milliseconds. A bare number is milliseconds.
std::uint32_t parse_duration_ms(const std::string& text, Diagnostics& diag, int line) {
    std::string digits = text;
    std::uint32_t multiplier = 1;
    auto ends_with = [&](const std::string& suffix) {
        return digits.size() > suffix.size() &&
               digits.compare(digits.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with("ms")) {
        digits.resize(digits.size() - 2);
    } else if (ends_with("s")) {
        digits.resize(digits.size() - 1);
        multiplier = 1000;
    } else if (ends_with("m")) {
        digits.resize(digits.size() - 1);
        multiplier = 60000;
    } else if (ends_with("h")) {
        digits.resize(digits.size() - 1);
        multiplier = 3600000;
    }
    try {
        std::size_t used = 0;
        const double v = std::stod(digits, &used);
        if (used != digits.size() || v < 0.0) {
            diag.error(line, "'" + text + "' is not a duration; write 500ms, 5s, 2m, 1h or a plain millisecond count");
            return 0;
        }
        const double ms = v * static_cast<double>(multiplier);
        if (ms > 4294967.0 * 1000.0) {
            diag.error(line, "'" + text + "' is longer than the 32-bit millisecond counter can express");
            return 0;
        }
        return static_cast<std::uint32_t>(ms + 0.5);
    } catch (const std::exception&) {
        diag.error(line, "'" + text + "' is not a duration; write 500ms, 5s, 2m, 1h or a plain millisecond count");
        return 0;
    }
}

/// Resolve a `when` list into an explicit conjunction or disjunction.
///
/// `when A B` reads as conjunction to any English speaker -- "start when the valve is certified and the pump has
/// failed" -- and means the opposite. I wrote exactly that mistake in the Piper Alpha casebook and it licensed
/// starting a pump without a pressure-safety-valve certificate. The compiler accepted it because it was well formed.
///
/// So a bare `when` with more than one condition is now an error rather than a default. The author must write `any`
/// or `all`. A single condition needs no qualifier because it cannot be misread.
///
/// Returns true if the list is conjunctive.
bool resolve_when(std::vector<std::string>& items, const std::string& owner, Diagnostics& diag, int line) {
    if (items.empty()) { return false; }

    if (items[0] == "any" || items[0] == "all") {
        const bool conjunctive = items[0] == "all";
        items.erase(items.begin());
        if (items.empty()) {
            diag.error(line, "'when " + (conjunctive ? std::string("all") : std::string("any")) +
                                 "' in '" + owner + "' needs at least one condition");
        }
        return conjunctive;
    }

    if (items.size() > 1) {
        std::string list;
        for (const std::string& i : items) { list += " " + i; }
        diag.error(line, "'when" + list + "' in '" + owner +
                             "' is ambiguous: write 'when any" + list + "' if any one suffices, or 'when all" +
                             list + "' if all are required. A bare 'when' with several conditions reads as 'and' "
                                    "and would mean 'or', which is not a mistake a safety casebook may make "
                                    "silently");
    }
    return false;
}

/// Levenshtein distance, for did-you-mean suggestions.
///
/// A typo'd keyword is the commonest authoring mistake there is, and telling somebody that `observ` is
/// "unrecognised" leaves them to spot the missing letter unaided. Two rows of a DP table is a small price for
/// turning that into "did you mean 'observe'?".
std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) { prev[j] = j; }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0U : 1U;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[b.size()];
}

/// "did you mean 'x'?", or an empty string when nothing is close enough to be worth guessing.
std::string did_you_mean(const std::string& word, const std::vector<std::string>& candidates) {
    // Returns a sentence ending in its own punctuation, or ". " when nothing is close enough, so callers can
    // concatenate without producing "'observe'?. A line must...".
    std::string best;
    std::size_t best_distance = 0;
    for (const std::string& c : candidates) {
        const std::size_t d = edit_distance(word, c);
        if (best.empty() || d < best_distance) {
            best = c;
            best_distance = d;
        }
    }
    // A distance of more than a third of the word's length is a different word, not a typo.
    const std::size_t tolerance = std::max<std::size_t>(1, word.size() / 3);
    if (best.empty() || best_distance > tolerance) { return "."; }
    return " -- did you mean '" + best + "'?";
}

Casebook parse(const std::string& text, Diagnostics& diag) {
    Casebook cb;
    const std::vector<LogicalLine> lines = logical_lines(text, diag);

    for (const LogicalLine& ll : lines) {
        const std::vector<Token>& t = ll.tokens;
        const std::string& head = t[0].value;
        const int line = ll.line;

        // ── header ──────────────────────────────────────────────────────────
        if (head == "casebook") {
            if (t.size() < 3) {
                diag.error(line, "expected: casebook <name> v<n>");
                continue;
            }
            if (!cb.name.empty()) { diag.error(line, "duplicate 'casebook' declaration"); }
            cb.name = t[1].value;
            cb.version = parse_version(t[2].value, line, diag);
            continue;
        }

        if (head == "source") {
            // source "<doc>" dated <date> by "<author>"
            cb.provenance.line = line;
            for (std::size_t i = 1; i < t.size(); ++i) {
                if (t[i].value == "dated" && !t[i].quoted && i + 1 < t.size()) {
                    cb.provenance.date = t[++i].value;
                } else if (t[i].value == "by" && !t[i].quoted && i + 1 < t.size()) {
                    cb.provenance.author = t[++i].value;
                } else if (cb.provenance.document.empty()) {
                    cb.provenance.document = t[i].value;
                } else {
                    diag.error(line, "unexpected token '" + t[i].value + "' in source declaration");
                }
            }
            if (cb.provenance.document.empty()) { diag.error(line, "source needs a document name"); }
            continue;
        }

        if (head == "note") {
            if (t.size() < 2) {
                diag.error(line, "note needs text");
                continue;
            }
            for (std::size_t i = 1; i < t.size(); ++i) { cb.provenance.notes.push_back(t[i].value); }
            continue;
        }

        // ── observations ────────────────────────────────────────────────────
        if (head == "observe") {
            if (t.size() < 2) {
                diag.error(line, "expected: observe <name> \"<description>\"");
                continue;
            }
            Observe o;
            o.name = t[1].value;
            o.line = line;
            if (t.size() >= 3) { o.description = t[2].value; }
            if (t.size() > 3) { diag.error(line, "observe takes a name and one description"); }
            cb.observes.push_back(std::move(o));
            continue;
        }

        // ── timed readings ──────────────────────────────────────────────────
        //
        //   reading <name> "<description>" when <source> within <T> after <earlier>
        //   reading <name> "<description>" when <source> within last <T>
        //   reading <name> "<description>" when <source> <N> times in <T>
        //
        // Parsed by hand, not through sections(): the clause is a sentence with a fixed shape, and an author who gets
        // the shape wrong deserves to be shown all three.
        if (head == "reading") {
            const std::string shapes =
                "write one of: `reading <name> \"<description>\" when <reading> within 10s after <reading>`, "
                "`... when <reading> within last 5m`, or `... when <reading> 3 times in 1h`";
            TimedReading r;
            r.line = line;
            if (t.size() < 2 || t[1].quoted) {
                diag.error(line, "expected a name after 'reading'; " + shapes);
                continue;
            }
            r.name = t[1].value;
            std::size_t i = 2;
            if (i < t.size() && t[i].quoted) { r.description = t[i++].value; }
            if (i >= t.size() || t[i].quoted || t[i].value != "when") {
                diag.error(line, "reading '" + r.name + "' needs a 'when' clause saying how time sets it; " + shapes);
                continue;
            }
            ++i;
            std::vector<std::string> w;
            for (; i < t.size(); ++i) {
                if (t[i].quoted) {
                    diag.error(line, "unexpected text \"" + t[i].value + "\" in the 'when' clause of '" + r.name + "'");
                    break;
                }
                w.push_back(t[i].value);
            }
            const auto is_count = [](const std::string& x) {
                if (x.empty() || x.size() > 3) { return false; }
                for (char c : x) {
                    if (std::isdigit(static_cast<unsigned char>(c)) == 0) { return false; }
                }
                return true;
            };
            if (w.size() == 5 && w[1] == "within" && w[3] == "after") {
                r.kind = TimedReading::Kind::After;
                r.source = w[0];
                r.window_ms = parse_duration_ms(w[2], diag, line);
                r.after = w[4];
            } else if (w.size() == 4 && w[1] == "within" && w[2] == "last") {
                r.kind = TimedReading::Kind::Recent;
                r.source = w[0];
                r.window_ms = parse_duration_ms(w[3], diag, line);
            } else if (w.size() == 5 && is_count(w[1]) && w[2] == "times" && w[3] == "in") {
                r.kind = TimedReading::Kind::Count;
                r.source = w[0];
                r.times = std::stoi(w[1]);
                r.window_ms = parse_duration_ms(w[4], diag, line);
                if (r.times < 2 || r.times > 16) {
                    diag.error(line, "reading '" + r.name + "' counts " + w[1] +
                                         " times; a count must be from 2 to 16 (for once, use `within last`)");
                }
            } else {
                diag.error(line, "cannot read the 'when' clause of reading '" + r.name + "'; " + shapes);
                continue;
            }
            if (r.window_ms == 0) { diag.error(line, "reading '" + r.name + "' needs a time above zero"); }
            cb.timed.push_back(std::move(r));
            continue;
        }

        if (head == "implies") {
            Implies im;
            im.line = line;
            if (t.size() < 3 || t[1].quoted || t[2].quoted) {
                diag.error(line, "expected: implies <reading> <reading it requires> [\"why\"] -- for example "
                                 "`implies flow_high current_high \"moving water draws motor current\"`");
                continue;
            }
            im.antecedent = t[1].value;
            im.consequent = t[2].value;
            if (t.size() >= 4 && t[3].quoted) { im.description = t[3].value; }
            if (t.size() > 4 || (t.size() == 4 && !t[3].quoted)) {
                diag.error(line, "implies takes exactly two readings and an optional quoted reason. To require "
                                 "several readings, write one `implies` line for each");
            }
            if (im.antecedent == im.consequent) { diag.error(line, "a reading cannot imply itself"); }
            cb.implications.push_back(std::move(im));
            continue;
        }

        if (head == "exclusive") {
            Exclusive e;
            e.line = line;
            for (std::size_t i = 1; i < t.size(); ++i) { e.members.push_back(t[i].value); }
            if (e.members.size() < 2) { diag.error(line, "exclusive needs at least two readings"); }
            cb.exclusives.push_back(std::move(e));
            continue;
        }

        // ── presumptions ────────────────────────────────────────────────────
        if (head == "claim" || head == "excuse" || head == "doubt") {
            if (t.size() < 2) {
                diag.error(line, std::string("expected: ") + head + " <name> \"<description>\" ...");
                continue;
            }
            Presumption p;
            p.kind = head == "claim" ? PresumptionKind::Claim
                                     : (head == "excuse" ? PresumptionKind::Excuse : PresumptionKind::Doubt);
            p.name = t[1].value;
            p.line = line;

            std::size_t next = 2;
            if (t.size() >= 3 && t[2].quoted) {
                p.description = t[2].value;
                next = 3;
            }

            auto sec = sections(t, next, diag, line);
            for (const auto& [key, values] : sec) {
                if (key == "needs") {
                    p.needs = values;
                } else if (key == "when") {
                    std::vector<std::string> items = values;
                    if (resolve_when(items, p.name, diag, line)) {
                        // `when all` is conjunction, which is exactly what `needs` already means. Fold it in rather
                        // than carrying two representations of one idea.
                        for (const std::string& i : items) { p.needs.push_back(i); }
                    } else {
                        p.when = items;
                    }
                } else if (key == "vote") {
                    // `vote 2 of lt_a_high lt_b_high lt_c_high`
                    if (values.size() < 4 || values[1] != "of") {
                        diag.error(line, "'vote' reads 'vote N of a b c' -- a threshold, the word 'of', then at "
                                         "least two things to count");
                    } else {
                        int k = 0;
                        bool numeric = !values[0].empty();
                        for (const char c : values[0]) {
                            if (c < '0' || c > '9') { numeric = false; }
                        }
                        if (numeric) { k = std::atoi(values[0].c_str()); }
                        const int n = static_cast<int>(values.size()) - 2;
                        if (!numeric || k < 1) {
                            diag.error(line, "'vote' needs a threshold of at least 1, as in 'vote 2 of a b c'");
                        } else if (k > n) {
                            diag.error(line, "'vote " + values[0] + " of' lists only " + std::to_string(n) +
                                                 " items, so the threshold can never be met -- this claim is dead");
                        } else if (k == n) {
                            // Not an error, but it is conjunction written the long way, and `needs` says it plainly.
                            diag.error(line, "'vote " + values[0] + " of' over " + std::to_string(n) +
                                                 " items is conjunction; write 'needs' instead");
                        } else {
                            p.vote_threshold = k;
                            p.vote_items.assign(values.begin() + 2, values.end());
                        }
                    }
                } else if (key == "severity") {
                    if (values.size() != 1) {
                        diag.error(line, "'severity' takes one number, 1 to 10, from the FMEA's severity column");
                    } else {
                        try {
                            const int v = std::stoi(values[0]);
                            if (v < 1 || v > 10) {
                                diag.error(line, "severity " + values[0] +
                                                     " is outside the FMEA scale; write a number from 1 to 10");
                            } else {
                                p.severity = v;
                            }
                        } catch (const std::exception&) {
                            diag.error(line, "'" + values[0] + "' is not a severity; write a number from 1 to 10");
                        }
                    }
                } else if (key == "undermines") {
                    if (p.kind != PresumptionKind::Doubt) {
                        diag.error(line, "only a doubt may 'undermines' in its declaration; use a separate "
                                         "attack line");
                    }
                    p.undermines = values;
                } else {
                    diag.error(line, "'" + key + "' is not valid in a " + head + " declaration");
                }
            }
            if (p.needs.empty() && p.when.empty() && p.kind == PresumptionKind::Doubt) {
                diag.warn(line, "doubt '" + p.name + "' has no evidence requirement, so it always fires");
            }
            cb.presumptions.push_back(std::move(p));
            continue;
        }

        // ── actions ─────────────────────────────────────────────────────────
        if (head == "action") {
            Action a;
            a.line = line;
            if (t.size() < 2) {
                diag.error(line, "expected: action <name> [idle] [safe] [when ...]");
                continue;
            }
            a.name = t[1].value;

            std::vector<std::string> flags;
            auto sec = sections(t, 2, diag, line, &flags);
            for (const std::string& f : flags) {
                if (f == "idle") {
                    a.idle = true;
                } else if (f == "safe") {
                    a.safe = true;
                } else {
                    diag.error(line, "unknown action flag '" + f + "' (expected idle or safe)");
                }
            }
            for (const auto& [key, values] : sec) {
                if (key == "when") {
                    std::vector<std::string> items = values;
                    if (resolve_when(items, a.name, diag, line)) {
                        for (const std::string& i : items) { a.needs.push_back(i); }
                    } else {
                        a.when = items;
                    }
                } else if (key == "needs") {
                    a.needs = values;
                } else {
                    diag.error(line, "'" + key + "' is not valid in an action declaration");
                }
            }
            cb.actions.push_back(std::move(a));
            continue;
        }


        if (head == "channel") {
            Channel ch;
            ch.line = line;
            if (t.size() < 2) {
                diag.error(line, "expected: channel <name> \"description\" unit <unit>");
                continue;
            }
            ch.name = t[1].value;
            std::size_t next = 2;
            if (next < t.size() && t[next].quoted) { ch.description = t[next++].value; }

            // Everything else is keyword-led, on this line or on indented continuations.
            for (std::size_t i = next; i < t.size();) {
                const std::string key = t[i].value;
                auto want = [&](std::size_t n) -> bool {
                    if (i + n >= t.size()) {
                        diag.error(line, "'" + key + "' needs " + std::to_string(n) + " value(s)");
                        i = t.size();
                        return false;
                    }
                    return true;
                };

                if (key == "unit") {
                    if (!want(1)) { break; }
                    ch.unit = t[i + 1].value;
                    i += 2;
                } else if (key == "range") {
                    if (!want(2)) { break; }
                    ch.valid_lo = parse_milli(t[i + 1].value, diag, line);
                    ch.valid_hi = parse_milli(t[i + 2].value, diag, line);
                    ch.has_range = true;
                    i += 3;
                } else if (key == "stale_after") {
                    if (!want(1)) { break; }
                    ch.stale_after_ms = parse_duration_ms(t[i + 1].value, diag, line);
                    i += 2;
                } else if (key == "flatline_after") {
                    if (!want(1)) { break; }
                    ch.flatline_after_ms = parse_duration_ms(t[i + 1].value, diag, line);
                    i += 2;
                } else if (key == "epsilon") {
                    if (!want(1)) { break; }
                    ch.flatline_epsilon = parse_milli(t[i + 1].value, diag, line);
                    i += 2;
                } else if (key == "asserts") {
                    // asserts <observation> above|below <value> release <value>
                    //
                    // Checked by hand rather than through `want(4)`, because "'asserts' needs 4 value(s)" is a true
                    // statement that tells an author nothing. The overwhelmingly likely mistake is a forgotten
                    // `release`, and that deserves to be named.
                    if (i + 4 >= t.size()) {
                        const std::string obs = i + 1 < t.size() ? t[i + 1].value : std::string("that observation");
                        bool has_release = false;
                        for (std::size_t k = i; k < t.size(); ++k) {
                            if (t[k].value == "release") { has_release = true; }
                        }
                        if (!has_release) {
                            diag.error(line,
                                       "threshold '" + obs +
                                           "' needs a 'release' value: write `asserts " + obs +
                                           " above <on> release <off>`. Hysteresis is not optional -- a bare "
                                           "threshold chatters around its setpoint, and a chattering observation "
                                           "makes the framework oscillate between verdicts, which is the alarm "
                                           "flood this product exists to prevent.");
                        } else {
                            diag.error(line, "incomplete 'asserts' clause for '" + obs +
                                                 "': write `asserts <observation> above|below <on> release <off>`");
                        }
                        i = t.size();
                        break;
                    }
                    Threshold th;
                    th.line = line;
                    th.observation = t[i + 1].value;
                    const std::string sense = t[i + 2].value;
                    if (sense == "above") {
                        th.above = true;
                    } else if (sense == "below") {
                        th.above = false;
                    } else {
                        diag.error(line, "expected 'above' or 'below' after '" + th.observation + "', not '" +
                                             sense + "'");
                    }
                    th.on = parse_milli(t[i + 3].value, diag, line);
                    if (t[i + 4].value != "release") {
                        diag.error(line,
                                   "threshold '" + th.observation +
                                       "' needs a 'release' value. Hysteresis is not optional: a bare threshold "
                                       "chatters around its setpoint, and a chattering observation makes the "
                                       "framework oscillate between verdicts");
                        i = t.size();
                        break;
                    }
                    if (i + 5 >= t.size()) {
                        diag.error(line, "'release' needs a value");
                        break;
                    }
                    th.off = parse_milli(t[i + 5].value, diag, line);
                    i += 6;
                    // Optional on-delay: `... release 85 for 5s`. The condition must be sustained that long.
                    if (i < t.size() && t[i].value == "for") {
                        if (i + 1 >= t.size()) {
                            diag.error(line, "'for' needs a duration, e.g. `for 5s` or `for 500ms`");
                            break;
                        }
                        th.hold_ms = parse_duration_ms(t[i + 1].value, diag, line);
                        if (th.hold_ms == 0) {
                            diag.error(line, "'for' needs a duration above zero; leave it out to assert at once");
                        }
                        i += 2;
                    }
                    ch.thresholds.push_back(std::move(th));
                } else {
                    diag.error(line, "'" + key + "' is not valid in a channel declaration" +
                                     did_you_mean(key, {"unit", "range", "stale_after", "flatline_after", "epsilon",
                                                        "asserts"}) +
                                     " Use one of: unit, range, stale_after, "
                                     "flatline_after, epsilon, asserts.");
                    break;
                }
            }
            cb.channels.push_back(std::move(ch));
            continue;
        }

        if (head == "prefer") {
            Prefer p;
            p.line = line;
            if (t.size() < 2) {
                diag.error(line, "expected: prefer <action> over <action> ...");
                continue;
            }
            p.order.push_back(t[1].value);
            for (std::size_t i = 2; i < t.size(); ++i) {
                if (t[i].value == "over" && !t[i].quoted) {
                    if (i + 1 >= t.size()) {
                        diag.error(line, "'over' needs an action after it");
                        break;
                    }
                    p.order.push_back(t[++i].value);
                } else {
                    diag.error(line, "expected 'over' before '" + t[i].value + "'");
                }
            }
            if (p.order.size() < 2) { diag.error(line, "prefer needs at least two actions"); }
            cb.prefers.push_back(std::move(p));
            continue;
        }

        // ── bare attack declarations: <from> <kind> <to>... ─────────────────
        if (t.size() >= 3 && !t[1].quoted &&
            (t[1].value == "undermines" || t[1].value == "rebuts" || t[1].value == "undercuts")) {
            AttackDecl a;
            a.from = head;
            a.line = line;
            a.kind = t[1].value == "undermines" ? AttackKind::Undermines
                                                : (t[1].value == "rebuts" ? AttackKind::Rebuts
                                                                          : AttackKind::Undercuts);
            for (std::size_t i = 2; i < t.size(); ++i) {
                if (is_section_keyword(t[i])) {
                    diag.error(line, "unexpected '" + t[i].value + "' in an attack declaration");
                    break;
                }
                a.to.push_back(t[i].value);
            }
            if (a.to.empty()) { diag.error(line, "attack declaration needs at least one target"); }
            cb.attacks.push_back(std::move(a));
            continue;
        }

        diag.error(line, "unrecognised declaration '" + head + "'" +
                                 did_you_mean(head, {"casebook", "source", "note", "observe", "exclusive", "implies",
                                                     "claim", "excuse", "doubt", "action", "prefer", "channel",
                                                     "reading"}) +
                                 " A line must begin with one of: casebook, source, note, observe, reading, exclusive, "
                                 "implies, claim, excuse, doubt, action, prefer, channel -- or be indented, to continue the "
                                 "declaration above it.");
    }

    if (cb.name.empty()) { diag.error(0, "missing 'casebook <name> v<n>' declaration"); }
    if (cb.provenance.document.empty()) {
        diag.error(0, "missing 'source' declaration. Write `source \"<document>\" dated <YYYY-MM-DD> by "
                      "\"<author>\"` near the top. Provenance is required, not optional: a casebook with no "
                      "document behind it cannot be reviewed, and an undated one is worthless "
                      "as an audit artefact");
    }

    return cb;
}

}  // namespace quorum::cbc
