#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mercury::core {

struct RomImage {
    std::filesystem::path source_path;
    // Name of the file inside the archive when source_path was a .zip, empty
    // otherwise. Anything that decides behaviour from the extension has to
    // prefer this, or a zipped .sms is mistaken for a Game Gear cartridge.
    std::string archive_entry;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] std::size_t size_kib() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
};

struct RomLoadResult {
    bool ok = false;
    RomImage image;
    std::string error;
};

[[nodiscard]] RomLoadResult load_rom_image(const std::filesystem::path& path);

}  // namespace mercury::core
