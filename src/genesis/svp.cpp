#include "mercury/genesis/svp.hpp"

#include <algorithm>
#include <cstdlib>

namespace mercury::genesis {
namespace {

// The DSP's external address space is 21 bits of WORD addresses. The top five
// bits of a PMC mode word select the region:
//   0x00-0x0F  cartridge ROM   (68k 0x000000-0x1FFFFF)
//   0x18       DRAM            (68k 0x300000-0x31FFFF)
//   0x1C       IRAM            (word 0x1C8000, i.e. with bit 15 of the address)
// Diagnostic switch, see pm_access. Folding the tile writes down by 0x6000 was
// tried to test whether they simply land in the wrong half of DRAM; it black-
// screens the game, because regions 6-F hold working data the DSP reads back.
constexpr bool kSvpFoldTileWrites = false;

// Cycles an instruction fetched from cartridge ROM costs, against one for an
// instruction in IRAM. 216ns / 43.5ns per cycle at 23.01MHz.
constexpr int kRomInstructionCycles = 5;

constexpr std::uint16_t kRegionDram = 0x18;
constexpr std::uint16_t kRegionIram = 0x1C;

// Auto-increment encoded in mode bits 11-13: 0,1,2,4,8,16,32,128 words, negated
// when the decrement bit is set.
[[nodiscard]] int increment_for(std::uint16_t mode) noexcept {
    int inc = (mode >> 11) & 7;
    if (inc == 0) {
        return 0;
    }
    if (inc != 7) {
        --inc;
    }
    inc = 1 << inc;
    return (mode & 0x8000) != 0 ? -inc : inc;
}

// "Overwrite" mode writes per nibble, and only where the source nibble is
// non-zero. The renderer uses it to draw a polygon over a tile without erasing
// the pixels it does not cover.
void overwrite_write(std::uint16_t& destination, std::uint16_t value) noexcept {
    for (int shift = 0; shift < 16; shift += 4) {
        const std::uint16_t mask = static_cast<std::uint16_t>(0xF << shift);
        if ((value & mask) != 0) {
            destination = static_cast<std::uint16_t>((destination & ~mask) | (value & mask));
        }
    }
}

}  // namespace

std::size_t Svp::dram_nonzero() const noexcept {
    std::size_t count = 0;
    for (const auto word : dram_) {
        if (word != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t Svp::dram_nonzero_in(std::uint32_t first, std::uint32_t count) const noexcept {
    std::size_t found = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (dram_[(first + i) & 0xFFFF] != 0) {
            ++found;
        }
    }
    return found;
}

// The two "cell arrange" mirrors, which reorder DRAM into the layout the VDP
// wants tiles in. 0x390000 and 0x3A0000 scramble different address bits.
//
// This lives in one place because the WINDOW IS WRITEABLE, not just readable -
// Virtua Racing's own self-test writes through it, and dropping those writes is
// what made its "DRAM R/W" test report ERROR.
std::uint32_t Svp::cell_arrange_index(std::uint32_t address) noexcept {
    const std::uint32_t a1 = (address & 0xFFFF) >> 1;
    // Pick the window by its base, NOT by a bit of the address: 0x39 has bit 16
    // set and 0x3A does not, so testing (address & 0x10000) selects the wrong
    // formula for BOTH windows. The game's own self-test catches this - at
    // window index 0x7FDF it wants DRAM index 0x7FFD and gets 0x77FF, which is
    // exactly formula 1 applied to a formula 2 address.
    const std::uint32_t index = address >= 0x3A0000
                                    ? ((a1 & 0xF801) | ((a1 & 0x001E) << 6) | ((a1 & 0x07E0) >> 4))
                                    : ((a1 & 0xF001) | ((a1 & 0x003E) << 6) | ((a1 & 0x0FC0) >> 5));
    return index & 0xFFFF;
}

// Cheap checksum over one 12K-word tile buffer, used to answer a single
// question: does the buffer change between the DSP announcing the frame is
// finished and the 68000 starting to transfer it? If it does, something is
// scribbling on a finished image and that is the corruption.
std::uint32_t Svp::buffer_crc(std::uint32_t first) const noexcept {
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i < 0x3000; ++i) {
        h = (h ^ dram_[(first + i) & 0xFFFF]) * 16777619u;
    }
    return h;
}

void Svp::note_unknown(std::uint16_t opcode) noexcept {
    ++unknown_opcodes_;
    // Where the DSP was when it fetched something undecodable. Executing blank
    // memory means control flow already went wrong somewhere earlier, and the
    // address it landed on is the only clue to where.
    if (first_unknown_pc_ == 0) {
        first_unknown_pc_ = pc();
    }
    last_unknown_pc_ = pc();
    for (auto& entry : unknown_samples_) {
        if (entry.second == 0 || entry.first == opcode) {
            entry.first = opcode;
            ++entry.second;
            return;
        }
    }
}

void Svp::attach_rom(const std::uint8_t* rom, std::size_t size) noexcept {
    rom_ = rom;
    rom_size_ = size;
}

void Svp::reset() noexcept {
    gr_.fill(0);
    r_.fill(0);
    ram0_.fill(0);
    ram1_.fill(0);
    iram_.fill(0);
    dram_.fill(0);
    stack_.fill(0);
    pm_read_.fill(0);
    pm_write_.fill(0);
    stack_depth_ = 0;
    pmc_has_address_ = false;
    pmc_pending_ = false;
    halted_ = false;
    alu32_ops_ = 0;
    cell_reads_ = 0;
    irom_reads_ = 0;
    irom_fetches_ = 0;
    carry_overflow_conds_ = 0;
    pmc_non_blind_ = 0;
    rpl_pointer_steps_ = 0;
    rpl_write_steps_ = 0;
    halt_writes_ = 0;
    writes_68k_.fill(0);
    cell_decrement_steps_ = 0;
    st_saturate_set_ = 0;
    st_macs_set_ = 0;
    mac_b_bit_set_ = 0;
    acc_op_unimplemented_ = 0;
    acc_op_kinds_.fill(0);
    ptr2_mod_used_ = 0;
    transfer_starts_ = 0;
    buffer_changed_a_ = 0;
    buffer_changed_b_ = 0;
    crc_a_done_ = 0;
    crc_b_done_ = 0;
    have_done_crc_ = false;
    erased_words_ = 0;
    erased_words_live_ = 0;
    clear_collisions_ = 0;
    clear_clean_ = 0;
    dsp_last_buffer_ = 2;
    writes_68k_halted_ = 0;
    writes_68k_running_ = 0;
    dram_reads_while_halted_ = 0;
    dram_reads_while_running_ = 0;
    unhandled_modes_ = 0;
    unhandled_mode_words_.fill({0, 0});
    halt_values_.fill({0, 0});
    pixel_histogram_.fill(0);
    std::fill(cell_word_counts_.begin(), cell_word_counts_.end(), 0u);
    current_opcode_ = 0;
    cell_nonzero_reads_ = 0;
    dram_writes_ = 0;
    dram_write_buckets_.fill(0);
    dram_nonzero_writes_.fill(0);
    dram_read_buckets_.fill(0);
    dram_write_modes_.fill({0, 0});
    pm_latch_buckets_.fill(0);
    cell_writes_ = 0;
    dram_nonzero_reads_ = 0;
    dsp_completion_writes_ = 0;
    instructions_at_command_ = 0;
    render_instructions_total_ = 0;
    render_samples_ = 0;
    wait_30fe06_ = false;
    wait_30fe08_ = false;
    wait_cycles_skipped_ = 0;
    instruction_pc_ = 0;
    std::fill(pc_histogram_.begin(), pc_histogram_.end(), 0u);
    cell_nonzero_writes_.fill(0);
    dram_overwrite_writes_ = 0;
    iram_writes_ = 0;
    instructions_ = 0;
    unknown_opcodes_ = 0;
    unknown_samples_.fill({0, 0});
    // Register 0 reads as all ones; it is the "-" blind operand.
    gr_[kRegBlind] = 0xFFFF0000u;
    // Execution starts at word 0x400, the first word of cartridge ROM in the
    // DSP's program space.
    set_pc(0x400);
}

std::uint16_t Svp::rom_word(std::uint32_t word_address) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(word_address) * 2;
    if (rom_ == nullptr || offset + 1 >= rom_size_) {
        return 0;
    }
    return static_cast<std::uint16_t>((rom_[offset] << 8) | rom_[offset + 1]);
}

std::uint16_t Svp::program_read(std::uint16_t word_address) const noexcept {
    if (word_address < iram_.size()) {
        return iram_[word_address];
    }
    // The chip has a 1K-word internal ROM at 0xFC00-0xFFFF holding boot code,
    // math helpers and a sine table. Nothing models it - here or in either
    // reference implementation - so anything reading there gets the cartridge,
    // which for this game is 0xFF padding. Counted so the question can be
    // answered rather than argued about.
    if (word_address >= 0xFC00) {
        ++irom_reads_;
    }
    return rom_word(word_address);
}

std::uint16_t Svp::fetch() noexcept {
    const std::uint16_t address = pc();
    if (address >= 0xFC00) {
        ++irom_fetches_;
    }
    set_pc(static_cast<std::uint16_t>(address + 1));
    return program_read(address);
}

std::uint16_t& Svp::ram_cell(bool bank1, std::uint8_t index) noexcept {
    return bank1 ? ram1_[index] : ram0_[index];
}

std::uint8_t Svp::pointer_address(bool bank1, std::uint8_t p, std::uint8_t m,
                                  bool modulo) noexcept {
    // r3 and r7 are fixed at the base of their bank; the modifier field is a
    // literal index into the first four words instead of a post-modifier.
    if (p == 3) {
        return m;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>((bank1 ? 4 : 0) + p);
    const std::uint8_t address = r_[slot];
    switch (m) {
        case 0:
            break;
        case 1:  // "+!" - plain post-increment, never wraps
            r_[slot] = static_cast<std::uint8_t>(address + 1);
            break;
        case 2:  // "-" and "+", both modulo RPL when it is non-zero
        case 3: {
            const int delta = (m == 3) ? 1 : -1;
            const std::uint16_t rpl = st() & kStRpl;
            if (rpl != 0) {
                ++rpl_pointer_steps_;
                if (!modulo) {
                    ++rpl_write_steps_;
                }
            }
            // Modulo addressing applies to pointer READS only. The reference
            // interpreter steps a write pointer plainly and flags "unhandled
            // RPL" if it ever sees one, so wrapping here walks a rasteriser
            // back over pixels it has already drawn.
            if (rpl == 0 || !modulo) {
                r_[slot] = static_cast<std::uint8_t>(address + delta);
            } else {
                // Wrap inside an aligned block of 2^RPL words, leaving the bits
                // above the block untouched.
                const std::uint8_t mask = static_cast<std::uint8_t>((1u << rpl) - 1u);
                r_[slot] = static_cast<std::uint8_t>((address & ~mask) |
                                                     ((address + delta) & mask));
            }
            break;
        }
        default:
            break;
    }
    return address;
}

// --- programmable memory ----------------------------------------------------

void Svp::latch_pmc(std::uint8_t reg, bool write) noexcept {
    if (reg < 6) {
        (write ? pm_write_ : pm_read_)[reg] = gr_[kRegPmc];
        // What the DSP actually asked for, before any auto-increment. If the
        // addresses it programs do not match the region the 68000 reads, the
        // fault is in this value, not in the transfer.
        if (write && ((gr_[kRegPmc] >> 16) & 0x1F) == kRegionDram) {
            const auto addr = static_cast<std::uint16_t>(gr_[kRegPmc] & 0xFFFF);
            ++pm_latch_buckets_[(addr >> 12) & 0x0F];
        }
    }
    pmc_pending_ = false;
    pmc_has_address_ = false;
}

bool Svp::pm_access(std::uint8_t reg, bool write, std::uint16_t& data) noexcept {
    // A completed PMC pair is claimed by the next access to a PM port, and that
    // access transfers nothing - it exists only to say which port was meant.
    //
    // That claiming access MUST be a blind one ("ld PMx, -" or "ld -, PMx").
    // A real transfer instruction arriving while PMC is armed does not program
    // the port; it is abandoned. Treating it as the latch instead both loses
    // the transfer and installs an address the DSP never asked for.
    if (pmc_pending_) {
        const bool blind = (current_opcode_ & 0xFF0F) == 0 || (current_opcode_ & 0xFFF0) == 0;
        if (!blind) {
            pmc_pending_ = false;
            pmc_has_address_ = false;
            ++pmc_non_blind_;
            data = 0;
            return true;
        }
        latch_pmc(reg, write);
        data = 0;
        return true;
    }
    pmc_has_address_ = false;
    // PM4 is always a memory port. The others only behave as one while ST5 or
    // ST6 is set; otherwise PM0 and XST are status registers.
    if (reg != 4 && (st() & kStSt56) == 0) {
        return false;
    }
    if (reg >= 6) {
        return false;
    }

    std::uint32_t& slot = write ? pm_write_[reg] : pm_read_[reg];
    const std::uint16_t mode = static_cast<std::uint16_t>(slot >> 16);
    // My region tests are looser than the reference's, which matches on exact
    // masks and does NOTHING - no write, no address step - for anything else.
    // A mode I accept and it rejects writes data the hardware never wrote and
    // then leaves the pointer somewhere it never went. Counted so "looser is
    // surely fine" stops being an assumption.
    {
        const bool reference_handles =
            write ? ((mode & 0x43FF) == 0x0018 || (mode & 0xFBFF) == 0x4018 ||
                     (mode & 0x47FF) == 0x001C)
                  : ((mode & 0xFFF0) == 0x0800 || (mode & 0x47FF) == 0x0018);
        if (!reference_handles) {
            ++unhandled_modes_;
            for (auto& entry : unhandled_mode_words_) {
                if (entry.second == 0 || entry.first == mode) {
                    entry.first = mode;
                    ++entry.second;
                    break;
                }
            }
        }
    }
    std::uint16_t address = static_cast<std::uint16_t>(slot & 0xFFFF);
    const std::uint16_t region = mode & 0x1F;
    // DIAGNOSTIC ONLY - not a fix. Confirms whether the rendered tiles are
    // simply landing 0x6000 words above where the 68000 reads them.
    if (kSvpFoldTileWrites && write && region == kRegionDram && address >= 0x6000) {
        address = static_cast<std::uint16_t>(address - 0x6000);
    }

    if (write) {
        if (region == kRegionDram && (mode & 0x4000) == 0) {
            ++dram_writes_;
            ++dram_write_buckets_[(address >> 12) & 0x0F];
            // Separating "wrote a zero" from "wrote data" says whether a region
            // full of writes but empty of content is being deliberately cleared
            // or is being rendered into with values that come out zero.
            // 0x30FE02 is the cell the 68000 polls to learn the frame is ready.
            // If the DSP never sets it, the game sits in its "frame lag" path
            // and never asks for the next frame.
            if (address == 0x7F01) {
                ++dsp_completion_writes_;
                crc_a_done_ = buffer_crc(0x0000);
                crc_b_done_ = buffer_crc(0x3000);
                have_done_crc_ = true;
                if (instructions_at_command_ != 0 && instructions_ > instructions_at_command_) {
                    render_instructions_total_ += instructions_ - instructions_at_command_;
                    ++render_samples_;
                }
            }
            if (data != 0) {
                ++dram_nonzero_writes_[(address >> 12) & 0x0F];
                if ((mode & 0x0400) != 0) {
                    ++dram_overwrite_writes_;
                }
                // The mode word that produced this address, so the actual
                // programming can be compared against what the region layout
                // says it should be.
                for (auto& entry : dram_write_modes_) {
                    if (entry.second == 0 || entry.first == mode) {
                        entry.first = mode;
                        ++entry.second;
                        break;
                    }
                }
            }
            if ((mode & 0x0400) != 0) {
                overwrite_write(dram_[address], data);
            } else {
                dram_[address] = data;
            }
            slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
        } else if (region == kRegionDram) {
            ++dram_writes_;
            ++dram_write_buckets_[(address >> 12) & 0x0F];
            // Cell-arrange writes counted separately: this is the mode a tile
            // rasteriser would use, and it was previously invisible here.
            ++cell_writes_;
            dsp_last_buffer_ = (address < 0x3000) ? 0 : ((address < 0x6000) ? 1 : 2);
            // The most common whole words, not just the most common nibbles: a
            // dominant single value is a buffer clear, a spread of values is a
            // renderer shading polygons.
            ++cell_word_counts_[data];

            // Which colour indices the rasteriser actually emits. The road, the
            // grass and the sky are separate palette entries; if the DSP only
            // ever writes one of them the fault is in shading, not in geometry.
            for (int shift = 0; shift < 16; shift += 4) {
                ++pixel_histogram_[(data >> shift) & 0x0F];
            }
            if (data != 0) {
                ++cell_nonzero_writes_[(address >> 12) & 0x0F];
                if ((mode & 0x0400) != 0) {
                    ++dram_overwrite_writes_;
                }
                // Cell mode is the rasteriser's own path, so its mode words are
                // the ones worth seeing. They were previously recorded only for
                // the linear branch, which is the bookkeeping path.
                for (auto& entry : dram_write_modes_) {
                    if (entry.second == 0 || entry.first == mode) {
                        entry.first = mode;
                        ++entry.second;
                        break;
                    }
                }
            }
            if (address == 0x7F01) {
                ++dsp_completion_writes_;
                crc_a_done_ = buffer_crc(0x0000);
                crc_b_done_ = buffer_crc(0x3000);
                have_done_crc_ = true;
            }
            // Cell-increment mode: step one word along a tile column, or jump to
            // the next column when leaving an odd address.
            //
            // Overwrite mode applies here exactly as it does to a linear write,
            // and this is the path the polygon rasteriser uses. Writing the
            // whole word unconditionally erases the pixels the polygon does not
            // cover, so each span blanks the tile it was supposed to draw into
            // and the finished buffer reads back empty.
            if ((mode & 0x0400) != 0) {
                overwrite_write(dram_[address], data);
            } else {
                dram_[address] = data;
            }
            // Cell-arrange stepping has a DIRECTION, and the reference
            // implementation ignores it - so did this, which is why running the
            // two against each other could never show it up.
            //
            // svpdev's "External registers behaviour" documents the decrement
            // case explicitly: "decrements 31 positions when the current
            // address is even, and 1 when the current address is odd", with the
            // measured sequence 0x0099 -> 0x0098 -> 0x0079. It is the mirror of
            // the increment case, not a copy of it: the parity test flips.
            if ((mode & 0x8000) != 0) {
                ++cell_decrement_steps_;
                slot = static_cast<std::uint32_t>(slot - ((address & 1) != 0 ? 1u : 31u));
            } else {
                slot = static_cast<std::uint32_t>(slot + ((address & 1) != 0 ? 31u : 1u));
            }
        } else if (region == kRegionIram) {
            ++iram_writes_;
            iram_[address & 0x3FF] = data;
            slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
        } else {
            // ROM is read-only; the address still advances.
            slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
        }
        // PMC follows the port that was last accessed, auto-increment included.
        // Code that reads PMC back to find out where a transfer got to needs
        // this; leaving PMC on the value it was programmed with reports the
        // start of every run instead of its end.
        gr_[kRegPmc] = slot;
        return true;
    }

    if ((mode & 0xFFF0) == 0x0800) {
        // Cartridge ROM, with the low nibble supplying address bits 16-19 and a
        // fixed step of one word. The test has to be this exact: a mode word of
        // zero is not a ROM access, and treating it as one hands the DSP ROM
        // bytes where the hardware gives it nothing.
        data = rom_word(static_cast<std::uint32_t>(address) |
                        (static_cast<std::uint32_t>(mode & 0x000F) << 16));
        slot = static_cast<std::uint32_t>(slot + 1u);
    } else if (region == kRegionDram) {
        data = dram_[address];
        slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
    } else if (region == kRegionIram) {
        data = iram_[address & 0x3FF];
        slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
    } else {
        data = 0;
        slot = static_cast<std::uint32_t>(slot + static_cast<std::uint32_t>(increment_for(mode)));
    }
    gr_[kRegPmc] = slot;
    return true;
}

// --- register file ----------------------------------------------------------

std::uint16_t Svp::read_reg(std::uint8_t index) noexcept {
    switch (index) {
        case kRegBlind:
            return 0xFFFF;
        case kRegA:
            return static_cast<std::uint16_t>(gr_[kRegA] >> 16);
        case kRegAl:
            // AL is the bottom half of the same accumulator, not its own slot.
            //
            // Reading it also CANCELS a half-built or finished PMC pair. The
            // DSP code uses "ld -, AL" as a deliberate throwaway after
            // programming a port, and without this a stale pending latch is
            // claimed by the next real port access - which then transfers
            // nothing and silently drops a word of the scene.
            pmc_pending_ = false;
            pmc_has_address_ = false;
            return static_cast<std::uint16_t>(gr_[kRegA] & 0xFFFF);
        case kRegP:
            update_product();
            return static_cast<std::uint16_t>(gr_[kRegP] >> 16);
        case kRegStack:
            // Six levels deep and it WRAPS rather than saturating: popping an
            // empty stack rolls back to the top entry. Saturating instead makes
            // a deep call chain return to the wrong place.
            if (stack_depth_ == 0) {
                stack_depth_ = static_cast<std::uint8_t>(stack_.size() - 1);
            } else {
                --stack_depth_;
            }
            return stack_[stack_depth_];
        case kRegPmc: {
            // Reading PMC walks the same two-step sequence as writing it, and
            // the second read arms the port latch exactly as a second write
            // does. Missing that leaves every programmed memory access pointing
            // at a stale address.
            const std::uint16_t low = static_cast<std::uint16_t>(gr_[kRegPmc] & 0xFFFF);
            if (!pmc_has_address_) {
                pmc_has_address_ = true;
                return low;
            }
            pmc_has_address_ = false;
            pmc_pending_ = true;
            return static_cast<std::uint16_t>(((low << 4) & 0xFFF0) | ((low >> 4) & 0x000F));
        }
        case kRegPm0: case kRegPm1: case kRegPm2: case kRegXst: case kRegPm4: {
            const std::uint8_t port = static_cast<std::uint8_t>(index - kRegPm0);
            std::uint16_t data = 0;
            if (pm_access(port, false, data)) {
                // PM4 is how the DSP reads the 68000's command cells out of
                // DRAM. Counted separately: if this never happens, the commands
                // the 68000 is writing are never seen.
                if (index == kRegPm4) {
                    ++pm4_reads_dsp_;
                    if (data != 0) {
                        ++pm4_nonzero_reads_;
                    }
                }
                return data;
            }
            if (index == kRegXst) {
                ++xst_reads_dsp_;
            }
            if (index == kRegPm0) {
                ++pm0_reads_dsp_;
                // Status register. Bit 1 says the 68000 has written XST; reading
                // it is what acknowledges that.
                const std::uint16_t status = static_cast<std::uint16_t>(gr_[kRegPm0] >> 16);
                gr_[kRegPm0] = static_cast<std::uint32_t>(status & ~0x0002u) << 16;
                return status;
            }
            return static_cast<std::uint16_t>(gr_[index] >> 16);
        }
        default:
            return static_cast<std::uint16_t>(gr_[index] >> 16);
    }
}

void Svp::write_reg(std::uint8_t index, std::uint16_t value) noexcept {
    switch (index) {
        case kRegBlind:
            // Writing the blind register discards the value, but the access
            // still counts as the one that claims a pending PMC pair.
            break;
        case kRegA:
            // A 16-bit write to A touches only its top half - AL is a register
            // in its own right and survives.
            gr_[kRegA] = (gr_[kRegA] & 0x0000FFFFu) | (static_cast<std::uint32_t>(value) << 16);
            break;
        case kRegAl:
            gr_[kRegA] = (gr_[kRegA] & 0xFFFF0000u) | value;
            break;
        case kRegStack:
            if (stack_depth_ >= stack_.size()) {
                stack_depth_ = 0;
            }
            stack_[stack_depth_++] = value;
            break;
        case kRegPmc:
            // Two writes program the port: address first, then mode.
            if (!pmc_has_address_) {
                gr_[kRegPmc] = (gr_[kRegPmc] & 0xFFFF0000u) | value;
                pmc_has_address_ = true;
            } else {
                gr_[kRegPmc] = (static_cast<std::uint32_t>(value) << 16) | (gr_[kRegPmc] & 0xFFFF);
                pmc_has_address_ = false;
                pmc_pending_ = true;
            }
            break;
        case kRegPm0: case kRegPm1: case kRegPm2: case kRegXst: case kRegPm4: {
            const std::uint8_t port = static_cast<std::uint8_t>(index - kRegPm0);
            std::uint16_t data = value;
            if (pm_access(port, true, data)) {
                break;
            }
            gr_[index] = static_cast<std::uint32_t>(value) << 16;
            if (index == kRegXst) {
                // The DSP answering the 68000: raise the "SVP has written" bit
                // the 68000 polls at 0xA15004.
                const std::uint16_t status = static_cast<std::uint16_t>(gr_[kRegPm0] >> 16);
                gr_[kRegPm0] = static_cast<std::uint32_t>(status | 0x0001u) << 16;
            }
            break;
        }
        case kRegSt:
            // Bits 8 and 9 are the two the public notes shrug at: bit 8 is
            // "OP (?)", which MAME reads as saturating arithmetic, and bit 9 is
            // "MACS (?)", a shift on the multiplier. Neither implementation
            // does anything with either. Saturation is exactly what fixed-point
            // 3D clipping needs, so find out whether this game asks for it
            // rather than inheriting the assumption that it does not.
            if ((value & 0x0100) != 0) {
                ++st_saturate_set_;
            }
            if ((value & 0x0200) != 0) {
                ++st_macs_set_;
            }
            gr_[kRegSt] = static_cast<std::uint32_t>(value) << 16;
            break;
        case kRegPc:
            set_pc(value);
            break;
        default:
            gr_[index] = static_cast<std::uint32_t>(value) << 16;
            break;
    }
}

// --- ALU --------------------------------------------------------------------

void Svp::update_flags() noexcept {
    // The reference clears L and V here as well and never sets them again; a
    // lockstep diff showed that leaving them alone is itself a divergence.
    std::uint16_t status =
        static_cast<std::uint16_t>(st() & ~(kFlagL | kFlagZ | kFlagV | kFlagN));
    if (gr_[kRegA] == 0) {
        status |= kFlagZ;
    }
    if ((gr_[kRegA] & 0x80000000u) != 0) {
        status |= kFlagN;
    }
    set_st(status);
}

void Svp::update_product() noexcept {
    // P is X * Y as a 32-bit signed product, shifted left one: the DSP treats
    // its operands as fixed-point fractions.
    const auto x = static_cast<std::int32_t>(static_cast<std::int16_t>(gr_[kRegX] >> 16));
    const auto y = static_cast<std::int32_t>(static_cast<std::int16_t>(gr_[kRegY] >> 16));
    gr_[kRegP] = static_cast<std::uint32_t>(x * y * 2);
}

void Svp::alu(std::uint8_t op, std::uint16_t operand) noexcept {
    // A 16-bit operand lines up with the TOP of the 32-bit accumulator.
    alu32(op, static_cast<std::uint32_t>(operand) << 16);
}

void Svp::alu32(std::uint8_t op, std::uint32_t value) noexcept {
    const std::uint32_t before = gr_[kRegA];
    switch (op) {
        case 1: gr_[kRegA] = before - value; break;            // sub
        case 3: {                                              // cmp - flags only
            gr_[kRegA] = before - value;
            update_flags();
            gr_[kRegA] = before;
            return;
        }
        case 4: gr_[kRegA] = before + value; break;            // add
        case 5: gr_[kRegA] &= value; break;                    // and
        case 6: gr_[kRegA] |= value; break;                    // or
        case 7: gr_[kRegA] ^= value; break;                    // eor
        default: break;
    }
    update_flags();
    // Carry (L) and overflow (V) are NOT computed, and that is deliberate.
    //
    // The wiki documents both as real testable flags, so they were implemented
    // - and a lockstep diff against the reference interpreter then failed on
    // the very first `sub a, #1`, with ST 9000 against 8000. Virtua Racing
    // never branches on either (measured: zero such conditions evaluated), but
    // it does read ST into A, mask it and write it back, so a flag the hardware
    // leaves alone propagates into the status register. The reference clears L
    // and V on every arithmetic op and never sets them; so do we.
    (void)before;
}

bool Svp::condition_true(std::uint8_t code, bool expected) const noexcept {
    if (code == 0) {
        return true;  // "always" ignores the expected-value bit
    }
    // L (4), Z (5), OV (6) and N (7) are all real, testable conditions and all
    // four are now computed. The reference interpreter only maintains Z and N
    // and treats the other two as never-taken; that is a shortcut, not the
    // hardware.
    // Only Z (5) and N (7) are taken. L (4) and OV (6) are documented as real
    // conditions but are never evaluated by this game - measured, zero - and
    // the reference treats them as never-taken, so we match it rather than
    // invent behaviour that cannot be checked against anything.
    if (code == 5 || code == 7) {
        const std::uint16_t mask = static_cast<std::uint16_t>(1u << (code + 8));
        return ((st() & mask) != 0) == expected;
    }
    if (code == 4 || code == 6) {
        ++carry_overflow_conds_;
    }
    return false;
}

void Svp::accumulator_op(std::uint8_t op) noexcept {
    auto& a = gr_[kRegA];
    // Only shr, shl, neg and abs. The disassembler names ror/rol/inc/dec for
    // codes 0, 1, 4 and 5, and the reference implements NONE of them - its
    // header says plainly "ops not used by VR are not implemented".
    //
    // That reasoning is circular, and it is a SHARED omission: running my core
    // against a copy of that reference can never reveal it, because both do
    // nothing. So count them instead of assuming.
    if (op == 0 || op == 1 || op == 4 || op == 5) {
        ++acc_op_unimplemented_;
        ++acc_op_kinds_[op & 7];
    }
    switch (op) {
        case 2: a = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> 1); break;
        case 3: a <<= 1; break;
        case 6: a = static_cast<std::uint32_t>(-static_cast<std::int32_t>(a)); break;
        case 7:
            if ((a & 0x80000000u) != 0) {
                a = static_cast<std::uint32_t>(-static_cast<std::int32_t>(a));
            }
            break;
        default: break;
    }
    update_flags();
}

// --- execution --------------------------------------------------------------

void Svp::run(int cycles) noexcept {
    if (rom_ == nullptr || halted_) {
        return;
    }
    for (int remaining = cycles; remaining > 0; --remaining) {
        // The reference implementations detect the DSP's two idle poll loops
        // (word PCs 0x2789 and 0x042A here) and stop until the 68000 writes the
        // cell being polled. That was implemented and measured: it correctly
        // reclaims ~77 MILLION DSP cycles over 6000 frames and removes both
        // loops from the hot list - and changes the game's output not at all,
        // bit for bit. The DSP finishes early and idles either way. So it is a
        // speed optimisation, not a functional requirement, and it is left out
        // rather than carrying two hardcoded ROM addresses for nothing.
        instruction_pc_ = pc();
        ++pc_histogram_[instruction_pc_];
        // Sega Retro documents that instruction cost depends on where the code
        // lives - 216ns per cycle from cartridge ROM against ~44ns from IRAM,
        // about five to one at this clock. That was implemented and measured:
        // it moves almost nothing (completions 535 -> 545) and delivers LESS
        // tile data (non-zero reads 428,554 -> 394,375), which is the third
        // independent demonstration that this DSP has cycles to spare and is
        // not what limits the game. Left at one cycle per instruction.
        const std::uint16_t opcode = fetch();
        // The PM ports need to know which instruction is touching them: only a
        // blind move may claim an armed PMC pair.
        current_opcode_ = opcode;
        ++instructions_;
        const std::uint8_t op = static_cast<std::uint8_t>(opcode >> 9);
        const bool bank1 = (opcode & 0x0100) != 0;
        const std::uint8_t pp = static_cast<std::uint8_t>(opcode & 3);
        const std::uint8_t mm = static_cast<std::uint8_t>((opcode >> 2) & 3);

        // Operand helpers shared by the load and ALU forms.
        const auto read_indirect = [&]() -> std::uint16_t {
            return ram_cell(bank1, pointer_address(bank1, pp, mm));
        };
        // Writes step their pointer without modulo wrap - see pointer_address.
        const auto write_indirect = [&](std::uint16_t value) {
            ram_cell(bank1, pointer_address(bank1, pp, mm, /*modulo=*/false)) = value;
        };
        const auto read_double_indirect = [&]() -> std::uint16_t {
            // The reference rejects any modifier on this form outright
            // ("invalid mod in ((rX))") and returns zero. Another shared blind
            // spot if the game actually uses one.
            if (mm != 0 && pp != 3) {
                ++ptr2_mod_used_;
            }
            const std::uint8_t slot = pointer_address(bank1, pp, mm);
            std::uint16_t& cell = ram_cell(bank1, slot);
            const std::uint16_t value = program_read(cell);
            cell = static_cast<std::uint16_t>(cell + 1);
            return value;
        };

        switch (op) {
            // --- loads ---
            case 0x00: {
                if (opcode == 0x0065) {  // ret, i.e. "ld PC, STACK"
                    set_pc(read_reg(kRegStack));
                    break;
                }
                if (opcode == 0x0000) {  // nop
                    break;
                }
                if (opcode == ((kRegA << 4) | kRegP)) {
                    // "ld A, P" is the one move that is 32 bits wide: it takes
                    // the whole product, not just its top half. The renderer
                    // depends on the low half surviving.
                    update_product();
                    gr_[kRegA] = gr_[kRegP];
                    break;
                }
                const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                const std::uint8_t s = static_cast<std::uint8_t>(opcode & 0xF);
                write_reg(d, read_reg(s));
                break;
            }
            case 0x01: {  // ld d, (ri)
                const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                write_reg(d, read_indirect());
                break;
            }
            case 0x02: {  // ld (ri), s
                const std::uint8_t s = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                write_indirect(read_reg(s));
                break;
            }
            case 0x03:  // ld A, adr - the whole low byte is the address, and
                        // the destination is always A. Treating bits 4-7 as a
                        // register field here loads from the wrong cell.
                write_reg(kRegA, ram_cell(bank1, static_cast<std::uint8_t>(opcode & 0xFF)));
                break;
            case 0x04: {  // ldi d, imm
                const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                write_reg(d, fetch());
                break;
            }
            case 0x05: {  // ld d, ((ri))
                const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                write_reg(d, read_double_indirect());
                break;
            }
            case 0x06: {  // ldi (ri), imm
                write_indirect(fetch());
                break;
            }
            case 0x07:  // ld adr, a
                ram_cell(bank1, static_cast<std::uint8_t>(opcode & 0xFF)) =
                    static_cast<std::uint16_t>(gr_[kRegA] >> 16);
                break;
            case 0x09: {  // ld d, ri
                const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                write_reg(d, r_[(bank1 ? 4 : 0) + pp]);
                break;
            }
            case 0x0A: {  // ld ri, s
                const std::uint8_t s = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                r_[(bank1 ? 4 : 0) + pp] = static_cast<std::uint8_t>(read_reg(s));
                break;
            }
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: {  // ldi ri, simm
                // The register is encoded across the opcode's own bits here:
                // 0001 1jpp iiii iiii.
                const std::uint8_t slot = static_cast<std::uint8_t>((opcode >> 8) & 7);
                r_[slot] = static_cast<std::uint8_t>(opcode & 0xFF);
                break;
            }

            // --- program control and the odd corners of the 0x2x block ---
            case 0x24: case 0x25: case 0x26: {
                if (op == 0x25) {  // ld d, (a)
                    const std::uint8_t d = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                    write_reg(d, program_read(static_cast<std::uint16_t>(gr_[kRegA] >> 16)));
                    break;
                }
                const std::uint8_t cond = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                const bool expected = (opcode & 0x0100) != 0;
                const std::uint16_t target = fetch();
                if (!condition_true(cond, expected)) {
                    break;
                }
                if (op == 0x24) {  // call
                    write_reg(kRegStack, pc());
                }
                set_pc(target);
                break;
            }

            // --- multiply / multiply-accumulate ---
            case 0x1B: case 0x4B: case 0x5B: {
                // mpys (0x1B), mpya (0x4B) and mld (0x5B) all load X and Y from
                // two pointers at once, then fold the previous product in.
                const std::uint8_t i = static_cast<std::uint8_t>(opcode & 3);
                const std::uint8_t j = static_cast<std::uint8_t>((opcode >> 4) & 3);
                const std::uint8_t mi = static_cast<std::uint8_t>((opcode >> 2) & 3);
                const std::uint8_t mj = static_cast<std::uint8_t>((opcode >> 6) & 3);
                // The documented syntax is "mld (rj), (ri) [, b]", where b is an
                // operand nobody has ever decoded. Both implementations discard
                // it, so if the game sets it we are both wrong together.
                if ((opcode & 0x0100) != 0) {
                    ++mac_b_bit_set_;
                }
                if (op == 0x5B) {  // mld: clear the accumulator first
                    // mld clears all four flags outright and then sets Z, which
                    // is not the same as recomputing them from a zero A now
                    // that carry and overflow are real.
                    gr_[kRegA] = 0;
                    set_st(static_cast<std::uint16_t>((st() & 0x0FFF) | kFlagZ));
                } else {
                    update_product();
                    if (op == 0x4B) {
                        gr_[kRegA] += gr_[kRegP];  // mpya
                    } else {
                        gr_[kRegA] -= gr_[kRegP];  // mpys
                    }
                    update_flags();
                }
                gr_[kRegX] = static_cast<std::uint32_t>(ram_cell(false, pointer_address(false, i, mi)))
                             << 16;
                gr_[kRegY] = static_cast<std::uint32_t>(ram_cell(true, pointer_address(true, j, mj)))
                             << 16;
                update_product();
                break;
            }

            // --- accumulator / flag modification ---
            case 0x48: {
                const std::uint8_t cond = static_cast<std::uint8_t>((opcode >> 4) & 0xF);
                const bool expected = (opcode & 0x0100) != 0;
                if (condition_true(cond, expected)) {
                    accumulator_op(static_cast<std::uint8_t>(opcode & 7));
                }
                break;
            }

            default: {
                // Everything left is the ALU block: the operation is the top
                // three bits and the low nibble of the block selects the operand
                // form, exactly mirroring the load encodings above.
                const std::uint8_t alu_op = static_cast<std::uint8_t>(op >> 4);
                // 0 is the load block and 2 is the branch block; both have their
                // own cases above, so anything reaching here with those values
                // is an encoding the DSP does not define. Falling through would
                // consume an immediate word and desynchronise the PC.
                if (alu_op == 0 || alu_op == 2) {
                    note_unknown(opcode);
                    break;
                }
                switch (op & 0x0F) {
                    case 0x00: {  // OP A, s
                        // P and A are 32-bit registers, and when either is the
                        // source the operation is 32 bits wide - it is not the
                        // 16-bit value placed at the top of the accumulator.
                        // "add a, p" is how every multiply-accumulate chain
                        // folds in its last product, so doing it 16 bits wide
                        // throws away the low half of every 3D coordinate.
                        const std::uint8_t s = static_cast<std::uint8_t>(opcode & 0xF);
                        if (s == kRegP) {
                            update_product();
                            ++alu32_ops_;
                            alu32(alu_op, gr_[kRegP]);
                        } else if (s == kRegA) {
                            ++alu32_ops_;
                            alu32(alu_op, gr_[kRegA]);
                        } else {
                            alu(alu_op, read_reg(s));
                        }
                        break;
                    }
                    case 0x01:  // OP A, (ri)
                        alu(alu_op, read_indirect());
                        break;
                    case 0x03:  // OP A, adr
                        alu(alu_op, ram_cell(bank1, static_cast<std::uint8_t>(opcode & 0xFF)));
                        break;
                    case 0x04:  // OPi A, imm
                        alu(alu_op, fetch());
                        break;
                    case 0x05:  // OP A, ((ri))
                        alu(alu_op, read_double_indirect());
                        break;
                    case 0x09:  // OP A, ri
                        alu(alu_op, r_[(bank1 ? 4 : 0) + pp]);
                        break;
                    case 0x0C:  // OPi A, simm
                        alu(alu_op, static_cast<std::uint16_t>(opcode & 0xFF));
                        break;
                    default:
                        note_unknown(opcode);
                        break;
                }
                break;
            }
        }
    }
}

// --- 68000 bus --------------------------------------------------------------

bool Svp::handles(std::uint32_t address) noexcept {
    // DSP DRAM and its two cell-arranged mirrors, plus the control registers.
    if (address >= 0x300000 && address <= 0x31FFFF) return true;
    if (address >= 0x390000 && address <= 0x3AFFFF) return true;
    if (address >= 0xA15000 && address <= 0xA1500B) return true;
    return false;
}

std::uint16_t Svp::read16(std::uint32_t address) noexcept {
    if (address >= 0x300000 && address <= 0x31FFFF) {
        const std::uint32_t word = (address & 0x1FFFF) >> 1;
        ++dram_read_buckets_[(word >> 12) & 0x0F];
        // The game brackets its tile transfer with the halt register. Any word
        // the 68000 takes out of the buffer while the DSP is still RUNNING can
        // have been rewritten underneath it - which is what a missing scanline
        // through a moving car looks like. A static logo cannot show this, so
        // the earlier clock-rate sweep could not rule it out.
        if (halted_) {
            ++dram_reads_while_halted_;
        } else {
            ++dram_reads_while_running_;
        }
        // What the 68000 actually receives. If the tile buffer is filled and
        // then blanked again before the transfer reads it, the writes look
        // healthy while every read still comes back empty.
        if (dram_[word] != 0) {
            ++dram_nonzero_reads_;
        }
        return dram_[word];
    }
    if (address >= 0x390000 && address <= 0x3AFFFF) {
        // "Cell arrange": the same DRAM seen in the order the VDP wants tiles,
        // so the 68000 can DMA rendered polygons straight into VRAM.
        const std::uint32_t index = cell_arrange_index(address);
        ++cell_reads_;
        if (dram_[index] != 0) {
            ++cell_nonzero_reads_;
        }
        return dram_[index];
    }
    if (address >= 0xA15000 && address <= 0xA15003) {
        return static_cast<std::uint16_t>(gr_[kRegXst] >> 16);
    }
    if (address >= 0xA15004 && address <= 0xA15005) {
        // Handshake status. Bit 0 means the DSP has answered; reading clears it.
        const std::uint16_t status = static_cast<std::uint16_t>(gr_[kRegPm0] >> 16);
        gr_[kRegPm0] = static_cast<std::uint32_t>(status & ~0x0001u) << 16;
        return status;
    }
    return 0xFFFF;
}

void Svp::write16(std::uint32_t address, std::uint16_t value) noexcept {
    if (address >= 0x300000 && address <= 0x31FFFF) {
        // 0x30FE06 and 0x30FE08 are the command cells the 68000 uses to ask the
        // DSP for a frame; the DSP polls them through PM4. If these are never
        // written the DSP has nothing to do, and the 68000 in turn waits on the
        // completion cell the DSP would have set - a standoff where neither
        // side moves.
        if (address == 0x30FE06 || address == 0x30FE07) {
            ++cmd_30fe06_writes_;
            // Start the stopwatch: how much DSP work a render actually costs is
            // the number that decides whether the game waits for it.
            instructions_at_command_ = instructions_;

        } else if (address == 0x30FE08 || address == 0x30FE09) {
            ++cmd_30fe08_writes_;

        }
        // What the 68000 itself puts into the DSP's memory. If this is only
        // the two command cells then the DSP works entirely from ROM and its
        // own state, and the 68000 cannot be feeding it bad geometry.
        ++writes_68k_[(((address & 0x1FFFF) >> 1) >> 12) & 0x0F];
        // The 68000 clears the tile buffers itself. The halt register brackets
        // its READS; if its WRITES land while the DSP is rendering, the two are
        // scribbling over each other and the survivors are the tearing.
        if (halted_) { ++writes_68k_halted_; } else { ++writes_68k_running_; }
        {
            const std::uint32_t word = (address & 0x1FFFF) >> 1;
            const int buffer = (word < 0x3000) ? 0 : ((word < 0x6000) ? 1 : 2);
            // Double buffering only works if the 68000 clears the buffer the
            // DSP is NOT drawing into. If these are ever the same buffer the
            // CPU is wiping tiles out from under the rasteriser, and only the
            // survivors reach the screen.
            if (buffer < 2) {
                if (buffer == dsp_last_buffer_) { ++clear_collisions_; }
                else { ++clear_clean_; }
            }
            // Touching the same buffer is not proof of harm. Writing a ZERO
            // over a word the rasteriser has already drawn is: that is a
            // rendered pixel being erased before it can be transferred.
            if (value == 0 && dram_[word] != 0) {
                ++erased_words_;
                if (buffer == dsp_last_buffer_) {
                    ++erased_words_live_;
                    // DIAGNOSTIC ONLY, MERCURY_SVP_NOERASE=1. Not a fix - it
                    // changes what the 68000's own clear does. It exists to
                    // answer one question: if these erasures never happened,
                    // would the picture come out clean? If yes the collision is
                    // the mechanism and the real fault is in the sequencing
                    // that lets the DSP start before the clear finishes.

                }
            }
        }
        dram_[(address & 0x1FFFF) >> 1] = value;
        return;
    }
    if (address >= 0x390000 && address <= 0x3AFFFF) {
        // The cell-arrange windows are writeable, through the same scrambling
        // they apply to reads. These writes used to be dropped on the floor.
        dram_[cell_arrange_index(address)] = value;
        return;
    }
    if (address >= 0xA15000 && address <= 0xA15003) {
        // The 68000 handing the DSP a command.
        ++xst_writes_68k_;
        last_xst_ = value;
        gr_[kRegXst] = static_cast<std::uint32_t>(value) << 16;
        const std::uint16_t status = static_cast<std::uint16_t>(gr_[kRegPm0] >> 16);
        gr_[kRegPm0] = static_cast<std::uint32_t>(status | 0x0002u) << 16;
        return;
    }
    if (address >= 0xA15006 && address <= 0xA15007) {
        ++halt_writes_;
        for (auto& entry : halt_values_) {
            if (entry.second == 0 || entry.first == value) {
                entry.first = value;
                ++entry.second;
                break;
            }
        }
        // The halt line. The public notes only say "writing 0xA appears to
        // halt", but this game's traffic settles it: 1275 writes of 0x000A and
        // 1275 of 0x0000, strictly alternating, about one pair per rendered
        // frame. That is a stop/start bracket around the tile transfer.
        //
        // Honouring it is what stops the 68000 reading a buffer the DSP is
        // still drawing into - the missing scanlines through cars and the torn
        // slivers across the logo. It was tried once before and rejected
        // because it starved the DSP, but that was when a render cost 1.2M
        // instructions; with the 32-bit ALU fixed it costs 274K, so the DSP can
        // now afford to stand still for the transfer.
        if (value != 0 && !halted_ && have_done_crc_) {
            if (buffer_crc(0x0000) != crc_a_done_) { ++buffer_changed_a_; }
            if (buffer_crc(0x3000) != crc_b_done_) { ++buffer_changed_b_; }
            ++transfer_starts_;
        }
        // MERCURY_SVP_HALT selects how to read this register, so the game's own
        // self-test can adjudicate instead of me guessing:
        //   0 = ignore it entirely (what both reference emulators do)
        //   1 = any non-zero value halts (the reading that broke the self-test)
        //   2 = only bit 3 halts
        // ACCEPTED AND IGNORED, and the cartridge itself says so. Writing
        // 0x000A here was read as "halt" for a while; that stalled the DSP 97%
        // of the time and made Virtua Racing's own self-test report
        // COMMUNICATION ERROR, because the chip could never answer the XST
        // handshake. Both reference emulators ignore this register and they are
        // right. Whatever 0x000A means, it is not a stop line.
        return;
    }
}

}  // namespace mercury::genesis









































