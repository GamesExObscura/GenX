#include "mercury/genesis/ym2612.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mercury::genesis {

namespace {

// The chip works in the logarithmic domain: a sine table of attenuations and an
// exponential table to get back to linear. Building them once at startup keeps
// the per-sample path to table lookups and adds, which is also what the
// hardware does.
struct Tables {
    std::array<std::uint16_t, 256> sine{};   // attenuation, 4.8 fixed point
    std::array<std::uint16_t, 256> power{};  // 2^-x mantissa

    // One attenuation unit is 1/256th of a halving, because that is the step
    // the exponential table below resolves: its index is the fractional part
    // and the shift applied to its result is the integer part. Every other
    // quantity - the envelope, total level, the sine - has to be expressed in
    // those same units or the operator comes out at the wrong gain, which in
    // FM does not just change the volume, it changes the timbre.
    Tables() {
        for (std::size_t i = 0; i < 256; ++i) {
            const double value = std::sin((i + 0.5) * 3.14159265358979323846 / 2.0 / 256.0);
            const double attenuation = -std::log(value) / std::log(2.0) * 256.0;
            sine[i] = static_cast<std::uint16_t>(attenuation + 0.5);
        }
        for (std::size_t i = 0; i < 256; ++i) {
            power[i] = static_cast<std::uint16_t>(
                std::round((std::pow(2.0, (255.0 - static_cast<double>(i)) / 256.0) - 1.0) * 2048.0));
        }
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

// Detune offsets, indexed by [detune][key code >> 2]. The sign comes from the
// top bit of the detune field.
constexpr std::uint8_t kDetuneTable[4][32] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,4},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,4,4,4,4,4,6,6,6,8,8,8,10,10,12,12,14,16,16,18,20,22,24,26,28,32,32,32,32},
};

// Rate to attenuation-step table. The envelope does not advance by a fixed
// amount: it steps by a pattern that depends on the low two bits of the
// effective rate and on where the global counter is, and that dither is what
// produces the chip's characteristic curve. Rows 0-3 cover rates 2-47 (paired
// with a shift), rows 4-15 cover rates 48-59, row 16 rates 60-63, and row 18 is
// the "never moves" row for rates 0 and 1.
constexpr std::uint8_t kEnvelopeIncrement[19][8] = {
    {0,1,0,1,0,1,0,1},  /*  0: rates 2-47, low bits 0 */
    {0,1,0,1,1,1,0,1},  /*  1 */
    {0,1,1,1,0,1,1,1},  /*  2 */
    {0,1,1,1,1,1,1,1},  /*  3 */
    {1,1,1,1,1,1,1,1},  /*  4: rate 48 */
    {1,1,1,2,1,1,1,2},  /*  5 */
    {1,2,1,2,1,2,1,2},  /*  6 */
    {1,2,2,2,1,2,2,2},  /*  7 */
    {2,2,2,2,2,2,2,2},  /*  8: rate 52 */
    {2,2,2,4,2,2,2,4},  /*  9 */
    {2,4,2,4,2,4,2,4},  /* 10 */
    {2,4,4,4,2,4,4,4},  /* 11 */
    {4,4,4,4,4,4,4,4},  /* 12: rate 56 */
    {4,4,4,8,4,4,4,8},  /* 13 */
    {4,8,4,8,4,8,4,8},  /* 14 */
    {4,8,8,8,4,8,8,8},  /* 15 */
    {8,8,8,8,8,8,8,8},  /* 16: rates 60-63 */
    {16,16,16,16,16,16,16,16},  /* 17: fastest attack */
    {0,0,0,0,0,0,0,0},  /* 18: rates 0-1 never move */
};

