#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string>

#include "mercury/thirtytwox/pwm.hpp"

namespace mercury::thirtytwox {

// The 32X adapter's shared registers - the part both the Mega Drive's 68000
// and the two SH-2s can see.
//
// GenX keeps the adapter ALWAYS CONNECTED, which is what people did with the
// real thing: it passes a Mega Drive cartridge straight through and there was
// no reason to unplug it. That is only safe because of ADEN.
//
// ADEN, bit 0 of the adapter control register, is the whole story. Sega's
// manual says that at power-on "address space of 68000 is mapped the same as
// the Mega Drive unit" - the adapter is inert until software sets ADEN. So
// "always connected" does NOT mean "always intruding": a Mega Drive cartridge
// that never touches $A15100 cannot tell the difference, which is the
// property that protects the 694 Mega Drive games that already work.
//
// The register block itself IS always decoded, because reading it is how a
// cartridge detects an adapter in the first place.
class Adapter {
  public:
    // 68000 side, all in the $A15100 block.
    static constexpr std::uint32_t kControl = 0xA15100;
    static constexpr std::uint32_t kInterruptControl = 0xA15102;
    static constexpr std::uint32_t kBank = 0xA15104;
    static constexpr std::uint32_t kDreqControl = 0xA15106;
    // The 68000 -> SH-2 FIFO's data register - see dreq_line().
    static constexpr std::uint32_t kDreqFifo = 0xA15112;
    // $A1511A from the Mega Drive. Bit 0 only, read and write, and it is NOT
    // the same register the processors see at that offset - theirs is the
    // command interrupt acknowledge. Third time this trap has come up, after
    // the bank register and the horizontal count.
    static constexpr std::uint32_t kTvRegister = 0xA1511A;
    static constexpr std::uint32_t kCommFirst = 0xA15120;
    static constexpr std::uint32_t kCommLast = 0xA1512E;
    static constexpr std::uint32_t kPwmFirst = 0xA15130;
    static constexpr std::uint32_t kPwmLast = 0xA1513E;

    // Adapter control bits, $A15100.
    //
    // These were guessed once and were wrong: nRES was put at bit 5 and REN at
    // bit 6. PicoDrive's register map has them at bits 1 and 7, and its
    // power-on value is marked "verified" against hardware, so that is what
    // GenX uses now. The old numbering happened to work only because GenX was
    // both the writer and the reader of its own private bits; a cartridge
    // driving the real ones would have been misread completely.
    static constexpr std::uint16_t kAden = 0x0001;    // adapter enabled
    static constexpr std::uint16_t kNotRes = 0x0002;  // nRES: 0 holds the SH-2s in reset
    static constexpr std::uint16_t kRen = 0x0080;     // read-only, set from power-on
    static constexpr std::uint16_t kFm = 0x8000;      // who owns the 32X VDP

    // What the register reads as before anything writes it. ADEN clear, so the
    // adapter is inert; nRES already high, so the SH-2s are not being held -
    // they simply have nowhere to run until ADEN arrives.
    static constexpr std::uint16_t kPowerOnControl = kRen | kNotRes;

    // Interrupt control bits, $A15102. Bit 0 the master, bit 1 the slave, and
    // nothing else in the register is writable.
    static constexpr std::uint16_t kIntm = 0x0001;  // interrupt the master SH-2
    static constexpr std::uint16_t kInts = 0x0002;  // ...and the slave

    // DREQ control bits, $A15106. 68S opens the 68000 -> SH-2 FIFO, and
    // closing it flushes whatever the SH-2s have not taken. FULL is live.
    // See dreq_line().
    static constexpr std::uint16_t kFull = 0x0080;  // DREQ FIFO full, read-only
    static constexpr std::uint16_t k68s = 0x0004;
    static constexpr std::uint16_t kDma = 0x0002;
    static constexpr std::uint16_t kRv = 0x0001;

    void reset() noexcept;

    // Does this 68000 address belong to the adapter? Deliberately narrow: the
    // register block only. The ROM windows at $880000 and $900000 are the
    // cartridge's business and are handled where cartridge mapping lives, so
    // that a plain Mega Drive game's reads cannot be intercepted here by
    // accident.
    // THE WINDOW IS 128 BYTES AND IT MIRRORS.
    //
    // The register block itself is 64 bytes, but the Mega Drive decodes
    // $A15100-$A1517F for it and selects a register with five address bits,
    // so the upper half is the lower half again. Decoding only the first 64
    // bytes leaves a game reading $A15140 with open bus where the hardware
    // would have answered. Taken from the MiSTer core's address decode, which
    // is a gate-level model rather than a reading of the manual.
    static constexpr std::uint32_t kBlockLast = 0xA1517F;
    static constexpr std::uint32_t kRegisterSelect = 0x3E;

