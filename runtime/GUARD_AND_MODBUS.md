# The AI guard and Modbus

Two ways for quorum to sit beside the equipment that already runs a plant. Both need nothing new in the image:
every image cbc has ever built supports them.

- **The guard** lets another controller, such as an AI model or a vendor's black box, *propose* an action. quorum
  decides from the approved rulebook as usual and acts on the proposal only if it is no less cautious.
- **Modbus RTU** is the serial protocol PLCs already speak. A PLC writes what it sees and reads back quorum's
  decision and the guard's verdict.

Source: `include/quorum/runtime/guard.hpp`, `modbus.hpp` (protocol), `modbus_map.hpp` (registers).
Master for testing: `tools/modbus/qmodbus.ps1`. Tests: `test_guard`, `test_modbus`.

## The guard

| proposal | result | reason code |
|---|---|---|
| the rulebook's own decision | accepted | 0 Agrees |
| outranks the decision in the rulebook's `prefer` order | accepted | 1 MoreCautious |
| the decision outranks it | **replaced** by the decision | 2 LessCautious |
| the rulebook ranks neither above the other | **replaced** | 3 Incomparable |
| not one of this rulebook's actions | **replaced** | 4 NotAnAction |
| — the rulebook did not decide | its fallback stands unless the proposal outranks it | 5 NoDecision |

"Outranks" comes from the rulebook itself: the attacks between actions that `prefer` generates. So **the output is
never less cautious than the rulebook acting alone**, in every state, whatever is proposed. That holds by
construction. It is still checked: `qblob sweep` on the host and every board at start-up try every state with every
possible proposal and print one line, for example:

```
GUARD centrifugal-pump pairs 49152 accepted 46326 replaced 2826 weaker 0 fnv1a 0xac18f721
```

`weaker` must be 0. Equal lines on the host and the board mean the board guarded every pair exactly as the host did.
Measured on both boards: Uno R4, `firmware/arduino/measured-guard-modbus-2026-10-09.txt`; Pico 2,
`firmware/pico2/measured-guard-modbus-2026-10-10.txt`.

What it does not do:

- **It does not make the other controller safe.** It makes the *combination* no less cautious than the rulebook,
  and the rulebook is what was checked. If the rulebook is wrong, the guard enforces the wrong thing.
- **It is only as cautious as the `prefer` order says.** The guard accepts anything the rulebook ranks above its
  decision, including actions ranked above the safe action. In the tank casebook that action is
  `stop_and_evacuate`, which includes stopping the transfer, so accepting it where the rulebook says stop is more
  cautious. (It used to be called `evacuate`, which did not promise the transfer stopped; it was renamed for this
  reason.) The guard checks the order, not the meaning of a name: whoever approves a rulebook must check that every
  action ranked above the safe one really includes it.
- A proposal outside the rulebook's actions is never passed through, however confident the model is.

## Modbus

Slave address 1. Functions 03 (read holding registers), 06 (write one) and 16 (write several). Any other function
gets exception 01, an address outside the map 02, and a bad value 03. A frame with a wrong CRC is dropped without a
reply, as the standard requires. Broadcasts (address 0) are acted on but never answered.

**How a board enters Modbus mode.** At start-up a board prints its text report over the USB serial port, then
repeats it. The first *valid* request (correct CRC, addressed to slave 1 or broadcast) switches it to Modbus mode
until it is reset, and the text stops so it cannot corrupt the master's framing. Stray bytes are ignored. Before
2026-10-10 any byte switched the mode, and a byte sent by Windows after the USB port appeared silenced the Pico.
The first request may be lost if it arrives mid-report, so masters retry, as `qmodbus.ps1` does.

### Register map, version 2

Addresses are 0-based 16-bit holding registers. Actions are given by their **position** in the rulebook's action
list (`qblob` prints it). For the pump that is 0 `continue`, 1 `maintain`, 2 `shutdown`.

