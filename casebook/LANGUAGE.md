# The casebook language, and the compiler that eats it

## Why there is research behind this rather than invention

The translation from structured domain knowledge into a Dung argumentation framework is not a new problem. It is
**ASPIC+**, and it has fifteen years of theory behind it. From the ASPIC tutorial: *"ASPIC+ is meant to generate
abstract argumentation frameworks in the sense of Dung (1995)."* That is this compiler's job description.

So the design below adopts ASPIC+'s **vocabulary** and **correctness criteria**, and deliberately rejects its
**argument construction machinery**. Reasons for both follow.

---

## 1. The attack taxonomy, which we had already discovered by accident

ASPIC+ builds arguments from *strict* and *defeasible* inference rules, and observes that this gives exactly three
ways to attack one:

> Arguments are defined as inference trees formed by applying two kinds of inference rules: strict and defeasible
> rules. This naturally leads to three ways of attacking an argument: attacking a premise, attacking a conclusion
> and attacking an inference. To resolve such attacks, preferences may be used, which leads to three corresponding
> kinds of defeat: **undermining**, **rebutting** and **undercutting** defeat.
> — Prakken, *An abstract framework for argumentation with structured arguments*

Checking our two hand-written casebooks against that taxonomy: every single attack we wrote falls into one of the
three, and we had invented all three without names.

| our edge | ASPIC+ name | what it attacks |
|---|---|---|
| `def_calib_stale → obs_vib_high` | **undermining** | a premise — the reading itself |
| `def_xmtr_untrusted → obs_xmtr_says_normal` | **undermining** | a premise |
| `dx_cavitation ↔ dx_impeller_defect` | **rebutting** | a conclusion — two incompatible diagnoses |
| `obs_suction_press_low → benign_duty_change` | **rebutting** | a conclusion |
| `benign_duty_change → dx_cavitation` | **undercutting** | the *inference* from vibration to cavitation |
| `def_current_incoherent → claim_current_broadband` | **undercutting** | the inference from raw reading to usable claim |

Three for three, on two unrelated equipment classes. That is strong evidence the taxonomy is complete for this
domain, and it means the source language gets principled keywords instead of invented ones.

## 2. The five rationality postulates — and the two that matter most

ASPIC-style frameworks are judged against five postulates from Caminada & Amgoud and Caminada et al.: **closure**,
**direct consistency**, **indirect consistency**, **non-interference**, and **crash-resistance**.

Two of these are exactly the properties a *palette* of casebooks needs, and this is the most valuable thing the
research turned up:

- **Non-interference** — combining two syntactically disjoint knowledge bases, neither influences the other's
  conclusions. Install a pump casebook and a tank casebook on one device and the pump's arguments must not change
  the tank's verdict. This is the **modularity guarantee** the whole product rests on, and it already has a formal
  definition and known proof techniques.
- **Crash-resistance** — no "contaminating" knowledge base exists whose presence renders every other knowledge
  base's conclusions irrelevant. In product terms: **a badly written casebook cannot break the others.**

Those are not nice-to-haves. Shipping a library of casebooks that compose on one device is the business, and these
two postulates are precisely the statement that it works. They are also *testable* — the compiler can check
syntactic disjointness, and the validator can verify that a union framework restricted to one casebook's arguments
gives the same verdicts as that casebook alone.

**Direct consistency** we already check in a special case: "no state selects two actions at once" is direct
consistency restricted to the action set. The general form is stronger and worth adopting.

One incidental validation: Heyninck & Straßer's ASPIC⊖ *"demonstrated compliance with all five postulates by
ignoring undercuts and focusing on **grounded semantics**, which only has singular solutions."* We chose grounded
for a WCET argument. It turns out to also be the semantics where the postulates are easiest to satisfy. Two
independent reasons for the same decision.

## 3. What we reject, and why

ASPIC+ constructs arguments as **inference trees**: every distinct derivation of a conclusion is a distinct
argument. That is right for a logic and wrong for a microcontroller — the argument count grows combinatorially with
rule depth, and `Framework<256, 2048>` has a hard ceiling.

