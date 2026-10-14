package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

// A fake model on this computer: the first answer is a small correct draft of the sump manual, every later
// question gets "nothing to add". The point is the plumbing: start, progress, result, approval, refusals.
func fakeModel(t *testing.T) *httptest.Server {
	q := "If the thermal switch has opened, the motor is overheated and the pump must be stopped."
	first := `{"items":[
 {"kind":"observe","name":"thermal_open","description":"motor winding overheated","quote":"` + q + `"},
 {"kind":"claim","name":"overheated","description":"motor overheated","severity":8,"needs":["thermal_open"],"quote":"` + q + `"},
 {"kind":"action","name":"keep_running","role":"idle","quote":"` + q + `"},
 {"kind":"action","name":"stop_pump","role":"safe","when":["overheated"],"quote":"` + q + `"}]}`
	calls := 0
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		resp := `{"items":[]}`
		if calls == 1 {
			resp = first
		}
		_ = json.NewEncoder(w).Encode(map[string]string{"response": resp})
	}))
}

func call(t *testing.T, h http.Handler, s *Server, method, path string, body any) (int, map[string]any) {
	var rd *strings.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = strings.NewReader(string(b))
	} else {
		rd = strings.NewReader("")
	}
	r := httptest.NewRequest(method, path, rd)
	r.Host = s.addr
	r.Header.Set("X-Quorum-Token", s.token)
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var out map[string]any
	_ = json.Unmarshal(w.Body.Bytes(), &out)
	return w.Code, out
}

func TestDraftFromAManualInTheStudio(t *testing.T) {
	manual, err := os.ReadFile("../../../casebook/fixtures/manual/sump_pump_manual.txt")
	if err != nil {
		t.Fatal(err)
	}
	m := fakeModel(t)
	defer m.Close()
	s, ln, err := Listen()
	if err != nil {
		t.Fatal(err)
	}
	ln.Close()
	h := s.Handler()

	// A model server elsewhere is refused unless the user allowed the manual to leave the computer.
	code, out := call(t, h, s, "POST", "/api/draft", map[string]any{"name": "sump.txt", "content": string(manual),
		"api": "ollama", "endpoint": "http://192.0.2.1:11434", "model": "m"})
	if code != http.StatusBadRequest || !strings.Contains(out["error"].(string), "not this computer") {
		t.Errorf("remote endpoint without permission: %d %v", code, out)
	}

	code, out = call(t, h, s, "POST", "/api/draft", map[string]any{"name": "sump.txt", "content": string(manual),
		"api": "ollama", "endpoint": m.URL, "model": "m", "rounds": 1, "bySentence": true})
	if code != http.StatusOK {
		t.Fatalf("start: %d %v", code, out)
	}
	id := out["id"].(string)
	if c, _ := call(t, h, s, "POST", "/api/draft", map[string]any{"name": "x", "content": "x", "api": "ollama",
		"endpoint": m.URL, "model": "m"}); c != http.StatusConflict {
		t.Errorf("a second draft while one runs: %d", c)
	}
	deadline := time.Now().Add(2 * time.Minute)
	for {
		_, out = call(t, h, s, "GET", "/api/draft/"+id, nil)
		if out["done"] == true || time.Now().After(deadline) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if out["done"] != true || out["error"] != "" {
		t.Fatalf("draft did not finish cleanly: %v", out)
	}
	src := out["source"].(string)
	if !strings.Contains(src, "UNAPPROVED DRAFT") || !strings.Contains(src, "claim overheated") {
		t.Errorf("draft source:\n%s", src)
	}
	if !strings.Contains(out["review"].(string), "REVIEW") || len(out["log"].([]any)) < 3 {
		t.Errorf("review or progress missing: %v", out)
	}
	_, out = call(t, h, s, "POST", "/api/approve", map[string]any{"source": src, "who": "A. Engineer"})
	if a := out["source"].(string); strings.Contains(a, "UNAPPROVED DRAFT") || !strings.Contains(a, "approved by A. Engineer") {
		t.Errorf("approved source:\n%s", a)
	}
	if c, _ := call(t, h, s, "POST", "/api/approve", map[string]any{"source": src, "who": " "}); c != http.StatusBadRequest {
		t.Errorf("approval with no name: %d", c)
	}
}
