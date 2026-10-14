// worksheet.hpp -- shared CSV worksheet parsing for the document importers.
//
// WHY THIS HEADER EXISTS
//
// hazop2cb and lopa2cb both read a safety worksheet exported from Excel, and both must survive the same set of
// things real worksheets do. Those behaviours were established the hard way, one falsified assumption at a time:
//
//   * a quoted cell may contain NEWLINES (RFC 4180) because teams bullet several causes into one cell. Reading
//     with a plain getline cut the record short, shifted every later column, and reported a hazard as unprotected
//     while its safeguard sat in the file.
//   * the header is NOT the first row. An Excel export carries a study title, a facility, a date and a blank row
//     above it.
//   * headers are compound -- "Control/Existing", "Control (existing)", "Existing-Controls" -- so matching needs a
//     token pass, and needs FORBIDDEN tokens so that "Control/suggested" (a recommendation) is never read as an
//     existing protection.
//
// Every one of those was a defect in the HAZOP importer before it was a feature. Duplicating this code into a
// second importer would guarantee that the next fix of that class lands in only one of them, and the whole premise
// of this project is that silent, confident, wrong output is the defect worth the most effort to prevent.

#ifndef QUORUM_CASEBOOK_FMEA_WORKSHEET_HPP
#define QUORUM_CASEBOOK_FMEA_WORKSHEET_HPP

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <istream>
#include <set>
#include <string>
#include <vector>

namespace quorum::worksheet {

/// Read one CSV RECORD, which may span several LINES.
///
/// RFC 4180 permits a newline inside a quoted field, and Excel emits them whenever a cell contains a bulleted
/// list -- which is exactly how HAZOP teams write several causes for one deviation.
///
/// Reading with a plain getline broke this silently and destructively: the record was cut at the embedded newline,
/// every column after it shifted, and a deviation with a real safeguard in the file was reported UNPROTECTED.
/// Wrong output, confidently produced, with data dropped -- the exact failure this project exists to catch.
/// `lines` receives the number of PHYSICAL lines this record consumed, so a caller can report a line number that
/// matches what the author sees in their file.
///
/// Counting records instead of lines produced the third occurrence of misleading line numbers in this project, and
/// the worst, because it reached a customer-facing report: a row physically at line 51 of a worksheet was reported
/// as "line 3", and the test asserted the wrong value, cementing it. A record may span several lines (RFC 4180) and
/// comment lines are skipped entirely, so neither records nor data rows track physical position.
bool read_record(std::istream& in, std::string& out, int* lines = nullptr) {
    out.clear();
    std::string line;
    bool quoted = false;
    while (std::getline(in, line)) {
        if (lines != nullptr) { ++*lines; }
        if (!out.empty()) { out.push_back('\n'); }
        out += line;
        for (const char c : line) {
            if (c == '"') { quoted = !quoted; }
        }
        if (!quoted) { return true; }
        // Unterminated quote: the field continues on the next line.
    }
    return !out.empty();
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; }
                else { quoted = false; }
            } else { cur.push_back(c); }   // includes embedded newlines, kept for the multi-cause split
        } else if (c == '"') { quoted = true; }
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else { cur.push_back(c); }
    }
    out.push_back(cur);
    for (std::string& s : out) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) { s.erase(s.begin()); }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) { s.pop_back(); }
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

/// An identifier cbc will accept and a human will recognise.
std::string ident(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') { out.pop_back(); }
    if (out.empty()) { out = "x"; }
    // A leading digit is not a legal identifier. Prefixing is what emit.cpp does, and it is why the X-ray FMEA's
    // nonsense `3_abnormal` observations compiled instead of failing loudly.
    if (std::isdigit(static_cast<unsigned char>(out[0])) != 0) { out.insert(out.begin(), '_'); }
    if (out.size() > 44) { out.resize(44); }
    while (!out.empty() && out.back() == '_') { out.pop_back(); }
    return out;
}

/// Split a header into lowercase word tokens. '/', '-', '(' and ')' are separators, because real worksheets write
/// compound headers like "Control/Existing", "Control (existing)" and "Existing-Controls".
std::set<std::string> header_tokens(const std::string& h) {
    std::set<std::string> out;
    std::string cur;
    for (const char c : lower(h)) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) { cur.push_back(c); }
        else { if (!cur.empty()) { out.insert(cur); cur.clear(); } }
    }
    if (!cur.empty()) { out.insert(cur); }
    return out;
}

/// Find a column by exact normalised name, then by required/forbidden tokens.
///
/// WHY THE TOKEN PASS EXISTS -- I normalised a real document to fit this function and did not notice.
///
/// The hospital-oxygen HAZOP (PLOS ONE 2023) labels its columns "Control/Existing", "Control/suggested" and
/// "Effects". When transcribing it I silently retyped those as "Safeguards", "Action" and "Consequence", got 13
/// findings, and reported them. With the paper's ACTUAL headers the tool reported NO SAFEGUARDS COLUMN and found
/// nothing at all.
///
/// That is worse than a missed finding. The entire argument for building this importer was that a mechanical import
/// makes a finding attributable to the customer's document rather than to my translation of it -- and I had done
/// the translation by hand anyway.
///
/// `forbidden` matters here: the same document has BOTH "Control/Existing" and "Control/suggested". Matching
/// loosely on "control" would silently pick the RECOMMENDATIONS column and analyse protections that do not exist
/// yet -- plausible output, entirely wrong, which is this project's signature failure.
int find_column(const std::vector<std::string>& headers, const std::vector<std::string>& names,
                const std::vector<std::vector<std::string>>& token_sets = {},
                const std::vector<std::string>& forbidden = {}) {
    for (std::size_t i = 0; i < headers.size(); ++i) {
        std::string norm;
        for (const char c : lower(headers[i])) { norm.push_back(c == ' ' || c == '-' ? '_' : c); }
        for (const std::string& n : names) {
            if (norm == n) { return static_cast<int>(i); }
        }
    }
    // Token pass, in the order given, so more specific sets win.
    for (const std::vector<std::string>& want : token_sets) {
        for (std::size_t i = 0; i < headers.size(); ++i) {
            const std::set<std::string> toks = header_tokens(headers[i]);
            bool bad = false;
            for (const std::string& f : forbidden) {
                if (toks.count(f) != 0) { bad = true; break; }
            }
            if (bad) { continue; }
            bool all = true;
            for (const std::string& w : want) {
                if (toks.count(w) == 0) { all = false; break; }
            }
            if (all) { return static_cast<int>(i); }
        }
    }
    return -1;
}

/// Tag-like tokens -- LT-101, PSV-101, FAL-101. Instrument tags are how a HAZOP names the thing that measures or
/// protects, so comparing tags between the Cause and Safeguards columns is how common cause becomes visible.
std::set<std::string> tags(const std::string& text) {
    std::set<std::string> out;
    std::string cur;
    const auto flush = [&]() {
        // A tag is letters, then a separator, then digits: at least two letters and one digit.
        std::size_t letters = 0, digits = 0;
        for (const char c : cur) {
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) { ++letters; }
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) { ++digits; }
        }
        if (letters >= 2 && digits >= 1) { out.insert(lower(cur)); }
        cur.clear();
    };
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') { cur.push_back(c); }
        else { flush(); }
    }
    flush();
    return out;
}

}  // namespace quorum::worksheet

#endif  // QUORUM_CASEBOOK_FMEA_WORKSHEET_HPP
