#include "mercury/thirtytwox/sh2_peripherals.hpp"

#include "mercury/thirtytwox/bus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mercury::thirtytwox {

namespace {
// Serial communication interface, the few registers with a non-zero value
// after reset.
constexpr std::uint32_t kBitRate = 0x01;
constexpr std::uint32_t kTransmitData = 0x03;
constexpr std::uint32_t kSerialStatus = 0x04;

// Processor cycles per timer tick, by the two low bits of the timer control
// register. The fourth setting is an external clock the 32X does not connect,
// so the counter stands still - which is a real state a game can ask for, not
// a gap here.
constexpr std::uint32_t kTickDivisors[4] = {8, 32, 128, 0};
}  // namespace

void Sh2Peripherals::reset() noexcept {
    regs_.fill(0);
    data_array_.fill(0);
    // The values the datasheet gives after a reset. They matter: every one of
    // them used to read as $FF, and software reads these back before writing
    // them.
    at(kBase + kBitRate) = 0xFF;
    at(kBase + kTransmitData) = 0xFF;
    at(kBase + kSerialStatus) = 0x84;
    at(kBase + kTier) = 0x01;
    at(kBase + kTocr) = 0xE0;
    // The watchdog powers on stopped, with its counter clear.
    at(kBase + kWdtSelect) = 0x18;
    at(kBase + kRstcsr) = 0x1F;
    wdt_select_ = 0;
    wdt_counter_ = 0;
    wdt_fraction_ = 0;
    wdt_overflows_ = 0;
    counter_ = 0;
    fraction_ = 0;
    compare_a_ = 0xFFFF;
    compare_b_ = 0xFFFF;
    matches_ = 0;
    divides_ = 0;
    dma_transfers_ = 0;
    dma_bytes_ = 0;
    irq_stale_ = true;
    owed_ = 0;
    quiet_ = 0;
}

std::uint8_t Sh2Peripherals::read8(std::uint32_t address) noexcept {
    settle();  // the counters as of this instruction, not as of the last flag
    const std::uint32_t offset = (address - kBase) & (kSize - 1);
    switch (offset) {
        // The watchdog reads back plainly even though it is written through a
        // select byte: $FE80 is the control register, $FE81 the counter.
        case kWdtData: return wdt_counter_;
        case kFrcHigh: return static_cast<std::uint8_t>(counter_ >> 8);
        case kFrcLow: return static_cast<std::uint8_t>(counter_);
        case kOcrHigh: {
            const std::uint16_t value = selecting_compare_b() ? compare_b_ : compare_a_;
            return static_cast<std::uint8_t>(value >> 8);
        }
        case kOcrLow: {
            const std::uint16_t value = selecting_compare_b() ? compare_b_ : compare_a_;
            return static_cast<std::uint8_t>(value);
        }
        default: return regs_[offset];
    }
}

