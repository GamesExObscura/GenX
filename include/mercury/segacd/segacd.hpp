#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mercury/core/m68k_cpu.hpp"
#include "mercury/segacd/disc.hpp"

namespace mercury::segacd {

// The Sega CD / Mega-CD: a second computer bolted under the Mega Drive.
//
// A 12.5MHz 68000 of its own (the "sub CPU") with 512KB of program RAM, a
// 256KB block of "Word RAM" the two processors pass back and forth, 8KB of
// battery-backed RAM, a CD drive, a CD data controller that moves sectors off
// the disc, an eight-channel PCM sound chip, and a rotation/scaling graphics
// processor. The two 68000s talk through a gate array: a few flag bytes and
// sixteen command/status words, and interrupts in one direction only - the
// main CPU can interrupt the sub CPU, never the reverse.
//
// Everything here is written from hardware facts. Genesis Plus GX is the
// reference it was checked against (read, never copied - its licence does not
// allow that), and each rule that came from there says so.
//
// ONE BOOT MODE: from CD. The BIOS sits at the bottom of the main CPU's map,
// the program RAM window at $020000 and Word RAM at $200000, and there is no
// cartridge. (A cartridge in the slot with a CD unit attached boots the
// cartridge instead and moves all of this to $400000; that "mode 1" is not
// modelled.)
class SegaCd {
  public:
    static constexpr std::uint32_t kSubClockHz = 12'500'000;
    static constexpr std::size_t kBiosSize = 0x20000;
    static constexpr std::size_t kPrgRamSize = 0x80000;
    static constexpr std::size_t kWordRamSize = 0x40000;
    static constexpr std::size_t kBackupRamSize = 0x2000;
    static constexpr std::size_t kPcmRamSize = 0x10000;
    // PCM and CD audio both come out at this rate, stereo, interleaved.
    static constexpr std::uint32_t kAudioRate = 44'100;

    SegaCd();

    // --- setup ---------------------------------------------------------------
    // The 128KB boot ROM. Nothing runs without it: unlike the 32X, the CD
    // unit's startup is the BIOS's own code on both processors, not a small
    // piece of firmware that could be stood in for.
    bool load_bios(const std::vector<std::uint8_t>& image, std::string& error);
    [[nodiscard]] bool bios_loaded() const noexcept { return bios_loaded_; }
    // What the BIOS image says about itself, for the load message.
    [[nodiscard]] std::string bios_description() const;
    // The BIOS's region, 'U', 'E' or 'J' from its header, or 0. The backup
    // RAM belongs to the console rather than to a disc, so the host keeps one
    // file per console region rather than one per game.
    [[nodiscard]] char bios_region() const noexcept;

    // A disc in the tray, or none - the BIOS with no disc is the console's own
    // CD player and memory manager screen, which is a perfectly good thing to
    // boot.
    bool insert_disc(const std::filesystem::path& image, std::string& error);
    void eject_disc();
    // Change the disc with the console still running, the way a multi-disc
    // game expects: tray out, new disc, tray in. Does NOT reset anything the
    // game is holding - see the note on the definition.
    bool swap_disc(const std::filesystem::path& image, std::string& error);
    [[nodiscard]] Disc& disc() noexcept { return disc_; }
    [[nodiscard]] const Disc& disc() const noexcept { return disc_; }

    // Power-on: clears the RAMs (not the battery RAM) and holds the sub CPU.
    // A console reset: the main CPU restarts and the CD side goes back to its
    // power-on register state; RAM survives.
    void reset(bool power_on);

