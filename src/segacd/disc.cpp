#include "mercury/segacd/disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace mercury::segacd {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

// "mm:ss:ff" into a block count. Returns -1 for anything malformed.
int parse_msf(const std::string& text) {
    int mm = 0, ss = 0, ff = 0;
    char c1 = 0, c2 = 0;
    std::istringstream in(text);
    if (!(in >> mm >> c1 >> ss >> c2 >> ff) || c1 != ':' || c2 != ':') {
        return -1;
    }
    return (mm * 60 + ss) * 75 + ff;
}

constexpr std::uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

}  // namespace

bool Disc::is_disc_image(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    if (extension == ".cue" || extension == ".iso" || extension == ".chd") {
        return true;
    }
    if (extension != ".bin") {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::uint8_t head[16] = {};
    in.read(reinterpret_cast<char*>(head), sizeof(head));
    if (!in) {
        return false;
    }
    return std::memcmp(head, "SEGADISCSYSTEM", 14) == 0 || std::memcmp(head, kSync, sizeof(kSync)) == 0;
}

void Disc::close() {
    tracks_.clear();
    files_.clear();
    file_paths_.clear();
    header_.clear();
    chd_.reset();
    lead_out_ = 0;
}

// A CHD's table of contents, turned into the same Track list every other
// format produces, so nothing downstream has to know which it came from.
//
// TWO POSITIONS PER TRACK AND THEY ARE NOT THE SAME. On the disc a track
// begins where the previous one ended; inside the file it begins at the next
// multiple of four frames. On Sonic CD that difference reaches 46 frames by
// the last track, so reading a track at its disc position gives the wrong
// data for every track but the first.
//
// The pregap is inside the track's frame count and inside the file, which is
// what chdman writes when it takes the pregap from a cue sheet's INDEX 00.
// Checked against the cue of the same disc: track 2 is at 59261 with a
// pregap of 150, and the cue puts its INDEX 01 at 59411.
bool Disc::open_chd(const std::filesystem::path& image, std::string& error) {
    chd_ = std::make_unique<mercury::core::Chd>();
    if (!chd_->open(image, error)) {
        chd_.reset();
        return false;
    }
    int disc_frame = 0;
    for (const auto& source : chd_->tracks()) {
        Track track;
        track.type = source.type == "AUDIO"      ? TrackType::audio
                     : source.type.rfind("MODE2", 0) == 0 ? TrackType::mode2
                                                          : TrackType::mode1;
        // Whatever the declared type, a CHD frame is 2352 bytes of sector
        // plus 96 of subcode. A cooked track stores its user data at the
        // front of that field instead of 16 bytes in, which is exactly the
        // distinction sector_size already carries everywhere else.
        track.sector_size = source.sector_size;
        track.file = -1;
        track.chd_frame0 = source.chd_first_frame;
        track.file_lba0 = disc_frame;
        track.start = disc_frame + source.pregap;
        track.end = disc_frame + source.frames;
        disc_frame = track.end;
        tracks_.push_back(track);
    }
    lead_out_ = disc_frame;
    return true;
}

bool Disc::read_chd_frame(const Track& track, int lba, std::uint8_t* frame2448) {
    if (chd_ == nullptr || track.chd_frame0 < 0 || lba < track.file_lba0) {
        return false;
    }
    const auto frame = static_cast<std::uint32_t>(track.chd_frame0 + (lba - track.file_lba0));
    std::string ignored;
    return chd_->read_frame(frame, frame2448, ignored);
}

int Disc::add_file(const std::filesystem::path& path) {
    auto stream = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*stream) {
        return -1;
    }
    files_.push_back(std::move(stream));
    file_paths_.push_back(path);
    return static_cast<int>(files_.size()) - 1;
}

