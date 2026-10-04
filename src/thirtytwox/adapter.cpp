#include "mercury/thirtytwox/adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mercury::thirtytwox {

namespace {
// Set GENX_32X_TRACE to watch the two sides talk. A 32X game's startup is a
// conversation through the communication ports, and when it stalls the only
// thing worth knowing is who said what and in which order.
bool tracing() {
    static const bool on = std::getenv("GENX_32X_TRACE") != nullptr;
    return on;
}

void trace_comm(const char* who, int port, std::uint16_t value) {
    if (tracing()) {
        std::fprintf(stderr, "[32x] %s writes comm%d = %04X\n", who, port, value);
    }
}

// The data request block, at the same offsets from either side. Source and
// target are 24-bit split across two words; the low words drop their odd bit
// and the length its low two, because transfers are word aligned.
constexpr std::uint32_t kDreqSourceHigh = 0x08;
constexpr std::uint32_t kDreqSourceLow = 0x0A;
constexpr std::uint32_t kDreqTargetHigh = 0x0C;
constexpr std::uint32_t kDreqTargetLow = 0x0E;
constexpr std::uint32_t kDreqLength = 0x10;
}  // namespace

void Adapter::reset() noexcept {
    // ADEN clear, so the 68000 sees an ordinary Mega Drive until a cartridge
    // asks for otherwise. REN and nRES are both already high at power-on -
    // PicoDrive marks that value verified against hardware - so the SH-2s are
    // not being held down; they just have no adapter to run against yet.
    control_ = kPowerOnControl;
    interrupt_control_ = 0;
    bank_ = 0;
    dreq_control_ = 0;
    comm_.fill(0);
    sh2_interrupt_mask_.fill(0);
    sh2_pending_.fill(0);
    sh2_hcount_ = 0;
    hint_countdown_ = 0;
    dreq_source_ = 0;
    dreq_target_ = 0;
    dreq_length_ = 0;
    tv_register_ = false;
    pwm_.reset();
    hint_during_vblank_ = false;
}

std::uint16_t Adapter::read16(std::uint32_t address) const noexcept {
    // Five address bits pick the register, so the 128-byte window folds onto
    // the 64-byte block - see kBlockLast. Working in offsets rather than
    // absolute addresses is what makes that fold free.
    const std::uint32_t even = address & kRegisterSelect;
    if (even >= (kCommFirst & kRegisterSelect) && even <= (kCommLast & kRegisterSelect)) {
        return comm_[(even - (kCommFirst & kRegisterSelect)) / 2];
    }
    if (even >= (kPwmFirst & kRegisterSelect)) {
        return pwm_.read16(even - (kPwmFirst & kRegisterSelect));
    }
    switch (even + kControl) {
        case kControl:
            // Bit 7 is REN, and it is simply high from power-on. An earlier
            // guess here synthesised bit 7 as "the SH-2s are up", inferred
            // from the 240p Test Suite sitting in `BTST #7,$A15101 / BEQ`.
            // That poll passes either way, so the cartridge could not tell
            // the two apart, but the guess was wrong: REN is a plain
            // read-only bit that no write from either side clears.
            return control_;
        case kInterruptControl: return interrupt_control_;
        case kBank: return bank_;
        // FULL is live: it is how the 68000 paces its writes into the FIFO.
        case kDreqControl:
            return static_cast<std::uint16_t>((dreq_control_ & ~kFull) |
                                              (dreq_fifo_full() ? kFull : 0u));
        case kTvRegister: return tv_register_ ? 0x0001u : 0x0000u;
        // The same data request block the processors see, from this side.
        case kControl + kDreqSourceHigh: return static_cast<std::uint16_t>(dreq_source_ >> 16);
        case kControl + kDreqSourceLow: return static_cast<std::uint16_t>(dreq_source_);
        case kControl + kDreqTargetHigh: return static_cast<std::uint16_t>(dreq_target_ >> 16);
        case kControl + kDreqTargetLow: return static_cast<std::uint16_t>(dreq_target_);
        case kControl + kDreqLength: return dreq_length_;
        default: return 0;
    }
}

