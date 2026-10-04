// The Gear-to-Gear link cable, measured.
//
// Two modes:
//
//   gearlink_check <rom> [frames]
//       One console, no cable. Counts accesses to the link registers, which
//       says whether a game reaches for the cable at all and what it writes
//       when it does. This is what was run before any of it was emulated.
//
//   gearlink_check --link <rom> [frames]
//       Two consoles joined by a cable, run in lockstep. Counts what actually
//       crossed. A game that transmits into an empty socket looks identical to
//       one that transmits successfully until you count the far end, so the
//       received figure is the one that matters.

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/gear_link_session.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Enough button pressing to get past a title screen and into a menu where a
// two-player option might be chosen.
void drive(mercury::gamegear::GameGear& machine, int frame) {
    using mercury::gamegear::Button;
    auto& input = machine.input_state();
    const int phase = frame % 100;
    input.set_button(Button::start, phase >= 10 && phase < 18);
    input.set_button(Button::button1, phase >= 30 && phase < 38);
    input.set_button(Button::down, phase >= 50 && phase < 56);
    input.set_button(Button::button2, phase >= 70 && phase < 76);
}

// A written-down route through a game's menus, because the cycling pattern
// above gets past a title screen and no further. One entry is
// "<frame>:<button>", optionally "<side>:<frame>:<button>" to press on one
// console only; entries are separated by commas.
//
//   --script 90:start,150:start,1:210:button1,2:210:button2
//
// A press is held for eight frames, long enough for anything that samples the
// pad once a frame and short enough not to register as two.
struct ScriptedPress {
    int side = 0;  // 0 = both
    int frame = 0;
    mercury::gamegear::Button button = mercury::gamegear::Button::start;
};

bool parse_button(const std::string& name, mercury::gamegear::Button& out) {
    using mercury::gamegear::Button;
    if (name == "up") { out = Button::up; return true; }
    if (name == "down") { out = Button::down; return true; }
    if (name == "left") { out = Button::left; return true; }
    if (name == "right") { out = Button::right; return true; }
    if (name == "start") { out = Button::start; return true; }
    if (name == "1" || name == "button1") { out = Button::button1; return true; }
    if (name == "2" || name == "button2") { out = Button::button2; return true; }
    return false;
}

std::vector<ScriptedPress> parse_script(const std::string& text) {
    std::vector<ScriptedPress> presses;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto end = text.find(',', at);
        const auto piece = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() + 1 : end + 1;
        if (piece.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        std::size_t cut = 0;
        while (cut <= piece.size()) {
            const auto colon = piece.find(':', cut);
            fields.push_back(
                piece.substr(cut, colon == std::string::npos ? std::string::npos : colon - cut));
            cut = colon == std::string::npos ? piece.size() + 1 : colon + 1;
        }
        ScriptedPress press;
        std::string button_name;
        if (fields.size() == 2) {
            press.frame = std::atoi(fields[0].c_str());
            button_name = fields[1];
        } else if (fields.size() == 3) {
            press.side = std::atoi(fields[0].c_str());
            press.frame = std::atoi(fields[1].c_str());
            button_name = fields[2];
        } else {
            std::printf("bad script entry '%s'\n", piece.c_str());
            continue;
        }
        if (!parse_button(button_name, press.button)) {
            std::printf("unknown button '%s'\n", button_name.c_str());
            continue;
        }
        presses.push_back(press);
    }
    return presses;
}

void drive_scripted(mercury::gamegear::GameGear& machine, int side, int frame,
                    const std::vector<ScriptedPress>& script) {
    using mercury::gamegear::Button;
    auto& input = machine.input_state();
    for (const auto button : {Button::up, Button::down, Button::left, Button::right, Button::start,
                              Button::button1, Button::button2}) {
        input.set_button(button, false);
    }
    for (const auto& press : script) {
        if (press.side != 0 && press.side != side) {
            continue;
        }
        if (frame >= press.frame && frame < press.frame + 8) {
            input.set_button(press.button, true);
        }
    }
}

