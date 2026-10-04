#include "mercury/thirtytwox/pwm.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mercury::thirtytwox {

void Pwm::reset() noexcept {
    control_ = 0;
    cycle_ = 0;
    depth_.fill(0);
    for (auto& channel : fifo_) {
        channel.fill(0);
    }
    held_.fill(0);
    primed_.fill(false);
    fraction_ = 0;
    countdown_ = 1;
    interrupts_ = 0;
    samples_ = 0;
    output_phase_ = 0;
    sample_buffer_.clear();
    dc_previous_input_ = 0.0;
    dc_state_ = 0.0;
    peak_ = 0;
    emitted_ = 0;
    clipped_ = 0;
    square_sum_ = 0.0;
    lowest_width_ = 0xFFFF;
    highest_width_ = 0;
    width_seen_.fill(0);
    distinct_widths_ = 0;
}

std::uint16_t Pwm::read16(std::uint32_t offset) const noexcept {
    switch (offset & 0x0E) {
        case kControl: return control_;
        case kCycle: return cycle_;
        case kLeft:
            // The channel registers read back as FIFO status, not as the
            // sample last written.
            if (depth_[0] >= kFifoDepth) return kFull;
            return depth_[0] == 0 ? kEmpty : 0;
        case kRight:
        case kMono:
            if (depth_[1] >= kFifoDepth) return kFull;
            return depth_[1] == 0 ? kEmpty : 0;
        default: return 0;
    }
}

void Pwm::write16(std::uint32_t offset, std::uint16_t value) noexcept {
    switch (offset & 0x0E) {
        case kControl:
            control_ = value;
            // A new interrupt period takes effect from the next cycle rather
            // than shortening the one already in flight.
            if (countdown_ <= 0) {
                countdown_ = timer_reload();
            }
            return;
        case kCycle:
            cycle_ = static_cast<std::uint16_t>(value & 0x0FFF);
            return;
        case kLeft:
            push(0, value);
            return;
        case kRight:
            push(1, value);
            return;
        case kMono:
            // One write, both channels: that is what mono means here.
            push(0, value);
            push(1, value);
            return;
        default:
            return;
    }
}

void Pwm::sanitize_loaded() noexcept {
    for (auto& depth : depth_) {
        depth = std::clamp(depth, 0, kFifoDepth);
    }
    // Both are remainders between calls; huge, they drive the sample loop for
    // ever.
    output_phase_ %= kClockHz;
    fraction_ %= period();
    if (countdown_ < 1 || countdown_ > 16) {
        countdown_ = timer_reload();
    }
    if (!std::isfinite(dc_previous_input_)) dc_previous_input_ = 0.0;
    if (!std::isfinite(dc_state_)) dc_state_ = 0.0;
}

// A full FIFO DROPS the write rather than overwriting the queue. Software is
// expected to check the full flag first, and a game that does not is losing
// that sample on hardware too.
void Pwm::push(int channel, std::uint16_t value) noexcept {
    auto& depth = depth_[channel];
    // Both ends: the bottom too, so no depth can ever address before the FIFO.
    if (depth >= kFifoDepth || depth < 0) {
        return;
    }
    const auto width = static_cast<std::uint16_t>(value & 0x0FFF);
    fifo_[channel][static_cast<std::size_t>(depth)] = width;
    ++depth;
    if (width < lowest_width_) lowest_width_ = width;
    if (width > highest_width_) highest_width_ = width;
    auto& word = width_seen_[width >> 6];
    const std::uint64_t bit = 1ull << (width & 63);
    if ((word & bit) == 0) {
        word |= bit;
        ++distinct_widths_;
    }
}

std::int16_t Pwm::level_of(std::uint16_t value) const noexcept {
    const std::int32_t span = static_cast<std::int32_t>(period());
    if (span <= 1) {
        return 0;
    }
    // Clamp rather than wrap: a width longer than the period is the pulse
    // simply never going low, which is full deflection and not a fold-over.
    const std::int32_t width = std::min<std::int32_t>(value, span);
    // (width/span - 1/2) * 2 * 32767, done in integers.
    const std::int64_t scaled =
        (static_cast<std::int64_t>(2 * width - span) * 32767) / span;
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(scaled, -32768, 32767));
}

