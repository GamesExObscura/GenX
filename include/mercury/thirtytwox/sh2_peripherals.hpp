#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mercury::thirtytwox {

class Bus;

// The SH-2's own on-chip peripherals, at $FFFFFE00 upward.
//
// ONE PER PROCESSOR. Everything else on the 32X is shared between the two
// SH-2s; this block is not. Each has its own timer, its own serial port and
// its own DMA controller, and a game uses them independently.
//
// Before this existed the whole block was undecoded, so every read came back
// as $FF. That is not a harmless "not implemented yet": $FF is a meaningful
// value in most of these registers. The timer output compare control register
// reads $E0 on real hardware, and bit 4 of it selects WHICH output compare
// register the two bytes at $FE14 answer for. Reading $FF sets that bit, so a
// game that reads the register, sets a bit and writes it back silently
// redirects every later timer write to the wrong register.
//
//   FE00-FE05  serial communication interface
//   FE10-FE19  free-running timer
//   FE60-FE68  interrupt vectors
//   FE80-FE83  watchdog timer
//   FEE0-FEE4  interrupt control and priorities
//   FF00-FF17  divider
//   FF80-FFB3  DMA controller
//   FFE0-FFFB  bus state controller
//
// Most of these are storage that software reads back and nothing else acts
// on, which is why the block is modelled as 512 bytes with behaviour attached
// only where something really happens.
class Sh2Peripherals {
  public:
    static constexpr std::uint32_t kBase = 0xFFFFFE00;
    static constexpr std::size_t kSize = 0x200;

    // Free-running timer, at their real addresses.
    static constexpr std::uint32_t kTier = 0x10;    // interrupt enables
    static constexpr std::uint32_t kFtcsr = 0x11;   // control and status flags
    static constexpr std::uint32_t kFrcHigh = 0x12;
    static constexpr std::uint32_t kFrcLow = 0x13;
    static constexpr std::uint32_t kOcrHigh = 0x14;
    static constexpr std::uint32_t kOcrLow = 0x15;
    static constexpr std::uint32_t kTcr = 0x16;     // clock select
    static constexpr std::uint32_t kTocr = 0x17;    // output compare select

    // The hardware divider, at $FFFFFF00. Software writes a divisor and a
    // dividend and reads back a quotient and remainder; there is no "go" bit,
    // the write to the last byte of the dividend IS the go. Every 32X game
    // doing perspective or scaling leans on it, and with it merely stored a
    // game reads its own dividend back as the answer.
    static constexpr std::uint32_t kDivisor = 0x100;
    static constexpr std::uint32_t kDividend32 = 0x104;   // write starts a 32/32 divide
    static constexpr std::uint32_t kDividendHigh = 0x110;  // and the remainder afterwards
    static constexpr std::uint32_t kDividendLow = 0x114;   // write starts a 64/32 divide
    static constexpr std::uint32_t kRemainderMirror = 0x118;
    static constexpr std::uint32_t kQuotientMirror = 0x11C;

    // The DMA controller, at $FFFFFF80: two channels of four longwords each,
    // then a shared enable.
    static constexpr std::uint32_t kDmaChannel0 = 0x180;
    static constexpr std::uint32_t kDmaChannelStride = 0x10;
    static constexpr std::uint32_t kDmaSource = 0x00;
    static constexpr std::uint32_t kDmaDestination = 0x04;
    static constexpr std::uint32_t kDmaCount = 0x08;
    static constexpr std::uint32_t kDmaControl = 0x0C;
    static constexpr std::uint32_t kDmaOperation = 0x1B0;

    // Channel control bits.
    static constexpr std::uint32_t kDmaDestinationDown = 0x8000;
    static constexpr std::uint32_t kDmaDestinationUp = 0x4000;
    static constexpr std::uint32_t kDmaSourceDown = 0x2000;
    static constexpr std::uint32_t kDmaSourceUp = 0x1000;
    static constexpr std::uint32_t kDmaSizeShift = 10;
    static constexpr std::uint32_t kDmaAutoRequest = 0x0200;
    static constexpr std::uint32_t kDmaInterruptEnable = 0x0004;
    static constexpr std::uint32_t kDmaTransferEnd = 0x0002;
    static constexpr std::uint32_t kDmaEnable = 0x0001;
    // Shared enable, bit 0 of the operation register.
    static constexpr std::uint32_t kDmaMasterEnable = 0x0001;

