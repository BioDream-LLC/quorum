#ifndef QUORUM_RUNTIME_MODBUS_MAP_HPP
#define QUORUM_RUNTIME_MODBUS_MAP_HPP

// The register map of modbus.hpp, implemented over a loaded image: one casebook (with its binding) or a palette.
// Freestanding: no heap, no exceptions.

#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/guard.hpp"
#include "quorum/runtime/layout.hpp"
#include "quorum/runtime/modbus.hpp"

namespace quorum::runtime::modbus {

constexpr std::uint16_t kId = 0x5155U;
constexpr std::uint16_t kMapVersion = 2U;
/// Written to register 10 to turn the test clock on. See write().
constexpr std::uint16_t kTestClockKey = 0x7E57U;
/// The most one write may move the test clock forward: 24 days, the longest window a timed reading may have, so the
/// timers' unsigned time differences can never wrap within one jump.
constexpr std::uint32_t kMaxClockJumpMs = 24U * 86400000U;
constexpr std::uint16_t kMemberBase = 16U;
constexpr std::uint16_t kMemberStride = 16U;
constexpr std::uint16_t kChannelBase = 256U;
constexpr std::uint16_t kNone = 0xFFFFU;

class QuorumMap final : public Registers {
public:
    /// One casebook. `binding` must already be configured from it (make_binding), or the casebook has no channels;
    /// likewise `timers` (make_timers), or it has no timed readings.
    void attach(const Casebook& cb, RtBinding* binding, RtFramework& fw, RtSolver& solver, const RtPolicy& policy,
                RtTimers* timers = nullptr) noexcept {
        cb_ = &cb;
        pal_ = nullptr;
        binding_ = cb.channel_count > 0 ? binding : nullptr;
        timers_ = cb.timer_count > 0 ? timers : nullptr;
        fw_ = &fw;
        solver_ = &solver;
        policy_[0] = &policy;
        members_ = 1;
        channel_bits_ = 0;
        for (std::int32_t c = 0; c < cb.channel_count; ++c) {
            channel_bits_ |= obs_mask(cb.channels[c].unavailable_obs) | obs_mask(cb.channels[c].flatline_obs);
        }
        for (std::int32_t t = 0; t < cb.threshold_count; ++t) { channel_bits_ |= obs_mask(cb.thresholds[t].observation); }
        // Timed readings are set by the device, never written from outside, exactly like channel readings. A timed
        // casebook whose timers did not configure keeps those bits clear rather than letting a master set them.
        for (std::int32_t t = 0; t < cb.timer_count; ++t) { channel_bits_ |= obs_mask(cb.timers[t].observation); }
        reset_proposals();
        offset_ms_ = 0;
        test_clock_ = false;
        written_ = false;
    }

    /// A palette: no binding, the presence mask is written directly. `policies[m]` per member.
    void attach(const Palette& p, RtFramework& fw, RtSolver& solver, const RtPolicy* policies) noexcept {
        pal_ = &p;
        cb_ = nullptr;
        binding_ = nullptr;
        timers_ = nullptr;
        fw_ = &fw;
        solver_ = &solver;
        members_ = p.member_count;
        for (std::int32_t m = 0; m < members_; ++m) { policy_[m] = &policies[m]; }
        channel_bits_ = 0;
        reset_proposals();
    }