Our model is **flat**: one argument per named claim, attacks between names. It is less expressive — no derivation
structure, no logical closure, no unification — and it fits in 29 KB with a proven WCET.

So: **ASPIC+ for the vocabulary and the correctness criteria, not for the construction.** A deliberate, documented
loss of generality in exchange for a bounded device footprint.

---

## 4. The language

Design rule, derived from counting our own hand-written casebooks: **the author writes domain content only, and the
compiler generates every piece of plumbing.** In the pump casebook 12 of 37 arguments were evidence gates; in the
tank, 10 of 33. Roughly a third of both was machinery no engineer should ever type.

### Declarations

```
casebook <name> v<n>                       # identity
source "<doc>" dated <date> by "<person>"  # provenance. Required.
note "<free text>"                         # rationale, citations

observe <name> "<description>"             # a sensor-facing reading
exclusive <name> <name> ...                # physically incompatible readings
implies <name> <name> ["why"]              # the first reading cannot occur without the second

claim  <name> "<desc>" needs <obs> ...     # a defeasible hypothesis + its evidence
excuse <name> "<desc>" needs <obs> ...     # a benign explanation
doubt  <name> "<desc>" needs <obs> ...     # a reason to distrust evidence

<a> undermines <b> ...                     # attack a premise
<a> rebuts      <b> ...                    # attack a conclusion
<a> undercuts   <b> ...                    # attack an inference

action <name> [idle] [safe] [when <claim> ...]
prefer <action> over <action> [over <action> ...]
```

That is the entire surface. Nine keywords.

### Worked example: the pump casebook

`pump.cb` below compiles to what is currently 44 hand-written arguments and 69 hand-written attacks in
`pump_casebook.hpp`. It is 40 lines and a reliability engineer can read it.

See `pump/pump.cb`.

---

## 5. What the compiler generates

Every one of these is mechanical, and every one was discovered by hand-writing casebooks first.

| generated | trigger | why |
|---|---|---|
| `gate_no_<obs>` per `needs` | every `needs` clause | an AF cannot say "absent"; a presumption must be attacked by a gate that its evidence defeats |
| conjunction gates | `needs` with 2+ observations | one gate per conjunct. The argument survives only when every gate is Out, which requires every observation In |
| `cw_closed_world` + attacks on absent observations | always | an absent reading must be decisively **Out**, not Undecided. Self-attack fails: a self-attacker is Undecided, and an Undecided attacker poisons everything downstream |
| raw/claim split | a `doubt` whose `needs` overlaps its `undermines` | otherwise `doubt → obs → gate → doubt` is a 3-cycle and the whole channel silently goes Undecided |
| incoherence doubt | `exclusive` | if two physically incompatible readings both appear, the instrument is faulty. One `exclusive` line replaces 5 arguments and 8 attacks |
| consistency doubt | `implies` | a reading present without one it physically requires means an instrument is wrong. Fires when the first is present and the second absent, discards the first, and is named `mismatch__<a>__<b>` so an action can cite it |
| idle action wiring | `action ... idle` | without an idle action a healthy quiet asset reports `Unsatisfied`, i.e. a defect on the most common operating state |
| precedence edges | `prefer ... over ...` | action mutual exclusion |
| name table | always | witnesses must render names and one-based `FileArg`, never internal indices |

## 6. Passes

```
1  parse            → AST. Line-oriented, no expressions, no precedence. A hand-written
                       recursive-descent parser is ~300 lines and needs no generator.
2  resolve          → names to indices. Undefined reference, duplicate, typo, and TYPE
                       detection: `undermines` must target a reading, `rebuts`/`undercuts`
                       must target a presumption, and an action can never be evidence.
3  lower            → generate gates, closed-world, raw/claim splits, incoherence doubts,
                       idle wiring, precedence. The whole table in §5.
4  check            → dead gates; closed-world unattacked; idle and safe actions present;
                       observation count within the 32-bit mask; the raw/claim split held.
5  validate         → exhaustive solve over 2^observations. Runs INSIDE the compiler, because
                       the lowered form is enough to build a framework -- so the entire input
                       space is checked before a line of C++ is written.
6  emit             → C++ header (device), .af and apx (interchange with the ICCMA engine and
                       other solvers), name table, provenance block.
```