    // FTCSR flags.
    static constexpr std::uint8_t kOverflowFlag = 0x02;
    static constexpr std::uint8_t kCompareBFlag = 0x04;
    static constexpr std::uint8_t kCompareAFlag = 0x08;
    static constexpr std::uint8_t kClearOnCompareA = 0x01;
    // TIER enables, in the same bit positions as the flags they gate.
    static constexpr std::uint8_t kOverflowEnable = 0x02;
    static constexpr std::uint8_t kCompareBEnable = 0x04;
    static constexpr std::uint8_t kCompareAEnable = 0x08;
    // TOCR bit 4 picks which output compare register $FE14 addresses.
    static constexpr std::uint8_t kSelectCompareB = 0x10;

    // The interrupt controller's own registers, for the on-chip sources.
    // IPRB's high byte carries the timer's priority in its low nibble; VCRC's
    // low byte carries the output-compare vector and VCRD's high byte the
    // overflow vector, seven bits each. IPRA's LOW byte carries the watchdog's
    // priority in its HIGH nibble, and VCRWDT's high byte its vector.
    static constexpr std::uint32_t kIprbHigh = 0x60;
    static constexpr std::uint32_t kVcrcLow = 0x67;
    static constexpr std::uint32_t kVcrdHigh = 0x68;
    static constexpr std::uint32_t kIpraLow = 0xE3;
    static constexpr std::uint32_t kVcrwdtHigh = 0xE4;
    // IPRA's HIGH byte holds the DMA controller's priority in its low nibble
    // and the divider's in its high nibble. Their vectors are chosen per
    // channel (VCRDMA0/1) and per divider (VCRDIV).
    static constexpr std::uint32_t kIpraHigh = 0xE2;
    static constexpr std::uint32_t kVcrdiv = 0x10F;
    static constexpr std::uint32_t kVcrdma0 = 0x1A3;
    static constexpr std::uint32_t kVcrdma1 = 0x1AB;
    // DVCR's low byte: bit 0 is the overflow flag, bit 1 its interrupt enable.
    static constexpr std::uint32_t kDvcrLow = 0x10B;
    static constexpr std::uint8_t kDivideOverflow = 0x01;
    static constexpr std::uint8_t kDivideOverflowEnable = 0x02;

    // THE WATCHDOG, WHICH GAMES USE AS AN INTERVAL TIMER. Two registers behind
    // one address pair: a write to $FE80 is a SELECT byte ($A5 means "the next
    // byte is the control register", $5A means "the counter"), and the value
    // itself goes to $FE81. Reads are plain: $FE80 is the control register and
    // $FE81 the counter.
    static constexpr std::uint32_t kWdtSelect = 0x80;
    static constexpr std::uint32_t kWdtData = 0x81;
    static constexpr std::uint32_t kRstcsr = 0x83;
    static constexpr std::uint8_t kWdtSelectControl = 0xA5;
    static constexpr std::uint8_t kWdtSelectCounter = 0x5A;
    // WTCSR bits.
    static constexpr std::uint8_t kWdtOverflow = 0x80;
    static constexpr std::uint8_t kWdtWatchdogMode = 0x40;  // clear = interval timer
    static constexpr std::uint8_t kWdtEnable = 0x20;
    static constexpr std::uint8_t kWdtClockSelect = 0x07;

    // THE PROCESSOR'S OWN TIMER IS AN INTERRUPT SOURCE, and it was the missing
    // half of the 32X interrupt picture: the five adapter interrupts were
    // wired up and the SH-2's own were not, so a game that drives itself from
    // the free-running timer waited for ever.
    //
    // Star Wars Arcade is that game. Its slave copies a handler into the cache
    // at $C0000000, points the timer's vector at it, sets a flag in SDRAM and
    // spins until the handler clears it. With no timer interrupt the slave
    // never left that spin, so it never answered the master, the master never
    // answered the 68000, and all three sat waiting: a black screen with both
    // processors busy.
    //
    // Level, not latched: the request stands while an enabled flag is set in
    // FTCSR, and software clears the flag - the same shape as the adapter's
    // interrupts, and the reason set_interrupt_level takes the CURRENT level
    // every instruction. Compare A and B share the output-compare vector;
    // overflow has its own.
    // Both on-chip sources, highest priority first. The free-running timer
    // asks through FTCSR/TIER; the watchdog, in interval-timer mode, asks on
    // every overflow of its counter. Star Wars Arcade's slave runs on the
    // WATCHDOG: it writes $5AFF then $A538 to $FE80 (counter, then control
    // with TME set and interval mode), points the vector at a handler it has
    // copied into the SH-2 cache, and waits on a flag that only that handler
    // clears. Without the watchdog the slave waited for ever, the master
    // waited on the slave, the 68000 waited on the master, and the screen
    // stayed black with every processor busy.
    //
    // CACHED, because the scheduler asks before every instruction and the
    // answer only changes when a register is written or a timer raises a
    // flag. Recomputing it each time was a tenth of all host time on Star
    // Wars Arcade. Everything that can change the answer marks it stale;
    // anything new that writes regs_ must do the same.
    [[nodiscard]] int on_chip_interrupt_level() const noexcept {
        if (irq_stale_) refresh_interrupt();
        return cached_level_;
    }
    [[nodiscard]] int on_chip_interrupt_vector() const noexcept {
        if (irq_stale_) refresh_interrupt();
        return cached_vector_;
    }
    // The uncached answers, for tests and for checking the cache against.
    [[nodiscard]] int compute_on_chip_interrupt_level() const noexcept;
    [[nodiscard]] int compute_on_chip_interrupt_vector() const noexcept;
    [[nodiscard]] int frt_interrupt_level() const noexcept;
    [[nodiscard]] int frt_interrupt_vector() const noexcept;
    [[nodiscard]] int wdt_interrupt_level() const noexcept;
    [[nodiscard]] int wdt_interrupt_vector() const noexcept;
    // A finished DMA channel and a divider overflow are interrupt sources too,
    // each asserted while its flag and its enable are both set.
    [[nodiscard]] int dma_interrupt_level() const noexcept;
    [[nodiscard]] int dma_interrupt_vector() const noexcept;
    [[nodiscard]] int divide_interrupt_level() const noexcept;
    [[nodiscard]] int divide_interrupt_vector() const noexcept;

