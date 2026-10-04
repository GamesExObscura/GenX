#include "mercury/core/chd.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "mercury/core/flac.hpp"
#include "mercury/core/lzma.hpp"
#include "mercury/core/zip_archive.hpp"

namespace mercury::core {

namespace {

std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t be24(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
           p[2];
}

std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t be48(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 6; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

std::uint64_t be64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

// CRC-16/CCITT-FALSE: polynomial $1021, seeded all-ones, most significant bit
// first, no final inversion. The map carries one of these over its own
// decoded bytes, which is what makes "the map decoded correctly" a fact
// rather than a hope.
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u)
                                       : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

// MOST SIGNIFICANT BIT FIRST, which is the opposite of DEFLATE's bit order
// next door in zip_archive.cpp. Both are in this project and they are not
// interchangeable; reading one with the other's reader decodes a few symbols
// and then diverges.
class BitReader {
  public:
    BitReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] std::uint32_t peek(int count) noexcept {
        if (count == 0) {
            return 0;
        }
        while (bits_ <= 24) {
            const int shift = 24 - bits_;
            if (offset_ < size_) {
                buffer_ |= static_cast<std::uint32_t>(data_[offset_]) << shift;
            } else {
                overflow_ = true;
            }
            ++offset_;
            bits_ += 8;
        }
        return buffer_ >> (32 - count);
    }

    void remove(int count) noexcept {
        buffer_ = count >= 32 ? 0 : (buffer_ << count);
        bits_ -= count;
    }

    [[nodiscard]] std::uint32_t read(int count) noexcept {
        const auto value = peek(count);
        remove(count);
        return value;
    }

    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    std::uint32_t buffer_ = 0;
    int bits_ = 0;
    bool overflow_ = false;
};

// The Huffman decoder the map's compression types are coded with. Small and
// fixed: sixteen symbols, at most eight bits each, so the lookup is a flat
// 256-entry table.
//
// The canonical code assignment is not the usual one and cannot be swapped
// for it: lengths are walked from 32 DOWN to 1, halving as it goes, which
// gives the same codes MAME's encoder produced. Doing it the familiar way
// round yields a self-consistent but different tree, and the map then decodes
// to rubbish that still passes every check except the CRC.
class MapHuffman {
  public:
    static constexpr std::uint32_t kNumCodes = 16;
    static constexpr int kMaxBits = 8;

    [[nodiscard]] bool import_rle(BitReader& in) {
        // Four bits per length, because maxbits is 8.
        constexpr int kNumBits = 4;
        std::uint32_t node = 0;
        while (node < kNumCodes) {
            const int length = static_cast<int>(in.read(kNumBits));
            if (length != 1) {
                lengths_[node++] = static_cast<std::uint8_t>(length);
                continue;
            }
            // A 1 is the escape. A second 1 means a single code of length 1;
            // anything else is that length repeated, three more times than
            // the count says.
            const int repeated = static_cast<int>(in.read(kNumBits));
            if (repeated == 1) {
                lengths_[node++] = 1;
                continue;
            }
            std::uint32_t count = in.read(kNumBits) + 3;
            if (count + node > kNumCodes) {
                return false;
            }
            while (count-- != 0) {
                lengths_[node++] = static_cast<std::uint8_t>(repeated);
            }
        }
        return assign_codes() && build_lookup();
    }

    [[nodiscard]] std::uint32_t decode_one(BitReader& in) const noexcept {
        const auto window = in.peek(kMaxBits);
        const auto entry = lookup_[window];
        in.remove(entry & 0x1F);
        return entry >> 5;
    }

  private:
    [[nodiscard]] bool assign_codes() {
        std::array<std::uint32_t, 33> histogram{};
        for (std::uint32_t code = 0; code < kNumCodes; ++code) {
            if (lengths_[code] > kMaxBits) {
                return false;
            }
            ++histogram[lengths_[code]];
        }
        std::uint32_t start = 0;
        for (int length = 32; length > 0; --length) {
            const std::uint32_t next = (start + histogram[length]) >> 1;
            if (length != 1 && next * 2 != start + histogram[length]) {
                return false;  // the lengths do not form a tree
            }
            histogram[length] = start;
            start = next;
        }
        for (std::uint32_t code = 0; code < kNumCodes; ++code) {
            if (lengths_[code] > 0) {
                bits_[code] = histogram[lengths_[code]]++;
            }
        }
        return true;
    }

