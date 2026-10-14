package draft

import (
	"context"
	"fmt"
	"strings"
)

// A Finding is one disagreement a second reader found between the manual and a draft.
type Finding struct {
	Verdict string `json:"verdict"` // missing, wrong or unsupported
	Rule    string `json:"rule"`    // the rulebook name it concerns, if any
	Quote   string `json:"quote"`   // the manual's words, verbatim
	Explain string `json:"explain"`
}

// CrossCheckPrompt asks a second reader -- another model, or the same one with a different job -- to compare a
// finished draft with the manual. The drafter is asked "what rules does this manual state?"; this asks "where does
// this rulebook disagree with the manual?". The two questions fail differently: on the Army Corps standard both
// Claude drafts made an alarm-only condition a shutdown and dropped a start permissive (2026-10-11), mistakes a reader
// holding the finished rulebook against the text is placed to see. The answer reuses the item schema (kind = verdict,
// name = rule, description = explanation), so it is parsed and its quotes checked exactly like a draft's.
func CrossCheckPrompt(manual, source string) string {
	return `You are checking a rulebook that someone else drafted from an equipment manual. Compare it with the MANUAL,
requirement by requirement, and report only real disagreements.

How to read the rulebook: "observe" is a reading; a "claim" holds when it "needs" ALL the listed readings or claims;
an "excuse" or a reading that "rebuts"/"undercuts" a claim cancels it; an action is taken "when any" of its listed
claims holds; "prefer A over B" means A wins when both apply; the action marked "safe" is the shutdown or safe state
and the one marked "idle" is carrying on normally. Lines starting with # quote the manual sentence each rule came from.

For every requirement the manual states for a running machine, check that the rulebook does exactly what the sentence
says: the right action (shut down, raise an alarm only, hold a start, carry on), for the right conditions, with "or"
and "and" as written, and exceptions that excuse only what the manual says they excuse.

Return JSON only: {"items":[{"kind":"<verdict>","name":"<rule name, or empty>","quote":"<manual text>","description":"<one sentence>"}]}
Verdicts:
- "missing": the manual states it and the rulebook does not do it at all.
- "wrong": the rulebook does it differently from the manual -- for example a shutdown where the manual asks only for
  an alarm, a start that is not held when the manual says it must wait, "and" where the manual says "or", or an
  exception that also cancels shutdowns the manual did not except.
- "unsupported": a rule the manual does not state.
Rules for your answer:
- "quote" is required: text copied EXACTLY, character for character, from the MANUAL (not from the rulebook) -- the
  words that show the disagreement. For "unsupported", quote the sentence the rule claims to come from.
- Design guidance a running machine cannot act on (what equipment to fit, where to mount it, sizes, batteries) is not
  a missing rule.
- Say what is wrong in plain words in "description", and what the manual asks for instead.
- If the rulebook does what the manual says, return {"items":[]}.

MANUAL:
` + manual + `

RULEBOOK:
` + source + "\n"
}

// CrossCheck runs the second reading. Findings whose quote is not in the manual are dropped -- a second opinion is
// held to the same standard as the draft, or it becomes a new way to invent rules -- as are unknown verdicts.
func CrossCheck(ctx context.Context, endpoint, model, manual, source string) ([]Finding, string, error) {
	items, raw, err := generate(ctx, endpoint, model, CrossCheckPrompt(manual, source))
	if err != nil {
		return nil, raw, err
	}
	nm := normalise(manual)
	var out []Finding
	for _, it := range items {
		v := strings.ToLower(strings.TrimSpace(it.Kind))
		q := normalise(it.Quote)
		if (v != "missing" && v != "wrong" && v != "unsupported") || len(q) < 12 || !strings.Contains(nm, q) {
			continue
		}
		out = append(out, Finding{Verdict: v, Rule: strings.TrimSpace(it.Name), Quote: strings.TrimSpace(it.Quote),
			Explain: strings.TrimSpace(it.Description)})
	}
	return out, raw, nil
}

// FindingsFeedback is how the second reader's findings are handed back to the drafter in a repair round.
func FindingsFeedback(fs []Finding) string {
	if len(fs) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString("A SECOND READER compared your rulebook with the manual and found these disagreements. Each may be " +
		"right or wrong; fix the ones the manual supports, and leave a rule unchanged if the manual supports it:\n")
	for _, f := range fs {
		rule := ""
		if f.Rule != "" {
			rule = " (" + f.Rule + ")"
		}
		fmt.Fprintf(&b, "- %s%s: %s\n  manual: %q\n", strings.ToUpper(f.Verdict), rule, f.Explain, f.Quote)
	}
	return b.String()
}