    [[nodiscard]] static bool decodes(std::uint32_t address) noexcept {
        return address >= kControl && address <= kBlockLast;
    }

    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const noexcept;
    void write16(std::uint32_t address, std::uint16_t value) noexcept;
    // Byte access picks a half of the addressed word, the same as everywhere
    // else on this 16-bit bus.
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const noexcept;
    void write8(std::uint32_t address, std::uint8_t value) noexcept;

    // The same eight words from the SH-2 side, where they live at
    // $20004020 upward. One storage, two windows - that is the point of them.
    [[nodiscard]] std::uint16_t comm_read(int index) const noexcept;
    void comm_write(int index, std::uint16_t value) noexcept;

    // ---- The SH-2 side of the register page, at offset 0 to $3F of $4000.
    //
    // The communication ports really are one storage seen from two places.
    // The CONTROL REGISTER IS NOT. $A15100 and $4000 are different registers
    // that happen to share the FM bit, and treating them as one is not a
    // harmless simplification: the 240p Test Suite's slave SH-2 writes its
    // own interrupt mask to $4001, which under the 68000's bit assignments
    // reads as "clear ADEN" and switched the whole adapter off three frames
    // into the game.
    //
    // From an SH-2, this register carries FM, a read-only copy of ADEN up at
    // bit 9, a flag for "no cartridge", and that processor's own four
    // interrupt enables. `which` is 0 for the master and 1 for the slave,
    // because the mask is per-processor and nothing else here is.
    static constexpr std::uint16_t kSh2Aden = 0x0200;  // ADEN, read-only, as the SH-2 sees it
    // Bit 7 is HEN, and it enables the HORIZONTAL INTERRUPT DURING VERTICAL
    // BLANK. It was labelled "cartridge not inserted" here, which is the bit
    // one place higher; the two sit next to each other in the register map
    // and the short names look alike.
    static constexpr std::uint8_t kSh2Hen = 0x80;
    // nCART, bit 8: NO CARTRIDGE IS PLUGGED IN. It is the bit above HEN, and
    // it is the one thing that makes a Sega CD 32X disc boot.
    //
    // The master's firmware branches on it. With a cartridge it does the
    // initial data load out of the cartridge header and jumps in. Without
    // one - which is every CD 32X title, because the disc unit occupies the
    // slot - there is nothing to load from, so it instead waits for the
    // 68000 to put "_CD_" in the first communication port, reads a descriptor
    // the 68000 left in the frame buffer, copies the SH-2 program from there
    // into SDRAM and jumps to the entry point in it. The slave's firmware
    // branches the same way.
    //
    // GenX reports the bit from whether a cartridge ROM is attached, so a
    // disc gets it for free. Reported wrong, the firmware takes the cartridge
    // path, reads a startup header out of a slot with nothing in it and both
    // processors run off into empty space.
    static constexpr std::uint16_t kSh2NotCart = 0x0100;
    // The four enables, and they sit exactly three bits below the interrupts
    // they gate. Bit 3 enables the VERTICAL interrupt, not reset - reset is
    // not maskable at all, which is easy to misread from the register map
    // where the bit is labelled only "V".
    static constexpr std::uint8_t kSh2IntVint = 0x08;
    static constexpr std::uint8_t kSh2IntHint = 0x04;
    static constexpr std::uint8_t kSh2IntCmd = 0x02;
    static constexpr std::uint8_t kSh2IntPwm = 0x01;

    [[nodiscard]] std::uint16_t sh2_read16(std::uint32_t offset, int which) const noexcept;
    [[nodiscard]] std::uint8_t sh2_read8(std::uint32_t offset, int which) const noexcept;
    void sh2_write16(std::uint32_t offset, std::uint16_t value, int which) noexcept;
    void sh2_write8(std::uint32_t offset, std::uint8_t value, int which) noexcept;

