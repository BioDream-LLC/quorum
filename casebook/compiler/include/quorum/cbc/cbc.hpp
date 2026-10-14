#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

/// \file
/// `cbc` -- the casebook compiler.
///
/// HOST TOOL. Runs on a workstation, never on a device, and therefore uses the standard library freely. This is
/// the architectural point that makes the whole split work: an engineer authors a casebook in text, the compiler
/// turns it into a fixed-capacity C++ header, and only the generated artefact ever goes near a microcontroller.
///
/// Design rule, derived by counting the two hand-written casebooks: **the author writes domain content only, and
/// the compiler generates every piece of plumbing.** 12 of 37 pump arguments and 10 of 33 tank arguments were
/// evidence gates -- roughly a third of both was machinery nobody should type.
///
/// Vocabulary is ASPIC+'s: undermining (attack a premise), rebutting (attack a conclusion), undercutting (attack
/// an inference). See `casebook/LANGUAGE.md` for why that taxonomy and not an invented one.
///
/// ## An honest note about the three attack keywords
///
/// All three lower to the *same* edge. An abstract argumentation framework has one attack relation; it does not
/// distinguish why. So the taxonomy buys **type checking and intent**, not different output: `undermines` must
/// target an observation, `rebuts` and `undercuts` must target a presumption, and the raw/claim rewiring depends
/// on knowing which edges originate at an observation. Claiming more than that would be overselling it.

