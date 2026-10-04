#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mercury::genesis {

// Yamaha YM2612 (OPN2). Six four-operator FM channels plus a DAC that steals
// channel six.
//
// Two things here are load-bearing beyond the sound itself:
//   * The timers. Boot code and sound drivers poll the status register waiting
//     for timer A or B to overflow, and a part that never sets those bits hangs
//     the game before a single note is played.
//   * The busy flag. Writes take time and drivers wait on it.
class Ym2612 {
public:
    static constexpr double kClockHz = 7'670'453.0;  // 53.693175MHz / 7
    // The chip divides its clock by 144 to make one sample of all six channels.
    static constexpr double kSampleRateHz = kClockHz / 144.0;
    static constexpr std::size_t kChannelCount = 6;
    static constexpr std::size_t kOperatorCount = 4;

    void reset() noexcept;

    // Address/data pairs. Port 0 covers channels 1-3, port 1 channels 4-6.
    void write(std::uint8_t port, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t read_status() const noexcept;

    void advance_cycles(std::uint32_t m68k_cycles) noexcept;

    [[nodiscard]] const std::vector<std::int16_t>& sample_buffer() const noexcept { return samples_; }
    void clear_sample_buffer() noexcept { samples_.clear(); }

    [[nodiscard]] std::string describe() const;

private:
    struct Operator {
        // Phase generator.
        std::uint32_t phase = 0;
        std::uint32_t increment = 0;
        std::uint8_t detune = 0;
        std::uint8_t multiple = 0;
        // Envelope generator.
        std::uint8_t total_level = 0x7F;
        std::uint8_t key_scale = 0;
        std::uint8_t attack_rate = 0;
        std::uint8_t decay_rate = 0;
        std::uint8_t sustain_rate = 0;
        std::uint8_t release_rate = 0;
        std::uint8_t sustain_level = 0;
        // SSG-EG, register 0x90. Stored as zero when the enable bit is clear, so
        // a non-zero value means "active" without re-testing the bit everywhere.
        // Bit 3 enable, bit 2 invert, bit 1 alternate, bit 0 hold.
        std::uint8_t ssg_eg = 0;
        // Whether the envelope is currently being read upside down. Separate
        // from the invert BIT: alternate mode flips this every time the envelope
        // reaches the halfway point, and it survives until the mode changes.
        bool ssg_inverted = false;
        bool amplitude_modulation = false;
        // Runtime envelope state. 0 is loudest, 1023 silent, matching the
        // chip's attenuation-domain envelope.
        enum class Phase : std::uint8_t { attack, decay, sustain, release } phase_state = Phase::release;
        std::int32_t envelope = 1023;
        bool key_on = false;
        std::int32_t output = 0;
        std::int32_t previous_output = 0;
    };

    struct Channel {
        std::array<Operator, kOperatorCount> operators{};
        std::uint16_t frequency_number = 0;
        std::uint8_t block = 0;
        std::uint8_t algorithm = 0;
        std::uint8_t feedback = 0;
        bool left = true;
        bool right = true;
        std::uint8_t am_sensitivity = 0;
        std::uint8_t fm_sensitivity = 0;
        std::int32_t feedback_memory[2] = {0, 0};
        // Channel three can run each operator at its own frequency.
        std::array<std::uint16_t, 3> extra_frequency{};
        std::array<std::uint8_t, 3> extra_block{};
    };

    void write_register(std::uint8_t bank, std::uint8_t address, std::uint8_t value) noexcept;
    void key_on_off(std::uint8_t value) noexcept;
    void update_phase_increment(Channel& channel, std::size_t op_index) noexcept;
  public:
    // Mixing-only mutes, for working out which part of the chip a sound is
    // coming from. They must NOT change emulation: the channels still run, the
    // envelopes still step, the samples are simply not added to the mix. Host
    // state, so deliberately absent from visit_state.
    void set_fm_muted(bool muted) noexcept { fm_muted_ = muted; }
    void set_dac_muted(bool muted) noexcept { dac_muted_ = muted; }
    [[nodiscard]] bool fm_muted() const noexcept { return fm_muted_; }
    [[nodiscard]] bool dac_muted() const noexcept { return dac_muted_; }

    // How many times the sound driver has pushed a byte into the DAC. Divided
    // by elapsed time this is the playback rate of every sampled sound in the
    // game, which is the one thing that can make a SAMPLE play stretched.
    [[nodiscard]] std::uint64_t dac_write_count() const noexcept { return dac_write_count_; }
    [[nodiscard]] std::uint64_t key_on_count() const noexcept { return key_on_count_; }
    // How many times a game has switched channel 3 into CSM mode, and how many
    // times it has enabled the LFO. Both are FM features that change how a
    // sound evolves over time, so both are worth knowing about before chasing
    // one that is never used.
    [[nodiscard]] std::uint64_t csm_enable_count() const noexcept { return csm_enable_count_; }
    [[nodiscard]] std::uint64_t lfo_enable_count() const noexcept { return lfo_enable_count_; }
    [[nodiscard]] std::uint64_t ssg_enable_count() const noexcept { return ssg_enable_count_; }

    // Every register the sound driver writes, with the chip sample it landed
    // on. This is what turns "the sound is wrong" into "the driver asked for X
    // and we did Y" - without it there is no way to tell a chip bug from the
    // driver never being told to do the thing in the first place.
    struct WriteLogEntry {
        std::uint64_t sample;
        std::uint8_t bank;
        std::uint8_t address;
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
    [[nodiscard]] std::uint64_t sample_index() const noexcept { return sample_count_; }

  private:
    bool fm_muted_ = false;
    bool dac_muted_ = false;
    std::uint64_t dac_write_count_ = 0;
    std::uint64_t key_on_count_ = 0;
    std::uint64_t csm_enable_count_ = 0;
    std::uint64_t lfo_enable_count_ = 0;
    std::uint64_t ssg_enable_count_ = 0;
    bool write_logging_ = false;
    std::vector<WriteLogEntry> write_log_;

    // The envelope's halfway point, where SSG-EG loops, inverts or holds. The
    // chip's envelope is ten bits here, so this is 0x200 - BlastEm's 0x800 in
    // its twelve-bit representation.
    static constexpr std::int32_t kSsgCenter = 512;
    static constexpr std::int32_t kMaxEnvelope = 1023;

    void advance_envelope(Operator& op, std::uint8_t key_code) noexcept;
    // Restart an operator's envelope from the top without touching its phase.
    void start_envelope(Operator& op) noexcept;
    // The SSG-EG loop/invert/hold check, run once per operator per sample.
    void run_ssg_eg(Operator& op) noexcept;
    [[nodiscard]] std::int32_t ssg_scaled_step(const Operator& op, std::int32_t step) const noexcept;
    void clamp_envelope(Operator& op) noexcept;
    // The envelope as the output stage sees it: upside down while SSG-EG has
    // the operator inverted. The stored envelope keeps counting up either way.
    [[nodiscard]] std::int32_t effective_envelope(const Operator& op) const noexcept;
    [[nodiscard]] std::int32_t operator_output(Operator& op, std::int32_t modulation) const noexcept;
    void generate_sample() noexcept;

  public:
    // samples_ is the output buffer the host drains each frame, not chip state,
    // and the counters are diagnostics; neither belongs in a save state.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(channels_);
        ar(latched_address_);
        ar(register_file_);
        ar(timer_a_period_);
        ar(timer_a_counter_);
        ar(timer_b_period_);
        ar(timer_b_counter_);
        ar(timer_b_prescale_);
        ar(timer_a_enabled_);
        ar(timer_b_enabled_);
        ar(timer_a_overflow_);
        ar(timer_b_overflow_);
        ar(timer_a_flag_enabled_);
        ar(timer_b_flag_enabled_);
        ar(ch3_mode_);
        ar(lfo_);
        ar(lfo_counter_);
        ar(lfo_step_);
        ar(dac_enabled_);
        ar(dac_value_);
        ar(cycle_accumulator_);
        ar(envelope_divider_);
        ar(envelope_counter_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else (netplay, a shared file).
    // Anything the hardware could never hold is brought back into range here,
    // so it cannot become an index past a table or a loop that never ends.
    void sanitize_loaded() noexcept;

  private:
    std::array<Channel, kChannelCount> channels_{};
    std::array<std::uint8_t, 2> latched_address_{};
    std::array<std::array<std::uint8_t, 256>, 2> register_file_{};

    // Timers. Counts are in chip sample ticks; A ticks every sample, B every
    // sixteenth.
    std::uint16_t timer_a_period_ = 0;
    std::uint16_t timer_a_counter_ = 0;
    std::uint8_t timer_b_period_ = 0;
    std::uint16_t timer_b_counter_ = 0;
    std::uint8_t timer_b_prescale_ = 0;
    bool timer_a_enabled_ = false;
    bool timer_b_enabled_ = false;
    bool timer_a_overflow_ = false;
    bool timer_b_overflow_ = false;
    bool timer_a_flag_enabled_ = false;
    bool timer_b_flag_enabled_ = false;

    std::uint8_t ch3_mode_ = 0;
    std::uint8_t lfo_ = 0;
    std::uint32_t lfo_counter_ = 0;
    std::uint8_t lfo_step_ = 0;

    bool dac_enabled_ = false;
    std::int16_t dac_value_ = 0;

    // Fractional accumulator: the chip runs at its own rate, not the CPU's.
    double cycle_accumulator_ = 0.0;
    std::uint32_t envelope_divider_ = 0;
    std::uint32_t envelope_counter_ = 0;

    std::vector<std::int16_t> samples_{};
    std::uint64_t sample_count_ = 0;
    std::uint64_t write_count_ = 0;
};

}  // namespace mercury::genesis
