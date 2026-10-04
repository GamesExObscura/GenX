#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mercury/thirtytwox/adapter.hpp"
#include "mercury/thirtytwox/sh2_peripherals.hpp"

namespace mercury::thirtytwox {

class Vdp;

// What the two SH-2s see.
//
// Both processors sit on one bus and have identical maps, which is why this is
// one object rather than two. The map has a quirk worth stating plainly: every
// region appears TWICE, once cached from 0x00000000 and once cache-through
// from 0x20000000. Software picks the view it wants by which address it uses -
// the frame buffer is written cache-through so the writes actually reach it,
// and code runs cached so it runs fast. Clearing bit 29 normalises the two
// views onto one map, which is all the cache modelling this needs: there is no
// cache here, so the only thing that distinguishes the views is speed.
//
//   00000000  boot ROM, 16KB
//   00004000  system registers, the 32X VDP's registers and the palette
//   02000000  cartridge ROM
//   04000000  frame buffer, and its overwrite image at +0x20000
//   06000000  SDRAM, 256KB - where a game's SH-2 code is loaded and run
class Bus {
  public:
    static constexpr std::size_t kBootRomSize = 0x4000;
    static constexpr std::size_t kSdramSize = 0x40000;      // 2 Mbit
    static constexpr std::size_t kFrameBufferSize = 0x20000;  // 1 Mbit per bank
    static constexpr std::size_t kPaletteEntries = 256;

    // Offsets inside the register page at normalised 0x00004000.
    static constexpr std::uint32_t kRegisterBase = 0x00004000;
    static constexpr std::uint32_t kVdpRegisterBase = 0x00004100;
    static constexpr std::uint32_t kPaletteBase = 0x00004200;

    void reset() noexcept;
    // The cartridge is shared with the Mega Drive side - one ROM, two buses -
    // so this takes a reference and does not copy several megabytes.
    void attach_rom(const std::vector<std::uint8_t>* rom) noexcept { rom_ = rom; }
    void attach_adapter(Adapter* adapter) noexcept { adapter_ = adapter; }
    // The video registers live in the same page as the system ones, so the
    // bus has to be able to reach them. Forward declared rather than
    // included, because the VDP reads the frame buffer back out of here.
    void attach_vdp(Vdp* vdp) noexcept { vdp_ = vdp; }

    // Which processor is on the bus right now: 0 the master, 1 the slave.
    // Almost nothing here cares, because the two share everything - but the
    // adapter's interrupt mask is per-processor, so the register page has to
    // know who is asking. Set by the scheduler before each processor steps.
    void set_active_sh2(int which) noexcept { active_sh2_ = which != 0 ? 1 : 0; }
    [[nodiscard]] int active_sh2() const noexcept { return active_sh2_; }

    // Each processor's own on-chip peripherals. Not shared, so which one
    // answers depends on who is on the bus.
    void attach_peripherals(Sh2Peripherals* master, Sh2Peripherals* slave) noexcept {
        peripherals_[0] = master;
        peripherals_[1] = slave;
    }

    // Each processor has its OWN boot ROM, and the two are different programs:
    // the master's brings the cartridge up, the slave's waits for it. Sharing
    // one image would have the slave run the master's startup.
    void load_boot_rom(int which, const std::vector<std::uint8_t>& image) noexcept {
        auto& rom = boot_rom_[which != 0 ? 1 : 0];
        rom.fill(0);
        const std::size_t count = image.size() < rom.size() ? image.size() : rom.size();
        for (std::size_t at = 0; at < count; ++at) {
            rom[at] = image[at];
        }
    }

    // What is actually in a boot ROM, without going through the map - which
    // processor a plain read8 reaches depends on who is on the bus, and a test
    // asking "did the right firmware image load?" is not asking about that.
    [[nodiscard]] std::uint8_t boot_rom_byte(int which, std::size_t at) const noexcept {
        const auto& rom = boot_rom_[which != 0 ? 1 : 0];
        return at < rom.size() ? rom[at] : 0;
    }

    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint32_t read32(std::uint32_t address) const noexcept;
    void write8(std::uint32_t address, std::uint8_t value) noexcept;
    void write16(std::uint32_t address, std::uint16_t value) noexcept;
    void write32(std::uint32_t address, std::uint32_t value) noexcept;

    // Where the processor on the bus can fetch instructions without coming
    // through read16: SDRAM, the cartridge, or its own cache RAM. See
    // core::Sh2Bus::fetch_window for the contract - every word inside the
    // window reads exactly as read16 would give it.
    [[nodiscard]] const std::uint8_t* fetch_window(std::uint32_t address, std::uint32_t& base,
                                                   std::uint32_t& size) const noexcept;

