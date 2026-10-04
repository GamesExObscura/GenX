#pragma once

// The light-gun crosshair, as pixels.
//
// Kept here rather than inside the Win32 shell so the preview tool and the
// emulator draw the identical thing - a cursor that is judged from one drawing
// and shipped from another is a cursor nobody has actually approved.
//
// The design problem is that this has to stay visible over any frame a game can
// put behind it, and the games it is used on are not consistent: bright desert,
// dark corridor, grey concrete. A single colour cannot do that. The answer is
// the same one road signs and gun sights use - a light core with a dark outline
// around every stroke. Whichever of the two the background matches, the other
// one carries the shape.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mercury::core {

struct CrosshairStyle {
    // Straight (not premultiplied) 0xAARRGGBB.
    std::uint32_t core = 0xFFFFFFFF;    // the bright strokes
    std::uint32_t outline = 0xFF000000;  // the dark edge that makes them read
    std::uint32_t centre = 0xFFFF3020;   // the dot at the aim point
};

// A square BGRA image, straight alpha, with the aim point at the exact centre.
// Anti-aliased: a hard-edged crosshair crawls badly as it moves over a picture.
inline std::vector<std::uint32_t> make_crosshair(int size, const CrosshairStyle& style = {}) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size, 0);
    const double centre = size / 2.0;
    // Everything is proportional to the size so the shape survives being asked
    // for at a different resolution.
    const double ring_radius = size * 0.30;
    const double arm_inner = size * 0.11;
    const double arm_outer = size * 0.47;
    const double dot_radius = size * 0.045;
    const double core_half = size * 0.035;   // half the bright stroke width
    const double edge = size * 0.033;        // how far the dark outline extends

    const auto channel = [](std::uint32_t colour, int shift) {
        return static_cast<double>((colour >> shift) & 0xFF);
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - centre;
            const double dy = y + 0.5 - centre;
            const double distance = std::sqrt(dx * dx + dy * dy);

            // Distance to the nearest part of the shape, whichever it is.
            double nearest = std::abs(distance - ring_radius);
            const double ax = std::abs(dx);
            const double ay = std::abs(dy);
            if (ay >= arm_inner && ay <= arm_outer) {
                nearest = std::min(nearest, ax);
            }
            if (ax >= arm_inner && ax <= arm_outer) {
                nearest = std::min(nearest, ay);
            }
            const double dot = distance - dot_radius;

            // Coverage of the bright core and of the core-plus-outline, each
            // feathered over a pixel so the edges are smooth.
            const auto coverage = [](double signed_distance) {
                return std::clamp(0.5 - signed_distance, 0.0, 1.0);
            };
            const double in_dot = coverage(dot);
            const double in_core = std::max(coverage(nearest - core_half), in_dot);
            const double in_edge =
                std::max(coverage(nearest - core_half - edge), coverage(dot - edge));

            if (in_edge <= 0.0) {
                continue;
            }

            // The centre dot is its own colour; everything else is the core
            // colour. Both sit on the dark outline.
            const std::uint32_t bright = in_dot > 0.5 ? style.centre : style.core;
            double r = channel(style.outline, 16);
            double g = channel(style.outline, 8);
            double b = channel(style.outline, 0);
            r += (channel(bright, 16) - r) * in_core;
            g += (channel(bright, 8) - g) * in_core;
            b += (channel(bright, 0) - b) * in_core;

            const auto to_byte = [](double value) {
                return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 255.0)));
            };
            const std::uint32_t alpha = to_byte(in_edge * 255.0);
            pixels[static_cast<std::size_t>(y) * size + x] =
                (alpha << 24) | (to_byte(r) << 16) | (to_byte(g) << 8) | to_byte(b);
        }
    }
    return pixels;
}

}  // namespace mercury::core
