// Package draft turns a maintenance manual into a DRAFT rulebook, using a language model that runs on this computer,
// and refuses every rule it cannot trace to a sentence in the manual.
//
// The model is the least trusted part of quorum, so it is given the least power. It proposes rules as JSON; each must
// carry a verbatim quote from the manual. The gate below then does everything that matters, deterministically:
//
//   - a quote that does not appear in the manual (after normalising whitespace, case and typographic punctuation) is
//     rejected, because a rule nobody can trace to the source is exactly what a language model invents;
//   - a name that is not a plain identifier, or collides with what the compiler generates, is rejected;
//   - a rule that refers to a name nobody defined, or to a rejected rule, is rejected, repeatedly until nothing changes;
//   - what survives is written as a casebook with each line preceded by the sentence it came from, and marked
//     UNAPPROVED. quorum studio refuses to build a chip image while that mark is present: deleting it is the human's act
//     of approval, after which the draft must still pass every check cbc makes, like any other rulebook.
//
// The manual never leaves the machine: by default only a loopback endpoint is accepted.
package draft

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Marker is the line whose presence means "not yet approved". Studio refuses to build an image while it is present.
const Marker = "UNAPPROVED DRAFT"

// Item is one proposed declaration.
type Item struct {
	Kind        string   `json:"kind"` // observe, claim, excuse, doubt, attack, implies, action, prefer
	Name        string   `json:"name,omitempty"`
	Description string   `json:"description,omitempty"`
	Needs       []string `json:"needs,omitempty"`      // all required
	When        []string `json:"when,omitempty"`       // any suffices
	Undermines  []string `json:"undermines,omitempty"` // doubts: readings to distrust
	Severity    int      `json:"severity,omitempty"`   // claims: 1..10
	From        string   `json:"from,omitempty"`       // attack: attacker
	Attack      string   `json:"attack,omitempty"`     // attack: rebuts or undercuts
	To          []string `json:"to,omitempty"`         // attack: targets; implies: the required reading
	Role        string   `json:"role,omitempty"`       // action: idle, safe, or empty
	Order       []string `json:"order,omitempty"`      // prefer: strongest first
	Quote       string   `json:"quote"`
}

// Rejection says why an item was dropped.
type Rejection struct {
	Item   Item   `json:"item"`
	Reason string `json:"reason"`
}

// Result is the gated draft.
type Result struct {
	Source   string      `json:"source"`   // the casebook, marked UNAPPROVED
	Accepted []Item      `json:"accepted"` // in declaration order
	Rejected []Rejection `json:"rejected"`
}

// Meta is what the casebook header records about where the draft came from.
type Meta struct {
	Casebook string // casebook name
	Manual   string // file name of the manual
	Model    string // the model that proposed the rules
	Date     string // YYYY-MM-DD
}

var (
	identRe = regexp.MustCompile(`^[a-z][a-z0-9_]{0,47}$`)
	spaceRe = regexp.MustCompile(`\s+`)
	typo    = strings.NewReplacer("\u2018", "'", "\u2019", "'", "\u201c", `"`, "\u201d", `"`, "\u2013", "-", "\u2014", "-",
		"\u00a0", " ")
)

func normalise(s string) string {
	return strings.TrimSpace(spaceRe.ReplaceAllString(strings.ToLower(typo.Replace(s)), " "))
}

func reserved(n string) bool {
	return strings.HasPrefix(n, "gate_no_") || strings.HasPrefix(n, "claim_") || strings.HasPrefix(n, "mismatch__") ||
		strings.HasPrefix(n, "incoherent") || n == "closed_world"
}

