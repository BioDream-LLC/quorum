package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"quorum/studio/internal/draft"
	"quorum/studio/internal/pipeline"
)

// A drafting job runs for minutes, so the page starts it, polls it and may cancel it. One at a time: the draft
// package's sampling and API settings are process-wide, and one model server is busy enough with one draft.
type draftJob struct {
	mu      sync.Mutex
	id      string
	log     []string
	done    bool
	err     string
	source  string
	review  string
	name    string
	cancel  context.CancelFunc
	started time.Time
}

type drafting struct {
	mu  sync.Mutex
	job *draftJob
}

// draftRequest is what the page sends. The key is used for this run only: it is never written to disk, logged or
// returned. When it is empty the key comes from $QUORUM_LLM_KEY, as on the command line.
type draftRequest struct {
	Name, Content        string
	API, Endpoint, Model string
	Key                  string
	AllowRemote          bool
	Rounds               int
	BySentence           bool
	SecondModel          string
}

func (s *Server) draftStart(w http.ResponseWriter, r *http.Request) {
	var req draftRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	if strings.TrimSpace(req.Content) == "" {
		fail(w, http.StatusBadRequest, errors.New("the manual is empty"))
		return
	}
	if req.API != "ollama" && req.API != "openai" {
		fail(w, http.StatusBadRequest, errors.New("the model server must be ollama or openai"))
		return
	}
	if strings.TrimSpace(req.Model) == "" {
		fail(w, http.StatusBadRequest, errors.New("name the model to ask"))
		return
	}
	ep, err := draft.Endpoint(req.Endpoint, req.AllowRemote)
	if err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	key := req.Key
	if key == "" {
		key = os.Getenv("QUORUM_LLM_KEY")
	}
	if req.API == "openai" && key == "" && !draft.IsLoopback(ep) {
		fail(w, http.StatusBadRequest, errors.New("this model server needs a key: enter it, or start the studio with "+
			"QUORUM_LLM_KEY set"))
		return
	}
	if req.Rounds < 0 || req.Rounds > 5 {
		req.Rounds = 2
	}

	s.drafts.mu.Lock()
	if j := s.drafts.job; j != nil {
		j.mu.Lock()
		running := !j.done
		j.mu.Unlock()
		if running {
			s.drafts.mu.Unlock()
			fail(w, http.StatusConflict, errors.New("a draft is already being written; wait for it or cancel it"))
			return
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	id, _ := newID()
	j := &draftJob{id: id, cancel: cancel, started: time.Now(), name: pipeline.CasebookName(req.Name) + "-draft"}
	s.drafts.job = j
	s.drafts.mu.Unlock()

	draft.API, draft.Key = req.API, key
	draft.Temperature, draft.Seed = 0, 1
	meta := draft.Meta{Casebook: j.name, Manual: req.Name, Model: req.Model, Date: time.Now().Format("2006-01-02")}
	go func() {
		defer cancel()
		o, err := draft.Run(ctx, req.Content, meta, draft.Options{Endpoint: ep, Model: req.Model, Rounds: req.Rounds,
			BySentence: req.BySentence, SecondModel: strings.TrimSpace(req.SecondModel), Check: func(src string) []string { return pipeline.Messages(ctx, src) }},
			func(line string) { j.mu.Lock(); j.log = append(j.log, line); j.mu.Unlock() })
		j.mu.Lock()
		defer j.mu.Unlock()
		if errors.Is(err, context.Canceled) {
			j.err = "cancelled"
		} else if err != nil {
			j.err = err.Error()
		}
		if o.Result.Source != "" {
			j.source, j.review = o.Result.Source, o.Review.Text()
		}
		j.log = append(j.log, fmt.Sprintf("finished in %s", time.Since(j.started).Round(time.Second)))
		j.done = true
	}()
	writeJSON(w, map[string]string{"id": id})
}

func (s *Server) job(id string) *draftJob {
	s.drafts.mu.Lock()
	defer s.drafts.mu.Unlock()
	if s.drafts.job != nil && s.drafts.job.id == id {
		return s.drafts.job
	}
	return nil
}

func (s *Server) draftStatus(w http.ResponseWriter, r *http.Request) {
	j := s.job(r.PathValue("id"))
	if j == nil {
		fail(w, http.StatusNotFound, errors.New("no such draft"))
		return
	}
	j.mu.Lock()
	defer j.mu.Unlock()
	writeJSON(w, map[string]any{"done": j.done, "log": j.log, "error": j.err, "source": j.source,
		"review": j.review, "name": j.name})
}

func (s *Server) draftCancel(w http.ResponseWriter, r *http.Request) {
	j := s.job(r.PathValue("id"))
	if j == nil {
		fail(w, http.StatusNotFound, errors.New("no such draft"))
		return
	}
	j.cancel()
	writeJSON(w, map[string]bool{"cancelled": true})
}

// draftApprove removes the unapproved marker. The page asks the person to confirm they have checked every rule
// against the manual before calling it; the check that follows is the same as for any rulebook.
func (s *Server) draftApprove(w http.ResponseWriter, r *http.Request) {
	var req struct{ Source, Who string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	who := strings.TrimSpace(req.Who)
	if who == "" {
		fail(w, http.StatusBadRequest, errors.New("say who approved it"))
		return
	}
	writeJSON(w, map[string]string{"source": draft.Approve(req.Source, who, time.Now())})
}
