package draft

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

func manual(t *testing.T) string {
	b, err := os.ReadFile("../../../casebook/fixtures/manual/sump_pump_manual.txt")
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// Proposals as a model might make them: mostly good, one invented, one with a paraphrased quote, one with a bad name,
// and one that depends on the invented rule and must fall with it.
var proposals = []Item{
	{Kind: "observe", Name: "float_high", Description: "high-water float switch closed",
		Quote: "The high-water float switch closes when the water in the pit is above the high-water mark."},
	{Kind: "observe", Name: "current_on", Description: "motor drawing running current",
		Quote: "The motor current relay closes when the motor is drawing running current."},
	{Kind: "observe", Name: "flow_on", Description: "water moving in the discharge",
		Quote: "The discharge flow switch\ncloses when water is moving in the discharge pipe."}, // line break in the quote: still found
	{Kind: "observe", Name: "pressure_high", Description: "discharge pressure high",
		Quote: "The pressure transducer reports discharge pressure in bar."}, // INVENTED: not in the manual
	{Kind: "claim", Name: "over_pressure", Description: "discharge over-pressure", Needs: []string{"pressure_high"},
		Quote: "the pump must be stopped"}, // depends on the invented reading
	{Kind: "claim", Name: "dry_run", Description: "running dry or blocked", Severity: 8,
		Needs: []string{"current_on"}, Quote: "Running dry destroys the shaft seal within minutes, so the pump must be stopped."},
	{Kind: "observe", Name: "Weekly-Test", Description: "weekly test in progress",
		Quote: "While the weekly test is in progress, running with no discharge flow is expected"}, // bad name
	{Kind: "excuse", Name: "fast_test", Description: "a quick test", Needs: []string{"current_on"},
		Quote: "The operator may test the pump at any time."}, // paraphrase, not in the manual
	{Kind: "attack", From: "flow_on", Attack: "rebuts", To: []string{"dry_run"},
		Quote: "If the motor current relay is closed and the discharge flow switch is not closed, the pump is running dry"},
	{Kind: "implies", Needs: []string{"flow_on"}, To: []string{"current_on"}, Description: "moving water draws current",
		Quote: "Water cannot move in the discharge pipe unless the motor is drawing current."},
	{Kind: "action", Name: "keep_running", Role: "idle",
		Quote: "the pump is idle and no action is needed"},
	{Kind: "action", Name: "stop_pump", Role: "safe", When: []string{"dry_run"},
		Quote: "so the pump must be stopped"},
	{Kind: "prefer", Order: []string{"stop_pump", "keep_running"},
		Quote: "Stopping the pump takes priority over reporting to maintenance"},
}

func TestGateKeepsOnlyWhatTheManualSays(t *testing.T) {
	r := Gate(manual(t), proposals, Meta{Casebook: "sump-draft", Manual: "sump_pump_manual.txt", Model: "test", Date: "2026-10-09"})
	why := map[string]string{}
	for _, x := range r.Rejected {
		why[x.Item.Name] = x.Reason
	}
	for name, want := range map[string]string{
		"pressure_high": "does not appear",
		"over_pressure": "not defined",
		"Weekly-Test":   "not a usable name",
		"fast_test":     "does not appear",
	} {
		if !strings.Contains(why[name], want) {
			t.Errorf("%s: rejection %q, want it to mention %q", name, why[name], want)
		}
	}
	if len(r.Rejected) != 4 || len(r.Accepted) != len(proposals)-4 {
		t.Errorf("kept %d, dropped %d; want %d and 4", len(r.Accepted), len(r.Rejected), len(proposals)-4)
	}
	for _, it := range r.Accepted {
		if !strings.Contains(r.Source, quoteLine(it.Quote)) {
			t.Errorf("accepted %s %s is not preceded by its quote", it.Kind, it.Name)
		}
	}
	if !Unapproved(r.Source) {
		t.Error("the draft is not marked unapproved")
	}
	for _, s := range []string{"pressure_high", "over_pressure", "fast_test", "Weekly"} {
		if strings.Contains(r.Source, s) {
			t.Errorf("a dropped item leaked into the draft: %s", s)
		}
	}
}

func TestEndpointMustBeThisComputer(t *testing.T) {
	for _, ok := range []string{"http://localhost:11434", "http://127.0.0.1:11434/", "http://[::1]:11434"} {
		if _, err := Endpoint(ok, false); err != nil {
			t.Errorf("%s refused: %v", ok, err)
		}
	}
	if _, err := Endpoint("http://192.168.1.127:11434", false); err == nil {
		t.Error("a LAN endpoint was accepted without -allow-remote")
	}
	if _, err := Endpoint("https://api.example.com", true); err != nil {
		t.Error("-allow-remote did not allow a remote endpoint")
	}
}

func TestAskTalksOllama(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var req map[string]any
		_ = json.NewDecoder(r.Body).Decode(&req)
		if r.URL.Path != "/api/generate" || req["format"] != "json" || req["stream"] != false {
			http.Error(w, "bad request", 400)
			return
		}
		items, _ := json.Marshal(map[string]any{"items": proposals[:3]})
		_ = json.NewEncoder(w).Encode(map[string]string{"response": string(items)})
	}))
	defer srv.Close()
	items, _, err := Ask(context.Background(), srv.URL, "fake", manual(t))
	if err != nil || len(items) != 3 || items[0].Name != "float_high" {
		t.Fatalf("Ask: %v, %d items", err, len(items))
	}
}