// A CUE SHEET AND ITS TRACKS CAN DISAGREE ABOUT THE NAME, and the disc is
// perfectly readable anyway.
//
// Supreme Warrior ships as "... (Disc 1) (Fire & Earth) (Sega CD 32X)
// (Track 1).bin" in its cue and "... (Disc 1) (Sega CD 32X) (Track 1).bin" on
// disk: one parenthesised group short. Renaming a set and leaving the cue
// alone is ordinary, and refusing the disc over it loses a game that would
// otherwise run - so when the named file is not there, look for the file in
// the cue's own folder that the name most nearly describes.
//
// The rule is deliberately dull. Split both names into words, count how many
// of the cue's words the candidate has, require the same extension, require a
// UNIQUE best match, and never hand back a file another FILE line already
// took. On the Supreme Warrior discs track 1 beats track 2 by exactly the two
// words that differ, which is the whole point: a track number is a word like
// any other, so the scoring cannot confuse two tracks of the same disc.
std::filesystem::path Disc::nearest_file(const std::filesystem::path& wanted) const {
    std::error_code ignored;
    const auto folder = wanted.parent_path();
    if (folder.empty() || !std::filesystem::is_directory(folder, ignored)) {
        return {};
    }
    const auto words = [](const std::string& text) {
        std::vector<std::string> out;
        std::string word;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            } else if (!word.empty()) {
                out.push_back(word);
                word.clear();
            }
        }
        if (!word.empty()) {
            out.push_back(word);
        }
        return out;
    };
    const auto extension = lower(wanted.extension().string());
    const auto asked = words(wanted.stem().string());
    if (asked.empty()) {
        return {};
    }

    std::filesystem::path best;
    std::size_t best_score = 0;
    bool tied = false;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
        if (!entry.is_regular_file(ignored) ||
            lower(entry.path().extension().string()) != extension) {
            continue;
        }
        // Already spoken for by an earlier FILE line.
        if (std::find(file_paths_.begin(), file_paths_.end(), entry.path()) != file_paths_.end()) {
            continue;
        }
        auto have = words(entry.path().stem().string());
        std::size_t score = 0;
        for (const auto& word : asked) {
            const auto at = std::find(have.begin(), have.end(), word);
            if (at != have.end()) {
                have.erase(at);  // each word matches at most once
                ++score;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = entry.path();
            tied = false;
        } else if (score == best_score && score != 0) {
            tied = true;
        }
    }
    // Half the words is the floor: below that the "match" is just two files
    // from the same library sharing a publisher's name.
    if (tied || best_score * 2 < asked.size()) {
        return {};
    }
    return best;
}

bool Disc::read_bytes(int file, std::uint64_t offset, void* out, std::size_t size) {
    if (file < 0 || file >= static_cast<int>(files_.size())) {
        return false;
    }
    auto& stream = *files_[static_cast<std::size_t>(file)];
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return stream.gcount() == static_cast<std::streamsize>(size);
}

bool Disc::open(const std::filesystem::path& image, std::string& error) {
    close();
    const auto extension = lower(image.extension().string());
    bool ok = false;
    if (extension == ".cue") {
        ok = open_cue(image, error);
    } else if (extension == ".chd") {
        ok = open_chd(image, error);
    } else {
        // A bare data track. If a .cue with the same name sits beside it, that
        // is the better description of the disc - it carries the audio tracks
        // too - so use it instead.
        auto cue = image;
        cue.replace_extension(".cue");
        std::error_code ignored;
        if (std::filesystem::exists(cue, ignored)) {
            ok = open_cue(cue, error);
        } else {
            ok = open_single(image, error);
        }
    }
    if (!ok) {
        close();
        return false;
    }
    load_header();
    if (header_.size() < 16 || std::memcmp(header_.data(), "SEGADISCSYSTEM", 14) != 0) {
        error = "not a Sega CD disc: the data track does not start with SEGADISCSYSTEM";
        close();
        return false;
    }
    return true;
}

bool Disc::open_single(const std::filesystem::path& image, std::string& error) {
    const int file = add_file(image);
    if (file < 0) {
        error = "cannot open " + image.string();
        return false;
    }
    std::uint8_t first[16] = {};
    if (!read_bytes(file, 0, first, sizeof(first))) {
        error = "the image is too short to hold a disc";
        return false;
    }
    Track track;
    track.file = file;
    track.file_lba0 = 0;
    track.start = 0;
    if (std::memcmp(first, "SEGADISCSYSTEM", 14) == 0) {
        track.type = TrackType::mode1;  // cooked: user data only
        track.sector_size = 2048;
    } else if (std::memcmp(first, kSync, sizeof(kSync)) == 0) {
        // Raw: the mode is the fourth byte of the block header.
        track.type = first[15] == 2 ? TrackType::mode2 : TrackType::mode1;
        track.sector_size = 2352;
    } else {
        error = "unrecognised disc image: neither a Sega CD header nor a raw sector";
        return false;
    }
    std::error_code ignored;
    const auto size = std::filesystem::file_size(image, ignored);
    // The BIOS insists on a data track of at least two seconds.
    track.end = std::max(150, static_cast<int>(size / static_cast<std::uint64_t>(track.sector_size)));
    tracks_.push_back(track);
    lead_out_ = track.end;
    return true;
}

