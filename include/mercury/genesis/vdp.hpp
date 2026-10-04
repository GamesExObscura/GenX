#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mercury::genesis {

// Sega 315-5313 (Mega Drive VDP), Mode 5.
//
// Shares almost nothing with the Game Gear's 315-5124 beyond the SN76489 that
// sits next to it, so it lives in its own class rather than growing a mode flag
// onto the Mark III part. The differences that shape the design:
//
//   * 64KB VRAM, 64 CRAM entries of nine-bit colour, 40 VSRAM entries. CRAM and
//     VSRAM are separate address spaces reached through the same data port, so
//     the port has to remember which one the pending command selected.
//   * The control port takes a 32-bit command as TWO 16-bit writes. A register
//     write is a single word instead, and the part decides which it is from the
//     top two bits of the first word. Getting the "is a command pending" state
//     wrong is the classic way to lose every other write.
//   * DMA in three flavours - 68000 memory to VDP, VRAM fill, VRAM copy - all
//     triggered by CD5 in the pending command.
//   * Two scrolling planes plus a window, and sprites that interleave with them
//     by priority rather than sitting strictly on top.
class Vdp {
public:
    static constexpr std::size_t kVramBytes = 0x10000;
    static constexpr std::size_t kCramEntries = 64;
    static constexpr std::size_t kVsramEntries = 40;
    static constexpr std::size_t kRegisterCount = 24;

    // Master clock 53.693175 MHz NTSC, 68000 at /7. A line is 3420 master
    // clocks in both H32 and H40 - only the pixel clock differs - so the CPU
    // budget per line is the same either way.
    static constexpr std::uint32_t kM68kCyclesPerLine = 488;
    static constexpr std::uint32_t kLinesPerFrameNtsc = 262;
    static constexpr std::uint32_t kLinesPerFramePal = 313;

    static constexpr std::uint32_t kMaxWidth = 320;
    // Tall enough for interlace mode 2, which draws 448 lines (480 on PAL) as
    // two fields. Both fields are woven into this one buffer so the picture is
    // steady instead of flickering at 30Hz the way a CRT shows it.
    static constexpr std::uint32_t kMaxHeight = 480;
    // Scanlines of active display - the timing quantity, always 224 or 240. Not
    // the same as screen_height(), which is what the framebuffer holds and is
    // twice this in interlace mode 2.
    [[nodiscard]] std::uint32_t active_scanlines() const noexcept;
    // Register 12 bits 2-1 both set: interlace mode 2, double vertical
    // resolution. Mode 1 (bit 1 alone) interlaces the display but draws the same
    // content in both fields, so it needs nothing special here.
    [[nodiscard]] bool interlace_double() const noexcept;

    // Sprite limits are per line and differ by width mode.
    static constexpr std::uint32_t kMaxSpritesH40 = 80;
    static constexpr std::uint32_t kMaxSpritesH32 = 64;

    // The VDP reaches 68000 memory directly during a DMA transfer, so it needs
    // a read hook back into the bus. Set by the machine at construction.
    using MemoryReader = std::function<std::uint16_t(std::uint32_t)>;

    // See GameQuirks::hint_raise_on_reload. Default is the behaviour the whole
    // library was verified against.
    void set_hint_raise_on_reload(bool on) noexcept { hint_raise_on_reload_ = on; }
    // See GameQuirks::hint_reload_on_write and ::hint_run_in_vblank.
    void set_hint_reload_on_write(bool on) noexcept { hint_reload_on_write_ = on; }
    void set_hint_run_in_vblank(bool on) noexcept { hint_run_in_vblank_ = on; }
    // See GameQuirks::blank_display_edges.
    void set_blank_display_edges(bool on) noexcept { blank_display_edges_ = on; }
    // See GameQuirks::ignore_midscreen_blank.
    void set_ignore_midscreen_blank(bool on) noexcept { ignore_midscreen_blank_ = on; }
    // Draw each line when its horizontal blank BEGINS rather than when the
    // line's time runs out, and latch the full-screen vertical scroll for the
    // next line at that moment. Per game - Mortal Kombat CD only, set from the
    // disc in Genesis::reset.
    //
    // Mortal Kombat CD's video loads a 31-colour palette for every line, timed
    // off the H counter: display off, palette DMA, display on - then a
    // vertical-scroll write that doubles each picture row. Drawn at the end of
    // the line, every line got its neighbour's palette and the video looked
    // shredded into streaks. Genesis Plus GX lands those writes on the right
    // lines; this timing does the same.
    //
    // NOT library-wide: GenX's H counter and its H-interrupt disagree by about
    // 110 cycles (the hardware reads H=0xA5 in H40 at the H-interrupt; GenX
    // reads 0), so a game timed off the H counter wants this and a game timed
    // off the H-interrupt does not. Measured against Genesis Plus GX, the
    // racing games' roads (OutRun, Road Rash 1-3, Super Hang-On, Chase H.Q. II,
    // Super Monaco GP) all came out a line off under it.
    void set_render_at_hblank(bool on) noexcept { render_at_hblank_ = on; }
    // The switch above, after the MERCURY_RENDER_AT_HBLANK test override.
    [[nodiscard]] bool hblank_timing() const noexcept;