func TestRepairHints(t *testing.T) {
	m := manual(t)
	c, sc := Closest(m, "When the high-water float switch is open and no other signal is present, the pump is idle")
	if !strings.HasPrefix(c, "When the float switch is open") || sc < 0.6 {
		t.Errorf("closest %q (%.2f)", c, sc)
	}
	p := RepairPrompt(m, nil, []Rejection{
		{Item{Kind: "action", Name: "idle", Quote: "When the high-water float switch is open, the pump is idle"}, "the quote does not appear in the manual"},
		{Item{Kind: "claim", Name: "hot", Needs: []string{"thermal_open"}, Quote: "x"}, "refers to \"thermal_open\", which is not defined by any accepted rule"},
	}, "")
	if !strings.Contains(p, "The closest sentence in the manual is: \"When the float switch is open") ||
		!strings.Contains(p, `add an item that declares "thermal_open"`) {
		t.Errorf("repair prompt lacks its hints:\n%s", p[len(p)-600:])
	}
	if !Absence(Item{Kind: "observe", Name: "flow_off"}) || Absence(Item{Kind: "observe", Name: "flow_on"}) ||
		!Absence(Item{Kind: "observe", Name: "no_current"}) || Absence(Item{Kind: "claim", Name: "flow_off"}) {
		t.Error("absence heuristic")
	}
}

