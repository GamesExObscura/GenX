// Game Genie code decoding.
//
// A decoder transcribed from a bit table is exactly the kind of code that is
// wrong in a way nothing notices: a code decodes to SOME address and patches
// SOMETHING, and the only symptom is that the cheat does not work. So the
// tests here are structural rather than a handful of remembered codes.
//
// THE MEGA DRIVE TEST THAT MATTERS IS THE BIJECTION. Eight symbols of five
// bits are forty bits, and the format spends them on a 24-bit address and a
// 16-bit value - 40 again, with nothing shared and nothing dropped. So
// flipping any ONE of the forty input bits has to change exactly one bit of
// the output, and no two input bits may move the same output bit. A
// transposed line in the table breaks that immediately: two symbols would
// write the same output bit and a third would write none.
//
// The Master System side has no such property - its format is a plain
// reordering of hex digits - so it is checked digit by digit instead.

#include "mercury/core/game_genie.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const char* what, bool ok) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

void check_equal(const char* what, unsigned long actual, unsigned long expected) {
    if (actual != expected) {
        std::printf("FAIL %-52s got %08lX, expected %08lX\n", what, actual, expected);
        ++failures;
    }
}

constexpr char kSymbols[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";

// A code from forty bits, most significant symbol first.
std::string code_of(std::uint64_t bits) {
    std::string out;
    for (int i = 0; i < 8; ++i) {
        const auto symbol = static_cast<int>((bits >> (35 - i * 5)) & 0x1F);
        out.push_back(kSymbols[symbol]);
    }
    return out;
}

// The decode as one 40-bit number: address in the top 24, value in the low 16.
std::uint64_t decoded(std::uint64_t bits) {
    mercury::core::Cheat cheat;
    std::string error;
    if (!mercury::core::decode_cheat(code_of(bits), mercury::core::CheatFormat::mega_drive, cheat, error)) {
        std::printf("FAIL code %s did not decode: %s\n", code_of(bits).c_str(), error.c_str());
        ++failures;
        return 0;
    }
    return (static_cast<std::uint64_t>(cheat.address) << 16) | cheat.value;
}

void test_mega_drive_is_a_bijection() {
    // Zero in, zero out: every symbol is 'A', which is symbol 0.
    check_equal("all-A code decodes to nothing set", static_cast<unsigned long>(decoded(0)), 0);

    // One input bit at a time. Each must move exactly one output bit, and no
    // output bit may be claimed twice.
    std::uint64_t seen = 0;
    for (int bit = 0; bit < 40; ++bit) {
        const std::uint64_t out = decoded(std::uint64_t{1} << bit);
        char what[80];
        std::snprintf(what, sizeof(what), "code bit %d moves exactly one output bit", bit);
        // Exactly one bit set.
        check(what, out != 0 && (out & (out - 1)) == 0);
        std::snprintf(what, sizeof(what), "code bit %d claims an output bit of its own", bit);
        check(what, (seen & out) == 0);
        seen |= out;
    }
    check_equal("the forty code bits cover all forty output bits",
                static_cast<unsigned long>(seen & 0xFFFFFFFFu),
                static_cast<unsigned long>(0xFFFFFFFFu));
    check("the address half is covered too", (seen >> 32) == 0xFF);

    // And it is linear over those bits, so the whole map follows from the
    // forty singles: a code with several bits set decodes to the OR of what
    // each would give alone.
    const std::uint64_t mixed = 0x0123456789ull & 0xFFFFFFFFFFull;
    std::uint64_t expected = 0;
    for (int bit = 0; bit < 40; ++bit) {
        if ((mixed >> bit) & 1) {
            expected |= decoded(std::uint64_t{1} << bit);
        }
    }
    check("a multi-bit code is the sum of its bits", decoded(mixed) == expected);
}

// THE BIJECTION IS NOT ENOUGH ON ITS OWN.
//
// Swapping two whole lines of the table above leaves it a perfect bijection
// and produces completely wrong addresses, so the structural test cannot
// catch a transposition. Only a code whose answer is known from outside can.
//
// This pair is a published worked example: the Game Genie code AFJT-EVSG is
// the address/value pair 0251E6:A801. One real anchor pins the whole table,
// because every symbol position contributes to it.
void test_mega_drive_against_a_known_code() {
    mercury::core::Cheat cheat;
    std::string error;
    check("AFJT-EVSG decodes", mercury::core::decode_cheat("AFJT-EVSG", mercury::core::CheatFormat::mega_drive, cheat, error));
    check_equal("AFJT-EVSG is address 0251E6", cheat.address, 0x0251E6);
    check_equal("AFJT-EVSG is value A801", cheat.value, 0xA801);
    check("a Mega Drive code has no compare value", cheat.compare < 0);
    check("and it is a word patch", cheat.word);

    // Its sibling from the same list differs only in the first group, and the
    // format says the first two symbols carry data bits and two address bits
    // - so the address must be nearly the same and the value different.
    mercury::core::Cheat sibling;
    check("97JT-EVSG decodes", mercury::core::decode_cheat("97JT-EVSG", mercury::core::CheatFormat::mega_drive, sibling, error));
    check("the sibling code patches a different value", sibling.value != cheat.value);
}

void test_mega_drive_rejects_what_it_should() {
    mercury::core::Cheat cheat;
    std::string error;
    check("a seven-letter code is refused",
          !mercury::core::decode_cheat("ABCD-EFG", mercury::core::CheatFormat::mega_drive, cheat, error));
    // I, O, Q and U are not in the alphabet - they look like 1, 0, 0 and V.
    check("the letter I is refused", !mercury::core::decode_cheat("ABCI-EFGH", mercury::core::CheatFormat::mega_drive, cheat, error));
    check("the letter O is refused", !mercury::core::decode_cheat("ABCO-EFGH", mercury::core::CheatFormat::mega_drive, cheat, error));
    check("the letter Q is refused", !mercury::core::decode_cheat("ABCQ-EFGH", mercury::core::CheatFormat::mega_drive, cheat, error));
    check("the letter U is refused", !mercury::core::decode_cheat("ABCU-EFGH", mercury::core::CheatFormat::mega_drive, cheat, error));
    // Separators are decoration; a code pasted without them still works.
    mercury::core::Cheat with;
    mercury::core::Cheat without;
    check("with dash", mercury::core::decode_cheat("RGEA-A6VR", mercury::core::CheatFormat::mega_drive, with, error));
    check("without dash", mercury::core::decode_cheat("RGEAA6VR", mercury::core::CheatFormat::mega_drive, without, error));
    check_equal("the dash changes nothing (address)", with.address, without.address);
    check_equal("the dash changes nothing (value)", with.value, without.value);
    check("and it is written back grouped", with.code == "RGEA-A6VR");
}

void test_master_system_digit_order() {
    mercury::core::Cheat cheat;
    std::string error;
    // One digit at a time, so each one's place in the result is pinned.
    // Digits 0 and 1 are the value.
    check("value high", mercury::core::decode_cheat("70-0000", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 0 is the value's high nibble", cheat.value, 0x70);
    check("value low", mercury::core::decode_cheat("07-0000", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 1 is the value's low nibble", cheat.value, 0x07);

    // Digits 2, 3 and 4 are the low twelve bits of the address, in order.
    check("address 8-11", mercury::core::decode_cheat("007-000", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 2 is address bits 8-11", cheat.address, 0xF700);
    check("address 4-7", mercury::core::decode_cheat("000-700", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 3 is address bits 4-7", cheat.address, 0xF070);
    check("address 0-3", mercury::core::decode_cheat("000-070", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 4 is address bits 0-3", cheat.address, 0xF007);

    // Digit 5 is the top nibble AND IT IS INVERTED - which is why every code
    // above, with a zero there, comes out with F at the top.
    check("address 12-15", mercury::core::decode_cheat("000-00F", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("digit 5 is the inverted top nibble", cheat.address, 0x0000);
    check("address 12-15 again", mercury::core::decode_cheat("000-00A", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("inverted: A gives 5", cheat.address, 0x5000);

    // Six digits means no check byte at all.
    check("six digits", mercury::core::decode_cheat("00A-000", mercury::core::CheatFormat::master_system, cheat, error));
    check("a six-digit code has no compare value", cheat.compare < 0);
}

void test_master_system_check_byte() {
    mercury::core::Cheat cheat;
    std::string error;
    // The check byte is digits 6 and 8 - the outer two of the last group -
    // rotated right two places and XORed with $BA. Nothing at all comes from
    // digit 7, so changing it must not change the answer.
    check("nine digits", mercury::core::decode_cheat("000-000-000", mercury::core::CheatFormat::master_system, cheat, error));
    const int with_zero_middle = cheat.compare;
    check("nine digits, middle changed",
          mercury::core::decode_cheat("000-000-0F0", mercury::core::CheatFormat::master_system, cheat, error));
    check("the middle digit of the last group carries nothing",
          cheat.compare == with_zero_middle);

    // 00 rotates to 00 and XORs to BA.
    check_equal("a zero check byte is BA", static_cast<unsigned long>(with_zero_middle), 0xBA);
    // $03 rotates right two to $C0, XOR $BA is $7A. The 3 goes in the LAST
    // digit, because the middle one of the group is the unused one.
    check("check byte 03", mercury::core::decode_cheat("000-000-003", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("03 rotates and xors to 7A", static_cast<unsigned long>(cheat.compare), 0x7A);
    // $04 rotates right two to $01, XOR $BA is $BB.
    check("check byte 04", mercury::core::decode_cheat("000-000-004", mercury::core::CheatFormat::master_system, cheat, error));
    check_equal("04 rotates and xors to BB", static_cast<unsigned long>(cheat.compare), 0xBB);

    check("a seven-digit code is refused",
          !mercury::core::decode_cheat("000-0000", mercury::core::CheatFormat::master_system, cheat, error));
    check("a hex code is refused as a Mega Drive one when it has the wrong length",
          !mercury::core::decode_cheat("00A-000", mercury::core::CheatFormat::mega_drive, cheat, error));
}

// WHICH FORMAT A CODE IS IN IS A PROPERTY OF THE CODE.
//
// This used to be decided by which machine was loaded, and with no game
// loaded that was whatever it last was - so the dialog told people Mega
// Drive codes were six or nine hex digits and refused real ones. Length
// settles it with no overlap at all.
void test_format_is_read_off_the_code() {
    using mercury::core::CheatFormat;
    mercury::core::Cheat cheat;
    std::string error;

    check("eight letters is a Mega Drive code",
          mercury::core::decode_cheat("AFJT-EVSG", CheatFormat::automatic, cheat, error));
    check_equal("and it decodes the same as when told so", cheat.address, 0x0251E6);
    check("which is a word patch", cheat.word);

    check("six digits is a Master System code",
          mercury::core::decode_cheat("00A-000", CheatFormat::automatic, cheat, error));
    check("which is a byte patch", !cheat.word);
    check("nine digits is one too",
          mercury::core::decode_cheat("000-000-000", CheatFormat::automatic, cheat, error));
    check("with a compare value", cheat.compare >= 0);

    // Seven or eleven is neither, and the message says both real lengths so
    // the reader can see which one they meant.
    check("seven characters is neither",
          !mercury::core::decode_cheat("ABCD-EFG", CheatFormat::automatic, cheat, error));
    check("and the message names both formats",
          error.find("Mega Drive") != std::string::npos &&
              error.find("Game Gear") != std::string::npos);

    // A real Sonic 3 code from the published list, pasted with its hyphen.
    check("a real published code is taken",
          mercury::core::decode_cheat("AWHA-CA92", CheatFormat::automatic, cheat, error));
    check("as a Mega Drive one", cheat.word);
}

// Published lists join a multi-part cheat with " + ". Pasting the line means
// both codes.
void test_multi_part_codes_split() {
    const auto two = mercury::core::split_codes("SCGA-BL0N + A4GA-A28T");
    check_equal("a plus-joined line is two codes",
                static_cast<unsigned long>(two.size()), 2);
    mercury::core::Cheat a;
    mercury::core::Cheat b;
    std::string error;
    check("the first half decodes",
          mercury::core::decode_cheat(two[0], mercury::core::CheatFormat::automatic, a, error));
    check("the second half decodes",
          mercury::core::decode_cheat(two[1], mercury::core::CheatFormat::automatic, b, error));
    check("and they are different codes", a.address != b.address || a.value != b.value);

    check_equal("one code is one code",
                static_cast<unsigned long>(mercury::core::split_codes("AFJT-EVSG").size()), 1);
    check_equal("commas separate too",
                static_cast<unsigned long>(mercury::core::split_codes("AAAA-AAAA,BBBB-BBBB").size()),
                2);
    check_equal("and trailing separators add nothing",
                static_cast<unsigned long>(mercury::core::split_codes("AFJT-EVSG + ").size()), 1);
    check_equal("nothing is nothing",
                static_cast<unsigned long>(mercury::core::split_codes("   ").size()), 0);
}

// The map the Z80's read path consults: a code fires at its own address, is
// silent everywhere else, and a code with a check byte is silent when the
// byte does not match.
void test_cheat_map() {
    mercury::core::CheatMap map;
    check("an empty map is empty", map.empty());
    check_equal("an empty map changes nothing", map.substitute(0x1234, 0x99), 0x99);

    map.add(0x1234, 0x42, -1);
    map.add(0x5678, 0x11, 0xAB);
    check("a map with codes is not empty", !map.empty());
    check_equal("a code with no compare always fires", map.substitute(0x1234, 0x99), 0x42);
    check_equal("another address is untouched", map.substitute(0x1235, 0x99), 0x99);
    check_equal("a compare that matches fires", map.substitute(0x5678, 0xAB), 0x11);
    check_equal("a compare that does not match is silent", map.substitute(0x5678, 0xAC), 0xAC);

    map.clear();
    check("clearing empties it", map.empty());
    check_equal("and the code stops firing", map.substitute(0x1234, 0x99), 0x99);
}

}  // namespace

int main() {
    test_mega_drive_is_a_bijection();
    test_mega_drive_against_a_known_code();
    test_mega_drive_rejects_what_it_should();
    test_master_system_digit_order();
    test_master_system_check_byte();
    test_format_is_read_off_the_code();
    test_multi_part_codes_split();
    test_cheat_map();
    if (failures == 0) {
        std::printf("game_genie_tests: all checks passed\n");
        return 0;
    }
    std::printf("game_genie_tests: %d failures\n", failures);
    return 1;
}