namespace quorum::cbc {

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────
struct Diagnostic {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    int line = 0;
    std::string message;
};

class Diagnostics {
public:
    void error(int line, std::string message) {
        items_.push_back({Diagnostic::Severity::Error, line, std::move(message)});
    }
    void warn(int line, std::string message) {
        items_.push_back({Diagnostic::Severity::Warning, line, std::move(message)});
    }
    [[nodiscard]] bool has_errors() const {
        for (const auto& d : items_) {
            if (d.severity == Diagnostic::Severity::Error) { return true; }
        }
        return false;
    }
    [[nodiscard]] const std::vector<Diagnostic>& items() const { return items_; }

private:
    std::vector<Diagnostic> items_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AST
// ─────────────────────────────────────────────────────────────────────────────

struct Provenance {
    std::string document;
    std::string date;
    std::string author;
    std::vector<std::string> notes;
    int line = 0;
};

struct Observe {
    std::string name;
    std::string description;
    int line = 0;
};

/// A set of readings that cannot physically co-occur.
///
/// This does NOT mean "never validate those states". It means the opposite: if they all appear at once, the
/// *instrument* is at fault, which is a real and reachable condition. The compiler therefore generates a doubt
/// that fires exactly then and discards the readings, and the validator still explores every state -- it just
/// reports instrument-fault states separately from genuine ambiguity.
struct Exclusive {
    std::vector<std::string> members;
    int line = 0;
};

enum class PresumptionKind {
    Claim,   ///< a defeasible hypothesis, e.g. a fault diagnosis
    Excuse,  ///< a benign explanation that competes with a claim
    Doubt,   ///< a reason to distrust evidence
};

[[nodiscard]] const char* kind_name(PresumptionKind k);

struct Presumption {
    PresumptionKind kind = PresumptionKind::Claim;
    std::string name;
    std::string description;
    /// CONJUNCTIVE evidence: every one of these must hold. Lowered as one gate each -- several gates on one
    /// argument is AND.
    ///
    /// Items may be observations OR other presumptions. A hazard that requires two claims to hold at once is a
    /// real and common shape (`unmeasured_filling needs more_level no_measure`), and restricting `needs` to
    /// observations would force the author to hand-write the gates the compiler exists to generate.
    std::vector<std::string> needs;
    /// DISJUNCTIVE evidence: any one of these suffices. Lowered as a single shared gate attacked by each -- several
    /// attackers on one gate is OR.
    ///
    /// The dual of `needs`, and necessary: "the level is safe if the transmitter says so OR somebody read the
    /// gauge" cannot be expressed with conjunction.
    std::vector<std::string> when;
    std::vector<std::string> undermines;  ///< observations this discards (doubts)
    /// COUNTING evidence: at least `vote_threshold` of `vote_items` must hold. `vote 2 of a b c`.
    ///
    /// Exists because redundancy is how every safety instrumented function in process industry is built, and 2oo3 is
    /// the canonical arrangement. Conjunction and disjunction can express it -- as the disjunction of the three pairs --
    /// but writing that by hand is how the vessel-hl-trip casebook acquired a real bug: "at least two of these three"
    /// appeared twice, and both times the natural-looking shortcut was wrong in the dangerous direction, failing to
    /// notice that redundancy had been lost. The expansion is mechanical, so the compiler should do it.
    int vote_threshold = 0;
    std::vector<std::string> vote_items;
    /// FMEA severity, 1..10, or 0 if not declared.
    ///
    /// This is the one column of an FMEA that translates into the framework, and it translates through ASPIC+'s
    /// preference ordering rather than through arithmetic. Risk priority numbers do not translate at all: RPN is
    /// severity x occurrence x detection, and occurrence and detection are statements about a population of
    /// machines, not about the one in front of the device. Severity is different -- it is a property of the failure
    /// itself, and it answers a question the framework genuinely has to resolve: when the evidence cannot
    /// distinguish two hypotheses, which one should the machine act on?
    ///
    /// The answer that keeps people alive is the more severe one, so higher severity means preferred.
    int severity = 0;
    bool generated = false;               ///< produced by `exclusive` or `implies`, not written by the author
    /// RAW readings whose presence cancels this presumption. Generated only, by `implies`: "A is present and B is
    /// not" needs B to defeat the doubt directly, and attacking from the raw reading keeps the result acyclic even
    /// when B is itself discarded by some other doubt -- the question is whether the instruments agree, not whether
    /// B is trusted.
    std::vector<std::string> unless;
    int line = 0;
};

/// A reading that physically requires another: `implies flow_high current_high`.
///
/// The dual of `exclusive`. Exclusive catches readings that cannot co-occur; this catches a reading that cannot occur
/// ALONE. A pump reporting high flow while its motor draws no current is not a pump with an exotic fault, it is an
/// instrument lying -- either the flow reading is false or the current channel is dead, and a dead channel reads
/// exactly like a quiet one. This is the analytical-redundancy idea from fault diagnosis, restricted to the form
/// that needs no arithmetic and can therefore be checked over every state like everything else.
///
/// One declaration becomes a doubt that fires when the antecedent is present and the consequent absent. It discards
/// the antecedent, because an uncorroborated reading is the one that must not be acted on, and it is an ordinary
/// doubt the author can name in an action (`action inspect when mismatch__flow_high__current_high`).
struct Implies {
    std::string antecedent;
    std::string consequent;
    std::string description;
    int line = 0;
};

enum class AttackKind { Undermines, Rebuts, Undercuts };

[[nodiscard]] const char* attack_kind_name(AttackKind k);

struct AttackDecl {
    std::string from;
    std::vector<std::string> to;
    AttackKind kind = AttackKind::Rebuts;
    int line = 0;
};

struct Action {
    std::string name;
    bool idle = false;  ///< taken when no other action applies
    bool safe = false;  ///< the fallback when the framework will not choose
    /// CONJUNCTIVE conditions: every one must hold. Written `when all a b c`, or `needs a b c`.
    std::vector<std::string> needs;
    /// DISJUNCTIVE conditions: any one suffices. Written `when any a b c`, or `when a` for a single condition.
    std::vector<std::string> when;
    int line = 0;
};

struct Prefer {
    std::vector<std::string> order;  ///< strongest first
    int line = 0;
};

/// One threshold on a channel: the point at which a number becomes a proposition.
///
/// Hysteresis is mandatory, not optional. A bare threshold chatters around its setpoint, a chattering observation
/// makes the framework oscillate between verdicts, and oscillating verdicts are the alarm flood this product exists
/// to prevent. Making the deadband optional would make the dangerous choice the convenient one.
struct Threshold {
    std::string observation;  ///< the observation this asserts
    bool above = true;        ///< assert on the way up, or on the way down
    std::int32_t on = 0;      ///< milli-units
    std::int32_t off = 0;     ///< milli-units
    /// `for <duration>`: how long the value must stay past `on` before the observation asserts. 0 = at once.
    std::uint32_t hold_ms = 0;
    int line = 0;
};

/// A physical signal, and the rules that turn it into propositions.
///
/// Every constant here lives in the casebook source rather than in device C, and that placement is the point. A
/// threshold compiled into firmware is a diagnostic decision that no witness will ever cite: move it and the machine
/// reaches a different conclusion while the explanation stays word for word the same. Declared here it is reviewed,
/// versioned, and carried in the provenance block alongside the FMEA it came from.
struct Channel {
    std::string name;
    std::string description;
    std::string unit;  ///< documentation, carried through for the witness
    std::int32_t valid_lo = 0;
    std::int32_t valid_hi = 0;
    bool has_range = false;
    std::uint32_t stale_after_ms = 0;
    /// How long the value may sit still before the channel is reported flatlined. 0 disables.
    ///
    /// Distinct from staleness, and the distinction is the whole reason this exists. A stale channel has stopped
    /// delivering; a flatlined channel delivers fresh, in-range, entirely plausible samples that never move. That is
    /// what a failed 4-20 mA transmitter does, and it is what nobody at CAPECO detected.
    std::uint32_t flatline_after_ms = 0;
    std::int32_t flatline_epsilon = 0;
    std::vector<Threshold> thresholds;
    int line = 0;
};

struct Casebook {
    std::string name;
    int version = 0;
    Provenance provenance;
    std::vector<Observe> observes;
    std::vector<Exclusive> exclusives;
    std::vector<Implies> implications;
    std::vector<Presumption> presumptions;
    std::vector<AttackDecl> attacks;
    std::vector<Action> actions;
    std::vector<Prefer> prefers;
    std::vector<Channel> channels;
};

/// Expand every `vote k of ...` clause into the combinations it denotes.
///
/// Runs before lowering, so the generated combination claims resolve by name like any others.
void desugar_votes(Casebook& cb, Diagnostics& diag);

// ─────────────────────────────────────────────────────────────────────────────
// Lowered form: a flat argumentation framework plus the metadata a device needs
// ─────────────────────────────────────────────────────────────────────────────

enum class Role {
    Observation,   ///< toggled by the binding layer each cycle
    UsableClaim,   ///< the trustworthy counterpart of a split raw reading
    Claim,
    Excuse,
    Doubt,
    Gate,          ///< generated: an AF cannot express "absent"
    Action,
    ClosedWorld,   ///< generated: unattacked, makes absent readings decisively Out
};

[[nodiscard]] const char* role_name(Role r);

struct LoweredArgument {
    std::string name;
    std::string description;
    Role role = Role::Claim;
    int severity = 0;  ///< FMEA severity 1..10, or 0 if not declared
    bool generated = false;
    /// For a Gate, the argument it gates on. For a UsableClaim, its raw reading. Otherwise empty.
    std::string relates_to;
    /// Source line, carried through lowering so `check` can point at the offending construct.
    ///
    /// Without this every structural error was reported at the `source` declaration, which is worse than reporting
    /// no line at all: it sends the reader to code that is correct.
    int line = 0;
};

struct LoweredEdge {
    std::int32_t from = 0;
    std::int32_t to = 0;
    /// Why this edge exists, carried through to the emitted header as a comment.
    std::string reason;
    /// True only for author-declared `rebuts` and `undermines` attacks between things that can carry a severity.
    ///
    /// ASPIC+ distinguishes preference-DEPENDENT attacks (rebutting, undermining) from preference-INDEPENDENT ones
    /// (undercutting). Only the former can be cancelled by a preference: an undercut says the inference does not
    /// apply at all, which is not a claim you can outrank.
    ///
    /// Generated plumbing -- gates, closed-world, precedence, idle wiring -- is never preference-dependent. Dropping
    /// one of those edges would not weaken an argument, it would silently rewire the framework.
    bool preference_dependent = false;
};

/// A channel in the lowered form: observation names resolved to indices.
struct LoweredChannel {
    std::string name;
    std::string description;
    std::string unit;
    std::int32_t valid_lo = 0;
    std::int32_t valid_hi = 0;
    std::uint32_t stale_after_ms = 0;
    std::uint32_t flatline_after_ms = 0;
    std::int32_t flatline_epsilon = 0;
    std::int32_t unavailable_obs = -1;  ///< generated observation, asserted when the channel cannot be believed
    std::int32_t flatline_obs = -1;     ///< generated observation, or -1 if no flatline check
    int line = 0;                       ///< source line of the `channel` declaration
};

struct LoweredThreshold {
    std::int32_t channel = 0;
    std::int32_t observation = -1;
    bool above = true;
    std::int32_t on = 0;
    std::int32_t off = 0;
    std::uint32_t hold_ms = 0;
};

struct Lowered {
    std::string casebook;
    int version = 0;
    Provenance provenance;

