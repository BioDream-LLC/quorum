# quorum studio

A desktop tool: load a casebook, import a HAZOP / LOPA / FMEA worksheet, or learn from past records; edit; check every
state; write a chip image for a Raspberry Pi Pico 2 (`.uf2`) or an Arduino Uno R4 Minima (`.bin`). Several casebooks
can share one chip (a palette). It does no reasoning itself: it embeds cbc, the importers, learn2cb and qblob, the
same programs the C++ test suite runs, plus the prebuilt runtime firmware for each board.

## Build

Everything runs in Docker, and one command builds it all: `studio/build.sh`. It needs the Docker image
`quorum-pico:1` and the Pico SDK at `$PICO_SDK_PATH` (default `~/pico-sdk`); it builds `quorum-arduino:1` itself.

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

`quorum-studio` opens the browser. `quorum-studio --no-browser` only prints the address.
`quorum-studio image pump.cb [-board unor4] [-with tank.cb] [-o out]` and `quorum-studio import hazop sheet.csv`
work without a browser.

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

On the test manual the best hosted model got all 13 rules right. On a real design standard it got about 9 of 12,
and one wrong rule passed every check. A person must approve.
