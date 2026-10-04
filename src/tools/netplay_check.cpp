// Two machines, one game, over a real socket.
//
// This runs both ends of a netplay session in one process: a host and a joiner
// on the loopback address, each with its own Genesis, playing the same
// cartridge in lockstep with scripted buttons. At the end it compares the two
// machines byte for byte.
//
// It exercises everything the feature is made of - the handshake, the
// passphrase-derived encryption, the cartridge check, the state handover and
// the frame-by-frame input exchange - and it fails loudly if the two ever
// disagree, which is the only failure mode netplay really has.
//
// Usage: netplay_check <rom> [frames] [port]
//        netplay_check <game gear rom> --link [frames] [port]
//
// --link plays a Gear Link pair instead: each end holds BOTH Game Gears and
// the cable between them, the way the shell does, and the handover goes
// through GearLinkSession::save_pair_state / load_pair_state - the same two
// calls the shell's netplay hooks make. All four consoles are compared at
// the end, and the two pairs' own blobs, which carry the session baselines.

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/gear_link_session.hpp"
#include "mercury/genesis/genesis.hpp"
#include "mercury/net/genesis_seats.hpp"
#include "mercury/net/netplay.hpp"
#include "mercury/segacd/bios.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::uint64_t hash_of(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Buttons as a bit mask, which is what crosses the wire.
std::uint16_t buttons_for(int frame, int who) {
    const int phase = (frame + who * 37) % 113;
    std::uint16_t mask = 0;
    if (phase >= 10 && phase < 18) mask |= 1u << 7;   // start
    if (phase >= 25 && phase < 40) mask |= 1u << 4;   // a
    if (phase >= 50 && phase < 70) mask |= 1u << 3;   // right
    if (phase >= 80 && phase < 88) mask |= 1u << 5;   // b
    return mask;
}

// THE CONDITIONS A REAL CONNECTION HAS AND LOOPBACK DOES NOT.
//
//   --latency MS --jitter MS   every message held back that long (see
//                              Session::set_simulated_latency), so the two ends
//                              spend most frames waiting on each other.
//   --live-input               buttons follow the CLOCK, not the frame number.
//
// The second one is the one that matters and the one this tool lacked. A
// person's hands keep moving while their machine waits for the other one, so
// asking "what is this player holding for frame N" twice can get two answers.
// With buttons worked out from the frame number, it never could - and the bug
// that parted two real computers in Sonic 3 (a frame's input re-sent with a
// different value while waiting) passed here every time.
struct Conditions {
    std::uint32_t latency_ms = 0;
    std::uint32_t jitter_ms = 0;
    bool live = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
Conditions g_conditions;

std::uint16_t local_buttons(int frame, int who) {
    if (!g_conditions.live) {
        return buttons_for(frame, who);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_conditions.start)
                        .count();
    return buttons_for(static_cast<int>(ms / 7), who);
}

// --guns: both seats hold a Justifier, the Lethal Enforcers two-player setup.
// Each end sends a moving aim and trigger pulls alongside its buttons, and both
// apply the two seats through apply_genesis_seats - the shell's own routing -
// so this proves the guns cross the wire and land identically, not merely
// that the buttons do. Before protocol 3 neither gun's aim crossed at all.
bool g_guns = false;
// --no-plug: the CONTROL. The joiner keeps its own (different) controllers
// instead of the host's, the way it did before protocol 3. Must drift - if it
// does not, the game never read the ports in the frames run and a pass
// proves nothing about them.
bool g_no_plug = false;

mercury::net::PlayerInput local_input(int frame, int who) {
    mercury::net::PlayerInput input;
    input.buttons = local_buttons(frame, who);
    if (!g_guns) {
        return input;
    }
    // The same clock as the buttons when the input is live, so a gun that
    // moves while this end waits is exercised too.
    int t = frame;
    if (g_conditions.live) {
        t = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - g_conditions.start)
                                 .count() /
                             7);
    }
    const int phase = (t + who * 41) % 97;
    if (phase >= 20 && phase < 26) {
        input.buttons |= 1u << mercury::net::PlayerInput::kTriggerBit;
    }
    if (phase >= 70 && phase < 74) {
        input.buttons |= 1u << mercury::net::PlayerInput::kReloadBit;
    }
    input.gun_x = static_cast<std::uint16_t>((40 + who * 120 + t * 3) % 320);
    input.gun_y = static_cast<std::uint16_t>((30 + who * 70 + t * 2) % 224);
    input.on_screen = !(phase >= 70 && phase < 74) && phase < 90;
    return input;
}

