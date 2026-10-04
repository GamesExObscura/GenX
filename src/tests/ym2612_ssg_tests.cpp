// SSG-EG tests for the YM2612.
//
// SSG-EG turns an operator's envelope from a one-shot decay into a repeating
// one: while it is active the envelope falls four times as fast, and on
// reaching the halfway point it loops, inverts or holds. An emulator that
// stores the register and ignores it - which this one did - plays those sounds
// four times too slowly and without the repeat, which is heard as a sound that
// is stretched out.
//
// The test drives the chip directly rather than through a game, because whether
// any particular ROM happens to use SSG-EG in its first few seconds is not
// something a regression test should depend on.

#include "mercury/genesis/ym2612.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(const char* what, bool ok, const char* detail = "") {
    if (ok) {
        std::printf("ok   %s\n", what);
        return;
    }
    std::printf("FAIL %s  %s\n", what, detail);
    ++failures;
}

// Address/data pair on port 0, which covers channels 1-3.
void write_reg(mercury::genesis::Ym2612& ym, std::uint8_t address, std::uint8_t value) {
    ym.write(0, address);
    ym.write(1, value);
}

// A single channel set up as one audible carrier, with a decay slow enough that
// the four-times speed-up and the loop are both easy to see.
void set_up_channel(mercury::genesis::Ym2612& ym, std::uint8_t ssg_eg) {
    ym.reset();
    // Algorithm 7: all four operators in parallel, so every one is a carrier and
    // nothing depends on modulation depth.
    write_reg(ym, 0xB0, 0x07);
    // Both speakers.
    write_reg(ym, 0xB4, 0xC0);
    for (std::uint8_t slot = 0; slot < 4; ++slot) {
        const auto op = static_cast<std::uint8_t>(slot * 4);
        write_reg(ym, static_cast<std::uint8_t>(0x30 + op), 0x01);  // detune 0, multiple 1
        write_reg(ym, static_cast<std::uint8_t>(0x40 + op), 0x00);  // total level: loudest
        write_reg(ym, static_cast<std::uint8_t>(0x50 + op), 0x1F);  // attack: instant
        // Decay 0x14. It has to be fast enough that the envelope actually
        // reaches the halfway point inside the test window - at 0x08 it never
        // gets there, every SSG-EG mode looks identical, and the test says
        // nothing. The rate shift is 11 - (2*DR >> 2), so DR 8 steps once per
        // 128 envelope ticks and DR 20 steps every other tick.
        write_reg(ym, static_cast<std::uint8_t>(0x60 + op), 0x14);
        write_reg(ym, static_cast<std::uint8_t>(0x70 + op), 0x00);  // sustain rate 0
        // Sustain level is the HIGH nibble; release is the low one. Writing
        // 0x0F here sets SL=0, which ends the decay immediately and freezes the
        // envelope at full volume - the envelope then never reaches the
        // halfway point and every SSG-EG mode looks alike.
        write_reg(ym, static_cast<std::uint8_t>(0x80 + op), 0xFF);
        write_reg(ym, static_cast<std::uint8_t>(0x90 + op), ssg_eg);
    }
    // A mid-range note.
    write_reg(ym, 0xA4, 0x22);
    write_reg(ym, 0xA0, 0x00);
    // Key on, all four operators of channel 1.
    write_reg(ym, 0x28, 0xF0);
}

// Total energy in the output, which stands in for "how long the note lasted".
double energy(mercury::genesis::Ym2612& ym, int m68k_cycles) {
    ym.clear_sample_buffer();
    ym.advance_cycles(static_cast<std::uint32_t>(m68k_cycles));
    double sum = 0.0;
    for (const auto sample : ym.sample_buffer()) {
        sum += std::fabs(static_cast<double>(sample));
    }
    return sum;
}

}  // namespace