    void reset() noexcept;
    // As reset(), but the elapsed-frame count survives. That counter is a
    // diagnostic measuring a whole run, not console state, and zeroing it in the
    // middle of one makes every frame-based measurement wrong afterwards.
    void soft_reset() noexcept {
        const auto frames = frames_elapsed_;
        reset();
        frames_elapsed_ = frames;
    }
    // A memory range whose DMA reads lag by one word. Only the SVP's DRAM
    // behaves this way, so this stays empty for every other cartridge.
    [[nodiscard]] const std::array<std::pair<std::uint32_t, std::uint16_t>, 128>&
    d_port_log() const noexcept { return d_port_log_; }
    [[nodiscard]] std::uint64_t d_port_log_pos() const noexcept { return d_port_log_pos_; }
    [[nodiscard]] const std::array<std::pair<std::uint16_t, std::uint16_t>, 96>&
    ctrl_log() const noexcept { return ctrl_log_; }
    [[nodiscard]] std::uint64_t ctrl_log_pos() const noexcept { return ctrl_log_pos_; }
    [[nodiscard]] std::uint64_t dma_skipped_disabled() const noexcept { return dma_skipped_disabled_; }
    // One row per DMA landing in the sprite-tile region: where it went, where
    // it came from, how long. Comparing a sprite whose art is clean against one
    // whose art is scattered needs both their sources, not just the last.
    struct DRow { std::uint32_t dest; std::uint32_t src; std::uint32_t len; };
    [[nodiscard]] std::uint8_t last_d_inc() const noexcept { return last_d_inc_; }
    [[nodiscard]] const std::array<DRow, 40>& d_dma_log() const noexcept { return d_dma_log_; }
    [[nodiscard]] std::uint64_t d_dma_log_pos() const noexcept { return d_dma_log_pos_; }
    [[nodiscard]] std::uint64_t d_dma_count() const noexcept { return d_dma_count_; }
    [[nodiscard]] std::uint32_t last_d_source() const noexcept { return last_d_source_; }
    [[nodiscard]] std::uint32_t last_d_dest() const noexcept { return last_d_dest_; }
    [[nodiscard]] std::uint32_t last_d_length() const noexcept { return last_d_length_; }
    [[nodiscard]] const std::array<std::uint64_t, 256>& dma_d_sources() const noexcept {
        return dma_d_sources_;
    }
    [[nodiscard]] const std::array<std::uint64_t, 16>& dma_dest_buckets() const noexcept {
        return dma_dest_buckets_;
    }
    void set_dma_lag_range(std::uint32_t first, std::uint32_t last) noexcept {
        dma_lag_start_ = first;
        dma_lag_end_ = last;
    }

    void set_memory_reader(MemoryReader reader) { read_memory_ = std::move(reader); }
    void set_pal(bool pal) noexcept;
    [[nodiscard]] bool pal() const noexcept { return pal_; }

