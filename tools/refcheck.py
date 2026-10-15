#!/usr/bin/env python3
"""refcheck: a second, independent implementation of the casebook language, compared with cbc on every state.

qcert re-checks cbc's certificate, and the CaDiCaL cross-check re-solves cbc's framework. Both start from the
framework cbc built, so neither can catch a mistake in cbc's parser or lowering: a rule read wrongly would be checked
perfectly. This file starts from the .cb text instead. It was written from the language documentation
(casebook/LANGUAGE.md, AUTHORING.md, TAXONOMY.md), not from cbc's source, and it does not build an argumentation
framework at all. It evaluates a three-valued fixed point directly over the declarations:

  reading     In if present and nothing that undermines it is In; Out if absent or something undermining it is In
  claim etc.  its evidence (needs = all, when any = some, when all = all, vote k of = at least k) must hold, and
              every attack on it must have failed; it is Out as soon as its evidence fails or an attack succeeds
  doubt       its evidence is the RAW reading, so it can fire on a reading it is about to discard
  action      as a claim, plus: the idle action needs no evidence and loses to every other action; `prefer a over b`
              means a, when In, defeats b
  severity    a rebut or undermine from a claim with a lower severity onto one with a higher severity is cancelled,
              when both declare one; an undercut is never cancelled
  generated   `implies a b` adds the doubt mismatch__a__b (raw a present, raw b absent; it discards a);
              `exclusive a b ...` adds a doubt that fires when all are present and discards all of them

Starting from everything undecided and repeating to a fixed point gives the least fixed point, which is what the
grounded semantics is. The decision rule is the arbiter's: exactly one action In is Decided; two or more is
Conflicted; none In with one undecided is Indeterminate; otherwise Unsatisfied; when not Decided the safe action is
taken.

    refcheck.py <cbc> <casebook.cb> [more.cb ...]
    refcheck.py <cbc> --fuzz [count] [seed]     random rulebooks instead: those cbc accepts are compared

For each casebook it runs `cbc --certificate --commissioning`, takes cbc's outcome and action for every state from
the certificate, computes its own, and reports every state where they differ. Exit status 1 if any does.
"""
import os
import re
import shlex
import subprocess
import sys
import tempfile
from itertools import combinations

IN, OUT, UND = 1, -1, 0
# REFCHECK_BREAK=<name> deliberately breaks one rule, to show that the comparison notices (tools/refcheck_test.sh).
BREAK = os.environ.get("REFCHECK_BREAK", "")
OUTCOMES = ["Decided", "Indeterminate", "Unsatisfied", "Conflicted", "Faulted"]


def logical_lines(text):
    """Join indented continuation lines; drop comments and blank lines."""
    out = []
    for raw in text.splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        if raw[0] in " \t" and out:
            out[-1] += " " + s
        else:
            out.append(s)
    return out


def words(line):
    """Split, dropping quoted descriptions."""
    return [w for w in shlex.split(line, posix=True)] if '"' in line else line.split()


KEYS = {"needs", "when", "undermines", "rebuts", "undercuts", "severity", "vote", "unit", "range", "stale_after",
        "flatline_after", "epsilon", "asserts", "above", "below", "release", "for"}


class Node:
    def __init__(self, name, kind):
        self.name, self.kind = name, kind
        self.all = []      # every one must be In
        self.any = []      # list of groups: in each, at least one must be In
        self.vote = None   # (k, items)
        self.severity = None
        self.idle = self.safe = False
        self.raw = False   # evidence read raw (doubts)