void Sh2Peripherals::write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t offset = (address - kBase) & (kSize - 1);
    // Any register write can move the interrupt request - an enable, a
    // priority, a vector, a flag cleared, a divide or DMA finishing.
    irq_stale_ = true;
    // And the timers' plan: the cycles owed so far are paid at the OLD
    // settings, then the next tick plans afresh from the new ones.
    settle();
    quiet_ = 0;
    switch (offset) {
        // THE WATCHDOG IS WRITTEN THROUGH A SELECT BYTE. $FE80 says which of
        // the two registers behind $FE81 the next write means: $A5 the control
        // register, $5A the counter. A game writes both as one word - $A538,
        // $5AFF - and the bus splits that into these two byte writes in order.
        case kWdtSelect: wdt_select_ = value; return;
        case kWdtData: {
            if (wdt_select_ == kWdtSelectControl) {
                auto& control = at(kBase + kWdtSelect);
                // The overflow flag can only be CLEARED by software, like the
                // timer's flags: a zero in bit 7 clears it, a one leaves it.
                const std::uint8_t kept =
                    static_cast<std::uint8_t>(control & value & kWdtOverflow);
                // Bits 4 and 3 are reserved and READ AS ONE, which is why the
                // reset value is $18 and why a game's own read-modify-write of
                // this register does not quietly clear them.
                control = static_cast<std::uint8_t>(
                    (value & (kWdtWatchdogMode | kWdtEnable | kWdtClockSelect)) | kept | 0x18u);
                if ((control & kWdtEnable) == 0) {
                    wdt_counter_ = 0;
                    wdt_fraction_ = 0;
                }
            } else if (wdt_select_ == kWdtSelectCounter) {
                wdt_counter_ = value;
                wdt_fraction_ = 0;
            }
            return;
        }
        case kFtcsr:
            // The status flags are cleared by writing a zero over a bit that
            // is already set, and cannot be set by software. Only the
            // clear-on-compare-match bit is an ordinary read/write bit.
            regs_[offset] = static_cast<std::uint8_t>((regs_[offset] & value & 0x8E) |
                                                      (value & kClearOnCompareA));
            return;
        case kFrcHigh:
            counter_ = static_cast<std::uint16_t>((counter_ & 0x00FF) |
                                                  (static_cast<std::uint16_t>(value) << 8));
            return;
        case kFrcLow:
            counter_ = static_cast<std::uint16_t>((counter_ & 0xFF00) | value);
            return;
        case kOcrHigh: {
            std::uint16_t& target = selecting_compare_b() ? compare_b_ : compare_a_;
            target = static_cast<std::uint16_t>((target & 0x00FF) |
                                                (static_cast<std::uint16_t>(value) << 8));
            return;
        }
        case kOcrLow: {
            std::uint16_t& target = selecting_compare_b() ? compare_b_ : compare_a_;
            target = static_cast<std::uint16_t>((target & 0xFF00) | value);
            return;
        }
        default:
            regs_[offset] = value;
            // The divider and the DMA controller have no "go" bit: writing
            // the LAST BYTE of a dividend starts a divide, and writing the
            // last byte of a channel control register may start a transfer.
            // The bus splits every longword write into four byte writes in
            // order, so the last byte is where the whole value is known.
            switch (offset) {
                case kDividend32 + 3: divide32(); return;
                case kDividendLow + 3: divide64(); return;
                case kDmaChannel0 + kDmaControl + 3: run_dma(0); return;
                case kDmaChannel0 + kDmaChannelStride + kDmaControl + 3: run_dma(1); return;
                case kDmaOperation + 3:
                    // Enabling the controller can release channels that were
                    // already armed and waiting for it.
                    run_dma(0);
                    run_dma(1);
                    return;
                default: return;
            }
    }
}

std::uint32_t Sh2Peripherals::read32(std::uint32_t offset) const noexcept {
    const std::uint32_t at = offset & (kSize - 4);
    return (static_cast<std::uint32_t>(regs_[at]) << 24) |
           (static_cast<std::uint32_t>(regs_[at + 1]) << 16) |
           (static_cast<std::uint32_t>(regs_[at + 2]) << 8) |
           static_cast<std::uint32_t>(regs_[at + 3]);
}

void Sh2Peripherals::write32(std::uint32_t offset, std::uint32_t value) noexcept {
    irq_stale_ = true;
    const std::uint32_t at = offset & (kSize - 4);
    regs_[at] = static_cast<std::uint8_t>(value >> 24);
    regs_[at + 1] = static_cast<std::uint8_t>(value >> 16);
    regs_[at + 2] = static_cast<std::uint8_t>(value >> 8);
    regs_[at + 3] = static_cast<std::uint8_t>(value);
}