    [[nodiscard]] bool build_lookup() {
        lookup_.fill(0);
        for (std::uint32_t code = 0; code < kNumCodes; ++code) {
            const int length = lengths_[code];
            if (length == 0) {
                continue;
            }
            const int shift = kMaxBits - length;
            const std::uint32_t first = bits_[code] << shift;
            const std::uint32_t last = ((bits_[code] + 1) << shift) - 1;
            if (last >= lookup_.size() || last < first) {
                return false;
            }
            const auto value = static_cast<std::uint16_t>((code << 5) | (length & 0x1F));
            for (std::uint32_t at = first; at <= last; ++at) {
                lookup_[at] = value;
            }
        }
        return true;
    }

    std::array<std::uint8_t, kNumCodes> lengths_{};
    std::array<std::uint32_t, kNumCodes> bits_{};
    std::array<std::uint16_t, 1u << kMaxBits> lookup_{};
};

// What a hunk's map entry says was done to it.
enum : std::uint8_t {
    kTypeCodec0 = 0,
    kTypeCodec1 = 1,
    kTypeCodec2 = 2,
    kTypeCodec3 = 3,
    kTypeNone = 4,
    kTypeSelf = 5,
    kTypeParent = 6,
    kTypeRleSmall = 7,
    kTypeRleLarge = 8,
    kTypeSelf0 = 9,
    kTypeSelf1 = 10,
    kTypeParentSelf = 11,
    kTypeParent0 = 12,
    kTypeParent1 = 13,
};

constexpr std::uint32_t four_cc(const char* text) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
}

// The codecs a CD CHD is made with. Each one compresses the sector half of a
// hunk with its own scheme and the subcode half with DEFLATE.
constexpr std::uint32_t kCodecNone = 0;
constexpr std::uint32_t kCodecCdZl = four_cc("cdzl");
constexpr std::uint32_t kCodecCdLz = four_cc("cdlz");
constexpr std::uint32_t kCodecCdFl = four_cc("cdfl");
constexpr std::uint32_t kCodecZlib = four_cc("zlib");
constexpr std::uint32_t kCodecLzma = four_cc("lzma");

// Metadata tags. Two kinds of CD track entry exist and a file may carry
// either; the second adds the pregap and postgap fields.
constexpr std::uint32_t kTagTrack = four_cc("CHTR");
constexpr std::uint32_t kTagTrack2 = four_cc("CHT2");

// How many bytes of a 2352-byte frame a track of this type actually uses.
// Everything here is stored raw in a CD CHD, so the frame on disk is always
// 2352 regardless; this is the size the DISC presents, which is what decides
// how a data track is read.
int sector_size_of(const std::string& type) {
    if (type == "MODE1" || type == "MODE2_FORM1") return 2048;
    if (type == "MODE2" || type == "MODE2_FORM_MIX") return 2336;
    if (type == "MODE2_FORM2") return 2324;
    return 2352;  // MODE1_RAW, MODE2_RAW, AUDIO
}

// Pull "KEY:value" out of a metadata string.
std::string field_of(const std::string& text, const std::string& key) {
    const auto at = text.find(key + ":");
    if (at == std::string::npos) {
        return {};
    }
    const auto from = at + key.size() + 1;
    const auto to = text.find(' ', from);
    return text.substr(from, to == std::string::npos ? to : to - from);
}

int int_field(const std::string& text, const std::string& key) {
    const auto value = field_of(text, key);
    return value.empty() ? 0 : std::atoi(value.c_str());
}

}  // namespace

Chd::~Chd() = default;
Chd::Chd(Chd&&) noexcept = default;
Chd& Chd::operator=(Chd&&) noexcept = default;

void Chd::close() {
    file_.close();
    map_.clear();
    tracks_.clear();
    hunk_cache_.clear();
    cached_hunk_ = 0xFFFFFFFFu;
}

bool Chd::read_at(std::uint64_t offset, void* out, std::size_t size) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return file_.gcount() == static_cast<std::streamsize>(size);
}

bool Chd::open(const std::filesystem::path& path, std::string& error) {
    close();
    path_ = path;
    file_.open(path, std::ios::binary);
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    if (!read_header(error) || !read_metadata(error) || !read_map(error)) {
        close();
        return false;
    }
    return true;
}

