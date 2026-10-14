// Does learning add anything to a hand-written casebook? The pre-registered test in learn/PREREGISTRATION.md.
//
//   eval_hybrid <ai4i2020.csv> [--quick] [--binding generic|learned]
//
// --binding generic is v1 (decile alarm limits, the default, reproducible exactly); --binding learned is v2 (MDL
// thresholds learned inside every training set). Both are registered in PREREGISTRATION.md.
//
// Every protocol choice below is the one registered there BEFORE this program was run: the observations and their
// thresholds, the 8 conditions, the methods, the k candidates, the training sizes, the folds and seed, the primary
// metric, and the four success criteria. If something here differs from that file, this program is wrong.
//
// --quick runs only the two training sizes the criteria are judged at (500 and 9000). It is what ctest runs; the full
// learning curve adds sizes 250, 1000 and 2500 for the record, and judges nothing extra.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "quorum/learn/aacbr.hpp"
#include "quorum/learn/baselines.hpp"
#include "quorum/learn/discretize.hpp"
#include "quorum/learn/features.hpp"
#include "quorum/learn/hybrid.hpp"
#include "quorum/learn/portable_random.hpp"

using quorum::learn::AaCbr;
using quorum::learn::Case;
using quorum::learn::Coherence;
using quorum::learn::Features;
using quorum::learn::NaiveBayes;
using quorum::learn::nearest_neighbour;
using quorum::learn::project;
using quorum::learn::top_k_features;

namespace {

constexpr int kFolds = 10;
constexpr std::uint32_t kSeed = 20260929U;  // the registered seed
std::uint32_t g_seed = kSeed;              // --seed overrides it, for robustness runs that judge nothing
// Bits the selection ranks over. v1 used 24; v2 uses all 32. Set once in main, before any thread starts.
int g_feature_count = 24;

// ── the binding layer ────────────────────────────────────────────────────────────────────────────────────────────
// Bits 0..18: generic alarm limits at the label-free 10th / 90th percentile of each signal over the whole file, plus
// product type. Bits 19..23: the documented mode thresholds, visible only in a condition whose casebook knows the mode.
enum Obs : int {
    kAirLow, kAirHigh, kProcLow, kProcHigh, kSpeedLow, kSpeedHigh, kTorqueLow, kTorqueHigh, kWearLow, kWearHigh,
    kDiffLow, kDiffHigh, kPowerLow, kPowerHigh, kStrainLow, kStrainHigh, kTypeL, kTypeM, kTypeH,
    kHdfDiff, kHdfSpeed, kPwfLow, kPwfHigh, kOsfStrain,
};
constexpr Features obs_bit(int o) { return Features{1} << o; }
constexpr Features kGeneric = (Features{1} << 19) - 1U;

const char* const kObsName[24] = {
    "air_low", "air_high", "proc_low", "proc_high", "speed_low", "speed_high", "torque_low", "torque_high",
    "wear_low", "wear_high", "tdiff_low", "tdiff_high", "power_low", "power_high", "strain_low", "strain_high",
    "type_l", "type_m", "type_h", "hdf_tdiff_lt_8_6", "hdf_speed_lt_1380", "pwf_power_lt_3500", "pwf_power_gt_9000",
    "osf_strain_over_limit",
};

// The documented modes. Each is its observations plus its rules, written as AA-CBR cases with outcome "failure".
struct Mode {
    const char* name;
    Features observations;
    std::vector<Case> rules;
};
const Mode kModes[3] = {
    {"HDF", obs_bit(kHdfDiff) | obs_bit(kHdfSpeed), {{obs_bit(kHdfDiff) | obs_bit(kHdfSpeed), true}}},
    {"PWF", obs_bit(kPwfLow) | obs_bit(kPwfHigh), {{obs_bit(kPwfLow), true}, {obs_bit(kPwfHigh), true}}},
    {"OSF", obs_bit(kOsfStrain), {{obs_bit(kOsfStrain), true}}},
};

constexpr int kSignals = 8;
const char* const kSignalName[kSignals] = {"air", "proc", "speed", "torque", "wear", "tdiff", "power", "strain"};

struct Row {
    Features x;  // v1 observations: generic deciles, type, mode thresholds
    bool failure;
    double signal[kSignals];
};

// v2 layout. Mode bits keep their v1 positions (19..23) so the casebook rules are the same Cases; type keeps 16..18;
// learned intervals fill the other 24 positions, in signal order.
constexpr Features kModeBits = ((Features{1} << 24) - 1U) & ~((Features{1} << 19) - 1U);
constexpr Features kTypeBits = (Features{1} << 16) | (Features{1} << 17) | (Features{1} << 18);
constexpr int kIntervalSlot[24] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                   24, 25, 26, 27, 28, 29, 30, 31};

