# Writing a casebook

For a reliability or process engineer who has an FMEA or a HAZOP and wants the machine to act on it.

You do not need to know what an argumentation framework is. You need to be able to say which pieces of evidence
contradict which conclusions, and what should happen. The compiler builds the rest and then checks it against every
state your equipment can be in.

Everything in this guide is compiled and validated as part of the test suite, including the worked example. If
something here does not work, the build fails.

---

## 1. The idea in one paragraph

You declare **observations** (things an instrument or a person can assert), **claims** (conclusions you want the
machine to draw), which claims are **defeated** by which evidence, and what **actions** are available. The compiler
turns that into a decision procedure and then solves it for every possible combination of observations — for a
14-observation casebook that is 16,384 combinations — and tells you which ones are ambiguous, which select no action,
and which select two. You fix those before anything ships.

The one rule that governs everything: **a claim holds unless something defeats it.** You are not writing `if`
statements. You are writing what would change your mind.

---

## 2. Start with the actions

Not the sensors. The actions. A casebook whose actions are wrong cannot be rescued by better evidence.

```
action keep_running  idle
action reduce_load
action shut_down     safe
prefer shut_down over reduce_load over keep_running
```

Three things are required and the compiler will refuse a casebook missing any of them.

**One action marked `idle`.** The thing to do when nothing is wrong. Without it, a perfectly healthy quiet machine
decisively rejects every action you declared, and that is reported as a casebook defect — on your most common
operating state.

**One action marked `safe`.** Where the machine goes when the reasoning cannot reach a conclusion. This is
asset-specific and the compiler will not guess: a pump stops, a tank stops filling, a drone must keep flying. There
is no universal safe state, so you declare yours.

**A `prefer` chain.** When two actions both apply, this decides. Put it in writing even when it feels obvious — the
whole point is that it stops being something in somebody's head at 3 a.m.

---

## 3. Observations are what somebody can assert

An observation is not a fact about the world. It is an assertion somebody or something made.

```
observe vibration_high   "10-1000 Hz velocity RMS in ISO 10816 zone C or worse"
observe gauge_read_high   "an operator walked out and read the local gauge as high"
observe permit_suspended  "a suspended permit-to-work has been found and read"
```

That distinction is the single most important thing in this guide, and section 7 is about why.

Write the description for whoever reads the explanation at 3 a.m., not for yourself today. It is carried through into
the generated header and into every witness the device prints.

Observations are limited to **30 per casebook**. If you need more, you have two casebooks; they can share a device.

---

## 4. Claims, and what defeats them

A claim is a conclusion. Say what it needs, and say what defeats it.

```
claim bearing_failing "a bearing is degrading"  needs vibration_high
```

`needs` is a **conjunction** — every item listed must hold. Items can be observations or other claims:

```
claim hazard "filling a tank whose level nobody can measure"  needs rising_level no_measurement
```

For **disjunction** — any one of these suffices — write `when any`:

```
claim level_is_high "the level is high"  when any transmitter_high gauge_read_high high_level_switch
```

`when all` also exists and means the same as `needs`.

> A bare `when a b` with more than one condition is a **compile error**. It reads as *and* in English and would mean
> *or*. That exact misreading in this project's own Piper Alpha casebook licensed starting a pump with no certificate
> that its pressure boundary was closed — the decision the casebook existed to prevent. Say which you mean.

### The three ways to defeat something

These come from the ASPIC+ literature and the distinction is genuinely useful, because each one attacks a different
part of the argument.

| form | attacks | use it when |
|---|---|---|
| `undermines` | a premise (an observation) | the reading itself should be discarded |
| `rebuts` | a conclusion (a claim) | the conclusion is wrong, though the evidence was fine |
| `undercuts` | an inference | the reasoning does not apply in these circumstances |

```
transmitter_frozen  undermines transmitter_high     # discard the reading
duty_changed        rebuts     bearing_failing      # vibration is explained by something else
recent_overhaul     undercuts  wear_progression     # the trend argument does not apply
```

The compiler type-checks these. `undermines` must target an observation and `rebuts` must target a claim; getting it
backwards is an error with the line number, not a silent misbehaviour.

### Doubts: when a reading cannot be trusted

A `doubt` is a claim whose job is to discard evidence.

