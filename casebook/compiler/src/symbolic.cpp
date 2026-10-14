// Symbolic validation: every state at once, as Boolean functions of the presence bits.
//
// Exhaustive validation visits 2^n states one by one. That is the right thing for the 14-reading pump (16,384 states,
// well under a second) and the wrong thing for a skid with 30 readings (about a billion states) or for a palette,
// whose combined space is the product of its members'. This pass computes the same grounded labelling for ALL states
// simultaneously, with each argument's In and Out status represented as a reduced ordered binary decision diagram
// over the presence bits.
//
// The grounded labelling is the least fixed point of
//     In(a)  = every attacker of a is Out
//     Out(a) = some attacker of a is In
// starting from nothing labelled. Lifted to BDDs the iteration is the same, except each label is now a set of states.
// The closed-world argument attacks an observation exactly in the states where its bit is clear, so for an
// observation the clauses gain "and its bit is set" / "or its bit is clear". Chaotic (in-place) iteration of a
// monotone operator from the bottom converges to the least fixed point, so updating in place is sound.
//
// The decision procedure is `arbiter::decide`, restated over sets: exactly one action In is Decided, two or more is
// Conflicted, none In with some action Undecided is Indeterminate, and otherwise Unsatisfied.
//
// What this does NOT compute: the per-state work count. That is why device outputs still come from exhaustive
// validation, which also finds the worst-case state the firmware measures.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "quorum/cbc/cbc.hpp"

namespace quorum::cbc {

namespace {

/// Unsigned 128-bit count, portable: `__int128` is a GNU extension and the studio also builds this for Windows.
struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    U128() = default;
    U128(std::uint64_t v) : lo(v) {}  // NOLINT: implicit by design, a count is a number
    friend U128 operator+(U128 a, U128 b) {
        U128 r;
        r.lo = a.lo + b.lo;
        r.hi = a.hi + b.hi + (r.lo < a.lo ? 1U : 0U);
        return r;
    }
    [[nodiscard]] U128 shl(unsigned k) const {
        if (k == 0) { return *this; }
        U128 r;
        if (k >= 64) {
            r.hi = lo << (k - 64);
        } else {
            r.hi = (hi << k) | (lo >> (64 - k));
            r.lo = lo << k;
        }
        return r;
    }
    [[nodiscard]] bool zero() const { return hi == 0 && lo == 0; }
};

/// A minimal reduced ordered BDD package. Node 0 is false, node 1 is true; variable i is tested at level i.
class Bdd {
public:
    explicit Bdd(std::int32_t vars) : vars_(vars) {
        nodes_.push_back({vars, 0, 0});  // false
        nodes_.push_back({vars, 1, 1});  // true
    }

    static constexpr std::uint32_t kFalse = 0;
    static constexpr std::uint32_t kTrue = 1;

    [[nodiscard]] std::uint32_t var(std::int32_t i) { return make(i, kFalse, kTrue); }
    [[nodiscard]] std::uint32_t nvar(std::int32_t i) { return make(i, kTrue, kFalse); }

    [[nodiscard]] std::uint32_t ite(std::uint32_t f, std::uint32_t g, std::uint32_t h) {
        if (f == kTrue) { return g; }
        if (f == kFalse) { return h; }
        if (g == h) { return g; }
        if (g == kTrue && h == kFalse) { return f; }
        const Key k{f, g, h};
        const auto hit = cache_.find(k);
        if (hit != cache_.end()) { return hit->second; }
        const std::int32_t v = std::min({level(f), level(g), level(h)});
        const std::uint32_t lo = ite(cof(f, v, false), cof(g, v, false), cof(h, v, false));
        const std::uint32_t hi = ite(cof(f, v, true), cof(g, v, true), cof(h, v, true));
        const std::uint32_t r = make(v, lo, hi);
        cache_.emplace(k, r);
        return r;
    }
    [[nodiscard]] std::uint32_t both(std::uint32_t f, std::uint32_t g) { return ite(f, g, kFalse); }
    [[nodiscard]] std::uint32_t either(std::uint32_t f, std::uint32_t g) { return ite(f, kTrue, g); }
    [[nodiscard]] std::uint32_t negate(std::uint32_t f) { return ite(f, kFalse, kTrue); }