void Sh2Peripherals::divide32() noexcept {
    ++divides_;
    const auto divisor = static_cast<std::int32_t>(read32(kDivisor));
    const auto dividend = static_cast<std::int32_t>(read32(kDividend32));
    if (divisor == 0) {
        // Division by zero saturates and raises the overflow flag, which can
        // now interrupt: the flag used to be left alone because nothing read
        // it, and that was only safe while the divider could not interrupt.
        write32(kDividend32, 0x7FFFFFFF);
        write32(kDividendLow, 0x7FFFFFFF);
        write32(kQuotientMirror, 0x7FFFFFFF);
        at(kBase + kDvcrLow) |= kDivideOverflow;
        return;
    }
    const std::int32_t quotient = dividend / divisor;
    const std::int32_t remainder = dividend % divisor;
    // The quotient lands back in the register that started the divide as well
    // as in both quotient slots, and the remainder in both of its own. That
    // mirroring is not decoration: software reads whichever it happens to.
    write32(kDividend32, static_cast<std::uint32_t>(quotient));
    write32(kDividendLow, static_cast<std::uint32_t>(quotient));
    write32(kQuotientMirror, static_cast<std::uint32_t>(quotient));
    write32(kDividendHigh, static_cast<std::uint32_t>(remainder));
    write32(kRemainderMirror, static_cast<std::uint32_t>(remainder));
}

void Sh2Peripherals::divide64() noexcept {
    ++divides_;
    const auto divisor = static_cast<std::int32_t>(read32(kDivisor));
    if (divisor == 0) {
        // Saturate and flag, as the 32-bit path does.
        write32(kDividendLow, 0x7FFFFFFF);
        write32(kQuotientMirror, 0x7FFFFFFF);
        at(kBase + kDvcrLow) |= kDivideOverflow;
        return;
    }
    const auto dividend = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(read32(kDividendHigh)) << 32) | read32(kDividendLow));
    const std::int64_t remainder = dividend % divisor;
    std::int64_t quotient = dividend / divisor;
    write32(kDividendHigh, static_cast<std::uint32_t>(remainder));
    write32(kRemainderMirror, static_cast<std::uint32_t>(remainder));
    // A 64 by 32 divide can produce more than 32 bits of quotient, and the
    // hardware saturates rather than truncating - truncating would hand back
    // a plausible small number and send a game somewhere quietly wrong.
    if (quotient > 0x7FFFFFFF) {
        quotient = 0x7FFFFFFF;
        at(kBase + kDvcrLow) |= kDivideOverflow;
    } else if (quotient < -0x80000000LL) {
        quotient = -0x80000000LL;
        at(kBase + kDvcrLow) |= kDivideOverflow;
    }
    write32(kDividendLow, static_cast<std::uint32_t>(quotient));
    write32(kQuotientMirror, static_cast<std::uint32_t>(quotient));
}

bool Sh2Peripherals::dma_armed(std::uint32_t control) const noexcept {
    return (control & kDmaEnable) != 0 && (read32(kDmaOperation) & kDmaMasterEnable) != 0;
}

std::uint32_t Sh2Peripherals::transfer_unit(std::uint32_t control, std::uint32_t& source,
                                            std::uint32_t& destination) noexcept {
    const std::uint32_t size_select = (control >> kDmaSizeShift) & 3;
    // 0 byte, 1 word, 2 longword, 3 a burst of four longwords at a time.
    const std::uint32_t step = size_select == 3 ? 16u : (1u << size_select);
    switch (size_select) {
        case 0: bus_->write8(destination, bus_->read8(source)); break;
        case 1: bus_->write16(destination, bus_->read16(source)); break;
        case 2: bus_->write32(destination, bus_->read32(source)); break;
        default:
            for (std::uint32_t word = 0; word < 16; word += 4) {
                bus_->write32(destination + word, bus_->read32(source + word));
            }
            break;
    }
    dma_bytes_ += step;
    // The burst always walks the source forward, whatever the source mode
    // says; only the destination follows its own bits.
    if (size_select == 3) {
        source += step;
    } else if ((control & kDmaSourceUp) != 0) {
        source += step;
    } else if ((control & kDmaSourceDown) != 0) {
        source -= step;
    }
    if ((control & kDmaDestinationUp) != 0) {
        destination += step;
    } else if ((control & kDmaDestinationDown) != 0) {
        destination -= step;
    }
    // A burst retires four transfers' worth of count, not one.
    return size_select == 3 ? 4u : 1u;
}

