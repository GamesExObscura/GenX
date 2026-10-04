#include "mercury/thirtytwox/bus.hpp"

#include "mercury/thirtytwox/vdp.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mercury::thirtytwox {

namespace {

// Which region a normalised address belongs to. The 32X map is sparse and
// regular: bits 26-24 pick the region and everything below is the offset.
enum class Region { boot_rom, registers, cartridge, frame_buffer, sdram, nothing };

Region region_of(std::uint32_t normalised) noexcept {
    switch (normalised >> 24) {
        case 0x00:
            // The boot ROM and the register page share the bottom region; the
            // registers sit immediately above the 16KB of ROM.
            return normalised < Bus::kBootRomSize ? Region::boot_rom : Region::registers;
        case 0x02:
            return Region::cartridge;
        case 0x04:
            return Region::frame_buffer;
        case 0x06:
            return Region::sdram;
        default:
            return Region::nothing;
    }
}

}  // namespace

void Bus::reset() noexcept {
    sdram_.fill(0);
    for (auto& bank : frame_buffer_) {
        bank.fill(0);
    }
    palette_.fill(0);
    displayed_bank_ = 0;
    on_chip_reads_.fill(0);
    on_chip_total_ = 0;
    register_reads_.fill(0);
}

std::uint8_t Bus::read8(std::uint32_t address) const noexcept {
    // Tested on the RAW address, because normalise folds the on-chip block
    // onto ordinary memory and the distinction would be gone by then.
    if (address >= kOnChipBase) {
        ++on_chip_reads_[address & 0x1FF];
        ++on_chip_total_;
        if (auto* peri = peripherals_[active_sh2_]; peri != nullptr) {
            return peri->read8(address);
        }
    }
    // The cache, addressed as scratch RAM. Also before normalise, which would
    // fold it onto the boot ROM.
    if (Sh2Peripherals::decodes_data_array(address)) {
        if (const auto* peri = peripherals_[active_sh2_]; peri != nullptr) {
            return peri->read_data_array(address);
        }
    }
    const std::uint32_t at = normalise(address);
    switch (region_of(at)) {
        case Region::boot_rom:
            return boot_rom_[active_sh2_][at];
        case Region::registers: {
            if (at >= kPaletteBase && at < kPaletteBase + kPaletteEntries * 2) {
                const auto entry = (at - kPaletteBase) / 2;
                const std::uint16_t colour = palette_[entry];
                return (at & 1u) != 0 ? static_cast<std::uint8_t>(colour)
                                      : static_cast<std::uint8_t>(colour >> 8);
            }
            if (adapter_ != nullptr && at >= kRegisterBase && at < kVdpRegisterBase) {
                ++register_reads_[((at - kRegisterBase) / 2) & 31];
                // The SH-2's own view. The communication ports are the same
                // storage the 68000 sees at $A15120, but the control register
                // is NOT the same register as $A15100 and must not be read
                // through the 68000's bit assignments.
                return adapter_->sh2_read8(at - kRegisterBase, active_sh2_);
            }
            if (vdp_ != nullptr && at >= kVdpRegisterBase && at < kPaletteBase) {
                const std::uint16_t word = vdp_->read_register(at - kVdpRegisterBase);
                return (at & 1u) != 0 ? static_cast<std::uint8_t>(word)
                                      : static_cast<std::uint8_t>(word >> 8);
            }
            return 0;
        }
        case Region::cartridge: {
            if (rom_ == nullptr) {
                return 0xFF;
            }
            const std::size_t offset = at & 0x003FFFFFu;
            return offset < rom_->size() ? (*rom_)[offset] : 0xFF;
        }
        case Region::frame_buffer: {
            const std::size_t offset = at & (kFrameBufferSize - 1);
            return frame_buffer_[draw_bank()][offset];
        }
        case Region::sdram:
            return sdram_[at & (kSdramSize - 1)];
        default:
            return 0xFF;
    }
}

