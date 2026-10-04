#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mercury::gamegear {

class Vdp {
public:
    static constexpr std::size_t kVramSize = 16 * 1024;
    static constexpr std::size_t kColorRamWordCount = 32;
    // Game Gear LCD window. The VDP always generates the full 256x192 scene;
    // the handheld simply shows the middle of it.
    static constexpr std::size_t kVisibleWidth = 160;
    static constexpr std::size_t kVisibleHeight = 144;
    // Master System mode shows the whole active area instead.
    static constexpr std::size_t kMasterSystemWidth = 256;
    static constexpr std::size_t kMasterSystemHeight = 192;
    // Mode 4 also has 224- and 240-line variants on the SMS2/GG VDP, selected
    // by M1-M4. Buffers are sized for the tallest so the mode can change at
    // runtime - Fantastic Dizzy and Cosmic Spacehead switch to 224 lines after
    // their logo screens.
    static constexpr std::size_t kMasterSystemHeightExtended = 224;
    static constexpr std::size_t kMaxVisibleWidth = kMasterSystemWidth;
    static constexpr std::size_t kMaxVisibleHeight = kMasterSystemHeightExtended;

    enum class DisplayMode {
        game_gear,       // 160x144 crop, 12-bit CRAM written as byte pairs
        master_system,   // full 256x192, 6-bit CRAM written a byte at a time
    };
    static constexpr std::size_t kVirtualTileWidth = 32;
    static constexpr std::size_t kVirtualTileHeight = 28;
    static constexpr std::uint32_t kCpuCyclesPerScanline = 228;
    static constexpr std::uint32_t kScanlinesPerFrame = 262;
    static constexpr std::uint32_t kVisibleScanlines = 192;

    void reset() noexcept;
    // Must be set before reset() so the frame buffer is sized correctly.
    void set_display_mode(DisplayMode mode) noexcept;
    [[nodiscard]] DisplayMode display_mode() const noexcept { return mode_; }
    [[nodiscard]] std::size_t visible_width() const noexcept {
        return mode_ == DisplayMode::master_system ? kMasterSystemWidth : kVisibleWidth;
    }
    // Height depends on the live mode bits, so it can change while a game runs.
    [[nodiscard]] std::size_t visible_height() const noexcept;
    // True when the mode bits select the 224- or 240-line display.
    [[nodiscard]] bool extended_display() const noexcept;
    void advance_cycles(std::uint32_t cpu_cycles) noexcept;
    [[nodiscard]] std::uint8_t read_v_counter() const noexcept;
    [[nodiscard]] std::uint8_t read_h_counter() const noexcept;
    [[nodiscard]] std::uint8_t read_data() noexcept;
    [[nodiscard]] std::uint8_t read_status() noexcept;
    void write_data(std::uint8_t value) noexcept;
    void write_control(std::uint8_t value) noexcept;
    [[nodiscard]] bool display_enabled() const noexcept;
    [[nodiscard]] bool access_requires_wait() const noexcept;
    [[nodiscard]] std::uint32_t frames_elapsed() const noexcept;
    [[nodiscard]] bool frame_interrupt_enabled() const noexcept;
    [[nodiscard]] bool line_interrupt_enabled() const noexcept;
    [[nodiscard]] bool interrupt_requested() const noexcept;
    [[nodiscard]] std::uint32_t data_write_count() const noexcept { return data_write_count_; }
    [[nodiscard]] std::uint32_t control_write_count() const noexcept { return control_write_count_; }
    [[nodiscard]] std::uint32_t register_write_count() const noexcept { return register_write_count_; }
    [[nodiscard]] std::uint8_t last_register_index() const noexcept { return last_register_index_; }
    [[nodiscard]] std::uint8_t last_register_value() const noexcept { return last_register_value_; }
    [[nodiscard]] std::uint8_t register_value(std::size_t index) const noexcept { return registers_[index & 0x0F]; }
    [[nodiscard]] std::uint16_t debug_current_address() const noexcept;
    [[nodiscard]] std::uint8_t debug_code() const noexcept { return code_; }
    [[nodiscard]] std::uint32_t debug_scanline() const noexcept { return scanline_; }
    // Side-effect-free view of the status byte; read_status() clears flags.
    [[nodiscard]] std::uint8_t debug_status() const noexcept { return status_; }
    [[nodiscard]] bool debug_line_irq_pending() const noexcept { return line_interrupt_pending_; }
    // Which kind of interrupt the VDP actually raised. "The game takes 59
    // interrupts a frame" is ambiguous until you can split it by source.
    [[nodiscard]] std::uint32_t debug_line_interrupt_count() const noexcept { return line_interrupt_count_; }
    [[nodiscard]] std::uint32_t debug_frame_interrupt_count() const noexcept { return frame_interrupt_count_; }
    [[nodiscard]] std::uint8_t debug_line_counter() const noexcept { return line_counter_; }