    // --- CPU-facing ports ---
    [[nodiscard]] std::uint16_t read_data() noexcept;
    void write_data(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t read_control() noexcept;   // status register
    void write_control(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t read_hv_counter() noexcept;

    // --- light gun ---------------------------------------------------------
    // A gun has seen the beam reach it on line y, at H counter value h. Freezes
    // the HV counter at that position and raises the external interrupt if the
    // game enabled it. Note the counter is frozen EITHER WAY - see the comment
    // on the definition; that is the whole reason a game's own crosshair can
    // track vertically and not horizontally.
    void latch_light_gun(std::uint32_t h, std::uint32_t y) noexcept;
    // Called on the lines the gun did not see. Releases a latch that the gun
    // itself took without the game asking, and leaves a game-requested one
    // alone.
    void release_light_gun_latch() noexcept;
    [[nodiscard]] bool hv_latch_enabled() const noexcept;
    [[nodiscard]] bool external_interrupt_enabled() const noexcept;
    [[nodiscard]] bool external_interrupt_pending() const noexcept {
        return external_interrupt_pending_;
    }
    // Diagnostics: how often the gun saw the beam, and how often the game had
    // actually asked to be told about it. A game that never enables either gate
    // is not listening, and no amount of gun emulation will reach it.
    [[nodiscard]] std::uint64_t light_gun_latches() const noexcept { return light_gun_latches_; }
    [[nodiscard]] std::uint64_t light_gun_hv_latches() const noexcept {
        return light_gun_hv_latches_;
    }
    [[nodiscard]] std::uint64_t light_gun_interrupts() const noexcept {
        return light_gun_interrupts_;
    }
    // What a game reading the HV counter would be handed right now. Peeking
    // rather than calling read_hv_counter, which has side effects.
    [[nodiscard]] std::uint16_t hv_latch_value() const noexcept { return hv_latch_; }
    [[nodiscard]] bool hv_latched() const noexcept { return hv_latched_; }
    [[nodiscard]] const std::array<std::uint64_t, 320>& blank_start_lines() const noexcept {
        return blank_start_lines_;
    }
    [[nodiscard]] const std::array<std::uint64_t, 256>& hv_v_histogram() const noexcept {
        return hv_v_histogram_;
    }

    // --- timing ---
    // Returns true when the frame completed during this call.
    bool advance_cycles(std::uint32_t m68k_cycles) noexcept;

    // Interrupt lines, highest pending level wins at the CPU.
    [[nodiscard]] bool vertical_interrupt_pending() const noexcept { return vertical_interrupt_pending_; }
    [[nodiscard]] bool horizontal_interrupt_pending() const noexcept { return horizontal_interrupt_pending_; }
    void acknowledge_vertical_interrupt() noexcept { vertical_interrupt_pending_ = false; }
    void acknowledge_horizontal_interrupt() noexcept { horizontal_interrupt_pending_ = false; }
    // What the hardware actually does on an interrupt acknowledge - retires the
    // highest-level request currently raised, whichever the CPU thinks it took.
    void acknowledge_interrupt() noexcept;
    // Clocked once per 68000 instruction; gives enable-bit changes their
    // one-instruction delay. See the definition.
    void commit_interrupt_enables() noexcept;
    [[nodiscard]] int interrupt_level() const noexcept;

    // --- geometry ---
    [[nodiscard]] bool wide_mode() const noexcept;      // H40 (320px) vs H32 (256px)
    [[nodiscard]] std::uint32_t screen_width() const noexcept;
    [[nodiscard]] std::uint32_t screen_height() const noexcept;
    [[nodiscard]] std::uint32_t lines_per_frame() const noexcept;
    [[nodiscard]] bool display_enabled() const noexcept;

    // --- output ---
    // Packed RGB24, screen_width() * screen_height() * 3, matching the Game
    // Gear side so the runtime and the probe tools stay uniform.
    [[nodiscard]] const std::vector<std::uint8_t>& frame_pixels() const noexcept { return frame_pixels_; }
    // One byte per pixel: 1 where nothing was drawn and the backdrop is
    // showing. Only the 32X needs this - its layer shows through the Mega
    // Drive picture exactly where the Mega Drive drew nothing, and
    // frame_pixels() has already resolved that away into RGB.
    //
    // Derived data, rebuilt every frame, and deliberately NOT in visit_state:
    // putting it in a save state would change the state format for a value
    // that the next rendered line regenerates anyway.
    [[nodiscard]] const std::vector<std::uint8_t>& frame_backdrop() const noexcept {
        return frame_backdrop_;
    }
    // Writable view of the finished picture, for the 32X to combine its own
    // layer into. Used at exactly one moment - after a frame completes and
    // before the next begins - so that every existing caller of
    // frame_pixels() gets the combined picture without knowing a 32X exists.
    [[nodiscard]] std::vector<std::uint8_t>& frame_pixels_for_overlay() noexcept {
        return frame_pixels_;
    }
    [[nodiscard]] std::uint32_t frames_elapsed() const noexcept { return frames_elapsed_; }
    // Is the beam in vertical blank right now? The 32X's own video layer
    // swaps its frame buffer banks on this edge, so it has to follow the real
    // one rather than a guess at where in the frame we are.
    [[nodiscard]] bool in_vblank() const noexcept { return vblank_flag_; }

    // --- introspection, for the probe tools ---
    [[nodiscard]] const std::array<std::uint8_t, kRegisterCount>& registers() const noexcept { return registers_; }
    [[nodiscard]] const std::vector<std::uint8_t>& vram() const noexcept { return vram_; }
    // Diagnostics: where plane A lives and how wide a row of it is, so a caller
    // can read the nametable without duplicating the register decode.
    [[nodiscard]] std::uint16_t plane_a_base_addr() const noexcept {
        return static_cast<std::uint16_t>((registers_[0x02] & 0x38) << 10);
    }
    [[nodiscard]] std::uint16_t plane_a_row_cells() const noexcept { return plane_width_cells(); }
    [[nodiscard]] std::uint16_t plane_b_base_addr() const noexcept {
        return static_cast<std::uint16_t>((registers_[0x04] & 0x07) << 13);
    }
    [[nodiscard]] std::uint16_t plane_h_px() const noexcept {
        return static_cast<std::uint16_t>(plane_height_cells() * 8);
    }
    [[nodiscard]] std::uint8_t reg(std::size_t i) const noexcept { return registers_[i]; }
    [[nodiscard]] const std::array<std::uint32_t, 7>& edge_debug() const noexcept { return edge_dbg_; }
    [[nodiscard]] const std::array<std::uint64_t, 256>& reg10_values() const noexcept {
        return reg10_values_;
    }
    [[nodiscard]] const std::array<std::uint64_t, 256>& reg18_values() const noexcept {
        return reg18_values_;
    }
    [[nodiscard]] const std::array<std::uint64_t, 320>& reg18_lines() const noexcept {
        return reg18_lines_;
    }
    [[nodiscard]] std::uint64_t vram_watch_count() const noexcept { return vram_watch_count_; }
    // MERCURY_RASTER_LOG=1: the two events that decide where a raster split
    // lands - register 10 being written, and the H-interrupt request being
    // raised - each with the frame and scanline. A split that comes out a
    // couple of lines off was either ASKED for late or SERVICED late, and
    // nothing but these two side by side tells those apart.
    // Per entry: frame, scanline, kind (0 reg10 write, 1 raised, 2 lost), value.
    [[nodiscard]] const std::array<std::array<std::uint32_t, 4>, 192>& raster_log() const noexcept {
        return raster_log_;
    }
    [[nodiscard]] std::uint64_t raster_log_pos() const noexcept { return raster_log_pos_; }
    [[nodiscard]] std::uint64_t raster_log_count() const noexcept { return raster_log_count_; }
    [[nodiscard]] const std::array<std::array<std::uint32_t, 5>, 40>& vsram_log() const noexcept {
        return vsram_log_;
    }
    [[nodiscard]] std::uint64_t vsram_log_pos() const noexcept { return vsram_log_pos_; }
    [[nodiscard]] std::uint64_t vsram_log_count() const noexcept { return vsram_log_count_; }
    // Per entry: frame, scanline, address, value, came-from-DMA.
    [[nodiscard]] const std::array<std::array<std::uint32_t, 5>, 128>& vram_watch_log() const noexcept {
        return vram_watch_log_;
    }
    [[nodiscard]] std::uint64_t vram_watch_pos() const noexcept { return vram_watch_pos_; }
    [[nodiscard]] std::uint16_t vsram_entry(std::size_t i) const noexcept {
        return static_cast<std::uint16_t>(vsram_[i % kVsramEntries] & 0x03FF);
    }
    [[nodiscard]] const std::array<std::uint16_t, kCramEntries>& cram() const noexcept { return cram_; }
    [[nodiscard]] std::uint32_t scanline() const noexcept { return scanline_; }
    [[nodiscard]] std::uint32_t cycles_into_line() const noexcept { return cycles_into_line_; }
    [[nodiscard]] std::uint64_t dma_count() const noexcept { return dma_count_; }
    // A DMA is not free: the VDP takes the bus and the 68000 stalls until the
    // transfer finishes. Games size their vblank work around that, so charging
    // nothing for it leaves them with far more CPU time than real hardware
    // gives. The machine collects this after every port write and folds it into
    // the step's cycle count.
    // Total stall time DMA has charged the 68000. A game whose timing is built
    // on H-interrupts is sensitive to this: every cycle the CPU is halted is a
    // cycle the raster keeps moving, and interrupts raised during the stall
    // collapse into one pending request.
    [[nodiscard]] std::uint64_t total_dma_cycles() const noexcept { return total_dma_cycles_; }
    [[nodiscard]] std::uint32_t take_pending_dma_cycles() noexcept {
        total_dma_cycles_ += pending_dma_cycles_;
        const auto cycles = pending_dma_cycles_;
        pending_dma_cycles_ = 0;
        return cycles;
    }
    // A log of what DMA actually did, for tracking down graphics that land in
    // the wrong place. Kept short - the first transfers are the ones that set
    // up a screen.
    struct DmaRecord {
        std::uint32_t destination;
        std::uint32_t source;
        std::uint32_t length;
        std::uint8_t code;
        char kind;  // 'm' memory->VDP, 'f' fill, 'c' copy
    };
    [[nodiscard]] const std::vector<DmaRecord>& dma_log() const noexcept { return dma_log_; }
    [[nodiscard]] std::uint64_t register_write_count() const noexcept { return register_write_count_; }
    [[nodiscard]] std::uint64_t data_write_count() const noexcept { return data_write_count_; }
    // Split by destination. "The palette is black" has two very different
    // causes - writes that never arrive, and writes that arrive carrying zero -
    // and only a per-target count tells them apart.
    [[nodiscard]] std::uint64_t hint_raise_count() const noexcept { return hint_raise_count_; }
    [[nodiscard]] std::uint64_t vram_write_count() const noexcept { return vram_write_count_; }
    [[nodiscard]] const std::array<std::uint64_t, 16>& vram_write_buckets() const noexcept {
        return vram_write_buckets_;
    }
    [[nodiscard]] std::uint64_t cram_write_count() const noexcept { return cram_write_count_; }
    [[nodiscard]] std::uint64_t vsram_write_count() const noexcept { return vsram_write_count_; }
    [[nodiscard]] std::uint64_t invalid_write_count() const noexcept { return invalid_write_count_; }
    // Per-line record of the state each line was drawn with. Raster effects are
    // invisible in a whole-frame view, so this is what makes a black bar or a
    // torn line explainable rather than guessable.
    struct LineRecord {
        bool display_on;
        std::uint16_t hscroll_a;
        std::uint16_t vscroll_a;
        std::uint8_t reg1;
    };
    [[nodiscard]] const std::array<LineRecord, kMaxHeight>& line_records() const noexcept {
        return line_records_;
    }
    [[nodiscard]] std::string describe() const;

private:
    enum class Target : std::uint8_t { vram, cram, vsram, invalid };

    [[nodiscard]] Target target_of(std::uint8_t code) const noexcept;
    void write_register(std::uint8_t index, std::uint8_t value) noexcept;
    void advance_address() noexcept;
    void run_dma() noexcept;
    void dma_from_memory() noexcept;
    void dma_fill(std::uint16_t value) noexcept;
    void dma_copy() noexcept;

    void render_line(std::uint32_t line) noexcept;
    void begin_frame() noexcept;

    // Plane fetch. Returns the 4-bit colour index plus the priority and palette
    // that came with the tile, because priority resolution needs all three.
    struct PlanePixel {
        std::uint8_t index = 0;      // 0-15, 0 means transparent
        std::uint8_t palette = 0;    // 0-3
        bool priority = false;
    };
    [[nodiscard]] PlanePixel fetch_plane_pixel(std::uint32_t name_base, std::uint32_t plane_x,
                                               std::uint32_t plane_y) const noexcept;
    [[nodiscard]] std::uint16_t plane_width_cells() const noexcept;
    [[nodiscard]] std::uint16_t plane_height_cells() const noexcept;
    [[nodiscard]] std::uint16_t horizontal_scroll(std::uint32_t line, bool plane_b) const noexcept;
    [[nodiscard]] std::uint16_t vertical_scroll(std::uint32_t column, bool plane_b) const noexcept;
    [[nodiscard]] bool window_covers(std::uint32_t cell_x, std::uint32_t line) const noexcept;

    [[nodiscard]] std::uint16_t read_vram16(std::uint32_t address) const noexcept;
    static void unpack_color(std::uint16_t entry, std::uint8_t* rgb) noexcept;

  public:
    // Save-state contents. Diagnostics - the write counters, the raster and DMA
    // logs, the histograms - are deliberately left out: they describe the run,
    // not the machine, and carrying them would bloat every state file for
    // nothing. So are the per-game quirk flags, which come from the quirk table
    // when the cartridge is loaded and must not be overwritten by a file.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(vram_);
        ar(cram_);
        ar(vsram_);
        ar(registers_);
        ar(first_word_written_);
        ar(pending_first_word_);
        ar(address_);
        ar(command_code_);
        ar(read_buffer_);
        ar(dma_fill_pending_);
        ar(dma_busy_);
        ar(pending_dma_cycles_);
        ar(pal_);
        ar(scanline_);
        ar(cycles_into_line_);
        ar(frames_elapsed_);
        ar(line_counter_);
        ar(odd_frame_);
        ar(vertical_interrupt_pending_);
        ar(horizontal_interrupt_pending_);
        ar(vint_flag_);
        ar(vint_assert_delay_);
        ar(vblank_flag_);
        ar(sprite_overflow_);
        ar(sprite_collision_);
        ar(hv_latch_);
        ar(hv_latched_);
        ar(hv_latch_soft_);
        ar(external_interrupt_pending_);
        ar(vint_enabled_effective_);
        ar(hint_enabled_effective_);
        ar(vint_enabled_pending_);
        ar(hint_enabled_pending_);
        // The picture itself, so a state that is loaded while paused shows the
        // frame it was saved on rather than whatever was on screen before.
        ar(frame_pixels_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else (netplay, a shared file).
    // Anything the hardware could never hold is brought back into range, so
    // it cannot become an index past a table or a loop that never ends.
    void sanitize_loaded() noexcept;

    // Save-state version 6: what the raster has already sampled of the line in
    // progress - whether it has been drawn yet, and the full-screen vertical
    // scroll latched for the next one. Kept apart from visit_state so a
    // version 4 or 5 file still reads field for field; for those,
    // derive_raster_samples rebuilds both from what the file does hold.
    template <typename Archive>
    void visit_raster_samples(Archive& ar) {
        ar(line_rendered_);
        ar(vscroll_latch_);
    }
    void derive_raster_samples() noexcept;

  private:
    std::vector<std::uint8_t> vram_ = std::vector<std::uint8_t>(kVramBytes, 0);
    std::array<std::uint16_t, kCramEntries> cram_{};
    std::array<std::uint16_t, kVsramEntries> vsram_{};
    std::array<std::uint8_t, kRegisterCount> registers_{};

    // Pending control-port command. first_word_written_ is the latch that makes
    // the second write mean "finish the command" rather than "start a new one".
    bool first_word_written_ = false;
    std::uint16_t pending_first_word_ = 0;
    std::uint32_t address_ = 0;
    std::uint8_t command_code_ = 0;

    // A read through the data port is prefetched: the value the CPU gets was
    // fetched when the command was set up, not when it read.
    std::uint16_t read_buffer_ = 0;
    std::array<std::uint64_t, 16> dma_dest_buckets_{};
    std::array<std::uint64_t, 256> dma_d_sources_{};
    std::array<std::pair<std::uint32_t, std::uint16_t>, 128> d_port_log_{};
    std::uint64_t d_port_log_pos_ = 0;
    std::array<std::pair<std::uint16_t, std::uint16_t>, 96> ctrl_log_{};
    std::uint64_t ctrl_log_pos_ = 0;
    std::array<std::uint64_t, 320> blank_start_lines_{};
    std::array<std::uint64_t, 256> hv_v_histogram_{};
    std::uint64_t dma_skipped_disabled_ = 0;
    std::uint64_t d_dma_count_ = 0;
    std::uint8_t last_d_inc_ = 0;
    std::array<DRow, 40> d_dma_log_{};
    std::uint64_t d_dma_log_pos_ = 0;
    std::uint32_t last_d_source_ = 0;
    std::uint32_t last_d_dest_ = 0;
    std::uint32_t last_d_length_ = 0;
    std::uint32_t dma_lag_start_ = 0;
    std::uint32_t dma_lag_end_ = 0;

    bool pal_ = false;
    std::uint32_t scanline_ = 0;
    std::uint32_t cycles_into_line_ = 0;
    // The current line has already been drawn - at the start of its blank.
    // See kHblankStartCycle in vdp.cpp.
    bool line_rendered_ = false;
    // Full-screen vertical scroll (planes A and B) as sampled in the blank
    // before the line being drawn. See advance_cycles.
    std::array<std::uint16_t, 2> vscroll_latch_{};
    std::uint32_t frames_elapsed_ = 0;
    // Counts down once per active line and fires the H interrupt at zero, then
    // reloads from register 10. Same reload-value rule as the Master System's
    // R10 - the register is not the live counter.
    std::uint32_t line_counter_ = 0;
    bool vertical_interrupt_pending_ = false;
    bool horizontal_interrupt_pending_ = false;
    // Status bit 7. Raster-derived and cleared by the read that reports it,
    // which makes it distinct from the interrupt request below.
    bool vint_flag_ = false;
    std::int32_t vint_assert_delay_ = 0;
    bool vblank_flag_ = false;
    bool sprite_overflow_ = false;
    bool sprite_collision_ = false;
    bool dma_busy_ = false;
    // A VRAM fill is armed by the control write and performed by the data write
    // that follows it, so the fill value has to come from the data port.
    bool dma_fill_pending_ = false;
    bool odd_frame_ = false;
    std::uint16_t hv_latch_ = 0;
    bool hv_latched_ = false;
    // The latch was taken by the light gun on a game that never set M3, so it
    // must be released again on the next line. A latch the game DID ask for
    // holds until the game clears M3 itself.
    bool hv_latch_soft_ = false;
    // Level 2, the external interrupt. Nothing but a light gun raises it.
    bool external_interrupt_pending_ = false;
    std::uint64_t light_gun_latches_ = 0;
    std::uint64_t light_gun_hv_latches_ = 0;
    std::uint64_t light_gun_interrupts_ = 0;

    MemoryReader read_memory_{};

    std::vector<std::uint8_t> frame_pixels_ =
        std::vector<std::uint8_t>(kMaxWidth * kMaxHeight * 3, 0);
    // Where the backdrop is showing, for the 32X to come through. See
    // frame_backdrop().
    std::vector<std::uint8_t> frame_backdrop_ =
        std::vector<std::uint8_t>(kMaxWidth * kMaxHeight, 1);
    // Per-line priority record, so sprites can be composited against whatever
    // the planes put down without re-fetching them.
    std::array<std::uint8_t, kMaxWidth> line_priority_{};
    std::array<LineRecord, kMaxHeight> line_records_{};

    // VRAM writes per 4KB region. "The game writes plenty but the screen is
    // empty" is answered by WHERE those writes land - tiles, name tables, or
    // somewhere nothing reads.
    std::array<std::uint64_t, 16> vram_write_buckets_{};
    std::uint64_t hint_raise_count_ = 0;
    std::uint64_t total_dma_cycles_ = 0;
    bool hint_raise_on_reload_ = false;
    bool hint_reload_on_write_ = false;
    bool hint_run_in_vblank_ = false;
    bool blank_display_edges_ = false;
    bool ignore_midscreen_blank_ = false;
    bool render_at_hblank_ = false;
    // plane_px, wrap_a, wrap_b, top_leftover, bottom_leftover, bottom_window, edges_on
    std::array<std::uint32_t, 7> edge_dbg_{};
    void note_vram_write(std::uint32_t address, std::uint16_t value, bool via_dma) noexcept;
    void note_vsram_write(std::uint32_t index, std::uint16_t value, bool via_dma) noexcept;
    std::array<std::array<std::uint32_t, 5>, 40> vsram_log_{};
    std::uint64_t vsram_log_pos_ = 0;
    std::uint64_t vsram_log_count_ = 0;
    std::array<std::uint64_t, 256> reg10_values_{};
    std::array<std::uint64_t, 256> reg18_values_{};
    std::array<std::uint64_t, 320> reg18_lines_{};
    std::array<std::array<std::uint32_t, 5>, 128> vram_watch_log_{};
    std::array<std::array<std::uint32_t, 4>, 192> raster_log_{};
    std::uint64_t raster_log_pos_ = 0;
    std::uint64_t raster_log_count_ = 0;
    void note_raster_event(std::uint32_t kind, std::uint32_t value) noexcept;
    std::uint64_t vram_watch_pos_ = 0;
    std::uint64_t vram_watch_count_ = 0;
    std::uint64_t hint_lost_count_ = 0;
    std::array<std::uint32_t, 256> hint_lost_lines_{};

public:
    [[nodiscard]] std::uint64_t hint_lost_count() const noexcept { return hint_lost_count_; }
    [[nodiscard]] const std::array<std::uint32_t, 256>& hint_lost_lines() const noexcept {
        return hint_lost_lines_;
    }

private:
    // Two-stage pipeline: a write lands in registers_, becomes "pending" at the
    // next instruction boundary and "effective" at the one after, which is what
    // interrupt_level() reads.
    bool vint_enabled_effective_ = false;
    bool hint_enabled_effective_ = false;
    bool vint_enabled_pending_ = false;
    bool hint_enabled_pending_ = false;

    // Compact event log for interrupt debugging: when requests are raised and
    // what each acknowledge actually retired. Inferring this from counts alone
    // does not work - the ordering is the whole question.
    struct IntEvent {
        std::uint32_t frame;
        std::uint32_t line;
        const char* what;
    };
    std::vector<IntEvent> int_events_{};
    void note_int_event(const char* what) noexcept {
        if (int_events_.size() < 40) {
            int_events_.push_back({frames_elapsed_, scanline_, what});
        }
    }

public:
    [[nodiscard]] const std::vector<IntEvent>& int_events() const noexcept { return int_events_; }

private:
    struct Reg1Write {
        std::uint32_t frame;
        std::uint32_t line;
        std::uint8_t value;
        bool pending_before;
    };
    std::vector<Reg1Write> reg1_log_{};

public:
    [[nodiscard]] const std::vector<Reg1Write>& reg1_log() const noexcept { return reg1_log_; }

private:
    std::array<std::uint32_t, 256> hint_raise_lines_{};
    std::uint64_t line_counter_ticks_ = 0;
    std::uint64_t line_counter_reload_sum_ = 0;

public:
    [[nodiscard]] const std::array<std::uint32_t, 256>& hint_raise_lines() const noexcept {
        return hint_raise_lines_;
    }
    [[nodiscard]] std::uint64_t line_counter_ticks() const noexcept { return line_counter_ticks_; }
    [[nodiscard]] std::uint64_t line_counter_reload_sum() const noexcept {
        return line_counter_reload_sum_;
    }

private:
    std::uint64_t dma_count_ = 0;
    std::vector<DmaRecord> dma_log_{};
    std::uint32_t pending_dma_cycles_ = 0;
    std::uint64_t register_write_count_ = 0;
    std::uint64_t data_write_count_ = 0;
    std::uint64_t vram_write_count_ = 0;
    std::uint64_t cram_write_count_ = 0;
    std::uint64_t vsram_write_count_ = 0;
    std::uint64_t invalid_write_count_ = 0;
};

}  // namespace mercury::genesis





























