package draft

import (
	"context"
	"fmt"
	"strings"
	"time"
)

// Options for Run: where the model is and how hard to work at a draft.
type Options struct {
	Endpoint   string // already checked with Endpoint
	Model      string
	Rounds     int  // repair rounds after the first answer and after the sentence pass
	BySentence bool // ask about every sentence of the manual in turn
	// Check returns the compiler's messages for a draft (errors and failed whole-rulebook checks), which are sent
	// back to the model. Injected so this package need not run the compiler itself.
	Check func(source string) []string
	// SecondModel, when set, is asked to compare the finished draft with the manual and list disagreements; they go
	// back to the drafter for one more repair, and whatever the second reader still finds is put in the review. It may
	// be the drafting model itself (a different question) or another model on the same server.
	SecondModel string
}

// Outcome is everything a drafting run produced.
type Outcome struct {
	Result   Result
	Review   Review
	Raw      string    // every reply, concatenated, for the record
	Proposed int       // every item the model proposed, counting repeats
	Skipped  []string  // sentences never asked about because the server failed
	Second   []Finding // the second reader's last findings, if one was asked
	Asked2nd bool      // a second reader was asked and answered
}

// Run drafts a casebook from a manual: one whole-manual answer, repair rounds, then (optionally) one question per
// sentence and more repair rounds. Every answer goes through the gate; what the model says is never trusted. log
// receives one line per step, for a terminal or a progress panel. The command line and the studio both call this,
// so they draft identically.
func Run(ctx context.Context, manual string, meta Meta, o Options, log func(string)) (Outcome, error) {
	if log == nil {
		log = func(string) {}
	}
	check := o.Check
	if check == nil {
		check = func(string) []string { return nil }
	}
	var out Outcome
	t0 := time.Now()
	log(fmt.Sprintf("asking %s for a first draft...", o.Model))
	items, raw, err := Ask(ctx, o.Endpoint, o.Model, manual)
	if err != nil {
		return out, err
	}
	log(fmt.Sprintf("the model proposed %d items in %s", len(items), time.Since(t0).Round(time.Second)))
	out.Raw, out.Proposed = raw, len(items)
	r := Gate(manual, items, meta)

	repair := func(extra string) {
		for round := 1; round <= o.Rounds; round++ {
			if ctx.Err() != nil {
				return
			}
			rv := Assess(manual, r)
			errs := check(r.Source)
			errs = append(errs, unreachableHints(r, errs)...)
			errs = append(errs, defeaterHints(errs)...)
			if len(r.Rejected) == 0 && len(rv.Problems) == 0 && len(errs) == 0 && len(rv.Flags) == 0 && extra == "" {
				return
			}
			log(fmt.Sprintf("round %d: kept %d; sending back %d dropped items, %d draft problems, %d compiler messages, "+
				"%d flags, %d unused sentences...", round, len(r.Accepted), len(r.Rejected), len(rv.Problems), len(errs),
				len(rv.Flags), len(rv.Uncovered)))
			fixed, more, err := Repair(ctx, o.Endpoint, o.Model, manual, r, rv.Feedback(errs)+extra)
			extra = "" // the second reader's findings are sent once; later rounds fix what that repair broke
			if err != nil {
				log("repair failed, keeping what passed: " + err.Error())
				return
			}
			out.Raw += "\n" + more
			out.Proposed += len(fixed)
			r = Update(manual, r.Accepted, fixed, meta)
		}
	}
	repair("")
	if o.BySentence {
		sents := Sentences(manual)
		log(fmt.Sprintf("sentence pass: asking about each of %d sentences in turn...", len(sents)))
		for i, sent := range sents {
			if ctx.Err() != nil {
				out.Skipped = append(out.Skipped, sents[i:]...)
				break
			}
			t1 := time.Now()
			got, more, err := AskSentence(ctx, o.Endpoint, o.Model, manual, r.Accepted, sent)
			if err != nil {
				log(fmt.Sprintf("  sentence %d: %v (skipped)", i+1, err))
				out.Skipped = append(out.Skipped, sent)
				continue
			}
			out.Raw += "\n" + more
			out.Proposed += len(got)
			in, drift := Within(got, sent)
			before := len(r.Accepted)
			r = Update(manual, r.Accepted, in, meta)
			log(fmt.Sprintf("  %2d/%d  %d proposed, now %d kept (was %d), %d quoted another sentence  (%s)  %.50s", i+1,
				len(sents), len(got), len(r.Accepted), before, len(drift), time.Since(t1).Round(time.Second), sent))
			for _, x := range r.Rejected {
				log(fmt.Sprintf("         dropped %s %s: %s", x.Item.Kind, strings.TrimSpace(x.Item.Name+" "+x.Item.From),
					x.Reason))
			}
		}
		repair("") // the pass can leave whole-draft problems (a new claim no action uses)
	}
	if o.SecondModel != "" && ctx.Err() == nil {
		// Two readings: the first finds disagreements to send back, the second records what is still disputed.
		for pass := 1; pass <= 2 && ctx.Err() == nil; pass++ {
			log(fmt.Sprintf("second opinion: asking %s to compare the draft with the manual...", o.SecondModel))
			fs, more, err := CrossCheck(ctx, o.Endpoint, o.SecondModel, manual, r.Source)
			if err != nil {
				log("second opinion failed, keeping the draft as it is: " + err.Error())
				break
			}
			out.Raw += "\n" + more
			out.Second, out.Asked2nd = fs, true
			log(fmt.Sprintf("  the second reader found %d disagreements", len(fs)))
			for _, f := range fs {
				log(fmt.Sprintf("    %s %s: %.90s", strings.ToUpper(f.Verdict), f.Rule, f.Explain))
			}
			if len(fs) == 0 || pass == 2 {
				break
			}
			// The second reader's advice is followed only if it leaves the draft at least as sound as it was. On the
			// sump manual it once led the drafter to give two actions the same trigger, and cbc refused the result as
			// "inspect_instruments unreachable" (2026-10-14); a second opinion must not be able to break a draft.
			before, beforeBad := r, faults(manual, r, check)
			repair(FindingsFeedback(fs))
			if after := faults(manual, r, check); after > beforeBad {
				log(fmt.Sprintf("  following the second opinion made the draft fail %d more checks; kept the draft from "+
					"before it", after-beforeBad))
				r = before
			}
		}
	}
	out.Result = r
	out.Review = Assess(manual, r)
	out.Review.Incomplete(out.Skipped)
	out.Review.Second, out.Review.Asked2nd = out.Second, out.Asked2nd
	if ctx.Err() != nil {
		return out, ctx.Err()
	}
	log(fmt.Sprintf("kept %d items (the model proposed %d in all, counting repeats); every kept one quotes the manual "+
		"word for word", len(r.Accepted), out.Proposed))
	return out, nil
}

