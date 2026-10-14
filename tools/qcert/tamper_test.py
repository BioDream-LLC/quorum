#!/usr/bin/env python3
"""qcert must accept an honest certificate and reject every forgery below.

Usage: tamper_test.py <cbc> <qcert> <casebook.cb> <workdir>

Each forgery is the smallest edit that would make a false claim: one label, one round, one decision, one missing
or repeated state, one edge. A checker that accepts any of them is not checking.
"""
import os, re, subprocess, sys

cbc, qcert, cb, work = sys.argv[1:5]
os.makedirs(work, exist_ok=True)
cert = os.path.join(work, "honest.cert")
subprocess.run([cbc, cb, "--quiet", "--certificate", cert], check=True, stderr=subprocess.DEVNULL)
lines = open(cert).read().split("\n")
first_state = next(i for i, l in enumerate(lines) if l.startswith("s "))

def run(text):
    p = os.path.join(work, "t.cert")
    open(p, "w").write(text)
    r = subprocess.run([qcert, p], capture_output=True, text=True)
    return r.returncode, r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""

failures = 0
code, msg = run("\n".join(lines))
print(f"  honest certificate        -> {msg}")
if code != 0:
    failures += 1

def state_line(pred):
    return next(i for i in range(first_state, len(lines)) if lines[i].startswith("s ") and pred(lines[i].split()))

def edit(i, f):
    out = list(lines)
    out[i] = f(out[i])
    return out

forgeries = []
# A Decided state: claim its chosen action was some other action.
i = state_line(lambda t: t[2] == "0")
acts = next(l for l in lines if l.startswith("actions ")).split()[2:]
t = lines[i].split()
other = next(a for a in acts if a != t[3])
forgeries.append(("decided action swapped", edit(i, lambda l: " ".join(t[:3] + [other] + t[4:]))))
forgeries.append(("outcome changed", edit(i, lambda l: " ".join(t[:2] + ["1"] + t[3:]))))
# Flip the first In label in that state to Out (keeping its round).
def flip(l, src, dst):
    parts = l.split()
    for k in range(4, len(parts)):
        if parts[k].startswith(src):
            parts[k] = dst + parts[k][1:] if dst != "U" else "U"
            return " ".join(parts)
    raise SystemExit("no label to flip")
forgeries.append(("one In label flipped to Out", edit(i, lambda l: flip(l, "I", "O"))))
forgeries.append(("one Out label flipped to In", edit(i, lambda l: flip(l, "O", "I"))))
forgeries.append(("one label erased to Undecided", edit(i, lambda l: flip(l, "O", "U"))))
# Make a round impossible: an In argument labelled at round 1 although it is attacked.
def bad_round(l):
    parts = l.split()
    for k in range(4, len(parts)):
        if re.fullmatch(r"I([2-9]|\d\d+)", parts[k]):
            parts[k] = "I1"
            return " ".join(parts)
    raise SystemExit("no attacked In argument")
forgeries.append(("round moved earlier", edit(i, bad_round)))
forgeries.append(("state deleted", lines[:i] + lines[i + 1:]))
forgeries.append(("state duplicated", lines[:i + 1] + [lines[i]] + lines[i + 2:]))
e = next(k for k, l in enumerate(lines) if l.startswith("e "))
forgeries.append(("one attack redirected", edit(e, lambda l: "e " + l.split()[1] + " " + str(int(l.split()[2]) + 1))))
forgeries.append(("fallback changed", [l if not l.startswith("fallback ") else "fallback " + acts[0] for l in lines]
                  if next(l for l in lines if l.startswith("fallback ")).split()[1] != acts[0] else
                  [l if not l.startswith("fallback ") else "fallback " + acts[1] for l in lines]))
idle = next(l for l in lines if l.startswith("idle ")).split()[1]
forgeries.append(("idle action changed", [l if not l.startswith("idle ") else
                  "idle " + next(a for a in acts if a != idle) for l in lines]))
t = next((k for k, l in enumerate(lines) if l.startswith("t ")), None)
if t is not None:  # a setpoint nudged: the kind of edit that looks like a typo and trips (or fails to trip) a plant
    forgeries.append(("setpoint changed", edit(t, lambda l: " ".join(l.split()[:4] + [str(int(l.split()[4]) + 16)]
                                                                         + l.split()[5:]))))
forgeries.append(("truncated", lines[: len(lines) // 2]))

# Pinned to the device: the honest certificate against the right and a wrong device fingerprint.
fp = next(l for l in lines if l.startswith("fingerprint ")).split()[1]
p = os.path.join(work, "t.cert")
open(p, "w").write("\n".join(lines))
for label, want_ok, arg in (("device fingerprint matches", True, fp), ("device fingerprint differs", False, "0x12345678")):
    r = subprocess.run([qcert, p, arg], capture_output=True, text=True)
    msg = r.stdout.strip().splitlines()[0]
    print(f"  {label:<26}-> {msg}")
    if (r.returncode == 0) != want_ok:
        failures += 1
        print(f"  FAIL  {label}")

# Since certificate v2 the fingerprint covers the fallback, the idle action and the binding, so each of those edits
# is refused even in a casebook where no state falls back.
for what, text in forgeries:
    code, msg = run("\n".join(text))
    ok = code != 0 and msg.startswith("REJECTED")
    print(f"  {what:<26}-> {msg}")
    if not ok:
        failures += 1
        print(f"  FAIL  qcert accepted a forgery: {what}")

print(f"{len(forgeries)} forgeries, {failures} failures")
sys.exit(1 if failures else 0)