    // Which of the two frame buffer banks the video side is currently showing.
    // The other one is the one the SH-2s draw into, and the pair swap.
    [[nodiscard]] int displayed_bank() const noexcept { return displayed_bank_; }
    void set_displayed_bank(int bank) noexcept { displayed_bank_ = bank & 1; }
    [[nodiscard]] std::uint64_t zero_byte_frame_writes() const noexcept {
        return zero_byte_frame_writes_;
    }
    [[nodiscard]] const std::array<std::uint8_t, kFrameBufferSize>& frame_buffer(
        int bank) const noexcept {
        return frame_buffer_[static_cast<std::size_t>(bank & 1)];
    }
    // The VDP's auto-fill: `words` copies of `value` from `word_address`
    // into the bank being drawn. The address advances within its own low
    // byte and never carries, so the fill stays in one 256-word row.
    void fill_draw_bank(std::uint16_t word_address, std::uint16_t value, int words) noexcept;

    [[nodiscard]] const std::array<std::uint16_t, kPaletteEntries>& palette() const noexcept {
        return palette_;
    }
    // The Mega Drive reaches the same palette through its own window at
    // $A15200, so it needs a way in that is not an SH-2 bus cycle.
    void set_palette_entry(std::size_t index, std::uint16_t value) noexcept {
        if (index < palette_.size()) {
            palette_[index] = value;
        }
    }
    [[nodiscard]] const std::array<std::uint8_t, kSdramSize>& sdram() const noexcept {
        return sdram_;
    }

    // Accesses to the SH-2's ON-CHIP peripheral block at $FFFFFE00 upward -
    // its timer, serial port, DMA controller and interrupt controller. GenX
    // does not model any of it, so every read returns the same value and a
    // game polling one of these registers waits for ever.
    //
    // Tallied rather than assumed: "26 of 30 cartridges are stuck" is worth
    // an explanation, and this says which register each one is sitting on
    // instead of leaving it to be guessed.
    static constexpr std::uint32_t kOnChipBase = 0xFFFFFE00;
    [[nodiscard]] const std::array<std::uint32_t, 512>& on_chip_reads() const noexcept {
        return on_chip_reads_;
    }
    [[nodiscard]] std::uint64_t on_chip_read_count() const noexcept { return on_chip_total_; }

    // The same idea for the adapter's own register page at $4000, per word.
    // A 32X game that runs but never draws is nearly always spinning on one
    // of these, and this says which.
    [[nodiscard]] const std::array<std::uint32_t, 32>& register_reads() const noexcept {
        return register_reads_;
    }

    [[nodiscard]] std::string describe() const;

    // The memories. The boot ROMs are firmware, not state - they come from
    // the files a person supplies, and the save state records which ones in
    // its header instead of carrying 32KB of them.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(sdram_);
        ar(frame_buffer_[0]);
        ar(frame_buffer_[1]);
        ar(palette_);
        ar(displayed_bank_);
        ar(active_sh2_);
        if constexpr (Archive::loading()) {
            // A loaded state can come from someone else. Both pick one of two
            // (frame buffers, processors) and index arrays directly; the SH-2s
            // draw into frame_buffer_[displayed_bank_ ^ 1], so a crafted bank
            // number was a write outside the frame buffers.
            displayed_bank_ &= 1;
            active_sh2_ &= 1;
        }
    }
    [[nodiscard]] const std::array<std::uint8_t, kBootRomSize>& boot_rom(int which) const noexcept {
        return boot_rom_[which != 0 ? 1 : 0];
    }

    // Normalise the cached and cache-through views of the same memory onto one
    // address. Public because the tests assert on it directly - the two views
    // being the same storage is a property worth pinning down.
    [[nodiscard]] static constexpr std::uint32_t normalise(std::uint32_t address) noexcept {
        return address & 0x1FFFFFFFu;
    }

  private:
    const std::vector<std::uint8_t>* rom_ = nullptr;
    Adapter* adapter_ = nullptr;
    Vdp* vdp_ = nullptr;

    std::array<std::uint8_t, kSdramSize> sdram_{};
    // Two banks. One is shown while the other is drawn into, which is how a
    // 32X game avoids tearing.
    std::array<std::array<std::uint8_t, kFrameBufferSize>, 2> frame_buffer_{};
    std::array<std::uint16_t, kPaletteEntries> palette_{};
    std::array<std::array<std::uint8_t, kBootRomSize>, 2> boot_rom_{};
    int displayed_bank_ = 0;
    int active_sh2_ = 0;
    // Byte writes of zero to the plain (not overwrite) frame buffer region.
    // Counted to settle whether a game relies on the hardware swallowing
    // them - see write8.
    std::uint64_t zero_byte_frame_writes_ = 0;
    // Diagnostic only, indexed by the low nine bits of the address. Mutable
    // because the read path is const and this counts reads.
    mutable std::array<std::uint32_t, 512> on_chip_reads_{};
    mutable std::uint64_t on_chip_total_ = 0;
    mutable std::array<std::uint32_t, 32> register_reads_{};
    std::array<Sh2Peripherals*, 2> peripherals_{nullptr, nullptr};

    // The bank the SH-2s draw into: the one that is not being shown.
    [[nodiscard]] std::size_t draw_bank() const noexcept {
        return static_cast<std::size_t>(displayed_bank_ ^ 1);
    }
};

}  // namespace mercury::thirtytwox
