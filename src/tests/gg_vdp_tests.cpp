// The Game Gear / Master System VDP: when a line's sprites are settled.
//
// The VDP picks a line's sprites during the line above and fetches them at
// the end of it, so a write to the sprite settings (R1's size bits, R5, R6)
// late in a line belongs to the line after next, not the next one. Madou
// Monogatari I (issue #16) switches to its text box's sprite table and to
// 8x16 sprites from a line interrupt; its R1 write lands in the last few
// cycles of the line above the split, and taking it there drew the last line
// of Arle's picture with her own sprites at double height.

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/vdp.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mercury::gamegear::Vdp;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void set_register(Vdp& vdp, unsigned index, unsigned value) {
    vdp.write_control(static_cast<std::uint8_t>(value));
    vdp.write_control(static_cast<std::uint8_t>(0x80 | index));
}

void set_vram_address(Vdp& vdp, unsigned address) {
    vdp.write_control(static_cast<std::uint8_t>(address & 0xFF));
    vdp.write_control(static_cast<std::uint8_t>(0x40 | ((address >> 8) & 0x3F)));
}

// The visible Game Gear rows begin on this scanline (43 blank lines, then the
// 24 lines of the 192 the Game Gear does not show).
constexpr unsigned kFirstVisibleScanline = 43 + 24;

// A machine with one red 8x8 sprite covering visible rows 6-13 in the table at
// $3F00 (R5 = FF), and an empty table at $3700 (R5 = EF).
void build_scene(Vdp& vdp) {
    vdp.reset();
    set_register(vdp, 0, 0x04);  // mode 4
    set_register(vdp, 1, 0x40);  // display on, 8x8 sprites
    set_register(vdp, 2, 0xFF);  // name table $3800 (all tile 0, transparent black)
    set_register(vdp, 5, 0xFF);  // sprite table $3F00
    set_register(vdp, 6, 0xFB);  // sprite patterns at $0000
    set_register(vdp, 10, 0xFF);

    set_vram_address(vdp, 0x0020);  // tile 1: every pixel colour 1
    for (int row = 0; row < 8; ++row) {
        vdp.write_data(0xFF);
        vdp.write_data(0x00);
        vdp.write_data(0x00);
        vdp.write_data(0x00);
    }
    set_vram_address(vdp, 0x3F00);  // table A: sprite 0 at line 30 (visible row 6), then the end marker
    vdp.write_data(29);
    vdp.write_data(0xD0);
    set_vram_address(vdp, 0x3F80);  // sprite 0: x 64 (visible column 16), tile 1
    vdp.write_data(64);
    vdp.write_data(1);
    set_vram_address(vdp, 0x3700);  // table B: empty
    vdp.write_data(0xD0);

    vdp.write_control(0x22);  // CRAM entry 17 (sprite colour 1) = red
    vdp.write_control(0xC0);
    vdp.write_data(0x0F);
    vdp.write_data(0x00);
}

bool red_at(const Vdp& vdp, unsigned row, unsigned column) {
    const auto& rgb = vdp.presented_frame_pixels();
    const auto index = (row * Vdp::kVisibleWidth + column) * 3;
    return index + 2 < rgb.size() && rgb[index] == 0xFF && rgb[index + 1] == 0x00 && rgb[index + 2] == 0x00;
}

// Switch to the empty table at the given cycle of the scanline that draws
// visible row 9, then run on past row 12.
void switch_table_during_row_9(Vdp& vdp, unsigned cycle) {
    build_scene(vdp);
    vdp.advance_cycles((kFirstVisibleScanline + 9) * Vdp::kCpuCyclesPerScanline + cycle);
    set_register(vdp, 5, 0xEF);
    vdp.advance_cycles(4 * Vdp::kCpuCyclesPerScanline);
}

}  // namespace

int main() {
    Vdp vdp;

    build_scene(vdp);
    vdp.advance_cycles((kFirstVisibleScanline + 14) * Vdp::kCpuCyclesPerScanline);
    check(red_at(vdp, 9, 16) && red_at(vdp, 10, 16), "the scene draws its sprite on rows 6-13");

    // Early in the line, the write is in time for the next line.
    switch_table_during_row_9(vdp, 100);
    check(red_at(vdp, 9, 16), "row 9 drawn before the write keeps its sprite");
    check(!red_at(vdp, 10, 16), "a write at cycle 100 empties row 10");

    // After the latch point, the next line's sprites are already settled.
    switch_table_during_row_9(vdp, Vdp::kSpriteLatchCycle + 4);
    check(red_at(vdp, 10, 16), "a write after the latch point leaves row 10 as it was");
    check(!red_at(vdp, 11, 16), "...and takes effect on row 11");

    // The sprite SIZE bit follows the same rule: 8x16 taken late in row 13's
    // line must not stretch the sprite onto row 14.
    build_scene(vdp);
    vdp.advance_cycles((kFirstVisibleScanline + 13) * Vdp::kCpuCyclesPerScanline + Vdp::kSpriteLatchCycle + 4);
    set_register(vdp, 1, 0x42);
    vdp.advance_cycles(3 * Vdp::kCpuCyclesPerScanline);
    check(!red_at(vdp, 14, 16), "8x16 written after the latch point does not reach row 14");
    check(red_at(vdp, 15, 16), "...and does reach row 15 (the sprite's second tile row)");

    // A VERSION 4 SAVE STATE STILL LOADS. Version 5 appended the 3-byte sprite
    // latch; a file from before it is the same bytes without them, and the
    // live registers stand in for the latch.
    {
        const auto rom_path = std::filesystem::temp_directory_path() / "genx_gg_vdp_tests.gg";
        {
            std::ofstream rom(rom_path, std::ios::binary);
            const std::vector<char> nops(32 * 1024, 0);
            rom.write(nops.data(), static_cast<std::streamsize>(nops.size()));
        }
        mercury::gamegear::GameGear machine;
        const bool loaded = machine.load_rom(rom_path).ok;
        check(loaded, "test cartridge loads");
        if (loaded) {
            machine.reset();
            for (int i = 0; i < 20000; ++i) {
                (void)machine.step();
            }
            const auto current = machine.save_state_blob();
            auto version4 = current;
            version4.resize(version4.size() - 3);
            const std::uint32_t four = 4;
            std::memcpy(version4.data() + 4, &four, sizeof(four));

            mercury::gamegear::GameGear other;
            (void)other.load_rom(rom_path);
            other.reset();
            std::string error;
            check(other.load_state_blob(version4, error), "a version 4 state (no sprite latch) loads");
            check(other.save_state_blob() == current, "...and comes back as the same machine");
        }
        std::error_code ec;
        std::filesystem::remove(rom_path, ec);
    }

    std::printf("\n%s\n", failures == 0 ? "all Game Gear VDP checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