void Adapter::write16(std::uint32_t address, std::uint16_t value) noexcept {
    const std::uint32_t even = address & kRegisterSelect;  // see read16
    if (even >= (kCommFirst & kRegisterSelect) && even <= (kCommLast & kRegisterSelect)) {
        const int port = static_cast<int>((even - (kCommFirst & kRegisterSelect)) / 2);
        trace_comm("68000", port, value);
        comm_[static_cast<std::size_t>(port)] = value;
        return;
    }
    if (even >= (kPwmFirst & kRegisterSelect)) {
        pwm_.write16(even - (kPwmFirst & kRegisterSelect), value);
        return;
    }
    switch (even + kControl) {
        case kControl: {
            // Only three bits of this register are writable from the Mega
            // Drive side: FM, nRES and ADEN. REN and everything else keep
            // whatever they had, which for REN means high for ever.
            constexpr std::uint16_t kWritable = kFm | kNotRes | kAden;
            std::uint16_t next = value;
            // Switching the adapter off also forces reset high. Not a
            // courtesy - it is what the hardware does, and it means a game
            // that clears ADEN cannot leave the SH-2s half alive.
            if ((control_ & kAden) != 0 && (next & kAden) == 0) {
                next |= kNotRes;
            }
            control_ = static_cast<std::uint16_t>((control_ & ~kWritable) | (next & kWritable));
            if (tracing()) {
                std::fprintf(stderr, "[32x] control write %04X -> %04X\n", value, control_);
            }
            return;
        }
        case kInterruptControl:
            // Bits 0 and 1 only; the rest of the register always reads zero.
            // The command interrupt follows this register rather than being
            // latched from it, so there is nothing to raise here - see
            // pending_sh2_level.
            interrupt_control_ = static_cast<std::uint16_t>(value & (kIntm | kInts));
            return;
        case kBank:
            // Four 512KB banks into the $900000 window, so two bits.
            bank_ = static_cast<std::uint16_t>(value & 0x0003u);
            return;
        case kDreqControl: {
            // FULL is the FIFO reporting on itself and is not writable.
            const bool was_open = (dreq_control_ & k68s) != 0;
            dreq_control_ = static_cast<std::uint16_t>(value & ~kFull);
            if (tracing()) {
                std::fprintf(stderr, "[32x] 68000 writes dreq control = %04X (fifo %zu, length %u)\n",
                             value, dreq_fifo_count_, static_cast<unsigned>(dreq_length_));
            }
            // Closing the request throws away whatever the SH-2s have not
            // taken yet, and the request line drops with it. ares and
            // PicoDrive agree on this.
            if (was_open && (dreq_control_ & k68s) == 0) {
                dreq_fifo_count_ = 0;
            }
            return;
        }
        // The FIFO. A word lands only while the 68000 has the request open and
        // there is room for it. The length counts words off and closes the
        // request when it runs out - without emptying the FIFO, because the
        // SH-2 side still has those words to take.
        case kDreqFifo:
            if ((dreq_control_ & k68s) == 0 || dreq_fifo_full()) {
                ++dreq_words_dropped_;
                if (tracing()) {
                    std::fprintf(stderr, "[32x] 68000 fifo push %04X DROPPED (%s)\n", value,
                                 (dreq_control_ & k68s) == 0 ? "request closed" : "full");
                }
                return;
            }
            dreq_fifo_[dreq_fifo_count_++] = value;
            ++dreq_words_in_;
            if (tracing() && dreq_words_in_ <= 400) {
                std::fprintf(stderr, "[32x] 68000 fifo push %04X (fifo %zu, length left %u)\n",
                             value, dreq_fifo_count_, static_cast<unsigned>(dreq_length_ - 1u));
            }
            if (dreq_length_ != 0 && --dreq_length_ == 0) {
                dreq_control_ = static_cast<std::uint16_t>(dreq_control_ & ~k68s);
            }
            return;
        case kTvRegister:
            tv_register_ = (value & 0x0001u) != 0;
            return;
        // The data request block, writable from this side too.
        case kControl + kDreqSourceHigh:
            dreq_source_ = (dreq_source_ & 0x00FFFFu) |
                           (static_cast<std::uint32_t>(value & 0xFFu) << 16);
            return;
        case kControl + kDreqSourceLow:
            dreq_source_ = (dreq_source_ & 0xFF0000u) | (value & 0xFFFEu);
            return;
        case kControl + kDreqTargetHigh:
            dreq_target_ = (dreq_target_ & 0x00FFFFu) |
                           (static_cast<std::uint32_t>(value & 0xFFu) << 16);
            return;
        case kControl + kDreqTargetLow:
            dreq_target_ = (dreq_target_ & 0xFF0000u) | (value & 0xFFFEu);
            return;
        case kControl + kDreqLength:
            dreq_length_ = static_cast<std::uint16_t>(value & 0xFFFCu);
            return;
        default:
            return;
    }
}

