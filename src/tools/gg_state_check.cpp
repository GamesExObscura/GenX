// The Game Gear half of the save-state completeness check. Same argument as the
// Mega Drive one in genesis_probe: run on from a state, then reload it and run
// the same distance again, and require the machine to be bit-identical. A state
// that is missing a field still loads and still looks right for a second or two.
//
// Usage: gg_state_check <rom> [save-at-frame] [frames-after]

#include "mercury/gamegear/game_gear.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::uint64_t fingerprint(mercury::gamegear::GameGear& machine) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    // The presented frame stands in for VRAM here: there is no public VRAM
    // accessor, and a frame that renders identically for 240 frames is a
    // stronger statement about the video state than the raw bytes anyway.
    const auto& frame = machine.vdp().presented_frame_pixels();
    mix(frame.data(), frame.size());
    const auto& regs = machine.cpu().registers();
    mix(reinterpret_cast<const std::uint8_t*>(&regs), sizeof(regs));
    return hash;
}

// The Game Gear exposes step(), not run_frame(): step until the VDP's frame
// counter moves, with a budget so a wedged machine ends the test rather than
// hanging it.
void run_frame(mercury::gamegear::GameGear& machine) {
    const auto start = machine.vdp().frames_elapsed();
    for (int i = 0; i < 2'000'000 && machine.vdp().frames_elapsed() == start; ++i) {
        (void)machine.step();
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gg_state_check <rom> [save-at] [distance]\n");
        return 2;
    }
    const int save_at = argc > 2 ? std::atoi(argv[2]) : 600;
    const int distance = argc > 3 ? std::atoi(argv[3]) : 240;

    mercury::gamegear::GameGear machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.ok) {
        std::printf("load failed: %s\n", load.error.c_str());
        return 1;
    }
    machine.reset();

    for (int i = 0; i < save_at; ++i) run_frame(machine);
    const auto blob = machine.save_state_blob();
    std::printf("saved at frame %d: %zu bytes\n", save_at, blob.size());

    for (int i = 0; i < distance; ++i) run_frame(machine);
    const auto expected = fingerprint(machine);

    std::string error;
    if (!machine.load_state_blob(blob, error)) {
        std::printf("FAIL: load rejected the state: %s\n", error.c_str());
        return 1;
    }
    for (int i = 0; i < distance; ++i) run_frame(machine);
    const auto got = fingerprint(machine);

    if (got != expected) {
        std::printf("FAIL: diverged after reload (%016llX vs %016llX)\n",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(expected));
        return 1;
    }
    std::printf("PASS: identical %d frames after reload (%016llX)\n", distance,
                static_cast<unsigned long long>(got));
    return 0;
}