bool Disc::open_cue(const std::filesystem::path& cue, std::string& error) {
    std::ifstream in(cue);
    if (!in) {
        error = "cannot open " + cue.string();
        return false;
    }

    // The file currently being described, where it sits on the disc, and how
    // much PREGAP silence (which is not in the file) has been declared in it
    // so far. Every block position in a file is shifted by that silence.
    int file = -1;
    bool file_is_wave = false;
    std::uint64_t file_data_offset = 0;
    int file_base = 0;
    int file_pregap = 0;
    // Index into tracks_ of the first track that lives in the current file.
    std::size_t file_first_track = 0;

    // Close off the current file: its last track runs to the end of the file.
    const auto finish_file = [&]() {
        if (file < 0 || tracks_.size() <= file_first_track) {
            return;
        }
        auto& last = tracks_.back();
        std::error_code ignored;
        const auto size = std::filesystem::file_size(file_paths_[static_cast<std::size_t>(file)], ignored);
        const auto payload = size > file_data_offset ? size - file_data_offset : 0;
        const int blocks = static_cast<int>((payload + static_cast<std::uint64_t>(last.sector_size) - 1) /
                                            static_cast<std::uint64_t>(last.sector_size));
        const int file_end = file_base + file_pregap + blocks;
        if (last.end == 0) {
            last.end = file_end;
        }
        file_base = std::max(file_end, last.end);
    };

    std::string line;
    while (std::getline(in, line)) {
        const auto text = trim(line);
        if (text.empty()) {
            continue;
        }
        std::istringstream words(text);
        std::string command;
        words >> command;
        command = lower(command);

        if (command == "file") {
            finish_file();
            // The name is quoted when it has spaces, which Redump's always do.
            std::string name;
            const auto open_quote = text.find('"');
            const auto close_quote = open_quote == std::string::npos ? std::string::npos
                                                                     : text.find('"', open_quote + 1);
            std::string kind;
            if (open_quote != std::string::npos && close_quote != std::string::npos) {
                name = text.substr(open_quote + 1, close_quote - open_quote - 1);
                kind = lower(trim(text.substr(close_quote + 1)));
            } else {
                words >> name >> kind;
                kind = lower(kind);
            }
            const auto path = cue.parent_path() / std::filesystem::u8path(name);
            file = add_file(path);
            if (file < 0) {
                // The name is wrong but the track may still be sitting there
                // under a slightly different one - see nearest_file.
                if (const auto near = nearest_file(path); !near.empty()) {
                    file = add_file(near);
                }
            }
            if (file < 0) {
                error = "the cue sheet names a file that cannot be opened: " + path.string();
                return false;
            }
            file_is_wave = kind == "wave";
            file_data_offset = 0;
            file_pregap = 0;
            file_first_track = tracks_.size();
            if (file_is_wave) {
                // Skip to the 'data' chunk. The audio inside is the same
                // 16-bit little-endian stereo as a raw track.
                char riff[12] = {};
                if (!read_bytes(file, 0, riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0 ||
                    std::memcmp(riff + 8, "WAVE", 4) != 0) {
                    error = "not a WAVE file: " + path.string();
                    return false;
                }
                std::uint64_t at = 12;
                for (int guard = 0; guard < 64; ++guard) {
                    std::uint8_t chunk[8] = {};
                    if (!read_bytes(file, at, chunk, sizeof(chunk))) break;
                    const std::uint32_t length = static_cast<std::uint32_t>(chunk[4]) |
                                                 (static_cast<std::uint32_t>(chunk[5]) << 8) |
                                                 (static_cast<std::uint32_t>(chunk[6]) << 16) |
                                                 (static_cast<std::uint32_t>(chunk[7]) << 24);
                    if (std::memcmp(chunk, "data", 4) == 0) {
                        file_data_offset = at + 8;
                        break;
                    }
                    at += 8 + length + (length & 1);
                }
                if (file_data_offset == 0) {
                    error = "WAVE file has no data chunk: " + path.string();
                    return false;
                }
            }
            continue;
        }

        if (command == "track") {
            int number = 0;
            std::string type;
            words >> number >> type;
            type = lower(type);
            if (tracks_.size() >= 99) {
                break;
            }
            Track track;
            track.file = file;
            track.data_offset = file_data_offset;
            if (type == "mode1/2048") {
                track.type = TrackType::mode1;
                track.sector_size = 2048;
            } else if (type == "mode1/2352") {
                track.type = TrackType::mode1;
                track.sector_size = 2352;
            } else if (type == "mode2/2352") {
                track.type = TrackType::mode2;
                track.sector_size = 2352;
            } else {
                track.type = TrackType::audio;
                track.sector_size = 2352;
            }
            // Filled in by INDEX 01. Until then, a track with no index is
            // not playable - start stays negative as the marker.
            track.start = -1;
            tracks_.push_back(track);
            continue;
        }

        if (command == "pregap") {
            std::string when;
            words >> when;
            const int blocks = parse_msf(when);
            if (blocks > 0) {
                file_pregap += blocks;
            }
            continue;
        }

        if (command == "index" && !tracks_.empty()) {
            int number = -1;
            std::string when;
            words >> number >> when;
            const int at = parse_msf(when);
            if (at < 0) {
                continue;
            }
            auto& current = tracks_.back();
            const bool same_file_as_previous = tracks_.size() >= 2 &&
                                               tracks_[tracks_.size() - 2].file == current.file &&
                                               tracks_.size() - 1 > file_first_track;
            if (number == 0) {
                // The start of this track's pause is the end of the one before.
                if (same_file_as_previous && tracks_[tracks_.size() - 2].end == 0) {
                    tracks_[tracks_.size() - 2].end = file_base + file_pregap + at;
                }
            } else if (number == 1) {
                current.file_lba0 = file_base + file_pregap;
                current.start = file_base + file_pregap + at;
                if (same_file_as_previous && tracks_[tracks_.size() - 2].end == 0) {
                    tracks_[tracks_.size() - 2].end = current.start;
                }
            }
            continue;
        }
    }
    finish_file();

    // Drop anything the sheet declared but never gave a position.
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [](const Track& t) { return t.start < 0 || t.end <= t.start; }),
                  tracks_.end());
    if (tracks_.empty()) {
        error = "the cue sheet describes no usable tracks";
        return false;
    }
    if (tracks_.front().type == TrackType::audio) {
        error = "the first track is audio - a Sega CD disc starts with its data track";
        return false;
    }
    // Two seconds minimum for the data track, as the BIOS expects.
    if (tracks_.front().end < 150) {
        tracks_.front().end = 150;
    }
    lead_out_ = tracks_.back().end;
    return true;
}

