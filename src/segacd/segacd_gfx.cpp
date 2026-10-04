// The Sega CD's graphics ASIC: rotation and scaling in hardware.
//
// The picture to transform is a "stamp map" - a grid of 16x16 or 32x32-pixel
// stamps, each of which can be flipped and rotated - held in 2M Word RAM. For
// every line it renders, the chip reads a "trace vector" (a start position and
// a per-pixel step, in fixed point) and walks the stamp map along it, writing
// 4-bit pixels into an image buffer laid out as Mega Drive tiles. So a line of
// output can sample the source along any straight line at any rate: that is
// the whole trick behind Sonic CD's special stages and every "Mode 7"-looking
// floor on the system.
//
// The register layout, the fixed-point formats and the timing model (three
// sub-CPU cycles per Word RAM access, counted per line) are Genesis Plus GX's,
// which took the timing from MiSTer's logic analyser captures.

#include <array>

#include "mercury/segacd/segacd.hpp"

namespace mercury::segacd {

namespace {

// Which of a stamp's cells a position falls in, after flip and rotation.
// Index: yyxxshrr - cell row, cell column, stamp size (32x32), then HFLIP and
// the two rotation bits.
std::array<std::uint8_t, 0x100> build_cell_table() {
    std::array<std::uint8_t, 0x100> table{};
    for (std::uint32_t i = 0; i < 0x100; ++i) {
        const std::uint32_t mask = (i & 8) != 0 ? 3 : 1;
        std::uint32_t row = (i >> 6) & mask;
        std::uint32_t col = (i >> 4) & mask;
        if ((i & 4) != 0) { col ^= mask; }                          // HFLIP, always first
        if ((i & 2) != 0) { col ^= mask; row ^= mask; }             // 180 degrees
        if ((i & 1) != 0) { const auto t = col; col = row ^ mask; row = t; }  // 90 degrees
        table[i] = static_cast<std::uint8_t>(row + col * (mask + 1));
    }
    return table;
}

// Which pixel of a cell, after flip and rotation. Index: yyyxxxhrr.
std::array<std::uint8_t, 0x200> build_pixel_table() {
    std::array<std::uint8_t, 0x200> table{};
    for (std::uint32_t i = 0; i < 0x200; ++i) {
        std::uint32_t row = (i >> 6) & 7;
        std::uint32_t col = (i >> 3) & 7;
        if ((i & 4) != 0) { col ^= 7; }
        if ((i & 2) != 0) { col ^= 7; row ^= 7; }
        if ((i & 1) != 0) { const auto t = col; col = row ^ 7; row = t; }
        table[i] = static_cast<std::uint8_t>(col + row * 8);
    }
    return table;
}

const std::array<std::uint8_t, 0x100>& cell_table() {
    static const auto table = build_cell_table();
    return table;
}

const std::array<std::uint8_t, 0x200>& pixel_table() {
    static const auto table = build_pixel_table();
    return table;
}

std::uint8_t priority_merge(int mode, std::uint8_t previous, std::uint8_t incoming) noexcept {
    const auto pick = [](std::uint8_t first, std::uint8_t second, std::uint8_t mask) {
        return static_cast<std::uint8_t>((first & mask) != 0 ? (first & mask) : (second & mask));
    };
    switch (mode & 3) {
        case 1: return static_cast<std::uint8_t>(pick(previous, incoming, 0x0F) | pick(previous, incoming, 0xF0));
        case 2: return static_cast<std::uint8_t>(pick(incoming, previous, 0x0F) | pick(incoming, previous, 0xF0));
        case 3: return previous;
        default: return incoming;
    }
}

}  // namespace

void SegaCd::gfx_reset() noexcept {
    gfx_ = Gfx{};
    gfx_.clock = now_;
}

// Writing the trace vector base starts an operation. Every other parameter
// was set up beforehand.
void SegaCd::gfx_start(std::uint16_t trace_base) noexcept {
    gfx_.trace = (static_cast<std::uint32_t>(trace_base) << 2) & 0x3FFF8;
    std::uint32_t map_mask = 0;
    switch ((lo(0x58 >> 1) >> 1) & 0x03) {
        case 0:  // 256x256 dots, 16x16-dot stamps
            gfx_.dot_mask = 0x07FFFF; gfx_.stamp_shift = 11 + 4; gfx_.map_shift = 4; map_mask = 0x3FE00;
            break;
        case 1:  // 256x256 dots, 32x32-dot stamps
            gfx_.dot_mask = 0x07FFFF; gfx_.stamp_shift = 11 + 5; gfx_.map_shift = 3; map_mask = 0x3FF80;
            break;
        case 2:  // 4096x4096 dots, 16x16-dot stamps
            gfx_.dot_mask = 0x7FFFFF; gfx_.stamp_shift = 11 + 4; gfx_.map_shift = 8; map_mask = 0x20000;
            break;
        default:  // 4096x4096 dots, 32x32-dot stamps
            gfx_.dot_mask = 0x7FFFFF; gfx_.stamp_shift = 11 + 5; gfx_.map_shift = 7; map_mask = 0x38000;
            break;
    }
    gfx_.map_base = (static_cast<std::uint32_t>(regs_[0x5A >> 1]) << 2) & map_mask;
    // The image buffer is tiles: 8 pixels a line within a cell, then on down
    // a column of cells. The offset jumps from the end of one cell's line to
    // the same line of the next cell across.
    gfx_.buffer_offset = (((lo(0x5C >> 1) & 0x1F) + 1) << 6) - 7;
    gfx_.buffer_start = ((static_cast<std::uint32_t>(regs_[0x5E >> 1]) << 3) & 0x7FFC0) +
                        (lo(0x60 >> 1) & 0x3F);
    gfx_.clock = now_;
    // Word RAM accesses per line: four to read the trace vector, two per
    // pixel (stamp map, then stamp data), and one read-modify-write per four
    // pixels written. Three sub-CPU cycles each.
    const std::uint32_t width = regs_[0x62 >> 1] & 0x1FF;
    const std::uint32_t writes = (width + (lo(0x60 >> 1) & 0x03) + 3) >> 2;
    gfx_.cycles_per_line = 3 * (4 + 2 * width + writes);
    if (gfx_.cycles_per_line == 0) gfx_.cycles_per_line = 1;
    set_hi(0x58 >> 1, 0x80);  // busy
}

void SegaCd::gfx_update() noexcept {
    // The chip needs Word RAM on the sub side (2M with RET clear, or 1M).
    if ((lo(0x01) & 0x05) == 0x01) {
        gfx_.clock = now_;  // halted while the main CPU has it
        return;
    }
    if (now_ <= gfx_.clock) {
        return;
    }
    std::uint64_t lines = (now_ - gfx_.clock + gfx_.cycles_per_line - 1) / gfx_.cycles_per_line;
    const std::uint32_t remaining = lo(0x64 >> 1);
    bool finished = false;
    if (lines < remaining) {
        set_lo(0x64 >> 1, static_cast<std::uint8_t>(remaining - lines));
    } else {
        lines = remaining;
        set_lo(0x64 >> 1, 0);
        finished = true;
    }
    gfx_.clock += lines * gfx_.cycles_per_line;
    const std::uint32_t width = regs_[0x62 >> 1] & 0x1FF;
    for (std::uint64_t line = 0; line < lines; ++line) {
        gfx_render_line(gfx_.buffer_start, width);
        gfx_.buffer_start += 8;  // the next line of the same cells
    }
    if (finished) {
        set_hi(0x58 >> 1, 0x00);
        if ((lo(0x19) & 0x02) != 0) {
            raise(1);
        }
    }
}

void SegaCd::gfx_render_line(std::uint32_t buffer_index, std::uint32_t width) noexcept {
    const bool large_stamps = (lo(0x58 >> 1) & 0x02) != 0;
    const bool repeat = (lo(0x58 >> 1) & 0x01) != 0;
    // The two low bits of a 32x32 stamp's number are ignored (Chuck Rock II).
    const std::uint32_t stamp_mask = large_stamps ? 0x7FC : 0x7FF;
    const auto word_at = [this](std::uint32_t offset) {
        offset &= 0x3FFFE;
        return static_cast<std::uint32_t>((word_ram_[offset] << 8) | word_ram_[offset + 1]);
    };

    // The trace vector: start position in 13.3 (made 13.11 here) and the
    // per-pixel step in signed 5.11. It wraps at the end of Word RAM.
    std::uint32_t xpos = word_at(gfx_.trace) << 8;
    std::uint32_t ypos = word_at(gfx_.trace + 2) << 8;
    const auto xstep = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word_at(gfx_.trace + 4))));
    const auto ystep = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word_at(gfx_.trace + 6))));
    gfx_.trace = (gfx_.trace + 8) & 0x3FFF8;

    const int mode = (lo(0x01) >> 3) & 3;
    const auto& cells = cell_table();
    const auto& pixels = pixel_table();
    while (width-- > 0) {
        if (repeat) {
            xpos &= gfx_.dot_mask;
            ypos &= gfx_.dot_mask;
        } else {
            xpos &= 0xFFFFFF;
            ypos &= 0xFFFFFF;
        }
        std::uint8_t pixel = 0;
        if (((xpos | ypos) & ~gfx_.dot_mask) == 0) {
            const std::uint32_t entry = (xpos >> gfx_.stamp_shift) | ((ypos >> gfx_.stamp_shift) << gfx_.map_shift);
            const std::uint32_t stamp = word_at(gfx_.map_base + entry * 2);
            std::uint32_t index = (stamp & stamp_mask) << 8;
            if (index != 0) {  // stamp 0 is never drawn
                const std::uint32_t orientation = (stamp >> 13) & 7;
                index |= static_cast<std::uint32_t>(
                             cells[orientation | (large_stamps ? 8u : 0u) | ((ypos >> 8) & 0xC0) | ((xpos >> 10) & 0x30)])
                         << 6;
                index |= pixels[orientation | ((xpos >> 8) & 0x38) | ((ypos >> 5) & 0x1C0)];
                const std::uint8_t pair = word_ram_[(index >> 1) & 0x3FFFF];
                pixel = (index & 1) != 0 ? static_cast<std::uint8_t>(pair & 0x0F)
                                         : static_cast<std::uint8_t>(pair >> 4);
            }
        }
        // Into the image buffer, two pixels a byte, through the priority mode.
        const std::uint32_t at = (buffer_index >> 1) & 0x3FFFF;
        const std::uint8_t previous = word_ram_[at];
        const std::uint8_t merged = (buffer_index & 1) != 0
                                        ? static_cast<std::uint8_t>((previous & 0xF0) | pixel)
                                        : static_cast<std::uint8_t>((pixel << 4) | (previous & 0x0F));
        word_ram_[at] = priority_merge(mode, previous, merged);
        if ((buffer_index & 7) != 7) {
            ++buffer_index;
        } else {
            buffer_index += gfx_.buffer_offset;
        }
        xpos += xstep;
        ypos += ystep;
    }
}

}  // namespace mercury::segacd
