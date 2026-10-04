#include "mercury/core/zip_archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

namespace mercury::core {
namespace {

std::uint16_t read_u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// --- DEFLATE ---------------------------------------------------------------

// Bits come out least-significant first within each byte, which is the opposite
// of the Huffman codes themselves - those are packed most-significant first and
// have to be reversed as they are read. Getting that backwards is the classic
// way to write an inflate that decodes the first few symbols and then diverges.
class BitReader {
  public:
    BitReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    bool bits(std::uint32_t count, std::uint32_t& value) noexcept {
        value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (bit_pos_ == 0 && byte_pos_ >= size_) {
                return false;
            }
            const std::uint32_t bit = (data_[byte_pos_] >> bit_pos_) & 1u;
            value |= bit << i;
            if (++bit_pos_ == 8) {
                bit_pos_ = 0;
                ++byte_pos_;
            }
        }
        return true;
    }

    void align_to_byte() noexcept {
        if (bit_pos_ != 0) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
    }

    [[nodiscard]] std::size_t byte_position() const noexcept { return byte_pos_; }
    [[nodiscard]] bool exhausted() const noexcept { return byte_pos_ >= size_; }
    void skip_bytes(std::size_t count) noexcept { byte_pos_ += count; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t byte_pos_ = 0;
    std::uint32_t bit_pos_ = 0;
};

// A canonical Huffman table, decoded the simple way: walk one bit at a time and
// compare against the first code of each length. Slower than a lookup table and
// far easier to get right; a ROM is a few megabytes and this runs once.
struct Huffman {
    std::array<int, 16> count{};
    std::vector<int> symbols;

    bool build(const std::uint8_t* lengths, std::size_t n) {
        count.fill(0);
        for (std::size_t i = 0; i < n; ++i) {
            ++count[lengths[i]];
        }
        count[0] = 0;
        // A table is only valid if it is neither over- nor under-subscribed.
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) {
                return false;
            }
        }
        std::array<int, 16> offsets{};
        for (int len = 1; len < 15; ++len) {
            offsets[len + 1] = offsets[len] + count[len];
        }
        symbols.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbols[offsets[lengths[i]]++] = static_cast<int>(i);
            }
        }
        return true;
    }

    bool decode(BitReader& in, int& symbol) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len < 16; ++len) {
            std::uint32_t bit = 0;
            if (!in.bits(1, bit)) {
                return false;
            }
            code |= static_cast<int>(bit);
            const int n = count[len];
            if (code - first < n) {
                symbol = symbols[static_cast<std::size_t>(index + (code - first))];
                return true;
            }
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return false;
    }
};

constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                                    15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,   33,   49,   65,    97,    129,
    193,  257,  385,  513,  769,  1025,  1537,  2049,  3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                      4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                      9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_block_data(BitReader& in, const Huffman& lit, const Huffman& dist,
                        std::vector<std::uint8_t>& out, std::string& error) {
    for (;;) {
        int symbol = 0;
        if (!lit.decode(in, symbol)) {
            error = "deflate stream ended inside a block";
            return false;
        }
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) {
            return true;
        }
        symbol -= 257;
        if (symbol >= static_cast<int>(kLengthBase.size())) {
            error = "invalid length code in deflate stream";
            return false;
        }
        std::uint32_t extra = 0;
        if (!in.bits(kLengthExtra[static_cast<std::size_t>(symbol)], extra)) {
            error = "deflate stream ended reading a length";
            return false;
        }
        const std::size_t length = kLengthBase[static_cast<std::size_t>(symbol)] + extra;

        int dsymbol = 0;
        if (!dist.decode(in, dsymbol) || dsymbol >= static_cast<int>(kDistanceBase.size())) {
            error = "invalid distance code in deflate stream";
            return false;
        }
        if (!in.bits(kDistanceExtra[static_cast<std::size_t>(dsymbol)], extra)) {
            error = "deflate stream ended reading a distance";
            return false;
        }
        const std::size_t distance = kDistanceBase[static_cast<std::size_t>(dsymbol)] + extra;
        if (distance > out.size()) {
            error = "deflate back-reference points before the start of the output";
            return false;
        }
        // Copied one byte at a time on purpose: a match may overlap itself,
        // which is how a run of repeated bytes is encoded, and a bulk copy
        // would read the source before it has been written.
        const std::size_t from = out.size() - distance;
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(out[from + i]);
        }
    }
}

bool build_fixed_tables(Huffman& lit, Huffman& dist) {
    std::array<std::uint8_t, 288> lit_lengths{};
    for (std::size_t i = 0; i < 144; ++i) lit_lengths[i] = 8;
    for (std::size_t i = 144; i < 256; ++i) lit_lengths[i] = 9;
    for (std::size_t i = 256; i < 280; ++i) lit_lengths[i] = 7;
    for (std::size_t i = 280; i < 288; ++i) lit_lengths[i] = 8;
    std::array<std::uint8_t, 30> dist_lengths{};
    dist_lengths.fill(5);
    return lit.build(lit_lengths.data(), lit_lengths.size()) &&
           dist.build(dist_lengths.data(), dist_lengths.size());
}