bool Chd::read_header(std::string& error) {
    std::array<std::uint8_t, 124> header{};
    if (!read_at(0, header.data(), header.size())) {
        error = "the file is too short to be a CHD";
        return false;
    }
    if (std::memcmp(header.data(), "MComprHD", 8) != 0) {
        error = "not a CHD: the file does not begin with MComprHD";
        return false;
    }
    const auto length = be32(&header[8]);
    const auto version = be32(&header[12]);
    // Only version 5. Versions 1 to 4 are a different header and a different
    // map, and nothing in this decade writes them - chdman has produced v5
    // since 2012. Saying which version it is beats "unsupported file".
    if (version != 5 || length != 124) {
        error = "CHD version " + std::to_string(version) +
                " is not supported - only version 5 is, which is what chdman has written "
                "since 2012";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        compressors_[static_cast<std::size_t>(i)] = be32(&header[16 + i * 4]);
    }
    logical_bytes_ = be64(&header[32]);
    map_offset_ = be64(&header[40]);
    meta_offset_ = be64(&header[48]);
    hunk_bytes_ = be32(&header[56]);
    unit_bytes_ = be32(&header[60]);
    if (hunk_bytes_ == 0 || unit_bytes_ == 0 || (hunk_bytes_ % unit_bytes_) != 0) {
        error = "the CHD header's hunk and unit sizes do not agree";
        return false;
    }
    return true;
}

std::string Chd::codec_names() const {
    std::string out;
    for (const auto codec : compressors_) {
        if (codec == 0) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto c = static_cast<char>((codec >> shift) & 0xFF);
            if (c != ' ') {
                out.push_back(c);
            }
        }
    }
    return out.empty() ? "none" : out;
}

std::map<std::string, std::uint32_t> Chd::hunk_tally() const {
    std::map<std::string, std::uint32_t> out;
    const auto name_of = [this](std::uint8_t type) -> std::string {
        switch (type) {
            case kTypeNone: return "uncompressed";
            case kTypeSelf: return "same as another hunk";
            case kTypeParent: return "from the parent file";
            default: break;
        }
        if (type >= compressors_.size()) {
            return "unknown";
        }
        const auto codec = compressors_[type];
        std::string text;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto c = static_cast<char>((codec >> shift) & 0xFF);
            if (c != ' ') text.push_back(c);
        }
        return text.empty() ? "none" : text;
    };
    for (const auto& entry : map_) {
        ++out[name_of(entry.compression)];
    }
    return out;
}

bool Chd::read_metadata(std::string& error) {
    // TRACKS ARE PADDED TO FOUR FRAMES INSIDE THE FILE and are not padded on
    // the disc. Two running totals, therefore: where a track sits in the CHD
    // and where it sits on the disc, and they drift apart by up to three
    // frames per track. Reading a track at its disc position is how every
    // track after the first comes out as the wrong data.
    constexpr int kTrackPadding = 4;
    int chd_frame = 0;
    int disc_frame = 0;
    std::uint64_t offset = meta_offset_;
    while (offset != 0) {
        std::array<std::uint8_t, 16> entry{};
        if (!read_at(offset, entry.data(), entry.size())) {
            break;
        }
        const auto tag = be32(&entry[0]);
        const auto length = be24(&entry[5]);
        const auto next = be64(&entry[8]);
        if (tag == kTagTrack || tag == kTagTrack2) {
            std::string text(length, '\0');
            if (!read_at(offset + 16, text.data(), length)) {
                break;
            }
            // The strings are NUL terminated inside their declared length.
            if (const auto nul = text.find('\0'); nul != std::string::npos) {
                text.resize(nul);
            }
            ChdTrack track;
            track.number = int_field(text, "TRACK");
            track.type = field_of(text, "TYPE");
            track.subtype = field_of(text, "SUBTYPE");
            track.frames = int_field(text, "FRAMES");
            track.pregap = int_field(text, "PREGAP");
            track.postgap = int_field(text, "POSTGAP");
            track.sector_size = sector_size_of(track.type);
            track.audio = track.type == "AUDIO";
            track.chd_first_frame = chd_frame;
            track.disc_first_frame = disc_frame;
            chd_frame += track.frames;
            // Round the CHD position up to the next multiple of four.
            chd_frame = ((chd_frame + kTrackPadding - 1) / kTrackPadding) * kTrackPadding;
            disc_frame += track.frames;
            tracks_.push_back(track);
        }
        offset = next;
    }
    if (tracks_.empty()) {
        error = "the CHD has no CD track metadata - it is probably a hard disk image";
        return false;
    }
    std::sort(tracks_.begin(), tracks_.end(),
              [](const ChdTrack& a, const ChdTrack& b) { return a.number < b.number; });
    return true;
}