    [[nodiscard]] static bool decodes(std::uint32_t address) noexcept {
        return address >= kBase;
    }

    // THE CACHE, ADDRESSED AS MEMORY. The SH-2 can hand half its cache over
    // to software as 4KB of fast scratch RAM at $C0000000, and 32X games use
    // it for exactly that - including for code they run from.
    //
    // Per processor, like everything else here. GenX used to fold this
    // address onto the boot ROM, where it read as zeros: Knuckles' Chaotix
    // and Virtua Fighter both ended up executing at $C00005F0 and decoding
    // nothing but zero words, tens of millions of them.
    static constexpr std::uint32_t kDataArrayBase = 0xC0000000;
    static constexpr std::uint32_t kDataArrayEnd = 0xC1000000;
    static constexpr std::size_t kDataArraySize = 0x1000;

    [[nodiscard]] static bool decodes_data_array(std::uint32_t address) noexcept {
        return address >= kDataArrayBase && address < kDataArrayEnd;
    }
    [[nodiscard]] std::uint8_t read_data_array(std::uint32_t address) const noexcept {
        return data_array_[address & (kDataArraySize - 1)];
    }
    void write_data_array(std::uint32_t address, std::uint8_t value) noexcept {
        data_array_[address & (kDataArraySize - 1)] = value;
    }
    // For the processor's instruction fetch window - code runs from here.
    [[nodiscard]] const std::uint8_t* data_array() const noexcept { return data_array_.data(); }

    void reset() noexcept;
    // The DMA controller moves memory, so it needs the bus the rest of the
    // machine is on. Forward declared to keep the include one-way: Bus owns
    // pointers to these, not the other way about.
    void attach_bus(Bus* bus) noexcept { bus_ = bus; }