void Sh2Peripherals::run_dma(int channel) noexcept {
    if (bus_ == nullptr) {
        return;
    }
    const std::uint32_t base = kDmaChannel0 + static_cast<std::uint32_t>(channel) *
                                                  kDmaChannelStride;
    const std::uint32_t control = read32(base + kDmaControl);
    // Three things must agree before anything moves: the channel is enabled,
    // the controller as a whole is enabled, and the channel asks for its own
    // transfers. A channel waiting on an external request line moves one unit
    // at a time as that line is sampled - see step_external_request.
    if (!dma_armed(control) || (control & kDmaAutoRequest) == 0) {
        return;
    }

    std::uint32_t source = read32(base + kDmaSource);
    std::uint32_t destination = read32(base + kDmaDestination);
    std::uint32_t count = read32(base + kDmaCount) & 0x00FFFFFF;

    // GENX_32X_TRACE: each self-triggering transfer, as it starts. Where it
    // reads from and writes to is the whole story when memory gets overwritten.
    static const bool tracing = std::getenv("GENX_32X_TRACE") != nullptr;
    static std::uint32_t traced = 0;
    if (tracing && traced < 200) {
        ++traced;
        std::fprintf(stderr, "[32x] dma%d auto %08X -> %08X count %u control %04X\n", channel,
                     source, destination, static_cast<unsigned>(count),
                     static_cast<unsigned>(control & 0xFFFF));
    }

    ++dma_transfers_;
    while (count != 0) {
        const std::uint32_t retired = transfer_unit(control, source, destination);
        count = count > retired ? count - retired : 0u;
    }

    write32(base + kDmaSource, source);
    write32(base + kDmaDestination, destination);
    write32(base + kDmaCount, 0);
    // Done: the end flag goes up and the enable comes down, which is what
    // software polls for.
    write32(base + kDmaControl,
            (control & ~kDmaEnable) | kDmaTransferEnd);
}

void Sh2Peripherals::step_external_request(int channel) noexcept {
    if (bus_ == nullptr) {
        return;
    }
    const std::uint32_t base = kDmaChannel0 + static_cast<std::uint32_t>(channel) *
                                                  kDmaChannelStride;
    const std::uint32_t control = read32(base + kDmaControl);
    // Only a channel armed for EXTERNAL requests answers the pin, and one that
    // has already finished waits for software to clear its end flag before
    // it will move again - which is also what ares and PicoDrive require.
    if (!dma_armed(control) || (control & kDmaAutoRequest) != 0 ||
        (control & kDmaTransferEnd) != 0) {
        return;
    }
    std::uint32_t count = read32(base + kDmaCount) & 0x00FFFFFF;
    if (count == 0) {
        return;
    }
    std::uint32_t source = read32(base + kDmaSource);
    std::uint32_t destination = read32(base + kDmaDestination);
    const std::uint32_t written_at = destination;
    const std::uint32_t retired = transfer_unit(control, source, destination);
    count = count > retired ? count - retired : 0u;
    // GENX_32X_TRACE: where each externally requested unit landed and what
    // it was, read back through the bus. The first few hundred only.
    static const bool tracing = std::getenv("GENX_32X_TRACE") != nullptr;
    static std::uint32_t traced = 0;
    if (tracing && traced < 400) {
        ++traced;
        std::fprintf(stderr, "[32x] dma%d external unit -> %08X = %04X (count left %u)\n",
                     channel, written_at, bus_->read16(written_at),
                     static_cast<unsigned>(count));
    }
    write32(base + kDmaSource, source);
    write32(base + kDmaDestination, destination);
    write32(base + kDmaCount, count);
    if (count == 0) {
        // The same finish the self-triggering path gives it.
        ++dma_transfers_;
        write32(base + kDmaControl, (control & ~kDmaEnable) | kDmaTransferEnd);
    }
}

void Sh2Peripherals::settle() noexcept {
    if (owed_ != 0) {
        const std::uint32_t cycles = owed_;
        owed_ = 0;
        advance(cycles);
    }
    quiet_ = quiet_cycles();
}