    /// Number of satisfying assignments over all `vars_` variables. Exact up to 2^127.
    [[nodiscard]] U128 count(std::uint32_t f) {
        std::unordered_map<std::uint32_t, U128> memo;
        // Paths from f, counting the variables at or below f's level.
        const std::function<U128(std::uint32_t)> paths = [&](std::uint32_t n) -> U128 {
            if (n == kFalse) { return U128{0}; }
            if (n == kTrue) { return U128{1}; }
            const auto it = memo.find(n);
            if (it != memo.end()) { return it->second; }
            const Node& x = nodes_[n];
            const U128 lo = paths(x.lo).shl(static_cast<unsigned>(level(x.lo) - x.var - 1));
            const U128 hi = paths(x.hi).shl(static_cast<unsigned>(level(x.hi) - x.var - 1));
            memo.emplace(n, lo + hi);
            return lo + hi;
        };
        return paths(f).shl(static_cast<unsigned>(level(f)));
    }

    /// The variables f actually depends on.
    [[nodiscard]] std::vector<bool> support(std::uint32_t f) const {
        std::vector<bool> seen_var(static_cast<std::size_t>(vars_), false);
        std::vector<bool> seen(nodes_.size(), false);
        std::vector<std::uint32_t> stack{f};
        while (!stack.empty()) {
            const std::uint32_t n = stack.back();
            stack.pop_back();
            if (n <= kTrue || seen[n]) { continue; }
            seen[n] = true;
            seen_var[static_cast<std::size_t>(nodes_[n].var)] = true;
            stack.push_back(nodes_[n].lo);
            stack.push_back(nodes_[n].hi);
        }
        return seen_var;
    }

    [[nodiscard]] bool eval(std::uint32_t f, const std::vector<bool>& bits) const {
        while (f > kTrue) {
            const Node& x = nodes_[f];
            f = bits[static_cast<std::size_t>(x.var)] ? x.hi : x.lo;
        }
        return f == kTrue;
    }

    [[nodiscard]] std::size_t size() const { return nodes_.size(); }

private:
    struct Node {
        std::int32_t var;
        std::uint32_t lo;
        std::uint32_t hi;
    };
    struct Key {
        std::uint64_t a, b, c;
        bool operator==(const Key& o) const { return a == o.a && b == o.b && c == o.c; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(k.a * 0x9E3779B97F4A7C15ULL ^ (k.b + 0x632BE59BD9B4E019ULL) * 31 ^ k.c);
        }
    };

    [[nodiscard]] std::int32_t level(std::uint32_t n) const { return nodes_[n].var; }
    [[nodiscard]] std::uint32_t cof(std::uint32_t n, std::int32_t v, bool high) const {
        const Node& x = nodes_[n];
        if (x.var != v) { return n; }
        return high ? x.hi : x.lo;
    }
    [[nodiscard]] std::uint32_t make(std::int32_t v, std::uint32_t lo, std::uint32_t hi) {
        if (lo == hi) { return lo; }
        const Key k{static_cast<std::uint64_t>(v), lo, hi};
        const auto it = unique_.find(k);
        if (it != unique_.end()) { return it->second; }
        nodes_.push_back({v, lo, hi});
        const auto id = static_cast<std::uint32_t>(nodes_.size() - 1);
        unique_.emplace(k, id);
        return id;
    }

