package draft

// The review: what the gate cannot decide, laid out for the person who must.
//
// The gate proves every kept rule cites real text. It cannot prove the rule follows from that text, or that the manual's
// other rules were not left out -- the two ways the 14B model's draft of the sump-pump manual was wrong while passing
// every check. This file finds evidence of both, deterministically, so the reviewer starts from a list rather than from
// a blank page:
//
//   - Problems: the draft as a whole cannot work (no idle or safe action, an action missing from the preference order,
//     a claim no action or attack uses). These go back to the model in a repair round.
//   - Flags: a kept rule names something its own quote never mentions. "flow_on undercuts overheating", quoting a
//     sentence about flow and current, is a real quote attached to an invented inference; the quote has no word for
//     overheating. Matching uses only DISTINCTIVE words (in at most a third of its sentences, lightly stemmed), because "pump" and
//     "motor" are in every sentence and would vouch for anything. A flag is a question, not a verdict.
//   - Uncovered: manual sentences no kept rule cites. Not every sentence states a rule, and a missing rule can hide in
//     a sentence cited for a different one (seen 2026-10-10: the weekly-test sentences, cited for an attack, also
//     state an excuse the draft lacked). Citation is not coverage.

import (
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strings"
)

// Headings: "3. Faults", "8-2 b. Engine control equipment", "9-8. Pump bearing temperature system".
var headingRe = regexp.MustCompile(`^(\d+([.-]\d+)*\.?\s+([a-z]\.\s+)?)?[A-Z][A-Za-z ,]{0,50}$`)

// exceptionRe finds sentences that state an exception: a condition that must NOT be treated as a fault or must NOT
// cause a trip. Such a sentence needs an excuse (or the absence of the trip), and an attack citing it is not enough:
// in every 14B draft of the sump-pump manual the weekly-test sentences were cited by an attack while the weekly-test
// excuse they state was missing, and nothing said so.
var orRe = regexp.MustCompile(`\bor\b`)

var exceptionRe = regexp.MustCompile(`\b(is not a fault|not a fault|is expected|is normal|should not (occur|trip|shut|cause|stop)|must not (occur|trip|shut|cause|stop)|is not an? (alarm|emergency|failure))\b`)

// Flag is a kept rule the reviewer should look at first.
type Flag struct {
	Item   Item   `json:"item"`
	Reason string `json:"reason"`
}

// Review is the reviewer's starting point.
type Review struct {
	Problems  []string `json:"problems"`
	Flags     []Flag   `json:"flags"`
	Uncovered []string `json:"uncovered"`
	Sentences int      `json:"sentences"`
	Skipped   []string `json:"skipped"` // sentences the model was never asked about because the server failed
	// Cautions are not errors: each names an excuse or reading that can stop the SAFE action from happening. Some
	// are exactly what the manual says (the weekly test excuses a dry run); some are not (seen 2026-10-11, Claude:
	// "minor power fluctuations should not cause a shutdown" drafted as excusing every oil and temperature shutdown,
	// so a power blip during a real overheat would keep the engine running). Only a person can tell which.
	Cautions []string `json:"cautions"`
}

// Incomplete records sentences the sentence pass could not ask about. A draft with any is not the draft the process
// describes, and the review says so first.
func (rv *Review) Incomplete(skipped []string) { rv.Skipped = append(rv.Skipped, skipped...) }

var stop = map[string]bool{}

func init() {
	for _, w := range strings.Fields(`a an the and or not no is are be been was were it its this that these those of in on at
		to for from by with as if when while then than so but any all each every one two up down off out into over under
		may must should can cannot will would has have had do does did which who what there their them they he she
		only also other same such some more most less very per`) {
		stop[w] = true
	}
}

// stem strips the commonest English endings, enough to make "overheated" and "overheating" meet.
func stem(w string) string {
	for _, suf := range []string{"ing", "ed", "es", "s"} {
		if len(w) > len(suf)+3 && strings.HasSuffix(w, suf) {
			return strings.TrimSuffix(w, suf)
		}
	}
	return w
}

func wordsOf(s string) []string {
	s = strings.NewReplacer("_", " ", "-", " ", "/", " ").Replace(normalise(s))
	var out []string
	for _, w := range strings.Fields(s) {
		w = strings.Trim(w, ".,;:()'\"!?")
		if len(w) < 3 || stop[w] {
			continue
		}
		out = append(out, stem(w))
	}
	return out
}