```
doubt transmitter_untrusted "a frozen transmitter is not a reading"
  when any level_flatline level_unavailable
  undermines transmitter_high transmitter_normal
```

There is one restriction, and the compiler enforces it: **a doubt's own conditions must be raw observations, not
claims.** A doubt that depends on the very thing it discards would create a cycle in which everything becomes
unknowable. The compiler detects the specific case where this is unavoidable and rewrites it for you, separating the
raw reading from the usable one — you will see `claim_*` arguments appear in the generated header when that happens.

---

## 5. Channels turn numbers into observations

If a signal comes from an instrument, declare a channel. This is where thresholds live — in the reviewed, versioned
document, not in firmware.

```
channel bearing_vibration "10-1000 Hz velocity RMS, radial, drive end" unit mm/s
  range 0 50
  stale_after 5s
  flatline_after 60m epsilon 0.05
  asserts vibration_high    above 7.1  release 6.6
  asserts vibration_severe  above 11.0 release 10.5
```

Every part is required except `flatline_after`, and each refusal has a reason.

**`range`** — what a physically plausible reading looks like. A value outside it is instrument nonsense, and
critically, it will never latch a threshold. A broken ADC cannot trip a shutdown.

**`stale_after`** — how long a sample stays believable. **There is no way to say "never expires."** A channel that
cannot be reported dead defeats the entire purpose of declaring it, so the compiler refuses.

**`release`** on every threshold — the hysteresis point. Not optional: a bare threshold chatters around its setpoint,
a chattering observation makes the verdict oscillate, and oscillating verdicts are the alarm flood this product
exists to prevent. Rising thresholds release below; falling thresholds release above; getting it backwards is an
error that shows you the corrected line.

**`flatline_after`** — how long a value may sit unchanged before the channel is reported stuck. This is **not**
staleness and the difference matters enormously. A stale channel has stopped talking. A flatlined channel delivers
fresh, in-range, entirely plausible samples that never move, which is what a failed 4–20 mA transmitter actually
does, and is what nobody detected at CAPECO before a tank overflowed and a terminal burned. `epsilon` is how much
movement counts as alive, so ADC dither is not mistaken for signal.

Write values in the channel's declared unit as ordinary decimals. The compiler converts to scaled integers at build
time; the device performs no floating-point arithmetic at all.

### Two observations you get for free

Every channel generates `<name>_unavailable`, and `<name>_flatline` if you asked for the check.

**Use them.** If nothing in your casebook reasons about `<name>_unavailable`, the compiler warns you, and the warning
is the most important one it emits. Section 7 explains why.

---

## 6. Readings that cannot co-occur

```
exclusive transmitter_normal transmitter_high
```

If both appear at once the instrument is at fault. The compiler generates a doubt that fires exactly then and
discards both readings.

Note what this does *not* mean: those states are still validated. If the instrument can report two contradictory
things, that is a real condition, and skipping it would mean never testing the mechanism built to handle it.

---

## 7. The mistake this tool exists to prevent

Read this section even if you skip the rest.

When no observation is asserted, the machine treats it as false. It has to — otherwise nothing could ever be
concluded. But that means:

> **"No sensor asserted vibration" and "there is no vibration" are the same input.**
> A dead sensor and a healthy quiet machine are indistinguishable.

This is not a theoretical concern. It is the mechanism of two of the accidents in this repository.

At **CAPECO** in 2009, a level transmitter had failed and was reporting a constant, plausible value. Fill time was
calculated by hand around that number. The tank overflowed, and the vapour cloud that ignited damaged around 300
homes and businesses up to 1.25 miles away.

At **Piper Alpha** in 1988, a permit-to-work sat suspended in a box. Nobody opened it. A competent night shift
inferred from the absence of any warning that the maintenance had not begun, started the standby condensate pump, and
condensate escaped from a flange where a pressure safety valve had been removed. 167 people died.

In both cases the absence of information was read as reassurance.

### What to do about it

**Two habits.** They are not difficult and they are the difference between a casebook that helps and one that
launders your assumptions.

**First: write claims that require positive acts, not absent warnings.**

```
# WRONG -- holds whenever nobody happened to say otherwise
claim work_complete "maintenance is finished"

# RIGHT -- holds only when somebody established it, and says what would establish it
claim work_status_unknown "nobody on this shift has established what was done"
psv_certificate_in_hand rebuts work_status_unknown
walkdown_completed      rebuts work_status_unknown
handover_briefing_done  rebuts work_status_unknown
```

