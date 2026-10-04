// The 32X video layer.
//
// The tests that matter are the priority ones. Everything else here is an
// ordinary "did the right pixel come out" check, but the through bit is a
// FLIP rather than a value, and getting that backwards puts a handful of
// pixels on the wrong side of the Mega Drive picture - which looks like a
// glitch in a game rather than like a bug in a renderer.

#include "mercury/thirtytwox/bus.hpp"
#include "mercury/thirtytwox/vdp.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(std::uint32_t got, std::uint32_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s: got %u want %u\n", what.c_str(), got, want);
    }
}

// A bus with a line table already laid out the ordinary way: line 0's data
// starts right after the 256-word table, each line 160 words further on.
struct Screen {
    mercury::thirtytwox::Bus bus;
    mercury::thirtytwox::Vdp vdp;

    Screen() {
        bus.reset();
        vdp.reset();
        vdp.attach_bus(&bus);
        // Bank 0 displayed, so every write lands in bank 1 - the SH-2s always
        // draw into whichever bank is NOT on screen. The tests then flip to
        // show what they drew, which is what a game does every frame.
        bus.set_displayed_bank(0);
        for (int line = 0; line < 240; ++line) {
            const std::uint32_t offset = 0x100 + static_cast<std::uint32_t>(line) * 160;
            write_word(static_cast<std::uint32_t>(line) * 2, static_cast<std::uint16_t>(offset));
        }
    }

    // Show the drawn bank, render, then go back to drawing. Wrapping it here
    // rather than in each test is what stops the whole suite quietly
    // measuring an empty buffer, which is exactly what it did first time.
    void render(int line, mercury::thirtytwox::Vdp::Pixel* out) {
        bus.set_displayed_bank(1);
        vdp.render_line(line, out);
        bus.set_displayed_bank(0);
    }

    // Straight into the drawn bank, which with bank 1 displayed is bank 0.
    void write_byte(std::uint32_t byte_offset, std::uint8_t value) {
        bus.write8(0x24000000 + byte_offset, value);
    }
    void write_word(std::uint32_t byte_offset, std::uint16_t value) {
        bus.write16(0x24000000 + byte_offset, value);
    }
    void set_palette(int index, std::uint16_t colour) {
        bus.write16(0x20004200 + static_cast<std::uint32_t>(index) * 2, colour);
    }
    void set_mode(std::uint16_t value) {
        vdp.write_register(mercury::thirtytwox::Vdp::kBitmapMode, value);
    }
    // Line `line`'s pixel data starts here, in bytes.
    static std::uint32_t line_start(int line) {
        return (0x100 + static_cast<std::uint32_t>(line) * 160) * 2;
    }
};

void test_blank_mode_draws_nothing() {
    std::printf("blank mode is genuinely nothing, not black\n");
    Screen screen;
    screen.set_mode(0);
    check(!screen.vdp.produces_picture(), "blank mode produces no picture at all");
    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check(!line[0].opaque, "and every pixel is transparent, so the Mega Drive shows");
    check(!line[319].opaque, "including the last one");
}

void test_packed_pixel_indexes_the_palette() {
    std::printf("packed pixel mode: one byte per pixel, index into the palette\n");
    Screen screen;
    screen.set_mode(1);
    screen.set_palette(1, 0x001F);  // pure red: five low bits
    screen.set_palette(2, 0x03E0);  // pure green
    screen.set_palette(3, 0x7C00);  // pure blue
    screen.write_byte(Screen::line_start(0) + 0, 1);
    screen.write_byte(Screen::line_start(0) + 1, 2);
    screen.write_byte(Screen::line_start(0) + 2, 3);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check_eq(line[0].r, 255, "index 1 is red");
    check_eq(line[0].g, 0, "and only red");
    check_eq(line[1].g, 255, "index 2 is green");
    check_eq(line[2].b, 255, "index 3 is blue");
    check(line[0].opaque && line[1].opaque && line[2].opaque, "all three are opaque");
}

