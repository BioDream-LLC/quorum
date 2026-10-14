// hazop2cb -- read an IEC 61882 HAZOP worksheet and draft a casebook from it.
//
// WHY THIS EXISTS, AND WHY IT IS SEPARATE FROM fmea2cb
//
// A HAZOP is a better starting point for a casebook than an FMEA, and it took feeding this project four real
// third-party documents to notice. The reason is structural rather than a matter of quality:
//
//   Deviation    a process excursion -- "High level", "No flow"     ->  observe   (ALREADY an observation)
//   Cause        what would produce it, to be inferred             ->  claim ... needs <deviation>
//   Consequence  what follows if nothing intervenes                ->  severity  (a hint; a human decides)
//   Safeguards   the protection already in place                   ->  the whole point; see below
//   Action       a recommendation to engineering                   ->  a comment, NOT a runtime action
//
// An FMEA's failure mode has to be INFERRED into an observation. A HAZOP's deviation already is one: it is a named
// process parameter excursion, which is exactly what `observe` means. And a HAZOP has a Safeguards column, which no
// FMEA has, and which is where a team records in plain words what already protects against the deviation -- and
// therefore also records when nothing does:
//
//   High level | LT-101 failed low | Overflow undetected | None - LT-101 is the only measurement
//
// That is the CAPECO explosion, written into the document years before the tank overflowed. The Safeguards analysis
// below is the reason this tool is worth having; the claim generation is the easy part.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not invent defeaters, severities, or actions. A HAZOP records what can go wrong and what protects against
// it; it does not record why a conclusion might not follow, and it does not record a runtime response. Those are
// engineering judgement and they are emitted as TODOs. The output is a DRAFT that cbc will refuse -- by design, and
// verified by casebook_fmea_draft_refused.
//
// Every heuristic here reports rather than guesses. The mobile X-ray FMEA demonstrated what silent, confident, wrong
// output costs: it turned detectability ratings into observations called `3_abnormal`, reported success, and passed
// every gate but one.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "findings.hpp"
#include "sha256.hpp"
#include "worksheet.hpp"

namespace {

using quorum::findings::Finding;
using quorum::findings::Report;
using quorum::findings::Weight;
using quorum::worksheet::find_column;
using quorum::worksheet::ident;
using quorum::worksheet::lower;
using quorum::worksheet::read_record;
using quorum::worksheet::split_csv;
using quorum::worksheet::tags;







/// A severity column graded in words, mapped onto the 1-10 scale as a SUGGESTION.
///
/// WHY: a real worksheet often grades severity already -- the five-point scale Insignificant / Minor / Moderate /
/// Major / Catastrophic is what the published hospital-oxygen study used, and it comes straight from the standard
/// 5x5 risk matrix. The importer used to ignore the column entirely and tell the author to invent severities from
/// nothing, which throws away work the HAZOP team has already done and argued over.
///
/// It is emitted as a SUGGESTION in a TODO, never written into the casebook. Two reasons. Severity in this language
/// decides which claims can rebut which, so getting it wrong silently changes what the device does; and PROC-D7 in
/// docs/assumptions.tsv records what happened last time this tool prescribed a remedy without measuring it.
/// The mapping below is a defensible reading of a 5-point scale onto 1-10, not a standard.
int severity_hint(const std::string& text) {
    const std::string t = lower(text);
    if (t.empty()) { return 0; }
    // Numeric first: many worksheets already use 1-10, and some use a 1-5 risk-matrix number.
    bool all_digit = true;
    for (const char c : t) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0 && c != ' ') { all_digit = false; break; }
    }
    if (all_digit) {
        const int n = std::atoi(t.c_str());
        if (n >= 1 && n <= 10) { return n; }
        return 0;
    }
    struct Word { const char* word; int hint; };
    static const Word words[] = {
        {"catastrophic", 10}, {"katastrophal", 10}, {"fatality", 10}, {"fatal", 10}, {"multiple fatalit", 10},
        {"major", 8}, {"severe", 8}, {"critical", 8}, {"gro\u00df", 8}, {"schwer", 8},
        {"serious", 7},
        {"moderate", 5}, {"medium", 5}, {"mittel", 5},
        {"minor", 3}, {"low", 3}, {"gering", 3},
        {"insignificant", 1}, {"negligible", 1}, {"unbedeutend", 1},
    };
    for (const Word& w : words) {
        if (t.find(w.word) != std::string::npos) { return w.hint; }
    }
    return 0;
}

/// Split a cell that packs several causes into one entry, as HAZOP teams routinely do.
///
/// Bullets on separate lines, or separated by ';', or numbered. NOT split on commas: prose uses commas constantly
/// ("valve fails, allowing backflow") and splitting there would invent causes that do not exist.
std::vector<std::string> split_causes(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&]() {
        std::string t = cur;
        // Strip a leading bullet or number marker.
        std::size_t b = 0;
        // A UTF-8 bullet (U+2022) is three bytes, so it cannot be compared as a char; match the sequence.
        for (;;) {
            if (b < t.size() && (t[b] == ' ' || t[b] == '\t' || t[b] == '-' || t[b] == '*')) { ++b; continue; }
            if (b + 2 < t.size() && static_cast<unsigned char>(t[b]) == 0xE2U
                && static_cast<unsigned char>(t[b + 1]) == 0x80U
                && static_cast<unsigned char>(t[b + 2]) == 0xA2U) { b += 3; continue; }
            break;
        }
        std::size_t d = b;
        while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d])) != 0) { ++d; }
        if (d > b && d < t.size() && (t[d] == '.' || t[d] == ')')) { b = d + 1; }
        t = t.substr(b);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) { t.erase(t.begin()); }
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) { t.pop_back(); }
        if (t.size() > 2) { out.push_back(t); }
        cur.clear();
    };
    for (const char c : text) {
        if (c == '\n' || c == ';') { flush(); } else { cur.push_back(c); }
    }
    flush();
    return out;
}

struct Row {
    std::string node;
    std::string deviation;
    std::string cause;
    std::string consequence;
    std::string safeguards;
    std::string action;
    std::string severity;
    std::string mitigative;
    int line = 0;
};

/// Does the Safeguards text say there is no protection at all?
///
/// HAZOP teams write this in a small number of recognisable ways, and it is the single most important thing in the
/// document: an unprotected deviation is not a hazard waiting to be scored, it is a hazard with nothing in the way.
bool safeguard_is_none(const std::string& text) {
    const std::string t = lower(text);
    if (t.empty()) { return true; }
    static const char* const nones[] = {"none", "no safeguard", "nil", "n/a", "na", "-", "not applicable",
                                        // German. The German-speaking world calls this method PAAG, not HAZOP:
                                        // Prognose, Auffinden der Ursache, Abschaetzen der Auswirkungen,
                                        // Gegenmassnahmen. "keine" is how a PAAG team writes "nothing protects
                                        // this", and reading it as unclassifiable prose UNDER-REPORTS the finding.
                                        "keine", "kein", "nicht vorhanden", "entf\u00e4llt", "entfaellt"};
    for (const char* n : nones) {
        if (t.compare(0, std::string(n).size(), n) == 0) { return true; }
    }
    return false;
}

