#include "win32_cd_drive.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The CD-ROM controls: the table of contents and raw CD audio reads.
#include <winioctl.h>
#include <ntddcdrm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mercury::platform {

namespace {

// Blocks fetched per drive request. Sixteen is 32KB of data or 37KB of audio,
// inside the 64KB a CD-ROM driver accepts in one transfer on any drive, and
// enough that streaming a 1x disc is a handful of requests a second.
constexpr int kChunkBlocks = 16;
// How far ahead of the game the reader keeps going. 64 chunks is 1,024
// blocks - nearly fourteen seconds of a 1x stream, which is room for a drive
// that idled down to spin up again before the game catches it.
constexpr int kReadAheadChunks = 64;
// What the cache may hold before the least recently used chunks go. Most
// Sega CD data tracks fit whole; long audio tracks cycle through it.
constexpr std::size_t kCacheBytes = 256u * 1024u * 1024u;
constexpr std::size_t kRawBlock = 2352;
// A drive that has not produced a block in this long is not going to, and the
// game gets zeroes or silence for it rather than a frozen window.
constexpr auto kReadTimeout = std::chrono::seconds(15);

std::wstring device_name(wchar_t letter) { return std::wstring(L"\\\\.\\") + letter + L":"; }
std::wstring root_name(wchar_t letter) { return std::wstring(1, letter) + L":\\"; }
std::string drive_text(wchar_t letter) { return std::string(1, static_cast<char>(letter)) + ":"; }

bool is_cd_drive(wchar_t letter) {
    return GetDriveTypeW(root_name(letter).c_str()) == DRIVE_CDROM;
}

// Asking an empty drive anything can make Windows put up its own "insert a
// disk" box. These questions have their own answers, so that is switched off
// for the duration.
class QuietErrors {
  public:
    QuietErrors() { SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_); }
    ~QuietErrors() { SetThreadErrorMode(previous_, nullptr); }
    QuietErrors(const QuietErrors&) = delete;
    QuietErrors& operator=(const QuietErrors&) = delete;

  private:
    DWORD previous_ = 0;
};