    // Not const: a read settles the timers first, so the counter it returns
    // is the counter as of now.
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) noexcept;
    void write8(std::uint32_t address, std::uint8_t value) noexcept;

    // The channel's DREQ pin is up: move ONE unit if the channel is armed for
    // external requests. Called between instructions while the 32X adapter's
    // FIFO holds anything, which is what that pin is wired to. A
    // self-triggering channel ignores it and runs from a register write
    // instead - see run_dma.
    void step_external_request(int channel) noexcept;

    [[nodiscard]] std::uint64_t divides() const noexcept { return divides_; }
    [[nodiscard]] std::uint64_t dma_transfers() const noexcept { return dma_transfers_; }
    [[nodiscard]] std::uint64_t dma_bytes() const noexcept { return dma_bytes_; }

    // Advance the free-running counter by that many SH-2 cycles. The counter
    // runs at the processor clock divided by 8, 32 or 128, chosen by the two
    // low bits of the timer control register; the fourth setting is an
    // external clock the 32X does not wire up, and it simply does not count.
    //
    // CALLED AFTER EVERY INSTRUCTION, SO IT MOSTLY JUST KEEPS COUNT. The
    // cycles are owed rather than spent until the next moment a timer could
    // raise a flag - a compare match, an overflow - and are paid off in one
    // go at exactly that instruction boundary, or earlier if software reads
    // or writes a register. Nothing outside can tell: the flags change at
    // the same instruction they always did, and every read settles first.
    void tick(std::uint32_t cycles) noexcept {
        owed_ += cycles;
        if (owed_ >= quiet_) {
            settle();
        }
    }
    // Pay off whatever is owed, now.
    void settle() noexcept;

    [[nodiscard]] std::uint16_t counter() const noexcept {
        const_cast<Sh2Peripherals*>(this)->settle();
        return counter_;
    }
    [[nodiscard]] std::uint64_t compare_matches() const noexcept {
        const_cast<Sh2Peripherals*>(this)->settle();
        return matches_;
    }

    [[nodiscard]] std::string describe() const;

    template <typename Archive>
    void visit_state(Archive& ar) {
        settle();  // a saved state holds the timers as they stand, nothing owed
        ar(regs_);
        ar(data_array_);
        ar(counter_);
        ar(fraction_);
        ar(compare_a_);
        ar(compare_b_);
        ar(wdt_select_);
        ar(wdt_counter_);
        ar(wdt_fraction_);
        irq_stale_ = true;  // a load replaces the registers underneath the cache
        owed_ = 0;
        quiet_ = 0;         // and the timers' plan with them
    }

  private:
    [[nodiscard]] std::uint8_t& at(std::uint32_t address) noexcept {
        return regs_[(address - kBase) & (kSize - 1)];
    }
    [[nodiscard]] std::uint8_t at(std::uint32_t address) const noexcept {
        return regs_[(address - kBase) & (kSize - 1)];
    }
    // Which output compare register the two bytes at $FE14 currently address.
    [[nodiscard]] bool selecting_compare_b() const noexcept {
        return (at(kBase + kTocr) & kSelectCompareB) != 0;
    }

    // The register block is byte storage, but the divider and the DMA
    // controller are longword registers, so both need a way in and out.
    [[nodiscard]] std::uint32_t read32(std::uint32_t offset) const noexcept;
    void write32(std::uint32_t offset, std::uint32_t value) noexcept;
    // Run a divide. Called when software writes the LAST BYTE of a dividend,
    // because that write is what starts one - there is no separate go bit.
    void divide32() noexcept;
    void divide64() noexcept;
    // Run one channel to completion, if it is enabled and self-triggering.
    void run_dma(int channel) noexcept;
    // Move one unit - byte, word, longword or a burst of four longwords -
    // and step the two addresses the way the control register says. Shared
    // by the self-triggering path and the external-request path so the two
    // cannot drift apart. Returns how many transfers' worth of count it used.
    std::uint32_t transfer_unit(std::uint32_t control, std::uint32_t& source,
                                std::uint32_t& destination) noexcept;
    // Whether a channel is armed and allowed to move at all.
    [[nodiscard]] bool dma_armed(std::uint32_t control) const noexcept;
    // Advance the watchdog's counter, raising its overflow flag.
    void tick_watchdog(std::uint32_t cycles) noexcept;
    // Both timers, by that many cycles, right now. What tick used to do on
    // every call.
    void advance(std::uint32_t cycles) noexcept;
    // How many cycles can pass before either timer could change a flag.
    [[nodiscard]] std::uint32_t quiet_cycles() const noexcept;
    // Cycles run but not yet applied to the timers, and how many may pile up
    // before one of them has something to say. quiet_ at zero forces the
    // next tick to settle and plan again, which is what a register write
    // does.
    std::uint32_t owed_ = 0;
    std::uint32_t quiet_ = 0;

    Bus* bus_ = nullptr;
    std::array<std::uint8_t, kSize> regs_{};
    std::array<std::uint8_t, kDataArraySize> data_array_{};
    std::uint64_t divides_ = 0;
    std::uint64_t dma_transfers_ = 0;
    std::uint64_t dma_bytes_ = 0;
    // The counter is kept apart from the register array because reading its
    // two halves must not tear and because it advances in fractions of a tick.
    std::uint16_t counter_ = 0;
    std::uint32_t fraction_ = 0;
    // Both compare registers, only one of which is visible at a time.
    std::uint16_t compare_a_ = 0xFFFF;
    std::uint16_t compare_b_ = 0xFFFF;
    std::uint64_t matches_ = 0;
    // The watchdog. Its counter and control register live behind one address
    // pair, chosen by the last SELECT byte written, so neither can be plain
    // storage in the register array.
    std::uint8_t wdt_select_ = 0;
    std::uint8_t wdt_counter_ = 0;
    std::uint32_t wdt_fraction_ = 0;
    std::uint64_t wdt_overflows_ = 0;

    // The on-chip interrupt request, as last computed. See
    // on_chip_interrupt_level.
    void refresh_interrupt() const noexcept {
        cached_level_ = compute_on_chip_interrupt_level();
        cached_vector_ = compute_on_chip_interrupt_vector();
        irq_stale_ = false;
    }
    mutable bool irq_stale_ = true;
    mutable int cached_level_ = 0;
    mutable int cached_vector_ = 0;
};

}  // namespace mercury::thirtytwox
