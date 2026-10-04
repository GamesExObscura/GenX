#pragma once

#include <cstdint>
#include <string>

namespace mercury::genesis {

struct RomHeader;

// Per-game behaviour switches.
//
// Some games only run under a setting that costs OTHER games if applied
// library-wide. Rather than pick one side of that trade globally, the machine
// looks the game up here at load time. Everything not named gets the defaults,
// which are the settings that boot the most of the library.
//
// This file is deliberately self-contained: adding a game here must never
// change behaviour for a game that is not listed.
struct GameQuirks {
    // Charge the 68000's real effective-address calculation time, making the
    // CPU slower relative to the video chip - and, on a Sega CD, relative to
    // the sub CPU and the disc.
    //
    // DEFAULT ON since 2026-09-19. It used to be off "because it costs working
    // games", naming Steel Empire and NBA Live 98; both of those claims have
    // since been disproven by measurement (Steel Empire had no bug at all, NBA
    // Live 98's menu fault changed shape but did not depend on this). Against
    // the ProcessorTests vectors, off is wrong on 292,752 of 653,265
    // instructions - every one charged too FEW cycles - and on is wrong on
    // 59,143. Running ~30% fast is not free: The Lawnmower Man (Sega CD) sat
    // on its title screen because its sub CPU outran the disc.
    bool charge_ea_time = true;

    // Battery RAM the cartridge really has but its header does not declare.
    // Mapping a window like this for every cartridge would shadow ROM that
    // other games legitimately read, so it is opt-in by game. Inclusive range;
    // ignored unless force_sram is set.
    bool force_sram = false;
    std::uint32_t sram_start = 0;
    std::uint32_t sram_end = 0;

    // Raise the H-interrupt on the line the counter reloads, rather than on the
    // line before. Both give the same interval, but the reload-line form uses
    // the register value from BEFORE the handler ran, so a handler that
    // reprograms the interval gets one extra interrupt at the old spacing.
    // Time Killers needs that extra interrupt; Lemmings 2 breaks on it, because
    // it runs with V-int disabled and nothing resets its raster chain, so the
    // extra interrupt walks the chain one stage too far. Per-game until the
    // real difference from the reference implementations is found.
    bool hint_raise_on_reload = false;

    // Writing register 10 restarts the countdown instead of only supplying the
    // next reload value. A handler that blanks the display, sets a short
    // interval and expects the matching un-blank one interval later gets it;
    // without this the short interval does not apply until the interval after
    // next and the blank lasts six times as long as the game intended.
    bool hint_reload_on_write = false;

    // Keep the line counter running through vblank rather than holding it
    // reloaded outside the active display. This is NOT what the hardware
    // reference says, and it is here for one game: Lemmings 2 free-runs a
    // six-stage raster chain with V-int disabled and re-phases it only 13 times
    // in 1500 frames, so the chain has to consume a whole number of interrupts
    // per frame or it walks down the screen. Under the documented rule it gets
    // 4.9 and drifts; running the counter through vblank gives it the six it
    // was written for. Never apply this library-wide.
    bool hint_run_in_vblank = false;

    // Do not display the first and last eight visible lines; show the backdrop
    // there instead.
    //
    // Lemmings 2 scrolls both planes to positions where they WRAP inside the
    // picture: plane A at 296 wraps at line 216, plane B at 1016 wraps at line
    // 8. What shows through is real data the game put there on purpose - the
    // previous scene's bottom row of artwork at the top, an opaque filler tile
    // at the bottom - which is why it reads as a bright band in one scene and a
    // purple band in another. Hardware shows black in both bands.
    //
    // The mechanism is NOT identified. The obvious candidate - the game's own
    // 8-line display blank - is ruled out by measurement: reg10 only ever takes
    // the values 0x2F, 0x07 and 0x01, so with the counter reloaded at line 0 the
    // blank can only ever begin on line 47, 95, 143, 191 or 239, and covering
    // line 216 needs a stage on line 215 that no interval can produce. Skipping
    // the line-0 reload does give that phase, but it also fixes the parity of
    // every raise line, and the game locks its raster chain by hunting for one
    // specific ODD value of the V counter (0xCF) - so the hunt then never
    // catches and the game sits in its 2-line search mode forever.
    //
    // This reproduces the reference output exactly on every scene tested, and is
    // deliberately confined to one serial until the real cause is found.
    bool blank_display_edges = false;

    // Keep drawing a line whose display-enable bit is off, when that line is in
    // the visible picture.
    //
    // Lemmings 2 blanks the display only to make a transfer fast, and it aims
    // that blank at vblank - measured landing on line 239 in 812 of 880 frames.
    // The rest slip: the un-blank interrupt at line 245 is raised while the one
    // from 238 is still unserviced, so it is dropped, register 10 keeps the
    // short interval, and the following frame blanks on line 190 instead - which
    // is inside the status panel and shows as a one-frame black band.
    //
    // The underlying fault is that the 68000 fails to service an interrupt for
    // more than seven scanlines. DMA cost is a third of it (making transfers
    // free drops the slip rate from 7.2% to 4.1%) and the remainder is not yet
    // identified. Until it is, this stops the symptom reaching the screen: the
    // blank exists to speed a transfer, so ignoring it mid-picture costs the
    // game nothing it can observe.
    bool ignore_midscreen_blank = false;
};

// Matched on the header serial (e.g. "GM T-32063 -00"), falling back to the
// domestic title when a serial is missing or shared across a series.
[[nodiscard]] GameQuirks quirks_for(const RomHeader& header) noexcept;

}  // namespace mercury::genesis







