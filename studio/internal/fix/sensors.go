// Package fix makes the one repair the studio offers for a refusal that is not a mistake in a line: a rulebook in which
// nothing can argue with any claim.
//
// cbc refuses such a rulebook on purpose. A pure list of "if this reading, then that action" cannot tell a dead sensor
// from a quiet machine, and is exactly what a worksheet import or a regulation with no exceptions produces. The usual
// answer is a failed-sensor check for each reading, which is what SensorChecks writes. It does not decide anything for
// the engineer: every added line is marked TODO, and the person must confirm or delete each one.
package fix

import (
	"errors"
	"fmt"
	"sort"
	"strings"
)

// Refusal is the cbc line that SensorChecks answers.
const Refusal = "at least one claim can be argued against"

// Added describes one reading that got a failed-sensor check.
type Added struct {
	Reading string `json:"reading"`
	Action  string `json:"action"` // the response taken when its instrument has failed
}

// Result is the rewritten rulebook and what was done to it.
type Result struct {
	Source  string   `json:"source"`
	Added   []Added  `json:"added"`
	Skipped []string `json:"skipped"` // readings left alone, each with the reason
}

type decl struct {
	first, last int      // physical line range, inclusive
	toks        []string // tokens of the logical line, quoted strings kept with their quotes
}

type action struct {
	decl
	name        string
	idle, safe  bool
	when        []string // conditions, without the any/all qualifier
	qualifier   string   // "any", "all" or "" (a single bare condition)
	conjunctive bool     // `when all` or `needs`: one more condition cannot be OR-ed in
}

