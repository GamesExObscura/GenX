// Where does the GAME think the gun is pointed?
//
// A light gun game draws its own crosshair from the position it worked out, so
// the crosshair's sprite IS the game's answer, readable straight out of the
// sprite attribute table. This aims at a series of points and reports what the
// game's own crosshair did in response - which turns "the cursor is over to the
// right and wraps round" from a description into a transfer function.
//
// The alternative was reasoning about what constant to add, and that is exactly
// the guess this tool exists to replace.
//
// Usage: gun_calibrate <rom> [--justifier] [--warmup N] [--settle N] [--y N]

#include "mercury/genesis/genesis.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// A screenshot per aim point, so the game's own crosshair can be LOOKED at
// rather than inferred from a sprite table that may not be where I think.
// stride_pixels is NOT always width: the framebuffer keeps its row pitch when a
// game switches to the narrow 256-pixel mode, so assuming width here produced a
// picture sheared diagonally across the screen and briefly looked like a
// rendering bug in the emulator rather than a bug in this function.
void write_bmp24(const std::string& path, const std::vector<std::uint8_t>& rgb,
                 std::uint32_t width, std::uint32_t height, std::uint32_t stride_pixels) {
    const std::uint32_t row_stride = (width * 3 + 3) & ~std::uint32_t{3};
    const std::uint32_t image_bytes = row_stride * height;
    const std::uint32_t file_size = 54 + image_bytes;
    std::vector<std::uint8_t> header(54, 0);
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header.data() + 2, &file_size, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header.data() + 10, &offset, 4);
    const std::uint32_t dib = 40;
    std::memcpy(header.data() + 14, &dib, 4);
    std::memcpy(header.data() + 18, &width, 4);
    std::memcpy(header.data() + 22, &height, 4);
    const std::uint16_t planes = 1;
    std::memcpy(header.data() + 26, &planes, 2);
    const std::uint16_t bpp = 24;
    std::memcpy(header.data() + 28, &bpp, 2);
    std::memcpy(header.data() + 34, &image_bytes, 4);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), 54);
    std::vector<std::uint8_t> row(row_stride, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t source_y = height - 1 - y;  // BMP rows run bottom-up
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(source_y) * stride_pixels + x) * 3;
            if (at + 2 >= rgb.size()) {
                continue;
            }
            row[x * 3 + 0] = rgb[at + 2];
            row[x * 3 + 1] = rgb[at + 1];
            row[x * 3 + 2] = rgb[at + 0];
        }
        out.write(reinterpret_cast<const char*>(row.data()), row_stride);
    }
}

struct Sprite {
    int x = 0;
    int y = 0;
};

// The sprite attribute table, as the VDP itself addresses it. In H40 the low
// bit of register 5 is ignored, which is a real hardware quirk and not a
// rounding convenience.
std::vector<Sprite> read_sprites(const mercury::genesis::Genesis& machine) {
    const auto& vdp = machine.vdp();
    const bool h40 = (vdp.reg(12) & 0x01) != 0;
    const std::uint32_t mask = h40 ? 0x7Eu : 0x7Fu;
    const std::uint32_t base = (static_cast<std::uint32_t>(vdp.reg(5)) & mask) << 9;
    const int count = h40 ? 80 : 64;
    const auto& vram = vdp.vram();
    std::vector<Sprite> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const std::uint32_t at = (base + static_cast<std::uint32_t>(i) * 8) & 0xFFFF;
        const int y = ((vram[at] << 8) | vram[at + 1]) & 0x3FF;
        const int x = ((vram[(at + 6) & 0xFFFF] << 8) | vram[(at + 7) & 0xFFFF]) & 0x1FF;
        out.push_back({x, y});
    }
    return out;
}

