# Running on hardware

The runtime firmware also prints a GUARD line and answers Modbus RTU once a valid request arrives: see
[runtime/GUARD_AND_MODBUS.md](../../runtime/GUARD_AND_MODBUS.md), including why Modbus belongs only on a link you control.

**Status, 2026-09-29: measured.** This firmware has run on a Raspberry Pi Pico 2 (RP2350, Cortex-M33 at 150 MHz) —
the core and clock the static bound was derived from — three times. Results are in the README's *Measured* section and
the raw serial capture is [`measured-2026-09-29.txt`](measured-2026-09-29.txt).

| | |
|---|---|
| the device does the work the model predicts | **settled** — 140 steps on device, 140 predicted, every run |
| the hardware picks the same worst state as `cbc` | **settled** — a 16,384-state cold-cache sweep on silicon agrees with `kWorstCaseMask` |
| the bound holds on the core it was derived from | **settled** — `solve` slowest of three runs 3,719 cycles, 1.45 CPI against the assumed 2 |
| the memory system, QSPI flash through a cache | **measured, not analysed** — see below |

The memory system is the one row that changed character rather than closing. `solve`'s fastest run was 2,809 or 2,810
cycles on every run, because the algorithm is deterministic; its slowest moved between 3,466 and 3,719, because the
cache is not. A cold cache costs a further 1.4–1.6x on the full cycle. The Uno R4, with no cache, gave min = max
exactly. So on this part **a measured maximum is not a stable number**, which is why the instruction bound is the figure
to budget and the measurements are evidence that it is not optimistic. Nothing here analyses cache behaviour
statically; an integrator who needs a true WCET for this part needs that, or needs to run from SRAM.

## What actually happened, so the next run is shorter

The build and flash worked first time. Everything that went wrong was around them.

- **The Pico SDK needs a host toolchain as well as the cross-compiler.** It builds `pioasm` and `picotool` for the
  host during a target build. `quorum-arm:1` is a bare cross-compiler, so there is now a [`Dockerfile`](Dockerfile)
  for `quorum-pico:1` that adds cmake, g++, python3 and libusb. SDK pinned to 2.1.1, mounted rather than baked in.
- **`--depth 1` does not fetch `lib/tinyusb`,** and `pico_enable_stdio_usb` will not build without it.
  `git submodule update --init --depth 1 lib/tinyusb`.
- **WSL cannot see the `RP2350` drive,** and mounting it needs sudo. PowerShell can: stage the `.uf2` on `C:` and
  `Copy-Item` it to the drive. Windows often reports a failure on that copy, because the board reboots the instant the
  write lands; that is normal.
- **No button press is needed after the first flash.** Opening the firmware's USB serial port at **1200 baud** makes
  `pico_stdio_usb` reboot into BOOTSEL, the drive appears, and copying the `.uf2` reboots into the new firmware.
  `Open()` throws "A device attached to the system is not functioning" while this happens; the reset works anyway.
  Reset, flash and capture together take about two seconds.
- **The capture must be listening before the board boots.** The firmware waits 2 s and then prints once. And
  `System.IO.Ports.SerialPort` needs `DtrEnable = $true`, or a CDC device may never send.
- **"USB device not recognized" / `VID_0000&PID_0002` was the hub, not the board — and cost most of a day.** A
  Genesys USB 2.0 hub (`VID_05E3&PID_0608`) latched into a state where it enumerated itself but failed every device
  behind it. The diagnosis went through the reset mechanism, the cable and the silicon before plugging the Uno R4 into
  the same hub and watching it fail identically. A PC reboot cleared it. **When a device fails to enumerate, try a
  second, unrelated device on the same path before theorising about the first.**

## Reporting errors the board exposed

Two, in the firmware's own printout, of the shape recorded as PROC-D18 — each made the result look worse or better
than it was:

- The sweep divided a **full cycle** by the **solve-only** 2,571-instruction bound and printed 8.15 cycles per
  instruction. The Uno R4 sketch had made the identical mistake and been fixed the same afternoon; this copy was not,
  because the logic is duplicated across two firmware trees. `solve` against its own bound is 1.45, and passes.
- The headroom lines were computed from the **warm pump** cycle while the same printout showed the tank cycle with
  binding and the cold-cache sweep both slower, overstating the margin by half. They now compare every candidate and
  name the one they use.

## Why this was the last open claim

The certification argument for putting a reasoner in a safety function runs: grounded semantics is polynomial, the
implementation has a proven step bound of `2n + 2m`, and exhaustive validation shows every state of every casebook
stays inside it. All of that was established before any board existed. None of it says how long a step takes.

That is the difference between "bounded" and "fast enough", and only the part could answer it. It has now: the bound
holds on this core at 1.45 CPI against the 2 assumed.

## What to buy

