// quorum studio: load a rulebook or a safety worksheet, check every situation, write the chip image.
//
//	quorum-studio [--no-browser]       open the studio in the browser (--no-browser: just print the address)
//	quorum-studio image <in.cb> [-o out.uf2] [-board pico2|unor4] [-with more.cb,...]
//	quorum-studio draft <manual.txt> [-model qwen2.5:7b] [-endpoint http://localhost:11434] [-o draft.cb] [-items in.json]
//	                    [-repair N] [-sentences=false]
//	quorum-studio import <hazop|lopa|fmea> <worksheet.csv>
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"quorum/studio/internal/draft"
	"runtime"
	"strings"
	"time"

	"quorum/studio/internal/pipeline"
	"quorum/studio/internal/server"
	"quorum/studio/web"
)

func main() {
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "image":
			os.Exit(imageCmd(os.Args[2:]))
		case "draft":
			os.Exit(draftCmd(os.Args[2:]))
		case "import":
			os.Exit(importCmd(os.Args[2:]))
		case "--no-browser":
			os.Exit(serve(false))
		case "-h", "--help", "help":
			fmt.Println("usage: quorum-studio [--no-browser | image <in.cb> [-o out.uf2] [-board pico2] | import <hazop|lopa|fmea> <file.csv>]")
			return
		}
	}
	os.Exit(serve(true))
}

func serve(browser bool) int {
	server.Web = web.FS
	s, ln, err := server.Listen()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	fmt.Println("quorum studio is running. Close this window to stop it.")
	fmt.Println("If the browser did not open, paste this address into it:")
	fmt.Println("  " + s.URL())
	if browser {
		openBrowser(s.URL())
	}
	if err := s.Serve(ctx, ln); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	_ = cmd.Start()
}

func imageCmd(args []string) int {
	fs := flag.NewFlagSet("image", flag.ContinueOnError)
	out := fs.String("o", "", "output .uf2 (default: <casebook>-<board>.uf2)")
	board := fs.String("board", "pico2", "target board: pico2 or unor4")
	with := fs.String("with", "", "more casebooks to put on the same chip, comma-separated (a palette)")
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: quorum-studio image <in.cb> [-o out] [-board pico2|unor4] [-with more.cb,...]")
		return 2
	}
	in := args[0]
	if err := fs.Parse(args[1:]); err != nil {
		return 2
	}
	src, err := os.ReadFile(in)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	var extras []pipeline.Extra
	if *with != "" {
		for _, f := range strings.Split(*with, ",") {
			b, err := os.ReadFile(strings.TrimSpace(f))
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			extras = append(extras, pipeline.Extra{Name: f, Source: string(b)})
		}
	}
	c, err := pipeline.Check(context.Background(), string(src), extras...)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	fmt.Println(c.Output)
	if !c.OK {
		fmt.Println("\nREFUSED: no image written.")
		return 1
	}
	u, err := pipeline.Image(*board, c.Blob)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if *out == "" {
		*out = pipeline.CasebookName(in) + "-" + *board + pipeline.Extension(*board)
	}
	if err := os.WriteFile(*out, u, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	fmt.Printf("\n%s\nwrote %s (%d bytes); the board should print:\n%s\n", c.Info, *out, len(u), c.Sweep)
	return 0
}

