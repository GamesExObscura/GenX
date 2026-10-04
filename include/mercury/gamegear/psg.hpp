#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mercury::gamegear {

class Psg {
public:
    static constexpr double kBaseClockHz = 3'579'545.0;
    static constexpr std::uint32_t kDefaultSampleRate = 44'100;
    static constexpr std::size_t kToneChannelCount = 3;
    static constexpr std::size_t kNoiseChannelCount = 1;
    static constexpr std::size_t kChannelCount = kToneChannelCount + kNoiseChannelCount;

    void reset() noexcept;
    void write(std::uint8_t value) noexcept;
    void advance_cycles(std::uint32_t cpu_cycles) noexcept;

    [[nodiscard]] const std::vector<std::int16_t>& sample_buffer() const noexcept;
    void clear_sample_buffer() noexcept;
    [[nodiscard]] std::uint64_t write_count() const noexcept;

    // Every byte the driver writes, with the output sample it landed on. The
    // same idea as the FM chip's log and for the same reason: it separates "the
    // driver asked for the wrong thing" from "we played the right thing
    // wrongly", which nothing else can. Off by default - it allocates.
    struct WriteLogEntry {
        std::uint64_t sample;
        std::uint8_t value;
    };
    void set_write_logging(bool on) noexcept {
        write_logging_ = on;
        if (!on) {
            write_log_.clear();
            write_log_.shrink_to_fit();
        }
    }
    [[nodiscard]] bool write_logging() const noexcept { return write_logging_; }
    [[nodiscard]] const std::vector<WriteLogEntry>& write_log() const noexcept {
        return write_log_;
    }
    void clear_write_log() noexcept { write_log_.clear(); }
    [[nodiscard]] std::uint64_t sample_index() const noexcept { return generated_sample_count_; }

    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string activity_summary() const;
    [[nodiscard]] std::array<const char*, 4> channels() const noexcept;

    // The rate the noise shift register actually advances at. Public so a test
    // can hold it against the datasheet rather than against itself - the four
    // shift-rate settings have a fixed relationship to each other on hardware,
    // and getting one of them wrong is inaudible in music and obvious in sound
    // effects.
    [[nodiscard]] double noise_shift_rate_hz() const noexcept;

public:
    // The chip's internal counters are clocked at the PSG clock divided by
    // sixteen. Everything else - tone frequency, noise shift rate - falls out of
    // that one divisor.
    static constexpr std::uint32_t kCounterClockDivider = 16;

private:
    // Counters, not phase accumulators. The old model advanced a floating-point
    // phase at each channel's AUDIBLE frequency and point-sampled it at 44.1kHz,
    // which aliases: a square wave has energy far above the output Nyquist, and
    // sampling it directly folds that energy back down as a crackle that sweeps
    // with the note. The chip is now run at its own counter rate and the output
    // is averaged down to the sample rate, which is what BlastEm and Kega both
    // do - the reason they sound clean at the same 44.1kHz output.
    struct ToneChannel {
        std::uint16_t period = 0x0001;
        std::uint16_t counter = 0;
        std::uint8_t attenuation = 0x0F;
        bool output_high = true;
    };

    struct NoiseChannel {
        std::uint8_t control = 0x00;
        std::uint8_t attenuation = 0x0F;
        std::uint16_t lfsr = 0x8000;
        std::uint16_t counter = 0;
        // The counter's flip-flop. The shift register advances on its rising
        // edge only, which is why a shift takes two counter periods.
        bool output_high = true;
        // The shift register bit actually being heard.
        bool noise_out = false;
    };

    [[nodiscard]] double attenuation_gain(std::uint8_t attenuation) const noexcept;
    [[nodiscard]] double tone_frequency_hz(std::size_t channel_index) const noexcept;
    [[nodiscard]] double noise_frequency_hz() const noexcept;
    // The noise counter's reload value: a fixed 0x10/0x20/0x40, or tone channel
    // 2's period unchanged.
    [[nodiscard]] std::uint16_t noise_counter_load() const noexcept;
    void tick_counters() noexcept;
    [[nodiscard]] double current_output() const noexcept;
    void mix_samples_for_cycles(std::uint32_t cpu_cycles) noexcept;

  public:
    // sample_buffer_ is the host's output queue and the rest are diagnostics.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(tone_channels_);
        ar(noise_channel_);
        ar(last_value_);
        ar(latched_channel_);
        ar(latched_volume_register_);
        ar(latched_noise_register_);
        ar(sample_phase_accumulator_);
        ar(tick_debt_);
        if constexpr (Archive::loading()) {
            // A loaded state can come from someone else. Both accumulators sit
            // under one step between calls; a huge or NaN value would make the
            // next advance tick the chip for billions of steps.
            if (!(sample_phase_accumulator_ >= 0.0 && sample_phase_accumulator_ < 1.0)) {
                sample_phase_accumulator_ = 0.0;
            }
            tick_debt_ %= kCounterClockDivider;
        }
    }

  private:
    std::array<ToneChannel, kToneChannelCount> tone_channels_{};
    NoiseChannel noise_channel_{};
    std::vector<std::int16_t> sample_buffer_{};
    std::uint8_t last_value_ = 0x00;
    std::uint8_t latched_channel_ = 0x00;
    bool latched_volume_register_ = false;
    bool latched_noise_register_ = false;
    double sample_phase_accumulator_ = 0.0;
    // Unspent PSG clocks, and the box filter that averages the ~5 counter ticks
    // that fall inside one output sample.
    std::uint32_t tick_debt_ = 0;
    double output_accumulator_ = 0.0;
    std::uint32_t accumulated_ticks_ = 0;
    bool write_logging_ = false;
    std::vector<WriteLogEntry> write_log_;
    std::uint64_t write_count_ = 0;
    std::uint64_t latch_write_count_ = 0;
    std::uint64_t data_write_count_ = 0;
    std::uint64_t generated_sample_count_ = 0;
    std::uint64_t nonzero_sample_count_ = 0;
    std::array<std::uint64_t, kToneChannelCount> tone_latch_count_{};
    std::array<std::uint64_t, kToneChannelCount> tone_data_count_{};
    std::array<std::uint64_t, kChannelCount> volume_latch_count_{};
    std::array<std::uint64_t, kChannelCount> volume_data_count_{};
    std::uint64_t noise_latch_count_ = 0;
    std::uint64_t noise_data_count_ = 0;
    std::array<std::uint16_t, kToneChannelCount> min_period_seen_{};
    std::array<std::uint16_t, kToneChannelCount> max_period_seen_{};
    std::array<std::uint64_t, kToneChannelCount> tone_period_update_count_{};
    std::int16_t min_sample_ = 0;
    std::int16_t max_sample_ = 0;
    double last_raw_mix_ = 0.0;
    double last_filtered_mix_ = 0.0;
};

}  // namespace mercury::gamegear
