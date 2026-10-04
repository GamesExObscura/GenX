#include "mercury/thirtytwox/vdp.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mercury::thirtytwox {

namespace {

// 5 bits to 8. Repeating the top bits rather than multiplying keeps full
// white at 255 and costs nothing.
constexpr std::uint8_t expand5(std::uint16_t value) {
    const auto five = static_cast<std::uint8_t>(value & 0x1F);
    return static_cast<std::uint8_t>((five << 3) | (five >> 2));
}

constexpr std::uint16_t kThroughBit = 0x8000;

}  // namespace

void Vdp::reset() noexcept {
    bitmap_mode_ = 0;
    screen_shift_ = 0;
    fill_start_ = 0;
    fill_length_ = 0;
    // Powers on blanked. PEN is not stored - it is answered from the mode and
    // the blank at every read - and blank mode makes it reachable anyway.
    frame_buffer_control_ = kVBlank;
    pending_frame_select_ = false;
    fill_wait_ = 0;
}

std::uint16_t Vdp::read_register(std::uint32_t offset) const noexcept {
    switch (offset & 0x0E) {
        case kBitmapMode:
            // The console-type bit is reported, not stored - see kNotPal.
            return static_cast<std::uint16_t>(bitmap_mode_ | (ntsc_ ? kNotPal : 0u));
        case kScreenShift: return screen_shift_;
        case kFillStart: return fill_start_;
        case kFillLength: return fill_length_;
        case kFrameBufferControl:
            // The busy and palette flags are live, not stored: software polls
            // here to find out whether the frame buffer and the palette are
            // its to touch. See palette_accessible() for why PEN is not a
            // stored vertical-blank marker.
            return static_cast<std::uint16_t>(
                (frame_buffer_control_ & ~(kFillNotEnded | kPaletteEnable)) |
                (framebuffer_engaged() ? kFillNotEnded : 0u) |
                (palette_accessible() ? kPaletteEnable : 0u));
        default: return 0;
    }
}

void Vdp::select_frame(bool second) noexcept {
    frame_buffer_control_ = second
                                ? static_cast<std::uint16_t>(frame_buffer_control_ | kFrameSelect)
                                : static_cast<std::uint16_t>(frame_buffer_control_ & ~kFrameSelect);
    if (bus_ != nullptr) {
        bus_->set_displayed_bank(second ? 1 : 0);
    }
}

namespace {
// GENX_32X_TRACE: frame buffer swaps and video mode changes, stamped with a
// count of vertical blanks so they can be lined up against the SH-2 side.
// Bounded, because a game writes the select bit every frame it draws.
bool trace_swaps() {
    static const bool on = std::getenv("GENX_32X_TRACE") != nullptr;
    return on;
}
std::uint32_t g_vblanks = 0;
std::uint32_t g_swap_lines = 0;
constexpr std::uint32_t kSwapLineLimit = 300;
}  // namespace

void Vdp::start_vblank() noexcept {
    ++g_vblanks;
    frame_buffer_control_ |= kVBlank;
    // A swap the game asked for during active display happens now.
    if (pending_frame_select_ != ((frame_buffer_control_ & kFrameSelect) != 0)) {
        if (trace_swaps() && g_swap_lines < kSwapLineLimit) {
            ++g_swap_lines;
            std::fprintf(stderr, "[32x vdp] vblank %u: deferred swap applied, FS now %d\n",
                         g_vblanks, pending_frame_select_ ? 1 : 0);
        }
        select_frame(pending_frame_select_);
    }
}

void Vdp::end_vblank() noexcept {
    frame_buffer_control_ = static_cast<std::uint16_t>(frame_buffer_control_ & ~kVBlank);
}

