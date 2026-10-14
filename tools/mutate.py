#!/usr/bin/env python3
"""Mutation testing: break the code on purpose and see whether the test suite notices.

WHY THIS EXISTS

Every other instrument in this project asks "is the software right?". This one asks a question none of them can:
**are the tests any good?** A suite of 43 passing tests says nothing about whether it would catch a defect, and
until this was run nobody had checked.

A SURVIVOR is a mutation the entire suite accepts. That is behaviour nobody verifies, and it is the only direct
measurement of a blind spot in the tests themselves.

WHAT IT FOUND FIRST RUN

    KILLED     binding: stop asserting the unavailable observation
    KILLED     binding: believe thresholds from a dead channel
    KILLED     binding: never report a flatlined channel
    SURVIVED   lower: raise the 30-observation cap past the mask width
    SURVIVED   lower: raise the vote expansion cap

The two survivors were the compiler's REFUSAL LIMITS -- the guards that stop silent corruption. Either could have
been loosened or deleted with all 43 tests still green. That matters because past 31 observations the failure is
silent: binding::bit() returns 0 for any index >= 32, so those observations would never assert and the device
would answer confidently from an incomplete picture. Both are now covered by casebook_limits_refused.

WHY IT IS NOT A CI GATE

Each mutation needs a rebuild and a full ctest run, and one survivor took 78 minutes to kill because the mutated
compiler dutifully tried to validate 2^31 states. Run it deliberately, not on every commit.

RESTORE GUARANTEE

Every mutation is reverted in a `finally`, and the script verifies byte-for-byte afterwards. Learned the hard way:
running these by hand, a mutation was left in lower.cpp and only spotted by grep several commands later. A tool
that edits source must guarantee restoration the way tools/figures-tamper.sh does with `trap restore EXIT`.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Each entry states a behaviour the code relies on, expressed as the smallest edit that breaks it.
# Prefer mutations that are SEMANTICALLY wrong but syntactically fine -- a mutation that fails to compile
# measures nothing.
MUTATIONS = [
    ("binding/include/quorum/binding/binding.hpp",
     "m |= bit(spec.unavailable_obs);", "",
     "binding: stop asserting the unavailable observation"),
    ("binding/include/quorum/binding/binding.hpp",
     "if (health(spec.channel, now_ms) != Health::Good) { continue; }", "if (false) { continue; }",
     "binding: believe thresholds from a dead channel"),
    ("binding/include/quorum/binding/binding.hpp",
     "if (flatlined(c, now_ms)) { m |= bit(spec.flatline_obs); }", "",
     "binding: never report a flatlined channel"),
    ("casebook/compiler/src/lower.cpp",
     "if (lo.observation_count > 30)", "if (lo.observation_count > 300)",
     "lower: raise the 30-observation cap past the mask width"),
    ("casebook/compiler/src/lower.cpp",
     "if (combinations > 64)", "if (combinations > 640)",
     "lower: raise the vote expansion cap"),
    ("casebook/compiler/src/lower.cpp",
     "if (ch.stale_after_ms == 0)", "if (false)",
     "lower: accept a channel with no staleness window"),
# ── the engine. Most safety-critical code in the project, and the first run left it essentially unmutated
    # because the one attempt did not compile. A mutation that does not compile measures nothing.
    ("solver/include/quorum/solver/grounded.hpp",
     "if (label_[ix(b)] != Label::Undecided) { continue; }",
     "if (label_[ix(b)] == Label::Undecided) { continue; }",
     "grounded: only attack arguments already decided (inverted guard)"),
    ("solver/include/quorum/solver/grounded.hpp",
     "label_[ix(b)] = Label::Out;", "label_[ix(b)] = Label::Undecided;",
     "grounded: an attacked argument stays undecided instead of out"),
    ("solver/include/quorum/solver/grounded.hpp",
     "if (undecided_[ix(c)] > 0) { --undecided_[ix(c)]; }", "",
     "grounded: never discharge an attacker that went out"),
    ("solver/include/quorum/solver/grounded.hpp",
     "if (label_[ix(c)] == Label::Undecided && undecided_[ix(c)] == 0) {",
     "if (label_[ix(c)] == Label::Undecided && undecided_[ix(c)] <= 1) {",
     "grounded: accept an argument with one attacker still undischarged"),
    ("solver/include/quorum/solver/grounded.hpp",
     "if (undecided_[ix(i)] == 0) {", "if (undecided_[ix(i)] <= 1) {",
     "grounded: seed the queue with singly-attacked arguments"),
    ("solver/include/quorum/solver/grounded.hpp",
     "return label_[ix(a.get())] == Label::In;", "return label_[ix(a.get())] != Label::Out;",
     "grounded: treat Undecided as acceptance -- the closed-world bug this project already had once"),

    # ── the arbiter: which action a verdict selects.
    ("arbiter/include/quorum/arbiter/verdict.hpp",
     "Outcome::Conflicted", "Outcome::Decided",
     "arbiter: report a conflicted verdict as decided"),

    # ── the newest analysis checks. They have tests; do the tests PIN them?
    # ── the ANALYSIS REPORTS added over the last two days. These are the product: the findings sold to a
    # customer. They all have tests -- the question mutation testing answers is whether those tests PIN the
    # behaviour or merely exercise it.
    ("casebook/compiler/src/main.cpp",
     "if (routes.size() == 1) {", "if (false) {",
     "analysis: stop reporting a sole route to an action"),
    ("casebook/compiler/src/main.cpp",
     "if (shared.empty()) { continue; }", "if (!shared.empty()) { continue; }",
     "analysis: invert the common-cause test (report only when independent)"),
    ("casebook/fmea/hazop2cb.cpp",
     "if (kv.second < threshold || kv.second < 2) { continue; }",
     "if (kv.second < threshold || kv.second < 200) { continue; }",
     "hazop2cb: stop reporting a shared protection layer"),
    ("casebook/fmea/hazop2cb.cpp",
     "if (g.count(Guard::Instrument) != 0) { continue; }", "",
     "hazop2cb: report every row as NOT AN IPL, including ones a real tagged instrument protects"),
    ("casebook/fmea/hazop2cb.cpp",
     "if (devs.size() < 2) { continue; }", "if (devs.size() < 200) { continue; }",
     "hazop2cb: stop reporting a cause spanning several deviations"),
    ("casebook/fmea/hazop2cb.cpp",
     'if (!tags(text).empty()) { out.insert(Guard::Instrument); }', "",
     "hazop2cb: never classify a safeguard as a tagged instrument"),
    # The detectors added on 2026-09-27/28, when thirteen untested assumptions were checked. Every one of these
    # has a passing test; none had been shown to FAIL if the detector broke. Given PROC-D11 -- three mutations that
    # looked killed only because a piped exit status hid a failed build -- a passing test is not evidence.
    ("casebook/fmea/hazop2cb.cpp",
     "if (c == '\\n' || c == ';') { flush(); }", "if (c == ';') { flush(); }",
     "hazop2cb: stop splitting packed causes on newlines (DOC-A7)"),
    ("casebook/fmea/hazop2cb.cpp",
     "if (!quoted) { return true; }", "return true;",
     "hazop2cb: cut a multi-line quoted record at the first newline, shifting every later column (DOC-A7)"),
    ("casebook/fmea/hazop2cb.cpp",
     "safeguard_is_none(r.safeguards) && safeguard_is_none(r.mitigative)", "safeguard_is_none(r.safeguards)",
     "hazop2cb: ignore the mitigative column, reporting a protected row as UNPROTECTED (DOC-A12)"),
    ("casebook/fmea/hazop2cb.cpp",
     "bool integrity_unproven(const std::string& text) {",
     "bool integrity_unproven(const std::string& text) { if (!text.empty()) { return false; }",
     "hazop2cb: count a never-proof-tested safeguard as a full protection layer (DOC-A13)"),
    ("casebook/fmea/hazop2cb.cpp",
     "for (const char* w : header_words) {", "for (const char* w : header_words) { looks_like_header = true;",
     "hazop2cb: take the first row as the header, refusing any file with an Excel banner (DOC-A8)"),
    ("casebook/fmea/hazop2cb.cpp",
     "int lineno = skipped + 1;", "int lineno = 1;",
     "hazop2cb: report line numbers that do not count the banner, so findings cannot be located (DOC-A8)"),
    ("casebook/fmea/hazop2cb.cpp",
     "r.deviation = guide.empty() ? param : (param.empty() ? guide : guide + \" \" + param);",
     "r.deviation = param;",
     "hazop2cb: build the deviation from the parameter alone, collapsing high and low into one observation"),
    ("casebook/fmea/hazop2cb.cpp",
     "const bool synth_dev = (c_dev < 0) && (c_param >= 0) && (c_guide >= 0);",
     "const bool synth_dev = (c_dev < 0) && (c_param >= 0);",
     "hazop2cb: synthesise a deviation from a parameter with no guide word instead of refusing"),
    # lopa2cb. PROC-D12 was falsified the first time it was checked -- eight detectors with passing tests, one of
    # which verified nothing -- so every detector here is mutated in the same change that adds it.
    ("casebook/fmea/lopa2cb.cpp",
     "if (ratio > 2.0 || ratio < 0.5) {", "if (false) {",
     "lopa2cb: stop checking a row's product against its own stated answer"),
    ("casebook/fmea/lopa2cb.cpp",
     "if (computed_total > target) {", "if (false) {",
     "lopa2cb: never report that a study misses the target it set itself"),
    ("casebook/fmea/lopa2cb.cpp",
     'if (t.find("sil") != std::string::npos || t.find("sif") != std::string::npos) { return nullptr; }', "",
     "lopa2cb: report a SIL-rated layer as over-credited -- a FALSE POSITIVE on a legitimate claim"),
    ("casebook/fmea/lopa2cb.cpp",
     'if (t.find("probability") == std::string::npos && t.find("likelihood") == std::string::npos) { return false; }',
     "if (true) { return false; }",
     "lopa2cb: stop noticing a conditional modifier counted as a protection layer"),
    ("casebook/fmea/lopa2cb.cpp",
     "if (!is_applicable(c.pfd[i])) { continue; }", "",
     "lopa2cb: treat a not-applicable layer (PFD 1) as though it protected that cause"),
    ("casebook/fmea/lopa2cb.cpp",
     '{"a person",           {"operator", "personnel", "manual", "human error", "by hand", "staff", "technician",',
     '{"a person",           {"nomatch_operator", "personnel", "manual", "human error", "by hand", "staff", "technician",',
     "lopa2cb: stop matching operator error against a human protection layer"),
    ("casebook/fmea/lopa2cb.cpp",
     'if (pfd >= 1e-2) { return "SIL 1"; }', 'if (pfd >= 1e-2) { return "SIL 2"; }',
     "lopa2cb: report the wrong SIL band for a required PFD"),
    # The report layer. PROC-D14 and D15 were both defects in what the report SAYS rather than in the analysis,
    # so the report's own claims are mutated too.
    ("casebook/fmea/findings.hpp",
     "for (Check& c : checks_) { if (c.kind == f.kind) { ++c.fired; } }", "",
     "findings: stop counting whether a check fired, so silent and firing checks look alike"),
    ("casebook/fmea/findings.hpp",
     'std::fprintf(f, "\\n## Checks that found nothing\\n\\n");', "",
     "findings: drop the section that makes silence evidence"),
    ("casebook/fmea/findings.hpp",
     "for (const std::string& l : limits_) { std::fprintf(f, \"- %s\\n\", l.c_str()); }", "",
     "findings: state no limits, misleading by omission"),
    ("casebook/fmea/findings.hpp",
     "Weight::Conclusion, Weight::Arithmetic", "Weight::Arithmetic, Weight::Conclusion",
     "findings: file a study's own conclusion under 'the document contradicts itself'"),
    ("casebook/fmea/hazop2cb.cpp",
     'report.confirmed("safeguards recorded", buf);', "",
     "hazop2cb: report only problems, with no evidence the document was understood"),
    ("casebook/fmea/fmea2cb.cpp",
     "det_is_score = (total > 0 && numeric == total);", "det_is_score = false;",
     "fmea2cb: accept a numeric Detection column as instrument names"),
    # The risk audit. Arithmetic and a published lookup table, so these are the cheapest detectors in the project
    # to verify and the ones a customer is most likely to check by hand.
    ("casebook/fmea/fmea2cb.cpp",
     "if (!trim(r.rpn).empty() && stated != product) {", "if (false) {",
     "fmea2cb: stop checking a stated RPN against S x O x D"),
    ("casebook/fmea/fmea2cb.cpp",
     "if (!(lo.ap == Ap::High && hi.ap != Ap::High)) { continue; }", "continue;",
     "fmea2cb: stop reporting the RPN rank inversion the 2019 standard was changed to fix"),
    ("casebook/fmea/fmea2cb.cpp",
     "if (r.ap != Ap::High || r.has_action) { continue; }", "if (r.has_action) { continue; }",
     "fmea2cb: demand an action on every row, not only Action Priority HIGH ones -- a false positive"),
    ("casebook/fmea/fmea2cb.cpp",
     "{Ap::Low, Ap::Low, Ap::Medium, Ap::High},\n         {Ap::Medium, Ap::High, Ap::High, Ap::High},",
     "{Ap::Low, Ap::Low, Ap::Medium, Ap::Medium},\n         {Ap::Medium, Ap::High, Ap::High, Ap::High},",
     "fmea2cb: weaken the Action Priority table for a severity 9-10 failure"),
    # Mutating the increment itself leaves the out-parameter unused and -Werror rejects it, so the mutation is
    # applied where the count is CONSUMED. A mutation that will not compile measures nothing.
    ("casebook/fmea/lopa2cb.cpp",
     "int lineno = phys;", "int lineno = 0;",
     "lopa2cb: number rows from the top of the table, so comment lines above it are invisible"),
]


def sh(cmd, timeout=None):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=ROOT, timeout=timeout)


def digest(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--filter", default="", help="only run mutations whose description matches this substring")
    ap.add_argument("--test-timeout", type=int, default=1800,
                    help="seconds allowed for one ctest run; a timeout counts as KILLED and says so")
    args = ap.parse_args()

    selected = [m for m in MUTATIONS if args.filter.lower() in m[3].lower()]
    if not selected:
        print(f"no mutation matches '{args.filter}'")
        return 2

    print(f"mutation testing: {len(selected)} mutation(s). Each needs a rebuild and a full ctest run.\n")
    results = []
    for path, find, repl, desc in selected:
        full = os.path.join(ROOT, path)
        before = digest(full)
        with open(full) as fh:
            original = fh.read()

        if find not in original:
            results.append(("STALE", desc, "pattern not found -- the code moved and this mutation needs rewriting"))
            continue

        try:
            with open(full, "w") as fh:
                fh.write(original.replace(find, repl, 1))

            # Check the BUILD'S EXIT STATUS, never its output.
            #
            # This line was `sh("cmake --build build -j 2>&1 | tail -5")` with a grep for "error", and it produced
            # THREE FALSE SURVIVORS on its first serious run. A shell pipeline returns the exit status of the LAST
            # command, so `| tail -5` reported success no matter how badly the build failed; ctest then ran the
            # STALE binary and passed. The grep was case-sensitive too, so it missed "Error 1" from make.
            #
            # A false SURVIVED is the worst output this tool can produce: it invents a blind spot that is not there
            # and hides the real state of the suite. Exactly the silent-plausible-output failure this project
            # exists to catch, in the instrument built to catch it.
            build = sh("cmake --build build -j")
            if build.returncode != 0:
                tail = (build.stdout + build.stderr).strip().splitlines()[-1:]
                results.append(("NO-COMPILE", desc,
                                f"build failed ({tail[0][:60] if tail else 'no output'}) -- measures nothing"))
                continue

            try:
                test = sh("ctest --test-dir build 2>&1 | tail -4", timeout=args.test_timeout)
            except subprocess.TimeoutExpired:
                results.append(("KILLED", desc, f"suite exceeded {args.test_timeout}s -- a hang is a detection"))
                continue

            if "0 tests failed" in test.stdout:
                results.append(("SURVIVED", desc, "the entire suite passed -- nothing verifies this"))
            else:
                m = re.search(r"(\d+) tests failed", test.stdout)
                results.append(("KILLED", desc, f"{m.group(1)} test(s) failed" if m else "suite failed"))
        finally:
            with open(full, "w") as fh:
                fh.write(original)
            if digest(full) != before:
                print(f"FATAL: could not restore {path}. Fix this before doing anything else.")
                return 3

    sh("cmake --build build -j >/dev/null 2>&1")

    print()
    for status, desc, note in results:
        print(f"  {status:11s} {desc:52s} {note}")

    killed = sum(1 for s, _, _ in results if s == "KILLED")
    survived = [d for s, d, _ in results if s == "SURVIVED"]
    print(f"\n  killed {killed}, survived {len(survived)}, unusable {len(results) - killed - len(survived)}")
    if survived:
        print("\n  SURVIVORS are the finding. Each is behaviour the suite does not check:")
        for d in survived:
            print(f"    - {d}")
        print("\n  Write a test that fails under the mutation, then re-run this to confirm it is killed.")
        print("  Adding a test that merely passes proves nothing.")
    else:
        print("\n  No survivors in this set. That is not a claim about the suite in general -- it is a claim")
        print("  about these mutations. Add more, especially ones you expect to survive.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
