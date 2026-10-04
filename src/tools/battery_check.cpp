// Checks cartridge battery saves end to end: does the emulator notice the cart
// has a battery, does a game actually write to it, and does a snapshot survive
// being written out and read back?
//
// "It compiled" is not evidence for save persistence - the failure mode is a
// file that is written but never read, or read into the wrong store, and that
// looks exactly like working right up until someone loses a save.
//
// Usage: battery_check <rom> [frames]

#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::uint64_t hash_of(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: battery_check <rom> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 1800;

    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }

    std::printf("battery backed : %s\n", machine.battery_backed() ? "yes" : "no");
    std::printf("kind           : %s\n", machine.battery_is_eeprom() ? "serial EEPROM" : "SRAM");

    // Tap start / A / C periodically, the same way the sweep does, so a game
    // that only touches its save area past the title screen gets there.
    for (int frame = 0; frame < frames; ++frame) {
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
        machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
        machine.run_frame();
    }

    std::printf("wrote to save  : %s (after %d frames)\n",
                machine.battery_dirty() ? "yes" : "no", frames);

    const auto snapshot = machine.battery_snapshot();
    std::printf("snapshot       : %zu bytes, hash %016llX\n", snapshot.size(),
                static_cast<unsigned long long>(hash_of(snapshot)));

    // The round trip: hand the bytes to a freshly booted machine and check they
    // come back byte for byte. This is the step that catches a restore writing
    // into the wrong store - SRAM bytes pushed into an EEPROM, say.
    mercury::genesis::Genesis fresh;
    if (!fresh.load_rom(argv[1]).loaded) {
        std::printf("FAIL: could not reload the ROM\n");
        return 1;
    }
    fresh.restore_battery(snapshot);
    const auto restored = fresh.battery_snapshot();
    if (restored != snapshot) {
        std::printf("FAIL: the restored save does not match what was written\n");
        return 1;
    }
    if (fresh.battery_dirty()) {
        std::printf("FAIL: restoring marked the save dirty, so it would be written straight back\n");
        return 1;
    }
    std::printf("PASS: %zu bytes survived save and restore intact\n", restored.size());
    return 0;
}
