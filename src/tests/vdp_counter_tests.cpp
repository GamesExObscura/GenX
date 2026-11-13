// The Mega Drive VDP's V counter, as the 68000 reads it, pinned.
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

    std::printf("\n%s\n", failures == 0 ? "all V counter checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
