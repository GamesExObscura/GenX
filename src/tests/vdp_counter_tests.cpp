// The Mega Drive VDP's V counter, as the 68000 reads it, pinned - and, at the
// end, when a line takes its horizontal scroll mode (Toy Story, issue #21),
// and where the DMA source registers are left after a transfer (issue #22).
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

    // A NAME TABLE SPANS 8KB AT MOST. Trouble Shooter's intro sets plane size
    // 128x64 - more than 4096 cells - and scrolls plane A down 256 lines to
    // reach its portrait. On hardware row 32 of a 128-wide plane wraps back to
    // row 0; read as a real 64-row plane it ran on into plane B's table and the
    // centre portrait was missing.
    {
        using mercury::genesis::Vdp;
        Vdp vdp;
        vdp.reset();
        vdp.write_control(0x8144);  // display on, mode 5
        vdp.write_control(0x8C81);  // H40
        vdp.write_control(0x8230);  // plane A at $C000
        vdp.write_control(0x8407);  // plane B at $E000
        vdp.write_control(0x9013);  // 128 x 64 cells: over the 8KB a table can span
        vdp.write_control(0x8F02);  // auto-increment 2
        vdp.write_control(0xC002);  // CRAM entry 1 ...
        vdp.write_control(0x0000);
        vdp.write_data(0x000E);     // ... bright red
        vdp.write_control(0x4020);  // tile 1, every pixel colour 1
        vdp.write_control(0x0000);
        for (int i = 0; i < 16; ++i) vdp.write_data(0x1111);
        vdp.write_control(0x4000);  // plane A, row 0, column 0 -> tile 1
        vdp.write_control(0x0003);
        vdp.write_data(0x0001);
        vdp.write_control(0x4000);  // VSRAM plane A = 256: row 32 at the top
        vdp.write_control(0x0010);
        vdp.write_data(0x0100);
        vdp.advance_cycles(Vdp::kM68kCyclesPerLine + 4);  // line 0 drawn
        const auto& rgb = vdp.frame_pixels();
        check_value(rgb.size() >= 3 ? rgb[0] : 0, 0xFF,
                    "row 32 of a 128x64 plane wraps to row 0 (red pixel, not plane B)");
    }

    // THE DMA SOURCE REGISTERS COUNT ALONG WITH THE TRANSFER. Charles
    // MacDonald's 1536 colour demo (issue #22) sets R21-R23 once a frame, then
    // on every line reloads only the length and DMAs four colours into CRAM.
    // Each DMA must carry on from where the last one stopped; with the
    // registers left unchanged, every line got the same four colours.
    {
        using mercury::genesis::Vdp;
        Vdp vdp;
        vdp.reset();
        static const std::uint16_t colours[8] = {0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00E, 0x020};
        vdp.set_memory_reader([](std::uint32_t address) -> std::uint16_t {
            const std::uint32_t n = (address - 0x00051E) / 2;
            return n < 8 ? colours[n] : 0x0EEE;
        });
        vdp.write_control(0x8114);  // mode 5, DMA enabled, display off
        vdp.write_control(0x8F02);  // auto-increment 2
        vdp.write_control(0x958F);  // source $00051E (word address $00028F)
        vdp.write_control(0x9602);
        vdp.write_control(0x9700);
        vdp.write_control(0x9304);  // four words
        vdp.write_control(0x9400);
        vdp.write_control(0xC008);  // CRAM colour 4, DMA
        vdp.write_control(0x0080);
        check_value(vdp.cram()[4], 0x002, "first DMA: colour 4 from $00051E");
        check_value(vdp.cram()[7], 0x008, "first DMA: colour 7 from $000524");
        check_value(vdp.registers()[0x15], 0x93, "R21 points past the block ($000526)");
        check_value(vdp.registers()[0x16], 0x02, "R22 points past the block");
        check_value(vdp.registers()[0x17], 0x00, "R23 is left alone");
        vdp.write_control(0x9304);  // the next line: only the length is reloaded
        vdp.write_control(0x9400);
        vdp.write_control(0xC010);  // CRAM colour 8, DMA
        vdp.write_control(0x0080);
        check_value(vdp.cram()[8], 0x00A, "second DMA carries on: colour 8 from $000526");
        check_value(vdp.cram()[11], 0x020, "second DMA carries on: colour 11 from $00052C");
    }

    // A VRAM COPY LEAVES ITS SOURCE PAST THE COPIED BYTES TOO.
    {
        using mercury::genesis::Vdp;
        Vdp vdp;
        vdp.reset();
        vdp.write_control(0x8114);  // mode 5, DMA enabled
        vdp.write_control(0x8F02);
        vdp.write_control(0x5000);  // VRAM write at $1000: AA BB CC DD
        vdp.write_control(0x0000);
        vdp.write_data(0xAABB);
        vdp.write_data(0xCCDD);
        vdp.write_control(0x8F01);  // auto-increment 1 for the copy
        vdp.write_control(0x9500);  // copy source $1000
        vdp.write_control(0x9610);
        vdp.write_control(0x97C0);  // VRAM copy mode
        vdp.write_control(0x9302);  // two bytes
        vdp.write_control(0x9400);
        vdp.write_control(0x2000);  // destination $2000, copy
        vdp.write_control(0x00C0);
        check_value(vdp.vram()[0x2000], 0xAA, "first copy: byte $2000");
        check_value(vdp.vram()[0x2001], 0xBB, "first copy: byte $2001");
        check_value(vdp.registers()[0x15], 0x02, "copy source R21 points past the block ($1002)");
        check_value(vdp.registers()[0x16], 0x10, "copy source R22 points past the block");
        vdp.write_control(0x9302);  // only the length is reloaded
        vdp.write_control(0x9400);
        vdp.write_control(0x2002);  // destination $2002, copy
        vdp.write_control(0x00C0);
        check_value(vdp.vram()[0x2002], 0xCC, "second copy carries on: byte $2002");
        check_value(vdp.vram()[0x2003], 0xDD, "second copy carries on: byte $2003");
    }

    std::printf("\n%s\n", failures == 0 ? "all V counter checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