    std::vector<LoweredArgument> arguments;
    std::vector<LoweredEdge> edges;

    std::int32_t observation_count = 0;  ///< observations are a contiguous prefix
    std::int32_t closed_world = -1;

    std::vector<std::int32_t> actions;
    std::int32_t idle_action = -1;
    std::int32_t safe_action = -1;

    /// Exclusive groups, as argument indices, for the validator's reporting.
    std::vector<std::vector<std::int32_t>> exclusive_groups;

    /// The doubts generated from `implies`, as argument indices, for the validator's reporting. Host-only: the device
    /// needs nothing extra, because each is an ordinary doubt with ordinary edges.
    std::vector<std::int32_t> consistency_doubts;

    /// Attacks dropped because the attacker was strictly less severe than its target. Reported, never silent.
    struct PreferenceResolution {
        std::string attacker;
        int attacker_severity = 0;
        std::string target;
        int target_severity = 0;
    };
    std::vector<PreferenceResolution> preference_resolutions;

    /// The binding: how physical signals become the observations above. Empty if the casebook declares no channels,
    /// in which case the integrator supplies the presence mask by hand.
    std::vector<LoweredChannel> channels;
    std::vector<LoweredThreshold> thresholds;

    std::int32_t max_args = 0;     ///< the capacity the emitted header will request
    std::int32_t max_attacks = 0;

