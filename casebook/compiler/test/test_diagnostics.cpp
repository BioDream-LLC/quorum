// Diagnostics quality: does cbc tell an engineer what they did wrong?
//
// This is the test that decides whether anybody other than the tool's author can use the language. A compiler that
// rejects bad input is table stakes; one that says *where* and *what to do instead* is the difference between a
// reliability engineer finishing a casebook and abandoning it.
//
// Every case below is a mistake a real author would plausibly make, and each asserts three things:
//
//   1. the error is reported AT THE LINE OF THE OFFENDING CONSTRUCT -- not at the top of the file, not at the
//      provenance block, not at line 0. A misleading line number is worse than none, because it sends the reader
//      to correct code;
//   2. the message NAMES the offending symbol, so it can be found by searching;
//   3. the message says what to DO, not only what is wrong.
//
// Criterion 3 is checked by requiring guidance vocabulary. That is a crude proxy, but a message with no verb telling
// the reader what to write is reliably a bad message, and the proxy catches those.

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "quorum/cbc/cbc.hpp"

namespace cbc = quorum::cbc;

namespace {

int g_checks = 0;
int g_failures = 0;

struct Compiled {
    cbc::Diagnostics diag;
    cbc::Lowered lo;
};

Compiled compile(const std::string& text) {
    Compiled c;
    const cbc::Casebook cb = cbc::parse(text, c.diag);
    if (!c.diag.has_errors()) {
        c.lo = cbc::lower(cb, c.diag);
        if (!c.diag.has_errors()) { cbc::check(c.lo, c.diag); }
    }
    return c;
}

/// Guidance vocabulary. A diagnostic containing none of these tells the reader a fact and leaves them stuck.
///
/// Matched case-insensitively, because "Use: unit, range, ..." is guidance and a case-sensitive check would reject it
/// for starting a sentence.
bool offers_guidance(const std::string& message) {
    std::string m = message;
    for (char& ch : m) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }

    static const char* const kVerbs[] = {"write",   "use ",   "expected", "needs",  "must",   "cannot",
                                         "declare", "mark",   "split",    "instead", "add ",  "remove",
                                         "rename",  "give",   "either",   "belong",  "put ",  "choose",
                                         "replace", "should", "requires"};
    for (const char* v : kVerbs) {
        if (m.find(v) != std::string::npos) { return true; }
    }
    return false;
}

/// One realistic authoring mistake.
struct Case {
    const char* what;      ///< the mistake, in an author's terms
    std::string source;    ///< a casebook containing it
    int expect_line;       ///< the line the author needs to be sent to; 0 means whole-file
    const char* expect;    ///< a substring the message must contain
    const char* symbol;    ///< the name that must appear, or nullptr
};

int g_line_failures = 0;
int g_symbol_failures = 0;
int g_guidance_failures = 0;
int g_cases = 0;

void run(const Case& c) {
    ++g_cases;
    std::printf("  %s\n", c.what);

    const Compiled r = compile(c.source);

    // Find the diagnostic that matches. Errors and warnings both count: a warning about a blind sensor has to be as
    // actionable as an error.
    auto lower = [](std::string v) {
        for (char& ch : v) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        return v;
    };
    const cbc::Diagnostic* found = nullptr;
    for (const cbc::Diagnostic& d : r.diag.items()) {
        if (lower(d.message).find(lower(c.expect)) != std::string::npos) {
            found = &d;
            break;
        }
    }

    if (found == nullptr) {
        std::printf("      FAIL  no diagnostic contained \"%s\". Got:\n", c.expect);
        for (const cbc::Diagnostic& d : r.diag.items()) {
            std::printf("              line %d: %s\n", d.line, d.message.c_str());
        }
        if (r.diag.items().empty()) { std::printf("              (nothing at all -- the mistake was ACCEPTED)\n"); }
        ++g_failures;
        ++g_checks;
        return;
    }
    ++g_checks;

    if (c.expect_line != 0 && found->line != c.expect_line) {
        std::printf("      FAIL  reported at line %d, author needs line %d\n", found->line, c.expect_line);
        std::printf("              \"%s\"\n", found->message.c_str());
        ++g_failures;
        ++g_line_failures;
    }
    ++g_checks;

    if (c.symbol != nullptr && lower(found->message).find(lower(c.symbol)) == std::string::npos) {
        std::printf("      FAIL  message does not name '%s', so it cannot be found by searching\n", c.symbol);
        std::printf("              \"%s\"\n", found->message.c_str());
        ++g_failures;
        ++g_symbol_failures;
    }
    ++g_checks;

    if (!offers_guidance(found->message)) {
        std::printf("      FAIL  message states a fact but does not say what to do\n");
        std::printf("              \"%s\"\n", found->message.c_str());
        ++g_failures;
        ++g_guidance_failures;
    }
    ++g_checks;
}