| item | price | why |
|---|---|---|
| **Raspberry Pi Pico 2** (SC1631) | ~$11 on Amazon, $5 direct | RP2350, Cortex-M33, 520 KB SRAM. The target the footprint numbers are against. |
| a micro-USB data cable | — | the Pico 2 is micro-USB B, not USB-C. Flashing and results both go over it. Plug it into the PC directly: a failing hub produced a day of `Device Descriptor Request Failed`. |

That is the whole list. **No serial adapter, no soldering, no debug probe.**

An earlier version of this document required a USB-serial adapter wired to GP0/GP1, because the firmware used UART
stdio: a USB CDC stack's interrupts would land inside the timed window and inflate the maximum, and a maximum is the
entire statistic a WCET measurement reports. That concern was real and the answer was wrong. It imposed hardware and
soldering, and it only excluded the interrupts someone happened to think of — a SysTick tick or a DMA completion would have
sailed straight through.

`bench::InterruptGuard` masks interrupts around each individual timed run instead, restoring `PRIMASK` rather than
blanket-enabling so it nests safely. That fixes the measurement rather than the wiring, and works the same on every
Cortex-M. Masking is per-run, not around the whole 2000-run loop, because holding interrupts off that long on a real
board means dropped USB and possibly a watchdog reset.

Optional, and genuinely useful rather than necessary:

| item | price | why |
|---|---|---|
| **Arduino Uno R4 Minima** | ~$20 | a second vendor at 48 MHz against the Pico 2's 150 MHz. Close cycle counts with ~3x different microsecond figures would show the numbers measure quorum, not RP2350. **Confirmed:** `solve` 3,608 cycles on the R4 against 2,809–3,719 on the Pico 2, and 75.2 us against 24.8 us is 3.0x. See `firmware/arduino/`. |
| Raspberry Pi Debug Probe | ~$12 | SWD debugging if something goes wrong at the SDK layer. Plugs into the Pico 2's JST-SH debug connector with no soldering. Not needed for the measurement. |

## Build and flash

```sh
# 1. Host build first. cbc is a host tool, and its exhaustive validation is what finds the
#    worst-case state the firmware measures -- without this the generated headers do not exist.
cd ~/quorum
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build

# 2. Pico SDK 2.1.1 (first-generation RP2350 support) and the one submodule stdio_usb needs
git clone --depth 1 --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
git -C ~/pico-sdk submodule update --init --depth 1 lib/tinyusb

# 3. Firmware, in the build image
docker build -t quorum-pico:1 - < firmware/pico2/Dockerfile
docker run --rm -v ~/quorum:/src -v ~/pico-sdk:/pico-sdk -w /src/firmware/pico2 quorum-pico:1 bash -c \
  'cmake -S . -B /tmp/pb -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/pb -j &&
   cp /tmp/pb/quorum_wcet.uf2 /src/build/'

# 4. Flash: hold BOOTSEL, plug in USB, release. The board appears as a USB drive.
#    (From WSL: stage on C: and Copy-Item from PowerShell. After the first flash, a 1200-baud
#    open of the serial port does the same thing without touching the board.)
cp build/quorum_wcet.uf2 /media/$USER/RP2350/

# 5. Read it -- same USB cable, no extra hardware. The board re-enumerates as a serial device.
screen /dev/ttyACM0 115200        # or: minicom -D /dev/ttyACM0 -b 115200
```

UART on GP0/GP1 at 115200 is also enabled, for a board already wired into a rig.

## What you should see

```
quorum on RP2350 -- worst-case execution time
=============================================
core clock            150000000 Hz
casebook              centrifugal-pump v3
worst-case state      0x0  (140 steps, proven bound 248)
runs per measurement  2000

cycle counter: DWT CYCCNT (32-bit)

pump casebook, worst-case state 0x0
  instantiate (build + finalize)     min ...  mean ...  max ... cycles   ... us
  solve (grounded fixpoint)          min ...  mean ...  max ... cycles   ... us
  FULL CYCLE (instantiate+decide)    min ...  mean ...  max ... cycles   ... us

  steps on device 140, predicted by cbc 140  MATCH
```

**Check the MATCH line first.** If the device executes a different number of steps than the compiler predicted, the
timing numbers are measuring something other than the validated casebook, and nothing below them means anything.

### Why the worst-case state is `0x0`

That is the state where **nothing is asserted**, and it is the worst case because every closed-world edge is then
live, so the framework carries its maximum number of attacks — 80 for the pump, against 66 fixed ones.

It is also a healthy machine's most common state. The worst case is not a rare corner to be discounted; it is what
the device does all day. That is convenient for certification and worth knowing before someone argues for an average.

`cbc` found this by solving all 16,384 states, and emits it as `kWorstCaseMask` so the firmware measures it rather
than a state somebody picked.

## If it does not work

The failure modes are ordered by likelihood, and the reasoning for each is given because a list of things to try is
less useful than knowing what each one would prove.

**No serial device appears.** The board needs a moment to re-enumerate after flashing; `/dev/ttyACM0` may take a
second or two. If it never appears, confirm the board runs at all by building a stock SDK `hello_world` and flashing
that. If `hello_world` enumerates and this does not, the problem is in `firmware/pico2/`; if neither does, it is the
cable, the hub, or the board — in that order of likelihood, and a second unrelated USB device on the same port tells
the first two apart from the third in a minute.

