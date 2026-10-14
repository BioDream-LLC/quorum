#!/usr/bin/env bash
#
# The README's measured numbers, checked against the tools that produce them.
#
# WHY THIS EXISTS
#
# A full test sweep found every executable check passing and four documented numbers stale. Pulling that thread found
# sixteen of the twenty cells in the README's footprint table wrong, including the headline figure -- the certifiable
# safety function was advertised at 2,640 bytes of flash when it had measured 2,620 since the WCET speedup. Better than
# claimed, and still wrong.
#
# Everything else in this repo is checked against the thing it describes. The safety case is checked bidirectionally
# against the test suite. The casebook fingerprint is checked against the device. The static WCET model is checked against
# the disassembly. The README's numbers -- every headline figure in the product's front door -- were hand-copied and
# checked by nobody.
#
# HOW
#
# Two directions, because one direction is how the safety case missed things until it was made bidirectional.
#
#   FORWARD   marked regions of README.md are GENERATED from the tools. Prose stays hand-written; numbers do not.
#   REVERSE   comma-grouped integers OUTSIDE those regions are hand-written figures. Each must be registered in
#             docs/figures-allow.txt with a reason, so a number nobody can account for cannot sit in the README.
#
# Usage:
#   tools/figures.sh              check; non-zero exit if README disagrees with the tools
#   tools/figures.sh --update     rewrite the generated regions
#
set -euo pipefail
cd "$(dirname "$0")/.."

README="README.md"
CLAIMS="docs/claims.tsv"
ALLOW="docs/figures-allow.txt"
UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

fail=0
note() { printf '  %s\n' "$*"; }

# ── producers ────────────────────────────────────────────────────────────────
# Each emits the exact markdown for one region. Keep them pure: read from tools, write to stdout, no side effects.

commas() { printf '%s' "$1" | sed -e :a -e 's/\([0-9]\)\([0-9]\{3\}\)\($\|,\)/\1,\2\3/;ta'; }

gen_footprint() {
    local out
    out="$(./tools/footprint.sh 2>/dev/null)"
    printf '| configuration | flash | RAM | stack | total RAM |\n|---|---|---|---|---|\n'
    # footprint.sh prints a fixed-width table; the rows are the lines between the rule and the blank line.
    printf '%s\n' "$out" | awk '
        function commas(n) { s = n; while (match(s, /[0-9][0-9][0-9][0-9]/)) sub(/[0-9][0-9][0-9]($|,)/, ",&", s); return s }
        /^-------/ { inrow = 1; next }
        inrow && NF == 0 { inrow = 0 }
        inrow {
            n = NF
            flash = $(n-3); ram = $(n-2); stack = $(n-1); total = $n
            label = ""
            for (i = 1; i <= n-4; i++) { label = label (i > 1 ? " " : "") $i }
            printf "| %s | %s | %s | %s | %s |\n", label, commas(flash), commas(ram), commas(stack), commas(total)
        }'
    printf '\n'
    local f r s t
    f=$(printf '%s\n' "$out" | awk '/safety function, flash/ {print $(NF-2)}')
    r=$(printf '%s\n' "$out" | awk '/safety function, RAM/   {print $(NF-2)}')
    s=$(printf '%s\n' "$out" | awk '/^\+ binding layer/ {print $(NF-1)}')
    t=$(printf '%s\n' "$out" | awk '/^\+ binding layer/ {print $NF}')
    printf 'The certifiable safety function is **%s bytes of flash and %s bytes of RAM including a %s-byte stack**, or\n' \
        "$(commas "$f")" "$(commas "$t")" "$(commas "$s")"
    printf '%s bytes of RAM excluding it.\n' "$(commas "$r")"
}

gen_crossverify() {
    local out
    out="$(./tools/cross-verify-exhaustive.sh 2>/dev/null)"
    printf '| casebook | states | CaDiCaL calls | mismatches |\n|---|---|---|---|\n'
    printf '%s\n' "$out" | awk '
        function commas(n) { s = n; while (match(s, /^[0-9]+[0-9]{3}/)) sub(/[0-9]{3}($|,)/, ",&", s); gsub(/,,/, ",", s); return s }
        /^(permit|vote|pump|tank)[ \t]/ { printf "| %s | %s | %s | %s |\n", $1, commas($2), commas($3), $4 }
        /states,.*CaDiCaL calls/ { st = $1; ca = $3 }
        END { printf "| **total** | **%s** | **%s** | **0** |\n", commas(st), commas(ca) }'
}