    /// The state that costs the most work, found by exhaustive validation. Filled in by pass 5.
    ///
    /// Emitted into the device header because worst-case execution time is the certification argument, and a timing
    /// measurement taken on a typical state does not bear on it at all. Firmware measures THIS state.
    std::uint32_t worst_case_mask = 0;
    std::int32_t worst_case_steps = 0;

    [[nodiscard]] std::int32_t index_of(const std::string& name) const;
    [[nodiscard]] std::int32_t count() const { return static_cast<std::int32_t>(arguments.size()); }
};

/// Structural fingerprint of a lowered casebook: counts, capacities, every edge in order, and the action list.
///
/// One definition, used both to emit `kCasebookFingerprint` into the device header and to print the number in the
/// validation report. Two copies of this arithmetic would eventually disagree, and the entire value of a fingerprint is
/// that the report and the device agree.
[[nodiscard]] std::uint32_t fingerprint(const Lowered& lo);

// ─────────────────────────────────────────────────────────────────────────────
// Passes
// ─────────────────────────────────────────────────────────────────────────────

/// Pass 1: text to AST. Line-oriented; an indented line continues the previous declaration.
[[nodiscard]] Casebook parse(const std::string& text, Diagnostics& diag);

/// Pass 2+3: resolve names and generate all plumbing.
[[nodiscard]] Lowered lower(const Casebook& cb, Diagnostics& diag);

/// Pass 4: structural checks on the lowered framework.
void check(const Lowered& lo, Diagnostics& diag);

/// Pass 6: emit a device header.
[[nodiscard]] std::string emit_header(const Lowered& lo);

/// Pass 6: emit the ICCMA `.af` interchange format.
[[nodiscard]] std::string emit_af(const Lowered& lo);

/// Pass 6: emit the ASPARTIX `apx` interchange format.
[[nodiscard]] std::string emit_apx(const Lowered& lo);

/// The validated casebook as a binary data image for the prebuilt runtime firmware (format in src/blob.cpp). Carries the
/// same tables and the same fingerprint as emit_header; casebook_blob_equivalence proves the two decide identically.
[[nodiscard]] std::string emit_blob(const Lowered& lo);

/// One casebook's place inside a merged palette.
struct PaletteMember {
    std::string casebook;
    /// Bit offset of this casebook's observations within the combined presence mask.
    std::int32_t observation_offset = 0;
    std::int32_t observation_count = 0;
    /// Member-local argument index -> combined argument index.
    std::vector<std::int32_t> map;
    std::vector<std::int32_t> actions;  ///< combined indices
    std::int32_t idle_action = -1;
    std::int32_t safe_action = -1;
    std::int32_t closed_world = -1;
};

/// Several casebooks on one device.
///
/// The frameworks are unioned; the POLICIES ARE NOT. Each casebook keeps its own action set and its own fallback,
/// and the device solves once and applies every policy to that single labelling. Merging the policies would be a
/// defect: a pump action and a tank action being In at the same time is correct behaviour, and a merged policy
/// would call it `Conflicted`.
struct Palette {
    Lowered all;
    std::vector<PaletteMember> members;
};

/// Pass 3b: union several lowered casebooks into one device framework.
///
/// Every name is prefixed with its casebook, so the members are syntactically disjoint by construction -- which is
/// exactly the precondition ASPIC+'s non-interference postulate requires, and therefore what must be true before
/// the property can be tested at all.
///
/// Each member keeps its OWN closed-world argument. Sharing one would behave identically (both are unattacked and
/// so always In), but it would introduce a syntactic edge between members and make any non-interference result
/// vacuous.
[[nodiscard]] Palette merge(const std::vector<Lowered>& members, Diagnostics& diag);

/// Pass 6: emit a device header for a whole palette.
///
/// One framework, one solver, N policies. The frameworks are unioned; the policies deliberately are not, because a
/// pump action and a tank action being In at the same time is correct behaviour and a merged policy would call it
/// `Conflicted`.
[[nodiscard]] std::string emit_palette_header(const Palette& p);

/// A palette as a format-2 data image for the runtime firmware (layout in src/blob.cpp). No binding layer.
[[nodiscard]] std::string emit_palette_blob(const Palette& p);
[[nodiscard]] std::uint32_t palette_fingerprint(const Palette& p);

/// One action policy for symbolic validation: the action arguments `arbiter::decide` chooses among.
struct SymbolicPolicy {
    std::string name;
    std::vector<std::int32_t> actions;
};

/// Pass 5s: validate every state at once, symbolically (src/symbolic.cpp).
///
/// Computes the grounded labelling of every argument as a Boolean function of the presence bits, using binary
/// decision diagrams, then restates `arbiter::decide` over those functions. The result is the exact number of states
/// in each outcome without visiting them one by one, so a 30-reading casebook or a whole palette is checked in the
/// time the BDDs take to build rather than in 2^n solver runs.
///
/// Exhaustive validation remains the source of device outputs, because it also finds the worst-case state the
/// firmware measures; this pass does not compute per-state work. Where both can run, they must agree state for state,
/// and `test_symbolic` checks exactly that.
class SymbolicCheck {
public:
    SymbolicCheck(const Lowered& lo, const std::vector<SymbolicPolicy>& policies);
    ~SymbolicCheck();
    SymbolicCheck(const SymbolicCheck&) = delete;
    SymbolicCheck& operator=(const SymbolicCheck&) = delete;