Pass 5 is where the competition engine finally earns its keep: exhaustive validation of *knowledge* is only
affordable because solving is nearly free.

### Correction to an earlier draft of this document

An earlier version said pass 5 should validate "2^observations, minus states excluded by `exclusive`". That was
wrong, and building it revealed why. If exclusive states are skipped, the instrument-incoherence doubt generated
*from* `exclusive` is never exercised — the compiler would emit a safety mechanism and then decline to test it.

Exclusive readings appearing together is not impossible; it is an **instrument fault**, which is real and
reachable. So the validator explores every state and instead *partitions* the ambiguity report:

```
ambiguity breakdown
  genuine diagnostic ambiguity       56
  instrument incoherence             30   (an exclusive group fully present)
```

Same total, but now an engineer can tell "add a sensor" from "fix a sensor", which are different purchase orders.

## 6a. Two evidence combinators, discovered by need

Writing the tank casebook in this language immediately broke the first draft, which had only conjunctive `needs`.
Two gaps:

- *"the level is safe if the transmitter says so **or** somebody read the gauge"* — disjunction.
- *"unmeasured filling = rising level **and** no independent measurement"* — conjunction over **claims**, not
  readings.

So `needs` (AND) gained a dual, `when` (OR), and both range over readings *and* presumptions. The lowering is the
duality made literal:

| combinator | lowering | why |
|---|---|---|
| `needs a b` | one gate **per** requirement | several gates on one argument ⇒ every one must fall ⇒ **AND** |
| `when a b` | one **shared** gate, attacked by each | several attackers on one gate ⇒ any one fells it ⇒ **OR** |

A doubt is the one exception: it always gates on the **raw** reading, never the usable claim, because a doubt must
be able to fire from evidence it is about to discard. That asymmetry is what keeps the raw/claim split acyclic.

## 7. Open design questions

- **Severity still does not translate.** FMEA carries Severity / Occurrence / Detection and an RPN; an AF has no
  weights. ASPIC+ resolves attacks into defeats using a **preference ordering**, which is the right hook — a
  `prefer` relation over claims, not just actions. That would let RPN inform which rebuttal wins. It also imports
  the restricted-vs-unrestricted rebuttal question, which has known consequences for which postulates hold.
- **Thresholds live outside.** `observe vib_high` is already thresholded. The binding layer owns the numbers and
  needs its own provenance and versioning; a casebook is not self-contained without it.
- **Two questions, two frameworks.** HAZOP writes consequences *assuming safeguards fail* (design-time
  worst-casing); an arbiter adjudicates what is defensible given live evidence. The same source document yields
  different frameworks depending on which is wanted, and the compiler must be told which.

## References

- Prakken, *An abstract framework for argumentation with structured arguments* — <https://webspace.science.uu.nl/~prakk101/pubs/aspicAF.pdf>
- ASPIC+ tutorial — <https://cdn.prod.website-files.com/674c48c6202b4f012a14375f/67cda7a65863caa70b89a3c3_ASPICtutorial.pdf>
- Modgil & Prakken, *A General Account of Argumentation with Preferences* — <https://arxiv.org/html/1804.06763>
- Cramer & Friese, *Satisfying Rationality Postulates of Structured Argumentation Through Deductive Support* — <https://arxiv.org/html/2604.21515v1>
- ICCMA solver requirements and the `apx` interchange format — <http://argumentationcompetition.org/2019/SolverRequirements.pdf>

## `vote k of` — the counting quantifier

Added after a fourth problem shape exposed the gap. Redundancy is how every safety instrumented function in process
industry is built, and 2-out-of-3 voting is the canonical arrangement (IEC 61511, ISA-TR84.00.02). The language could
already express it — as the disjunction of the three pairs — so nothing was *impossible*. What was missing was safety.

`vessel-hl-trip` needs "at least two of these three" in two places. Written by hand, the second one was wrong:

```
claim leg_frozen when any lt_a_flatline lt_b_flatline lt_c_flatline
claim leg_blind  when any lt_a_unavailable lt_b_unavailable lt_c_unavailable
claim vote_collapsed needs leg_frozen leg_blind        # WRONG
```

That reads as "two legs compromised". It means "at least one leg frozen **and** at least one leg blind", which is a
different predicate in two ways. It over-fires when a single transmitter is both — prevented here only by the binding
layer's mutual exclusion, so the safety argument silently depended on a downstream component, and an integrator
supplying masks directly would have had a SIF that trips on one dead transmitter. And it **under-fires** when two legs
fail the *same* way: two frozen transmitters, or two unavailable ones, never triggered collapse at all. That is the
dangerous direction — a voted function that has quietly become 1oo1 and does not say so.

The correct form is the pair disjunction over per-leg claims. It is mechanical, so the compiler generates it:

```
claim vote_collapsed "two legs are compromised; one transmitter is now deciding alone" severity 10
  vote 2 of leg_a_compromised leg_b_compromised leg_c_compromised
```

Each of the `C(n,k)` combinations becomes a conjunctive claim and the original becomes their disjunction. Generated
names carry the parent and the members (`vote_collapsed__vote_leg_a_compromised_leg_b_compromised`), because a witness
has to stay readable. Branches inherit the parent's `severity`: a branch is not a separate hypothesis, it is one way of
establishing the same one, so it must not lose a preference the author declared.

Diagnostics:

- `vote k of` with `k` equal to the number of items is conjunction, and is rejected with a pointer to `needs`.
- `k` greater than the item count is a claim that can never hold, and is rejected as dead.
- Expansions above 64 arguments are rejected with the advice to group items into intermediate claims. `vote 5 of`
  fifteen sensors is 3,003 arguments and would exhaust a microcontroller with no indication of why.

`vote.cb` and `vote_manual.cb` differ only in this construct, and `casebook_vote_equivalence` compares them across all
512 states: **0 outcome differences, 0 action differences.** A counting quantifier that silently voted 1oo3 or 3oo3
would read as correct in the source and fail only in the plant, so it is checked rather than reviewed.

### Where it deliberately is *not* used

The same casebook's trip condition is also 2-of-3 and is still written as an explicit pair disjunction. The pairs are
needed three lines later to say *which* transmitter dissents — "A and B agree and C does not" is a statement about a
specific pair, and a counting quantifier abstracts precisely that away.

This is worth stating as a design limit rather than hiding. A PLC can already tell you that two of three agree. Naming
the transmitter to go and calibrate is the part worth paying for, and for that the combinations have to stay visible.
`vote k of` earns its place where the count is genuinely all you need.

## `implies` — a reading that cannot stand alone

`exclusive` catches readings that cannot co-occur. `implies` is its dual: a reading that cannot occur *alone*.

```
implies flow_high current_high "moving water draws motor current"
```

A pump reporting flow while its motor draws no current has no exotic fault. One of the two instruments is wrong: the
flow reading is false, or the current channel is dead, and under closed world a dead channel reads exactly like a quiet
one. This is the analytical-redundancy idea from fault diagnosis, restricted to the form that needs no arithmetic and so
can be checked over every state like everything else.

The compiler generates one doubt, `mismatch__flow_high__current_high`, that

- fires when the antecedent's RAW reading is present and the consequent's RAW reading is absent. Raw on both sides,
  because the question is whether the instruments agree, not whether either is trusted;
- discards the antecedent, because an uncorroborated reading is the one that must not be acted on. Since it needs and
  discards the same reading, the antecedent gets the raw/claim split, which keeps the framework acyclic;
- is an ordinary doubt, so an action can cite it: `action inspect when mismatch__flow_high__current_high`.

The validator reports how many states each consistency doubt fires in, and flags one that fires in none. The fixture is
`casebook/fixtures/consistency/flow_current.cb`; `test_cbc` checks, with a solver written independently of the device's,
that the doubt fires in exactly the states where the antecedent stands alone and is never Undecided.

Nothing changes on the device: the doubt and its edges are ordinary arguments and attacks, so the data image format,
the runtime and both firmwares are untouched.