func TestGateRejectsStructuralMistakes(t *testing.T) {
	m := manual(t)
	q := "If the thermal switch has opened, the motor is overheated and the pump must be stopped."
	items := []Item{
		{Kind: "observe", Name: "hot", Description: "thermal switch opened", Quote: q},
		{Kind: "claim", Name: "c1", Description: "x", Needs: []string{"hot"}, Undermines: []string{"hot"}, Quote: q},
		{Kind: "claim", Name: "c2", Needs: []string{"hot"}, Quote: q},
		{Kind: "doubt", Name: "d1", Description: "x", Needs: []string{"hot"}, Quote: q},
		{Kind: "attack", From: "hot", Attack: "rebuts", To: []string{"hot"}, Quote: q},
	}
	r := Gate(m, items, Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-09"})
	if len(r.Accepted) != 1 || len(r.Rejected) != 4 {
		for _, x := range r.Rejected {
			t.Log(x.Item.Name, x.Reason)
		}
		t.Fatalf("kept %d dropped %d, want 1 and 4", len(r.Accepted), len(r.Rejected))
	}
}

func replayFixture(t *testing.T, name string) Result {
	t.Helper()
	b, err := os.ReadFile("../../../casebook/fixtures/manual/" + name)
	if err != nil {
		t.Fatal(err)
	}
	var fx struct{ Rounds [][]Item }
	if err := json.Unmarshal(b, &fx); err != nil || len(fx.Rounds) == 0 {
		t.Fatalf("%s: %v", name, err)
	}
	return Replay(manual(t), fx.Rounds, Meta{Casebook: "x", Manual: "m", Model: "t", Date: "2026-10-09"})
}

// The 14B model's real draft passed every cbc check and was wrong in two attacks (each a real quote attached to an
// inference the manual does not make) and incomplete (running dry and reporting to maintenance missing). The review
// must find both attacks, flag nothing else, and list the sentences the missing rules come from.
func TestReviewFindsWhatTheRealDraftGotWrong(t *testing.T) {
	r := replayFixture(t, "proposals-qwen2.5-14b-2026-10-09.json")
	// The live run kept 11. Today's gate keeps one more: an action the model gave an invented role, which the gate of
	// 2026-10-09 refused outright and now keeps without the role.
	if len(r.Accepted) != 12 {
		t.Fatalf("replay kept %d, want 12", len(r.Accepted))
	}

	rv := Assess(manual(t), r)
	got := map[string]bool{}
	for _, f := range rv.Flags {
		got[label(f.Item)] = true
	}
	// failed_start is flagged too, and rightly: "...and the motor current relay is not closed" became a claim that
	// needs only the float switch, which nothing from current_on argues against.
	want := []string{"flow_on undercuts overheating", "flow_on rebuts failed_start", "failed_start"}
	if len(rv.Flags) != len(want) {
		t.Errorf("flags %v, want exactly %v", got, want)
	}
	for _, w := range want {
		if !got[w] {
			t.Errorf("not flagged: %s", w)
		}
	}
	unc := strings.Join(rv.Uncovered, "\n")
	// (The report-to-maintenance action is now kept, so its sentence is cited and no longer uncovered.)
	for _, s := range []string{"the pump is running dry or the discharge is blocked"} {
		if !strings.Contains(unc, s) {
			t.Errorf("uncovered sentences do not include %q:\n%s", s, unc)
		}
	}
	want2 := []string{"actions missing from the prefer order: report_failure", "observe current_on: no rule uses this reading",
		"the manual states an exception, and no excuse expresses it: \"While the weekly test is in progress"}
	if len(rv.Problems) != len(want2) {
		t.Errorf("problems %q, want %q", rv.Problems, want2)
	}
	for i, w := range want2 {
		if i < len(rv.Problems) && !strings.HasPrefix(rv.Problems[i], w) {
			t.Errorf("problem %d: %q, want it to start %q", i, rv.Problems[i], w)
		}
	}
}

// The 7B model's real draft could not compile: no action survived. The review says why, in terms a repair round can use.
func TestReviewExplainsWhyTheSmallModelsDraftCannotWork(t *testing.T) {
	rv := Assess(manual(t), replayFixture(t, "proposals-qwen2.5-7b-2026-10-09.json"))
	p := strings.Join(rv.Problems, "\n")
	for _, s := range []string{`no action has role "idle"`, `no action has role "safe"`, "excuse weekly_test: it attacks nothing"} {
		if !strings.Contains(p, s) {
			t.Errorf("problems lack %q:\n%s", s, p)
		}
	}
	fb := rv.Feedback([]string{"casebook:2: error: a casebook needs at least one action"})
	for _, s := range []string{"PROBLEM: no action", "COMPILER: casebook:2", "If the thermal switch has opened"} {
		if !strings.Contains(fb, s) {
			t.Errorf("feedback lacks %q", s)
		}
	}
	if !strings.Contains(RepairPrompt(manual(t), nil, nil, fb), "PROBLEMS WITH THE DRAFT AS A WHOLE:\n- PROBLEM") {
		t.Error("the repair prompt does not carry the feedback")
	}
}

func TestSentencesSkipHeadings(t *testing.T) {
	s := sentences(manual(t))
	for _, x := range s {
		if strings.HasPrefix(x, "Instruments") || strings.HasPrefix(x, "1.") || strings.Contains(x, "Priorities") {
			t.Errorf("heading leaked into a sentence: %q", x)
		}
	}
	if len(s) != 17 {
		t.Errorf("%d sentences, want 17", len(s))
	}
}

func TestSentencePass(t *testing.T) {
	m := manual(t)
	sent := "While the weekly test is in progress, running with no discharge flow is expected and is not a fault."
	in, out := Within([]Item{
		{Kind: "excuse", Name: "weekly", Quote: "running with no discharge flow is expected and is not a fault"},
		{Kind: "observe", Name: "elsewhere", Quote: "The thermal switch in the motor winding opens when the winding is overheated."},
	}, sent)
	if len(in) != 1 || len(out) != 1 || in[0].Name != "weekly" {
		t.Errorf("Within: in %v out %v", in, out)
	}
	p := SentencePrompt(m, []Item{{Kind: "observe", Name: "flow_on"}}, sent)
	if !strings.Contains(p, "Now look at ONE sentence of the manual only:\n\""+sent) || !strings.Contains(p, `"flow_on"`) {
		t.Error("sentence prompt lacks the sentence or the current draft")
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var req map[string]any
		_ = json.NewDecoder(r.Body).Decode(&req)
		if !strings.Contains(req["prompt"].(string), sent) {
			http.Error(w, "wrong sentence", 400)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]string{"response": `{"items":[]}`})
	}))
	defer srv.Close()
	items, _, err := AskSentence(context.Background(), srv.URL, "fake", m, nil, sent)
	if err != nil || len(items) != 0 {
		t.Errorf("AskSentence: %v %v", items, err)
	}
	// a repeated attack is written once
	q := "If the motor current relay is closed and the discharge flow switch is not closed, the pump is running dry"
	r := Gate(m, []Item{
		{Kind: "observe", Name: "flow_on", Description: "flow", Quote: q},
		{Kind: "claim", Name: "dry_run", Description: "dry", Needs: []string{"flow_on"}, Quote: q},
		{Kind: "attack", From: "flow_on", Attack: "rebuts", To: []string{"dry_run"}, Quote: q},
		{Kind: "attack", From: "flow_on", Attack: "rebuts", To: []string{"dry_run"}, Quote: q},
	}, Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-10"})
	if len(r.Accepted) != 3 || strings.Count(r.Source, "flow_on rebuts dry_run") != 1 {
		t.Errorf("duplicate attack: kept %d\n%s", len(r.Accepted), r.Source)
	}
}

