// Does a light gun game actually listen to the gun?
//
// The gun mechanism has two gates the GAME controls: the HV counter latch
// (mode register 1 bit 1) and the external interrupt (mode register 3 bit 3).
// If a game never sets either, nothing the emulator does with the gun can
// reach it, and the fault is upstream - the game has not accepted that a gun is
// plugged in. Distinguishing those two cases from the outside is impossible,
// which is what this tool is for.
//
// It also sweeps the aim across the screen and prints the H counter the game
// would read at each point. That is the answer to "the game's own crosshair
// moves up and down but not left and right": if the H column does not change
// as the aim does, no game can track horizontally, and the fault is here rather
// than in the game.
//
// Usage: lightgun_check <rom> [frames] [x] [y] [--justifier]

#include "mercury/genesis/genesis.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: lightgun_check <rom> [frames] [x] [y] [--justifier]\n");
        return 2;
    }
    bool justifier = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--justifier") == 0) {
            justifier = true;
        }
    }
    // Positional arguments skip the flags, so --justifier can go anywhere.
    const auto number = [&](int slot, int fallback) {
        int seen = 0;
        for (int i = 2; i < argc; ++i) {
            if (argv[i][0] == '-') {
                continue;
            }
            if (seen++ == slot) {
                return std::atoi(argv[i]);
            }
        }
        return fallback;
    };
    const int frames = number(0, 1800);
    const auto aim_x = static_cast<std::uint32_t>(number(1, 160));
    const auto aim_y = static_cast<std::uint32_t>(number(2, 112));

    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }

    using Kind = mercury::genesis::ControllerPort::Kind;
    // Both guns live in port 2 - that is where the Menacer's infrared receiver
    // box and the Justifier's cable go. Port 1 keeps a pad, because most of
    // these games are navigated with one and will not reach gameplay without it.
    machine.port2().set_kind(justifier ? Kind::justifier : Kind::menacer);
    machine.port2().set_aim(aim_x, aim_y, true);

    bool saw_hv_enable = false;
    bool saw_ext_enable = false;
    int first_hv_frame = -1;
    int first_ext_frame = -1;

    for (int frame = 0; frame < frames; ++frame) {
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::a, phase >= 60 && phase < 68);
        // Hold the trigger down for part of every cycle, since some games only
        // arm the gun once they have seen it fire.
        machine.port2().set_button(Button::a, phase >= 90 && phase < 104);
        machine.port2().set_aim(aim_x, aim_y, true);
        machine.run_frame();

        if (!saw_hv_enable && machine.vdp().hv_latch_enabled()) {
            saw_hv_enable = true;
            first_hv_frame = frame;
        }
        if (!saw_ext_enable && machine.vdp().external_interrupt_enabled()) {
            saw_ext_enable = true;
            first_ext_frame = frame;
        }
    }

    const auto& vdp = machine.vdp();
    std::printf("frames run              : %d\n", frames);
    std::printf("aim point               : %u, %u  (screen %ux%u)\n", aim_x, aim_y,
                vdp.screen_width(), vdp.screen_height());
    std::printf("gun saw the beam        : %llu times\n",
                static_cast<unsigned long long>(vdp.light_gun_latches()));
    std::printf("  ...HV counter latched : %llu  (game had the latch enabled)\n",
                static_cast<unsigned long long>(vdp.light_gun_hv_latches()));
    std::printf("  ...external interrupt : %llu  (game had level 2 enabled)\n",
                static_cast<unsigned long long>(vdp.light_gun_interrupts()));
    std::printf("HV latch ever enabled   : %s%s\n", saw_hv_enable ? "yes" : "NO",
                saw_hv_enable ? ("  first at frame " + std::to_string(first_hv_frame)).c_str()
                              : "");
    std::printf("level 2 ever enabled    : %s%s\n", saw_ext_enable ? "yes" : "NO",
                saw_ext_enable ? ("  first at frame " + std::to_string(first_ext_frame)).c_str()
                               : "");
    std::printf("port 2 reads            : %llu\n",
                static_cast<unsigned long long>(machine.port2().reads()));
    // The number that says whether the machine is healthy. One per frame is
    // right. Thousands per frame means the interrupt is being raised and never
    // retired, and the CPU is doing nothing but re-entering the handler.
    const auto serviced = machine.m68k_ext_count();
    const double per_frame = frames == 0 ? 0.0 : static_cast<double>(serviced) / frames;
    std::printf("level 2 taken by the CPU: %llu  (%.2f per frame)%s\n",
                static_cast<unsigned long long>(serviced), per_frame,
                per_frame > 10.0 ? "   <-- INTERRUPT STORM" : "");

    // Does the position the game can read actually follow the aim? Run one more
    // frame per sample and read back what the HV counter holds on the gun's own
    // line. H must change with x and V with y; when it was gated on the game
    // setting M3, H sat still and only V moved.
    std::printf("\naim -> latched HV counter\n");
    for (std::uint32_t x = 0; x < vdp.screen_width(); x += vdp.screen_width() / 8) {
        const std::uint32_t y = 60 + (x / 8);
        machine.port2().set_aim(x, y, true);
        // Both Justifier guns, or the reading goes stale on every frame the
        // game happens to have the OTHER gun selected - only the selected gun
        // can see the beam, so aiming just one produces a column of repeated
        // values that looks like the counter has stopped tracking.
        machine.port2().set_gun2_aim(x, y, true);
        machine.run_frame();
        const auto hv = machine.vdp().hv_latch_value();
        std::printf("  x=%3u y=%3u  ->  H=0x%02X V=0x%02X  %s\n", x, y, hv & 0xFF, (hv >> 8) & 0xFF,
                    machine.vdp().hv_latched() ? "latched" : "FREE-RUNNING");
    }

    if (!saw_hv_enable && !saw_ext_enable) {
        std::printf(
            "\nThe game never opened either gate, so it is not listening for a gun.\n"
            "That is a detection or a menu problem, not a gun-emulation one.\n");
    }
    return 0;
}
