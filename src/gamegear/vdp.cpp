#include "mercury/gamegear/vdp.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace mercury::gamegear {

namespace {

struct BackgroundRenderResult {
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> sprite_priority_mask;
};

struct ScanlineRenderResult {
    std::array<std::uint8_t, Vdp::kMaxVisibleWidth * 3> pixels{};
    std::array<std::uint8_t, Vdp::kMaxVisibleWidth> sprite_priority_mask{};
};

constexpr std::size_t kTileWidth = 8;
constexpr std::size_t kTileHeight = 8;
constexpr std::size_t kBytesPerTile = 32;
constexpr std::size_t kMapWidthTiles = 32;
constexpr std::size_t kMapHeightTiles = 28;
constexpr std::size_t kMapWidthPixels = kMapWidthTiles * kTileWidth;
constexpr std::size_t kMapHeightPixels = kMapHeightTiles * kTileHeight;
// The tilemap is 32x32 rather than 32x28 in the extended line modes.
constexpr std::size_t kExtendedMapHeightPixels = 32 * kTileHeight;

// Mode 4 has three display heights, selected by M1-M4 spread across R0 and R1:
// M4 M3 M2 M1 = 1 0 0 0 -> 192 lines, 1 0 1 1 -> 224 lines, 1 1 1 0 -> 240.
// The 224/240 variants exist only on the SMS2/Game Gear VDP and change how the
// name table base is decoded, so they cannot be ignored even when the extra
// lines themselves fall outside the visible window.
[[nodiscard]] bool extended_line_mode(const std::array<std::uint8_t, 16>& registers) {
    const bool m2 = (registers[0] & 0x02) != 0;
    const bool m4 = (registers[0] & 0x04) != 0;
    const bool m3 = (registers[1] & 0x08) != 0;
    const bool m1 = (registers[1] & 0x10) != 0;
    if (!m4 || !m2) {
        return false;
    }
    return (m1 && !m3) || (m3 && !m1);
}

[[nodiscard]] std::size_t map_height_pixels(const std::array<std::uint8_t, 16>& registers) {
    return extended_line_mode(registers) ? kExtendedMapHeightPixels : kMapHeightPixels;
}

// NTSC frames are always 262 lines; the extended modes spend more of them on
// active display and correspondingly fewer on blanking.
[[nodiscard]] std::size_t active_display_lines(const std::array<std::uint8_t, 16>& registers) {
    return extended_line_mode(registers) ? 224 : 192;
}

// Scanline 0 in this core is the first vblank line, so the active area begins
// after the leading blank lines: 43 of them in 192-line mode, 27 in 224-line.
// Those counts are what make the documented V counter sequences come out right
// (D5-FF then 00-DA at 192 lines; E5-FF then 00-EA at 224).
[[nodiscard]] std::size_t active_area_start(const std::array<std::uint8_t, 16>& registers) {
    return extended_line_mode(registers) ? 27 : 43;
}

[[nodiscard]] std::uint8_t v_counter_wrap_base(const std::array<std::uint8_t, 16>& registers) {
    return extended_line_mode(registers) ? 0xE5 : 0xD5;
}

// In 192-line mode R2 bits 3-1 pick the base in 2KB steps. In the extended
// modes only bits 3-2 select it, and the table sits 0x700 above the 4KB
// boundary they choose.
[[nodiscard]] std::size_t name_table_base_address(const std::array<std::uint8_t, 16>& registers) {
    if (extended_line_mode(registers)) {
        return static_cast<std::size_t>(((registers[2] & 0x0C) << 10) | 0x0700) & 0x3FFF;
    }
    return static_cast<std::size_t>(((registers[2] & 0x0E) << 10) & 0x3FFF);
}
constexpr std::size_t kGameGearViewportXOffset = 48;
constexpr std::size_t kGameGearViewportYOffset = 24;
// The VDP always renders the same 256x192 scene; Game Gear just shows the
// middle of it. Master System mode shows all of it, from the origin.
constexpr std::size_t viewport_x_offset(Vdp::DisplayMode mode) {
    return mode == Vdp::DisplayMode::master_system ? 0 : kGameGearViewportXOffset;
}
constexpr std::size_t viewport_y_offset(Vdp::DisplayMode mode) {
    return mode == Vdp::DisplayMode::master_system ? 0 : kGameGearViewportYOffset;
}
constexpr std::size_t viewport_width(Vdp::DisplayMode mode) {
    return mode == Vdp::DisplayMode::master_system ? Vdp::kMasterSystemWidth : Vdp::kVisibleWidth;
}
// Needs the registers, not just the mode: the 224-line variant is selected by
// M1-M4 at runtime. Leaving this at a fixed 192 made the whole-frame render
// path disagree with the scanline path about how tall a frame is, so a caller
// that asked for 224 rows got 192 and wrapped into the top of the picture.
std::size_t viewport_height(Vdp::DisplayMode mode,
                            const std::array<std::uint8_t, 16>& registers) {
    if (mode != Vdp::DisplayMode::master_system) {
        return Vdp::kVisibleHeight;
    }
    return extended_line_mode(registers) ? Vdp::kMasterSystemHeightExtended
                                         : Vdp::kMasterSystemHeight;
}
constexpr std::size_t kFrameEffectiveAreaStartScanline = 43;
constexpr std::size_t kFrameEffectiveAreaEndScanline =
    kFrameEffectiveAreaStartScanline + Vdp::kVisibleScanlines;
// Display line 0xC1 in 192-line mode: one line past the end of the active
// area, which is where the VDP raises the frame-interrupt flag.
constexpr std::size_t kFrameInterruptScanline = kFrameEffectiveAreaEndScanline + 1;
constexpr std::size_t kGameGearViewportStartScanline =
    kFrameEffectiveAreaStartScanline + kGameGearViewportYOffset;
constexpr std::size_t kGameGearViewportEndScanline =
    kGameGearViewportStartScanline + Vdp::kVisibleHeight;
constexpr std::size_t kGameGearTopRowsInhibitPixels = 16;
constexpr std::size_t kGameGearRightColumnsInhibitPixels = 16;
// Real hardware asserts the frame IRQ at the start of the line where the
// status flag transitions to 1. We don't model the small intra-line phase
// offset and simply let the CPU sample the line as soon as it's set.
constexpr std::uint32_t kFrameInterruptAssertDelayCycles = 0;
constexpr std::uint32_t kHCounterCountsPerScanline = 171;
constexpr std::uint32_t kHCounterWrapPrefixCounts = 23;

std::uint8_t compute_h_counter(std::uint32_t cycles_into_scanline) {
    const auto h_count =
        std::min<std::uint32_t>(
            kHCounterCountsPerScanline - 1,
            (cycles_into_scanline * kHCounterCountsPerScanline) / Vdp::kCpuCyclesPerScanline);
    if (h_count < kHCounterWrapPrefixCounts) {
        return static_cast<std::uint8_t>(0xE9 + h_count);
    }
    return static_cast<std::uint8_t>(h_count - kHCounterWrapPrefixCounts);
}

std::array<std::uint8_t, 3> decode_debug_color(
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    std::size_t palette_index,
    std::uint8_t color_index,
    Vdp::DisplayMode mode) {
    const auto entry = static_cast<std::size_t>(palette_index * 16 + (color_index & 0x0F));

    if (mode == Vdp::DisplayMode::master_system) {
        // Master System CRAM is 32 single bytes laid out --BBGGRR, two bits per
        // channel. Replicate each pair across the byte so 0b11 reaches 0xFF.
        const auto value = cram[entry];
        const auto expand2 = [](std::uint8_t pair) {
            const auto p = static_cast<std::uint8_t>(pair & 0x03);
            return static_cast<std::uint8_t>((p << 6) | (p << 4) | (p << 2) | p);
        };
        return {expand2(static_cast<std::uint8_t>(value)),
                expand2(static_cast<std::uint8_t>(value >> 2)),
                expand2(static_cast<std::uint8_t>(value >> 4))};
    }

    // Game Gear CRAM is 32 words, ----BBBBGGGGRRRR: red and green in the even
    // byte, blue in the odd one.
    const auto cram_index = entry * 2;
    const auto byte0 = cram[cram_index];
    const auto byte1 = cram[cram_index + 1];

    const auto expand = [](std::uint8_t nibble) {
        return static_cast<std::uint8_t>((nibble << 4) | nibble);
    };

    const auto red = expand(static_cast<std::uint8_t>(byte0 & 0x0F));
    const auto green = expand(static_cast<std::uint8_t>((byte0 >> 4) & 0x0F));
    const auto blue = expand(static_cast<std::uint8_t>(byte1 & 0x0F));
    return {red, green, blue};
}

std::array<std::uint8_t, 3> decode_backdrop_color(
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    Vdp::DisplayMode mode) {
    return decode_debug_color(cram, 1, static_cast<std::uint8_t>(registers[7] & 0x0F), mode);
}

bool write_bmp24(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& pixels,
                 std::size_t image_width,
                 std::size_t image_height) {
    const std::uint32_t row_stride = static_cast<std::uint32_t>((image_width * 3 + 3) & ~std::size_t(3));
    const std::uint32_t pixel_data_size = row_stride * static_cast<std::uint32_t>(image_height);
    const std::uint32_t file_size = 14 + 40 + pixel_data_size;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const auto write_u16 = [&out](std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
    };
    const auto write_u32 = [&out](std::uint32_t value) {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
        out.put(static_cast<char>((value >> 16) & 0xFF));
        out.put(static_cast<char>((value >> 24) & 0xFF));
    };

    out.put('B');
    out.put('M');
    write_u32(file_size);
    write_u16(0);
    write_u16(0);
    write_u32(14 + 40);

    write_u32(40);
    write_u32(static_cast<std::uint32_t>(image_width));
    write_u32(static_cast<std::uint32_t>(image_height));
    write_u16(1);
    write_u16(24);
    write_u32(0);
    write_u32(pixel_data_size);
    write_u32(2835);
    write_u32(2835);
    write_u32(0);
    write_u32(0);

    std::vector<std::uint8_t> row(row_stride, 0x00);
    for (std::size_t y = image_height; y-- > 0;) {
        for (std::size_t x = 0; x < image_width; ++x) {
            const auto src = (y * image_width + x) * 3;
            const auto dst = x * 3;
            row[dst + 0] = pixels[src + 2];
            row[dst + 1] = pixels[src + 1];
            row[dst + 2] = pixels[src + 0];
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    return out.good();
}

BackgroundRenderResult render_background_pixels(
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    const std::array<std::uint8_t, Vdp::kMaxVisibleHeight>* horizontal_scroll_latch,
    std::uint8_t vertical_scroll_latch,
    std::size_t image_width,
    std::size_t image_height,
    std::size_t source_x_offset,
    std::size_t source_y_offset,
    bool apply_scroll,
    bool force_blank,
    Vdp::DisplayMode mode) {
    BackgroundRenderResult result{};
    result.pixels.assign(image_width * image_height * 3, 0x00);
    result.sprite_priority_mask.assign(image_width * image_height, 0x00);
    if (force_blank) {
        const auto rgb = decode_backdrop_color(cram, registers, mode);
        for (std::size_t pixel_index = 0; pixel_index < image_width * image_height; ++pixel_index) {
            result.pixels[pixel_index * 3 + 0] = rgb[0];
            result.pixels[pixel_index * 3 + 1] = rgb[1];
            result.pixels[pixel_index * 3 + 2] = rgb[2];
        }
        return result;
    }

    const auto name_table_base = name_table_base_address(registers);
    const auto vertical_scroll = static_cast<std::size_t>(vertical_scroll_latch);
    const bool horizontal_scroll_inhibit_top_rows = (registers[0] & 0x40) != 0;
    const bool vertical_scroll_inhibit_right_columns = (registers[0] & 0x80) != 0;
    const bool left_column_blank = (registers[0] & 0x20) != 0;
    const auto backdrop_rgb = decode_backdrop_color(cram, registers, mode);

    for (std::size_t y = 0; y < image_height; ++y) {
        const auto screen_y = source_y_offset + y;
        std::size_t horizontal_scroll = static_cast<std::size_t>(registers[8]);
        if (apply_scroll && horizontal_scroll_latch != nullptr && screen_y < Vdp::kMaxVisibleHeight) {
            horizontal_scroll = static_cast<std::size_t>((*horizontal_scroll_latch)[screen_y]);
        }
        // In GG mode we crop to the central 160x144 viewport, so the SMS/GG top
        // two rows scroll lock only applies if the cropped source area still
        // overlaps visible scanlines 0-15 of the 256x192 display. With the
        // normal GG crop starting at y=24 this has no visible effect.
        if (apply_scroll && horizontal_scroll_inhibit_top_rows && screen_y < kGameGearTopRowsInhibitPixels) {
            horizontal_scroll = 0;
        }

        for (std::size_t x = 0; x < image_width; ++x) {
            const auto screen_x = source_x_offset + x;
            if (apply_scroll && left_column_blank && screen_x < 8) {
                const auto pixel_index = (y * image_width + x) * 3;
                result.pixels[pixel_index + 0] = backdrop_rgb[0];
                result.pixels[pixel_index + 1] = backdrop_rgb[1];
                result.pixels[pixel_index + 2] = backdrop_rgb[2];
                result.sprite_priority_mask[y * image_width + x] = 0x01;
                continue;
            }

            std::size_t effective_vertical_scroll = apply_scroll ? vertical_scroll : 0;
            if (apply_scroll &&
                vertical_scroll_inhibit_right_columns &&
                screen_x >= (kMapWidthPixels - 64)) {
                effective_vertical_scroll = 0;
            }

            const auto source_y =
                (source_y_offset + y + effective_vertical_scroll) % map_height_pixels(registers);
            const auto tile_y = source_y / kTileHeight;
            const auto row_in_tile = source_y % kTileHeight;
            auto source_x = source_x_offset + x;
            if (apply_scroll) {
                // See render_background_scanline: increasing R8 shifts the
                // picture right, so the source pixel is screen_x minus scroll.
                source_x += kMapWidthPixels - (horizontal_scroll % kMapWidthPixels);
            }
            source_x %= kMapWidthPixels;

            const auto tile_x = source_x / kTileWidth;
            const auto column_in_tile = source_x % kTileWidth;
            const auto entry_address =
                (name_table_base + (tile_y * kMapWidthTiles + tile_x) * 2) & 0x3FFF;
            const auto low = vram[entry_address];
            const auto high = vram[(entry_address + 1) & 0x3FFF];

            const auto tile_index = static_cast<std::size_t>(low | ((high & 0x01) << 8));
            const bool horizontal_flip = (high & 0x02) != 0;
            const bool vertical_flip = (high & 0x04) != 0;
            const std::size_t palette_index = (high & 0x08) != 0 ? 1 : 0;
            const auto tile_base = (tile_index * kBytesPerTile) & 0x3FFF;
            const auto source_row = vertical_flip ? (kTileHeight - 1 - row_in_tile) : row_in_tile;
            const auto plane0 = vram[tile_base + source_row * 4 + 0];
            const auto plane1 = vram[tile_base + source_row * 4 + 1];
            const auto plane2 = vram[tile_base + source_row * 4 + 2];
            const auto plane3 = vram[tile_base + source_row * 4 + 3];
            const auto source_column =
                horizontal_flip ? (kTileWidth - 1 - column_in_tile) : column_in_tile;
            const auto bit = static_cast<std::uint8_t>(7 - source_column);
            const auto color_index =
                static_cast<std::uint8_t>(((plane0 >> bit) & 0x01) |
                                          (((plane1 >> bit) & 0x01) << 1) |
                                          (((plane2 >> bit) & 0x01) << 2) |
                                          (((plane3 >> bit) & 0x01) << 3));

            const bool high_priority = (high & 0x10) != 0;
            const auto rgb = decode_debug_color(cram, palette_index, color_index, mode);
            const auto pixel_index = (y * image_width + x) * 3;
            result.pixels[pixel_index + 0] = rgb[0];
            result.pixels[pixel_index + 1] = rgb[1];
            result.pixels[pixel_index + 2] = rgb[2];
            if (color_index != 0 && high_priority) {
                result.sprite_priority_mask[y * image_width + x] = 0x01;
            }
        }
    }

    return result;
}

ScanlineRenderResult render_background_scanline(
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    const std::array<std::uint8_t, Vdp::kMaxVisibleHeight>& horizontal_scroll_latch,
    std::uint8_t vertical_scroll_latch,
    std::size_t visible_y,
    bool display_enabled,
    Vdp::DisplayMode mode) {
    ScanlineRenderResult result{};
    const auto backdrop_rgb = decode_backdrop_color(cram, registers, mode);
    if (!display_enabled) {
        for (std::size_t x = 0; x < viewport_width(mode); ++x) {
            const auto pixel_index = x * 3;
            result.pixels[pixel_index + 0] = backdrop_rgb[0];
            result.pixels[pixel_index + 1] = backdrop_rgb[1];
            result.pixels[pixel_index + 2] = backdrop_rgb[2];
            result.sprite_priority_mask[x] = 0x01;
        }
        return result;
    }

    const auto name_table_base = name_table_base_address(registers);
    const bool horizontal_scroll_inhibit_top_rows = (registers[0] & 0x40) != 0;
    const bool vertical_scroll_inhibit_right_columns = (registers[0] & 0x80) != 0;
    const bool left_column_blank = (registers[0] & 0x20) != 0;
    const auto screen_y = viewport_y_offset(mode) + visible_y;

    std::size_t horizontal_scroll = static_cast<std::size_t>(horizontal_scroll_latch[screen_y]);
    if (horizontal_scroll_inhibit_top_rows && screen_y < kGameGearTopRowsInhibitPixels) {
        horizontal_scroll = 0;
    }

    for (std::size_t x = 0; x < viewport_width(mode); ++x) {
        const auto screen_x = viewport_x_offset(mode) + x;
        const auto pixel_index = x * 3;
        if (left_column_blank && screen_x < 8) {
            result.pixels[pixel_index + 0] = backdrop_rgb[0];
            result.pixels[pixel_index + 1] = backdrop_rgb[1];
            result.pixels[pixel_index + 2] = backdrop_rgb[2];
            result.sprite_priority_mask[x] = 0x01;
            continue;
        }

        std::size_t effective_vertical_scroll = static_cast<std::size_t>(vertical_scroll_latch);
        if (vertical_scroll_inhibit_right_columns && screen_x >= (kMapWidthPixels - 64)) {
            effective_vertical_scroll = 0;
        }

        const auto source_y = (screen_y + effective_vertical_scroll) % map_height_pixels(registers);
        const auto tile_y = source_y / kTileHeight;
        const auto row_in_tile = source_y % kTileHeight;
        // R8 scrolls the picture to the RIGHT as it increases, so the source
        // pixel is screen_x MINUS the scroll (the VDP docs phrase this as the
        // starting column being 32 minus the register's upper five bits).
        // R9 is the opposite sense and stays an addition.
        auto source_x =
            (screen_x + kMapWidthPixels - (horizontal_scroll % kMapWidthPixels)) % kMapWidthPixels;
        const auto tile_x = source_x / kTileWidth;
        const auto column_in_tile = source_x % kTileWidth;
        const auto entry_address =
            (name_table_base + (tile_y * kMapWidthTiles + tile_x) * 2) & 0x3FFF;
        const auto low = vram[entry_address];
        const auto high = vram[(entry_address + 1) & 0x3FFF];

        const auto tile_index = static_cast<std::size_t>(low | ((high & 0x01) << 8));
        const bool horizontal_flip = (high & 0x02) != 0;
        const bool vertical_flip = (high & 0x04) != 0;
        const std::size_t palette_index = (high & 0x08) != 0 ? 1 : 0;
        const auto tile_base = (tile_index * kBytesPerTile) & 0x3FFF;
        const auto source_row = vertical_flip ? (kTileHeight - 1 - row_in_tile) : row_in_tile;
        const auto plane0 = vram[tile_base + source_row * 4 + 0];
        const auto plane1 = vram[tile_base + source_row * 4 + 1];
        const auto plane2 = vram[tile_base + source_row * 4 + 2];
        const auto plane3 = vram[tile_base + source_row * 4 + 3];
        const auto source_column =
            horizontal_flip ? (kTileWidth - 1 - column_in_tile) : column_in_tile;
        const auto bit = static_cast<std::uint8_t>(7 - source_column);
        const auto color_index =
            static_cast<std::uint8_t>(((plane0 >> bit) & 0x01) |
                                      (((plane1 >> bit) & 0x01) << 1) |
                                      (((plane2 >> bit) & 0x01) << 2) |
                                      (((plane3 >> bit) & 0x01) << 3));

        const auto rgb = decode_debug_color(cram, palette_index, color_index, mode);
        result.pixels[pixel_index + 0] = rgb[0];
        result.pixels[pixel_index + 1] = rgb[1];
        result.pixels[pixel_index + 2] = rgb[2];
        if (color_index != 0 && (high & 0x10) != 0) {
            result.sprite_priority_mask[x] = 0x01;
        }
    }

    return result;
}

std::uint8_t decode_pattern_color(
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    std::size_t tile_base,
    std::size_t row,
    std::size_t column) {
    const auto plane0 = vram[(tile_base + row * 4 + 0) & 0x3FFF];
    const auto plane1 = vram[(tile_base + row * 4 + 1) & 0x3FFF];
    const auto plane2 = vram[(tile_base + row * 4 + 2) & 0x3FFF];
    const auto plane3 = vram[(tile_base + row * 4 + 3) & 0x3FFF];
    const auto bit = static_cast<std::uint8_t>(7 - column);
    return static_cast<std::uint8_t>(((plane0 >> bit) & 0x01) |
                                     (((plane1 >> bit) & 0x01) << 1) |
                                     (((plane2 >> bit) & 0x01) << 2) |
                                     (((plane3 >> bit) & 0x01) << 3));
}

void render_sprite_overlay(
    std::vector<std::uint8_t>& pixels,
    std::size_t image_width,
    std::size_t image_height,
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    const std::vector<std::uint8_t>* sprite_priority_mask,
    std::size_t viewport_x_offset,
    std::size_t viewport_y_offset,
    Vdp::DisplayMode mode);

// Status bits the sprite unit produces as a side effect of drawing a line.
// Games read them; Fantastic Dizzy spins on the collision bit and never starts
// without it.
struct SpriteScanlineFlags {
    bool collision = false;
    bool overflow = false;
};

SpriteScanlineFlags render_sprite_scanline(
    std::uint8_t* pixels,
    const std::array<std::uint8_t, Vdp::kMaxVisibleWidth>& sprite_priority_mask,
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    std::size_t visible_y,
    Vdp::DisplayMode mode) {
    SpriteScanlineFlags flags;
    // Collision is detected across the whole 256-pixel active display, not just
    // the part a Game Gear happens to show, so occupancy is tracked in display
    // space rather than viewport space.
    std::array<bool, kMapWidthPixels> occupied{};
    const auto sprite_attribute_table_base = static_cast<std::size_t>((registers[5] & 0x7E) << 7);
    const auto sprite_generator_table_base = static_cast<std::size_t>((registers[6] & 0x04) << 11);
    const bool sprite_size_8x16 = (registers[1] & 0x02) != 0;
    const bool early_clock = (registers[0] & 0x08) != 0;
    const std::size_t sprite_height = sprite_size_8x16 ? 16 : 8;
    const int scanline_y = static_cast<int>(visible_y + viewport_y_offset(mode));
    int sprite_count = 64;
    int sprites_on_line = 0;

    // The Y = D0h sprite-list terminator ONLY works in 192-line mode. In the
    // 224- and 240-line modes D0h is an ordinary on-screen Y coordinate, so
    // honouring it there truncates the list at whatever sprite happens to sit
    // at that height and every sprite after it vanishes. That is what made
    // Fantastic Dizzy's highlighted menu entry flicker and the character
    // disappear while moving: a sprite's Y crosses D0h and takes the rest of
    // the list with it.
    if (!extended_line_mode(registers)) {
        for (int sprite_index = 0; sprite_index < 64; ++sprite_index) {
            const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
            if (y_value == 0xD0) {
                sprite_count = sprite_index;
                break;
            }
        }
    }

    // The VDP scans the sprite table in INDEX ORDER and latches the first
    // eight sprites that overlap this line; every later one is dropped (and
    // the overflow flag is set). Selecting them while walking the table
    // backwards kept the eight HIGHEST indices instead of the eight lowest,
    // i.e. exactly the sprites hardware discards - so on a crowded line the
    // wrong sprites vanished.
    std::array<int, 8> line_sprites{};
    for (int sprite_index = 0; sprite_index < sprite_count; ++sprite_index) {
        const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
        if (y_value == 0xE0) {
            continue;
        }
        int sprite_y = static_cast<int>(y_value) + 1;
        if (sprite_y >= 0xE0) {
            sprite_y -= 0x100;
        }
        const int row = scanline_y - sprite_y;
        if (row < 0 || row >= static_cast<int>(sprite_height)) {
            continue;
        }
        if (sprites_on_line >= 8) {
            // A ninth sprite on the line sets the overflow bit and is dropped.
            // The scan has to keep going to find it, which is why the cutoff
            // lives here rather than in the loop condition.
            flags.overflow = true;
            break;
        }
        line_sprites[static_cast<std::size_t>(sprites_on_line)] = sprite_index;
        ++sprites_on_line;
    }

    // Draw back to front so the lower index, which has priority, lands on top.
    for (int slot = sprites_on_line - 1; slot >= 0; --slot) {
        const int sprite_index = line_sprites[static_cast<std::size_t>(slot)];
        const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
        int sprite_y = static_cast<int>(y_value) + 1;
        if (sprite_y >= 0xE0) {
            sprite_y -= 0x100;
        }
        const int row = scanline_y - sprite_y;

        const auto attribute_base =
            (sprite_attribute_table_base + 0x80 + static_cast<std::size_t>(sprite_index) * 2) & 0x3FFF;
        const auto x_value = vram[attribute_base];
        auto tile_number = vram[(attribute_base + 1) & 0x3FFF];

        int sprite_x = static_cast<int>(x_value);
        if (early_clock) {
            sprite_x -= 8;
        }
        if (sprite_size_8x16) {
            tile_number = static_cast<std::uint8_t>(tile_number & 0xFE);
        }

        const auto pattern_index = static_cast<std::size_t>(tile_number) + (static_cast<std::size_t>(row) / 8);
        const auto tile_base = (sprite_generator_table_base + pattern_index * kBytesPerTile) & 0x3FFF;
        const auto tile_row = static_cast<std::size_t>(row) % 8;

        for (std::size_t column = 0; column < kTileWidth; ++column) {
            const auto color_index = decode_pattern_color(vram, tile_base, tile_row, column);
            if (color_index == 0) {
                continue;
            }
            // Collision is a property of the sprite unit, independent of what
            // ends up visible: it fires wherever two opaque sprite pixels land
            // on the same display column, even behind the background or
            // outside the Game Gear's crop.
            const int screen_x = sprite_x + static_cast<int>(column);
            if (screen_x >= 0 && screen_x < static_cast<int>(kMapWidthPixels)) {
                if (occupied[static_cast<std::size_t>(screen_x)]) {
                    flags.collision = true;
                }
                occupied[static_cast<std::size_t>(screen_x)] = true;
            }

            const int dest_x = screen_x - static_cast<int>(viewport_x_offset(mode));
            if (dest_x < 0 || dest_x >= static_cast<int>(viewport_width(mode))) {
                continue;
            }
            if (sprite_priority_mask[static_cast<std::size_t>(dest_x)] != 0) {
                continue;
            }
            const auto rgb = decode_debug_color(cram, 1, color_index, mode);
            const auto pixel_index = static_cast<std::size_t>(dest_x) * 3;
            pixels[pixel_index + 0] = rgb[0];
            pixels[pixel_index + 1] = rgb[1];
            pixels[pixel_index + 2] = rgb[2];
        }
    }
    return flags;
}

std::vector<std::uint8_t> render_visible_frame_pixels(
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    const std::array<std::uint8_t, Vdp::kMaxVisibleHeight>& horizontal_scroll_latch,
    std::uint8_t vertical_scroll_latch,
    bool display_enabled,
    bool ignore_blanking,
    bool include_background,
    bool include_sprites,
    Vdp::DisplayMode mode) {
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> sprite_priority_mask;
    if (include_background) {
        auto background = render_background_pixels(
            vram,
            cram,
            registers,
            &horizontal_scroll_latch,
            vertical_scroll_latch,
            viewport_width(mode),
            viewport_height(mode, registers),
            viewport_x_offset(mode),
            viewport_y_offset(mode),
            true,
            !ignore_blanking && !display_enabled,
            mode);
        pixels = std::move(background.pixels);
        sprite_priority_mask = std::move(background.sprite_priority_mask);
    } else {
        pixels.assign(viewport_width(mode) * viewport_height(mode, registers) * 3, 0x00);
    }

    if (include_sprites && (ignore_blanking || display_enabled)) {
        render_sprite_overlay(
            pixels,
            viewport_width(mode),
            viewport_height(mode, registers),
            vram,
            cram,
            registers,
            include_background ? &sprite_priority_mask : nullptr,
            viewport_x_offset(mode),
            viewport_y_offset(mode),
            mode);
    }

    return pixels;
}

void render_sprite_overlay(
    std::vector<std::uint8_t>& pixels,
    std::size_t image_width,
    std::size_t image_height,
    const std::array<std::uint8_t, Vdp::kVramSize>& vram,
    const std::array<std::uint8_t, Vdp::kColorRamWordCount * 2>& cram,
    const std::array<std::uint8_t, 16>& registers,
    const std::vector<std::uint8_t>* sprite_priority_mask,
    std::size_t viewport_x_offset,
    std::size_t viewport_y_offset,
    Vdp::DisplayMode mode) {
    const auto sprite_attribute_table_base = static_cast<std::size_t>((registers[5] & 0x7E) << 7);
    const auto sprite_generator_table_base = static_cast<std::size_t>((registers[6] & 0x04) << 11);
    const bool sprite_size_8x16 = (registers[1] & 0x02) != 0;
    const bool early_clock = (registers[0] & 0x08) != 0;
    const std::size_t sprite_height = sprite_size_8x16 ? 16 : 8;
    int sprite_count = 64;
    std::array<std::uint8_t, Vdp::kVisibleHeight> sprites_per_line{};
    std::array<std::uint16_t, 64> drawable_row_masks{};

    // The Y = D0h sprite-list terminator ONLY works in 192-line mode. In the
    // 224- and 240-line modes D0h is an ordinary on-screen Y coordinate, so
    // honouring it there truncates the list at whatever sprite happens to sit
    // at that height and every sprite after it vanishes. That is what made
    // Fantastic Dizzy's highlighted menu entry flicker and the character
    // disappear while moving: a sprite's Y crosses D0h and takes the rest of
    // the list with it.
    if (!extended_line_mode(registers)) {
        for (int sprite_index = 0; sprite_index < 64; ++sprite_index) {
            const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
            if (y_value == 0xD0) {
                sprite_count = sprite_index;
                break;
            }
        }
    }

    for (int sprite_index = 0; sprite_index < sprite_count; ++sprite_index) {
        const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
        if (y_value == 0xE0) {
            continue;
        }

        int sprite_y = static_cast<int>(y_value) + 1;
        if (sprite_y >= 0xE0) {
            sprite_y -= 0x100;
        }

        std::uint16_t row_mask = 0;
        for (std::size_t row = 0; row < sprite_height; ++row) {
            const auto dest_y = sprite_y + static_cast<int>(row) - static_cast<int>(viewport_y_offset);
            if (dest_y < 0 || dest_y >= static_cast<int>(image_height)) {
                continue;
            }

            auto& count = sprites_per_line[static_cast<std::size_t>(dest_y)];
            if (count >= 8) {
                continue;
            }

            ++count;
            row_mask |= static_cast<std::uint16_t>(1u << row);
        }

        drawable_row_masks[static_cast<std::size_t>(sprite_index)] = row_mask;
    }

    for (int sprite_index = sprite_count - 1; sprite_index >= 0; --sprite_index) {
        const auto y_value = vram[(sprite_attribute_table_base + static_cast<std::size_t>(sprite_index)) & 0x3FFF];
        if (y_value == 0xE0) {
            continue;
        }
        const auto drawable_row_mask = drawable_row_masks[static_cast<std::size_t>(sprite_index)];
        if (drawable_row_mask == 0) {
            continue;
        }

        const auto attribute_base =
            (sprite_attribute_table_base + 0x80 + static_cast<std::size_t>(sprite_index) * 2) & 0x3FFF;
        const auto x_value = vram[attribute_base];
        auto tile_number = vram[(attribute_base + 1) & 0x3FFF];

        int sprite_x = static_cast<int>(x_value);
        if (early_clock) {
            sprite_x -= 8;
        }

        int sprite_y = static_cast<int>(y_value) + 1;
        if (sprite_y >= 0xE0) {
            sprite_y -= 0x100;
        }

        if (sprite_size_8x16) {
            tile_number = static_cast<std::uint8_t>(tile_number & 0xFE);
        }

        for (std::size_t row = 0; row < sprite_height; ++row) {
            if ((drawable_row_mask & static_cast<std::uint16_t>(1u << row)) == 0) {
                continue;
            }
            const auto pattern_index = static_cast<std::size_t>(tile_number) + (row / 8);
            const auto tile_base =
                (sprite_generator_table_base + pattern_index * kBytesPerTile) & 0x3FFF;
            const auto tile_row = row % 8;

            const auto dest_y = sprite_y + static_cast<int>(row) - static_cast<int>(viewport_y_offset);
            if (dest_y < 0 || dest_y >= static_cast<int>(image_height)) {
                continue;
            }

            for (std::size_t column = 0; column < kTileWidth; ++column) {
                const auto color_index = decode_pattern_color(vram, tile_base, tile_row, column);
                if (color_index == 0) {
                    continue;
                }

                const auto dest_x = sprite_x + static_cast<int>(column) - static_cast<int>(viewport_x_offset);
                if (dest_x < 0 || dest_x >= static_cast<int>(image_width)) {
                    continue;
                }

                if (sprite_priority_mask != nullptr &&
                    (*sprite_priority_mask)[static_cast<std::size_t>(dest_y) * image_width +
                                           static_cast<std::size_t>(dest_x)] != 0) {
                    continue;
                }

                const auto rgb = decode_debug_color(cram, 1, color_index, mode);
                const auto pixel_index =
                    (static_cast<std::size_t>(dest_y) * image_width + static_cast<std::size_t>(dest_x)) * 3;
                pixels[pixel_index + 0] = rgb[0];
                pixels[pixel_index + 1] = rgb[1];
                pixels[pixel_index + 2] = rgb[2];
            }
        }
    }
}

}  // namespace

void Vdp::set_display_mode(DisplayMode mode) noexcept {
    mode_ = mode;
}

bool Vdp::extended_display() const noexcept {
    return extended_line_mode(registers_);
}

std::size_t Vdp::visible_height() const noexcept {
    if (mode_ != DisplayMode::master_system) {
        // The Game Gear LCD is 144 lines whatever the VDP is doing; the extra
        // lines of an extended mode simply fall outside the window.
        return kVisibleHeight;
    }
    return extended_line_mode(registers_) ? kMasterSystemHeightExtended : kMasterSystemHeight;
}
void Vdp::reset() noexcept {
    vram_.fill(0x00);
    vram_written_.fill(0x00);
    cram_.fill(0x00);
    registers_.fill(0x00);
    // The VDP's own reset leaves R10 at 1, but on a real machine the boot ROM
    // runs first and pre-loads the registers before handing control to the
    // cartridge. R10 = 1 means a line interrupt every 2 lines, which fires
    // spurious raster interrupts at any game that enables IE1 without setting
    // R10 itself - Rise of the Robots writes only R0-R4 and is destroyed by
    // them, because its handler treats a line interrupt as a dispatch through
    // a table that only the frame-interrupt path initialises. 0xFF makes the
    // counter reload during vblank without ever underflowing, so no line
    // interrupt occurs until a game actually asks for one.
    registers_[10] = 0xFF;
    registers_[15] = 0x01;
    v_counter_ = 0xB0;
    h_counter_ = 0x00;
    status_ = 0x00;
    data_buffer_ = 0xFF;
    control_first_byte_ = 0x00;
    control_second_byte_ = 0x00;
    awaiting_second_control_byte_ = false;
    address_ = 0x0000;
    code_ = 0x00;
    scanline_ = 0;
    cycles_into_scanline_ = 0;
    frame_interrupt_pending_ = false;
    line_interrupt_pending_ = false;
    frame_interrupt_assert_delay_cycles_ = 0;
    line_counter_ = registers_[10];
    frames_elapsed_ = 0;
    frame_interrupt_count_ = 0;
    line_interrupt_count_ = 0;
    horizontal_scroll_latch_.fill(0x00);
    vertical_scroll_latch_ = 0x00;
    status_read_count_ = 0;
    data_read_count_ = 0;
    control_write_count_ = 0;
    data_write_count_ = 0;
    register_write_count_ = 0;
    vram_write_count_ = 0;
    cram_write_count_ = 0;
    last_register_index_ = 0x00;
    last_register_value_ = 0x00;
    last_data_address_ = 0x0000;
    last_data_was_cram_ = false;
    cram_latch_ = 0x00;
    // Sized for the tallest mode: a game can switch to the 224-line display at
    // any point (Fantastic Dizzy does it after its logo), and the buffer must
    // not have to reallocate mid-frame.
    presented_frame_pixels_.assign(kMaxVisibleWidth * kMaxVisibleHeight * 3, 0x00);
    set_v_counter_from_scanline();
    h_counter_ = compute_h_counter(cycles_into_scanline_);
}

void Vdp::sanitize_loaded() noexcept {
    if (mode_ != DisplayMode::master_system) {
        mode_ = DisplayMode::game_gear;
    }
    // A position within a line is under one line, and a line number under one
    // frame; larger makes the next advance_cycles walk millions of lines.
    cycles_into_scanline_ %= kCpuCyclesPerScanline;
    scanline_ %= kScanlinesPerFrame;
    if (frame_interrupt_assert_delay_cycles_ > kCpuCyclesPerScanline) {
        frame_interrupt_assert_delay_cycles_ = 0;
    }
    address_ &= 0x3FFF;
}

void Vdp::advance_cycles(std::uint32_t cpu_cycles) noexcept {
    if (frame_interrupt_assert_delay_cycles_ > 0) {
        frame_interrupt_assert_delay_cycles_ =
            frame_interrupt_assert_delay_cycles_ > cpu_cycles
                ? frame_interrupt_assert_delay_cycles_ - cpu_cycles
                : 0;
    }

    cycles_into_scanline_ += cpu_cycles;
    h_counter_ = compute_h_counter(std::min<std::uint32_t>(cycles_into_scanline_, kCpuCyclesPerScanline - 1));

    while (cycles_into_scanline_ >= kCpuCyclesPerScanline) {
        cycles_into_scanline_ -= kCpuCyclesPerScanline;
        scanline_ = (scanline_ + 1) % kScanlinesPerFrame;
        set_v_counter_from_scanline();
        h_counter_ = compute_h_counter(cycles_into_scanline_);

        if (scanline_ == 0) {
            vertical_scroll_latch_ = registers_[9];
        }
        const auto area_start = active_area_start(registers_);
        const auto area_lines = active_display_lines(registers_);
        if (scanline_ >= area_start && scanline_ < area_start + area_lines) {
            horizontal_scroll_latch_[scanline_ - area_start] = registers_[8];
        }

        const auto viewport_start =
            area_start + viewport_y_offset(mode_);
        const auto viewport_end = viewport_start + visible_height();
        if (scanline_ >= viewport_start && scanline_ < viewport_end) {
            const auto visible_y = scanline_ - viewport_start;
            const auto display_on = display_enabled();
            const auto background = render_background_scanline(
                vram_,
                cram_,
                registers_,
                horizontal_scroll_latch_,
                vertical_scroll_latch_,
                visible_y,
                display_on,
                mode_);
            const auto line_base = visible_y * visible_width() * 3;
            std::copy(
                background.pixels.begin(),
                background.pixels.begin() + static_cast<std::ptrdiff_t>(visible_width() * 3),
                presented_frame_pixels_.begin() + static_cast<std::ptrdiff_t>(line_base));
            if (display_on) {
                const auto sprite_flags = render_sprite_scanline(
                    presented_frame_pixels_.data() + static_cast<std::ptrdiff_t>(line_base),
                    background.sprite_priority_mask,
                    vram_,
                    cram_,
                    registers_,
                    visible_y,
                    mode_);
                // Both bits latch until the CPU reads the status port, so they
                // accumulate across the frame rather than tracking one line.
                if (sprite_flags.collision) {
                    status_ |= 0x20;
                }
                if (sprite_flags.overflow) {
                    status_ |= 0x40;
                }
            }
        }

        // Mode 4 line interrupts count across the 192 active display lines
        // plus the first vblank line (193 decrements total per frame). When
        // the counter underflows from 0 it reloads from R10 and asserts the
        // line IRQ if IE1 (R0 bit 4) is set. The remaining vblank lines
        // continuously reload the counter from R10.
        if (scanline_ >= area_start && scanline_ <= area_start + area_lines) {
            if (line_counter_ == 0x00) {
                line_interrupt_pending_ = line_interrupt_enabled();
                if (line_interrupt_pending_) {
                    ++line_interrupt_count_;
                }
                line_counter_ = registers_[10];
            } else {
                --line_counter_;
            }
        } else {
            line_counter_ = registers_[10];
        }

        // The frame-interrupt flag sets on display line 0xC1 (193) in
        // 192-line mode - one line INTO vblank, not on the first vblank line.
        // That single line matters: with R10 = 192 the line interrupt lands on
        // the first vblank line, and if the frame flag were set there too the
        // two would coincide. Games whose IRQ handler does `BIT 7,A` on the
        // status byte to tell a line interrupt from a frame interrupt (Last
        // Action Hero and Spider-Man both do) would then never see a line
        // interrupt and would hang waiting for it.
        if (scanline_ == area_start + area_lines + 1) {
            status_ |= 0x80;
            frame_interrupt_pending_ = frame_interrupt_enabled();
            if (frame_interrupt_pending_) {
                ++frame_interrupt_count_;
                frame_interrupt_assert_delay_cycles_ = kFrameInterruptAssertDelayCycles;
            }
        }

        if (scanline_ == 0) {
            ++frames_elapsed_;
        }
    }

    h_counter_ = compute_h_counter(cycles_into_scanline_);
}

std::uint8_t Vdp::read_v_counter() const noexcept {
    return v_counter_;
}

std::uint8_t Vdp::read_h_counter() const noexcept {
    return h_counter_;
}

std::uint8_t Vdp::read_data() noexcept {
    awaiting_second_control_byte_ = false;
    ++data_read_count_;

    const auto value = data_buffer_;
    data_buffer_ = 0xFF;

    // SMS/GG VDP reads always come from VRAM through the read buffer,
    // regardless of the current code register value. The full 14-bit address
    // register advances independently; CRAM writes only ignore the upper bits
    // when the access itself is decoded as CRAM.
    const auto address = current_address();
    if (address < vram_.size()) {
        data_buffer_ = vram_[address];
    }
    increment_address();

    return value;
}

std::uint8_t Vdp::read_status() noexcept {
    awaiting_second_control_byte_ = false;
    ++status_read_count_;
    const auto value = status_;
    status_ = 0x00;
    frame_interrupt_pending_ = false;
    line_interrupt_pending_ = false;
    frame_interrupt_assert_delay_cycles_ = 0;
    return value;
}

void Vdp::write_data(std::uint8_t value) noexcept {
    awaiting_second_control_byte_ = false;
    ++data_write_count_;
    data_buffer_ = value;

    const auto address = current_address();
    last_data_address_ = address;

    if (code_ == 0x03) {
        last_data_was_cram_ = true;
        if (mode_ == DisplayMode::master_system) {
            // Master System CRAM is 32 single bytes, written directly - there
            // is no even-address latch, that is a Game Gear addition.
            const auto cram_address = static_cast<std::uint16_t>(address & 0x001F);
            cram_[cram_address] = value;
            ++cram_write_count_;
            increment_address();
            return;
        }
        const auto cram_address = static_cast<std::uint16_t>(address & 0x003F);
        if ((cram_address & 0x01) == 0) {
            cram_latch_ = value;
        } else if (cram_address < cram_.size()) {
            const auto even_address = static_cast<std::size_t>(cram_address - 1);
            cram_[even_address] = cram_latch_;
            cram_[cram_address] = value;
            cram_write_count_ += 2;
        }
    } else {
        last_data_was_cram_ = false;
        if (address < vram_.size()) {
            vram_[address] = value;
            vram_written_[address >> 3] |= static_cast<std::uint8_t>(1u << (address & 0x07));
            ++vram_write_count_;
        }
    }

    increment_address();
}

void Vdp::write_control(std::uint8_t value) noexcept {
    ++control_write_count_;
    if (!awaiting_second_control_byte_) {
        control_first_byte_ = value;
        address_ = static_cast<std::uint16_t>((address_ & 0xFF00) | control_first_byte_);
        awaiting_second_control_byte_ = true;
        return;
    }

    control_second_byte_ = value;
    awaiting_second_control_byte_ = false;

    code_ = static_cast<std::uint8_t>((control_second_byte_ >> 6) & 0x03);
    address_ = static_cast<std::uint16_t>(((control_second_byte_ & 0x3F) << 8) | control_first_byte_);

    if (code_ == 0x02) {
        const auto register_index = static_cast<std::size_t>(control_second_byte_ & 0x0F);
        if (register_index < registers_.size()) {
            registers_[register_index] = control_first_byte_;
            ++register_write_count_;
            last_register_index_ = static_cast<std::uint8_t>(register_index);
            last_register_value_ = control_first_byte_;
            // R10 is only the RELOAD value for the line counter, never the
            // counter itself. The live counter keeps counting down and takes
            // the new value at its next underflow (or from the continuous
            // reload during vblank, handled in advance_cycles).
            //
            // Slamming the counter here breaks the common multi-split idiom:
            // a handler that reprograms R10 for the *next* split does so
            // after the underflow has already reloaded the counter with the
            // previous value, so the current split period must survive the
            // write. George Foreman's KO Boxing runs a three-stage split and
            // writes R10=FF from the first line handler; overwriting the
            // counter there killed the second line interrupt, so its state
            // machine never advanced and the match froze on the first frame.
        }
    } else if (code_ == 0x00) {
        const auto address = current_address();
        if (address < vram_.size()) {
            data_buffer_ = vram_[address];
        } else {
            data_buffer_ = 0xFF;
        }
        increment_address();
    }
}

bool Vdp::display_enabled() const noexcept {
    return (registers_[1] & 0x40) != 0;
}

bool Vdp::access_requires_wait() const noexcept {
    return display_enabled() &&
           scanline_ >= kFrameEffectiveAreaStartScanline &&
           scanline_ < kFrameEffectiveAreaEndScanline &&
           h_counter_ <= 0x93;
}

std::uint32_t Vdp::frames_elapsed() const noexcept {
    return frames_elapsed_;
}

bool Vdp::frame_interrupt_enabled() const noexcept {
    return (registers_[1] & 0x20) != 0;
}

bool Vdp::line_interrupt_enabled() const noexcept {
    return (registers_[0] & 0x10) != 0;
}

bool Vdp::interrupt_requested() const noexcept {
    // On real hardware the F flag in the status register is what drives the
    // frame IRQ line: bit 7 is set each vblank and stays set until the CPU
    // reads the status port. The IRQ line is therefore asserted any time
    // bit 7 is set AND R1.IE is currently enabled â€” even if IE was toggled on
    // after vblank started. The latched pending bit only governs the optional
    // post-assertion delay used to model the small hardware sample window.
    const bool frame_flag_set = (status_ & 0x80) != 0;
    const bool frame_irq_asserted =
        frame_flag_set && frame_interrupt_enabled() &&
        frame_interrupt_assert_delay_cycles_ == 0;
    return frame_irq_asserted ||
           (line_interrupt_pending_ && line_interrupt_enabled());
}

std::string Vdp::describe() const {
    std::ostringstream out;
    out << "VDP: " << kVramSize << " bytes VRAM, "
        << kColorRamWordCount << " color RAM words, "
        << "visible LCD " << kVisibleWidth << "x" << kVisibleHeight
        << ", virtual tile plane " << kVirtualTileWidth << "x" << kVirtualTileHeight
        << ", control address " << current_address()
        << ", scanline " << scanline_;
    return out.str();
}

std::string Vdp::activity_summary() const {
    const auto nonzero_vram = static_cast<std::uint32_t>(std::count_if(
        vram_.begin(),
        vram_.end(),
        [](std::uint8_t value) { return value != 0x00; }));
    const auto nonzero_cram = static_cast<std::uint32_t>(std::count_if(
        cram_.begin(),
        cram_.end(),
        [](std::uint8_t value) { return value != 0x00; }));

    std::ostringstream out;
    out << "VDP activity: "
        << "frames=" << frames_elapsed_
        << ", frame IRQs=" << frame_interrupt_count_
        << ", line IRQs=" << line_interrupt_count_
        << ", control writes=" << control_write_count_
        << ", register writes=" << register_write_count_
        << ", data writes=" << data_write_count_
        << ", VRAM writes=" << vram_write_count_
        << ", CRAM writes=" << cram_write_count_
        << ", data reads=" << data_read_count_
        << ", status reads=" << status_read_count_
        << ", nonzero VRAM=" << nonzero_vram
        << "/" << kVramSize
        << ", nonzero CRAM=" << nonzero_cram
        << "/" << cram_.size()
        << ", display=" << (display_enabled() ? "on" : "off")
        << ", scroll=(" << std::dec << static_cast<int>(registers_[8])
        << "," << static_cast<int>(registers_[9]) << ")"
        << ", last data target="
        << (last_data_was_cram_ ? "CRAM[" : "VRAM[")
        << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << last_data_address_
        << "]"
        << ", last reg=#" << std::dec << static_cast<int>(last_register_index_)
        << "=" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(last_register_value_);
    return out.str();
}

std::string Vdp::register_summary() const {
    std::ostringstream out;
    out << "VDP regs:";
    for (std::size_t index = 0; index < registers_.size(); ++index) {
        out << " r" << std::dec << index
            << "=" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(registers_[index]);
    }
    out << " addr=" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << current_address()
        << " code=" << std::setw(2) << static_cast<int>(code_)
        << " scanline=" << std::dec << scanline_
        << " hcounter=" << static_cast<int>(h_counter_)
        << " vcounter=" << static_cast<int>(v_counter_)
        << " line_counter=" << static_cast<int>(line_counter_);
    return out.str();
}

std::vector<std::uint8_t> Vdp::visible_frame_pixels(bool ignore_blanking) const {
    return render_visible_frame_pixels(
        vram_,
        cram_,
        registers_,
        horizontal_scroll_latch_,
        vertical_scroll_latch_,
        display_enabled(),
        ignore_blanking,
        true,
        true,
        mode_);
}

const std::vector<std::uint8_t>& Vdp::presented_frame_pixels() const noexcept {
    return presented_frame_pixels_;
}

std::vector<std::uint8_t> Vdp::visible_background_pixels(bool ignore_blanking) const {
    return render_visible_frame_pixels(
        vram_,
        cram_,
        registers_,
        horizontal_scroll_latch_,
        vertical_scroll_latch_,
        display_enabled(),
        ignore_blanking,
        true,
        false,
        mode_);
}

std::vector<std::uint8_t> Vdp::visible_sprite_pixels(bool ignore_blanking) const {
    return render_visible_frame_pixels(
        vram_,
        cram_,
        registers_,
        horizontal_scroll_latch_,
        vertical_scroll_latch_,
        display_enabled(),
        ignore_blanking,
        false,
        true,
        mode_);
}

bool Vdp::export_tilesheet_bmp(const std::filesystem::path& path) const {
    constexpr std::size_t kTileColumns = 16;
    constexpr std::size_t kTileCount = kVramSize / kBytesPerTile;
    constexpr std::size_t kTileRows = kTileCount / kTileColumns;
    constexpr std::size_t kPaletteCount = 2;

    const std::size_t image_width = kTileColumns * kTileWidth * kPaletteCount;
    const std::size_t image_height = kTileRows * kTileHeight;
    std::vector<std::uint8_t> pixels(image_width * image_height * 3, 0x00);

    for (std::size_t tile_index = 0; tile_index < kTileCount; ++tile_index) {
        const auto tile_base = tile_index * kBytesPerTile;
        const auto tile_x = (tile_index % kTileColumns) * kTileWidth;
        const auto tile_y = (tile_index / kTileColumns) * kTileHeight;

        for (std::size_t row = 0; row < kTileHeight; ++row) {
            const auto plane0 = vram_[tile_base + row * 4 + 0];
            const auto plane1 = vram_[tile_base + row * 4 + 1];
            const auto plane2 = vram_[tile_base + row * 4 + 2];
            const auto plane3 = vram_[tile_base + row * 4 + 3];

            for (std::size_t column = 0; column < kTileWidth; ++column) {
                const auto bit = static_cast<std::uint8_t>(7 - column);
                const auto color_index =
                    static_cast<std::uint8_t>(((plane0 >> bit) & 0x01) |
                                              (((plane1 >> bit) & 0x01) << 1) |
                                              (((plane2 >> bit) & 0x01) << 2) |
                                              (((plane3 >> bit) & 0x01) << 3));

                for (std::size_t palette_index = 0; palette_index < kPaletteCount; ++palette_index) {
                    const auto rgb = decode_debug_color(cram_, palette_index, color_index, mode_);
                    const auto x = tile_x + column + palette_index * (kTileColumns * kTileWidth);
                    const auto y = tile_y + row;
                    const auto pixel_index = (y * image_width + x) * 3;
                    pixels[pixel_index + 0] = rgb[0];
                    pixels[pixel_index + 1] = rgb[1];
                    pixels[pixel_index + 2] = rgb[2];
                }
            }
        }
    }

    return write_bmp24(path, pixels, image_width, image_height);
}

bool Vdp::export_background_map_bmp(const std::filesystem::path& path) const {
    // Export the whole tilemap for the current mode - 32x32 in the extended
    // line modes. Clamping this to 28 rows hid the rows a scrolled frame
    // actually reads, which is exactly where a corruption can be hiding.
    const auto map_height = map_height_pixels(registers_);
    const auto background = render_background_pixels(
        vram_,
        cram_,
        registers_,
        nullptr,
        registers_[9],
        kMapWidthPixels,
        map_height,
        0,
        0,
        false,
        false,
        mode_);
    return write_bmp24(path, background.pixels, kMapWidthPixels, map_height);
}

bool Vdp::export_visible_frame_bmp(const std::filesystem::path& path) const {
    if (presented_frame_pixels_.size() == visible_width() * visible_height() * 3) {
        return write_bmp24(path, presented_frame_pixels_, visible_width(), visible_height());
    }
    const auto pixels = visible_frame_pixels(false);
    return write_bmp24(path, pixels, visible_width(), visible_height());
}

bool Vdp::export_visible_frame_debug_bmp(const std::filesystem::path& path) const {
    const auto pixels = visible_frame_pixels(true);
    return write_bmp24(path, pixels, visible_width(), visible_height());
}

Vdp::GfxAudit Vdp::debug_gfx_audit() const noexcept {
    GfxAudit audit;

    const auto was_written = [this](std::size_t address) {
        address &= 0x3FFF;
        return (vram_written_[address >> 3] & (1u << (address & 0x07))) != 0;
    };
    const auto region_written = [&was_written](std::size_t base, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            if (was_written(base + i)) {
                return true;
            }
        }
        return false;
    };
    const auto pattern_written = [&was_written](std::size_t tile_base) {
        for (std::size_t i = 0; i < kBytesPerTile; ++i) {
            if (was_written(tile_base + i)) {
                return true;
            }
        }
        return false;
    };

    const auto name_table_base = name_table_base_address(registers_);
    const auto sprite_table_base = static_cast<std::size_t>((registers_[5] & 0x7E) << 7);
    const auto sprite_pattern_base = static_cast<std::size_t>((registers_[6] & 0x04) << 11);

    audit.name_table_written = region_written(name_table_base, 32 * 28 * 2);
    audit.sprite_table_written = region_written(sprite_table_base, 256);
    audit.sprite_patterns_written = region_written(sprite_pattern_base, kBytesPerTile * 8);

    // Walk exactly the tiles the Game Gear viewport shows.
    std::array<bool, 512> seen_tile{};
    for (std::size_t y = 0; y < kVisibleHeight; y += kTileHeight) {
        for (std::size_t x = 0; x < kVisibleWidth; x += kTileWidth) {
            const auto source_y = (kGameGearViewportYOffset + y +
                                   static_cast<std::size_t>(vertical_scroll_latch_)) %
                                  kMapHeightPixels;
            const auto source_x = (kGameGearViewportXOffset + x + kMapWidthPixels -
                                   (static_cast<std::size_t>(registers_[8]) % kMapWidthPixels)) %
                                  kMapWidthPixels;
            const auto entry_address =
                (name_table_base + ((source_y / kTileHeight) * kMapWidthTiles +
                                    (source_x / kTileWidth)) * 2) & 0x3FFF;
            const auto tile_index = static_cast<std::size_t>(
                vram_[entry_address] | ((vram_[(entry_address + 1) & 0x3FFF] & 0x01) << 8));

            ++audit.visible_tiles;
            if (tile_index < seen_tile.size() && !seen_tile[tile_index]) {
                seen_tile[tile_index] = true;
                ++audit.distinct_tile_indices;
            }
            if (!pattern_written((tile_index * kBytesPerTile) & 0x3FFF)) {
                ++audit.tiles_unwritten;
            }
        }
    }

    // Sprites: how many are live, and how many land outside what the LCD shows.
    for (int i = 0; i < 64; ++i) {
        const auto y_value = vram_[(sprite_table_base + static_cast<std::size_t>(i)) & 0x3FFF];
        if (y_value == 0xD0) {
            break;
        }
        if (y_value == 0xE0) {
            continue;
        }
        ++audit.active_sprites;

        const auto attribute_base =
            (sprite_table_base + 0x80 + static_cast<std::size_t>(i) * 2) & 0x3FFF;
        const int sprite_x = static_cast<int>(vram_[attribute_base]) -
                             ((registers_[0] & 0x08) != 0 ? 8 : 0);
        int sprite_y = static_cast<int>(y_value) + 1;
        if (sprite_y >= 0xE0) {
            sprite_y -= 0x100;
        }
        const int dx = sprite_x - static_cast<int>(viewport_x_offset(mode_));
        const int dy = sprite_y - static_cast<int>(viewport_y_offset(mode_));
        if (dx <= -8 || dx >= static_cast<int>(visible_width()) ||
            dy <= -16 || dy >= static_cast<int>(visible_height())) {
            ++audit.sprites_offscreen;
        }
    }

    return audit;
}

std::uint16_t Vdp::debug_current_address() const noexcept {
    return current_address();
}

std::uint16_t Vdp::current_address() const noexcept {
    return static_cast<std::uint16_t>(address_ & 0x3FFF);
}

void Vdp::increment_address() noexcept {
    // SMS/Game Gear mode 4 does not implement the Mega Drive-style
    // auto-increment register; the VDP always advances the full 14-bit address
    // register by one. CRAM accesses only ignore the upper address bits during
    // the write itself.
    constexpr std::uint16_t increment = 0x0001;
    address_ = static_cast<std::uint16_t>((address_ + increment) & 0x3FFF);
}

void Vdp::set_v_counter_from_scanline() noexcept {
    // The V counter follows the SMS/GG NTSC sequence documented by Sega, and it
    // differs per display height: D5-FF then 00-DA at 192 lines, E5-FF then
    // 00-EA at 224. Either way the frame is 262 lines and the values in the
    // wrap region appear twice.
    const auto area_start = active_area_start(registers_);
    if (scanline_ < area_start) {
        v_counter_ = static_cast<std::uint8_t>(v_counter_wrap_base(registers_) + scanline_);
    } else {
        v_counter_ = static_cast<std::uint8_t>(scanline_ - area_start);
    }
}

}  // namespace mercury::gamegear