// The Game Gear exposes single instructions, not frames, so a frame is "step
// until the VDP says it finished one" - with a ceiling so a wedged machine
// cannot spin here for ever.
void run_frame(mercury::gamegear::GameGear& machine) {
    const auto at_entry = machine.vdp().frames_elapsed();
    for (int guard = 0; guard < 2'000'000 && machine.vdp().frames_elapsed() == at_entry; ++guard) {
        (void)machine.step();
    }
}

void print_side(const char* label, const mercury::gamegear::SystemControl& control) {
    const auto& serial = control.serial_activity();
    std::printf("%s\n", label);
    std::printf("  transmit writes ($03): %llu   last value 0x%02X\n",
                static_cast<unsigned long long>(serial.tx_writes),
                static_cast<unsigned>(serial.last_tx));
    std::printf("  bytes that left       : %llu\n",
                static_cast<unsigned long long>(serial.bytes_sent));
    std::printf("  bytes that arrived    : %llu   last value 0x%02X\n",
                static_cast<unsigned long long>(serial.bytes_received),
                static_cast<unsigned>(serial.last_rx));
    std::printf("  frame errors          : %llu\n",
                static_cast<unsigned long long>(serial.frame_errors));
    std::printf("  receive reads   ($04) : %llu\n",
                static_cast<unsigned long long>(serial.rx_reads));
    std::printf("  control writes  ($05) : %llu   last value 0x%02X\n",
                static_cast<unsigned long long>(serial.control_writes),
                static_cast<unsigned>(serial.last_control));
    std::printf("  control reads   ($05) : %llu\n",
                static_cast<unsigned long long>(serial.control_reads));
    std::printf("  pin writes      ($01) : %llu   last value 0x%02X\n",
                static_cast<unsigned long long>(serial.parallel_writes),
                static_cast<unsigned>(serial.last_parallel));
    std::printf("  pin reads       ($01) : %llu\n",
                static_cast<unsigned long long>(serial.parallel_reads));
    std::printf("  direction writes($02) : %llu   last value 0x%02X\n",
                static_cast<unsigned long long>(serial.direction_writes),
                static_cast<unsigned>(serial.last_direction));
    std::printf("  link interrupts       : %llu\n",
                static_cast<unsigned long long>(serial.link_nmis));
    std::printf("  writes LOST (busy)    : %llu",
                static_cast<unsigned long long>(serial.tx_dropped));
    if (serial.tx_dropped != 0) {
        std::printf("   at bit:");
        for (int bit = 0; bit < 10; ++bit) {
            if (serial.tx_dropped_at[bit] != 0) {
                std::printf(" %d=%llu", bit,
                            static_cast<unsigned long long>(serial.tx_dropped_at[bit]));
            }
        }
    }
    std::printf("\n");
}

// Where the trace writes to. A raw callback carries no state of its own, and
// what makes a trace readable is the frame number and which console it was.
struct TraceState {
    int frame = 0;
    int first_frame = -1;
    int last_frame = -1;

    // A polling loop reads the same port thousands of times a frame. Printing
    // each one buries everything else, and a line ceiling silently truncates
    // the interesting part - which it did, and cost a wrong conclusion. So
    // identical consecutive accesses collapse into one line with a count.
    bool have_pending = false;
    const char* pending_side = nullptr;
    bool pending_write = false;
    std::uint8_t pending_port = 0;
    std::uint8_t pending_value = 0;
    int pending_first_frame = 0;
    int pending_last_frame = 0;
    long long pending_count = 0;
} g_trace;

void flush_trace() {
    if (!g_trace.have_pending) {
        return;
    }
    if (g_trace.pending_count == 1) {
        std::printf("f%-5d %s %s $%02X = %02X\n", g_trace.pending_first_frame, g_trace.pending_side,
                    g_trace.pending_write ? "W" : "R", static_cast<unsigned>(g_trace.pending_port),
                    static_cast<unsigned>(g_trace.pending_value));
    } else {
        std::printf("f%-5d %s %s $%02X = %02X   x%lld (through f%d)\n", g_trace.pending_first_frame,
                    g_trace.pending_side, g_trace.pending_write ? "W" : "R",
                    static_cast<unsigned>(g_trace.pending_port),
                    static_cast<unsigned>(g_trace.pending_value), g_trace.pending_count,
                    g_trace.pending_last_frame);
    }
    g_trace.have_pending = false;
}

