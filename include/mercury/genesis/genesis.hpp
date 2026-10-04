#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mercury/core/m68k_cpu.hpp"
#include "mercury/core/module_status.hpp"
#include "mercury/core/z80_cpu.hpp"
#include "mercury/gamegear/psg.hpp"
#include "mercury/genesis/cartridge.hpp"
#include "mercury/genesis/eeprom.hpp"
#include "mercury/genesis/io.hpp"
#include "mercury/genesis/svp.hpp"
#include "mercury/genesis/vdp.hpp"
#include "mercury/genesis/ym2612.hpp"
#include "mercury/segacd/segacd.hpp"
#include "mercury/thirtytwox/thirtytwox.hpp"

namespace mercury::genesis {

// A Mega Drive. Two CPUs that share a bus, one video chip, two sound chips.
//
// The Z80 is a full second processor with its own address space, not a
// coprocessor: it has 8KB of its own RAM, a bank register that gives it a
// 32KB sliding window into 68000 space, and a bus-arbitration handshake the
// 68000 must complete before it can touch any of it. Modelling that handshake
// honestly is what makes sound drivers work.
class Genesis {
public:
    // NTSC master clock over seven. The whole machine's timing derives from it.
    static constexpr std::uint32_t kM68kClockHz = 7'670'453;
    static constexpr std::uint32_t kZ80ClockHz = 3'579'545;

    Genesis();

    struct LoadResult {
        bool loaded = false;
        std::string message;
    };

    [[nodiscard]] LoadResult load_rom(const std::filesystem::path& path);
    // Boot the Sega CD instead of a cartridge: its BIOS at the bottom of the
    // map, and a disc in the drive - or no disc, which boots the BIOS's own
    // CD player and memory manager. See segacd::SegaCd.
    [[nodiscard]] LoadResult load_cd(const std::filesystem::path& disc,
                                     const std::vector<std::uint8_t>& bios);
    [[nodiscard]] bool cd_mode() const noexcept { return cd_mode_; }
    [[nodiscard]] mercury::segacd::SegaCd& segacd() noexcept { return *segacd_; }
    [[nodiscard]] const mercury::segacd::SegaCd& segacd() const noexcept { return *segacd_; }
    void reset() noexcept;

    // Runs until the VDP finishes a frame. Returns the number of 68000 cycles
    // spent, which is roughly kM68kClockHz / 60.
    std::uint32_t run_frame() noexcept;
    [[nodiscard]] mercury::core::M68kStepResult step() noexcept;

    [[nodiscard]] Vdp& vdp() noexcept { return vdp_; }
    [[nodiscard]] const Vdp& vdp() const noexcept { return vdp_; }
    // The console's RESET button: processors and video chip restart, RAM and
    // battery saves survive. Distinct from reset(), which is a power cycle.
    void soft_reset() noexcept;