/// Does the safeguard text record that its INTEGRITY is unknown or absent?
///
/// A LOPA assigns every Independent Protection Layer a probability of failure on demand, which rests entirely on it
/// being proof tested. A HAZOP often records the truth in passing -- "assumed tested annually - no record",
/// "never proof tested" -- and that is the definition of a hidden failure: a layer everyone counts on and nobody
/// has demonstrated works.
///
/// This was waved through because the cell also contained a tag. A tagged instrument that has never been tested is
/// not a protection layer, it is an assumption with a part number.
bool integrity_unproven(const std::string& text) {
    const std::string t = lower(text);
    static const char* const bad[] = {"never proof tested", "never tested", "not proof tested", "not tested",
                                      "no record", "no proof test", "untested", "assumed tested",
                                      "test interval unknown", "not verified", "unverified",
                                      "nicht gepr\u00fcft", "ungepr\u00fcft", "kein nachweis"};
    for (const char* k : bad) { if (t.find(k) != std::string::npos) { return true; } }
    return false;
}



/// What KIND of thing is this safeguard?
///
/// WHY THIS EXISTS -- a real document defeated the tag matcher completely.
///
/// The first version of this tool looked for shared instrument tags (LT-101 in both the Cause and Safeguards
/// columns). Run against a published HAZOP of a hospital oxygen plant (Yousofnejad, Afsari & Es'haghi, PLOS ONE
/// 2023, doi:10.1371/journal.pone.0280918, CC BY), it reported ZERO problems -- on a worksheet where 13 of 14
/// safeguards were "visual inspection" and 10 of 14 were "the alarm of the device". Not one row contained an
/// instrument tag, so there was nothing for the matcher to match.
///
/// That is a false negative on a document with a systemic independence failure, which is the precise defect class
/// this project exists to catch. Real HAZOP teams name their safeguards in prose, not in tag numbers.
///
/// The CCPS criteria for an Independent Protection Layer are that it be effective, independent of the initiating
/// event AND of every other layer for the same scenario, and auditable. Judged against those:
///
///   HUMAN       "visual inspection", "operator round" -- depends on a person being present, trained and looking.
///                Not independent of any other human-dependent layer, and hard to audit.
///   PROCEDURAL  "existing instructions", "training" -- a document is not a protection layer; it describes one.
///   ON-DEVICE   "the alarm of the device" -- NOT independent of the initiating event when the initiating event is
///                a failure INSIDE that device. This is the shape that matters most and the easiest to miss.
///   INSTRUMENT  a tagged instrument. Independence then depends on which tag, which the tag matcher handles.
/// Cause text with filler words removed and the rest sorted, so near-identical prose collides.
///
/// WHY: a real worksheet writes the same physical failure slightly differently under each deviation it causes.
/// The hospital-oxygen HAZOP has "Failure and burning of the solenoid valve" under Over pressure of gas and
/// "Failure and burning of solenoid valve" under Over voltage -- one word apart, the same component, the same node.
std::string cause_key(const std::string& text) {
    static const std::set<std::string> filler = {"the", "of", "a", "an", "in", "to", "and", "due", "its", "on",
                                                 "at", "by", "for", "from", "with", "or", "is", "was"};
    std::set<std::string> words;
    std::string cur;
    for (const char c : lower(text)) {
        if (std::isalpha(static_cast<unsigned char>(c)) != 0) { cur.push_back(c); }
        else {
            if (!cur.empty() && filler.count(cur) == 0) { words.insert(cur); }
            cur.clear();
        }
    }
    if (!cur.empty() && filler.count(cur) == 0) { words.insert(cur); }
    std::string out;
    for (const std::string& w : words) { out += w; out.push_back(' '); }
    return out;
}

///   DEFERRED    "TBD", "see note 4", "under review" -- the document is pointing elsewhere or has not decided.
///               NOT protection, and the most dangerous to treat as such: it reads like an entry.
///   UNCLASSIFIED prose none of the keyword lists recognise. Might be a real safeguard; might be anything.
///
/// UNCLASSIFIED exists because mutation testing found the Instrument branch was decorative: deleting it left the
/// whole suite green, because a `Other` fallback gave unrecognised prose exactly the same standing as a tagged
/// instrument. Measured consequence -- "See note 4" and "TBD pending review" were reported as fully protected.
/// A tool that cannot read a safeguard must say so rather than assume the best.
enum class Guard { Human, Procedural, OnDevice, Instrument, Deferred, Unclassified };

const char* guard_name(Guard g) {
    switch (g) {
        case Guard::Human:      return "human inspection";
        case Guard::Procedural: return "procedure or instruction";
        case Guard::OnDevice:   return "the failing device's own alarm";
        case Guard::Instrument:   return "a tagged instrument";
        case Guard::Deferred:     return "a deferral, not a protection";
        case Guard::Unclassified: return "text this tool cannot classify";
    }
    return "unclassified";
}