// SensorChecks adds, for each plain reading that some claim relies on, a failed-instrument reading, a doubt that
// undercuts the claims drawn from the reading while it is set, and a claim that triggers the response the reading itself would
// have led to (the highest-ranked one, so a dead shutdown sensor still shuts down: fail-safe).
func SensorChecks(src string) (Result, error) {
	lines := strings.Split(strings.ReplaceAll(src, "\r\n", "\n"), "\n")
	decls := logical(lines)

	declared := map[string]bool{}
	var readings []string // `observe` lines, in order
	channelObs := map[string]bool{}
	evidence := map[string][]string{} // claim -> readings and claims it needs
	isClaim := map[string]bool{}
	undermined := map[string]bool{}
	var actions []*action
	var prefer [][]string

	for _, d := range decls {
		t := d.toks
		if len(t) == 0 {
			continue
		}
		switch t[0] {
		case "observe":
			if len(t) > 1 {
				readings = append(readings, t[1])
				declared[t[1]] = true
			}
		case "reading":
			// A timed reading is worked out by the board from other readings; it has no instrument of its own to
			// fail, so it gets no failed-sensor check. Its name is taken, though.
			if len(t) > 1 {
				declared[t[1]] = true
			}
		case "channel":
			if len(t) > 1 {
				declared[t[1]] = true
				channelObs[t[1]+"_unavailable"], channelObs[t[1]+"_flatline"] = true, true
			}
			for i := 0; i+1 < len(t); i++ {
				if t[i] == "asserts" {
					channelObs[t[i+1]] = true
				}
			}
		case "claim", "excuse", "doubt":
			if len(t) < 2 {
				continue
			}
			declared[t[1]] = true
			sec := sections(t[2:])
			if t[0] == "claim" {
				isClaim[t[1]] = true
				evidence[t[1]] = append(stripQualifier(sec["needs"]), stripQualifier(sec["when"])...)
			}
			for _, x := range sec["undermines"] {
				undermined[x] = true
			}
		case "action":
			if len(t) < 2 {
				continue
			}
			declared[t[1]] = true
			a := &action{decl: d, name: t[1]}
			for _, f := range t[2:] {
				if f == "when" || f == "needs" {
					break
				}
				a.idle = a.idle || f == "idle"
				a.safe = a.safe || f == "safe"
			}
			sec := sections(t[2:])
			if w := sec["when"]; len(w) > 0 {
				if w[0] == "any" || w[0] == "all" {
					a.qualifier = w[0]
					w = w[1:]
				}
				a.when = append([]string(nil), w...)
				a.conjunctive = a.qualifier == "all"
			}
			if len(sec["needs"]) > 0 {
				a.conjunctive = true
			}
			actions = append(actions, a)
		case "prefer":
			var chain []string
			for _, x := range t[1:] {
				if x != "over" {
					chain = append(chain, x)
				}
			}
			prefer = append(prefer, chain)
		default:
			if len(t) >= 3 && (t[1] == "undermines" || t[1] == "rebuts" || t[1] == "undercuts") {
				if t[1] == "undermines" {
					for _, x := range t[2:] {
						undermined[x] = true
					}
				}
			}
		}
	}

	// beats[a][b]: a is preferred over b, directly or through a chain of prefer lines.
	beats := map[string]map[string]bool{}
	set := func(a, b string) bool {
		if beats[a] == nil {
			beats[a] = map[string]bool{}
		}
		if beats[a][b] {
			return false
		}
		beats[a][b] = true
		return true
	}
	for _, c := range prefer {
		for i := range c {
			for j := i + 1; j < len(c); j++ {
				set(c[i], c[j])
			}
		}
	}
	for changed := true; changed; {
		changed = false
		for a, bs := range beats {
			for b := range bs {
				for c := range beats[b] {
					if set(a, c) {
						changed = true
					}
				}
			}
		}
	}

	// The claims whose own evidence names the reading: the ones its doubt undercuts. cbc counts only an attack on a
	// claim as an argument against it; undermining the reading itself would not count.
	direct := func(reading string) []string {
		var out []string
		for _, d := range decls {
			if len(d.toks) > 1 && d.toks[0] == "claim" {
				for _, e := range evidence[d.toks[1]] {
					if e == reading {
						out = append(out, d.toks[1])
						break
					}
				}
			}
		}
		return out
	}

	// The claims a reading feeds, directly or through other claims.
	feeds := func(reading string) map[string]bool {
		out := map[string]bool{}
		for changed := true; changed; {
			changed = false
			for c, ev := range evidence {
				if out[c] {
					continue
				}
				for _, e := range ev {
					if e == reading || out[e] {
						out[c] = true
						changed = true
						break
					}
				}
			}
		}
		return out
	}

	var res Result
	extra := map[*action][]string{}
	var block []string
	for _, r := range readings {
		switch {
		case channelObs[r]:
			continue
		case undermined[r]:
			res.Skipped = append(res.Skipped, r+": something already questions it")
			continue
		case declared[r+"_failed"] || declared[r+"_untrusted"] || declared[r+"_blind"]:
			res.Skipped = append(res.Skipped, r+": the names "+r+"_failed / _untrusted / _blind are taken")
			continue
		}
		claims := feeds(r)
		if len(claims) == 0 {
			continue // a reading no claim uses needs no check: nothing would trust it
		}
		var cands []*action
		for _, a := range actions {
			if a.idle {
				continue
			}
			for _, w := range a.when {
				if claims[w] {
					cands = append(cands, a)
					break
				}
			}
		}
		// The best candidate that can take one more condition: one no other such candidate is preferred over, the safe
		// action first, else the first declared. An action whose trigger is an AND cannot take an OR-ed condition.
		var open []*action
		for _, a := range cands {
			if !a.conjunctive {
				open = append(open, a)
			}
		}
		var chosen *action
		for _, a := range open {
			beaten := false
			for _, b := range open {
				if b != a && beats[b.name][a.name] {
					beaten = true
				}
			}
			if !beaten && (chosen == nil || (a.safe && !chosen.safe)) {
				chosen = a
			}
		}
		if chosen == nil {
			res.Skipped = append(res.Skipped, r+": every action it leads to needs all of its conditions together, so "+
				"add its check by hand")
			continue
		}
		extra[chosen] = append(extra[chosen], r+"_blind")
		res.Added = append(res.Added, Added{Reading: r, Action: chosen.name})
		block = append(block,
			"",
			fmt.Sprintf("# TODO: confirm the device can tell when the %s instrument has failed (a broken wire, a stuck", r),
			fmt.Sprintf("# or out-of-range value), and that %s is the right response then. If %s is not an", chosen.name, r),
			"# instrument at all, delete these lines.",
			fmt.Sprintf("observe %s_failed \"the instrument behind %s has failed or is disconnected\"", r, r),
			fmt.Sprintf("doubt %s_untrusted \"%s is not trusted while its instrument has failed\" needs %s_failed", r, r, r),
			fmt.Sprintf("%s_untrusted undercuts %s", r, strings.Join(direct(r), " ")),
			fmt.Sprintf("claim %s_blind \"there is no trustworthy reading of %s\" needs %s_failed", r, r, r))
	}
	if len(res.Added) == 0 {
		return res, errors.New("found no plain reading to add a check for; add a doubt, an excuse or a rebuttal by hand")
	}

	// Rewrite each chosen action as one line with the new conditions OR-ed in. Later lines first, so earlier line
	// numbers stay valid.
	var chosen []*action
	for a := range extra {
		chosen = append(chosen, a)
	}
	sort.Slice(chosen, func(i, j int) bool { return chosen[i].first > chosen[j].first })
	for _, a := range chosen {
		var head []string
		for _, tok := range a.toks {
			if tok == "when" || tok == "needs" {
				break
			}
			head = append(head, tok)
		}
		line := strings.Join(head, " ") + " when any " + strings.Join(append(append([]string(nil), a.when...), extra[a]...), " ")
		lines = append(lines[:a.first], append([]string{line}, lines[a.last+1:]...)...)
	}

	out := strings.TrimRight(strings.Join(lines, "\n"), "\n")
	out += "\n\n# ── Failed-sensor checks, added by quorum studio ─────────────────────────────────────────────────────────\n" +
		"# cbc refused this rulebook because nothing could argue with any claim, so it could not tell a dead sensor\n" +
		"# from a quiet machine. Each block below stops claims being drawn from one reading while its instrument has\n" +
		"# failed, and takes the response that reading would have led to instead. Each is a suggestion: confirm or\n# delete it.\n" +
		strings.Join(block, "\n") + "\n"
	res.Source = out
	return res, nil
}