std::array<mercury::genesis::ControllerPort::Kind, 2> seat_kinds_for_test() {
    using Kind = mercury::genesis::ControllerPort::Kind;
    return g_guns ? std::array<Kind, 2>{Kind::justifier, Kind::justifier}
                  : std::array<Kind, 2>{Kind::pad3, Kind::pad3};
}

// The shell's apply_port_kinds_for, for the two layouts this tool uses.
void plug_in(mercury::genesis::Genesis& machine) {
    using Kind = mercury::genesis::ControllerPort::Kind;
    if (g_guns) {
        machine.port1().set_kind(Kind::pad3);
        machine.port2().set_kind(Kind::justifier);
        machine.port2().set_gun2_present(true);
    } else {
        machine.port1().set_kind(Kind::pad3);
        machine.port2().set_kind(Kind::pad3);
        machine.port2().set_gun2_present(false);
    }
}

// Everything one end applied, in order, folded into one number - compared at
// the end, it says the two ends were handed the same inputs on every frame.
void fold_inputs(std::uint64_t& hash, std::uint32_t frame, const mercury::net::PlayerInput& first,
                 const mercury::net::PlayerInput& second) {
    const auto mix = [&hash](std::uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            hash ^= static_cast<std::uint8_t>(value >> (i * 8));
            hash *= 1099511628211ULL;
        }
    };
    mix(frame, 4);
    for (const auto* input : {&first, &second}) {
        mix(input->buttons, 2);
        mix(input->gun_x, 2);
        mix(input->gun_y, 2);
        mix(input->on_screen ? 1 : 0, 1);
    }
}

// True if argv[i] was one of the options above (and consumes its value).
bool take_condition(int& i, int argc, char** argv) {
    const std::string arg = argv[i];
    if (arg == "--guns") {
        g_guns = true;
        return true;
    }
    if (arg == "--no-plug") {
        g_no_plug = true;
        return true;
    }
    if (arg == "--latency" && i + 1 < argc) {
        g_conditions.latency_ms = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        return true;
    }
    if (arg == "--jitter" && i + 1 < argc) {
        g_conditions.jitter_ms = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        return true;
    }
    if (arg == "--live-input") {
        g_conditions.live = true;
        return true;
    }
    return false;
}

// How long to allow: a session running on a slow connection runs slowly.
std::chrono::steady_clock::duration time_allowed(int frames) {
    return std::chrono::seconds(60) +
           std::chrono::milliseconds(static_cast<long long>(frames) *
                                     (g_conditions.latency_ms + g_conditions.jitter_ms));
}

void report_conditions() {
    if (g_conditions.latency_ms != 0 || g_conditions.jitter_ms != 0 || g_conditions.live) {
        std::printf("conditions  : %u ms latency, up to %u ms jitter, %s input\n",
                    g_conditions.latency_ms, g_conditions.jitter_ms,
                    g_conditions.live ? "live (clock-driven)" : "frame-driven");
    }
}

void apply(mercury::genesis::ControllerPort& port, std::uint16_t mask) {
    using mercury::genesis::Button;
    for (int i = 0; i < static_cast<int>(Button::count); ++i) {
        port.set_button(static_cast<Button>(i), (mask & (1u << i)) != 0);
    }
}

