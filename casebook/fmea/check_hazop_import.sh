#!/bin/bash
# hazop2cb must import a HAZOP, name the CAPECO row, and produce a draft cbc REFUSES.
#
# Three properties, and the third is the one that matters most. A HAZOP records deviations, causes and safeguards;
# it does NOT record defeaters, severities or a runtime response. So a faithful import is necessarily incomplete,
# and if cbc ever accepted one the safety net would be gone -- exactly the argument behind the FMEA draft test.
set -u
HAZOP2CB="$1"; CBC="$2"; CSV="$3"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

"$HAZOP2CB" "$CSV" v101 > "$D/draft.cb" 2> "$D/analysis.txt" || { echo "FAIL: import failed"; exit 1; }

# 1. Deviations must be deduplicated into observations. Three causes share 'High level' in the fixture, and a
#    deviation reached by several causes is the differential-diagnosis shape the whole engine exists for.
if ! grep -q 'observe high_level .*# 3 cause' "$D/draft.cb"; then
    echo "FAIL: 'High level' should be one observation carrying 3 causes"
    grep '^observe' "$D/draft.cb" | sed 's/^/       /'
    exit 1
fi

# 2. The safeguard analysis must find the CAPECO row: the only protection against 'LT-101 failed low' is LT-101.
if ! grep -q "UNPROTECTED" "$D/analysis.txt"; then
    echo "FAIL: the unprotected deviation was not reported"; sed 's/^/       /' "$D/analysis.txt"; exit 1
fi
if ! grep -q "COMMON CAUSE" "$D/analysis.txt"; then
    echo "FAIL: the safeguard naming the same instrument as the cause was not reported"
    sed 's/^/       /' "$D/analysis.txt"; exit 1
fi
if ! grep -qi "shared tag: *lt-101" "$D/analysis.txt"; then
    echo "FAIL: the shared instrument tag was not named"; sed 's/^/       /' "$D/analysis.txt"; exit 1
fi

# 3. The draft must be REFUSED. A HAZOP cannot supply a runtime response, and guessing one would be the
#    silent-plausible-output failure the mobile X-ray FMEA demonstrated.
OUT=$("$CBC" "$D/draft.cb" 2>&1); STATUS=$?
if [ "$STATUS" -eq 0 ]; then
    echo "FAIL: cbc ACCEPTED a HAZOP-derived draft. A HAZOP records no defeaters, no severities and no"
    echo "      runtime actions, so accepting one means those were guessed."
    exit 1
fi

# 4. And it must stay SILENT on a HAZOP whose safeguards are genuinely independent.
#
# A detector that always fires is as useless as one that never does. In the independent fixture the inlet valve is
# XV-201, the loop is LIC-201, and the protection is LSH-202 on separate I/O plus a direct-acting PSV-203 -- no tag
# is shared between any cause and its safeguard, which is what independence looks like written down.
CLEAN="$(dirname "$0")/hazop_independent.csv"
"$HAZOP2CB" "$CLEAN" clean >/dev/null 2> "$D/clean.txt" || { echo "FAIL: import of the clean fixture failed"; exit 1; }
if grep -qE "UNPROTECTED|COMMON CAUSE" "$D/clean.txt"; then
    echo "FAIL: reported a problem in a HAZOP whose safeguards ARE independent."
    echo "      Crying wolf here would make every real finding worthless."
    sed 's/^/       /' "$D/clean.txt"
    exit 1
fi
if ! grep -q "0 unprotected, 0 sharing the cause's instrument" "$D/clean.txt"; then
    echo "FAIL: expected an explicit 0/0 on the independent fixture"; sed 's/^/       /' "$D/clean.txt"; exit 1
fi

# 5. The real published HAZOP must produce the findings the tag matcher could not see.
#
# Hospital oxygen plant, PLOS ONE 2023, CC BY. 13 of 14 safeguards are "visual inspection" and 10 of 14 are
# "the alarm of the device"; no row contains an instrument tag. The first version of this tool reported the
# document CLEAN -- a false negative on a systemic independence failure, which is the defect class this whole
# project exists to catch. Both new reports are asserted because each catches something the other cannot: the
# SHARED LAYER is a cross-row property invisible per row, and NOT AN IPL is a per-row property invisible in
# aggregate.
PSA="$(dirname "$0")/hazop_oxygen_psa.csv"
"$HAZOP2CB" "$PSA" psa >/dev/null 2> "$D/psa.txt" || { echo "FAIL: import of the oxygen HAZOP failed"; exit 1; }
if ! grep -q "SHARED LAYER .*visual inspection.* in 13 of 14 rows" "$D/psa.txt"; then
    echo "FAIL: 'visual inspection' carrying 13 of 14 rows was not reported as a shared layer."
    echo "      Each row looks fine alone; together they are one layer protecting the whole plant."
    sed 's/^/       /' "$D/psa.txt"; exit 1