std::uint32_t Sh2Peripherals::quiet_cycles() const noexcept {
    // Settle at least this often even with both timers stopped, so the count
    // owed can never grow without bound.
    std::uint32_t quiet = 1u << 20;
    const std::uint32_t divisor = kTickDivisors[at(kBase + kTcr) & 0x03];
    if (divisor != 0) {
        // Ticks until the counter next lands on something that sets a flag:
        // either compare register, or the wrap to zero. A distance of zero
        // means a whole lap away. A compare-A clear to zero happens on the
        // compare-A tick itself, so it is covered by that distance.
        const auto lap = [](std::uint32_t distance) { return distance == 0 ? 0x10000u : distance; };
        std::uint32_t ticks = lap(static_cast<std::uint16_t>(compare_a_ - counter_));
        ticks = std::min(ticks, lap(static_cast<std::uint16_t>(compare_b_ - counter_)));
        ticks = std::min(ticks, lap(static_cast<std::uint16_t>(0u - counter_)));
        // That tick arrives once the fraction reaches ticks x divisor. Right
        // after a clock change the fraction can already be past a tick at
        // the new rate, and then the answer is "settle at once", not a
        // subtraction that wraps round to never.
        const std::uint32_t due = ticks * divisor;
        quiet = fraction_ >= due ? 0u : std::min(quiet, due - fraction_);
    }
    const auto control = at(kBase + kWdtSelect);
    if ((control & kWdtEnable) != 0) {
        static constexpr std::uint32_t kDivisors[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
        const std::uint32_t wdt_divisor = kDivisors[control & kWdtClockSelect];
        const std::uint32_t ticks = 0x100u - wdt_counter_;  // to the overflow
        const std::uint32_t due = ticks * wdt_divisor;
        quiet = wdt_fraction_ >= due ? 0u : std::min(quiet, due - wdt_fraction_);
    }
    return quiet;
}

void Sh2Peripherals::advance(std::uint32_t cycles) noexcept {
    tick_watchdog(cycles);
    const std::uint32_t select = at(kBase + kTcr) & 0x03;
    const std::uint32_t divisor = kTickDivisors[select];
    if (divisor == 0) {
        return;  // waiting on a clock pin nothing drives
    }
    // Called after every instruction, and nearly always with less than one
    // tick owed, so that case returns before any arithmetic. The divisors are
    // powers of two, so what is left is a shift and a mask - the same answer
    // the divide gave, without a divide per instruction.
    fraction_ += cycles;
    if (fraction_ < divisor) {
        return;
    }
    static constexpr std::uint32_t kTickShifts[4] = {3, 5, 7, 0};
    std::uint32_t ticks = fraction_ >> kTickShifts[select];
    fraction_ &= divisor - 1;
    // Step rather than jump, because a compare match anywhere inside the step
    // has to be seen. A long slice with a short compare period would
    // otherwise skip the match entirely and a game waiting on it would hang -
    // the same class of fault as never running the timer at all.
    //
    // Capped so that a game which leaves the timer on its fastest setting and
    // a long catch-up slice cannot cost an unbounded loop; anything past a
    // full period of the counter is indistinguishable anyway.
    if (ticks > 0x20000) {
        ticks = 0x20000;
    }
    // Ticks that cannot land on a compare value or the wrap are plain
    // increments, so skip straight past them and step only from the first
    // one that can. With the cycles owed until exactly that tick (see
    // quiet_cycles), the loop below is usually one or two steps.
    {
        const auto lap = [](std::uint32_t distance) { return distance == 0 ? 0x10000u : distance; };
        std::uint32_t first = lap(static_cast<std::uint16_t>(compare_a_ - counter_));
        first = std::min(first, lap(static_cast<std::uint16_t>(compare_b_ - counter_)));
        first = std::min(first, lap(static_cast<std::uint16_t>(0u - counter_)));
        const std::uint32_t plain = ticks < first ? ticks : first - 1;
        counter_ = static_cast<std::uint16_t>(counter_ + plain);
        ticks -= plain;
    }
    auto& status = at(kBase + kFtcsr);
    const std::uint8_t status_before = status;
    for (std::uint32_t step = 0; step < ticks; ++step) {
        const auto next = static_cast<std::uint16_t>(counter_ + 1);
        if (next == 0) {
            status |= kOverflowFlag;
        }
        counter_ = next;
        if (counter_ == compare_a_) {
            status |= kCompareAFlag;
            ++matches_;
            // The counter restarts from zero on a match with A when software
            // has asked for it, which is how a periodic tick is built.
            if ((status & kClearOnCompareA) != 0) {
                counter_ = 0;
            }
        }
        if (counter_ == compare_b_) {
            status |= kCompareBFlag;
            ++matches_;
        }
    }
    if (status != status_before) {
        irq_stale_ = true;
    }
}

namespace {
// Both halves of the question "is the timer asking for attention", so the
// level and the vector cannot disagree about which source won.
struct FrtRequest {
    bool compare = false;
    bool overflow = false;
    [[nodiscard]] bool any() const noexcept { return compare || overflow; }
};
}  // namespace

void Sh2Peripherals::tick_watchdog(std::uint32_t cycles) noexcept {
    auto& control = at(kBase + kWdtSelect);
    if ((control & kWdtEnable) == 0) {
        return;  // not started
    }
    // The watchdog's eight clock settings, from the datasheet. Powers of two,
    // so a shift and a mask - see tick.
    static constexpr std::uint32_t kDivisors[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
    static constexpr std::uint32_t kShifts[8] = {1, 6, 7, 8, 9, 10, 12, 13};
    const std::uint32_t select = control & kWdtClockSelect;
    const std::uint32_t divisor = kDivisors[select];
    wdt_fraction_ += cycles;
    if (wdt_fraction_ < divisor) {
        return;
    }
    std::uint32_t ticks = wdt_fraction_ >> kShifts[select];
    wdt_fraction_ &= divisor - 1;
    // Capped for the same reason the free-running timer's step is: a long
    // catch-up slice must not cost an unbounded loop, and more than a full
    // period of an eight-bit counter is indistinguishable anyway.
    if (ticks > 0x200) {
        ticks = 0x200;
    }
    // Counted in one go rather than a tick at a time - the counter is eight
    // bits and nothing happens between overflows, so the carry out of the
    // sum is exactly how many times it wrapped.
    const std::uint32_t total = static_cast<std::uint32_t>(wdt_counter_) + ticks;
    if (const std::uint32_t overflows = total >> 8; overflows != 0) {
        // An overflow in interval-timer mode asks for an interrupt and the
        // counter carries on from zero. The flag stays up until software
        // clears it, which is what holds the request asserted.
        control |= kWdtOverflow;
        wdt_overflows_ += overflows;
        irq_stale_ = true;
    }
    wdt_counter_ = static_cast<std::uint8_t>(total);
}

int Sh2Peripherals::wdt_interrupt_level() const noexcept {
    const auto control = at(kBase + kWdtSelect);
    // Only the interval-timer mode interrupts. In watchdog mode an overflow
    // resets the machine instead, which is not modelled and which no 32X game
    // asks for.
    if ((control & kWdtWatchdogMode) != 0 || (control & kWdtOverflow) == 0) {
        return 0;
    }
    return static_cast<int>((at(kBase + kIpraLow) >> 4) & 0x0F);
}

int Sh2Peripherals::wdt_interrupt_vector() const noexcept {
    return static_cast<int>(at(kBase + kVcrwdtHigh) & 0x7F);
}

int Sh2Peripherals::dma_interrupt_level() const noexcept {
    // Either channel, while it has finished and asked to be told about it.
    for (int channel = 0; channel < 2; ++channel) {
        const std::uint32_t base =
            kDmaChannel0 + static_cast<std::uint32_t>(channel) * kDmaChannelStride;
        const std::uint32_t control = read32(base + kDmaControl);
        if ((control & kDmaTransferEnd) != 0 && (control & kDmaInterruptEnable) != 0) {
            return static_cast<int>(at(kBase + kIpraHigh) & 0x0F);
        }
    }
    return 0;
}

int Sh2Peripherals::dma_interrupt_vector() const noexcept {
    const std::uint32_t control0 = read32(kDmaChannel0 + kDmaControl);
    const bool first = (control0 & kDmaTransferEnd) != 0 && (control0 & kDmaInterruptEnable) != 0;
    return static_cast<int>(at(kBase + (first ? kVcrdma0 : kVcrdma1)));
}

int Sh2Peripherals::divide_interrupt_level() const noexcept {
    const auto control = at(kBase + kDvcrLow);
    if ((control & kDivideOverflow) == 0 || (control & kDivideOverflowEnable) == 0) {
        return 0;
    }
    return static_cast<int>((at(kBase + kIpraHigh) >> 4) & 0x0F);
}

int Sh2Peripherals::divide_interrupt_vector() const noexcept {
    return static_cast<int>(at(kBase + kVcrdiv) & 0x7F);
}

int Sh2Peripherals::compute_on_chip_interrupt_level() const noexcept {
    int level = frt_interrupt_level();
    for (const int other : {wdt_interrupt_level(), dma_interrupt_level(),
                            divide_interrupt_level()}) {
        if (other > level) level = other;
    }
    return level;
}

int Sh2Peripherals::compute_on_chip_interrupt_vector() const noexcept {
    // Whichever source owns the level this returns the vector for, so the two
    // cannot disagree. Ties go to the earlier source, as the controller's own
    // fixed priority order does.
    const int level = compute_on_chip_interrupt_level();
    if (level == 0) {
        return 0;
    }
    if (wdt_interrupt_level() == level) return wdt_interrupt_vector();
    if (dma_interrupt_level() == level) return dma_interrupt_vector();
    if (divide_interrupt_level() == level) return divide_interrupt_vector();
    return frt_interrupt_vector();
}

int Sh2Peripherals::frt_interrupt_level() const noexcept {
    const auto enables = at(kBase + kTier);
    const auto flags = at(kBase + kFtcsr);
    const FrtRequest request{
        ((enables & kCompareAEnable) != 0 && (flags & kCompareAFlag) != 0) ||
            ((enables & kCompareBEnable) != 0 && (flags & kCompareBFlag) != 0),
        (enables & kOverflowEnable) != 0 && (flags & kOverflowFlag) != 0};
    if (!request.any()) {
        return 0;
    }
    // Priority zero means "do not interrupt me", which is the reset state and
    // is why this cannot simply assume a level.
    return static_cast<int>(at(kBase + kIprbHigh) & 0x0F);
}

int Sh2Peripherals::frt_interrupt_vector() const noexcept {
    const auto enables = at(kBase + kTier);
    const auto flags = at(kBase + kFtcsr);
    const bool compare = ((enables & kCompareAEnable) != 0 && (flags & kCompareAFlag) != 0) ||
                         ((enables & kCompareBEnable) != 0 && (flags & kCompareBFlag) != 0);
    return static_cast<int>(compare ? (at(kBase + kVcrcLow) & 0x7F)
                                    : (at(kBase + kVcrdHigh) & 0x7F));
}

std::string Sh2Peripherals::describe() const {
    const_cast<Sh2Peripherals*>(this)->settle();  // report the counters as of now
    std::ostringstream out;
    const std::uint32_t divisor = kTickDivisors[at(kBase + kTcr) & 0x03];
    out << "SH-2 timer: ";
    if (divisor == 0) {
        out << "external clock, not counting";
    } else {
        out << "clock/" << divisor;
    }
    out << ", counter " << counter_ << ", compare A " << compare_a_ << ", compare B "
        << compare_b_ << ", " << matches_ << " matches";
    // The interrupt side of the timer, because "the timer is running" and "the
    // timer can interrupt" are different questions and only the second one
    // gets a game out of a wait loop. Printed as raw registers: TIER says
    // which flags are allowed to interrupt, FTCSR which are set.
    out << std::hex << std::uppercase << "; tier " << static_cast<unsigned>(at(kBase + kTier))
        << " ftcsr " << static_cast<unsigned>(at(kBase + kFtcsr)) << " iprb.frt "
        << static_cast<unsigned>(at(kBase + kIprbHigh) & 0x0F) << " vector "
        << static_cast<unsigned>(at(kBase + kVcrcLow) & 0x7F) << std::dec << std::nouppercase
        << " -> level " << frt_interrupt_level();
    return out.str();
}

}  // namespace mercury::thirtytwox