void Disc::load_header() {
    header_.assign(0x210, 0);
    if (tracks_.empty()) {
        return;
    }
    // Block 0 of the data track: the Sega CD system header and, right after
    // it, the security code.
    std::vector<std::uint8_t> block(2048, 0);
    if (!read_data(0, block.data())) {
        header_.clear();
        return;
    }
    std::copy(block.begin(), block.begin() + 0x210, header_.begin());
}

int Disc::track_at(int lba) const noexcept {
    for (int index = 0; index < static_cast<int>(tracks_.size()); ++index) {
        if (lba < tracks_[static_cast<std::size_t>(index)].end) {
            return index;
        }
    }
    return static_cast<int>(tracks_.size());
}

bool Disc::read_data(int lba, std::uint8_t* out, std::uint8_t* subheader) {
    const std::size_t size = subheader != nullptr ? 2328 : 2048;
    std::fill(out, out + size, std::uint8_t{0});
    const int index = track_at(lba);
    if (index >= track_count() || lba < 0) {
        return false;
    }
    const auto& track = tracks_[static_cast<std::size_t>(index)];
    if (track.type == TrackType::audio || lba < track.file_lba0) {
        return false;
    }
    if (track.chd_frame0 >= 0) {
        std::uint8_t frame[2448];
        if (!read_chd_frame(track, lba, frame)) {
            return false;
        }
        // A cooked track's user data is at the front of the field; a raw
        // one's is past the twelve-byte sync and the four-byte header.
        const std::size_t at = track.sector_size == 2048 ? 0 : 16;
        if (subheader == nullptr) {
            std::memcpy(out, frame + at, 2048);
            return true;
        }
        std::memcpy(subheader, frame + at + 4, 4);
        std::memcpy(out, frame + at + 8, 2328);
        return true;
    }
    const auto block = static_cast<std::uint64_t>(lba - track.file_lba0);
    if (track.sector_size == 2048) {
        return read_bytes(track.file, block * 2048u, out, 2048);
    }
    const auto base = block * 2352u + track.data_offset;
    if (subheader == nullptr) {
        // Skip the 12-byte sync and the 4-byte header to the user data.
        return read_bytes(track.file, base + 16, out, 2048);
    }
    // Mode 2: the sub-header is written twice after the header; the second
    // copy is the one read, then up to 2328 bytes of user data after both.
    if (!read_bytes(track.file, base + 16 + 4, subheader, 4)) {
        return false;
    }
    return read_bytes(track.file, base + 16 + 8, out, 2328);
}