fi
if ! grep -q "NOT AN IPL" "$D/psa.txt"; then
    echo "FAIL: rows protected only by people and paper were not reported"; sed 's/^/       /' "$D/psa.txt"; exit 1
fi
if ! grep -q "own alarm is NOT independent of a failure inside that device" "$D/psa.txt"; then
    echo "FAIL: the on-device alarm was not called out as non-independent of its own device's failure"
    sed 's/^/       /' "$D/psa.txt"; exit 1
fi
# 14 of 14, not 13. The fourteenth is "Check the purity of the output oxygen", which no keyword list recognises.
# It used to be waved through by a `Guard::Other` fallback that gave unreadable prose the same standing as a
# tagged instrument -- found by mutation testing, which showed the Instrument branch could be deleted with the
# whole suite green. The tool now says it cannot read that text instead of assuming the best.
if ! grep -q "14 not an IPL" "$D/psa.txt"; then
    echo "FAIL: expected all 14 rows reported as not an IPL -- the oxygen plant has no instrumented protection"
    sed 's/^/       /' "$D/psa.txt"; exit 1
fi

# 8. Deferral language must not be read as protection.
#
# "See note 4" is a cross-reference this tool cannot follow; "TBD pending review" records that nobody has decided.
# Both used to be counted as full protection, which is the most dangerous possible reading -- they LOOK like
# entries. A genuine tagged trip in the same file must still pass.
DEF="$(dirname "$0")/hazop_deferred.csv"
"$HAZOP2CB" "$DEF" def >/dev/null 2> "$D/def.txt" || { echo "FAIL: import of the deferral fixture failed"; exit 1; }
if ! grep -q "2 not an IPL" "$D/def.txt"; then
    echo "FAIL: the two deferral rows must be reported, and the TSH-301 row must not"
    sed 's/^/       /' "$D/def.txt"; exit 1
fi
if ! grep -q "this is a DEFERRAL" "$D/def.txt"; then
    echo "FAIL: a deferral must be named as such, not merely counted"; sed 's/^/       /' "$D/def.txt"; exit 1
fi
# 6. One failure written up under several deviations must be reported, not silently split.
#
# The inverse of the protection-layer question and the more diagnostically useful half. If over-pressure and
# over-voltage become separate claims, observing both looks like two independent faults -- improbable, easy to
# dismiss -- when the document says it is one solenoid valve.
if ! grep -q "SHARED CAUSE" "$D/psa.txt"; then
    echo "FAIL: one cause appearing under two deviations was not reported"; sed 's/^/       /' "$D/psa.txt"; exit 1
fi
if ! grep -q "1 cause(s) producing several deviations" "$D/psa.txt"; then
    echo "FAIL: expected exactly one shared cause in the oxygen HAZOP"; sed 's/^/       /' "$D/psa.txt"; exit 1
fi
if grep -q "SHARED CAUSE" "$D/clean.txt"; then
    echo "FAIL: reported a shared cause in a worksheet that has none"; sed 's/^/       /' "$D/clean.txt"; exit 1
fi

# 7. A two-row extract sharing one safeguard must still be reported.
#
# A floor of 3 rows used to silence the shared-layer check at 100% sharing. A single-node extract is exactly what
# a customer sends as a first sample, so the floor hid the finding in the most likely first document.
TWO="$(dirname "$0")/hazop_two_row.csv"
"$HAZOP2CB" "$TWO" two >/dev/null 2> "$D/two.txt" || { echo "FAIL: import of the two-row extract failed"; exit 1; }
if ! grep -q "SHARED LAYER .*2 of 2 rows (100%)" "$D/two.txt"; then
    echo "FAIL: 100% sharing on a two-row extract must be reported"; sed 's/^/       /' "$D/two.txt"; exit 1
fi
if ! grep -q "small extract" "$D/two.txt"; then
    echo "FAIL: a small extract must be labelled as weaker evidence"; sed 's/^/       /' "$D/two.txt"; exit 1
fi

