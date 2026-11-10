#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mercury::segacd {

// Finding the Sega CD's BIOS.
//
// Unlike the 32X, a Sega CD cannot boot without one, and a disc only boots on
// the BIOS of its own region: the security code on the disc is checked by the
// BIOS, and each region's discs carry a different one. So a folder of BIOS
// images is searched for the one matching the disc.
//
// Images are recognised by size (128KB) and their region is read from the
// BIOS's own header - the region field at $1F0, as in a cartridge header -
// with the file name ("USA", "Europe", "Japan", "us_", "eu_", "jp_"...) as a
// fallback for images whose header does not say.
struct BiosImage {
    std::filesystem::path path;
    // The discs it boots: 'U', 'E', 'J', or 0 when it could not be told.
    char region = 0;
    // The console it must run on - see bios_console_region. Not always the
    // same region: the JVC X'Eye boots US discs on a Japanese-type console.
    char console = 0;
    std::string description;
};

// Which console a BIOS belongs to: 'U' (USA and other NTSC regions), 'E'
// (Europe, PAL), 'J' (Japan, NTSC) or 'A' (Southeast Asia, PAL), or 0.
//
// A Mega-CD and its BIOS are one unit, and every BIOS checks the console's
// version register at power-on and refuses to start anywhere else - "ERROR!
// THIS IS A PAL-COMPATIBLE MEGA CD FOR EXCLUSIVE USE IN SOUTHEAST ASIA." So
// the answer is read from the message the BIOS shows when the check fails,
// which says exactly what it wants. The header's region letter is the
// fallback for images with no message, and only that: it is wrong on several
// real images - the JVC X'Eye's header says U and its check wants Japan, the
// US LaserActive's says J and wants the US, and the Asian PAL BIOS says J.
[[nodiscard]] char bios_console_region(const std::vector<std::uint8_t>& image);

// Every 128KB image in a folder that looks like a Sega CD BIOS.
[[nodiscard]] std::vector<BiosImage> scan_bios_folder(const std::filesystem::path& folder);

// Pick and read the BIOS for a disc region ('U', 'E', 'J'; 0 means "any").
// The disc must match the BIOS's header region; among those, a BIOS whose
// console is the same region comes first. So a Japanese disc takes a Japanese
// BIOS, and the Southeast Asian PAL one only when there is no other - those
// consoles played Japanese discs, but at 50Hz.
// Looks in the configured folder first, then for a folder called BIOS beside
// the disc or up to three levels above it. On failure `error` says what was
// looked for and where.
[[nodiscard]] bool load_bios_for(char region, const std::filesystem::path& configured,
                                 const std::filesystem::path& disc,
                                 std::vector<std::uint8_t>& image, std::string& error,
                                 std::filesystem::path* chosen = nullptr);

}  // namespace mercury::segacd