    // --- the main CPU's view -------------------------------------------------
    // $000000-$3FFFFF (BIOS, program RAM window, Word RAM) and $A12000-$A120FF
    // (the gate array). The accessors below take either range.
    [[nodiscard]] static bool main_decodes(std::uint32_t address) noexcept {
        address &= 0xFFFFFF;
        return address < 0x400000 || main_decodes_registers(address);
    }
    [[nodiscard]] static bool main_decodes_registers(std::uint32_t address) noexcept {
        return (address & 0xFFFF00) == 0xA12000;
    }
    [[nodiscard]] std::uint16_t main_read16(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint8_t main_read8(std::uint32_t address) noexcept;
    void main_write16(std::uint32_t address, std::uint16_t value) noexcept;
    void main_write8(std::uint32_t address, std::uint8_t value) noexcept;

    // --- time ------------------------------------------------------------------
    // Run the CD side for the time the main CPU just spent. The sub CPU and
    // everything clocked from it catch up to the same moment, so each side
    // sees the other's register writes within one main-CPU instruction.
    void advance(std::uint32_t main_cycles, std::uint32_t main_clock_hz) noexcept;
    // The frame boundary: flushes audio that is generated lazily.
    void end_frame() noexcept;

    // --- audio -----------------------------------------------------------------
    // PCM and CD audio mixed, stereo interleaved at kAudioRate. The host takes
    // it each frame like the other sound chips' buffers.
    [[nodiscard]] const std::vector<std::int16_t>& audio() const noexcept { return audio_; }
    void clear_audio() noexcept { audio_.clear(); }

    // --- battery RAM ---------------------------------------------------------
    [[nodiscard]] const std::vector<std::uint8_t>& backup_ram() const noexcept { return backup_ram_; }
    void restore_backup_ram(const std::vector<std::uint8_t>& bytes) noexcept;
    // Write the empty directory a formatted backup RAM carries, losing
    // whatever was saved. See segacd.cpp.
    void format_backup_ram() noexcept;
    [[nodiscard]] bool backup_ram_formatted() const noexcept;
    [[nodiscard]] bool backup_ram_dirty() const noexcept { return backup_ram_dirty_; }
    void clear_backup_ram_dirty() noexcept { backup_ram_dirty_ = false; }

    // --- diagnostics -----------------------------------------------------------
    [[nodiscard]] mercury::core::M68kCpu& sub() noexcept { return sub_; }
    [[nodiscard]] const mercury::core::M68kCpu& sub() const noexcept { return sub_; }
    [[nodiscard]] bool sub_running() const noexcept;
    [[nodiscard]] std::uint64_t sub_instructions() const noexcept { return sub_instructions_; }
    // MERCURY_SCD_PROFILE=1: every sub-CPU instruction counted by its PC,
    // from MERCURY_SCD_PROFILE_FROM frames on (default 0). "Where is the sub
    // CPU spending its time" is the first question when the main CPU waits
    // on it for ever.
    [[nodiscard]] const std::unordered_map<std::uint32_t, std::uint64_t>& sub_pc_counts() const noexcept {
        return sub_pc_counts_;
    }
    [[nodiscard]] std::uint64_t sub_unimplemented() const noexcept { return sub_unimplemented_; }
    [[nodiscard]] const std::array<std::uint64_t, 7>& sub_interrupts() const noexcept {
        return sub_interrupts_;
    }
    [[nodiscard]] std::uint64_t cdd_commands() const noexcept { return cdd_commands_; }
    [[nodiscard]] std::uint64_t sectors_decoded() const noexcept { return sectors_decoded_; }
    [[nodiscard]] std::uint16_t reg(std::size_t word_index) const noexcept {
        return regs_[word_index & 0xFF];
    }
    [[nodiscard]] const std::vector<std::uint8_t>& prg_ram() const noexcept { return prg_ram_; }
    [[nodiscard]] const std::vector<std::uint8_t>& word_ram() const noexcept { return word_ram_; }
    [[nodiscard]] std::string describe() const;

    // --- save states -----------------------------------------------------------
    // The whole CD side, in one pass that serves both directions.
    //
    // THE STRUCTS GO FIELD BY FIELD, never whole. Several of them have gaps
    // the compiler pads - PcmChannel has two bytes of it between `ls` and
    // `st` - and padding holds whatever was on the stack. Written raw, two
    // machines in exactly the same state would save different bytes, and
    // netplay, which compares the hash of a save state at both ends, would
    // call that a desync.
    //
    // Carried: the battery RAM, because a game reads it while it runs and a
    // restored game must see what it saw. Not carried: the BIOS and the disc
    // (the machine's state header says which ones), the host's audio buffer
    // (taken every frame), and the diagnostic counters.
    template <typename Archive>
    void visit_state(Archive& ar) {
        sub_.visit_state(ar);
        ar(prg_ram_);
        ar(word_ram_);
        ar(backup_ram_);
        ar(regs_);
        ar(hint_vector_);
        ar(led_);
        ar(dmna_pending_);
        ar(sub_word_ram_wait_);
        ar(pending_);
        std::uint32_t deferred = static_cast<std::uint32_t>(deferred_.size());
        ar(deferred);
        if constexpr (Archive::loading()) {
            deferred_.resize(deferred > 64 ? 0 : deferred);
        }
        for (auto& write : deferred_) {
            ar(write.address);
            ar(write.value);
            ar(write.word);
        }
        ar(now_);
        ar(budget_);
        ar(timer_left_);
        ar(stopwatch_base_);
        ar(cdd_phase_);
        ar(cdd_status_);
        ar(cdd_pending_);
        ar(cdd_latency_);
        ar(cdd_lba_);
        ar(cdd_track_);
        ar(cdd_scan_offset_);
        ar(fader_);

        ar(cdc_.ifstat);
        ar(cdc_.ifctrl);
        ar(cdc_.dbc);
        ar(cdc_.dac);
        ar(cdc_.pt);
        ar(cdc_.wa);
        ar(cdc_.ctrl);
        ar(cdc_.head);
        ar(cdc_.stat);
        ar(cdc_.irq);
        ar(cdc_.dma);
        ar(cdc_.halted_dma);
        ar(cdc_.dma_clock);
        ar(cdc_.decoder_until);
        ar(cdc_.ram);

        ar(gfx_.clock);
        ar(gfx_.cycles_per_line);
        ar(gfx_.dot_mask);
        ar(gfx_.stamp_shift);
        ar(gfx_.map_shift);
        ar(gfx_.map_base);
        ar(gfx_.trace);
        ar(gfx_.buffer_offset);
        ar(gfx_.buffer_start);

        for (auto& channel : pcm_channels_) {
            ar(channel.env);
            ar(channel.pan);
            ar(channel.fd);
            ar(channel.ls);
            ar(channel.st);
            ar(channel.addr);
        }
        ar(pcm_ram_);
        ar(pcm_bank_);
        ar(pcm_on_);
        ar(pcm_selected_);
        ar(pcm_enabled_);
        ar(pcm_clock_);
        // Both queues are emptied (PCM) or trimmed to 4704 samples (CD audio)
        // every frame, so 64K is far more than either can hold when a state
        // is taken - and a ceiling for one that arrives from elsewhere.
        ar.variable(pcm_native_, kMaxQueuedSamplesInState);
        ar(pcm_resample_phase_);
        ar(pcm_last_);
        // CD audio already read off the disc and waiting to be played. Without
        // it a load drops up to a few frames of music.
        ar.variable(cdda_, kMaxQueuedSamplesInState);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    static constexpr std::size_t kMaxQueuedSamplesInState = 65536;
    // A loaded state can come from someone else (the netplay host sends the
    // joiner its machine; state files are shared). Every bank, channel number,
    // track number and clock is brought back to what the hardware could hold,
    // so none of them can index past a buffer or spin a catch-up loop.
    void sanitize_loaded() noexcept;

    // Which BIOS image is running, as a number, for the same reason the 32X
    // has firmware_id: a state only means something to the code it came from.
    [[nodiscard]] std::uint64_t bios_id() const noexcept;
    // Which disc is in the tray: the Sega CD header, the track count and the
    // lead-out. 0 with no disc.
    [[nodiscard]] std::uint64_t disc_id() const noexcept;

    // Drive states, as reported in the first status nibble.
    enum DriveStatus : std::uint8_t {
        kStop = 0x00,
        kPlay = 0x01,
        kSeek = 0x02,
        kScan = 0x03,
        kPause = 0x04,
        kTrayOpen = 0x05,
        kReadingToc = 0x09,
        kNoDisc = 0x0B,
        kDiscEnd = 0x0C,
    };

  private:
    // --- the sub CPU's view ----------------------------------------------------
    [[nodiscard]] std::uint16_t sub_read16(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint8_t sub_read8(std::uint32_t address) noexcept;
    void sub_write16(std::uint32_t address, std::uint16_t value) noexcept;
    void sub_write8(std::uint32_t address, std::uint8_t value) noexcept;

    // --- gate array ------------------------------------------------------------
    [[nodiscard]] std::uint16_t main_register_read16(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint8_t main_register_read8(std::uint32_t address) noexcept;
    void main_register_write16(std::uint32_t address, std::uint16_t value) noexcept;
    void main_register_write8(std::uint32_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint16_t sub_register_read16(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint8_t sub_register_read8(std::uint32_t address) noexcept;
    void sub_register_write16(std::uint32_t address, std::uint16_t value) noexcept;
    void sub_register_write8(std::uint32_t address, std::uint8_t value) noexcept;

    // $A12001: reset and bus request for the sub CPU.
    void write_sub_control(std::uint8_t value) noexcept;
    // $A12003 from the main side and $FF8003 from the sub side: who has Word
    // RAM, and in which layout.
    void main_write_memory_mode(std::uint8_t value) noexcept;
    void sub_write_memory_mode(std::uint8_t value) noexcept;
    void main_write_memory_mode_inner(std::uint8_t value) noexcept;
    void sub_write_memory_mode_inner(std::uint8_t value) noexcept;
    void log_memory_mode(char side, std::uint8_t value, std::uint8_t before) const;
    // Main-CPU register writes waiting for the end of their instruction. See
    // main_write16 in segacd.cpp.
    struct DeferredWrite {
        std::uint32_t address;
        std::uint16_t value;
        bool word;
    };
    std::vector<DeferredWrite> deferred_;
    void flush_main_writes() noexcept;
    // The main CPU interrupting the sub CPU (level 2).
    void request_level2() noexcept;

    // Register file helpers, word-indexed by byte address / 2.
    [[nodiscard]] std::uint8_t hi(std::size_t index) const noexcept {
        return static_cast<std::uint8_t>(regs_[index] >> 8);
    }
    [[nodiscard]] std::uint8_t lo(std::size_t index) const noexcept {
        return static_cast<std::uint8_t>(regs_[index]);
    }
    void set_hi(std::size_t index, std::uint8_t value) noexcept {
        regs_[index] = static_cast<std::uint16_t>((regs_[index] & 0x00FF) | (value << 8));
    }
    void set_lo(std::size_t index, std::uint8_t value) noexcept {
        regs_[index] = static_cast<std::uint16_t>((regs_[index] & 0xFF00) | value);
    }

    // --- Word RAM ----------------------------------------------------------------
    // One 256KB array holds both layouts. In 2M mode it is linear. In 1M mode
    // it is two 128KB banks that are the EVEN and ODD words of that same array
    // - which is how the hardware is wired, so a mode switch moves no data.
    [[nodiscard]] static std::uint32_t bank_offset(int bank, std::uint32_t offset) noexcept {
        return ((offset >> 1) << 2) | (static_cast<std::uint32_t>(bank & 1) << 1) | (offset & 1);
    }
    [[nodiscard]] bool one_meg_mode() const noexcept { return (lo(0x01) & 0x04) != 0; }
    // 2M mode: who owns the whole block. RET set means the main CPU.
    [[nodiscard]] bool main_owns_word_ram() const noexcept { return (lo(0x01) & 0x01) != 0; }
    // 1M mode: which bank each side has. RET selects.
    [[nodiscard]] int main_bank() const noexcept { return (lo(0x01) & 0x01) != 0 ? 1 : 0; }
    [[nodiscard]] int sub_bank() const noexcept { return main_bank() ^ 1; }
    [[nodiscard]] std::uint32_t cell_image_offset(std::uint32_t address) const noexcept;

    // --- interrupts to the sub CPU ---------------------------------------------
    // Raise a level (1-6) if the sub side has it enabled.
    void raise(int level) noexcept;
    void update_sub_interrupt() noexcept;
    void acknowledge(int level) noexcept;

    // --- clocks ------------------------------------------------------------------
    void clock(std::uint32_t sub_cycles) noexcept;

    // --- CD drive ----------------------------------------------------------------
    void cdd_reset() noexcept;
    void cdd_tick() noexcept;     // 75 times a second
    void cdd_command() noexcept;  // the BIOS wrote a command
    void cdd_write_checksum() noexcept;
    void cdd_report_time(int lba) noexcept;
    void cdd_play_audio_block() noexcept;

    // --- CD data controller (LC8951) ---------------------------------------------
    void cdc_reset() noexcept;
    void cdc_register_write(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t cdc_register_read() noexcept;
    [[nodiscard]] std::uint16_t cdc_host_read(bool main_side) noexcept;
    void cdc_start_transfer() noexcept;
    void cdc_run_dma(std::uint64_t now) noexcept;
    void cdc_finish_transfer() noexcept;
    void cdc_decode_block(std::uint32_t header) noexcept;
    void cdc_update_decoder_window() noexcept;
    void cdc_raise_if_edge(std::uint8_t previous_irq) noexcept;

    // --- graphics ASIC (rotation and scaling) -------------------------------------
    void gfx_reset() noexcept;
    void gfx_start(std::uint16_t trace_base) noexcept;
    void gfx_update() noexcept;
    void gfx_render_line(std::uint32_t buffer_index, std::uint32_t width) noexcept;
    [[nodiscard]] bool gfx_busy() const noexcept { return (hi(0x58 >> 1) & 0x80) != 0; }

    // --- PCM (RF5C164) -------------------------------------------------------------
    void pcm_reset() noexcept;
    void pcm_write(std::uint32_t offset, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t pcm_read(std::uint32_t offset) noexcept;
    void pcm_catch_up() noexcept;

    // --- state -----------------------------------------------------------------
    mercury::core::M68kCpu sub_;
    Disc disc_;

    std::vector<std::uint8_t> bios_ = std::vector<std::uint8_t>(kBiosSize, 0xFF);
    std::vector<std::uint8_t> prg_ram_ = std::vector<std::uint8_t>(kPrgRamSize, 0);
    std::vector<std::uint8_t> word_ram_ = std::vector<std::uint8_t>(kWordRamSize, 0);
    std::vector<std::uint8_t> backup_ram_ = std::vector<std::uint8_t>(kBackupRamSize, 0);
    bool bios_loaded_ = false;
    bool backup_ram_dirty_ = false;

    // The gate array's registers, 256 words, indexed by (byte address / 2).
    // Both processors see the same storage through different windows and
    // with different rules; see the register functions.
    std::array<std::uint16_t, 0x100> regs_{};
    // The main CPU's horizontal interrupt vector, $A12006. It replaces the
    // low word of the BIOS's own vector at $000072.
    std::uint16_t hint_vector_ = 0xFFFF;
    // The sub side's LED register, which shares its address with the main
    // side's reset/bus-request register and so cannot live in regs_[0].
    std::uint8_t led_ = 0;
    // 1M mode: the main CPU asked for the banks to be swapped (wrote DMNA);
    // the swap happens when the sub CPU next writes RET.
    bool dmna_pending_ = false;
    // The sub CPU is stopped mid-instruction on Word RAM it does not own.
    // See sub_word_ram_locked in segacd.cpp.
    bool sub_word_ram_wait_ = false;
    [[nodiscard]] bool sub_word_ram_locked() const noexcept;
    void note_sub_word_ram_wait() noexcept;

    // Interrupt levels waiting to be taken, bit n for level n.
    std::uint8_t pending_ = 0;

    // Time, in sub-CPU cycles since power-on.
    std::uint64_t now_ = 0;
    // Main-CPU time converted into sub-CPU time, in units of 1/(main*sub) of a
    // second so the conversion never drifts.
    std::int64_t budget_ = 0;
    // Timer: cycles until the next level 3 interrupt, 0 when stopped.
    std::int64_t timer_left_ = 0;
    // Stopwatch: cycles banked towards the next tick.
    std::uint64_t stopwatch_base_ = 0;
    // The drive's 75Hz clock, counted in thirds of a sub-CPU cycle.
    std::uint64_t cdd_phase_ = 0;

    // Drive state.
    std::uint8_t cdd_status_ = kNoDisc;
    std::uint8_t cdd_pending_ = 0;   // state to enter once a seek finishes
    int cdd_latency_ = 0;            // 75Hz ticks until the drive is ready
    int cdd_lba_ = 0;
    int cdd_track_ = 0;
    int cdd_scan_offset_ = 0;
    std::uint16_t fader_ = 0x400;

    // Data controller state.
    struct Cdc {
        std::uint8_t ifstat = 0xFF;
        std::uint8_t ifctrl = 0;
        std::uint16_t dbc = 0;   // data byte counter
        std::uint16_t dac = 0;   // data address counter
        std::uint16_t pt = 0;    // block pointer
        std::uint16_t wa = 0;    // write address
        std::array<std::uint8_t, 2> ctrl{};
        std::array<std::array<std::uint8_t, 4>, 2> head{};
        std::array<std::uint8_t, 4> stat{};
        std::uint8_t irq = 0;    // which of DTEI/DECI is driving /INT
        // Transfer destination while a DMA runs (the $04 DD value, 0 = none),
        // and a DMA waiting because the other CPU holds its target memory.
        std::uint8_t dma = 0;
        std::uint8_t halted_dma = 0;
        std::uint64_t dma_clock = 0;
        std::uint64_t decoder_until = 0;
        // 16KB of buffer RAM, plus one block of slack so a sector written at
        // the very end can run over and be folded back to the start.
        std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(0x4000 + 2352, 0);
    } cdc_;

    // Graphics ASIC state, for the operation in progress. Offsets are byte
    // offsets into 2M Word RAM; positions are in the chip's fixed point.
    struct Gfx {
        std::uint64_t clock = 0;            // sub cycle the operation has reached
        std::uint32_t cycles_per_line = 1;
        std::uint32_t dot_mask = 0x07FFFF;  // stamp map size, in 13.11 dots
        std::uint32_t stamp_shift = 15;
        std::uint32_t map_shift = 4;
        std::uint32_t map_base = 0;
        std::uint32_t trace = 0;
        std::uint32_t buffer_offset = 0;
        std::uint32_t buffer_start = 0;
    } gfx_;

    // PCM chip state.
    struct PcmChannel {
        std::uint8_t env = 0;
        std::uint8_t pan = 0xFF;
        std::uint16_t fd = 0;     // step, 5.11 fixed point
        std::uint16_t ls = 0;     // loop start
        std::uint32_t st = 0;     // start, 16.11 fixed point
        std::uint32_t addr = 0;   // current position, 16.11 fixed point
    };
    std::array<PcmChannel, 8> pcm_channels_{};
    std::vector<std::uint8_t> pcm_ram_ = std::vector<std::uint8_t>(kPcmRamSize, 0);
    std::uint32_t pcm_bank_ = 0;       // byte offset of the 4KB window
    std::uint8_t pcm_on_ = 0;          // bit per channel, 1 = playing
    std::uint8_t pcm_selected_ = 0;
    bool pcm_enabled_ = false;
    std::uint64_t pcm_clock_ = 0;      // sub cycle the chip has been run up to
    // Chip-rate output waiting to be resampled to kAudioRate.
    std::vector<std::int32_t> pcm_native_;
    double pcm_resample_phase_ = 0.0;
    std::array<std::int32_t, 2> pcm_last_{};

    // Mixed output for the host.
    std::vector<std::int16_t> audio_;
    // CD audio blocks at 44.1kHz waiting to be mixed.
    std::vector<std::int16_t> cdda_;
    // How many times the CD audio queue was trimmed for running long. A
    // steadily rising count means the drive and the video frame disagree
    // about how fast a second is; zero means the queue is only lumpy, which
    // is normal. See end_frame.
    std::uint64_t cdda_overruns_ = 0;

    // Diagnostics.
    std::uint64_t sub_instructions_ = 0;
    std::unordered_map<std::uint32_t, std::uint64_t> sub_pc_counts_;
    std::unordered_set<std::uint32_t> sub_first_seen_;  // MERCURY_SCD_FIRST_TRACE
    std::uint64_t sub_unimplemented_ = 0;
    std::array<std::uint64_t, 7> sub_interrupts_{};
    std::uint64_t cdd_commands_ = 0;
    std::uint64_t sectors_decoded_ = 0;
};

}  // namespace mercury::segacd