// Gate checks every item against the manual and writes the surviving ones as a casebook.
func Gate(manual string, items []Item, m Meta) Result {
	text := normalise(manual)
	var r Result
	reject := func(it Item, why string) { r.Rejected = append(r.Rejected, Rejection{it, why}) }

	// 1. Traceability and well-formedness, item by item.
	var alive []Item
	defined := map[string]string{} // name -> kind
	roles := map[string]string{}   // idle/safe -> the action holding it
	seen := map[string]bool{}      // unnamed items (attack, implies, prefer) by meaning, so a repeat is not written twice
	for _, it := range items {
		it.Kind = strings.ToLower(strings.TrimSpace(it.Kind))
		if it.Name == "" {
			key := it.Kind + "|" + it.From + "|" + it.Attack + "|" + strings.Join(it.To, ",") + "|" +
				strings.Join(it.Needs, ",") + "|" + strings.Join(it.Order, ",")
			if seen[key] {
				reject(it, "the same rule is already in the draft")
				continue
			}
			seen[key] = true
		}
		q := normalise(it.Quote)
		switch {
		case len(q) < 12:
			reject(it, "no quote, or a quote too short to identify a sentence")
			continue
		case !strings.Contains(text, q):
			reject(it, "the quote does not appear in the manual")
			continue
		}
		switch it.Kind {
		case "observe", "claim", "excuse", "doubt", "action":
			if !identRe.MatchString(it.Name) || reserved(it.Name) {
				reject(it, fmt.Sprintf("%q is not a usable name (lower case letters, digits and _, not reserved)", it.Name))
				continue
			}
			if _, dup := defined[it.Name]; dup {
				reject(it, "the name is already defined")
				continue
			}
			// Only idle and safe mean anything. Models invent others ("urgent", "caution", "investigate", seen
			// 2026-10-10); refusing the action for that lost real actions, so an invented role is simply dropped.
			if it.Kind == "action" {
				it.Role = strings.ToLower(strings.TrimSpace(it.Role))
				if it.Role != "idle" && it.Role != "safe" {
					it.Role = ""
				}
			}
			// One idle action and one safe action: the first to claim a role keeps it. Without this the model marked
			// three actions safe in one run (2026-10-10), and every repair added another.
			if it.Kind == "action" && it.Role != "" {
				if holder, ok := roles[it.Role]; ok && holder != it.Name {
					reject(it, fmt.Sprintf("%s is already the %s action; an asset has exactly one. Give this action no "+
						"role, or return %s without it first", holder, it.Role, holder))
					continue
				}
				roles[it.Role] = it.Name
			}
			// A non-idle action with no reason is always justified, so it outranks and hides every action below it
			// (seen 2026-10-11, Claude: "action shutoff_intake_air" with no trigger made the shutdown unreachable).
			if it.Kind == "action" && it.Role == "" && len(it.When) == 0 && len(it.Needs) == 0 {
				reject(it, "an action other than the idle or safe one needs a reason: say when it applies (when ...)")
				continue
			}
			// A claim with no evidence always holds (seen 2026-10-11: "engine overspeeding" with no reading).
			if it.Kind == "claim" && len(it.Needs) == 0 && len(it.When) == 0 {
				reject(it, "a claim needs evidence: say which reading or readings it needs")
				continue
			}
			if it.Kind == "claim" && (it.Severity < 0 || it.Severity > 10) {
				reject(it, "severity must be 1..10")
				continue
			}
			if it.Kind != "action" && strings.TrimSpace(it.Description) == "" {
				reject(it, "a description is required: say in a few words what this means")
				continue
			}
			if len(it.Undermines) > 0 && it.Kind != "doubt" {
				reject(it, "only a doubt may have undermines; to argue against a claim, use an attack with rebuts")
				continue
			}
			if it.Kind == "doubt" && len(it.Undermines) == 0 {
				reject(it, "a doubt must say which readings it undermines")
				continue
			}
			defined[it.Name] = it.Kind
		case "attack":
			if it.Attack != "rebuts" && it.Attack != "undercuts" {
				reject(it, "an attack must be rebuts or undercuts")
				continue
			}
			if len(it.To) == 0 {
				reject(it, "an attack needs at least one target in to")
				continue
			}
			for _, t := range it.To {
				if t == it.From {
					reject(it, "a rule cannot attack itself")
					break
				}
			}
			if r.Rejected != nil && r.Rejected[len(r.Rejected)-1].Item.Quote == it.Quote &&
				r.Rejected[len(r.Rejected)-1].Reason == "a rule cannot attack itself" {
				continue
			}
		case "implies":
			if len(it.Needs) != 1 || len(it.To) != 1 {
				reject(it, "implies needs exactly one reading in needs and one in to")
				continue
			}
		case "prefer":
			if len(it.Order) < 2 {
				reject(it, "prefer needs at least two actions")
				continue
			}
		default:
			reject(it, fmt.Sprintf("unknown kind %q", it.Kind))
			continue
		}
		alive = append(alive, it)
	}
	// cbc turns `implies a b` into a doubt named mismatch__a__b, and an action may cite it. Without this an action
	// citing it was refused, so models wrote a separate "instrument fault" claim needing a -- which the doubt then
	// discards whenever it would fire, so it never held (seen 2026-10-11 in two Claude drafts; cbc: unreachable).
	for _, it := range alive {
		if it.Kind == "implies" {
			defined["mismatch__"+it.Needs[0]+"__"+it.To[0]] = "doubt"
		}
	}

	// 2. References, to a fixed point: dropping one item can orphan another.
	refs := func(it Item) []string {
		var out []string
		out = append(out, it.Needs...)
		out = append(out, it.When...)
		out = append(out, it.Undermines...)
		out = append(out, it.To...)
		out = append(out, it.Order...)
		if it.From != "" {
			out = append(out, it.From)
		}
		return out
	}
	for changed := true; changed; {
		changed = false
		var next []Item
		for _, it := range alive {
			bad, why := "", ""
			for _, n := range refs(it) {
				if _, ok := defined[n]; !ok {
					bad, why = n, fmt.Sprintf("refers to %q, which is not defined by any accepted rule", n)
					break
				}
			}
			if bad == "" && it.Kind == "prefer" {
				for _, n := range it.Order {
					if defined[n] != "action" {
						bad, why = n, fmt.Sprintf("prefer orders actions only, and %q is a %s", n, defined[n])
						break
					}
				}
			}
			if bad != "" {
				reject(it, why)
				if it.Name != "" {
					delete(defined, it.Name)
				}
				changed = true
				continue
			}
			next = append(next, it)
		}
		alive = next
	}
	r.Accepted = alive
	r.Source = write(alive, m)
	return r
}

