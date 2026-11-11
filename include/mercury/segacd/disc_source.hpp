#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mercury::segacd {

// A disc that is not an image file: a real one in an optical drive.
//
// The core never talks to a drive itself - that is platform code, and the
// core has none. A front end supplies a DiscSourceOpener (see
// Disc::set_source_opener) and Disc reads every block through this
// interface instead of through its files, so the drive, the BIOS search,
// disc swaps and the CD audio player all work unchanged on either.
class DiscSource {
  public:
    struct Track {
        bool audio = false;
        // The first block of the track proper (its INDEX 01), as a logical
        // block number from the first data block - MSF 00:02:00 is block 0.
        int start = 0;
    };

    virtual ~DiscSource() = default;

    // The table of contents, tracks in disc order, and where the lead-out
    // begins. Fixed for as long as the source is open.
    [[nodiscard]] virtual const std::vector<Track>& tracks() const = 0;
    [[nodiscard]] virtual int lead_out() const = 0;

    // The 2048 bytes of user data of a Mode 1 block.
    virtual bool read_data(int lba, std::uint8_t* out2048) = 0;
    // One block of CD audio: 2352 bytes, 16-bit little-endian stereo, as a
    // drive returns it and as it sits in a .bin.
    virtual bool read_audio(int lba, std::uint8_t* out2352) = 0;

    // For messages and the load summary: "CD drive E:".
    [[nodiscard]] virtual std::string describe() const = 0;
};

// How a front end turns a path into a source. `claims` must be cheap - it is
// asked about every path before it is opened, to tell a drive from a file.
// `open` either returns a source or returns null with `error` saying why
// ("no disc in drive E:").
struct DiscSourceOpener {
    std::function<bool(const std::filesystem::path&)> claims;
    std::function<std::unique_ptr<DiscSource>(const std::filesystem::path&, std::string& error)> open;
};

}  // namespace mercury::segacd
