// European cartridges that refuse a 60Hz console (issue #12), pinned:
// needs_pal_console's matching rules, and load_rom picking PAL for a "JUE"
// header that is on the list.

#include "mercury/genesis/cartridge.hpp"
#include "mercury/genesis/game_quirks.hpp"
#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mercury::genesis::Genesis;
using mercury::genesis::RomHeader;
using mercury::genesis::needs_pal_console;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

RomHeader header(const char* serial, std::uint16_t checksum) {
    RomHeader h;
    h.serial = serial;
    h.region = "JUE";
    h.checksum = checksum;
    return h;
}

// A 512KB image with a SEGA header: the given serial and region, and a reset
// vector that spins in place.
std::filesystem::path write_rom(const char* name, const char* serial, const char* region) {
    std::vector<std::uint8_t> rom(0x80000, 0x00);
    const auto put32 = [&rom](std::size_t at, std::uint32_t v) {
        rom[at] = static_cast<std::uint8_t>(v >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(v >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(v >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(v);
    };
    put32(0, 0x00FFFE00);
    put32(4, 0x00000200);
    rom[0x200] = 0x60;  // BRA.S *
    rom[0x201] = 0xFE;
    const auto put_text = [&rom](std::size_t at, const char* text, std::size_t width) {
        std::memset(&rom[at], ' ', width);
        std::memcpy(&rom[at], text, std::strlen(text));
    };
    put_text(0x100, "SEGA MEGA DRIVE", 16);
    put_text(0x180, serial, 14);
    put_text(0x1F0, region, 16);
    put32(0x1A0, 0x00000000);
    put32(0x1A4, 0x0007FFFF);
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()),
                                                static_cast<std::streamsize>(rom.size()));
    return path;
}

}  // namespace

int main() {
    // The verified one, in both spellings dumps use.
    check(needs_pal_console(header("GM T-69046-50", 0xE697), 0xE697), "BTTF III (Europe) needs PAL");
    check(needs_pal_console(header("GM T-69046 -50", 0x1234), 0x1234), "the same serial with a space before the dash");
    check(!needs_pal_console(header("GM T-69046-00", 0x0000), 0x9999), "BTTF III (USA) does not");

    // Alisia Dragoon: Europe and USA share a serial; only Europe's checksum counts.
    check(needs_pal_console(header("GM T-45033 -00", 0x0F81), 0x0F81), "Alisia Dragoon (Europe) needs PAL");
    check(!needs_pal_console(header("GM T-45033 -00", 0x95AA), 0x95AA), "Alisia Dragoon (USA) does not");

    check(needs_pal_console(header("GM T-120106-00", 0x0001), 0x0001), "Brian Lara Cricket needs PAL");
    check(needs_pal_console(header("GM T-97126 -50", 0x0001), 0x0001), "Williams Arcade's Greatest Hits (Europe)");
    check(!needs_pal_console(header("GM T-97126 -00", 0x0001), 0x0001), "Williams Arcade's Greatest Hits (USA) does not");
    check(needs_pal_console(header("GM T-113026-50", 0x0001), 0x0001), "Wiz 'n' Liz (Europe) needs PAL");
    check(needs_pal_console(header("GM T-70096 -00", 0x0001), 0x0001), "Muhammad Ali Heavyweight Boxing needs PAL");

    // Radica Sensible Soccer Plus: no serial, header checksum 0, own sum 1F7F.
    check(needs_pal_console(header("", 0x0000), 0x1F7F), "Radica Sensible Soccer Plus needs PAL");
    check(!needs_pal_console(header("GM T-15056 -00", 0x0000), 0x4321),
          "another cartridge with a zero header checksum does not");

    // End to end: load_rom picks PAL for a listed "JUE" cartridge, NTSC otherwise.
    {
        Genesis machine;
        const auto path = write_rom("genx_region_eu.md", "GM T-69046-50", "JUE");
        check(machine.load_rom(path).loaded && machine.pal(), "load_rom: listed JUE cartridge runs PAL");
        std::filesystem::remove(path);
    }
    {
        Genesis machine;
        const auto path = write_rom("genx_region_us.md", "GM T-69046-00", "JUE");
        check(machine.load_rom(path).loaded && !machine.pal(), "load_rom: unlisted JUE cartridge runs NTSC");
        std::filesystem::remove(path);
    }
    {
        Genesis machine;
        const auto path = write_rom("genx_region_e.md", "GM T-00000-00", "E");
        check(machine.load_rom(path).loaded && machine.pal(), "load_rom: an E-only header still runs PAL");
        std::filesystem::remove(path);
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