// Which row of the table above an effective rate uses.
//
// Rates 2-7 do NOT follow the rate & 3 pattern the rest of the range does. The
// chip collapses them onto two rows only: rates 6 and 7 use row 2, and
// everything else below 8 uses row 0. BlastEm builds this as
// rate_table_base[((rate & 6) == 6 ? 16 : 0) + cycle], where 16 is the start of
// its third row. Applying rate & 3 down here makes rates 3, 5 and 7 step faster
// than the hardware does.
constexpr std::uint8_t envelope_row(std::uint32_t rate) noexcept {
    if (rate < 2) return 18;
    if (rate < 8) return (rate & 0x06) == 0x06 ? 2 : 0;
    if (rate < 48) return static_cast<std::uint8_t>(rate & 0x03);
    if (rate < 60) return static_cast<std::uint8_t>(4 + (rate - 48));
    return 16;
}

// Sustain level is four bits of "how far down before switching to the sustain
// rate", in steps of 32 - except that 15 means all the way down, not 15/32nds.
constexpr std::int32_t sustain_threshold(std::uint8_t level) noexcept {
    return level == 15 ? 31 * 32 : static_cast<std::int32_t>(level) * 32;
}

constexpr std::uint8_t kKeyCodeTable[16] = {0,0,0,0,0,0,0,1,2,3,3,3,3,3,3,3};

std::int32_t clamp16(std::int32_t value) noexcept {
    return std::clamp(value, -32768, 32767);
}

}  // namespace

void Ym2612::reset() noexcept {
    channels_ = {};
    latched_address_.fill(0);
    for (auto& bank : register_file_) {
        bank.fill(0);
    }
    for (auto& channel : channels_) {
        for (auto& op : channel.operators) {
            op.total_level = 0x7F;
            op.envelope = 1023;
            op.phase_state = Operator::Phase::release;
        }
    }
    timer_a_period_ = 0;
    timer_a_counter_ = 0;
    timer_b_period_ = 0;
    timer_b_counter_ = 0;
    timer_b_prescale_ = 0;
    timer_a_enabled_ = false;
    timer_b_enabled_ = false;
    timer_a_overflow_ = false;
    timer_b_overflow_ = false;
    timer_a_flag_enabled_ = false;
    timer_b_flag_enabled_ = false;
    ch3_mode_ = 0;
    lfo_ = 0;
    lfo_counter_ = 0;
    lfo_step_ = 0;
    dac_enabled_ = false;
    dac_value_ = 0;
    cycle_accumulator_ = 0.0;
    envelope_divider_ = 0;
    envelope_counter_ = 0;
    samples_.clear();
    sample_count_ = 0;
    write_count_ = 0;
}

void Ym2612::sanitize_loaded() noexcept {
    // Between samples the accumulator is under one sample period (144 cycles).
    // A huge or NaN value makes advance_cycles produce samples for ever.
    if (!(cycle_accumulator_ >= 0.0 && cycle_accumulator_ < 144.0)) {
        cycle_accumulator_ = 0.0;
    }
    envelope_divider_ %= 3;
}

std::uint8_t Ym2612::read_status() const noexcept {
    // Bit 7 busy, bit 1 timer B overflow, bit 0 timer A overflow. The busy bit
    // is reported clear: writes complete instantly here, and a driver that
    // spins on it would otherwise never come out.
    std::uint8_t status = 0;
    if (timer_a_overflow_) status |= 0x01;
    if (timer_b_overflow_) status |= 0x02;
    return status;
}

void Ym2612::write(std::uint8_t port, std::uint8_t value) noexcept {
    ++write_count_;
    const std::uint8_t bank = (port >> 1) & 0x01;
    if ((port & 0x01) == 0) {
        latched_address_[bank] = value;
        return;
    }
    write_register(bank, latched_address_[bank], value);
}

