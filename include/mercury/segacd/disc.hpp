#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mercury/core/chd.hpp"

namespace mercury::segacd {

// A compact disc, as the drive sees it: a table of contents and a way to read
// any sector of it.
//
// ADDRESSES ARE LOGICAL BLOCKS FROM THE FIRST DATA SECTOR. The drive and the
// BIOS talk in minutes:seconds:frames, where 00:02:00 is block 0 - the two
// seconds before it are the lead-in pause that every disc starts with. So
// MSF = LBA + 150, and every conversion in this code goes through that one
// offset.
//
// The image formats are the ones Sega CD libraries actually come in:
//   - .cue with its .bin files, single-file or one file per track (Redump);
//   - a bare .iso of the data track (2048 bytes a sector), or a raw .bin with
//     the sector sync pattern (2352), with a .cue alongside picked up if there
//     is one so the audio tracks come with it;
//   - .chd, MAME's compressed container, through mercury::core::Chd.
//
// A CHD track is addressed by frame rather than by byte, and its frames are
// NOT where the disc says they are: tracks are padded to a multiple of four
// inside the file. Each track therefore carries both positions.
class Disc {
  public:
    enum class TrackType : std::uint8_t {
        audio = 0,
        mode1 = 1,
        mode2 = 2,
    };

    struct Track {
        TrackType type = TrackType::audio;
        // First block of the track proper (its INDEX 01), and one past its
        // last. The pause before a track - its INDEX 00, or a PREGAP - belongs
        // to no playable position, so it lies between the previous track's end
        // and this one's start.
        int start = 0;
        int end = 0;
        // Which open file holds this track, and the block number that
        // corresponds to the first byte of that file. A PREGAP is silence that
        // is NOT in the file, so it pushes this later than the file's own
        // position on the disc.
        int file = -1;
        int file_lba0 = 0;
        // Bytes per block in the file: 2048 for a cooked data track, 2352
        // for raw data or audio.
        int sector_size = 2352;
        // Where in the file the audio data starts - non-zero for a WAVE file,
        // whose header comes first.
        std::uint64_t data_offset = 0;
        // For a CHD: the frame inside the container that `file_lba0` maps to.
        // -1 when the track lives in an ordinary file.
        int chd_frame0 = -1;
    };

    // Is this file a Sega CD image rather than a cartridge? By extension for
    // .cue, .iso and .chd; a .bin could be either, so for that it looks at the
    // first bytes - a disc starts with the CD sync pattern (raw) or with
    // SEGADISCSYSTEM (cooked), and no cartridge does.
    [[nodiscard]] static bool is_disc_image(const std::filesystem::path& path);

    [[nodiscard]] bool open(const std::filesystem::path& image, std::string& error);
    void close();
    [[nodiscard]] bool loaded() const noexcept { return !tracks_.empty(); }

    [[nodiscard]] int track_count() const noexcept { return static_cast<int>(tracks_.size()); }
    [[nodiscard]] const Track& track(int index) const { return tracks_.at(static_cast<std::size_t>(index)); }
    // One past the last block on the disc: where the lead-out begins.
    [[nodiscard]] int lead_out() const noexcept { return lead_out_; }
    // Which track holds a block, or track_count() when it is past the end.
    // Blocks in the pause before a track count as that track's.
    [[nodiscard]] int track_at(int lba) const noexcept;

    // The 2048 bytes of user data in a data block, and for a Mode 2 block its
    // four-byte sub-header as well (pass nullptr when it is not wanted).
    // Returns false for a block that is not in a data track or cannot be read,
    // leaving the buffer zeroed rather than stale.
    bool read_data(int lba, std::uint8_t* out, std::uint8_t* subheader = nullptr);

    // One block of CD audio: 588 stereo frames, little-endian 16-bit as they
    // sit in the file, interleaved left then right. Silence outside an audio
    // track, including in a track's pause.
    bool read_audio(int lba, std::int16_t* out1176);

    // The first 0x210 bytes of the data track - the Sega CD header the BIOS
    // checks, "SEGADISCSYSTEM" first, and the security code after it. Empty
    // until a disc is open.
    [[nodiscard]] const std::vector<std::uint8_t>& header() const noexcept { return header_; }
    // The disc's region, from the header: 'U', 'E' or 'J', or 0 if the header
    // does not say. It picks which BIOS a disc should boot with.
    [[nodiscard]] char region() const noexcept;
    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::string describe() const;

  private:
    bool open_cue(const std::filesystem::path& cue, std::string& error);
    bool open_chd(const std::filesystem::path& image, std::string& error);
    bool open_single(const std::filesystem::path& image, std::string& error);
    // One 2448-byte frame out of the container, or false. The buffer is the
    // caller's; a CHD keeps one hunk of eight frames cached behind this.
    bool read_chd_frame(const Track& track, int lba, std::uint8_t* frame2448);
    int add_file(const std::filesystem::path& path);
    // The file in the same folder whose name most nearly matches, for a cue
    // sheet that disagrees with the tracks beside it. Empty when there is no
    // clear single answer.
    [[nodiscard]] std::filesystem::path nearest_file(const std::filesystem::path& wanted) const;
    bool read_bytes(int file, std::uint64_t offset, void* out, std::size_t size);
    void load_header();

    std::vector<Track> tracks_;
    int lead_out_ = 0;
    // Owned by pointer so a Disc can be moved without re-opening its files.
    std::vector<std::unique_ptr<std::ifstream>> files_;
    std::vector<std::filesystem::path> file_paths_;
    std::vector<std::uint8_t> header_;
    // Set only for a .chd. Held by pointer so the header does not have to
    // pull the whole CHD reader in, and so a Disc stays movable.
    std::unique_ptr<mercury::core::Chd> chd_;
};

// MSF <-> block conversions, shared by the drive and the tests. MSF values are
// plain numbers here, not BCD.
[[nodiscard]] constexpr int msf_to_lba(int minutes, int seconds, int frames) noexcept {
    return (minutes * 60 + seconds) * 75 + frames - 150;
}

}  // namespace mercury::segacd