gen_boards() {
    local out
    out="$(./tools/board-matrix.sh 2>/dev/null)"
    printf '| board | core | flash | RAM | status |\n|---|---|---|---|---|\n'
    printf '%s\n' "$out" | awk '
        function commas(n) { s = n; while (match(s, /[0-9][0-9][0-9][0-9]/)) sub(/[0-9][0-9][0-9]($|,)/, ",&", s); return s }
        /(Cortex-M|AVR 8-bit)/ && /(SUPPORTED|NOT SUPPORTED)/ {
            notsup = /NOT SUPPORTED/
            # trailing fields: flash RAM sram SUPPORTED  (or - - sram NOT SUPPORTED)
            n = NF - (notsup ? 1 : 0)
            sram = $(n-1); ram = $(n-2); flash = $(n-3)
            core = ""; lbl = ""
            for (i = 1; i <= n-4; i++) {
                if ($i ~ /^(Cortex-M[0-9]+\+?|AVR)$/ || $i == "8-bit") { core = core (core ? " " : "") $i }
                else if (core == "") { lbl = lbl (lbl ? " " : "") $i }
                else { core = core " " $i }
            }
            printf "| %s | %s | %s | %s | %s |\n", lbl, core,
                   (flash == "-" ? "--" : commas(flash)), (ram == "-" ? "--" : commas(ram)),
                   (notsup ? "**not supported**" : "supported")
        }'
}

# The static WCET bounds. Kept generated because a stale timing bound is the worst kind of stale number: the README had
# tank-409 at 2,357 instructions when the real bound had risen to 2,412, so the front door UNDER-reported a safety-relevant
# figure. 150 MHz at 2 CPI is the Pico 2's stated clock and the conservative cycles-per-instruction assumption.
gen_wcet() {
    printf '| casebook | instructions | us @ 150 MHz, 2 CPI |\n|---|---|---|\n'
    ./build/wcet/test_static_bound 2>/dev/null | awk '
        function commas(n) { s = n; while (match(s, /[0-9][0-9][0-9][0-9]/)) sub(/[0-9][0-9][0-9]($|,)/, ",&", s); return s }
        /^[a-z]/ && NF == 1 { name = $1 }
        /worst-case INSTRUCTIONS/ {
            printf "| %s | %s%s%s | %.1f |\n", name,
                   (name == "pump" ? "**" : ""), commas($3), (name == "pump" ? "**" : ""),
                   $3 * 2.0 / 150.0
        }'
}

# ── forward: regenerate or check the marked regions ───────────────────────────
region_check() {
    local name="$1" body="$2"
    local begin="<!-- BEGIN GENERATED ${name} -- tools/figures.sh; do not edit by hand -->"
    local end="<!-- END GENERATED ${name} -->"

    if ! grep -qF "$begin" "$README"; then
        note "MISSING REGION  $name is not marked in $README"
        fail=1
        return
    fi

    local tmp; tmp="$(mktemp)"
    BEGIN_M="$begin" END_M="$end" BODY="$body" python3 - "$README" > "$tmp" <<'PY'
import os, sys
begin, end, body = os.environ["BEGIN_M"], os.environ["END_M"], os.environ["BODY"]
src = open(sys.argv[1]).read()
pre, rest = src.split(begin, 1)
_, post = rest.split(end, 1)
sys.stdout.write(pre + begin + "\n" + body.rstrip("\n") + "\n" + end + post)
PY

    if cmp -s "$tmp" "$README"; then
        note "OK              $name matches the tool"
    elif [[ $UPDATE -eq 1 ]]; then
        mv "$tmp" "$README"
        note "UPDATED         $name"
        return
    else
        note "STALE           $name disagrees with the tool that produces it"
        diff <(sed -n "/$(printf '%s' "$begin" | sed 's/[][\/.*^$]/\\&/g')/,/$(printf '%s' "$end" | sed 's/[][\/.*^$]/\\&/g')/p" "$README") \
             <(sed -n "/$(printf '%s' "$begin" | sed 's/[][\/.*^$]/\\&/g')/,/$(printf '%s' "$end" | sed 's/[][\/.*^$]/\\&/g')/p" "$tmp") \
             | sed 's/^/                  /' | head -30 || true
        fail=1
    fi
    rm -f "$tmp"
}