void trace_port(void* user, bool write, std::uint8_t port, std::uint8_t value) {
    if (g_trace.frame < g_trace.first_frame || g_trace.frame > g_trace.last_frame) {
        return;
    }
    const char* side = static_cast<const char*>(user);
    if (g_trace.have_pending && g_trace.pending_side == side && g_trace.pending_write == write &&
        g_trace.pending_port == port && g_trace.pending_value == value) {
        ++g_trace.pending_count;
        g_trace.pending_last_frame = g_trace.frame;
        return;
    }
    flush_trace();
    g_trace.have_pending = true;
    g_trace.pending_side = side;
    g_trace.pending_write = write;
    g_trace.pending_port = port;
    g_trace.pending_value = value;
    g_trace.pending_first_frame = g_trace.frame;
    g_trace.pending_last_frame = g_trace.frame;
    g_trace.pending_count = 1;
}

// Where each console's processor actually spends its time.
//
// The question "both machines run a full loop every frame and neither ever
// advances" cannot be answered from the wire: the wire says the two are
// talking. It is answered by ranking executed addresses and then reading what
// is at the top of the list.
struct HotPcs {
    const mercury::gamegear::GameGear* machine[2]{nullptr, nullptr};
    std::unordered_map<std::uint16_t, std::uint64_t> hits[2];
    bool collecting = false;
} g_hot;

void observe_step(void* /*user*/, int index) {
    if (!g_hot.collecting || index < 0 || index > 1 || g_hot.machine[index] == nullptr) {
        return;
    }
    ++g_hot.hits[index][g_hot.machine[index]->cpu().registers().pc];
}