func quoteLine(q string) string {
	return "# manual: \"" + spaceRe.ReplaceAllString(strings.TrimSpace(q), " ") + "\"\n"
}

func esc(s string) string { return strings.ReplaceAll(strings.ReplaceAll(s, `"`, "'"), "\n", " ") }

func write(items []Item, m Meta) string {
	var b strings.Builder
	fmt.Fprintf(&b, "casebook %s v1\n", m.Casebook)
	fmt.Fprintf(&b, "source \"%s\" dated %s by \"drafted by %s from the manual; not yet approved\"\n", esc(m.Manual),
		m.Date, esc(m.Model))
	fmt.Fprintf(&b, "note \"%s: written by a language model from %s. Every declaration below cites the sentence\"\n",
		Marker, esc(m.Manual))
	b.WriteString("note \"it came from. Check each against the manual, add what the manual does not say, then delete\"\n")
	b.WriteString("note \"this note to approve. quorum studio will not build a chip image while it is here.\"\n")

	order := map[string]int{"observe": 0, "implies": 1, "claim": 2, "excuse": 2, "doubt": 2, "attack": 3, "action": 4,
		"prefer": 5}
	sorted := append([]Item(nil), items...)
	sort.SliceStable(sorted, func(i, j int) bool { return order[sorted[i].Kind] < order[sorted[j].Kind] })
	for _, it := range sorted {
		b.WriteString("\n")
		b.WriteString(quoteLine(it.Quote))
		switch it.Kind {
		case "observe":
			fmt.Fprintf(&b, "observe %s \"%s\"\n", it.Name, esc(it.Description))
		case "implies":
			if len(it.To) == 1 && len(it.Needs) == 1 {
				fmt.Fprintf(&b, "implies %s %s \"%s\"\n", it.Needs[0], it.To[0], esc(it.Description))
			}
		case "claim", "excuse", "doubt":
			fmt.Fprintf(&b, "%s %s \"%s\"", it.Kind, it.Name, esc(it.Description))
			if it.Kind == "claim" && it.Severity > 0 {
				fmt.Fprintf(&b, " severity %d", it.Severity)
			}
			if len(it.Needs) > 0 {
				fmt.Fprintf(&b, " needs %s", strings.Join(it.Needs, " "))
			}
			if len(it.When) > 0 {
				fmt.Fprintf(&b, " when any %s", strings.Join(it.When, " "))
			}
			b.WriteString("\n")
			if len(it.Undermines) > 0 {
				fmt.Fprintf(&b, "  undermines %s\n", strings.Join(it.Undermines, " "))
			}
		case "attack":
			fmt.Fprintf(&b, "%s %s %s\n", it.From, it.Attack, strings.Join(it.To, " "))
		case "action":
			fmt.Fprintf(&b, "action %s", it.Name)
			if it.Role != "" {
				fmt.Fprintf(&b, " %s", it.Role)
			}
			if len(it.Needs) > 0 {
				fmt.Fprintf(&b, " needs %s", strings.Join(it.Needs, " "))
			}
			if len(it.When) > 0 {
				fmt.Fprintf(&b, " when any %s", strings.Join(it.When, " "))
			}
			b.WriteString("\n")
		case "prefer":
			fmt.Fprintf(&b, "prefer %s\n", strings.Join(it.Order, " over "))
		}
	}
	return b.String()
}

// Unapproved reports whether a casebook still carries the draft marker.
func Unapproved(source string) bool { return strings.Contains(source, Marker) }

