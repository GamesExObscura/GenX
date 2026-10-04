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
    char region = 0;  // 'U', 'E', 'J', or 0 when it could not be told
    std::string description;
};

// Every 128KB image in a folder that looks like a Sega CD BIOS.
[[nodiscard]] std::vector<BiosImage> scan_bios_folder(const std::filesystem::path& folder);

// Pick and read the BIOS for a disc region ('U', 'E', 'J'; 0 means "any").
// Looks in the configured folder first, then for a folder called BIOS beside
// the disc or up to three levels above it. On failure `error` says what was
// looked for and where.
[[nodiscard]] bool load_bios_for(char region, const std::filesystem::path& configured,
                                 const std::filesystem::path& disc,
                                 std::vector<std::uint8_t>& image, std::string& error,
                                 std::filesystem::path* chosen = nullptr);

}  // namespace mercury::segacd
