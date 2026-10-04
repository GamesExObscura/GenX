#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mercury::genesis {

// The Sega Virtua Processor: a Samsung SSP1601 DSP with its own memory
// controller, sitting on the Virtua Racing cartridge.
//
// The 68000 cannot draw Virtua Racing's polygons itself. It hands the SVP a
// scene, the SVP transforms it and rasterises 8x8 tiles into its own 128KB
// DRAM, and the 68000 then DMAs those tiles into VRAM. There is no way to fake
// this: the game polls a handshake word in that DRAM and reads the tile data
// back, so without the DSP the screen stays black past the title.
//
// The DSP is 16-bit with a 32-bit accumulator, two 256-word RAM banks, 1K words
// of instruction RAM and a 21-bit external address space reached through
// "programmable memory" registers rather than normal loads.
class Svp {
public:
    // The SSP executes from cartridge ROM above its own IRAM, so it needs the
    // image. Passing nullptr (or a non-SVP cart) leaves the chip inert and every
    // bus handler declines, which is what a normal cartridge wants.
    void attach_rom(const std::uint8_t* rom, std::size_t size) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool present() const noexcept { return rom_ != nullptr; }

    // The DSP's registers and memories, and the idle-wait flags - a DSP
    // restored mid-wait must still be waiting, or it runs its polling loop
    // on a clock the original never spent. Everything else in this class is
    // a diagnostic counter or the reference interpreter used for diffing.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(gr_);
        ar(r_);
        ar(ram0_);
        ar(ram1_);
        ar(iram_);
        ar(dram_);
        ar(stack_);
        ar(stack_depth_);
        ar(pm_read_);
        ar(pm_write_);
        ar(pmc_has_address_);
        ar(pmc_pending_);
        ar(halted_);
        ar(wait_30fe06_);
        ar(wait_30fe08_);
        ar(instruction_pc_);
    }

    // Run for the given number of DSP cycles. Every instruction is charged one
    // cycle; the SSP is not documented well enough to do better, and the game
    // synchronises through the handshake word rather than by counting.
    void run(int cycles) noexcept;

    // --- 68000 side -------------------------------------------------------
    // True for the windows the cartridge answers: DSP DRAM, the two cell-arrange
    // mirrors of it, and the control registers at 0xA150xx.
    [[nodiscard]] static bool handles(std::uint32_t address) noexcept;
    [[nodiscard]] static std::uint32_t cell_arrange_index(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) noexcept;
    // Which 4K-word region of DRAM the 68000 actually reads, counted over the
    // whole run. The DMA log only keeps its first entries, which for this game
    // are all from the intro and say nothing about the in-race transfers.
    [[nodiscard]] const std::array<std::uint64_t, 16>& dram_read_buckets() const noexcept {
        return dram_read_buckets_;
    }
    void write16(std::uint32_t address, std::uint16_t value) noexcept;

    [[nodiscard]] std::uint64_t instructions() const noexcept { return instructions_; }
    [[nodiscard]] std::uint64_t unknown_opcodes() const noexcept { return unknown_opcodes_; }
    // Distinct opcodes the decoder rejected, most-seen first, for diagnosis.
    [[nodiscard]] const std::array<std::pair<std::uint16_t, std::uint64_t>, 16>&
    unknown_samples() const noexcept { return unknown_samples_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    // Command handshake traffic. The 68000 hands the DSP a scene through XST
    // and polls PM0; if that conversation is one-sided the DSP can render the
    // same buffer forever instead of alternating.
    [[nodiscard]] std::uint64_t alu32_ops() const noexcept { return alu32_ops_; }
    // 68000 reads through the cell-arrange windows: the tile DMA's real source.
    [[nodiscard]] std::uint64_t cell_reads() const noexcept { return cell_reads_; }
    // Does this game ever touch the unmodelled internal ROM at 0xFC00-0xFFFF?
    [[nodiscard]] std::uint64_t irom_reads() const noexcept { return irom_reads_; }
    [[nodiscard]] std::uint64_t irom_fetches() const noexcept { return irom_fetches_; }
    // Branches that ask about carry or overflow, and PM accesses abandoned
    // because a non-blind instruction found PMC armed.
    [[nodiscard]] std::uint64_t carry_overflow_conds() const noexcept { return carry_overflow_conds_; }
    [[nodiscard]] std::uint64_t pmc_non_blind() const noexcept { return pmc_non_blind_; }
    [[nodiscard]] std::uint64_t rpl_pointer_steps() const noexcept { return rpl_pointer_steps_; }
    [[nodiscard]] std::uint64_t rpl_write_steps() const noexcept { return rpl_write_steps_; }
    [[nodiscard]] std::uint64_t halt_writes() const noexcept { return halt_writes_; }
    [[nodiscard]] const std::array<std::uint64_t, 16>& writes_68k() const noexcept { return writes_68k_; }
    [[nodiscard]] std::uint64_t cell_decrement_steps() const noexcept { return cell_decrement_steps_; }
    [[nodiscard]] std::uint64_t st_saturate_set() const noexcept { return st_saturate_set_; }
    [[nodiscard]] std::uint64_t st_macs_set() const noexcept { return st_macs_set_; }
    [[nodiscard]] std::uint64_t mac_b_bit_set() const noexcept { return mac_b_bit_set_; }
    [[nodiscard]] std::uint64_t acc_op_unimplemented() const noexcept { return acc_op_unimplemented_; }
    [[nodiscard]] const std::array<std::uint64_t, 8>& acc_op_kinds() const noexcept { return acc_op_kinds_; }
    [[nodiscard]] std::uint64_t ptr2_mod_used() const noexcept { return ptr2_mod_used_; }
    [[nodiscard]] std::uint64_t transfer_starts() const noexcept { return transfer_starts_; }
    [[nodiscard]] std::uint64_t buffer_changed_a() const noexcept { return buffer_changed_a_; }
    [[nodiscard]] std::uint64_t buffer_changed_b() const noexcept { return buffer_changed_b_; }
    [[nodiscard]] std::uint64_t erased_words() const noexcept { return erased_words_; }
    [[nodiscard]] std::uint64_t erased_words_live() const noexcept { return erased_words_live_; }
    [[nodiscard]] std::uint64_t clear_collisions() const noexcept { return clear_collisions_; }
    [[nodiscard]] std::uint64_t clear_clean() const noexcept { return clear_clean_; }
    [[nodiscard]] std::uint64_t writes_68k_halted() const noexcept { return writes_68k_halted_; }
    [[nodiscard]] std::uint64_t writes_68k_running() const noexcept { return writes_68k_running_; }
    [[nodiscard]] std::uint64_t dram_reads_while_halted() const noexcept { return dram_reads_while_halted_; }
    [[nodiscard]] std::uint64_t dram_reads_while_running() const noexcept { return dram_reads_while_running_; }
    [[nodiscard]] std::uint64_t unhandled_modes() const noexcept { return unhandled_modes_; }
    [[nodiscard]] const std::array<std::pair<std::uint16_t, std::uint64_t>, 12>&
    unhandled_mode_words() const noexcept { return unhandled_mode_words_; }
    [[nodiscard]] const std::array<std::pair<std::uint16_t, std::uint64_t>, 8>&
    halt_values() const noexcept { return halt_values_; }
    [[nodiscard]] const std::vector<std::uint32_t>& cell_word_counts() const noexcept {
        return cell_word_counts_;
    }
    // Colour indices the rasteriser emits, counted per nibble written.
    [[nodiscard]] const std::array<std::uint64_t, 16>& pixel_histogram() const noexcept {
        return pixel_histogram_;
    }
    [[nodiscard]] std::uint64_t cell_nonzero_reads() const noexcept { return cell_nonzero_reads_; }
    [[nodiscard]] std::uint64_t pm4_reads_dsp() const noexcept { return pm4_reads_dsp_; }
    [[nodiscard]] std::uint64_t pm4_nonzero_reads() const noexcept { return pm4_nonzero_reads_; }
    [[nodiscard]] std::uint64_t cmd_30fe06_writes() const noexcept { return cmd_30fe06_writes_; }
    [[nodiscard]] std::uint64_t cmd_30fe08_writes() const noexcept { return cmd_30fe08_writes_; }
    [[nodiscard]] std::uint64_t xst_writes_68k() const noexcept { return xst_writes_68k_; }
    [[nodiscard]] std::uint64_t xst_reads_dsp() const noexcept { return xst_reads_dsp_; }
    [[nodiscard]] std::uint64_t pm0_reads_dsp() const noexcept { return pm0_reads_dsp_; }
    [[nodiscard]] std::uint16_t last_xst() const noexcept { return last_xst_; }
    [[nodiscard]] std::uint64_t dram_writes() const noexcept { return dram_writes_; }
    [[nodiscard]] std::uint64_t iram_writes() const noexcept { return iram_writes_; }
    // How much of DRAM is non-zero: the rendered tiles, if the DSP is drawing.
    [[nodiscard]] std::size_t dram_nonzero() const noexcept;
    // One DRAM word by index, for looking at the handshake cells the 68000
    // polls (0x30FE02 is word 0x7F01, 0x30FE06 is 0x7F03, 0x30FE08 is 0x7F04).
    [[nodiscard]] std::uint16_t dram_word(std::uint32_t index) const noexcept {
        return dram_[index & 0xFFFF];
    }
    // Where in DRAM the DSP writes, in 4K-word buckets. The 68000 DMAs tiles
    // out of a specific region; if the DSP is rendering somewhere else, the
    // transfer is real and the data it carries is zero.
    [[nodiscard]] const std::array<std::uint64_t, 16>& dram_write_buckets() const noexcept {
        return dram_write_buckets_;
    }
    [[nodiscard]] std::size_t dram_nonzero_in(std::uint32_t first, std::uint32_t count) const noexcept;

private:
    // Register file. 16-bit registers keep their value in the HIGH half so that
    // A (32-bit) and its low half AL can share one slot and so that the ALU,
    // which works on the top of the accumulator, needs no shifting.
    enum Reg : std::uint8_t {
        kRegBlind = 0,  // reads as 0xFFFF; the "-" operand
        kRegX = 1,
        kRegY = 2,
        kRegA = 3,
        kRegSt = 4,
        kRegStack = 5,
        kRegPc = 6,
        kRegP = 7,
        kRegPm0 = 8,
        kRegPm1 = 9,
        kRegPm2 = 10,
        kRegXst = 11,
        kRegPm4 = 12,
        kRegExt5 = 13,
        kRegPmc = 14,
        kRegAl = 15,
    };

    // Status register bits. RPL is the modulo size for pointer arithmetic; ST5
    // and ST6 switch PM0 and XST from status registers to memory ports.
    static constexpr std::uint16_t kStRpl = 0x0007;
    static constexpr std::uint16_t kStSt56 = 0x0060;
    static constexpr std::uint16_t kFlagL = 0x1000;
    static constexpr std::uint16_t kFlagZ = 0x2000;
    static constexpr std::uint16_t kFlagV = 0x4000;
    static constexpr std::uint16_t kFlagN = 0x8000;

    // --- register access ---
    [[nodiscard]] std::uint16_t st() const noexcept {
        return static_cast<std::uint16_t>(gr_[kRegSt] >> 16);
    }
    void set_st(std::uint16_t value) noexcept {
        gr_[kRegSt] = static_cast<std::uint32_t>(value) << 16;
    }
    [[nodiscard]] std::uint16_t pc() const noexcept {
        return static_cast<std::uint16_t>(gr_[kRegPc] >> 16);
    }
    void set_pc(std::uint16_t value) noexcept {
        gr_[kRegPc] = static_cast<std::uint32_t>(value) << 16;
    }

    [[nodiscard]] std::uint16_t read_reg(std::uint8_t index) noexcept;
    void write_reg(std::uint8_t index, std::uint16_t value) noexcept;

    // --- memory ---
    [[nodiscard]] std::uint16_t program_read(std::uint16_t word_address) const noexcept;
    [[nodiscard]] std::uint16_t fetch() noexcept;
    [[nodiscard]] std::uint16_t rom_word(std::uint32_t word_address) const noexcept;

    // Pointer-register addressing: bank j picks RAM0/RAM1, p picks the register
    // inside it, m is the post-modifier. r3 and r7 are fixed and use m as a
    // small immediate index instead.
    [[nodiscard]] std::uint16_t& ram_cell(bool bank1, std::uint8_t index) noexcept;
    [[nodiscard]] std::uint8_t pointer_address(bool bank1, std::uint8_t p, std::uint8_t m,
                                               bool modulo = true) noexcept;

    // --- programmable memory ports ---
    // Returns false when the port is not acting as a memory port, which is how
    // PM0 and XST fall through to their status-register behaviour.
    [[nodiscard]] bool pm_access(std::uint8_t reg, bool write, std::uint16_t& data) noexcept;
    void latch_pmc(std::uint8_t reg, bool write) noexcept;

    // --- ALU ---
    void update_flags() noexcept;
    void alu(std::uint8_t op, std::uint16_t operand) noexcept;
    // The same operations against the full 32-bit accumulator, used when the
    // source operand is one of the two 32-bit registers (P or A).
    void alu32(std::uint8_t op, std::uint32_t value) noexcept;
    [[nodiscard]] bool condition_true(std::uint8_t code, bool expected) const noexcept;
    void accumulator_op(std::uint8_t op) noexcept;
    void update_product() noexcept;

    const std::uint8_t* rom_ = nullptr;
    std::size_t rom_size_ = 0;

    std::array<std::uint32_t, 16> gr_{};
    std::array<std::uint8_t, 8> r_{};
    std::array<std::uint16_t, 256> ram0_{};
    std::array<std::uint16_t, 256> ram1_{};
    std::array<std::uint16_t, 0x400> iram_{};
    // 128KB of DSP DRAM, addressed as words. This is the buffer the 68000 reads
    // finished tiles out of.
    std::array<std::uint16_t, 0x10000> dram_{};
    std::array<std::uint16_t, 6> stack_{};
    std::uint8_t stack_depth_ = 0;

    // PMC is programmed by two consecutive writes - address then mode - and the
    // pending pair is latched into one of these on the next access to the
    // matching PM register.
    std::array<std::uint32_t, 6> pm_read_{};
    std::array<std::uint32_t, 6> pm_write_{};
    bool pmc_has_address_ = false;  // first of the two writes has landed
    bool pmc_pending_ = false;      // the pair is complete, awaiting its port

    bool halted_ = false;
    // How often an ALU op took its 32-bit form. If this is zero the game never
    // uses "OP a, p" / "OP a, a" and the width of those ops cannot be the bug.
    std::uint64_t alu32_ops_ = 0;
    std::uint64_t cell_reads_ = 0;
    mutable std::uint64_t irom_reads_ = 0;
    std::uint64_t irom_fetches_ = 0;
    mutable std::uint64_t carry_overflow_conds_ = 0;
    std::uint64_t pmc_non_blind_ = 0;
    std::uint64_t rpl_pointer_steps_ = 0;
    std::uint64_t rpl_write_steps_ = 0;
    // What the 68000 writes to the undocumented halt register at 0xA15006.
    std::uint64_t halt_writes_ = 0;
    std::array<std::uint64_t, 16> writes_68k_{};
    std::uint64_t cell_decrement_steps_ = 0;
    std::uint64_t st_saturate_set_ = 0;
    std::uint64_t st_macs_set_ = 0;
    std::uint64_t mac_b_bit_set_ = 0;
    std::uint64_t acc_op_unimplemented_ = 0;
    std::array<std::uint64_t, 8> acc_op_kinds_{};
    std::uint64_t ptr2_mod_used_ = 0;
    std::uint64_t transfer_starts_ = 0;
    std::uint64_t buffer_changed_a_ = 0;
    std::uint64_t buffer_changed_b_ = 0;
    std::uint32_t crc_a_done_ = 0;
    std::uint32_t crc_b_done_ = 0;
    bool have_done_crc_ = false;
    [[nodiscard]] std::uint32_t buffer_crc(std::uint32_t first) const noexcept;
    std::uint64_t erased_words_ = 0;
    std::uint64_t erased_words_live_ = 0;
    std::uint64_t clear_collisions_ = 0;
    std::uint64_t clear_clean_ = 0;
    int dsp_last_buffer_ = 2;
    std::uint64_t writes_68k_halted_ = 0;
    std::uint64_t writes_68k_running_ = 0;
    std::uint64_t dram_reads_while_halted_ = 0;
    std::uint64_t dram_reads_while_running_ = 0;
    // PM accesses with a mode word this implementation does not handle.
    std::uint64_t unhandled_modes_ = 0;
    std::array<std::pair<std::uint16_t, std::uint64_t>, 12> unhandled_mode_words_{};
    std::array<std::pair<std::uint16_t, std::uint64_t>, 8> halt_values_{};
    std::array<std::uint64_t, 16> pixel_histogram_{};
    std::vector<std::uint32_t> cell_word_counts_ = std::vector<std::uint32_t>(0x10000, 0u);
    std::uint16_t current_opcode_ = 0;
    std::uint64_t cell_nonzero_reads_ = 0;
    std::uint64_t pm4_reads_dsp_ = 0;
    std::uint64_t pm4_nonzero_reads_ = 0;
    std::uint64_t cmd_30fe06_writes_ = 0;
    std::uint64_t cmd_30fe08_writes_ = 0;
    std::uint64_t xst_writes_68k_ = 0;
    std::uint64_t xst_reads_dsp_ = 0;
    std::uint64_t pm0_reads_dsp_ = 0;
    std::uint16_t last_xst_ = 0;
    std::uint64_t dram_writes_ = 0;
    std::array<std::uint64_t, 16> dram_write_buckets_{};
    std::array<std::uint64_t, 16> dram_nonzero_writes_{};
    std::array<std::uint64_t, 16> dram_read_buckets_{};
    std::array<std::pair<std::uint16_t, std::uint64_t>, 12> dram_write_modes_{};
    std::array<std::uint64_t, 16> pm_latch_buckets_{};
    std::uint64_t dram_nonzero_reads_ = 0;
    std::uint64_t dsp_completion_writes_ = 0;
    std::uint64_t instructions_at_command_ = 0;
    std::uint64_t render_instructions_total_ = 0;
    std::uint64_t render_samples_ = 0;

public:
    // Average DSP instructions between the 68000 asking for a frame and the DSP
    // saying it is done. Compare against the budget: at 800 instructions per
    // scanline and 262 lines, one display frame is 209,600.
    [[nodiscard]] std::uint64_t render_instructions_avg() const noexcept {
        return render_samples_ ? render_instructions_total_ / render_samples_ : 0;
    }
    [[nodiscard]] std::uint64_t render_samples() const noexcept { return render_samples_; }

private:

    // Idle-wait state. The DSP polls two DRAM cells and the PM0 status flag in
    // tight loops while it waits for the 68000, and those loops are not cheap:
    // measured here they consume about a THIRD of every cycle the chip is
    // given. Standing in them is not just wasted time, it is time the renderer
    // needed - the game asks for a frame roughly every fourth display frame and
    // was only getting one finished every eleventh. Detecting the loop and
    // stopping until the 68000 actually writes is what the reference
    // implementations do, and it is a functional requirement, not a speed hack.
    //
    // Loop addresses are the reference implementation's, converted from its byte
    // offsets to word PCs: 0x4F12 -> 0x2789 (waiting on 0x30FE06) and
    // 0x0854 -> 0x042A (waiting on 0x30FE08).
    static constexpr std::uint16_t kWaitPc30fe06 = 0x2789;
    static constexpr std::uint16_t kWaitPc30fe08 = 0x042A;
    bool wait_30fe06_ = false;
    bool wait_30fe08_ = false;
    std::uint16_t instruction_pc_ = 0;
    [[nodiscard]] bool waiting() const noexcept { return wait_30fe06_ || wait_30fe08_; }

public:
    [[nodiscard]] std::uint64_t wait_cycles_skipped() const noexcept {
        return wait_cycles_skipped_;
    }

private:
    std::uint64_t wait_cycles_skipped_ = 0;
    // Where the DSP burns its cycles. A chip that is not finishing frames in
    // time is either doing too much work or stuck somewhere, and only the
    // distribution says which.
    std::vector<std::uint32_t> pc_histogram_ = std::vector<std::uint32_t>(0x10000, 0);

public:
    [[nodiscard]] const std::vector<std::uint32_t>& pc_histogram() const noexcept {
        return pc_histogram_;
    }

private:

public:
    [[nodiscard]] std::uint64_t dsp_completion_writes() const noexcept {
        return dsp_completion_writes_;
    }

private:

public:
    [[nodiscard]] std::uint64_t dram_nonzero_reads() const noexcept { return dram_nonzero_reads_; }

private:
    std::uint64_t cell_writes_ = 0;
    std::array<std::uint64_t, 16> cell_nonzero_writes_{};

public:
    [[nodiscard]] std::uint64_t cell_writes() const noexcept { return cell_writes_; }
    [[nodiscard]] const std::array<std::uint64_t, 16>& cell_nonzero_writes() const noexcept {
        return cell_nonzero_writes_;
    }

private:

public:
    [[nodiscard]] const std::array<std::uint64_t, 16>& pm_latch_buckets() const noexcept {
        return pm_latch_buckets_;
    }

private:

public:
    [[nodiscard]] const std::array<std::pair<std::uint16_t, std::uint64_t>, 12>&
    dram_write_modes() const noexcept { return dram_write_modes_; }

private:
    std::uint64_t dram_overwrite_writes_ = 0;

public:
    [[nodiscard]] const std::array<std::uint64_t, 16>& dram_nonzero_writes() const noexcept {
        return dram_nonzero_writes_;
    }
    [[nodiscard]] std::uint64_t dram_overwrite_writes() const noexcept {
        return dram_overwrite_writes_;
    }

private:
    std::uint64_t iram_writes_ = 0;
    std::uint64_t instructions_ = 0;
    std::uint64_t unknown_opcodes_ = 0;
    void note_unknown(std::uint16_t opcode) noexcept;
    std::uint16_t first_unknown_pc_ = 0;
    std::uint16_t last_unknown_pc_ = 0;

public:
    [[nodiscard]] std::uint16_t first_unknown_pc() const noexcept { return first_unknown_pc_; }
    [[nodiscard]] std::uint16_t last_unknown_pc() const noexcept { return last_unknown_pc_; }

private:
    std::array<std::pair<std::uint16_t, std::uint64_t>, 16> unknown_samples_{};
};

}  // namespace mercury::genesis
