// Prompt is what the model is asked. Kept here, versioned with the gate, because the two must agree on the schema.
func Prompt(manual string) string {
	return `You are helping a maintenance engineer turn an equipment manual into a rulebook. Read the MANUAL and
output JSON only: {"items":[...]}. Each item is one of:
  {"kind":"observe","name":"bearing_hot","description":"bearing temperature high","quote":"..."}
  {"kind":"claim","name":"overheating","description":"bearing overheating","severity":7,"needs":["bearing_hot"],"quote":"..."}
  {"kind":"excuse","name":"greasing_heat","description":"grease churning warms it briefly","needs":["greasing"],"quote":"..."}
  {"kind":"doubt","name":"sensor_suspect","description":"sensor may be faulty","needs":["calib_overdue"],"undermines":["bearing_hot"],"quote":"..."}
  {"kind":"attack","from":"greasing_heat","attack":"undercuts","to":["overheating"],"quote":"..."}
  {"kind":"implies","needs":["flow_high"],"to":["current_high"],"description":"moving water draws current","quote":"..."}
  {"kind":"action","name":"stop_pump","role":"safe","when":["overheating"],"quote":"..."}
  {"kind":"action","name":"keep_running","role":"idle","quote":"..."}
  {"kind":"prefer","order":["stop_pump","keep_running"],"quote":"..."}
Rules:
- an "implies" item makes a doubt named mismatch__<needs>__<to> (here mismatch__flow_high__current_high) that holds
  when the first reading is present without the second, and sets the first reading aside. To act on such an
  instrument check, cite that name in an action's "when". Do not also write a claim for it: a claim needing the first
  reading can never hold, because the doubt sets that reading aside exactly when the claim would apply.
- "observe" is something a sensor or a person reports. Claims are faults, excuses are harmless explanations that
  cancel a claim, doubts are reasons to distrust a reading.
- ABSENCE IS NOT A READING. Never invent an "observe" for a signal being off, open or missing. "A, and B is not
  present" is written as a claim that needs A, plus an attack in which B rebuts that claim. Example, for "if the
  current relay is closed and the flow switch is not closed, the pump is running dry":
    {"kind":"claim","name":"dry_run","needs":["current_on"],...}
    {"kind":"attack","from":"flow_on","attack":"rebuts","to":["dry_run"],...}
- every name you use in needs, when, undermines, to, from or order must be declared by another item.
- an EXCEPTION ("... is expected and is not a fault", "... should not cause a shutdown") is an "excuse" that
  undercuts the claim it excuses. If it depends on an activity or mode -- a test, maintenance, start-up -- declare an
  "observe" for that mode even though the manual names no instrument for it. In a manual that says "during
  calibration a low reading is expected":
    {"kind":"observe","name":"calibrating","description":"calibration in progress","quote":"..."}
    {"kind":"excuse","name":"calibration","needs":["calibrating"],...}  {"kind":"attack","from":"calibration","attack":"undercuts","to":["low_reading"],...}
  Name a mode only if THIS manual mentions it.
- SHUTDOWN OR ALARM ONLY. Read what the manual asks for each condition. If it asks for a shutdown (with or without
  an alarm), put the claim in the "safe" action's "when". If it asks ONLY for an alarm, warning, indication or report
  and says nothing about stopping, do NOT put it in the safe action: declare a separate action for it, for example
    {"kind":"action","name":"raise_alarm","when":["bearing_hot_alarm"],"quote":"..."}
  and rank it between the safe action and the idle action in "prefer". One alarm action may serve several conditions.
- START PERMISSIVES. "X must not start until Y", "before X is allowed to start", "X may start only when Y" is a
  condition on STARTING, not a shutdown. Declare a reading for the start being requested and one for Y (Y being done
  is a reading; Y not being done is its absence), a claim that needs the start request, Y rebutting that claim, and
  an action that holds the start:
    {"kind":"observe","name":"start_requested","description":"a start has been requested","quote":"..."}
    {"kind":"observe","name":"prelube_done","description":"the prelube cycle has finished","quote":"..."}
    {"kind":"claim","name":"start_too_soon","description":"starting now would skip the prelube","severity":5,"needs":["start_requested"],"quote":"..."}
    {"kind":"attack","from":"prelube_done","attack":"rebuts","to":["start_too_soon"],"quote":"..."}
    {"kind":"action","name":"hold_start","when":["start_too_soon"],"quote":"..."}
  Rank the hold below the safe action and above the idle action.
- An exception excuses ONLY what its sentence names. "X should not cause a shutdown" cancels a shutdown caused by X;
  it does not cancel shutdowns for other faults that happen while X is going on.
- names: lower case letters, digits and underscores only.
- EVERY item needs "quote": text copied EXACTLY, character for character, from the manual, that justifies the item.
  A whole sentence or part of one. Do not add, drop or change any word, and do not include headings or numbers.
  Items whose quote is not found verbatim in the manual are thrown away. Do not invent anything the manual does not say.
- exactly one action has role "idle" (nothing wrong) and exactly one has role "safe" (the safe state).
- list every action in one "prefer" item, most cautious first.

MANUAL:
` + manual + "\n"
}