The second form is longer and it is the one that would have stopped Piper Alpha. Default to not knowing, and let
positive acts change your mind.

**Second: reason about `<channel>_unavailable`.** If your casebook ignores it, a dead sensor behaves exactly like a
healthy quiet one. When the compiler warns that nothing reasons about it, either fix it or write down in the casebook
why the failure is conservative in your case — the two real casebooks here do one of each, and the reasoning is in
the file.

### The witness will tell you

Every explanation marks steps that rest on absence:

```
gauge_high (#2) defeated -- ASSUMED ABSENT: no sensor asserted it. This is absence of
  evidence, not evidence of absence; a dead sensor reads the same way.

17 steps, depth 6, of which 4 rest on ASSUMED ABSENCE of a reading
```

If a verdict you care about rests on four assumed absences, you now know what to go and check. That line is the
product.

---

## 7a. Severity: the one FMEA column that translates

An FMEA's risk priority number does not translate into a casebook, and it is worth being clear about why. RPN is
severity × occurrence × detection, and occurrence and detection are statements about a *population* of machines. The
device is attached to one machine. Multiplying them produces a number with no meaning for the thing in front of you.

**Severity is different.** It is a property of the failure itself, and it answers a question the framework genuinely
has to resolve: when the evidence cannot distinguish two hypotheses, which one should the machine act on?

```
claim cavitation      "impeller cavitation"     needs vibration_high suction_low
  severity 7
claim impeller_defect "impeller geometric wear" needs vibration_high blade_pass_harmonics
  severity 6

cavitation      rebuts impeller_defect
impeller_defect rebuts cavitation
```

Those two claims attack each other. Without severity, when both conditions hold neither can win, both are Undecided,
and the verdict is `Indeterminate` — honest, but it escalates to a human every single time evidence cannot separate
erosion from wear.

With severity declared, the less severe claim's attack is cancelled, cavitation survives, and the machine acts on the
worse of the two readings. **You cannot talk it out of a serious hypothesis using a mild one.**

Two things this does *not* do.

It does not touch `undercuts`. An undercut says the inference does not apply at all, and no amount of severity
outranks that.

It does not apply unless **both** claims have a severity. An undeclared severity is unknown, not zero — treating it as
the weakest possible value would silently strip attacks from every claim you had not got round to annotating.

Every cancelled attack is printed in the generation report. Silently deleting an attack you wrote would be
indefensible.

### What it measured

`pump_severity.cb` is the pump casebook with four severity annotations and nothing else changed, so the difference is
attributable to the annotations alone:

| | plain | with severity |
|---|---|---|
| Decided | 16,298 | 16,346 |
| Indeterminate | 86 | **38** |
| `continue` | 14,668 | **14,668** |
| `maintain` | 606 | 654 |
| `shutdown` | 1,024 | **1,024** |

48 of 86 ambiguous states resolved, **all 48 to `maintain`**. `continue` and `shutdown` are selected in exactly the
same states as before, and no state that was already decided changed its action.

That last row is the point. Any mechanism that makes a machine commit more often will reduce ambiguity, including a
dangerous one. What matters is the direction: severity made the machine neither more willing to keep running nor more
trigger-happy. It only broke ties, and it broke them towards the more serious failure.

---

## 8. Compile it

```sh
cbc mycasebook.cb --header mycasebook_generated.hpp
```

The compiler solves **every state of the entire input space** before writing anything. Read the report:

```
validation over all 16384 states
  Decided         16298    99.5%
  Indeterminate      86     0.5%
  Unsatisfied         0     0.0%
  Conflicted          0     0.0%
  worst-case work 140 steps

  ambiguity breakdown
    genuine diagnostic ambiguity       56
    instrument incoherence             30

  action selection over the whole input space
    keep_running              8192    50.0%
    reduce_load               6014    36.7%
    shut_down                 2178    13.3%
```