void Ym2612::key_on_off(std::uint8_t value) noexcept {
    const std::uint8_t channel_select = value & 0x07;
    // 0-2 are channels 1-3, 4-6 are channels 4-6; 3 and 7 do not exist.
    if ((channel_select & 0x03) == 0x03) {
        return;
    }
    const std::size_t channel_index = (channel_select & 0x04)
        ? (3 + (channel_select & 0x03))
        : (channel_select & 0x03);
    auto& channel = channels_[channel_index];
    // Operator order in the key-on byte is 1, 3, 2, 4 - not 1, 2, 3, 4.
    static constexpr std::size_t kOrder[4] = {0, 2, 1, 3};
    for (std::size_t i = 0; i < 4; ++i) {
        const bool on = (value & (0x10u << i)) != 0;
        auto& op = channel.operators[kOrder[i]];
        if (on && !op.key_on) {
            ++key_on_count_;
            op.key_on = true;
            op.phase = 0;
            // Key-on starts the envelope the same way an SSG-EG loop restarts
            // it, so both go through one function.
            op.ssg_inverted = false;
            start_envelope(op);
        } else if (!on && op.key_on) {
            op.key_on = false;
            op.phase_state = Operator::Phase::release;
        }
    }
}

void Ym2612::update_phase_increment(Channel& channel, std::size_t op_index) noexcept {
    auto& op = channel.operators[op_index];
    std::uint16_t fnum = channel.frequency_number;
    std::uint8_t block = channel.block;
    // Channel three in special mode gives operators 1-3 their own frequency;
    // operator 4 keeps the channel's.
    if (&channel == &channels_[2] && ch3_mode_ != 0 && op_index < 3) {
        fnum = channel.extra_frequency[op_index];
        block = channel.extra_block[op_index];
    }

    std::uint32_t increment = (static_cast<std::uint32_t>(fnum) << block) >> 1;
    const std::uint8_t key_code = static_cast<std::uint8_t>((block << 2) | kKeyCodeTable[(fnum >> 7) & 0x0F]);

    const std::uint8_t detune_index = op.detune & 0x03;
    const std::int32_t detune = kDetuneTable[detune_index][key_code & 0x1F];
    if ((op.detune & 0x04) != 0) {
        increment -= static_cast<std::uint32_t>(detune);
    } else {
        increment += static_cast<std::uint32_t>(detune);
    }
    increment &= 0x1FFFF;

    // Multiple of zero means a half, not zero.
    increment = op.multiple == 0 ? (increment >> 1) : (increment * op.multiple);
    op.increment = increment & 0xFFFFF;
}

