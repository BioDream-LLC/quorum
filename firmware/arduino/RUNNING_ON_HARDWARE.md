# Running on an Arduino Uno R4 Minima

The runtime firmware also prints a GUARD line and answers Modbus RTU once a valid request arrives: see
[runtime/GUARD_AND_MODBUS.md](../../runtime/GUARD_AND_MODBUS.md), including why Modbus belongs only on a link you control.

The first real-silicon measurement this project has. Renesas RA4M1, Cortex-M4 at 48 MHz, DWT CYCCNT.

## The result

Captured from the board on 2026-09-29 with the corrected sketch; raw output in
[`measured-2026-09-29.txt`](measured-2026-09-29.txt).

```
core clock            48000000
casebook              centrifugal-pump v3
arguments / attacks   44 / 66
worst-case state      0x0  (140 steps, proven bound 248)

cycle counter: DWT CYCCNT (32-bit)

  solve                 min 3608  mean 3608  max 3608 cycles   75.166 us
  FULL CYCLE            min 13646  mean 13646  max 13646 cycles   284.291 us
  binding sample+mask   min 1496  mean 1533  max 1534 cycles   31.958 us

  bound check (instructions vs cycles are DIFFERENT UNITS):
  solve                 max cycles / 2571 bound instructions = 1.40 cycles per instruction -- AT OR UNDER the 2 CPI that README assumes

  steps on device 140, predicted by cbc 140  MATCH
  verdict         Decided -> continue
```

Two claims settled, in order of importance:

1. **The device does what the model says.** 140 steps on device against 140 predicted by `cbc`, inside the proven bound
   of 248. A device that did different work than the host model would make every timing figure a measurement of
   something else.
2. **The bound survives conversion to time.** 1.40 cycles per instruction, under the 2 CPI README assumes, and
   identical to the cycle on two separate flashes. The instruction bound transfers to this core:
   `tools/wcet-disassemble.sh cortex-m4` gives an identical instruction count and an identical opcode mix to the
   Cortex-M33 original, differing only in scheduling.

**Min = mean = max, exactly.** The RA4M1 executes from internal flash with no cache, so the same work costs the same
cycles every time. The Pico 2 does not behave that way — its slowest run moves between flashes while its fastest does
not — and the contrast between the two parts is the clearest evidence in this project that the variance on RP2350 is the
memory system and not the algorithm.

**The cross-vendor prediction held.** The Pico 2 guide predicted that a second vendor at 48 MHz would give close cycle
counts and microsecond figures about 3x apart, which would show the numbers measure quorum rather than RP2350. `solve`
is 3,608 cycles here against 2,809–3,719 on the Pico 2, and 75.2 us against 24.8 us is 3.0x.

## Reproducing it

`stage-portable.sh` emits two directories, and the split is not cosmetic:

```
build/arduino-portable/quorum_wcet/          the sketch: quorum_wcet.ino
build/arduino-portable/libraries/quorum/     every header, under src/quorum/...
```

The headers must be a **library**. The Arduino build never puts the sketch folder on the include path — verified by
reading the compiler command line it generates — so quoted includes resolve only relative to the including *file*. The
`.ino` could find `quorum/arbiter/verdict.hpp` while that header's own `"quorum/solver/framework.hpp"` could not,
because it was looked for at `quorum/arbiter/quorum/solver/`. A library's `src/` **is** on the include path, and that
is the only arrangement that works in the IDE as well as the CLI.

```bash
# 1. stage
bash firmware/arduino/stage-portable.sh

# 2. install the library where the IDE looks, and copy the sketch anywhere
cp -r build/arduino-portable/libraries/quorum  "$USERPROFILE/Documents/Arduino/libraries/"
cp -r build/arduino-portable/quorum_wcet       /some/work/dir/

# 3. build and flash (arduino-cli; the IDE works too)
arduino-cli core install arduino:renesas_uno
arduino-cli compile --fqbn arduino:renesas_uno:minima \
    --output-dir ./out /some/work/dir/quorum_wcet
arduino-cli upload -p COM3 --fqbn arduino:renesas_uno:minima --input-dir ./out
```

Then read the serial port at **115200**. The sketch prints once from `setup()` and `loop()` is empty, so a monitor
attached late sees nothing — start the monitor first, or reset the board while it is attached.

## Three things that went wrong, and are worth knowing before they go wrong for you