    // Structural render audit. Statistical measures (colour counts, animation
    // rate) cannot tell a wrong picture from a right one; these ask instead
    // whether the tables the VDP is being pointed at contain data the game
    // actually uploaded.
    struct GfxAudit {
        int visible_tiles = 0;          // background tiles inside the GG viewport
        int tiles_unwritten = 0;        // ...whose 32-byte pattern was never written
        int distinct_tile_indices = 0;
        bool name_table_written = false;
        bool sprite_table_written = false;
        bool sprite_patterns_written = false;
        int active_sprites = 0;
        int sprites_offscreen = 0;      // active sprites landing outside the viewport
    };
    [[nodiscard]] GfxAudit debug_gfx_audit() const noexcept;
    [[nodiscard]] std::uint32_t debug_cycles_into_scanline() const noexcept {
        return cycles_into_scanline_;
    }

    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string activity_summary() const;
    [[nodiscard]] std::string register_summary() const;
    [[nodiscard]] std::vector<std::uint8_t> visible_frame_pixels(bool ignore_blanking = false) const;
    [[nodiscard]] const std::vector<std::uint8_t>& presented_frame_pixels() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> visible_background_pixels(bool ignore_blanking = false) const;
    [[nodiscard]] std::vector<std::uint8_t> visible_sprite_pixels(bool ignore_blanking = false) const;
    [[nodiscard]] bool export_tilesheet_bmp(const std::filesystem::path& path) const;
    [[nodiscard]] bool export_background_map_bmp(const std::filesystem::path& path) const;
    [[nodiscard]] bool export_visible_frame_bmp(const std::filesystem::path& path) const;
    [[nodiscard]] bool export_visible_frame_debug_bmp(const std::filesystem::path& path) const;

private:
    [[nodiscard]] std::uint16_t current_address() const noexcept;
    void increment_address() noexcept;
    void set_v_counter_from_scanline() noexcept;

  public:
    // Save-state contents. The write-audit bitmap and the read/write counters
    // are diagnostics about the run, not machine state.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(vram_);
        ar(cram_);
        ar(mode_);
        ar(registers_);
        ar(v_counter_);
        ar(h_counter_);
        ar(status_);
        ar(data_buffer_);
        ar(control_first_byte_);
        ar(control_second_byte_);
        ar(awaiting_second_control_byte_);
        ar(address_);
        ar(code_);
        ar(scanline_);
        ar(cycles_into_scanline_);
        ar(frame_interrupt_pending_);
        ar(line_interrupt_pending_);
        ar(frame_interrupt_assert_delay_cycles_);
        ar(line_counter_);
        ar(frames_elapsed_);
        ar(horizontal_scroll_latch_);
        ar(vertical_scroll_latch_);
        // The picture itself, so a state loaded while paused shows the frame it
        // was taken on rather than whatever happened to be on screen.
        ar(presented_frame_pixels_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else (netplay, a shared file).
    // Values the hardware could never hold are brought back into range, so
    // none can become an index past a table or a loop that never ends.
    void sanitize_loaded() noexcept;

  private:
    std::array<std::uint8_t, kVramSize> vram_{};
    // One bit per VRAM byte, set when the game writes it. Lets the audit tell
    // "tile the game uploaded" from "tile that is just power-on zeroes".
    std::array<std::uint8_t, kVramSize / 8> vram_written_{};
    std::array<std::uint8_t, kColorRamWordCount * 2> cram_{};
    DisplayMode mode_ = DisplayMode::game_gear;
    std::array<std::uint8_t, 16> registers_{};
    std::uint8_t v_counter_ = 0xB0;
    std::uint8_t h_counter_ = 0x00;
    std::uint8_t status_ = 0x00;
    std::uint8_t data_buffer_ = 0xFF;
    std::uint8_t control_first_byte_ = 0x00;
    std::uint8_t control_second_byte_ = 0x00;
    bool awaiting_second_control_byte_ = false;
    std::uint16_t address_ = 0x0000;
    std::uint8_t code_ = 0x00;
    std::uint32_t scanline_ = 0;
    std::uint32_t cycles_into_scanline_ = 0;
    bool frame_interrupt_pending_ = false;
    bool line_interrupt_pending_ = false;
    std::uint32_t frame_interrupt_assert_delay_cycles_ = 0;
    std::uint8_t line_counter_ = 0x00;
    std::uint32_t frames_elapsed_ = 0;
    std::uint32_t frame_interrupt_count_ = 0;
    std::uint32_t line_interrupt_count_ = 0;
    // One entry per ACTIVE DISPLAY line, which is 224 in Mode 4's extended
    // variants - not kVisibleScanlines (192). Sizing this 192 meant the
    // 224-line path indexed 32 entries past the end, both writing (corrupting
    // whatever followed) and reading (garbage per-line scroll values, which
    // showed up as a corrupted band around row 195).
    std::array<std::uint8_t, kMaxVisibleHeight> horizontal_scroll_latch_{};
    std::uint8_t vertical_scroll_latch_ = 0x00;
    std::uint32_t status_read_count_ = 0;
    std::uint32_t data_read_count_ = 0;
    std::uint32_t control_write_count_ = 0;
    std::uint32_t data_write_count_ = 0;
    std::uint32_t register_write_count_ = 0;
    std::uint32_t vram_write_count_ = 0;
    std::uint32_t cram_write_count_ = 0;
    std::uint8_t last_register_index_ = 0x00;
    std::uint8_t last_register_value_ = 0x00;
    std::uint16_t last_data_address_ = 0x0000;
    bool last_data_was_cram_ = false;
    std::uint8_t cram_latch_ = 0x00;
    std::vector<std::uint8_t> presented_frame_pixels_{};
};

}  // namespace mercury::gamegear