**`cycle counter: SysTick` instead of DWT.** Expected and fine. The DWT cycle counter is implementation-dependent —
the Cortex-M33 Technical Reference Manual says so explicitly — and the harness checks the `NOCYCCNT` bit and falls
back to SysTick, which is architecturally mandated. Both count core clock cycles. SysTick is 24-bit, which is 112 ms
at 150 MHz and far more than any measurement here needs.

**`FATAL: the DWT cycle counter will not start`.** Neither counter advanced. The firmware deliberately refuses to run
rather than print zeroes, because a zero cycle count reads as an impossibly fast result rather than as a broken rig.
If this happens, something is holding the debug block; try without a debugger attached.

**`*** MISMATCH ***` on the steps line.** The most interesting failure, and the only one that would be a real finding.
It would mean the same casebook produces different work on ARM than on x86-64, which should be impossible: the solver
is integer-only, allocation-free, and the framework is built from the same constant tables. Do not adjust anything —
capture the output and treat it as a defect in the engine.

**Times far larger than expected.** Check whether the SDK's `-O0` default crept in; the CMakeLists forces `-Os`. Also
check nothing else is running: the numbers are for one core with no interrupts.

## What the numbers will and will not mean

They will be the cost of adjudicating a casebook at its worst state on one Cortex-M33 core at the printed clock, with
no interrupts and nothing else running.

They will **not** include reading real instruments. On a slow Modbus link a single register read can take milliseconds
and will dominate everything measured here. The reasoning is not the bottleneck in a real plant, and this measurement
is about establishing that rather than about being impressive.

## After the measurement

The number exists now, which makes two things possible.

A **stated cycle time** with headroom, which is what a functional safety assessment asks for: not "it is fast" but
"the worst case is N microseconds at a 10 ms cycle, giving a margin of M". Measured: the worst full cycle is 145 us,
the pump with a cold cache — 6.9x on a 1 ms cycle, 69x on 10 ms. That is a measurement on a cached part, so it states
what was observed, not a guarantee.

And a **second measurement worth taking**: the same casebook on a part without an FPU and without a cache, to show
the numbers are not resting on RP2350-specific behaviour. An RP2040 Pico 1 at $4 is the obvious candidate and the
firmware should need only `-DPICO_BOARD=pico`; note that the Cortex-M0+ has no DWT at all, so the SysTick fallback
stops being a contingency and becomes the mechanism.

## A learned casebook (`quorum_learned`)

`learned.cpp` runs a casebook that quorum's learner wrote (MONK-1, starting from a casebook that knew one of the two
published rules; 45 arguments, 98 attacks, 10 observations). It decides all 1,024 states, times each one cold, and
prints, every 3 s:

```
DEVICE monks-1-knows-2 states 1024 asserts 795 fnv1a 0x98512208
```

That line must equal `build/learn/learned/monks-1-knows-2.expected`, which the host writes from the learner itself.
The same header already passes that comparison on the host CPU on every build (`ctest -R learn_device_header`).

```bash
ctest --test-dir build -R learn_casebook_e2e          # writes build/learn/learned/*.cb and *.expected
./build/casebook/compiler/cbc build/learn/learned/monks-1-knows-2.cb \
    --header build/learn/learned/monks_learned_generated.hpp
docker run --rm -v ~/quorum:/src -v ~/pico-sdk:/pico-sdk -w /src/firmware/pico2 quorum-pico:1 bash -c \
  'cmake -S . -B /tmp/pb -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/pb -j && cp /tmp/pb/quorum_learned.uf2 /src/build/'
cp build/quorum_learned.uf2 /mnt/c/quorum-pico/
powershell.exe -File 'C:\quorum-pico\cycle.ps1' -Uf2Name quorum_learned.uf2 -Seconds 120 -WaitForBoardSeconds 14400
```

`-WaitForBoardSeconds` makes `cycle.ps1` wait until the board enumerates, then flash and capture by itself.

**Result 2026-10-01: PASS.** After a PC reboot cleared the hub, the board printed `DEVICE monks-1-knows-2 states 1024
asserts 795 fnv1a 0x98512208`, identical to the host; 0 not Decided, max 167 steps (bound 306), worst cold
instantiate+decide 170.6 us at 150 MHz. Raw: `measured-learned-2026-10-01.txt`.

Earlier status, 2026-09-30: built, not yet run on silicon. The first attempt found COM4 listed but unopenable ("A device
attached to the system is not functioning"), then `VID_0000&PID_0002 Device Descriptor Request Failed` under the
Genesys hub `VID_05E3&PID_0608` -- the same hub fault as before, which a replug directly into the PC (or a PC
reboot) cleared last time. `pnputil /scan-devices` needs administrator rights, so it could not be reset from here.
A watcher was left armed for 4 hours.