std::uint8_t Adapter::read8(std::uint32_t address) const noexcept {
    const std::uint16_t word = read16(address & ~1u);
    return (address & 1u) != 0 ? static_cast<std::uint8_t>(word)
                               : static_cast<std::uint8_t>(word >> 8);
}

void Adapter::write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t even = address & ~1u;
    const std::uint16_t word = read16(even);
    const std::uint16_t merged =
        (address & 1u) != 0
            ? static_cast<std::uint16_t>((word & 0xFF00u) | value)
            : static_cast<std::uint16_t>((word & 0x00FFu) | (static_cast<std::uint16_t>(value) << 8));
    write16(even, merged);
}

namespace {
// Offsets inside the SH-2's register page. Only the ones with behaviour.
constexpr std::uint32_t kSh2Control = 0x00;
constexpr std::uint32_t kSh2Interrupt = 0x02;
constexpr std::uint32_t kSh2Bank = 0x04;
constexpr std::uint32_t kSh2Dreq = 0x06;
constexpr std::uint32_t kSh2ClearFirst = 0x14;  // VRES, VINT, HINT, CMD, PWM
constexpr std::uint32_t kSh2ClearLast = 0x1C;
constexpr std::uint32_t kSh2CommFirst = 0x20;
constexpr std::uint32_t kSh2CommLast = 0x2E;
constexpr std::uint32_t kSh2PwmFirst = 0x30;
constexpr std::uint32_t kSh2PwmLast = 0x3E;
}  // namespace

std::uint16_t Adapter::sh2_read16(std::uint32_t offset, int which) const noexcept {
    const std::uint32_t even = offset & 0x3Eu;
    if (even >= kSh2CommFirst && even <= kSh2CommLast) {
        return comm_[(even - kSh2CommFirst) / 2];
    }
    if (even >= kSh2PwmFirst && even <= kSh2PwmLast) {
        return pwm_.read16(even - kSh2PwmFirst);
    }
    switch (even) {
        case kSh2Control: {
            std::uint16_t value = static_cast<std::uint16_t>(control_ & kFm);
            if (enabled()) {
                value |= kSh2Aden;
            }
            value |= sh2_interrupt_mask(which);
            if (hint_during_vblank_) {
                value |= kSh2Hen;
            }
            if (!cartridge_present_) {
                value |= kSh2NotCart;
            }
            return value;
        }
        case kSh2Interrupt: return 0;
        // $4004 is the horizontal interrupt count here, NOT the ROM bank the
        // 68000 sees at the same offset of its own block.
        case kSh2Bank: return sh2_hcount_;
        // The data request block reads back what was written - see
        // dreq_source(). Returning zero told a game its own setup vanished.
        case kDreqSourceHigh: return static_cast<std::uint16_t>(dreq_source_ >> 16);
        case kDreqSourceLow: return static_cast<std::uint16_t>(dreq_source_);
        case kDreqTargetHigh: return static_cast<std::uint16_t>(dreq_target_ >> 16);
        case kDreqTargetLow: return static_cast<std::uint16_t>(dreq_target_);
        case kDreqLength: return dreq_length_;
        // The FIFO data register, the word after the length. READING IT TAKES
        // A WORD OUT - it is a queue, not storage - and an empty FIFO reads as
        // zero. The reader is normally a DMA channel rather than software.
        case kDreqLength + 2: {
            static std::uint32_t traced_reads = 0;
            if (dreq_fifo_count_ == 0) {
                if (tracing() && traced_reads++ < 400) {
                    std::fprintf(stderr, "[32x] %s reads $4012 with the fifo EMPTY\n",
                                 which != 0 ? "slave" : "master");
                }
                return 0;
            }
            const std::uint16_t word = dreq_fifo_[0];
            for (std::size_t i = 1; i < dreq_fifo_count_; ++i) {
                dreq_fifo_[i - 1] = dreq_fifo_[i];
            }
            --dreq_fifo_count_;
            if (tracing() && traced_reads++ < 400) {
                std::fprintf(stderr, "[32x] %s takes %04X from $4012 (fifo %zu left)\n",
                             which != 0 ? "slave" : "master", word, dreq_fifo_count_);
            }
            return word;
        }
        case kSh2Dreq:
            // FULL and EMPTY, bits 15 and 14, are the FIFO reporting on itself
            // rather than whatever was written.
            return static_cast<std::uint16_t>((dreq_control_ & ~(kFull | 0xC000u)) |
                                              (dreq_fifo_full() ? 0x8000u : 0u) |
                                              (dreq_fifo_count_ == 0 ? 0x4000u : 0u));
        default: return 0;
    }
}