void Ym2612::write_register(std::uint8_t bank, std::uint8_t address, std::uint8_t value) noexcept {
    register_file_[bank][address] = value;

    if (write_logging_) {
        // Capped so a log left running does not eat memory without bound; the
        // interesting window is always the few seconds around a sound.
        constexpr std::size_t kMaxLoggedWrites = 4'000'000;
        if (write_log_.size() < kMaxLoggedWrites) {
            write_log_.push_back(WriteLogEntry{sample_count_, bank, address, value});
        }
    }

    if (bank == 0 && address < 0x30) {
        switch (address) {
            case 0x22:
                if ((value & 0x08) != 0 && (lfo_ & 0x08) == 0) {
                    ++lfo_enable_count_;
                }
                lfo_ = value;
                return;
            case 0x24: timer_a_period_ = static_cast<std::uint16_t>((timer_a_period_ & 0x03) | (value << 2)); return;
            case 0x25: timer_a_period_ = static_cast<std::uint16_t>((timer_a_period_ & 0x3FC) | (value & 0x03)); return;
            case 0x26: timer_b_period_ = value; return;
            case 0x27:
                // Mode 2 is CSM: timer A is supposed to key channel 3 on and
                // off by itself. Counted so it can be established whether any
                // given game actually uses it before anyone implements it.
                if (((value >> 6) & 0x03) == 0x02 && ch3_mode_ != 0x02) {
                    ++csm_enable_count_;
                }
                ch3_mode_ = static_cast<std::uint8_t>((value >> 6) & 0x03);
                timer_a_enabled_ = (value & 0x01) != 0;
                timer_b_enabled_ = (value & 0x02) != 0;
                timer_a_flag_enabled_ = (value & 0x04) != 0;
                timer_b_flag_enabled_ = (value & 0x08) != 0;
                if ((value & 0x10) != 0) timer_a_overflow_ = false;  // reset flag A
                if ((value & 0x20) != 0) timer_b_overflow_ = false;  // reset flag B
                return;
            case 0x28: key_on_off(value); return;
            case 0x2A:
                // DAC data is unsigned, centred at 0x80.
                ++dac_write_count_;
                dac_value_ = static_cast<std::int16_t>((static_cast<int>(value) - 0x80) * 128);
                return;
            case 0x2B: dac_enabled_ = (value & 0x80) != 0; return;
            default: return;
        }
    }

    const std::size_t slot = address & 0x03;
    if (slot == 0x03) {
        return;  // there is no fourth channel in a bank
    }
    const std::size_t channel_index = bank * 3 + slot;
    if (channel_index >= kChannelCount) {
        return;
    }
    auto& channel = channels_[channel_index];

    // Operator ordering in the register map is 1, 3, 2, 4 by address bits 3-2.
    static constexpr std::size_t kSlotToOperator[4] = {0, 2, 1, 3};
    const std::size_t op_index = kSlotToOperator[(address >> 2) & 0x03];

    if (address >= 0x30 && address <= 0x9F) {
        auto& op = channel.operators[op_index];
        switch (address & 0xF0) {
            case 0x30:
                op.detune = static_cast<std::uint8_t>((value >> 4) & 0x07);
                op.multiple = static_cast<std::uint8_t>(value & 0x0F);
                update_phase_increment(channel, op_index);
                return;
            case 0x40:
                op.total_level = static_cast<std::uint8_t>(value & 0x7F);
                return;
            case 0x50:
                op.key_scale = static_cast<std::uint8_t>((value >> 6) & 0x03);
                op.attack_rate = static_cast<std::uint8_t>(value & 0x1F);
                return;
            case 0x60:
                op.amplitude_modulation = (value & 0x80) != 0;
                op.decay_rate = static_cast<std::uint8_t>(value & 0x1F);
                return;
            case 0x70:
                op.sustain_rate = static_cast<std::uint8_t>(value & 0x1F);
                return;
            case 0x80:
                op.sustain_level = static_cast<std::uint8_t>((value >> 4) & 0x0F);
                op.release_rate = static_cast<std::uint8_t>(value & 0x0F);
                return;
            case 0x90: {
                // Stored as zero unless the enable bit is set, so every later
                // test is just "is it non-zero". Flipping the invert bit while
                // the mode is running also flips which way round the envelope
                // is currently being read.
                auto ssg = static_cast<std::uint8_t>(value & 0x0F);
                if ((ssg & 0x08) == 0) {
                    ssg = 0;
                } else if (op.ssg_eg == 0) {
                    ++ssg_enable_count_;
                }
                if (((ssg ^ op.ssg_eg) & 0x04) != 0) {
                    op.ssg_inverted = !op.ssg_inverted;
                }
                op.ssg_eg = ssg;
                return;
            }
            default:
                return;
        }
    }

    switch (address & 0xFC) {
        case 0xA0:
            channel.frequency_number = static_cast<std::uint16_t>((channel.frequency_number & 0x0700) | value);
            for (std::size_t i = 0; i < kOperatorCount; ++i) update_phase_increment(channel, i);
            return;
        case 0xA4:
            channel.frequency_number = static_cast<std::uint16_t>((channel.frequency_number & 0x00FF) |
                                                                  ((value & 0x07) << 8));
            channel.block = static_cast<std::uint8_t>((value >> 3) & 0x07);
            for (std::size_t i = 0; i < kOperatorCount; ++i) update_phase_increment(channel, i);
            return;
        case 0xA8: {
            // Channel three per-operator frequency, bank 0 only.
            if (bank != 0 || slot > 2) return;
            auto& ch3 = channels_[2];
            ch3.extra_frequency[slot] = static_cast<std::uint16_t>((ch3.extra_frequency[slot] & 0x0700) | value);
            for (std::size_t i = 0; i < kOperatorCount; ++i) update_phase_increment(ch3, i);
            return;
        }
        case 0xAC: {
            if (bank != 0 || slot > 2) return;
            auto& ch3 = channels_[2];
            ch3.extra_frequency[slot] = static_cast<std::uint16_t>((ch3.extra_frequency[slot] & 0x00FF) |
                                                                   ((value & 0x07) << 8));
            ch3.extra_block[slot] = static_cast<std::uint8_t>((value >> 3) & 0x07);
            for (std::size_t i = 0; i < kOperatorCount; ++i) update_phase_increment(ch3, i);
            return;
        }
        case 0xB0:
            channel.feedback = static_cast<std::uint8_t>((value >> 3) & 0x07);
            channel.algorithm = static_cast<std::uint8_t>(value & 0x07);
            return;
        case 0xB4:
            channel.left = (value & 0x80) != 0;
            channel.right = (value & 0x40) != 0;
            channel.am_sensitivity = static_cast<std::uint8_t>((value >> 4) & 0x03);
            channel.fm_sensitivity = static_cast<std::uint8_t>(value & 0x07);
            return;
        default:
            return;
    }
}