HANDLE open_device(wchar_t letter) {
    return CreateFileW(device_name(letter).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Whether a disc is in. A changed disc is reported as an error ONCE, so a
// "media changed" answer is asked again rather than taken as "no disc".
bool media_present(HANDLE handle) {
    DWORD got = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (DeviceIoControl(handle, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0, &got, nullptr)) {
            return true;
        }
        if (GetLastError() != ERROR_MEDIA_CHANGED) {
            return false;
        }
    }
    return false;
}

int msf_lba(const TRACK_DATA& track) {
    return (track.Address[1] * 60 + track.Address[2]) * 75 + track.Address[3] - 150;
}

// One disc in one drive, read on a thread of its own.
//
// A real drive is slow in ways an image file is not: a seek costs a tenth of
// a second, a drive that has idled takes seconds to spin up, and a scratch
// can stall a read for longer still. The emulation must not sit through each
// of those block by block, so a single reader thread does ALL the drive's I/O
// - the handle is never touched from two threads - and keeps a cache running
// ahead of wherever the game last read. A block the game asks for that is not
// cached yet jumps the queue and the game waits for that one only.
class CdDriveSource final : public segacd::DiscSource {
  public:
    CdDriveSource(wchar_t letter, HANDLE handle, std::vector<Track> tracks, int lead_out)
        : letter_(letter), handle_(handle), tracks_(std::move(tracks)), lead_out_(lead_out) {
        worker_ = std::thread([this] { run(); });
    }

    ~CdDriveSource() override {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        ready_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        CloseHandle(handle_);
    }

    CdDriveSource(const CdDriveSource&) = delete;
    CdDriveSource& operator=(const CdDriveSource&) = delete;

    const std::vector<Track>& tracks() const override { return tracks_; }
    int lead_out() const override { return lead_out_; }
    bool read_data(int lba, std::uint8_t* out2048) override { return block(lba, out2048, 2048); }
    bool read_audio(int lba, std::uint8_t* out2352) override { return block(lba, out2352, kRawBlock); }
    std::string describe() const override { return "CD drive " + drive_text(letter_); }

  private:
    struct Chunk {
        std::vector<std::uint8_t> bytes;  // kChunkBlocks blocks, kRawBlock apart
        std::vector<bool> good;           // which blocks of it actually read
        std::list<int>::iterator age;
    };

    bool block(int lba, std::uint8_t* out, std::size_t size);
    void run();
    Chunk fetch(int index);
    bool read_run(int lba, int count, bool audio, std::uint8_t* out);
    [[nodiscard]] bool is_audio(int lba) const;
    [[nodiscard]] bool next_to_read_ahead(int& index) const;
    void store(int index, Chunk chunk);
    void note_error(DWORD error);

    const wchar_t letter_;
    const HANDLE handle_;
    const std::vector<Track> tracks_;
    const int lead_out_;

    std::mutex mutex_;
    std::condition_variable wake_;   // to the reader: there is something to do
    std::condition_variable ready_;  // to the game: a chunk has arrived
    std::unordered_map<int, Chunk> cache_;
    std::list<int> ages_;  // least recently used first
    std::size_t cached_bytes_ = 0;
    int wanted_ = -1;  // a chunk the game is waiting for
    int head_ = 0;     // the chunk the game last read; read-ahead runs from it
    bool stop_ = false;
    // The disc went away - taken out, or changed. Every read fails from then
    // on; the front end opens the drive again when a disc arrives.
    std::atomic<bool> lost_{false};
    std::thread worker_;
};

bool CdDriveSource::block(int lba, std::uint8_t* out, std::size_t size) {
    if (lba < 0 || lba >= lead_out_) {
        return false;
    }
    const int index = lba / kChunkBlocks;
    std::unique_lock lock(mutex_);
    head_ = index;
    auto found = cache_.find(index);
    if (found == cache_.end()) {
        if (lost_) {
            return false;
        }
        wanted_ = index;
        wake_.notify_all();
        ready_.wait_for(lock, kReadTimeout,
                        [&] { return cache_.count(index) != 0 || lost_ || stop_; });
        found = cache_.find(index);
        if (found == cache_.end()) {
            return false;
        }
    } else {
        // The game moved on: keep the read-ahead in front of it.
        wake_.notify_all();
    }
    auto& chunk = found->second;
    ages_.splice(ages_.end(), ages_, chunk.age);
    const int at = lba - index * kChunkBlocks;
    if (!chunk.good[static_cast<std::size_t>(at)]) {
        return false;
    }
    std::memcpy(out, chunk.bytes.data() + static_cast<std::size_t>(at) * kRawBlock, size);
    return true;
}

void CdDriveSource::run() {
    QuietErrors quiet;
    std::unique_lock lock(mutex_);
    while (!stop_) {
        int next = -1;
        if (wanted_ >= 0) {
            next = cache_.count(wanted_) != 0 ? -1 : wanted_;
            wanted_ = -1;
        }
        if (next < 0 && !lost_ && !next_to_read_ahead(next)) {
            next = -1;
        }
        if (next < 0) {
            wake_.wait(lock);
            continue;
        }
        lock.unlock();
        Chunk chunk = fetch(next);
        lock.lock();
        store(next, std::move(chunk));
        ready_.notify_all();
    }
}

bool CdDriveSource::next_to_read_ahead(int& index) const {
    const int last = (lead_out_ + kChunkBlocks - 1) / kChunkBlocks;
    for (int candidate = head_; candidate < std::min(head_ + kReadAheadChunks, last); ++candidate) {
        if (cache_.count(candidate) == 0) {
            index = candidate;
            return true;
        }
    }
    return false;
}

bool CdDriveSource::is_audio(int lba) const {
    bool audio = false;
    for (const auto& track : tracks_) {
        if (track.start > lba) {
            break;
        }
        audio = track.audio;
    }
    return audio;
}

CdDriveSource::Chunk CdDriveSource::fetch(int index) {
    Chunk chunk;
    chunk.bytes.assign(static_cast<std::size_t>(kChunkBlocks) * kRawBlock, 0);
    chunk.good.assign(kChunkBlocks, false);
    const int first = index * kChunkBlocks;
    const int last = std::min(first + kChunkBlocks, lead_out_);
    int lba = first;
    while (lba < last && !lost_) {
        // One request per run of blocks of the same kind: data and audio are
        // read with different calls.
        const bool audio = is_audio(lba);
        int end = lba + 1;
        while (end < last && is_audio(end) == audio) {
            ++end;
        }
        auto* at = chunk.bytes.data() + static_cast<std::size_t>(lba - first) * kRawBlock;
        if (read_run(lba, end - lba, audio, at)) {
            for (int b = lba; b < end; ++b) {
                chunk.good[static_cast<std::size_t>(b - first)] = true;
            }
        } else {
            // One unreadable block - a scratch, or the pause before an audio
            // track that the table of contents counts as data - must not cost
            // the blocks around it. So the run is tried again a block at a time.
            for (int b = lba; b < end && !lost_; ++b) {
                auto* one = chunk.bytes.data() + static_cast<std::size_t>(b - first) * kRawBlock;
                chunk.good[static_cast<std::size_t>(b - first)] = read_run(b, 1, audio, one);
            }
        }
        lba = end;
    }
    return chunk;
}

bool CdDriveSource::read_run(int lba, int count, bool audio, std::uint8_t* out) {
    DWORD got = 0;
    if (audio) {
        RAW_READ_INFO info{};
        // The offset is in 2048-byte units whatever the track: that is how the
        // control is defined, not a mistake for 2352.
        info.DiskOffset.QuadPart = static_cast<LONGLONG>(lba) * 2048;
        info.SectorCount = static_cast<ULONG>(count);
        info.TrackMode = CDDA;
        const auto size = static_cast<DWORD>(static_cast<std::size_t>(count) * kRawBlock);
        if (DeviceIoControl(handle_, IOCTL_CDROM_RAW_READ, &info, sizeof(info), out, size, &got,
                            nullptr) &&
            got == size) {
            return true;
        }
    } else {
        // Cooked Mode 1 blocks, 2048 bytes each, read at their own offset and
        // then spread out to the cache's 2352-byte stride.
        std::vector<std::uint8_t> staging(static_cast<std::size_t>(count) * 2048);
        const auto offset = static_cast<std::uint64_t>(lba) * 2048u;
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        if (ReadFile(handle_, staging.data(), static_cast<DWORD>(staging.size()), &got, &at) &&
            got == staging.size()) {
            for (int i = 0; i < count; ++i) {
                std::memcpy(out + static_cast<std::size_t>(i) * kRawBlock,
                            staging.data() + static_cast<std::size_t>(i) * 2048, 2048);
            }
            return true;
        }
    }
    note_error(GetLastError());
    return false;
}

void CdDriveSource::note_error(DWORD error) {
    if (error == ERROR_MEDIA_CHANGED || error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE ||
        error == ERROR_DEVICE_NOT_CONNECTED) {
        lost_ = true;
        ready_.notify_all();
    }
}

void CdDriveSource::store(int index, Chunk chunk) {
    if (cache_.count(index) != 0) {
        return;
    }
    const auto size = chunk.bytes.size();
    while (cached_bytes_ + size > kCacheBytes && !ages_.empty()) {
        const int oldest = ages_.front();
        ages_.pop_front();
        const auto old = cache_.find(oldest);
        if (old != cache_.end()) {
            cached_bytes_ -= old->second.bytes.size();
            cache_.erase(old);
        }
    }
    ages_.push_back(index);
    chunk.age = std::prev(ages_.end());
    cached_bytes_ += size;
    cache_.emplace(index, std::move(chunk));
}

std::unique_ptr<segacd::DiscSource> open_drive(wchar_t letter, std::string& error) {
    QuietErrors quiet;
    const HANDLE handle = open_device(letter);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "cannot open CD drive " + drive_text(letter) + " (Windows error " +
                std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    if (!media_present(handle)) {
        CloseHandle(handle);
        error = "there is no disc in drive " + drive_text(letter);
        return nullptr;
    }
    CDROM_TOC toc{};
    DWORD got = 0;
    if (!DeviceIoControl(handle, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &got, nullptr)) {
        CloseHandle(handle);
        error = "cannot read the table of contents of the disc in drive " + drive_text(letter);
        return nullptr;
    }
    const int count = static_cast<int>(toc.LastTrack) - static_cast<int>(toc.FirstTrack) + 1;
    if (count <= 0 || count >= MAXIMUM_NUMBER_TRACKS) {
        CloseHandle(handle);
        error = "the disc in drive " + drive_text(letter) + " has no usable tracks";
        return nullptr;
    }
    std::vector<segacd::DiscSource::Track> tracks;
    for (int i = 0; i < count; ++i) {
        const auto& entry = toc.TrackData[i];
        segacd::DiscSource::Track track;
        // Control bit 2 marks a data track; everything else is audio.
        track.audio = (entry.Control & 0x4) == 0;
        track.start = msf_lba(entry);
        tracks.push_back(track);
    }
    // The entry after the last track is the lead-out (track AA).
    const int lead_out = msf_lba(toc.TrackData[count]);
    return std::make_unique<CdDriveSource>(letter, handle, std::move(tracks), lead_out);
}

}  // namespace