// sentences splits a manual into sentences, skipping headings and lines without a full stop.
func sentences(manual string) []string {
	var out []string
	for _, para := range strings.Split(strings.ReplaceAll(manual, "\r", ""), "\n\n") {
		// Drop numbered headings ("3. Faults") before flattening, or they run into the sentence that follows.
		var lines []string
		for _, l := range strings.Split(para, "\n") {
			if !headingRe.MatchString(strings.TrimSpace(l)) {
				lines = append(lines, l)
			}
		}
		flat := strings.TrimSpace(spaceRe.ReplaceAllString(strings.Join(lines, " "), " "))
		for _, s := range sentenceRe.FindAllString(flat, -1) {
			s = strings.TrimSpace(s)
			if len(wordsOf(s)) >= 4 {
				out = append(out, s)
			}
		}
	}
	return out
}

// Assess reviews a gated draft against its manual.
func Assess(manual string, r Result) Review {
	var rv Review
	sents := sentences(manual)
	rv.Sentences = len(sents)

	// Distinctive words: in at most a third of the sentences (and at least two sentences' worth of manual).
	df := map[string]int{}
	for _, s := range sents {
		seen := map[string]bool{}
		for _, w := range wordsOf(s) {
			if !seen[w] {
				df[w]++
				seen[w] = true
			}
		}
	}
	limit := len(sents) / 3
	if limit < 2 {
		limit = 2
	}
	distinctive := func(s string) map[string]bool {
		m := map[string]bool{}
		for _, w := range wordsOf(s) {
			if df[w] > 0 && df[w] <= limit {
				m[w] = true
			}
		}
		return m
	}

	byName := map[string]Item{}
	for _, it := range r.Accepted {
		if it.Name != "" {
			byName[it.Name] = it
		}
	}
	mentions := func(quote, name string) bool {
		it, ok := byName[name]
		desc := name
		if ok {
			desc += " " + it.Description
		}
		want := distinctive(desc)
		if len(want) == 0 {
			return true // nothing distinctive to look for: cannot tell, so no flag
		}
		have := distinctive(quote)
		for w := range want {
			if have[w] {
				return true
			}
		}
		return false
	}
	flag := func(it Item, names ...string) {
		var missing []string
		for _, n := range names {
			if !mentions(it.Quote, n) {
				missing = append(missing, n)
			}
		}
		if len(missing) > 0 {
			rv.Flags = append(rv.Flags, Flag{it, fmt.Sprintf("its quote never mentions %s: either the manual does "+
				"not say this, or it says it in another sentence and this is the wrong citation",
				strings.Join(missing, ", "))})
		}
	}

	// Problems with the draft as a whole.
	var idle, safe []string
	actions := map[string]bool{}
	used := map[string]bool{}
	ranked := map[string]bool{}
	for _, it := range r.Accepted {
		switch it.Kind {
		case "action":
			actions[it.Name] = true
			if it.Role == "idle" {
				idle = append(idle, it.Name)
			}
			if it.Role == "safe" {
				safe = append(safe, it.Name)
			}
		case "prefer":
			for _, n := range it.Order {
				ranked[n] = true
			}
		case "attack":
			used[it.From] = true
			for _, t := range it.To {
				used[t] = true
			}
		}
		for _, n := range append(append(append([]string(nil), it.Needs...), it.When...), it.Undermines...) {
			used[n] = true
		}
	}
	switch {
	case len(idle) == 0:
		rv.Problems = append(rv.Problems, `no action has role "idle" (what to do when nothing is wrong)`)
	case len(idle) > 1:
		rv.Problems = append(rv.Problems, "more than one idle action: "+strings.Join(idle, ", "))
	}
	switch {
	case len(safe) == 0:
		rv.Problems = append(rv.Problems, `no action has role "safe" (the safe state)`)
	case len(safe) > 1:
		rv.Problems = append(rv.Problems, "more than one safe action: "+strings.Join(safe, ", "))
	}
	var unranked []string
	for a := range actions {
		if len(actions) > 1 && !ranked[a] {
			unranked = append(unranked, a)
		}
	}
	sort.Strings(unranked)
	if len(unranked) > 0 {
		rv.Problems = append(rv.Problems, "actions missing from the prefer order: "+strings.Join(unranked, ", "))
	}
	for _, it := range r.Accepted {
		if it.Kind == "observe" && !used[it.Name] {
			rv.Problems = append(rv.Problems, fmt.Sprintf("observe %s: no rule uses this reading", it.Name))
		}
		if (it.Kind == "claim" || it.Kind == "excuse") && !used[it.Name] {
			what := "no action or attack uses it, so it can never change a decision"
			if it.Kind == "excuse" {
				what = "it attacks nothing, so it can never excuse anything"
			}
			rv.Problems = append(rv.Problems, fmt.Sprintf("%s %s: %s", it.Kind, it.Name, what))
		}
	}

	// "needs" means ALL of them. A rule needing several readings whose quote lists them with "or" fires only when
	// every one is present at once (seen 2026-10-11, Llama 70B: "shutdown should occur for high jacket water
	// temperature, high lubricating oil temperature, low lubricating oil pressure ... or ..." drafted as one claim
	// needing all five, so a single fault would never shut the engine down).
	for _, it := range r.Accepted {
		if len(it.Needs) >= 2 && orRe.MatchString(normalise(it.Quote)) {
			rv.Problems = append(rv.Problems, fmt.Sprintf("%s %s needs all of %s at once, but its quote says \"or\": "+
				"if any one of them is enough, make one rule per reading, or use when (any)", it.Kind, it.Name,
				strings.Join(it.Needs, ", ")))
		}
	}

	// Cautions: what can switch the safe action off.
	safeWhen := map[string]bool{}
	safeName := ""
	for _, it := range r.Accepted {
		if it.Kind == "action" && it.Role == "safe" {
			safeName = it.Name
			for _, w := range it.When {
				safeWhen[w] = true
			}
		}
	}
	for _, it := range r.Accepted {
		if it.Kind != "attack" {
			continue
		}
		var hit []string
		for _, t := range it.To {
			if safeWhen[t] {
				hit = append(hit, t)
			}
		}
		if len(hit) > 0 {
			rv.Cautions = append(rv.Cautions, fmt.Sprintf("%s can stop %s (the safe action) for %s. Check that the manual "+
				"means this, in every case listed: %q", it.From, safeName, strings.Join(hit, ", "), it.Quote))
		}
	}

	// Flags: each kept rule against its own quote.
	for _, it := range r.Accepted {
		switch it.Kind {
		case "attack":
			flag(it, append([]string{it.From}, it.To...)...)
		case "claim", "excuse", "doubt":
			// The rule's own name, and what it depends on: "instrument_faulty needs flow_on thermal_off", quoting a
			// sentence about flow and current, has the right subject and an invented condition (seen 2026-10-10).
			flag(it, append(append(append([]string{it.Name}, it.Needs...), it.When...), it.Undermines...)...)
		case "implies":
			flag(it, append(append([]string(nil), it.Needs...), it.To...)...)
		}
	}

	// A condition the quote states and the rule ignores: "if the float switch is closed and the current relay is NOT
	// closed, the pump has failed to start", drafted as `failed_start needs high_water_on` with nothing from current_on,
	// fires whenever the water is high -- including while the pump runs normally (seen in every 14B draft, 2026-10-10).
	attackedBy := map[string]map[string]bool{}
	for _, it := range r.Accepted {
		if it.Kind == "attack" {
			for _, t := range it.To {
				if attackedBy[t] == nil {
					attackedBy[t] = map[string]bool{}
				}
				attackedBy[t][it.From] = true
			}
		}
	}
	for _, it := range r.Accepted {
		if it.Kind != "claim" && it.Kind != "excuse" && it.Kind != "doubt" {
			continue
		}
		involved := map[string]bool{}
		for _, n := range append(append(append([]string(nil), it.Needs...), it.When...), it.Undermines...) {
			involved[n] = true
		}
		// Words the rule already accounts for: those of the readings it uses or is attacked by. A second reading
		// about the same instrument (current_off beside current_on) is then not reported as ignored.
		explained := map[string]bool{}
		for _, o := range r.Accepted {
			if o.Kind == "observe" && (involved[o.Name] || attackedBy[it.Name][o.Name]) {
				for w := range distinctive(o.Name + " " + o.Description) {
					explained[w] = true
				}
			}
		}
		have := distinctive(it.Quote)
		var ignored []string
		for _, o := range r.Accepted {
			if o.Kind != "observe" || involved[o.Name] || attackedBy[it.Name][o.Name] {
				continue
			}
			want := distinctive(o.Name + " " + o.Description)
			hits := 0
			for w := range want {
				if have[w] && !explained[w] {
					hits++
				}
			}
			if hits >= 2 || (hits == 1 && len(want) == 1) {
				ignored = append(ignored, o.Name)
			}
		}
		if len(ignored) > 0 {
			sort.Strings(ignored)
			rv.Flags = append(rv.Flags, Flag{it, fmt.Sprintf("its quote mentions %s, but the rule neither needs it nor "+
				"is argued against by it: a condition in the sentence (often a 'not') may be missing",
				strings.Join(ignored, ", "))})
		}
	}

	// Readings the manual never mentions. The distinctive-word flags cannot see these (a word in no sentence is
	// in no quote either, so there is nothing to compare), and a model copies names from the prompt's examples:
	// seen 2026-10-11, a pumping-station draft excused power fluctuations with a "weekly_test" reading.
	for _, it := range r.Accepted {
		if it.Kind != "observe" {
			continue
		}
		found, any := false, false
		for _, w := range wordsOf(it.Name + " " + it.Description) {
			if stop[w] || len(w) < 4 {
				continue
			}
			any = true
			if df[w] > 0 {
				found = true
				break
			}
		}
		if any && !found {
			rv.Problems = append(rv.Problems, fmt.Sprintf("observe %s: no word of %q appears anywhere in the manual, "+
				"so the manual does not mention this reading", it.Name, it.Description))
		}
	}

	// Exceptions: a sentence stating one must be cited by an excuse.
	for _, s := range sents {
		if !exceptionRe.MatchString(normalise(s)) {
			continue
		}
		ns, excused := normalise(s), false
		for _, it := range r.Accepted {
			if q := normalise(it.Quote); it.Kind == "excuse" && len(q) >= 12 && (strings.Contains(ns, q) || strings.Contains(q, ns)) {
				excused = true
				break
			}
		}
		if !excused {
			rv.Problems = append(rv.Problems, fmt.Sprintf("the manual states an exception, and no excuse expresses it: %q. "+
				"An exception that depends on an activity or mode (a test, maintenance, start-up) needs a reading for "+
				"that mode, even if the manual names no instrument for it", s))
		}
	}

	// Coverage: sentences no kept rule cites.
	var cited []string
	for _, it := range r.Accepted {
		cited = append(cited, normalise(it.Quote))
	}
	for _, s := range sents {
		ns := normalise(s)
		hit := false
		for _, q := range cited {
			if len(q) >= 12 && (strings.Contains(ns, q) || strings.Contains(q, ns)) {
				hit = true
				break
			}
		}
		if !hit {
			rv.Uncovered = append(rv.Uncovered, s)
		}
	}
	return rv
}