    [[nodiscard]] std::uint8_t sh2_interrupt_mask(int which) const noexcept {
        return sh2_interrupt_mask_[which != 0 ? 1 : 0];
    }
    // How many scanlines between horizontal interrupts, less one. Lives at
    // $4004 on the SH-2 side, which is the SAME ADDRESS as the 68000's ROM
    // bank register at $A15104 and a completely different register. Sharing
    // one storage for the two makes a game's bank switching move its raster
    // timing, and its raster timing move its banks.
    [[nodiscard]] std::uint8_t sh2_hcount() const noexcept { return sh2_hcount_; }
    // Writing the period RESTARTS the count, so a game that reprograms it
    // mid-frame gets a full period from that moment rather than whatever was
    // left of the old one. Kept here with the register rather than in the
    // scheduler, because the write is what resets it.
    [[nodiscard]] int hint_countdown() const noexcept { return hint_countdown_; }
    void set_hint_countdown(int lines) noexcept { hint_countdown_ = lines; }

    // The data request registers. Not acted on - transfers driven from the
    // Mega Drive side are not modelled - but they READ BACK what was written,
    // because software sets them up and reads them again. Returning zero
    // there tells a game its own setup did not take.
    [[nodiscard]] std::uint32_t dreq_source() const noexcept { return dreq_source_; }
    [[nodiscard]] std::uint32_t dreq_target() const noexcept { return dreq_target_; }
    [[nodiscard]] std::uint16_t dreq_length() const noexcept { return dreq_length_; }

    // THE 68000 -> SH-2 FIFO, and the request line it drives.
    //
    // The 68000 pushes words at $A15112 while the 68S bit of $A15106 is set;
    // an SH-2 takes them out by reading $4012. Nothing on the SH-2 side reads
    // that register by hand. A DMA channel in external-request mode does,
    // with its source pointed at $4012, copying one word each time this line
    // is up - and the line is simply "the FIFO is not empty".
    //
    // This is how a 32X game hands its drawing commands across. Knuckles'
    // Chaotix acknowledges the 68000's command interrupt by arming exactly
    // such a channel into a command buffer its main loop then interprets.
    // With no FIFO the buffer stayed zero, the loop only ever saw "nothing to
    // do", and the frame buffer got 255 bytes in 600 frames.
    [[nodiscard]] bool dreq_line() const noexcept { return dreq_fifo_count_ != 0; }
    [[nodiscard]] bool dreq_fifo_full() const noexcept {
        return dreq_fifo_count_ == dreq_fifo_.size();
    }
    [[nodiscard]] std::uint64_t dreq_words_in() const noexcept { return dreq_words_in_; }
    [[nodiscard]] std::uint64_t dreq_words_dropped() const noexcept { return dreq_words_dropped_; }
    // HEN: whether the horizontal interrupt also fires while the beam is in
    // vertical blank. Off, it fires only during the active picture.
    [[nodiscard]] bool hint_during_vblank() const noexcept { return hint_during_vblank_; }

    // Pulse width modulation audio, reachable from both sides at the top of
    // this register block.
    [[nodiscard]] Pwm& pwm() noexcept { return pwm_; }
    [[nodiscard]] const Pwm& pwm() const noexcept { return pwm_; }

    // Whether the cartridge slot has anything in it. Deliberately NOT part of
    // reset() or of the saved state: it is a property of what is loaded, not
    // a register, and the machine sets it whenever a medium is attached.
    void set_cartridge_present(bool present) noexcept { cartridge_present_ = present; }
    [[nodiscard]] bool cartridge_present() const noexcept { return cartridge_present_; }

    [[nodiscard]] bool enabled() const noexcept { return (control_ & kAden) != 0; }
    // Both SH-2s are held in reset until the Mega Drive side lets them go.
    // This is normal for most of a game's startup, and the slave often stays
    // there far longer than the master.
    [[nodiscard]] bool sh2_in_reset() const noexcept { return (control_ & kNotRes) == 0; }
    // The pair only actually execute when the adapter is on AND reset is
    // released. Either alone is not enough, and the two can be written in
    // either order, so the machine watches this rather than one bit.
    [[nodiscard]] bool sh2_running() const noexcept {
        return (control_ & (kAden | kNotRes)) == (kAden | kNotRes);
    }
    // Which side may touch the 32X VDP right now. Both writing at once is
    // undefined on hardware, and the manual warns about it explicitly.
    [[nodiscard]] bool sh2_owns_vdp() const noexcept { return (control_ & kFm) != 0; }
    [[nodiscard]] int rom_bank() const noexcept { return bank_ & 0x03; }