def parse(text):
    obs, nodes, attacks, prefer = [], {}, [], []  # attacks: (from, kind, to)
    channels = []
    for line in logical_lines(text):
        # descriptions may contain anything; remove them before splitting
        bare = re.sub(r'"(?:[^"\\]|\\.)*"', " ", line)
        t = bare.split()
        head = t[0]
        if head in ("casebook", "source", "note"):
            continue
        if head == "observe":
            obs.append(t[1])
            continue
        if head == "channel":
            name = t[1]
            channels.append((name, "flatline_after" in t))
            rest = t[2:]
            for i, w in enumerate(rest):
                if w == "asserts":
                    obs.append(rest[i + 1])
            continue
        if head == "exclusive":
            n = Node("exclusive:" + "+".join(t[1:]), "doubt")
            n.all, n.raw = list(t[1:]), True
            nodes[n.name] = n
            for x in t[1:]:
                attacks.append((n.name, "undermines", x))
            continue
        if head == "implies":
            a, b = t[1], t[2]
            n = Node("mismatch__%s__%s" % (a, b), "doubt")
            n.all, n.raw = [a, "!" + b], True
            nodes[n.name] = n
            attacks.append((n.name, "undermines", a))
            continue
        if head == "prefer":
            chain = [x for x in t[1:] if x != "over"]
            prefer.append(chain)
            continue
        if head in ("claim", "excuse", "doubt", "action"):
            n = Node(t[1], head)
            n.raw = head == "doubt"
            key, cur = None, []

            def close():
                if key == "needs":
                    n.all += cur
                elif key == "when":
                    if cur and cur[0] == "all":
                        n.all += cur[1:]
                    elif cur and cur[0] == "any":
                        n.any.append(cur[1:])
                    elif cur:
                        n.all += cur  # a single bare condition
                elif key == "vote":
                    n.vote = (int(cur[0]), cur[2:])  # vote k of a b c
                elif key == "severity":
                    n.severity = int(cur[0])
                elif key == "undermines":
                    for x in cur:
                        attacks.append((n.name, "undermines", x))

            for w in t[2:]:
                if w in ("needs", "when", "vote", "severity", "undermines"):
                    close()
                    key, cur = w, []
                elif key is None:
                    n.idle |= w == "idle"
                    n.safe |= w == "safe"
                else:
                    cur.append(w)
            close()
            nodes[n.name] = n
            continue
        if len(t) >= 3 and t[1] in ("rebuts", "undercuts", "undermines"):
            for x in t[2:]:
                attacks.append((t[0], t[1], x))
            continue
        raise ValueError("refcheck does not understand: " + line)
    for c, flat in channels:
        obs += [c + "_unavailable"] + ([c + "_flatline"] if flat else [])
    return obs, nodes, attacks, prefer, channels


def evaluate(obs_present, nodes, attacks, prefer, actions):
    """The least fixed point for one state. obs_present: dict reading -> bool."""
    lab = {n: UND for n in nodes}
    usable = {r: UND for r in obs_present}  # a reading once doubts are taken into account

    attackers = {}
    for f, k, to in attacks:
        attackers.setdefault(to, []).append((f, k))
    beats = {}
    for chain in prefer:
        for i, a in enumerate(chain):
            for b in chain[i + 1:]:
                beats.setdefault(b, set()).add(a)
    idle = [a for a in actions if nodes[a].idle]

    def val(x, raw):
        if x.startswith("!"):
            v = val(x[1:], raw)
            return -v
        if x in obs_present:
            if raw and BREAK != "doubts-read-usable":
                return IN if obs_present[x] else OUT
            return usable[x]
        return lab[x]

    def ev(n):
        terms = []
        for x in n.all:
            terms.append(val(x, n.raw))
        for g in n.any:
            vs = [val(x, n.raw) for x in g]
            if BREAK == "any-is-all":
                terms += vs
                continue
            terms.append(IN if IN in vs else (OUT if all(v == OUT for v in vs) else UND))
        if n.vote:
            k, items = n.vote
            vs = [val(x, n.raw) for x in items]
            ins, outs = vs.count(IN), vs.count(OUT)
            k = k + 1 if BREAK == "vote-off-by-one" else k
            terms.append(IN if ins >= k else (OUT if len(vs) - outs < k else UND))
        if not terms:
            return IN
        if OUT in terms:
            return OUT
        return IN if all(v == IN for v in terms) else UND

    def cancelled(f, k, to):
        if k == "undercuts" and BREAK != "undercut-cancels":
            return False
        if BREAK == "no-severity":
            return False
        a, b = nodes.get(f), nodes.get(to)
        if a is None or b is None or a.severity is None or b.severity is None:
            return False
        return a.severity < b.severity

    for _ in range(10 * (len(nodes) + len(obs_present)) + 10):
        changed = False
        for r, present in obs_present.items():
            if usable[r] != UND:
                continue
            if not present:
                v = OUT
            else:
                att = [val(f, False) for f, k in attackers.get(r, []) if k == "undermines"]
                v = IN if all(a == OUT for a in att) else (OUT if IN in att else UND)
            if v != UND:
                usable[r], changed = v, True
        for name, n in nodes.items():
            if lab[name] != UND:
                continue
            e = ev(n)
            att = [val(f, False) for f, k in attackers.get(name, []) if not cancelled(f, k, name)]
            if n.kind == "action":
                att += [lab[a] for a in beats.get(name, ())] if BREAK != "no-prefer" else []
                if n.idle:
                    att += [lab[a] for a in actions if a not in idle]
            if e == OUT or IN in att:
                v = OUT
            elif e == IN and all(a == OUT for a in att):
                v = IN
            else:
                v = UND
            if v != UND:
                lab[name], changed = v, True
        if not changed:
            break
    ins = [a for a in actions if lab[a] == IN]
    safe = [a for a in actions if nodes[a].safe][0]
    if len(ins) == 1:
        return "Decided", ins[0]
    if len(ins) > 1:
        return "Conflicted", safe
    if any(lab[a] == UND for a in actions):
        return "Indeterminate", safe
    return "Unsatisfied", safe