// IsLoopback reports whether an endpoint URL names this computer.
func IsLoopback(raw string) bool {
	u, err := url.Parse(raw)
	if err != nil {
		return false
	}
	h := u.Hostname()
	return h == "localhost" || h == "127.0.0.1" || h == "::1"
}

// Endpoint checks that an Ollama endpoint is on this machine unless remote use was explicitly allowed.
func Endpoint(raw string, allowRemote bool) (string, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme == "" || u.Host == "" {
		return "", fmt.Errorf("not a URL: %q", raw)
	}
	h := u.Hostname()
	if !allowRemote && h != "localhost" && h != "127.0.0.1" && h != "::1" {
		return "", fmt.Errorf("%s is not this computer: the manual would leave the machine (use -allow-remote "+
			"only if that is intended)", h)
	}
	return strings.TrimRight(raw, "/"), nil
}

var sentenceRe = regexp.MustCompile(`[^.!?]+[.!?]`)

// Closest returns the manual sentence sharing the most words with q, and the share, as a hint for a repair. It is a
// hint only: the gate still requires the quote verbatim.
func Closest(manual, q string) (string, float64) {
	words := func(s string) map[string]bool {
		m := map[string]bool{}
		for _, w := range strings.Fields(normalise(s)) {
			m[strings.Trim(w, ".,;:()'\"")] = true
		}
		return m
	}
	want := words(q)
	best, score := "", 0.0
	for _, sent := range sentenceRe.FindAllString(spaceRe.ReplaceAllString(manual, " "), -1) {
		have := words(sent)
		common := 0
		for w := range want {
			if have[w] {
				common++
			}
		}
		union := len(want) + len(have) - common
		if union > 0 && float64(common)/float64(union) > score {
			best, score = strings.TrimSpace(sent), float64(common)/float64(union)
		}
	}
	return best, score
}

// Absence reports whether a reading's name looks like the ABSENCE of a signal ("flow_off", "no_current"). quorum
// already treats an unreported reading as absent, so such a reading is usually a modelling mistake: it would have to
// be asserted by something, and a dead sensor would never assert it. A warning for the reviewer, not a rejection.
func Absence(it Item) bool {
	if it.Kind != "observe" {
		return false
	}
	n := "_" + it.Name + "_"
	for _, w := range []string{"_off_", "_open_", "_opened_", "_not_", "_no_", "_missing_", "_absent_", "_lost_", "_none_"} {
		if strings.Contains(n, w) {
			return true
		}
	}
	return false
}

// RepairPrompt hands the gate's verdicts back to the model: what was kept, what was dropped and exactly why, and asks
// for corrected versions of the dropped items only. The gate then re-checks everything, so a repair is never trusted
// either -- this is the verification-guided refinement loop, with the gate as the verifier.
func RepairPrompt(manual string, kept []Item, dropped []Rejection, feedback string) string {
	k, _ := json.Marshal(kept)
	var b strings.Builder
	for _, d := range dropped {
		j, _ := json.Marshal(d.Item)
		fmt.Fprintf(&b, "- %s\n  REJECTED BECAUSE: %s\n", j, d.Reason)
		if strings.Contains(d.Reason, "quote") {
			if c, sc := Closest(manual, d.Item.Quote); sc >= 0.3 {
				fmt.Fprintf(&b, "  The closest sentence in the manual is: \"%s\" -- quote it exactly, or part of it.\n", c)
			}
		}
		if i := strings.Index(d.Reason, "refers to \""); i >= 0 {
			name := strings.SplitN(d.Reason[i+11:], "\"", 2)[0]
			fmt.Fprintf(&b, "  Either use a name that is already ACCEPTED, or add an item that declares %q with its own"+
				" quote.\n", name)
		}
	}
	return Prompt(manual) + `
A checker has already reviewed your first answer.
ACCEPTED (do not repeat these; you may refer to their names):
` + string(k) + `
REJECTED, with the reason for each:
` + b.String() + `
PROBLEMS WITH THE DRAFT AS A WHOLE:
` + feedback + `
Return {"items":[...]} with corrected versions of the REJECTED items, plus any NEW items needed to fix the problems
above. To CHANGE an accepted item (for example to remove a wrong "idle" role), return it again with the same name:
your version replaces it. A new "prefer" item replaces the old order, so list every action in it. Fix each rejected item using its reason: copy the quote character for character from the manual, and use only
names that are ACCEPTED or that you declare in this answer. Do not repeat accepted items. Omit any item the manual
does not support.
`
}

