# quorum studio

A desktop tool: load a casebook, import a HAZOP / LOPA / FMEA worksheet, or learn from past records; edit; check every
state; write a chip image for a Raspberry Pi Pico 2 (`.uf2`) or an Arduino Uno R4 Minima (`.bin`). Several casebooks
can share one chip (a palette). It does no reasoning itself: it embeds cbc, the importers, learn2cb and qblob, the
same programs the C++ test suite runs, plus the prebuilt runtime firmware for each board.

## Build

Everything runs in Docker, and one command builds it all: `studio/build.sh`. It needs only Docker, git and bash. It
makes its images (`quorum-pico:1`, `quorum-arduino:1`, `quorum-studio-build:1`) from the Dockerfiles in the repository,
uses the Pico SDK at `$PICO_SDK_PATH` or `~/pico-sdk` if there is one, and otherwise fetches release 2.1.1 into
`build/pico-sdk`.

```
studio/build.sh     # 1. firmware/pico2/build-runtime.sh     -> build/quorum_runtime.uf2
                    # 2. firmware/arduino/build-runtime.sh   -> build/quorum_runtime_unor4.bin
                    # 3. tools, tests, binaries, installer   -> studio/dist/
```

`build-runtime.sh` compiles the Uno R4 sketch with arduino-cli 1.5.2-rc.1 and the renesas_uno 1.6.0 core, the
versions the earlier Windows builds used (`firmware/arduino/Dockerfile`). `build.sh` then cross-compiles the C++ tools for Windows with MinGW (statically linked), runs gofmt, go vet and the Go
tests, builds `quorum-studio` (Linux) and `quorum-studio.exe`, and packages `quorum-studio-setup-<version>.exe`
(NSIS, per-user, no administrator rights).

Signing: set `SIGN_PFX=<cert.pfx> SIGN_PASS=<password>` (and optionally `SIGN_TS=<timestamp URL>`). Both executables
are then Authenticode-signed and the build fails unless the signature verifies. There is no certificate yet, so
builds are unsigned and Windows SmartScreen warns on first run.

macOS: `studio/build-macos.sh` runs after `build.sh` and builds on a Mac over ssh (`MAC=user@host`; it needs the
Xcode command-line tools and Homebrew `cmake` and `go`). It builds the C++ tools and the studio as universal binaries
for Apple Silicon and Intel, runs the Go tests there, and writes `studio/dist/quorum-studio-macos.zip`. The program
is ad-hoc signed only, with no Apple Developer ID and no notarization, so macOS asks before the first run. The
`README.txt` in the zip explains how to allow it.

## Test

- `go test ./...` (inside build.sh): UF2 and .bin composition and refusals, layout constants against
  `runtime/include/quorum/runtime/layout.hpp`, the whole pipeline on the pump, tank, HAZOP fixture, AI4I and a palette,
  and the server's token and Host checks.
- Browser test, 31 checks: `dist/quorum-studio --no-browser > /tmp/studio.log &` then
  `PLAYWRIGHT=<path to the playwright package> node test/ui_test.js`.

## Security

Listens on 127.0.0.1 only, on a random port, and refuses any request without the per-launch token or with an
unexpected Host header. "Write to board" copies to a Pico 2 bootloader drive, or runs the dfu-util that the Arduino
UNO R4 board package installs; the studio does not ship dfu-util.

## Run

`quorum-studio` opens the browser. On a Mac, a Pico 2 in BOOTSEL mode is found under `/Volumes`, and for an Uno R4
the studio uses the dfu-util that the Arduino IDE installs in `~/Library/Arduino15`. `quorum-studio --no-browser` only prints the address.
`quorum-studio image pump.cb [-board unor4] [-with tank.cb] [-o out]`, `quorum-studio import hazop sheet.csv`,
`quorum-studio sheet pump.cb` and `quorum-studio bench pump.cb -port COM4` work without a browser.

## Commissioning: the check proves the rules, not the wiring

Checking every situation proves what the rules do. It cannot prove that the wire labelled "overspeed" is the overspeed
switch, that a threshold is set where the document says, or that the controller passes each reading on. On a plant
that is the loop check, and the studio prints the sheet for it.

- **Commissioning sheet** (after a passing check, or `quorum-studio sheet pump.cb`): one line per reading, giving
  how it is set, the action quorum must report when it is set alone, and, when it changes nothing alone, the fewest
  other readings with which it does, with the action with and without it. There are columns for what was seen and
  who saw it, and the image fingerprint the board must report. It comes from `cbc --commissioning`.
