#include "mercury/core/lzma.hpp"

#include <array>

namespace mercury::core {

namespace {

// LZMA's fixed sizes. None of these are choices - they are the format.
constexpr int kNumPosBitsMax = 4;
constexpr int kNumStates = 12;
constexpr int kNumLenToPosStates = 4;
constexpr int kNumAlignBits = 4;
constexpr int kEndPosModelIndex = 14;
constexpr int kNumFullDistances = 1 << (kEndPosModelIndex >> 1);  // 128
constexpr int kMatchMinLen = 2;

// A probability is eleven bits, and it moves a thirty-second of the way
// towards its new value each time it is used. Both numbers are the format's.
constexpr std::uint16_t kProbInit = (1u << 11) / 2;
constexpr int kNumMoveBits = 5;

// THE RANGE DECODER.
//
// Arithmetic coding, not Huffman: there is no symbol boundary to find, only
// a running interval that every decoded bit narrows. `code` is where the
// encoded number sits inside `range`, and both are kept scaled up so the top
// byte is always significant - which is what the normalise step maintains.
class RangeDecoder {
  public:
    RangeDecoder(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] bool start() noexcept {
        // Five bytes: the first is always zero and is skipped, the next four
        // are the initial code.
        if (size_ < 5 || data_[0] != 0) {
            return false;
        }
        at_ = 1;
        code_ = 0;
        range_ = 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) {
            code_ = (code_ << 8) | next();
        }
        return true;
    }

    [[nodiscard]] unsigned bit(std::uint16_t& prob) noexcept {
        const std::uint32_t bound = (range_ >> 11) * prob;
        unsigned result;
        if (code_ < bound) {
            range_ = bound;
            prob = static_cast<std::uint16_t>(prob + (((1u << 11) - prob) >> kNumMoveBits));
            result = 0;
        } else {
            range_ -= bound;
            code_ -= bound;
            prob = static_cast<std::uint16_t>(prob - (prob >> kNumMoveBits));
            result = 1;
        }
        normalise();
        return result;
    }

    // Bits with no model behind them, used for the high part of a long
    // distance where the encoder found no structure worth modelling.
    [[nodiscard]] std::uint32_t direct(int count) noexcept {
        std::uint32_t result = 0;
        while (count-- > 0) {
            range_ >>= 1;
            code_ -= range_;
            const std::uint32_t t = 0u - (code_ >> 31);
            code_ += range_ & t;
            normalise();
            result = (result << 1) + t + 1;
        }
        return result;
    }

    // A balanced tree of probabilities: `count` bits, most significant
    // first, the path so far selecting the next probability.
    [[nodiscard]] std::uint32_t tree(std::uint16_t* probs, int count) noexcept {
        std::uint32_t m = 1;
        for (int i = 0; i < count; ++i) {
            m = (m << 1) + bit(probs[m]);
        }
        return m - (1u << count);
    }

    // The same tree read the other way up, which is what distances and the
    // alignment bits use.
    [[nodiscard]] std::uint32_t tree_reverse(std::uint16_t* probs, int count) noexcept {
        std::uint32_t m = 1;
        std::uint32_t symbol = 0;
        for (int i = 0; i < count; ++i) {
            const unsigned b = bit(probs[m]);
            m = (m << 1) + b;
            symbol |= static_cast<std::uint32_t>(b) << i;
        }
        return symbol;
    }

    // The same, but with the base given as a SIGNED offset into an array of
    // known length, because the distance models are addressed from a base
    // that can be one before the array - see the caller. Out of range reads
    // a spare probability rather than memory that is not ours; a stream
    // that asks for one is corrupt and will fail its length check anyway.
    [[nodiscard]] std::uint32_t tree_reverse_offset(std::uint16_t* probs, std::ptrdiff_t base,
                                                    int count, std::size_t length) noexcept {
        std::uint32_t m = 1;
        std::uint32_t symbol = 0;
        std::uint16_t spare = kProbInit;
        for (int i = 0; i < count; ++i) {
            const auto at = base + static_cast<std::ptrdiff_t>(m);
            std::uint16_t& prob =
                (at >= 0 && static_cast<std::size_t>(at) < length) ? probs[at] : spare;
            const unsigned b = bit(prob);
            m = (m << 1) + b;
            symbol |= static_cast<std::uint32_t>(b) << i;
        }
        return symbol;
    }

    [[nodiscard]] bool ran_out() const noexcept { return overrun_; }

  private:
    [[nodiscard]] std::uint8_t next() noexcept {
        if (at_ >= size_) {
            overrun_ = true;
            return 0;
        }
        return data_[at_++];
    }