func importCmd(args []string) int {
	if len(args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: quorum-studio import <hazop|lopa|fmea> <worksheet.csv>")
		return 2
	}
	b, err := os.ReadFile(args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	d, err := pipeline.Import(context.Background(), args[0], args[1], b)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Fprint(os.Stderr, d.Analysis)
	fmt.Print(d.Source)
	return 0
}

// draftCmd: a language model on this computer proposes rules from a manual; the gate keeps only those it can trace to
// a sentence in the manual; cbc checks the result; the draft is marked unapproved so no chip image can be made from it.
func draftCmd(args []string) int {
	fs := flag.NewFlagSet("draft", flag.ContinueOnError)
	model := fs.String("model", "qwen2.5:7b", "Ollama model to ask")
	endpoint := fs.String("endpoint", "http://localhost:11434", "Ollama server; must be this computer unless -allow-remote")
	allowRemote := fs.Bool("allow-remote", false, "allow a model server on another machine (the manual is sent to it)")
	api := fs.String("api", "ollama", "server protocol: ollama, or openai (chat completions, e.g. https://openrouter.ai/api/v1); "+
		"the key is read from $QUORUM_LLM_KEY")
	out := fs.String("o", "", "output casebook (default: <manual>-draft.cb)")
	itemsFile := fs.String("items", "", "gate proposals from this JSON file instead of asking a model")
	rounds := fs.Int("repair", 2, "repair rounds: send dropped items and the reasons back to the model this many times")
	bySentence := fs.Bool("sentences", true, "after the first draft, ask about every sentence of the manual in turn")
	second := fs.String("second", "", "ask this model (on the same server) to compare the finished draft with the manual; "+
		"its findings go back to the drafter once and the rest into the review. \"same\" uses -model")
	temperature := fs.Float64("temperature", 0, "sampling temperature; 0 is reproducible, above 0 gives a different sample per seed")
	seed := fs.Int("seed", 1, "sampling seed (matters only above temperature 0)")
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: quorum-studio draft <manual.txt> [-model m] [-endpoint url] [-o out.cb] [-items in.json]")
		return 2
	}
	in := args[0]
	if err := fs.Parse(args[1:]); err != nil {
		return 2
	}
	draft.Temperature, draft.Seed = *temperature, *seed
	draft.API, draft.Key = *api, os.Getenv("QUORUM_LLM_KEY")
	if *api != "ollama" && *api != "openai" {
		fmt.Fprintln(os.Stderr, "-api must be ollama or openai")
		return 2
	}
	manual, err := os.ReadFile(in)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	source := *model
	if *temperature > 0 {
		source = fmt.Sprintf("%s (temperature %.2g, seed %d)", *model, *temperature, *seed)
	}
	name := pipeline.CasebookName(in) + "-draft"
	meta := draft.Meta{Casebook: name, Manual: filepath.Base(in), Date: time.Now().Format("2006-01-02")}
	var r draft.Result
	var raw string
	var skipped []string
	var findings []draft.Finding
	var asked2nd bool
	if *itemsFile != "" {
		b, err := os.ReadFile(*itemsFile)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 2
		}
		raw = string(b)
		items, err := draft.Parse(raw)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 2
		}
		meta.Model = "proposals in " + filepath.Base(*itemsFile)
		r = draft.Gate(string(manual), items, meta)
	} else {
		ep, err := draft.Endpoint(*endpoint, *allowRemote)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 2
		}
		meta.Model = source
		fmt.Printf("using %s at %s (this can take minutes without a GPU)...\n", *model, ep)
		o, err := draft.Run(context.Background(), string(manual), meta, draft.Options{Endpoint: ep, Model: *model,
			Rounds: *rounds, BySentence: *bySentence, SecondModel: secondModel(*second, *model),
			Check: func(src string) []string { return pipeline.Messages(context.Background(), src) }},
			func(line string) {
				if strings.Contains(line, "(skipped)") || strings.HasPrefix(line, "repair failed") {
					fmt.Fprintln(os.Stderr, line)
				} else {
					fmt.Println(line)
				}
			})
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		r, raw, skipped, findings, asked2nd = o.Result, o.Raw, o.Skipped, o.Second, o.Asked2nd
	}
	for _, x := range r.Rejected {
		label := x.Item.Name
		if label == "" {
			label = x.Item.From + " " + x.Item.Attack + " " + strings.Join(x.Item.To, " ") + strings.Join(x.Item.Order, " over ")
		}
		fmt.Printf("  DROPPED %-8s %-28s %s\n", x.Item.Kind, strings.TrimSpace(label), x.Reason)
	}
	for _, it := range r.Accepted {
		if draft.Absence(it) {
			fmt.Printf("  REVIEW  observe  %-28s looks like the ABSENCE of a signal. quorum already treats an unreported\n"+
				"                                         reading as absent; a dead sensor would never assert this one.\n", it.Name)
		}
	}
	if *out == "" {
		*out = pipeline.CasebookName(in) + "-draft.cb"
	}
	if err := os.WriteFile(*out, []byte(r.Source), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if raw != "" {
		_ = os.WriteFile(*out+".proposals.json", []byte(raw), 0o644)
	}
	rv := draft.Assess(string(manual), r)
	rv.Incomplete(skipped)
	rv.Second, rv.Asked2nd = findings, asked2nd
	review := rv.Text()
	_ = os.WriteFile(*out+".review.txt", []byte(review), 0o644)
	fmt.Printf("\n%s\n(also in %s.review.txt)\n", review, *out)
	fmt.Printf("\nwrote %s (UNAPPROVED). What cbc makes of it:\n\n", *out)
	c, err := pipeline.Check(context.Background(), r.Source)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	fmt.Println(c.Output)
	return 0
}

// secondModel resolves the -second flag: empty is no second reader, "same" is the drafting model.
func secondModel(flag, model string) string {
	if flag == "same" {
		return model
	}
	return flag
}