| what you see | what it means |
|---|---|
| `Conflicted` above zero | two actions at once. A defect, in the dangerous direction. Fix before anything else. |
| `Unsatisfied` above zero | some state rejects every action. Usually a missing `idle`, or conditions that cannot all be met. |
| `Indeterminate` | genuine ambiguity: your evidence cannot distinguish two explanations. Often a **finding**, not a bug — it is telling you which sensor you are missing. |
| an action at 0.0% | that response can never be selected. Usually a `prefer` chain that cannot let it through. This fails the build. |
| `genuine` vs `instrument incoherence` | "add a sensor" versus "fix a sensor". Different purchase orders. |

The `Indeterminate` states are the ones worth your afternoon. In the pump casebook here, exhaustive validation
surfaced an ambiguity between shaft misalignment and structural resonance that no hand-written scenario had touched —
6 states that genuinely cannot be resolved without a phase measurement the machine does not have. That is a real
instrumentation finding, produced by the compiler, from a casebook that looked finished.

---

## 9. A complete worked example

Below is a small but real casebook: a gearbox with two sensors and a maintenance-override path. It compiles, validates
over all 512 states, and is built as part of the test suite.

See [`example/gearbox.cb`](example/gearbox.cb). Its report:

```
lowered
  TOTAL arguments   30  (19 written, 11 generated = 36% plumbing)

validation over all 512 states
  Decided           512   100.0%
  worst-case work 86 steps

  action selection over the whole input space
    keep_running                 47     9.2%
    schedule_work                 5     1.0%
    reduce_load                 180    35.2%
    shut_down                   280    54.7%
```

Three things worth noticing.

**A third of the arguments are generated.** You wrote 19 lines of domain content; the compiler produced the gates, the
closed-world argument, the sensor-health observations, and the precedence edges.

**`schedule_work` is selected in 1% of states.** It is reachable, so it is not dead — but a response that narrow
usually means its condition is nearly always overridden by something above it in the `prefer` chain. That is a
question the report is asking you, not an error.

**`shut_down` at 55% looks alarming and is not.** Most of those 512 states include a dead or stuck vibration channel,
and this casebook treats a blind gearbox as a reason to stop. That is the conservative choice made visible: if you
disagree with it, the number tells you to go and argue with the casebook rather than discovering it in service.

---

## 10. Common mistakes, and what the compiler says

| you wrote | you get |
|---|---|
| `observ a "x"` | `unrecognised declaration 'observ' -- did you mean 'observe'?` |
| `claim c "x" when a b` | `ambiguous: write 'when any a b' ... or 'when all a b' ...` |
| `asserts hot above 90.0` | `threshold 'hot' needs a 'release' value: write 'asserts hot above <on> release <off>'` |
| `asserts hot above 90.0 release 95.0` | `...releases at 95, which is higher -- so once asserted it could never clear` |
| a channel with no `stale_after` | `...can never be reported dead, which defeats the purpose of declaring it` |
| `a undermines some_claim` | `...not an observation ... to attack a conclusion write 'rebuts' instead` |
| a claim requiring an action | `'shut_down' is an action, which cannot serve as evidence` |
| two actions marked `idle` | `action 'j' is also marked 'idle', but 'i' already is ... remove 'idle' from one of them` |

Every diagnostic names the line of the construct at fault and the symbol involved. If you ever get one that does not
tell you what to do, that is a bug in the compiler — `compiler/test/test_diagnostics.cpp` exists to keep that true,
and it asserts for 24 realistic mistakes that the message points at the right line, names the symbol, and says what
to write instead.

Writing this guide found three defects in the compiler, which is the usual outcome of writing documentation for
something. Two error messages pointed at the top of the file instead of the offending line, and the check for two
actions marked `idle` had never worked — it counted actions equal to a single stored index, so it could not exceed
one. The guide promised a diagnostic that did not exist, which is how it was noticed.

---

## 11. What this cannot do

Worth knowing before you plan around it.

- **No temporal reasoning.** A casebook cannot say "X happened before Y". All history lives in channel latches,
  staleness, and flatline windows.
- **RPN does not translate**, though severity does — see §7a. Occurrence and detection are population statistics and
  have no representation here.
- **30 observations per casebook.**
- **Grounded semantics only.** The device will sometimes say "I cannot decide" where a human would commit. That is
  deliberate: the alternatives have no bounded execution time and so cannot be a safety function.

## §10b Which pump casebook to read

There are two, and the difference matters.