/// Thresholds learned from one training set, and the encoding they induce.
struct Binder {
    std::vector<double> cuts[kSignals];
    int first_slot[kSignals]{};
    Features encode(const Row& r) const {
        Features x = r.x & (kModeBits | kTypeBits);
        for (int s = 0; s < kSignals; ++s) {
            const auto& c = cuts[s];
            if (c.empty()) { continue; }
            std::size_t interval = 0;
            while (interval < c.size() && r.signal[s] >= c[interval]) { ++interval; }
            x |= Features{1} << kIntervalSlot[first_slot[s] + static_cast<int>(interval)];
        }
        return x;
    }
};

Binder learn_binder(const std::vector<Row>& rows, const std::vector<std::size_t>& idx) {
    Binder b;
    std::vector<bool> y;
    y.reserve(idx.size());
    for (std::size_t i : idx) { y.push_back(rows[i].failure); }
    int slot = 0;
    for (int s = 0; s < kSignals; ++s) {
        std::vector<double> v;
        v.reserve(idx.size());
        for (std::size_t i : idx) { v.push_back(rows[i].signal[s]); }
        b.cuts[s] = quorum::learn::mdlp_cuts(v, y, 2);
        b.first_slot[s] = slot;
        slot += b.cuts[s].empty() ? 0 : static_cast<int>(b.cuts[s].size()) + 1;
    }
    return b;
}

std::vector<Row> load(const char* path) {
    std::ifstream in(path);
    std::vector<Row> out;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) { f.push_back(cell); }
        if (f.size() < 9) { continue; }
        const char type = f[2].empty() ? '?' : f[2][0];
        const double air = std::stod(f[3]);
        const double proc = std::stod(f[4]);
        const double speed = std::stod(f[5]);
        const double torque = std::stod(f[6]);
        const double wear = std::stod(f[7]);
        const double diff = proc - air;
        const double power = torque * speed * 2.0 * 3.14159265358979323846 / 60.0;
        const double strain = wear * torque;
        const double limit = type == 'L' ? 11000.0 : type == 'M' ? 12000.0 : 13000.0;
        Features x = 0;
        auto set = [&x](bool c, int o) { if (c) { x |= obs_bit(o); } };
        set(air < 297.4, kAirLow);         set(air > 302.7, kAirHigh);
        set(proc < 308.1, kProcLow);       set(proc > 311.9, kProcHigh);
        set(speed < 1364.0, kSpeedLow);    set(speed > 1746.0, kSpeedHigh);
        set(torque < 27.2, kTorqueLow);    set(torque > 52.6, kTorqueHigh);
        set(wear < 20.0, kWearLow);        set(wear > 195.0, kWearHigh);
        set(diff < 8.80, kDiffLow);        set(diff > 11.30, kDiffHigh);
        set(power < 4915.0, kPowerLow);    set(power > 7641.0, kPowerHigh);
        set(strain < 731.0, kStrainLow);   set(strain > 8282.0, kStrainHigh);
        set(type == 'L', kTypeL);          set(type == 'M', kTypeM);          set(type == 'H', kTypeH);
        set(diff < 8.6, kHdfDiff);         set(speed < 1380.0, kHdfSpeed);
        set(power < 3500.0, kPwfLow);      set(power > 9000.0, kPwfHigh);
        set(strain > limit, kOsfStrain);
        out.push_back({x, f[8] == "1", {air, proc, speed, torque, wear, diff, power, strain}});
    }
    return out;
}

