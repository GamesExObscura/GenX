#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "mercury/thirtytwox/bus.hpp"

namespace mercury::thirtytwox {

// The 32X's video layer.
//
// It does NOT replace the Mega Drive's picture, it is combined with it, and
// that is the whole difficulty. Two things decide which side a pixel lands
// on:
//
//   * the PRI bit in the bitmap mode register, which sets the default for the
//     whole screen, and
//   * a per-pixel "through bit" - bit 15 of the palette entry, or of the
//     pixel itself in direct colour - which puts THAT pixel on the opposite
//     side of the Mega Drive layers from wherever PRI put everything else.
//
// So priority is not a single answer per frame. It is per pixel, and the
// through bit is a flip rather than a value.
//
// The other thing worth knowing before reading this: there is no tilemap and
// no scrolling register. Every line's pixels are found through a LINE TABLE
// of 256 word-offsets at the start of the frame buffer, and scrolling is done
// by rewriting those offsets. A line can point anywhere, two lines can point
// at the same data, and that is how 32X games do raster effects.
class Vdp {
  public:
    static constexpr int kWidth = 320;
    static constexpr int kMaxHeight = 240;

    // Bitmap mode register. Bit 7 is priority and bit 6 the 240-line mode -
    // NOT the other way round, which is how one summary of the manual reads
    // and what two other sources corrected.
    static constexpr std::uint16_t kPriority = 0x0080;
    static constexpr std::uint16_t kTallScreen = 0x0040;
    static constexpr std::uint16_t kModeMask = 0x0003;
    // Bit 15 says the machine is NOT PAL, so it is SET on an NTSC console.
    // Read-only: it reports the hardware, and a game writing the mode
    // register must not be able to change what console it is plugged into.
    //
    // A 32X game cross-checks this against the Mega Drive's own version
    // register and refuses to run if the two disagree. Leaving it clear says
    // "PAL adapter", which against an NTSC console is a mismatch: Golf
    // Magazine masked interrupts and branched to a dead loop four instructions
    // later, and six other cartridges did the same.
    static constexpr std::uint16_t kNotPal = 0x8000;

    enum class Mode : std::uint8_t {
        blank = 0,
        packed_pixel = 1,
        direct_colour = 2,
        run_length = 3,
    };

    // One pixel of 32X output, ready to be combined with the Mega Drive's.
    struct Pixel {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        // Nothing here: the Mega Drive shows through regardless of priority.
        bool opaque = false;
        // Where this pixel sits relative to the Mega Drive's layers, after
        // the screen-wide PRI bit and this pixel's through bit have both been
        // taken into account.
        bool in_front = false;
    };

    // Frame buffer control register, $400A. Only FS is writable; the rest is
    // the video layer reporting on itself.
    static constexpr std::uint16_t kVBlank = 0x8000;
    static constexpr std::uint16_t kHBlank = 0x4000;
    static constexpr std::uint16_t kPaletteEnable = 0x2000;
    static constexpr std::uint16_t kFillNotEnded = 0x0002;
    static constexpr std::uint16_t kFrameSelect = 0x0001;

    void reset() noexcept;
    void attach_bus(Bus* bus) noexcept { bus_ = bus; }
    // Follows the Mega Drive it is plugged into. See kNotPal.
    void set_ntsc(bool ntsc) noexcept { ntsc_ = ntsc; }
    [[nodiscard]] bool ntsc() const noexcept { return ntsc_; }

    // The two frame buffer banks swap at the start of vertical blank, not
    // when the game asks. A write to FS during active display is remembered
    // and applied here; a write while already blanked, or with the display
    // off, takes effect at once. Getting this wrong shows up as a game
    // drawing perfectly into a bank that is never shown - which is exactly
    // what the 240p Test Suite did, 53KB of picture in the bank nobody saw.
    void start_vblank() noexcept;
    void end_vblank() noexcept;

    // IS THE FRAME BUFFER REACHABLE RIGHT NOW?
    //
    // Two things take it away from software. A bank swap that has been asked
    // for but not yet happened, because the pair are mid-exchange; and an
    // auto-fill, which owns the memory until it finishes. Software polls the
    // frame buffer control register to find out, so both have to be visible
    // there rather than merely obeyed.
    [[nodiscard]] bool framebuffer_engaged() const noexcept {
        return fill_wait_ > 0 || refreshing() ||
               (produces_picture() &&
                pending_frame_select_ != ((frame_buffer_control_ & kFrameSelect) != 0));
    }

