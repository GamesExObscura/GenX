#include "mercury/core/game_genie.hpp"

#include <cctype>

namespace mercury::core {

namespace {

// The Game Genie's own alphabet. Thirty-two symbols, five bits each, and the
// missing letters are the ones that look like digits: no I (1), no O (0), no
// Q (0), no U (V). A code is eight of these, so forty bits in all.
constexpr char kSymbols[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";

int symbol_value(char c) noexcept {
    const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int i = 0; i < 32; ++i) {
        if (kSymbols[i] == upper) {
            return i;
        }
    }
    return -1;
}

int hex_value(char c) noexcept {
    const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper >= '0' && upper <= '9') {
        return upper - '0';
    }
    if (upper >= 'A' && upper <= 'F') {
        return upper - 'A' + 10;
    }
    return -1;
}

// Everything that is not a code character. Separators are for reading and a
// person may type them anywhere, or not at all.
std::string strip(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// The code written the way the manual writes it, so what goes back on screen
// matches the book it was copied out of.
std::string grouped(const std::string& bare, std::size_t group) {
    std::string out;
    for (std::size_t i = 0; i < bare.size(); ++i) {
        if (i != 0 && i % group == 0) {
            out.push_back('-');
        }
        out.push_back(bare[i]);
    }
    return out;
}

// MEGA DRIVE: forty bits of code into a 24-bit address and a 16-bit word.
//
// The scramble is the point of the device - it stops a code being read off as
// an address - and there is no pattern to reconstruct it from, so each
// symbol's contribution is written out one line at a time exactly as the
// format defines it.
bool decode_mega_drive(const std::string& bare, Cheat& out, std::string& error) {
    if (bare.size() != 8) {
        error = "a Mega Drive code is eight letters, written ABCD-EFGH";
        return false;
    }
    std::uint32_t address = 0;
    std::uint16_t data = 0;
    for (int i = 0; i < 8; ++i) {
        const int n = symbol_value(bare[static_cast<std::size_t>(i)]);
        if (n < 0) {
            error = std::string("'") + bare[static_cast<std::size_t>(i)] +
                    "' is not a Game Genie letter - I, O, Q and U are never used";
            return false;
        }
        const auto v = static_cast<std::uint32_t>(n);
        switch (i) {
            case 0: data |= static_cast<std::uint16_t>(v << 3); break;
            case 1:
                data |= static_cast<std::uint16_t>(v >> 2);
                address |= (v & 3u) << 14;
                break;
            case 2: address |= v << 9; break;
            case 3: address |= ((v & 0xFu) << 20) | ((v >> 4) << 8); break;
            case 4:
                data |= static_cast<std::uint16_t>((v & 1u) << 12);
                address |= (v >> 1) << 16;
                break;
            case 5:
                data |= static_cast<std::uint16_t>(((v & 1u) << 15) | ((v >> 1) << 8));
                break;
            case 6:
                data |= static_cast<std::uint16_t>((v >> 3) << 13);
                address |= (v & 7u) << 5;
                break;
            case 7: address |= v; break;
            default: break;
        }
    }
    // Every one of the forty bits of the code lands in exactly one bit of the
    // address or the data - which is the check that the table above is
    // transcribed right: 24 and 16 make 40, with nothing used twice and
    // nothing left over. So the address is kept exactly as decoded, including
    // bit 0; the word alignment belongs to whoever applies it, not here.
    out.address = address;
    out.value = data;
    out.compare = -1;
    out.word = true;
    out.code = grouped(bare, 4);
    return true;
}

// MASTER SYSTEM AND GAME GEAR: hexadecimal, with the address's top nibble
// inverted and an optional check byte that is rotated and XORed.
//
// Nine digits rather than eight, in three groups of three, and the middle
// digit of the LAST group carries nothing: the check byte is the first and
// third digits of that group. That is not a mistake here - it is how the
// device reads them.
bool decode_master(const std::string& bare, Cheat& out, std::string& error) {
    if (bare.size() != 6 && bare.size() != 9) {
        error = "a Master System or Game Gear code is six or nine digits, written "
                "00A-000 or 00A-000-000";
        return false;
    }
    int digits[9] = {};
    for (std::size_t i = 0; i < bare.size(); ++i) {
        digits[i] = hex_value(bare[i]);
        if (digits[i] < 0) {
            error = std::string("'") + bare[i] + "' is not a hexadecimal digit";
            return false;
        }
    }
    out.value = static_cast<std::uint16_t>((digits[0] << 4) | digits[1]);
    // The first two digits are the value, the next three are the LOW twelve
    // bits of the address, and the sixth is its top nibble - inverted. The
    // top nibble coming last and upside down is the format's own scrambling,
    // such as it is.
    const auto high = static_cast<std::uint32_t>(digits[5] ^ 0xF);
    out.address = (high << 12) | (static_cast<std::uint32_t>(digits[2]) << 8) |
                  (static_cast<std::uint32_t>(digits[3]) << 4) |
                  static_cast<std::uint32_t>(digits[4]);
    out.compare = -1;
    if (bare.size() == 9) {
        // The check byte is the FIRST and THIRD digits of the last group. The
        // middle one carries nothing at all - that is the format, not an
        // omission here.
        auto check = static_cast<std::uint8_t>((digits[6] << 4) | digits[8]);
        // Rotated right two places, then XORed with $BA.
        check = static_cast<std::uint8_t>((check >> 2) | ((check & 0x03u) << 6));
        check ^= 0xBA;
        out.compare = check;
    }
    out.word = false;
    out.code = grouped(bare, 3);
    return true;
}

}  // namespace

bool decode_cheat(std::string_view text, CheatFormat format, Cheat& out, std::string& error) {
    const auto bare = strip(text);
    if (bare.empty()) {
        error = "no code";
        return false;
    }
    error.clear();
    if (format == CheatFormat::automatic) {
        // Length alone, because the two formats do not share one. Anything
        // else is neither, and saying so beats guessing at a format and then
        // complaining about the characters.
        if (bare.size() == 8) {
            format = CheatFormat::mega_drive;
        } else if (bare.size() == 6 || bare.size() == 9) {
            format = CheatFormat::master_system;
        } else {
            error = "that is " + std::to_string(bare.size()) +
                    " characters - a Mega Drive code is 8 (ABCD-EFGH) and a Master System or "
                    "Game Gear code is 6 or 9 (00A-000 or 00A-000-000)";
            return false;
        }
    }
    return format == CheatFormat::mega_drive ? decode_mega_drive(bare, out, error)
                                             : decode_master(bare, out, error);
}

std::vector<std::string> split_codes(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&]() {
        // Only the code characters matter; a piece that is all punctuation is
        // the separator itself and not a code.
        if (!strip(current).empty()) {
            out.push_back(current);
        }
        current.clear();
    };
    for (const char c : text) {
        if (c == '+' || c == ',' || c == ';') {
            flush();
            continue;
        }
        current.push_back(c);
    }
    flush();
    return out;
}

}  // namespace mercury::core