    // ---- The five interrupts the 32X raises into each SH-2.
    //
    // These are LEVELS, held until the processor writes the matching clear
    // register, not pulses. The bit positions are chosen so that the bit
    // index is half the interrupt's priority level, because the SH-2 takes
    // these on its IRL pins and auto-vectors them: vector = 64 + level / 2.
    //
    // Without these a 32X game draws exactly one frame and stops. The 240p
    // Test Suite did precisely that: one complete picture in the bank it was
    // not showing, then nothing for the next 600 frames, because its main
    // loop was waiting on a vertical interrupt that never came.
    static constexpr std::uint8_t kIrqVres = 0x80;  // level 14, never masked
    static constexpr std::uint8_t kIrqVint = 0x40;  // level 12
    static constexpr std::uint8_t kIrqHint = 0x20;  // level 10
    static constexpr std::uint8_t kIrqCmd = 0x10;   // level 8
    static constexpr std::uint8_t kIrqPwm = 0x08;   // level 6

    // Raise these sources in both processors, each gated by its own mask.
    // Reset is the exception: it is not maskable.
    void raise_sh2_interrupt(std::uint8_t sources) noexcept;
    // The highest level standing against one processor, or 0 for none.
    // Inline, because the scheduler asks before every SH-2 instruction.
    [[nodiscard]] int pending_sh2_level(int which) const noexcept {
        const std::size_t index = which != 0 ? 1 : 0;
        unsigned pending = sh2_pending_[index];
        // The command interrupt is not latched. It stands for exactly as long
        // as the 68000 leaves its bit set in the interrupt control register,
        // so a game that clears the register drops it without the SH-2 doing
        // anything.
        const std::uint16_t asked = index != 0 ? kInts : kIntm;
        if ((interrupt_control_ & asked) != 0 && (sh2_interrupt_mask_[index] & kSh2IntCmd) != 0) {
            pending |= kIrqCmd;
        }
        // Highest bit set wins, and the level is twice its index.
        return pending == 0 ? 0 : (std::bit_width(pending) - 1) * 2;
    }
    [[nodiscard]] static constexpr int vector_for_level(int level) noexcept {
        return 64 + level / 2;
    }

    [[nodiscard]] std::uint16_t control() const noexcept { return control_; }
    [[nodiscard]] std::string describe() const;

    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(control_);
        ar(interrupt_control_);
        ar(bank_);
        ar(dreq_control_);
        ar(comm_);
        ar(sh2_interrupt_mask_);
        ar(sh2_pending_);
        ar(sh2_hcount_);
        ar(hint_countdown_);
        ar(dreq_source_);
        ar(dreq_target_);
        ar(dreq_length_);
        ar(tv_register_);
        pwm_.visit_state(ar);
        ar(hint_during_vblank_);
        // The FIFO's words, not only how many there are: a state taken in the
        // middle of a DREQ transfer resumes it with the data it was carrying.
        ar(dreq_fifo_);
        ar(dreq_fifo_count_);
    }

  private:
    std::uint16_t control_ = kPowerOnControl;
    std::uint16_t interrupt_control_ = 0;
    std::uint16_t bank_ = 0;
    std::uint16_t dreq_control_ = 0;
    std::array<std::uint16_t, 8> comm_{};
    // One per processor: master first, slave second.
    std::array<std::uint8_t, 2> sh2_interrupt_mask_{};
    // Interrupts standing against each processor. The command interrupt is
    // not here: it follows the interrupt control register directly, so that
    // the 68000 clearing the register drops it.
    std::array<std::uint8_t, 2> sh2_pending_{};
    // Shared by both processors, and nothing to do with bank_.
    std::uint8_t sh2_hcount_ = 0;
    int hint_countdown_ = 0;
    std::uint32_t dreq_source_ = 0;
    std::uint32_t dreq_target_ = 0;
    std::uint16_t dreq_length_ = 0;
    // The FIFO itself, oldest word first, eight deep. Mutable because taking a
    // word out is what READING $4012 does - the register read is the pop.
    mutable std::array<std::uint16_t, 8> dreq_fifo_{};
    mutable std::size_t dreq_fifo_count_ = 0;
    std::uint64_t dreq_words_in_ = 0;
    std::uint64_t dreq_words_dropped_ = 0;
    bool tv_register_ = false;
    bool cartridge_present_ = true;
    Pwm pwm_{};
    bool hint_during_vblank_ = false;
};

}  // namespace mercury::thirtytwox