func TestAFailedReplacementKeepsTheOriginal(t *testing.T) {
	m := manual(t)
	q := "The motor current relay closes when the motor is drawing running current."
	kept := Gate(m, []Item{{Kind: "observe", Name: "current_on", Description: "motor current relay closed", Quote: q}},
		Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-10"}).Accepted
	r := Update(m, kept, []Item{{Kind: "observe", Name: "current_on", Description: "x", Quote: "an invented sentence here"}},
		Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-10"})
	if len(r.Accepted) != 1 || r.Accepted[0].Quote != q || len(r.Rejected) != 1 ||
		!strings.Contains(r.Rejected[0].Reason, "earlier version is kept") {
		t.Errorf("accepted %+v rejected %+v", r.Accepted, r.Rejected)
	}
}

func TestOneSafeActionOneIdleAction(t *testing.T) {
	m := manual(t)
	q := "Stopping the pump takes priority over reporting to maintenance, and reporting to maintenance takes priority over normal running."
	meta := Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-10"}
	kept := Gate(m, []Item{
		{Kind: "action", Name: "stop_pump", Role: "safe", Quote: q},
		{Kind: "action", Name: "keep_running", Role: "idle", Quote: q},
		{Kind: "observe", Name: "pit_high", Description: "pit high", Quote: q},
		{Kind: "action", Name: "report", When: []string{"pit_high"}, Quote: q},
		{Kind: "prefer", Order: []string{"stop_pump", "report", "keep_running"}, Quote: q},
	}, meta).Accepted
	// a second safe action is refused, and the prefer that names it fails -- without losing the old order
	r := Update(m, kept, []Item{
		{Kind: "action", Name: "start_pump", Role: "safe", Quote: q},
		{Kind: "prefer", Order: []string{"start_pump", "stop_pump", "report", "keep_running"}, Quote: q},
	}, meta)
	why := ""
	for _, x := range r.Rejected {
		why += x.Item.Kind + ": " + x.Reason + "\n"
	}
	if !strings.Contains(why, "stop_pump is already the safe action") || strings.Count(r.Source, "\nprefer ") != 1 ||
		!strings.Contains(r.Source, "prefer stop_pump over report over keep_running") {
		t.Errorf("rejections:\n%s\nsource:\n%s", why, r.Source)
	}
	// the holder itself may be replaced and keep its role
	r = Update(m, kept, []Item{{Kind: "action", Name: "stop_pump", Role: "safe", When: nil, Quote: q}}, meta)
	if len(r.Rejected) != 0 {
		t.Errorf("replacing the safe action itself was refused: %+v", r.Rejected)
	}
}

func TestActionsSurviveInventedRolesAndKeepTheirRoutes(t *testing.T) {
	m := manual(t)
	q := "Running dry destroys the shaft seal within minutes, so the pump must be stopped."
	meta := Meta{Casebook: "t", Manual: "m", Model: "t", Date: "2026-10-10"}
	base := []Item{
		{Kind: "observe", Name: "current_on", Description: "current", Quote: q},
		{Kind: "claim", Name: "dry_run", Description: "dry", Needs: []string{"current_on"}, Quote: q},
		{Kind: "claim", Name: "hot", Description: "hot", Needs: []string{"current_on"}, Quote: q},
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"hot", "dry_run"}, Quote: q},
		{Kind: "action", Name: "report", Role: "urgent", When: []string{"dry_run"}, Quote: q},
		{Kind: "prefer", Order: []string{"stop", "dry_run"}, Quote: q},
	}
	r := Gate(m, base, meta)
	if strings.Contains(r.Source, "urgent") || !strings.Contains(r.Source, "action report when any dry_run") {
		t.Errorf("an invented role cost the action:\n%s", r.Source)
	}
	why := ""
	for _, x := range r.Rejected {
		why += x.Reason
	}
	if !strings.Contains(why, `prefer orders actions only, and "dry_run" is a claim`) {
		t.Errorf("a prefer naming a claim was accepted: %s", why)
	}
	r = Update(m, r.Accepted, []Item{{Kind: "action", Name: "stop", Role: "safe", When: []string{"hot"}, Quote: q}}, meta)
	if !strings.Contains(r.Source, "action stop safe when any hot dry_run") {
		t.Errorf("a replacement dropped a route to the safe action:\n%s", r.Source)
	}
}