# 9. A German PAAG worksheet must be analysed, not merely parsed.
#
# PAAG is the German-speaking world's name for a HAZOP, with entirely different column words: Abweichung,
# Ursache, Auswirkung, Gegenmassnahmen. Europe is the stated market, so this is a product requirement.
#
# Asserting the SEVERITY is right, not just that something was reported: "keine" must be UNPROTECTED, because
# reading it as unclassifiable prose under-reports a deviation with nothing in the way.
PAAG="$(dirname "$0")/paag_german.csv"
"$HAZOP2CB" "$PAAG" paag >/dev/null 2> "$D/paag.txt" || { echo "FAIL: import of the PAAG worksheet failed"; exit 1; }
if grep -q "NO SAFEGUARDS COLUMN" "$D/paag.txt"; then
    echo "FAIL: 'Gegenmassnahmen' is the German safeguards column and must be recognised"
    sed 's/^/       /' "$D/paag.txt"; exit 1
fi
if ! grep -q "UNPROTECTED" "$D/paag.txt"; then
    echo "FAIL: 'keine' means nothing protects this deviation and must be UNPROTECTED, not unclassifiable prose"
    sed 's/^/       /' "$D/paag.txt"; exit 1
fi
if ! grep -q "failing device.s own alarm" "$D/paag.txt"; then
    echo "FAIL: 'geraeteeigener Alarm' is the device's own alarm and must be recognised as non-independent"
    sed 's/^/       /' "$D/paag.txt"; exit 1
fi
if ! grep -q "2 not an IPL" "$D/paag.txt"; then
    echo "FAIL: expected the two Sichtpruefung rows reported as not an IPL"
    sed 's/^/       /' "$D/paag.txt"; exit 1
fi

# 10. The shapes a REAL worksheet arrives in, and the information a real worksheet already contains.
#
# Every case below was an UNTESTED row in docs/assumptions.tsv until 2026-09-27. Each is checked here because each
# was found to be wrong, and several failed in the dangerous direction rather than the safe one.
D10="$(dirname "$0")"

# DOC-A8: an Excel export has a banner -- title, facility, date, blank row -- ABOVE the header. Taking row 1 as the
# header refused the file and quoted the banner back as though it were column names. Line numbers must also match
# the file, or a finding cannot be located.
"$HAZOP2CB" "$D10/hazop_excel_banner.csv" b >/dev/null 2> "$D/banner.txt" || { echo "FAIL: banner import failed"; exit 1; }
if ! grep -q "skipped 3 row(s) above the header" "$D/banner.txt"; then
    echo "FAIL: the three banner rows above the header must be skipped and reported"
    sed 's/^/       /' "$D/banner.txt"; exit 1
fi
if ! grep -q "line 5: 'High level'" "$D/banner.txt"; then
    echo "FAIL: the reported line number must match the file (the row is physically at line 5)"
    sed 's/^/       /' "$D/banner.txt"; exit 1
fi

# DOC-A12: two safeguard columns. Preventive "None" with a real mitigative layer was a FALSE UNPROTECTED -- a false
# finding costs more credibility than a missed one.
"$HAZOP2CB" "$D10/hazop_mitigative.csv" m >/dev/null 2> "$D/mit.txt" || { echo "FAIL: mitigative import failed"; exit 1; }
if ! grep -q "MITIGATIVE ONLY" "$D/mit.txt"; then
    echo "FAIL: a deviation with only a mitigative safeguard must be named as such"
    sed 's/^/       /' "$D/mit.txt"; exit 1
fi
# Assert on the row actually AT RISK, which is line 3: Preventive "None" with Mitigative "PSV-203 relief to flare".
# The first version of this assertion checked line 2 -- a row with a preventive safeguard that was never in danger
# of being misreported -- so it tested nothing, and mutation testing caught it by deleting the mitigative check and
# watching the whole suite pass anyway. Neither row may be reported unprotected; this file has no unprotected row.
if grep -qE "^ +UNPROTECTED +line" "$D/mit.txt"; then
    echo "FAIL: every row here has a safeguard in one column or the other; none may be reported unprotected"
    sed 's/^/       /' "$D/mit.txt"; exit 1
fi
if ! grep -q "0 unprotected" "$D/mit.txt"; then
    echo "FAIL: the summary must count zero unprotected deviations"
    sed 's/^/       /' "$D/mit.txt"; exit 1
fi

# DOC-A13: a tagged instrument nobody has proof tested is an assumption with a part number.
"$HAZOP2CB" "$D10/hazop_unproven.csv" u >/dev/null 2> "$D/unp.txt" || { echo "FAIL: unproven import failed"; exit 1; }
if ! grep -q "2 unproven layer" "$D/unp.txt"; then
    echo "FAIL: 'never proof tested' and 'assumed tested - no record' must both be reported"
    sed 's/^/       /' "$D/unp.txt"; exit 1