    Exception read(std::uint16_t address, std::uint16_t count, std::uint16_t* out, std::uint32_t now_ms) override {
        evaluate(now_ms + offset_ms_);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!get(static_cast<std::uint16_t>(address + i), out[i])) { return Exception::IllegalAddress; }
        }
        return Exception::None;
    }

    Exception write(std::uint16_t address, std::uint16_t count, const std::uint16_t* v, std::uint32_t now_ms) override {
        // Validate the whole request before changing anything, so a rejected write has no partial effect.
        for (std::uint16_t i = 0; i < count; ++i) {
            const Exception e = check(static_cast<std::uint16_t>(address + i), v[i]);
            if (e != Exception::None) { return e; }
        }
        // The test clock: a bench moves the board's clock forward, so that a 1-hour window can be tested in seconds.
        // It can be turned on only before anything else has been written since start-up, it stays on until the board
        // is reset, and register 10 shows it. A controller in service writes readings from its first scan, so the
        // test clock cannot be turned on behind its back without a reset, and a controller can check register 10.
        const bool touches_clock = address <= 12U && static_cast<std::uint32_t>(address) + count > 10U;
        if (touches_clock) {
            // Turning it on: register 10 alone, the key, and nothing else written yet.
            const bool turn_on = address == 10U && count == 1U && v[0] == kTestClockKey && !written_;
            // Moving it: 11 and 12 together in one function 16, at most 24 days, and only once it is on.
            bool advance = false;
            if (address == 11U && count == 2U && test_clock_) {
                advance = ((static_cast<std::uint32_t>(v[0]) << 16U) | v[1]) <= kMaxClockJumpMs;
            }
            if (!turn_on && !advance) { return Exception::IllegalValue; }
        }
        if (address == 10U) {
            test_clock_ = true;
            return Exception::None;
        }
        if (address == 11U && count == 2U) {
            // Step at the old time, then at the new one, so every timer passes through the jump as through real time
            // with no reading changing in between.
            tick(now_ms);
            std::uint32_t left = (static_cast<std::uint32_t>(v[0]) << 16U) | v[1];
            // Stop at each moment a timed reading expires on the way, as the board would in real time, so that a
            // timed reading watching another sees it expire then and not at the end of the jump.
            std::uint32_t in_ms = 0;
            // It also stops 1 ms before, the last moment the reading is still set, which a board ticking every
            // millisecond would have seen too.
            while (timers_ != nullptr && left > 0U && timers_->next_change(now_ms + offset_ms_, in_ms) && in_ms <= left) {
                if (in_ms > 1U) {
                    offset_ms_ += in_ms - 1U;
                    left -= in_ms - 1U;
                    tick(now_ms);
                }
                offset_ms_ += 1U;
                left -= 1U;
                tick(now_ms);
            }
            offset_ms_ += left;
            tick(now_ms);
            return Exception::None;
        }
        written_ = true;
        // Step the timed readings on the state before this write, then again after it, so that every state the
        // readings pass through is seen: a reading written set and cleared by the next request, with no read or tick
        // between, still counts, and so does a hold that matured with time alone just before a sample cleared it.
        tick(now_ms);
        for (std::uint16_t i = 0; i < count; ++i) {
            const auto a = static_cast<std::uint16_t>(address + i);
            if (a == 8U) { mask_ = (mask_ & 0x0000FFFFU) | (static_cast<std::uint32_t>(v[i]) << 16U); }
            if (a == 9U) { mask_ = (mask_ & 0xFFFF0000U) | v[i]; }
            if (a >= kMemberBase && a < kChannelBase && static_cast<std::uint32_t>(a - kMemberBase) % kMemberStride == 0U) {
                proposal_[static_cast<std::uint32_t>(a - kMemberBase) / kMemberStride] = v[i];
            }
            if (a >= kChannelBase) {
                const auto c = static_cast<std::int32_t>(static_cast<std::uint32_t>(a - kChannelBase) / 2U);
                if (static_cast<std::uint32_t>(a - kChannelBase) % 2U == 0U) {
                    pending_high_[c] = v[i];
                } else if (binding_ != nullptr) {
                    const auto milli = static_cast<std::int32_t>((static_cast<std::uint32_t>(pending_high_[c]) << 16U) | v[i]);
                    (void)binding_->sample(c, milli, now_ms + offset_ms_);
                }
            }
        }
        tick(now_ms);
        return Exception::None;
    }

    /// Step the timed readings without deciding. The firmware calls this every millisecond or so between requests,
    /// so a timer sees a reading come and go even when no master is polling. Does nothing without timers.
    void tick(std::uint32_t now_ms) noexcept { (void)base_and_timers(now_ms + offset_ms_); }

    [[nodiscard]] std::uint32_t evaluations() const noexcept { return evaluations_; }
    [[nodiscard]] std::uint32_t presence() const noexcept { return presence_; }

