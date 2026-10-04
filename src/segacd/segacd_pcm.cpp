// The PCM sound chip, a Ricoh RF5C164: eight channels of 8-bit samples played
// out of 64KB of wave RAM, each with its own pitch, volume, pan and loop point.
//
// It runs at the sub CPU's clock over 384 - 32,552Hz - and is caught up lazily:
// whenever the sub CPU touches it, and at the end of every frame, it produces
// however many samples have fallen due since it last ran. The sample format and
// the channel arithmetic follow Genesis Plus GX.

#include <algorithm>

#include "mercury/segacd/segacd.hpp"

namespace mercury::segacd {

namespace {
constexpr std::uint32_t kPcmClockDivider = 384;
}

void SegaCd::pcm_reset() noexcept {
    for (auto& channel : pcm_channels_) {
        channel = PcmChannel{};
    }
    std::fill(pcm_ram_.begin(), pcm_ram_.end(), std::uint8_t{0});
    pcm_bank_ = 0;
    pcm_on_ = 0;
    pcm_selected_ = 0;
    pcm_enabled_ = false;
    pcm_clock_ = now_;
    pcm_native_.clear();
    pcm_resample_phase_ = 0.0;
    pcm_last_ = {0, 0};
}

void SegaCd::pcm_catch_up() noexcept {
    if (now_ <= pcm_clock_) {
        return;
    }
    const std::uint64_t samples = (now_ - pcm_clock_) / kPcmClockDivider;
    pcm_clock_ += samples * kPcmClockDivider;
    for (std::uint64_t n = 0; n < samples; ++n) {
        std::int32_t left = 0;
        std::int32_t right = 0;
        if (pcm_enabled_) {
            for (std::size_t index = 0; index < pcm_channels_.size(); ++index) {
                if ((pcm_on_ & (1u << index)) == 0) {
                    continue;
                }
                auto& channel = pcm_channels_[index];
                std::int32_t data = pcm_ram_[(channel.addr >> 11) & 0xFFFF];
                if (data == 0xFF) {
                    // $FF is the loop marker, never a sample: jump back to
                    // the loop point and play what is there.
                    channel.addr = static_cast<std::uint32_t>(channel.ls) << 11;
                    data = pcm_ram_[channel.ls];
                } else {
                    channel.addr += channel.fd;
                }
                if (data == 0xFF) {
                    continue;  // a loop onto a loop marker is silence
                }
                // Sign and magnitude, not two's complement: bit 7 set is
                // positive.
                const std::int32_t sample = (data & 0x80) != 0 ? (data & 0x7F) : -(data & 0x7F);
                left += (sample * channel.env * (channel.pan & 0x0F)) >> 5;
                right += (sample * channel.env * (channel.pan >> 4)) >> 5;
            }
        }
        pcm_native_.push_back(std::clamp(left, -32768, 32767));
        pcm_native_.push_back(std::clamp(right, -32768, 32767));
    }
}

void SegaCd::pcm_write(std::uint32_t offset, std::uint8_t value) noexcept {
    pcm_catch_up();
    if (offset >= 0x1000) {
        pcm_ram_[pcm_bank_ + (offset & 0xFFF)] = value;
        return;
    }
    auto& channel = pcm_channels_[pcm_selected_];
    switch (offset) {
        case 0x00: channel.env = value; return;
        case 0x01: channel.pan = value; return;
        case 0x02: channel.fd = static_cast<std::uint16_t>((channel.fd & 0xFF00) | value); return;
        case 0x03: channel.fd = static_cast<std::uint16_t>((channel.fd & 0x00FF) | (value << 8)); return;
        case 0x04: channel.ls = static_cast<std::uint16_t>((channel.ls & 0xFF00) | value); return;
        case 0x05: channel.ls = static_cast<std::uint16_t>((channel.ls & 0x00FF) | (value << 8)); return;
        case 0x06:
            channel.st = static_cast<std::uint32_t>(value) << (8 + 11);
            // A channel that is off starts again from here.
            if ((pcm_on_ & (1u << pcm_selected_)) == 0) {
                channel.addr = channel.st;
            }
            return;
        case 0x07:
            // Bit 6 picks what the low bits mean: a channel to address, or a
            // 4KB bank of wave RAM for the $1000-$1FFF window.
            if ((value & 0x40) != 0) {
                pcm_selected_ = static_cast<std::uint8_t>(value & 0x07);
            } else {
                pcm_bank_ = static_cast<std::uint32_t>(value & 0x0F) << 12;
            }
            pcm_enabled_ = (value & 0x80) != 0;
            return;
        case 0x08:
            // A set bit turns a channel OFF, and an off channel sits at its
            // start address ready to play.
            pcm_on_ = static_cast<std::uint8_t>(~value);
            for (std::size_t index = 0; index < pcm_channels_.size(); ++index) {
                if ((value & (1u << index)) != 0) {
                    pcm_channels_[index].addr = pcm_channels_[index].st;
                }
            }
            return;
        default:
            return;
    }
}

std::uint8_t SegaCd::pcm_read(std::uint32_t offset) noexcept {
    pcm_catch_up();
    if (offset >= 0x1000) {
        return pcm_ram_[pcm_bank_ + (offset & 0xFFF)];
    }
    if (offset >= 0x10 && offset < 0x20) {
        // Each channel's current position, so software can tell how far a
        // sample has played.
        const auto& channel = pcm_channels_[(offset >> 1) & 0x07];
        return (offset & 1) != 0 ? static_cast<std::uint8_t>(channel.addr >> (11 + 8))
                                 : static_cast<std::uint8_t>(channel.addr >> 11);
    }
    return 0xFF;
}

}  // namespace mercury::segacd
