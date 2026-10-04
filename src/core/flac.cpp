#include "mercury/core/flac.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mercury::core {

namespace {

// Most significant bit first, and unlike the CHD map's reader this one has
// to be able to give back an arbitrary width up to 32 and to count a run of
// zeros, which is how FLAC codes the high part of every residual.
class BitReader {
  public:
    BitReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] std::uint32_t read(int count) noexcept {
        std::uint32_t value = 0;
        while (count > 0) {
            if (bit_ == 0 && byte_ >= size_) {
                overflow_ = true;
                return value;
            }
            const int available = 8 - bit_;
            const int take = count < available ? count : available;
            const auto byte = byte_ < size_ ? data_[byte_] : 0;
            const auto chunk =
                static_cast<std::uint32_t>((byte >> (available - take)) & ((1u << take) - 1));
            value = (value << take) | chunk;
            bit_ += take;
            if (bit_ == 8) {
                bit_ = 0;
                ++byte_;
            }
            count -= take;
        }
        return value;
    }

    // Two's complement in `count` bits, sign-extended.
    [[nodiscard]] std::int32_t read_signed(int count) noexcept {
        if (count == 0) {
            return 0;
        }
        const auto raw = read(count);
        const std::uint32_t sign = 1u << (count - 1);
        return static_cast<std::int32_t>((raw ^ sign) - sign);
    }

    // Zeros until a one, returning how many zeros there were.
    [[nodiscard]] std::uint32_t unary() noexcept {
        std::uint32_t zeros = 0;
        while (read(1) == 0) {
            if (overflow_) {
                return zeros;
            }
            ++zeros;
            if (zeros > (1u << 20)) {  // a corrupt stream, not a long run
                overflow_ = true;
                return zeros;
            }
        }
        return zeros;
    }

    void align() noexcept {
        if (bit_ != 0) {
            bit_ = 0;
            ++byte_;
        }
    }

    [[nodiscard]] std::size_t byte_position() const noexcept { return byte_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t byte_ = 0;
    int bit_ = 0;
    bool overflow_ = false;
};

// The four fixed predictors. Order 0 predicts silence, order 1 the previous
// sample, and so on up to a cubic fit - between them they cover most audio
// without the encoder having to send any coefficients at all.
void undo_fixed(int order, std::vector<std::int64_t>& samples, std::size_t warmup,
                std::size_t count) {
    for (std::size_t i = warmup; i < warmup + count; ++i) {
        std::int64_t prediction = 0;
        switch (order) {
            case 0: prediction = 0; break;
            case 1: prediction = samples[i - 1]; break;
            case 2: prediction = 2 * samples[i - 1] - samples[i - 2]; break;
            case 3:
                prediction = 3 * samples[i - 1] - 3 * samples[i - 2] + samples[i - 3];
                break;
            default:
                prediction = 4 * samples[i - 1] - 6 * samples[i - 2] + 4 * samples[i - 3] -
                             samples[i - 4];
                break;
        }
        samples[i] += prediction;
    }
}

}  // namespace

