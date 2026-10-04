#include "mercury/gamegear/psg.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mercury::gamegear {

namespace {

constexpr double kHighPassPole = 0.995;

double clamp_unit(double value) {
    return std::clamp(value, -1.0, 1.0);
}

void update_period_observation(std::array<std::uint16_t, Psg::kToneChannelCount>& min_period_seen,
                               std::array<std::uint16_t, Psg::kToneChannelCount>& max_period_seen,
                               std::array<std::uint64_t, Psg::kToneChannelCount>& update_count,
                               std::size_t channel_index,
                               std::uint16_t period) {
    if (update_count[channel_index] == 0) {
        min_period_seen[channel_index] = period;
        max_period_seen[channel_index] = period;
    } else {
        min_period_seen[channel_index] = std::min(min_period_seen[channel_index], period);
        max_period_seen[channel_index] = std::max(max_period_seen[channel_index], period);
    }
    ++update_count[channel_index];
}

}  // namespace

void Psg::reset() noexcept {
    for (auto& channel : tone_channels_) {
        channel.period = 0x0001;
        channel.counter = 0;
        channel.attenuation = 0x0F;
        channel.output_high = true;
    }

    noise_channel_.control = 0x00;
    noise_channel_.attenuation = 0x0F;
    noise_channel_.lfsr = 0x8000;
    noise_channel_.counter = 0;
    noise_channel_.output_high = true;
    noise_channel_.noise_out = false;
    tick_debt_ = 0;
    output_accumulator_ = 0.0;
    accumulated_ticks_ = 0;

    sample_buffer_.clear();
    last_value_ = 0x00;
    latched_channel_ = 0x00;
    latched_volume_register_ = false;
    latched_noise_register_ = false;
    sample_phase_accumulator_ = 0.0;
    write_count_ = 0;
    latch_write_count_ = 0;
    data_write_count_ = 0;
    generated_sample_count_ = 0;
    nonzero_sample_count_ = 0;
    tone_latch_count_.fill(0);
    tone_data_count_.fill(0);
    volume_latch_count_.fill(0);
    volume_data_count_.fill(0);
    noise_latch_count_ = 0;
    noise_data_count_ = 0;
    min_period_seen_.fill(0);
    max_period_seen_.fill(0);
    tone_period_update_count_.fill(0);
    min_sample_ = 0;
    max_sample_ = 0;
    last_raw_mix_ = 0.0;
    last_filtered_mix_ = 0.0;
}

void Psg::write(std::uint8_t value) noexcept {
    last_value_ = value;
    ++write_count_;
    if (write_logging_) {
        write_log_.push_back({generated_sample_count_, value});
    }

    if ((value & 0x80) != 0) {
        ++latch_write_count_;
        const auto channel = static_cast<std::uint8_t>((value >> 5) & 0x03);
        const bool is_volume = (value & 0x10) != 0;
        latched_channel_ = channel;
        latched_volume_register_ = is_volume;
        latched_noise_register_ = (channel == 0x03) && !is_volume;

        if (is_volume) {
            ++volume_latch_count_[channel];
            if (channel == 0x03) {
                noise_channel_.attenuation = static_cast<std::uint8_t>(value & 0x0F);
            } else {
                tone_channels_[channel].attenuation = static_cast<std::uint8_t>(value & 0x0F);
            }
            return;
        }

        if (channel == 0x03) {
            ++noise_latch_count_;
            noise_channel_.control = static_cast<std::uint8_t>(value & 0x07);
            noise_channel_.lfsr = 0x8000;
            return;
        }

        ++tone_latch_count_[channel];
        auto& tone = tone_channels_[channel];
        // Stored exactly as written. A period of zero behaves as one on this
        // chip, but that belongs where the period is USED, not here: a game
        // sets the low nibble and the high bits in two writes, and clamping the
        // half-written value to 1 leaves that 1 sitting in the low nibble for
        // the second write to inherit. Writing 0x100 came out as 0x101.
        tone.period = static_cast<std::uint16_t>((tone.period & 0x03F0) | (value & 0x0F));
        update_period_observation(min_period_seen_, max_period_seen_, tone_period_update_count_, channel, tone.period);
        return;
    }

    ++data_write_count_;

    if (latched_volume_register_) {
        if (latched_channel_ == 0x03) {
            ++volume_data_count_[3];
            noise_channel_.attenuation = static_cast<std::uint8_t>(value & 0x0F);
        } else if (latched_channel_ < kToneChannelCount) {
            ++volume_data_count_[latched_channel_];
            tone_channels_[latched_channel_].attenuation = static_cast<std::uint8_t>(value & 0x0F);
        }
        return;
    }

    if (latched_noise_register_) {
        // Counted but otherwise ignored, which is what BlastEm does: the noise
        // register is written whole in the latch byte and has no second half.
        // Treating a following data byte as another noise write reset the shift
        // register again, and a shift register that keeps restarting stops
        // sounding like noise and starts sounding like a rattle.
        ++noise_data_count_;
        return;
    }

    if (latched_channel_ < kToneChannelCount) {
        ++tone_data_count_[latched_channel_];
        auto& tone = tone_channels_[latched_channel_];
        tone.period = static_cast<std::uint16_t>(((value & 0x3F) << 4) | (tone.period & 0x000F));
        update_period_observation(
            min_period_seen_,
            max_period_seen_,
            tone_period_update_count_,
            latched_channel_,
            tone.period);
    }
}

