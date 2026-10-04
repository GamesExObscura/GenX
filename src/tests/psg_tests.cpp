// SN76489 noise-rate tests.
//
// The four noise shift-rate settings are not four independent numbers on
// hardware. All four load the same counter, which is clocked from the same
// place; settings 0, 1 and 2 load 0x10, 0x20 and 0x40, and setting 3 loads
// whatever tone channel 2 is loaded with. So the shift rate is always
// clock / (32 * load), and setting 3 with tone 2 at 0x10 has to come out
// identical to setting 0. That equivalence is the whole test: it cannot be
// satisfied by a formula that scales one case and not the others.
//
// This matters because the error is close to inaudible in music - Sonic 3's
// percussion is sampled, not noise - and glaring in sound effects, which are
// where the swept noise lives.

#include "mercury/gamegear/psg.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check_close(const char* what, double actual, double expected) {
    // A tenth of a percent: these are exact ratios of one clock, so anything
    // beyond floating-point noise is a real difference.
    const double tolerance = expected * 0.001;
    if (std::fabs(actual - expected) > tolerance) {
        std::printf("FAIL %-44s got %10.3f Hz, expected %10.3f Hz  (x%.3f)\n", what, actual,
                    expected, expected == 0.0 ? 0.0 : actual / expected);
        ++failures;
        return;
    }
    std::printf("ok   %-44s %10.3f Hz\n", what, actual);
}

// Latch the noise control register: 1 11 0 -xxx.
void write_noise_control(mercury::gamegear::Psg& psg, int feedback, int rate) {
    psg.write(static_cast<std::uint8_t>(0xE0 | ((feedback & 1) << 2) | (rate & 3)));
}

// Latch then complete tone channel 2's ten-bit period.
void write_tone2_period(mercury::gamegear::Psg& psg, int period) {
    psg.write(static_cast<std::uint8_t>(0xC0 | (period & 0x0F)));
    psg.write(static_cast<std::uint8_t>((period >> 4) & 0x3F));
}

}  // namespace

int main() {
    using mercury::gamegear::Psg;
    constexpr double clock = Psg::kBaseClockHz;

    Psg psg;
    psg.reset();

    // The three fixed rates.
    write_noise_control(psg, 1, 0);
    check_close("white noise, rate 0", psg.noise_shift_rate_hz(), clock / 512.0);
    write_noise_control(psg, 1, 1);
    check_close("white noise, rate 1", psg.noise_shift_rate_hz(), clock / 1024.0);
    write_noise_control(psg, 1, 2);
    check_close("white noise, rate 2", psg.noise_shift_rate_hz(), clock / 2048.0);

    // Rate 3 takes tone channel 2's counter load, unscaled. The periods with a
    // zero low nibble are the ones that catch a clamp applied to the register
    // rather than to its use: the low-nibble write leaves the value at zero
    // half-way through, and anything that "fixes" it there corrupts the second
    // write.
    for (const int period : {0x010, 0x020, 0x040, 0x080, 0x100, 0x0C7}) {
        char label[64];
        std::snprintf(label, sizeof(label), "rate 3, tone 2 period 0x%03X", period);
        write_tone2_period(psg, period);
        write_noise_control(psg, 1, 3);
        check_close(label, psg.noise_shift_rate_hz(), clock / (32.0 * period));
    }

    // The equivalence that pins it: rate 3 with tone 2 loaded to 0x10, 0x20 and
    // 0x40 must reproduce rates 0, 1 and 2 exactly.
    const int loads[3] = {0x10, 0x20, 0x40};
    for (int rate = 0; rate < 3; ++rate) {
        write_noise_control(psg, 1, rate);
        const double fixed = psg.noise_shift_rate_hz();
        char label[64];
        std::snprintf(label, sizeof(label), "rate 3 at load 0x%02X matches fixed rate %d",
                      loads[rate], rate);
        write_tone2_period(psg, loads[rate]);
        write_noise_control(psg, 1, 3);
        check_close(label, psg.noise_shift_rate_hz(), fixed);
    }

    // Writing the noise register reloads the shift register, so a sound effect
    // that retriggers noise starts from the same place every time rather than
    // wherever the previous one left off.
    psg.reset();
    write_noise_control(psg, 1, 0);
    psg.advance_cycles(20000);
    const auto before = psg.sample_buffer().size();
    write_noise_control(psg, 1, 0);
    psg.advance_cycles(20000);
    if (psg.sample_buffer().size() <= before) {
        std::printf("FAIL noise produced no samples after a retrigger\n");
        ++failures;
    } else {
        std::printf("ok   noise still runs after a retrigger\n");
    }

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall PSG noise checks passed\n");
    return 0;
}
