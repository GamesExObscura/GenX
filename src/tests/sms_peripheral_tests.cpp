// The Master System's port 1 devices (issue #5): the Light Phaser and the
// Paddle Control, as SystemControl answers a controller-port read.

#include "mercury/gamegear/sms_peripherals.hpp"
#include "mercury/gamegear/system_control.hpp"
#include "mercury/gamegear/vdp.hpp"

#include <cstdio>
#include <cstring>
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
    {
        mercury::gamegear::Vdp vdp;
        vdp.set_display_mode(mercury::gamegear::Vdp::DisplayMode::master_system);
        vdp.reset();
        vdp.advance_cycles(100);
        const auto live = vdp.live_h_counter();
        check(vdp.read_h_counter() == live, "before any latch, $7F reads the live counter");
        vdp.latch_h_counter(0x44);
        vdp.advance_cycles(57);
        check(vdp.read_h_counter() == 0x44, "after a Phaser latch, $7F holds it");
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