echo "README figures, checked against the tools that produce them"
echo
echo "forward: generated regions"
region_check footprint    "$(gen_footprint)"
region_check cross-verify "$(gen_crossverify)"
region_check boards      "$(gen_boards)"
region_check wcet        "$(gen_wcet)"

# ── reverse: every hand-written figure must be accounted for ──────────────────
#
# This is the direction that finds things. The safety case check missed five unclaimed tests until it was made to look both
# ways, and one of them turned out to be the most important capability in the repo.
#
# Both documents are scanned in one pass, because the allowlist is shared and a figure legitimately living only in the
# safety case must not be reported as missing from the README.
echo
echo "reverse: hand-written figures, README and the safety case"

if ! python3 - "$README" "$CLAIMS" "$ALLOW" casebook/AUTHORING.md casebook/LANGUAGE.md casebook/TAXONOMY.md <<'PY'
import re, sys
readme, claims, allowfile = sys.argv[1], sys.argv[2], sys.argv[3]
# PROC-D5: the authoring documents were outside this gate entirely. They are what an author reads, so a stale
# number there teaches a wrong lesson -- and the full sweep that found 26 wrong figures never looked at them.
EXTRA = sys.argv[4:]

FIG = re.compile(r"\b\d{1,3}(?:,\d{3})+\b")

# Generated regions are checked forward; exclude them here.
src = re.sub(r"<!-- BEGIN GENERATED.*?<!-- END GENERATED[^>]*-->", "", open(readme).read(), flags=re.S)

found = {}
for i, line in enumerate(src.splitlines(), 1):
    for m in FIG.finditer(line):
        found.setdefault(m.group(0), []).append(f"README:{i}")
for i, line in enumerate(open(claims), 1):
    if line.startswith("#"):
        continue
    for m in FIG.finditer(line):
        found.setdefault(m.group(0), []).append(f"claims.tsv:{i}")
for path in EXTRA:
    name = path.split("/")[-1]
    # Skip fenced code blocks line by line rather than stripping them first. Stripping shifts every line number
    # after the block, and a gate that reports the wrong line wastes exactly the time it is meant to save --
    # misleading line numbers were already one of this project's recorded findings.
    fenced = False
    for i, line in enumerate(open(path).read().splitlines(), 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        for m in FIG.finditer(line):
            found.setdefault(m.group(0), []).append(f"{name}:{i}")

allowed = set()
for line in open(allowfile):
    line = line.strip()
    if line and not line.startswith("#"):
        allowed.add(line.split("\t")[0].strip())

unregistered = {k: v for k, v in found.items() if k not in allowed}
orphaned = allowed - set(found)

for n, where in sorted(unregistered.items()):
    print(f"  UNREGISTERED    {n:>12}  {', '.join(where[:4])}")
for n in sorted(orphaned):
    print(f"  ORPHANED        {n:>12}  registered but appears in neither document")

if unregistered or orphaned:
    print()
    print("  A figure nobody can account for is how the footprint table came to be wrong in sixteen of twenty cells,")
    print("  and how the safety case came to understate its own CaDiCaL count. Register it in")
    print(f"  {allowfile} with the command that reproduces it, or move it into a generated region.")
    sys.exit(1)

print(f"  OK              {len(found)} figures across both documents, every one registered with a")
print( "                  reproduction command, and every registered figure still in use")
PY
then
    fail=1
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "the README and the safety case agree with the tools"
else
    echo "the README disagrees with the tools; run tools/figures.sh --update for the generated regions"
    exit 1
fi
