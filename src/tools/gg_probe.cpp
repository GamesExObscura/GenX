// gg_probe: headless Game Gear diagnostic driver.
//
// Runs a ROM frame-by-frame with scripted button input and watches for two
// kinds of failure:
//   * execution collapsing into a tiny PC loop (classic hang), and
//   * the presented framebuffer going static for many frames (visible freeze).
//
// When either trips it dumps the loop body, CPU/interrupt state, VDP state,
// bank state and the memory the loop is reading, so hangs can be diagnosed
// from evidence rather than guesswork.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "mercury/gamegear/game_gear.hpp"

namespace {

using mercury::gamegear::Button;
using mercury::gamegear::GameGear;

std::string hex8(std::uint8_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X", value);
    return buffer;
}

std::string hex16(std::uint16_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04X", value);
    return buffer;
}

struct PcInfo {
    std::uint64_t hits = 0;
    std::uint8_t opcode = 0;
    std::uint8_t prefix = 0;
    std::string message;
};

// Rolling backtrace of executed instructions. Deliberately stores no strings
// so recording stays cheap enough to leave on for whole runs.
struct TraceEntry {
    std::uint16_t pc = 0;
    std::uint8_t prefix = 0;
    std::uint8_t opcode = 0;
    std::uint8_t a = 0;
    std::uint8_t f = 0;
    std::uint16_t sp = 0;
    std::uint16_t bc = 0;
    std::uint16_t de = 0;
    std::uint16_t hl = 0;
    std::uint16_t ix = 0;
    std::uint16_t iy = 0;
    std::uint8_t ctrl = 0;
    std::uint8_t slot1 = 0;
};

// Optional extra memory window shown in the crash dump.
bool g_extra_dump = false;
std::uint16_t g_extra_dump_addr = 0;

// Tally of which I/O ports the game actually reads, to catch port-decoding
// gaps (hardware decodes ranges, not exact addresses).
bool g_log_io = false;
std::unordered_map<std::uint8_t, std::uint64_t> g_port_reads;
// Writes matter as much as reads: an unimplemented control port (3F is the
// classic one) is invisible in a read tally but changes what the game expects
// to read back.
std::unordered_map<std::uint8_t, std::uint64_t> g_port_writes;
// Which code reads a given port, and what it got back. "The game polls DC and
// ignores it" is only actionable once you know which routine does the polling.
int g_trace_port = -1;
int g_traced_out_count = 0;
// Where the CPU actually spends its time. The question "the game runs a full
// main loop every frame but never advances" is only answerable by ranking the
// executed addresses.
bool g_hot_pcs = false;
bool g_dump_layers = false;
std::unordered_map<std::uint16_t, std::uint64_t> g_pc_hits;
struct PortSite {
    std::uint64_t hits = 0;
    std::uint64_t hits_with_button = 0;  // reads that returned a pressed bit
};
std::unordered_map<std::uint16_t, PortSite> g_trace_port_pcs;

void note_trace_port(const GameGear& game_gear, std::uint8_t port, std::uint16_t pc) {
    if (g_trace_port < 0 || port != static_cast<std::uint8_t>(g_trace_port)) {
        return;
    }
    auto& site = g_trace_port_pcs[pc];
    ++site.hits;
    // Active low, so a clear bit in the low six means a button was down when
    // this particular site sampled the port.
    const auto value = game_gear.system_control().peek_port(
        port == 0xDD ? 0xC1 : 0xC0);
    if ((value & 0x3F) != 0x3F) {
        ++site.hits_with_button;
    }
}

void record_port_read(const GameGear& game_gear,
                      const mercury::core::Z80StepResult& result) {
    if (!g_log_io && g_trace_port < 0) {
        return;
    }
    if (result.prefix == 0x00 && result.opcode == 0xDB) {
        // IN A,(n) - operand byte follows the opcode.
        const auto port = game_gear.bus().read8(
            static_cast<std::uint16_t>(result.pc_before + 1));
        ++g_port_reads[port];
        note_trace_port(game_gear, port, result.pc_before);
    } else if (result.prefix == 0xED && (result.opcode & 0xC7) == 0x40) {
        // IN r,(C) - port comes from C.
        const auto port = game_gear.cpu().registers().c;
        ++g_port_reads[port];
        note_trace_port(game_gear, port, result.pc_before);
    } else if (result.prefix == 0x00 && result.opcode == 0xD3) {
        // OUT (n),A
        const auto port = game_gear.bus().read8(
            static_cast<std::uint16_t>(result.pc_before + 1));
        ++g_port_writes[port];
        if (g_trace_port >= 0 && port == static_cast<std::uint8_t>(g_trace_port) &&
            g_traced_out_count < 24) {
            ++g_traced_out_count;
            std::cout << "  OUT (" << hex8(port) << "),A = "
                      << hex8(game_gear.cpu().registers().a)
                      << "   from PC=" << std::hex << std::uppercase << std::setw(4)
                      << std::setfill('0') << result.pc_before << std::dec << "\n";
        }
    } else if (result.prefix == 0xED && (result.opcode & 0xC7) == 0x41) {
        // OUT (C),r
        ++g_port_writes[game_gear.cpu().registers().c];
    }
}

void print_hot_pcs(const GameGear& game_gear) {
    if (!g_hot_pcs || g_pc_hits.empty()) {
        return;
    }
    std::vector<std::pair<std::uint16_t, std::uint64_t>> hot(g_pc_hits.begin(), g_pc_hits.end());
    std::sort(hot.begin(), hot.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::uint64_t total = 0;
    for (const auto& entry : hot) {
        total += entry.second;
    }
    std::cout << "\n-- hottest PCs (" << total << " steps sampled) --\n";
    for (std::size_t i = 0; i < hot.size() && i < 24; ++i) {
        std::cout << "  PC=" << std::hex << std::uppercase << std::setw(4)
                  << std::setfill('0') << hot[i].first << std::dec
                  << "  " << hot[i].second
                  << "  (" << (hot[i].second * 100 / (total ? total : 1)) << "%)  bytes:";
        for (int b = 0; b < 6; ++b) {
            std::cout << " " << hex8(game_gear.bus().read8(
                static_cast<std::uint16_t>(hot[i].first + b)));
        }
        std::cout << "\n";
    }
}

void print_trace_port() {
    if (g_trace_port < 0 || g_trace_port_pcs.empty()) {
        return;
    }
    std::vector<std::pair<std::uint16_t, PortSite>> sites(g_trace_port_pcs.begin(),
                                                          g_trace_port_pcs.end());
    std::sort(sites.begin(), sites.end(),
              [](const auto& a, const auto& b) { return a.second.hits > b.second.hits; });
    std::cout << "\n-- code sites reading port " << hex8(static_cast<std::uint8_t>(g_trace_port))
              << " --\n";
    for (std::size_t i = 0; i < sites.size() && i < 20; ++i) {
        std::cout << "  PC=" << std::hex << std::uppercase << std::setw(4)
                  << std::setfill('0') << sites[i].first << std::dec
                  << "  reads=" << sites[i].second.hits
                  << "  while-pressed=" << sites[i].second.hits_with_button << "\n";
    }
}

void print_port_reads() {
    if (!g_log_io || g_port_reads.empty()) {
        return;
    }
    std::vector<std::pair<std::uint8_t, std::uint64_t>> sorted(g_port_reads.begin(),
                                                               g_port_reads.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "\n-- I/O port reads --\n";
    for (std::size_t i = 0; i < sorted.size() && i < 24; ++i) {
        std::cout << "  port " << hex8(sorted[i].first) << " : " << sorted[i].second;
        const auto p = sorted[i].first;
        if (p >= 0xC0) {
            std::cout << "   (joypad range: even->DC, odd->DD)";
        } else if (p >= 0x80) {
            std::cout << "   (VDP)";
        } else if (p >= 0x40) {
            std::cout << "   (V/H counter)";
        } else if (p <= 0x06) {
            std::cout << "   (GG system regs)";
        }
        std::cout << "\n";
    }

    if (g_port_writes.empty()) {
        return;
    }
    std::vector<std::pair<std::uint8_t, std::uint64_t>> writes(g_port_writes.begin(),
                                                               g_port_writes.end());
    std::sort(writes.begin(), writes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "-- I/O port writes --\n";
    for (std::size_t i = 0; i < writes.size() && i < 24; ++i) {
        std::cout << "  port " << hex8(writes[i].first) << " : " << writes[i].second;
        const auto p = writes[i].first;
        if (p == 0x3F) {
            std::cout << "   (*** I/O control / TH nationalisation ***)";
        } else if (p >= 0xC0) {
            std::cout << "   (joypad range - writes ignored)";
        } else if (p >= 0x80) {
            std::cout << "   (VDP)";
        } else if (p >= 0x40) {
            std::cout << "   (PSG)";
        } else if (p <= 0x06) {
            std::cout << "   (GG system regs)";
        } else {
            std::cout << "   (memory control range)";
        }
        std::cout << "\n";
    }
}

std::vector<TraceEntry> g_trace;
std::size_t g_trace_next = 0;
bool g_trace_wrapped = false;

void trace_record(const GameGear& game_gear,
                  const mercury::core::Z80StepResult& result) {
    if (g_trace.empty()) {
        return;
    }

    const auto& reg = game_gear.cpu().registers();
    const auto regs = game_gear.cartridge().mapper_registers();

    TraceEntry entry;
    entry.pc = result.pc_before;
    entry.prefix = result.prefix;
    entry.opcode = result.opcode;
    entry.a = reg.a;
    entry.f = reg.f;
    entry.sp = reg.sp;
    entry.bc = static_cast<std::uint16_t>((reg.b << 8) | reg.c);
    entry.de = static_cast<std::uint16_t>((reg.d << 8) | reg.e);
    entry.hl = static_cast<std::uint16_t>((reg.h << 8) | reg.l);
    entry.ix = reg.ix;
    entry.iy = reg.iy;
    entry.ctrl = regs[0];
    entry.slot1 = regs[2];

    g_trace[g_trace_next] = entry;
    g_trace_next = (g_trace_next + 1) % g_trace.size();
    if (g_trace_next == 0) {
        g_trace_wrapped = true;
    }
}

void print_trace() {
    if (g_trace.empty()) {
        return;
    }

    const std::size_t count = g_trace_wrapped ? g_trace.size() : g_trace_next;
    if (count == 0) {
        return;
    }

    std::cout << "\n-- execution backtrace (oldest first, " << count << " steps) --\n";
    const std::size_t first = g_trace_wrapped ? g_trace_next : 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& e = g_trace[(first + i) % g_trace.size()];
        std::cout << "  " << (i + 1 == count ? ">>" : "  ")
                  << " PC=" << hex16(e.pc) << " op=";
        if (e.prefix != 0x00) {
            std::cout << hex8(e.prefix) << ":";
        } else {
            std::cout << "   ";
        }
        std::cout << hex8(e.opcode)
                  << " A=" << hex8(e.a) << " F=" << hex8(e.f)
                  << " BC=" << hex16(e.bc) << " DE=" << hex16(e.de)
                  << " HL=" << hex16(e.hl)
                  << " IX=" << hex16(e.ix) << " IY=" << hex16(e.iy)
                  << " SP=" << hex16(e.sp)
                  << " ctrl=" << hex8(e.ctrl) << " s1=" << hex8(e.slot1)
                  << "\n";
    }
}

struct FrameStats {
    std::uint64_t steps = 0;
    std::uint64_t halt_steps = 0;
    std::uint64_t irq_accepted = 0;
    std::unordered_map<std::uint16_t, PcInfo> pcs;

    void reset() {
        steps = 0;
        halt_steps = 0;
        irq_accepted = 0;
        pcs.clear();
    }
};

struct PressEvent {
    Button button = Button::start;
    const char* name = "start";
    int frame = 0;
    int hold = 8;
};

struct Options {
    std::filesystem::path rom;
    int max_frames = 4000;
    int log_every = 60;
    int auto_start_at = -1;
    int auto_start_every = 0;
    int auto_start_hold = 8;
    int dump_every = 0;
    std::filesystem::path dump_dir;
    std::size_t stall_pc_threshold = 24;
    int stall_frames = 60;
    int static_screen_frames = 240;
    std::uint64_t max_steps_per_frame = 4'000'000;
    std::vector<PressEvent> presses;
    int trace_depth = 0;
    bool trap_mapper = false;
    std::uint8_t expected_control = 0x80;
    bool watch_enabled = false;
    std::uint16_t watch_addr = 0;
    std::uint16_t watch_value = 0;
    int from_frame = 0;
    bool log_display = false;
    bool log_vdp_regs = false;
    bool gfx_report = false;
    int gfx_start_frame = 150;
    int gfx_interval = 25;
    bool trap_sp_outside = false;
    std::uint16_t sp_lo = 0xC000;
    std::uint16_t sp_hi = 0xDFFF;
    bool trap_exec_ram = false;
    std::uint16_t exec_ram_min = 0xC000;
    bool trap_pc_in = false;
    std::uint16_t pc_in_lo = 0;
    std::uint16_t pc_in_hi = 0;
    bool watch_lt_enabled = false;
    std::uint16_t watch_lt_addr = 0;
    std::uint16_t watch_lt_value = 0;
};

struct ScreenInfo {
    std::uint64_t hash = 0;
    bool solid = false;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

ScreenInfo screen_info(const GameGear& game_gear) {
    ScreenInfo info;
    const auto& pixels = game_gear.vdp().presented_frame_pixels();
    if (pixels.empty()) {
        return info;
    }

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto value : pixels) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ull;
    }
    info.hash = hash;

    info.r = pixels[0];
    info.g = pixels[1];
    info.b = pixels[2];
    info.solid = true;
    for (std::size_t i = 0; i + 2 < pixels.size(); i += 3) {
        if (pixels[i] != info.r || pixels[i + 1] != info.g || pixels[i + 2] != info.b) {
            info.solid = false;
            break;
        }
    }
    return info;
}

// Per-frame graphics metrics. None of these prove a game looks right, but
// they cheaply separate "plausible picture" from "flat, frozen or starved of
// palette", which is enough to decide what a human should eyeball.
struct GfxSample {
    int distinct_colors = 0;
    int dominant_percent = 0;
    std::uint64_t hash = 0;
};

GfxSample gfx_sample(const GameGear& game_gear) {
    GfxSample sample;
    const auto& pixels = game_gear.vdp().presented_frame_pixels();
    if (pixels.size() < 3) {
        return sample;
    }

    std::unordered_map<std::uint32_t, int> counts;
    counts.reserve(64);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i + 2 < pixels.size(); i += 3) {
        const auto rgb = (static_cast<std::uint32_t>(pixels[i]) << 16) |
                         (static_cast<std::uint32_t>(pixels[i + 1]) << 8) |
                         static_cast<std::uint32_t>(pixels[i + 2]);
        ++counts[rgb];
        hash ^= rgb;
        hash *= 1099511628211ull;
    }

    int best = 0;
    int total = 0;
    for (const auto& entry : counts) {
        total += entry.second;
        best = std::max(best, entry.second);
    }

    sample.distinct_colors = static_cast<int>(counts.size());
    sample.dominant_percent = total > 0 ? (best * 100) / total : 100;
    sample.hash = hash;
    return sample;
}

std::string bank_text(const GameGear& game_gear) {
    const auto banks = game_gear.cartridge().visible_banks();
    const auto regs = game_gear.cartridge().mapper_registers();
    return "[" + std::to_string(banks[0]) + "," + std::to_string(banks[1]) + "," +
           std::to_string(banks[2]) + "] regs=" + hex8(regs[0]) + "," + hex8(regs[1]) + "," +
           hex8(regs[2]) + "," + hex8(regs[3]);
}

void dump_state(GameGear& game_gear,
                const FrameStats& stats,
                int frame,
                const char* reason) {
    const auto& cpu = game_gear.cpu();
    const auto& reg = cpu.registers();
    const auto& vdp = game_gear.vdp();
    const auto screen = screen_info(game_gear);

    std::cout << "\n================ PROBE STOP ================\n";
    std::cout << "reason           : " << reason << "\n";
    std::cout << "frame            : " << frame << "\n";
    std::cout << "steps this frame : " << stats.steps << "\n";
    std::cout << "distinct PCs     : " << stats.pcs.size() << "\n";
    std::cout << "HALT steps       : " << stats.halt_steps << "\n";
    std::cout << "IRQs accepted    : " << stats.irq_accepted << "\n";
    std::cout << "screen           : " << (screen.solid ? "SOLID COLOR" : "varied");
    if (screen.solid) {
        std::cout << " rgb=" << hex8(screen.r) << hex8(screen.g) << hex8(screen.b);
    }
    std::cout << "\n";

    std::vector<std::pair<std::uint16_t, PcInfo>> sorted(stats.pcs.begin(), stats.pcs.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::cout << "\n-- loop body (top PCs) --\n";
    for (std::size_t i = 0; i < sorted.size() && i < 20; ++i) {
        const auto& entry = sorted[i];
        std::cout << "  PC=" << hex16(entry.first) << "  op=";
        if (entry.second.prefix != 0x00) {
            std::cout << hex8(entry.second.prefix) << " ";
        }
        std::cout << hex8(entry.second.opcode) << "  hits=" << entry.second.hits
                  << "  " << entry.second.message << "\n";
    }

    std::cout << "\n-- CPU --\n";
    std::cout << "  " << cpu.describe() << "\n";
    std::cout << "  IX=" << hex16(reg.ix) << " IY=" << hex16(reg.iy)
              << " I=" << hex8(reg.i) << " R=" << hex8(reg.r) << "\n";
    std::cout << "  halted=" << (cpu.halted() ? "YES" : "no")
              << "  IFF1(interrupts_enabled)=" << (cpu.interrupts_enabled() ? "YES" : "no") << "\n";

    std::cout << "\n-- interrupt delivery --\n";
    std::cout << "  vdp.interrupt_requested() = " << (vdp.interrupt_requested() ? "ASSERTED" : "clear") << "\n";
    std::cout << "  status byte               = " << hex8(vdp.debug_status())
              << (((vdp.debug_status() & 0x80) != 0) ? "  (bit7 SET = frame flag)"
                                                     : "  (bit7 clear)") << "\n";
    std::cout << "  line IRQ pending          = " << (vdp.debug_line_irq_pending() ? "YES" : "no")
              << "   line counter = " << hex8(vdp.debug_line_counter()) << "\n";
    std::cout << "  frame IRQ enabled (R1.IE) = " << (vdp.frame_interrupt_enabled() ? "yes" : "no") << "\n";
    std::cout << "  line  IRQ enabled (R0.IE1)= " << (vdp.line_interrupt_enabled() ? "yes" : "no") << "\n";
    if (cpu.halted() && !cpu.interrupts_enabled()) {
        std::cout << "  *** HALT with interrupts DISABLED - CPU can never wake ***\n";
    }
    if (vdp.interrupt_requested() && !cpu.interrupts_enabled()) {
        std::cout << "  *** IRQ asserted but IFF1=0 - interrupt is being ignored ***\n";
    }
    if (stats.irq_accepted == 0) {
        std::cout << "  *** no interrupt was accepted during this frame ***\n";
    }

    std::cout << "\n-- VDP --\n";
    std::cout << "  display=" << (vdp.display_enabled() ? "ON" : "OFF")
              << "  frames=" << vdp.frames_elapsed() << "\n";
    std::cout << "  " << vdp.register_summary() << "\n";
    std::cout << "  " << vdp.activity_summary() << "\n";

    std::cout << "\n-- banks --\n  " << bank_text(game_gear) << "\n";

    std::cout << "\n-- stack (from SP=" << hex16(reg.sp) << ") --\n  ";
    for (int i = 0; i < 8; ++i) {
        const auto address = static_cast<std::uint16_t>(reg.sp + i * 2);
        const auto low = game_gear.bus().read8(address);
        const auto high = game_gear.bus().read8(static_cast<std::uint16_t>(address + 1));
        std::cout << hex16(static_cast<std::uint16_t>((high << 8) | low)) << " ";
    }
    std::cout << "\n";

    std::cout << "\n-- memory at IY (" << hex16(reg.iy) << ") --\n  ";
    for (int i = 0; i < 16; ++i) {
        std::cout << hex8(game_gear.bus().read8(static_cast<std::uint16_t>(reg.iy + i))) << " ";
    }
    std::cout << "\n";

    std::cout << "\n-- memory at IX (" << hex16(reg.ix) << ") --\n  ";
    for (int i = 0; i < 16; ++i) {
        std::cout << hex8(game_gear.bus().read8(static_cast<std::uint16_t>(reg.ix + i))) << " ";
    }
    std::cout << "\n";

    std::cout << "\n-- work RAM D200-D21F --\n  ";
    for (int i = 0; i < 32; ++i) {
        if (i == 16) {
            std::cout << "\n  ";
        }
        std::cout << hex8(game_gear.bus().read8(static_cast<std::uint16_t>(0xD200 + i))) << " ";
    }
    std::cout << "\n";

    if (g_extra_dump) {
        std::cout << "\n-- memory at " << hex16(g_extra_dump_addr) << " --\n  ";
        for (int i = 0; i < 32; ++i) {
            if (i == 16) {
                std::cout << "\n  ";
            }
            std::cout << hex8(game_gear.bus().read8(
                             static_cast<std::uint16_t>(g_extra_dump_addr + i)))
                      << " ";
        }
        std::cout << "\n";
    }

    print_port_reads();
    print_trace_port();
    print_trace();
    std::cout << "============================================\n";
}

bool parse_press(const std::string& spec, PressEvent& event) {
    const auto at = spec.find('@');
    if (at == std::string::npos) {
        return false;
    }

    const auto name = spec.substr(0, at);
    auto rest = spec.substr(at + 1);
    int hold = 8;
    const auto colon = rest.find(':');
    if (colon != std::string::npos) {
        hold = std::atoi(rest.substr(colon + 1).c_str());
        rest = rest.substr(0, colon);
    }

    if (name == "start") {
        event.button = Button::start;
        event.name = "start";
    } else if (name == "b1") {
        event.button = Button::button1;
        event.name = "button1";
    } else if (name == "b2") {
        event.button = Button::button2;
        event.name = "button2";
    } else if (name == "up") {
        event.button = Button::up;
        event.name = "up";
    } else if (name == "down") {
        event.button = Button::down;
        event.name = "down";
    } else if (name == "left") {
        event.button = Button::left;
        event.name = "left";
    } else if (name == "right") {
        event.button = Button::right;
        event.name = "right";
    } else {
        return false;
    }

    event.frame = std::atoi(rest.c_str());
    event.hold = hold > 0 ? hold : 1;
    return true;
}

// --battery-in / --battery-out: hand a save file to the cartridge before the
// run, and write what the cartridge holds afterwards. Boot once with no file,
// boot again with what came out, and a game that accepts its own save behaves
// differently the second time - the end-to-end check for battery RAM and the
// baseball carts' serial EEPROM alike.
std::string g_battery_in;
std::string g_battery_out;

bool parse_args(int argc, char** argv, Options& options) {
    if (argc < 2) {
        std::cerr << "usage: gg_probe <rom> [options]\n"
                     "  --max-frames N        total frames to run (default 4000)\n"
                     "  --log-every N         per-frame log interval (default 60, 1 = every frame)\n"
                     "  --press SPEC          e.g. start@430:8  b1@900:8  (repeatable)\n"
                     "  --auto-start AT:EVERY:HOLD   periodic START presses\n"
                     "  --dump-every N        export a BMP every N frames\n"
                     "  --dump-dir DIR        where BMPs go (default %TEMP%)\n"
                     "  --stall-pcs T         stall if distinct PCs <= T (default 24)\n"
                     "  --stall-frames S      ...for S consecutive frames (default 60)\n"
                     "  --static-frames F     stop if screen unchanged F frames (default 240, 0=off)\n"
                     "  --battery-in FILE     restore the cartridge's save from FILE first\n"
                     "  --battery-out FILE    write the cartridge's save to FILE at the end\n";
        return false;
    }

    options.rom = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };

        if (flag == "--max-frames") {
            options.max_frames = std::atoi(next());
        } else if (flag == "--log-every") {
            options.log_every = std::max(1, std::atoi(next()));
        } else if (flag == "--press") {
            PressEvent event;
            const std::string spec = next();
            if (!parse_press(spec, event)) {
                std::cerr << "bad --press spec: " << spec << "\n";
                return false;
            }
            options.presses.push_back(event);
        } else if (flag == "--auto-start") {
            const std::string spec = next();
            int at = 0;
            int every = 0;
            int hold = 8;
            std::sscanf(spec.c_str(), "%d:%d:%d", &at, &every, &hold);
            options.auto_start_at = at;
            options.auto_start_every = every;
            options.auto_start_hold = hold > 0 ? hold : 8;
        } else if (flag == "--dump-every") {
            options.dump_every = std::atoi(next());
        } else if (flag == "--dump-dir") {
            options.dump_dir = next();
        } else if (flag == "--stall-pcs") {
            options.stall_pc_threshold = static_cast<std::size_t>(std::atoi(next()));
        } else if (flag == "--stall-frames") {
            options.stall_frames = std::atoi(next());
        } else if (flag == "--static-frames") {
            options.static_screen_frames = std::atoi(next());
        } else if (flag == "--trace") {
            options.trace_depth = std::min(4096, std::max(0, std::atoi(next())));
        } else if (flag == "--trap-mapper") {
            options.trap_mapper = true;
        } else if (flag == "--expect-control") {
            options.expected_control =
                static_cast<std::uint8_t>(std::strtol(next(), nullptr, 16));
        } else if (flag == "--watch-lt") {
            // ADDR:MIN (hex). Arms once the word at ADDR looks like a valid
            // pointer (>= MIN), then stops the moment it drops below MIN.
            const std::string spec = next();
            const auto colon = spec.find(':');
            if (colon == std::string::npos) {
                std::cerr << "bad --watch-lt spec (want ADDR:MIN): " << spec << "\n";
                return false;
            }
            options.watch_lt_addr =
                static_cast<std::uint16_t>(std::strtol(spec.substr(0, colon).c_str(), nullptr, 16));
            options.watch_lt_value =
                static_cast<std::uint16_t>(std::strtol(spec.substr(colon + 1).c_str(), nullptr, 16));
            options.watch_lt_enabled = true;
        } else if (flag == "--trap-sp-outside") {
            options.trap_sp_outside = true;
        } else if (flag == "--sp-range") {
            const std::string spec = next();
            const auto colon = spec.find(':');
            if (colon != std::string::npos) {
                options.sp_lo = static_cast<std::uint16_t>(
                    std::strtol(spec.substr(0, colon).c_str(), nullptr, 16));
                options.sp_hi = static_cast<std::uint16_t>(
                    std::strtol(spec.substr(colon + 1).c_str(), nullptr, 16));
            }
        } else if (flag == "--battery-in") {
            g_battery_in = next();
        } else if (flag == "--battery-out") {
            g_battery_out = next();
        } else if (flag == "--log-io") {
            g_log_io = true;
        } else if (flag == "--dump-layers") {
            g_dump_layers = true;
        } else if (flag == "--hot-pcs") {
            g_hot_pcs = true;
        } else if (flag == "--trace-port") {
            g_trace_port = static_cast<int>(std::strtol(next(), nullptr, 16));
        } else if (flag == "--dump-mem") {
            g_extra_dump_addr = static_cast<std::uint16_t>(std::strtol(next(), nullptr, 16));
            g_extra_dump = true;
        } else if (flag == "--gfx-report") {
            options.gfx_report = true;
        } else if (flag == "--log-vdp-regs") {
            options.log_vdp_regs = true;
        } else if (flag == "--log-display") {
            options.log_display = true;
        } else if (flag == "--trap-pc-in") {
            const std::string spec = next();
            const auto colon = spec.find(':');
            if (colon == std::string::npos) {
                std::cerr << "bad --trap-pc-in spec (want LO:HI): " << spec << "\n";
                return false;
            }
            options.pc_in_lo = static_cast<std::uint16_t>(
                std::strtol(spec.substr(0, colon).c_str(), nullptr, 16));
            options.pc_in_hi = static_cast<std::uint16_t>(
                std::strtol(spec.substr(colon + 1).c_str(), nullptr, 16));
            options.trap_pc_in = true;
        } else if (flag == "--trap-exec-ram") {
            options.trap_exec_ram = true;
        } else if (flag == "--exec-ram-min") {
            options.exec_ram_min =
                static_cast<std::uint16_t>(std::strtol(next(), nullptr, 16));
        } else if (flag == "--from-frame") {
            options.from_frame = std::atoi(next());
        } else if (flag == "--watch16") {
            // ADDR:VALUE, both hex. Arms once the word equals VALUE, then
            // stops the moment anything changes it.
            const std::string spec = next();
            const auto colon = spec.find(':');
            if (colon == std::string::npos) {
                std::cerr << "bad --watch16 spec (want ADDR:VALUE): " << spec << "\n";
                return false;
            }
            options.watch_addr =
                static_cast<std::uint16_t>(std::strtol(spec.substr(0, colon).c_str(), nullptr, 16));
            options.watch_value =
                static_cast<std::uint16_t>(std::strtol(spec.substr(colon + 1).c_str(), nullptr, 16));
            options.watch_enabled = true;
        } else {
            std::cerr << "unknown flag: " << flag << "\n";
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }

    GameGear game_gear;
    const auto load_result = game_gear.load_rom(options.rom);
    if (!load_result.ok) {
        std::cerr << "ROM load failed: " << load_result.error << "\n";
        return 1;
    }
    game_gear.reset();
    if (!g_battery_in.empty()) {
        std::ifstream in(g_battery_in, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
        game_gear.mutable_cartridge().restore_battery(bytes.data(), bytes.size());
        std::cout << "battery in: " << bytes.size() << " bytes from " << g_battery_in << "\n";
    }

    if (options.trace_depth > 0) {
        g_trace.assign(static_cast<std::size_t>(options.trace_depth), TraceEntry{});
        g_trace_next = 0;
        g_trace_wrapped = false;
    }

    std::cout << "gg_probe: " << options.rom.string() << "\n";
    std::cout << "max_frames=" << options.max_frames
              << " log_every=" << options.log_every
              << " stall(pcs<=" << options.stall_pc_threshold
              << " for " << options.stall_frames << "f)"
              << " static_screen=" << options.static_screen_frames << "f\n";
    for (const auto& press : options.presses) {
        std::cout << "  press " << press.name << " at frame " << press.frame
                  << " hold " << press.hold << "\n";
    }
    std::cout << "\n";

    FrameStats stats;
    int consecutive_small = 0;
    std::uint64_t irq_during_streak = 0;
    std::uint64_t halt_during_streak = 0;

    std::unordered_map<int, int> holds;  // button index -> frames remaining
    const auto button_index = [](Button button) { return static_cast<int>(button); };

    std::uint64_t previous_hash = 0;
    int static_streak = 0;
    bool previous_display = game_gear.vdp().display_enabled();
    std::uint64_t frames_without_irq = 0;
    bool mapper_trap_armed = false;
    bool watch_armed = false;
    bool watch_lt_armed = false;
    bool sp_trap_armed = false;
    std::uint32_t last_reg_write_count = 0;
    int reg_events = 0;
    std::uint32_t last_line_irq_count = 0;
    int gfx_max_colors = 0;
    int gfx_min_dominant = 100;
    int gfx_samples = 0;
    std::unordered_map<std::uint64_t, int> gfx_hashes;
    bool display_state = game_gear.vdp().display_enabled();
    int display_events = 0;

    for (int frame = 0; frame < options.max_frames; ++frame) {
        for (const auto& press : options.presses) {
            if (press.frame == frame) {
                game_gear.input_state().set_button(press.button, true);
                holds[button_index(press.button)] = press.hold;
                std::cout << "[frame " << frame << "] " << press.name << " DOWN\n";
            }
        }
        if (options.auto_start_every > 0 && options.auto_start_at >= 0 &&
            frame >= options.auto_start_at &&
            ((frame - options.auto_start_at) % options.auto_start_every) == 0) {
            game_gear.input_state().set_button(Button::start, true);
            holds[button_index(Button::start)] = options.auto_start_hold;
            std::cout << "[frame " << frame << "] start DOWN (auto)\n";
        }

        for (auto it = holds.begin(); it != holds.end();) {
            if (--it->second <= 0) {
                game_gear.input_state().set_button(static_cast<Button>(it->first), false);
                it = holds.erase(it);
            } else {
                ++it;
            }
        }

        last_line_irq_count = game_gear.vdp().debug_line_interrupt_count();
        stats.reset();
        const auto frame_at_entry = game_gear.vdp().frames_elapsed();
        bool hard_hang = false;

        while (game_gear.vdp().frames_elapsed() == frame_at_entry) {
            if (stats.steps >= options.max_steps_per_frame) {
                hard_hang = true;
                break;
            }

            const auto result = game_gear.step();
            ++stats.steps;

            if (!result.executed || result.unimplemented) {
                std::cout << "UNIMPLEMENTED: PC=" << hex16(result.pc_before)
                          << " prefix=" << hex8(result.prefix)
                          << " op=" << hex8(result.opcode)
                          << " msg=" << result.message << "\n";
                dump_state(game_gear, stats, frame, "unimplemented opcode");
                return 3;
            }

            auto& info = stats.pcs[result.pc_before];
            if (info.hits == 0) {
                info.opcode = result.opcode;
                info.prefix = result.prefix;
                info.message = result.message;
            }
            ++info.hits;

            if (result.message == "HALT state") {
                ++stats.halt_steps;
            }
            if (result.message.rfind("INT accepted", 0) == 0) {
                ++stats.irq_accepted;
            }

            trace_record(game_gear, result);
            record_port_read(game_gear, result);
            if (g_hot_pcs && frame >= options.from_frame) {
                ++g_pc_hits[result.pc_before];
            }

            if (options.log_vdp_regs && frame >= options.from_frame) {
                const auto writes = game_gear.vdp().register_write_count();
                // Cap generously: at 60 this silently truncated a raster
                // engine's per-frame register traffic and made "R10 is only
                // written once" look true when it is written every split.
                if (writes != last_reg_write_count && reg_events < 4000) {
                    last_reg_write_count = writes;
                    const auto index = game_gear.vdp().last_register_index();
                    const auto value = game_gear.vdp().last_register_value();
                    std::cout << "  [R" << static_cast<int>(index) << "=" << hex8(value)
                              << "] frame=" << frame
                              << " scanline=" << game_gear.vdp().debug_scanline()
                              << " pc=" << hex16(result.pc_before);
                    if (index == 0) {
                        std::cout << "   IE1(line)=" << (((value & 0x10) != 0) ? "ON" : "off");
                    } else if (index == 1) {
                        std::cout << "   display=" << (((value & 0x40) != 0) ? "ON" : "off")
                                  << " IE(frame)=" << (((value & 0x20) != 0) ? "ON" : "off");
                    } else if (index == 10) {
                        std::cout << "   line-IRQ every " << (static_cast<int>(value) + 1) << " lines";
                    }
                    std::cout << "\n";
                    ++reg_events;
                }
            }

            if (options.trap_sp_outside) {
                const auto sp = game_gear.cpu().registers().sp;
                const bool inside = sp >= options.sp_lo && sp <= options.sp_hi;
                // Arm once the game has established a sane stack, then stop
                // the instant SP leaves work RAM - the corruption itself,
                // rather than whatever wreckage follows it.
                if (!sp_trap_armed) {
                    if (inside) {
                        sp_trap_armed = true;
                    }
                } else if (!inside) {
                    std::cout << "\nSP TRAP: SP=" << hex16(sp) << " left "
                              << hex16(options.sp_lo) << ".." << hex16(options.sp_hi)
                              << " after PC=" << hex16(result.pc_before) << " op=";
                    if (result.prefix != 0x00) {
                        std::cout << hex8(result.prefix) << ":";
                    }
                    std::cout << hex8(result.opcode) << " (" << result.message << ")\n";
                    dump_state(game_gear, stats, frame, "stack pointer left work RAM");
                    return 11;
                }
            }

            if (options.log_display && frame >= options.from_frame) {
                const bool display_now = game_gear.vdp().display_enabled();
                if (display_now != display_state) {
                    display_state = display_now;
                    const auto scanline = game_gear.vdp().debug_scanline();
                    // GG shows the middle 144 lines of the 192-line active
                    // area, which starts at scanline 43.
                    const bool visible = scanline >= 67 && scanline < 211;
                    const bool active = scanline >= 43 && scanline < 235;
                    if (display_events < 400) {
                        std::cout << "  [disp " << (display_now ? "ON " : "OFF")
                                  << "] frame=" << frame
                                  << " scanline=" << scanline
                                  << " cyc=" << game_gear.vdp().debug_cycles_into_scanline()
                                  << " pc=" << hex16(result.pc_before)
                                  << (visible ? "   <== INSIDE GG VIEWPORT"
                                              : (active ? "   <== inside active area" : ""))
                                  << "\n";
                        ++display_events;
                    }
                }
            }

            if (options.trap_pc_in && frame >= options.from_frame &&
                result.pc_before >= options.pc_in_lo && result.pc_before <= options.pc_in_hi) {
                std::cout << "\nPC-RANGE TRAP: entered " << hex16(options.pc_in_lo) << ".."
                          << hex16(options.pc_in_hi) << " at PC=" << hex16(result.pc_before)
                          << " op=" << hex8(result.opcode) << " (" << result.message << ")\n";
                dump_state(game_gear, stats, frame, "PC entered the watched range");
                return 12;
            }

            if (options.trap_exec_ram && frame >= options.from_frame &&
                result.pc_before >= options.exec_ram_min) {
                std::cout << "\nEXEC-IN-RAM TRAP: PC=" << hex16(result.pc_before)
                          << " op=" << hex8(result.opcode)
                          << " (" << result.message << ")\n";
                dump_state(game_gear, stats, frame, "CPU began executing from work RAM");
                return 9;
            }

            if (options.watch_lt_enabled && frame >= options.from_frame) {
                const auto low = game_gear.bus().read8(options.watch_lt_addr);
                const auto high =
                    game_gear.bus().read8(static_cast<std::uint16_t>(options.watch_lt_addr + 1));
                const auto now = static_cast<std::uint16_t>((high << 8) | low);
                if (!watch_lt_armed) {
                    if (now >= options.watch_lt_value) {
                        watch_lt_armed = true;
                    }
                } else if (now < options.watch_lt_value) {
                    std::cout << "\nWATCH-LT TRAP: [" << hex16(options.watch_lt_addr)
                              << "] became " << hex16(now) << " (below "
                              << hex16(options.watch_lt_value) << ")"
                              << " by PC=" << hex16(result.pc_before)
                              << " op=";
                    if (result.prefix != 0x00) {
                        std::cout << hex8(result.prefix) << ":";
                    }
                    std::cout << hex8(result.opcode) << " (" << result.message << ")\n";
                    dump_state(game_gear, stats, frame, "watched pointer went out of range");
                    return 10;
                }
            }

            if (options.watch_enabled && frame >= options.from_frame) {
                const auto low = game_gear.bus().read8(options.watch_addr);
                const auto high =
                    game_gear.bus().read8(static_cast<std::uint16_t>(options.watch_addr + 1));
                const auto now = static_cast<std::uint16_t>((high << 8) | low);
                if (!watch_armed) {
                    if (now == options.watch_value) {
                        watch_armed = true;
                    }
                } else if (now != options.watch_value) {
                    std::cout << "\nWATCH TRAP: [" << hex16(options.watch_addr) << "] changed "
                              << hex16(options.watch_value) << " -> " << hex16(now)
                              << " by PC=" << hex16(result.pc_before)
                              << " op=";
                    if (result.prefix != 0x00) {
                        std::cout << hex8(result.prefix) << ":";
                    }
                    std::cout << hex8(result.opcode)
                              << " (" << result.message << ")\n";
                    dump_state(game_gear, stats, frame, "watched memory word changed");
                    return 8;
                }
            }

            if (options.trap_mapper) {
                const auto control = game_gear.cartridge().mapper_registers()[0];
                // Arm only once the game has installed its normal control
                // value, so the power-on default doesn't trip the trap.
                if (!mapper_trap_armed && control == options.expected_control) {
                    mapper_trap_armed = true;
                }
                if (mapper_trap_armed && control != options.expected_control) {
                    std::cout << "\nMAPPER TRAP: $FFFC became " << hex8(control)
                              << " (expected " << hex8(options.expected_control) << ")"
                              << " after PC=" << hex16(result.pc_before)
                              << " op=" << hex8(result.opcode)
                              << " (" << result.message << ")\n";
                    dump_state(game_gear, stats, frame,
                               "mapper control register written with an unexpected value");
                    return 7;
                }
            }
        }

        if (hard_hang) {
            dump_state(game_gear, stats, frame,
                       "no frame boundary reached within the per-frame step cap");
            return 4;
        }

        if (stats.irq_accepted == 0) {
            ++frames_without_irq;
        }

        const auto screen = screen_info(game_gear);
        if (frame > 0 && screen.hash == previous_hash) {
            ++static_streak;
        } else {
            static_streak = 0;
        }
        previous_hash = screen.hash;

        const bool display_now = game_gear.vdp().display_enabled();
        if (display_now != previous_display) {
            std::cout << "[frame " << frame << "] display -> "
                      << (display_now ? "ON" : "OFF") << "\n";
            previous_display = display_now;
        }

        if (stats.pcs.size() <= options.stall_pc_threshold) {
            ++consecutive_small;
            irq_during_streak += stats.irq_accepted;
            halt_during_streak += stats.halt_steps;
        } else {
            consecutive_small = 0;
            irq_during_streak = 0;
            halt_during_streak = 0;
        }

        if (consecutive_small >= options.stall_frames) {
            std::cout << "\nStall streak: " << consecutive_small << " frames"
                      << ", IRQs during streak=" << irq_during_streak
                      << ", HALT steps during streak=" << halt_during_streak << "\n";
            dump_state(game_gear, stats, frame, "execution collapsed into a tiny loop");
            return 5;
        }

        if (options.static_screen_frames > 0 && static_streak >= options.static_screen_frames) {
            std::cout << "\nScreen has been byte-identical for " << static_streak << " frames.\n";
            dump_state(game_gear, stats, frame, "presented framebuffer frozen");
            return 6;
        }

        // --from-frame also gates the BMP dump: capturing every frame of a long
        // run to reach a late window writes thousands of files for nothing.
        if (options.dump_every > 0 && frame >= options.from_frame &&
            (frame % options.dump_every) == 0) {
            auto directory = options.dump_dir.empty()
                                 ? std::filesystem::temp_directory_path()
                                 : options.dump_dir;
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const auto path = directory / ("probe_frame_" + std::to_string(frame) + ".bmp");
            if (!game_gear.vdp().export_visible_frame_bmp(path)) {
                std::cout << "  (frame " << frame << " bmp export failed)\n";
            }
            if (g_dump_layers) {
                // "Is this wall background or sprites, and is it misplaced in
                // its own layer?" cannot be answered from the composited
                // frame. The map export additionally shows the whole 256x224
                // tilemap, i.e. the picture before any scroll is applied.
                (void)game_gear.vdp().export_background_map_bmp(
                    directory / ("probe_map_" + std::to_string(frame) + ".bmp"));
                (void)game_gear.vdp().export_tilesheet_bmp(
                    directory / ("probe_tiles_" + std::to_string(frame) + ".bmp"));
            }
        }

        if (options.gfx_report && frame >= options.gfx_start_frame &&
            (frame % options.gfx_interval) == 0) {
            const auto gs = gfx_sample(game_gear);
            gfx_max_colors = std::max(gfx_max_colors, gs.distinct_colors);
            gfx_min_dominant = std::min(gfx_min_dominant, gs.dominant_percent);
            ++gfx_hashes[gs.hash];
            ++gfx_samples;
        }
        if ((frame % options.log_every) == 0) {
            std::cout << "f" << frame
                      << " steps=" << stats.steps
                      << " pcs=" << stats.pcs.size()
                      << " irq=" << stats.irq_accepted
                      << " halt=" << stats.halt_steps
                      << " disp=" << (display_now ? "on" : "off")
                      << " scr=" << (screen.solid ? "SOLID" : "varied")
                      << " static=" << static_streak
                      // D203 is where Sonic's IRQ handler latches the joypad
                      // byte it built from ports $00 and $DC (active low).
                      << " r8=" << hex8(game_gear.vdp().register_value(8))
                      << " r9=" << hex8(game_gear.vdp().register_value(9))
                      << " r10=" << hex8(game_gear.vdp().register_value(10))
                      // Line IRQs the VDP RAISED this frame vs irq= accepted:
                      // the gap is how much of a raster engine we are dropping.
                      << " lineRaised=" << (game_gear.vdp().debug_line_interrupt_count() - last_line_irq_count)
                      << " d203=" << hex8(game_gear.bus().read8(0xD203))
                      << " p00=" << hex8(game_gear.bus().read8(0xD200))
                      // The actual joypad byte the game would see this frame,
                      // active low. Distinguishes "press never reached the
                      // port" from "game read it and ignored it".
                      << " DC=" << hex8(game_gear.system_control().peek_port(0xC0))
                      << " banks=" << bank_text(game_gear)
                      << "\n";
        }
    }

    if (options.gfx_report) {
        const auto a = game_gear.vdp().debug_gfx_audit();
        const int unwritten_pct = a.visible_tiles > 0
            ? (a.tiles_unwritten * 100) / a.visible_tiles : 0;
        std::cout << "AUDIT unwrittenTilePct=" << unwritten_pct
                  << " distinctTiles=" << a.distinct_tile_indices
                  << " nameTbl=" << (a.name_table_written ? 1 : 0)
                  << " sprTbl=" << (a.sprite_table_written ? 1 : 0)
                  << " sprPat=" << (a.sprite_patterns_written ? 1 : 0)
                  << " sprites=" << a.active_sprites
                  << " sprOff=" << a.sprites_offscreen << "\n";
    }    if (options.gfx_report) {
        std::cout << "GFX maxColors=" << gfx_max_colors
                  << " minDominantPct=" << gfx_min_dominant
                  << " uniqueFrames=" << gfx_hashes.size()
                  << " samples=" << gfx_samples << "\n";
    }
    // The trap report also prints this, but a clean run never reaches that
    // path - and "which ports did the game read" is exactly the question you
    // ask about a game that runs fine yet ignores input.
    print_port_reads();
    print_trace_port();
    print_hot_pcs(game_gear);
    std::cout << "\nVDP interrupts raised: frame=" << game_gear.vdp().debug_frame_interrupt_count()
              << "  line=" << game_gear.vdp().debug_line_interrupt_count()
              << "  (per frame: "
              << (game_gear.vdp().debug_frame_interrupt_count() /
                  static_cast<std::uint32_t>(options.max_frames))
              << " / "
              << (game_gear.vdp().debug_line_interrupt_count() /
                  static_cast<std::uint32_t>(options.max_frames))
              << ")\n";
    std::cout << "\nCompleted " << options.max_frames << " frames with no stall detected.\n";
    std::cout << "Frames that accepted zero interrupts: " << frames_without_irq
              << " / " << options.max_frames << "\n";
    {
        const auto& cart = game_gear.cartridge();
        const auto& bytes = cart.battery_bytes();
        // The EEPROM's 128 bytes are the head of the store; count what moved
        // off the blank value there and in the whole store.
        const auto changed = [&](std::size_t count) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < count; ++i) {
                n += bytes[i] != 0xFF ? 1 : 0;
            }
            return n;
        };
        std::cout << "battery: dirty=" << (cart.battery_dirty() ? "yes" : "no")
                  << " eeprom_cart=" << (cart.uses_serial_eeprom() ? "yes" : "no")
                  << " non_blank_first128=" << changed(128)
                  << " non_blank_all=" << changed(bytes.size()) << "\n";
        if (cart.uses_serial_eeprom()) {
            const auto& a = cart.serial_eeprom().activity();
            std::cout << "eeprom: read=" << a.reads << " write=" << a.writes
                      << " erase=" << a.erases << " ewen=" << a.enables << " ewds=" << a.disables
                      << " eral=" << a.erase_all << " wral=" << a.write_all << "\n";
        }
        if (!g_battery_out.empty()) {
            std::ofstream out(g_battery_out, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            std::cout << "battery out: " << bytes.size() << " bytes to " << g_battery_out << "\n";
        }
    }
    return 0;
}







