#!/bin/bash
# The compiler must REFUSE these casebooks.
#
# Registered as a test because "cbc rejects bad input" is only a property if something checks it. Each fixture is a
# realistic authoring mistake that reaches a Verdict outcome no correct casebook ever reaches -- which is exactly why
# the three real casebooks cannot test it. All three report Conflicted and Unsatisfied in 0 states, as they should.
#
# Each fixture must fail, and the failure must name the offending actions and an example state. A refusal that says
# only "1 states" is not actionable, and this asserts the actionable part too.
set -u

cbc="$1"
dir="$2"
failures=0

expect_rejected() {
    local name="$1"
    shift
    local output
    output=$("$cbc" "$dir/$name.cb" 2>&1)
    local code=$?

    if [ "$code" -eq 0 ]; then
        echo "FAIL  $name.cb was ACCEPTED; it is deliberately broken and must be refused"
        failures=$((failures + 1))
        return
    fi
    echo "OK    $name.cb refused (exit $code)"

    for needle in "$@"; do
        if ! printf '%s' "$output" | grep -qF -- "$needle"; then
            echo "FAIL  $name.cb: the refusal never mentions \"$needle\""
            echo "$output" | sed 's/^/        /'
            failures=$((failures + 1))
        else
            echo "OK      says: $needle"
        fi
    done
}

# Two actions with no ordering between them. Must name both, and the state, and suggest a prefer.
expect_rejected conflicted \
    "no state selects two actions at once" \
    "are BOTH selected when" \
    "open_valve" \
    "close_valve" \
    "prefer open_valve over close_valve"

# An idle action with a condition. Must say where to look and why idle actions take no conditions.
expect_rejected unsatisfied \
    "every state selects an action" \
    "no action is selected when" \
    "idle action should have NO conditions"

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures failures"
    exit 1
fi
echo "every broken fixture was refused, with an actionable reason"
