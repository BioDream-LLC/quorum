#ifndef QUORUM_RUNTIME_MODBUS_HPP
#define QUORUM_RUNTIME_MODBUS_HPP

// Modbus RTU slave: how a PLC talks to quorum.
//
// Modbus is what the controllers already in a plant speak, so this is the shortest path from "a rulebook on a chip" to
// "a rulebook beside the PLC". The PLC writes what it sees -- presence bits, or raw channel values that quorum's own
// binding turns into readings with hysteresis, holds and staleness -- and reads back the decision, the fallback flag
// and, if an AI or other controller proposed an action, what the guard did with it.
//
// Function codes: 03 read holding registers, 06 write single register, 16 write multiple registers. Anything else is
// answered with exception 01; an address outside the map with 02; a bad value with 03. A frame whose CRC is wrong is
// dropped silently, as the standard requires -- a garbled request must not be guessed at.
//
// REGISTER MAP, version 2 (16-bit holding registers, 0-based addresses). Version 2 added registers 10-12.
//
//     0  0x5155 ("QU")                       read only
//     1  map version (2)                     read only
//     2  fingerprint, high 16 bits           read only   -- the same number the device prints at start-up
//     3  fingerprint, low 16 bits            read only
//     4  observation count                   read only
//     5  member count (1 unless a palette)   read only
//     6  channel count                       read only
//     7  evaluation counter                  read only   -- increments every time the rulebook is decided
//     8  presence mask, high 16 bits         read/write  -- observations not driven by a channel
//     9  presence mask, low 16 bits          read/write
//    10  test clock: 1 if on                 read; write 0x7E57 to turn it on, only before anything else is
//                                                        written since start-up. It stays on until the board resets.
//    11  test clock moved, high 16 bits      read; write 11 and 12 in one function 16 to move the clock forward
//    12  test clock moved, low 16 bits             by that many ms (at most 24 days at a time), test clock on only
//   16 + 16*m, for member m:
//    +0  proposed action, as a position in the action list, 0xFFFF = none     read/write
//    +1  outcome: 0 Decided, 1 Indeterminate, 2 Unsatisfied, 3 Conflicted, 4 Faulted
//    +2  the rulebook's action, as a position (the fallback when not Decided)
//    +3  1 if that is the fallback
//    +4  the action to take after the guard, as a position (= +2 when nothing is proposed)
//    +5  1 if the proposal was accepted
//    +6  guard reason (runtime::GuardReason), 0xFFFF when nothing is proposed
//    +7  solver steps
//    +8  action count
//   256 + 2*c, for channel c: the latest sample in milli-units, int32, high word first   write
//          Writing the LOW word records the sample at the current time. Write both with one function 16.
//
// Every read is answered from a fresh evaluation at the time of the request, because holds mature and channels go
// stale with time alone; a PLC that stops writing a channel sees that channel become unavailable on schedule.
//
// Freestanding: no heap, no exceptions, <cstdint>/<cstddef> only.

#include <cstddef>
#include <cstdint>

namespace quorum::runtime::modbus {

/// CRC-16/MODBUS: polynomial 0xA001 reflected, initial 0xFFFF. Transmitted low byte first.
[[nodiscard]] inline std::uint16_t crc16(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint32_t c = 0xFFFFU;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) { c = (c & 1U) != 0U ? (c >> 1U) ^ 0xA001U : c >> 1U; }
    }
    return static_cast<std::uint16_t>(c);
}

enum class Exception : std::uint8_t { None = 0, IllegalFunction = 1, IllegalAddress = 2, IllegalValue = 3 };

/// What the slave asks of the device. Implemented by the firmware over its loaded image.
class Registers {
public:
    /// Read `count` registers from `address` into `out`. Return IllegalAddress if any is outside the map.
    virtual Exception read(std::uint16_t address, std::uint16_t count, std::uint16_t* out, std::uint32_t now_ms) = 0;
    /// Write `count` registers starting at `address`.
    virtual Exception write(std::uint16_t address, std::uint16_t count, const std::uint16_t* values,
                            std::uint32_t now_ms) = 0;

protected:
    ~Registers() = default;
};

/// Byte-at-a-time RTU framing. Frames are delimited by length (from the function code) and by an inter-byte gap: a
/// partial frame older than `kGapMs` is discarded, which is how RTU resynchronises after noise.
class Slave {
public:
    static constexpr std::size_t kMaxFrame = 256;
    static constexpr std::uint32_t kGapMs = 20;
    static constexpr std::uint16_t kMaxRead = 125;  ///< the standard's limit for function 03
    static constexpr std::uint16_t kMaxWrite = 123; ///< and for function 16

    explicit Slave(std::uint8_t address) noexcept : address_(address) {}

    /// Feed one received byte. Returns the length of a response now waiting in `response()`, or 0.
    [[nodiscard]] std::size_t feed(std::uint8_t byte, std::uint32_t now_ms, Registers& regs) noexcept {
        if (len_ > 0 && static_cast<std::uint32_t>(now_ms - last_ms_) > kGapMs) { len_ = 0; }
        last_ms_ = now_ms;
        if (len_ >= kMaxFrame) { len_ = 0; }
        in_[len_++] = byte;
        const std::size_t need = expected_length();
        if (need == 0 || len_ < need) { return 0; }
        const std::size_t frame = len_;
        len_ = 0;
        return handle(frame, now_ms, regs);
    }