void Psg::advance_cycles(std::uint32_t cpu_cycles) noexcept {
    mix_samples_for_cycles(cpu_cycles);
}

const std::vector<std::int16_t>& Psg::sample_buffer() const noexcept {
    return sample_buffer_;
}

void Psg::clear_sample_buffer() noexcept {
    sample_buffer_.clear();
}

std::uint64_t Psg::write_count() const noexcept {
    return write_count_;
}

std::array<const char*, 4> Psg::channels() const noexcept {
    return {"Tone 1", "Tone 2", "Tone 3", "Noise"};
}

std::string Psg::describe() const {
    std::ostringstream out;
    out << "PSG: 3 tone + 1 noise channel, base clock " << static_cast<long long>(kBaseClockHz)
        << " Hz, sample rate " << kDefaultSampleRate
        << " Hz, stereo routing via system-control port 06, buffered samples "
        << sample_buffer_.size()
        << ", last write " << static_cast<int>(last_value_);
    return out.str();
}

std::string Psg::activity_summary() const {
    std::ostringstream out;
    out << "PSG activity: writes=" << write_count_
        << " (latch=" << latch_write_count_
        << ", data=" << data_write_count_ << ")"
        << ", generated samples=" << generated_sample_count_
        << ", buffered samples=" << sample_buffer_.size()
        << ", nonzero samples=" << nonzero_sample_count_
        << ", min/max sample=" << min_sample_ << "/" << max_sample_
        << ", tone periods=[" << tone_channels_[0].period
        << "," << tone_channels_[1].period
        << "," << tone_channels_[2].period
        << "], attenuations=[" << static_cast<int>(tone_channels_[0].attenuation)
        << "," << static_cast<int>(tone_channels_[1].attenuation)
        << "," << static_cast<int>(tone_channels_[2].attenuation)
        << "," << static_cast<int>(noise_channel_.attenuation)
        << "], noise control=" << static_cast<int>(noise_channel_.control)
        << ", tone write counts[latch/data]=[" << tone_latch_count_[0] << "/" << tone_data_count_[0]
        << "," << tone_latch_count_[1] << "/" << tone_data_count_[1]
        << "," << tone_latch_count_[2] << "/" << tone_data_count_[2]
        << "], volume write counts[latch/data]=[" << volume_latch_count_[0] << "/" << volume_data_count_[0]
        << "," << volume_latch_count_[1] << "/" << volume_data_count_[1]
        << "," << volume_latch_count_[2] << "/" << volume_data_count_[2]
        << "," << volume_latch_count_[3] << "/" << volume_data_count_[3]
        << "], noise writes[latch/data]=" << noise_latch_count_ << "/" << noise_data_count_
        << ", tone period range=[";
    for (std::size_t channel_index = 0; channel_index < kToneChannelCount; ++channel_index) {
        if (channel_index != 0) {
            out << ",";
        }
        if (tone_period_update_count_[channel_index] == 0) {
            out << "-";
        } else {
            out << min_period_seen_[channel_index] << "-" << max_period_seen_[channel_index];
        }
    }
    out << "], tone period updates=[" << tone_period_update_count_[0]
        << "," << tone_period_update_count_[1]
        << "," << tone_period_update_count_[2]
        << "]"
        << ", latched channel=" << static_cast<int>(latched_channel_)
        << (latched_noise_register_ ? " (noise)" : "");
    return out.str();
}

// Two decibels per step, and the last step is off. Precomputed: this is asked
// four times per counter tick, which is a few hundred thousand times a second,
// and std::pow in that path is pure waste.
double Psg::attenuation_gain(std::uint8_t attenuation) const noexcept {
    static const std::array<double, 16> table = [] {
        std::array<double, 16> made{};
        for (int step = 0; step < 15; ++step) {
            made[static_cast<std::size_t>(step)] = std::pow(10.0, -2.0 * step / 20.0);
        }
        made[15] = 0.0;
        return made;
    }();
    return table[attenuation & 0x0F];
}

double Psg::tone_frequency_hz(std::size_t channel_index) const noexcept {
    const auto period = std::max<std::uint16_t>(1, tone_channels_[channel_index].period);
    return kBaseClockHz / (32.0 * static_cast<double>(period));
}

double Psg::noise_shift_rate_hz() const noexcept { return noise_frequency_hz(); }

