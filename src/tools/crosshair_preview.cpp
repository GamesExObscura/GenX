// Renders the light-gun crosshair over a range of backgrounds so the design can
// be judged before it ships. Writes a 24-bit BMP.
//
// The point of the exercise is "does it stay visible over anything a game can
// put behind it", and that is not a question anyone can answer by reading the
// code - it has to be looked at, over dark and light and saturated colour.

#include "mercury/core/crosshair.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_bmp(const std::string& path, const std::vector<std::uint32_t>& rgb, int width,
               int height) {
    // Bottom-up 24-bit BMP: rows padded to four bytes.
    const int row_bytes = ((width * 3) + 3) & ~3;
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(row_bytes) * height;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::printf("could not write %s\n", path.c_str());
        return;
    }
    const auto put16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto put32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    out.write("BM", 2);
    put32(14 + 40 + pixel_bytes);
    put16(0);
    put16(0);
    put32(14 + 40);
    put32(40);
    put32(static_cast<std::uint32_t>(width));
    put32(static_cast<std::uint32_t>(height));
    put16(1);
    put16(24);
    put32(0);
    put32(pixel_bytes);
    put32(2835);
    put32(2835);
    put32(0);
    put32(0);
    std::vector<char> row(static_cast<std::size_t>(row_bytes), 0);
    for (int y = height - 1; y >= 0; --y) {
        std::fill(row.begin(), row.end(), char{0});
        for (int x = 0; x < width; ++x) {
            const std::uint32_t colour = rgb[static_cast<std::size_t>(y) * width + x];
            row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<char>(colour & 0xFF);
            row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>((colour >> 8) & 0xFF);
            row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>((colour >> 16) & 0xFF);
        }
        out.write(row.data(), row_bytes);
    }
    std::printf("wrote %s (%dx%d)\n", path.c_str(), width, height);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "crosshair_preview.bmp";
    const int cursor_size = argc > 2 ? std::atoi(argv[2]) : 32;
    // Drawn at the real size and again magnified, because both matter: one
    // shows what it will actually look like, the other shows the shape.
    const int zoom = 6;

    const auto cursor = mercury::core::make_crosshair(cursor_size);

    // Backgrounds chosen to be the hard cases: black, white, mid grey, and the
    // saturated colours a Mega Drive palette leans on.
    const std::uint32_t backgrounds[] = {
        0x000000, 0xFFFFFF, 0x808080, 0x2060C0, 0x20A040,
        0xC02020, 0xD8C060, 0x603070, 0xE0E0E0, 0x303030,
    };
    const int count = static_cast<int>(std::size(backgrounds));
    const int cell = cursor_size * zoom;
    const int small_cell = cursor_size * 2;
    const int width = cell * count;
    const int height = cell + small_cell;
    std::vector<std::uint32_t> image(static_cast<std::size_t>(width) * height, 0);

    for (int tile = 0; tile < count; ++tile) {
        const std::uint32_t background = backgrounds[tile];
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < cell; ++x) {
                image[static_cast<std::size_t>(y) * width + tile * cell + x] = background;
            }
        }
        // Magnified, nearest-neighbour so individual pixels stay visible.
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                const std::uint32_t source = cursor[static_cast<std::size_t>(y / zoom) * cursor_size +
                                                    (x / zoom)];
                const double alpha = ((source >> 24) & 0xFF) / 255.0;
                if (alpha <= 0.0) {
                    continue;
                }
                auto& target = image[static_cast<std::size_t>(y) * width + tile * cell + x];
                const auto mix = [alpha](std::uint32_t over, std::uint32_t under, int shift) {
                    const double a = static_cast<double>((over >> shift) & 0xFF);
                    const double b = static_cast<double>((under >> shift) & 0xFF);
                    return static_cast<std::uint32_t>(b + (a - b) * alpha) << shift;
                };
                target = mix(source, target, 16) | mix(source, target, 8) | mix(source, target, 0);
            }
        }
        // And at actual size, twice over, so the real thing can be judged too.
        for (int copy = 0; copy < 2; ++copy) {
            const int origin_x = tile * cell + 8 + copy * (cursor_size + 12);
            const int origin_y = cell + (small_cell - cursor_size) / 2;
            for (int y = 0; y < cursor_size; ++y) {
                for (int x = 0; x < cursor_size; ++x) {
                    const std::uint32_t source =
                        cursor[static_cast<std::size_t>(y) * cursor_size + x];
                    const double alpha = ((source >> 24) & 0xFF) / 255.0;
                    if (alpha <= 0.0) {
                        continue;
                    }
                    const int px = origin_x + x;
                    const int py = origin_y + y;
                    if (px < 0 || px >= width || py < 0 || py >= height) {
                        continue;
                    }
                    auto& target = image[static_cast<std::size_t>(py) * width + px];
                    const auto mix = [alpha](std::uint32_t over, std::uint32_t under, int shift) {
                        const double a = static_cast<double>((over >> shift) & 0xFF);
                        const double b = static_cast<double>((under >> shift) & 0xFF);
                        return static_cast<std::uint32_t>(b + (a - b) * alpha) << shift;
                    };
                    target =
                        mix(source, target, 16) | mix(source, target, 8) | mix(source, target, 0);
                }
            }
        }
    }

    write_bmp(path, image, width, height);
    return 0;
}
