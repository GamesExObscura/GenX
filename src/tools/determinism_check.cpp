// Can this emulator do lockstep netplay at all?
//
// Lockstep netplay sends nothing but the players' BUTTONS. Both machines run
// the same code on the same cartridge and are relied on to arrive at the same
// state, frame after frame, for as long as the session lasts. If they can
// drift even slightly - one uninitialised byte, one decision made from the wall
// clock - the two pictures separate and no amount of network code will fix it.
//
// So this is the precondition, and it is worth answering before writing a
// single socket: run the same cartridge twice with the same inputs and check
// that the two machines are byte-for-byte identical afterwards. The save state
// is the whole machine, so hashing it is the check.
//
// Usage: determinism_check <rom> [frames]

#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// FNV-1a. Not a security hash - this only has to notice a difference.
std::uint64_t hash_of(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// A fixed, repeatable button pattern. Both runs must see exactly this.
void drive(mercury::genesis::Genesis& machine, int frame) {
    using mercury::genesis::Button;
    const int phase = frame % 137;  // prime, so the pattern does not line up with anything
    machine.port1().set_button(Button::start, phase >= 20 && phase < 26);
    machine.port1().set_button(Button::a, phase >= 40 && phase < 48);
    machine.port1().set_button(Button::right, phase >= 60 && phase < 90);
    machine.port1().set_button(Button::b, phase >= 100 && phase < 106);
    machine.port2().set_button(Button::c, phase >= 70 && phase < 80);
}

struct Run {
    std::uint64_t state_hash;
    std::uint64_t frame_hash;
    std::size_t state_size;
};

Run play(const char* rom, int frames, bool& ok) {
    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        ok = false;
        return {};
    }
    for (int frame = 0; frame < frames; ++frame) {
        drive(machine, frame);
        machine.run_frame();
    }
    const auto blob = machine.save_state_blob();
    ok = true;
    return {hash_of(blob), hash_of(machine.vdp().frame_pixels()), blob.size()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: determinism_check <rom> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 900;

    bool ok = false;
    const auto first = play(argv[1], frames, ok);
    if (!ok) {
        return 1;
    }
    const auto second = play(argv[1], frames, ok);
    if (!ok) {
        return 1;
    }

    std::printf("frames run  : %d\n", frames);
    std::printf("state size  : %zu bytes\n", first.state_size);
    std::printf("run 1 state : %016llX   picture %016llX\n",
                static_cast<unsigned long long>(first.state_hash),
                static_cast<unsigned long long>(first.frame_hash));
    std::printf("run 2 state : %016llX   picture %016llX\n",
                static_cast<unsigned long long>(second.state_hash),
                static_cast<unsigned long long>(second.frame_hash));

    const bool same_state = first.state_hash == second.state_hash;
    const bool same_frame = first.frame_hash == second.frame_hash;
    std::printf("\nmachine state identical : %s\n", same_state ? "YES" : "NO");
    std::printf("picture identical       : %s\n", same_frame ? "YES" : "NO");
    if (same_state && same_frame) {
        std::printf(
            "\nDeterministic over %d frames. Lockstep netplay is possible: two\n"
            "machines fed the same buttons will stay together.\n",
            frames);
        return 0;
    }
    std::printf(
        "\nNOT deterministic. Two machines fed identical buttons ended up in\n"
        "different states, so lockstep netplay would desynchronise. Find the\n"
        "source of the difference before building any network code on top.\n");
    return 1;
}