    std::int32_t vars_;
    std::vector<Node> nodes_;
    std::unordered_map<Key, std::uint32_t, KeyHash> unique_;
    std::unordered_map<Key, std::uint32_t, KeyHash> cache_;
};

std::string decimal(U128 v) {
    if (v.zero()) { return "0"; }
    // Long division by 10 over four 32-bit limbs, most significant first.
    std::uint32_t limb[4] = {static_cast<std::uint32_t>(v.hi >> 32), static_cast<std::uint32_t>(v.hi),
                             static_cast<std::uint32_t>(v.lo >> 32), static_cast<std::uint32_t>(v.lo)};
    std::string s;
    for (;;) {
        bool all_zero = true;
        std::uint64_t rem = 0;
        for (std::uint32_t& l : limb) {
            const std::uint64_t cur = (rem << 32) | l;
            l = static_cast<std::uint32_t>(cur / 10);
            rem = cur % 10;
            if (l != 0) { all_zero = false; }
        }
        s.push_back(static_cast<char>('0' + static_cast<int>(rem)));
        if (all_zero) { break; }
    }
    std::reverse(s.begin(), s.end());
    return s;
}

}  // namespace

struct SymbolicCheck::Impl {
    explicit Impl(std::int32_t vars) : bdd(vars) {}
    Bdd bdd;
    std::vector<std::uint32_t> in;
    std::vector<std::uint32_t> out;
    std::vector<std::vector<std::uint32_t>> outcome;  ///< per policy, per Outcome value
};