void Ym2612::advance_envelope(Operator& op, std::uint8_t key_code) noexcept {
    // Decay hands over to sustain BEFORE the rate is read, not after the
    // envelope has moved. The order matters enormously for the common patch
    // shape "decay rate 0, sustain level 0": the envelope is already at or past
    // the sustain level the instant the note starts, so it must switch to
    // sustain immediately and fall at the SUSTAIN rate.
    //
    // Deciding this after reading the rate meant a decay rate of zero took the
    // "rate zero never moves" early return first, and the operator stayed in
    // decay for ever - holding the note at full volume until the driver keyed
    // it off. Sonic 3's lightning shield is exactly this patch: attack 31,
    // decay 0, sustain level 0, sustain rates 13/13/14, keyed on for 714ms. It
    // came out as a flat 700ms tone instead of a fading one.
    if (op.phase_state == Operator::Phase::decay &&
        op.envelope >= sustain_threshold(op.sustain_level)) {
        op.phase_state = Operator::Phase::sustain;
    }

    // Effective rate folds in key scaling: higher notes decay faster.
    const std::uint8_t key_scale_shift = static_cast<std::uint8_t>(key_code >> (3 - op.key_scale));
    std::uint8_t base = 0;
    switch (op.phase_state) {
        case Operator::Phase::attack: base = op.attack_rate; break;
        case Operator::Phase::decay: base = op.decay_rate; break;
        case Operator::Phase::sustain: base = op.sustain_rate; break;
        case Operator::Phase::release: base = static_cast<std::uint8_t>((op.release_rate << 1) | 1); break;
    }
    if (base == 0) {
        return;  // rate zero never moves
    }
    const std::uint32_t rate =
        std::min<std::uint32_t>(63u, static_cast<std::uint32_t>(base) * 2 + key_scale_shift);

    // Below rate 48 the counter is divided down; at 48 and above it is used
    // directly. Writing the shift as zero for the fast rates makes the gate and
    // the selector below work for both halves without a special case.
    const std::uint32_t shift = rate < 48 ? (11 - (rate >> 2)) : 0;
    if ((envelope_counter_ & ((1u << shift) - 1)) != 0) {
        return;
    }
    const std::uint32_t select = (envelope_counter_ >> shift) & 0x07;
    const std::int32_t step = kEnvelopeIncrement[envelope_row(rate)][select];

    switch (op.phase_state) {
        case Operator::Phase::attack:
            // The attack is exponential towards zero attenuation, not linear.
            op.envelope += (~op.envelope * step) >> 4;
            if (op.envelope <= 0) {
                op.envelope = 0;
                op.phase_state = Operator::Phase::decay;
            }
            break;
        case Operator::Phase::decay:
            // No sustain check here: it happens at the top of the next tick,
            // which is where the hardware makes the decision.
            op.envelope += ssg_scaled_step(op, step);
            clamp_envelope(op);
            break;
        case Operator::Phase::sustain:
        case Operator::Phase::release:
            op.envelope += ssg_scaled_step(op, step);
            clamp_envelope(op);
            break;
    }
}

