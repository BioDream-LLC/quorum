// fmea2cb: turn an FMEA table into a casebook DRAFT.
//
// ── The honest version of "feed it a document" ───────────────────────────────
//
// An FMEA is not prose, it is a table: Item, Function, Failure Mode, Effect, Cause, Severity, Detection, Controls. That is
// structured data, and mapping it mechanically into casebook skeleton is safe because every step is traceable to a cell.
//
//   Failure Mode      -> a claim
//   Cause             -> what evidence the claim needs
//   Severity          -> the severity annotation, which quorum already uses for preference
//   Detection Method  -> the observation that detects it, and a candidate channel
//   Effect            -> a comment, because it informs which action is appropriate
//
// ── What this CANNOT do, and why that is the whole point ─────────────────────
//
// An FMEA does not contain the DEFEATERS, and the defeaters are where the safety value lives.
//
// An FMEA row says "high vibration indicates bearing wear". It does not say "unless the duty was raised in the last hour,
// which explains higher vibration without any deterioration". It does not say "unless the machine was overhauled
// yesterday, which undercuts a progression argument entirely". Those are engineering judgement, they are what a human
// knows and a table does not, and they are exactly what quorum exists to represent.
//
// A casebook generated without defeaters is not a weak casebook, it is a DANGEROUS one -- it is the "relies on absence"
// pattern that was found in the tank-409 casebook, where `continue_transfer` fired whenever nothing happened to object.
//
// So this tool emits `# TODO` markers where a human must supply judgement, and refuses to pretend the result is finished.
// The generated draft is deliberately NOT valid input until those are addressed: cbc's pass 6 will report every claim that
// cannot reach an action, which is precisely what an un-defeated skeleton looks like.
//
// ── Why extraction is the safe part and inference is not ─────────────────────
//
// Reading a table is mechanical and checkable: every line of output cites the row it came from. Inferring defeaters from
// prose would be neither. A language model can produce plausible defeaters, and plausible-but-wrong safety logic that
// reads well is the worst output this product could generate -- it is the documented failure mode of the whole project
// ("answers about the neighbouring argument: plausible, internally consistent, wrong").
//
// The division of labour: a machine extracts what the document states, a human supplies what it does not, and cbc's
// exhaustive validation catches what the human forgot.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "findings.hpp"
#include "worksheet.hpp"
#include "sha256.hpp"
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The RFC 4180 record reader and CSV splitter are SHARED with the HAZOP and LOPA importers, and they are shared
// because this file proved the argument for it after the fact.
//
// worksheet.hpp was extracted with a comment predicting that duplicating the reader "would guarantee the next fix of
// that class lands in only one of them". That had already happened here, unnoticed: when a quoted cell containing a
// bulleted list was fixed in hazop2cb, this importer kept its own std::getline, and a worksheet with two causes in
// one cell lost the second cause AND shifted every later column -- the Detection Method silently became empty, so
// the observation was named "undetected_abnormal" and the real instrument was dropped. Confirmed by running it.
using quorum::worksheet::read_record;
using quorum::worksheet::split_csv;


std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) { ++b; }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) { --e; }
    return s.substr(b, e - b);
}

/// A casebook identifier: lowercase, underscores, nothing else. Deterministic, so the same cell always yields the same
/// name and a regenerated draft can be diffed against a hand-edited one.
std::string ident(const std::string& text) {
    std::string out;
    bool last_us = false;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            last_us = false;
        } else if (!last_us && !out.empty()) {
            out += '_';
            last_us = true;
        }
    }
    while (!out.empty() && out.back() == '_') { out.pop_back(); }
    return out.empty() ? "unnamed" : out;
}

/// Action Priority, from the AIAG & VDA FMEA Handbook (2019).
///
/// WHY THIS TABLE IS WORTH 60 LINES
///
/// The 2019 harmonised handbook REPLACED the Risk Priority Number with Action Priority, because RPN has a documented
/// mathematical problem: multiplying three 1-10 ratings lets a cosmetic defect outrank a failure that hurts
/// somebody. A worked pair, both computed by the code below:
///
///   S=10 O=2 D=7   RPN = 140   AP = HIGH     a safety failure, rarely occurring, hard to detect
///   S=3  O=8 D=8   RPN = 192   AP = MEDIUM   a cosmetic defect, frequent, hard to detect
///
/// RPN ranks the cosmetic issue ABOVE the safety issue. Any team sorting by RPN, or applying an RPN threshold,
/// works on the wrong one first. That is not a hypothetical -- it is the reason the standard changed.
///
/// AP weights Severity first, then Occurrence, then Detection, and never lets a high-severity high-occurrence item
/// fall out of the High band however good the detection is. The bands are reproduced exactly; they are a lookup,
/// not a formula, and cannot be derived.
enum class Ap { Low, Medium, High };