// replaySentences re-gates a recorded sentence-pass run the way the CLI does: first answer, `repair` rounds, one answer
// per sentence (kept only if quoted from that sentence), then the final repair rounds.
func replaySentences(t *testing.T, name string, repair int) Result {
	t.Helper()
	b, err := os.ReadFile("../../../casebook/fixtures/manual/" + name)
	if err != nil {
		t.Fatal(err)
	}
	var fx struct{ Rounds [][]Item }
	if err := json.Unmarshal(b, &fx); err != nil {
		t.Fatal(err)
	}
	m := manual(t)
	meta := Meta{Casebook: "sump_pump_manual-draft", Manual: "sump_pump_manual.txt", Model: "qwen2.5:14b", Date: "2026-10-10"}
	sents := Sentences(m)
	if len(fx.Rounds) != 1+repair+len(sents)+repair {
		t.Fatalf("%d answers, want %d", len(fx.Rounds), 1+repair+len(sents)+repair)
	}
	r := Gate(m, fx.Rounds[0], meta)
	k := 1
	for ; k <= repair; k++ {
		r = Update(m, r.Accepted, fx.Rounds[k], meta)
	}
	for _, sent := range sents {
		in, _ := Within(fx.Rounds[k], sent)
		r = Update(m, r.Accepted, in, meta)
		k++
	}
	for ; k < len(fx.Rounds); k++ {
		r = Update(m, r.Accepted, fx.Rounds[k], meta)
	}
	return r
}

func TestReviewOfTheSentencePassDraft(t *testing.T) {
	r := replaySentences(t, "proposals-qwen2.5-14b-sentences-2026-10-10.json", 2)
	live, err := os.ReadFile("../../../casebook/fixtures/manual/draft-qwen2.5-14b-2026-10-10-sentences.cb")
	if err != nil {
		t.Fatal(err)
	}
	strip := func(s string) string { return s[strings.Index(s, "\n\n"):] } // the header carries the date
	if strip(r.Source) != strip(string(live)) {
		t.Fatalf("replay differs from the live draft:\n%s", r.Source)
	}
	rv := Assess(manual(t), r)
	got := map[string]string{}
	for _, f := range rv.Flags {
		got[label(f.Item)] = f.Reason
	}
	// failed_start ignores "the motor current relay is not closed": a real error the earlier flags could not see
	if !strings.Contains(got["failed_start"], "mentions current_off, current_on") {
		t.Errorf("failed_start not flagged for its ignored condition: %q", got)
	}
	for _, ok := range []string{"dry_run", "overheating"} {
		if got[ok] != "" {
			t.Errorf("%s flagged, but it is right: %s", ok, got[ok])
		}
	}
}

func TestABrokenAnswerIsAskedAgain(t *testing.T) {
	calls := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		resp := "not json at all"
		if calls == 2 {
			resp = `{"items":[{"kind":"observe","name":"x","quote":"q"}]}`
		}
		_ = json.NewEncoder(w).Encode(map[string]string{"response": resp})
	}))
	defer srv.Close()
	items, _, err := Ask(context.Background(), srv.URL, "fake", "m")
	if err != nil || len(items) != 1 || calls != 2 {
		t.Errorf("items %v err %v calls %d", items, err, calls)
	}
}

func TestServerFailuresAreRetried(t *testing.T) {
	old := RetryWait
	RetryWait = time.Millisecond
	defer func() { RetryWait = old }()
	calls := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if calls < 3 {
			http.Error(w, `{"error":"model requires more system memory"}`, 500)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]string{"response": `{"items":[]}`})
	}))
	defer srv.Close()
	if _, _, err := Ask(context.Background(), srv.URL, "fake", "m"); err != nil || calls != 3 {
		t.Errorf("err %v after %d calls", err, calls)
	}
	rv := Review{Sentences: 2}
	rv.Incomplete([]string{"A sentence."})
	if !strings.HasPrefix(strings.SplitN(rv.Text(), "\n\n", 2)[1], "INCOMPLETE: the model server failed and 1 sentences") {
		t.Errorf("review does not lead with INCOMPLETE:\n%s", rv.Text())
	}
}

