#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mercury::core {

// GAME GENIE CODES, for the Mega Drive and for the Master System / Game Gear.
//
// The two devices are not the same machine and their codes are not the same
// format. Both are decoded here because both end up doing the same thing -
// substituting a value the processor would otherwise have read out of the
// cartridge - and keeping the two decoders side by side is what makes it
// obvious which rules belong to which.
//
//   Mega Drive   ABCD-EFGH, eight symbols from a 32-letter alphabet, five
//                bits each. Forty bits scrambled into a 24-bit cartridge
//                address and the 16-bit WORD to put there. No compare value:
//                the device sits in the cartridge slot and simply answers
//                that address itself.
//
//   Master /     ABC-DEF or ABC-DEF-GHI, plain hexadecimal. A byte, a 16-bit
//   Game Gear    processor address whose top nibble is inverted, and - in the
//                nine-digit form - a check byte, rotated and XORed, that the
//                byte already there has to match before the substitution
//                happens. The check exists because these codes address the
//                Z80's map, where a bank switch can put something else at the
//                same address.
//
// The bit layouts below are the published formats. They are not guessable and
// they are not derived from anything here: they are what the hardware does.
struct Cheat {
    // As typed, upper-cased, separators kept - this is what goes back on the
    // screen and into the file, so it stays in the form a person recognises.
    std::string code;
    // Their own note about what it does. Codes are unreadable by design.
    std::string description;
    bool enabled = true;

    std::uint32_t address = 0;
    std::uint16_t value = 0;
    // What the byte at that address has to be for the substitution to happen,
    // or -1 for "whatever is there". Only the nine-digit Master System /
    // Game Gear form carries one.
    int compare = -1;
    // A Mega Drive code replaces a word of cartridge ROM. A Master System or
    // Game Gear code replaces a byte at a processor address.
    bool word = false;

    // What was there before it was applied, so turning it off puts the game
    // back without reloading. Only meaningful while `applied`.
    std::uint16_t original = 0;
    bool applied = false;
};

// WHICH FORMAT A CODE IS IN IS A PROPERTY OF THE CODE, NOT OF THE MACHINE.
//
// Length settles it and there is no overlap: a Mega Drive code is always
// eight symbols, a Master System or Game Gear code always six or nine. So
// `automatic` is the right default and the only one the dialog needs -
// asking the machine instead was a bug, because with no game loaded the
// machine is whatever it last was and the answer came out wrong.
//
// The explicit values stay for callers that know, and for saying "that is a
// Mega Drive code and this is a Game Gear" rather than silently taking it.
enum class CheatFormat {
    automatic,
    mega_drive,
    master_system,
};

// Decode one code. Returns false and fills `error` with something a person
// can act on rather than "invalid code".
[[nodiscard]] bool decode_cheat(std::string_view text, CheatFormat format, Cheat& out,
                                std::string& error);

// Split a line that may hold several codes. Published lists join the codes of
// a multi-part cheat with " + " - Sonic 3's list has "SCGA-BL0N + A4GA-A28T" -
// and someone pasting that in means both of them, not one broken one.
// Commas and semicolons are taken the same way.
[[nodiscard]] std::vector<std::string> split_codes(std::string_view text);

// The codes in force, in a shape the Z80's read path can ask about cheaply.
//
// A Master System or Game Gear code is checked ON EVERY READ rather than
// patched into the ROM once, because its address is a processor address and a
// bank switch changes what lives there. That is the hot path of the whole
// emulator, so the question "is there a cheat at this address" has to be one
// bit test: the bitmap covers all 64K of the address space in 8KB and is only
// consulted at all when at least one code is active.
class CheatMap {
  public:
    void clear() noexcept {
        entries_.clear();
        present_.fill(0);
        any_ = false;
    }
    void add(std::uint16_t address, std::uint8_t value, int compare) {
        entries_.push_back(Entry{address, value, compare});
        present_[static_cast<std::size_t>(address) >> 3] |=
            static_cast<std::uint8_t>(1u << (address & 7u));
        any_ = true;
    }
    [[nodiscard]] bool empty() const noexcept { return !any_; }

    // The value the processor actually sees. `read` is what the cartridge or
    // RAM answered.
    [[nodiscard]] std::uint8_t substitute(std::uint16_t address, std::uint8_t read) const noexcept {
        if (!any_ || (present_[static_cast<std::size_t>(address) >> 3] &
                      static_cast<std::uint8_t>(1u << (address & 7u))) == 0) {
            return read;
        }
        for (const auto& entry : entries_) {
            if (entry.address != address) {
                continue;
            }
            // A code with a check byte only fires when the byte matches, which
            // is how one code can be correct in one bank and silent in another.
            if (entry.compare >= 0 && read != static_cast<std::uint8_t>(entry.compare)) {
                continue;
            }
            return entry.value;
        }
        return read;
    }

  private:
    struct Entry {
        std::uint16_t address;
        std::uint8_t value;
        int compare;
    };
    std::vector<Entry> entries_;
    // One bit per address. 64Ki bits is 8KB, which is small enough to keep
    // warm and turns the common "no cheat here" answer into a single test.
    std::array<std::uint8_t, 8192> present_{};
    bool any_ = false;
};

}  // namespace mercury::core