// Text renders the review for a person.
func (rv Review) Text() string {
	var b strings.Builder
	b.WriteString("REVIEW -- what the checks cannot decide. Start here.\n\n")
	if len(rv.Skipped) > 0 {
		b.WriteString(fmt.Sprintf("INCOMPLETE: the model server failed and %d sentences were never asked about. This draft "+
			"is not\nthe result of the full process; run it again.\n", len(rv.Skipped)))
		for _, s := range rv.Skipped {
			b.WriteString("  - " + s + "\n")
		}
		b.WriteString("\n")
	}
	if len(rv.Problems) == 0 {
		b.WriteString("draft as a whole: no structural problems found\n")
	} else {
		b.WriteString("draft as a whole:\n")
		for _, p := range rv.Problems {
			b.WriteString("  PROBLEM  " + p + "\n")
		}
	}
	if len(rv.Cautions) > 0 {
		b.WriteString("\nwhat can stop the safe action (each may be right; a person must decide):\n")
		for _, c := range rv.Cautions {
			b.WriteString("  CAUTION  " + c + "\n")
		}
	}
	b.WriteString("\nrules to check first (a real quote does not mean a real rule):\n")
	if len(rv.Flags) == 0 {
		b.WriteString("  none flagged -- which is not the same as all correct\n")
	}
	for _, f := range rv.Flags {
		b.WriteString(fmt.Sprintf("  FLAG     %s %s\n           %s\n           quote: %q\n", f.Item.Kind, label(f.Item),
			f.Reason, f.Item.Quote))
	}
	b.WriteString(fmt.Sprintf("\nmanual sentences no rule cites (%d of %d). A missing rule is usually in here, but not\n"+
		"always: a sentence cited for one rule can state another that is still missing.\n", len(rv.Uncovered), rv.Sentences))
	for _, s := range rv.Uncovered {
		b.WriteString("  - " + s + "\n")
	}
	return b.String()
}