// All four settings load the SAME counter from the same clock; only the reload
// value differs. Settings 0-2 load 0x10, 0x20 and 0x40, and setting 3 loads
// tone channel 2's value unchanged - which is why the rate is always
// clock / (32 * load), the identical expression tone_frequency_hz() uses.
//
// This used to divide the tone-2 case by a further sixteen, which put swept
// noise four octaves below where it belongs. Music never showed it - Sonic 3's
// percussion is sampled through the DAC, not the noise channel - but every
// sound effect built on swept noise came out slurred: the lightning shield
// sounded stretched and explosions lost their crack.
double Psg::noise_frequency_hz() const noexcept {
    switch (noise_channel_.control & 0x03) {
        case 0x00:
            return kBaseClockHz / 512.0;
        case 0x01:
            return kBaseClockHz / 1024.0;
        case 0x02:
            return kBaseClockHz / 2048.0;
        case 0x03:
        default:
            return tone_frequency_hz(2);
    }
}

std::uint16_t Psg::noise_counter_load() const noexcept {
    const auto rate = static_cast<std::uint8_t>(noise_channel_.control & 0x03);
    if (rate == 0x03) {
        return tone_channels_[2].period;
    }
    return static_cast<std::uint16_t>(0x10u << rate);
}

// One counter tick, which is sixteen PSG clocks. Straight from BlastEm's
// psg_run: decrement if non-zero, and on reaching zero reload and flip the
// output. A reload value of zero therefore flips every tick, which is the
// hardware behaviour for a period of zero and the reason the period register
// must not be clamped when it is written.
void Psg::tick_counters() noexcept {
    for (auto& tone : tone_channels_) {
        if (tone.counter != 0) {
            --tone.counter;
        }
        if (tone.counter == 0) {
            tone.counter = tone.period;
            tone.output_high = !tone.output_high;
        }
    }

    auto& noise = noise_channel_;
    if (noise.counter != 0) {
        --noise.counter;
    }
    if (noise.counter == 0) {
        noise.counter = noise_counter_load();
        noise.output_high = !noise.output_high;
        // The shift register advances on the rising edge only - once per two
        // counter periods - which is where the 32 in clock/(32*period) comes
        // from.
        if (noise.output_high) {
            noise.noise_out = (noise.lfsr & 0x0001) != 0;
            noise.lfsr = static_cast<std::uint16_t>((noise.lfsr >> 1) | (noise.lfsr << 15));
            // White noise taps bits 0 and 3 of the pre-rotate value; after the
            // rotate that is bit 2, and the tap flips the bit just rotated in.
            // Periodic noise is the bare rotation, giving a 1-in-16 pulse.
            if ((noise.control & 0x04) != 0 && (noise.lfsr & 0x0004) != 0) {
                noise.lfsr ^= 0x8000;
            }
        }
    }
}

// All four channels carry equal weight on hardware. The noise channel used to
// be mixed a fifth quieter than the tones for no reason anyone recorded.
double Psg::current_output() const noexcept {
    double sum = 0.0;
    for (const auto& tone : tone_channels_) {
        if (tone.output_high) {
            sum += attenuation_gain(tone.attenuation);
        }
    }
    if (noise_channel_.noise_out) {
        sum += attenuation_gain(noise_channel_.attenuation);
    }
    return sum * 0.25;
}

void Psg::mix_samples_for_cycles(std::uint32_t cpu_cycles) noexcept {
    // How much of an output sample one counter tick is worth. About a fifth:
    // the counters run near 224kHz and the output is 44.1kHz, so roughly five
    // ticks are averaged into every sample.
    constexpr double kSamplesPerTick =
        static_cast<double>(kDefaultSampleRate) * kCounterClockDivider / kBaseClockHz;

    tick_debt_ += cpu_cycles;
    while (tick_debt_ >= kCounterClockDivider) {
        tick_debt_ -= kCounterClockDivider;

        tick_counters();
        output_accumulator_ += current_output();
        ++accumulated_ticks_;

        sample_phase_accumulator_ += kSamplesPerTick;
        if (sample_phase_accumulator_ < 1.0) {
            continue;
        }
        sample_phase_accumulator_ -= 1.0;

        // The average across the ticks this sample spans, not the value at the
        // instant it ends. That average is the box filter that keeps the
        // chip's ultrasonic content from folding down into the audible band.
        const auto mixed = output_accumulator_ / static_cast<double>(accumulated_ticks_);
        output_accumulator_ = 0.0;
        accumulated_ticks_ = 0;

        const auto filtered = mixed - last_raw_mix_ + (kHighPassPole * last_filtered_mix_);
        last_raw_mix_ = mixed;
        last_filtered_mix_ = filtered;

        const auto sample = static_cast<std::int16_t>(clamp_unit(filtered) * 32767.0);
        sample_buffer_.push_back(sample);
        ++generated_sample_count_;
        if (sample != 0) {
            ++nonzero_sample_count_;
        }
        if (generated_sample_count_ == 1) {
            min_sample_ = sample;
            max_sample_ = sample;
        } else {
            min_sample_ = std::min(min_sample_, sample);
            max_sample_ = std::max(max_sample_, sample);
        }
    }
}

}  // namespace mercury::gamegear