// While SSG-EG is running the envelope falls FOUR TIMES as fast, and stops dead
// once it passes the halfway point - the loop logic in run_ssg_eg takes over
// from there. Leaving this out is what made every SSG-EG sound last four times
// too long: a fast repeating envelope came out as one slow decay.
std::int32_t Ym2612::ssg_scaled_step(const Operator& op, std::int32_t step) const noexcept {
    if (op.ssg_eg == 0) {
        return step;
    }
    return op.envelope < kSsgCenter ? step * 4 : 0;
}

void Ym2612::clamp_envelope(Operator& op) noexcept {
    // In release, an SSG-EG operator is silenced as soon as it reaches the
    // halfway point rather than running all the way down.
    if (op.envelope > kMaxEnvelope ||
        (op.ssg_eg != 0 && op.phase_state == Operator::Phase::release &&
         op.envelope >= kSsgCenter)) {
        op.envelope = kMaxEnvelope;
    }
}

void Ym2612::start_envelope(Operator& op) noexcept {
    op.phase_state = Operator::Phase::attack;
    // A maximal attack rate means "already there" rather than an infinitely
    // fast ramp, which is why a note with AR=31 is instant.
    if (op.attack_rate >= 31) {
        op.envelope = 0;
        op.phase_state = Operator::Phase::decay;
    }
}

// Once per operator per sample. When the envelope passes the halfway point an
// SSG-EG operator either flips which way round it is read, restarts its phase,
// or holds - and unless it is holding, the envelope starts again from the top,
// which is what makes the mode a repeating envelope rather than a decay.
void Ym2612::run_ssg_eg(Operator& op) noexcept {
    if (op.ssg_eg == 0 || op.envelope < kSsgCenter) {
        return;
    }
    const bool alternate = (op.ssg_eg & 0x02) != 0;
    const bool hold = (op.ssg_eg & 0x01) != 0;
    const bool invert = (op.ssg_eg & 0x04) != 0;
    if (alternate) {
        // Holding stops the flipping only once it has settled the right way up.
        if (op.phase_state != Operator::Phase::release && (!hold || invert == op.ssg_inverted)) {
            op.ssg_inverted = !op.ssg_inverted;
        }
    } else if (!hold) {
        op.phase = 0;
    }
    if (!hold && (op.phase_state == Operator::Phase::decay ||
                  op.phase_state == Operator::Phase::sustain)) {
        start_envelope(op);
    }
}

std::int32_t Ym2612::effective_envelope(const Operator& op) const noexcept {
    if (op.ssg_eg == 0 || !op.ssg_inverted) {
        return op.envelope;
    }
    return (kSsgCenter - op.envelope) & kMaxEnvelope;
}

std::int32_t Ym2612::operator_output(Operator& op, std::int32_t modulation) const noexcept {
    const auto& table = tables();
    const std::uint32_t phase = ((op.phase >> 10) + static_cast<std::uint32_t>(modulation)) & 0x3FF;
    const std::uint32_t quadrant = (phase >> 8) & 0x03;
    const std::uint32_t index = (quadrant & 1) ? (255 - (phase & 0xFF)) : (phase & 0xFF);

    // The envelope is ten bits spanning the full 96dB range, so one envelope
    // step is four attenuation units; total level is seven bits over the same
    // range, so one TL step is thirty-two. Getting these weights wrong scales
    // every operator, and because a modulator's amplitude IS the modulation
    // index, wrong weights come out as distortion rather than wrong volume.
    std::int32_t attenuation = table.sine[index];
    attenuation += (effective_envelope(op) + (static_cast<std::int32_t>(op.total_level) << 3)) << 2;
    if (attenuation >= 4096) {
        return 0;  // past 96dB down, which is silence at this word width
    }
    // The exponential table holds 2^-x MINUS one, because the leading bit is
    // implicit - the same trick a float mantissa uses. Restoring it with the
    // OR is not optional: without it the curve collapses to zero at the bottom
    // of every 256-step band and the output is a buzz instead of a note.
    std::int32_t value = ((table.power[attenuation & 0xFF] | 0x800) << 2) >> (attenuation >> 8);
    if (quadrant & 2) {
        value = -value;
    }
    return value;
}