## Symbolic validation — every state at once

`cbc --symbolic` validates without visiting states one by one. Each argument's In and Out status becomes a Boolean
function of the presence bits, held as a reduced ordered binary decision diagram, and the grounded labelling is
computed as the least fixed point of "In when every attacker is Out, Out when some attacker is In" over those
functions. `arbiter::decide` is then restated over sets of states, which gives the exact number of states in each
outcome and the states in which each action is chosen.

| | exhaustive | symbolic |
|---|---|---|
| pump (16,384 states) | 0.04 s | under 1 ms, 412 nodes |
| pump + tank palette (536,870,912 states) | not run: about 2.5 µs per state on this PC for the pump alone, so an estimated 20+ minutes | under 1 ms, 883 nodes |

For a palette it also PROVES non-interference over the whole combined space: it asks which presence bits each member's
outcome depends on, and fails if any belongs to another member. The exhaustive palette check argues the same thing
structurally (the merge preserves each member exactly); this checks it semantically, in every combined state.

Trust comes from agreement where both can run. `test_symbolic` visits every state of every shipped casebook and
compares the symbolic label of every argument and the outcome against `solver::Grounded` and `arbiter::decide`, the
code that runs on the chips — 0 disagreements — and samples 5,127 palette states the same way.

What it does not do: compute per-state work. The timing bound and the worst-case state the firmware measures still come
from exhaustive validation, so device images still require it. The 32-bit presence mask is a device limit and is
unchanged.

## Commissioning sheet — `cbc --commissioning`

`cbc pump.cb --commissioning pump.sheet` writes the loop-check sheet as tab-separated text: the image fingerprint, the
actions in the order the device numbers them, the action at rest, and one row per reading. Each row gives how the
reading is set (a presence bit, or the channel threshold, unavailability or flatline that sets it), the action with
it alone, and the fewest other readings (up to three) with which it changes the decision, with the action with and
without it. quorum studio prints it and drives a board through it over Modbus (`studio/README.md`). It exists
because exhaustive validation proves the rules and says nothing about whether the plant feeds them the right readings.

## A second implementation — `tools/refcheck.py`

`tools/refcheck.py <cbc> <file.cb> ...` reads rulebooks itself, evaluates them by the rules in this document (no
framework, no gates: a three-valued fixed point over the declarations), and compares its outcome and action with cbc's
certificate in every state. `--fuzz <n> <seed>` does the same for random rulebooks. `tools/refcheck_test.sh` (ctest
`refcheck`) runs both and requires six deliberate mistakes in refcheck to be noticed.

One rule it pinned down, by finding cbc breaking it: **a doubt reads its evidence raw, whatever else discards that
reading.** That includes the consequent of an `implies`. A reading that something discards and some doubt reads raw is
therefore split into raw and usable forms, and every discarding attack lands on the usable form, however it is written.

## Proof certificates — `cbc --certificate` and `tools/qcert`

`cbc pump.cb --certificate pump.cert` writes, for every state, the decision and the complete grounded labelling with
the round in which each argument was labelled. `qcert pump.cert 0x1a375f9d` checks it. `qcert` is about 200 lines of
plain C99 with no quorum headers, so an auditor can read the whole of what they are trusting in one sitting instead of
the compiler.

Rounds are what make a grounded labelling checkable locally. In at round r needs every attacker Out before r; Out at
round r needs some attacker In before r; an unlabelled argument must have no In attacker and some attacker not Out.
The first two show the labelled part is contained in the grounded labelling, the third that it is a fixed point, and
the grounded labelling is the least fixed point, so they are equal. The outcome and action are then re-derived from
the labelling by the decision rule, every one of the 2^n states must appear exactly once, and the FNV-1a fingerprint
is recomputed from the framework in the certificate. Passing the fingerprint the device printed at start-up pins the
certificate to the rulebook actually running.

`cbc` computes the rounds by its own synchronous iteration and refuses to write a certificate unless it agrees with
`solver::Grounded` in every argument of every state. The pump certificate is 2.4 MB and checks in under 0.1 s.

