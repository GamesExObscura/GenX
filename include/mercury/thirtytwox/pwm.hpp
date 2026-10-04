#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mercury::thirtytwox {

// The 32X's pulse width modulation audio, at $A15130 from the Mega Drive and
// $4030 from the SH-2s.
//
// It arrived here for its INTERRUPT: a 32X game paces its audio work on the
// PWM interrupt, and two of the cartridges measured - Virtua Fighter and
// T-Mek - enable that interrupt on the slave processor and no other. With
// nothing driving it their slaves never run a handler at all, and both sides
// settle into polling a communication port the handler was supposed to write.
//
// IT NOW ALSO MAKES THE SOUND. The FIFOs used to count depth and throw the
// sample values away, which was enough for the flags and the interrupt and
// left the whole 32X silent. That is most of a 32X game's audio: Star Wars
// Arcade has no FM or PSG output AT ALL (both peaks measured zero over 1,200
// frames) - its music, its lasers and its speech are all PWM, so discarding
// the values meant the game ran in silence.
//
// The sample FIFOs are three deep per channel. Software checks the full and
// empty flags before pushing, so reporting them honestly matters.
class Pwm {
  public:
    // Offsets from the base of the register block, the same from either side.
    static constexpr std::uint32_t kControl = 0x00;
    static constexpr std::uint32_t kCycle = 0x02;
    static constexpr std::uint32_t kLeft = 0x04;
    static constexpr std::uint32_t kRight = 0x06;
    static constexpr std::uint32_t kMono = 0x08;

    static constexpr std::uint16_t kFull = 0x8000;
    static constexpr std::uint16_t kEmpty = 0x4000;
    // Bits 3-0 are the two output modes, bits 11-8 how many PWM cycles pass
    // between interrupts. Zero there means sixteen, not none.
    static constexpr std::uint16_t kOutputMask = 0x000F;
    static constexpr std::uint16_t kTimerMask = 0x0F00;

    static constexpr int kFifoDepth = 3;

    void reset() noexcept;

    [[nodiscard]] std::uint16_t read16(std::uint32_t offset) const noexcept;
    void write16(std::uint32_t offset, std::uint16_t value) noexcept;

    // Advance by that many 32X clock cycles. Returns true when a PWM
    // interrupt falls due in this span.
    //
    // Called after nearly every SH-2 instruction, and nearly every call is
    // too short for a period to end or an output sample to fall due - so
    // that case is answered here, inline, and only the rest goes to
    // tick_due. The two give identical results; this one just skips the
    // call.
    [[nodiscard]] bool tick(std::uint32_t cycles) noexcept {
        const std::uint32_t fraction = fraction_ + cycles;
        const std::uint64_t phase =
            output_phase_ + static_cast<std::uint64_t>(cycles) * kOutputRate;
        if (fraction < period() && phase < kClockHz) {
            fraction_ = fraction;
            output_phase_ = phase;
            return false;
        }
        return tick_due(cycles);
    }
    [[nodiscard]] bool tick_due(std::uint32_t cycles) noexcept;

    [[nodiscard]] std::uint64_t interrupts() const noexcept { return interrupts_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] bool enabled() const noexcept { return (control_ & kOutputMask) != 0; }

    // Mono output at the host rate, the same shape the PSG and the YM2612
    // hand over, so the runtime mixes all three the same way. Generated here
    // rather than at the PWM's own rate because that rate is whatever the
    // cycle register says and changes while a game runs - resampling a moving
    // rate downstream is far more work than emitting at a fixed one.
    static constexpr std::uint32_t kOutputRate = 44'100;
    // The 32X's own clock, which is what tick() is counting.
    static constexpr std::uint32_t kClockHz = 23'011'360;

    [[nodiscard]] const std::vector<std::int16_t>& sample_buffer() const noexcept {
        return sample_buffer_;
    }
    void clear_sample_buffer() noexcept { sample_buffer_.clear(); }

    [[nodiscard]] std::int16_t peak() const noexcept { return peak_; }
    // How the output actually sits, which the peak alone cannot say: one
    // extreme sample pins a peak at full scale whether the rest of the run is
    // a healthy signal or a saturated mess.
    [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }
    [[nodiscard]] std::uint64_t clipped() const noexcept { return clipped_; }
    [[nodiscard]] double rms() const noexcept;
    // The widths software is writing, against the period they are written
    // into. "Is a width of zero this game's silence or its full deflection?"
    // is not answerable from the register map alone.
    [[nodiscard]] std::uint16_t lowest_width() const noexcept { return lowest_width_; }
    [[nodiscard]] std::uint16_t highest_width() const noexcept { return highest_width_; }
    // How many DIFFERENT widths a channel has been given. One means a sound
    // engine that is running - filling the queue every period - and feeding it
    // nothing but silence, which looks identical to a working one from every
    // counter except this.
    [[nodiscard]] std::size_t distinct_widths() const noexcept { return distinct_widths_; }
    [[nodiscard]] std::uint32_t last_period() const noexcept { return period(); }

