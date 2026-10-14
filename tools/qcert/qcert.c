/* qcert -- check a quorum proof certificate, trusting nothing but this file.
 *
 * WHY THIS EXISTS. The compiler `cbc` says "every situation was checked". An auditor should not have to take that on
 * trust, nor read 5,000 lines of C++ to believe it. `cbc --certificate` writes down, for every state, the decision and
 * the full reasoning behind it in a form that can be checked locally; this program checks it. It is deliberately
 * plain C99, shares no code or headers with quorum, and is short enough to read in one sitting.
 *
 * WHAT IT PROVES, for the framework written in the certificate:
 *   1. Every one of the 2^observations states appears exactly once.
 *   2. In each state, the labelling given is THE grounded labelling. Checked with the rounds supplied:
 *        In  at round r  -> every attacker is Out at a round < r
 *        Out at round r  -> some attacker is In at a round < r
 *        unlabelled      -> no attacker is In, and some attacker is not Out
 *      The first two make the labelled part sound (contained in the grounded labelling); the third makes it a fixed
 *      point; the grounded labelling is the least fixed point, so they are equal.
 *   3. The outcome and action follow from that labelling by the decision rule: exactly one action In is Decided and
 *      takes it; two or more is Conflicted; none In with an action unlabelled is Indeterminate; otherwise Unsatisfied.
 *      Anything but Decided takes the fallback.
 *   4. The fingerprint in the header is the FNV-1a fingerprint of the framework, the idle and fallback actions and
 *      the binding (every setpoint, hysteresis, hold and staleness limit), recomputed here, so the certificate is
 *      tied to the image a device reports at start-up. The binding numbers are hashed, not interpreted: whether a
 *      setpoint is right is for whoever approves the rulebook.
 *
 * In every state the closed-world argument attacks each observation whose bit is clear -- that is how a state is
 * applied to the framework.
 *
 * Build: cc -std=c99 -O2 -o qcert qcert.c        Use: qcert pump.cert [0x1a375f9d]
 * The optional second argument is the fingerprint the DEVICE reported at start-up (or the one printed for the image
 * file). Without it, a forger could rewrite the framework and recompute the fingerprint; with it, the certificate is
 * pinned to the rulebook actually running on the machine.
 * Exit status 0 means CERTIFIED; anything else means REJECTED, with the reason printed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ARGS 4096
#define MAX_OBS 26 /* 64M states: a bitmap of seen states stays small */

static int fail(const char *why, long state) {
    if (state >= 0) printf("REJECTED: state %ld: %s\n", state, why);
    else printf("REJECTED: %s\n", why);
    return 1;
}

static uint32_t fnv;
static void mix(int32_t v) {
    uint32_t u = (uint32_t)v;
    int b;
    for (b = 0; b < 4; ++b) {
        fnv ^= (u >> (b * 8)) & 0xFFu;
        fnv *= 16777619u;
    }
}