    [[nodiscard]] const Svp& svp() const noexcept { return svp_; }
    [[nodiscard]] mercury::thirtytwox::ThirtyTwoX& thirtytwox() noexcept { return *thirtytwox_; }
    [[nodiscard]] const mercury::thirtytwox::ThirtyTwoX& thirtytwox() const noexcept {
        return *thirtytwox_;
    }
    [[nodiscard]] std::uint64_t vdp_data_reads() const noexcept { return vdp_data_reads_; }
    [[nodiscard]] std::uint64_t vdp_status_reads() const noexcept { return vdp_status_reads_; }
    [[nodiscard]] std::uint64_t vdp_hv_reads() const noexcept { return vdp_hv_reads_; }
    [[nodiscard]] const std::array<std::pair<std::uint32_t, std::uint64_t>, 12>&
    hv_reader_pcs() const noexcept { return hv_reader_pcs_; }
    [[nodiscard]] std::uint32_t watch_write_pc() const noexcept { return watch_write_pc_; }
    [[nodiscard]] const std::vector<std::uint32_t>& watch_stack() const noexcept { return watch_stack_; }
    [[nodiscard]] std::uint32_t watch_stack_line() const noexcept { return watch_stack_line_; }
    [[nodiscard]] const std::array<std::uint64_t, 320>& watch_lines() const noexcept { return watch_lines_; }
    [[nodiscard]] const std::array<std::pair<std::uint32_t, std::uint64_t>, 12>&
    ctrl_writer_pcs() const noexcept { return ctrl_writer_pcs_; }
    [[nodiscard]] const std::array<std::pair<std::uint32_t, std::uint64_t>, 12>&
    watch_writer_pcs() const noexcept { return watch_writer_pcs_; }
    [[nodiscard]] std::uint64_t watch_write_count() const noexcept { return watch_write_count_; }
    [[nodiscard]] std::uint32_t needle_cmd_pc() const noexcept { return needle_cmd_pc_; }
    [[nodiscard]] std::uint64_t needle_cmd_count() const noexcept { return needle_cmd_count_; }
    // Games keep their handler tables and state flags in work RAM; when one
    // dispatches through a pointer that turns out to be zero, the question is
    // whether the whole region is uninitialised or just that one slot.
    [[nodiscard]] const std::vector<std::uint8_t>& work_ram() const noexcept { return work_ram_; }
    // A Z80 executing all-zero RAM looks healthy from the outside: 0x00 is a
    // valid NOP, so the instruction count climbs and nothing reports an error.
    // The only way to tell that apart from a running driver is to look.
    [[nodiscard]] const std::vector<std::uint8_t>& z80_ram() const noexcept { return z80_ram_; }
    [[nodiscard]] Ym2612& ym2612() noexcept { return ym2612_; }
    [[nodiscard]] mercury::gamegear::Psg& psg() noexcept { return psg_; }
    [[nodiscard]] Cartridge& cartridge() noexcept { return cartridge_; }
    [[nodiscard]] const Cartridge& cartridge() const noexcept { return cartridge_; }
    [[nodiscard]] mercury::core::M68kCpu& m68k() noexcept { return m68k_; }
    // WHAT IS THE 68000 DOING? The 32X side has had a per-instruction trace
    // sink since the SH-2 work; this is the same thing for the Mega Drive
    // processor, and it exists because two 32X cartridges came down to the
    // 68000 half of a handshake with no way to see it. Off unless a tool sets
    // it, and it costs one null check per instruction when it is.
    using M68kTraceSink =
        std::function<void(const mercury::core::M68kStepResult&,
                           const mercury::core::M68kRegisters&)>;
    void set_m68k_trace(M68kTraceSink sink) noexcept { m68k_trace_ = std::move(sink); }
    [[nodiscard]] mercury::core::Z80Cpu& z80() noexcept { return z80_; }
    [[nodiscard]] ControllerPort& port1() noexcept { return port1_; }
    [[nodiscard]] ControllerPort& port2() noexcept { return port2_; }
    [[nodiscard]] const ControllerPort& port1() const noexcept { return port1_; }
    [[nodiscard]] const ControllerPort& port2() const noexcept { return port2_; }

    void set_pal(bool pal) noexcept;
    [[nodiscard]] bool pal() const noexcept { return pal_; }

    [[nodiscard]] mercury::core::ModuleStatuses module_statuses() const;
    [[nodiscard]] std::string describe() const;

    // --- 68000 address space ---
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) noexcept;
    void write16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) noexcept;
    void write8(std::uint32_t address, std::uint8_t value) noexcept;
    // Cartridge offset for the 32X's $880000/$900000 windows, or -1 when the
    // address is not one of them - which is always, while the adapter is off.
    [[nodiscard]] std::int64_t thirtytwox_rom_offset(std::uint32_t address) const noexcept;
    // The per-disc behaviour switches, standing in for the cartridge quirk
    // table a disc has no header for. See the definitions.
    [[nodiscard]] bool disc_charges_ea_time() const;
    [[nodiscard]] bool disc_renders_at_hblank() const;
    // Combine the 32X's layer into the finished Mega Drive picture, so that
    // every caller of vdp().frame_pixels() sees one image. Returns at once
    // for a cartridge with no 32X, which is nearly all of them.
    void composite_thirtytwox() noexcept;
    // Let the SH-2s run out whatever 68000 time has passed since they last
    // did, and carry the vertical blank edge across to them.
    //
    // THE TWO SIDES HAVE TO INTERLEAVE. Running a whole 68000 frame and then
    // a whole 32X frame cannot work: a 32X game's startup is a conversation
    // through the communication ports, the 68000 writes a word and then spins
    // waiting for the answer, and at frame granularity the answer cannot
    // arrive until the 68000 has already spent the frame spinning. Measured
    // across the library - 26 of 30 cartridges sat with BOTH sides polling
    // communication port 0 and neither able to move.
    void thirtytwox_catch_up() noexcept;

