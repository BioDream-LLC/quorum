# Results: four unseen regulations, five drafts each (2026-10-14)

Claude Sonnet 5.5 through OpenRouter, temperature 0, the studio's defaults, no second opinion. The code was frozen at
the commit that added the key (950d71d). 20 runs, about $7.20 of credit. Drafts and reviews are in `runs/`.

## Scores against the pre-registered key

| document | runs | checks per run | score | cbc accepted the draft as written |
|---|---|---|---|---|
| A gas turbine | 5 | 10 | 40 / 50 (8, 7, 9, 9, 7) | 4 of 5 |
| B steering alarms | 5 | 8 | 39 / 40 (8, 7, 8, 8, 8) | 0 of 5 |
| C boilers | 5 | 4 | 17 / 20 (3, 3, 3, 4, 4) | 4 of 5 |
| D mine CO warning | 5 | 5 | 20 / 25 (4, 3, 4, 5, 4) | 1 of 5 |
| **all** | 20 | | **116 / 135 (86%)** | 9 of 20 |

Two things about the scoring must be said.

- **One scorer bug was fixed after the runs.** `matters` (check C3) was implemented more strictly than the key
  defines it: it required the other readings to be at rest. Fixed to the key's own definition. Before the fix, C
  scored 13/20 and the total was 112/135.
- **One post-hoc reading, reported apart and not in the table.** In 3 drafts of A, the overspeed reading cites the
  heading "Automatic shutdown must occur under the following conditions:" instead of the two-word item "(i)
  Overspeed.". So the pre-registered rule (find a reading by the sentence it cites) cannot find it, and checks A1 and
  A9 fail. In all 3 the reading is named `overspeed` and does shut the turbine down. Matching by name as well
  (`SCORE_ALIAS="Overspeed=overspeed"`) gives A 46/50 and a total of 122/135 (90%). The table keeps the
  pre-registered number.

## What it got right and wrong

- **Shutdown versus alarm only, its weak spot before the prompt changes, held up on documents it had never seen.**
  - In A, the four alarm-only conditions are alarms in 20 of 20 cases. That includes "reduced lubricating oil
    pressure" (alarm) beside "low lubricating oil pressure" (shutdown), and "excessive speed" beside "overspeed".
  - The antifriction-bearing exception made low oil pressure alarm only in 5 of 5.
  - In B, no draft invented a shutdown for steering gear (5 of 5).
- **Missed: the remote manual shutdown device (A, 4 of 5).** A single sentence that names a device, not a condition.
- **Disputable: the flame safety system (C, 2 of 5 treat it as alarm only).** The section requires an alarm "when a
  flame safety system shutdown occurs", which can be read as the shutdown being done by the flame safeguard itself.
  The key says the rulebook should shut down; a reader could reasonably disagree.
- **Missed: the monitoring exception in D (4 of 5).** "operation of the belt may continue if ... a trained person
  must continuously monitor" became a monitoring action, not an exception that lets the belt keep running.
- **Too much in D.** The drafts declared 14 to 18 actions, one per duty in the regulation (notify, investigate,
  patrol...). Many could never be chosen, which is why cbc refused 4 of 5. A regulation about what people must do
  is not a good fit for a rulebook about what a machine must do, and the drafter does not say so.
- **Design rules drafted as run-time rules** (A, 3 of 5): exhaust and air-inlet location requirements became readings and alarms.
  The key does not score them. They are the kind of unsupported rule a person must delete.

## cbc's refusals, and the repair

All 5 steering drafts (B) were refused for one reason: nothing could argue with any claim. The section states no
exception, which is the case the studio's repair is for. After `quorum-studio fix-sensors`, all 5 pass cbc, and their
scores are unchanged (39/40). The other refusals were for unreachable actions (C 1, D 4) and two actions chosen at once
(A 1). Those are mistakes the person must fix, and the review says where.

## What this does and does not show

It is one model on four short documents, 20 runs, scored by a key we wrote. It is a fairer test than the earlier
ones, because the key and the code were fixed before any run. Still, the documents were chosen by the same people
who wrote the prompt, and a different reader would change some lines of the key. The 86% says the drafter is a
useful first pass on regulations of this kind. It does not say a draft can go to a machine without a person, and
nothing in quorum lets it.