func TestReviewAsksForTheMissingException(t *testing.T) {
	// Run 4's real draft: the weekly-test sentences are cited by an attack, and no excuse exists.
	rv := Assess(manual(t), replaySentences(t, "proposals-qwen2.5-14b-sentences-2026-10-10.json", 2))
	p := strings.Join(rv.Problems, "\n")
	if !strings.Contains(p, "no excuse expresses it: \"While the weekly test is in progress") {
		t.Errorf("the missing weekly-test excuse is not reported:\n%s", p)
	}
	b, err := os.ReadFile("../../../casebook/fixtures/manual/usace/em1110-2-3105-engine-controls.txt")
	if err != nil {
		t.Fatal(err)
	}
	var heads []string
	for _, s := range Sentences(string(b)) {
		if strings.HasPrefix(s, "8-") || strings.HasPrefix(s, "9-") || strings.Contains(s, "Engine control equipment Control") {
			heads = append(heads, s)
		}
	}
	if len(heads) != 0 {
		t.Errorf("headings leaked into sentences: %q", heads)
	}
	rv = Assess(string(b), Result{})
	if !strings.Contains(strings.Join(rv.Problems, "\n"), "minor fluctuations in commercial power") {
		t.Errorf("the power-fluctuation exception is not found: %q", rv.Problems)
	}
}

func TestReviewCatchesAReadingTheManualNeverMentions(t *testing.T) {
	b, err := os.ReadFile("../../../casebook/fixtures/manual/usace/em1110-2-3105-engine-controls.txt")
	if err != nil {
		t.Fatal(err)
	}
	q := "Visual and audible alarms and automatic engine shutdown (as a minimum) should occur for high jacket water temperature"
	r := Gate(string(b), []Item{
		{Kind: "observe", Name: "weekly_test", Description: "weekly test in progress", Quote: q},
		{Kind: "observe", Name: "jacket_water_hot", Description: "high jacket water temperature", Quote: q},
	}, Meta{Casebook: "t"})
	p := strings.Join(Assess(string(b), r).Problems, "\n")
	if !strings.Contains(p, "observe weekly_test: no word") {
		t.Errorf("the invented reading is not reported:\n%s", p)
	}
	if strings.Contains(p, "observe jacket_water_hot: no word") {
		t.Errorf("a real reading is reported as invented:\n%s", p)
	}
	// In the sump manual the weekly test is real.
	if p := strings.Join(Assess(manual(t), Gate(manual(t), []Item{{Kind: "observe", Name: "weekly_test",
		Description: "weekly test in progress", Quote: "During the weekly test the operator runs the pump"}}, Meta{Casebook: "t"})).Problems, "\n"); strings.Contains(p, "no word") {
		t.Errorf("the sump manual's weekly test is reported as invented:\n%s", p)
	}
}

func TestChatAPIKeepsThePromptOffRetainingProviders(t *testing.T) {
	b := ChatBody("https://openrouter.ai/api/v1", "m", "p", 0, 1)
	p, ok := b["provider"].(map[string]any)
	if !ok || p["zdr"] != true || p["data_collection"] != "deny" {
		t.Fatalf("an OpenRouter request must demand zero data retention: %v", b["provider"])
	}
	if _, ok := ChatBody("http://localhost:8000/v1", "m", "p", 0, 1)["provider"]; ok {
		t.Errorf("provider routing sent to a server that is not OpenRouter")
	}
	oldAPI, oldKey, oldWait := API, Key, RetryWait
	API, Key, RetryWait = "openai", "secret", time.Millisecond
	defer func() { API, Key, RetryWait = oldAPI, oldKey, oldWait }()
	calls := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.URL.Path != "/v1/chat/completions" || r.Header.Get("Authorization") != "Bearer secret" {
			t.Errorf("path %s, auth %q", r.URL.Path, r.Header.Get("Authorization"))
		}
		if calls == 1 {
			http.Error(w, `{"error":{"message":"rate limited"}}`, http.StatusTooManyRequests)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"choices": []map[string]any{{"message": map[string]string{
			"content": "```json\n{\"items\":[{\"kind\":\"observe\",\"name\":\"a\"}]}\n```"}}}})
	}))
	defer srv.Close()
	items, _, err := Ask(context.Background(), srv.URL+"/v1", "m", "manual")
	if err != nil || len(items) != 1 || items[0].Name != "a" || calls != 2 {
		t.Errorf("items %v, err %v, calls %d", items, err, calls)
	}
}

