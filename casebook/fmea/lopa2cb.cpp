// lopa2cb -- audit a Layer of Protection Analysis, and draft a casebook from it.
//
// WHY A LOPA IS THE STRONGEST OF THE THREE DOCUMENTS
//
// MISSION.md ranks the documents this project can read, and LOPA sits at the top for one reason: it states its own
// arithmetic. A HAZOP records protections in prose, so every finding about them rests on my reading of English --
// and twenty-six of the assumptions I wrote down about how people write that English turned out to be wrong. A
// LOPA hands the reasoning over as numbers:
//
//   Initiating cause        what starts the sequence            frequency, per year
//   IPLs                    the independent protection layers   a PFD each
//   Conditional modifiers   probability the consequence lands    a probability each
//   Mitigated frequency     frequency x every PFD               STATED IN THE DOCUMENT
//   Target frequency        what the organisation will accept    STATED IN THE DOCUMENT
//
// The last two are the opportunity. A LOPA asserts a conclusion and shows its working, which means a machine can
// redo the working and disagree. A finding of that kind cannot be argued with in the way a prose finding can, and
// it cannot be wrong because I misread a phrase.
//
// The arithmetic is done in a spreadsheet, by hand, across dozens of scenarios, by people under time pressure. A
// wrong exponent changes a conclusion by a factor of ten and looks exactly like a right one.
//
// WHAT IS CHECKED, AND WHY EACH ONE IS WORTH CHECKING
//
//   ARITHMETIC            the row's own product against the row's own stated answer
//   TOTAL                 the sum of the rows against the stated total
//   TARGET NOT MET        the stated total against the stated target, with the required PFD and SIL band
//   NOT A PROTECTION      a conditional modifier credited as though it were a protection layer
//   NOT INDEPENDENT       a layer that depends on the very system named as the initiating cause
//   SHARED LAYER          one layer carrying most or all of the initiating causes
//   CREDIT LIMIT          a PFD better than the layer's kind is normally allowed to claim
//   UNIFORM PFD           every layer at the same round number, which means defaults, not measurements
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not change a number, and it does not decide whether a finding matters. A LOPA is semi-quantitative by
// construction -- order-of-magnitude estimates, chosen conservatively -- so a tool that reported every rounding
// difference as an error would be unusable and would be wrong about what the method is for. Tolerances below are
// deliberately loose, and the computed and stated values are always printed together so the reader judges.
//
// Nor does it prescribe a remedy. Compiling a "fix" and measuring it, rather than reasoning about it, is a lesson
// this project paid for: advice to merge two duplicated claims in a HAZOP would have made the plant shut down LESS
// often than leaving them separate.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <set>
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

/// A protection layer or conditional modifier, as the worksheet declares it.
struct Layer {
    std::string name;        ///< the column header, minus its IPL/CM marker
    bool declared_cm = false;///< the worksheet itself marked this column a conditional modifier
    int column = -1;
};

/// One initiating cause: a row of the worksheet.
struct Cause {
    std::string description;
    double frequency = 0.0;
    std::vector<double> pfd;      ///< parallel to the layer list; NOT_APPLICABLE where the cell is blank or 1
    double stated_mitigated = 0.0;
    bool has_stated = false;
    int line = 0;
};

/// A LOPA sets a PFD to 1, or leaves it blank, when a layer does not apply to that initiating cause.
///
/// This convention is stated explicitly in the biocontainment paper: "A PFD is set to 1 if that IPL or CM does not
/// apply to that IC and is therefore guaranteed to fail." It matters for more than arithmetic, where multiplying by
/// one is harmless. It matters for INDEPENDENCE: a layer entered as 1 is not protecting that cause at all, so
/// counting it as a shared layer across causes would manufacture a finding out of a column of ones.
constexpr double kNotApplicable = 1.0;

bool is_applicable(const double pfd) { return pfd > 0.0 && pfd < kNotApplicable; }

/// Parse a number as a worksheet writes it: 0.1, 1.00E-06, 1e-6, 2.6 x 10-4, or a percentage.
bool parse_number(const std::string& raw, double& out) {
    std::string t;
    for (const char c : raw) {
        if (c != ' ' && c != ',' && c != '\t' && c != '\r') { t.push_back(c); }
    }
    if (t.empty()) { return false; }
    bool percent = false;
    if (t.back() == '%') { percent = true; t.pop_back(); }
    // "2.6x10-4" and "2.6*10^-4" are both written by hand in real worksheets.
    const std::size_t x = t.find_first_of("xX*");
    if (x != std::string::npos && t.find("10", x) != std::string::npos) {
        const std::string mant = t.substr(0, x);
        std::size_t e = t.find("10", x) + 2;
        if (e < t.size() && t[e] == '^') { ++e; }
        if (mant.empty() || e >= t.size()) { return false; }
        try {
            out = std::stod(mant) * std::pow(10.0, std::stod(t.substr(e)));
        } catch (...) { return false; }
        if (percent) { out /= 100.0; }
        return true;
    }
    try {
        std::size_t used = 0;
        out = std::stod(t, &used);
        if (used != t.size()) { return false; }
    } catch (...) { return false; }
    if (percent) { out /= 100.0; }
    return true;
}