bool Chd::read_map(std::string& error) {
    const std::uint32_t hunks =
        static_cast<std::uint32_t>((logical_bytes_ + hunk_bytes_ - 1) / hunk_bytes_);
    map_.assign(hunks, MapEntry{});

    const bool compressed = compressors_[0] != kCodecNone;
    if (!compressed) {
        // A plain array of hunk positions, in units of the hunk size.
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(hunks) * 4);
        if (!read_at(map_offset_, raw.data(), raw.size())) {
            error = "the CHD's map runs past the end of the file";
            return false;
        }
        for (std::uint32_t i = 0; i < hunks; ++i) {
            map_[i].compression = kTypeNone;
            map_[i].length = hunk_bytes_;
            map_[i].offset = static_cast<std::uint64_t>(be32(&raw[i * 4])) * hunk_bytes_;
        }
        return true;
    }

    std::array<std::uint8_t, 16> head{};
    if (!read_at(map_offset_, head.data(), head.size())) {
        error = "the CHD's map header runs past the end of the file";
        return false;
    }
    const auto map_bytes = be32(&head[0]);
    const auto first_offset = be48(&head[4]);
    const auto map_crc = be16(&head[10]);
    const int length_bits = head[12];
    const int self_bits = head[13];
    const int parent_bits = head[14];
    if (length_bits > 32 || self_bits > 32 || parent_bits > 32) {
        error = "the CHD's map header declares impossible field widths";
        return false;
    }
    std::vector<std::uint8_t> raw(map_bytes);
    if (!read_at(map_offset_ + 16, raw.data(), raw.size())) {
        error = "the CHD's compressed map runs past the end of the file";
        return false;
    }

    BitReader in(raw.data(), raw.size());
    MapHuffman huffman;
    if (!huffman.import_rle(in)) {
        error = "the CHD's map has a malformed Huffman tree";
        return false;
    }

    // Pass one: the compression type of every hunk, run-length coded on top
    // of the Huffman coding because long runs of one type are the norm.
    std::uint32_t repeat = 0;
    std::uint8_t last = 0;
    for (std::uint32_t hunk = 0; hunk < hunks; ++hunk) {
        if (repeat > 0) {
            map_[hunk].compression = last;
            --repeat;
            continue;
        }
        const auto symbol = static_cast<std::uint8_t>(huffman.decode_one(in));
        if (symbol == kTypeRleSmall) {
            map_[hunk].compression = last;
            repeat = 2 + huffman.decode_one(in);
        } else if (symbol == kTypeRleLarge) {
            map_[hunk].compression = last;
            repeat = 2 + 16 + (huffman.decode_one(in) << 4);
            repeat += huffman.decode_one(in);
        } else {
            last = symbol;
            map_[hunk].compression = symbol;
        }
    }

    // Pass two: the length, position and checksum of each, with the position
    // running forward from the first one.
    std::uint64_t current = first_offset;
    std::uint64_t last_self = 0;
    std::uint64_t last_parent = 0;
    for (std::uint32_t hunk = 0; hunk < hunks; ++hunk) {
        auto& entry = map_[hunk];
        std::uint64_t offset = current;
        std::uint32_t length = 0;
        std::uint16_t crc = 0;
        switch (entry.compression) {
            case kTypeCodec0:
            case kTypeCodec1:
            case kTypeCodec2:
            case kTypeCodec3:
                length = in.read(length_bits);
                current += length;
                crc = static_cast<std::uint16_t>(in.read(16));
                break;
            case kTypeNone:
                length = hunk_bytes_;
                current += length;
                crc = static_cast<std::uint16_t>(in.read(16));
                break;
            case kTypeSelf:
                offset = in.read(self_bits);
                last_self = offset;
                break;
            case kTypeParent:
                offset = in.read(parent_bits);
                last_parent = offset;
                break;
            case kTypeSelf1:
                ++last_self;
                [[fallthrough]];
            case kTypeSelf0:
                entry.compression = kTypeSelf;
                offset = last_self;
                break;
            case kTypeParentSelf:
                entry.compression = kTypeParent;
                offset = (static_cast<std::uint64_t>(hunk) * hunk_bytes_) / unit_bytes_;
                last_parent = offset;
                break;
            case kTypeParent1:
                last_parent += hunk_bytes_ / unit_bytes_;
                [[fallthrough]];
            case kTypeParent0:
                entry.compression = kTypeParent;
                offset = last_parent;
                break;
            default:
                error = "the CHD's map holds an unknown hunk type";
                return false;
        }
        entry.length = length;
        entry.offset = offset;
        entry.crc = crc;
    }

    // THE MAP CHECKS ITSELF. Rebuild the twelve-byte-per-hunk form the CRC
    // was taken over and compare. Everything above - the bit order, the
    // canonical code assignment, the run-length rules, the field widths -
    // has to be right for this to match, which is why it is worth doing
    // rather than trusting the decode.
    std::vector<std::uint8_t> rebuilt(static_cast<std::size_t>(hunks) * 12);
    for (std::uint32_t hunk = 0; hunk < hunks; ++hunk) {
        auto* at = &rebuilt[static_cast<std::size_t>(hunk) * 12];
        const auto& entry = map_[hunk];
        at[0] = entry.compression;
        at[1] = static_cast<std::uint8_t>(entry.length >> 16);
        at[2] = static_cast<std::uint8_t>(entry.length >> 8);
        at[3] = static_cast<std::uint8_t>(entry.length);
        for (int i = 0; i < 6; ++i) {
            at[4 + i] = static_cast<std::uint8_t>(entry.offset >> (40 - i * 8));
        }
        at[10] = static_cast<std::uint8_t>(entry.crc >> 8);
        at[11] = static_cast<std::uint8_t>(entry.crc);
    }
    if (const auto got = crc16(rebuilt.data(), rebuilt.size()); got != map_crc) {
        char said[128];
        std::snprintf(said, sizeof(said),
                      "the CHD's map failed its own checksum (%04X, expected %04X)", got, map_crc);
        error = said;
        return false;
    }
    return true;
}