func TestParseTakesTheJSONAfterAnExplanation(t *testing.T) {
	items, err := Parse("The draft already covers it.\n\n{\"items\":[{\"kind\":\"observe\",\"name\":\"a\",\"quote\":\"x {y}\"}]}\n")
	if err != nil || len(items) != 1 || items[0].Name != "a" {
		t.Errorf("items %v, err %v", items, err)
	}
	if _, err := Parse("I cannot help with that."); err == nil {
		t.Errorf("prose with no JSON was accepted")
	}
}

func TestGateAndReviewCatchWhatTheBigModelsGotWrong(t *testing.T) {
	b, err := os.ReadFile("../../../casebook/fixtures/manual/usace/em1110-2-3105-engine-controls.txt")
	if err != nil {
		t.Fatal(err)
	}
	m := string(b)
	q := "Automatic engine shutdown is required for engine overspeed."
	list := "Visual and audible alarms and automatic engine shutdown (as a minimum) should occur for high jacket water temperature, high lubricating oil temperature, low lubricating oil pressure, gear reducer high lubricating oil temperature, or gear reducer low lubricating oil pressure"
	r := Gate(m, []Item{
		{Kind: "observe", Name: "hot", Description: "high jacket water temperature", Quote: list},
		{Kind: "observe", Name: "oil", Description: "low lubricating oil pressure", Quote: list},
		{Kind: "claim", Name: "overspeed", Description: "engine overspeeding", Severity: 9, Quote: q},
		{Kind: "claim", Name: "engine_fault", Description: "engine fault", Severity: 7, Needs: []string{"hot", "oil"}, Quote: list},
		{Kind: "action", Name: "shutoff_air", Quote: q},
		{Kind: "action", Name: "run", Role: "idle", Quote: q},
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"engine_fault"}, Quote: q},
	}, Meta{Casebook: "t"})
	why := map[string]string{}
	for _, x := range r.Rejected {
		why[x.Item.Name] = x.Reason
	}
	if !strings.Contains(why["overspeed"], "needs evidence") || !strings.Contains(why["shutoff_air"], "needs a reason") {
		t.Errorf("rejections %v", why)
	}
	if p := strings.Join(Assess(m, r).Problems, "\n"); !strings.Contains(p, "engine_fault needs all of hot, oil at once") {
		t.Errorf("the and-for-or rule is not reported:\n%s", p)
	}
}

func TestAnActionMayCiteTheMismatchAnImpliesMakes(t *testing.T) {
	m := manual(t)
	q := "Water cannot move in the discharge pipe unless the motor is drawing current."
	r := Gate(m, []Item{
		{Kind: "observe", Name: "flow_on", Description: "flow", Quote: q},
		{Kind: "observe", Name: "current_on", Description: "current", Quote: q},
		{Kind: "implies", Needs: []string{"flow_on"}, To: []string{"current_on"}, Description: "flow needs current", Quote: q},
		{Kind: "action", Name: "inspect", When: []string{"mismatch__flow_on__current_on"}, Quote: q},
	}, Meta{Casebook: "t"})
	if len(r.Rejected) != 0 || !strings.Contains(r.Source, "action inspect when any mismatch__flow_on__current_on") {
		t.Errorf("rejected %+v\n%s", r.Rejected, r.Source)
	}
}

func TestReviewNamesWhatCanStopTheSafeAction(t *testing.T) {
	m := manual(t)
	rv := Assess(m, replaySentences(t, "proposals-qwen2.5-14b-sentences-2026-10-10.json", 2))
	c := strings.Join(rv.Cautions, "\n")
	if !strings.Contains(c, "can stop stop_pump (the safe action) for dry_run") {
		t.Errorf("cautions %q", rv.Cautions)
	}
	if !strings.Contains(rv.Text(), "CAUTION") {
		t.Errorf("the review text does not show the cautions")
	}
}

func TestApproveRemovesOnlyTheMarker(t *testing.T) {
	m := manual(t)
	q := "If the thermal switch has opened, the motor is overheated and the pump must be stopped."
	r := Gate(m, []Item{
		{Kind: "observe", Name: "thermal_open", Description: "winding overheated", Quote: q},
		{Kind: "claim", Name: "overheated", Description: "motor overheated", Severity: 8, Needs: []string{"thermal_open"}, Quote: q},
		{Kind: "action", Name: "run", Role: "idle", Quote: q},
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"overheated"}, Quote: q},
	}, Meta{Casebook: "t", Manual: "m.txt", Model: "x", Date: "2026-10-13"})
	if !Unapproved(r.Source) {
		t.Fatal("a draft must start unapproved")
	}
	a := Approve(r.Source, "A. Engineer", time.Date(2026, 10, 13, 0, 0, 0, 0, time.UTC))
	if Unapproved(a) || strings.Contains(a, "note \"") {
		t.Errorf("marker still present:\n%s", a)
	}
	if !strings.Contains(a, "approved by A. Engineer on 2026-10-13") {
		t.Errorf("approval not recorded:\n%s", a)
	}
	if strings.Count(a, "\n") != strings.Count(r.Source, "\n")-3 {
		t.Errorf("approval removed more than the three marker lines")
	}
}

