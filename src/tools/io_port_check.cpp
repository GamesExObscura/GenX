// Which controller-port registers does this game actually use?
//
// The Mega Drive's I/O chip is sixteen registers: a version byte, three
// controller ports of data and control, and a UART for each of those three
// ports. GenX implements the pads and answers zero for the nine serial
// registers, which is fine for every game that only has controllers plugged
// in and useless for one that talks to a peripheral.
//
// Before building a UART, the question is whether a given game reaches for
// one, at which port, and what it writes to the control register - because
// that is where the baud rate and the direction bits live. The Miracle Piano
// is the reason this exists.
//
// Usage: io_port_check <rom> [frames]

#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: io_port_check <rom> [frames] [--trace N] [--only <register>]\n");
        return 2;
    }
    const int frames = argc > 2 && argv[2][0] != '-' ? std::atoi(argv[2]) : 1800;
    int trace_lines = 0;
    int decode_count = 0;
    bool decode = false;
    const char* only = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_lines = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (std::strcmp(argv[i], "--decode") == 0) {
            decode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                decode_count = std::atoi(argv[++i]);
            }
        }
    }

    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }

    // With --piano, plug the keyboard into port 2 and report whether the game
    // notices. This is the only test that matters for the device: the game
    // either recognises the instrument and moves on, or it does not.
    bool piano = false;
    // Plug the keyboard in partway through instead of before boot. This is
    // what pressing M during a game actually does, and it is a different
    // question from "does the device work": the console probes its ports at
    // startup, and a peripheral that turns up later may simply be missed.
    int piano_at = 0;
    int press_at = 0;
    int show_notes = 0;
    std::vector<mercury::genesis::MiraclePiano::SoundEvent> note_log;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--piano") == 0) {
            piano = true;
        } else if (std::strcmp(argv[i], "--piano-at") == 0 && i + 1 < argc) {
            piano = true;
            piano_at = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--press-at") == 0 && i + 1 < argc) {
            press_at = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--notes") == 0) {
            show_notes = i + 1 < argc && argv[i + 1][0] != '-' ? std::atoi(argv[++i]) : 40;
        }
    }
    if (piano && piano_at == 0) {
        machine.port2().set_kind(mercury::genesis::ControllerPort::Kind::miracle_piano);
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
                machine.port2().piano().set_reply_mask(
                    static_cast<std::uint8_t>(std::strtol(argv[++i], nullptr, 0)));
            } else if (std::strcmp(argv[i], "--invert") == 0) {
                machine.port2().piano().set_reply_inverted(true);
            } else if (std::strcmp(argv[i], "--cancel") == 0) {
                machine.port2().piano().set_cancel_on_strobe(true);
            }
        }
    }

    if (trace_lines > 0 || decode) {
        // A byte costs about 27 accesses, so decoding N of them needs a
        // generous buffer.
        const std::size_t room =
            static_cast<std::size_t>(trace_lines) * 8 +
            static_cast<std::size_t>(decode_count > 0 ? decode_count : 64) * 64 + 8192;
        machine.set_io_trace(true, room);
    }

    for (int frame = 0; frame < frames; ++frame) {
        if (piano_at != 0 && frame == piano_at) {
            machine.port2().set_kind(mercury::genesis::ControllerPort::Kind::miracle_piano);
            std::printf("(keyboard plugged in at frame %d)\n", frame);
        }
        // A key held down from this frame on, so the note has to travel the
        // whole way: window to device to queue to the console.
        if (press_at != 0 && frame == press_at &&
            machine.port2().kind() ==
                mercury::genesis::ControllerPort::Kind::miracle_piano) {
            machine.port2().piano().set_key(24, true);
            std::printf("(middle key pressed at frame %d)\n", frame);
        }
        // The same periodic taps the other probes use, so a title screen that
        // wants a button press to move on can get past itself.
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::a, phase >= 60 && phase < 68);
        machine.run_frame();
        // Drained every frame, exactly as the emulator's mixer does, so the
        // queue never overflows and drops the events being studied.
        if (piano && machine.port2().kind() ==
                         mercury::genesis::ControllerPort::Kind::miracle_piano) {
            mercury::genesis::MiraclePiano::SoundEvent events[32];
            auto& device = machine.port2().piano();
            while (const auto count = device.take_sound_events(events, 32)) {
                for (std::size_t i = 0; i < count; ++i) {
                    note_log.push_back(events[i]);
                }
                if (count < 32) {
                    break;
                }
            }
        }
    }

    const auto& io = machine.io_activity();
    std::printf("frames run: %d\n\n", frames);
    std::printf("%-9s  %-8s  %12s  %12s  %s\n", "register", "address", "reads", "writes",
                "last write");
    bool any_serial = false;
    for (std::size_t slot = 0; slot < 16; ++slot) {
        const auto reads = io.reads[slot];
        const auto writes = io.writes[slot];
        if (reads == 0 && writes == 0) {
            continue;
        }
        std::printf("%-9s  $A100%02X  %12llu  %12llu  0x%02X\n",
                    mercury::genesis::Genesis::io_register_name(slot),
                    static_cast<unsigned>((slot << 1) | 1),
                    static_cast<unsigned long long>(reads),
                    static_cast<unsigned long long>(writes),
                    static_cast<unsigned>(io.last_write[slot]));
        // Slots 7 and up are the three UARTs.
        if (slot >= 7) {
            any_serial = true;
        }
    }

    if (trace_lines > 0) {
        std::printf("\nfirst %d accesses%s:\n", trace_lines,
                    only != nullptr ? " (filtered)" : "");
        int shown = 0;
        std::uint64_t previous = 0;
        for (const auto& entry : machine.io_trace()) {
            const char* name = mercury::genesis::Genesis::io_register_name(entry.slot);
            if (only != nullptr && std::strcmp(name, only) != 0) {
                continue;
            }
            if (shown >= trace_lines) {
                break;
            }
            // The gap matters more than the timestamp: a run of accesses with
            // no gap is one transaction, and a gap is the boundary between
            // two of them.
            const auto gap = shown == 0 ? 0 : entry.cycle - previous;
            previous = entry.cycle;
            std::printf("  %-8s %s 0x%02X   +%llu\n", name, entry.write ? "W" : "R",
                        static_cast<unsigned>(entry.value),
                        static_cast<unsigned long long>(gap));
            ++shown;
        }
        if (shown == 0) {
            std::printf("  nothing matched\n");
        }
    }

    // Reconstruct the bytes a bit-banged port is clocking out.
    //
    // With the control register at 0x60 the game owns TH (bit 6) and TR
    // (bit 5) and reads everything else. The trace shows TR pulsing while TH
    // holds steady either side of it, which is a clock and a data line: each
    // rising edge of TR presents one bit of TH. Eight of those make a byte.
    //
    // Printed both ways round because the bit order is not something the
    // waveform reveals - only recognising the result does. MIDI is the tell:
    // a status byte has its top bit set, and a sysex message starts 0xF0 and
    // ends 0xF7.
    if (decode) {
        std::printf("\nbytes clocked out of port 2 (TR rising edge samples TH):\n");
        bool previous_clock = false;
        bool have_previous = false;
        unsigned msb = 0;
        unsigned lsb = 0;
        int bits = 0;
        int printed = 0;
        for (const auto& entry : machine.io_trace()) {
            if (entry.slot != 2 || !entry.write) {  // slot 2 is data2
                continue;
            }
            const bool clock = (entry.value & 0x20) != 0;
            const bool data = (entry.value & 0x40) != 0;
            if (have_previous && !previous_clock && clock) {
                msb = (msb << 1) | (data ? 1u : 0u);
                lsb = (lsb >> 1) | (data ? 0x80u : 0u);
                if (++bits == 8) {
                    std::printf("  msb-first 0x%02X    lsb-first 0x%02X\n", msb & 0xFF,
                                lsb & 0xFF);
                    msb = 0;
                    lsb = 0;
                    bits = 0;
                    if (++printed >= (decode_count > 0 ? decode_count : 32)) {
                        break;
                    }
                }
            }
            previous_clock = clock;
            have_previous = true;
        }
        if (printed == 0) {
            std::printf("  no complete bytes seen\n");
        }
    }

    if (piano && show_notes) {
        // What the device believes the game is asking the instrument to
        // play. A stuck note is either a note-on whose note-off never
        // arrives, or garbage that decoded as one; only the stream itself
        // separates those two.
        std::printf("\nnote events the game sent (first %d):\n", show_notes);
        int shown = 0;
        int sounding = 0;
        std::array<int, 128> held{};
        for (const auto& event : note_log) {
            if (shown < show_notes) {
                std::printf("  ch%-2u note %3u (%s) velocity %3u program %u\n",
                            event.channel, event.note,
                            event.velocity != 0 ? "on " : "OFF", event.velocity,
                            event.program);
                ++shown;
            }
            if (event.note < 128) {
                if (event.velocity != 0) {
                    ++held[event.note];
                } else if (held[event.note] > 0) {
                    --held[event.note];
                }
            }
        }
        for (const auto count : held) {
            if (count > 0) {
                ++sounding;
            }
        }
        std::printf("  %zu note events total, %d note(s) left sounding at the end\n",
                    note_log.size(), sounding);
        if (sounding > 0) {
            std::printf(
                "  A note left sounding is a note-on the game never turned off. If the\n"
                "  stream above looks like music it is a missed note-off; if it looks\n"
                "  like noise the byte framing is still wrong.\n");
        }
    }

    if (piano) {
        const auto& a = machine.port2().piano().activity();
        std::printf("\nMiracle Piano on port 2:\n");
        std::printf("  bytes the game sent   : %llu   last 0x%02X\n",
                    static_cast<unsigned long long>(a.bytes_from_console),
                    static_cast<unsigned>(a.last_from_console));
        std::printf("  bytes the piano sent  : %llu   last 0x%02X\n",
                    static_cast<unsigned long long>(a.bytes_to_console),
                    static_cast<unsigned>(a.last_to_console));
        std::printf("  polls                 : %llu\n",
                    static_cast<unsigned long long>(a.polls));
        std::printf("  sysex messages handled: %llu\n",
                    static_cast<unsigned long long>(a.sysex_handled));
        std::printf("  sysex commands seen   :");
        for (std::size_t command = 0; command < a.sysex_commands.size(); ++command) {
            if (a.sysex_commands[command] != 0) {
                std::printf(" %02X x%llu", static_cast<unsigned>(command),
                            static_cast<unsigned long long>(a.sysex_commands[command]));
            }
        }
        std::printf("\n");
        for (std::size_t slot = 0; slot < a.sysex_sample_count; ++slot) {
            std::printf("    message %zu:", slot);
            for (std::uint8_t i = 0; i < a.sysex_sample_length[slot]; ++i) {
                std::printf(" %02X", static_cast<unsigned>(a.sysex_samples[slot][i]));
            }
            std::printf("\n");
        }
        std::printf("  first bytes received  :");
        for (std::size_t i = 0; i < a.first_count; ++i) {
            if (i != 0 && (i % 16) == 0) {
                std::printf("\n                         ");
            }
            std::printf(" %02X", static_cast<unsigned>(a.first_from_console[i]));
        }
        std::printf("\n");
        if (a.bytes_to_console == 0) {
            std::printf(
                "\nThe piano never got a byte back to the console. Either the poll is not\n"
                "being recognised, or the reply is going out on the wrong pin.\n");
        }
    }

    std::printf("\n");
    if (any_serial) {
        std::printf(
            "This game DRIVES THE SERIAL HARDWARE. The control register's top three\n"
            "bits are the baud rate, so the last value written there says how fast\n"
            "the peripheral is expected to talk.\n");
    } else {
        std::printf(
            "Only the pad registers were touched. Either this game has no serial\n"
            "peripheral, or the run never reached the point where it looks for one.\n");
    }
    return 0;
}
