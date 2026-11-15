// The Master System's port 1 devices (issue #5): the Light Phaser and the
// Paddle Control, as SystemControl answers a controller-port read.

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/sms_peripherals.hpp"
#include "mercury/gamegear/system_control.hpp"
#include "mercury/gamegear/vdp.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using mercury::gamegear::SmsPeripheral;
using mercury::gamegear::SystemControl;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main() {
    // --- CRC and the cartridge table ------------------------------------
    {
        const char* text = "123456789";
        check(mercury::gamegear::crc32(reinterpret_cast<const std::uint8_t*>(text), 9) == 0xCBF43926u,
              "CRC32 of \"123456789\" is the standard check value");
        std::vector<std::uint8_t> nothing(0x8000, 0x00);
        check(mercury::gamegear::sms_device_for_rom(nothing).peripheral == SmsPeripheral::control_pad,
              "an unknown cartridge gets the pad");
    }

    // --- Light Phaser ----------------------------------------------------
    {
        SystemControl control;
        control.reset();
        control.set_peripheral(SmsPeripheral::light_phaser);
        control.set_phaser_x_offset(20);
        control.set_phaser(100, 80, true, false);
        int latch = -1;

        // Beam far away (line 10): TH high, nothing latched.
        auto dd = control.read_controller(true, 0xFF, 10, 0x20, latch);
        check((dd & 0x40) != 0 && latch < 0, "beam far from the sight: TH high, no latch");

        // Beam on the sight's line, 20 pixels before it (H counter 40 = pixel 80).
        dd = control.read_controller(true, 0xFF, 80, 40, latch);
        check((dd & 0x40) == 0, "beam passing the sight pulls TH low");
        check(latch == 20 + 100 / 2, "...and latches H = offset + x/2");

        // Within five lines either way, not six.
        latch = -1;
        dd = control.read_controller(true, 0xFF, 85, 50, latch);
        check((dd & 0x40) == 0, "five lines below still sees it");
        dd = control.read_controller(true, 0xFF, 86, 50, latch);
        check((dd & 0x40) != 0, "six lines below does not");

        // TH set as an output on port $3F: the sensor cannot pull it.
        control.write_io_control(0xFD);  // port A TH direction = output
        latch = -1;
        dd = control.read_controller(true, 0xFF, 80, 50, latch);
        check((dd & 0x40) != 0 && latch < 0, "TH as an output ignores the sensor");
        control.write_io_control(0xFF);

        // Off screen: nothing.
        control.set_phaser(100, 80, false, false);
        latch = -1;
        dd = control.read_controller(true, 0xFF, 80, 50, latch);
        check((dd & 0x40) != 0 && latch < 0, "aimed off the screen: no light");

        // $DC: directions and TR read high, the trigger is TL (bit 4); port 2's
        // up and down (bits 6-7) are left as the pad read them.
        control.set_phaser(100, 80, true, false);
        auto dc = control.read_controller(false, 0x80, 0, 0, latch);
        check(dc == 0xBF, "trigger up: $DC reads all of port 1 high");
        control.set_phaser(100, 80, true, true);
        dc = control.read_controller(false, 0xFF, 0, 0, latch);
        check(dc == 0xEF, "trigger pulled: TL low");
    }

    // --- The VDP's H counter latch ---------------------------------------
    // Master System mode: $7F only ever returns the latched value (FluBBa's
    // VDP Test, issue #3: "HCounter keeps value").
    {
        mercury::gamegear::Vdp vdp;
        vdp.set_display_mode(mercury::gamegear::Vdp::DisplayMode::master_system);
        vdp.reset();
        const auto first = vdp.read_h_counter();
        vdp.advance_cycles(100);
        check(vdp.read_h_counter() == first, "with nothing latched, $7F keeps its value");
        vdp.latch_h_counter(0x44);
        vdp.advance_cycles(57);
        check(vdp.read_h_counter() == 0x44, "after a Phaser latch, $7F holds it");
    }
    // Game Gear mode is as it was: the live counter until something latches.
    {
        mercury::gamegear::Vdp vdp;
        vdp.reset();
        vdp.advance_cycles(100);
        check(vdp.read_h_counter() == vdp.live_h_counter(), "Game Gear mode: $7F reads the live counter");
    }

    // --- The counter, cycle by cycle -------------------------------------
    // 171 counts in 228 cycles, $00-$93 then $E9-$FF; the counts that last two
    // cycles are the ones VDP Test's table doubles: $02, $05, ... $FF. The
    // line begins at $F4.
    {
        using mercury::gamegear::Vdp;
        Vdp vdp;
        vdp.set_display_mode(Vdp::DisplayMode::master_system);
        vdp.reset();
        check(vdp.sms_h_counter_in(0) == 0xF4, "the line begins at H count $F4");
        int counts[256] = {};
        for (std::uint32_t c = 0; c < Vdp::kCpuCyclesPerScanline; ++c) {
            ++counts[vdp.sms_h_counter_in(c)];
        }
        bool shape = true;
        int distinct = 0;
        for (int v = 0; v < 256; ++v) {
            const bool valid = v <= 0x93 || v >= 0xE9;
            const int index = v <= 0x93 ? v : 0x94 + (v - 0xE9);
            const int want = !valid ? 0 : (index % 3 == 2 ? 2 : 1);
            shape = shape && counts[v] == want;
            distinct += counts[v] != 0 ? 1 : 0;
        }
        check(distinct == 171 && shape, "every count appears; every third one lasts two cycles");
    }

    // --- $3F latches the counter (through the bus) -----------------------
    // A real program: TH driven low, then let go (it floats high) - the edge
    // latches. Two latches 64 cycles apart are exactly 48 counts apart (three
    // counts every four cycles), and reading $7F again changes nothing.
    {
        const auto rom_path = std::filesystem::temp_directory_path() / "genx_sms_hcounter_test.sms";
        {
            std::vector<unsigned char> rom(32 * 1024, 0x00);
            const unsigned char program[] = {
                0xF3,              // di
                0x3E, 0x55,        // ld a,$55
                0xD3, 0x3F,        // out ($3F),a     TH low
                0x3E, 0xFF,        // ld a,$FF
                0xD3, 0x3F,        // out ($3F),a     TH released: latch 1
                0xDB, 0x7F,        // in a,($7F)
                0x32, 0x00, 0xC0,  // ld ($C000),a
                0x3E, 0x55,        // ld a,$55
                0xD3, 0x3F,        // out ($3F),a
                0x00,              // nop
                0x3E, 0xFF,        // ld a,$FF
                0xD3, 0x3F,        // out ($3F),a     latch 2, 64 cycles after latch 1
                0xDB, 0x7F,        // in a,($7F)
                0x32, 0x01, 0xC0,  // ld ($C001),a
                0xDB, 0x7F,        // in a,($7F)      no edge: the same value
                0x32, 0x02, 0xC0,  // ld ($C002),a
                0x76,              // halt
            };
            std::memcpy(rom.data(), program, sizeof(program));
            std::ofstream out(rom_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        }
        mercury::gamegear::GameGear machine;
        const bool loaded = machine.load_rom(rom_path).ok;
        check(loaded, "test cartridge (.sms) loads");
        if (loaded) {
            machine.reset();
            for (int i = 0; i < 40; ++i) {
                (void)machine.step();
            }
            const auto first = machine.bus().read8(0xC000);
            const auto second = machine.bus().read8(0xC001);
            const auto third = machine.bus().read8(0xC002);
            const auto index = [](std::uint8_t v) { return v <= 0x93 ? v : 0x94 + (v - 0xE9); };
            const int apart = (index(second) - index(first) + 171) % 171;
            std::printf("     latches $%02X and $%02X\n", first, second);
            check(apart == 48, "two $3F latches 64 cycles apart are 48 counts apart");
            check(third == second, "reading $7F again, with no edge, gives the same value");
        }
        std::error_code ec;
        std::filesystem::remove(rom_path, ec);
    }

    // --- The line interrupt, a cycle into the line ------------------------
    {
        using mercury::gamegear::Vdp;
        for (const auto mode : {Vdp::DisplayMode::master_system, Vdp::DisplayMode::game_gear}) {
            Vdp vdp;
            vdp.set_display_mode(mode);
            vdp.reset();
            vdp.write_control(0x14);  // R0: mode 4, line interrupts on
            vdp.write_control(0x80);
            vdp.write_control(0x00);  // R10 = 0: every line
            vdp.write_control(0x8A);
            vdp.write_control(0x40);  // R1: display on
            vdp.write_control(0x81);
            // Through a vertical blank, where the line counter reloads from R10
            // (it starts at $FF), to the middle of line $40.
            vdp.advance_cycles(Vdp::kScanlinesPerFrame * Vdp::kCpuCyclesPerScanline);
            while (vdp.read_v_counter() != 0x40) {
                vdp.advance_cycles(1);
            }
            vdp.advance_cycles(100);  // mid-line
            (void)vdp.read_status();  // acknowledge what is pending
            // Walk to the exact start of the next line.
            const auto line = vdp.read_v_counter();
            while (vdp.read_v_counter() == line) {
                vdp.advance_cycles(1);
            }
            const bool at_start = vdp.interrupt_requested();
            vdp.advance_cycles(1);
            const bool a_cycle_in = vdp.interrupt_requested();
            std::printf("     line $%02X: at start %d, a cycle in %d\n", vdp.read_v_counter(), at_start ? 1 : 0,
                        a_cycle_in ? 1 : 0);
            if (mode == Vdp::DisplayMode::master_system) {
                check(!at_start && a_cycle_in, "Master System: the line interrupt is seen a cycle into the line");
            } else {
                check(at_start && a_cycle_in, "Game Gear mode: the line interrupt is seen as the line begins");
            }
        }
    }

    // --- The collision flag ----------------------------------------------
    // Two sprites on top of each other at column $83, rows $61-$68. By
    // default the flag is up as line $61 begins, as this core draws the line
    // then (Fantastic Dizzy's status bar split needs the two to agree - see
    // Vdp::advance_cycles). With MERCURY_SMS_COL_AT_PIXEL=1 it goes up at
    // cycle 111, when the beam reaches column $83, as VDP Test measures.
    {
        const bool at_pixel = mercury::gamegear::Vdp::sms_collision_at_pixel();
        using mercury::gamegear::Vdp;
        Vdp vdp;
        vdp.set_display_mode(Vdp::DisplayMode::master_system);
        vdp.reset();
        const auto reg = [&vdp](int index, std::uint8_t value) {
            vdp.write_control(value);
            vdp.write_control(static_cast<std::uint8_t>(0x80 | index));
        };
        const auto vram_at = [&vdp](std::uint16_t address) {
            vdp.write_control(static_cast<std::uint8_t>(address & 0xFF));
            vdp.write_control(static_cast<std::uint8_t>(0x40 | ((address >> 8) & 0x3F)));
        };
        reg(0, 0x04);  // mode 4
        reg(5, 0x7F);  // sprite table at $3F00
        reg(6, 0x00);  // sprite patterns at $0000
        vram_at(0x0020);  // tile 1: every pixel colour 1
        for (int row = 0; row < 8; ++row) {
            vdp.write_data(0xFF);
            vdp.write_data(0x00);
            vdp.write_data(0x00);
            vdp.write_data(0x00);
        }
        vram_at(0x3F00);
        vdp.write_data(0x60);
        vdp.write_data(0x60);
        vdp.write_data(0xD0);
        vram_at(0x3F80);
        vdp.write_data(0x83);
        vdp.write_data(0x01);
        vdp.write_data(0x83);
        vdp.write_data(0x01);
        reg(1, 0x40);  // display on
        // To the start of line $61.
        while (vdp.read_v_counter() != 0x60) {
            vdp.advance_cycles(1);
        }
        (void)vdp.read_status();
        while (vdp.read_v_counter() == 0x60) {
            vdp.advance_cycles(1);
        }
        if (at_pixel) {
            (void)vdp.read_status();
            vdp.advance_cycles(110);
            const bool at_110 = (vdp.read_status() & 0x20) != 0;
            vdp.advance_cycles(1);
            const bool at_111 = (vdp.read_status() & 0x20) != 0;
            check(!at_110 && at_111, "collision at column $83: flag clear at cycle 110, set at 111");
        } else {
            check((vdp.read_status() & 0x20) != 0, "collision on line $61: flag up as the line begins");
        }
    }

    // --- Paddle ----------------------------------------------------------
    {
        SystemControl control;
        control.reset();
        control.set_peripheral(SmsPeripheral::paddle);
        control.set_paddle(0xA7, false);
        int latch = -1;
        int high = 0;
        int low = 0;
        bool saw_high_value = false;
        bool saw_low_value = false;
        for (int i = 0; i < 12; ++i) {
            const auto dc = control.read_controller(false, 0xC0, 0, 0, latch);
            if ((dc & 0x20) != 0) {
                ++high;
                saw_high_value = saw_high_value || (dc & 0x0F) == 0x0A;
            } else {
                ++low;
                saw_low_value = saw_low_value || (dc & 0x0F) == 0x07;
            }
            if ((dc & 0x10) == 0) {
                saw_high_value = false;  // the button is up: TL must read high
            }
        }
        check(high == 6 && low == 6, "the halves alternate, three reads each");
        check(saw_high_value, "TR high carries the high nibble");
        check(saw_low_value, "TR low carries the low nibble");
        // A game leaving TH as an output (Galactic Protector's region check
        // does) does not stop the paddle running free.
        control.write_io_control(0x55);
        int changes = 0;
        bool previous = (control.read_controller(false, 0xC0, 0, 0, latch) & 0x20) != 0;
        for (int i = 0; i < 9; ++i) {
            const bool now = (control.read_controller(false, 0xC0, 0, 0, latch) & 0x20) != 0;
            changes += now != previous ? 1 : 0;
            previous = now;
        }
        check(changes >= 2, "...and keeps flipping with $3F = $55");
        control.set_paddle(0x00, true);
        const auto dc = control.read_controller(false, 0xC0, 0, 0, latch);
        check((dc & 0x10) == 0, "the paddle's button is TL");
        check((dc & 0xC0) == 0xC0, "port 2's lines on $DC are untouched");
    }

    // --- The pad is untouched --------------------------------------------
    {
        SystemControl control;
        control.reset();
        int latch = -1;
        check(control.read_controller(false, 0x5A, 0, 0, latch) == 0x5A && latch < 0,
              "with the pad in port 1 a read passes through");
    }

    std::printf("\n%s\n", failures == 0 ? "all port 1 device checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