`pump.cb` is a **frozen compiler-verification fixture**, not a model to copy. It is the fixed point of the equivalence
proof against `pump_casebook.hpp` — hand-written C++ from before the compiler existed — and the baseline of two controlled
experiments. Editing it would force editing the hand-written twin, which would turn *"the compiler reproduces independent
work"* into *"the compiler reproduces whatever was just changed to match it"*, destroying the only independence the proof has.
So it stays as written, defect included: every claim `needs` two symptoms, so it responds to **no single symptom**, and a
hot bearing on its own yields `continue` where an ordinary alarm system would shut the pump down.

**`pump_conditions.cb` is the one to read.** It is `pump.cb` plus the three condition claims that fix that, and it is a
better model precisely because the diff is the lesson.

`cbc` now prints every `note` in a casebook under `evidence`, and `pump.cb` carries this warning as a note — so anyone who
compiles it is told before they copy it.

## §10a Where these categories come from

The categories in this guide were, embarrassingly, discovered one incident at a time. `condition` was invented after an
alarm-system comparison showed `pump.cb` could not respond to a single symptom; "relies on absence" was invented after a
reachability check found an inert claim in the tank casebook. Both are established concepts with standard names.

[`TAXONOMY.md`](TAXONOMY.md) now lists them from the sources — ISO 13379-1's descriptor/symptom/failure-mode hierarchy,
ISO 14224's failure coding, RCM's consequence categories including hidden failures, and the Carneades
assumption-vs-exception distinction that turns out to be the formal name for §7. It ends with a checklist, and with an
explicit list of what cbc cannot check.

Read it before authoring. The point is that the next category you need should cost you a review rather than an incident.

## §11 Claims that do nothing

`cbc` reports three ways a claim you wrote can fail to matter. None of them stops the compile — the casebook behaves
correctly, and you may have a reason — but all three mean the casebook reads differently to a reviewer than it behaves.

```
  claim reach
    NEVER HOLDS     bearing_failing        no state at all makes this true
    ONLY IMPOSSIBLE xmtr_confirmed         holds in 12 states, every one excluded by the binding
    NO PATH         level_safe             cannot reach any action, so it can never change a decision
```

**NEVER HOLDS** means no combination of observations satisfies the claim. Usually a `needs` clause listing conditions
that cannot co-occur, often because an `exclusive` group forbids it.

**ONLY IMPOSSIBLE** is worse than dead, because it looks live. The claim holds only in states your own channel
declarations rule out — a transmitter that is unavailable and reporting a value at the same time, say. It will never fire
on real plant, and your safety argument silently rests on the binding layer being present.

**NO PATH** means the claim cannot reach any action through the attack graph, so no state in which it is true can change
what the machine does. This is the one to take seriously, and it is the one that was found in a real casebook here.

### The real example

`tank-409` had:

```
claim level_safe "the level is safe" when any xmtr_normal gauge_normal
no_measure  rebuts level_safe
level_high  rebuts level_safe
overfilling rebuts level_safe

action continue_transfer when inlet_flow          # <- level_safe appears nowhere
```

Three rebuttals *into* `level_safe` and nothing out of it. The claim was carefully defeasible and completely inert.

The net behaviour was correct, because `stop_transfer` outranks `continue_transfer` and covers every unsafe case. But
read what the casebook actually said: **keep filling whenever product is moving and nothing raised an objection.** The
difference between "nothing said stop" and "something said safe" is the entire subject of §7, and the reason the Piper
Alpha casebook exists. A reviewer seeing `level_safe` would reasonably believe the level was being checked before the
transfer continued. It was not.

The fix is one word:

```
action continue_transfer needs inlet_flow level_safe
```

Now the transfer continues only if an instrument positively says the level is normal. `continue_transfer` drops from
firing on flow alone to 54 of 7,776 reachable states, and the states where the level is simply unknown fall through to
`monitor` instead of being treated as permission.

### Why this check exists at all

A dangerous bug in the `vessel-hl-trip` casebook passed every one of pass 5's checks — 100% `Decided`, six green OKs.
It under-fired: a 2-out-of-3 voted trip that failed to notice when two legs had failed the *same* way, so the function
could silently become 1-out-of-1. No mechanical check recovers an author's intent, and none of these three would have
caught that specific bug. But the family is the same — logic that looks like it is doing something and is not — and
these three are checkable. Run them and read the output, even when nothing is red.