int main() {
    using mercury::genesis::Ym2612;

    // A quarter of a second or so of chip time.
    constexpr int kCycles = 2'000'000;

    Ym2612 plain;
    set_up_channel(plain, 0x00);
    const double plain_energy = energy(plain, kCycles);

    Ym2612 looping;
    // Enable + alternate: the classic repeating SSG-EG setting.
    set_up_channel(looping, 0x0A);
    const double looping_energy = energy(looping, kCycles);

    Ym2612 held;
    // Enable + hold: decays once and stops, so it must NOT keep sounding.
    set_up_channel(held, 0x09);
    const double held_energy = energy(held, kCycles);

    std::printf("energy: plain %.0f  looping %.0f  held %.0f\n", plain_energy, looping_energy,
                held_energy);

    check("a plain envelope produces output", plain_energy > 0.0);

    // The whole point: with SSG-EG looping, the note keeps restarting, so over
    // the same span it carries decisively more energy than a single decay.
    char detail[160];
    std::snprintf(detail, sizeof(detail), "plain %.0f vs looping %.0f", plain_energy,
                  looping_energy);
    check("SSG-EG looping changes the envelope at all", looping_energy != plain_energy, detail);
    check("SSG-EG looping sustains the note rather than decaying once",
          looping_energy > plain_energy * 1.2, detail);

    // Hold is the opposite end: it must behave differently from the looping
    // mode, or the mode bits are not being read.
    std::snprintf(detail, sizeof(detail), "held %.0f vs looping %.0f", held_energy,
                  looping_energy);
    check("SSG-EG hold differs from SSG-EG loop", held_energy != looping_energy, detail);

    // The enable bit has to matter: 0x04 is the invert bit with enable CLEAR,
    // which the chip treats as off entirely.
    Ym2612 disabled;
    set_up_channel(disabled, 0x04);
    const double disabled_energy = energy(disabled, kCycles);
    std::snprintf(detail, sizeof(detail), "disabled %.0f vs plain %.0f", disabled_energy,
                  plain_energy);
    check("SSG-EG with the enable bit clear behaves as off", disabled_energy == plain_energy,
          detail);

    // --- decay rate 0 with sustain level 0 ---------------------------------
    //
    // Sonic 3's lightning shield patch: attack 31, decay rate 0, sustain level
    // 0, a real sustain rate. The envelope is already at the sustain level when
    // the note starts, so the operator must move straight to sustain and fall
    // at the SUSTAIN rate. Reading the decay rate first and bailing out on zero
    // left it in decay for ever, holding the note at full volume until key-off
    // - a flat tone where there should be a fade.
    Ym2612 fading;
    fading.reset();
    write_reg(fading, 0xB0, 0x07);
    write_reg(fading, 0xB4, 0xC0);
    for (std::uint8_t slot = 0; slot < 4; ++slot) {
        const auto op = static_cast<std::uint8_t>(slot * 4);
        write_reg(fading, static_cast<std::uint8_t>(0x30 + op), 0x01);
        write_reg(fading, static_cast<std::uint8_t>(0x40 + op), 0x00);
        write_reg(fading, static_cast<std::uint8_t>(0x50 + op), 0x1F);  // attack 31
        write_reg(fading, static_cast<std::uint8_t>(0x60 + op), 0x00);  // decay rate 0
        write_reg(fading, static_cast<std::uint8_t>(0x70 + op), 0x0D);  // sustain rate 13
        write_reg(fading, static_cast<std::uint8_t>(0x80 + op), 0x0F);  // SL 0, RR 15
        write_reg(fading, static_cast<std::uint8_t>(0x90 + op), 0x00);
    }
    write_reg(fading, 0xA4, 0x22);
    write_reg(fading, 0xA0, 0x00);
    write_reg(fading, 0x28, 0xF0);

    // Two halves of the note, held throughout - no key-off, exactly as the
    // game does it.
    const double first_half = energy(fading, 500'000);
    const double second_half = energy(fading, 500'000);
    std::printf("decay-0 patch: first half %.0f  second half %.0f\n", first_half, second_half);
    check("a decay-rate-0, sustain-level-0 note makes sound at all", first_half > 0.0);
    std::snprintf(detail, sizeof(detail), "first %.0f vs second %.0f", first_half, second_half);
    check("it FADES while held rather than sitting at a constant level",
          second_half < first_half * 0.7, detail);

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall SSG-EG checks passed\n");
    return 0;
}