std::set<Guard> classify_safeguards(const std::string& text) {
    const std::string t = lower(text);
    std::set<Guard> out;
    static const char* const human[] = {"visual inspection", "visual check", "operator round", "operator rounds",
                                        "manual check", "walkdown", "walk down", "observation by operator",
                                        "visual monitoring", "inspection by operator", "daily check",
                                        // German
                                        "field check", "field inspection", "surveillance", "rounds",
                                        "routine rounds", "attended operation", "attended", "patrol",
                                        "shift check", "manned", "operator attention", "spot check",
                                        "sichtpr\u00fcfung", "sichtpruefung", "sichtkontrolle", "begehung",
                                        "regelm\u00e4\u00dfige kontrolle", "manuelle kontrolle"};
    static const char* const proc[]  = {"existing instruction", "instruction", "procedure", "training",
                                        "checklist", "work permit", "logbook", "log book", "sop",
                                        // German
                                        "betriebsanweisung", "arbeitsanweisung", "anweisung", "schulung",
                                        "verfahrensanweisung", "checkliste", "betriebshandbuch"};
    static const char* const dev[]   = {"alarm of the device", "alarm of device", "device alarm", "the alarm of",
                                        "built-in alarm", "internal alarm", "onboard alarm",
                                        // German
                                        "ger\u00e4teeigener alarm", "geraeteeigener alarm", "ger\u00e4tealarm",
                                        "eigener alarm", "interner alarm"};
    for (const char* k : human) { if (t.find(k) != std::string::npos) { out.insert(Guard::Human); break; } }
    for (const char* k : proc)  { if (t.find(k) != std::string::npos) { out.insert(Guard::Procedural); break; } }
    for (const char* k : dev)   { if (t.find(k) != std::string::npos) { out.insert(Guard::OnDevice); break; } }
    // Deferral language. A HAZOP that says "TBD" against a deviation has recorded that nobody decided yet, which
    // is the opposite of a protection layer, and "see note 4" is a cross-reference this tool cannot follow.
    static const char* const defer[] = {"tbd", "to be determined", "to be confirmed", "to be advised",
                                        "see note", "see remark", "refer to", "as above", "under review",
                                        "pending", "not assessed", "not yet", "open action",
                                        // German
                                        "noch festzulegen", "wird festgelegt", "noch offen", "offen",
                                        "siehe anmerkung", "siehe hinweis", "in pr\u00fcfung", "in pruefung"};
    for (const char* k : defer) { if (t.find(k) != std::string::npos) { out.insert(Guard::Deferred); break; } }

    if (!tags(text).empty()) { out.insert(Guard::Instrument); }
    if (out.empty() && !safeguard_is_none(text)) { out.insert(Guard::Unclassified); }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: hazop2cb <worksheet.csv> [casebook-name]\n"
            "\n"
            "Reads an IEC 61882 HAZOP worksheet and drafts a casebook. Columns recognised:\n"
            "  Deviation (required)  Cause (required)  Consequence  Safeguards  Node  Action\n"
            "\n"
            "The output is a DRAFT. cbc will refuse it until an engineer supplies the defeaters, the\n"
            "severities and the safe state, because a HAZOP does not record any of those.\n"
            "\n"
            "Read the SAFEGUARD ANALYSIS on stderr first. That is the part a HAZOP alone cannot give you.\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::string name = argc >= 3 ? argv[2] : "imported_hazop";
    std::string report_path;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--report") { report_path = argv[i + 1]; }
    }

    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "error: cannot open %s\n", path.c_str()); return 1; }

    // Skip blank and '#'-commented preamble before the header row.
    //
    // Real worksheets arrive with provenance above the table -- who did the study, when, under what licence. The
    // first version took line 1 as the header unconditionally, which broke the moment a fixture carried its own
    // citation. Caught by the test suite, not by reasoning about it.
    // SEARCH FORWARD for the header row instead of assuming it is first.
    //
    // A worksheet exported from Excel -- which is how every real one arrives -- carries a banner: a study title, a
    // facility name, a date, a chair, then a blank row, THEN the header. Taking row 1 as the header refused the
    // file outright with the banner text quoted back as though it were column names.
    //
    // A header row is recognised by containing a word this tool knows. Scanning at most 20 records keeps a stray
    // match deep in the data from being mistaken for a header.
    static const char* const header_words[] = {"deviation", "abweichung", "cause", "ursache", "guide word",
                                               "guideword", "safeguard", "consequence", "auswirkung",
                                               "gegenma", "schutzma", "failure mode", "hazard"};
    std::string header_line;
    int skipped = 0;
    int phys = 0;   // physical lines consumed, so a reported line matches the author's file
    bool found_header = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!read_record(in, header_line, &phys)) { break; }
        std::string probe = lower(header_line);
        while (!probe.empty() && (probe.front() == ' ' || probe.front() == '\t')) { probe.erase(probe.begin()); }
        if (probe.empty() || probe[0] == '#') { continue; }
        // A file of binary rubbish is a different problem and deserves a different message.
        if (attempt == 0) {
            for (const char c : header_line.substr(0, 64)) {
                if (c != '\n' && c != '\t' && c != '\r'
                    && (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) == 0x7FU)) {
                    std::fprintf(stderr,
                        "error: %s is not a text file. It looks like a spreadsheet or document saved in its own\n"
                        "       binary format (.xlsx, .xls, .doc, .pdf).\n"
                        "\n"
                        "       Export it as CSV first -- in Excel, File > Save As > CSV UTF-8. Banner rows above\n"
                        "       the table are fine; they are skipped. A cell containing a bulleted list is fine too.\n",
                        path.c_str());
                    return 1;
                }
            }
        }
        bool looks_like_header = false;
        for (const char* w : header_words) {
            if (probe.find(w) != std::string::npos) { looks_like_header = true; break; }
        }
        if (looks_like_header) { found_header = true; break; }
        ++skipped;
    }
    if (!found_header) {
        std::fprintf(stderr, "error: %s has no row that looks like a HAZOP header.\n", path.c_str());
        if (!header_line.empty()) {
            std::fprintf(stderr, "       last row examined: '%.90s'\n", header_line.c_str());
        }
        std::fprintf(stderr,
            "       A header row must name at least a deviation and a cause. See the column names listed\n"
            "       below by running this tool on a file that has a header.\n");
        return 1;
    }
    if (skipped > 0) {
        std::fprintf(stderr, "  note: skipped %d row(s) above the header (study title, facility, date and so on)\n",
                     skipped);
    }
    const std::vector<std::string> headers = split_csv(header_line);

    // German PAAG column names alongside the English. Confirmed from German process-safety sources, not guessed:
    // the acronym PAAG itself spells out Prognose / Auffinden der Ursache / Abschaetzen der Auswirkungen /
    // Gegenmassnahmen. ASCII transliterations are included because teams often write "ss" for eszett.
    const int c_dev  = find_column(headers, {"deviation", "deviations", "abweichung", "abweichungen"},
                                   {{"deviation"}, {"deviations"}, {"abweichung"}, {"abweichungen"}});
    // THE STANDARD HAZOP WORKSHEET HAS NO "DEVIATION" COLUMN.
    //
    // A deviation IS a guide word applied to a process parameter -- that is the whole method. The textbook
    // recording format (IEC 61882, and every worksheet template that follows it) therefore has PARAMETER and
    // GUIDE WORD as separate columns, and the deviation is read aloud as their combination: "No flow",
    // "High level". Requiring a pre-combined Deviation column meant refusing the most standard layout there is.
    //
    // The two must be combined, never used alone. The guide word alone ("High") says nothing about what is high.
    // The parameter alone is worse than useless here: deviations are DEDUPLICATED into observations, so "Level"
    // from a High row and "Level" from a Low row would collapse into ONE observation, silently merging two
    // opposite hazards with opposite responses.
    const int c_param = find_column(headers, {"parameter", "process_parameter", "parameters", "prozessparameter"},
        {{"parameter"}, {"parameters"}, {"prozessparameter"}});
    const int c_guide = find_column(headers, {"guide_word", "guideword", "guide_words", "guidewords",
                                             "leitwort", "suchwort"},
        {{"guide", "word"}, {"guideword"}, {"guidewords"}, {"leitwort"}, {"suchwort"}});
    const int c_cau  = find_column(headers, {"cause", "causes", "possible_causes", "possible_cause",
                                             "ursache", "ursachen", "m\u00f6gliche_ursache", "moegliche_ursache"},
        {{"cause"}, {"causes"}, {"ursache"}, {"ursachen"}});
    const int c_con  = find_column(headers, {"consequence", "consequences", "possible_consequences",
                                             "possible_consequence", "effect", "effects"},
        {{"consequence"}, {"consequences"}, {"effect"}, {"effects"}, {"impact"},
         {"auswirkung"}, {"auswirkungen"}, {"folgen"}, {"folge"}});
    // "Control/Existing" and "Existing Controls" both mean safeguards. "Control/suggested" does NOT -- it is a
    // recommendation, and analysing it as a protection would report layers that have not been built.
    const int c_safe = find_column(headers,
        {"safeguards", "safeguard", "existing_safeguards", "protection", "protections", "existing_controls",
         "controls", "control",
         // German PAAG. `header_tokens` splits on the eszett byte, so "Gegenma\u00dfnahmen" tokenises as
         // {gegenma, nahmen} -- hence the token entries below as well as these exact names.
         "gegenma\u00dfnahmen", "gegenmassnahmen", "schutzma\u00dfnahmen", "schutzmassnahmen",
         "vorhandene_schutzma\u00dfnahmen", "sicherheitsma\u00dfnahmen"},
        {{"safeguard"}, {"safeguards"}, {"existing", "control"}, {"existing", "controls"},
         {"existing", "safeguards"}, {"protection"}, {"protections"}, {"control"}, {"controls"},
         {"gegenma"}, {"schutzma"}, {"gegenmassnahmen"}, {"schutzmassnahmen"}, {"sicherheitsma"}},
        {"suggested", "suggest", "recommended", "recommendation", "proposed", "additional", "further",
         "empfohlen", "empfehlung", "zus\u00e4tzlich", "vorgeschlagen"});
    const int c_node = find_column(headers, {"node", "study_node", "item", "line", "equipment"});
    // A second safeguards column. Industrial HAZOPs (IChemE Hazards 31, Chevron) split PREVENTIVE from MITIGATIVE
    // safeguards, and the distinction is decisive: a mitigative layer does not stop the deviation, it reduces the
    // consequence. Picking one column and ignoring the other produced a FALSE POSITIVE -- a deviation reported
    // UNPROTECTED because its preventive cell said "None", while a relief to flare sat in the next column.
    const int c_mit  = find_column(headers, {"mitigative_safeguards", "mitigating_safeguards", "mitigation"},
        {{"mitigative"}, {"mitigating"}, {"mitigation"}});
    const int c_sev  = find_column(headers,
        {"severity", "sev", "severity_rating", "consequence_severity", "schwere", "ausma\u00df", "schweregrad"},
        {{"severity"}, {"schwere"}, {"schweregrad"}});
    const int c_act  = find_column(headers, {"action", "actions", "recommendation", "recommendations",
                                             "action_required"},
        {{"suggested", "control"}, {"recommended", "control"}, {"recommendation"}, {"recommendations"},
         {"action"}, {"actions"}},
        {"existing"});

    // A Parameter plus a Guide Word IS a deviation, so the file is complete without a Deviation column.
    const bool synth_dev = (c_dev < 0) && (c_param >= 0) && (c_guide >= 0);
    if (synth_dev) {
        std::fprintf(stderr,
            "  note: no Deviation column; combining Guide Word with Parameter, which is what a deviation is\n");
    }
    if ((c_dev < 0 && !synth_dev) || c_cau < 0) {
        std::fprintf(stderr, "error: a HAZOP needs both a Deviation and a Cause column. Headers seen:");
        for (const std::string& h : headers) { std::fprintf(stderr, " '%s'", h.c_str()); }
        std::fprintf(stderr,
            "\n       The deviation becomes an observation and the cause becomes a claim; without both there\n"
            "       is nothing to build. If this is an FMEA rather than a HAZOP, use fmea2cb.\n"
            "\n"
            "       Column names recognised:\n"
            "         deviation   Deviation, Deviations, Abweichung(en)\n"
            "         cause       Cause(s), Possible Cause, Ursache(n)\n"
            "         consequence Consequence(s), Effect(s), Auswirkung(en), Folgen\n"
            "         safeguards  Safeguards, Existing Controls, Control/Existing, Protection,\n"
            "                     Gegenma\u00dfnahmen, Schutzma\u00dfnahmen\n"
            "\n"
            "       German PAAG worksheets are supported (Prognose, Auffinden der Ursache, Absch\u00e4tzen der\n"
            "       Auswirkungen, Gegenma\u00dfnahmen). Other languages are NOT: the safeguard keyword lists are\n"
            "       English and German only, so a worksheet in another language will parse but its safeguards\n"
            "       will all be reported as text this tool cannot classify -- which is honest, and useless.\n");
        if (c_dev < 0 && c_param >= 0 && c_guide < 0) {
            // Reachable and worth explaining: this worksheet is one column short, not malformed, and the author
            // is entitled to know why the missing column is not a formality.
            std::fprintf(stderr,
                "\n"
                "       This worksheet has a Parameter column but no Guide Word and no Deviation. Parameter\n"
                "       alone is not enough, and the reason is not pedantic: deviations are DEDUPLICATED into\n"
                "       observations, so two rows both reading 'Level' -- one for high, one for low -- collapse\n"
                "       into a single observation carrying both causes. Overflow and cavitation would then share\n"
                "       one symptom, and they call for opposite responses.\n"
                "\n"
                "       Add the Guide Word column, or write the combined deviation ('High level') in one column.\n");
        }
        return 1;
    }

    std::vector<Row> rows;
    std::string line;
    // Start after the banner AND the header, so a reported line number matches what the author sees in the file.
    // Reporting "line 2" for a row physically at line 5 is the misleading-line-number failure this project has
    // already recorded twice; a finding nobody can locate is barely a finding.
    int lineno = phys;   // the header's own last line; each record below advances it by what it consumes
    int consumed = 0;
    while (read_record(in, line, &consumed)) {
        const int row_start = lineno + 1;
        lineno += consumed;
        consumed = 0;
        if (line.empty()) { continue; }
        {
            std::string probe = line;
            while (!probe.empty() && (probe.front() == ' ' || probe.front() == '\t')) { probe.erase(probe.begin()); }
            if (!probe.empty() && probe[0] == '#') { continue; }
        }
        const std::vector<std::string> f = split_csv(line);
        const auto at = [&](int i) { return i >= 0 && i < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(i)]
                                                                                    : std::string(); };
        Row r;
        r.node = at(c_node);
        if (synth_dev) {
            const std::string guide = at(c_guide);
            const std::string param = at(c_param);
            // Guide word first, because that is how a HAZOP team says it: "No flow", "High level", "Reverse flow".
            r.deviation = guide.empty() ? param : (param.empty() ? guide : guide + " " + param);
        } else {
            r.deviation = at(c_dev);
        }
        r.cause = at(c_cau);
        r.consequence = at(c_con);
        r.safeguards = at(c_safe);
        r.action = at(c_act);
        r.severity = at(c_sev);
        r.mitigative = at(c_mit);
        r.line = row_start;
        if (!r.deviation.empty() && !r.cause.empty()) { rows.push_back(r); }
    }
    if (rows.empty()) { std::fprintf(stderr, "error: no usable rows in %s\n", path.c_str()); return 1; }

    // ── deviations become observations, deduplicated ────────────────────────
    // One deviation is typically reached by several causes, which is precisely the differential-diagnosis shape: the
    // deviation alone cannot say which cause produced it.
    std::vector<std::string> dev_order;
    std::map<std::string, std::vector<const Row*>> by_dev;
    for (const Row& r : rows) {
        const std::string key = ident(r.deviation);
        if (by_dev.find(key) == by_dev.end()) { dev_order.push_back(key); }
        by_dev[key].push_back(&r);
    }

    std::printf("casebook %s v1\n", name.c_str());
    std::printf("source \"imported from %s -- HAZOP worksheet, %zu rows\" dated TODO by \"TODO\"\n",
                path.c_str(), rows.size());
    std::printf("note \"GENERATED DRAFT from a HAZOP worksheet. NOT a finished casebook.\"\n");
    std::printf("note \"\"\n");
    std::printf("note \"A HAZOP records deviations, causes and existing safeguards. It does NOT record defeaters --\"\n");
    std::printf("note \"the reasons a conclusion does not follow even when its evidence holds -- nor severities, nor\"\n");
    std::printf("note \"a runtime response. Those three are engineering judgement and are marked TODO below.\"\n");
    std::printf("note \"\"\n");
    std::printf("note \"Read the SAFEGUARD ANALYSIS printed on stderr during import. A safeguard that names the same\"\n");
    std::printf("note \"instrument as the cause is a hidden failure, and that is the CAPECO accident.\"\n\n");

    std::printf("# ── Deviation -> observe.  A HAZOP deviation IS a process excursion, so it is already an\n");
    std::printf("#    observation. This is the part that is easier than importing an FMEA.\n");
    for (const std::string& d : dev_order) {
        const Row* first = by_dev[d].front();
        std::printf("observe %-28s \"%s\"   # %zu cause(s)\n", d.c_str(), first->deviation.c_str(), by_dev[d].size());
    }
    if (dev_order.size() > 30) {
        // The instantiation mask is 32 bits and validation is 2^n, so cbc refuses past 30 observations. Letting it
        // refuse later with no guidance wastes the author's time; the real message is architectural.
        std::fprintf(stderr,
            "\n=== TOO MANY DEVIATIONS FOR ONE CASEBOOK ===\n\n"
            "  This worksheet has %zu distinct deviations. A casebook is limited to 30 observations, because the\n"
            "  presence mask is 32 bits and validation checks every one of 2^n states exhaustively.\n\n"
            "  That is not a defect to work around -- it is the shape of the product. A casebook covers ONE NODE:\n"
            "  a vessel, a compressor, a transfer line. A full plant HAZOP runs to hundreds of rows across dozens\n"
            "  of nodes, and encoding it as one casebook would mean 2^100 states, which is not validation, it is\n"
            "  hope.\n\n"
            "  Split by node -- the Node column if there is one -- and compile each separately. Several casebooks\n"
            "  can then share one device as a palette, and non-interference between them is proved exhaustively.\n\n",
            dev_order.size());
    }

    std::printf("\n# TODO: a deviation measured CONTINUOUSLY should become a `channel` with unit, range,\n");
    std::printf("#       stale_after and thresholds, so the device can tell a dead sensor from a quiet one.\n");
    std::printf("#       A deviation noticed on an operator round is an observation a person asserts.\n\n");

    // ── causes become claims ────────────────────────────────────────────────
    std::printf("# ── Cause -> claim.  Each claim needs its deviation, so it can only be considered when the\n");
    std::printf("#    process actually deviated. Where one deviation has several causes, the deviation alone\n");
    std::printf("#    cannot choose between them -- that is what the defeaters below are for.\n\n");
    std::set<std::string> used;
    for (const Row& r : rows) {
        std::string cid = ident(r.cause);
        while (used.count(cid) != 0) { cid += "_2"; }
        used.insert(cid);
        const std::string dev = ident(r.deviation);

        std::printf("# HAZOP line %d", r.line);
        if (!r.node.empty()) { std::printf(" | node: %s", r.node.c_str()); }
        if (!r.consequence.empty()) { std::printf(" | consequence: %s", r.consequence.c_str()); }
        std::printf("\n");
        if (!r.safeguards.empty()) { std::printf("# safeguards: %s\n", r.safeguards.c_str()); }
        if (!r.action.empty()) { std::printf("# HAZOP action (engineering, not runtime): %s\n", r.action.c_str()); }
        std::printf("claim %s \"%s\"\n", cid.c_str(), r.cause.c_str());
        std::printf("  needs %s\n", dev.c_str());
        const int hint = severity_hint(r.severity);
        if (hint > 0) {
            std::printf("# TODO severity: the worksheet grades this '%s'. On a 1-10 scale that suggests %d --\n",
                        r.severity.c_str(), hint);
            std::printf("#   a SUGGESTION from a defensible reading of a 5-point scale, not a standard, and not\n");
            std::printf("#   written in for you. Severity decides which claims may rebut which, so a wrong value\n");
            std::printf("#   silently changes what the device does. Confirm it, then write `severity %d`.\n", hint);
        } else if (!r.severity.empty()) {
            std::printf("# TODO severity: the worksheet says '%s', which this tool cannot map to 1-10.\n",
                        r.severity.c_str());
        } else {
            std::printf("# TODO severity: no severity column. The consequence column says '%s'.\n",
                        r.consequence.empty() ? "(nothing)" : r.consequence.c_str());
        }
        std::printf("#   Severity is 1-10 and must be DECLARED, not inferred from prose. Both this claim and\n");
        std::printf("#   anything that rebuts it need one, or the preference is dropped.\n");
        std::printf("# TODO defeater: what makes '%s' not follow even when %s holds?\n", r.cause.c_str(), dev.c_str());
        std::printf("#   Another cause of the same deviation is a competing explanation -- an `excuse` that rebuts\n");
        std::printf("#   this. A reason the inference does not apply at all -- a test in progress, a known\n");
        std::printf("#   maintenance state -- `undercuts` it. An excuse with no `needs` holds in EVERY state and\n");
        std::printf("#   would make this claim unreachable, so gate it on an observation.\n\n");
    }

    std::printf("# ── actions ────────────────────────────────────────────────────────────────\n");
    std::printf("# TODO: a HAZOP's Action column records recommendations to engineering -- 'confirm LAH tested',\n");
    std::printf("#       'add independent LSH'. Those are project actions, NOT runtime responses, and they are\n");
    std::printf("#       carried above as comments only. The runtime actions have to be decided here.\n");
    std::printf("#\n");
    std::printf("# TODO: exactly one action must be marked `safe`, and it is asset-specific: a pump stops, a tank\n");
    std::printf("#       stops filling, a drone must keep flying. cbc refuses to guess.\n");
    std::printf("#\n");
    std::printf("# action stop   safe when any <the claims that warrant stopping>\n");
    std::printf("# action review      when any <the claims that warrant a person looking>\n");
    std::printf("# action run    idle\n");
    std::printf("# prefer stop over review over run\n");

    // ── SAFEGUARD ANALYSIS: the reason this tool exists ─────────────────────
    //
    // Everything above is transcription. This is the finding.
    std::fprintf(stderr, "\n=== SAFEGUARD ANALYSIS ===\n\n");
    std::fprintf(stderr,
        "A HAZOP's Safeguards column is the one an FMEA never has, and it is where the document already\n"
        "records what protects against each deviation. Two things in it are worth more than the rest of the\n"
        "worksheet put together.\n\n");

    int unprotected = 0;
    int common_cause = 0;
    std::size_t dev_unprotected = 0;

    // Declared before any of them runs, so the report can list the checks that found NOTHING. That section is what
    // turns silence into evidence: a review naming three findings could be thorough or could be a tool that knows
    // three things.
    Report report;
    report.check("UNPROTECTED", "does the document record any protection at all for this deviation?");
    report.check("COMMON CAUSE", "does the safeguard name the same instrument whose failure is the cause?");
    report.check("SHARED LAYER", "is one safeguard carrying most of the worksheet?");
    report.check("NOT AN IPL", "is the only protection people and paper rather than an instrument?");
    report.check("UNPROVEN LAYER", "does the document itself record that a safeguard is not proof tested?");
    report.check("MITIGATIVE ONLY", "is the consequence reduced but the deviation not prevented?");
    report.check("SHARED CAUSE", "is one failure written up under several unrelated deviations?");
    report.check("PACKED CAUSES", "are several causes packed into one cell, so only the first becomes a claim?");
    report.limit("Whether a safeguard actually WORKS. This reads the words in the cell. An instrument named here "
                 "may be uncalibrated, bypassed, or wired to a failed I/O card, and the worksheet would look "
                 "identical.");
    report.limit("Whether the list of deviations is COMPLETE. A deviation nobody wrote down cannot be checked, and "
                 "completeness has no mechanical test -- it is the honest limit of this whole approach.");
    report.limit("Whether a severity or a consequence is right. Those are engineering judgement and are carried "
                 "across as suggestions only, never written into anything.");
    report.limit("Independence beyond the words used. Two safeguards on the same power supply, air supply or "
                 "calibration source are invisible here unless the document names them.");

    if (c_safe < 0) {
        std::fprintf(stderr,
            "  NO SAFEGUARDS COLUMN. This worksheet records deviations and causes but not what protects\n"
            "  against them, so the most valuable analysis a HAZOP supports cannot be done. Ask for the\n"
            "  column; a HAZOP without it is a hazard list.\n\n");
    } else {
        for (const Row& r : rows) {
            if (safeguard_is_none(r.safeguards) && safeguard_is_none(r.mitigative)) {
                ++unprotected;
                ++dev_unprotected;
                report.add(Finding{"UNPROTECTED", Weight::Conclusion, r.line, r.deviation,
                    "safeguards column reads: " + (r.safeguards.empty() ? std::string("(blank)") : r.safeguards),
                    "The document already records that nothing protects against this deviation. That is not a "
                    "discovery, it is a restatement -- and restating it is the point, because the CAPECO tank "
                    "overflow was written into a HAZOP years before it happened, in a cell exactly like this one. "
                    "A blank cell is read the same way as 'None', which conflates 'nothing protects this' with "
                    "'nobody looked'; both warrant the same attention here.",
                    "Either a protection layer, or an explicit record that the risk is accepted and why. A cell "
                    "reading 'None' that nobody ever revisits is the failure mode this tool exists to surface."});
                std::fprintf(stderr, "  UNPROTECTED    line %d: '%s' caused by '%s'\n", r.line,
                             r.deviation.c_str(), r.cause.c_str());
                std::fprintf(stderr, "                 safeguards recorded: '%s'\n",
                             r.safeguards.empty() ? "(blank)" : r.safeguards.c_str());
            }
        }
        if (unprotected > 0) { std::fprintf(stderr, "\n"); }

        for (const Row& r : rows) {
            const std::set<std::string> ct = tags(r.cause);
            const std::set<std::string> st = tags(r.safeguards);
            std::vector<std::string> both;
            for (const std::string& t : ct) {
                if (st.count(t) != 0) { both.push_back(t); }
            }
            if (both.empty()) { continue; }
            ++common_cause;
            {
                std::string shared_tags;
                for (const std::string& t : both) { shared_tags += (shared_tags.empty() ? "" : ", ") + t; }
                report.add(Finding{"COMMON CAUSE", Weight::Structural, r.line, r.deviation,
                    "cause:      " + r.cause + "\nsafeguard:  " + r.safeguards + "\nboth name:  " + shared_tags,
                    "The instrument credited with protecting against this deviation is the same instrument whose "
                    "failure causes it. When it fails there is no protection and no indication that protection is "
                    "gone -- the worksheet reads as protected and the plant is not. This is quieter than an "
                    "unprotected row and worse, because a reviewer scanning the Safeguards column sees a tag and "
                    "moves on.",
                    "A measurement from a different instrument. If the existing tag is genuinely a separate device "
                    "that merely shares a name, saying so in the worksheet closes this."});
            }
            std::fprintf(stderr, "  COMMON CAUSE   line %d: the safeguard names the same instrument as the cause\n",
                         r.line);
            std::fprintf(stderr, "                 deviation:  %s\n", r.deviation.c_str());
            std::fprintf(stderr, "                 cause:      %s\n", r.cause.c_str());
            std::fprintf(stderr, "                 safeguards: %s\n", r.safeguards.c_str());
            std::fprintf(stderr, "                 shared tag:");
            for (const std::string& t : both) { std::fprintf(stderr, " %s", t.c_str()); }
            std::fprintf(stderr, "\n");
        }
        if (common_cause > 0) { std::fprintf(stderr, "\n"); }
    }

    // ── SEVERAL CAUSES IN ONE CELL ──────────────────────────────────────────
    //
    // HAZOP teams bullet three or four causes into a single Cause cell. Read row by row, that becomes ONE claim
    // named after the whole list, and the other causes vanish from the casebook entirely.
    //
    // Reported, never split automatically. Whether three bulleted causes are three claims or one depends on
    // whether they need different responses, and that is an engineering judgement -- the same reason the SHARED
    // CAUSE report refuses to merge for you.
    int packed_causes = 0;
    for (const Row& r : rows) {
        const std::vector<std::string> parts = split_causes(r.cause);
        if (parts.size() < 2) { continue; }
        ++packed_causes;
        {
            std::string listed;
            for (const std::string& part : parts) { listed += "  - " + part + "\n"; }
            char hdr[128];
            std::snprintf(hdr, sizeof(hdr), "%zu causes in one cell:\n", parts.size());
            report.add(Finding{"PACKED CAUSES", Weight::Observation, r.line, r.deviation, hdr + listed,
                "Only the whole cell became a claim, so every cause after the first is absent from the generated "
                "casebook. Whether these are separate claims or one depends on whether they need different "
                "responses, which is engineering judgement -- so this tool will not split them for you. But a "
                "casebook that silently reasons about one of four causes is worse than one that refuses.",
                "Splitting the cell into one row per cause, or confirming that a single response covers all of "
                "them. Either answer is fine; the draft cannot tell which you meant."});
        }
        std::fprintf(stderr, "  PACKED CAUSES  line %d: %zu causes in one cell, for deviation '%s'\n",
                     r.line, parts.size(), r.deviation.c_str());
        for (const std::string& part : parts) {
            std::fprintf(stderr, "                   - %s\n", part.c_str());
        }
    }
    if (packed_causes > 0) {
        std::fprintf(stderr,
            "\n                 Only the whole cell became a claim, so every cause after the first is absent from\n"
            "                 the draft. Whether these are separate claims or one depends on whether they need\n"
            "                 different responses -- an engineering judgement, so this tool will not split them\n"
            "                 for you. But a casebook that silently reasons about one of four causes is worse\n"
            "                 than one that refuses.\n\n");
    }

    // ── SHARED CAUSE: one failure producing several deviations ──────────────
    //
    // The inverse of the protection-layer question, and the more diagnostically valuable half.
    //
    // LOPA-1 in the compiler asks whether the routes to a protective action share a dependency. This asks whether
    // the DEVIATIONS share a cause. A HAZOP is organised by deviation, so one component failure gets written up
    // once per deviation it produces, in slightly different words each time -- and an importer reading row by row
    // turns one fault into several unrelated claims.
    //
    // That inverts the diagnosis. If over-pressure and over-voltage are separate claims, seeing both looks like TWO
    // independent faults, which is improbable and invites dismissal. Recognising one shared cause makes it a single
    // likely fault, and makes the pair of deviations CORROBORATING evidence for it -- `needs dev_a dev_b`, which is
    // precisely the differential-diagnosis shape this engine exists to evaluate.
    //
    // Found in the hospital-oxygen HAZOP, whose own discussion says the compressor's three critical deviations are
    // "directly related to the defect of the electronic board and its related components". The document knew. The
    // importer discarded it.
    int shared_cause = 0;
    {
        std::map<std::string, std::vector<const Row*>> by_cause;
        std::vector<std::string> order;
        for (const Row& r : rows) {
            const std::string k = cause_key(r.cause);
            if (by_cause.find(k) == by_cause.end()) { order.push_back(k); }
            by_cause[k].push_back(&r);
        }
        for (const std::string& k : order) {
            const std::vector<const Row*>& group = by_cause[k];
            std::set<std::string> devs;
            for (const Row* r : group) { devs.insert(ident(r->deviation)); }
            if (devs.size() < 2) { continue; }
            ++shared_cause;
            {
                std::string devlist;
                for (const std::string& d : devs) { devlist += "  - " + d + "\n"; }
                report.add(Finding{"SHARED CAUSE", Weight::Observation, 0, k,
                    "appears under " + std::to_string(devs.size()) + " deviations:\n" + devlist,
                    "One component failure written up under several deviations becomes several unrelated claims in "
                    "the casebook, each with its own evidence. Whether that is right depends on whether the "
                    "deviations need different responses, and this tool deliberately does not advise -- compiling "
                    "both versions of exactly this case showed the split form tripping in 3 of 8 states and the "
                    "merged form in 1 of 8, so advice to merge would have made the plant shut down LESS often.",
                    "Deciding whether these deviations share a response. Keep them separate if they need different "
                    "ones, merge them if they do not, and either way the worksheet should say which."});
            }
            std::fprintf(stderr, "  SHARED CAUSE   one failure is written up under %zu different deviations:\n",
                         devs.size());
            for (const Row* r : group) {
                std::fprintf(stderr, "                 line %-3d %-28s %s\n", r->line, r->deviation.c_str(),
                             r->cause.c_str());
            }
        }
        if (shared_cause > 0) {
            std::fprintf(stderr,
                "\n                 Each row above became a SEPARATE claim in the draft, so the casebook treats one\n"
                "                 fault as several. Seeing all those deviations at once then looks like several\n"
                "                 independent failures -- improbable, and easy to dismiss -- when it is one likely\n"
                "                 failure announcing itself several ways.\n"
                "\n                 DO NOT SIMPLY MERGE THEM. Measured on a 3-observation probe: two separate claims,\n"
                "                 either sufficient, trip in 3 of 8 states; one merged claim NEEDING both deviations\n"
                "                 trips in 1 of 8. Merging converts two independently sufficient conditions into one\n"
                "                 conjunctive necessary condition, and the safety function acts LESS often.\n"
                "\n"
                "                 Both readings are defensible and the document does not settle it:\n"
                "                   SEPARATE  each deviation alone warrants acting. Conservative; more spurious trips.\n"
                "                   MERGED    only corroborated evidence warrants acting. Fewer false trips, but a\n"
                "                             fault presenting only ONE way is now invisible -- a hidden failure.\n"
                "                   BOTH      one claim per deviation for the response, plus a third claim needing\n"
                "                             both, at higher severity, for the confirmed diagnosis. Usually right,\n"
                "                             and only possible once you know the cause is shared.\n"
                "\n"
                "                 What is NOT defensible is not knowing the cause is shared, which is the state this\n"
                "                 report found the draft in. The decision is an engineer's; the fact is the\n"
                "                 document's.\n\n");
        }
    }

    // ── SHARED LAYER: the same protection across many rows ──────────────────
    //
    // The per-row checks above ask whether ONE row's safeguard depends on its own cause. This asks a question the
    // per-row view structurally cannot: is the SAME protection carrying the whole document?
    //
    // A LOPA asks about layers protecting one scenario. But when one safeguard appears against nearly every
    // deviation in the study, it is a single point of failure for the entire plant -- and each row looks perfectly
    // acceptable in isolation, which is why a reviewer reading row by row will not see it.
    int shared_layer = 0;
    int not_a_layer = 0;
    int mitigative_only = 0;
    int unproven = 0;
    if (c_safe >= 0) {
        std::map<Guard, int> kind_rows;
        std::map<std::string, int> phrase_rows;
        for (const Row& r : rows) {
            for (const Guard g : classify_safeguards(r.safeguards)) { ++kind_rows[g]; }
            const std::string t = lower(r.safeguards);
            for (const char* probe : {"visual inspection", "alarm of the device", "alarm of device",
                                      "existing instruction", "operator round", "procedure", "training"}) {
                if (t.find(probe) != std::string::npos) { ++phrase_rows[probe]; }
            }
        }
        const int total = static_cast<int>(rows.size());
        // Two thirds is a judgement call, and it is stated rather than hidden. Below it, a repeated safeguard is
        // ordinary good practice; above it, the document has one layer wearing many hats.
        const int threshold = (total * 2 + 2) / 3;

        for (const auto& kv : phrase_rows) {
            // A floor of 3 rows used to sit here, and it silenced the check at 100% sharing on small inputs:
            // a 2-row extract where BOTH rows name "visual inspection" reported nothing at all. A single-node
            // extract of a few rows is exactly what a customer sends as a first sample, so the floor was hiding
            // the finding in the most likely first document. Two rows is enough to be a shared layer; the row
            // count is printed so the reader can weigh how much evidence it is.
            if (kv.second < threshold || kv.second < 2) { continue; }
            ++shared_layer;
            {
                char det[320];
                std::snprintf(det, sizeof(det), "named as the safeguard in %d of %d rows (%d%%)%s",
                              kv.second, total, total > 0 ? (kv.second * 100) / total : 0,
                              total < 5 ? "\nsmall extract: weaker evidence, same shape" : "");
                report.add(Finding{"SHARED LAYER", Weight::Structural, 0, kv.first, det,
                    "A protection named in most rows is a single point of failure for most of the document. This is "
                    "invisible row by row, which is how a worksheet gets reviewed: each row shows a safeguard and "
                    "looks protected. The published hospital oxygen study named visual inspection in 13 of 14 rows "
                    "and the alarm of the device in 10 of 14, with no instrument tag anywhere in the document.",
                    "Recording what makes this safeguard available and how its absence would be noticed. A layer "
                    "carrying most of a worksheet warrants attention the others do not."});
            }
            std::fprintf(stderr, "  SHARED LAYER   '%s' is the safeguard in %d of %d rows (%d%%)%s\n",
                         kv.first.c_str(), kv.second, total,
                         total > 0 ? (kv.second * 100) / total : 0,
                         total < 5 ? "  [small extract -- weaker evidence, same shape]" : "");
        }
        if (shared_layer > 0) {
            std::fprintf(stderr,
                "\n                 Each of those rows looks acceptable on its own, which is why reading the\n"
                "                 worksheet row by row will not reveal this. Taken together they are ONE layer:\n"
                "                 whatever disables it removes the protection from every deviation at once.\n\n");
        }

        // ── NOT AN IPL: rows protected only by people and paper ─────────────
        // Preventive-only is worth naming: the deviation still happens, the consequence is merely smaller.
        for (const Row& r : rows) {
            if (!safeguard_is_none(r.mitigative) && safeguard_is_none(r.safeguards)) {
                ++mitigative_only;
                std::fprintf(stderr,
                    "  MITIGATIVE ONLY line %d: '%s' has no PREVENTIVE safeguard; only mitigation after the fact\n",
                    r.line, r.deviation.c_str());
                report.add(Finding{"MITIGATIVE ONLY", Weight::Structural, r.line, r.deviation,
                    "preventive:  " + (r.safeguards.empty() ? std::string("(none)") : r.safeguards)
                        + "\nmitigative:  " + r.mitigative,
                    "A mitigative layer does not stop the deviation, it makes the consequence smaller. Counting it "
                    "as protection hides the fact that nothing prevents the event: the bund catches the spill, but "
                    "the tank still overflows. Reading only one of the two safeguard columns is how this tool once "
                    "reported a deviation as unprotected while a relief to flare sat in the next column -- a false "
                    "finding, which costs more than a missed one.",
                    "Either a preventive layer, or a record that only the consequence is being managed here. Both "
                    "are legitimate positions; conflating them is not."});
                std::fprintf(stderr, "                 mitigative: %s\n", r.mitigative.c_str());
            }
        }
        if (mitigative_only > 0) {
            std::fprintf(stderr,
                "\n                 A mitigative layer does not stop the deviation, it makes the consequence\n"
                "                 smaller. Counting it as protection hides the fact that nothing prevents the\n"
                "                 event -- the bund catches the spill, but the tank still overflows.\n\n");
        }

        for (const Row& r : rows) {
            if (integrity_unproven(r.safeguards) || integrity_unproven(r.mitigative)) {
                ++unproven;
                report.add(Finding{"UNPROVEN LAYER", Weight::Structural, r.line, r.deviation,
                    "safeguards: " + (r.safeguards.empty() ? r.mitigative : r.safeguards),
                    "A LOPA gives every protection layer a probability of failure on demand, and that number rests "
                    "entirely on the layer being proof tested. A tagged instrument nobody has tested is not a "
                    "protection layer, it is an assumption with a part number -- and it is a hidden failure by "
                    "definition: counted on by everyone, demonstrated by no one. This was waved through until "
                    "recently because the cell contained a tag.",
                    "A proof-test record, or an interval and an owner. The document already admits the gap; what is "
                    "missing is a decision about it."});
                std::fprintf(stderr,
                    "  UNPROVEN LAYER line %d: the document itself records that this safeguard is not demonstrated\n",
                    r.line);
                std::fprintf(stderr, "                 deviation:  %s\n", r.deviation.c_str());
                std::fprintf(stderr, "                 safeguards: %s\n",
                             r.safeguards.empty() ? r.mitigative.c_str() : r.safeguards.c_str());
            }
        }
        if (unproven > 0) {
            std::fprintf(stderr,
                "\n                 A LOPA gives every protection layer a probability of failure on demand, and that\n"
                "                 number rests entirely on the layer being proof tested. A tagged instrument nobody\n"
                "                 has tested is not a protection layer, it is an assumption with a part number --\n"
                "                 and it is a hidden failure by definition: counted on by everyone, demonstrated by\n"
                "                 no one.\n\n");
        }

        for (const Row& r : rows) {
            const std::set<Guard> g = classify_safeguards(r.safeguards);
            if (g.empty()) { continue; }
            // Only a tagged instrument counts as a candidate Independent Protection Layer. Deferral language and
            // prose this tool cannot read are reported, not waved through: mutation testing showed the previous
            // `Other` fallback made "See note 4" indistinguishable from a real trip.
            if (g.count(Guard::Instrument) != 0) { continue; }
            ++not_a_layer;
            {
                std::string kinds;
                for (const Guard k : g) {
                    kinds += (kinds.empty() ? "" : ", ");
                    switch (k) {
                        case Guard::Human:        kinds += "a person looking"; break;
                        case Guard::Procedural:   kinds += "a procedure"; break;
                        case Guard::OnDevice:     kinds += "the device's own alarm"; break;
                        case Guard::Deferred:     kinds += "a decision not yet made"; break;
                        case Guard::Unclassified: kinds += "text this tool cannot classify"; break;
                        case Guard::Instrument:   break;
                    }
                }
                report.add(Finding{"NOT AN IPL", Weight::Structural, r.line, r.deviation,
                    "protected only by: " + kinds + "\nsafeguards cell:   " + r.safeguards,
                    "None of these is an independent protection layer in the CCPS sense. A person looking, a "
                    "procedure, and the alarm built into the device that is failing all share a dependency with "
                    "the event or with each other. The device's own alarm is the sharpest case: when the device "
                    "fails, the alarm that was supposed to warn about it fails with it.",
                    "Naming an instrument independent of the failing equipment, or recording that this deviation is "
                    "knowingly managed procedurally. The worksheet currently implies a protection layer it does "
                    "not have."});
            }
            std::fprintf(stderr, "  NOT AN IPL     line %d: '%s' is protected only by", r.line, r.deviation.c_str());
            bool first_kind = true;
            for (const Guard k : g) {
                std::fprintf(stderr, "%s %s", first_kind ? "" : " and", guard_name(k));
                first_kind = false;
            }
            std::fprintf(stderr, "\n                 cause:      %s\n", r.cause.c_str());
            std::fprintf(stderr, "                 safeguards: %s\n", r.safeguards.c_str());
            if (g.count(Guard::Deferred) != 0) {
                std::fprintf(stderr,
                    "                 ^ this is a DEFERRAL. The document has not recorded a protection here, and\n"
                    "                   an unfollowed cross-reference or a 'TBD' must not be read as one.\n");
            }
            if (g.count(Guard::Unclassified) != 0) {
                std::fprintf(stderr,
                    "                 ^ this tool cannot classify that text. It may well be a real safeguard --\n"
                    "                   read it by hand. Silence here would have meant assuming the best.\n");
            }
            if (g.count(Guard::OnDevice) != 0) {
                std::fprintf(stderr,
                    "                 ^ the device's own alarm is NOT independent of a failure inside that device\n");
            }
        }
        if (not_a_layer > 0) {
            std::fprintf(stderr,
                "\n                 CCPS requires an Independent Protection Layer to be effective, independent of\n"
                "                 the initiating event and of all other layers, and auditable. A person looking at\n"
                "                 a machine fails independence against every other human-dependent layer; a written\n"
                "                 instruction describes a protection rather than being one; and a device's built-in\n"
                "                 alarm is not independent of a fault inside that device.\n\n");
        }
    }

    // Figures worth stating even when nothing is wrong, because a review that reports only problems offers no
    // evidence it read the document.
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%zu deviation(s) across %zu row(s)", dev_order.size(), rows.size());
        report.confirmed("worksheet read", buf);
        // Credit where it is due, and ONLY where it is due. An earlier version listed "N of M rows name an
        // instrument" here, which on the hospital oxygen study read "0 of 14" -- filing that document's single
        // worst characteristic under a heading that reads as reassurance. It is the NOT AN IPL finding, and it
        // belongs there. What goes here is what the document got right.
        std::snprintf(buf, sizeof(buf), "%zu of %zu row(s) record some protection", rows.size() - dev_unprotected,
                      rows.size());
        report.confirmed("safeguards recorded", buf);
    }

    if (!report_path.empty()) {
        std::ifstream raw(path, std::ios::binary);
        std::ostringstream all;
        all << raw.rdbuf();
        const std::string sha = quorum::sha256::hex(all.str());
        const std::string prov =
            "This review was produced mechanically from the worksheet named above. No column was renamed and no\n"
            "cell was retyped on the way in. That matters more than it sounds: the first published HAZOP this\n"
            "project analysed was silently re-headed to suit the importer, which produced thirteen findings that\n"
            "all vanished once the document's real column names were used. A finding derived from a transcription\n"
            "is a finding about the transcription.";
        const std::string cmd = "hazop2cb " + path + " " + name + " --report " + report_path;
        if (!report.markdown(report_path, path, sha, "hazop2cb", prov, cmd)) {
            std::fprintf(stderr, "error: cannot write report to %s\n", report_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "  report written to %s  (%zu finding(s), document SHA-256 %.16s...)\n\n",
                     report_path.c_str(), report.size(), sha.c_str());
    }

    std::fprintf(stderr, "  %d unprotected, %d sharing the cause's instrument, %d shared across the document, %d not an IPL,\n"
                 "  %d cause(s) producing several deviations, %d cell(s) packing several causes,\n"
                 "  %d mitigative-only, %d unproven layer(s)\n\n",
                 unprotected, common_cause, shared_layer, not_a_layer, shared_cause,
                 packed_causes, mitigative_only, unproven);

    if (unprotected > 0 || common_cause > 0 || shared_layer > 0 || not_a_layer > 0 || shared_cause > 0
        || packed_causes > 0 || mitigative_only > 0 || unproven > 0) {
        std::fprintf(stderr,
            "  An UNPROTECTED deviation has nothing in the way. A COMMON CAUSE is worse and quieter: the\n"
            "  protection depends on the very instrument whose failure causes the deviation, so when it fails\n"
            "  the deviation happens AND the protection is gone AND no alarm sounds. The absence of an alarm\n"
            "  is not evidence of a safe state.\n\n"
            "  That is the CAPECO explosion (CSB 2010-02-I-PR): a level transmitter delivering fresh, in-range,\n"
            "  entirely plausible samples that had not moved in thirty minutes, and no independent measurement.\n"
            "  It is written into worksheets like this one before it happens.\n\n"
            "  Tag matching is a heuristic. It finds 'LT-101' in both columns; it cannot find a shared power\n"
            "  supply, a shared I/O card, a shared control logic or a shared calibration, which are common\n"
            "  causes a real LOPA also considers. Read the rows, do not trust the count.\n\n");
    }

    std::fprintf(stderr,
        "  Next: supply severities, defeaters and the actions, then run cbc. Its protection-layer\n"
        "  independence report answers the question this analysis only opens -- whether every route to a\n"
        "  protective action passes through the same reading.\n\n");

    return 0;
}