// Ask sends the manual to a local Ollama server and returns the proposed items and the raw reply.
func Ask(ctx context.Context, endpoint, model, manual string) ([]Item, string, error) {
	return generate(ctx, endpoint, model, Prompt(manual))
}

// Repair asks for corrections of the dropped items.
func Repair(ctx context.Context, endpoint, model, manual string, r Result, feedback string) ([]Item, string, error) {
	return generate(ctx, endpoint, model, RepairPrompt(manual, r.Accepted, r.Rejected, feedback))
}

// SentencePrompt asks about ONE sentence of the manual: does it state something the draft does not yet say? Whole-manual
// drafting misses rules (four of about thirteen on the sump-pump manual, 2026-10-10); asking sentence by sentence
// turns "find every rule" into many small questions, each with a definite subject.
func SentencePrompt(manual string, kept []Item, sentence string) string {
	k, _ := json.Marshal(kept)
	return Prompt(manual) + `
A rulebook has already been drafted from this manual:
` + string(k) + `

Now look at ONE sentence of the manual only:
"` + sentence + `"

Does this sentence state a reading, a fault, an exception, an instrument check, an action or a priority that the
rulebook above does NOT yet express? Read it carefully: one sentence can state more than one rule, and a sentence
already quoted for one rule can state another.
- If it does, return {"items":[...]} with ONLY the new items needed. Every new item's quote must be copied from THIS
  sentence. You may refer to names already in the rulebook. Declare any new name you use.
- If the rulebook already expresses everything in this sentence, or the sentence states no rule, return {"items":[]}.
`
}

// Sentences are the manual's sentences, in order, without headings: the units the sentence pass asks about.
func Sentences(manual string) []string { return sentences(manual) }

// Within keeps only items whose quote comes from the given sentence. The sentence pass is about one sentence; an item
// quoting elsewhere is the model drifting back to the whole manual, which is what the pass exists to avoid.
func Within(items []Item, sentence string) (in, out []Item) {
	ns := normalise(sentence)
	for _, it := range items {
		if q := normalise(it.Quote); len(q) >= 12 && strings.Contains(ns, q) {
			in = append(in, it)
		} else {
			out = append(out, it)
		}
	}
	return in, out
}

// AskSentence runs the sentence prompt.
func AskSentence(ctx context.Context, endpoint, model, manual string, kept []Item, sentence string) ([]Item, string, error) {
	return generate(ctx, endpoint, model, SentencePrompt(manual, kept, sentence))
}

// Merge adds a new answer to what is already kept. A named item with the same name as a kept one REPLACES it, so a
// repair round can correct an accepted item (without this, "three actions marked idle" could never be fixed: every
// corrected version was refused as a redefinition, seen 2026-10-10). A new prefer replaces all earlier ones, because
// the order is one decision. Everything is gated again afterwards, so a replacement is checked like any other item.
func Merge(kept, more []Item) []Item {
	repl := map[string]bool{}
	newPrefer := false
	for _, it := range more {
		if it.Name != "" {
			repl[it.Name] = true
		}
		if strings.EqualFold(strings.TrimSpace(it.Kind), "prefer") {
			newPrefer = true
		}
	}
	// A replacement action keeps every trigger the old one had: dropping a route to an action -- above all to the safe
	// action -- is a decision for a person, not something a later answer may do in passing (seen 2026-10-10: the
	// sentence pass replaced "stop when overheating or dry" by "stop when overheating").
	old := map[string]Item{}
	for _, it := range kept {
		if it.Kind == "action" {
			old[it.Name] = it
		}
	}
	more = append([]Item(nil), more...)
	for i, it := range more {
		// Only the SAFE action keeps its old triggers. Applied to every action, the rule made one kind of mistake
		// impossible to repair: an action that fires on every trigger of a lower-ranked one makes that one
		// unreachable, and the only fix -- dropping the shared trigger -- was silently undone, so cbc refused the
		// sump draft in four runs of five (2026-10-14). Losing a route to the safe action is the dangerous direction;
		// for other actions the compiler's reachability check catches what a dropped trigger breaks.
		if o, ok := old[it.Name]; ok && strings.EqualFold(strings.TrimSpace(it.Kind), "action") &&
			strings.EqualFold(strings.TrimSpace(o.Role), "safe") {
			have := map[string]bool{}
			for _, w := range it.When {
				have[w] = true
			}
			for _, w := range o.When {
				if !have[w] {
					more[i].When = append(more[i].When, w)
				}
			}
		}
	}
	var out []Item
	for _, it := range kept {
		if (it.Name != "" && repl[it.Name]) || (newPrefer && it.Kind == "prefer") {
			continue
		}
		out = append(out, it)
	}
	return append(out, more...)
}

