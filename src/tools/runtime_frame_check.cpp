// Replicates the genx player's update_framebuffer exactly and reports what it
// actually produces.
//
// Written because I verified a display fix with gg_probe, which uses the
// per-scanline path, and told the user it was fixed - while the runtime takes a
// different path and was still wrong. Testing the path the user actually runs
// is the whole point of this tool.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mercury/gamegear/game_gear.hpp"

namespace {

using mercury::gamegear::GameGear;

bool write_bmp(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& rgb,
               std::size_t width,
               std::size_t height) {
    const std::uint32_t stride = static_cast<std::uint32_t>((width * 3 + 3) & ~std::size_t(3));
    const std::uint32_t pixels = stride * static_cast<std::uint32_t>(height);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto u16 = [&](std::uint16_t v) { out.put(char(v & 0xFF)); out.put(char((v >> 8) & 0xFF)); };
    auto u32 = [&](std::uint32_t v) {
        out.put(char(v & 0xFF)); out.put(char((v >> 8) & 0xFF));
        out.put(char((v >> 16) & 0xFF)); out.put(char((v >> 24) & 0xFF));
    };
    out.put('B'); out.put('M');
    u32(14 + 40 + pixels); u16(0); u16(0); u32(14 + 40);
    u32(40); u32(static_cast<std::uint32_t>(width)); u32(static_cast<std::uint32_t>(height));
    u16(1); u16(24); u32(0); u32(pixels); u32(2835); u32(2835); u32(0); u32(0);
    std::vector<char> row(stride, 0);
    for (std::size_t y = 0; y < height; ++y) {
        const auto source = (height - 1 - y) * width * 3;
        for (std::size_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = static_cast<char>(rgb[source + x * 3 + 2]);
            row[x * 3 + 1] = static_cast<char>(rgb[source + x * 3 + 1]);
            row[x * 3 + 2] = static_cast<char>(rgb[source + x * 3 + 0]);
        }
        out.write(row.data(), static_cast<std::streamsize>(stride));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: runtime_frame_check <rom> <frames> [out.bmp]\n");
        return 2;
    }
    const std::filesystem::path rom = argv[1];
    const int target_frames = std::atoi(argv[2]);

    GameGear system;
    const auto load = system.load_rom(rom);
    if (!load.ok) {
        std::printf("load failed: %s\n", load.error.c_str());
        return 1;
    }

    // Drive it the way the runtime does, pressing START periodically so the
    // game gets past its logo into the mode it actually plays in.
    int frame = 0;
    while (frame < target_frames) {
        const auto before = system.vdp().frames_elapsed();
        int guard = 0;
        while (system.vdp().frames_elapsed() == before && guard < 2000000) {
            const auto r = system.step();
            if (!r.executed && r.unimplemented) break;
            ++guard;
        }
        ++frame;
        const bool press = (frame % 70) < 20 && frame > 200 && frame < 1600;
        system.input_state().set_button(mercury::gamegear::Button::start, press);
        system.input_state().set_button(mercury::gamegear::Button::button1,
                                        ((frame + 35) % 70) < 18 && frame > 200 && frame < 1600);
    }

    // --- this block mirrors update_framebuffer exactly ---
    const auto& presented = system.vdp().presented_frame_pixels();
    std::vector<std::uint8_t> rgb_frame;
    const char* source = nullptr;
    if (!presented.empty()) {
        rgb_frame = presented;
        source = "presented_frame_pixels()";
    } else {
        rgb_frame = system.vdp().visible_frame_pixels(true);
        source = "visible_frame_pixels(true)";
    }
    const auto width = system.vdp().visible_width();
    const auto height = system.vdp().visible_height();
    const auto needed = width * height * 3;

    std::printf("frame source      : %s\n", source);
    std::printf("buffer bytes      : %zu\n", rgb_frame.size());
    std::printf("visible_width()   : %zu\n", width);
    std::printf("visible_height()  : %zu\n", height);
    std::printf("bytes needed      : %zu\n", needed);
    std::printf("extended_display(): %s\n", system.vdp().extended_display() ? "yes (224 lines)" : "no (192 lines)");
    if (rgb_frame.size() < needed) {
        std::printf("*** SHORT BY %zu BYTES - the runtime reads past the end and wraps ***\n",
                    needed - rgb_frame.size());
    } else if (rgb_frame.size() > needed) {
        std::printf("(buffer is larger than needed by %zu bytes; only the first %zu are shown)\n",
                    rgb_frame.size() - needed, needed);
    } else {
        std::printf("buffer size matches exactly\n");
    }

    // Also report the second path, so the two can be compared directly.
    const auto alternate = system.vdp().visible_frame_pixels(true);
    std::printf("visible_frame_pixels(true) bytes: %zu  (%s)\n",
                alternate.size(),
                alternate.size() == needed ? "matches" : "MISMATCH");

    if (argc >= 4 && rgb_frame.size() >= needed) {
        rgb_frame.resize(needed);
        if (write_bmp(argv[3], rgb_frame, width, height)) {
            std::printf("wrote %s\n", argv[3]);
        }
    }
    return 0;
}