// logical joins indented continuation lines onto the declaration before them, and drops comments and blank lines.
func logical(lines []string) []decl {
	var out []decl
	for i, raw := range lines {
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		toks := tokens(trimmed)
		if (raw[0] == ' ' || raw[0] == '\t') && len(out) > 0 {
			out[len(out)-1].toks = append(out[len(out)-1].toks, toks...)
			out[len(out)-1].last = i
			continue
		}
		out = append(out, decl{first: i, last: i, toks: toks})
	}
	return out
}

// tokens splits on spaces, keeping a quoted string (with its quotes) as one token.
func tokens(s string) []string {
	var out []string
	for i := 0; i < len(s); {
		for i < len(s) && (s[i] == ' ' || s[i] == '\t') {
			i++
		}
		if i >= len(s) {
			break
		}
		j := i
		if s[i] == '"' {
			j++
			for j < len(s) && s[j] != '"' {
				if s[j] == '\\' {
					j++
				}
				j++
			}
			j++
			if j > len(s) {
				j = len(s)
			}
		} else {
			for j < len(s) && s[j] != ' ' && s[j] != '\t' {
				j++
			}
		}
		out = append(out, s[i:j])
		i = j
	}
	return out
}

var keywords = map[string]bool{"needs": true, "when": true, "undermines": true, "rebuts": true, "undercuts": true,
	"severity": true, "over": true}

// sections groups the words after each keyword.
func sections(t []string) map[string][]string {
	out := map[string][]string{}
	key := ""
	for _, x := range t {
		if keywords[x] {
			key = x
			continue
		}
		if key != "" && !strings.HasPrefix(x, "\"") {
			out[key] = append(out[key], x)
		}
	}
	return out
}

func stripQualifier(w []string) []string {
	if len(w) > 0 && (w[0] == "any" || w[0] == "all") {
		return w[1:]
	}
	return w
}