    [[nodiscard]] std::string describe() const;

    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(control_);
        ar(cycle_);
        ar(depth_);
        ar(fraction_);
        ar(countdown_);
        // The queue's CONTENTS: depth_ alone restored the count and left the
        // words from before the load behind it. The rest is the output side -
        // when the next host sample falls due and where the DC filter sits -
        // so a load does not click.
        ar(fifo_);
        ar(held_);
        ar(primed_);
        ar(output_phase_);
        ar(dc_previous_input_);
        ar(dc_state_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else (netplay, a shared file).
    // depth_ above all: push() writes the next sample at fifo_[depth], and
    // only checks the TOP of the range - a negative depth from a crafted file
    // turned an ordinary sound write into a write anywhere before the FIFO.
    void sanitize_loaded() noexcept;

  private:
    // How many cycles one PWM period lasts. The register holds the period
    // less one and wraps within its twelve bits, so a register nobody has
    // written yet means the longest period rather than the shortest. That
    // matters now that the queue drains with the outputs off: read as one,
    // an unwritten register would clock a sample every single cycle.
    [[nodiscard]] std::uint32_t period() const noexcept {
        return ((static_cast<std::uint32_t>(cycle_) - 1u) & 0x0FFFu) + 1u;
    }
    [[nodiscard]] int timer_reload() const noexcept {
        const int declared = (control_ & kTimerMask) >> 8;
        return declared == 0 ? 16 : declared;
    }

    // Is this channel's output wired to anything? The two bits are 00 for off,
    // 01 for this channel, 10 for the other one and 11 for off again. The mix
    // here is mono, so the crossed case is not distinguishable from the
    // straight one - what matters is that 00 and 11 are SILENT, which is how a
    // game mutes itself without stopping the FIFOs.
    [[nodiscard]] bool channel_audible(int channel) const noexcept {
        const int mode = (control_ >> (channel * 2)) & 3;
        return mode == 1 || mode == 2;
    }

    // Turn one pulse width into a level. The output is a pulse `value` cycles
    // wide out of `period`, so after the low-pass on the other side of the
    // socket the level is value/period - silence is HALF the period, not zero,
    // and the full-scale swing is the whole period whatever that period is.
    [[nodiscard]] std::int16_t level_of(std::uint16_t value) const noexcept;

    // Append one output sample built from the current held values.
    void emit_sample() noexcept;

    // Queue one sample value on a channel, if there is room for it.
    void push(int channel, std::uint16_t value) noexcept;

    std::uint16_t control_ = 0;
    std::uint16_t cycle_ = 0;
    // One per channel: left, and right shared with mono.
    std::array<int, 2> depth_{};
    // The queued sample values, oldest first, and the one most recently
    // clocked out. The hardware holds its last width until a new one arrives,
    // so an empty FIFO is a held level rather than silence - going to zero
    // instead puts a click on every underrun.
    std::array<std::array<std::uint16_t, kFifoDepth>, 2> fifo_{};
    std::array<std::uint16_t, 2> held_{};
    // Has this channel ever clocked a sample out? A width of zero is not
    // silence, it is the pulse never going HIGH, which is full negative
    // deflection - so a channel switched on before anything has been queued
    // would sit at -32767 and put a loud DC step on the front of every game.
    // Until the first real sample, the channel is simply quiet.
    std::array<bool, 2> primed_{};
    std::uint32_t fraction_ = 0;
    int countdown_ = 1;
    std::uint64_t interrupts_ = 0;
    std::uint64_t samples_ = 0;
    // Output generation: phase counts cycles scaled by the output rate, so a
    // sample falls due every time it passes one 32X second's worth.
    std::uint64_t output_phase_ = 0;
    std::vector<std::int16_t> sample_buffer_{};
    std::int16_t peak_ = 0;
    // The output coupling capacitor. 0.9995 at 44.1kHz is a corner near 3.5Hz:
    // below anything anyone can hear, so it removes the offset and nothing
    // else. See emit_sample.
    static constexpr double kDcPole = 0.9995;
    double dc_previous_input_ = 0.0;
    double dc_state_ = 0.0;
    std::uint64_t emitted_ = 0;
    std::uint64_t clipped_ = 0;
    double square_sum_ = 0.0;
    std::uint16_t lowest_width_ = 0xFFFF;
    std::uint16_t highest_width_ = 0;
    // A bitmap of every 12-bit width ever written, and its population.
    std::array<std::uint64_t, 64> width_seen_{};
    std::size_t distinct_widths_ = 0;
};

}  // namespace mercury::thirtytwox
