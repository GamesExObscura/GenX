// The Konami Justifier's port protocol, pinned.
//
// This device is ADDRESSED rather than simply read, and every one of the rules
// below is a way to get it silently wrong: buttons that are active low where a
// pad's are active high, a second gun that reads as permanently firing if its
// idle value is off by two bits, and a TH line whose meaning inverts depending
// on whether the game configured it as an output. None of that is visible from
// the outside - a wrong value just means the game ignores the gun - so it is
// worth a test rather than a play session.

#include "mercury/genesis/io.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void check_value(std::uint8_t got, std::uint8_t want, const char* what) {
    const bool ok = got == want;
    std::printf("%-4s %-52s got 0x%02X want 0x%02X\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) {
        ++failures;
    }
}

using mercury::genesis::Button;
using mercury::genesis::ControllerPort;

// Drive the port the way a game does: set the direction bits, then the levels.
void drive(ControllerPort& port, std::uint8_t control, std::uint8_t data) {
    port.write_control(control);
    port.write_data(data);
}

}  // namespace

int main() {
    // TH and TR as outputs, which is what a game must do to address the gun.
    //
    // A pin the game drives reads back what the game WROTE, not what the device
    // would put there - so with these two as outputs, bits 5 and 6 of every
    // read below are the game's own levels and only the low bits come from the
    // gun. That is why the expected values are not the raw device bytes; the
    // first version of this test asserted those and failed on the blend, which
    // was the test being wrong rather than the port.
    constexpr std::uint8_t kOutputs = 0x60;

    {
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        drive(port, kOutputs, 0x40);  // TH high
        // Device byte 0x30, with TH reading back the high level the game drove.
        // The part that matters is bits 0 and 1 CLEAR: that is "no gun here".
        check_value(port.read_data(), 0x50, "TH high reports no gun selected");
    }

    {
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        drive(port, kOutputs, 0x00);  // TH low, TR low: gun one
        // Nothing pressed. Active low, so both button bits are SET.
        check_value(port.read_data(), 0x13, "gun one idle");
        port.set_button(Button::a, true);
        check_value(port.read_data(), 0x12, "gun one trigger pulls bit 0 LOW");
        port.set_button(Button::a, false);
        port.set_button(Button::start, true);
        check_value(port.read_data(), 0x11, "gun one Start pulls bit 1 low");
    }

    {
        // THE REGRESSION. With one gun plugged in the port must not multiplex,
        // because the game drives TR constantly and the light pulse happens at
        // a single scanline - so letting TR gate the pulse threw the player's
        // position away on about half of all frames and the shots stopped
        // landing. Selection is only real when a second gun is actually there.
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        port.set_gun2_present(false);
        port.set_aim(40, 60, true);
        drive(port, kOutputs, 0x20);  // the game asks for gun two...
        check(port.selected_gun() == 0, "one gun: TR does not switch away from it");
        check(port.selected_aim_x() == 40 && port.selected_on_screen(),
              "one gun: its aim survives the game driving TR");
    }

    {
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        port.set_gun2_present(true);
        drive(port, kOutputs, 0x20);  // TH low, TR high: gun two
        // The regression this exists for: clearing these two bits would report
        // player two holding both buttons down for ever.
        check_value(port.read_data(), 0x33, "an untouched gun two is NOT firing");
        port.set_gun2_trigger(true);
        check_value(port.read_data(), 0x32, "gun two has its own trigger");
        // Gun one's trigger must not leak across.
        port.set_gun2_trigger(false);
        port.set_button(Button::a, true);
        check_value(port.read_data(), 0x33, "gun one's trigger does not reach gun two");
    }

    {
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        // TH left as an INPUT. Genesis Plus GX clears bits that are not
        // outputs before decoding, so this reads as "selected", not as the
        // floating-high a pad would see. Lethal Enforcers II depends on it.
        drive(port, 0x00, 0x40);
        check(port.read_data() != 0x30, "TH that is not an output does not read as unselected");
    }

    {
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        port.set_gun2_present(true);
        drive(port, kOutputs, 0x00);
        check(port.selected_gun() == 0, "TR low selects gun one");
        drive(port, kOutputs, 0x20);
        check(port.selected_gun() == 1, "TR high selects gun two");

        // Only the selected gun is visible to the machine, which is how one
        // port carries two positions.
        port.set_aim(40, 60, true);
        port.set_gun2_aim(200, 150, true);
        check(port.selected_aim_x() == 200 && port.selected_aim_y() == 150,
              "the selected gun's aim is the one reported");
        drive(port, kOutputs, 0x00);
        check(port.selected_aim_x() == 40 && port.selected_aim_y() == 60,
              "selecting the other gun reports the other aim");
    }

    {
        // A Menacer is a single gun and never multiplexes, so selection must
        // not follow TR for it.
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::menacer);
        drive(port, kOutputs, 0x20);
        check(port.selected_gun() == 0, "a Menacer is always gun one whatever TR does");
    }

    {
        // The H counter mapping, at the origin measured against T2's own
        // crosshair. Screen left is not counter zero.
        ControllerPort port;
        port.set_kind(ControllerPort::Kind::justifier);
        port.set_aim(0, 100, true);
        const auto left = port.light_gun_h(true);
        port.set_aim(318, 100, true);
        const auto right = port.light_gun_h(true);
        check(left == ControllerPort::kDefaultHOffsetWide,
              "screen left maps to the calibrated origin");

        // The whole chain: selecting a gun must change the number the VDP is
        // handed, or two players share one position.
        ControllerPort pair;
        pair.set_kind(ControllerPort::Kind::justifier);
        pair.set_gun2_present(true);
        pair.set_aim(40, 60, true);
        pair.set_gun2_aim(240, 60, true);
        drive(pair, kOutputs, 0x00);
        const auto one = pair.light_gun_h(true);
        drive(pair, kOutputs, 0x20);
        const auto two = pair.light_gun_h(true);
        check(one != two, "each gun feeds the counter its OWN position");
        std::printf("     gun one H=0x%02X  gun two H=0x%02X\n", one, two);
        // 318/2 = 159 steps on from the origin, wrapping round the 210-step
        // line and through the counter's blanking jump.
        check(right != left, "the counter moves across the screen");
        std::printf("     left=0x%02X right=0x%02X\n", left, right);
    }

    std::printf("\n%s\n", failures == 0 ? "all Justifier checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