/// Is this column a protection layer or a conditional modifier?
///
/// Marked columns are honoured as marked: a worksheet that labels a column "CM2" has told us what it is, and
/// second-guessing the author would be the same mistake as reading "Control/suggested" as an existing protection.
bool layer_column(const std::string& header, bool& is_cm, std::string& name) {
    const std::string h = lower(header);
    static const char* const cm_markers[] = {"cm", "conditional modifier", "conditional modifiers", "modifier"};
    static const char* const ipl_markers[] = {"ipl", "ipls", "independent protection layer", "protection layer",
                                              "schutzebene", "schutzschicht"};
    // A marker may be the whole header ("IPL1") or a prefix ("IPL A: Drainage level switches").
    for (const char* m : cm_markers) {
        const std::string mm(m);
        if (h.compare(0, mm.size(), mm) == 0) { is_cm = true; name = header; return true; }
    }
    for (const char* m : ipl_markers) {
        const std::string mm(m);
        if (h.compare(0, mm.size(), mm) == 0) { is_cm = false; name = header; return true; }
    }
    return false;
}

/// Strip the "IPL A:" / "CM2 -" marker so the reported name is the engineer's own description.
std::string layer_label(const std::string& header) {
    const std::size_t colon = header.find_first_of(":-");
    if (colon != std::string::npos && colon + 1 < header.size()) {
        std::string t = header.substr(colon + 1);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) { t.erase(t.begin()); }
        if (t.size() > 2) { return t; }
    }
    return header;
}

/// Which SIL band does a required PFD fall in? IEC 61508/61511, low demand mode.
///
/// SIL 1 is 1e-1..1e-2, SIL 2 is 1e-2..1e-3, SIL 3 is 1e-3..1e-4, SIL 4 is 1e-4..1e-5. A requirement weaker than
/// SIL 1 needs no safety instrumented function at all, and one stronger than SIL 4 is outside what the standards
/// will certify -- IEC 61511 says a single SIL 4 function should be avoided where reasonably practicable, so a
/// demand beyond it is a signal that the design, not the instrumentation, is what needs to change.
std::string sil_band(const double pfd) {
    if (pfd >= 1e-1) { return "below SIL 1 (no safety instrumented function required)"; }
    if (pfd >= 1e-2) { return "SIL 1"; }
    if (pfd >= 1e-3) { return "SIL 2"; }
    if (pfd >= 1e-4) { return "SIL 3"; }
    if (pfd >= 1e-5) { return "SIL 4"; }
    return "BEYOND SIL 4 -- outside what the standards will certify for a single function";
}

/// Does this layer's description name a kind whose credit is capped by the standards?
///
/// The control system is the important case. IEC 61511 allows a basic process control system that is not itself
/// designed to the standard a single risk reduction credit -- a PFD of 0.1, no better. Claiming 0.01 for a control
/// loop is claiming certified behaviour from uncertified equipment, and it is the most common way a LOPA arrives at
/// a total it has not earned.
///
/// Human and procedural layers are capped for a different reason: the number describes a person, and 0.01 asserts
/// that someone fails to follow a procedure once in a hundred demands, under the conditions of an upset.
struct Cap {
    const char* kind;
    double floor_pfd;
    const char* why;
};

const Cap* credit_cap(const std::string& text) {
    static const Cap caps[] = {
        {"control system", 0.1,
         "IEC 61511 allows a basic process control system not designed to the standard a single risk reduction\n"
         "                 credit -- PFD 0.1, no better. A tighter number claims certified behaviour from\n"
         "                 uncertified equipment."},
        {"human or procedural action", 0.1,
         "this number describes a person acting correctly during an upset. Better than 0.1 asserts fewer than one\n"
         "                 lapse in a hundred demands, which is not a claim operating experience usually supports."},
    };
    const std::string t = lower(text);
    static const char* const control_words[] = {"control system", "bpcs", "basic process control", "control loop",
                                                "dcs", "plc", "control room", "scada", "leitsystem"};
    static const char* const human_words[] = {"procedure", "procedural", "operator", "personnel", "manual",
                                              "training", "trained", "washdown", "inspection", "rounds",
                                              "housekeeping", "checklist", "permit"};
    // A layer that declares a SIL is exempt: it IS designed to the standard, which is the whole point of the rating.
    if (t.find("sil") != std::string::npos || t.find("sif") != std::string::npos) { return nullptr; }
    for (const char* w : control_words) { if (t.find(w) != std::string::npos) { return &caps[0]; } }
    for (const char* w : human_words) { if (t.find(w) != std::string::npos) { return &caps[1]; } }
    return nullptr;
}

/// Words that describe a conditional modifier rather than a protection layer.
///
/// This is the quietest overcount in a LOPA. "Probability of ignition 0.1" and "probability the area is occupied
/// 0.1" both reduce the arithmetic exactly as a protection layer does, and neither one protects anything: they say
/// the consequence might not land, not that the plant intervened. Counting them among the IPLs inflates the number
/// of protection layers a scenario appears to have, which is the figure a reviewer eyeballs.
bool describes_modifier(const std::string& text) {
    const std::string t = lower(text);
    // Standalone phrases that are modifiers on their own.
    static const char* const alone[] = {"occupancy", "occupied", "time at risk", "fraction of time",
                                        "mortality rate", "wind direction", "ignition probability"};
    for (const char* w : alone) { if (t.find(w) != std::string::npos) { return true; } }
    // "probability of X" where X is an OUTCOME rather than a protective act. These must not be matched as one
    // fixed phrase: the biocontainment paper writes "Probability of SAPO infection", with the pathogen's name
    // sitting between the two words, and a literal search for "probability of infection" found nothing at all.
    // The detector was written to catch exactly that column and silently missed it.
    if (t.find("probability") == std::string::npos && t.find("likelihood") == std::string::npos) { return false; }
    static const char* const outcome[] = {"infection", "ignition", "fatality", "fatalities", "death", "injury",
                                          "exposure", "presence", "infected", "harm", "explosion"};
    for (const char* w : outcome) { if (t.find(w) != std::string::npos) { return true; } }
    return false;
}