func label(it Item) string {
	switch it.Kind {
	case "attack":
		return it.From + " " + it.Attack + " " + strings.Join(it.To, " ")
	case "implies":
		return strings.Join(it.Needs, " ") + " -> " + strings.Join(it.To, " ")
	case "prefer":
		return strings.Join(it.Order, " over ")
	}
	return it.Name
}

// Feedback is what a repair round tells the model beyond the per-item rejections.
func (rv Review) Feedback(cbcErrors []string) string {
	var b strings.Builder
	for _, p := range rv.Problems {
		b.WriteString("- PROBLEM: " + p + "\n")
	}
	for _, e := range cbcErrors {
		b.WriteString("- COMPILER: " + e + "\n")
	}
	for _, f := range rv.Flags {
		j, _ := json.Marshal(f.Item)
		b.WriteString("- QUESTION about " + string(j) + ": " + f.Reason + ". Return a corrected version (same name) " +
			"if it is wrong, for example add an attack from the ignored reading.\n")
	}
	if len(rv.Uncovered) > 0 {
		b.WriteString("- These manual sentences are not yet used by any item. If one states a fault, an exception, an " +
			"instrument check or an action, add items for it:\n")
		for _, s := range rv.Uncovered {
			b.WriteString("    \"" + s + "\"\n")
		}
	}
	return b.String()
}