    [[nodiscard]] const std::uint8_t* response() const noexcept { return out_; }
    [[nodiscard]] std::uint32_t frames_handled() const noexcept { return handled_; }
    [[nodiscard]] std::uint32_t crc_errors() const noexcept { return crc_errors_; }

private:
    /// Total request length implied by what has arrived, or 0 if not yet known.
    [[nodiscard]] std::size_t expected_length() const noexcept {
        if (len_ < 2) { return 0; }
        // Request lengths of the standard function codes, so an unsupported but well-formed request is still framed
        // correctly and answered with IllegalFunction rather than misread as several garbled frames.
        switch (in_[1]) {
            case 1: case 2: case 3: case 4: case 5: case 6: return 8;
            case 15: case 16: return len_ < 7 ? 0 : static_cast<std::size_t>(9U + in_[6]);
            case 7: case 11: case 12: case 17: return 4;
            default: return 8;  // unknown: the commonest shape; a wrong guess fails the CRC and resyncs on the gap
        }
    }

    [[nodiscard]] static std::uint16_t be16(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
    }

    std::size_t finish(std::size_t n) noexcept {
        const std::uint16_t c = crc16(out_, n);
        out_[n] = static_cast<std::uint8_t>(c & 0xFFU);
        out_[n + 1] = static_cast<std::uint8_t>(c >> 8U);
        return n + 2;
    }

    std::size_t exception(std::uint8_t function, Exception e) noexcept {
        out_[0] = address_;
        out_[1] = static_cast<std::uint8_t>(function | 0x80U);
        out_[2] = static_cast<std::uint8_t>(e);
        return finish(3);
    }

    std::size_t handle(std::size_t n, std::uint32_t now_ms, Registers& regs) noexcept {
        const std::uint16_t got = static_cast<std::uint16_t>(in_[n - 2] | (in_[n - 1] << 8U));
        if (crc16(in_, n - 2) != got) {
            ++crc_errors_;
            return 0;
        }
        // Address 0 is broadcast: act, never answer. Any other address but ours is not for us.
        const bool broadcast = in_[0] == 0;
        if (!broadcast && in_[0] != address_) { return 0; }
        ++handled_;
        const std::uint8_t fn = in_[1];
        std::size_t r = 0;
        if (fn == 3) {
            const std::uint16_t addr = be16(in_ + 2);
            const std::uint16_t count = be16(in_ + 4);
            if (count == 0 || count > kMaxRead) {
                r = exception(fn, Exception::IllegalValue);
            } else {
                std::uint16_t values[kMaxRead];
                const Exception e = regs.read(addr, count, values, now_ms);
                if (e != Exception::None) {
                    r = exception(fn, e);
                } else {
                    out_[0] = address_;
                    out_[1] = fn;
                    out_[2] = static_cast<std::uint8_t>(count * 2U);
                    for (std::uint16_t i = 0; i < count; ++i) {
                        out_[3 + 2 * i] = static_cast<std::uint8_t>(values[i] >> 8U);
                        out_[4 + 2 * i] = static_cast<std::uint8_t>(values[i] & 0xFFU);
                    }
                    r = finish(3U + 2U * count);
                }
            }
        } else if (fn == 6) {
            const std::uint16_t value = be16(in_ + 4);
            const Exception e = regs.write(be16(in_ + 2), 1, &value, now_ms);
            if (e != Exception::None) {
                r = exception(fn, e);
            } else {
                for (std::size_t i = 0; i < 6; ++i) { out_[i] = in_[i]; }  // echo
                r = finish(6);
            }
        } else if (fn == 16) {
            const std::uint16_t addr = be16(in_ + 2);
            const std::uint16_t count = be16(in_ + 4);
            if (count == 0 || count > kMaxWrite || in_[6] != count * 2U) {
                r = exception(fn, Exception::IllegalValue);
            } else {
                std::uint16_t values[kMaxWrite];
                for (std::uint16_t i = 0; i < count; ++i) { values[i] = be16(in_ + 7 + 2 * i); }
                const Exception e = regs.write(addr, count, values, now_ms);
                if (e != Exception::None) {
                    r = exception(fn, e);
                } else {
                    for (std::size_t i = 0; i < 6; ++i) { out_[i] = in_[i]; }
                    r = finish(6);
                }
            }
        } else {
            r = exception(fn, Exception::IllegalFunction);
        }
        return broadcast ? 0 : r;
    }

    std::uint8_t address_;
    std::uint8_t in_[kMaxFrame]{};
    std::uint8_t out_[kMaxFrame + 4]{};
    std::size_t len_ = 0;
    std::uint32_t last_ms_ = 0;
    std::uint32_t handled_ = 0;
    std::uint32_t crc_errors_ = 0;
};

}  // namespace quorum::runtime::modbus

#endif  // QUORUM_RUNTIME_MODBUS_HPP
