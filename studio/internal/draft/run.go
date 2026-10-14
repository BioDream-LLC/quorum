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
}

// Outcome is everything a drafting run produced.
type Outcome struct {
	Result   Result
	Review   Review
	Raw      string   // every reply, concatenated, for the record
	Proposed int      // every item the model proposed, counting repeats
	Skipped  []string // sentences never asked about because the server failed
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

	repair := func() {
		for round := 1; round <= o.Rounds; round++ {
			if ctx.Err() != nil {
				return
			}
			rv := Assess(manual, r)
			errs := check(r.Source)
			if len(r.Rejected) == 0 && len(rv.Problems) == 0 && len(errs) == 0 && len(rv.Flags) == 0 {
				return
			}
			log(fmt.Sprintf("round %d: kept %d; sending back %d dropped items, %d draft problems, %d compiler messages, "+
				"%d flags, %d unused sentences...", round, len(r.Accepted), len(r.Rejected), len(rv.Problems), len(errs),
				len(rv.Flags), len(rv.Uncovered)))
			fixed, more, err := Repair(ctx, o.Endpoint, o.Model, manual, r, rv.Feedback(errs))
			if err != nil {
				log("repair failed, keeping what passed: " + err.Error())
				return
			}
			out.Raw += "\n" + more
			out.Proposed += len(fixed)
			r = Update(manual, r.Accepted, fixed, meta)
		}
	}
	repair()
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
		repair() // the pass can leave whole-draft problems (a new claim no action uses)
	}
	out.Result = r
	out.Review = Assess(manual, r)
	out.Review.Incomplete(out.Skipped)
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