- **Test the board** (or `quorum-studio bench pump.cb -port COM4`) drives a board over Modbus, the way a PLC would,
  through every line of the sheet: at rest, each reading alone, and each with the readings that make it matter. It
  compares each answer and refuses a board running a different image. It covers rulebooks without raw channels: a
  channel's readings come from samples that a controller must keep writing, so those are left to the sheet. A
  rulebook with timed readings ("pressure high within 10 s after a start") is tested by playing a script through time
  on the board's test clock, so an hour-long window takes seconds. That needs a board that has just been reset, and
  leaves the test clock on until the next reset: **reset the board before it goes into service.** On a board that has
  been written to already, `-real-time` (or the box next to the button) plays the script by waiting instead.
  Measured on a Pico 2 with the pump image: 41 of 41 states as the sheet says. With the timed fixtures on its test
  clock: 19 of 19 and 17 of 17, and 19 of 19 for `timed.cb` in real time (71 minutes;
  `firmware/pico2/measured-bench-timed-2026-10-26.txt`). The Uno R4 has not run the timed bench test.
- The bench test checks the board, not the plant. Only the sheet, worked through on the real machine, checks the
  wiring.

## A rulebook nothing can argue with

cbc refuses a rulebook in which no claim can be argued against. A worksheet import or a regulation with no
exceptions produces exactly that, and such a rulebook cannot tell a dead sensor from a quiet machine. After that
refusal the studio offers **Add a failed-sensor check for each reading**. For each plain reading a claim relies on, it
adds:
- a reading for its instrument having failed;
- a doubt that undercuts the claims drawn from it while that is set;
- a claim that takes the response the reading itself led to (the highest-ranked one, so a dead shutdown sensor still
  shuts down).

Each block is marked TODO and shown in the editor; nothing is applied silently. Each added reading doubles the
number of situations to check.

## Drafting a rulebook from a manual

**Draft from a manual (.txt)** in the Load panel asks an AI model to draft a rulebook from an equipment manual.
The same loop is `quorum-studio draft manual.txt -model <m>` on the command line.

- **On this computer:** an Ollama server, for example `http://localhost:11434` with `qwen2.5:14b`. Nothing leaves the
  machine.
- **Hosted service:** any OpenAI-compatible chat API, for example `https://openrouter.ai/api/v1` with
  `anthropic/claude-sonnet-5.5`. The manual and quorum's drafting instructions are sent to it, so the studio asks you
  to allow that first. For OpenRouter every request asks for providers that keep no copy and do not train on it. The
  key is typed in for one draft and never saved, or is read from `QUORUM_LLM_KEY`.

The model only proposes. Every rule it keeps must quote the manual word for word. Refusals, review problems and
failed checks are sent back to it to fix, and it is then asked about every sentence in turn. Progress shows while it
works, and **Cancel** stops it.

The draft opens in the editor with a review. The review lists, first, everything that can stop the safe action. The
draft is marked **UNAPPROVED**, and no chip image can be built from it. After checking every rule against the manual,
enter your name and press **Approve draft**. That records who approved it and when, and removes the mark. The full
check still runs: approval skips nothing.

**Second opinion** (optional): name another model and it compares the finished draft with the manual. Disagreements it
can back with a word-for-word quote go back to the drafter once, and what it still disputes is listed in the review.
Its advice is kept only if the draft fails no more checks than before. It has not yet been shown to improve drafts,
and it roughly doubles the cost.

With Claude Sonnet 5.5:
- The test manual: 13 of 13 rules in most runs.
- A US Army Corps standard: 12 of 12 in 10 runs, after the drafter was taught alarm-only and start rules on that
  same standard.
- A Coast Guard regulation it had never seen: all 6 rules in 8 of 8 runs.
- A fairer test: four more unseen regulations, with the answer key and scorer committed before any run, and five
  runs each. It scored 116 of 135 behavioural checks (86%). Shutdown versus alarm only was right in every case. It
  missed a manual shutdown device (4 of 5 runs) and a monitoring exception (4 of 5), and it drafted design rules as
  run-time rules. cbc accepted 9 of 20 drafts as written, and 14 after the failed-sensor repair. Details, including
  one scorer bug fixed after the runs: `casebook/fixtures/manual/eval/RESULTS.md`.

These are a few documents, not a guarantee. A person must approve every draft.