bool flac_decode_cd(const std::uint8_t* data, std::size_t size, std::size_t sample_frames,
                    std::vector<std::uint8_t>& out, std::size_t& consumed, std::string& error) {
    constexpr int kChannels = 2;
    constexpr int kBitsPerSample = 16;

    out.assign(sample_frames * 4, 0);
    consumed = 0;
    BitReader in(data, size);
    std::size_t produced = 0;
    std::array<std::vector<std::int64_t>, kChannels> channel;

    // MERCURY_FLAC_DRAIN=1: keep going past what was asked for, until the
    // stream runs out, and report the total. How many samples the frames
    // really hold is the one number that says whether a sample is two bytes
    // of the sector or one.
    static const bool drain = [] {
        const char* set = std::getenv("MERCURY_FLAC_DRAIN");
        return set != nullptr && *set == '1';
    }();
    std::size_t drained = 0;
    while (produced < sample_frames || (drain && !in.overflowed() &&
                                        in.byte_position() + 4 < size)) {
        // --- frame header ---
        const auto sync = in.read(14);
        if (sync != 0x3FFE) {
            if (drain) {
                std::fprintf(stderr,
                             "[flac] drained %zu sample frames from %zu bytes (asked for %zu), "
                             "stopped at byte %zu of %zu\n",
                             drained, size, sample_frames, in.byte_position(), size);
            }
            error = "the FLAC stream lost its frame sync";
            return false;
        }
        (void)in.read(1);  // reserved
        const auto variable_block = in.read(1);
        const auto block_code = in.read(4);
        const auto rate_code = in.read(4);
        const auto channel_code = in.read(4);
        const auto depth_code = in.read(3);
        (void)in.read(1);  // reserved

        // The frame or sample number, in the same variable-length coding
        // UTF-8 uses. Only its length matters here - nothing downstream
        // cares which frame this is, because they arrive in order.
        const auto first = in.read(8);
        int extra = 0;
        if ((first & 0x80) != 0) {
            for (std::uint32_t mask = 0x40; (first & mask) != 0 && extra < 6; mask >>= 1) {
                ++extra;
            }
        }
        for (int i = 0; i < extra; ++i) {
            (void)in.read(8);
        }

        std::uint32_t block_size = 0;
        switch (block_code) {
            case 1: block_size = 192; break;
            case 2: case 3: case 4: case 5:
                block_size = 576u << (block_code - 2);
                break;
            case 6: block_size = in.read(8) + 1; break;
            case 7: block_size = in.read(16) + 1; break;
            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                block_size = 256u << (block_code - 8);
                break;
            default:
                error = "the FLAC frame does not say how long it is";
                return false;
        }
        // Sample rate, when the frame carries one rather than deferring to
        // a stream header this stream does not have. Read and discarded: a
        // CD is 44.1kHz by definition.
        if (rate_code == 12) {
            (void)in.read(8);
        } else if (rate_code == 13 || rate_code == 14) {
            (void)in.read(16);
        }
        (void)in.read(8);  // the header's CRC-8
        (void)variable_block;
        (void)depth_code;

        if (channel_code > 10) {
            error = "the FLAC frame declares more channels than a CD has";
            return false;
        }
        const int channels = channel_code < 8 ? static_cast<int>(channel_code) + 1 : 2;
        if (channels != kChannels) {
            error = "the FLAC frame is not stereo";
            return false;
        }

        // --- the subframes, one per channel ---
        for (int ch = 0; ch < channels; ++ch) {
            // A side channel carries one more bit than the others, because
            // a difference of two sixteen-bit numbers needs seventeen.
            int bits = kBitsPerSample;
            if ((channel_code == 8 && ch == 1) || (channel_code == 9 && ch == 0) ||
                (channel_code == 10 && ch == 1)) {
                ++bits;
            }

            if (in.read(1) != 0) {
                error = "a FLAC subframe has its padding bit set";
                return false;
            }
            const auto type = in.read(6);
            std::uint32_t wasted = 0;
            if (in.read(1) != 0) {
                wasted = in.unary() + 1;
            }
            // MERCURY_FLAC_NOWASTED=1: parse the wasted-bits count but do not
            // act on it, to tell a misparse apart from a stream that really
            // says its low bits are zero.
            static const bool ignore_wasted = [] {
                const char* set = std::getenv("MERCURY_FLAC_NOWASTED");
                return set != nullptr && *set == '1';
            }();
            if (ignore_wasted) {
                wasted = 0;
            }
            bits -= static_cast<int>(wasted);
            if (bits <= 0 || bits > 32) {
                error = "a FLAC subframe wastes more bits than it has";
                return false;
            }

            // MERCURY_FLAC_LOG=N: the shape of the first N subframes. Which
            // predictor, which order, how many bits - the numbers that decide
            // everything and that nothing else can show.
            {
                static const long limit = [] {
                    const char* set = std::getenv("MERCURY_FLAC_LOG");
                    return set != nullptr ? std::strtol(set, nullptr, 10) : 0;
                }();
                static long said = 0;
                if (said < limit) {
                    ++said;
                    std::fprintf(stderr,
                                 "[flac] block=%u chan=%u ch=%d type=%u wasted=%u bits=%d\n",
                                 block_size, channel_code, ch, type, wasted, bits);
                }
            }

            auto& samples = channel[static_cast<std::size_t>(ch)];
            samples.assign(block_size, 0);

            int order = 0;
            bool lpc = false;
            std::vector<std::int32_t> coefficients;
            int shift = 0;

            if (type == 0) {
                // CONSTANT: one value for the whole block.
                const auto value = in.read_signed(bits);
                for (auto& sample : samples) {
                    sample = value;
                }
            } else if (type == 1) {
                // VERBATIM: the encoder gave up, the samples are just there.
                for (auto& sample : samples) {
                    sample = in.read_signed(bits);
                }
            } else if (type >= 8 && type <= 12) {
                order = static_cast<int>(type - 8);
            } else if (type >= 32) {
                order = static_cast<int>(type - 31);
                lpc = true;
            } else {
                error = "a FLAC subframe uses a reserved type";
                return false;
            }

            if (type >= 8) {
                if (static_cast<std::uint32_t>(order) > block_size) {
                    error = "a FLAC subframe predicts from more samples than it has";
                    return false;
                }
                for (int i = 0; i < order; ++i) {
                    samples[static_cast<std::size_t>(i)] = in.read_signed(bits);
                }
                if (lpc) {
                    const int precision = static_cast<int>(in.read(4)) + 1;
                    if (precision == 16) {
                        error = "a FLAC subframe uses an invalid coefficient precision";
                        return false;
                    }
                    shift = static_cast<int>(in.read_signed(5));
                    if (shift < 0) {
                        error = "a FLAC subframe uses a negative shift";
                        return false;
                    }
                    coefficients.resize(static_cast<std::size_t>(order));
                    for (int i = 0; i < order; ++i) {
                        coefficients[static_cast<std::size_t>(i)] = in.read_signed(precision);
                    }
                }

                // --- the residual, Rice coded in partitions ---
                const auto method = in.read(2);
                if (method > 1) {
                    error = "a FLAC residual uses a reserved coding method";
                    return false;
                }
                const int param_bits = method == 0 ? 4 : 5;
                const std::uint32_t escape = method == 0 ? 0xFu : 0x1Fu;
                const auto partition_order = in.read(4);
                const std::uint32_t partitions = 1u << partition_order;
                if (block_size % partitions != 0) {
                    error = "a FLAC residual's partitions do not divide the block";
                    return false;
                }
                std::size_t at = static_cast<std::size_t>(order);
                for (std::uint32_t p = 0; p < partitions; ++p) {
                    std::uint32_t count = block_size / partitions;
                    if (p == 0) {
                        if (count < static_cast<std::uint32_t>(order)) {
                            error = "a FLAC residual's first partition is shorter than its order";
                            return false;
                        }
                        count -= static_cast<std::uint32_t>(order);
                    }
                    const auto parameter = in.read(param_bits);
                    if (parameter == escape) {
                        // An escaped partition stores the residuals raw at
                        // a fixed width, which may be zero - meaning they
                        // are all zero and nothing is stored at all.
                        const int raw_bits = static_cast<int>(in.read(5));
                        for (std::uint32_t i = 0; i < count; ++i, ++at) {
                            samples[at] = raw_bits == 0 ? 0 : in.read_signed(raw_bits);
                        }
                    } else {
                        for (std::uint32_t i = 0; i < count; ++i, ++at) {
                            const std::uint32_t high = in.unary();
                            const std::uint32_t low = in.read(static_cast<int>(parameter));
                            const std::uint32_t folded = (high << parameter) | low;
                            // Zig-zag: the sign is in the low bit.
                            samples[at] = static_cast<std::int32_t>((folded >> 1) ^
                                                                    (0u - (folded & 1u)));
                        }
                    }
                }

                const std::size_t count = block_size - static_cast<std::size_t>(order);
                if (lpc) {
                    for (std::size_t i = static_cast<std::size_t>(order); i < block_size; ++i) {
                        std::int64_t sum = 0;
                        for (int c = 0; c < order; ++c) {
                            sum += static_cast<std::int64_t>(
                                       coefficients[static_cast<std::size_t>(c)]) *
                                   samples[i - 1 - static_cast<std::size_t>(c)];
                        }
                        samples[i] += sum >> shift;
                    }
                } else {
                    undo_fixed(order, samples, static_cast<std::size_t>(order), count);
                }
            }

            if (wasted != 0) {
                for (auto& sample : samples) {
                    sample <<= wasted;
                }
            }
            {
                static const long limit = [] {
                    const char* set = std::getenv("MERCURY_FLAC_LOG");
                    return set != nullptr ? std::strtol(set, nullptr, 10) : 0;
                }();
                static long said = 0;
                if (said < limit) {
                    ++said;
                    std::fprintf(stderr, "[flac] ch=%d first samples:", ch);
                    for (int k = 0; k < 6; ++k) {
                        std::fprintf(stderr, " %lld", static_cast<long long>(samples[k]));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }

        in.align();
        (void)in.read(16);  // the frame's CRC-16
        if (in.overflowed()) {
            error = "the FLAC stream ended in the middle of a frame";
            return false;
        }

        // --- put the two channels back together ---
        //
        // Stereo is coded as whichever pair compresses best: the two
        // channels on their own, or one of them plus the difference. Mid and
        // side is the fiddly one - the mid channel has lost its bottom bit,
        // which is recovered from the side channel's.
        auto& left = channel[0];
        auto& right = channel[1];
        drained += block_size;
        for (std::uint32_t i = 0; i < block_size && produced < sample_frames; ++i, ++produced) {
            std::int64_t l = left[i];
            std::int64_t r = right[i];
            if (channel_code == 8) {
                r = l - r;  // left and difference
            } else if (channel_code == 9) {
                l = r + l;  // difference and right
            } else if (channel_code == 10) {
                const std::int64_t side = r;
                std::int64_t mid = (l << 1) | (side & 1);
                l = (mid + side) >> 1;
                r = (mid - side) >> 1;
            }
            auto* at = &out[produced * 4];
            at[0] = static_cast<std::uint8_t>((l >> 8) & 0xFF);
            at[1] = static_cast<std::uint8_t>(l & 0xFF);
            at[2] = static_cast<std::uint8_t>((r >> 8) & 0xFF);
            at[3] = static_cast<std::uint8_t>(r & 0xFF);
        }
    }

    consumed = in.byte_position();
    return true;
}

}  // namespace mercury::core