std::uint8_t Adapter::sh2_read8(std::uint32_t offset, int which) const noexcept {
    const std::uint16_t word = sh2_read16(offset & ~1u, which);
    return (offset & 1u) != 0 ? static_cast<std::uint8_t>(word)
                              : static_cast<std::uint8_t>(word >> 8);
}

void Adapter::sh2_write16(std::uint32_t offset, std::uint16_t value, int which) noexcept {
    const std::uint32_t even = offset & 0x3Eu;
    if (even >= kSh2CommFirst && even <= kSh2CommLast) {
        const int port = static_cast<int>((even - kSh2CommFirst) / 2);
        trace_comm(which != 0 ? "slave" : "master", port, value);
        comm_[static_cast<std::size_t>(port)] = value;
        return;
    }
    if (even >= kSh2PwmFirst && even <= kSh2PwmLast) {
        pwm_.write16(even - kSh2PwmFirst, value);
        return;
    }
    switch (even) {
        case kSh2Control: {
            // FM from the top of the word, and THE INTERRUPT MASK FROM THE
            // BOTTOM OF IT.
            //
            // This used to take FM and nothing else, on the reading that only
            // a byte write to the odd half could reach the mask. That is
            // wrong: a word write carries both halves, and its low half sets
            // the mask exactly as a byte write would. Ares does it this way
            // and it is the behaviour that matters - Knuckles' Chaotix sets
            // its master's mask with a word write, so with the low half
            // discarded the master had every interrupt masked off, could
            // never acknowledge the 68000's command, and the two sides sat
            // waiting on each other for ever.
            control_ = static_cast<std::uint16_t>((control_ & ~kFm) | (value & kFm));
            const std::size_t index = which != 0 ? 1 : 0;
            sh2_interrupt_mask_[index] = static_cast<std::uint8_t>(value & 0x0F);
            hint_during_vblank_ = (value & kSh2Hen) != 0;
            return;
        }
        case kSh2Bank:
            // A byte, and the low half of the word at that. See sh2_hcount.
            // The write also RESTARTS the count from a full period.
            sh2_hcount_ = static_cast<std::uint8_t>(value);
            hint_countdown_ = static_cast<int>(sh2_hcount_) + 1;
            return;
        case kDreqSourceHigh: dreq_source_ = (dreq_source_ & 0x00FFFFu) |
                                     (static_cast<std::uint32_t>(value & 0xFFu) << 16); return;
        case kDreqSourceLow:  dreq_source_ = (dreq_source_ & 0xFF0000u) | (value & 0xFFFEu); return;
        case kDreqTargetHigh: dreq_target_ = (dreq_target_ & 0x00FFFFu) |
                                     (static_cast<std::uint32_t>(value & 0xFFu) << 16); return;
        case kDreqTargetLow:  dreq_target_ = (dreq_target_ & 0xFF0000u) | (value & 0xFFFEu); return;
        case kDreqLength:     dreq_length_ = static_cast<std::uint16_t>(value & 0xFFFCu); return;
        default: {
            if (even < kSh2ClearFirst || even > kSh2ClearLast) {
                return;
            }
            // Interrupt acknowledgement, one register per source, and it
            // clears only for the processor that wrote it. These are what
            // make the interrupts levels rather than pulses: without the
            // write the interrupt stays standing.
            const std::size_t index = which != 0 ? 1 : 0;
            switch (even) {
                case 0x14: sh2_pending_[index] &= static_cast<std::uint8_t>(~kIrqVres); return;
                case 0x16: sh2_pending_[index] &= static_cast<std::uint8_t>(~kIrqVint); return;
                case 0x18: sh2_pending_[index] &= static_cast<std::uint8_t>(~kIrqHint); return;
                case 0x1A:
                    // The command interrupt lives in the 68000's register, so
                    // acknowledging it clears that processor's bit there.
                    interrupt_control_ = static_cast<std::uint16_t>(
                        interrupt_control_ & ~(index != 0 ? kInts : kIntm));
                    return;
                case 0x1C: sh2_pending_[index] &= static_cast<std::uint8_t>(~kIrqPwm); return;
                default: return;
            }
        }
    }
}

