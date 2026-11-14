// WaterMelon's T-5740 cartridge board (Pier Solar, issue #7), pinned.
//
// Three things on the board, all needed for the game to get past its
// disclaimer screen and to save:
//   * byte reads of $018100-$0181FF answer from the start of the ROM - the
//     game's key for its code-in-RAM bank switcher is built from them;
//   * its own bank registers at $A13003/05/07 (the Sega mapper's $A130F1-FF
//     do not exist on it);
//   * a 64KB SPI EEPROM driven through $A13009 and read back on bit 0 of the
//     $A130xx block.

#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mercury::genesis::Genesis;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

// A 1MB image: SEGA header with the given product code, a reset vector that
// spins in place, and marker bytes where the tests look.
std::filesystem::path write_rom(const char* name, const char* serial) {
    std::vector<std::uint8_t> rom(0x100000, 0x00);
    const auto put32 = [&rom](std::size_t at, std::uint32_t v) {
        rom[at] = static_cast<std::uint8_t>(v >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(v >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(v >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(v);
    };
    put32(0x000, 0x00FFFE00);
    put32(0x004, 0x00000200);
    std::memcpy(&rom[0x100], "SEGA MEGA DRIVE ", 16);
    std::memcpy(&rom[0x180], serial, std::strlen(serial));
    put32(0x1A0, 0x000000);
    put32(0x1A4, 0x0FFFFF);
    std::memcpy(&rom[0x1F0], "JUE", 3);
    rom[0x200] = 0x60;  // BRA.S *
    rom[0x201] = 0xFE;
    rom[0x01C8] = 0x33;   // what the mirror answers for $0181C8
    rom[0x181C8] = 0x77;  // what a plain ROM read gives
    rom[0x181C9] = 0x78;
    rom[0x080000] = 0xA5;  // bank 1's first byte
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return path;
}

// Lines on $A13009: bit 0 data, bit 1 clock, bit 2 /HOLD, bit 3 /CS.
constexpr std::uint8_t kHold = 0x04;
constexpr std::uint8_t kDeselect = 0x08;

void spi_deselect(Genesis& machine) {
    machine.write8(0xA13009, kHold | kDeselect);
}

// Clocks one byte out MSB first and returns the 8 bits the chip presented,
// read back after each rising edge.
std::uint8_t spi_byte(Genesis& machine, std::uint8_t value) {
    std::uint8_t in = 0;
    for (int bit = 7; bit >= 0; --bit) {
        const auto data = static_cast<std::uint8_t>((value >> bit) & 1);
        machine.write8(0xA13009, kHold | data);           // clock low, data set up
        machine.write8(0xA13009, kHold | data | 0x02);    // rising edge
        in = static_cast<std::uint8_t>((in << 1) | (machine.read8(0xA13001) & 1));
    }
    return in;
}

}  // namespace

int main() {
    // --- the board ------------------------------------------------------
    const auto board_rom = write_rom("genx_t5740_tests.bin", "GM T-574023-00");
    Genesis machine;
    check(machine.load_rom(board_rom).loaded, "T-5740 test cartridge loads");
    check(machine.t5740_board(), "the product code selects the T-5740 board");

    check(machine.read8(0x0181C8) == 0x33, "byte read of $0181C8 answers from the ROM's start");
    check(machine.read16(0x0181C8) == 0x7778, "a word read there is the plain ROM");
    check(machine.read8(0x0182C8) == 0x00, "the mirror covers $018100-$0181FF only");

    machine.write8(0xA13003, 0x00);
    check(machine.read8(0x280000) == 0x00, "$A13003 = 0 pages bank 0 in at $280000");
    machine.write8(0xA13003, 0x01);
    check(machine.read8(0x280000) == 0xA5, "$A13003 = 1 pages bank 1 in at $280000");
    machine.write8(0xA13005, 0x01);
    check(machine.read8(0x300000) == 0xA5, "$A13005 pages the $300000 window");
    machine.write8(0xA13007, 0x01);
    check(machine.read8(0x380000) == 0xA5, "$A13007 pages the $380000 window");
    machine.write8(0xA130F3, 0x00);  // the Sega mapper would page bank 0 in here
    check(machine.read8(0x080000) == 0xA5, "the Sega mapper's $A130F3 does nothing on this board");
    machine.write8(0xA130FF, 0x00);
    check(machine.read8(0x380000) == 0xA5, "nor does $A130FF");

    // --- the EEPROM -----------------------------------------------------
    check(machine.battery_backed(), "the board counts as battery-backed");
    check(machine.battery_snapshot().size() == 0x10000 && machine.battery_snapshot()[0x1234] == 0xFF,
          "64KB, erased at power-on");

    // A write without WREN first is ignored.
    spi_deselect(machine);
    spi_byte(machine, 0x02);
    spi_byte(machine, 0x12);
    spi_byte(machine, 0x34);
    spi_byte(machine, 0x99);
    spi_deselect(machine);
    check(machine.battery_snapshot()[0x1234] == 0xFF, "WRITE without WREN changes nothing");

    spi_byte(machine, 0x06);  // WREN
    spi_deselect(machine);
    spi_byte(machine, 0x05);  // RDSR
    check((spi_byte(machine, 0x00) & 0x02) != 0, "RDSR shows the write-enable latch after WREN");
    spi_deselect(machine);

    machine.clear_battery_dirty();
    spi_byte(machine, 0x02);
    spi_byte(machine, 0x12);
    spi_byte(machine, 0x34);
    spi_byte(machine, 0x5A);
    spi_byte(machine, 0xC3);
    spi_deselect(machine);
    check(machine.battery_snapshot()[0x1234] == 0x5A && machine.battery_snapshot()[0x1235] == 0xC3,
          "WRITE stores sequential bytes");
    check(machine.battery_dirty(), "...and marks the save for writing out");

    spi_byte(machine, 0x03);
    spi_byte(machine, 0x12);
    spi_byte(machine, 0x34);
    const auto first = spi_byte(machine, 0x00);
    const auto second = spi_byte(machine, 0x00);
    spi_deselect(machine);
    check(first == 0x5A && second == 0xC3, "READ returns them in order");
    check((machine.read8(0xA13001) & 1) == 1, "deselecting floats the output high");
    check(machine.read8(0xA13000) == 0x00, "an even byte of the block reads 0");

    // Page wrap: a write that runs past the end of a 128-byte page wraps to
    // its start.
    spi_byte(machine, 0x06);
    spi_deselect(machine);
    spi_byte(machine, 0x02);
    spi_byte(machine, 0x00);
    spi_byte(machine, 0x7F);
    spi_byte(machine, 0x11);
    spi_byte(machine, 0x22);
    spi_deselect(machine);
    check(machine.battery_snapshot()[0x007F] == 0x11 && machine.battery_snapshot()[0x0000] == 0x22,
          "sequential writes wrap inside the 128-byte page");

    // A save state taken in the middle of a READ resumes it.
    spi_byte(machine, 0x03);
    spi_byte(machine, 0x12);
    spi_byte(machine, 0x34);
    const auto blob = machine.save_state_blob();
    Genesis other;
    (void)other.load_rom(board_rom);
    std::string error;
    check(other.load_state_blob(blob, error), "a state taken mid-READ loads");
    check(spi_byte(other, 0x00) == 0x5A, "...and the READ carries on where it was");

    // The save file comes back.
    const auto save = machine.battery_snapshot();
    Genesis fresh;
    (void)fresh.load_rom(board_rom);
    fresh.restore_battery(save);
    spi_byte(fresh, 0x03);
    spi_byte(fresh, 0x12);
    spi_byte(fresh, 0x35);
    check(spi_byte(fresh, 0x00) == 0xC3, "a restored save reads back through the EEPROM");

    // --- every other cartridge ------------------------------------------
    const auto plain_rom = write_rom("genx_t5740_tests_plain.bin", "GM 00001009-00");
    Genesis plain;
    check(plain.load_rom(plain_rom).loaded && !plain.t5740_board(), "another product code is not the board");
    check(plain.read8(0x0181C8) == 0x77, "...and reads $0181C8 from the ROM");
    check(plain.read8(0x080000) == 0xA5, "...bank 1 at $080000 to begin with");
    plain.write8(0xA130F3, 0x00);
    check(plain.read8(0x080000) == 0x00, "...and keeps the Sega mapper ($A130F3 = 0 pages bank 0 in)");

    std::error_code ec;
    std::filesystem::remove(board_rom, ec);
    std::filesystem::remove(plain_rom, ec);
    std::printf("\n%s\n", failures == 0 ? "all T-5740 checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