| address | meaning | access |
|---|---|---|
| 0 | `0x5155` ("QU") | read |
| 1 | map version, 2 (version 2 added registers 10 to 12) | read |
| 2, 3 | fingerprint, high then low 16 bits: the number the board prints and cbc reports | read |
| 4 | observation count | read |
| 5 | member count (1 unless a palette) | read |
| 6 | channel count | read |
| 7 | evaluation counter | read |
| 8, 9 | presence mask, high then low: the readings that are true, for observations no channel drives | read/write |
| 10 | test clock: 1 if on. Write `0x7E57` to turn it on (see below) | read/write |
| 11, 12 | how far the test clock has moved the board's clock, ms, high then low. Write both in one function 16 to move it forward | read/write |
| 16 + 16·m | member m: proposed action position, `0xFFFF` = none | read/write |
| +1 | outcome: 0 Decided, 1 Indeterminate, 2 Unsatisfied, 3 Conflicted, 4 Faulted | read |
| +2 | the rulebook's action position (the fallback when not Decided) | read |
| +3 | 1 if that action is the fallback | read |
| +4 | **the action to take**, after the guard (= +2 when nothing is proposed) | read |
| +5 | 1 if the proposal was accepted | read |
| +6 | guard reason (table above), `0xFFFF` when nothing is proposed | read |
| +7 | solver steps | read |
| +8 | action count | read |
| 256 + 2·c | channel c: a raw sample in milli-units, int32, high word first. Write both words in one function 16 | write |

Each read is answered from a fresh decision made at the time of the request. Time alone changes a rulebook with
channels: a `for 5s` hold matures, and a channel the PLC stops writing goes stale on schedule. Raw samples go
through quorum's own binding, with its ranges, hysteresis, holds and staleness, so the PLC does not decide what
counts as "hot". A write is checked in full before anything changes, so a rejected write has no partial effect.

A PLC's loop is: write the presence mask and/or channel samples, optionally write a proposal, read +1..+6, and act
on +4.

**The test clock** lets a bench test a rulebook's timed readings without waiting: a 1-hour window takes seconds. Writing
`0x7E57` to register 10 turns it on; then each function-16 write of N to registers 11 and 12 moves the board's clock
N ms forward, at most 24 days at a time. The board steps its timed readings just before and just after each move, as
real time would. Three rules keep it out of service:

- It can be turned on only before anything else has been written since start-up. A controller in service writes
  readings from its first scan, so after that the key is refused until the board is reset.
- Once on, it stays on until the board is reset, and register 10 reads 1. **A controller should read register 10 at
  start-up and refuse to run if it is not 0.**
- Moving the clock only moves it forward, and it changes when things happen, never which rules apply: every answer
  the board gives is still one of the answers exhaustive validation checked.

Anyone who can reset the board and send it the key before the controller's first write can turn it on. That is the
same person who could write a different image to it, which is a bigger power.

### Trying it: `tools/modbus/qmodbus.ps1`

PowerShell, no dependencies. Stage it on C: when working from WSL.

```
qmodbus.ps1 -Port COM4 -Command info            identity, fingerprint, current decision
qmodbus.ps1 -Port COM4 -Command sweep           every state through the registers; prints a MODBUS line
qmodbus.ps1 -Port COM4 -Command guard -State 9  propose each action in the quiet state and in state 0x9
qmodbus.ps1 -Port COM4 -Command held            held-demo image: a 5 s hold, an instant trip, a stale sensor
```

`sweep` prints `MODBUS <casebook> states N fnv1a 0x...`, and `test_modbus` prints the same line on the host. For the
pump, both boards gave `0x074d5609` in all 16,384 states, at 2.4 ms per state on the Uno and 2.0 ms on the Pico 2,
counting the round trip.

### Security: Modbus has no login

Modbus RTU has no authentication and no encryption. Anything that can send bytes on the link can:

- write the presence mask or channel samples, which means **choosing what quorum decides**;
- write a proposal;
- read every register.

The guard cannot help here. A writer who fakes the readings changes the rulebook's own decision, which the guard
then enforces faithfully. So:

- Use it only on a link you control: a USB cable or a dedicated serial line between the board and one PLC. Never
  bridge it onto a shared network through a Modbus/TCP gateway without a firewall or an authenticated tunnel in
  front.
- Treat the board's decision as no more trustworthy than whatever writes its inputs.
- Read the fingerprint (registers 2–3) at connection time and refuse to proceed if it is not the approved
  rulebook's. That proves which rulebook is loaded, including its fallback and every setpoint, not who is
  writing to it.

Current boards speak Modbus over the USB serial port only. An RS-485 bus would need a transceiver and firmware
changes for its UART, which are not done.