// INDEX 0 IS A COLOUR LIKE ANY OTHER. This test used to assert the opposite,
// and that rule is what broke Star Wars Arcade's SEGA splash: the game puts
// black WITH THE THROUGH BIT in entry 0, so the whole empty background of the
// 32X layer comes in front of the Mega Drive and hides its tiled text outside
// the big letters. ares plots index 0 through the palette like every other
// index; only priority, the through bit and the Mega Drive's backdrop decide
// whether it shows.
void test_index_zero_is_an_ordinary_colour() {
    std::printf("index 0 is drawn from palette entry 0 like any other index\n");
    Screen screen;
    screen.set_mode(1);
    screen.set_palette(0, 0x7FFF);
    screen.write_byte(Screen::line_start(0) + 0, 0);
    screen.write_byte(Screen::line_start(0) + 1, 0);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check(line[0].opaque, "index 0 is drawn, not skipped");
    check_eq(line[0].r, 255, "in palette entry 0's colour");
    check(line[1].opaque, "and so is the next one");

    // The case that matters: entry 0 is black with the through bit, the
    // screen-wide priority says "behind the Mega Drive", and the through bit
    // flips every index-0 pixel IN FRONT.
    screen.set_palette(0, 0x8000);
    screen.render(0, line);
    check(line[0].opaque && line[0].in_front,
          "black with the through bit comes in front, covering the Mega Drive");
    check_eq(line[0].r, 0, "and it is black");
}

void test_the_line_table_decides_where_a_line_comes_from() {
    std::printf("the line table, not a scroll register, decides what a line shows\n");
    Screen screen;
    screen.set_mode(1);
    screen.set_palette(9, 0x001F);
    // Put a pixel in line 5's data, then point line 0 at line 5's data too.
    screen.write_byte(Screen::line_start(5) + 0, 9);
    screen.write_word(0, static_cast<std::uint16_t>(0x100 + 5 * 160));

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check(line[0].opaque, "line 0 now shows line 5's data");
    check_eq(line[0].r, 255, "and it is the right pixel");

    screen.render(5, line);
    check(line[0].opaque, "line 5 still shows it too - two lines, one source");
}

void test_direct_colour_carries_the_colour_itself() {
    std::printf("direct colour mode: the pixel IS the colour, no palette\n");
    Screen screen;
    screen.set_mode(2);
    screen.write_word(Screen::line_start(0) + 0, 0x001F);  // red
    screen.write_word(Screen::line_start(0) + 2, 0x0000);  // nothing
    screen.write_word(Screen::line_start(0) + 4, 0x7C00);  // blue

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check_eq(line[0].r, 255, "the first pixel is red");
    // Black without the through bit: drawn, and it sits on whichever side the
    // screen priority puts it. It shows only where the Mega Drive left
    // backdrop when the layer is behind - which is composite_line's call, not
    // the renderer's.
    check(line[1].opaque, "a word of zero is black, not nothing");
    check_eq(line[1].r, 0, "and it is black");
    check_eq(line[2].b, 255, "the third is blue");
}

void test_screen_priority_puts_the_layer_in_front_or_behind() {
    std::printf("PRI decides which side the whole 32X picture sits on\n");
    Screen screen;
    screen.set_palette(1, 0x001F);
    screen.write_byte(Screen::line_start(0), 1);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.set_mode(1);  // priority clear
    screen.render(0, line);
    check(!line[0].in_front, "with PRI clear the Mega Drive is in front");

    screen.set_mode(1 | mercury::thirtytwox::Vdp::kPriority);
    screen.render(0, line);
    check(line[0].in_front, "with PRI set the 32X is in front");
}

void test_the_through_bit_flips_that_pixel_only() {
    std::printf("THE THROUGH BIT: a flip, not a value\n");
    Screen screen;
    // Two pixels, identical except for the through bit in their palette
    // entries. Whatever PRI says, these two must disagree with each other.
    screen.set_palette(1, 0x001F);           // through bit clear
    screen.set_palette(2, 0x001F | 0x8000);  // through bit set
    screen.write_byte(Screen::line_start(0) + 0, 1);
    screen.write_byte(Screen::line_start(0) + 1, 2);

    mercury::thirtytwox::Vdp::Pixel line[320];

    screen.set_mode(1);  // PRI clear: the layer is behind
    screen.render(0, line);
    check(!line[0].in_front, "the ordinary pixel is behind");
    check(line[0].opaque && line[1].opaque, "both are still drawn");
    check(line[1].in_front, "the through-bit pixel is flipped IN FRONT");

    screen.set_mode(1 | mercury::thirtytwox::Vdp::kPriority);  // PRI set
    screen.render(0, line);
    check(line[0].in_front, "now the ordinary pixel is in front");
    check(!line[1].in_front, "and the through-bit pixel is flipped BEHIND");
}

