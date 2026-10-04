// What is actually inside a CHD.
//
// The map carries a CRC over its own decoded form, so "it opened" already
// means the whole bit-level decode is right. Past that the useful questions
// are which codecs the hunks actually use - a file can name three and use
// two - and whether the tracks line up with the .cue of the same game.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "mercury/core/chd.hpp"
#include "mercury/segacd/disc.hpp"

namespace {

// THE SAME DISC IN TWO FORMATS IS THE ONLY REAL CHECK.
//
// A decoder can be self-consistent and wrong: the map's CRC proves the map,
// and nothing proves the audio. Comparing a .chd against the .cue of the
// same game does - sector for sector, both data and audio, through the same
// Disc that the emulator itself reads through.
int compare(const std::string& left_path, const std::string& right_path, int from, int count) {
    mercury::segacd::Disc left;
    mercury::segacd::Disc right;
    std::string error;
    if (!left.open(left_path, error)) {
        std::printf("cannot open %s: %s\n", left_path.c_str(), error.c_str());
        return 1;
    }
    if (!right.open(right_path, error)) {
        std::printf("cannot open %s: %s\n", right_path.c_str(), error.c_str());
        return 1;
    }
    if (left.track_count() != right.track_count() || left.lead_out() != right.lead_out()) {
        std::printf("the two images disagree about the disc: %d vs %d tracks, lead-out %d vs %d\n",
                    left.track_count(), right.track_count(), left.lead_out(), right.lead_out());
        return 1;
    }
    std::printf("comparing %d blocks from %d, %d tracks, lead-out %d\n", count, from,
                left.track_count(), left.lead_out());

    int data_same = 0, data_diff = 0, audio_same = 0, audio_diff = 0, skipped = 0;
    long audio_samples = 0, audio_exact = 0, audio_high_same = 0, audio_low_zero = 0;
    int first_bad = -1;
    for (int i = 0; i < count; ++i) {
        const int lba = from + i;
        const int index = left.track_at(lba);
        if (index >= left.track_count()) {
            ++skipped;
            continue;
        }
        const bool audio = left.track(index).type == mercury::segacd::Disc::TrackType::audio;
        if (audio) {
            std::int16_t a[1176];
            std::int16_t b[1176];
            const bool ok_a = left.read_audio(lba, a);
            const bool ok_b = right.read_audio(lba, b);
            if (!ok_a || !ok_b) {
                ++skipped;
                continue;
            }
            if (std::memcmp(a, b, sizeof(a)) == 0) {
                ++audio_same;
            } else {
                if (first_bad < 0) {
                    first_bad = lba;
                    std::printf("  first audio difference at block %d:\n    cue:", lba);
                    for (int k = 0; k < 8; ++k) std::printf(" %6d", a[k]);
                    std::printf("\n    chd:");
                    for (int k = 0; k < 8; ++k) std::printf(" %6d", b[k]);
                    std::printf("\n");
                }
                ++audio_diff;
            }
            // MISALIGNED OR IMPRECISE? Two different faults that both show
            // up as "different", and the top eight bits tell them apart: a
            // decoder reading the wrong place gets neither half right, one
            // losing precision gets the top half right every time.
            for (int k = 0; k < 1176; ++k) {
                ++audio_samples;
                if (a[k] == b[k]) {
                    ++audio_exact;
                }
                if ((a[k] & ~0xFF) == (b[k] & ~0xFF)) {
                    ++audio_high_same;
                }
                if ((b[k] & 0xFF) == 0) {
                    ++audio_low_zero;
                }
            }
        } else {
            std::uint8_t a[2048];
            std::uint8_t b[2048];
            const bool ok_a = left.read_data(lba, a);
            const bool ok_b = right.read_data(lba, b);
            if (!ok_a || !ok_b) {
                ++skipped;
                continue;
            }
            if (std::memcmp(a, b, sizeof(a)) == 0) {
                ++data_same;
            } else {
                if (first_bad < 0) {
                    first_bad = lba;
                    std::printf("  first data difference at block %d\n", lba);
                }
                ++data_diff;
            }
        }
    }
    std::printf("data  : %d identical, %d different\n", data_same, data_diff);
    std::printf("audio : %d identical, %d different\n", audio_same, audio_diff);
    if (audio_samples > 0) {
        std::printf("        %ld samples: %ld exact, %ld with the top byte right, %ld whose low "
                    "byte came out zero\n",
                    audio_samples, audio_exact, audio_high_same, audio_low_zero);
    }
    std::printf("skipped (outside a track or unreadable): %d\n", skipped);
    return (data_diff == 0 && audio_diff == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: chd_probe <file.chd> [--frame N] [--hunks]\n");
        return 2;
    }
    const std::string path = argv[1];
    int frame = -1;
    int raw = -1;
    int verify = 0;
    bool hunks = false;
    std::string against;
    int from = 0;
    int count = 2000;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frame" && i + 1 < argc) {
            frame = std::atoi(argv[++i]);
        } else if (arg == "--raw" && i + 1 < argc) {
            raw = std::atoi(argv[++i]);
        } else if (arg == "--verify" && i + 1 < argc) {
            verify = std::atoi(argv[++i]);
        } else if (arg == "--against" && i + 1 < argc) {
            against = argv[++i];
        } else if (arg == "--from" && i + 1 < argc) {
            from = std::atoi(argv[++i]);
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::atoi(argv[++i]);
        } else if (arg == "--hunks") {
            hunks = true;
        }
    }
    if (!against.empty()) {
        return compare(against, path, from, count);
    }

    mercury::core::Chd chd;
    std::string error;
    if (!chd.open(path, error)) {
        std::printf("FAILED: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s", chd.describe().c_str());

    if (hunks) {
        // Which codec each hunk really used. A CD is usually mostly one of
        // them with the audio tracks in another, and the split says how much
        // of the file each decoder is responsible for - and therefore which
        // one is worth writing first.
        for (const auto& [name, count] : chd.hunk_tally()) {
            std::printf("  %-22s %u hunks\n", name.c_str(), count);
        }
    }

    if (verify > 0) {
        // Spread the sample across the whole file rather than taking the
        // first N: the codecs are not evenly mixed, and the data track at
        // the front would hide every audio hunk.
        std::map<std::string, std::pair<int, int>> tally;  // codec -> {passed, failed}
        const std::uint32_t total = chd.hunk_count();
        const std::uint32_t step = total > static_cast<std::uint32_t>(verify)
                                       ? total / static_cast<std::uint32_t>(verify)
                                       : 1;
        for (std::uint32_t h = 0; h < total; h += step) {
            bool ok = false;
            std::string codec;
            if (!chd.verify_hunk(h, ok, codec, error)) {
                std::printf("hunk %u (%s): %s\n", h, codec.c_str(), error.c_str());
                ++tally[codec].second;
                continue;
            }
            (ok ? tally[codec].first : tally[codec].second)++;
        }
        std::printf("checked against each hunk's own CRC:\n");
        for (const auto& [codec, counts] : tally) {
            std::printf("  %-22s %d passed, %d FAILED\n", codec.c_str(), counts.first,
                        counts.second);
        }
        return 0;
    }

    if (raw >= 0) {
        std::vector<std::uint8_t> bytes;
        std::string codec;
        if (!chd.raw_hunk(static_cast<std::uint32_t>(raw), bytes, codec)) {
            std::printf("hunk %d: cannot be read raw\n", raw);
            return 1;
        }
        std::printf("hunk %d is %s, %zu bytes; first 24:\n  ", raw, codec.c_str(), bytes.size());
        for (std::size_t i = 0; i < 24 && i < bytes.size(); ++i) {
            std::printf("%02X ", bytes[i]);
        }
        std::printf("\n");
        // Where the FLAC frames start. How many there are decides how the
        // sector bytes are laid into samples, which nothing else reveals.
        // The sync is FOURTEEN bits, so the second byte is F8 to FB - looking
        // only for F8 hides three quarters of the possible frame starts.
        std::printf("  frame syncs at:");
        int found = 0;
        for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
            if (bytes[i] == 0xFF && (bytes[i + 1] & 0xFC) == 0xF8) {
                std::printf(" %zu", i);
                if (++found >= 12) {
                    std::printf(" ...");
                    break;
                }
            }
        }
        std::printf("\n");
    }

    if (frame >= 0) {
        std::vector<std::uint8_t> buffer(2448);
        if (!chd.read_frame(static_cast<std::uint32_t>(frame), buffer.data(), error)) {
            std::printf("frame %d: %s\n", frame, error.c_str());
            return 1;
        }
        std::printf("frame %d, first 32 bytes:\n  ", frame);
        for (int i = 0; i < 32; ++i) {
            std::printf("%02X ", buffer[static_cast<std::size_t>(i)]);
        }
        std::printf("\n  as text: ");
        for (int i = 16; i < 48; ++i) {
            const auto c = buffer[static_cast<std::size_t>(i)];
            std::printf("%c", c >= 32 && c < 127 ? static_cast<char>(c) : '.');
        }
        std::printf("\n");
    }
    return 0;
}