using quorum::learn::choose_k;
using quorum::learn::Confusion;
using quorum::learn::Folds;
using quorum::learn::Invariants;
using quorum::learn::Knowledge;
using quorum::learn::kMethods;
using quorum::learn::Method;
using quorum::learn::run_method;
using quorum::learn::uses_k;
const auto& kAll = quorum::learn::kAllMethods;
const char* name(Method m) { return quorum::learn::method_label(m); }

struct Condition {
    unsigned modes;  // bit i = casebook knows kModes[i]
    Features visible;
    Knowledge kb;    // the rules assert "failure"; the default is "normal"
    std::string label;
};

Condition make_condition(unsigned modes) {
    Condition c{modes, kGeneric, {}, ""};
    for (int i = 0; i < 3; ++i) {
        if (((modes >> i) & 1U) == 0U) { continue; }
        c.visible |= kModes[i].observations;
        for (const Case& r : kModes[i].rules) {
            c.kb.priors.push_back(r);
            c.kb.features |= r.x;
        }
        c.label += c.label.empty() ? "" : "+";
        c.label += kModes[i].name;
    }
    if (c.label.empty()) { c.label = "(none)"; }
    return c;
}

struct Cell {
    Confusion conf[kMethods];
    std::string ks[kMethods];
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: eval_hybrid <ai4i2020.csv> [--quick] [--binding generic|learned]\n");
        return 2;
    }
    bool quick = false;
    bool learned_binding = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") {
            quick = true;
        } else if (a == "--seed" && i + 1 < argc) {
            g_seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--binding" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v != "generic" && v != "learned") {
                std::printf("FAIL  --binding must be generic or learned\n");
                return 2;
            }
            learned_binding = v == "learned";
        } else {
            std::printf("FAIL  unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }
    g_feature_count = learned_binding ? 32 : 24;
    const std::vector<Row> rows = load(argv[1]);
    int failures_total = 0;
    for (const Row& r : rows) { failures_total += r.failure ? 1 : 0; }
    std::printf("casebook + learning -- UCI AI4I 2020, %zu rows, %d failures, %d-fold CV, seed %u%s\n", rows.size(),
                failures_total, kFolds, g_seed, quick ? ", QUICK (sizes 500 and 9000 only)" : "");
    if (g_seed != kSeed) {
        std::printf("NOT THE REGISTERED SEED: a robustness run. Its criteria lines below judge nothing.\n");
    }
    std::printf("binding layer: %s\n", learned_binding ? "LEARNED -- MDL thresholds inside every training set (v2)"
                                                        : "GENERIC -- decile alarm limits (v1)");
    if (rows.size() != 10000U || failures_total != 339) {
        std::printf("FAIL  the data file is not the registered one\n");
        return 1;
    }

    std::vector<std::size_t> order(rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::mt19937 rng(g_seed);
    quorum::learn::rnd::shuffle(order.begin(), order.end(), rng);

    std::vector<int> sizes = quick ? std::vector<int>{500, 9000} : std::vector<int>{250, 500, 1000, 2500, 9000};
    std::vector<Condition> conds;
    for (unsigned s = 0; s < 8U; ++s) { conds.push_back(make_condition(s)); }

    // One task per (size, condition, fold). Each writes only its own slot, so the result is independent of scheduling.
    struct Task { int size_ix, cond_ix, fold; };
    std::vector<Task> tasks;
    for (int si = 0; si < static_cast<int>(sizes.size()); ++si) {
        for (int ci = 0; ci < 8; ++ci) {
            for (int f = 0; f < kFolds; ++f) { tasks.push_back({si, ci, f}); }
        }
    }
    std::vector<Cell> per_task(tasks.size());
    std::vector<Invariants> inv_task(tasks.size());
    std::vector<std::array<int, kMethods>> k_task(tasks.size());

    auto work = [&](std::size_t t) {
        const Task& tk = tasks[t];
        const Condition& cond = conds[static_cast<std::size_t>(tk.cond_ix)];
        // What this condition can see. A mode's observations exist only if the casebook knows the mode.
        const Features visible = learned_binding ? (~kModeBits | (cond.visible & kModeBits)) : cond.visible;
        std::vector<std::size_t> train_ix, test_ix;
        for (std::size_t i = 0; i < order.size(); ++i) {
            (static_cast<int>(i % kFolds) == tk.fold ? test_ix : train_ix).push_back(order[i]);
        }
        const std::size_t n = static_cast<std::size_t>(sizes[static_cast<std::size_t>(tk.size_ix)]);
        if (train_ix.size() > n) { train_ix.resize(n); }

        // Encode `rows` with thresholds learned on `fit_ix` alone (v2), or with the fixed v1 observations.
        auto encode = [&](const std::vector<std::size_t>& fit_ix, const std::vector<std::size_t>& ix) {
            std::vector<Case> out;
            out.reserve(ix.size());
            if (learned_binding) {
                const Binder b = learn_binder(rows, fit_ix);
                for (std::size_t i : ix) { out.push_back({b.encode(rows[i]) & visible, rows[i].failure}); }
            } else {
                for (std::size_t i : ix) { out.push_back({rows[i].x & visible, rows[i].failure}); }
            }
            return out;
        };
        const std::vector<Case> train = encode(train_ix, train_ix);
        const std::vector<Case> test = encode(train_ix, test_ix);
        Folds inner;
        for (int f = 0; f < 5; ++f) {
            std::vector<std::size_t> tr, va;
            for (std::size_t i = 0; i < train_ix.size(); ++i) {
                (static_cast<int>(i % 5U) == f ? va : tr).push_back(train_ix[i]);
            }
            inner.emplace_back(encode(tr, tr), encode(tr, va));
        }
        for (int mi = 0; mi < kMethods; ++mi) {
            const Method m = kAll[mi];
            const int k = uses_k(m) ? choose_k(m, cond.kb, inner, g_feature_count) : 0;
            k_task[t][static_cast<std::size_t>(mi)] = k;
            per_task[t].conf[mi] = run_method(m, cond.kb, train, test, k, g_feature_count, &inv_task[t]);
        }
    };
    const unsigned threads = std::max(1U, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    std::size_t next = 0;
    std::mutex mu;
    for (unsigned w = 0; w < threads; ++w) {
        pool.emplace_back([&] {
            for (;;) {
                std::size_t t;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    if (next >= tasks.size()) { return; }
                    t = next++;
                }
                work(t);
            }
        });
    }
    for (auto& th : pool) { th.join(); }

    // Aggregate over folds.
    std::vector<std::vector<Cell>> cell(sizes.size(), std::vector<Cell>(8));
    Invariants inv;
    for (std::size_t t = 0; t < tasks.size(); ++t) {
        Cell& c = cell[static_cast<std::size_t>(tasks[t].size_ix)][static_cast<std::size_t>(tasks[t].cond_ix)];
        for (int mi = 0; mi < kMethods; ++mi) {
            c.conf[mi].add(per_task[t].conf[mi]);
            if (uses_k(kAll[mi])) {
                c.ks[mi] += (c.ks[mi].empty() ? "" : ",") + std::to_string(k_task[t][static_cast<std::size_t>(mi)]);
            }
        }
        inv.add(inv_task[t]);
    }

    auto ba = [&](std::size_t si, std::size_t ci, Method m) { return 100.0 * cell[si][ci].conf[static_cast<int>(m)].ba(); };
    for (std::size_t si = 0; si < sizes.size(); ++si) {
        std::printf("\nBALANCED ACCURACY, %%, training size %d per fold (pooled over %d test folds)\n", sizes[si], kFolds);
        std::printf("  %-12s", "casebook");
        for (Method m : kAll) { std::printf(" %17s", name(m)); }
        std::printf("\n");
        for (std::size_t ci = 0; ci < 8U; ++ci) {
            std::printf("  %-12s", conds[ci].label.c_str());
            for (Method m : kAll) { std::printf(" %17.2f", ba(si, ci, m)); }
            std::printf("\n");
        }
    }

    const std::size_t s500 = static_cast<std::size_t>(std::find(sizes.begin(), sizes.end(), 500) - sizes.begin());
    const std::size_t s9000 = static_cast<std::size_t>(std::find(sizes.begin(), sizes.end(), 9000) - sizes.begin());
    for (std::size_t si : {s500, s9000}) {
        std::printf("\nMISSED FAILURES / FALSE ALARMS, training size %d (of %d failures, %d normals)\n", sizes[si],
                    failures_total, static_cast<int>(rows.size()) - failures_total);
        std::printf("  %-12s %16s %16s %16s\n", "casebook", "casebook", "learned", "casebook+learned");
        for (std::size_t ci = 0; ci < 8U; ++ci) {
            std::printf("  %-12s", conds[ci].label.c_str());
            for (Method m : {Method::Casebook, Method::Learned, Method::Hybrid}) {
                const Confusion& c = cell[si][ci].conf[static_cast<int>(m)];
                std::printf("      %4d / %4d", c.fn, c.fp);
            }
            std::printf("\n");
        }
    }
    std::printf("\nk chosen per fold for casebook+learned at size 9000:\n");
    for (std::size_t ci = 0; ci < 8U; ++ci) {
        std::printf("  %-12s %s\n", conds[ci].label.c_str(), cell[s9000][ci].ks[static_cast<int>(Method::Hybrid)].c_str());
    }

    // ── the pre-registered criteria ─────────────────────────────────────────────────────────────────────────────
    std::printf("\nPRE-REGISTERED CRITERIA (learn/PREREGISTRATION.md)\n");
    auto incomplete = [](std::size_t ci) { return ci != 0U && ci != 7U; };
    int c1_wins = 0, c2_wins = 0;
    double c1_gain = 0.0, c2_gain = 0.0;
    for (std::size_t ci = 0; ci < 8U; ++ci) {
        if (!incomplete(ci)) { continue; }
        const double g1 = ba(s9000, ci, Method::Hybrid) - ba(s9000, ci, Method::Casebook);
        const double g2 = ba(s500, ci, Method::Hybrid) - ba(s500, ci, Method::Learned);
        c1_wins += g1 > 0.0 ? 1 : 0;
        c2_wins += g2 > 0.0 ? 1 : 0;
        c1_gain += g1 / 6.0;
        c2_gain += g2 / 6.0;
    }
    const bool c1 = c1_wins == 6 && c1_gain >= 10.0;
    const bool c2 = c2_wins == 6 && c2_gain >= 5.0;
    const double c3_loss = ba(s9000, 7, Method::Casebook) - ba(s9000, 7, Method::Hybrid);
    const bool c3 = c3_loss <= 1.0;
    const bool c4 = inv.not_total == 0 && inv.over_bound == 0;
    std::printf("  1. learning improves the incomplete casebook (n=9000): wins %d/6, mean gain %+.2f pts   %s\n", c1_wins,
                c1_gain, c1 ? "PASS" : "FAIL");
    std::printf("  2. the casebook improves learning (n=500):             wins %d/6, mean gain %+.2f pts   %s\n", c2_wins,
                c2_gain, c2 ? "PASS" : "FAIL");
    std::printf("  3. no harm with the complete casebook (n=9000):        loss %+.2f pts (limit 1.00)       %s\n", c3_loss,
                c3 ? "PASS" : "FAIL");
    std::printf("  4. invariants: %ld AA-CBR queries, not total %ld, over bound %ld; largest %d args, %d attacks   %s\n",
                inv.queries, inv.not_total, inv.over_bound, inv.max_args, inv.max_attacks, c4 ? "PASS" : "FAIL");
    const bool impact = c1 && c2 && c4;
    std::printf("\n  VERDICT: learning makes an impactful difference to a casebook: %s\n", impact ? "YES" : "NOT SHOWN");
    if (!c3) { std::printf("  DEFECT: learning harms the complete casebook\n"); }
    return c4 ? 0 : 1;
}