void Vdp::write_register(std::uint32_t offset, std::uint16_t value) noexcept {
    switch (offset & 0x0E) {
        case kBitmapMode:
            if (trace_swaps() && ((value ^ bitmap_mode_) & kModeMask) != 0 &&
                g_swap_lines < kSwapLineLimit) {
                ++g_swap_lines;
                std::fprintf(stderr, "[32x vdp] vblank %u: mode %u -> %u%s\n", g_vblanks,
                             static_cast<unsigned>(bitmap_mode_ & kModeMask),
                             static_cast<unsigned>(value & kModeMask),
                             (frame_buffer_control_ & kVBlank) != 0 ? " (in vblank)" : "");
            }
            // Software cannot change which console it is plugged into.
            bitmap_mode_ = static_cast<std::uint16_t>(value & ~kNotPal);
            return;
        case kScreenShift: screen_shift_ = value; return;
        case kFillStart: fill_start_ = value; return;
        case kFillLength:
            // A byte, so the longest fill is 256 words.
            fill_length_ = static_cast<std::uint16_t>(value & 0x00FF);
            return;
        case kFillData: {
            // Writing the data register runs the fill: that many words plus
            // one, starting at the fill address, into the bank being drawn.
            //
            // The address wraps within its own low byte rather than carrying
            // into the high byte, so a fill stays inside one 256-word row of
            // the frame buffer however long it is asked to be. That is a
            // hardware quirk, not a guard against overrun.
            if (bus_ != nullptr) {
                bus_->fill_draw_bank(fill_start_, value, static_cast<int>(fill_length_) + 1);
            }
            // The memory is the fill's until it finishes, and software can
            // see that through the busy flag.
            fill_wait_ = 7u + 3u * (static_cast<std::uint32_t>(fill_length_) + 1u);
            fill_start_ = static_cast<std::uint16_t>(
                (fill_start_ & 0xFF00u) |
                ((fill_start_ + static_cast<std::uint16_t>(fill_length_) + 1) & 0x00FFu));
            return;
        }
        case kFrameBufferControl: {
            // FS is the only writable bit, and it is only obeyed now if the
            // screen is blanked or the display is off. Otherwise it waits.
            pending_frame_select_ = (value & kFrameSelect) != 0;
            const bool differs =
                pending_frame_select_ != ((frame_buffer_control_ & kFrameSelect) != 0);
            const bool now = (frame_buffer_control_ & kVBlank) != 0 || !produces_picture();
            if (trace_swaps() && g_swap_lines < kSwapLineLimit) {
                ++g_swap_lines;
                std::fprintf(stderr, "[32x vdp] vblank %u: FS write %d, mode %u - %s\n", g_vblanks,
                             pending_frame_select_ ? 1 : 0,
                             static_cast<unsigned>(bitmap_mode_ & kModeMask),
                             !differs ? "no change" : now ? "applied now" : "DEFERRED to vblank");
            }
            if (now && differs) {
                select_frame(pending_frame_select_);
            }
            return;
        }
        default: return;
    }
}

Vdp::Pixel Vdp::unpack_colour(std::uint16_t colour, bool screen_priority) noexcept {
    Pixel pixel;
    pixel.b = expand5(static_cast<std::uint16_t>(colour >> 10));
    pixel.g = expand5(static_cast<std::uint16_t>(colour >> 5));
    pixel.r = expand5(colour);
    pixel.opaque = true;
    // The through bit FLIPS this pixel to the other side of the Mega Drive's
    // layers from wherever the screen-wide priority put everything else. It
    // is not "in front" on its own - which is the easiest thing here to get
    // backwards, and would show up as a whole layer on the wrong side only
    // for the handful of pixels that set it.
    const bool through = (colour & kThroughBit) != 0;
    pixel.in_front = through ? !screen_priority : screen_priority;
    return pixel;
}

