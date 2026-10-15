# A fair test of drafting from documents: four unseen regulations

Earlier drafting results were measured on documents the prompt was later tuned on. This test is meant to avoid
that. Four public-domain US federal regulations (eCFR, 2026-10-01; only markup removed):

| id | document | what it tests |
|---|---|---|
| A | 46 CFR 58.10-15, gas turbine installations | shutdown versus alarm only for near-identical conditions (low versus reduced oil pressure, overspeed versus excessive speed); a conditional exception; a manual shutdown |
| B | 46 CFR 58.25-25, steering gear alarms | alarms only, in two places; nothing may shut the steering down |
| C | 46 CFR 63.15-7 and 63.25-1, automatic auxiliary boilers | safety shutdowns that come with alarms; a start permissive (prepurge) |
| D | 30 CFR 75.1103-5, mine belt CO warning | warnings and responses, an operator-judgement exception, inoperative sensors with a monitoring exception |

Protocol, fixed before any run:
1. The answer key `key.tsv` and the scorer `tools/score-draft.py` are committed before any model sees the documents.
2. The prompt and the drafting code are frozen at that commit. Nothing is changed between runs.
3. Each document is drafted 5 times by `anthropic/claude-sonnet-5.5` through OpenRouter (zero-data-retention providers
   only), temperature 0, with the studio's defaults and no second opinion.
4. Every run is scored automatically, by the draft's behaviour rather than by its names. Results are reported as
   they come out, failures included, in `RESULTS.md`.

The key is our reading of each document. Someone else may read a clause differently: D's monitoring exception, for
example, or whether C's prepurge is a run-time rule at all. The key's lines say which clause each check comes from,
so a reader can disagree line by line.