// A cartridge, or a Sega CD disc with the BIOS for its region found the way
// genesis_probe finds it (MERCURY_SEGACD_BIOS, or a BIOS folder near the disc).
mercury::genesis::Genesis::LoadResult load_any(mercury::genesis::Genesis& machine,
                                               const std::filesystem::path& image) {
    if (!mercury::segacd::Disc::is_disc_image(image)) {
        return machine.load_rom(image);
    }
    mercury::genesis::Genesis::LoadResult failed;
    mercury::segacd::Disc peek;
    std::string error;
    if (!peek.open(image, error)) {
        failed.message = error;
        return failed;
    }
    std::filesystem::path configured;
    if (const char* e = std::getenv("MERCURY_SEGACD_BIOS"); e != nullptr && *e != '\0') {
        configured = e;
    }
    std::vector<std::uint8_t> bios;
    if (!mercury::segacd::load_bios_for(peek.region(), configured, image, bios, error)) {
        failed.message = error;
        return failed;
    }
    return machine.load_cd(image, bios);
}

mercury::net::Hooks hooks_for(mercury::genesis::Genesis& machine, std::uint64_t fingerprint) {
    mercury::net::Hooks hooks;
    // The disc, the BIOS and the 32X firmware as well as the cartridge, the
    // way the shell's handshake folds them in.
    fingerprint ^= machine.media_id();
    hooks.rom_fingerprint = [fingerprint]() { return fingerprint; };
    hooks.capture_state = [&machine]() { return machine.save_state_blob(); };
    hooks.restore_state = [&machine](const std::vector<std::uint8_t>& blob, std::string& error) {
        return machine.load_state_blob(blob, error);
    };
    return hooks;
}

// --- a linked pair over the network -----------------------------------------

// Both Game Gears and the cable between them. Held by pointer because the
// session keeps pointers to the two consoles and must never see them move.
struct LinkedPair {
    mercury::gamegear::GameGear first;
    mercury::gamegear::GameGear second;
    mercury::gamegear::GearLinkSession session;
};

// A netplay mask onto one console, bit for bit as the shell's
// apply_mask_to_game_gear does it: the mask is laid out like a Mega Drive
// pad, A is button 1 and B is button 2.
void apply_to_game_gear(mercury::gamegear::GameGear& machine, std::uint16_t mask) {
    using mercury::gamegear::Button;
    const auto held = [mask](int bit) { return (mask & (1u << bit)) != 0; };
    auto& input = machine.input_state();
    input.set_button(Button::up, held(0));
    input.set_button(Button::down, held(1));
    input.set_button(Button::left, held(2));
    input.set_button(Button::right, held(3));
    input.set_button(Button::button1, held(4));
    input.set_button(Button::button2, held(5));
    input.set_button(Button::start, held(7));
}