func TestCrossCheckKeepsOnlyVerbatimFindings(t *testing.T) {
	m := manual(t)
	q := "If the thermal switch has opened, the motor is overheated and the pump must be stopped."
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]string{"response": `{"items":[
 {"kind":"missing","name":"","quote":"` + q + `","description":"no shutdown for overheating"},
 {"kind":"wrong","name":"stop","quote":"the pump explodes when it is cold","description":"invented"},
 {"kind":"praise","name":"","quote":"` + q + `","description":"not a verdict"}]}`})
	}))
	defer srv.Close()
	fs, _, err := CrossCheck(context.Background(), srv.URL, "m", m, "casebook x v1\n")
	if err != nil {
		t.Fatal(err)
	}
	if len(fs) != 1 || fs[0].Verdict != "missing" || fs[0].Quote != q {
		t.Fatalf("want only the verbatim, known-verdict finding, got %+v", fs)
	}
	if fb := FindingsFeedback(fs); !strings.Contains(fb, "MISSING") || !strings.Contains(fb, q) {
		t.Errorf("feedback: %s", fb)
	}
}

func TestPromptTeachesAlarmOnlyAndStartPermissives(t *testing.T) {
	p := Prompt("x")
	for _, want := range []string{"SHUTDOWN OR ALARM ONLY", "START PERMISSIVES", "excuses ONLY what its sentence names"} {
		if !strings.Contains(p, want) {
			t.Errorf("prompt lacks %q", want)
		}
	}
}

func TestUnreachableHintNamesTheActionInTheWay(t *testing.T) {
	r := Result{Accepted: []Item{
		{Kind: "action", Name: "report", When: []string{"failed_start", "mismatch"}},
		{Kind: "action", Name: "inspect", When: []string{"mismatch"}},
		{Kind: "prefer", Order: []string{"stop", "report", "inspect", "run"}},
	}}
	h := unreachableHints(r, []string{"FAIL every declared action is reachable (1 unreachable: inspect) -- an action is unreachable when"})
	if len(h) != 1 || !strings.Contains(h[0], "inspect can never be chosen because report") ||
		!strings.Contains(h[0], "remove mismatch from report's") {
		t.Fatalf("hint: %v", h)
	}
	if h := unreachableHints(r, []string{"error: something else"}); len(h) != 0 {
		t.Errorf("no hint wanted, got %v", h)
	}
}

func TestMergeKeepsSafeTriggersButLetsOtherActionsDropOne(t *testing.T) {
	kept := []Item{
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"dry", "hot"}},
		{Kind: "action", Name: "report", When: []string{"failed", "mismatch"}},
	}
	got := Merge(kept, []Item{
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"hot"}},
		{Kind: "action", Name: "report", When: []string{"failed"}},
	})
	w := map[string]string{}
	for _, it := range got {
		w[it.Name] = strings.Join(it.When, " ")
	}
	if w["stop"] != "hot dry" {
		t.Errorf("the safe action must keep every route to it, got %q", w["stop"])
	}
	if w["report"] != "failed" {
		t.Errorf("another action may drop a trigger, got %q", w["report"])
	}
}

func TestReviewSaysWhenNothingCanCancelAClaim(t *testing.T) {
	m := manual(t)
	q := "If the thermal switch has opened, the motor is overheated and the pump must be stopped."
	r := Gate(m, []Item{
		{Kind: "observe", Name: "thermal_open", Description: "winding overheated", Quote: q},
		{Kind: "claim", Name: "overheated", Description: "motor overheated", Severity: 8, Needs: []string{"thermal_open"}, Quote: q},
		{Kind: "action", Name: "run", Role: "idle", Quote: q},
		{Kind: "action", Name: "stop", Role: "safe", When: []string{"overheated"}, Quote: q},
	}, Meta{Casebook: "t", Manual: "m.txt", Model: "x", Date: "2026-10-14"})
	rv := Assess(m, r)
	found := false
	for _, c := range rv.Cautions {
		found = found || strings.Contains(c, "nothing in this draft can cancel any claim")
	}
	if !found {
		t.Errorf("cautions: %v", rv.Cautions)
	}
	if strings.Contains(rv.Feedback(nil), "nothing in this draft can cancel") {
		t.Error("the caution must not be sent back to the model, which would invent an exception")
	}
}
