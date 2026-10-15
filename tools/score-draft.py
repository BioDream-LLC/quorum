#!/usr/bin/env python3
"""score-draft: score drafted rulebooks against a pre-registered answer key, by behaviour.

    score-draft.py <key.tsv> <doc-id> <draft.cb> [more drafts ...]

A reading "for X" is any `observe` whose cited manual sentence (the `# manual: "..."` line the drafter writes above
each declaration) contains X, case-insensitively. Decisions come from evaluating the draft with tools/refcheck.py,
so the score does not depend on cbc accepting the draft; whether cbc accepts it is reported beside the score. The
check types are described at the top of the key.
"""
import itertools
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refcheck  # noqa: E402


def citations(text):
    """observe name -> the manual sentence quoted in the comment above it."""
    out, quote = {}, ""
    for line in text.splitlines():
        m = re.match(r'#\s*manual:\s*"(.*)"\s*$', line.strip())
        if m:
            quote = m.group(1)
        elif re.match(r"observe\s+\S+", line):
            out[line.split()[1]] = quote
            quote = ""
        elif line.strip() and not line.strip().startswith("#") and not line.startswith((" ", "\t")):
            quote = ""
    return out


class Draft:
    def __init__(self, path):
        self.text = open(path, encoding="utf-8").read()
        self.obs, self.nodes, self.attacks, self.prefer, _ = refcheck.parse(self.text)
        self.actions = [n for n, x in self.nodes.items() if x.kind == "action"]
        self.cited = citations(self.text)
        self.cache = {}

    def readings(self, x):
        found = [o for o in self.obs if x.lower() in self.cited.get(o, "").lower()]
        # SCORE_ALIAS="Overspeed=overspeed;...": POST HOC, not part of the pre-registered rule. Also accept a reading
        # whose NAME contains the given text, for drafts that cite a heading instead of the item. Reported apart.
        for pair in filter(None, os.environ.get("SCORE_ALIAS", "").split(";")):
            k, v = pair.split("=", 1)
            if k == x:
                found += [o for o in self.obs if v in o and o not in found]
        return found

    def decide(self, on):
        key = frozenset(on)
        if key not in self.cache:
            present = {o: o in key for o in self.obs}
            self.cache[key] = refcheck.evaluate(present, self.nodes, self.attacks, self.prefer, self.actions)[1]
        return self.cache[key]

    def kind(self, a):
        n = self.nodes[a]
        return "safe" if n.safe else ("idle" if n.idle else "alarm")

    def lead(self, r):
        """(others, action): the fewest other readings, at rest themselves, with which r changes the decision."""
        rest = [o for o in self.obs if o != r]
        for size in range(3):
            for s in itertools.combinations(rest, size):
                base = self.decide(s)
                if self.kind(base) != "idle":
                    continue
                with_r = self.decide(s + (r,))
                if with_r != base:
                    return s, with_r
        return None, None


def check(d, kind, args, expect):
    xs = args.split("|")
    if kind == "rest":
        a = d.decide(())
        return d.kind(a) == "idle", "at rest: " + a
    if kind == "noaction":
        bad = [a for a in d.actions if re.search(xs[0], a, re.I)]
        return not bad, "actions: " + " ".join(d.actions)
    rs = [d.readings(x) for x in xs]
    for x, r in zip(xs, rs):
        if not r:
            return False, "no reading cites %r" % x
    if kind == "lead":
        seen = []
        for r in rs[0]:
            _, a = d.lead(r)
            k = d.kind(a) if a else "nothing"
            seen.append("%s->%s(%s)" % (r, a, k))
            if a and (k == expect or (expect == "nonidle" and k in ("safe", "alarm"))):
                return True, seen[-1]
        return False, "; ".join(seen)
    if kind == "matters":  # as the key defines it: any up to two other readings, idle or not
        for r in rs[0]:
            rest = [o for o in d.obs if o != r]
            for size in range(3):
                for s in itertools.combinations(rest, size):
                    if d.decide(s) != d.decide(s + (r,)):
                        return True, "%s changes %s to %s with %s" % (r, d.decide(s), d.decide(s + (r,)),
                                                                     "+".join(s) or "nothing else")
        return False, "changes nothing with up to two other readings"
    if kind == "worst":
        for a, b in itertools.product(rs[0], rs[1]):
            if d.kind(d.decide((a, b))) == "safe":
                return True, "%s+%s -> %s" % (a, b, d.decide((a, b)))
        return False, "never the safe action with both"
    if kind == "differ":
        for a, b in itertools.product(rs[0], rs[1]):
            la, lb = d.lead(a)[1], d.lead(b)[1]
            if la and lb and la != lb:
                return True, "%s->%s, %s->%s" % (a, la, b, lb)
        return False, "same action, or one leads nowhere"
    if kind == "excuse":
        for a, e in itertools.product(rs[0], rs[1]):
            s, act = d.lead(a)
            if not act:
                continue
            after = d.decide(s + (a, e))
            if after != act and d.kind(after) != "safe":
                return True, "%s->%s, with %s->%s" % (a, act, e, after)
        return False, "the exception changes nothing, or leads to the safe action"
    raise ValueError("unknown check " + kind)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    key = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
    rows = [k for k in key if k[0] == sys.argv[2]]
    cbc = os.environ.get("CBC")
    for path in sys.argv[3:]:
        try:
            d = Draft(path)
        except Exception as e:  # a draft the second implementation cannot read scores nothing
            print("%s: 0/%d (unreadable: %s)" % (path, len(rows), e))
            continue
        res = [(r, *check(d, r[2], r[3], r[4])) for r in rows]
        accepted = ""
        if cbc:
            ok = subprocess.run([cbc, path, "--quiet"], capture_output=True).returncode == 0
            accepted = "  cbc: " + ("accepted" if ok else "refused")
        print("%s: %d/%d%s" % (os.path.basename(path), sum(1 for _, ok, _ in res if ok), len(rows), accepted))
        for r, ok, why in res:
            if not ok:
                print("    MISS %s%s %s -- %s" % (r[0], r[1], r[5], why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
