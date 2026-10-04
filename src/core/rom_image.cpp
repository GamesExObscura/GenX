#include "mercury/core/rom_image.hpp"

#include "mercury/core/zip_archive.hpp"

#include <fstream>
#include <iterator>

namespace mercury::core {

std::size_t RomImage::size_bytes() const noexcept {
    return bytes.size();
}

std::size_t RomImage::size_kib() const noexcept {
    return size_bytes() / 1024;
}

bool RomImage::empty() const noexcept {
    return bytes.empty();
}

RomLoadResult load_rom_image(const std::filesystem::path& path) {
    RomImage image;
    image.source_path = path;
    // read_rom_file unwraps a .zip transparently, so a zipped cartridge reaches
    // the machine exactly as a loose one would.
    std::string error;
    if (!read_rom_file(path, image.bytes, error, &image.archive_entry)) {
        return {
            .ok = false,
            .image = {},
            .error = error,
        };
    }

    if (image.empty()) {
        return {
            .ok = false,
            .image = {},
            .error = "ROM file was empty.",
        };
    }

    return {
        .ok = true,
        .image = std::move(image),
        .error = {},
    };
}

}  // namespace mercury::core