`tools/qcert/tamper_test.py` (ctest `qcert_*`, on four casebooks) makes 12 or 13 minimal forgeries — one label, one
round, one outcome, one action, a deleted or repeated state, a redirected attack, a changed fallback, a changed idle
action, a nudged setpoint where the casebook has thresholds, truncation — plus a wrong device fingerprint, and requires
every one to be rejected.

The fingerprint covers the framework, the action list, the closed-world argument, the idle and fallback actions, and
the binding: every channel's range, staleness and flatline limits and every threshold's setpoint, release, sense and
hold. Certificate format 2 carries all of it so `qcert` can recompute the whole number; `qcert` hashes the setpoints
without judging them. Until 2026-10-11 the fingerprint covered only the framework and the action list, so two images
differing only in their fallback, or in a setpoint, reported the same fingerprint; closing that changed every
fingerprint (the pump's from 0x2e4c18a0 to 0x1a375f9d), and the measurement files in `firmware/` from before that date
quote the old values. Palettes are certified member by member.

## Time in rules — `for <duration>` on a threshold

```
channel bearing_temp "bearing temperature" unit degC
  range -20 200
  stale_after 10s
  asserts bearing_hot above 90.0 release 85.0 for 5s
```

`bearing_hot` asserts only once the temperature has STAYED at or above 90.0 for 5 s: an on-delay, which is how alarm
philosophies are written (ISA-18.2) and what separates a sustained condition from a spike. A sample back below `on`
before the hold elapses restarts it. Once asserted, release is ordinary hysteresis at `off` and is NOT delayed, because
delaying the release of an alarm condition is the dangerous direction. A channel that goes stale or out of range
suppresses its thresholds, matured hold or not.

Time lives in the binding layer (`binding.hpp`), not in the framework, deliberately. The framework stays a function of
the present presence mask, so exhaustive validation, `--symbolic` and `--certificate` still cover every state exactly:
time decides only WHEN a reading becomes true, never what a true reading means. The hold matures on schedule at mask
time even if no new sample arrives.

On the device a held threshold needs data-image FORMAT 3: format 1 plus a `hold_ms` word per threshold. cbc writes
format 3 only when some threshold has a hold, so every other casebook still produces byte-identical format-1 images
(checked for pump, tank, permit and vote against the previous compiler), and older firmware refuses a held image with
BadFormat rather than silently dropping the hold. Fixture: `casebook/fixtures/held/held.cb` (runtime test
`runtime_blob_held`); behaviour: `test_binding` (spikes ignored, maturing exactly on time without a sample, deadband,
immediate release, a zero hold identical to none).

The fingerprint covers the binding tables, holds included (since 2026-10-11; see proof certificates above), so a
deliberately edited setpoint with a recomputed CRC is refused as a fingerprint mismatch, not loaded. `test_blob` edits
the fallback and the first setpoint of every image that has one, repairs the CRC, and requires the refusal.


## Timed readings — what happened before what

```
reading dead_head      "pressure went high within 10 s of a start" when pressure_high within 10s after motor_started
reading ran_hot        "the bearing was hot in the last 5 minutes"   when bearing_hot within last 5m
reading repeated_trips "the overload relay tripped 3 times in an hour" when overload_trip 3 times in 1h
reading starting       "the first 30 s after a start"                 when motor_started within first 30s
reading door_off       "the guard switch has read open for 2 s"       when door_open for 2s
```

A `reading` is a yes/no reading the device works out from other readings over time. Rules use it like any other
reading: `claim closed_valve "..." needs dead_head`. There are five forms:

| form | set when | clears when |
|---|---|---|
| `X within T after Y` | X becomes set no more than T after Y became set (Y may have cleared since) | X clears |
| `X within last T` | X is set, and for T after X was last seen set | T has passed since X was last seen set |
| `X N times in T` | X has become set at least N times (2 to 16) in the last T | the N-th most recent of those is older than T |
| `X within first T` | X is set, and became set no more than T ago | X clears, or T has passed since it became set |
| `X for T` | X is set, and became set at least T ago | X clears |

`within first` is the start-up bypass: `excuse start_transient "..." needs starting` with `start_transient undercuts
cavitation` keeps low suction from raising an alarm for the first 30 s of each start, and no longer. `for` is an
on-delay for any reading, not only a channel threshold: a switch must read open for 2 s before it counts, so a bounce
does not. A source that clears and sets again starts both over.

Durations are written `500ms`, `5s`, `2m` or `1h`, up to 24 days. Every window is inclusive: pressure exactly 10 s after
the start counts. A relay that stays tripped is one trip, not many.

**As with the threshold hold, time lives in the binding layer and the framework never sees it.** Each timed reading is
one presence bit, so exhaustive validation, `--symbolic` and `--certificate` still cover every combination of readings,
the timed ones included. What they do not cover is WHEN the device sets the bit; `test_timers` checks that against a
second implementation that keeps the whole history and applies the table above literally.

Three rules keep this simple enough to check:

- **A timed reading watches readings, never conclusions**: an `observe`, a reading a channel sets, or another timed
  reading (`reading hot_twice "..." when ran_hot 2 times in 1h`). cbc orders the table so that each timed reading
  comes after those it watches, and the device works them out in that order, so each sees the others as they are at
  the same moment. Two timed readings that watch each other, directly or through others, are refused. A conclusion is
  refused because the rules' own answers would then feed back into their inputs, and exhaustive validation would no
  longer cover what the device does.
- **The device sees every reading the controller writes.** It steps the timed readings just before and just after
  each Modbus write, so a reading written set and cleared by the next write still counts, however close together the
  two writes are. Changes that come with time alone (a hold maturing, a channel going stale) are seen at the next
  step, about every millisecond on both boards. What the device cannot see is a pulse on the plant that the
  controller never writes: that limit is the controller's scan time. Times are kept to the millisecond. At start-up,
  a reading already set counts as having just become set: the device does not assume a quiet history it never saw.
- **A doubt does not reach through a timed reading.** `doubt gauge_bad ... undermines pressure_high` discards
  `pressure_high` but not `dead_head`, which the device works out from the raw bit. Name both if a suspect gauge should
  discard both; cbc warns when a doubt names a source but not the timed reading built from it.

Timed readings are set by the device and cannot be written over Modbus, like channel readings. On the commissioning
sheet each is described as `timed: ...`, and the sheet's answers include the timed readings that react at once
(setting `motor_started` with `pressure_high` sets `dead_head`). At most 8 per rulebook (the board runtime's table),
each counting against the 30 readings.

**Testing a timed rulebook on the bench.** A timed reading cannot be set by writing its bit, so `cbc --commissioning`
adds a script to the sheet: each timed reading just inside and just outside its window, with the readings that make it
matter, then every line of the sheet. A chained reading is driven through the base readings at the bottom of its
chain. Every expected answer comes from replaying the whole script through the table above, over the full history,
not from the device's timer code; CI plays each fixture's script against the device's Modbus map
(`runtime_timed_script_*`).

The studio's bench test plays the script on the board's **test clock** (Modbus registers 10 to 12,
`runtime/GUARD_AND_MODBUS.md`), which moves the board's clock forward, so an hour-long window takes seconds. The test
clock can be turned on only before anything else has been written to the board since it started, and it stays on
until the board is reset, so **reset the board before it goes into service**; the studio says so after each run. On a
board that has already been written to, the bench test asks for a reset, or with `-real-time` (a box in the studio)
plays the script by waiting: 71 minutes for `timed.cb`, about 8 hours for `chained.cb`.

On the device a timed rulebook needs data-image FORMAT 4: format 3 plus the timer table. cbc writes it only when a
`reading` exists, so every other rulebook still produces byte-identical images, headers, certificates and fingerprints
(checked for all 67 repository rulebooks against the previous compiler, 2026-10-26), and firmware that predates timers
refuses a timed image instead of running it with the timed readings stuck at absent. The fingerprint and the
certificate cover the timer table. A table where a timed reading watches another is also format 4, but firmware from
before chains refuses it at load rather than working the chain out wrongly. Fixtures:
`casebook/fixtures/timed/timed.cb`, and `chained.cb` with five timed readings, chained three deep.