int main(int argc, char **argv) {
    FILE *f;
    char word[64], name[256];
    unsigned fp_claimed;
    int version, n, obs, cap_args, cap_attacks, cw, edges, nact, fallback, idle, nch, nth, nbind = 0, i, k;
    int32_t bind[64 * 7 + 64 * 6];
    int *efrom, *eto, *act, *start, *list, *fill, *lab, *rnd;
    unsigned char *seen;
    unsigned long states, total, s;
    long counts[4] = {0, 0, 0, 0};

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: qcert <certificate> [fingerprint the device reported]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return fail("cannot open the certificate", -1);

    /* Version 1 certificates (before 2026-10-11) carried a fingerprint that did not cover the fallback or setpoints. */
    if (fscanf(f, "quorum-certificate %d", &version) != 1 || version != 2) return fail("not a version-2 certificate", -1);
    if (fscanf(f, " casebook %255s v%d", name, &version) != 2) return fail("bad casebook line", -1);
    if (fscanf(f, " fingerprint %x", &fp_claimed) != 1) return fail("bad fingerprint line", -1);
    if (fscanf(f, " arguments %d observations %d capacity %d %d closed_world %d edges %d", &n, &obs, &cap_args,
               &cap_attacks, &cw, &edges) != 6)
        return fail("bad header", -1);
    if (n < 1 || n > MAX_ARGS || obs < 0 || obs > MAX_OBS || obs > n || cw < obs || cw >= n || edges < 0)
        return fail("header values out of range", -1);

    efrom = malloc(sizeof(int) * (size_t)(edges + 1));
    eto = malloc(sizeof(int) * (size_t)(edges + 1));
    for (i = 0; i < edges; ++i) {
        if (fscanf(f, " e %d %d", &efrom[i], &eto[i]) != 2) return fail("bad edge line", -1);
        if (efrom[i] < 0 || efrom[i] >= n || eto[i] < 0 || eto[i] >= n) return fail("edge out of range", -1);
        if (eto[i] == cw) return fail("the closed-world argument must be unattacked", -1);
    }
    if (fscanf(f, " actions %d", &nact) != 1 || nact < 1 || nact > n) return fail("bad actions line", -1);
    act = malloc(sizeof(int) * (size_t)nact);
    for (i = 0; i < nact; ++i)
        if (fscanf(f, " %d", &act[i]) != 1 || act[i] < 0 || act[i] >= n) return fail("bad action index", -1);
    if (fscanf(f, " fallback %d", &fallback) != 1) return fail("bad fallback line", -1);
    for (i = 0; i < nact && act[i] != fallback; ++i) {}
    if (i == nact) return fail("the fallback is not one of the actions", -1);
    if (fscanf(f, " idle %d", &idle) != 1) return fail("bad idle line", -1);
    if (fscanf(f, " binding %d %d", &nch, &nth) != 2 || nch < 0 || nth < 0 || nch > 64 || nth > 64)
        return fail("bad binding line", -1);
    for (i = 0; i < nch; ++i)
        for (k = 0; k < 7; ++k) {
            long v;
            if (fscanf(f, k == 0 ? " c %ld" : " %ld", &v) != 1) return fail("bad channel line", -1);
            bind[nbind++] = (int32_t)(uint32_t)v;
        }
    for (i = 0; i < nth; ++i)
        for (k = 0; k < 6; ++k) {
            long v;
            if (fscanf(f, k == 0 ? " t %ld" : " %ld", &v) != 1) return fail("bad threshold line", -1);
            bind[nbind++] = (int32_t)(uint32_t)v;
        }

    /* The fingerprint, recomputed from the framework exactly as the device header and image carry it. */
    fnv = 2166136261u;
    mix(n); mix(obs); mix(cap_args); mix(cap_attacks); mix(edges);
    for (i = 0; i < edges; ++i) { mix(efrom[i]); mix(eto[i]); }
    mix(nact);
    for (i = 0; i < nact; ++i) mix(act[i]);
    mix(cw); mix(idle); mix(fallback);
    mix(nch);
    for (i = 0; i < nch * 7; ++i) mix(bind[i]);
    mix(nth);
    for (i = nch * 7; i < nbind; ++i) mix(bind[i]);
    if (fnv != fp_claimed) return fail("the fingerprint does not match the framework in this certificate", -1);
    if (argc == 3 && strtoul(argv[2], NULL, 0) != fnv)
        return fail("this certificate is for a different rulebook than the one the device reported", -1);

    /* Attackers of each argument, as a compressed list. */
    start = calloc((size_t)n + 1, sizeof(int));
    list = malloc(sizeof(int) * (size_t)(edges + 1));
    fill = calloc((size_t)n, sizeof(int));
    for (i = 0; i < edges; ++i) start[eto[i] + 1]++;
    for (i = 0; i < n; ++i) start[i + 1] += start[i];
    for (i = 0; i < edges; ++i) list[start[eto[i]] + fill[eto[i]]++] = efrom[i];

    if (fscanf(f, " states %lu", &states) != 1) return fail("bad states line", -1);
    total = 1UL << obs;
    if (states != total) return fail("the state count is not 2^observations", -1);
    seen = calloc(total / 8 + 1, 1);
    lab = malloc(sizeof(int) * (size_t)n);
    rnd = malloc(sizeof(int) * (size_t)n);

    for (s = 0; s < total; ++s) {
        unsigned long mask;
        int outcome, action, a, k, in_actions = 0, und_actions = 0, chosen = -1, want, want_action;
        if (fscanf(f, " s %lu %d %d", &mask, &outcome, &action) != 3) return fail("bad state line", (long)s);
        if (mask >= total) return fail("state out of range", (long)mask);
        if (seen[mask / 8] & (1u << (mask % 8))) return fail("state appears twice", (long)mask);
        seen[mask / 8] |= (unsigned char)(1u << (mask % 8));
        for (a = 0; a < n; ++a) {
            char c;
            if (fscanf(f, " %c", &c) != 1) return fail("truncated labelling", (long)mask);
            if (c == 'U') { lab[a] = 0; rnd[a] = 0; continue; }
            if (c != 'I' && c != 'O') return fail("label must be I, O or U", (long)mask);
            lab[a] = c == 'I' ? 1 : -1;
            if (fscanf(f, "%d", &rnd[a]) != 1 || rnd[a] < 1) return fail("a label needs a round >= 1", (long)mask);
        }
        for (a = 0; a < n; ++a) {
            int all_out_before = 1, some_in_before = 0, some_in = 0, some_not_out = 0;
            int nat = start[a + 1] - start[a];
            int cw_attacks = a < obs && !(mask >> a & 1UL);
            for (k = 0; k < nat + cw_attacks; ++k) {
                int b = k < nat ? list[start[a] + k] : cw;
                if (lab[b] == 1) { some_in = 1; if (rnd[b] < rnd[a] || lab[a] == 0) some_in_before = 1; }
                if (lab[b] != -1) some_not_out = 1;
                if (!(lab[b] == -1 && rnd[b] < rnd[a])) all_out_before = 0;
            }
            if (lab[a] == 1 && !all_out_before)
                return fail("an In argument has an attacker that is not Out at an earlier round", (long)mask);
            if (lab[a] == -1 && !some_in_before)
                return fail("an Out argument has no attacker that is In at an earlier round", (long)mask);
            if (lab[a] == 0 && (some_in || !some_not_out))
                return fail("an unlabelled argument could be labelled, so this is not the grounded labelling",
                            (long)mask);
        }
        for (k = 0; k < nact; ++k) {
            if (lab[act[k]] == 1) { ++in_actions; chosen = act[k]; }
            else if (lab[act[k]] == 0) ++und_actions;
        }
        want = in_actions == 1 ? 0 : in_actions > 1 ? 3 : und_actions > 0 ? 1 : 2;
        want_action = want == 0 ? chosen : fallback;
        if (outcome != want) return fail("the outcome does not follow from the labelling", (long)mask);
        if (action != want_action) return fail("the action does not follow from the labelling", (long)mask);
        counts[want]++;
    }
    if (fscanf(f, " %63s", word) != 1 || strcmp(word, "end") != 0) return fail("missing end marker", -1);
    fclose(f);

    printf("CERTIFIED  %s v%d  fingerprint 0x%08x%s\n", name, version, fp_claimed,
           argc == 3 ? "  (matches the device)" : "  (not compared with a device)");
    printf("  %lu of %lu states checked, each labelling proved grounded and each decision derived from it\n",
           total, total);
    printf("  Decided %ld  Indeterminate %ld  Unsatisfied %ld  Conflicted %ld\n", counts[0], counts[1], counts[2],
           counts[3]);
    if (counts[3] != 0) printf("  WARNING: some states select two actions at once\n");
    if (counts[2] != 0) printf("  WARNING: some states reject every action\n");
    return 0;
}