fi

# DOC-A7: a bulleted list inside a quoted cell contains NEWLINES, which used to shift every column after it.
"$HAZOP2CB" "$D10/hazop_packed_causes.csv" p >/dev/null 2> "$D/pack.txt" || { echo "FAIL: packed-cause import failed"; exit 1; }
if ! grep -q "2 cell(s) packing several causes" "$D/pack.txt"; then
    echo "FAIL: both multi-cause cells must be reported"; sed 's/^/       /' "$D/pack.txt"; exit 1
fi
# Match the REPORT LINE, not the prose. The advice paragraph contains the sentence "An UNPROTECTED deviation has
# nothing in the way", so a bare grep for the word fails against correct output -- which it did, the first time
# this assertion was written. An over-broad grep in a test is a false alarm generator.
if grep -qE "^ +UNPROTECTED +line" "$D/pack.txt"; then
    echo "FAIL: the multi-line cell shifted the columns again -- LAH-102 is a real safeguard in that file"
    sed 's/^/       /' "$D/pack.txt"; exit 1
fi

# DOC-A5: a worksheet that already graded severity in words had that work thrown away.
"$HAZOP2CB" "$D10/hazop_word_severity.csv" w > "$D/sev.cb" 2>/dev/null || { echo "FAIL: severity import failed"; exit 1; }
if ! grep -q "grades this 'Catastrophic'. On a 1-10 scale that suggests 10" "$D/sev.cb"; then
    echo "FAIL: a word-graded severity must be carried across as a suggestion"
    grep -i severity "$D/sev.cb" | sed 's/^/       /'; exit 1
fi
if grep -qE "^  severity [0-9]" "$D/sev.cb"; then
    echo "FAIL: severity must NEVER be written into the casebook -- it decides which claims may rebut which"
    exit 1
fi

# 11. THE STANDARD HAZOP WORKSHEET, which has no Deviation column at all.
#
# A deviation IS a guide word applied to a parameter, so the textbook recording format (IEC 61882 and the templates
# that follow it) has PARAMETER and GUIDE WORD as separate columns. This tool used to refuse that layout outright --
# it required a pre-combined Deviation column, which is a simplification, not the standard.
"$HAZOP2CB" "$D10/hazop_guideword.csv" gw > "$D/gw.cb" 2> "$D/gw.txt" || { echo "FAIL: guide-word import failed"; exit 1; }
if ! grep -q "combining Guide Word with Parameter" "$D/gw.txt"; then
    echo "FAIL: the synthesised deviation must be announced, not done silently"
    sed 's/^/       /' "$D/gw.txt"; exit 1
fi
# The whole point: High and Low must stay APART. Deviations are deduplicated into observations, so combining on the
# parameter alone would merge two opposite hazards into one symptom -- measured: two 'Level' rows become ONE
# observation carrying 2 causes. Overflow and cavitation need opposite responses.
for want in high_level low_level no_flow; do
    if ! grep -qE "^observe $want " "$D/gw.cb"; then
        echo "FAIL: expected a distinct observation '$want'; opposite deviations must not collapse"
        grep '^observe' "$D/gw.cb" | sed 's/^/       /'; exit 1
    fi
done
if [ "$(grep -c '^observe' "$D/gw.cb")" != "3" ]; then
    echo "FAIL: 3 rows with distinct guide words must give exactly 3 observations"
    grep '^observe' "$D/gw.cb" | sed 's/^/       /'; exit 1
fi

# Parameter with NO guide word must be refused, and the refusal must explain the collapse rather than just listing
# column names. A worksheet one column short is not malformed, and the author is entitled to know why it matters.
if "$HAZOP2CB" "$D10/hazop_parameter_only.csv" po > /dev/null 2> "$D/po.txt"; then
    echo "FAIL: a Parameter column with no Guide Word and no Deviation must be refused, not guessed at"
    exit 1
fi
if ! grep -q "Parameter column but no Guide Word" "$D/po.txt"; then
    echo "FAIL: the refusal must explain that parameter alone collapses high and low into one observation"
    sed 's/^/       /' "$D/po.txt"; exit 1
fi

# And the clean fixture must still score zero on BOTH new reports.
if ! grep -q "0 shared across the document, 0 not an IPL" "$D/clean.txt"; then
    echo "FAIL: the independent fixture should score zero on the new reports too"
    sed 's/^/       /' "$D/clean.txt"; exit 1
fi

echo "OK: HAZOP imported, deviations deduplicated, CAPECO row named, draft refused,"
echo "    and silent on a worksheet whose safeguards are genuinely independent"