def check(cbc, path):
    text = open(path, encoding="utf-8").read()
    obs, nodes, attacks, prefer, _ = parse(text)
    with tempfile.TemporaryDirectory() as d:
        cert, sheet = os.path.join(d, "c"), os.path.join(d, "s")
        r = subprocess.run([cbc, path, "--quiet", "--certificate", cert, "--commissioning", sheet],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None, "cbc refused it"
        order = None
        bits = {}
        for line in open(sheet):
            f = line.rstrip("\n").split("\t")
            if f[0] == "actions":
                order = f[1:]
            if f[0] == "row":
                bits[int(f[1])] = f[2]
        cert_actions, rows = None, []
        for line in open(cert):
            if line.startswith("actions "):
                cert_actions = [int(x) for x in line.split()[2:]]
            elif line.startswith("s "):
                p = line.split()
                rows.append((int(p[1]), int(p[2]), int(p[3])))
    names = [bits[b] for b in sorted(bits)]
    missing = set(names) ^ set(obs)
    if missing:
        return None, "the readings disagree: " + ", ".join(sorted(missing))
    position = {a: i for i, a in enumerate(cert_actions)}
    differ = []
    for mask, outcome, action in rows:
        present = {names[b]: bool(mask >> b & 1) for b in range(len(names))}
        mine = evaluate(present, nodes, attacks, prefer, order)
        theirs = (OUTCOMES[outcome], order[position[action]])
        if mine != theirs:
            differ.append((mask, theirs, mine))
    return len(rows), differ


def random_casebook(rng, i):
    """A random rulebook in the language, built to have a fair chance of passing cbc's checks."""
    n_obs = rng.randint(3, 9)
    obs = ["r%d" % k for k in range(n_obs)]
    L = ["casebook fuzz%d v1" % i, 'source "random" dated 2026-10-14 by "refcheck"']
    L += ['observe %s "reading %s"' % (o, o) for o in obs]
    pres = []
    for k in range(rng.randint(2, 6)):
        kind = rng.choice(["claim", "claim", "claim", "excuse", "doubt"])
        name = "%s%d" % (kind[0], k)
        pool = obs + [p for p, kd in pres if kd == "claim"] if kind == "claim" else obs
        form = rng.choice(["needs", "needs", "any", "all", "vote"]) if kind != "doubt" else "needs"
        m = rng.randint(1, min(3, len(pool)))
        items = rng.sample(pool, m)
        if form == "vote" and len(items) < 3:
            form = "any"
        if form == "vote":
            ev = "vote %d of %s" % (rng.randint(1, len(items) - 1), " ".join(items))
        elif form in ("any", "all") and len(items) > 1:
            ev = "when %s %s" % (form, " ".join(items))
        else:
            ev = "needs " + " ".join(items)
        sev = " severity %d" % rng.randint(1, 10) if kind == "claim" and rng.random() < 0.5 else ""
        if kind == "doubt" and rng.random() < 0.5:  # both spellings of a discarding attack
            ev += " undermines " + rng.choice(obs)
        L.append('%s %s "%s"%s %s' % (kind, name, name, sev, ev))
        pres.append((name, kind))
    claims = [p for p, kd in pres if kd == "claim"]
    if not claims:
        return None
    for p, kd in pres:
        if kd == "claim" and rng.random() < 0.7:
            att = rng.choice([q for q, _ in pres if q != p] + obs)
            how = rng.choice(["rebuts", "undercuts"]) if att not in obs or rng.random() < 0.5 else "rebuts"
            L.append("%s %s %s" % (att, how, p))
        if kd == "doubt":
            L.append("%s undermines %s" % (p, rng.choice(obs)))
    if rng.random() < 0.3:
        a, b = rng.sample(obs, 2)
        L.append("implies %s %s" % (a, b))
    if rng.random() < 0.2:
        L.append("exclusive %s" % " ".join(rng.sample(obs, 2)))
    acts = []
    for k in range(rng.randint(1, 3)):
        trig = rng.sample(claims, rng.randint(1, min(2, len(claims))))
        acts.append(("a%d" % k, trig))
    L.append("action idle_run idle")
    for k, (a, trig) in enumerate(acts):
        safe = " safe" if k == 0 else ""
        L.append("action %s%s when %s%s" % (a, safe, "any " if len(trig) > 1 else "", " ".join(trig)))
    L.append("prefer " + " over ".join([a for a, _ in acts] + ["idle_run"]))
    return "\n".join(L) + "\n"


def fuzz(cbc, count, seed):
    import random
    rng = random.Random(seed)
    tried = compared = bad = states = 0
    with tempfile.TemporaryDirectory() as d:
        while compared < count and tried < count * 60:
            tried += 1
            text = random_casebook(rng, tried)
            if text is None:
                continue
            path = os.path.join(d, "fuzz%d.cb" % tried)
            open(path, "w").write(text)
            n, res = check(cbc, path)
            if n is None:
                continue
            compared += 1
            states += n
            if res:
                bad += 1
                if bad <= int(os.environ.get("REFCHECK_SHOW", "3")):
                    print("DIFF on a random rulebook (%d of %d states):\n%s" % (len(res), n, text))
                    for mask, theirs, mine in res[:3]:
                        print("     state 0x%x: cbc %s %s, refcheck %s %s" % (mask, theirs[0], theirs[1], mine[0], mine[1]))
    print("%s random rulebooks: %d generated, %d passed cbc and were compared over %d states, %d differed" %
          ("FUZZ" if not bad else "DIFF", tried, compared, states, bad))
    return 1 if bad else 0


def main():
    if len(sys.argv) >= 3 and sys.argv[2] == "--fuzz":
        return fuzz(sys.argv[1], int(sys.argv[3]) if len(sys.argv) > 3 else 300,
                    int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cbc, bad = sys.argv[1], 0
    for path in sys.argv[2:]:
        states, res = check(cbc, path)
        if states is None:
            print("SKIP %s: %s" % (path, res))
            continue
        if res:
            bad += 1
            print("DIFF %s: %d of %d states" % (path, len(res), states))
            for mask, theirs, mine in res[:5]:
                print("     state 0x%x: cbc %s %s, refcheck %s %s" % (mask, theirs[0], theirs[1], mine[0], mine[1]))
        else:
            print("SAME %s: all %d states" % (path, states))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