// A valid casebook, so each case differs from it by exactly one mistake. Line numbers below are counted against
// whichever string each case builds, so they are written out in full rather than concatenated from fragments.

}  // namespace

int main() {
    std::printf("diagnostics quality: can somebody else use this language?\n\n");

    const std::vector<Case> cases{
        // ── provenance ──────────────────────────────────────────────────────
        {"forgot the 'source' line entirely",
         "casebook t v1\n"                             // 1
         "observe a \"a\"\n"                           // 2
         "action i idle\n"                             // 3
         "action s safe when a\n",                     // 4
         0, "provenance", nullptr},

        // ── names ───────────────────────────────────────────────────────────
        {"typo in an observation name",
         "casebook t v1\n"                             // 1
         "source \"d\" dated 2026-01-01 by \"x\"\n"    // 2
         "observe vibration \"v\"\n"                   // 3
         "claim c \"c\" needs vibraton\n"              // 4  <- typo
         "action i idle\n"                             // 5
         "action s safe when c\n"                      // 6
         "prefer s over i\n",                          // 7
         4, "vibraton", "vibraton"},

        {"declared the same name twice",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"first\"\n"                       // 3
         "observe a \"second\"\n"                      // 4  <- duplicate
         "action i idle\n"
         "action s safe when a\n"
         "prefer s over i\n",
         4, "already declared", "a"},

        {"used a reserved name",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe closed_world \"oops\"\n"             // 3
         "action i idle\n"
         "action s safe when closed_world\n",
         3, "reserved", "closed_world"},

        // ── the three ASPIC+ attack forms, used wrongly ─────────────────────
        {"tried to undermine a claim instead of a reading",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "claim c \"c\" needs a\n"                     // 4
         "a undermines c\n"                            // 5  <- undermining targets premises
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "not an observation", "c"},

        {"tried to rebut a reading instead of a claim",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "claim c \"c\" needs a\n"
         "c rebuts a\n"                                // 5  <- rebutting targets conclusions
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "not a claim, excuse or doubt", "a"},

        {"used an action as evidence for a claim",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action i idle\n"
         "action s safe when a\n"
         "claim c \"c\" needs s\n"                      // 6  <- an action is a conclusion
         "prefer s over i\n",
         6, "cannot serve as evidence", "s"},

        {"a doubt that gates on a claim rather than a raw reading",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "claim c \"c\" needs a\n"
         "doubt d \"d\" needs c\n"                      // 5
         "  undermines a\n"                             // 6
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "gate on raw readings", "c"},

        // ── the ambiguity trap that caused the Piper Alpha bug ──────────────
        {"a bare 'when' with two conditions, meaning 'and'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "observe b \"b\"\n"
         "claim c \"c\" when a b\n"                     // 5  <- ambiguous
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "ambiguous", "c"},

        // ── actions ─────────────────────────────────────────────────────────
        {"forgot to mark any action 'idle'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action s safe when a\n",                      // 4
         4, "idle", "s"},

        {"forgot to mark any action 'safe'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action i idle\n",                             // 4
         4, "safe", "i"},

        {"'prefer' naming something that is not an action",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "claim c \"c\" needs a\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over c\n",                           // 7  <- c is a claim
         7, "not an action", "c"},

        {"'prefer' missing the word 'over'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action i idle\n"
         "action s safe when a\n"
         "prefer s i\n",                                // 6
         6, "over", "i"},

        // ── channels: the place a wrong number is most dangerous ────────────
        {"threshold with no hysteresis",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  range 0 200\n"                                 // 5
         "  stale_after 5s\n"                               // 6
         "  asserts hot above 90.0\n"                       // 7  <- no release
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "release", "hot"},

        {"hysteresis running the wrong way",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  range 0 200\n"
         "  stale_after 5s\n"
         "  asserts hot above 90.0 release 95.0\n"          // 7  <- releases above assert
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "could never clear", "hot"},

        {"channel with no freshness requirement",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  range 0 200\n"
         "  asserts hot above 90.0 release 85.0\n"
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "stale_after", "temp"},

        {"channel with no valid range",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  stale_after 5s\n"
         "  asserts hot above 90.0 release 85.0\n"
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "range", "temp"},

        {"setpoint outside the channel's valid range",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  range 0 100\n"
         "  stale_after 5s\n"
         "  asserts hot above 900.0 release 850.0\n"        // 7  <- 900 C is outside 0..100
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "never be asserted", "hot"},

        {"wrote the unit into the number",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"bearing temperature\" unit C\n"   // 4
         "  range 0 200\n"
         "  stale_after 5s\n"
         "  asserts hot above 90.0C release 85.0\n"         // 7
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "unit", "90.0C"},

        // ── syntax ──────────────────────────────────────────────────────────
        {"misspelled a declaration keyword",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observ a \"a\"\n"                              // 3
         "action i idle\n"
         "action s safe\n",
         3, "unrecognised declaration", "observ"},

        {"unclosed quote",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"never closed\n"                    // 3
         "action i idle\n"
         "action s safe when a\n",
         3, "unterminated quoted string", nullptr},

        {"marked two actions 'idle'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action i idle\n"                              // 4
         "action j idle\n"                              // 5  <- second idle
         "action s safe when a\n"
         "prefer s over i over j\n",
         5, "already is", "j"},

        {"marked two actions 'safe'",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe a \"a\"\n"
         "action i idle\n"
         "action s safe when a\n"                       // 5
         "action t safe when a\n"                       // 6  <- second safe
         "prefer s over t over i\n",
         6, "already is", "t"},

        {"unknown keyword inside a channel",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "channel temp \"t\" unit C\n"                   // 4
         "  range 0 200\n"
         "  stale_after 5s\n"
         "  wibble 3\n"                                   // 7
         "  asserts hot above 90.0 release 85.0\n"
         "action i idle\n"
         "action s safe when hot\n"
         "prefer s over i\n",
         4, "wibble", "wibble"},

        {"timed reading with no when clause",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\"\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "'when'", "late"},

        {"timed reading in a shape cbc cannot read",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\" when hot after start\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "within 10s after", "late"},

        {"timed reading counting once",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading again \"again\" when hot 1 times in 1h\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "2 to 16", "again"},

        {"timed reading with a bad duration",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\" when hot within soon after start\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "duration", "soon"},

        {"timed reading watching an unknown reading",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\" when hot within 5s after strat\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "undefined", "strat"},

        {"timed reading watching itself",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\" when hot within 5s after hot\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "two different", "late"},

        {"timed reading watching a conclusion",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading late \"late\" when c within last 5s\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         5, "conclusion", "c"},

        {"timed reading watching a timed reading",
         "casebook t v1\n"
         "source \"d\" dated 2026-01-01 by \"x\"\n"
         "observe hot \"hot\"\n"
         "observe start \"start\"\n"
         "reading r1 \"r1\" when hot within last 5s\nreading r2 \"r2\" when r1 within last 5s\n"
         "claim c \"c\" needs hot\n"
         "action i idle\n"
         "action s safe when c\n"
         "prefer s over i\n",
         6, "itself a timed reading", "r1"},
    };

    for (const Case& c : cases) { run(c); }

    std::printf("\n%d mistakes exercised, %d checks, %d failures\n", g_cases, g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("\n  by category:\n"
                    "    wrong or missing line number  %d\n"
                    "    message omits the symbol      %d\n"
                    "    message offers no guidance    %d\n",
                    g_line_failures, g_symbol_failures, g_guidance_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