void Vdp::render_line(int line, Pixel* out) const noexcept {
    for (int x = 0; x < kWidth; ++x) {
        out[x] = Pixel{};
    }
    if (bus_ == nullptr || !produces_picture() || line < 0 || line >= kMaxHeight) {
        return;
    }

    const auto& frame = bus_->frame_buffer(bus_->displayed_bank());
    const bool priority = screen_priority();
    const auto& palette = bus_->palette();

    // Every line is found through the line table: 256 words at the very start
    // of the frame buffer, one per line, each holding a WORD offset to that
    // line's pixels. Scrolling, raster splits and showing the same line twice
    // are all done by rewriting these, so there is no scroll register to
    // look for.
    const std::size_t table_entry = static_cast<std::size_t>(line) * 2;
    if (table_entry + 1 >= frame.size()) {
        return;
    }
    const auto word_offset = static_cast<std::uint32_t>((frame[table_entry] << 8) |
                                                        frame[table_entry + 1]);
    std::size_t at = static_cast<std::size_t>(word_offset) * 2;

    const auto fetch16 = [&frame](std::size_t byte_offset) -> std::uint16_t {
        if (byte_offset + 1 >= frame.size()) {
            return 0;
        }
        return static_cast<std::uint16_t>((frame[byte_offset] << 8) | frame[byte_offset + 1]);
    };

    // NO VALUE IS TRANSPARENT IN ITSELF. Every pixel in every mode - palette
    // index 0 and a direct-colour word of zero included - is a colour like any
    // other, and where it lands is decided ONLY by the screen priority, that
    // colour's through bit, and whether the Mega Drive drew anything at that
    // spot (see composite_line). ares plots every pixel this way in all three
    // modes, with no zero test anywhere.
    //
    // This used to treat index 0 (and a zero word) as "nothing, the Mega Drive
    // shows through", which is right only when palette entry 0 happens to be
    // black without the through bit. Star Wars Arcade's SEGA splash is the
    // case where it is not: the game sets entry 0 so the whole background of
    // the 32X layer comes IN FRONT and blacks out the Mega Drive's tiled SEGA
    // text everywhere except inside the big letters. Skipping index 0 left the
    // tiled text showing across the whole screen instead.
    switch (mode()) {
        case Mode::packed_pixel: {
            // Two pixels to a word, each an index into the palette.
            //
            // The line table can only address whole words, so a one-pixel
            // horizontal scroll is impossible through it. That is what the
            // screen shift bit is for, and it shifts the whole line left by
            // one pixel.
            const int shift = (screen_shift_ & 1) != 0 ? 1 : 0;
            for (int x = 0; x < kWidth; ++x) {
                const std::size_t index_at = at + static_cast<std::size_t>(x + shift);
                if (index_at >= frame.size()) {
                    break;
                }
                out[x] = unpack_colour(palette[frame[index_at]], priority);
            }
            return;
        }

        case Mode::direct_colour: {
            // One word per pixel, the colour itself rather than an index.
            for (int x = 0; x < kWidth; ++x) {
                out[x] = unpack_colour(fetch16(at + static_cast<std::size_t>(x) * 2), priority);
            }
            return;
        }

        case Mode::run_length: {
            // Each word is a run: the low byte is a palette index and the
            // high byte is how many pixels less one. A line ends when it has
            // filled the width, not at any terminator. ares reads the word the
            // same way round (count high, colour low), which settles the order
            // this was once unsure of.
            int x = 0;
            std::size_t cursor = at;
            while (x < kWidth) {
                const std::uint16_t unit = fetch16(cursor);
                cursor += 2;
                if (cursor >= frame.size()) {
                    break;
                }
                const auto index = static_cast<std::uint8_t>(unit & 0x00FF);
                const int run = static_cast<int>((unit >> 8) & 0xFF) + 1;
                const Pixel pixel = unpack_colour(palette[index], priority);
                for (int step = 0; step < run && x < kWidth; ++step, ++x) {
                    out[x] = pixel;
                }
            }
            return;
        }

        case Mode::blank:
        default:
            return;
    }
}

void Vdp::composite_line(const Pixel* layer, std::uint8_t* rgb, const std::uint8_t* backdrop,
                         int width) noexcept {
    if (layer == nullptr || rgb == nullptr) {
        return;
    }
    for (int x = 0; x < width; ++x) {
        const Pixel& pixel = layer[x];
        if (!pixel.opaque) {
            continue;
        }
        // Behind the Mega Drive means visible only where the Mega Drive put
        // nothing. With no backdrop information at all the safe reading is
        // that the Mega Drive drew everywhere, so a layer behind stays
        // hidden rather than punching through a picture.
        if (!pixel.in_front) {
            if (backdrop == nullptr || backdrop[x] == 0) {
                continue;
            }
        }
        rgb[x * 3 + 0] = pixel.r;
        rgb[x * 3 + 1] = pixel.g;
        rgb[x * 3 + 2] = pixel.b;
    }
}

std::string Vdp::describe() const {
    static const char* kModeNames[] = {"blank", "packed pixel", "direct colour", "run length"};
    std::ostringstream out;
    out << "32X VDP: " << kModeNames[static_cast<int>(mode())] << ", "
        << (screen_priority() ? "in front of" : "behind") << " the Mega Drive, " << height()
        << " lines";
    return out.str();
}

}  // namespace mercury::thirtytwox