std::uint16_t Bus::read16(std::uint32_t address) const noexcept {
    // Before normalising, always. normalise() folds $FFFFFExx onto ordinary
    // memory, so a word access that goes through it can never reach the
    // on-chip block - which is how the timer looked implemented and behaved
    // as though it were not.
    if (address >= kOnChipBase || Sh2Peripherals::decodes_data_array(address)) {
        const std::uint32_t raw = address & ~1u;
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(read8(raw)) << 8) |
                                          read8(raw + 1));
    }
    const std::uint32_t at = normalise(address) & ~1u;
    // THE PLAIN MEMORIES, READ AS A WORD. Every instruction an SH-2 runs is a
    // word read from one of these, and building it from two byte reads paid
    // the whole decode twice. Same bytes, same order, same out-of-range
    // answer: anything these cannot answer exactly falls through to the byte
    // path below.
    switch (at >> 24) {
        case 0x06: {
            const std::size_t offset = at & (kSdramSize - 1);
            return static_cast<std::uint16_t>((sdram_[offset] << 8) | sdram_[offset + 1]);
        }
        case 0x02: {
            const std::size_t offset = at & 0x003FFFFFu;
            if (rom_ != nullptr && offset + 1 < rom_->size()) {
                const auto& rom = *rom_;
                return static_cast<std::uint16_t>((rom[offset] << 8) | rom[offset + 1]);
            }
            break;
        }
        case 0x04: {
            const auto& bank = frame_buffer_[draw_bank()];
            const std::size_t offset = at & (kFrameBufferSize - 1);
            return static_cast<std::uint16_t>((bank[offset] << 8) | bank[offset + 1]);
        }
        default:
            break;
    }
    if (region_of(at) == Region::registers && at >= kPaletteBase &&
        at < kPaletteBase + kPaletteEntries * 2) {
        return palette_[(at - kPaletteBase) / 2];
    }
    // The adapter registers take a word read whole, for the same reason the
    // write path does. Most of them are plain storage and could not tell the
    // difference, but the 68000 -> SH-2 FIFO at $4012 can: reading it TAKES A
    // WORD OUT, so assembling a word from two byte reads took two words and
    // returned half of each. A four-word command arrived as one scrambled
    // word and zeros, and Knuckles' Chaotix's dispatcher read nothing but
    // "end of list" for 600 frames.
    if (adapter_ != nullptr && region_of(at) == Region::registers && at >= kRegisterBase &&
        at < kVdpRegisterBase) {
        register_reads_[((at - kRegisterBase) / 2) & 31] += 2;  // the tally counts bytes
        return adapter_->sh2_read16(at - kRegisterBase, active_sh2_);
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(read8(at)) << 8) |
                                      read8(at + 1));
}