void Ym2612::generate_sample() noexcept {
    std::int32_t mix_left = 0;
    std::int32_t mix_right = 0;

    // The envelope generator runs at a third of the sample rate. The counter
    // it reads has to advance exactly once per tick and the envelopes have to
    // step only on that tick - stepping them every sample runs every attack,
    // decay and release three times too fast.
    bool envelope_tick = false;
    if (++envelope_divider_ >= 3) {
        envelope_divider_ = 0;
        ++envelope_counter_;
        envelope_tick = true;
    }

    for (std::size_t ci = 0; ci < kChannelCount; ++ci) {
        auto& channel = channels_[ci];
        const std::uint8_t key_code =
            static_cast<std::uint8_t>((channel.block << 2) |
                                      kKeyCodeTable[(channel.frequency_number >> 7) & 0x0F]);

        // Channel six turns into the DAC when enabled, and its FM output is
        // silenced entirely - not mixed with the sample.
        if (ci == 5 && dac_enabled_) {
            if (!dac_muted_) {
                if (channel.left) mix_left += dac_value_;
                if (channel.right) mix_right += dac_value_;
            }
            continue;
        }

        for (auto& op : channel.operators) {
            if (envelope_tick) {
                advance_envelope(op, key_code);
            }
            // Every sample, not every envelope tick: the loop point has to be
            // caught as soon as it is crossed or the repeat rate is wrong.
            run_ssg_eg(op);
            op.phase = (op.phase + op.increment) & 0xFFFFF;
        }

        auto& ops = channel.operators;
        // A modulator's output is added to the carrier's phase, so the two have
        // to be in the same units: a full-scale operator (+/-16336) must come
        // out as a few cycles of phase, not sixteen. Four full cycles at
        // maximum is what the hardware does.
        const auto modulate = [](std::int32_t value) { return value >> 2; };
        // Operator one is fed by its own two-sample history, scaled by the
        // feedback level. Zero feedback means no self-modulation at all; at
        // seven it is about two cycles of phase.
        const std::int32_t feedback_sum = channel.feedback == 0
            ? 0
            : ((channel.feedback_memory[0] + channel.feedback_memory[1]) << (channel.feedback + 6)) >> 17;
        const std::int32_t out1 = operator_output(ops[0], feedback_sum);
        channel.feedback_memory[1] = channel.feedback_memory[0];
        channel.feedback_memory[0] = out1;

        std::int32_t output = 0;
        switch (channel.algorithm) {
            case 0: {  // 1 -> 2 -> 3 -> 4
                const auto o2 = operator_output(ops[1], modulate(out1));
                const auto o3 = operator_output(ops[2], modulate(o2));
                output = operator_output(ops[3], modulate(o3));
                break;
            }
            case 1: {  // (1 + 2) -> 3 -> 4
                const auto o2 = operator_output(ops[1], 0);
                const auto o3 = operator_output(ops[2], modulate(out1 + o2));
                output = operator_output(ops[3], modulate(o3));
                break;
            }
            case 2: {  // 1 + (2 -> 3) -> 4
                const auto o2 = operator_output(ops[1], 0);
                const auto o3 = operator_output(ops[2], modulate(o2));
                output = operator_output(ops[3], modulate(out1 + o3));
                break;
            }
            case 3: {  // (1 -> 2) + 3 -> 4
                const auto o2 = operator_output(ops[1], modulate(out1));
                const auto o3 = operator_output(ops[2], 0);
                output = operator_output(ops[3], modulate(o2 + o3));
                break;
            }
            case 4: {  // (1 -> 2) + (3 -> 4)
                const auto o2 = operator_output(ops[1], modulate(out1));
                const auto o3 = operator_output(ops[2], 0);
                output = o2 + operator_output(ops[3], modulate(o3));
                break;
            }
            case 5: {  // 1 -> (2, 3, 4), summed
                const auto modulation = modulate(out1);
                output = operator_output(ops[1], modulation) +
                         operator_output(ops[2], modulation) +
                         operator_output(ops[3], modulation);
                break;
            }
            case 6: {  // (1 -> 2) + 3 + 4
                output = operator_output(ops[1], modulate(out1)) +
                         operator_output(ops[2], 0) +
                         operator_output(ops[3], 0);
                break;
            }
            default: {  // 7: all four in parallel
                output = out1 + operator_output(ops[1], 0) +
                         operator_output(ops[2], 0) + operator_output(ops[3], 0);
                break;
            }
        }

        // Each channel goes through its own DAC before the summing node. The
        // limit has to leave room for FOUR carriers at full scale, because
        // algorithms 4 to 7 sum that many - those are the bright, additive
        // patches used for leads and bells, and clamping them at one operator's
        // worth is hard clipping on exactly the sounds that show it most.
        output = std::clamp(output, -65535, 65535);
        // The operators above have already run and updated their state; the
        // mute only decides whether the result reaches the mix.
        if (fm_muted_) {
            continue;
        }
        if (channel.left) mix_left += output;
        if (channel.right) mix_right += output;
    }

    // Six channels at full tilt overflow a signed 16-bit sample, so scale down
    // rather than clip - clipping here sounds like distortion on every loud
    // passage, which is not what the hardware does. Six is the divisor that
    // keeps a realistic three-or-four-voice arrangement well clear of the rail
    // while still using most of the range.
    samples_.push_back(static_cast<std::int16_t>(clamp16(mix_left / 6)));
    samples_.push_back(static_cast<std::int16_t>(clamp16(mix_right / 6)));
    ++sample_count_;
}

