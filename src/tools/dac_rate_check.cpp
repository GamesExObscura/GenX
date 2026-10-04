// How fast is the sound driver actually feeding the DAC?
//
// A sampled sound that plays "stretched" is a sample being fed to the DAC too
// slowly. The driver pushes one byte per sample into YM2612 register 0x2A, so
// counting those writes over a known span of emulated time gives the playback
// rate directly - no guessing, and no need to identify the sound by ear.
//
// A Mega Drive sound driver pushing DAC samples is typically in the 8-26kHz
// range. Well under that and every sampled sound in the game is stretched; the
// figure to compare against is real hardware or another emulator, but an order
// of magnitude error shows up on its own.
//
// Usage: dac_rate_check <rom> [frames]

#include "mercury/genesis/genesis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The same mix the runtime plays: the FM resampled to the output rate, the PSG
// added at the ratio BlastEm's constants imply, then the console's output-stage
// low-pass. Duplicated here rather than shared because the runtime's copy lives
// inside the Win32 shell - but it has to stay the same mix, or a recording made
// with this tool cannot be compared against one made with the emulator.
constexpr double kLowPassHz = 3390.0;

void write_wav(const std::string& path, const std::vector<std::int16_t>& samples) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::printf("could not write %s\n", path.c_str());
        return;
    }
    const std::uint32_t rate = mercury::gamegear::Psg::kDefaultSampleRate;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    const auto put32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    put32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    out.write("data", 4);
    put32(data_bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    std::printf("wrote %s (%zu samples, %.2f s)\n", path.c_str(), samples.size(),
                static_cast<double>(samples.size()) / rate);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dac_rate_check <rom> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 1800;

    mercury::genesis::Genesis machine;
    const auto load = machine.load_rom(argv[1]);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }

    // --wav <path> writes what the emulator would have played, so a headless
    // run can be compared against a recording from another emulator without
    // anyone having to reproduce the same input by hand.
    std::string wav_path;
    // --no-input leaves the pad alone, which makes a boot-to-title recording
    // byte-for-byte reproducible and directly comparable between emulators.
    bool auto_input = true;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--wav" && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (arg == "--no-input") {
            auto_input = false;
        }
    }

    std::uint64_t previous_dac = 0;
    std::uint64_t busiest_dac = 0;
    int busiest_frame = 0;

    std::vector<std::int16_t> mixed_out;
    double resample_phase = 0.0;
    int previous_mono = 0;
    double lowpass = 0.0;
    const double alpha =
        1.0 - std::exp(-2.0 * 3.14159265358979323846 * kLowPassHz /
                       static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate));
    const double ratio = mercury::genesis::Ym2612::kSampleRateHz /
                         static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate);

    for (int frame = 0; frame < frames; ++frame) {
        using mercury::genesis::Button;
        // The same tapping the sweep uses, to get past a title screen.
        const int phase = frame % 120;
        if (auto_input) {
            machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
            machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
            machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
        }
        machine.run_frame();

        if (!wav_path.empty()) {
            const auto& fm = machine.ym2612().sample_buffer();
            const auto& psg = machine.psg().sample_buffer();
            const std::size_t fm_frames = fm.size() / 2;
            std::vector<int> mono;
            mono.reserve(fm_frames + 1);
            mono.push_back(previous_mono);
            for (std::size_t i = 0; i < fm_frames; ++i) {
                mono.push_back((static_cast<int>(fm[i * 2]) + static_cast<int>(fm[i * 2 + 1])) / 2);
            }
            std::vector<std::int16_t> resampled;
            while (resample_phase < static_cast<double>(fm_frames)) {
                const auto index = static_cast<std::size_t>(resample_phase);
                const double fraction = resample_phase - static_cast<double>(index);
                const int a = mono[index];
                const int b = mono[index + 1];
                resampled.push_back(
                    static_cast<std::int16_t>(std::clamp(a + static_cast<int>((b - a) * fraction),
                                                         -32768, 32767)));
                resample_phase += ratio;
            }
            resample_phase -= static_cast<double>(fm_frames);
            previous_mono = mono.back();

            const std::size_t common = std::min(resampled.size(), psg.size());
            for (std::size_t i = 0; i < resampled.size(); ++i) {
                int value = (static_cast<int>(resampled[i]) * 3) / 4;
                if (i < common) {
                    value += (static_cast<int>(psg[i]) * 7) / 32;
                }
                lowpass += alpha * (static_cast<double>(std::clamp(value, -32768, 32767)) - lowpass);
                mixed_out.push_back(
                    static_cast<std::int16_t>(std::clamp(static_cast<int>(lowpass), -32768, 32767)));
            }
            machine.ym2612().clear_sample_buffer();
            machine.psg().clear_sample_buffer();
        }

        const auto now = machine.ym2612().dac_write_count();
        const auto in_frame = now - previous_dac;
        previous_dac = now;
        if (in_frame > busiest_dac) {
            busiest_dac = in_frame;
            busiest_frame = frame;
        }
    }

    const auto total = machine.ym2612().dac_write_count();
    const double seconds = static_cast<double>(frames) / 60.0;

    std::printf("frames run        : %d (%.2f s of emulated time)\n", frames, seconds);
    std::printf("DAC writes        : %llu\n", static_cast<unsigned long long>(total));
    std::printf("average rate      : %.0f Hz (over the whole run, silence included)\n",
                static_cast<double>(total) / seconds);
    // The average is diluted by every frame with no sample playing. The busiest
    // single frame is the honest measure of the driver's playback rate.
    std::printf("busiest frame     : %llu writes on frame %d -> %.0f Hz while actually playing\n",
                static_cast<unsigned long long>(busiest_dac), busiest_frame,
                static_cast<double>(busiest_dac) * 60.0);
    std::printf("FM key-ons        : %llu\n",
                static_cast<unsigned long long>(machine.ym2612().key_on_count()));
    std::printf("CSM mode enabled  : %llu times%s\n",
                static_cast<unsigned long long>(machine.ym2612().csm_enable_count()),
                machine.ym2612().csm_enable_count() == 0 ? "  (so CSM is not what this game uses)"
                                                         : "  (CSM IS used - it is unimplemented)");
    std::printf("LFO enabled       : %llu times\n",
                static_cast<unsigned long long>(machine.ym2612().lfo_enable_count()));
    std::printf("SSG-EG enabled    : %llu times%s\n",
                static_cast<unsigned long long>(machine.ym2612().ssg_enable_count()),
                machine.ym2612().ssg_enable_count() == 0
                    ? "  (so SSG-EG cannot be involved in this game)"
                    : "");
    std::printf("PSG writes        : %llu\n",
                static_cast<unsigned long long>(machine.psg().write_count()));
    if (!wav_path.empty()) {
        write_wav(wav_path, mixed_out);
    }
    return 0;
}
