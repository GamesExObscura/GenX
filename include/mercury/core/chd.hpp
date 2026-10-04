#pragma once

// CHD (MAME's "Compressed Hunks of Data"), enough of it to read a CD.
//
// Written rather than pulled in, like the DEFLATE next door: the project has
// no third-party dependencies. The format is read out of its published
// description and out of libchdr (BSD licensed) the same way the hardware is
// read out of PicoDrive - for facts, not for code.
//
// WHAT A CD CHD ACTUALLY IS. The file is a list of fixed-size "hunks", each
// separately compressed, plus a map saying where each one lives and which
// codec it used. For a CD the unit is 2448 bytes - a 2352-byte sector with
// its 96 bytes of subcode after it - and a hunk is eight of those. A hunk is
// NOT stored as eight units one after another: the eight sectors come first
// and then the eight subcode blocks, because that separation is what makes
// them compress. The CD codecs compress the two halves independently.
//
// Two more things about CDs specifically, both of which are silent corruption
// if missed: tracks are padded out to a multiple of four frames, so a track's
// position in the file is not its position on the disc; and audio is stored
// big-endian, the opposite way round from a .bin file.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mercury::core {

// One track, as the CHD's own metadata describes it.
struct ChdTrack {
    int number = 0;
    std::string type;      // MODE1_RAW, MODE2_RAW, AUDIO, ...
    std::string subtype;   // NONE, RW, RW_RAW
    int frames = 0;        // frames of actual data
    int pregap = 0;        // frames of pregap INSIDE those, for a v2 entry
    int postgap = 0;
    // Where this track's first frame sits in the CHD's own frame numbering,
    // which includes the four-frame padding between tracks.
    int chd_first_frame = 0;
    // Where it sits on the disc, in absolute frames from the start.
    int disc_first_frame = 0;
    // Bytes of user data in one sector of this track, from its type.
    int sector_size = 0;
    bool audio = false;
};

class Chd {
  public:
    Chd() = default;
    ~Chd();
    Chd(const Chd&) = delete;
    Chd& operator=(const Chd&) = delete;
    Chd(Chd&&) noexcept;
    Chd& operator=(Chd&&) noexcept;

    // Open and read the header, the metadata and the hunk map. The map
    // carries its own CRC, so a successful open means the map really was
    // decoded correctly rather than merely plausibly.
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string& error);
    void close();
    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    [[nodiscard]] const std::vector<ChdTrack>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] std::uint32_t hunk_bytes() const noexcept { return hunk_bytes_; }
    [[nodiscard]] std::uint32_t unit_bytes() const noexcept { return unit_bytes_; }
    [[nodiscard]] std::uint64_t logical_bytes() const noexcept { return logical_bytes_; }
    [[nodiscard]] std::uint32_t hunk_count() const noexcept {
        return static_cast<std::uint32_t>(map_.size());
    }
    // Which codecs the file says it uses, as their four-character names, for
    // reporting what is and is not supported.
    [[nodiscard]] std::string codec_names() const;
    // How many hunks each codec actually accounts for. A file naming three
    // codecs may use only two, and this says which decoder is worth writing
    // first - and, once they are all written, that none was missed.
    [[nodiscard]] std::map<std::string, std::uint32_t> hunk_tally() const;

    // One 2448-byte CD frame - 2352 of sector then 96 of subcode - by its
    // index in the CHD's own frame numbering. False when the hunk holding it
    // cannot be decoded, which is how an unsupported codec surfaces.
    [[nodiscard]] bool read_frame(std::uint32_t frame, std::uint8_t* out, std::string& error);

    [[nodiscard]] std::string describe() const;

    // A hunk exactly as it sits in the file, with the codec that made it.
    // Diagnostic: a compressed stream's own framing is the only way to
    // settle how many bytes of wrapper come before it.
    [[nodiscard]] bool raw_hunk(std::uint32_t hunk, std::vector<std::uint8_t>& out,
                                std::string& codec);

    // THE FILE CHECKS ITS OWN DECOMPRESSION. Every compressed hunk's map
    // entry carries a CRC-16 of the hunk's DECODED bytes, so a decoder can
    // be proved right or wrong against the file alone, with no second
    // implementation and no reference dump to compare against. Returns false
    // only if the hunk could not be decoded at all; `ok` says whether what
    // came out matches what the file says should have.
    [[nodiscard]] bool verify_hunk(std::uint32_t hunk, bool& ok, std::string& codec,
                                   std::string& error);

  private:
    [[nodiscard]] bool read_header(std::string& error);
    [[nodiscard]] bool read_metadata(std::string& error);
    [[nodiscard]] bool read_map(std::string& error);
    [[nodiscard]] bool read_at(std::uint64_t offset, void* out, std::size_t size);
    // Decompress one hunk into hunk_cache_, or return false with a reason.
    [[nodiscard]] bool load_hunk(std::uint32_t hunk, std::string& error);

    struct MapEntry {
        std::uint8_t compression = 0;
        std::uint32_t length = 0;
        std::uint64_t offset = 0;
        std::uint16_t crc = 0;
    };

    std::ifstream file_;
    std::filesystem::path path_;
    std::array<std::uint32_t, 4> compressors_{};
    std::uint64_t logical_bytes_ = 0;
    std::uint64_t map_offset_ = 0;
    std::uint64_t meta_offset_ = 0;
    std::uint32_t hunk_bytes_ = 0;
    std::uint32_t unit_bytes_ = 0;
    std::vector<MapEntry> map_;
    std::vector<ChdTrack> tracks_;

    // One hunk at a time is enough: a CD is read forwards, and eight sectors
    // of look-ahead is what the drive does anyway.
    std::vector<std::uint8_t> hunk_cache_;
    std::uint32_t cached_hunk_ = 0xFFFFFFFFu;
    std::vector<std::uint8_t> compressed_;
};

}  // namespace mercury::core