// Enough activity to get past a title screen and into play, which is the only
// state where a crosshair exists to measure.
//
// The TRIGGER matters as much as the pad: the Menacer 6-Game Cartridge is a
// gun-only title and sits on its logo forever if all you send it is Start. It
// is deliberately not pulled during measurement, where firing at the screen
// would change the very thing being measured.
void drive_pad(mercury::genesis::Genesis& machine, int frame, bool fire) {
    using mercury::genesis::Button;
    const int phase = frame % 120;
    machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
    machine.port1().set_button(Button::a, phase >= 60 && phase < 68);
    machine.port1().set_button(Button::c, phase >= 90 && phase < 98);
    machine.port2().set_button(Button::a, fire && phase >= 40 && phase < 50);
    machine.port2().set_button(Button::start, fire && phase >= 100 && phase < 110);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gun_calibrate <rom> [--justifier] [--warmup N] [--settle N] [--y N]\n");
        return 2;
    }
    bool justifier = false;
    int warmup = 900;
    int settle = 30;
    std::uint32_t aim_y = 112;
    // What ships, unless --hoffset overrides it. Defaulting this to zero made
    // the tool silently measure a configuration nobody runs. Set from the
    // narrow-mode value and then corrected below once the game has picked its
    // video mode, since the two are different numbers.
    std::uint32_t h_offset = 0;
    bool h_offset_given = false;
    bool scan = false;
    bool fire_test = false;
    bool scan_fire = false;
    // The fire sweep's x range and granularity. Sixteen pixels was enough to
    // find the mapping but far too coarse to settle a residual of a few, which
    // is what a player notices as "the shot is a bit left of the sight".
    int fire_step = 16;
    int fire_from = 0;
    int fire_to = -1;
    int slot = -1;
    std::string shots;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--justifier") == 0) {
            justifier = true;
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--settle") == 0 && i + 1 < argc) {
            settle = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            aim_y = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
            shots = argv[++i];
        } else if (std::strcmp(argv[i], "--hoffset") == 0 && i + 1 < argc) {
            h_offset = static_cast<std::uint32_t>(std::atoi(argv[++i]));
            h_offset_given = true;
        } else if (std::strcmp(argv[i], "--scan") == 0) {
            scan = true;
        } else if (std::strcmp(argv[i], "--fire") == 0) {
            fire_test = true;
        } else if (std::strcmp(argv[i], "--scan-fire") == 0) {
            scan_fire = true;
        } else if (std::strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
            fire_step = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            fire_from = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            fire_to = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
            slot = std::atoi(argv[++i]);
        }
    }

    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }

    using Kind = mercury::genesis::ControllerPort::Kind;
    machine.port2().set_kind(justifier ? Kind::justifier : Kind::menacer);
    // One gun, as a player testing on their own has.
    machine.port2().set_gun2_present(false);

    machine.port2().set_aim(160, aim_y, true);
    for (int frame = 0; frame < warmup; ++frame) {
        drive_pad(machine, frame, true);
        machine.run_frame();
    }
    // AFTER the warm-up, when the game has chosen its video mode: the shipping
    // origin is a different number in each, so which one to compare against is
    // not known until the mode is.
    if (!h_offset_given) {
        h_offset = machine.vdp().wide_mode()
                       ? mercury::genesis::ControllerPort::kDefaultHOffsetWide
                       : mercury::genesis::ControllerPort::kDefaultHOffsetNarrow;
    }
    machine.port2().set_light_gun_h_offset(h_offset);
    // A snapshot of the game IN PLAY. Every later sample restores it first, so
    // each measurement sees the same starting state - without that, a scan of
    // two hundred offsets is measuring a game that has meanwhile been shot,
    // reset and returned to its title screen.
    const auto snapshot = machine.save_state_blob();
    // AFTER the warm-up: the mode is whatever the game set, not the reset
    // default, and reading it before running gave a 256-wide sweep on a game
    // that turned out to be running 320-wide.
    const std::uint32_t width =
        machine.vdp().screen_width() != 0 ? machine.vdp().screen_width() : 320;

    // Aim points across the whole width, including both edges.
    std::vector<std::uint32_t> points;
    for (int i = 0; i <= 8; ++i) {
        points.push_back(static_cast<std::uint32_t>((width - 1) * i / 8));
    }

    // One sample: put the game back where it was, aim, let it react, read back.
    const auto sample_at = [&](std::uint32_t x, std::uint32_t offset) {
        std::string error;
        (void)machine.load_state_blob(snapshot, error);
        machine.port2().set_light_gun_h_offset(offset);
        for (int frame = 0; frame < settle; ++frame) {
            drive_pad(machine, warmup + frame, false);
            machine.port2().set_aim(x, aim_y, true);
            machine.run_frame();
        }
        return read_sprites(machine);
    };

    // Which sprite is the crosshair. Naming it by slot number does not survive a
    // state load - the game rebuilds its sprite list and the same crosshair
    // comes back in a different slot, which made a whole scan measure a static
    // sprite. It is identified by BEHAVIOUR instead: the crosshair is drawn on
    // the row the gun is pointed at, so its y is exactly aim_y plus the VDP's
    // 128-pixel sprite origin, whatever the game does with x.
    const auto crosshair_x = [&](const std::vector<Sprite>& sprites) {
        const int want_y = static_cast<int>(aim_y) + 128;
        for (const auto& sprite : sprites) {
            if (sprite.y == want_y) {
                return sprite.x - 128;
            }
        }
        return INT_MIN;
    };

    // Does PULLING THE TRIGGER change anything at all?
    //
    // "The crosshair is in the right place but nothing dies" has two completely
    // different causes and they need separating before either can be chased:
    // the trigger may not be reaching the game, or it may be reaching it and
    // being acted on somewhere other than where the sight is. Running the same
    // snapshot twice - once firing, once not - isolates the effect of the shot
    // from everything else the game was going to do anyway.
    // Calibrate a game that draws no crosshair, using its BULLET HOLES.
    //
    // The crosshair scan cannot work on Lethal Enforcers or Snatcher - there is
    // no sight sprite to compare against. But a shot that lands on the scene
    // changes the picture where it lands, and a shot decoded to a position the
    // game rejects changes almost nothing beyond the ammo counter. So the total
    // picture disturbance over a spread of aim points is a score, and the origin
    // that maximises it is the origin where shots are actually landing on
    // things. Same closed loop as the crosshair scan, different oracle.
    if (scan_fire) {
        const std::uint32_t width = machine.vdp().screen_width();
        const std::uint32_t stride = mercury::genesis::Vdp::kMaxWidth;
        const std::uint32_t height = machine.vdp().screen_height();
        const std::uint32_t steps = machine.vdp().wide_mode() ? 210u : 171u;
        const int run = 40;
        std::vector<std::uint32_t> aims;
        for (std::uint32_t x = 12; x < width; x += width / 8) {
            aims.push_back(x);
        }
        const auto play = [&](std::uint32_t x, std::uint32_t y, std::uint32_t origin, bool pull) {
            std::string error;
            (void)machine.load_state_blob(snapshot, error);
            machine.port2().set_light_gun_h_offset(origin);
            for (int frame = 0; frame < run; ++frame) {
                drive_pad(machine, warmup + frame, false);
                machine.port2().set_aim(x, y, true);
                machine.port2().set_button(mercury::genesis::Button::a,
                                           pull && frame >= 5 && frame < 20);
                machine.run_frame();
            }
            return machine.vdp().frame_pixels();
        };
        std::printf("scanning %u origins against where the shots LAND, %zu aim points each\n",
                    steps, aims.size());
        std::vector<std::pair<long long, std::uint32_t>> scores;
        for (std::uint32_t origin = 0; origin < steps; ++origin) {
            long long disturbance = 0;
            for (const auto x : aims) {
                const auto quiet = play(x, aim_y, origin, false);
                const auto fired = play(x, aim_y, origin, true);
                for (std::uint32_t row = 0; row < height; ++row) {
                    for (std::uint32_t col = 0; col < width; ++col) {
                        const std::size_t i = (static_cast<std::size_t>(row) * stride + col) * 3;
                        if (i + 2 < quiet.size() &&
                            (quiet[i] != fired[i] || quiet[i + 1] != fired[i + 1] ||
                             quiet[i + 2] != fired[i + 2])) {
                            ++disturbance;
                        }
                    }
                }
            }
            scores.push_back({disturbance, origin});
        }
        std::sort(scores.rbegin(), scores.rend());
        std::printf("\nbest origins (total pixels the shots disturbed):\n");
        for (std::size_t i = 0; i < 8 && i < scores.size(); ++i) {
            std::printf("  offset %3u   %8lld px\n", scores[i].second, scores[i].first);
        }
        std::printf("\nworst for comparison:\n");
        for (std::size_t i = 0; i < 3 && i < scores.size(); ++i) {
            const auto& row = scores[scores.size() - 1 - i];
            std::printf("  offset %3u   %8lld px\n", row.second, row.first);
        }
        return 0;
    }

    if (fire_test) {
        // Long enough for a shot to land and something to react to it. Sixty
        // frames is a second; the first attempt used forty with the trigger
        // held for six and aimed only at one row, which was the empty sky.
        const int run = std::max(settle, 60);
        const auto shoot_at = [&](std::uint32_t x, std::uint32_t y, bool pull) {
            std::string error;
            (void)machine.load_state_blob(snapshot, error);
            machine.port2().set_light_gun_h_offset(h_offset);
            for (int frame = 0; frame < run; ++frame) {
                drive_pad(machine, warmup + frame, false);
                machine.port2().set_aim(x, y, true);
                machine.port2().set_button(mercury::genesis::Button::a,
                                           pull && frame >= 5 && frame < 25);
                machine.run_frame();
            }
            return machine.vdp().frame_pixels();
        };

        const std::uint32_t height =
            machine.vdp().screen_height() != 0 ? machine.vdp().screen_height() : 224;
        const std::uint32_t width = machine.vdp().screen_width();
        // The framebuffer keeps the widest mode's row pitch whatever the game
        // is displaying, so this is a constant and not either of the two things
        // it was guessed to be.
        const std::uint32_t stride = mercury::genesis::Vdp::kMaxWidth;
        std::printf("firing vs not firing from the same state, %d frames each\n", run);
        std::printf("frame %ux%u, buffer stride %u pixels\n", width, height, stride);
        // WHERE the shot landed, not just whether it did anything. The impact
        // mark is the only thing that differs between the two runs, so the
        // middle of the changed pixels IS the point the game decided was shot -
        // which is the measurement a game that draws no crosshair otherwise
        // refuses to give up.
        std::printf("\n  aimed at   ->  game put the shot at   (offset)\n");
        long long total = 0;
        // Finer across x than the crosshair sweep: the question here is the
        // SHAPE of the mapping, and nine points cannot show where it breaks.
        std::vector<std::uint32_t> fine;
        const std::uint32_t last = fire_to < 0 ? width - 1 : static_cast<std::uint32_t>(fire_to);
        for (std::uint32_t x = static_cast<std::uint32_t>(std::max(0, fire_from)); x <= last &&
             x < width; x += static_cast<std::uint32_t>(std::max(1, fire_step))) {
            fine.push_back(x);
        }
        for (std::uint32_t y = aim_y; y < aim_y + 1; ++y) {
            for (const auto x : fine) {
                const auto quiet = shoot_at(x, y, false);
                const auto fired = shoot_at(x, y, true);
                long long changed = 0;
                long long sum_x = 0;
                long long sum_y = 0;
                const std::size_t count = std::min(quiet.size(), fired.size());
                for (std::uint32_t row = 0; row < height; ++row) {
                    for (std::uint32_t col = 0; col < width; ++col) {
                        const std::size_t i = (static_cast<std::size_t>(row) * stride + col) * 3;
                        if (i + 2 >= count) {
                            continue;
                        }
                        if (quiet[i] != fired[i] || quiet[i + 1] != fired[i + 1] ||
                            quiet[i + 2] != fired[i + 2]) {
                            ++changed;
                            sum_x += col;
                            sum_y += row;
                        }
                    }
                }
                total += changed;
                if (changed < 40) {
                    std::printf("  %3u,%3u    ->  no impact (%lld px changed)\n", x, y, changed);
                    continue;
                }
                const long long hit_x = sum_x / changed;
                const long long hit_y = sum_y / changed;
                std::printf("  %3u,%3u    ->  %3lld,%3lld   (%+4lld,%+4lld)  %lld px\n", x, y, hit_x,
                            hit_y, hit_x - x, hit_y - y, changed);
            }
        }
        std::printf("\n%s\n", total == 0
                                  ? "NOTHING changed. The trigger is not reaching the game at all,\n"
                                    "so this is a button problem and not an aiming one."
                                  : "The shot has an effect, so the trigger IS reaching the game.");

        // What the game is actually being handed. If the trigger bit is moving
        // here then the emulator is delivering it and the game is refusing it
        // for some other reason; if it is not, the fault is upstream of the game
        // and this is where it is.
        std::printf("\nbytes the game read from port 2 (trigger is bit 1, 0x02):\n");
        for (int bank = 0; bank < 2; ++bank) {
            const auto& log = machine.port2().read_log(bank);
            for (std::size_t value = 0; value < log.size(); ++value) {
                if (log[value] != 0) {
                    std::printf("  TH %s  0x%02zX  x%llu\n", bank == 0 ? "high" : "low ", value,
                                static_cast<unsigned long long>(log[value]));
                }
            }
        }
        return 0;
    }

    if (scan) {
        (void)slot;
        // Score every possible origin against the game's own crosshair. The
        // right one is the one where the game draws where the gun is pointed.
        std::printf("scanning 210 origins against the game's own crosshair over %zu aim points...\n",
                    points.size());
        std::uint32_t best = 0;
        double best_error = 1e30;
        std::vector<std::pair<double, std::uint32_t>> scores;
        for (std::uint32_t offset = 0; offset < 210; ++offset) {
            double error = 0.0;
            for (const auto x : points) {
                const int drawn = crosshair_x(sample_at(x, offset));
                error += drawn == INT_MIN ? 320.0
                                          : std::abs(static_cast<double>(drawn - static_cast<int>(x)));
            }
            error /= static_cast<double>(points.size());
            scores.push_back({error, offset});
            if (error < best_error) {
                best_error = error;
                best = offset;
            }
        }
        std::sort(scores.begin(), scores.end());
        std::printf("\nbest origins (mean pixels away from where the gun points):\n");
        for (std::size_t i = 0; i < 6 && i < scores.size(); ++i) {
            std::printf("  offset %3u   error %6.1f px\n", scores[i].second, scores[i].first);
        }
        std::printf("\nwith offset %u:\n  aim   :", best);
        for (const auto x : points) {
            std::printf(" %4u", x);
        }
        std::printf("\n  drawn :");
        for (const auto x : points) {
            std::printf(" %4d", crosshair_x(sample_at(x, best)));
        }
        std::printf("\n");
        return 0;
    }

    std::vector<std::vector<Sprite>> samples;
    int index = 0;
    for (const auto x : points) {
        samples.push_back(sample_at(x, h_offset));
        if (!shots.empty()) {
            char name[512];
            std::snprintf(name, sizeof(name), "%s/aim_%d_x%03u.bmp", shots.c_str(), index, x);
            // Row pitch is kMaxWidth ALWAYS, even in the narrow 256-pixel mode.
            // Guessing it - first from the visible width, then from the buffer
            // size - sheared the picture into diagonal streaks and, far worse,
            // silently wrecked the shot-position measurement below.
            write_bmp24(name, machine.vdp().frame_pixels(), machine.vdp().screen_width(),
                        machine.vdp().screen_height(), mercury::genesis::Vdp::kMaxWidth);
        }
        ++index;
    }

    const std::size_t slots = samples.front().size();
    std::printf("screen %ux%u, aim y=%u, %zu sprite slots\n", machine.vdp().screen_width(),
                machine.vdp().screen_height(), aim_y, slots);
    std::printf("H counter handed to the game: ");
    for (const auto x : points) {
        machine.port2().set_aim(x, aim_y, true);
        std::printf("%3u->0x%02X ", x, machine.port2().light_gun_h(machine.vdp().wide_mode()));
    }
    std::printf("\n\n");

    // A sprite that follows the aim is the crosshair. Anything static is scenery
    // and anything that moves on its own will not be monotonic across the sweep.
    bool found = false;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        int lo = 0x1FF;
        int hi = 0;
        for (const auto& sample : samples) {
            lo = sample[slot].x < lo ? sample[slot].x : lo;
            hi = sample[slot].x > hi ? sample[slot].x : hi;
        }
        if (hi - lo < 8) {
            continue;  // not tracking anything
        }
        found = true;
        std::printf("sprite %2zu  x:", slot);
        for (const auto& sample : samples) {
            std::printf(" %3d", sample[slot].x);
        }
        std::printf("   y:");
        for (const auto& sample : samples) {
            std::printf(" %3d", sample[slot].y);
        }
        std::printf("\n");
    }
    if (!found) {
        std::printf("No sprite followed the aim. Either the game is not in play\n"
                    "(try a bigger --warmup) or it draws its crosshair another way.\n");
    }
    // Sprite X is offset by 128 in the VDP's own coordinates: a sprite at screen
    // x=0 is stored as 128. Printed raw above so the numbers can be checked by
    // hand against a screenshot.
    std::printf("\naim points   :");
    for (const auto x : points) {
        std::printf(" %3u", x);
    }
    std::printf("\n(subtract 128 from sprite x for screen position)\n");
    return 0;
}