// THE ERROR CORRECTION A DATA SECTOR CARRIES, REBUILT.
//
// A Mode 1 sector spends its last 288 bytes on a checksum and two layers of
// parity, and those bytes are entirely derived from the 2064 before them. So
// chdman throws them away along with the twelve-byte sync pattern and writes
// them back on the way out, and a decoder that does not do the same produces
// a hunk that is right everywhere a game looks and wrong in the 288 bytes it
// does not - which is exactly what the hunk's own CRC catches.
//
// Nothing in a Sega CD reads these: the drive corrects with them and hands
// over corrected data. They are here because the CRC is the only end-to-end
// proof this whole decoder is right, and it cannot pass without them.
//
// Two Galois field tables over GF(2^8) with the standard CD polynomial, and
// a checksum with the CD's own reversed polynomial. Both are the format's.
struct EccTables {
    std::array<std::uint8_t, 256> forward{};
    std::array<std::uint8_t, 256> back{};
    std::array<std::uint32_t, 256> edc{};

    EccTables() {
        for (int i = 0; i < 256; ++i) {
            const auto doubled =
                static_cast<std::uint8_t>((i << 1) ^ ((i & 0x80) != 0 ? 0x11D : 0));
            forward[static_cast<std::size_t>(i)] = doubled;
            back[static_cast<std::size_t>(i ^ doubled)] = static_cast<std::uint8_t>(i);
        }
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1) != 0 ? 0xD8018001u : 0u);
            }
            edc[i] = value;
        }
    }
};

const EccTables& ecc_tables() {
    static const EccTables tables;
    return tables;
}