// Update gates a new answer merged into what is kept. A replacement that fails the gate does not cost the original:
// the original stays, and the failed replacement is reported as rejected.
func Update(manual string, kept, more []Item, m Meta) Result {
	r := Gate(manual, Merge(kept, more), m)
	had := map[string]bool{}
	for _, it := range kept {
		if it.Name != "" {
			had[it.Name] = true
		}
	}
	hadPrefer := false
	for _, it := range kept {
		hadPrefer = hadPrefer || it.Kind == "prefer"
	}
	failed := map[string]bool{}
	failedPrefer := false
	var failedRej []Rejection
	for _, x := range r.Rejected {
		switch {
		case x.Item.Name != "" && had[x.Item.Name]:
			failed[x.Item.Name] = true
		case strings.EqualFold(x.Item.Kind, "prefer") && hadPrefer:
			failedPrefer = true
		default:
			continue
		}
		x.Reason += " (the earlier version is kept)"
		failedRej = append(failedRej, x)
	}
	if len(failed) == 0 && !failedPrefer {
		return r
	}
	var keep []Item
	for _, it := range more {
		if (it.Name != "" && failed[it.Name]) || (failedPrefer && strings.EqualFold(it.Kind, "prefer")) {
			continue
		}
		keep = append(keep, it)
	}
	r2 := Gate(manual, Merge(kept, keep), m)
	r2.Rejected = append(r2.Rejected, failedRej...)
	return r2
}

// Replay re-gates recorded answers exactly as the repair loop does: the first answer, then each round's answer
// added to what the previous round kept. Used to test the gate and the review against real model output.
func Replay(manual string, rounds [][]Item, m Meta) Result {
	if len(rounds) == 0 {
		return Gate(manual, nil, m)
	}
	r := Gate(manual, rounds[0], m)
	for _, more := range rounds[1:] {
		r = Update(manual, r.Accepted, more, m)
	}
	return r
}

// generate asks once deterministically and, if the answer is not the JSON item list, once more with a little
// randomness: at temperature 0 the same prompt gives the same broken answer (seen 2026-10-11 in a final repair round).
// Sampling. Temperature 0 makes every answer reproducible, so repeating a run with it proves nothing; to measure how
// much a result depends on luck, run again with a temperature above 0 and a different seed.
var (
	Temperature = 0.0
	Seed        = 1
)

// Retries for a server that fails rather than answers (out of memory, a crashed runner: seen 2026-10-11 when builds
// ran beside the model). Waits double each time. Variables so tests need not wait.
var (
	Retries   = 3
	RetryWait = 30 * time.Second
)

func generate(ctx context.Context, endpoint, model, prompt string) ([]Item, string, error) {
	items, raw, err := generateRetrying(ctx, endpoint, model, prompt, Temperature, Seed)
	if err != nil && raw != "" && strings.Contains(err.Error(), "did not return the JSON") {
		items2, raw2, err2 := generateRetrying(ctx, endpoint, model, prompt, Temperature+0.4, Seed+1000)
		return items2, raw + "\n" + raw2, err2
	}
	return items, raw, err
}

// errServer marks a failure of the server rather than of the answer, which is worth retrying.
type errServer struct{ error }

func generateRetrying(ctx context.Context, endpoint, model, prompt string, temperature float64, seed int) ([]Item, string, error) {
	wait := RetryWait
	for attempt := 0; ; attempt++ {
		items, raw, err := generateOnce(ctx, endpoint, model, prompt, temperature, seed)
		var se errServer
		if err == nil || !errors.As(err, &se) || attempt >= Retries {
			return items, raw, err
		}
		select {
		case <-ctx.Done():
			return nil, raw, ctx.Err()
		case <-time.After(wait):
		}
		wait *= 2
	}
}

// API selects the server protocol: "ollama" (the default, a local Ollama server) or "openai" (an OpenAI-compatible
// chat-completions server such as OpenRouter). Key is the bearer token for "openai", read from the environment by the
// caller and never written anywhere. ZDR asks OpenRouter to route only to providers that keep no data and do not train
// on it; it is always on for openrouter.ai.
var (
	API = "ollama"
	Key = ""
)