    // THE FRAME BUFFER IS DRAM, AND DRAM GETS REFRESHED. During a refresh the
    // memory is not software's, and FEN says so - which means FEN is not only
    // "a swap or a fill is in progress", it also ticks busy at a steady rate
    // all the time. ares reports `MegaDrive::vdp.refreshing() ||
    // framebufferEngaged()` for this bit, with refreshing coming from the
    // video chip's own slot table.
    //
    // Metal Head needs it: its master polls $410B and waits for FEN to be
    // SET - the opposite of the usual "wait until it is free" - and with FEN
    // only ever set by a swap or a fill, the loop never ended. It sat there
    // with a full frame of picture in the buffer and a palette faded to black.
    //
    // Modelled as a window rather than a slot table: a short busy stretch
    // every so often, which is what a poll loop can see and all it can see.
    // The numbers are one refresh per quarter-line at about a sixteenth duty,
    // in SH-2 cycles (a scanline is close to 1,464 of them).
    static constexpr std::uint32_t kRefreshPeriod = 366;
    static constexpr std::uint32_t kRefreshWindow = 24;
    [[nodiscard]] bool refreshing() const noexcept {
        return refresh_phase_ < kRefreshWindow;
    }
    // IS THE PALETTE REACHABLE RIGHT NOW?
    //
    // PEN is live, like the busy flag, and it is NOT a "we are in vertical
    // blank" marker - which is how it was reported here, set at the start of
    // the blank and cleared at the end. The palette is software's whenever the
    // picture is not being drawn out of it: during vertical blank, during
    // horizontal blank, and in any mode that never reads it at all. The two
    // such modes are blank and direct colour, which are exactly the two whose
    // low mode bit is clear, so the mode test is that one bit. ares reports
    // `!paletteEngaged()` with `paletteEngaged = !vblank && !hblank &&
    // mode.bit(0)`, which is the same rule.
    //
    // Blackthorne reads this register while its 32X side is still in blank
    // mode: hardware and ares answer "yours", and answering "not yours" put
    // GenX on a different path through the game's own code.
    //
    // Horizontal blank is not modelled - the 32X side is stepped once per line
    // - so in the two modes that do use the palette this reports one blank
    // short during the picture. Nothing in the library polls it there.
    [[nodiscard]] bool palette_accessible() const noexcept {
        return (frame_buffer_control_ & kVBlank) != 0 || (bitmap_mode_ & 0x0001u) == 0;
    }
    // An auto-fill is not instant. The cost is a fixed overhead plus three
    // cycles a word, which is what the hardware documentation gives, and it
    // is long enough for software to see the busy flag it polls for.
    void tick_fill(std::uint32_t cycles) noexcept {
        fill_wait_ = fill_wait_ > cycles ? fill_wait_ - cycles : 0;
        // A subtraction rather than a modulo: this runs after every master
        // instruction and the step is almost never a whole period.
        refresh_phase_ += cycles;
        while (refresh_phase_ >= kRefreshPeriod) {
            refresh_phase_ -= kRefreshPeriod;
        }
    }

    // Register access, from either side - the adapter's FM bit decides who is
    // allowed, and that is enforced by the caller rather than here.
    [[nodiscard]] std::uint16_t read_register(std::uint32_t offset) const noexcept;
    void write_register(std::uint32_t offset, std::uint16_t value) noexcept;

    [[nodiscard]] Mode mode() const noexcept {
        return static_cast<Mode>(bitmap_mode_ & kModeMask);
    }
    // Nothing at all is drawn in blank mode, which is also the state a
    // cartridge that never touches the 32X leaves it in.
    [[nodiscard]] bool produces_picture() const noexcept { return mode() != Mode::blank; }
    [[nodiscard]] bool screen_priority() const noexcept {
        return (bitmap_mode_ & kPriority) != 0;
    }
    [[nodiscard]] int height() const noexcept {
        return (bitmap_mode_ & kTallScreen) != 0 ? 240 : 224;
    }

    // Build one scanline. `out` must have room for kWidth pixels.
    void render_line(int line, Pixel* out) const noexcept;

    // Combine one 32X line with one Mega Drive line, in place.
    //
    // `rgb` is the Mega Drive's picture for this line, three bytes a pixel,
    // and `backdrop` is one byte a pixel saying where it drew nothing. Both
    // come straight from the Genesis VDP. The rule is short:
    //
    //   - a transparent 32X pixel never replaces anything;
    //   - an opaque 32X pixel marked in_front always replaces;
    //   - an opaque 32X pixel behind replaces ONLY where the Mega Drive left
    //     backdrop.
    //
    // Static, and taking plain pointers, so it can be tested without either
    // machine and so the Genesis renderer needs to know nothing about the
    // 32X.
    static void composite_line(const Pixel* layer, std::uint8_t* rgb,
                               const std::uint8_t* backdrop, int width) noexcept;

    [[nodiscard]] std::string describe() const;

    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(bitmap_mode_);
        ar(screen_shift_);
        ar(fill_start_);
        ar(fill_length_);
        ar(frame_buffer_control_);
        ar(pending_frame_select_);
        ar(ntsc_);
        ar(fill_wait_);
        ar(refresh_phase_);
    }

    // Offsets within the VDP register page, from the base the bus decodes.
    static constexpr std::uint32_t kBitmapMode = 0x00;
    static constexpr std::uint32_t kScreenShift = 0x02;
    // LENGTH FIRST, THEN START. These were the other way round, and ares and
    // the games agree on this order: Knuckles' Chaotix writes $FF to $4104
    // and row addresses $0100, $0200... to $4106. Swapped, every auto-fill
    // took the row address as its length and the length as where to start.
    static constexpr std::uint32_t kFillLength = 0x04;
    static constexpr std::uint32_t kFillStart = 0x06;
    static constexpr std::uint32_t kFillData = 0x08;
    static constexpr std::uint32_t kFrameBufferControl = 0x0A;

  private:
    // A palette entry, or a direct-colour pixel, unpacked. Bit 15 is the
    // through bit; the rest is 5 bits each of blue, green and red in that
    // order, the same way round as the Mega Drive's own colour RAM.
    static Pixel unpack_colour(std::uint16_t colour, bool screen_priority) noexcept;

    // Apply a frame select, moving the displayed bank with it.
    void select_frame(bool second) noexcept;

    Bus* bus_ = nullptr;
    std::uint16_t bitmap_mode_ = 0;
    std::uint16_t screen_shift_ = 0;
    std::uint16_t fill_start_ = 0;
    std::uint16_t fill_length_ = 0;
    std::uint16_t frame_buffer_control_ = 0;
    // What the game last asked for, which is not always what is showing.
    bool pending_frame_select_ = false;
    bool ntsc_ = true;
    std::uint32_t fill_wait_ = 0;
    // Where in the refresh cycle the frame buffer is. Free-running: nothing
    // starts or stops it, the memory is always being refreshed.
    std::uint32_t refresh_phase_ = 0;
};

}  // namespace mercury::thirtytwox