// The shell folds both consoles into the number it reports, so a drift on
// either one is caught.
std::uint64_t pair_hash(LinkedPair& pair) {
    std::uint64_t hash = hash_of(pair.first.save_state_blob());
    const auto other = hash_of(pair.second.save_state_blob());
    hash ^= other + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

mercury::net::Hooks hooks_for_pair(LinkedPair& pair, std::uint64_t fingerprint) {
    mercury::net::Hooks hooks;
    hooks.rom_fingerprint = [fingerprint]() { return fingerprint; };
    hooks.capture_state = [&pair]() { return pair.session.save_pair_state(); };
    hooks.restore_state = [&pair](const std::vector<std::uint8_t>& blob, std::string& error) {
        if (!mercury::gamegear::GearLinkSession::is_pair_state(blob)) {
            error = "the host did not send a linked pair";
            return false;
        }
        return pair.session.load_pair_state(blob, error);
    };
    return hooks;
}

int run_link(const char* rom, int frames, std::uint16_t port, bool host_second) {
    auto host_pair = std::make_unique<LinkedPair>();
    auto join_pair = std::make_unique<LinkedPair>();
    for (auto* machine : {&host_pair->first, &host_pair->second, &join_pair->first,
                          &join_pair->second}) {
        const auto load = machine->load_rom(rom);
        if (!load.ok) {
            std::printf("load failed: %s\n", load.error.c_str());
            return 1;
        }
    }
    host_pair->session.connect(host_pair->first, host_pair->second);
    // The joiner's pair is somewhere else entirely before the handover:
    // opened later, run less far and pressed differently. A handover that
    // carried too little would show up as a joiner still wearing its own
    // history.
    for (int frame = 0; frame < 23; ++frame) {
        auto& alone = join_pair->first;
        const auto at_entry = alone.vdp().frames_elapsed();
        for (int guard = 0; guard < 2'000'000 && alone.vdp().frames_elapsed() == at_entry; ++guard) {
            (void)alone.step();
        }
    }
    join_pair->session.connect(join_pair->first, join_pair->second);
    for (int frame = 0; frame < 240; ++frame) {
        apply_to_game_gear(host_pair->first, buttons_for(frame, 0));
        apply_to_game_gear(host_pair->second, buttons_for(frame, 1));
        (void)host_pair->session.run_frame();
    }
    for (int frame = 0; frame < 137; ++frame) {
        apply_to_game_gear(join_pair->first, buttons_for(frame + 5, 1));
        (void)join_pair->session.run_frame();
    }
    std::printf("host pair ran 240 frames, joiner pair 137 of its own, before the handover\n");
    std::printf("before: host %016llX  joiner %016llX\n",
                static_cast<unsigned long long>(pair_hash(*host_pair)),
                static_cast<unsigned long long>(pair_hash(*join_pair)));

    // The same fold the shell applies when a link is open, so the handshake
    // is tested with the number it really sees.
    const std::uint64_t fingerprint = 0x1234ABCD5678EF90ULL ^ 0x4745415220204C4EULL;
    const std::string passphrase = "test-passphrase-1234";

    mercury::net::Session host;
    mercury::net::Session client;
    host.set_delay(3);
    client.set_delay(3);
    host.set_host_plays_second(host_second);
    host.set_simulated_latency(g_conditions.latency_ms, g_conditions.jitter_ms);
    client.set_simulated_latency(g_conditions.latency_ms, g_conditions.jitter_ms);
    report_conditions();

    std::string error;
    if (!host.host(port, "hostplayer", passphrase, hooks_for_pair(*host_pair, fingerprint), error)) {
        std::printf("host failed: %s\n", error.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!client.join("127.0.0.1", port, "joiner", passphrase,
                     hooks_for_pair(*join_pair, fingerprint), error)) {
        std::printf("join failed: %s\n", error.c_str());
        return 1;
    }

    int host_frames = 0;
    int client_frames = 0;
    int state_checks = 0;
    const auto deadline = std::chrono::steady_clock::now() + time_allowed(frames);
    while ((host_frames < frames || client_frames < frames) &&
           std::chrono::steady_clock::now() < deadline) {
        if (host.status() == mercury::net::Status::failed ||
            client.status() == mercury::net::Status::failed) {
            break;
        }
        bool moved = false;
        std::uint16_t p1 = 0;
        std::uint16_t p2 = 0;
        // Each end offers only its own player's buttons; the session hands
        // back both, in console order.
        if (host_frames < frames && host.exchange(local_buttons(host_frames, 0), p1, p2)) {
            apply_to_game_gear(host_pair->first, p1);
            apply_to_game_gear(host_pair->second, p2);
            if (!host_pair->session.run_frame()) {
                std::printf("host pair stopped producing frames at %d\n", host_frames);
                break;
            }
            if (host_frames % mercury::net::Session::kStateCheckInterval == 0) {
                host.report_state_hash(static_cast<std::uint32_t>(host_frames),
                                       pair_hash(*host_pair));
                ++state_checks;
            }
            ++host_frames;
            moved = true;
        }
        if (client_frames < frames && client.exchange(local_buttons(client_frames, 1), p1, p2)) {
            apply_to_game_gear(join_pair->first, p1);
            apply_to_game_gear(join_pair->second, p2);
            if (!join_pair->session.run_frame()) {
                std::printf("joiner pair stopped producing frames at %d\n", client_frames);
                break;
            }
            if (client_frames % mercury::net::Session::kStateCheckInterval == 0) {
                client.report_state_hash(static_cast<std::uint32_t>(client_frames),
                                         pair_hash(*join_pair));
            }
            ++client_frames;
            moved = true;
        }
        if (!moved) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::printf("host status  : %d  %s\n", static_cast<int>(host.status()), host.message().c_str());
    std::printf("client status: %d  %s\n", static_cast<int>(client.status()),
                client.message().c_str());
    std::printf("host plays console %d, joiner plays console %d\n",
                host.local_port_index() + 1, client.local_port_index() + 1);
    std::printf("frames run   : host %d  client %d  (asked for %d), %d state checks\n", host_frames,
                client_frames, frames, state_checks);

    const bool first_same = host_pair->first.save_state_blob() == join_pair->first.save_state_blob();
    const bool second_same =
        host_pair->second.save_state_blob() == join_pair->second.save_state_blob();
    const bool pair_same = host_pair->session.save_pair_state() == join_pair->session.save_pair_state();
    const auto crossed =
        host_pair->session.cable().revision(0) + host_pair->session.cable().revision(1);

    const bool sides_differ = host.local_port_index() != client.local_port_index();
    const bool noticed = host.desynced() || client.desynced();
    host.close();
    client.close();

    const bool ran = host_frames == frames && client_frames == frames;
    std::printf("\nsession noticed a drift       : %s\n", noticed ? "YES" : "no");
    std::printf("both reached the end          : %s\n", ran ? "yes" : "NO");
    std::printf("each end plays a different one: %s\n", sides_differ ? "yes" : "NO");
    std::printf("console 1 identical           : %s\n", first_same ? "yes" : "NO");
    std::printf("console 2 identical           : %s\n", second_same ? "yes" : "NO");
    std::printf("pair incl. baselines identical: %s\n", pair_same ? "yes" : "NO");
    std::printf("cable pin changes on the host : %u\n", crossed);
    if (ran && !noticed && sides_differ && first_same && second_same && pair_same) {
        std::printf(
            "\nTwo linked Game Gear pairs played %d frames over a socket and\n"
            "finished byte for byte identical, cable and all.\n",
            frames);
        return 0;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) != "--link") {
            continue;
        }
        int frames = 600;
        std::uint16_t port = 47216;
        bool host_second = false;
        int positional = 0;
        for (int j = 2; j < argc; ++j) {
            const std::string arg = argv[j];
            if (arg == "--link" || take_condition(j, argc, argv)) {
                continue;
            }
            if (arg == "--host-second") {
                host_second = true;
            } else if (positional++ == 0) {
                frames = std::atoi(argv[j]);
            } else {
                port = static_cast<std::uint16_t>(std::atoi(argv[j]));
            }
        }
        return run_link(argv[1], frames, port, host_second);
    }

    if (argc < 2) {
        std::printf("usage: netplay_check <rom> [frames] [port]\n");
        return 2;
    }
    int frames = 600;
    std::uint16_t port = 47215;
    bool wrong_passphrase = false;
    bool wrong_rom = false;
    bool host_second = false;
    int lead = 240;
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        if (take_condition(i, argc, argv)) {
            continue;
        }
        if (std::string(argv[i]) == "--wrong-passphrase") {
            wrong_passphrase = true;
        } else if (std::string(argv[i]) == "--wrong-rom") {
            wrong_rom = true;
        } else if (std::string(argv[i]) == "--host-second") {
            host_second = true;
        } else if (std::string(argv[i]) == "--lead" && i + 1 < argc) {
            // How far the host runs alone first. A Sega CD disc spends its
            // first thousand frames in the BIOS, so a handover at 240 would
            // test the BIOS rather than the game.
            lead = std::atoi(argv[++i]);
        } else if (positional++ == 0) {
            frames = std::atoi(argv[i]);
        } else {
            port = static_cast<std::uint16_t>(std::atoi(argv[i]));
        }
    }

    // On the heap: a Genesis carries the SVP's DRAM inline.
    auto host_owner = std::make_unique<mercury::genesis::Genesis>();
    auto client_owner = std::make_unique<mercury::genesis::Genesis>();
    auto& host_machine = *host_owner;
    auto& client_machine = *client_owner;
    for (auto* machine : {&host_machine, &client_machine}) {
        const auto load = load_any(*machine, argv[1]);
        if (!load.loaded) {
            std::printf("load failed: %s\n", load.message.c_str());
            return 1;
        }
    }
    // The joiner starts with the OTHER layout, the way a computer whose Input
    // tab disagrees with the host's would - and is plugged into the host's
    // only once the session runs, as the shell does.
    plug_in(host_machine);
    {
        const bool guns = g_guns;
        g_guns = !guns;
        plug_in(client_machine);
        g_guns = guns;
    }
    // The host runs on a bit before anyone joins, so the state handover has
    // something real to carry and a joiner starting from a blank machine would
    // be obvious.
    for (int i = 0; i < lead; ++i) {
        host_machine.run_frame();
    }
    std::printf("host ran %d frames alone before the joiner arrived\n", lead);

    const std::uint64_t fingerprint = 0x1234ABCD5678EF90ULL;
    const std::string passphrase = "test-passphrase-1234";

    mercury::net::Session host;
    mercury::net::Session client;
    host.set_delay(3);
    client.set_delay(3);
    host.set_host_plays_second(host_second);
    host.set_simulated_latency(g_conditions.latency_ms, g_conditions.jitter_ms);
    client.set_simulated_latency(g_conditions.latency_ms, g_conditions.jitter_ms);
    // Opaque to the session; the shell sends its PadKind values. The joiner
    // offers something else, which the host's must replace.
    host.set_seat_kinds(g_guns ? 2 : 0, g_guns ? 2 : 0);
    client.set_seat_kinds(7, 9);
    report_conditions();
    if (g_guns) {
        std::printf("seats       : two Justifiers, aimed and fired from both ends\n");
    }

    std::string error;
    if (!host.host(port, "hostplayer", passphrase, hooks_for(host_machine, fingerprint), error)) {
        std::printf("host failed: %s\n", error.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // The two ways a session must refuse rather than limp on: a passphrase that
    // does not match, and a different cartridge. Both are silent
    // desynchronisations if they are not caught at the handshake.
    const std::string joiner_passphrase = wrong_passphrase ? "not-the-passphrase" : passphrase;
    const std::uint64_t joiner_fingerprint = wrong_rom ? fingerprint ^ 0xFFULL : fingerprint;
    if (!client.join("127.0.0.1", port, "joiner", joiner_passphrase,
                     hooks_for(client_machine, joiner_fingerprint), error)) {
        std::printf("join failed: %s\n", error.c_str());
        return 1;
    }
    if (wrong_passphrase || wrong_rom) {
        // Expect a refusal, not a game.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < give_up) {
            if (host.status() == mercury::net::Status::failed ||
                client.status() == mercury::net::Status::failed) {
                break;
            }
            std::uint16_t a = 0, b = 0;
            host.exchange(0, a, b);
            client.exchange(0, a, b);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("host  : %s\n", host.message().c_str());
        std::printf("client: %s\n", client.message().c_str());
        const bool refused = host.status() == mercury::net::Status::failed ||
                             client.status() == mercury::net::Status::failed;
        host.close();
        client.close();
        std::printf("\nrefused as it should be: %s\n", refused ? "yes" : "NO");
        return refused ? 0 : 1;
    }

    // Both ends are driven from this one loop, alternating, the way two
    // separate programs would each drive their own.
    int host_frames = 0;
    int client_frames = 0;
    std::uint64_t host_inputs = 1469598103934665603ULL;
    std::uint64_t client_inputs = 1469598103934665603ULL;
    bool client_plugged = false;
    const auto seats = seat_kinds_for_test();
    const auto deadline = std::chrono::steady_clock::now() + time_allowed(frames);
    while ((host_frames < frames || client_frames < frames) &&
           std::chrono::steady_clock::now() < deadline) {
        if (host.status() == mercury::net::Status::failed ||
            client.status() == mercury::net::Status::failed) {
            break;
        }
        bool moved = false;
        mercury::net::PlayerInput first;
        mercury::net::PlayerInput second;
        if (host_frames < frames && host.exchange(local_input(host_frames, 0), first, second)) {
            mercury::net::apply_genesis_seats(host_machine, seats, {first, second});
            fold_inputs(host_inputs, static_cast<std::uint32_t>(host_frames), first, second);
            host_machine.run_frame();
            if (host_frames % mercury::net::Session::kStateCheckInterval == 0) {
                host.report_state_hash(static_cast<std::uint32_t>(host_frames),
                                       hash_of(host_machine.save_state_blob()));
            }
            ++host_frames;
            moved = true;
        }
        if (client_frames < frames && client.exchange(local_input(client_frames, 1), first, second)) {
            if (!client_plugged && !g_no_plug) {
                // The host's controllers, from its greeting, before the first
                // frame - what netplay_step does in the shell.
                plug_in(client_machine);
                client_plugged = true;
            }
            mercury::net::apply_genesis_seats(client_machine, seats, {first, second});
            fold_inputs(client_inputs, static_cast<std::uint32_t>(client_frames), first, second);
            client_machine.run_frame();
            if (client_frames % mercury::net::Session::kStateCheckInterval == 0) {
                client.report_state_hash(static_cast<std::uint32_t>(client_frames),
                                         hash_of(client_machine.save_state_blob()));
            }
            ++client_frames;
            moved = true;
        }
        if (!moved) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::printf("host status : %d  %s\n", static_cast<int>(host.status()),
                host.message().c_str());
    std::printf("client status: %d  %s\n", static_cast<int>(client.status()),
                client.message().c_str());
    std::printf("host saw peer '%s', client saw peer '%s'\n", host.peer().nickname.c_str(),
                client.peer().nickname.c_str());
    std::printf("frames run  : host %d  client %d  (asked for %d)\n", host_frames, client_frames,
                frames);

    const auto host_hash = hash_of(host_machine.save_state_blob());
    const auto client_hash = hash_of(client_machine.save_state_blob());
    std::printf("host state  : %016llX\n", static_cast<unsigned long long>(host_hash));
    std::printf("client state: %016llX\n", static_cast<unsigned long long>(client_hash));

    const bool noticed = host.desynced() || client.desynced();
    const auto joiner_seats = client.seat_kinds();
    const bool seats_crossed = joiner_seats == host.seat_kinds();
    host.close();
    client.close();

    const bool ran = host_frames == frames && client_frames == frames;
    const bool together = host_hash == client_hash;
    const bool same_inputs = host_inputs == client_inputs;
    std::printf("\nhost's seat setup reached the joiner : %s (%u, %u)\n",
                seats_crossed ? "yes" : "NO", joiner_seats[0], joiner_seats[1]);
    std::printf("same inputs applied at both ends     : %s\n", same_inputs ? "yes" : "NO");
    std::printf("session noticed a drift : %s\n", noticed ? "YES" : "no");
    std::printf("both reached the end    : %s\n", ran ? "yes" : "NO");
    std::printf("still in sync           : %s\n", together ? "yes" : "NO");
    if (ran && together && !noticed && same_inputs && seats_crossed) {
        std::printf(
            "\nTwo machines played %d frames of the same game over a socket and\n"
            "finished byte for byte identical.\n",
            frames);
        return 0;
    }
    return 1;
}