    [[nodiscard]] std::string states() const;  ///< 2^n, in decimal
    /// States with the given `arbiter::Outcome` value (0 Decided .. 4 Faulted) under policy `policy`, in decimal.
    [[nodiscard]] std::string count(std::size_t policy, int outcome) const;
    [[nodiscard]] bool empty(std::size_t policy, int outcome) const;
    /// States in which `action` is the decided action.
    [[nodiscard]] std::string selected(std::size_t policy, std::int32_t action) const;
    [[nodiscard]] std::string in_count(std::int32_t argument) const;
    /// States in which at least one doubt is Undecided -- an odd cycle that survived lowering.
    [[nodiscard]] std::string undecided_doubt_states() const;
    [[nodiscard]] bool undecided_doubt_possible() const { return undecided_doubt_ != 0; }
    /// Evaluate in one state, for cross-checking against the device solver.
    [[nodiscard]] int outcome_at(std::size_t policy, std::uint64_t mask) const;
    [[nodiscard]] int label_at(std::int32_t argument, std::uint64_t mask) const;  ///< 1 In, -1 Out, 0 Undecided
    /// Which presence bits the policy's outcome depends on at all. For a palette, a member that depends on another
    /// member's bits is interference, PROVED absent or present over the whole combined space.
    [[nodiscard]] std::vector<bool> depends_on(std::size_t policy) const;
    [[nodiscard]] std::vector<bool> action_depends_on(std::int32_t action) const;
    [[nodiscard]] int rounds() const { return rounds_; }
    [[nodiscard]] std::size_t nodes() const;
    [[nodiscard]] double seconds() const { return seconds_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::int32_t observations_ = 0;
    std::uint32_t undecided_doubt_ = 0;
    int rounds_ = 0;
    double seconds_ = 0;
};

/// Pass 6: emit a human-readable report of what was generated.
[[nodiscard]] std::string emit_report(const Casebook& cb, const Lowered& lo);

}  // namespace quorum::cbc
