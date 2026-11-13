// The Mega Drive VDP's V counter, as the 68000 reads it, pinned - and, at the
// end, when a line takes its horizontal scroll mode (Toy Story, issue #21).
//
// Interlace mode 2 reports the line number doubled, with the bit shifted out
// rotated back in at bit 0. Combat Cars (issue #19) times its two-player
// split by polling for that value - it rotates the line number itself before
// comparing - so a plain line number put the split at the wrong line and
// left the two woven fields holding different pictures.

#include "mercury/genesis/vdp.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check_value(unsigned got, unsigned want, const char* what) {
    const bool ok = got == want;
    std::printf("%-4s %-56s got 0x%02X want 0x%02X\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) {
        ++failures;
    }
}

// The V counter at the start of a given line, in a fresh machine with the
// given value in register 12 (mode set 4).
unsigned v_counter_at(unsigned line, std::uint8_t mode4) {
    mercury::genesis::Vdp vdp;
    vdp.reset();
    vdp.write_control(0x8104);  // mode 5, display off
    vdp.write_control(static_cast<std::uint16_t>(0x8C00 | mode4));
    vdp.advance_cycles(line * mercury::genesis::Vdp::kM68kCyclesPerLine + 4);
    return vdp.read_hv_counter() >> 8;
}

}  // namespace

int main() {
    // Progressive: the line number itself.
    check_value(v_counter_at(104, 0x81), 0x68, "progressive, line 104");
    check_value(v_counter_at(200, 0x81), 0xC8, "progressive, line 200");

    // Interlace mode 2 (register 12 bits 1-2 both set): doubled, rotated.
    check_value(v_counter_at(104, 0x87), 0xD0, "interlace mode 2, line 104 (Combat Cars' split)");
    check_value(v_counter_at(120, 0x87), 0xF0, "interlace mode 2, line 120");
    check_value(v_counter_at(200, 0x87), 0x91, "interlace mode 2, line 200: bit 7 comes back at bit 0");

    // Interlace mode 1 is left as it was - see read_hv_counter.
    check_value(v_counter_at(104, 0x83), 0x68, "interlace mode 1 is unchanged");

    // THE HORIZONTAL SCROLL MODE IS TAKEN WHEN THE LINE BEGINS. Toy Story
    // switches register 11 to per-line scroll from its H-interrupt handler,
    // which runs after the chip has fetched that line's scroll. The line must
    // still use the whole-screen value; the per-line table starts on the next.
    {
        using mercury::genesis::Vdp;
        Vdp vdp;
        vdp.reset();
        vdp.write_control(0x8104);  // mode 5
        vdp.write_control(0x8D30);  // horizontal scroll table at $C000
        vdp.write_control(0x8B00);  // whole-screen scroll
        // The table: entry 0 (whole screen) = 0x111, entry 10 = 0x2AA.
        const auto set_entry = [&vdp](unsigned line, std::uint16_t value) {
            const unsigned address = 0xC000 + line * 4;
            vdp.write_control(static_cast<std::uint16_t>(0x4000 | (address & 0x3FFF)));
            vdp.write_control(static_cast<std::uint16_t>((address >> 14) & 0x03));
            vdp.write_data(value);
        };
        set_entry(0, 0x111);
        set_entry(10, 0x2AA);
        set_entry(11, 0x2BB);
        vdp.advance_cycles(10 * Vdp::kM68kCyclesPerLine + 4);  // line 10 has begun
        vdp.write_control(0x8B03);  // the handler switches to per-line scroll
        vdp.advance_cycles(2 * Vdp::kM68kCyclesPerLine);  // lines 10 and 11 drawn
        check_value(vdp.line_records()[10].hscroll_a, 0x111,
                    "the line already begun keeps the whole-screen scroll");
        check_value(vdp.line_records()[11].hscroll_a, 0x2BB,
                    "the next line takes its own entry");
    }

    // A DATA-PORT WRITE UNDER A READ COMMAND STORES NOTHING. OutRunners'
    // H-interrupt handler runs one line past its table, picks up the command
    // 0000 0000 (read VRAM at 0) and writes a colour; that colour used to land
    // in tile 0, the sky's tile - a white dot in every tile.
    {
        using mercury::genesis::Vdp;
        Vdp vdp;
        vdp.reset();
        vdp.write_control(0x8104);  // mode 5
        vdp.write_control(0x8F02);  // auto-increment 2
        vdp.write_control(0x0000);  // VRAM read, address 0
        vdp.write_control(0x0000);
        vdp.write_data(0x00A0);
        check_value(vdp.vram()[1], 0x00, "a write under a VRAM read command is dropped");
        // The same write under a write command does land.
        vdp.write_control(0x4000);  // VRAM write, address 0
        vdp.write_control(0x0000);
        vdp.write_data(0x00A0);
        check_value(vdp.vram()[1], 0xA0, "a write under a VRAM write command lands");
    }

    std::printf("\n%s\n", failures == 0 ? "all V counter checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