std::vector<CdDrive> list_cd_drives() {
    QuietErrors quiet;
    std::vector<CdDrive> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }
        const auto letter = static_cast<wchar_t>(L'A' + i);
        if (!is_cd_drive(letter)) {
            continue;
        }
        CdDrive drive;
        drive.letter = letter;
        const HANDLE handle = open_device(letter);
        if (handle != INVALID_HANDLE_VALUE) {
            drive.has_disc = media_present(handle);
            CloseHandle(handle);
        }
        if (drive.has_disc) {
            wchar_t label[MAX_PATH + 1] = L"";
            if (GetVolumeInformationW(root_name(letter).c_str(), label, MAX_PATH + 1, nullptr, nullptr,
                                      nullptr, nullptr, 0)) {
                drive.label = label;
            }
        }
        drives.push_back(drive);
    }
    return drives;
}

std::filesystem::path cd_drive_path(wchar_t letter) {
    return std::filesystem::path(root_name(letter));
}

wchar_t cd_drive_letter(const std::filesystem::path& path) {
    const auto& text = path.native();
    if (text.size() < 2 || text.size() > 3 || text[1] != L':') {
        return 0;
    }
    if (text.size() == 3 && text[2] != L'\\' && text[2] != L'/') {
        return 0;
    }
    const auto letter = static_cast<wchar_t>(std::towupper(text[0]));
    if (letter < L'A' || letter > L'Z') {
        return 0;
    }
    return is_cd_drive(letter) ? letter : 0;
}

segacd::DiscSourceOpener cd_drive_opener() {
    segacd::DiscSourceOpener opener;
    opener.claims = [](const std::filesystem::path& path) { return cd_drive_letter(path) != 0; };
    opener.open = [](const std::filesystem::path& path, std::string& error) {
        return open_drive(cd_drive_letter(path), error);
    };
    return opener;
}

}  // namespace mercury::platform