void Pwm::emit_sample() noexcept {
    std::int32_t sum = 0;
    int audible = 0;
    for (int channel = 0; channel < 2; ++channel) {
        if (!channel_audible(channel) || !primed_[channel]) {
            continue;
        }
        sum += level_of(held_[channel]);
        ++audible;
    }
    // Mono out, so the two channels average rather than add - two identical
    // channels must not be twice as loud as one.
    const std::int32_t raw = audible == 0 ? 0 : sum / audible;

    // BLOCK THE DC, the way the socket does.
    //
    // A pulse width is an absolute level, not a signed sample, so where a game
    // puts its silence is the game's own business - and the library does not
    // agree. After Burner writes widths across 0-1236 of a 1475 period, which
    // straddles the halfway mark; Star Wars Arcade writes 0-508 of 1047, which
    // sits ENTIRELY BELOW IT. Taking half the period as silence would leave
    // Star Wars with a permanent offset of about half full scale, which is a
    // click at the start, a quiet signal riding on a rail, and clipping on
    // every peak.
    //
    // The hardware does not care either, because the output is AC coupled: the
    // capacitor between the adapter and the socket passes the music and throws
    // the offset away. This is that capacitor. One pole, cutoff a few hertz -
    // low enough to leave the bass alone, high enough to track a game that
    // changes where its silence sits between one tune and the next.
    const double filtered = static_cast<double>(raw) - dc_previous_input_ + kDcPole * dc_state_;
    dc_previous_input_ = static_cast<double>(raw);
    dc_state_ = filtered;
    const std::int16_t sample = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(static_cast<std::int32_t>(filtered), -32768, 32767));
    sample_buffer_.push_back(sample);
    const std::int32_t magnitude = sample < 0 ? -static_cast<std::int32_t>(sample) : sample;
    if (magnitude > peak_) {
        peak_ = static_cast<std::int16_t>(magnitude);
    }
    ++emitted_;
    if (magnitude >= 32700) {
        ++clipped_;
    }
    square_sum_ += static_cast<double>(magnitude) * static_cast<double>(magnitude);
}

double Pwm::rms() const noexcept {
    if (emitted_ == 0) {
        return 0.0;
    }
    return std::sqrt(square_sum_ / static_cast<double>(emitted_));
}

bool Pwm::tick_due(std::uint32_t cycles) noexcept {
    // THE QUEUE KEEPS DRAINING WITH THE OUTPUTS OFF.
    //
    // The obvious reading is that silencing both channels stops the hardware
    // clocking samples out, and that is what this used to do. It is wrong,
    // and PicoDrive says so from the other side: its note is that the Mars
    // Check Program disagrees with that reading. The cost of getting it wrong
    // is not silence, it is a hang - Virtua Racing Deluxe polled the left
    // channel's queue fourteen million times waiting for a space that could
    // never open, because nothing was taking samples out of it.
    //
    // What the output mode bits change is whether anything is audible, which
    // is not this function's business.
    // Called for nearly every SH-2 instruction with far less than a period
    // owed, so the divide is only paid once one has actually elapsed.
    fraction_ += cycles;
    const std::uint32_t step = period();
    std::uint32_t elapsed = 0;
    if (fraction_ >= step) {
        elapsed = fraction_ / step;
        fraction_ %= step;
    }

    // A long catch-up slice can cover many periods. Capping keeps the cost
    // bounded; a game cannot tell the difference between a great many missed
    // interrupts and a few, because it only ever services one at a time.
    const std::uint32_t counted = elapsed > 1024 ? 1024u : elapsed;
    bool due = false;
    for (std::uint32_t period_index = 0; period_index < counted; ++period_index) {
        // One sample leaves each FIFO per PWM period, and the value that
        // leaves is the one the output holds until the next one does.
        for (int channel = 0; channel < 2; ++channel) {
            auto& depth = depth_[channel];
            if (depth > 0) {
                held_[channel] = fifo_[channel][0];
                primed_[channel] = true;
                for (int at = 1; at < kFifoDepth; ++at) {
                    fifo_[channel][static_cast<std::size_t>(at - 1)] =
                        fifo_[channel][static_cast<std::size_t>(at)];
                }
                --depth;
                ++samples_;
            }
        }
        if (--countdown_ <= 0) {
            countdown_ = timer_reload();
            due = true;
            ++interrupts_;
        }
    }
    // Emit at the host rate. One output sample falls due each time the phase
    // passes a whole 32X second's worth of cycles. Driven by the raw cycle
    // count rather than by the period loop above, so the sound keeps real time
    // even across a catch-up slice that loop caps.
    output_phase_ += static_cast<std::uint64_t>(cycles) * kOutputRate;
    while (output_phase_ >= kClockHz) {
        output_phase_ -= kClockHz;
        emit_sample();
    }
    return due;
}

std::string Pwm::describe() const {
    std::ostringstream out;
    out << "32X PWM: ";
    if (!enabled()) {
        out << "outputs off";
    } else {
        out << "period " << period() << ", interrupt every " << timer_reload() << " periods";
    }
    out << " (control " << std::hex << control_ << std::dec << ", left mode " << (control_ & 3)
        << ", right mode " << ((control_ >> 2) & 3) << ")";
    out << ", " << interrupts_ << " interrupts, " << samples_ << " samples clocked out";
    out << "; widths " << (lowest_width_ == 0xFFFF ? 0 : lowest_width_) << "-" << highest_width_
        << " (" << distinct_widths_ << " distinct) into a period of " << period();
    // Name them when there are few enough to name. Three widths and a period
    // is the whole story for a game whose sound engine is idling.
    if (distinct_widths_ > 0 && distinct_widths_ <= 8) {
        out << " [";
        bool first = true;
        for (std::size_t word = 0; word < width_seen_.size(); ++word) {
            for (int bit = 0; bit < 64; ++bit) {
                if ((width_seen_[word] & (1ull << bit)) == 0) continue;
                if (!first) out << ' ';
                out << (word * 64 + static_cast<std::size_t>(bit));
                first = false;
            }
        }
        out << "]";
    }
    out << "; audio " << emitted_ << " samples, peak " << peak_ << ", rms " << rms() << ", "
        << clipped_ << " clipped";
    return out.str();
}

}  // namespace mercury::thirtytwox