std::uint32_t Bus::read32(std::uint32_t address) const noexcept {
    if (address >= kOnChipBase || Sh2Peripherals::decodes_data_array(address)) {
        const std::uint32_t raw = address & ~3u;
        return (static_cast<std::uint32_t>(read16(raw)) << 16) | read16(raw + 2);
    }
    const std::uint32_t at = normalise(address) & ~3u;
    // SDRAM, whole, for the same reason read16 takes the plain memories whole.
    if ((at >> 24) == 0x06) {
        const std::size_t offset = at & (kSdramSize - 1);
        return (static_cast<std::uint32_t>(sdram_[offset]) << 24) |
               (static_cast<std::uint32_t>(sdram_[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(sdram_[offset + 2]) << 8) |
               static_cast<std::uint32_t>(sdram_[offset + 3]);
    }
    return (static_cast<std::uint32_t>(read16(at)) << 16) | read16(at + 2);
}

const std::uint8_t* Bus::fetch_window(std::uint32_t address, std::uint32_t& base,
                                      std::uint32_t& size) const noexcept {
    if (address >= kOnChipBase) {
        return nullptr;  // registers, never code
    }
    if (Sh2Peripherals::decodes_data_array(address)) {
        // Each processor's own cache RAM, which is why the window lives in
        // the processor and not here: the other one gets its own.
        const auto* peri = peripherals_[active_sh2_];
        if (peri == nullptr) {
            return nullptr;
        }
        base = address & ~static_cast<std::uint32_t>(Sh2Peripherals::kDataArraySize - 1);
        size = static_cast<std::uint32_t>(Sh2Peripherals::kDataArraySize);
        return peri->data_array();
    }
    // One mirror at a time, keeping the address's own top bits: every address
    // in [base, base + size) then normalises into the same region at the
    // same offset, which is what read16 would decode.
    switch (normalise(address) >> 24) {
        case 0x06:
            base = address & ~static_cast<std::uint32_t>(kSdramSize - 1);
            size = static_cast<std::uint32_t>(kSdramSize);
            return sdram_.data();
        case 0x02: {
            if (rom_ == nullptr || rom_->size() < 2) {
                return nullptr;
            }
            // No further than the cartridge goes: past its end read16 gives
            // open bus, so those words stay on the slow path.
            const std::size_t span = rom_->size() < 0x400000 ? rom_->size() : 0x400000;
            base = address & ~0x003FFFFFu;
            size = static_cast<std::uint32_t>(span) & ~1u;
            return rom_->data();
        }
        default:
            return nullptr;  // boot ROM, registers, frame buffer: through the bus
    }
}

void Bus::write8(std::uint32_t address, std::uint8_t value) noexcept {
    // THE CACHE'S CONTROL SPACE IS NOT MEMORY.
    //
    // $40000000-$5FFFFFFF is the purge area - a write there throws away cache
    // entries for that address - and $60000000-$7FFFFFFF is the cache's
    // associative address array. Neither is a view of the bus. normalise()
    // only clears the top three bits, so without this a purge of $46004F10
    // wrote a ZERO over SDRAM at $06004F10.
    //
    // That is exactly what a game does before reading a buffer it is about to
    // run: Knuckles' Chaotix purges 64 longwords over its command buffer and
    // then dispatches on it. GenX erased every command the 68000 had sent a
    // moment earlier, so the dispatcher only ever saw "end of list". ares
    // routes only the cached and uncached areas to the bus, and PicoDrive
    // maps an explicit ignore over the purge area.
    if ((address >> 29) == 2 || (address >> 29) == 3) {
        return;
    }
    // The writing processor's own on-chip block, before the address is
    // normalised onto shared memory.
    if (address >= kOnChipBase) {
        if (auto* peri = peripherals_[active_sh2_]; peri != nullptr) {
            peri->write8(address, value);
            return;
        }
    }
    if (Sh2Peripherals::decodes_data_array(address)) {
        if (auto* peri = peripherals_[active_sh2_]; peri != nullptr) {
            peri->write_data_array(address, value);
            return;
        }
    }
    const std::uint32_t at = normalise(address);
    switch (region_of(at)) {
        case Region::registers: {
            if (at >= kPaletteBase && at < kPaletteBase + kPaletteEntries * 2) {
                const auto entry = (at - kPaletteBase) / 2;
                const std::uint16_t colour = palette_[entry];
                palette_[entry] =
                    (at & 1u) != 0
                        ? static_cast<std::uint16_t>((colour & 0xFF00u) | value)
                        : static_cast<std::uint16_t>((colour & 0x00FFu) |
                                                     (static_cast<std::uint16_t>(value) << 8));
                return;
            }
            if (adapter_ != nullptr && at >= kRegisterBase && at < kVdpRegisterBase) {
                adapter_->sh2_write8(at - kRegisterBase, value, active_sh2_);
                return;
            }
            if (vdp_ != nullptr && at >= kVdpRegisterBase && at < kPaletteBase) {
                const std::uint32_t offset = (at - kVdpRegisterBase) & ~1u;
                const std::uint16_t word = vdp_->read_register(offset);
                const std::uint16_t merged =
                    (at & 1u) != 0
                        ? static_cast<std::uint16_t>((word & 0xFF00u) | value)
                        : static_cast<std::uint16_t>((word & 0x00FFu) |
                                                     (static_cast<std::uint16_t>(value) << 8));
                vdp_->write_register(offset, merged);
            }
            return;
        }
        case Region::frame_buffer: {
            const std::size_t offset = at & (kFrameBufferSize - 1);
            // The overwrite image at +0x20000 is the same memory with one
            // rule attached: a zero byte leaves what is already there. It is
            // how a 32X game draws a sprite with transparent pixels without
            // reading the frame buffer back first.
            //
            // THE PLAIN REGION SWALLOWS A BYTE WRITE OF ZERO TOO. ares:
            // "8-bit 0x00 writes do not go through", on both the SH-2 and the
            // 68000 side. Only BYTE writes - a word write of zero clears as
            // normal, which is how a frame buffer gets cleared, and is handled
            // on the word path below.
            //
            // This was held back once because it cost Virtua Fighter its whole
            // picture. That was some other fault since fixed: re-measured
            // 2026-09-18, Virtua Fighter's frame 900 is pixel-identical with it
            // on and off. It became NECESSARY when index 0 stopped being
            // treated as transparent (see Vdp::render_line): Spider-Man blits
            // its sprites with byte writes, zeros for the see-through pixels,
            // and letting those zeros land punched boxes of palette entry 0 -
            // pure green in that game - around every sprite. On the hardware
            // they never land, so the background survives under the sprite.
            //
            // MERCURY_32X_KEEP_ZERO_BYTES=1 lets them through again, for
            // bisecting a picture fault against this rule.
            const bool overwrite = (at & kFrameBufferSize) != 0;
            if (overwrite && value == 0) {
                return;
            }
            if (value == 0) {
                ++zero_byte_frame_writes_;
                static const bool keep = [] {
                    const char* e = std::getenv("MERCURY_32X_KEEP_ZERO_BYTES");
                    return e != nullptr && *e != '0';
                }();
                if (!keep) {
                    return;
                }
            }
            frame_buffer_[draw_bank()][offset] = value;
            return;
        }
        case Region::sdram:
            sdram_[at & (kSdramSize - 1)] = value;
            return;
        case Region::boot_rom:
        case Region::cartridge:
        default:
            return;  // read-only, or nothing there
    }
}

void Bus::write16(std::uint32_t address, std::uint16_t value) noexcept {
    if ((address >> 29) == 2 || (address >> 29) == 3) {
        return;  // the cache's control space - see write8
    }
    // Same reason as the read path: normalise would send this to memory.
    if (address >= kOnChipBase || Sh2Peripherals::decodes_data_array(address)) {
        const std::uint32_t raw = address & ~1u;
        write8(raw, static_cast<std::uint8_t>(value >> 8));
        write8(raw + 1, static_cast<std::uint8_t>(value));
        return;
    }
    const std::uint32_t at = normalise(address) & ~1u;
    // SDRAM takes the word directly - see read16.
    if ((at >> 24) == 0x06) {
        const std::size_t offset = at & (kSdramSize - 1);
        sdram_[offset] = static_cast<std::uint8_t>(value >> 8);
        sdram_[offset + 1] = static_cast<std::uint8_t>(value);
        return;
    }
    if (region_of(at) == Region::registers && at >= kPaletteBase &&
        at < kPaletteBase + kPaletteEntries * 2) {
        palette_[(at - kPaletteBase) / 2] = value;
        return;
    }
    if (region_of(at) == Region::frame_buffer) {
        // A word write to the overwrite image tests each byte separately, so
        // one transparent half of a pair still lets the other through.
        const bool overwrite = (at & kFrameBufferSize) != 0;
        const std::size_t offset = at & (kFrameBufferSize - 1);
        // GENX_32X_TRACE: every word written to four LINE TABLE entries (lines
        // 0, 1, 100 and 200). Pixel values arriving there are what turn a
        // picture into identical vertical stripes, so the sequence of values
        // each entry takes, and who wrote them, is the thing to see.
        static const bool trace_lines = std::getenv("GENX_32X_TRACE") != nullptr;
        static std::uint32_t traced_lines = 0;
        if (trace_lines && (offset == 0 || offset == 2 || offset == 200 || offset == 400) &&
            traced_lines < 2000) {
            ++traced_lines;
            std::fprintf(stderr, "[32x fb] word %04X -> bank %zu line %zu%s (sh2 %d, raw %08X)\n",
                         value, draw_bank(), offset / 2, overwrite ? " overwrite-image" : "",
                         active_sh2_, address);
        }
        const auto high = static_cast<std::uint8_t>(value >> 8);
        const auto low = static_cast<std::uint8_t>(value);
        if (!overwrite || high != 0) {
            frame_buffer_[draw_bank()][offset] = high;
        }
        if (!overwrite || low != 0) {
            frame_buffer_[draw_bank()][(offset + 1) & (kFrameBufferSize - 1)] = low;
        }
        return;
    }
    // The adapter registers have to take the whole word at once. Splitting a
    // word write into two byte writes is wrong here: a word write to the
    // control register carries FM and nothing else, while a byte write to its
    // odd half sets that processor's interrupt mask. Two different registers
    // reached through the same address, and the access width picks which.
    if (adapter_ != nullptr && region_of(at) == Region::registers && at >= kRegisterBase &&
        at < kVdpRegisterBase) {
        adapter_->sh2_write16(at - kRegisterBase, value, active_sh2_);
        return;
    }
    // THE VIDEO REGISTERS TAKE A WORD WHOLE TOO.
    //
    // Split into two byte writes, each half went through write_register on
    // its own, merged with whatever the register read back. For most of them
    // the second write simply wins. The auto-fill data register is different:
    // writing it RUNS the fill. A word write ran the fill twice - once with
    // the new high byte over a zero, then, from the address the first fill
    // had advanced to, again with the low byte alone. Virtua Racing Deluxe's
    // line table came out as $3200 in one entry and $0032 in the next, one
    // colour delivered as two halves, and Knuckles' Chaotix's read $0024 on
    // every line, so every line showed the same row: vertical stripes. ares
    // treats the write as one access and runs one fill.
    if (vdp_ != nullptr && region_of(at) == Region::registers && at >= kVdpRegisterBase &&
        at < kPaletteBase) {
        vdp_->write_register(at - kVdpRegisterBase, value);
        return;
    }
    write8(at, static_cast<std::uint8_t>(value >> 8));
    write8(at + 1, static_cast<std::uint8_t>(value));
}

void Bus::write32(std::uint32_t address, std::uint32_t value) noexcept {
    // Checked here as well as in write8: this path normalises BEFORE handing
    // on, so by the time a purge reached write8 it would already look like
    // SDRAM. Chaotix's purge is a longword write.
    if ((address >> 29) == 2 || (address >> 29) == 3) {
        return;  // the cache's control space - see write8
    }
    if (address >= kOnChipBase || Sh2Peripherals::decodes_data_array(address)) {
        const std::uint32_t raw = address & ~3u;
        write16(raw, static_cast<std::uint16_t>(value >> 16));
        write16(raw + 2, static_cast<std::uint16_t>(value));
        return;
    }
    const std::uint32_t at = normalise(address) & ~3u;
    if ((at >> 24) == 0x06) {
        const std::size_t offset = at & (kSdramSize - 1);
        sdram_[offset] = static_cast<std::uint8_t>(value >> 24);
        sdram_[offset + 1] = static_cast<std::uint8_t>(value >> 16);
        sdram_[offset + 2] = static_cast<std::uint8_t>(value >> 8);
        sdram_[offset + 3] = static_cast<std::uint8_t>(value);
        return;
    }
    write16(at, static_cast<std::uint16_t>(value >> 16));
    write16(at + 2, static_cast<std::uint16_t>(value));
}

void Bus::fill_draw_bank(std::uint16_t word_address, std::uint16_t value, int words) noexcept {
    auto& bank = frame_buffer_[draw_bank()];
    std::uint16_t cursor = word_address;
    for (int written = 0; written < words; ++written) {
        const std::size_t at = (static_cast<std::size_t>(cursor) * 2) & (kFrameBufferSize - 1);
        bank[at] = static_cast<std::uint8_t>(value >> 8);
        bank[at + 1] = static_cast<std::uint8_t>(value);
        // The carry stops at the byte boundary. This is the quirk, not a
        // bounds check: a 256-word fill from the middle of a row wraps back
        // to the start of that same row.
        cursor = static_cast<std::uint16_t>((cursor & 0xFF00u) | ((cursor + 1) & 0x00FFu));
    }
}

std::string Bus::describe() const {
    std::ostringstream out;
    out << "32X bus: " << (rom_ != nullptr ? std::to_string(rom_->size()) + " byte cartridge"
                                           : std::string("no cartridge"))
        << ", showing frame buffer bank " << displayed_bank_;
    return out.str();
}

}  // namespace mercury::thirtytwox