    void normalise() noexcept {
        if (range_ < (1u << 24)) {
            range_ <<= 8;
            code_ = (code_ << 8) | next();
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
    std::uint32_t range_ = 0;
    std::uint32_t code_ = 0;
    bool overrun_ = false;
};

// The length of a match, in three ranges so short matches - much the most
// common - cost the fewest bits.
struct LengthModel {
    std::uint16_t choice = kProbInit;
    std::uint16_t choice2 = kProbInit;
    std::array<std::uint16_t, 16 * 8> low{};
    std::array<std::uint16_t, 16 * 8> mid{};
    std::array<std::uint16_t, 256> high{};

    void reset() {
        choice = kProbInit;
        choice2 = kProbInit;
        low.fill(kProbInit);
        mid.fill(kProbInit);
        high.fill(kProbInit);
    }

    [[nodiscard]] std::uint32_t decode(RangeDecoder& in, std::uint32_t pos_state) {
        if (in.bit(choice) == 0) {
            return in.tree(&low[pos_state * 8], 3);
        }
        if (in.bit(choice2) == 0) {
            return 8 + in.tree(&mid[pos_state * 8], 3);
        }
        return 16 + in.tree(high.data(), 8);
    }
};

}  // namespace

bool lzma_decode(const std::uint8_t* data, std::size_t data_size, std::size_t size, int lc, int lp,
                 int pb, std::vector<std::uint8_t>& out, std::string& error) {
    if (lc < 0 || lc > 8 || lp < 0 || lp > 4 || pb < 0 || pb > kNumPosBitsMax) {
        error = "LZMA properties out of range";
        return false;
    }
    RangeDecoder in(data, data_size);
    if (!in.start()) {
        error = "the LZMA stream does not start with a range coder header";
        return false;
    }

    std::array<std::uint16_t, kNumStates << kNumPosBitsMax> is_match{};
    std::array<std::uint16_t, kNumStates> is_rep{};
    std::array<std::uint16_t, kNumStates> is_rep_g0{};
    std::array<std::uint16_t, kNumStates> is_rep_g1{};
    std::array<std::uint16_t, kNumStates> is_rep_g2{};
    std::array<std::uint16_t, kNumStates << kNumPosBitsMax> is_rep0_long{};
    std::array<std::uint16_t, kNumLenToPosStates << 6> pos_slot{};
    std::array<std::uint16_t, kNumFullDistances - kEndPosModelIndex> spec_pos{};
    std::array<std::uint16_t, 1 << kNumAlignBits> align{};
    is_match.fill(kProbInit);
    is_rep.fill(kProbInit);
    is_rep_g0.fill(kProbInit);
    is_rep_g1.fill(kProbInit);
    is_rep_g2.fill(kProbInit);
    is_rep0_long.fill(kProbInit);
    pos_slot.fill(kProbInit);
    spec_pos.fill(kProbInit);
    align.fill(kProbInit);
    LengthModel length;
    LengthModel rep_length;
    length.reset();
    rep_length.reset();
    std::vector<std::uint16_t> literal(static_cast<std::size_t>(0x300) << (lc + lp), kProbInit);

    const std::uint32_t pos_mask = (1u << pb) - 1;
    const std::uint32_t literal_pos_mask = (1u << lp) - 1;

    out.assign(size, 0);
    std::size_t position = 0;
    int state = 0;
    std::uint32_t rep0 = 0;
    std::uint32_t rep1 = 0;
    std::uint32_t rep2 = 0;
    std::uint32_t rep3 = 0;

    while (position < size) {
        const std::uint32_t pos_state = static_cast<std::uint32_t>(position) & pos_mask;
        if (in.bit(is_match[(static_cast<std::size_t>(state) << kNumPosBitsMax) + pos_state]) ==
            0) {
            // A LITERAL. Which set of probabilities to use depends on the
            // top bits of the byte before and on the low bits of the
            // position - text and binary data want different contexts and
            // lc/lp are how the encoder said which.
            const std::uint8_t previous = position > 0 ? out[position - 1] : 0;
            const std::size_t index =
                0x300 * ((((static_cast<std::uint32_t>(position) & literal_pos_mask) << lc) +
                          (previous >> (8 - lc))));
            std::uint16_t* probs = &literal[index];
            std::uint32_t symbol = 1;
            if (state >= 7) {
                // Straight after a match the encoder codes the literal
                // against the byte that WOULD have come next, bit by bit,
                // and stops doing so as soon as they differ.
                if (rep0 + 1 > position) {
                    error = "an LZMA literal refers to data before the start of the hunk";
                    return false;
                }
                std::uint32_t matched = out[position - rep0 - 1];
                do {
                    const std::uint32_t match_bit = (matched >> 7) & 1;
                    matched <<= 1;
                    const unsigned b = in.bit(probs[((1 + match_bit) << 8) + symbol]);
                    symbol = (symbol << 1) | b;
                    if (match_bit != b) {
                        while (symbol < 0x100) {
                            symbol = (symbol << 1) | in.bit(probs[symbol]);
                        }
                        break;
                    }
                } while (symbol < 0x100);
            } else {
                while (symbol < 0x100) {
                    symbol = (symbol << 1) | in.bit(probs[symbol]);
                }
            }
            out[position++] = static_cast<std::uint8_t>(symbol);
            state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            continue;
        }

        std::uint32_t match_length;
        if (in.bit(is_rep[static_cast<std::size_t>(state)]) == 0) {
            // A NEW DISTANCE. The three previous ones shuffle down; games
            // and program data repeat the same few offsets constantly,
            // which is what the rep slots exist for.
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            match_length = length.decode(in, pos_state);
            state = state < 7 ? 7 : 10;
            const std::uint32_t len_state =
                match_length < kNumLenToPosStates ? match_length : kNumLenToPosStates - 1;
            const std::uint32_t slot = in.tree(&pos_slot[len_state << 6], 6);
            if (slot >= 4) {
                const int direct_bits = static_cast<int>((slot >> 1) - 1);
                rep0 = (2 | (slot & 1)) << direct_bits;
                if (slot < kEndPosModelIndex) {
                    // THE OFFSET IS LEGITIMATELY -1 for the first slot, and
                    // that is not a mistake: the reverse tree indexes from
                    // one, so the base is deliberately one before the array
                    // and the first probability it touches is element zero.
                    // Computed signed - in unsigned arithmetic this is four
                    // billion and the pointer is wild.
                    const auto base = static_cast<std::ptrdiff_t>(rep0) -
                                      static_cast<std::ptrdiff_t>(slot) - 1;
                    rep0 += in.tree_reverse_offset(spec_pos.data(), base, direct_bits,
                                                   spec_pos.size());
                } else {
                    rep0 += in.direct(direct_bits - kNumAlignBits) << kNumAlignBits;
                    rep0 += in.tree_reverse(align.data(), kNumAlignBits);
                }
            } else {
                rep0 = slot;
            }
            if (rep0 == 0xFFFFFFFFu) {
                break;  // the end marker, which a CHD does not use but which costs nothing
            }
        } else {
            // ONE OF THE LAST FOUR DISTANCES AGAIN.
            if (in.bit(is_rep_g0[static_cast<std::size_t>(state)]) == 0) {
                if (in.bit(is_rep0_long[(static_cast<std::size_t>(state) << kNumPosBitsMax) +
                                        pos_state]) == 0) {
                    // The shortest thing LZMA can say: one more byte from
                    // where the last match came from.
                    state = state < 7 ? 9 : 11;
                    if (rep0 + 1 > position) {
                        error = "an LZMA match refers to data before the start of the hunk";
                        return false;
                    }
                    out[position] = out[position - rep0 - 1];
                    ++position;
                    continue;
                }
            } else {
                std::uint32_t distance;
                if (in.bit(is_rep_g1[static_cast<std::size_t>(state)]) == 0) {
                    distance = rep1;
                } else {
                    if (in.bit(is_rep_g2[static_cast<std::size_t>(state)]) == 0) {
                        distance = rep2;
                    } else {
                        distance = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = distance;
            }
            match_length = rep_length.decode(in, pos_state);
            state = state < 7 ? 8 : 11;
        }

        const std::size_t run = match_length + kMatchMinLen;
        if (rep0 + 1 > position) {
            error = "an LZMA match refers to data before the start of the hunk";
            return false;
        }
        // Byte at a time on purpose: a match may overlap itself, which is
        // how LZMA writes a run of one repeated byte, so a block copy would
        // be wrong.
        for (std::size_t i = 0; i < run && position < size; ++i, ++position) {
            out[position] = out[position - rep0 - 1];
        }
    }

    if (in.ran_out()) {
        error = "the LZMA stream ended before the hunk was complete";
        return false;
    }
    if (position != size) {
        error = "the LZMA stream decoded to " + std::to_string(position) + " bytes, expected " +
                std::to_string(size);
        return false;
    }
    return true;
}

}  // namespace mercury::core