void test_the_through_bit_works_in_direct_colour_too() {
    std::printf("the through bit is bit 15 of a direct-colour pixel as well\n");
    Screen screen;
    screen.set_mode(2);
    screen.write_word(Screen::line_start(0) + 0, 0x001F);
    screen.write_word(Screen::line_start(0) + 2, 0x001F | 0x8000);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check(!line[0].in_front, "the ordinary pixel follows PRI");
    check(line[1].in_front, "the through-bit pixel is flipped");
    check_eq(line[1].r, 255, "and keeps its colour - bit 15 is not part of the red");
}

void test_run_length_fills_a_run() {
    std::printf("run length mode expands each word into a run of pixels\n");
    Screen screen;
    screen.set_mode(3);
    screen.set_palette(4, 0x001F);
    screen.set_palette(5, 0x7C00);
    // Three of index 4, then two of index 5.
    screen.write_word(Screen::line_start(0) + 0, 0x0204);
    screen.write_word(Screen::line_start(0) + 2, 0x0105);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.render(0, line);
    check_eq(line[0].r, 255, "the run starts");
    check_eq(line[2].r, 255, "and covers three pixels");
    check_eq(line[3].b, 255, "then the next run begins");
    check_eq(line[4].b, 255, "for two pixels");
}

void test_screen_shift_moves_the_line_one_pixel() {
    std::printf("the screen shift bit moves a packed-pixel line by one\n");
    Screen screen;
    screen.set_palette(1, 0x001F);
    screen.set_palette(2, 0x7C00);
    screen.write_byte(Screen::line_start(0) + 0, 1);
    screen.write_byte(Screen::line_start(0) + 1, 2);

    mercury::thirtytwox::Vdp::Pixel line[320];
    screen.set_mode(1);
    screen.render(0, line);
    check_eq(line[0].r, 255, "unshifted, the first pixel is the first byte");

    screen.vdp.write_register(mercury::thirtytwox::Vdp::kScreenShift, 1);
    screen.render(0, line);
    check_eq(line[0].b, 255, "shifted, the line starts one pixel later");
}

void test_height_follows_the_tall_screen_bit() {
    std::printf("224 lines or 240, from the mode register\n");
    Screen screen;
    screen.set_mode(1);
    check_eq(static_cast<std::uint32_t>(screen.vdp.height()), 224, "224 by default");
    screen.set_mode(1 | mercury::thirtytwox::Vdp::kTallScreen);
    check_eq(static_cast<std::uint32_t>(screen.vdp.height()), 240, "240 when asked");
}

// --- compositing -----------------------------------------------------------
//
// The rule being tested: a transparent 32X pixel never replaces anything, an
// in-front pixel always does, and a behind pixel replaces only where the Mega
// Drive left backdrop.

void test_compositing_leaves_transparent_pixels_alone() {
    std::printf("compositing: a transparent 32X pixel changes nothing\n");
    mercury::thirtytwox::Vdp::Pixel layer[2]{};
    std::uint8_t rgb[6] = {10, 20, 30, 40, 50, 60};
    std::uint8_t backdrop[2] = {1, 1};  // the Mega Drive drew nothing at all
    mercury::thirtytwox::Vdp::composite_line(layer, rgb, backdrop, 2);
    check_eq(rgb[0], 10, "the Mega Drive pixel survives");
    check_eq(rgb[3], 40, "and so does the next one");
}

void test_compositing_in_front_always_wins() {
    std::printf("compositing: an in-front 32X pixel covers the Mega Drive\n");
    mercury::thirtytwox::Vdp::Pixel layer[1]{};
    layer[0] = {1, 2, 3, true, true};
    std::uint8_t rgb[3] = {10, 20, 30};
    std::uint8_t backdrop[1] = {0};  // the Mega Drive DID draw here
    mercury::thirtytwox::Vdp::composite_line(layer, rgb, backdrop, 1);
    check_eq(rgb[0], 1, "in front beats a drawn Mega Drive pixel");
    check_eq(rgb[2], 3, "all three channels");
}