/// Which SHARED SUPPORT SYSTEMS does this text name, reduced to families?
///
/// Families, not literal phrases. The biocontainment paper's third initiating cause is "Control system failures"
/// and one of the layers credited against it is "Valve position feedback available to notify control room". Those
/// are two different strings and one dependency, and matching on the strings found nothing -- the first version of
/// this function missed the clearest finding in the document it was written for.
///
/// ONLY SUPPORT SYSTEMS, AND THE DISTINCTION IS THE WHOLE CHECK
///
/// A layer is dependent on a system when it needs that system IN ORDER TO WORK -- power to run, instrument air to
/// stroke, the control system to annunciate. A layer is NOT dependent on the system it merely MEASURES: that is
/// its job.
///
/// This was measured, not reasoned about. The first version included "the drainage network" as a family, and on
/// the biocontainment paper it reported "Blockage in the drainage header" against "Drainage level switches
/// available in every drain" -- twice. A level switch in a drain is not defeated by the drain blocking; detecting
/// that is precisely what it is for. Two of four findings on a published document were false, and a false finding
/// costs more than a missed one because it spends the credibility the true ones depend on.
///
/// A person stays in the list. If human error is the initiating cause, a procedure carried out by people is not
/// clearly independent of it -- the same factor appears on both sides of the multiplication.
std::set<std::string> systems(const std::string& text) {
    const std::string t = lower(text);
    std::set<std::string> out;
    struct Family { const char* name; const char* words[10]; };
    static const Family families[] = {
        {"the control system", {"control system", "bpcs", "basic process control", "control loop", "dcs", "plc",
                                "control room", "scada", "leitsystem", nullptr}},
        {"electrical power",   {"power supply", "loss of power", "power failure", "electrical supply", "ups",
                                "stromversorgung", nullptr, nullptr, nullptr, nullptr}},
        {"ventilation",        {"ventilation", "hvac", "air handling", "extract fan", "l\xc3\xbc" "ftung",
                                nullptr, nullptr, nullptr, nullptr, nullptr}},
        {"instrument air",     {"instrument air", "air supply", "pneumatic",
                                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
        {"a person",           {"operator", "personnel", "manual", "human error", "by hand", "staff", "technician",
                                nullptr, nullptr, nullptr}},
    };
    for (const Family& f : families) {
        for (const char* const w : f.words) {
            if (w == nullptr) { break; }
            if (t.find(w) != std::string::npos) { out.insert(f.name); break; }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: lopa2cb <worksheet.csv> <name>\n"
            "\n"
            "Audits a Layer of Protection Analysis and drafts a casebook on stdout; findings go to stderr.\n"
            "\n"
            "Expected columns. Protection layers and conditional modifiers are recognised by a MARKED header, so\n"
            "that the worksheet tells this tool which is which rather than the tool guessing:\n"
            "\n"
            "  Initiating Cause      the event that starts the sequence        required\n"
            "  Frequency             per year                                  required\n"
            "  IPL <anything>        one column per protection layer, PFD      one or more\n"
            "  CM <anything>         one column per conditional modifier, PFD  optional\n"
            "  Mitigated Frequency   the row's stated answer, per year         optional but checked when present\n"
            "  Target Frequency      what the organisation will accept         optional but checked when present\n"
            "\n"
            "A blank PFD cell, or 1, means the layer does not apply to that cause -- the convention LOPA uses.\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::string name = argv[2];
    std::string report_path;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--report") { report_path = argv[i + 1]; }
    }

    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return 1;
    }

    // Search forward for the header, exactly as the HAZOP importer does: an Excel export carries a study title, a
    // facility and a date above the table, and taking row 1 as the header refused real files outright.
    static const char* const header_words[] = {"initiating", "ipl", "pfd", "cause", "frequency", "scenario",
                                               "ursache", "eintrittsh"};
    std::string header_line;
    int skipped = 0;
    int phys = 0;          // physical lines consumed, so reported line numbers match the author's file
    bool found = false;
    int attempt = 0;
    while (attempt < 20) {
        if (!read_record(in, header_line, &phys)) { break; }
        const std::string probe = lower(header_line);
        // A '#' comment does NOT consume an attempt. This fixture's own provenance header -- which records exactly
        // what was changed in transcription, because silently retyping a document's headers is this project's worst
        // recorded finding -- is forty lines long, and counting those as banner rows made the importer refuse it.
        if (probe.empty() || probe[0] == '#') { continue; }
        ++attempt;
        if (attempt == 0) {
            for (const char c : header_line.substr(0, 64)) {
                if (c != '\n' && c != '\t' && c != '\r'
                    && (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) == 0x7FU)) {
                    std::fprintf(stderr,
                        "error: %s is not a text file. It looks like a spreadsheet saved in its own binary format\n"
                        "       (.xlsx, .xls). Export it as CSV first: File > Save As > CSV UTF-8.\n",
                        path.c_str());
                    return 1;
                }
            }
        }
        bool hit = false;
        for (const char* w : header_words) { if (probe.find(w) != std::string::npos) { hit = true; break; } }
        if (hit) { found = true; break; }
        ++skipped;
    }
    if (!found) {
        std::fprintf(stderr, "error: %s has no row that looks like a LOPA header.\n", path.c_str());
        return 1;
    }
    if (skipped > 0) {
        std::fprintf(stderr, "  note: skipped %d row(s) above the header (study title, facility, date)\n", skipped);
    }

    const std::vector<std::string> headers = split_csv(header_line);
    const int c_cause = find_column(headers, {"initiating_cause", "initiating_event", "initiating_cause_event",
                                              "cause", "causes", "initiating_cause_event_description",
                                              "ursache", "ausloesendes_ereignis"},
        {{"initiating"}, {"cause"}, {"ursache"}}, {{"mitigated"}});
    const int c_freq  = find_column(headers, {"frequency", "frequency_yr", "initiating_event_frequency",
                                              "haeufigkeit", "eintrittshaeufigkeit"},
        {{"frequency"}, {"haeufigkeit"}}, {{"mitigated"}, {"target"}, {"total"}});
    const int c_mit   = find_column(headers, {"mitigated_event_frequency", "mitigated_frequency", "mitigated",
                                              "mitigated_event_frequency_yr"},
        {{"mitigated"}}, {{"total"}});
    const int c_tgt   = find_column(headers, {"target_risk_frequency", "target_frequency", "target",
                                              "tolerable_frequency", "zielhaeufigkeit"},
        {{"target"}, {"tolerable"}, {"zielhaeufigkeit"}}, {});

    std::vector<Layer> layers;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        bool is_cm = false;
        std::string lname;
        if (layer_column(headers[i], is_cm, lname)) {
            Layer l;
            l.name = layer_label(lname);
            l.declared_cm = is_cm;
            l.column = static_cast<int>(i);
            layers.push_back(l);
        }
    }

    if (c_cause < 0 || c_freq < 0 || layers.empty()) {
        std::fprintf(stderr, "error: a LOPA needs an initiating cause, a frequency, and at least one marked\n"
                             "       protection-layer column. Headers seen:");
        for (const std::string& h : headers) { std::fprintf(stderr, " '%s'", h.c_str()); }
        std::fprintf(stderr, "\n"
            "\n"
            "       Protection layers must be marked in the header -- 'IPL1', 'IPL A: drainage level switch',\n"
            "       'CM2', 'CM: probability of ignition'. This tool will not guess which columns hold PFDs,\n"
            "       because guessing wrong in either direction changes the arithmetic silently: a missed layer\n"
            "       makes the scenario look worse than the document says, and a column of something else read as\n"
            "       a PFD makes it look better.\n"
            "\n"
            "       Mark the columns, or use hazop2cb if this is a HAZOP worksheet.\n");
        return 1;
    }

    std::vector<Cause> causes;
    int lineno = phys;     // the header's own last line; each record below advances it by what it consumes
    std::string line;
    int consumed = 0;
    while (read_record(in, line, &consumed)) {
        const int row_start = lineno + 1;
        lineno += consumed;
        consumed = 0;
        if (line.empty() || line[0] == '#') { continue; }
        const std::vector<std::string> f = split_csv(line);
        const auto at = [&](const int i) -> std::string {
            return (i >= 0 && static_cast<std::size_t>(i) < f.size()) ? f[static_cast<std::size_t>(i)]
                                                                      : std::string();
        };
        Cause c;
        c.description = at(c_cause);
        c.line = row_start;
        if (c.description.empty()) { continue; }
        if (!parse_number(at(c_freq), c.frequency)) {
            // A total row at the foot of the table has no frequency; that is not an error.
            const std::string d = lower(c.description);
            if (d.find("total") == std::string::npos) {
                std::fprintf(stderr, "  note: line %d: no readable frequency ('%s'); row skipped\n",
                             c.line, at(c_freq).c_str());
            }
            continue;
        }
        for (const Layer& l : layers) {
            double p = 0.0;
            if (!parse_number(at(l.column), p)) { p = kNotApplicable; }
            c.pfd.push_back(p);
        }
        double m = 0.0;
        if (parse_number(at(c_mit), m)) { c.stated_mitigated = m; c.has_stated = true; }
        causes.push_back(c);
    }

    if (causes.empty()) {
        std::fprintf(stderr, "error: %s has a header but no readable initiating causes\n", path.c_str());
        return 1;
    }

    double target = 0.0;
    bool has_target = false;
    {
        // The target is usually stated once for the scenario, so take the first row that carries one.
        std::ifstream again(path);
        std::string l;
        while (read_record(again, l)) {  // NOLINT: separate stream, numbering untouched
            const std::vector<std::string> f = split_csv(l);
            if (c_tgt >= 0 && static_cast<std::size_t>(c_tgt) < f.size()) {
                double t = 0.0;
                if (parse_number(f[static_cast<std::size_t>(c_tgt)], t) && t > 0.0) {
                    target = t; has_target = true; break;
                }
            }
        }
    }

    Report report;
    // Every check is DECLARED before any of them runs, so the report can list the ones that found nothing. A
    // review naming three findings could be thorough or could be a tool that knows three things; naming the silent
    // checks is what turns silence into evidence.
    report.check("ARITHMETIC", "does each row's own product equal its own stated answer?");
    report.check("TARGET", "does the total meet the target the organisation set?");
    report.check("NOT A PROTECTION", "is a conditional modifier counted as a protection layer?");
    report.check("NOT INDEPENDENT", "does a layer depend on the system named as the initiating cause?");
    report.check("SHARED LAYER", "does one layer carry every initiating cause?");
    report.check("CREDIT LIMIT", "does a layer claim more credit than its kind is normally allowed?");
    report.check("UNIFORM PFD", "is every layer the same value, meaning a default rather than an assessment?");
    report.limit("Independence is checked only where the document NAMES a shared support system. Two layers wired "
                 "to the same power supply, I/O card, or calibration source without saying so are invisible here. "
                 "This implements a fragment of one CCPS criterion, not the criterion.");
    report.limit("Whether a stated frequency or PFD is CORRECT. Those come from the facility's own operating "
                 "experience and no tool can audit them. Only the arithmetic joining them is checked.");
    report.limit("Whether the list of initiating causes is COMPLETE. A cause nobody wrote down cannot be checked, "
                 "and completeness has no mechanical test -- it is the honest limit of this whole approach.");
    report.limit("Whether the consequence description matches the severity implied by the target frequency.");

    std::fprintf(stderr, "\n=== LOPA audit: %s ===\n\n", path.c_str());
    std::fprintf(stderr, "  %zu initiating cause(s), %zu protection layer column(s)",
                 causes.size(), layers.size());
    const std::size_t declared_cms =
        static_cast<std::size_t>(std::count_if(layers.begin(), layers.end(),
                                               [](const Layer& l) { return l.declared_cm; }));
    if (declared_cms > 0) {
        std::fprintf(stderr, " of which %zu declared conditional modifiers", declared_cms);
    }
    std::fprintf(stderr, "\n\n");

    int arithmetic = 0;
    int not_a_protection = 0;
    int not_independent = 0;
    int shared = 0;
    int over_credit = 0;

    // ── ARITHMETIC: redo each row's own product ─────────────────────────────
    //
    // The tolerance is deliberately loose. LOPA is semi-quantitative and its numbers are rounded to one significant
    // figure on purpose, so a 20% difference is the method working as intended. A factor of two is not roundable,
    // and a factor of ten is a wrong exponent -- which is what a spreadsheet typo produces and what nobody catches
    // by eye, because 1.00E-06 and 1.00E-05 differ by one character.
    double computed_total = 0.0;
    for (const Cause& c : causes) {
        double product = c.frequency;
        for (const double p : c.pfd) { product *= (p > 0.0 ? p : 1.0); }
        computed_total += product;
        if (!c.has_stated) { continue; }
        const double ratio = (c.stated_mitigated > 0.0) ? (product / c.stated_mitigated) : 0.0;
        if (ratio > 2.0 || ratio < 0.5) {
            ++arithmetic;
            std::fprintf(stderr, "  ARITHMETIC     line %d: this row's own numbers do not give its own answer\n",
                         c.line);
            std::fprintf(stderr, "                 cause:     %s\n", c.description.c_str());
            std::fprintf(stderr, "                 %.3g", c.frequency);
            for (const double p : c.pfd) {
                if (p > 0.0 && p < kNotApplicable) { std::fprintf(stderr, " x %.3g", p); }
            }
            std::fprintf(stderr, " = %.3g\n", product);
            std::fprintf(stderr, "                 stated:    %.3g   (out by a factor of %.3g)\n",
                         c.stated_mitigated, ratio > 1.0 ? ratio : 1.0 / ratio);
            char det[512];
            std::snprintf(det, sizeof(det),
                          "frequency %.3g times the applicable PFDs gives %.3g\nthe row states               %.3g\n"
                          "out by a factor of           %.3g",
                          c.frequency, product, c.stated_mitigated, ratio > 1.0 ? ratio : 1.0 / ratio);
            report.add(Finding{"ARITHMETIC", Weight::Arithmetic, c.line, c.description, det,
                "The document supplies both the inputs and the answer, and they disagree. A LOPA is "
                "semi-quantitative, so rounding to one significant figure is the method working as intended and is "
                "not reported here; a factor of two is not rounding, and a factor of ten is a wrong exponent. "
                "1.00E-06 and 1.00E-05 differ by one character in a spreadsheet and by an order of magnitude in the "
                "conclusion.",
                "Recomputing this row. Whichever value is wrong, the document currently states two different "
                "answers for the same scenario, and the total is built from one of them."});
        }
    }
    if (arithmetic > 0) {
        std::fprintf(stderr,
            "\n                 A LOPA is semi-quantitative, so rounding to one significant figure is the method\n"
            "                 working as intended and is not reported. A factor of two is not rounding, and a\n"
            "                 factor of ten is a wrong exponent -- 1.00E-06 and 1.00E-05 differ by one character\n"
            "                 in a spreadsheet and by an order of magnitude in the conclusion.\n\n");
    }

    // ── TARGET: the one conclusion the document exists to reach ─────────────
    std::fprintf(stderr, "  total mitigated frequency, recomputed: %.3g per year (1 in %.0f years)\n",
                 computed_total, computed_total > 0.0 ? 1.0 / computed_total : 0.0);
    if (has_target) {
        std::fprintf(stderr, "  target frequency stated in the document: %.3g per year\n", target);
        if (computed_total > target) {
            const double required = target / computed_total;
            std::fprintf(stderr,
                "\n  TARGET NOT MET the recomputed total is %.3g times the target the organisation set.\n",
                computed_total / target);
            std::fprintf(stderr,
                "                 A further layer with a PFD of %.3g or better would close the gap, which is\n"
                "                 %s.\n\n", required, sil_band(required).c_str());
            char det[512];
            std::snprintf(det, sizeof(det),
                          "recomputed total   %.3g per year  (1 in %.0f years)\n"
                          "stated target      %.3g per year\n"
                          "ratio              %.3g times the target\n"
                          "a further layer of %.3g or better would close the gap: %s",
                          computed_total, 1.0 / computed_total, target, computed_total / target, required,
                          sil_band(required).c_str());
            report.add(Finding{"TARGET", Weight::Conclusion, 0, "the total does not meet the stated target", det,
                "This is the one conclusion a LOPA exists to reach, and a well-run study states it plainly. It is "
                "reported here whether or not the document already does, because a reader must be able to see "
                "where the study landed without recomputing it. If your worksheet already says this, the finding "
                "is a confirmation rather than a criticism.",
                "Either a further protection layer, or an explicit decision by the organisation to accept the "
                "residual risk and record why. Both are legitimate; leaving the gap unstated is not."});
        } else {
            std::fprintf(stderr, "  the recomputed total meets the stated target.\n\n");
        }
    } else {
        std::fprintf(stderr,
            "\n  NO TARGET      this worksheet states no target risk frequency, so the one conclusion a LOPA\n"
            "                 exists to reach -- is this tolerable? -- cannot be checked, and was not checked\n"
            "                 by the arithmetic either. A mitigated frequency with nothing to compare it against\n"
            "                 is a number, not a decision.\n\n");
    }

    // ── NOT A PROTECTION: a conditional modifier credited as a layer ────────
    for (const Layer& l : layers) {
        if (l.declared_cm) { continue; }
        if (!describes_modifier(l.name)) { continue; }
        ++not_a_protection;
        std::fprintf(stderr, "  NOT A PROTECTION  '%s'\n", l.name.c_str());
        std::fprintf(stderr, "                 is marked as a protection layer, but it reads as a conditional\n"
                             "                 modifier.\n");
        report.add(Finding{"NOT A PROTECTION", Weight::Structural, 0, l.name,
            "marked as a protection layer; reads as a conditional modifier",
            "A conditional modifier reduces the arithmetic exactly as a protection layer does, and protects "
            "nothing: it says the consequence might not land, not that anything intervened. Counted among the IPLs "
            "it inflates the number of layers the scenario appears to have, which is the figure a reviewer "
            "eyeballs before approving the total. The arithmetic is identical either way, which is why nobody "
            "notices.",
            "Moving the column out of the IPL block and labelling it a conditional modifier. No number changes; "
            "what changes is what the document claims to have."});
    }
    if (not_a_protection > 0) {
        std::fprintf(stderr,
            "\n                 A conditional modifier reduces the arithmetic exactly as a protection layer does,\n"
            "                 and protects nothing: it says the consequence might not land, not that anything\n"
            "                 intervened. Counted among the IPLs it inflates the number of layers the scenario\n"
            "                 appears to have, which is the figure a reviewer eyeballs before approving it.\n\n");
    }

    // ── NOT INDEPENDENT of the initiating cause ─────────────────────────────
    //
    // CCPS criterion: an IPL must be independent of the initiating event. This is the check that found a real
    // violation in this project's own reference casebook the first time it ran, so it is worth applying to the
    // clearest case a LOPA offers -- the cause and the layer naming the same system in the same document.
    for (const Cause& c : causes) {
        const std::set<std::string> cause_sys = systems(c.description);
        if (cause_sys.empty()) { continue; }
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (!is_applicable(c.pfd[i])) { continue; }
            const std::set<std::string> layer_sys = systems(layers[i].name);
            std::vector<std::string> both;
            std::set_intersection(cause_sys.begin(), cause_sys.end(), layer_sys.begin(), layer_sys.end(),
                                  std::back_inserter(both));
            if (both.empty()) { continue; }
            ++not_independent;
            std::fprintf(stderr, "  NOT INDEPENDENT line %d: a layer credited here names the same system as the\n"
                                 "                 initiating cause\n", c.line);
            std::fprintf(stderr, "                 cause: %s\n", c.description.c_str());
            std::fprintf(stderr, "                 layer: %s   (PFD %.3g)\n", layers[i].name.c_str(), c.pfd[i]);
            std::fprintf(stderr, "                 shared: ");
            for (const std::string& s : both) { std::fprintf(stderr, "'%s' ", s.c_str()); }
            std::fprintf(stderr, "\n");
            std::string shared_names;
            for (const std::string& s : both) { shared_names += (shared_names.empty() ? "" : ", ") + s; }
            char det[1024];
            std::snprintf(det, sizeof(det),
                          "initiating cause  %.300s\nlayer credited    %.300s   (PFD %.3g)\nboth name         %s",
                          c.description.c_str(), layers[i].name.c_str(), c.pfd[i], shared_names.c_str());
            report.add(Finding{"NOT INDEPENDENT", Weight::Structural, c.line,
                "a layer credited here names the same support system as the cause", det,
                "CCPS requires a protection layer to be independent of the initiating event. If the named system "
                "is what failed, a layer that needs that system in order to function has not been shown to "
                "survive the failure it is credited against -- and the arithmetic multiplied its PFD in as though "
                "it had. This is a candidate, not a verdict: a layer may name a system and still be wired "
                "independently of it. The document does not say, which is the finding.",
                "One line in the worksheet stating how the layer is powered, actuated and annunciated. If it is "
                "genuinely independent, saying so costs nothing and the finding closes."});
        }
    }
    if (not_independent > 0) {
        std::fprintf(stderr,
            "\n                 CCPS requires a protection layer to be independent of the initiating event. If the\n"
            "                 control system is what failed, a layer that annunciates through that control system\n"
            "                 has not been shown to survive the failure it is credited against -- and the\n"
            "                 arithmetic multiplied its PFD in as though it had.\n"
            "\n"
            "                 This is a candidate, not a verdict: a layer may name a system and still be wired\n"
            "                 independently of it. The document does not say, which is the finding.\n\n");
    }

    // ── SHARED LAYER: one layer carrying most of the causes ─────────────────
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].declared_cm) { continue; }
        int used = 0;
        for (const Cause& c : causes) { if (is_applicable(c.pfd[i])) { ++used; } }
        const int total = static_cast<int>(causes.size());
        if (total < 2 || used < total) { continue; }
        ++shared;
        std::fprintf(stderr, "  SHARED LAYER   '%s'\n", layers[i].name.c_str());
        std::fprintf(stderr, "                 is credited against ALL %d initiating causes\n", total);
        char det[512];
        std::snprintf(det, sizeof(det), "credited against all %d of %d initiating causes", used, total);
        report.add(Finding{"SHARED LAYER", Weight::Observation, 0, layers[i].name, det,
            "A layer credited against every cause is a single point of failure for the whole scenario: if it is "
            "unavailable, every row's arithmetic is wrong at once, and the total understates the risk by the same "
            "factor in every row. This is not automatically a defect -- a well-chosen layer may genuinely cover "
            "every cause -- but it is the assumption the total most depends on and the one least often stated.",
            "Recording what makes this layer available, and how its unavailability would be detected. A layer "
            "carrying an entire scenario is worth a proof-test interval the others do not need."});
    }
    if (shared > 0) {
        std::fprintf(stderr,
            "\n                 A layer credited against every cause is a single point of failure for the whole\n"
            "                 scenario: if it is unavailable, every row's arithmetic is wrong at once, and the\n"
            "                 total understates the risk by the same factor in every row. This is not\n"
            "                 automatically a defect -- a well-chosen layer may genuinely cover every cause --\n"
            "                 but it is the assumption the total most depends on and the one least often stated.\n\n");
    }

    // ── CREDIT LIMIT: a PFD better than the kind of layer may claim ─────────
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Cap* cap = credit_cap(layers[i].name);
        if (cap == nullptr) { continue; }
        double best = kNotApplicable;
        int where = 0;
        for (const Cause& c : causes) {
            if (is_applicable(c.pfd[i]) && c.pfd[i] < best) { best = c.pfd[i]; where = c.line; }
        }
        if (best >= cap->floor_pfd) { continue; }
        ++over_credit;
        std::fprintf(stderr, "  CREDIT LIMIT   '%s'\n", layers[i].name.c_str());
        std::fprintf(stderr, "                 claims PFD %.3g (line %d), better than the %.3g usually allowed for\n"
                             "                 a %s layer.\n", best, where, cap->floor_pfd, cap->kind);
        std::fprintf(stderr, "                 %s\n", cap->why);
        char det[512];
        std::snprintf(det, sizeof(det), "claims PFD %.3g (line %d)\n%.3g is the usual limit for a %s layer",
                      best, where, cap->floor_pfd, cap->kind);
        report.add(Finding{"CREDIT LIMIT", Weight::Structural, where, layers[i].name, det,
            "IEC 61511 allows a basic process control system not designed to the standard a single risk reduction "
            "credit -- PFD 0.1, no better -- and a human or procedural layer is limited for the related reason "
            "that the number describes a person acting correctly during an upset. Each order of magnitude claimed "
            "beyond the limit is an order of magnitude the total is understated by.",
            "Either the evidence that justifies the tighter number -- a SIL rating, a proof-test record -- stated "
            "in the worksheet, or the PFD relaxed to the limit and the total recomputed. A layer declaring a SIL "
            "is exempt from this check, because that rating is exactly the evidence being asked for."});
    }
    if (over_credit > 0) {
        std::fprintf(stderr,
            "\n                 Each order of magnitude claimed here is an order of magnitude the total is\n"
            "                 understated by. If the claim is justified -- a certified function, a documented\n"
            "                 proof-test record -- say so in the worksheet, because the number alone does not.\n\n");
    }

    // ── UNIFORM PFD: defaults, not measurements ─────────────────────────────
    {
        std::set<double> distinct;
        for (const Cause& c : causes) {
            for (const double p : c.pfd) { if (is_applicable(p)) { distinct.insert(p); } }
        }
        if (distinct.size() == 1 && layers.size() > 1) {
            std::fprintf(stderr, "  UNIFORM PFD    every applicable layer in this study is %.3g\n", *distinct.begin());
            std::fprintf(stderr,
                "\n                 One value across every layer of every cause means a default was applied, not\n"
                "                 that each layer was assessed and found identical. That is a legitimate opening\n"
                "                 position for a screening study, and it is worth stating plainly, because the\n"
                "                 total inherits whatever confidence that default deserves -- and a reader cannot\n"
                "                 tell a default from a measurement by looking at it.\n\n");
            char det[256];
            std::snprintf(det, sizeof(det), "every applicable layer in this study is %.3g", *distinct.begin());
            report.add(Finding{"UNIFORM PFD", Weight::Observation, 0,
                "one PFD value across every layer of every cause", det,
                "One value everywhere means a default was applied, not that each layer was assessed and found "
                "identical. That is a legitimate opening position for a screening study and it is worth stating "
                "plainly, because the total inherits whatever confidence that default deserves -- and a reader "
                "cannot tell a default from a measurement by looking at it.",
                "Marking the study as a screening pass, or replacing the defaults with per-layer values where the "
                "total is close to the target."});
        }
    }

    // ── the draft casebook ──────────────────────────────────────────────────
    std::printf("# DRAFT casebook generated by lopa2cb from %s\n", path.c_str());
    std::printf("#\n");
    std::printf("# cbc WILL REFUSE THIS FILE, by design. A LOPA records what protects against a hazard and how\n");
    std::printf("# reliable each layer is; it does not record why a conclusion might not follow, which is what a\n");
    std::printf("# casebook reasons about. The defeaters, severities and runtime actions below are TODOs for an\n");
    std::printf("# engineer, and no tool can supply them.\n");
    std::printf("#\n");
    std::printf("# A LOPA's numbers do NOT survive into the casebook. A PFD is a long-run reliability figure and a\n");
    std::printf("# casebook reasons about the state the plant is in right now: whether a layer is AVAILABLE, not\n");
    std::printf("# how often it fails. Carrying 0.1 in as though it meant something at runtime would be the\n");
    std::printf("# category error this file exists to avoid.\n");
    std::printf("\ncasebook %s v1\n", ident(name).c_str());
    std::printf("source \"%s -- imported by lopa2cb\" dated TODO by \"TODO\"\n\n", path.c_str());

    std::printf("# Each initiating cause becomes an observation: it is a thing the plant can detect or be told.\n");
    std::set<std::string> seen;
    for (const Cause& c : causes) {
        const std::string id = ident(c.description);
        if (id.empty() || !seen.insert(id).second) { continue; }
        std::printf("observe %-28s \"%.60s\"\n", id.c_str(), c.description.c_str());
    }
    std::printf("\n# Each protection layer becomes an observation too -- of its OWN availability. That is the\n");
    std::printf("# question a runtime casebook can answer about a layer, and the PFD is not.\n");
    for (const Layer& l : layers) {
        if (l.declared_cm) { continue; }
        const std::string id = ident(l.name + " available");
        if (id.empty() || !seen.insert(id).second) { continue; }
        std::printf("observe %-28s \"%.60s available\"\n", id.c_str(), l.name.c_str());
    }
    std::printf("\n");
    for (const Cause& c : causes) {
        const std::string id = ident(c.description);
        if (id.empty()) { continue; }
        std::printf("claim %s_unprotected \"%.50s with no layer available\" needs %s\n",
                    id.c_str(), c.description.c_str(), id.c_str());
        std::printf("# TODO: add `excuse` clauses naming the layers that would defeat this claim, and a severity.\n");
    }
    std::printf("\n# TODO: declare the protective action, and one marked `idle`.\n");
    std::printf("# TODO: no claim above has a defeater yet, so cbc will refuse this file as not defeasible.\n");

    // Figures the document states that were recomputed and agree. A review that reports only problems offers no
    // evidence it understood the document, and on a correct study this is the entire result.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.3g per year (1 in %.0f years)", computed_total,
                      computed_total > 0.0 ? 1.0 / computed_total : 0.0);
        report.confirmed("total mitigated event frequency, recomputed from the rows", buf);
        int agreed = 0;
        for (const Cause& c : causes) {
            if (!c.has_stated) { continue; }
            double product = c.frequency;
            for (const double p : c.pfd) { product *= (p > 0.0 ? p : 1.0); }
            const double r = (c.stated_mitigated > 0.0) ? (product / c.stated_mitigated) : 0.0;
            if (r <= 2.0 && r >= 0.5) { ++agreed; }
        }
        std::snprintf(buf, sizeof(buf), "%d of %zu row(s) reproduce their own stated answer", agreed,
                      causes.size());
        report.confirmed("row arithmetic", buf);
        if (has_target && computed_total <= target) {
            std::snprintf(buf, sizeof(buf), "%.3g per year, and the total is below it", target);
            report.confirmed("target risk frequency", buf);
        }
    }

    if (!report_path.empty()) {
        std::ifstream raw(path, std::ios::binary);
        std::ostringstream all;
        all << raw.rdbuf();
        const std::string sha = quorum::sha256::hex(all.str());
        std::string prov =
            "This review was produced mechanically from the worksheet named above. No cell was retyped,\n"
            "reworded or reinterpreted on the way in -- which matters, because a finding derived from a\n"
            "transcription is a finding about the transcription.";
        const std::string cmd = "lopa2cb " + path + " " + name + " --report " + report_path;
        if (!report.markdown(report_path, path, sha, "lopa2cb", prov, cmd)) {
            std::fprintf(stderr, "error: cannot write report to %s\n", report_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "  report written to %s  (%zu finding(s), document SHA-256 %.16s...)\n\n",
                     report_path.c_str(), report.size(), sha.c_str());
    }

    const int findings = arithmetic + not_a_protection + not_independent + shared + over_credit;
    std::fprintf(stderr, "  %d arithmetic, %d not a protection, %d not independent, %d shared layer(s),\n"
                         "  %d over the usual credit limit\n\n", arithmetic, not_a_protection, not_independent,
                 shared, over_credit);
    if (findings > 0) {
        std::fprintf(stderr,
            "  None of these changes a number in your document. Each one names a place where the document's own\n"
            "  conclusion rests on something the document does not state, and every one was derived from the\n"
            "  worksheet as written rather than from an opinion about how it should have been written.\n\n");
    }
    return 0;
}