// Approve removes the unapproved-draft marker from a draft and records who approved it and when. It is what a
// person does after checking every rule against the manual; the studio asks them to confirm first.
func Approve(source, who string, date time.Time) string {
	var b strings.Builder
	for _, ln := range strings.SplitAfter(source, "\n") {
		t := strings.TrimSpace(ln)
		if strings.HasPrefix(t, "note ") && (strings.Contains(t, Marker) ||
			strings.Contains(t, "it came from. Check each against the manual") ||
			strings.Contains(t, "this note to approve. quorum studio will not build")) {
			continue
		}
		if strings.HasPrefix(t, "source ") && strings.Contains(t, "; not yet approved\"") {
			ln = strings.Replace(ln, "; not yet approved\"", fmt.Sprintf("; approved by %s on %s\"", esc(who),
				date.Format("2006-01-02")), 1)
		}
		b.WriteString(ln)
	}
	return b.String()
}

// faults counts what is wrong with a draft that the checks can see: compiler messages and whole-draft problems.
func faults(manual string, r Result, check func(string) []string) int {
	return len(check(r.Source)) + len(Assess(manual, r).Problems)
}

// unreachableHints explains a compiler "unreachable" failure in terms of the draft. The compiler says which action
// can never be chosen; it does not say why, and the model did not work it out from the bare message in two repair
// rounds (sump manual, 2026-10-14, twice: "report_maintenance when any failed_start mismatch" ranked above
// "inspect_instruments when any mismatch"). The usual cause is a higher-ranked action that fires on every trigger of
// the lower one; this names it.
func unreachableHints(r Result, errs []string) []string {
	var order []string
	when := map[string][]string{}
	for _, it := range r.Accepted {
		switch it.Kind {
		case "prefer":
			order = it.Order
		case "action":
			when[it.Name] = it.When
		}
	}
	rank := map[string]int{}
	for i, n := range order {
		rank[n] = i
	}
	var out []string
	for _, e := range errs {
		i := strings.Index(e, "unreachable: ")
		if i < 0 {
			continue
		}
		names := strings.FieldsFunc(e[i+len("unreachable: "):], func(c rune) bool { return c == ',' || c == ')' || c == ' ' })
		for _, x := range names {
			lo, ok := rank[x]
			if !ok || len(when[x]) == 0 {
				continue
			}
			for _, y := range order[:lo] {
				has := map[string]bool{}
				for _, t := range when[y] {
					has[t] = true
				}
				all := true
				for _, t := range when[x] {
					all = all && has[t]
				}
				if all {
					out = append(out, fmt.Sprintf("%s can never be chosen because %s is ranked above it and also fires "+
						"on %s. Unless the manual says %s should happen in that case, remove %s from %s's \"when\" "+
						"(return %s again with the corrected list).", x, y, strings.Join(when[x], " "), y,
						strings.Join(when[x], " "), y, y))
				}
			}
		}
	}
	return out
}

// defeaterHints answers the compiler's refusal of a rulebook in which no claim can be argued against. The refusal is
// deliberate (a pure rule list cannot tell a dead sensor from a quiet machine), but sent back bare it invited the model
// to find SOMETHING to argue with: on 46 CFR 112.50-1, a regulation that states no exception at all, two drafts of
// five passed only by turning a design requirement (no starting aid, except a jacket-water heater) into a run-time
// excuse (2026-10-14). The model is told not to invent one; the review tells the person what to decide.
func defeaterHints(errs []string) []string {
	for _, e := range errs {
		if strings.Contains(e, "at least one claim can be argued against") {
			return []string{"the compiler refuses a rulebook in which nothing can cancel any claim. Do NOT invent an " +
				"exception, a doubt or a reading to satisfy it, and do not turn a design or installation requirement " +
				"into one. If the manual states no exception or instrument check for a running machine, leave the " +
				"draft as it is: a person will decide what may cancel each claim."}
		}
	}
	return nil
}