const char* ap_name(const Ap a) {
    switch (a) {
        case Ap::High:   return "HIGH";
        case Ap::Medium: return "MEDIUM";
        case Ap::Low:    return "LOW";
    }
    return "?";
}

/// Band an S, O or D rating. The boundaries are the handbook's, not a convenience.
int s_band(const int v) { return v >= 9 ? 4 : (v >= 7 ? 3 : (v >= 4 ? 2 : (v >= 2 ? 1 : 0))); }
int o_band(const int v) { return v >= 8 ? 4 : (v >= 6 ? 3 : (v >= 4 ? 2 : (v >= 2 ? 1 : 0))); }
int d_band(const int v) { return v >= 7 ? 3 : (v >= 5 ? 2 : (v >= 2 ? 1 : 0)); }

Ap ap_of(const int sev, const int occ, const int det) {
    if (sev < 1 || occ < 1 || det < 1 || sev > 10 || occ > 10 || det > 10) { return Ap::Low; }
    static const Ap table[5][5][4] = {
        // S = 1: nothing to act on, whatever the other two say.
        {{Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Low, Ap::Low},
         {Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Low, Ap::Low},
         {Ap::Low, Ap::Low, Ap::Low, Ap::Low}},
        // S = 2-3
        {{Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Low, Ap::Low},
         {Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Low, Ap::Low},
         {Ap::Low, Ap::Low, Ap::Medium, Ap::Medium}},
        // S = 4-6
        {{Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Low, Ap::Low},
         {Ap::Low, Ap::Low, Ap::Low, Ap::Medium}, {Ap::Low, Ap::Medium, Ap::Medium, Ap::Medium},
         {Ap::Medium, Ap::Medium, Ap::High, Ap::High}},
        // S = 7-8
        {{Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Medium, Ap::Medium},
         {Ap::Medium, Ap::Medium, Ap::Medium, Ap::High}, {Ap::Medium, Ap::High, Ap::High, Ap::High},
         {Ap::High, Ap::High, Ap::High, Ap::High}},
        // S = 9-10
        {{Ap::Low, Ap::Low, Ap::Low, Ap::Low}, {Ap::Low, Ap::Low, Ap::Medium, Ap::High},
         {Ap::Medium, Ap::High, Ap::High, Ap::High}, {Ap::High, Ap::High, Ap::High, Ap::High},
         {Ap::High, Ap::High, Ap::High, Ap::High}},
    };
    return table[s_band(sev)][o_band(occ)][d_band(det)];
}

/// Read a 1-10 rating. Returns 0 when the cell is not a plain rating, which is not an error: plenty of FMEAs grade
/// in words, and a tool that invented a number here would be inventing the analysis.
int rating(const std::string& text) {
    std::string t;
    for (const char c : text) { if (std::isdigit(static_cast<unsigned char>(c)) != 0) { t.push_back(c); } }
    if (t.empty() || t.size() > 2) { return 0; }
    const int v = std::atoi(t.c_str());
    return (v >= 1 && v <= 10) ? v : 0;
}

/// Flatten a cell for emission into a casebook string.
///
/// Reading RFC 4180 records correctly means a cell can now contain newlines, and a quoted .cb string cannot span
/// lines. Without this, fixing the multi-line bug traded a wrong casebook for an unparseable one.
std::string flatten(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!out.empty() && !space) { out += "; "; space = true; }
            continue;
        }
        if (c == '"') { out += '\''; space = false; continue; }
        out.push_back(c);
        space = false;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == ';')) { out.pop_back(); }
    return out;
}

struct Row {
    std::string item;
    std::string function;
    std::string mode;
    std::string effect;
    std::string cause;
    std::string severity;
    std::string detection;
    std::string control;
    std::string occurrence;
    std::string detection_rating;
    std::string rpn;
    std::string action;
    int line = 0;
};