### 1. Vendor macros eat ordinary identifiers

Two collisions, both invisible until a real BSP was involved:

| quorum had | the vendor defines | result |
|---|---|---|
| `enum class Source { ..., SysTick }` | CMSIS: `#define SysTick ((SysTick_Type *) SysTick_BASE)` | the enumerator became a cast expression |
| `static constexpr uint32_t bit(int32_t)` | Arduino core: `#define bit(b) (1UL << (b))` | the *definition* became `uint32_t (1UL << (observation))` |

In both cases GCC reported the error inside the vendor header, tens of thousands of lines from the cause. Now
`Source::Systick` and `obs_bit()`, with the reason recorded at both definitions so it does not drift back.

**Why no test caught this.** The portability gate (`tools/arduino-portable-check.sh`) compiles the staged sketch for
cortex-m4 against a **stub** `Arduino.h` containing none of the vendor macros. That gate proves the tree is
self-contained, which is worth proving, and it cannot prove the code survives a real toolchain. A stub is only hostile
in the ways you remember to make it hostile.

### 2. The DFU driver is a separate install

The upload fails with `Cannot open DFU device 2341:0069 (LIBUSB_ERROR_NOT_FOUND)` until Windows has the driver, which
shows as `DFU-RT Port … Status: Error` in Device Manager. The Arduino package ships the installer; it needs
Administrator:

```powershell
cd "$env:LOCALAPPDATA\Arduino15\packages\arduino\hardware\renesas_uno\1.6.0"
.\post_install.bat
```

A failed upload can also leave the board not enumerating at all. Unplug and replug, or double-tap reset.

### 3. Cycles are not instructions

The bound is **2,571 instructions**; the board reports **cycles**. The first version of the staged instructions told the
reader that a measured maximum above 2,571 meant the machine model was wrong. It does not — a cycle count on real
silicon is normally higher, because a flash access that stalls costs several cycles and remains one instruction.

Worse, the first version of the sketch divided the **full decide cycle** by a **solve-only** bound and reported
`5.30 CPI -- README is UNDERSTATED`. Both scopes were wrong together: `solve` against the solve bound is 1.40 CPI and
passes. The bound covers exactly `quorum_solve(...) { return g.solve(fw); }` — see `tools/wcet-disassemble.sh`.

That was the third time a tool in this project had been unfair to correct work by mis-scoping a comparison. The pattern
is consistent enough to state as a rule: **when a tool compares two numbers, name the scope of each at the point of
comparison, and say which units they are in.**

## Read the margin carefully

| | margin on a 1 ms control cycle |
|---|---|
| `solve` bound, 150 MHz Cortex-M33 | 29x |
| `solve` measured, 48 MHz Cortex-M4 | 13x |
| **full decide cycle measured, 48 MHz Cortex-M4** | **3.5x** |

All three are true. The last is what an integrator budgets, and it is the one to quote.

## A learned casebook (`quorum_learned`)

Same check as `firmware/pico2/learned.cpp`: every one of 1,024 states of a casebook quorum's learner wrote, fingerprinted
against the host. Pass when the board prints the line in `build/learn/learned/monks-1-knows-2.expected`:

```
DEVICE monks-1-knows-2 states 1024 asserts 795 fnv1a 0x98512208
```

`stage-portable.sh` stages it beside `quorum_wcet`. Library detection does not find `quorum/...` headers (it matches
only a library's top-level headers), so pass the include path explicitly:

```
arduino-cli compile --fqbn arduino:renesas_uno:minima \
    --build-property "compiler.cpp.extra_flags=-IC:/quorum-arduino/libraries/quorum/src" \
    --output-dir C:\quorum-arduino\out-learned C:\quorum-arduino\quorum_learned
```

Uno R4 Minima: 43,156 bytes flash (16%), 6,112 bytes RAM (18%). `C:\quorum-arduino\learned.ps1` waits for the board,
uploads and captures to `serial-learned.txt`. Result 2026-10-01: PASS. The Uno R4 printed `DEVICE monks-1-knows-2 states 1024 asserts 795 fnv1a 0x98512208`,
identical to the host and the Pico 2; 0 not Decided, max 167 steps (bound 306), worst cold instantiate+decide
394.3 us at 48 MHz. Raw: `measured-learned-2026-10-01.txt`.