bool Disc::read_audio(int lba, std::int16_t* out1176) {
    std::fill(out1176, out1176 + 1176, std::int16_t{0});
    const int index = track_at(lba);
    if (index >= track_count() || lba < 0) {
        return false;
    }
    const auto& track = tracks_[static_cast<std::size_t>(index)];
    if (track.type != TrackType::audio || lba < track.file_lba0) {
        return false;
    }
    std::uint8_t raw[2352];
    if (track.chd_frame0 >= 0) {
        std::uint8_t frame[2448];
        if (!read_chd_frame(track, lba, frame)) {
            return false;
        }
        // A CHD STORES CD AUDIO THE OTHER WAY ROUND. Samples are big-endian
        // in the container and little-endian in a .bin, so the same track
        // read from the two files is not the same bytes. Getting this wrong
        // is not silence or a click - it is full-volume noise.
        for (int i = 0; i < 1176; ++i) {
            out1176[i] = static_cast<std::int16_t>((frame[i * 2] << 8) | frame[i * 2 + 1]);
        }
        return true;
    }
    const auto offset = static_cast<std::uint64_t>(lba - track.file_lba0) * 2352u + track.data_offset;
    if (!read_bytes(track.file, offset, raw, sizeof(raw))) {
        return false;
    }
    for (int i = 0; i < 1176; ++i) {
        out1176[i] = static_cast<std::int16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
    }
    return true;
}

char Disc::region() const noexcept {
    if (header_.size() < 0x20C) {
        return 0;
    }
    // The security code differs per region, and it - not the header's region
    // string - is what the BIOS actually checks.
    switch (header_[0x20B]) {
        case 0x64: return 'E';
        case 0xA1: return 'J';
        default: return 'U';
    }
}

std::string Disc::title() const {
    if (header_.size() < 0x180) {
        return {};
    }
    // The overseas title, falling back to the domestic one, as a cartridge
    // header lays them out.
    auto pick = [this](std::size_t from) {
        std::string text(reinterpret_cast<const char*>(header_.data() + from), 0x30);
        return trim(text);
    };
    auto name = pick(0x150);
    if (name.empty()) {
        name = pick(0x120);
    }
    return name;
}

std::string Disc::describe() const {
    std::ostringstream out;
    out << "disc: " << track_count() << " track" << (track_count() == 1 ? "" : "s")
        << ", lead-out at block " << lead_out_ << ", region " << (region() ? region() : '?');
    for (int i = 0; i < track_count(); ++i) {
        const auto& t = track(i);
        out << "\n  track " << (i + 1) << ": "
            << (t.type == TrackType::audio ? "audio" : t.type == TrackType::mode1 ? "mode 1" : "mode 2")
            << " blocks " << t.start << "-" << t.end << " (" << t.sector_size << "-byte sectors)";
    }
    return out.str();
}

}  // namespace mercury::segacd