/// Find a column by a set of acceptable header names. FMEA exports differ between tools, so a few spellings are accepted
/// -- but a MISSING column is reported rather than guessed, because silently mapping the wrong column would produce a
/// casebook that looks right and describes a different machine.
int find_column(const std::vector<std::string>& headers, const std::vector<std::string>& names) {
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const std::string h = ident(headers[i]);
        for (const std::string& n : names) {
            if (h == n) { return static_cast<int>(i); }
        }
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: fmea2cb <fmea.csv> [casebook-name]\n"
                     "\n"
                     "Emits a casebook DRAFT on stdout. The draft is deliberately incomplete: an FMEA does not\n"
                     "record defeaters, and a casebook without defeaters relies on absence, which is the failure\n"
                     "mode this product exists to prevent. Every `# TODO` marks judgement a human must supply.\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::string name = argc > 2 ? argv[2] : "imported-fmea";
    std::string report_path;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--report") { report_path = argv[i + 1]; }
    }

    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return 1;
    }

    std::string header_line;
    int skipped = 0;
    int phys = 0;   // physical lines consumed, so a reported line matches the author's file
    bool found_header = false;
    for (int attempt = 0; attempt < 20; ) {
        if (!read_record(in, header_line, &phys)) { break; }
        const std::string probe = ident(header_line);
        if (header_line.empty() || header_line[0] == '#') { continue; }
        ++attempt;
        // HAZOP words are here on purpose. This tool recognises a HAZOP worksheet and points the user at
        // hazop2cb, which it can only do if it STOPS on that document's header rather than walking past it into
        // the data looking for the word "failure" -- which is what the first version of this search did, turning a
        // helpful redirect into "no Failure Mode column found".
        static const char* const header_words[] = {"failure", "mode", "item", "severity", "component",
                                                   "deviation", "guide", "safeguard", "cause", "effect",
                                                   "consequence", "function"};
        bool hit = false;
        for (const char* w : header_words) { if (probe.find(w) != std::string::npos) { hit = true; break; } }
        if (hit) { found_header = true; break; }
        ++skipped;
    }
    if (skipped > 0) {
        std::fprintf(stderr, "  note: skipped %d row(s) above the header (study title, facility, date)\n", skipped);
    }
    if (!found_header) {
        std::fprintf(stderr, "error: '%s' is empty\n", path.c_str());
        return 1;
    }
    const std::vector<std::string> headers = split_csv(header_line);

    const int c_item = find_column(headers, {"item", "component", "part"});
    const int c_func = find_column(headers, {"function"});
    const int c_mode = find_column(headers, {"failure_mode", "mode", "failure"});
    const int c_effect = find_column(headers, {"effect", "effects", "effect_of_failure"});
    const int c_cause = find_column(headers, {"cause", "causes", "root_cause"});
    const int c_sev = find_column(headers, {"severity", "sev", "s"});
    const int c_det = find_column(headers, {"detection_method", "detection", "detectability"});

    const int c_ctl = find_column(headers, {"current_control", "current_controls", "controls", "control"});
    // The scored half of an FMEA. Absent from earlier versions of this importer, which read only the text columns
    // and therefore could not audit the one part of an FMEA that IS mechanically checkable.
    const int c_occ = find_column(headers, {"occurrence", "occ", "o", "probability", "likelihood"});
    // A realistic FMEA carries BOTH a Detection Method (text, which becomes an observation) and a D rating (a
    // number, which feeds the arithmetic). Reading only one column meant a properly filled-in worksheet -- the best
    // kind -- had its ratings ignored and no risk analysis done at all.
    const int c_dnum = find_column(headers, {"d", "det", "detection_rating", "detection_ranking",
                                             "detectability_rating", "detectability_ranking"});
    const int c_rpn = find_column(headers, {"rpn", "risk_priority_number", "risk_priority"});
    const int c_act = find_column(headers, {"recommended_action", "recommended_actions", "action", "actions",
                                            "corrective_action", "corrective_actions", "mitigation"});

    if (c_mode < 0) {
        // Is this actually a HAZOP rather than an FMEA?
        //
        // Worth distinguishing, because a generic "no Failure Mode column" is useless advice to someone
        // holding a perfectly good HAZOP -- and a HAZOP is a BETTER input to a casebook than an FMEA is.
        // IEC 61882 worksheets carry Deviation / Cause / Consequence / Safeguards / Action, and each of
        // those maps onto this language more naturally than an FMEA's columns do:
        //
        //   Deviation    a process excursion -- "High level", "No flow"     -> observe
        //   Cause        what would produce it, to be inferred              -> claim ... needs <deviation>
        //   Consequence  what follows, which is what severity is FOR        -> severity
        //   Safeguards   the protection already in place                    -> the HIDDEN FAILURE question
        //   Action       a recommendation to engineering, not a response     -> not a runtime action
        //
        // The Safeguards column is the one an FMEA never has, and it is the most valuable: a safeguard
        // that is the SAME instrument as the cause is a hidden failure, and HAZOP teams write that down
        // in plain words -- "None, LT-101 is the only measurement". That is the CAPECO accident, recorded
        // in the document, before it happened. quorum's own tank casebook came from a HAZOP.
        //
        // Recognised and explained rather than imported, because a rushed HAZOP importer that produced a
        // plausible-looking casebook would be worse than none. The mobile X-ray FMEA already demonstrated
        // what silent, confident, wrong output costs.
        const int c_dev  = find_column(headers, {"deviation", "guide_word", "guideword"});
        const int c_safe = find_column(headers, {"safeguards", "safeguard", "existing_safeguards",
                                                 "protection", "protections"});
        if (c_dev >= 0 || c_safe >= 0) {
            std::fprintf(stderr,
                "error: this looks like a HAZOP worksheet, not an FMEA. Headers seen:");
            for (const std::string& h : headers) { std::fprintf(stderr, " '%s'", h.c_str()); }
            std::fprintf(stderr,
                "\n"
                "\n       A HAZOP is a BETTER starting point than an FMEA, and this importer cannot yet read one."
                "\n       The mapping it would use, which you can apply by hand today:"
                "\n"
                "\n         Deviation    a process excursion (High level, No flow)    ->  observe"
                "\n         Cause        what would produce it, to be inferred        ->  claim ... needs <deviation>"
                "\n         Consequence  what follows if nothing intervenes           ->  severity"
                "\n         Safeguards   the protection already in place              ->  see below"
                "\n         Action       a recommendation to engineering              ->  NOT a runtime action"
                "\n"
                "\n       Read the Safeguards column first. An FMEA never has one, and it is where a HAZOP team"
                "\n       writes down, in plain words, that the only protection against a deviation is the very"
                "\n       instrument whose failure causes it -- 'None, LT-101 is the only measurement'. That is a"
                "\n       hidden failure: the absence of an alarm is not evidence of a safe level. It is also the"
                "\n       CAPECO accident, recorded in the document years before the tank overflowed.\n");
            return 1;
        }

        std::fprintf(stderr, "error: no 'Failure Mode' column found. Headers seen:");
        for (const std::string& h : headers) { std::fprintf(stderr, " '%s'", h.c_str()); }
        std::fprintf(stderr, "\n       A failure mode becomes a claim, so without it there is nothing to import.\n");
        return 1;
    }

    std::vector<Row> rows;
    std::string line;
    int lineno = phys;   // the header's own last line; each record below advances it by what it consumes
    int consumed = 0;
    while (read_record(in, line, &consumed)) {
        const int row_start = lineno + 1;
        lineno += consumed;
        consumed = 0;
        if (trim(line).empty()) { continue; }
        const std::vector<std::string> f = split_csv(line);
        const auto at = [&f](int i) { return i >= 0 && i < static_cast<int>(f.size()) ? trim(f[static_cast<std::size_t>(i)]) : std::string{}; };
        Row r;
        r.item = at(c_item);
        r.function = at(c_func);
        r.mode = at(c_mode);
        r.effect = at(c_effect);
        r.cause = at(c_cause);
        r.severity = at(c_sev);
        r.detection = at(c_det);
        r.occurrence = at(c_occ);
        r.detection_rating = at(c_dnum >= 0 ? c_dnum : c_det);
        r.rpn = at(c_rpn);
        r.action = at(c_act);
        r.control = at(c_ctl);
        r.line = row_start;
        if (!r.mode.empty()) { rows.push_back(r); }
    }
    // Is the detection column a METHOD or a SCORE?
    //
    // This importer was built assuming "Detection" names an instrument -- "Vibration monitoring",
    // "Bearing RTD" -- because that is what I put in the synthetic FMEA I tested against. In standard
    // AIAG practice Detection is a 1-10 DETECTABILITY RATING, and the method, if recorded at all,
    // lives in a separate "Current Controls" column.
    //
    // A real published FMEA found this: Sci Rep 2025 doi 10.1038/s41598-025-09518-6, 22 mobile X-ray
    // machine failure modes with numeric O/S/D and no method column anywhere. The importer read the
    // ratings 3..8 as method names and manufactured observations called `3_abnormal`, `4_abnormal`,
    // described as "3 reports abnormal". It printed "14 failure modes -> 6 observations" and did not
    // warn. cbc then ACCEPTED those identifiers, validated 64 states at 100% Decided, and passed the
    // claim-reach check. The only thing that refused the casebook was the defeasibility gate.
    //
    // Silent, plausible-looking output is the exact defect class this whole project exists to catch,
    // sitting in this project's own importer.
    bool det_is_score = false;
    if (c_det >= 0) {
        std::size_t numeric = 0, total = 0;
        for (const Row& r : rows) {
            const std::string& v = r.detection;
            if (v.empty()) { continue; }
            ++total;
            bool all_digits = true;
            for (const char ch : v) {
                if (std::isdigit(static_cast<unsigned char>(ch)) == 0 && ch != '.' && ch != ' ') {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits) { ++numeric; }
        }
        det_is_score = (total > 0 && numeric == total);
    }
    // ── RISK ANALYSIS ───────────────────────────────────────────────────────
    //
    // This runs BEFORE the refusal below, and that ordering is the point. A Detection column holding scores makes an
    // FMEA useless for generating observations -- you cannot take a reading called "7" -- and simultaneously makes it
    // AUDITABLE, because the numbers are what the arithmetic checks need.
    //
    // MISSION.md ranks the FMEA last of the three documents. That ranking is right for casebook generation and was
    // wrong as a general claim: an FMEA states S, O, D and RPN, so like a LOPA it asserts a conclusion and shows its
    // working, and a machine can redo the working and disagree.
    using quorum::findings::Finding;
    using quorum::findings::Report;
    using quorum::findings::Weight;
    Report report;
    report.check("RPN ARITHMETIC", "does the stated RPN equal Severity x Occurrence x Detection?");
    report.check("RANK INVERSION", "does a lower-RPN row carry a HIGHER Action Priority than a higher-RPN row?");
    report.check("NO ACTION", "is an Action Priority HIGH row left without a recommended action?");
    report.check("DETECTION AS SCORE", "does the Detection column hold ratings where a method is needed?");
    report.limit("Whether an S, O or D rating is RIGHT. Those are the team's judgement and no tool can audit them. "
                 "Only the arithmetic and the priority derived from them are checked.");
    report.limit("Whether the list of failure modes is COMPLETE. A mode nobody wrote down cannot be checked, and "
                 "completeness has no mechanical test.");
    report.limit("Whether a recommended action is ADEQUATE, or was ever carried out. Only its presence is checked.");
    report.limit("An FMEA has no Safeguards column, so the protection-layer questions this project asks of a HAZOP "
                 "or a LOPA cannot be asked of it at all. That is the reason an FMEA is the weakest of the three "
                 "documents for building a safety case, whatever its numbers support.");

    {
        struct Scored { int line; std::string mode; int sev; int occ; int det; int rpn; Ap ap; bool has_action; };
        std::vector<Scored> scored;
        int rpn_wrong = 0;
        for (const Row& r : rows) {
            const int sv = rating(r.severity);
            const int ov = rating(r.occurrence);
            const int dv = rating(r.detection_rating);
            if (sv == 0 || ov == 0 || dv == 0) { continue; }
            const int product = sv * ov * dv;
            const Ap a = ap_of(sv, ov, dv);
            const bool acted = !trim(r.action).empty();
            scored.push_back(Scored{r.line, r.mode, sv, ov, dv, product, a, acted});

            const int stated = std::atoi(trim(r.rpn).c_str());
            if (!trim(r.rpn).empty() && stated != product) {
                ++rpn_wrong;
                char det[320];
                std::snprintf(det, sizeof(det),
                              "S=%d  O=%d  D=%d\nS x O x D = %d\nthe row states RPN = %d",
                              sv, ov, dv, product, stated);
                std::fprintf(stderr, "  RPN ARITHMETIC line %d: %d x %d x %d = %d, but the row states %d\n",
                             r.line, sv, ov, dv, product, stated);
                report.add(Finding{"RPN ARITHMETIC", Weight::Arithmetic, r.line, r.mode, det,
                    "The document supplies both the ratings and the product, and they disagree. An RPN is entered "
                    "by hand or by a formula that someone later dragged over the wrong range, and a wrong one "
                    "survives review because the cell looks exactly like a right one.",
                    "Recomputing the row. Whichever value is wrong, any sorting or thresholding done on this "
                    "column used the stated figure."});
            }
        }

        // ── RANK INVERSION: the documented reason the standard changed ────────
        for (const Scored& hi : scored) {
            for (const Scored& lo : scored) {
                if (hi.rpn <= lo.rpn) { continue; }
                if (!(lo.ap == Ap::High && hi.ap != Ap::High)) { continue; }
                char det[640];
                std::snprintf(det, sizeof(det),
                              "this row      S=%2d O=%2d D=%2d  RPN=%4d  Action Priority %s\n"
                              "               %.60s\n"
                              "outranked     S=%2d O=%2d D=%2d  RPN=%4d  Action Priority %s   (line %d)\n"
                              "               %.60s",
                              hi.sev, hi.occ, hi.det, hi.rpn, ap_name(hi.ap), hi.mode.c_str(),
                              lo.sev, lo.occ, lo.det, lo.rpn, ap_name(lo.ap), lo.line, lo.mode.c_str());
                std::fprintf(stderr,
                    "  RANK INVERSION line %d (RPN %d, AP %s) outranks line %d (RPN %d, AP %s)\n",
                    hi.line, hi.rpn, ap_name(hi.ap), lo.line, lo.rpn, ap_name(lo.ap));
                report.add(Finding{"RANK INVERSION", Weight::Structural, hi.line, hi.mode, det,
                    "RPN ranks this row above the other; Action Priority ranks the other above this one. This is the "
                    "documented mathematical problem with multiplying three 1-10 ratings, and it is why the AIAG & "
                    "VDA 2019 handbook replaced RPN with Action Priority: the product lets a frequent minor defect "
                    "outrank a rare severe one. A team sorting by RPN, or applying an RPN cutoff, works on the wrong "
                    "row first.",
                    "Prioritising by Action Priority rather than by RPN, or recording why RPN order was preferred "
                    "for this study. Both rows may well deserve work; the question is which one first."});
                break;  // one example per row is enough to make the point
            }
        }

        // ── NO ACTION on an Action Priority HIGH row ──────────────────────────
        for (const Scored& r : scored) {
            if (r.ap != Ap::High || r.has_action) { continue; }
            char det[320];
            std::snprintf(det, sizeof(det),
                          "S=%d  O=%d  D=%d   RPN=%d   Action Priority HIGH\nrecommended action: (blank)",
                          r.sev, r.occ, r.det, r.rpn);
            std::fprintf(stderr, "  NO ACTION      line %d: Action Priority HIGH with no recommended action"
                                 " (RPN %d)\n", r.line, r.rpn);
            report.add(Finding{"NO ACTION", Weight::Structural, r.line, r.mode, det,
                "The handbook requires an Action Priority HIGH item to carry either an action or a documented "
                "justification that the existing controls are acceptable. A blank cell is neither. Note the RPN: if "
                "this study applied an RPN threshold, this row may have fallen below it while still being the "
                "highest priority the standard recognises.",
                "An action, or a recorded reason the current controls are accepted. The standard treats those as "
                "equally valid and a blank as neither."});
        }
        if (!scored.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%zu row(s) carry a full S, O and D rating", scored.size());
            report.confirmed("ratings read", buf);
            int ok = 0, with_rpn = 0;
            for (const Row& r : rows) {
                if (trim(r.rpn).empty()) { continue; }
                ++with_rpn;
                const int sv = rating(r.severity), ov = rating(r.occurrence), dv = rating(r.detection_rating);
                if (sv != 0 && ov != 0 && dv != 0 && std::atoi(trim(r.rpn).c_str()) == sv * ov * dv) { ++ok; }
            }
            if (with_rpn > 0) {
                std::snprintf(buf, sizeof(buf), "%d of %d stated RPN(s) equal S x O x D", ok, with_rpn);
                report.confirmed("RPN arithmetic", buf);
            }
            int high = 0;
            for (const Scored& r : scored) { if (r.ap == Ap::High) { ++high; } }
            std::snprintf(buf, sizeof(buf), "%d of %zu row(s) are Action Priority HIGH under AIAG-VDA 2019",
                          high, scored.size());
            report.confirmed("action priority", buf);
        }
        (void)rpn_wrong;
    }

    if (det_is_score) {
        report.add(Finding{"DETECTION AS SCORE", Weight::Structural, 0,
            headers[static_cast<std::size_t>(c_det)],
            "every value in this column is a number",
            "A detectability rating says how likely a failure is to be noticed. It does not say WHICH INSTRUMENT "
            "would notice it, and an observation has to name a reading somebody can actually take. Importing the "
            "ratings would produce observations called '7_abnormal', which is meaningless and would still compile -- "
            "which is exactly what an earlier version of this tool did to a published mobile X-ray FMEA, reporting "
            "success. The ratings are still used for the risk analysis above; they cannot become observations.",
            "A column naming the detection METHOD or the current controls -- 'Vibration monitoring', 'Bearing RTD', "
            "'Annual survey'. The scored columns are enough to audit the risk numbers and not enough to build a "
            "casebook."});
    }

    if (!report_path.empty()) {
        std::ifstream raw(path, std::ios::binary);
        std::ostringstream all;
        all << raw.rdbuf();
        const std::string sha = quorum::sha256::hex(all.str());
        const std::string prov =
            "This review was produced mechanically from the worksheet named above. No column was renamed and no\n"
            "cell was retyped on the way in -- a finding derived from a transcription is a finding about the\n"
            "transcription.\n"
            "\n"
            "Action Priority is computed from the lookup tables in the AIAG & VDA FMEA Handbook (2019), which\n"
            "replaced the Risk Priority Number. Where this review disagrees with an RPN ranking, it is the\n"
            "standard disagreeing, not a preference.";
        const std::string cmd = "fmea2cb " + path + " " + name + " --report " + report_path;
        if (!report.markdown(report_path, path, sha, "fmea2cb", prov, cmd)) {
            std::fprintf(stderr, "error: cannot write report to %s\n", report_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "\n  report written to %s  (%zu finding(s), document SHA-256 %.16s...)\n\n",
                     report_path.c_str(), report.size(), sha.c_str());
    }

    if (det_is_score) {
        std::fprintf(stderr,
            "error: the '%s' column holds SCORES, not detection methods -- every value is a number.\n"
            "       A detectability rating says how likely a failure is to be noticed. It does NOT say\n"
            "       WHICH INSTRUMENT would notice it, and an observation has to name a reading someone\n"
            "       can actually take. Importing the ratings would produce observations called\n"
            "       '7_abnormal', which is meaningless and would still compile.\n"
            "\n"
            "       Supply a column naming the detection METHOD or the current controls -- 'Vibration\n"
            "       monitoring', 'Bearing RTD', 'Annual survey' -- or author the observations by hand.\n"
            "       Refusing here rather than emitting a plausible casebook from unusable input.\n",
            headers[static_cast<std::size_t>(c_det)].c_str());
        // With --report the tool is being used as an auditor, and the audit above is valid: the ratings are exactly
        // what the risk arithmetic needs. No casebook is written either way. Exiting 0 here reports "the review
        // succeeded and the casebook was refused", which is the truth; exiting 1 would report that nothing worked.
        return report_path.empty() ? 1 : 0;
    }

    if (rows.empty()) {
        std::fprintf(stderr, "error: no data rows with a failure mode\n");
        return 1;
    }

    // Distinct detection methods become observations. An FMEA usually names a handful of methods across many rows, and
    // that grouping is the useful signal: rows sharing a detection method share evidence.
    std::map<std::string, std::vector<const Row*>> by_detection;
    for (const Row& r : rows) {
        by_detection[r.detection.empty() ? std::string("undetected") : r.detection].push_back(&r);
    }

    std::printf("casebook %s v1\n", ident(name).c_str());
    std::printf("source \"imported from %s -- FMEA table, %zu failure modes\" dated TODO by \"TODO\"\n", path.c_str(),
                rows.size());
    std::printf("note \"GENERATED DRAFT from an FMEA table. NOT a finished casebook and not yet safe to compile.\"\n");
    std::printf("note \"\"\n");
    std::printf("note \"Every claim below traces to one FMEA row, cited by line number. What an FMEA does NOT\"\n");
    std::printf("note \"record is the DEFEATERS -- the reasons a conclusion does not follow even when its evidence\"\n");
    std::printf("note \"holds. High vibration indicates bearing wear UNLESS the duty was just raised. A table does\"\n");
    std::printf("note \"not say that; an engineer knows it. Those are the TODO items below.\"\n");
    std::printf("note \"\"\n");
    std::printf("note \"A casebook with no defeaters is not merely weak, it decides on the ABSENCE of objections\"\n");
    std::printf("note \"rather than the presence of evidence. That exact pattern was found in this project's own\"\n");
    std::printf("note \"tank casebook and is the failure mode quorum exists to prevent.\"\n\n");

    // ── observations, one per detection method ──────────────────────────────
    std::printf("# ── observations: one per distinct FMEA detection method ────────────────────\n");
    std::printf("# Rows sharing a detection method share evidence, which is why they are grouped this way.\n");
    for (const auto& [det, group] : by_detection) {
        std::printf("observe %-28s \"%s reports abnormal\"   # %zu failure mode(s)\n",
                    (ident(det) + "_abnormal").c_str(), det.c_str(), group.size());
    }
    std::printf("\n");
    std::printf("# TODO: a detection method that is CONTINUOUS (a DCS alarm) should become a `channel` with units,\n");
    std::printf("#       range, stale_after and thresholds, so the device can tell a dead sensor from a quiet one.\n");
    std::printf("#       An annual survey or a monthly route reading cannot be a channel: it is an observation a\n");
    std::printf("#       person asserts, and it goes stale in months rather than seconds.\n");
    for (const auto& [det, group] : by_detection) {
        bool continuous = false;
        for (const Row* r : group) {
            std::string c = r->control;
            std::transform(c.begin(), c.end(), c.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (c.find("continuous") != std::string::npos) { continuous = true; }
        }
        if (continuous) {
            std::printf("#       CANDIDATE CHANNEL: %s (FMEA lists a continuous control)\n", det.c_str());
        }
    }
    std::printf("\n");

    // ── claims, one per failure mode ────────────────────────────────────────
    std::printf("# ── claims: one per FMEA failure mode ──────────────────────────────────────\n");
    for (const Row& r : rows) {
        const std::string claim = ident(r.mode);
        const std::string obs = ident(r.detection.empty() ? std::string("undetected") : r.detection) + "_abnormal";

        std::printf("\n# FMEA line %d", r.line);
        if (!r.item.empty()) { std::printf(" | item: %s", flatten(r.item).c_str()); }
        if (!r.effect.empty()) { std::printf(" | effect: %s", flatten(r.effect).c_str()); }
        std::printf("\n");
        if (!r.cause.empty()) { std::printf("# cause: %s\n", flatten(r.cause).c_str()); }

        std::printf("claim %s \"%s\"", claim.c_str(), flatten(r.mode).c_str());
        if (!r.severity.empty()) {
            bool numeric = true;
            for (const char ch : r.severity) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) { numeric = false; }
            }
            if (numeric) { std::printf(" severity %s", r.severity.c_str()); }
        }
        std::printf("\n  needs %s\n", obs.c_str());

        // The part a table cannot supply.
        std::printf("# TODO defeater: what makes '%s' NOT follow even when %s holds?\n",
                    flatten(r.mode).c_str(), obs.c_str());
        if (!r.cause.empty()) {
            std::printf("#   The FMEA gives the cause as '%s'. If something else produces the same reading,\n",
                        r.cause.c_str());
            std::printf("#   that is an EXCUSE and it rebuts this claim. If something makes the inference itself\n");
            std::printf("#   unsound (a recent overhaul, a known test in progress), that UNDERCUTS it.\n");
        }
        std::printf("#   <excuse_observation> rebuts %s\n", claim.c_str());
    }

    // ── actions ─────────────────────────────────────────────────────────────
    std::printf("\n# ── actions ────────────────────────────────────────────────────────────────\n");
    std::printf("# TODO: an FMEA records effects, not responses, so these are placeholders.\n");
    std::printf("# Severity is the guide: 8+ generally warrants stopping, 4-7 a maintenance response.\n");

    std::set<std::string> high;
    std::set<std::string> mid;
    for (const Row& r : rows) {
        int sev = 0;
        for (const char ch : r.severity) {
            if (std::isdigit(static_cast<unsigned char>(ch))) { sev = sev * 10 + (ch - '0'); }
        }
        if (sev >= 8) { high.insert(ident(r.mode)); }
        else if (sev > 0) { mid.insert(ident(r.mode)); }
    }

    if (!high.empty()) {
        std::printf("action stop      safe when any");
        for (const std::string& c : high) { std::printf(" %s", c.c_str()); }
        std::printf("\n");
    }
    if (!mid.empty()) {
        std::printf("action maintain  when any");
        for (const std::string& c : mid) { std::printf(" %s", c.c_str()); }
        std::printf("\n");
    }
    std::printf("action run       idle\n");
    if (!high.empty() && !mid.empty()) {
        std::printf("prefer stop over maintain over run\n");
    } else if (!high.empty()) {
        std::printf("prefer stop over run\n");
    } else if (!mid.empty()) {
        std::printf("prefer maintain over run\n");
    } else {
        // NEITHER bucket has anything, which happens when the table has no severity column at all.
        //
        // This branch was missing, and a real published FMEA found it: 20 wind turbine drivetrain
        // failure modes from Sci Rep 2026 (doi 10.1038/s41598-026-63651-4), whose table records
        // failure type, impact, cause and category -- and no severity, no detection, no controls.
        // The old code fell into the `mid` case unconditionally and emitted `prefer maintain over
        // run` for an action that had never been declared, so the draft did not compile at all.
        //
        // It went unnoticed because every FMEA I tested against was one I wrote, and I always gave
        // it a severity column. Three cases were handled; the fourth needed someone else's document.
        //
        // No preference is emitted, because with no severity there is no declared basis for one.
        // The draft still compiles, which matters: cbc's own claim-reach pass will then report all
        // of these claims as NO PATH, and that report is the useful output -- an FMEA that records
        // no severity and no detection cannot say what to DO, only what can go wrong.
        std::printf("\n# TODO: this table declared NO SEVERITY, so nothing above chose a response and `run` is the\n");
        std::printf("#       only action. Every claim will be reported as NO PATH by cbc, and that is accurate\n");
        std::printf("#       rather than a defect in the import: a table of failure modes with no severity and\n");
        std::printf("#       no detection method records what can go wrong and NOT what to do about it. Supply\n");
        std::printf("#       severities, or decide the responses directly, before this is a casebook.\n");
    }

    std::printf("\n# TODO: is `stop` the right safe state for this asset? It is declared, never assumed --\n");
    std::printf("#       a pump stops, a tank stops filling, and a drone must keep flying.\n");

    std::fprintf(stderr, "%zu failure modes -> %zu observations, %zu claims.\n", rows.size(), by_detection.size(),
                 rows.size());
    std::fprintf(stderr, "DRAFT ONLY: %zu defeaters must be supplied by an engineer. Run cbc on the result and read\n",
                 rows.size());
    std::fprintf(stderr, "the 'claim reach' section -- every claim with NO PATH is one this table could not finish.\n");
    return 0;
}