void print_hot_pcs(const mercury::gamegear::GameGear& machine, int index, const char* label) {
    auto& table = g_hot.hits[index];
    if (table.empty()) {
        return;
    }
    std::vector<std::pair<std::uint16_t, std::uint64_t>> ranked(table.begin(), table.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::uint64_t total = 0;
    for (const auto& entry : ranked) {
        total += entry.second;
    }
    std::printf("\n-- %s: hottest addresses (%llu instructions, %llu distinct) --\n", label,
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(ranked.size()));
    for (std::size_t i = 0; i < ranked.size() && i < 60; ++i) {
        std::printf("  PC=%04X  %9llu  (%2llu%%)  bytes:", ranked[i].first,
                    static_cast<unsigned long long>(ranked[i].second),
                    static_cast<unsigned long long>(ranked[i].second * 100 /
                                                    (total != 0 ? total : 1)));
        for (int byte = 0; byte < 8; ++byte) {
            std::printf(" %02X", machine.bus().read8(
                                    static_cast<std::uint16_t>(ranked[i].first + byte)));
        }
        std::printf("\n");
    }
}

struct Options {
    int frames = 1800;
    // Start ranking addresses at this frame, so the boot and the menus do not
    // bury the loop that matters. Negative means do not collect at all.
    int hot_from = -1;
    // RAM addresses to print once a frame, for both consoles. The wire says
    // what crossed; this says what the game did with it, which is the half a
    // link protocol cannot be understood without.
    std::vector<std::uint16_t> watch;
    int watch_from = 0;
    int trace_first = -1;
    int trace_last = -1;
    std::string shot_dir;
    int shot_every = 0;
    std::vector<ScriptedPress> script;
    // Frames console 2 is held in reset before the pair is joined. Two real
    // Game Gears are never switched on in the same millisecond, and a game
    // that picks a leader by racing cannot pick one from a perfect tie.
    int stagger = 0;
    // The same idea but finer: whole frames leave the two consoles still
    // agreeing about where in the frame they are, which is most of the
    // symmetry. Instructions break that too.
    int stagger_steps = 0;
    // The session's own head start, overridable so a fault can be pinned on it
    // or cleared of it. -1 means leave the default alone.
    long long startup_offset = -1;
};

int run_single(const char* rom, const Options& options) {
    const int frames = options.frames;
    mercury::gamegear::GameGear machine;
    const auto load = machine.load_rom(rom);
    if (!load.ok) {
        std::printf("load failed: %s\n", load.error.c_str());
        return 1;
    }

    g_trace.first_frame = options.trace_first;
    g_trace.last_frame = options.trace_last;
    machine.mutable_system_control().set_port_trace(trace_port, const_cast<char*>("solo"));

    int first_touch = -1;
    for (int frame = 0; frame < frames; ++frame) {
        g_trace.frame = frame;
        if (options.script.empty()) {
            drive(machine, frame);
        } else {
            drive_scripted(machine, 1, frame, options.script);
        }
        run_frame(machine);
        if (options.shot_every > 0 && !options.shot_dir.empty() &&
            frame % options.shot_every == 0) {
            std::error_code ignored;
            std::filesystem::create_directories(options.shot_dir, ignored);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "/a_%05d.bmp", frame);
            (void)machine.vdp().export_visible_frame_bmp(
                std::filesystem::path(options.shot_dir + buffer));
        }
        const auto& serial = machine.system_control().serial_activity();
        if (first_touch < 0 &&
            (serial.tx_writes != 0 || serial.control_writes != 0 || serial.rx_reads != 0 ||
        serial.parallel_writes != 0 || serial.parallel_reads != 0 ||
        serial.direction_writes != 0)) {
            first_touch = frame;
        }
    }

    flush_trace();
    std::printf("frames run          : %d\n", frames);
    print_side("one console, no cable:", machine.system_control());
    std::printf("first touched at    : %s\n",
                first_touch < 0 ? "never" : ("frame " + std::to_string(first_touch)).c_str());

    const auto& serial = machine.system_control().serial_activity();
    const bool touched =
        serial.tx_writes != 0 || serial.control_writes != 0 || serial.rx_reads != 0 ||
        serial.parallel_writes != 0 || serial.parallel_reads != 0 ||
        serial.direction_writes != 0;
    if (!touched) {
        std::printf(
            "\nThis game never touched the link registers in this run. Either it has\n"
            "no link mode, or the run never reached the menu where one is chosen.\n");
    } else {
        std::printf(
            "\nThe game DOES use the link registers. Run again with --link to see how\n"
            "much of that reaches a second console.\n");
    }
    return 0;
}

// Both consoles' screens, so a linked run can be SEEN rather than inferred
// from counters. Getting a two-player game as far as its link screen means
// navigating menus, and the only way to know whether the scripted buttons
// got there is to look.
void shoot_pair(const mercury::gamegear::GameGear& first,
                const mercury::gamegear::GameGear& second, const std::string& directory,
                int frame) {
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const auto name = [&](const char* side) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "/%s_%05d.bmp", side, frame);
        return std::filesystem::path(directory + buffer);
    };
    (void)first.vdp().export_visible_frame_bmp(name("a"));
    (void)second.vdp().export_visible_frame_bmp(name("b"));
}