void Adapter::sh2_write8(std::uint32_t offset, std::uint8_t value, int which) noexcept {
    const std::uint32_t even = offset & 0x3Eu;
    if (even == kSh2Control) {
        if ((offset & 1u) == 0) {
            // The even half carries FM in its top bit.
            control_ = static_cast<std::uint16_t>(
                (control_ & ~kFm) | ((static_cast<std::uint16_t>(value) << 8) & kFm));
        } else {
            sh2_interrupt_mask_[which != 0 ? 1 : 0] = static_cast<std::uint8_t>(value & 0x0F);
            hint_during_vblank_ = (value & kSh2Hen) != 0;
        }
        return;
    }
    const std::uint16_t word = sh2_read16(even, which);
    const std::uint16_t merged =
        (offset & 1u) != 0
            ? static_cast<std::uint16_t>((word & 0xFF00u) | value)
            : static_cast<std::uint16_t>((word & 0x00FFu) | (static_cast<std::uint16_t>(value) << 8));
    sh2_write16(even, merged, which);
}

std::uint16_t Adapter::comm_read(int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(comm_.size())) {
        return 0;
    }
    return comm_[static_cast<std::size_t>(index)];
}

void Adapter::comm_write(int index, std::uint16_t value) noexcept {
    if (index < 0 || index >= static_cast<int>(comm_.size())) {
        return;
    }
    comm_[static_cast<std::size_t>(index)] = value;
}

void Adapter::raise_sh2_interrupt(std::uint8_t sources) noexcept {
    for (std::size_t which = 0; which < sh2_pending_.size(); ++which) {
        // Reset gets through whatever the mask says. Everything else needs
        // its enable, and the four enable bits sit three places below the
        // interrupts they gate.
        const auto allowed =
            static_cast<std::uint8_t>(kIrqVres | (sh2_interrupt_mask_[which] << 3));
        sh2_pending_[which] |= static_cast<std::uint8_t>(sources & allowed);
    }
}

std::string Adapter::describe() const {
    std::ostringstream out;
    out << "32X adapter: " << (enabled() ? "enabled" : "present but inert (ADEN clear)")
        << ", SH-2s " << (sh2_in_reset() ? "held in reset" : "running")
        << ", VDP owned by " << (sh2_owns_vdp() ? "the SH-2s" : "the Mega Drive")
        << ", ROM bank " << rom_bank();
    return out.str();
}

}  // namespace mercury::thirtytwox