private:
    void attach_buses() noexcept;
    // Fires a light gun when the raster reaches the line it is aimed at.
    void service_light_guns() noexcept;
    // Shared by reset() and soft_reset(); power_on additionally clears RAM and
    // the diagnostic counters.
    void reset_internal(bool power_on) noexcept;
    [[nodiscard]] std::uint8_t read_io(std::uint32_t address) noexcept;
    void write_io(std::uint32_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool in_sram(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint32_t sram_offset(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint32_t map_rom(std::uint32_t address) const noexcept;
    [[nodiscard]] bool serial_eeprom_cart() const noexcept;
    [[nodiscard]] bool in_hidden_sram(std::uint32_t address) const noexcept;
    void note_hidden_sram_write(std::uint32_t address) noexcept;

    // --- Z80 address space ---
    [[nodiscard]] std::uint8_t z80_read(std::uint16_t address) noexcept;
    void z80_write(std::uint16_t address, std::uint8_t value) noexcept;

  public:
    // --- controller-port register traffic ---------------------------------
    //
    // Which of the sixteen I/O registers does a game actually touch, and how
    // often? The data and control registers are the pads. The nine above them
    // are three UARTs, one per port, and GenX answers zero for all of them -
    // so a peripheral that talks serially, like the Miracle Piano, cannot
    // work and cannot be diagnosed either.
    //
    // Counted before any of that is built, for the same reason the Game Gear
    // link counters came first: the numbers say whether a game reaches for
    // the hardware at all, which settles what has to be emulated.
    struct IoActivity {
        // Indexed by (address & 0x1F) | 1, halved: register 0x01 is slot 0,
        // 0x03 is slot 1, up to 0x1F at slot 15.
        std::array<std::uint64_t, 16> reads{};
        std::array<std::uint64_t, 16> writes{};
        std::array<std::uint8_t, 16> last_write{};
    };
    [[nodiscard]] const IoActivity& io_activity() const noexcept { return io_activity_; }
    void clear_io_activity() noexcept { io_activity_ = IoActivity{}; }

    // A trace of the traffic itself, not just how much of it there was.
    //
    // Counting says a game drives a port. Only the sequence of values, with
    // the times between them, says what protocol it is speaking - which pin
    // is the clock, which carries data, how wide a pulse is, and what it
    // expects to read back. Off until switched on, because recording every
    // I/O access costs more than counting them.
    struct IoTraceEntry {
        std::uint64_t cycle = 0;
        std::uint8_t slot = 0;
        std::uint8_t value = 0;
        bool write = false;
    };
    void set_io_trace(bool enabled, std::size_t limit = 65536) {
        io_trace_enabled_ = enabled;
        io_trace_.clear();
        if (enabled) {
            io_trace_.reserve(limit);
        }
        io_trace_limit_ = limit;
    }
    [[nodiscard]] const std::vector<IoTraceEntry>& io_trace() const noexcept { return io_trace_; }
    // The register name for a slot, for anything that prints these.
    [[nodiscard]] static const char* io_register_name(std::size_t slot) noexcept;

  private:
    IoActivity io_activity_{};
    std::vector<IoTraceEntry> io_trace_{};
    std::size_t io_trace_limit_ = 0;
    bool io_trace_enabled_ = false;
    void note_io(std::uint32_t reg, std::uint8_t value, bool write) noexcept;

  public:
    // --- battery-backed saves ---------------------------------------------
    // A cartridge's SRAM or serial EEPROM survives a power cycle on real
    // hardware, so it has to reach a file or a game's progress dies with the
    // process. Kept separate from save states: a state is a snapshot of the
    // whole machine, this is the one thing the cartridge itself remembers.
    [[nodiscard]] bool battery_backed() const noexcept;
    // True when a serial EEPROM rather than parallel SRAM, which decides which
    // of the two blobs below is the meaningful one.
    [[nodiscard]] bool battery_is_eeprom() const noexcept;
    [[nodiscard]] bool battery_dirty() const noexcept {
        return cd_mode_ ? segacd_->backup_ram_dirty() : battery_dirty_;
    }
    void clear_battery_dirty() noexcept {
        battery_dirty_ = false;
        segacd_->clear_backup_ram_dirty();
    }
    // Raw bytes, no header - the same shape every other emulator's .srm has.
    [[nodiscard]] std::vector<std::uint8_t> battery_snapshot() const;
    void restore_battery(const std::vector<std::uint8_t>& bytes) noexcept;

    // Save states. The ROM itself is not carried - a state is only meaningful
    // against the cartridge it was taken from, and save_state_blob() records
    // enough to refuse a mismatch rather than loading garbage into a machine.
    // The per-game quirk flags and the forced-SRAM window are configuration
    // that comes from the quirk table at load time, so they are not in here
    // either.
    //
    // WHAT ELSE IS ATTACHED, from version 5. A state carries the SVP, the 32X
    // and the Sega CD when they are in use and says so in `media` (the
    // kMedia bits), which the header records so a load knows what to read.
    // Before version 5 none of the three were saved at all: a 32X or Sega CD
    // state restored the Mega Drive half and left the rest where it was, and
    // netplay refused those machines for exactly that reason.
    //
    // Version 6 appends the VDP's raster samples (Vdp::visit_raster_samples)
    // at the very end, so everything before them is laid out exactly as in
    // version 5 and the older files keep loading.
    static constexpr std::uint32_t kStateVersion = 6;
    static constexpr std::uint8_t kMediaCd = 0x01;
    static constexpr std::uint8_t kMedia32x = 0x02;
    static constexpr std::uint8_t kMediaSvp = 0x04;

    template <typename Archive>
    void visit_state(Archive& ar, std::uint32_t version, std::uint8_t media) {
        m68k_.visit_state(ar);
        z80_.visit_state(ar);
        vdp_.visit_state(ar);
        ym2612_.visit_state(ar);
        psg_.visit_state(ar);
        port1_.visit_state(ar);
        port2_.visit_state(ar);
        ext_.visit_state(ar);
        eeprom_.visit_state(ar);

        ar(work_ram_);
        ar(z80_ram_);
        ar(sram_);
        ar(sram_enabled_);
        ar(rom_banks_);
        ar(hidden_sram_active_);
        ar(z80_bus_requested_);
        ar(z80_reset_asserted_);
        ar(z80_bank_);
        ar(z80_bank_bits_);
        ar(pal_);
        ar(z80_cycle_debt_);
        ar(psg_cycle_debt_);
        ar(svp_cycle_debt_);
        ar(z80_in_vblank_);
        ar(z80_interrupt_cycles_);
        if (version >= 5) {
            visit_media_blocks(ar, media);
        }
        if (version >= 6) {
            vdp_.visit_raster_samples(ar);
        } else if constexpr (Archive::loading()) {
            vdp_.derive_raster_samples();
        }
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else - the netplay host sends the
    // joiner its whole machine, and state files are shared. Values the
    // hardware could never hold are brought back into range, so none of them
    // can become an address past the ROM or a loop that never ends.
    void sanitize_loaded() noexcept;

  private:
    template <typename Archive>
    void visit_media_blocks(Archive& ar, std::uint8_t media) {
        if ((media & kMediaSvp) != 0) {
            svp_.visit_state(ar);
        }
        if ((media & kMedia32x) != 0) {
            ar(thirtytwox_quantum_);
            ar(thirtytwox_owed_);
            ar(thirtytwox_vblank_);
            ar(thirtytwox_last_line_);
            thirtytwox_->visit_state(ar);
        } else if constexpr (Archive::loading()) {
            // Saved before the adapter was switched on - a Sega CD 32X disc
            // still in its Mega Drive startup. Put the adapter back to that.
            if (thirtytwox_->adapter().enabled()) {
                thirtytwox_->reset();
            }
            thirtytwox_quantum_ = 0;
            thirtytwox_owed_ = 0;
            thirtytwox_vblank_ = false;
            thirtytwox_last_line_ = 0;
        }
        if ((media & kMediaCd) != 0) {
            segacd_->visit_state(ar);
        }
    }

  public:
    // Which kMedia blocks a state taken now would carry.
    [[nodiscard]] std::uint8_t state_media() const noexcept;
    // Everything a save state or a netplay session has to agree on beyond
    // the cartridge bytes: which disc, which Sega CD BIOS, and which 32X
    // firmware when a 32X cartridge is in. One number, for the netplay
    // handshake; the state header keeps the parts separate so a refusal can
    // say which one differs.
    [[nodiscard]] std::uint64_t media_id() const noexcept;

    // Serialise into / restore from a byte blob, with a header that identifies
    // the cartridge. Returns false and leaves the machine untouched-looking on
    // a mismatch or a truncated file.
    [[nodiscard]] std::vector<std::uint8_t> save_state_blob() const;
    [[nodiscard]] bool load_state_blob(const std::vector<std::uint8_t>& blob, std::string& error);

  private:
    // The last scanline a light gun was considered on, so the check runs once
    // per line instead of once per instruction.
    std::uint32_t light_gun_line_ = 0xFFFFFFFF;
    Cartridge cartridge_;
    mercury::core::M68kCpu m68k_;
    mercury::core::Z80Cpu z80_;
    Vdp vdp_;
    Ym2612 ym2612_;
    mercury::gamegear::Psg psg_;
    ControllerPort port1_;
    ControllerPort port2_;
    ControllerPort ext_;

    std::vector<std::uint8_t> work_ram_ = std::vector<std::uint8_t>(0x10000, 0);
    std::vector<std::uint8_t> z80_ram_ = std::vector<std::uint8_t>(0x2000, 0);
    std::vector<std::uint8_t> sram_ = std::vector<std::uint8_t>(0x10000, 0xFF);
    bool sram_enabled_ = false;
    // Set by every write that reaches SRAM or the EEPROM, so the host knows
    // there is something worth flushing without diffing 64KB each frame.
    bool battery_dirty_ = false;
    // The Sega mapper fitted to cartridges larger than the 4MB the 68000 can
    // see. It splits that window into eight 512KB slots; the low slot is fixed
    // and the other seven can each be pointed at any 512KB page of the ROM.
    // Super Street Fighter II is 5MB and unreachable without it.
    std::array<std::uint32_t, 8> rom_banks_{};
    // Set the first time a game writes into the ROM-space battery window.
    bool hidden_sram_active_ = false;
    // Battery RAM supplied by the quirk table for a cartridge whose header does
    // not declare what it actually carries.
    bool forced_sram_ = false;
    std::uint32_t forced_sram_start_ = 0;
    std::uint32_t forced_sram_end_ = 0;
    SerialEeprom eeprom_;
    // The DSP on the Virtua Racing cartridge. Inert unless the loaded ROM is an
    // SVP cart, and its address windows sit above every normal cartridge, so it
    // cannot affect any other game.
    Svp svp_;
    // The 32X, ALWAYS ATTACHED. It starts inert - see thirtytwox::Adapter -
    // so a Mega Drive cartridge that never reads $A15100 cannot tell it is
    // here, and the only cost on the ordinary path is one range check per
    // bus access in a gap no Mega Drive hardware uses.
    //
    // ON THE HEAP, and that is not a style choice. The 32X carries 256KB of
    // SDRAM, two 128KB frame buffer banks and a boot ROM page - about 410KB.
    // Genesis is built as a plain local in the probe and in most of the test
    // tools, and a 1MB default stack cannot hold that: every one of them died
    // with STACK_OVERFLOW the moment the adapter became a member, which is how
    // a 693-ROM sweep came back PROBE_FAIL on every single row. A unique_ptr
    // keeps the object at a fixed address for the lifetime of the machine, so
    // the bus lambdas that capture it stay valid.
    std::unique_ptr<mercury::thirtytwox::ThirtyTwoX> thirtytwox_;
    // The Sega CD. On the heap for the same two reasons as the 32X - it is
    // about a megabyte, and its sub CPU's bus lambdas capture its address -
    // and inert unless cd_mode_ is set, which only load_cd does. Every bus
    // access pays one boolean test for it.
    std::unique_ptr<mercury::segacd::SegaCd> segacd_;
    bool cd_mode_ = false;
    // 68000 cycles run since the SH-2s last caught up, and the SH-2 cycles
    // still owed to them after the last slice overshot or fell short.
    std::uint32_t thirtytwox_quantum_ = 0;
    M68kTraceSink m68k_trace_{};
    std::int64_t thirtytwox_owed_ = 0;
    bool thirtytwox_vblank_ = false;
    std::uint32_t thirtytwox_last_line_ = 0;
    std::uint64_t vdp_data_reads_ = 0;
    std::uint64_t vdp_status_reads_ = 0;
    std::uint64_t vdp_hv_reads_ = 0;
    std::array<std::pair<std::uint32_t, std::uint64_t>, 12> hv_reader_pcs_{};
    std::uint32_t watch_write_pc_ = 0;
    std::array<std::pair<std::uint32_t, std::uint64_t>, 12> watch_writer_pcs_{};
    std::vector<std::uint32_t> watch_stack_{};
    std::uint32_t watch_stack_line_ = 0;
    std::array<std::uint64_t, 320> watch_lines_{};
    std::array<std::pair<std::uint32_t, std::uint64_t>, 12> ctrl_writer_pcs_{};
    std::uint64_t watch_write_count_ = 0;
    std::uint32_t needle_cmd_pc_ = 0;
    std::uint64_t needle_cmd_count_ = 0;
    // Carried remainder for the DSP's clock, which runs at 23MHz against the
    // 68000's 7.67MHz - very close to three to one.
    std::int64_t svp_cycle_debt_ = 0;

    // Bus arbitration. The 68000 asks for the Z80's bus and must see the
    // request granted before its accesses to A00000-A0FFFF mean anything.
    bool z80_bus_requested_ = false;
    bool z80_reset_asserted_ = true;
    std::uint32_t z80_bank_ = 0;
    // Bank register is loaded one bit at a time, LSB first, over nine writes.
    std::uint32_t z80_bank_bits_ = 0;

    bool pal_ = false;
    // The Z80 runs at 7/15 of the 68000's rate; the remainder is carried so the
    // ratio stays exact over a frame instead of drifting.
    std::int64_t z80_cycle_debt_ = 0;
    // Same carried remainder for the PSG's clock division.
    std::int64_t psg_cycle_debt_ = 0;
    // The VDP's vertical interrupt reaches the Z80 as well as the 68000, held
    // for about one scanline. Sound drivers tick their sequencer off it, so
    // without this the music never advances even though the driver is running.
    bool z80_in_vblank_ = false;
    std::int32_t z80_interrupt_cycles_ = 0;

public:
    // Diagnostics. A silent game is usually a stalled Z80, and these say so
    // directly instead of leaving it to be inferred.
    [[nodiscard]] std::uint64_t z80_unimplemented_count() const noexcept { return z80_unimplemented_count_; }
    [[nodiscard]] std::uint16_t z80_last_bad_pc() const noexcept { return z80_last_bad_pc_; }
    [[nodiscard]] std::uint64_t z80_instructions() const noexcept { return z80_instructions_; }
    [[nodiscard]] std::uint64_t z80_interrupts() const noexcept { return z80_interrupts_; }
    [[nodiscard]] std::uint64_t m68k_hint_count() const noexcept { return m68k_hint_count_; }
    [[nodiscard]] std::uint64_t m68k_vint_count() const noexcept { return m68k_vint_count_; }
    // Level 2, the light gun's external interrupt. Should sit at roughly one
    // per frame; far more than that means it is being raised and not retired.
    [[nodiscard]] std::uint64_t m68k_ext_count() const noexcept { return m68k_ext_count_; }
    [[nodiscard]] bool battery_window_written() const noexcept { return hidden_sram_active_; }
    // Writes into cartridge space that nothing claimed, so they went nowhere.
    // A game whose saved data does not survive a reset is usually writing to a
    // battery window this emulator has not mapped, and those writes are
    // invisible unless they are counted where they are dropped.
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& dropped_rom_writes() const noexcept {
        return dropped_rom_writes_;
    }
    [[nodiscard]] bool z80_running() const noexcept { return !z80_reset_asserted_ && !z80_bus_requested_; }
    // Unimplemented 68000 opcodes, with a tally per opcode. Across a whole
    // library this is the list of what still needs writing, ordered by how much
    // it matters - far more useful than a single count.
    [[nodiscard]] std::uint64_t m68k_unimplemented_count() const noexcept { return m68k_unimplemented_count_; }
    [[nodiscard]] const std::map<std::uint16_t, std::uint64_t>& m68k_unimplemented_opcodes() const noexcept {
        return m68k_unimplemented_opcodes_;
    }

    // Read tally. When a game wedges in a polling loop the question is always
    // "waiting for WHAT" - and the addresses it reads answer that directly,
    // where staring at the PC only says where it is stuck.
    void set_trace_reads(bool on) noexcept { trace_reads_ = on; }
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& read_tally() const noexcept {
        return read_tally_;
    }
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& pc_tally() const noexcept {
        return pc_tally_;
    }

private:
    bool trace_reads_ = false;
    std::map<std::uint32_t, std::uint64_t> read_tally_{};
    std::map<std::uint32_t, std::uint64_t> dropped_rom_writes_{};
    std::map<std::uint32_t, std::uint64_t> pc_tally_{};
    std::uint64_t m68k_unimplemented_count_ = 0;
    std::map<std::uint16_t, std::uint64_t> m68k_unimplemented_opcodes_{};
    std::uint64_t z80_unimplemented_count_ = 0;
    std::uint16_t z80_last_bad_pc_ = 0;
    std::uint64_t z80_instructions_ = 0;
    std::uint64_t z80_interrupts_ = 0;
    std::uint64_t m68k_hint_count_ = 0;
    std::uint64_t m68k_vint_count_ = 0;
    std::uint64_t m68k_ext_count_ = 0;
};

}  // namespace mercury::genesis







