// learn2cb -- draft a casebook from a table of past records. HOST ONLY.
//
//   learn2cb <records.csv> --outcome <column> [--positive <value>] [--name <id>] [--k <n>]
//            [--ignore <col,col>] [--asserts <word>] [--otherwise <word>] [--by <who>] [--dated <YYYY-MM-DD>]
//
// One row per past event; one column says what it turned out to be. Numeric columns are cut where the outcome
// changes (MDL discretisation, at most 3 cuts each) into `<column>_above_<value>` observations; text columns with at
// most 8 distinct values become `<column>_is_<value>`; anything else is skipped and the skip is reported. The k most
// informative observations are kept (default 10, at most 20, because cbc validates every one of 2^k states and the
// device sweeps them) and AA-CBR is fitted over them. The casebook goes to stdout, the report to stderr.
//
// WHAT THE REPORT SAYS, AND WHY
//
// The learning stage was tested four times against pre-registered criteria and its benefit was NOT SHOWN all four
// times (learn/PREREGISTRATION.md). This tool therefore estimates its own worth on the records it was given, by 5-fold
// cross-validation against the majority class, and prints that estimate rather than letting the casebook's existence
// imply one. It also flags any single observation that almost determines the outcome: in real maintenance tables
// that is usually a column recorded AFTER the event (AI4I's failure-type flags are the textbook case), and a casebook
// that learned it would be useless on a live machine.
//
// The output is a DRAFT for an engineer. cbc validates it exactly as it validates a hand-written casebook.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/learn/aacbr.hpp"
#include "quorum/learn/baselines.hpp"
#include "quorum/learn/discretize.hpp"
#include "quorum/learn/emit_cb.hpp"
#include "quorum/learn/features.hpp"
#include "quorum/learn/hybrid.hpp"

namespace L = quorum::learn;

namespace {

std::vector<std::vector<std::string>> read_csv(std::istream& in) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string f;
    bool q = false;
    char ch = 0;
    bool any = false;
    while (in.get(ch)) {
        any = true;
        if (q) {
            if (ch == '"') {
                if (in.peek() == '"') {
                    f += '"';
                    in.get(ch);
                } else {
                    q = false;
                }
            } else {
                f += ch;
            }
        } else if (ch == '"') {
            q = true;
        } else if (ch == ',') {
            row.push_back(f);
            f.clear();
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && in.peek() == '\n') { in.get(ch); }
            row.push_back(f);
            f.clear();
            rows.push_back(row);
            row.clear();
        } else {
            f += ch;
        }
    }
    if (any && (!f.empty() || !row.empty())) {
        row.push_back(f);
        rows.push_back(row);
    }
    return rows;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) { s.pop_back(); }
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) { ++i; }
    return s.substr(i);
}

/// A valid casebook identifier: lower case, [a-z0-9_], starting with a letter.
std::string ident(const std::string& s) {
    std::string o;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            o += static_cast<char>(std::tolower(u));
        } else if (!o.empty() && o.back() != '_') {
            o += '_';
        }
    }
    while (!o.empty() && o.back() == '_') { o.pop_back(); }
    if (o.empty() || std::isalpha(static_cast<unsigned char>(o[0])) == 0) { o = "x_" + o; }
    return o;
}

std::string number_ident(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    std::string s;
    for (const char c : std::string(buf)) {
        if (c == '.') {
            s += 'p';
        } else if (c == '-') {
            s += "neg";
        } else if (c == '+') {
        } else {
            s += c;
        }
    }
    return s;
}

bool parse_number(const std::string& s, double& v) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(v);
}

