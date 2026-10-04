#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "mercury/genesis/miracle_piano.hpp"

namespace mercury::genesis {

enum class Button : std::uint8_t {
    up, down, left, right, a, b, c, start,
    // Six-button pad only.
    x, y, z, mode,
    count
};

// One controller port. The Mega Drive pad is not a latch you read - it is a
// multiplexer driven by the TH line, and the six-button pad counts TH edges to
// decide which of four banks to present. Software drives TH by writing the data
// port, so the port has to remember the last value written and how many times
// it has flipped.
class ControllerPort {
public:
    // The two light guns a Mega Drive game can actually read. The Master
    // System's Light Phaser used to sit here too and has been dropped: no Mega
    // Drive cartridge reads it, so offering it was offering a device that could
    // never do anything. The Konami Justifier took its place - that is what
    // Lethal Enforcers, Lethal Enforcers II and Snatcher use, and those are the
    // gun games that draw no crosshair of their own.
    enum class Kind : std::uint8_t { none, pad3, pad6, menacer, justifier, miracle_piano };

    // A light gun is aimed at the screen, so anything that would hide or grab
    // the mouse pointer has to leave it alone.
    [[nodiscard]] static constexpr bool is_light_gun(Kind kind) noexcept {
        return kind == Kind::menacer || kind == Kind::justifier;
    }

    void reset() noexcept;
    void set_kind(Kind kind) noexcept { kind_ = kind; }
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    void set_button(Button button, bool pressed) noexcept;
    [[nodiscard]] bool is_pressed(Button button) const noexcept;

    // Where a light gun is pointed, in the VDP's own pixel coordinates, and
    // whether it is pointed at the screen at all. Off screen the gun never
    // sees the beam, which is how a game tells that the player has aimed away
    // - several of them use that deliberately as the reload gesture.
    void set_aim(std::uint32_t x, std::uint32_t y, bool on_screen) noexcept {
        aim_x_ = x;
        aim_y_ = y;
        aim_on_screen_ = on_screen;
    }
    [[nodiscard]] std::uint32_t aim_x() const noexcept { return aim_x_; }
    [[nodiscard]] std::uint32_t aim_y() const noexcept { return aim_y_; }
    [[nodiscard]] bool aim_on_screen() const noexcept { return aim_on_screen_; }

    // The Justifier's second gun. It is not a second PORT - it daisy-chains off
    // the first gun's cable and answers on the same port, with the console
    // choosing between the two by driving TR. So one port holds two aims, and
    // only the selected one is looking at the screen at any moment.
    void set_gun2_aim(std::uint32_t x, std::uint32_t y, bool on_screen) noexcept {
        gun2_x_ = x;
        gun2_y_ = y;
        gun2_on_screen_ = on_screen;
    }
    void set_gun2_trigger(bool pressed) noexcept { gun2_trigger_ = pressed; }
    void set_gun2_start(bool pressed) noexcept { gun2_start_ = pressed; }
    [[nodiscard]] std::uint32_t gun2_aim_x() const noexcept { return gun2_x_; }
    [[nodiscard]] std::uint32_t gun2_aim_y() const noexcept { return gun2_y_; }
    [[nodiscard]] bool gun2_on_screen() const noexcept { return gun2_on_screen_; }

    // Whether a second gun is plugged into the first. With only one, the port
    // must not multiplex at all - see selected_gun.
    void set_gun2_present(bool present) noexcept { gun2_present_ = present; }
    [[nodiscard]] bool gun2_present() const noexcept { return gun2_present_; }

    // Which of the two the console is talking to, from the TR line it drives.
    // Always gun one for anything that is not a Justifier.
    //
    // AND always gun one when no second gun is plugged in, which is not a
    // shortcut but the fix for a real regression. The game drives TR constantly
    // to alternate between the two guns - measured on Lethal Enforcers, 8,794
    // reads with it low against 10,500 with it high. The light pulse, though,
    // happens at ONE scanline per frame, and whether TR is low at that instant
    // is chance. Letting selection gate the pulse therefore threw away the
    // position on roughly half of all frames for a player with one gun, and
    // that is what stopped the shots landing.
    [[nodiscard]] int selected_gun() const noexcept {
        if (kind_ != Kind::justifier || !gun2_present_) {
            return 0;
        }
        return ((data_ & control_) & 0x20) != 0 ? 1 : 0;
    }
    [[nodiscard]] std::uint32_t selected_aim_x() const noexcept {
        return selected_gun() == 1 ? gun2_x_ : aim_x_;
    }
    [[nodiscard]] std::uint32_t selected_aim_y() const noexcept {
        return selected_gun() == 1 ? gun2_y_ : aim_y_;
    }
    [[nodiscard]] bool selected_on_screen() const noexcept {
        return selected_gun() == 1 ? gun2_on_screen_ : aim_on_screen_;
    }