void Ym2612::advance_cycles(std::uint32_t m68k_cycles) noexcept {
    // 144 master-clock-equivalents per chip sample; the 68000 runs at the same
    // clock the chip does, so the ratio is exact.
    cycle_accumulator_ += static_cast<double>(m68k_cycles);
    while (cycle_accumulator_ >= 144.0) {
        cycle_accumulator_ -= 144.0;

        // Timer A counts up to 1024 from its programmed start value; timer B
        // does the same over 256 but only every sixteenth sample.
        if (timer_a_enabled_) {
            if (++timer_a_counter_ >= 1024) {
                timer_a_counter_ = timer_a_period_;
                if (timer_a_flag_enabled_) timer_a_overflow_ = true;
            }
        }
        if (timer_b_enabled_) {
            if (++timer_b_prescale_ >= 16) {
                timer_b_prescale_ = 0;
                if (++timer_b_counter_ >= 256) {
                    timer_b_counter_ = timer_b_period_;
                    if (timer_b_flag_enabled_) timer_b_overflow_ = true;
                }
            }
        }

        generate_sample();
    }
}

std::string Ym2612::describe() const {
    std::ostringstream out;
    out << "YM2612 (OPN2)"
        << "\n  writes    : " << write_count_
        << "\n  samples   : " << sample_count_
        << "\n  dac       : " << (dac_enabled_ ? "on" : "off")
        << "\n  timer a   : " << (timer_a_enabled_ ? "running" : "stopped")
        << " period " << timer_a_period_
        << "\n  timer b   : " << (timer_b_enabled_ ? "running" : "stopped")
        << " period " << static_cast<int>(timer_b_period_);
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto& channel = channels_[i];
        out << "\n  ch" << (i + 1) << "       : alg " << static_cast<int>(channel.algorithm)
            << " fb " << static_cast<int>(channel.feedback)
            << " fnum " << channel.frequency_number
            << " block " << static_cast<int>(channel.block);
    }
    return out.str();
}

}  // namespace mercury::genesis