func generateOnce(ctx context.Context, endpoint, model, prompt string, temperature float64, seed int) ([]Item, string, error) {
	if API == "openai" {
		return generateChat(ctx, endpoint, model, prompt, temperature, seed)
	}
	body, _ := json.Marshal(map[string]any{
		"model": model, "prompt": prompt, "format": "json", "stream": false,
		"options": map[string]any{"temperature": temperature, "seed": seed, "num_ctx": 8192},
	})
	ctx, cancel := context.WithTimeout(ctx, 30*time.Minute)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint+"/api/generate", bytes.NewReader(body))
	if err != nil {
		return nil, "", err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("cannot reach the model at %s (is Ollama running?): %w", endpoint, err)
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, "", err
	}
	if resp.StatusCode != http.StatusOK {
		e := fmt.Errorf("model server said %s: %s", resp.Status, strings.TrimSpace(string(raw)))
		if resp.StatusCode >= 500 || resp.StatusCode == http.StatusRequestTimeout {
			return nil, string(raw), errServer{e}
		}
		return nil, string(raw), e
	}
	var g struct {
		Response string `json:"response"`
	}
	if err := json.Unmarshal(raw, &g); err != nil {
		return nil, string(raw), fmt.Errorf("unexpected reply from the model server: %w", err)
	}
	items, err := Parse(g.Response)
	return items, g.Response, err
}

// ChatBody is the request generateChat sends; exported for the test that checks the privacy fields.
func ChatBody(endpoint, model, prompt string, temperature float64, seed int) map[string]any {
	b := map[string]any{
		"model":           model,
		"messages":        []map[string]string{{"role": "user", "content": prompt}},
		"temperature":     temperature,
		"seed":            seed,
		"max_tokens":      8000,
		"response_format": map[string]string{"type": "json_object"},
	}
	if u, err := url.Parse(endpoint); err == nil && strings.HasSuffix(u.Hostname(), "openrouter.ai") {
		// Only zero-data-retention endpoints that do not collect data; a request no such endpoint can serve fails
		// rather than falling back to one that keeps the prompt.
		b["provider"] = map[string]any{"zdr": true, "data_collection": "deny", "allow_fallbacks": true}
	}
	return b
}

func generateChat(ctx context.Context, endpoint, model, prompt string, temperature float64, seed int) ([]Item, string, error) {
	body, _ := json.Marshal(ChatBody(endpoint, model, prompt, temperature, seed))
	ctx, cancel := context.WithTimeout(ctx, 10*time.Minute)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return nil, "", err
	}
	req.Header.Set("Content-Type", "application/json")
	if Key != "" {
		req.Header.Set("Authorization", "Bearer "+Key)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, "", errServer{fmt.Errorf("cannot reach the model at %s: %w", endpoint, err)}
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, "", errServer{err}
	}
	if resp.StatusCode != http.StatusOK {
		e := fmt.Errorf("model server said %s: %s", resp.Status, strings.TrimSpace(string(raw)))
		// 408 is an upstream provider timing out (seen 2026-10-14 through OpenRouter, Azure): worth retrying too.
		if resp.StatusCode >= 500 || resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode == http.StatusRequestTimeout {
			return nil, string(raw), errServer{e}
		}
		return nil, string(raw), e
	}
	var g struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.Unmarshal(raw, &g); err != nil {
		return nil, string(raw), errServer{fmt.Errorf("unexpected reply from the model server: %w", err)}
	}
	if g.Error != nil {
		return nil, string(raw), errServer{errors.New("model server error: " + g.Error.Message)}
	}
	if len(g.Choices) == 0 {
		return nil, string(raw), errServer{errors.New("model server returned no answer")}
	}
	text := g.Choices[0].Message.Content
	items, err := Parse(text)
	return items, text, err
}

var fenceRe = regexp.MustCompile("(?s)^\\s*```(?:json)?\\s*(.*?)\\s*```\\s*$")

// Parse reads {"items":[...]} or a bare array, also inside a ```json fence (chat models add one).
func Parse(s string) ([]Item, error) {
	if m := fenceRe.FindStringSubmatch(s); m != nil {
		s = m[1]
	}
	var wrapped struct {
		Items []Item `json:"items"`
	}
	if err := json.Unmarshal([]byte(s), &wrapped); err == nil && wrapped.Items != nil {
		return wrapped.Items, nil
	}
	var bare []Item
	if err := json.Unmarshal([]byte(s), &bare); err == nil {
		return bare, nil
	}
	// A chat model may explain itself first and put the JSON last (seen 2026-10-11, Claude: "The sentence describes
	// the thermal switch ... {"items":[]}"). Take the LAST complete {"items":...} object in the reply.
	for i := strings.LastIndex(s, "{"); i >= 0; i = strings.LastIndex(s[:i], "{") {
		var w struct {
			Items []Item `json:"items"`
		}
		dec := json.NewDecoder(strings.NewReader(s[i:]))
		if err := dec.Decode(&w); err == nil && w.Items != nil {
			return w.Items, nil
		}
	}
	return nil, errors.New("the model did not return the JSON item list it was asked for")
}