SymbolicCheck::SymbolicCheck(const Lowered& lo, const std::vector<SymbolicPolicy>& policies)
    : impl_(std::make_unique<Impl>(lo.observation_count)), observations_(lo.observation_count) {
    const auto t0 = std::chrono::steady_clock::now();
    Bdd& b = impl_->bdd;
    const auto n = static_cast<std::size_t>(lo.count());

    std::vector<std::vector<std::int32_t>> attackers(n);
    for (const LoweredEdge& e : lo.edges) { attackers[static_cast<std::size_t>(e.to)].push_back(e.from); }

    std::vector<std::uint32_t>& in = impl_->in;
    std::vector<std::uint32_t>& out = impl_->out;
    in.assign(n, Bdd::kFalse);
    out.assign(n, Bdd::kFalse);

    for (bool changed = true; changed;) {
        changed = false;
        ++rounds_;
        for (std::size_t a = 0; a < n; ++a) {
            const bool is_obs = static_cast<std::int32_t>(a) < lo.observation_count;
            std::uint32_t i = is_obs ? b.var(static_cast<std::int32_t>(a)) : Bdd::kTrue;
            std::uint32_t o = is_obs ? b.nvar(static_cast<std::int32_t>(a)) : Bdd::kFalse;
            for (const std::int32_t att : attackers[a]) {
                i = b.both(i, out[static_cast<std::size_t>(att)]);
                o = b.either(o, in[static_cast<std::size_t>(att)]);
            }
            if (i != in[a] || o != out[a]) {
                in[a] = i;
                out[a] = o;
                changed = true;
            }
        }
    }

    for (std::size_t a = 0; a < n; ++a) {
        const std::uint32_t und = b.negate(b.either(in[a], out[a]));
        if (lo.arguments[a].role == Role::Doubt) { undecided_doubt_ = b.either(undecided_doubt_, und); }
    }

    for (const SymbolicPolicy& p : policies) {
        std::uint32_t none_in = Bdd::kTrue;
        std::uint32_t exactly_one = Bdd::kFalse;
        std::uint32_t two_or_more = Bdd::kFalse;
        std::uint32_t any_undecided = Bdd::kFalse;
        for (const std::int32_t act : p.actions) {
            const std::uint32_t ai = in[static_cast<std::size_t>(act)];
            const std::uint32_t au = b.negate(b.either(ai, out[static_cast<std::size_t>(act)]));
            // Adding one action: two-or-more gains (already exactly one) and (this In); exactly-one becomes
            // (exactly one and this not In) or (none and this In).
            two_or_more = b.either(two_or_more, b.both(exactly_one, ai));
            exactly_one = b.either(b.both(exactly_one, b.negate(ai)), b.both(none_in, ai));
            none_in = b.both(none_in, b.negate(ai));
            any_undecided = b.either(any_undecided, au);
        }
        std::vector<std::uint32_t> oc(5, Bdd::kFalse);
        oc[0] = exactly_one;                                  // Decided
        oc[1] = b.both(none_in, any_undecided);               // Indeterminate
        oc[2] = b.both(none_in, b.negate(any_undecided));     // Unsatisfied
        oc[3] = two_or_more;                                  // Conflicted
        oc[4] = Bdd::kFalse;                                  // Faulted: the solver always completes
        impl_->outcome.push_back(std::move(oc));
    }

    seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

SymbolicCheck::~SymbolicCheck() = default;

std::string SymbolicCheck::states() const { return decimal(U128{1}.shl(static_cast<unsigned>(observations_))); }

std::string SymbolicCheck::count(std::size_t policy, int outcome) const {
    return decimal(impl_->bdd.count(impl_->outcome[policy][static_cast<std::size_t>(outcome)]));
}

bool SymbolicCheck::empty(std::size_t policy, int outcome) const {
    return impl_->outcome[policy][static_cast<std::size_t>(outcome)] == Bdd::kFalse;
}

std::string SymbolicCheck::selected(std::size_t policy, std::int32_t action) const {
    Bdd& b = impl_->bdd;
    return decimal(b.count(b.both(impl_->outcome[policy][0], impl_->in[static_cast<std::size_t>(action)])));
}

std::string SymbolicCheck::in_count(std::int32_t argument) const {
    return decimal(impl_->bdd.count(impl_->in[static_cast<std::size_t>(argument)]));
}

std::string SymbolicCheck::undecided_doubt_states() const { return decimal(impl_->bdd.count(undecided_doubt_)); }

int SymbolicCheck::outcome_at(std::size_t policy, std::uint64_t mask) const {
    std::vector<bool> bits(static_cast<std::size_t>(observations_));
    for (std::int32_t i = 0; i < observations_; ++i) { bits[static_cast<std::size_t>(i)] = (mask >> i & 1U) != 0U; }
    for (int o = 0; o < 5; ++o) {
        if (impl_->bdd.eval(impl_->outcome[policy][static_cast<std::size_t>(o)], bits)) { return o; }
    }
    return -1;
}

int SymbolicCheck::label_at(std::int32_t argument, std::uint64_t mask) const {
    std::vector<bool> bits(static_cast<std::size_t>(observations_));
    for (std::int32_t i = 0; i < observations_; ++i) { bits[static_cast<std::size_t>(i)] = (mask >> i & 1U) != 0U; }
    if (impl_->bdd.eval(impl_->in[static_cast<std::size_t>(argument)], bits)) { return 1; }
    if (impl_->bdd.eval(impl_->out[static_cast<std::size_t>(argument)], bits)) { return -1; }
    return 0;
}

std::vector<bool> SymbolicCheck::depends_on(std::size_t policy) const {
    std::vector<bool> dep(static_cast<std::size_t>(observations_), false);
    Bdd& b = impl_->bdd;
    for (const std::uint32_t f : impl_->outcome[policy]) {
        const std::vector<bool> s = b.support(f);
        for (std::size_t i = 0; i < dep.size(); ++i) { dep[i] = dep[i] || s[i]; }
    }
    return dep;
}

std::vector<bool> SymbolicCheck::action_depends_on(std::int32_t action) const {
    Bdd& b = impl_->bdd;
    std::vector<bool> s = b.support(impl_->in[static_cast<std::size_t>(action)]);
    const std::vector<bool> t = b.support(impl_->out[static_cast<std::size_t>(action)]);
    for (std::size_t i = 0; i < s.size(); ++i) { s[i] = s[i] || t[i]; }
    return s;
}

std::size_t SymbolicCheck::nodes() const { return impl_->bdd.size(); }

}  // namespace quorum::cbc