private:
    /// Not `bit`: the Arduino core #defines bit(b), and a function of that name is rewritten by the preprocessor.
    [[nodiscard]] static std::uint32_t obs_mask(std::int32_t obs) noexcept {
        return obs < 0 || obs >= 32 ? 0U : 1U << static_cast<std::uint32_t>(obs);
    }

    void reset_proposals() noexcept {
        for (std::uint16_t& p : proposal_) { p = kNone; }
        for (std::uint16_t& h : pending_high_) { h = 0; }
        mask_ = 0;
    }

    [[nodiscard]] std::int32_t observations() const noexcept {
        return cb_ != nullptr ? cb_->observation_count : pal_->observation_count;
    }
    [[nodiscard]] std::int32_t channels() const noexcept { return cb_ != nullptr ? cb_->channel_count : 0; }
    [[nodiscard]] const std::int32_t* actions(std::int32_t m) const noexcept {
        return cb_ != nullptr ? cb_->actions : pal_->members[m].actions;
    }
    [[nodiscard]] std::int32_t action_count(std::int32_t m) const noexcept {
        return cb_ != nullptr ? cb_->action_count : pal_->members[m].action_count;
    }
    [[nodiscard]] std::uint16_t position(std::int32_t m, std::int32_t arg) const noexcept {
        for (std::int32_t k = 0; k < action_count(m); ++k) {
            if (actions(m)[k] == arg) { return static_cast<std::uint16_t>(k); }
        }
        return kNone;
    }

    [[nodiscard]] Exception check(std::uint16_t a, std::uint16_t v) const noexcept {
        if (a >= 10U && a <= 12U) { return Exception::None; }  // the test clock, checked in write()
        if (a == 8U || a == 9U) {
            const std::int32_t n = observations();
            const std::uint32_t allowed = n >= 32 ? 0xFFFFFFFFU : (1U << static_cast<std::uint32_t>(n)) - 1U;
            const std::uint32_t bits = a == 8U ? static_cast<std::uint32_t>(v) << 16U : v;
            return (bits & ~allowed) != 0U ? Exception::IllegalValue : Exception::None;
        }
        if (a >= kMemberBase && a < kChannelBase) {
            const auto m = static_cast<std::int32_t>(static_cast<std::uint32_t>(a - kMemberBase) / kMemberStride);
            if (static_cast<std::uint32_t>(a - kMemberBase) % kMemberStride != 0U || m >= members_) { return Exception::IllegalAddress; }
            return v == kNone || v < action_count(m) ? Exception::None : Exception::IllegalValue;
        }
        if (a >= kChannelBase && static_cast<std::int32_t>(static_cast<std::uint32_t>(a - kChannelBase) / 2U) < channels()) {
            return Exception::None;
        }
        return Exception::IllegalAddress;
    }

    /// The presence mask: written readings, then channel readings, then timed readings worked out from both.
    [[nodiscard]] std::uint32_t base_and_timers(std::uint32_t now_ms) noexcept {
        const std::uint32_t base = (mask_ & ~channel_bits_) | (binding_ != nullptr ? binding_->mask(now_ms) : 0U);
        return timers_ != nullptr ? base | timers_->step(base, now_ms) : base;
    }

    void evaluate(std::uint32_t now_ms) noexcept {
        presence_ = base_and_timers(now_ms);
        const bool built = (cb_ != nullptr ? instantiate(*fw_, *cb_, presence_) : instantiate(*fw_, *pal_, presence_)) ==
                           solver::Status::Ok;
        ++evaluations_;
        for (std::int32_t m = 0; m < members_; ++m) {
            arbiter::Verdict v{};
            if (built) { v = arbiter::decide(*fw_, *solver_, *policy_[m]); }
            out_[m][0] = static_cast<std::uint16_t>(v.outcome);
            out_[m][1] = v.action.valid() ? position(m, v.action.get()) : kNone;
            out_[m][2] = v.is_fallback ? 1U : 0U;
            if (proposal_[m] == kNone || !v.action.valid()) {
                out_[m][3] = out_[m][1];
                out_[m][4] = 0U;
                out_[m][5] = kNone;
            } else {
                const GuardResult g = cb_ != nullptr ? guard(*cb_, v, actions(m)[proposal_[m]])
                                                     : guard(*pal_, m, v, actions(m)[proposal_[m]]);
                out_[m][3] = position(m, g.output);
                out_[m][4] = g.accepted ? 1U : 0U;
                out_[m][5] = static_cast<std::uint16_t>(g.reason);
            }
            out_[m][6] = static_cast<std::uint16_t>(v.steps);
        }
    }

    [[nodiscard]] bool get(std::uint16_t a, std::uint16_t& v) const noexcept {
        const std::uint32_t fp = cb_ != nullptr ? cb_->fingerprint : pal_->fingerprint;
        switch (a) {
            case 0: v = kId; return true;
            case 1: v = kMapVersion; return true;
            case 2: v = static_cast<std::uint16_t>(fp >> 16U); return true;
            case 3: v = static_cast<std::uint16_t>(fp & 0xFFFFU); return true;
            case 4: v = static_cast<std::uint16_t>(observations()); return true;
            case 5: v = static_cast<std::uint16_t>(members_); return true;
            case 6: v = static_cast<std::uint16_t>(channels()); return true;
            case 7: v = static_cast<std::uint16_t>(evaluations_ & 0xFFFFU); return true;
            case 8: v = static_cast<std::uint16_t>(mask_ >> 16U); return true;
            case 9: v = static_cast<std::uint16_t>(mask_ & 0xFFFFU); return true;
            case 10: v = test_clock_ ? 1U : 0U; return true;
            case 11: v = static_cast<std::uint16_t>(offset_ms_ >> 16U); return true;
            case 12: v = static_cast<std::uint16_t>(offset_ms_ & 0xFFFFU); return true;
            default: break;
        }
        if (a >= kMemberBase && a < kChannelBase) {
            const auto m = static_cast<std::int32_t>(static_cast<std::uint32_t>(a - kMemberBase) / kMemberStride);
            const auto k = static_cast<std::int32_t>(static_cast<std::uint32_t>(a - kMemberBase) % kMemberStride);
            if (m >= members_ || k > 8) { return false; }
            if (k == 0) { v = proposal_[m]; return true; }
            if (k == 8) { v = static_cast<std::uint16_t>(action_count(m)); return true; }
            v = out_[m][k - 1];
            return true;
        }
        return false;  // channel registers are write-only
    }

    const Casebook* cb_ = nullptr;
    const Palette* pal_ = nullptr;
    RtBinding* binding_ = nullptr;
    RtTimers* timers_ = nullptr;
    RtFramework* fw_ = nullptr;
    RtSolver* solver_ = nullptr;
    const RtPolicy* policy_[kMaxMembers]{};
    std::int32_t members_ = 0;
    std::uint32_t channel_bits_ = 0;
    std::uint32_t mask_ = 0;
    std::uint32_t presence_ = 0;
    std::uint32_t evaluations_ = 0;
    std::uint32_t offset_ms_ = 0;  ///< how far the test clock has moved the board's clock forward
    bool test_clock_ = false;
    bool written_ = false;         ///< anything other than the test clock written since start-up
    std::uint16_t proposal_[kMaxMembers]{};
    std::uint16_t pending_high_[kMaxChannels]{};
    std::uint16_t out_[kMaxMembers][7]{};
};

}  // namespace quorum::runtime::modbus

#endif  // QUORUM_RUNTIME_MODBUS_MAP_HPP