bool read_dynamic_tables(BitReader& in, Huffman& lit, Huffman& dist, std::string& error) {
    // The order the code-length alphabet's own lengths are stored in - chosen so
    // the rarely used ones fall at the end and can be omitted.
    static constexpr std::array<std::uint8_t, 19> kOrder{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                         11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::uint32_t hlit = 0;
    std::uint32_t hdist = 0;
    std::uint32_t hclen = 0;
    if (!in.bits(5, hlit) || !in.bits(5, hdist) || !in.bits(4, hclen)) {
        error = "deflate stream ended in a dynamic block header";
        return false;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;

    std::array<std::uint8_t, 19> code_lengths{};
    for (std::uint32_t i = 0; i < hclen; ++i) {
        std::uint32_t value = 0;
        if (!in.bits(3, value)) {
            error = "deflate stream ended in the code-length table";
            return false;
        }
        code_lengths[kOrder[i]] = static_cast<std::uint8_t>(value);
    }
    Huffman code_tree;
    if (!code_tree.build(code_lengths.data(), code_lengths.size())) {
        error = "invalid code-length table in deflate stream";
        return false;
    }

    std::vector<std::uint8_t> lengths(hlit + hdist, 0);
    std::size_t written = 0;
    while (written < lengths.size()) {
        int symbol = 0;
        if (!code_tree.decode(in, symbol)) {
            error = "deflate stream ended reading code lengths";
            return false;
        }
        if (symbol < 16) {
            lengths[written++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint32_t repeat = 0;
        std::uint8_t value = 0;
        if (symbol == 16) {
            if (written == 0) {
                error = "deflate code-length repeat with nothing to repeat";
                return false;
            }
            value = lengths[written - 1];
            std::uint32_t extra = 0;
            if (!in.bits(2, extra)) {
                error = "deflate stream ended in a code-length repeat";
                return false;
            }
            repeat = 3 + extra;
        } else if (symbol == 17) {
            std::uint32_t extra = 0;
            if (!in.bits(3, extra)) {
                error = "deflate stream ended in a code-length repeat";
                return false;
            }
            repeat = 3 + extra;
        } else {
            std::uint32_t extra = 0;
            if (!in.bits(7, extra)) {
                error = "deflate stream ended in a code-length repeat";
                return false;
            }
            repeat = 11 + extra;
        }
        if (written + repeat > lengths.size()) {
            error = "deflate code-length repeat runs past the table";
            return false;
        }
        for (std::uint32_t i = 0; i < repeat; ++i) {
            lengths[written++] = value;
        }
    }

    if (!lit.build(lengths.data(), hlit) || !dist.build(lengths.data() + hlit, hdist)) {
        error = "invalid Huffman table in deflate stream";
        return false;
    }
    return true;
}

// Which entry in an archive is the ROM. Scored rather than "the first one"
// because archives routinely carry a readme or a .nfo alongside the image.
int extension_score(const std::string& name) {
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto dot = lower.rfind('.');
    if (dot == std::string::npos) {
        return 0;
    }
    const auto ext = lower.substr(dot);
    if (ext == ".md" || ext == ".gen" || ext == ".smd" || ext == ".bin" || ext == ".gg" ||
        ext == ".sms" || ext == ".32x" || ext == ".68k") {
        return 2;
    }
    if (ext == ".txt" || ext == ".nfo" || ext == ".diz" || ext == ".jpg" || ext == ".png") {
        return -1;
    }
    return 0;
}

}  // namespace

bool looks_like_zip(const std::vector<std::uint8_t>& data) noexcept {
    return data.size() >= 4 && data[0] == 'P' && data[1] == 'K' &&
           ((data[2] == 0x03 && data[3] == 0x04) || (data[2] == 0x05 && data[3] == 0x06));
}

bool inflate_raw(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
                 std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    out.reserve(expected_size);
    BitReader in(data, size);
    for (;;) {
        std::uint32_t final_block = 0;
        std::uint32_t type = 0;
        if (!in.bits(1, final_block) || !in.bits(2, type)) {
            error = "deflate stream ended at a block header";
            return false;
        }
        if (type == 0) {
            in.align_to_byte();
            const auto at = in.byte_position();
            if (at + 4 > size) {
                error = "deflate stored block header runs past the end";
                return false;
            }
            const std::uint16_t len = read_u16(data + at);
            const std::uint16_t nlen = read_u16(data + at + 2);
            if (static_cast<std::uint16_t>(~len) != nlen) {
                error = "deflate stored block length check failed";
                return false;
            }
            if (at + 4 + len > size) {
                error = "deflate stored block runs past the end";
                return false;
            }
            out.insert(out.end(), data + at + 4, data + at + 4 + len);
            in.skip_bytes(4 + len);
        } else if (type == 1 || type == 2) {
            Huffman lit;
            Huffman dist;
            const bool ok = type == 1 ? build_fixed_tables(lit, dist)
                                      : read_dynamic_tables(in, lit, dist, error);
            if (!ok) {
                if (type == 1) {
                    error = "could not build the fixed Huffman tables";
                }
                return false;
            }
            if (!inflate_block_data(in, lit, dist, out, error)) {
                return false;
            }
        } else {
            error = "reserved deflate block type";
            return false;
        }
        if (final_block != 0) {
            break;
        }
        if (in.exhausted()) {
            error = "deflate stream ended before the final block";
            return false;
        }
    }
    if (expected_size != 0 && out.size() != expected_size) {
        error = "decompressed size does not match the archive's declared size";
        return false;
    }
    return true;
}

bool extract_rom_from_zip(const std::vector<std::uint8_t>& archive, std::vector<std::uint8_t>& out,
                          std::string& entry_name, std::string& error) {
    constexpr std::size_t kEocdSize = 22;
    if (archive.size() < kEocdSize) {
        error = "file is too small to be a zip archive";
        return false;
    }
    // The end-of-central-directory record is last, but a zip comment can follow
    // it, so it has to be searched for backwards.
    std::size_t eocd = archive.size();
    const std::size_t limit = archive.size() > 0xFFFF + kEocdSize
                                  ? archive.size() - (0xFFFF + kEocdSize)
                                  : 0;
    for (std::size_t i = archive.size() - kEocdSize + 1; i-- > limit;) {
        if (archive[i] == 'P' && archive[i + 1] == 'K' && archive[i + 2] == 0x05 &&
            archive[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == archive.size()) {
        error = "no zip end-of-central-directory record found";
        return false;
    }

    const std::uint16_t entries = read_u16(archive.data() + eocd + 10);
    const std::uint32_t directory_offset = read_u32(archive.data() + eocd + 16);
    if (entries == 0) {
        error = "the zip archive is empty";
        return false;
    }
    if (directory_offset == 0xFFFFFFFFu) {
        error = "zip64 archives are not supported";
        return false;
    }

    struct Candidate {
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t compressed = 0;
        std::uint32_t uncompressed = 0;
        std::uint32_t local_offset = 0;
        int score = 0;
    };
    Candidate best;
    bool have_best = false;

    std::size_t at = directory_offset;
    for (std::uint16_t i = 0; i < entries; ++i) {
        if (at + 46 > archive.size() || read_u32(archive.data() + at) != 0x02014B50u) {
            break;
        }
        Candidate entry;
        entry.method = read_u16(archive.data() + at + 10);
        entry.compressed = read_u32(archive.data() + at + 20);
        entry.uncompressed = read_u32(archive.data() + at + 24);
        const std::uint16_t name_length = read_u16(archive.data() + at + 28);
        const std::uint16_t extra_length = read_u16(archive.data() + at + 30);
        const std::uint16_t comment_length = read_u16(archive.data() + at + 32);
        entry.local_offset = read_u32(archive.data() + at + 42);
        if (at + 46 + name_length > archive.size()) {
            break;
        }
        entry.name.assign(reinterpret_cast<const char*>(archive.data() + at + 46), name_length);
        at += 46u + name_length + extra_length + comment_length;

        // Directories and anything that is plainly not a cartridge image.
        if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\')) {
            continue;
        }
        if (entry.uncompressed == 0) {
            continue;
        }
        entry.score = extension_score(entry.name);
        if (entry.score < 0) {
            continue;
        }
        const bool better = !have_best || entry.score > best.score ||
                            (entry.score == best.score && entry.uncompressed > best.uncompressed);
        if (better) {
            best = entry;
            have_best = true;
        }
    }

    if (!have_best) {
        error = "no ROM-looking file inside the archive";
        return false;
    }
    if (best.method != 0 && best.method != 8) {
        error = "unsupported zip compression method (only stored and deflate are handled)";
        return false;
    }

    const std::size_t local = best.local_offset;
    if (local + 30 > archive.size() || read_u32(archive.data() + local) != 0x04034B50u) {
        error = "the archive's local file header is missing or malformed";
        return false;
    }
    const std::uint16_t local_name = read_u16(archive.data() + local + 26);
    const std::uint16_t local_extra = read_u16(archive.data() + local + 28);
    const std::size_t data_at = local + 30u + local_name + local_extra;
    if (data_at + best.compressed > archive.size()) {
        error = "the archive's file data runs past the end of the file";
        return false;
    }

    entry_name = best.name;
    if (best.method == 0) {
        out.assign(archive.begin() + static_cast<std::ptrdiff_t>(data_at),
                   archive.begin() + static_cast<std::ptrdiff_t>(data_at + best.compressed));
        return true;
    }
    return inflate_raw(archive.data() + data_at, best.compressed, best.uncompressed, out, error);
}

bool read_rom_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                   std::string& error, std::string* entry_name) {
    if (entry_name != nullptr) {
        entry_name->clear();
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.empty()) {
        error = path.string() + " is empty";
        return false;
    }
    if (!looks_like_zip(out)) {
        return true;
    }
    std::vector<std::uint8_t> inner;
    std::string name;
    if (!extract_rom_from_zip(out, inner, name, error)) {
        error = path.filename().string() + ": " + error;
        return false;
    }
    out = std::move(inner);
    if (entry_name != nullptr) {
        *entry_name = name;
    }
    return true;
}

}  // namespace mercury::core