// CAN A LINKED PAIR BE COPIED TO ANOTHER MACHINE AND STAY IDENTICAL?
//
// This is what netplay does to start a Gear Link session: the host sends
// both consoles and the session's two clock baselines, and the joiner loads
// them over a pair that was somewhere else entirely. The claim is that from
// then on, given the same buttons, the two pairs are the same machine.
//
// Tested the only honest way - two separate pairs in two different states,
// one copied onto the other, then run side by side comparing all four
// consoles' saved state every frame. `skip_baselines` copies the consoles
// but not the session, which is the version of this that shipped first and
// the control that shows the baselines are load-bearing.
int run_transfer(const char* rom, const Options& options, int transfer_at, bool skip_baselines) {
    using mercury::gamegear::GameGear;
    GameGear host_one, host_two, join_one, join_two;
    for (auto* machine : {&host_one, &host_two, &join_one, &join_two}) {
        const auto load = machine->load_rom(rom);
        if (!load.ok) {
            std::printf("load failed: %s\n", load.error.c_str());
            return 1;
        }
    }
    mercury::gamegear::GearLinkSession host;
    mercury::gamegear::GearLinkSession join;
    host.connect(host_one, host_two);
    // The joiner's pair starts from a different place: it is opened later,
    // runs a different number of frames and is pressed differently.
    for (int frame = 0; frame < 23; ++frame) {
        run_frame(join_one);
    }
    join.connect(join_one, join_two);

    for (int frame = 0; frame < transfer_at; ++frame) {
        drive(host_one, frame);
        drive(host_two, frame + 37);
        (void)host.run_frame();
    }
    for (int frame = 0; frame < transfer_at / 2 + 17; ++frame) {
        drive(join_one, frame + 5);
        drive(join_two, frame + 61);
        (void)join.run_frame();
    }

    // The transfer. The real one goes through save_pair_state and
    // load_pair_state - the same two calls the netplay hooks make. The bare
    // control loads the consoles by hand and leaves the session alone.
    std::string error;
    if (skip_baselines) {
        if (!join_one.load_state_blob(host_one.save_state_blob(), error) ||
            !join_two.load_state_blob(host_two.save_state_blob(), error)) {
            std::printf("state load failed: %s\n", error.c_str());
            return 1;
        }
    } else {
        const auto blob = host.save_pair_state();
        if (!join.load_pair_state(blob, error)) {
            std::printf("pair load failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("pair blob: %zu bytes\n", blob.size());
    }
    std::printf("transferred at frame %d%s; host skew %lld, joiner skew %lld\n", transfer_at,
                skip_baselines ? " WITHOUT the session baselines" : "",
                static_cast<long long>(host.skew()), static_cast<long long>(join.skew()));

    int first_mismatch = -1;
    int matched = 0;
    for (int frame = 0; frame < options.frames; ++frame) {
        const int at = transfer_at + frame;
        drive(host_one, at);
        drive(host_two, at + 37);
        drive(join_one, at);
        drive(join_two, at + 37);
        (void)host.run_frame();
        (void)join.run_frame();
        const bool same = host_one.save_state_blob() == join_one.save_state_blob() &&
                          host_two.save_state_blob() == join_two.save_state_blob();
        if (same) {
            ++matched;
        } else if (first_mismatch < 0) {
            first_mismatch = frame;
        }
    }
    const auto crossed = host.cable().revision(0) + host.cable().revision(1);
    std::printf("after the transfer: %d of %d frames identical on all four consoles", matched,
                options.frames);
    if (first_mismatch >= 0) {
        std::printf(", first difference %d frames in\n", first_mismatch);
    } else {
        std::printf("\n");
    }
    std::printf("cable activity on the host pair: %u pin changes\n", crossed);
    return first_mismatch < 0 ? 0 : 3;
}

int run_linked(const char* rom, const Options& options) {
    const int frames = options.frames;
    mercury::gamegear::GameGear first;
    mercury::gamegear::GameGear second;

    for (auto* machine : {&first, &second}) {
        const auto load = machine->load_rom(rom);
        if (!load.ok) {
            std::printf("load failed: %s\n", load.error.c_str());
            return 1;
        }
    }

    // Console 1 runs alone first, so that when the pair is joined they are not
    // sitting on the same instruction of the same ROM.
    for (int frame = 0; frame < options.stagger; ++frame) {
        run_frame(first);
    }
    for (int step = 0; step < options.stagger_steps; ++step) {
        (void)first.step();
    }

    mercury::gamegear::GearLinkSession session;
    if (options.startup_offset >= 0) {
        session.set_startup_offset(options.startup_offset);
    }
    session.connect(first, second);

    g_trace.first_frame = options.trace_first;
    g_trace.last_frame = options.trace_last;
    first.mutable_system_control().set_port_trace(trace_port, const_cast<char*>("one"));
    second.mutable_system_control().set_port_trace(trace_port, const_cast<char*>("two"));

    if (options.hot_from >= 0) {
        g_hot.machine[0] = &first;
        g_hot.machine[1] = &second;
        session.set_step_observer(observe_step, nullptr);
    }

    int first_arrival = -1;
    std::int64_t worst_skew = 0;
    int stalled_frame = -1;

    for (int frame = 0; frame < frames; ++frame) {
        g_trace.frame = frame;
        g_hot.collecting = options.hot_from >= 0 && frame >= options.hot_from;
        if (options.script.empty()) {
            drive(first, frame);
            // The second console is driven on a different phase, so if a game
            // has a two-player menu the two consoles are not always agreeing by
            // accident.
            drive(second, frame + 37);
        } else {
            drive_scripted(first, 1, frame, options.script);
            drive_scripted(second, 2, frame, options.script);
        }

        if (!session.run_frame() && stalled_frame < 0) {
            stalled_frame = frame;
        }

        if (options.shot_every > 0 && !options.shot_dir.empty() &&
            frame % options.shot_every == 0) {
            shoot_pair(first, second, options.shot_dir, frame);
        }

        // Only when something moved. A value printed every frame whether it
        // changed or not buries the one frame where it did.
        if (!options.watch.empty() && frame >= options.watch_from) {
            static std::vector<std::uint8_t> previous;
            std::vector<std::uint8_t> now;
            now.reserve(options.watch.size() * 2);
            for (const auto address : options.watch) {
                now.push_back(first.bus().read8(address));
                now.push_back(second.bus().read8(address));
            }
            if (now != previous) {
                std::printf("f%-5d ", frame);
                for (std::size_t i = 0; i < options.watch.size(); ++i) {
                    std::printf(" %04X=%02X/%02X", options.watch[i], now[i * 2],
                                now[i * 2 + 1]);
                }
                std::printf("\n");
                previous = now;
            }
        }

        const auto skew = session.skew();
        const auto magnitude = skew < 0 ? -skew : skew;
        if (magnitude > worst_skew) {
            worst_skew = magnitude;
        }

        if (first_arrival < 0 &&
            (first.system_control().serial_activity().bytes_received != 0 ||
             second.system_control().serial_activity().bytes_received != 0)) {
            first_arrival = frame;
        }
    }

    flush_trace();
    std::printf("frames run          : %d\n", frames);
    std::printf("stagger             : %d frames\n", options.stagger);
    std::printf("worst clock skew    : %lld cycles\n", static_cast<long long>(worst_skew));
    if (stalled_frame >= 0) {
        std::printf("STALLED             : a console stopped producing frames at frame %d\n",
                    stalled_frame);
    }
    std::printf("\n");
    print_side("console 1:", first.system_control());
    std::printf("\n");
    print_side("console 2:", second.system_control());
    print_hot_pcs(first, 0, "console 1");
    print_hot_pcs(second, 1, "console 2");

    const auto sent = first.system_control().serial_activity().bytes_sent +
                      second.system_control().serial_activity().bytes_sent;
    const auto received = first.system_control().serial_activity().bytes_received +
                          second.system_control().serial_activity().bytes_received;
    const auto errors = first.system_control().serial_activity().frame_errors +
                        second.system_control().serial_activity().frame_errors;

    std::printf("\n%llu bytes put on the wire, %llu arrived, %llu framing errors.\n",
                static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(errors));
    const auto pin_traffic = first.system_control().serial_activity().parallel_writes +
                             second.system_control().serial_activity().parallel_writes;

    if (first_arrival >= 0) {
        std::printf("First byte crossed at frame %d.\n", first_arrival);
    } else if (sent != 0) {
        std::printf(
            "Bytes were transmitted but none were received. Either the receivers were\n"
            "never enabled, or the two consoles are configured for different baud rates.\n");
    } else if (pin_traffic > 8) {
        // Not every link game uses the UART. Both Mortal Kombats bit-bang the
        // seven parallel pins and never write $03 at all, so a byte count of
        // zero says nothing about them.
        std::printf(
            "Neither console used the serial port, but the parallel pins were driven\n"
            "%llu times. This game bit-bangs the cable rather than using the UART, so\n"
            "the byte counters above do not apply to it.\n",
            static_cast<unsigned long long>(pin_traffic));
    } else {
        std::printf(
            "Neither console transmitted. This run never reached a link mode, which is\n"
            "a statement about the button-pressing above, not about the cable.\n");
    }
    return 0;
}

}  // namespace

const char* kUsage =
    "usage: gearlink_check [--link] <rom> [frames]\n"
    "  --shot <dir>        write both screens as BMPs\n"
    "  --shot-every <n>    how often to write them, in frames (default 60)\n"
    "  --script <list>     [side:]frame:button, comma separated\n"
    "                      buttons: up down left right start 1 2\n"
    "  --stagger <n>       run console 1 alone for n frames before joining\n"
    "  --stagger-steps <n> the same in instructions, for a sub-frame offset\n"
    "  --offset <cycles>   the session's own head start; 0 is exact lockstep\n"
    "  --hot-pcs <frame>   rank executed addresses on both consoles from here\n"
    "  --watch <a,b,...>   print these RAM addresses (hex) when they change\n"
    "  --watch-from <n>    start watching at this frame\n"
    "  --trace <a>-<b>     print every link-port access over that frame span\n";

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("%s", kUsage);
        return 2;
    }

    bool linked = false;
    Options options;
    const char* rom = nullptr;
    bool frames_given = false;
    int transfer_at = -1;
    bool skip_baselines = false;

    for (int argument = 1; argument < argc; ++argument) {
        const std::string flag = argv[argument];
        // --transfer <frame>: copy one pair onto another the way netplay
        // does, then check they stay identical. --transfer-bare does the
        // same without the session baselines, as the control.
        if ((flag == "--transfer" || flag == "--transfer-bare") && argument + 1 < argc) {
            transfer_at = std::atoi(argv[++argument]);
            skip_baselines = flag == "--transfer-bare";
            continue;
        }
        const auto value = [&](const char* name) -> const char* {
            if (argument + 1 >= argc) {
                std::printf("%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++argument];
        };
        if (flag == "--link") {
            linked = true;
        } else if (flag == "--shot") {
            options.shot_dir = value("--shot");
            if (options.shot_every == 0) {
                options.shot_every = 60;
            }
        } else if (flag == "--shot-every") {
            options.shot_every = std::atoi(value("--shot-every"));
        } else if (flag == "--script") {
            options.script = parse_script(value("--script"));
        } else if (flag == "--stagger") {
            options.stagger = std::atoi(value("--stagger"));
        } else if (flag == "--watch") {
            const std::string list = value("--watch");
            std::size_t at = 0;
            while (at <= list.size()) {
                const auto end = list.find(',', at);
                const auto piece =
                    list.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? list.size() + 1 : end + 1;
                if (!piece.empty()) {
                    options.watch.push_back(
                        static_cast<std::uint16_t>(std::strtoul(piece.c_str(), nullptr, 16)));
                }
            }
        } else if (flag == "--watch-from") {
            options.watch_from = std::atoi(value("--watch-from"));
        } else if (flag == "--hot-pcs") {
            options.hot_from = std::atoi(value("--hot-pcs"));
        } else if (flag == "--offset") {
            options.startup_offset = std::atoll(value("--offset"));
        } else if (flag == "--stagger-steps") {
            options.stagger_steps = std::atoi(value("--stagger-steps"));
        } else if (flag == "--trace") {
            const std::string span = value("--trace");
            const auto dash = span.find('-');
            options.trace_first = std::atoi(span.c_str());
            options.trace_last = dash == std::string::npos
                                     ? options.trace_first
                                     : std::atoi(span.c_str() + dash + 1);
        } else if (rom == nullptr) {
            rom = argv[argument];
        } else if (!frames_given) {
            options.frames = std::atoi(argv[argument]);
            frames_given = true;
        }
    }

    if (rom == nullptr) {
        std::printf("%s", kUsage);
        return 2;
    }

    if (transfer_at >= 0) {
        return run_transfer(rom, options, transfer_at, skip_baselines);
    }
    return linked ? run_linked(rom, options) : run_single(rom, options);
}