void test_compositing_behind_only_shows_through_backdrop() {
    std::printf("compositing: a behind pixel shows only where the Mega Drive is empty\n");
    mercury::thirtytwox::Vdp::Pixel layer[2]{};
    layer[0] = {9, 9, 9, true, false};  // behind, over a DRAWN pixel
    layer[1] = {7, 7, 7, true, false};  // behind, over BACKDROP
    std::uint8_t rgb[6] = {10, 10, 10, 20, 20, 20};
    std::uint8_t backdrop[2] = {0, 1};
    mercury::thirtytwox::Vdp::composite_line(layer, rgb, backdrop, 2);
    check_eq(rgb[0], 10, "hidden behind a drawn Mega Drive pixel");
    check_eq(rgb[3], 7, "visible through the backdrop");
}

void test_compositing_without_backdrop_information_hides_the_layer() {
    std::printf("compositing: with no backdrop information, a behind layer stays hidden\n");
    mercury::thirtytwox::Vdp::Pixel layer[1]{};
    layer[0] = {9, 9, 9, true, false};
    std::uint8_t rgb[3] = {10, 20, 30};
    mercury::thirtytwox::Vdp::composite_line(layer, rgb, nullptr, 1);
    check_eq(rgb[0], 10, "the safe reading is that the Mega Drive drew everywhere");
}

void test_a_whole_line_composites_the_way_a_game_would_see_it() {
    std::printf("compositing: a realistic line, 32X behind a Mega Drive sprite\n");
    Screen screen;
    screen.set_mode(1);                   // packed pixel, PRI clear, so behind
    screen.set_palette(1, 0x001F);        // red
    screen.set_palette(2, 0x001F | 0x8000);  // red, through bit set
    for (int x = 0; x < 4; ++x) {
        screen.write_byte(Screen::line_start(0) + static_cast<std::uint32_t>(x), 1);
    }
    // One pixel with the through bit, which must punch through regardless.
    screen.write_byte(Screen::line_start(0) + 2, 2);

    mercury::thirtytwox::Vdp::Pixel layer[320];
    screen.render(0, layer);

    // A Mega Drive line that drew a solid green sprite over the first three
    // pixels and nothing after that.
    std::uint8_t rgb[320 * 3];
    std::uint8_t backdrop[320];
    for (int x = 0; x < 320; ++x) {
        const bool drawn = x < 3;
        rgb[x * 3 + 0] = drawn ? 0 : 99;
        rgb[x * 3 + 1] = drawn ? 255 : 99;
        rgb[x * 3 + 2] = drawn ? 0 : 99;
        backdrop[x] = drawn ? 0 : 1;
    }
    mercury::thirtytwox::Vdp::composite_line(layer, rgb, backdrop, 320);

    check_eq(rgb[0 * 3 + 1], 255, "the Mega Drive sprite still covers pixel 0");
    check_eq(rgb[1 * 3 + 1], 255, "and pixel 1");
    check_eq(rgb[2 * 3 + 0], 255, "but the through-bit pixel punches through it");
    check_eq(rgb[2 * 3 + 1], 0, "and it is the 32X's red, not the sprite's green");
    check_eq(rgb[3 * 3 + 0], 255, "past the sprite the 32X shows through the backdrop");
    // Pixel 4 is index 0. It is NOT "nothing": over the Mega Drive's backdrop
    // the 32X always draws, and index 0 draws palette entry 0 - black here.
    // ares and PicoDrive agree on this; neither has an index-0 exception.
    check_eq(rgb[4 * 3 + 0], 0, "over the backdrop, index 0 draws palette entry 0");
    check_eq(rgb[4 * 3 + 1], 0, "which is black, replacing the backdrop colour");
}

}  // namespace

int main() {
    std::printf("32X video tests\n===============\n\n");
    test_blank_mode_draws_nothing();
    test_packed_pixel_indexes_the_palette();
    test_index_zero_is_an_ordinary_colour();
    test_the_line_table_decides_where_a_line_comes_from();
    test_direct_colour_carries_the_colour_itself();
    test_screen_priority_puts_the_layer_in_front_or_behind();
    test_the_through_bit_flips_that_pixel_only();
    test_the_through_bit_works_in_direct_colour_too();
    test_run_length_fills_a_run();
    test_screen_shift_moves_the_line_one_pixel();
    test_height_follows_the_tall_screen_bit();
    test_compositing_leaves_transparent_pixels_alone();
    test_compositing_in_front_always_wins();
    test_compositing_behind_only_shows_through_backdrop();
    test_compositing_without_backdrop_information_hides_the_layer();
    test_a_whole_line_composites_the_way_a_game_would_see_it();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