std::uint32_t ecc_edc(const std::uint8_t* data, std::size_t size) {
    const auto& tables = ecc_tables();
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ tables.edc[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}

// One parity layer. The two differ only in how they walk the sector - P down
// the columns, Q along the diagonals - so they are the same routine twice.
void ecc_layer(const std::uint8_t* src, std::uint32_t major_count, std::uint32_t minor_count,
               std::uint32_t major_mult, std::uint32_t minor_inc, std::uint8_t* dest) {
    const auto& tables = ecc_tables();
    const std::uint32_t size = major_count * minor_count;
    for (std::uint32_t major = 0; major < major_count; ++major) {
        std::uint32_t index = (major >> 1) * major_mult + (major & 1);
        std::uint8_t a = 0;
        std::uint8_t b = 0;
        for (std::uint32_t minor = 0; minor < minor_count; ++minor) {
            const std::uint8_t value = src[index];
            index += minor_inc;
            if (index >= size) {
                index -= size;
            }
            a ^= value;
            b ^= value;
            a = tables.forward[a];
        }
        a = tables.back[tables.forward[a] ^ b];
        dest[major] = a;
        dest[major + major_count] = static_cast<std::uint8_t>(a ^ b);
    }
}

void ecc_generate(std::uint8_t* sector) {
    // The checksum covers everything from the sync to the end of the user
    // data; the eight bytes after it are defined to be zero and are part of
    // what the parity is taken over.
    const std::uint32_t edc = ecc_edc(sector, 0x810);
    sector[0x810] = static_cast<std::uint8_t>(edc);
    sector[0x811] = static_cast<std::uint8_t>(edc >> 8);
    sector[0x812] = static_cast<std::uint8_t>(edc >> 16);
    sector[0x813] = static_cast<std::uint8_t>(edc >> 24);
    std::fill(sector + 0x814, sector + 0x81C, std::uint8_t{0});
    ecc_layer(sector + 0xC, 86, 24, 2, 86, sector + 0x81C);
    ecc_layer(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);
}

// A CD hunk is not eight 2448-byte frames in a row. The eight SECTORS come
// first and then the eight SUBCODE blocks, because separating them is what
// lets them compress - subcode is nearly all zeros and sector data is not.
// Every CD codec compresses the two halves independently and this puts them
// back together.
//
// The sync header and the error correction are stripped from sectors that
// have the standard ones, and a bitmap at the front of the hunk says which.
// Both are put back - see ecc_generate.
constexpr std::size_t kFrameBytes = 2448;
constexpr std::size_t kSectorBytes = 2352;
constexpr std::size_t kSubcodeBytes = 96;
constexpr std::uint8_t kSyncHeader[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

bool Chd::load_hunk(std::uint32_t hunk, std::string& error) {
    if (hunk == cached_hunk_) {
        return true;
    }
    if (hunk >= map_.size()) {
        error = "hunk " + std::to_string(hunk) + " is past the end of the CHD";
        return false;
    }
    const auto& entry = map_[hunk];
    hunk_cache_.assign(hunk_bytes_, 0);

    switch (entry.compression) {
        case kTypeNone:
            if (!read_at(entry.offset, hunk_cache_.data(), hunk_bytes_)) {
                error = "a CHD hunk runs past the end of the file";
                return false;
            }
            cached_hunk_ = hunk;
            return true;
        case kTypeSelf:
            // The same bytes as another hunk. Recursion would be unbounded
            // in a malformed file, so it is one step: a self reference to a
            // self reference is not something chdman writes.
            if (entry.offset >= map_.size() || map_[entry.offset].compression == kTypeSelf) {
                error = "a CHD hunk refers to itself";
                return false;
            }
            if (!load_hunk(static_cast<std::uint32_t>(entry.offset), error)) {
                return false;
            }
            cached_hunk_ = hunk;
            return true;
        case kTypeParent:
            error = "this CHD needs its parent file, which GenX does not read";
            return false;
        default:
            break;
    }

    const std::size_t codec_index = entry.compression;
    if (codec_index >= compressors_.size()) {
        error = "a CHD hunk names a codec slot that does not exist";
        return false;
    }
    const auto codec = compressors_[codec_index];
    compressed_.assign(entry.length, 0);
    if (!read_at(entry.offset, compressed_.data(), compressed_.size())) {
        error = "a compressed CHD hunk runs past the end of the file";
        return false;
    }

    const std::size_t frames = hunk_bytes_ / kFrameBytes;
    if (frames * kFrameBytes != hunk_bytes_) {
        error = "this CHD's hunks are not whole CD frames";
        return false;
    }

    std::vector<std::uint8_t> sectors;
    std::vector<std::uint8_t> subcode;

    // CDFL HAS NO WRAPPER AT ALL, unlike the other two.
    //
    // Checked rather than assumed: every cdfl hunk in the file begins with
    // FF F8, which is the FLAC frame sync, at byte zero. It needs neither of
    // the two header fields the others carry - there is no sync header to
    // strip from an audio sector, so no bitmap of which lost one, and the
    // FLAC stream's own framing says where it ends, so no length either.
    if (codec == kCodecCdFl) {
        std::size_t used = 0;
        if (!flac_decode_cd(compressed_.data(), compressed_.size(),
                            frames * kSectorBytes / 4, sectors, used, error)) {
            return false;
        }
        if (const char* set = std::getenv("MERCURY_FLAC_LOG"); set != nullptr && *set != '0') {
            std::fprintf(stderr, "[cdfl] hunk %u: %zu bytes, FLAC used %zu, %zu left for subcode\n",
                         hunk, compressed_.size(), used,
                         used <= compressed_.size() ? compressed_.size() - used : 0);
        }
        if (used > compressed_.size()) {
            error = "the FLAC stream in a CHD hunk ran past the end of it";
            return false;
        }
        const std::size_t sub_left = compressed_.size() - used;
        if (sub_left == 0) {
            subcode.assign(frames * kSubcodeBytes, 0);
        } else if (!inflate_raw(compressed_.data() + used, sub_left, frames * kSubcodeBytes,
                                subcode, error)) {
            return false;
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            auto* out = &hunk_cache_[frame * kFrameBytes];
            std::memcpy(out, &sectors[frame * kSectorBytes], kSectorBytes);
            std::memcpy(out + kSectorBytes, &subcode[frame * kSubcodeBytes], kSubcodeBytes);
        }
        cached_hunk_ = hunk;
        return true;
    }

    // The wrapper cdzl and cdlz share: a bitmap of which sectors lost their
    // sync header, then the compressed length of the sector half, then the
    // two compressed streams back to back.
    const std::size_t ecc_bytes = (frames + 7) / 8;
    const std::size_t length_bytes = hunk_bytes_ < 65536 ? 2 : 3;
    const std::size_t header_bytes = ecc_bytes + length_bytes;
    if (compressed_.size() < header_bytes) {
        error = "a CHD hunk is too short to hold its own header";
        return false;
    }
    std::size_t base_length = 0;
    for (std::size_t i = 0; i < length_bytes; ++i) {
        base_length = (base_length << 8) | compressed_[ecc_bytes + i];
    }
    if (header_bytes + base_length > compressed_.size()) {
        error = "a CHD hunk's two halves do not fit inside it";
        return false;
    }

    const std::uint8_t* base_at = compressed_.data() + header_bytes;
    const std::uint8_t* sub_at = base_at + base_length;
    const std::size_t sub_length = compressed_.size() - header_bytes - base_length;

    // THE SUBCODE HALF IS ALWAYS DEFLATE, whichever codec the sector half
    // used. Only the sector half differs between cdzl, cdlz and cdfl.
    if (codec == kCodecCdZl) {
        if (!inflate_raw(base_at, base_length, frames * kSectorBytes, sectors, error)) {
            return false;
        }
    } else if (codec == kCodecCdLz) {
        // chdman writes no properties byte: it builds the encoder with the
        // library defaults at level 9 and the decoder is expected to know
        // them. lc=3, lp=0, pb=2 are those defaults.
        if (!lzma_decode(base_at, base_length, frames * kSectorBytes, 3, 0, 2, sectors, error)) {
            return false;
        }
    } else {
        // cdlz and cdfl are not decoded yet. Saying which one stops this
        // looking like a broken file.
        char said[96];
        std::snprintf(said, sizeof(said), "this CHD uses the %c%c%c%c codec, which GenX cannot "
                                          "read yet",
                      static_cast<char>((codec >> 24) & 0xFF), static_cast<char>((codec >> 16) & 0xFF),
                      static_cast<char>((codec >> 8) & 0xFF), static_cast<char>(codec & 0xFF));
        error = said;
        return false;
    }
    // A hunk whose subcode is entirely absent is legal: chdman stores
    // nothing at all when there is nothing to store.
    if (sub_length == 0) {
        subcode.assign(frames * kSubcodeBytes, 0);
    } else if (!inflate_raw(sub_at, sub_length, frames * kSubcodeBytes, subcode, error)) {
        return false;
    }
    if (sectors.size() < frames * kSectorBytes || subcode.size() < frames * kSubcodeBytes) {
        error = "a CHD hunk decoded to less data than it should have";
        return false;
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        auto* out = &hunk_cache_[frame * kFrameBytes];
        std::memcpy(out, &sectors[frame * kSectorBytes], kSectorBytes);
        std::memcpy(out + kSectorBytes, &subcode[frame * kSubcodeBytes], kSubcodeBytes);
        // The bitmap says this sector had the standard sync pattern and
        // therefore standard error correction, both of which were thrown
        // away. Put the sync back and rebuild the rest from the data.
        if ((compressed_[frame / 8] & (1u << (frame % 8))) != 0) {
            std::memcpy(out, kSyncHeader, sizeof(kSyncHeader));
            ecc_generate(out);
        }
    }
    cached_hunk_ = hunk;
    return true;
}

bool Chd::read_frame(std::uint32_t frame, std::uint8_t* out, std::string& error) {
    const std::uint32_t per_hunk = hunk_bytes_ / static_cast<std::uint32_t>(kFrameBytes);
    if (per_hunk == 0) {
        error = "this CHD's hunks are smaller than a CD frame";
        return false;
    }
    const std::uint32_t hunk = frame / per_hunk;
    if (!load_hunk(hunk, error)) {
        return false;
    }
    std::memcpy(out, &hunk_cache_[(frame % per_hunk) * kFrameBytes], kFrameBytes);
    return true;
}

bool Chd::raw_hunk(std::uint32_t hunk, std::vector<std::uint8_t>& out, std::string& codec) {
    if (hunk >= map_.size()) {
        return false;
    }
    const auto& entry = map_[hunk];
    codec = "type " + std::to_string(entry.compression);
    if (entry.compression < compressors_.size()) {
        const auto name = compressors_[entry.compression];
        std::string text;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto c = static_cast<char>((name >> shift) & 0xFF);
            if (c != ' ') text.push_back(c);
        }
        if (!text.empty()) codec = text;
    }
    out.assign(entry.length, 0);
    return entry.length != 0 && read_at(entry.offset, out.data(), out.size());
}

bool Chd::verify_hunk(std::uint32_t hunk, bool& ok, std::string& codec, std::string& error) {
    ok = false;
    if (hunk >= map_.size()) {
        error = "no such hunk";
        return false;
    }
    const auto& entry = map_[hunk];
    codec = "type " + std::to_string(entry.compression);
    if (entry.compression < compressors_.size()) {
        std::string text;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto c = static_cast<char>((compressors_[entry.compression] >> shift) & 0xFF);
            if (c != ' ') text.push_back(c);
        }
        if (!text.empty()) codec = text;
    } else if (entry.compression == kTypeNone) {
        codec = "uncompressed";
    } else if (entry.compression == kTypeSelf) {
        codec = "same as another hunk";
    }
    // Only the codec types carry a checksum; a self reference or an
    // uncompressed hunk has nothing to check against.
    if (entry.compression > kTypeCodec3) {
        ok = true;
        return true;
    }
    // A fresh read every time: the cache would otherwise answer for a hunk
    // that was decoded before this check existed.
    cached_hunk_ = 0xFFFFFFFFu;
    if (!load_hunk(hunk, error)) {
        return false;
    }
    ok = crc16(hunk_cache_.data(), hunk_cache_.size()) == entry.crc;
    return true;
}

std::string Chd::describe() const {
    std::string out = "CHD v5, " + std::to_string(hunk_count()) + " hunks of " +
                      std::to_string(hunk_bytes_) + " bytes (" +
                      std::to_string(hunk_bytes_ / (unit_bytes_ != 0 ? unit_bytes_ : 1)) +
                      " frames each), codecs " + codec_names() + "\n";
    for (const auto& track : tracks_) {
        out += "  track " + std::to_string(track.number) + ": " + track.type + "/" +
               track.subtype + " " + std::to_string(track.frames) + " frames, disc " +
               std::to_string(track.disc_first_frame) + ", chd " +
               std::to_string(track.chd_first_frame);
        if (track.pregap != 0) {
            out += ", pregap " + std::to_string(track.pregap);
        }
        out += "\n";
    }
    return out;
}

}  // namespace mercury::core