    // The H counter value the VDP should latch for this aim point.
    //
    // The counter advances once per two pixels, so the position within the line
    // is x/2 - but that is an INDEX into the line, not the counter value. The
    // counter runs 0x00..0xB6 and then jumps to 0xE5..0xFF, 210 steps in all,
    // and the visible screen does not begin at step zero. h_offset_ is where it
    // does begin, and the index wraps around the line the way the real counter
    // does. Getting this wrong does not tilt the aim, it OFFSETS it: the game's
    // own crosshair sits half a screen to the right of the pointer and wraps
    // round the edge, which is exactly what T2 showed.
    //
    // The Menacer alone is scaled first. Its games multiply the raw reading up
    // by a fixed factor of their own, so the emulator has to hand them a value
    // that is correspondingly short or their crosshair runs away from the
    // pointer as it crosses the screen. 289/320 is the reference's constant.
    [[nodiscard]] std::uint32_t light_gun_h(bool wide) const noexcept {
        const std::uint32_t steps = wide ? 210u : 171u;
        const std::uint32_t aim = selected_aim_x();
        const std::uint32_t x = kind_ == Kind::menacer ? (aim * 289u) / 320u : aim;
        const std::uint32_t origin = wide ? h_offset_wide_ : h_offset_narrow_;
        const std::uint32_t index = (x / 2u + origin) % steps;
        // The counter's own numbering: contiguous, then the blanking jump.
        const std::uint32_t last = wide ? 0xB6u : 0x93u;
        const std::uint32_t after = wide ? 0xE5u : 0xE9u;
        return index <= last ? index : (after + (index - last - 1u));
    }
    // Where the visible screen starts along the scanline, in H counter steps.
    // BOTH are measured against real games, and neither is derived from the
    // other - that was tried and is wrong.
    //
    // 320-wide: 136, from T2: The Arcade Game. Found by scanning all 210
    // possible origins and scoring each against where the game drew its own
    // crosshair versus where the gun was pointed. A clean minimum, 1.6 pixels
    // average error across the screen, rising either side.
    //
    // 256-wide: 0, meaning no offset at all - the plain x/2 that Genesis Plus
    // GX uses. Getting here took three steps. Scaling the wide value by line
    // length gave 110 and put the shot half a screen away. Firing at Lethal
    // Enforcers and finding which aim hits a target narrowed it to about 168,
    // but that oracle is only good to ten pixels or so, because the impact mark
    // is centred on the ENEMY SPRITE rather than on the bullet - two different
    // targets disagreed by fourteen pixels. The player then reported the shot
    // landing slightly left of the sight, which needs a few steps MORE, and
    // 168 plus three steps wraps to exactly 0. Two independent routes to the
    // same place, one of them the reference implementation.
    //
    // The first attempt scaled one from the other by line length, on the theory
    // that the origin is a position in time and a narrow line has fewer steps
    // in the same time. That predicted 110 and put the shot most of a screen
    // away from the sight. Whatever the origin really describes, it is not that.
    //
    // The two axes cannot be separated with the games available: every Menacer
    // title in the library runs 320-wide and every Justifier title 256-wide, so
    // "per video mode" and "per gun" fit the measurements equally well. Stored
    // per mode because that is where the step COUNT differs.
    static constexpr std::uint32_t kDefaultHOffsetWide = 136;
    static constexpr std::uint32_t kDefaultHOffsetNarrow = 0;
    // Sets both, so a calibration tool can pass a value in whichever mode the
    // game it is measuring happens to run in.
    void set_light_gun_h_offset(std::uint32_t offset) noexcept {
        h_offset_wide_ = offset % 210u;
        h_offset_narrow_ = offset % 171u;
    }

    void write_data(std::uint8_t value) noexcept;
    void write_control(std::uint8_t value) noexcept;

    // The keyboard on this port, if one is plugged in. Held here rather than
    // as a separate device because it IS the controller: the game reaches it
    // through the same data and control registers a pad uses.
    [[nodiscard]] MiraclePiano& piano() noexcept { return piano_; }
    [[nodiscard]] const MiraclePiano& piano() const noexcept { return piano_; }
    [[nodiscard]] std::uint64_t control_writes() const noexcept { return control_writes_; }
    [[nodiscard]] std::uint64_t reads() const noexcept { return reads_; }
    [[nodiscard]] const std::array<std::uint64_t, 256>& read_log(int bank) const noexcept {
        return read_log_[bank];
    }
    [[nodiscard]] std::uint8_t read_data() const noexcept;
    [[nodiscard]] std::uint8_t control() const noexcept { return control_; }

    // Called once per frame: the six-button counter times out after roughly
    // 1.5ms of no TH activity, which is what stops a game that only reads once
    // per frame from walking off the end of the sequence.
    void tick_timeout() noexcept;

    [[nodiscard]] std::string describe() const;

private:
    [[nodiscard]] std::uint8_t bank_value(std::uint32_t phase, bool th_high) const noexcept;
    [[nodiscard]] std::uint8_t menacer_value() const noexcept;
    [[nodiscard]] std::uint8_t justifier_value() const noexcept;

  public:
    // The TH line's latched state, not the buttons: which half of the pad the
    // game is part-way through reading matters if a state is saved mid-read.
    // The button bits themselves come from whoever is holding the pad.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(control_);
        ar(th_high_);
        ar(th_edges_);
    }

  private:
    Kind kind_ = Kind::pad3;
    std::uint32_t aim_x_ = 0;
    std::uint32_t aim_y_ = 0;
    std::uint32_t h_offset_wide_ = kDefaultHOffsetWide;
    std::uint32_t h_offset_narrow_ = kDefaultHOffsetNarrow;
    std::uint32_t gun2_x_ = 0;
    std::uint32_t gun2_y_ = 0;
    bool gun2_present_ = false;
    bool gun2_on_screen_ = false;
    bool gun2_trigger_ = false;
    bool gun2_start_ = false;
    bool aim_on_screen_ = false;
    std::array<bool, static_cast<std::size_t>(Button::count)> pressed_{};
    std::uint8_t data_ = 0x00;
    std::uint8_t control_ = 0x00;
    MiraclePiano piano_{};
    void update_th() noexcept;
    std::uint64_t control_writes_ = 0;
    mutable std::uint64_t reads_ = 0;
    mutable std::array<std::array<std::uint64_t, 256>, 2> read_log_{};
    bool th_high_ = true;
    std::uint32_t th_edges_ = 0;
    std::uint32_t idle_frames_ = 0;
};

}  // namespace mercury::genesis