struct Candidate {
    std::string name;   ///< casebook identifier
    std::string label;  ///< human description
    std::vector<bool> value;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: learn2cb <records.csv> --outcome <column> [--positive <value>] [--name <id>] [--k <n>]\n"
                 "                [--ignore <col,col>] [--asserts <word>] [--otherwise <word>] [--by <who>]\n"
                 "                [--dated <YYYY-MM-DD>]\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); }
    std::string path = argv[1], outcome_col, positive, name, by = "quorum learn2cb (AA-CBR)", dated;
    std::string asserts = "fault", otherwise = "normal";
    std::set<std::string> ignore;
    int k = 10;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) { usage(); }
        const std::string v = argv[++i];
        if (a == "--outcome") {
            outcome_col = v;
        } else if (a == "--positive") {
            positive = v;
        } else if (a == "--name") {
            name = v;
        } else if (a == "--k") {
            k = std::atoi(v.c_str());
        } else if (a == "--by") {
            by = v;
        } else if (a == "--dated") {
            dated = v;
        } else if (a == "--asserts") {
            asserts = ident(v);
        } else if (a == "--otherwise") {
            otherwise = ident(v);
        } else if (a == "--ignore") {
            std::stringstream ss(v);
            for (std::string c; std::getline(ss, c, ',');) { ignore.insert(trim(c)); }
        } else {
            usage();
        }
    }
    if (outcome_col.empty()) { usage(); }
    if (dated.empty()) {  // the day the draft was made, which is what the provenance line records
        const std::time_t now = std::time(nullptr);
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&now));
        dated = buf;
    }
    if (k < 1 || k > 20) {
        std::fprintf(stderr, "error: --k must be 1..20 (cbc validates all 2^k states)\n");
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
        return 2;
    }
    auto rows = read_csv(in);
    while (!rows.empty() && rows.back().size() == 1 && trim(rows.back()[0]).empty()) { rows.pop_back(); }
    if (rows.size() < 11) {
        std::fprintf(stderr, "error: need a header and at least 10 records; found %zu rows\n", rows.size());
        return 1;
    }
    std::vector<std::string> header;
    for (auto h : rows[0]) {
        if (header.empty() && h.size() >= 3 && static_cast<unsigned char>(h[0]) == 0xEF) { h = h.substr(3); }  // BOM
        header.push_back(trim(h));
    }
    const auto oc = std::find(header.begin(), header.end(), outcome_col);
    if (oc == header.end()) {
        std::fprintf(stderr, "error: no column named '%s'. Columns:", outcome_col.c_str());
        for (const auto& h : header) { std::fprintf(stderr, " '%s'", h.c_str()); }
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::size_t ocol = static_cast<std::size_t>(oc - header.begin());

    // Outcome: --positive names the value that means "it happened"; otherwise 1/yes/true, or the rarer of two values.
    std::vector<std::vector<std::string>> data;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        if (rows[r].size() == header.size()) { data.push_back(rows[r]); }
    }
    const std::size_t dropped = rows.size() - 1 - data.size();
    std::map<std::string, int> ovals;
    for (const auto& r : data) { ++ovals[trim(r[ocol])]; }
    if (positive.empty()) {
        for (const char* p : {"1", "yes", "Yes", "YES", "true", "True", "TRUE", "y", "Y"}) {
            if (ovals.count(p) != 0U) {
                positive = p;
                break;
            }
        }
        if (positive.empty() && ovals.size() == 2) {
            positive = ovals.begin()->second <= std::next(ovals.begin())->second ? ovals.begin()->first
                                                                                  : std::next(ovals.begin())->first;
        }
    }
    if (positive.empty() || ovals.count(positive) == 0U) {
        std::fprintf(stderr, "error: cannot tell which value of '%s' means the outcome happened; pass --positive. "
                     "Values seen:", outcome_col.c_str());
        for (const auto& kv : ovals) { std::fprintf(stderr, " '%s' (%d)", kv.first.c_str(), kv.second); }
        std::fprintf(stderr, "\n");
        return 1;
    }
    std::vector<bool> y;
    for (const auto& r : data) { y.push_back(trim(r[ocol]) == positive); }
    const int pos = static_cast<int>(std::count(y.begin(), y.end(), true));
    if (pos == 0 || pos == static_cast<int>(y.size())) {
        std::fprintf(stderr, "error: every record has the same outcome; there is nothing to learn\n");
        return 1;
    }

    // Candidate observations.
    std::vector<Candidate> cands;
    std::vector<std::string> skipped;
    for (std::size_t c = 0; c < header.size(); ++c) {
        if (c == ocol) { continue; }
        if (ignore.count(header[c]) != 0U) {
            skipped.push_back("'" + header[c] + "': ignored as asked");
            continue;
        }
        std::vector<double> nums;
        bool numeric = true;
        std::set<std::string> distinct;
        for (const auto& r : data) {
            const std::string v = trim(r[c]);
            distinct.insert(v);
            double d = 0;
            if (numeric && parse_number(v, d)) {
                nums.push_back(d);
            } else {
                numeric = false;
            }
        }
        const std::string base = ident(header[c]);
        if (distinct.size() <= 1) {
            skipped.push_back("'" + header[c] + "': the same in every record");
        } else if (numeric && distinct.size() > 2) {
            if (distinct.size() == data.size()) {
                // A strictly increasing counter is almost always an ID; cutting it learns the order of the file.
                bool monotone = true;
                for (std::size_t i = 1; i < nums.size() && monotone; ++i) { monotone = nums[i] > nums[i - 1]; }
                if (monotone) {
                    skipped.push_back("'" + header[c] + "': every value distinct and increasing, so an ID");
                    continue;
                }
            }
            const std::vector<double> cuts = L::mdlp_cuts(nums, y, 3);
            if (cuts.empty()) {
                skipped.push_back("'" + header[c] + "': no cut where the outcome changes (MDL)");
            }
            for (const double cut : cuts) {
                Candidate cd;
                cd.name = base + "_above_" + number_ident(cut);
                char lab[160];
                std::snprintf(lab, sizeof lab, "%s above %.6g", header[c].c_str(), cut);
                cd.label = lab;
                for (const double d : nums) { cd.value.push_back(d > cut); }
                cands.push_back(cd);
            }
        } else if (distinct.size() <= 8) {
            for (const auto& v : distinct) {
                if (distinct.size() == 2 && v == *distinct.begin()) { continue; }  // two values: one observation
                Candidate cd;
                cd.name = base + "_is_" + ident(v.empty() ? std::string("blank") : v);
                cd.label = header[c] + " is " + (v.empty() ? "blank" : v);
                for (const auto& r : data) { cd.value.push_back(trim(r[c]) == v); }
                cands.push_back(cd);
            }
        } else {
            skipped.push_back("'" + header[c] + "': " + std::to_string(distinct.size()) +
                              " distinct text values; not a category");
        }
    }
    if (cands.empty()) {
        std::fprintf(stderr, "error: no column could be turned into an observation\n");
        return 1;
    }

    // Rank every candidate by information gain over the outcome; the learner sees the best 32 (one bit each).
    const auto gain = [&y](const std::vector<bool>& v) {
        double n = static_cast<double>(y.size()), p = 0, n1 = 0, p1 = 0;
        for (std::size_t i = 0; i < y.size(); ++i) {
            p += y[i] ? 1 : 0;
            if (v[i]) {
                n1 += 1;
                p1 += y[i] ? 1 : 0;
            }
        }
        const double h = L::entropy2(p, n);
        const double rem = (n1 / n) * L::entropy2(p1, n1) + ((n - n1) / n) * L::entropy2(p - p1, n - n1);
        return std::make_pair(h - rem, h);
    };
    std::vector<std::pair<double, std::size_t>> ranked;
    std::vector<std::string> leaks;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const auto g = gain(cands[i].value);
        ranked.emplace_back(g.first, i);
        // Two signs of a column filled in after the event: it alone explains nearly all of the outcome, or the outcome
        // follows it almost without exception. AI4I's failure-mode flags (TWF, HDF, ...) show the second: each covers
        // only part of the failures, but every record that has one failed.
        std::size_t on = 0, on_pos = 0;
        for (std::size_t r = 0; r < y.size(); ++r) {
            if (cands[i].value[r]) {
                ++on;
                on_pos += y[r] ? 1U : 0U;
            }
        }
        const bool explains = g.second > 0 && g.first >= 0.9 * g.second;
        const bool certain = on >= 10 && static_cast<double>(on_pos) >= 0.98 * static_cast<double>(on);
        if (explains || certain) {
            char why[96];
            std::snprintf(why, sizeof why, "   (outcome in %zu of %zu records that have it)", on_pos, on);
            leaks.push_back(cands[i].label + why);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (ranked.size() > 32) { ranked.resize(32); }
    const int feature_count = static_cast<int>(ranked.size());
    if (k > feature_count) { k = feature_count; }

    std::vector<L::Case> cases(data.size());
    for (std::size_t r = 0; r < data.size(); ++r) {
        L::Features x = 0;
        for (int f = 0; f < feature_count; ++f) {
            if (cands[ranked[static_cast<std::size_t>(f)].second].value[r]) { x |= 1U << static_cast<std::uint32_t>(f); }
        }
        cases[r] = {x, y[r]};
    }
    static std::vector<std::string> names(32), labels(32);
    static const char* name_ptrs[32];
    for (int f = 0; f < 32; ++f) {
        if (f < feature_count) {
            names[static_cast<std::size_t>(f)] = cands[ranked[static_cast<std::size_t>(f)].second].name;
            labels[static_cast<std::size_t>(f)] = cands[ranked[static_cast<std::size_t>(f)].second].label;
        } else {
            names[static_cast<std::size_t>(f)] = "unused_" + std::to_string(f);
        }
        name_ptrs[f] = names[static_cast<std::size_t>(f)].c_str();
    }

    // 5-fold cross-validation: the learned casebook against always predicting the majority outcome.
    L::Confusion learned_cv, majority_cv;
    const L::Knowledge kb{};
    for (int fold = 0; fold < 5; ++fold) {
        std::vector<L::Case> train, test;
        for (std::size_t r = 0; r < cases.size(); ++r) { (static_cast<int>(r % 5) == fold ? test : train).push_back(cases[r]); }
        if (train.empty() || test.empty()) { continue; }
        learned_cv.add(L::run_method(L::Method::Learned, kb, train, test, k, feature_count, nullptr));
        const bool maj = L::majority_outcome(train);
        for (const auto& t : test) { majority_cv.add(maj, t.outcome); }
    }

    L::Features mask = 0;
    const auto model = L::fit_aacbr(L::Method::Learned, kb, cases, k, feature_count, &mask);
    L::EmitOptions opt;
    opt.name = name.empty() ? ident(path.substr(path.find_last_of("/\\") + 1, path.find_last_of('.') - path.find_last_of("/\\") - 1)) : ident(name);
    opt.document = "learned from " + path.substr(path.find_last_of("/\\") + 1) + " -- " + std::to_string(data.size()) +
                   " records, outcome column '" + outcome_col + "' = '" + positive + "'";
    opt.dated = dated;
    opt.by = by;
    opt.feature_names = name_ptrs;
    opt.asserts = asserts;
    opt.otherwise = otherwise;
    opt.notes = {"LEARNED DRAFT. Read every rule before trusting it: a learner finds patterns in the records it was",
                 "given, including accidents of how they were recorded.",
                 "5-fold cross-validated balanced accuracy on these records: see the learn2cb report."};
    L::CasebookText text;
    try {
        text = L::emit_casebook(*model, mask, opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::string src = text.source;
    // Describe each observation in words, so the draft reads as the records' columns rather than bit names.
    for (int f = 0; f < feature_count; ++f) {
        const std::string& n = names[static_cast<std::size_t>(f)];
        const std::string from = "observe " + n + " ";
        const std::size_t at = src.find(from);
        if (at == std::string::npos) { continue; }
        const std::size_t eol = src.find('\n', at);
        const std::string line = src.substr(at, eol - at);
        const std::string bare = "\"" + n + "\"";
        const std::string worded = "\"" + labels[static_cast<std::size_t>(f)] + "\"";
        const std::size_t q = line.find(bare);
        if (q != std::string::npos) {
            std::string l2 = line;
            l2.replace(q, bare.size(), worded);
            src.replace(at, eol - at, l2);
        } else if (line.find('"') == std::string::npos) {
            src.replace(at, eol - at, line + "  " + worded);
        }
    }
    std::fputs(src.c_str(), stdout);

    std::fprintf(stderr, "\n=== LEARNING REPORT ===\n\n");
    std::fprintf(stderr, "records          %zu used, %zu dropped (wrong number of fields)\n", data.size(), dropped);
    std::fprintf(stderr, "outcome          '%s' = '%s' in %d of %zu records (the casebook's '%s')\n", outcome_col.c_str(),
                 positive.c_str(), pos, data.size(), asserts.c_str());
    std::fprintf(stderr, "observations     %zu candidates from the columns; the %d most informative kept\n",
                 cands.size(), k);
    for (int f = 0; f < feature_count; ++f) {
        if ((mask & (1U << static_cast<std::uint32_t>(f))) != 0U) {
            std::fprintf(stderr, "                 %-36s %s\n", names[static_cast<std::size_t>(f)].c_str(),
                         labels[static_cast<std::size_t>(f)].c_str());
        }
    }
    for (const auto& s : skipped) { std::fprintf(stderr, "skipped          %s\n", s.c_str()); }
    std::fprintf(stderr, "cases            %zu kept, %d pruned (no path to the default)\n", model->cases().size(),
                 text.spikes_pruned);
    std::fprintf(stderr, "\nHOW GOOD IS IT? 5-fold cross-validation on these records, balanced accuracy\n");
    std::fprintf(stderr, "  learned casebook          %.3f\n  always the majority (%s)  %.3f\n", learned_cv.ba(),
                 L::majority_outcome(cases) ? asserts.c_str() : otherwise.c_str(), majority_cv.ba());
    if (learned_cv.ba() <= majority_cv.ba() + 0.02) {
        std::fprintf(stderr, "  The learned casebook is NO BETTER than always giving the same answer on these records.\n");
    }
    std::fprintf(stderr, "  This is an estimate on the records given, not a property of any machine. In four\n"
                         "  pre-registered tests the learning stage's benefit was NOT SHOWN (learn/PREREGISTRATION.md).\n");
    if (!leaks.empty()) {
        std::fprintf(stderr, "\nWARNING: the outcome almost always follows these. Check that each is known BEFORE the\n"
                             "event; a column filled in afterwards teaches nothing a machine can use. Re-run with\n"
                             "--ignore <column,...> to leave such columns out:\n");
        for (const auto& l : leaks) { std::fprintf(stderr, "  %s\n", l.c_str()); }
    }
    std::fprintf(stderr,
                 "\nThe casebook is a DRAFT. Read the rules, then check it. If the check refuses it because no claim\n"
                 "can be argued against, the records were explained by plain rules with no exceptions: that is a\n"
                 "checklist, and quorum adds nothing over one. Add the excuses an engineer knows, or use a checklist.\n");
    return 0;
}
