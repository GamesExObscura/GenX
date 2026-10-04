#include "mercury/genesis/game_quirks.hpp"

#include "mercury/genesis/cartridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace mercury::genesis {
namespace {

// Serials carry padding and inconsistent spacing between dumps of the same
// game, so compare on the alphanumerics only: "GM T-32063 -00" and
// "GM T-32063-00" have to match each other.
std::string squeeze(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

struct QuirkEntry {
    std::string_view serial;  // squeezed form
    GameQuirks quirks;
};

// One line per game, with the reason it is here. A game earns a line only after
// a measurement shows the default setting is what breaks it AND that the fix
// makes a difference - an entry that changes nothing is worse than no entry,
// because the next reader believes it.
//
// Games that were CONSIDERED and rejected, so the reasoning is not repeated:
// El. Viento looked like it needed charge_ea_time - it sat on a flat 2-colour
// screen without it - but it was only waiting for a button press, and once the
// test harness pressed one it reached 63.9% coverage either way.
constexpr std::array<QuirkEntry, 5> kTable{{
    // NBA Live 98 (USA), serial "GM T-172186-00". Its header declares no
    // battery RAM - the SRAM tag is two spaces - but the game saves rosters and
    // settings. Without a window mapped it reads its save area as open bus and
    // gets repeating byte patterns back: entering the rules screen faults with
    // an address error on a pointer of $2010201, and deleting a player faults
    // on $1010101. Both are odd addresses, which is what raises the fault.
    //
    // The ROM is exactly 2MB, so it ends at 0x200000 and a window there shadows
    // nothing. The generic undeclared-battery path cannot serve this: it keeps
    // its window two bytes wide on purpose, having shadowed real ROM and broken
    // other games when it was the full 64KB.
    //
    // It also drives its menus from a per-line H-interrupt chain: register 10
    // sits at 0 for the whole active area and the handler writes plane A's
    // vertical scroll on every scanline, which is how the pop-up selectors
    // (arcade/custom/simulation, rookie/starter/all star, the rules and top
    // users screens) get a window into the middle of the plane. At line 219 the
    // chain sets register 10 to 8 to stop the per-line interrupts, and the
    // vblank handler is supposed to set it back to 0 before the next frame.
    //
    // Measured: the V-interrupt IS taken every frame at line 224, but the
    // handler only reaches that write every OTHER frame - so alternate frames
    // run the whole screen at 9-line interrupt spacing instead of 1. At that
    // spacing the scroll window lands 3 lines low: the first list entry loses
    // its top rows and a ghost copy of it appears above the list, which is
    // exactly the "extra arcade near the top" and the corruption over "top
    // users". Frames alternate good/bad, so on screen it reads as both at once.
    //
    // Running the line counter through vblank hands the chain the interrupt it
    // is waiting for and every frame then renders identically and correctly -
    // measured over frames 4350-4355, all six clean. This is a workaround, not
    // the cure: the real defect is whatever delays that vblank write by a frame,
    // and it is worth another look once there is a way to trace the handler.
    // charge_ea_time was tried here too - the comment on ea_cycles() names this
    // game - and it changed which frame was bad without stopping the alternation.
    // Fields must be listed in declaration order - a designated initialiser out
    // of order is a compile error, not a silent misassignment, which is the
    // whole reason this table stopped using positional initialisers.
    {"GMT17218600", GameQuirks{.force_sram = true,
                               .sram_start = 0x200000, .sram_end = 0x20FFFF,
                               .hint_run_in_vblank = true}},

    // Lemmings 2 - The Tribes (USA), serial "GM T-113036-00". Runs with V-int
    // disabled and drives everything from a six-stage H-interrupt chain, which
    // it phase-locks exactly once - by setting register 10 to 1, watching for
    // the V counter to read 0xCF, and only then installing the real handler.
    // After that the chain free-runs: the handler pointer is rewritten 13 times
    // in 1500 frames. Stage 5 blanks the display and sets register 10 to 7,
    // stage 0 un-blanks it and sets 0x2F back.
    //
    // That only holds still if the chain consumes a whole number of interrupts
    // per frame. Under the documented counter rules it gets 4.91, so the chain
    // completes 0.82 cycles a frame and the 8-line blank walks the screen -
    // measured landing on lines 47, 95, 143 and 191, which is the flicker, and
    // 960 of 1225 blanks fell outside the window the game's own code declares
    // safe. Frames went fully black where the blank swallowed the picture.
    //
    // Both switches are needed and neither helps alone: separately the blank
    // still wandered over four lines in 12-14 of 14 sampled frames, together in
    // 5, and the intro renders instead of stalling.
    // Named fields, not positional. A comment saying which field a value is
    // meant for is not checked by anything, and a mismatch here is invisible:
    // the game just behaves as if the switch had never been set.
    // Named fields: the same table used positional initialisers and a
    // mismatch there is invisible - the game simply behaves as if the switch
    // were never set, which cost hours of chasing the wrong subsystem.
    {"GMT11303600", GameQuirks{.hint_reload_on_write = true,
                               .hint_run_in_vblank = true,
                               .blank_display_edges = true,
                               .ignore_midscreen_blank = true}},

    // Fatal Rewind (USA, Europe) (Rev A), serial "GM T-50256 -00". Enables HINT
    // and then VINT on consecutive instructions three scanlines before vblank,
    // and has to reach the code that installs its VINT handler before that
    // vblank arrives - otherwise the handler dispatches through a null pointer
    // and the game jumps to 0x000000. How much of that code fits in three lines
    // is decided by instruction timing.
    {"GMT5025600", GameQuirks{.charge_ea_time = true}},

    // Time Killers (USA), serial "GM T-100046-00". Its H-interrupt raster chain
    // has five stages but only four fit in a frame at the intervals it programs
    // - unless the interrupt is raised on the reload line, which yields one
    // extra interrupt at the previous spacing. Without it the final stage never
    // runs, the tick its whole task scheduler is gated on never advances, and
    // the screen stays black. Applied per-game because Lemmings 2 breaks under
    // the same rule.
    // (charge_ea_time was also tried here - it is reported to "need accurate
    // timing" - and measured to change nothing.)
    {"GMT10004600", GameQuirks{.hint_raise_on_reload = true}},

    // HardBall III (USA) (Unl), serial "GM ACLD012 -00". An Accolade cartridge
    // with battery RAM at 0x200000-0x20FFFF that its header declares as absent.
    // It probes the top of that window at boot - one write to 0x20FFFF and four
    // reads back - and when the value does not survive it reruns first-time
    // setup, prints "First time initialization complete. Press RESET to
    // continue" and waits there forever, because the reset finds nothing saved.
    // The generic undeclared-battery path cannot cover this: it deliberately
    // keeps its window two bytes wide, having shadowed real ROM and broken
    // other games when it was the full 64KB.
    {"GMACLD01200", GameQuirks{.force_sram = true,
                               .sram_start = 0x200000, .sram_end = 0x20FFFF}},
}};

}  // namespace

GameQuirks quirks_for(const RomHeader& header) noexcept {
    const std::string serial = squeeze(header.serial);
    const auto match = std::find_if(kTable.begin(), kTable.end(), [&](const QuirkEntry& entry) {
        return entry.serial == serial;
    });
    return match != kTable.end() ? match->quirks : GameQuirks{};
}

}  // namespace mercury::genesis

















