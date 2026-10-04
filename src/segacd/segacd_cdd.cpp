// The CD drive, as the sub CPU talks to it.
//
// The drive is a separate microcontroller. Every 1/75 of a second it answers
// the last command with a ten-nibble status and raises level 4; the BIOS reads
// the status and writes the next command. Everything here is the status and
// command protocol, and the drive's own idea of where the laser is.
//
// The rules - what each command reports, which nibble means what, when a seek
// counts as finished - are Genesis Plus GX's, which in turn matched them
// against real hardware and a long list of games that each depend on some
// detail. Where a rule exists for one particular game, the comment says which.

#include <cstdio>
#include <cstdlib>

#include "mercury/segacd/segacd.hpp"

namespace mercury::segacd {

namespace {

// Packed BCD in a word: tens in the high byte, units in the low. The status
// registers hold one digit per byte because each byte is one nibble on the
// wire.
std::uint16_t bcd_word(int value) noexcept {
    value %= 100;
    return static_cast<std::uint16_t>(((value / 10) << 8) | (value % 10));
}

std::uint8_t bcd_byte(int value) noexcept {
    value %= 100;
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

}  // namespace

void SegaCd::cdd_reset() noexcept {
    cdd_latency_ = 0;
    cdd_track_ = 0;
    cdd_lba_ = 0;
    cdd_status_ = disc_.loaded() ? kReadingToc : kNoDisc;
    cdd_pending_ = 0;
    cdd_scan_offset_ = 0;
    fader_ = 0x400;
    // Anything still queued belongs to where the laser WAS - see end_frame,
    // which keeps up to a frame of CD audio back to line it up with the PCM
    // chip. Carrying it across a seek would play a fragment of the old track
    // over the start of the new one.
    cdda_.clear();
}

// CHANGE THE DISC WITHOUT RESTARTING THE CONSOLE.
//
// Half of the Sega CD's FMV library is two or three discs and each one asks
// for the next partway through - Night Trap, Fahrenheit, Supreme Warrior,
// Ground Zero Texas. The game is not restarted when that happens on hardware:
// it keeps running, the tray opens, the disc changes, the tray closes and it
// carries on reading. A swap that rebooted the machine would lose the point
// in the story the player had reached.
//
// So this does exactly what the drive does for that sequence, in the order
// the drive does it: the two halves are the $0D and $0C commands, written out
// here rather than called, because a swap is one atomic thing to the outside
// and the sub CPU never sees a half-open tray. Reg $36's high byte is the
// "not ready" flag the main CPU polls; $38's low nibble is $0F, the drive's
// "no meaningful report" answer, which is what it sends while the tray moves.
bool SegaCd::swap_disc(const std::filesystem::path& image, std::string& error) {
    // Tray open, and the laser back to the start.
    set_hi(0x36 >> 1, 0x01);
    cdd_status_ = kTrayOpen;
    cdd_track_ = 0;
    cdd_lba_ = 0;
    cdd_latency_ = 0;
    cdd_pending_ = 0;
    cdd_scan_offset_ = 0;

    // The new disc. A failure leaves the tray open with nothing in it, which
    // is a real state the drive can be in and reports honestly.
    if (!disc_.open(image, error)) {
        cdd_status_ = kNoDisc;
        regs_[0x38 >> 1] = static_cast<std::uint16_t>((kNoDisc << 8) | 0x0F);
        regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
        regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kNoDisc + 0x0F) & 0x0F);
        return false;
    }

    // Tray closed: reading the table of contents, reporting a stopped drive
    // until the first real status goes out.
    cdd_status_ = kReadingToc;
    regs_[0x38 >> 1] = static_cast<std::uint16_t>((kStop << 8) | 0x0F);
    regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
    regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kStop + 0x0F) & 0x0F);
    return true;
}

// Absolute time of a block, into status nibbles RS2-RS7.
void SegaCd::cdd_report_time(int lba) noexcept {
    const int msf = lba + 150;
    regs_[0x3A >> 1] = bcd_word((msf / 75) / 60);
    regs_[0x3C >> 1] = bcd_word((msf / 75) % 60);
    regs_[0x3E >> 1] = bcd_word(msf % 75);
    const bool data = cdd_track_ < disc_.track_count() &&
                      disc_.track(cdd_track_).type != Disc::TrackType::audio;
    // RS8: bit 2 says the current block is data rather than audio.
    set_hi(0x40 >> 1, data ? 0x04 : 0x00);
}

void SegaCd::cdd_write_checksum() noexcept {
    // RS9 is the ones' complement of the sum of the nine nibbles before it.
    int sum = 0;
    for (std::uint32_t at = 0x38; at < 0x40; at += 2) {
        sum += hi(at >> 1) + lo(at >> 1);
    }
    sum += hi(0x40 >> 1);
    set_lo(0x40 >> 1, static_cast<std::uint8_t>(~sum & 0x0F));
}

void SegaCd::cdd_play_audio_block() noexcept {
    std::int16_t block[1176];
    const bool audible = cdd_status_ == kPlay && cdd_latency_ == 0 &&
                         cdd_track_ < disc_.track_count() &&
                         disc_.track(cdd_track_).type == Disc::TrackType::audio &&
                         cdd_lba_ >= disc_.track(cdd_track_).start &&
                         disc_.read_audio(cdd_lba_, block);
    if (!audible) {
        cdda_.insert(cdda_.end(), 1176, std::int16_t{0});
        return;
    }
    for (int i = 0; i < 1176; ++i) {
        cdda_.push_back(static_cast<std::int16_t>((static_cast<std::int32_t>(block[i]) * fader_) >> 10));
    }
}

void SegaCd::cdd_tick() noexcept {
    // CD audio is produced here, one block per tick, so it always runs at
    // exactly 75 blocks - 44,100 frames - a second.
    cdd_play_audio_block();

    if (cdd_latency_ > 0) {
        --cdd_latency_;
        // The decoder keeps running while the drive seeks
        // (mcd-verificator's CDC init test).
        cdc_decode_block(0);
    } else if (cdd_status_ == kPlay) {
        if (cdd_track_ >= disc_.track_count()) {
            cdd_status_ = kDiscEnd;
            return;
        }
        const auto& track = disc_.track(cdd_track_);
        if (track.type != Disc::TrackType::audio) {
            // A data block: its header is its own address, in BCD, and its
            // mode.
            const int msf = cdd_lba_ + 150;
            const std::uint32_t header =
                (static_cast<std::uint32_t>(bcd_byte((msf / 75) / 60)) << 24) |
                (static_cast<std::uint32_t>(bcd_byte((msf / 75) % 60)) << 16) |
                (static_cast<std::uint32_t>(bcd_byte(msf % 75)) << 8) |
                static_cast<std::uint32_t>(track.type == Disc::TrackType::mode2 ? 2 : 1);
            cdc_decode_block(header);
        } else {
            if (cdd_lba_ >= track.start) {
                set_hi(0x36 >> 1, 0x00);  // audio is playing
            }
            // Audio blocks go through the decoder too.
            cdc_decode_block(0);
        }
        ++cdd_lba_;
        if (cdd_lba_ >= track.end) {
            // On to the next track, which on a Sega CD disc can only be audio.
            ++cdd_track_;
            set_hi(0x36 >> 1, 0x01);
        }
    } else {
        // The decoder runs while the disc is idle too (mcd-verificator's CDC
        // flags test).
        cdc_decode_block(0);
        if (cdd_status_ == kScan) {
            int index = cdd_track_;
            cdd_lba_ += cdd_scan_offset_;
            if (index < disc_.track_count() && cdd_lba_ >= disc_.track(index).end) {
                ++index;
                if (index < disc_.track_count()) {
                    cdd_lba_ = disc_.track(index).start;
                } else {
                    cdd_lba_ = disc_.lead_out();
                    cdd_track_ = disc_.track_count();
                    cdd_status_ = kDiscEnd;
                    set_hi(0x36 >> 1, 0x01);
                    return;
                }
            } else if (index < disc_.track_count() && cdd_lba_ < disc_.track(index).start) {
                if (index > 0) {
                    --index;
                    cdd_lba_ = disc_.track(index).end;
                } else {
                    cdd_lba_ = 0;
                }
            }
            const bool audio = index < disc_.track_count() &&
                               disc_.track(index).type == Disc::TrackType::audio;
            set_hi(0x36 >> 1, audio ? 0x00 : 0x01);
            cdd_track_ = index;
        }
    }

    // A seek or play command waits one tick before it starts - Radical Rex
    // sets up its RAM in that gap and loses sprites without it.
    if (cdd_pending_ != 0) {
        const int target = ((hi(0x44 >> 1) * 10 + lo(0x44 >> 1)) * 60 +
                            (hi(0x46 >> 1) * 10 + lo(0x46 >> 1))) * 75 +
                           (hi(0x48 >> 1) * 10 + lo(0x48 >> 1)) - 150;
        // THE DRIVE TAKES TIME TO GET THERE: twelve ticks before a read or a
        // seek starts, plus a seek time proportional to distance (worst case
        // 120 ticks across a whole disc). Genesis Plus GX calls this "CD
        // access time" and has it ON by default - its own note is that a few
        // games crash if the data arrives too soon, and the Wolf Team titles
        // need at least twelve. This used to be two ticks, on the mistaken
        // belief that the minimum was GPGX's default; Radical Rex re-issued
        // the same read every few frames for ever with the data that early.
        // (Fewer than two and the BIOS hangs.)
        if (cdd_latency_ == 0) {
            cdd_latency_ = 12;
        }
        cdd_latency_ += (std::abs(target - cdd_lba_) * 120) / 270000;
        cdd_lba_ = target;
        int index = 0;
        while (index < disc_.track_count() && disc_.track(index).end <= target) {
            ++index;
        }
        cdd_track_ = index;
        set_hi(0x36 >> 1, 0x01);  // nothing audible until the seek ends
        cdd_status_ = cdd_pending_;
        cdd_pending_ = 0;
    }
}

void SegaCd::cdd_command() noexcept {
    ++cdd_commands_;
    const std::uint8_t command = hi(0x42 >> 1) & 0x0F;
    // MERCURY_SCD_CDD_LOG=1: every drive command except the status polls,
    // with the ten command nibbles, the laser position and the drive state -
    // the same line the GPGX oracle host prints, so the two can be diffed.
    static const bool logging = [] {
        const char* e = std::getenv("MERCURY_SCD_CDD_LOG");
        return e != nullptr && *e != '0';
    }();
    if (logging && command != 0) {
        std::fprintf(stderr, "[cdd] f%d cmd=%X%X%X%X%X%X%X%X%X%X lba=%d status=%X\n",
                     static_cast<int>(now_ / (kSubClockHz / 60)), hi(0x42 >> 1) & 0x0F,
                     lo(0x42 >> 1) & 0x0F, hi(0x44 >> 1) & 0x0F, lo(0x44 >> 1) & 0x0F,
                     hi(0x46 >> 1) & 0x0F, lo(0x46 >> 1) & 0x0F, hi(0x48 >> 1) & 0x0F,
                     lo(0x48 >> 1) & 0x0F, hi(0x4A >> 1) & 0x0F, lo(0x4A >> 1) & 0x0F,
                     cdd_lba_, cdd_status_);
    }
    switch (command) {
        case 0x00: {  // get status
            // The reported state only moves once the previous command has
            // been carried out. Radical Rex and Annet Futatabi need at least
            // two and three ticks of "playing" before blocks start counting.
            if (cdd_latency_ <= 3) {
                set_hi(0x38 >> 1, cdd_status_);
                if (cdd_status_ == kStop || cdd_status_ > kPause) {
                    break;
                }
                const std::uint8_t kind = lo(0x38 >> 1);
                if (kind == 0x0F || kind == 0x00) {
                    // 0x0F marked the track info as invalid during a seek; the
                    // seek is over, so report absolute time (Lunar).
                    set_lo(0x38 >> 1, 0x00);
                    cdd_report_time(cdd_lba_);
                } else if (kind == 0x01) {
                    const int start = cdd_track_ < disc_.track_count() ? disc_.track(cdd_track_).start : 0;
                    const int relative = std::abs(cdd_lba_ - start);
                    regs_[0x3A >> 1] = bcd_word((relative / 75) / 60);
                    regs_[0x3C >> 1] = bcd_word((relative / 75) % 60);
                    regs_[0x3E >> 1] = bcd_word(relative % 75);
                    const bool data = cdd_track_ < disc_.track_count() &&
                                      disc_.track(cdd_track_).type != Disc::TrackType::audio;
                    set_hi(0x40 >> 1, data ? 0x04 : 0x00);
                } else if (kind == 0x02) {
                    regs_[0x3A >> 1] = cdd_track_ < disc_.track_count() ? bcd_word(cdd_track_ + 1) : 0x0A0A;
                }
            }
            break;
        }

        case 0x01:  // stop
            cdd_status_ = disc_.loaded() ? kReadingToc : kNoDisc;
            set_hi(0x36 >> 1, 0x01);
            regs_[0x38 >> 1] = static_cast<std::uint16_t>((kStop << 8) | 0x0F);
            regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
            regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kStop + 0x0F) & 0x0F);
            cdd_track_ = 0;
            cdd_lba_ = 0;
            return;

        case 0x02: {  // report table of contents
            switch (lo(0x44 >> 1)) {
                case 0x00:  // absolute time
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>(cdd_status_ << 8);
                    cdd_report_time(cdd_lba_);
                    break;
                case 0x01: {  // time within the track
                    const int start = cdd_track_ < disc_.track_count() ? disc_.track(cdd_track_).start : 0;
                    const int relative = std::abs(cdd_lba_ - start);
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x01);
                    regs_[0x3A >> 1] = bcd_word((relative / 75) / 60);
                    regs_[0x3C >> 1] = bcd_word((relative / 75) % 60);
                    regs_[0x3E >> 1] = bcd_word(relative % 75);
                    const bool data = cdd_track_ < disc_.track_count() &&
                                      disc_.track(cdd_track_).type != Disc::TrackType::audio;
                    set_hi(0x40 >> 1, data ? 0x04 : 0x00);
                    break;
                }
                case 0x02:  // current track number
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x02);
                    regs_[0x3A >> 1] = cdd_track_ < disc_.track_count() ? bcd_word(cdd_track_ + 1) : 0x0A0A;
                    regs_[0x3C >> 1] = 0;
                    regs_[0x3E >> 1] = 0;
                    set_hi(0x40 >> 1, 0x00);
                    break;
                case 0x03: {  // length of the disc
                    const int msf = disc_.lead_out() + 150;
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x03);
                    regs_[0x3A >> 1] = bcd_word((msf / 75) / 60);
                    regs_[0x3C >> 1] = bcd_word((msf / 75) % 60);
                    regs_[0x3E >> 1] = bcd_word(msf % 75);
                    set_hi(0x40 >> 1, 0x00);
                    break;
                }
                case 0x04:  // first and last track numbers
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x04);
                    regs_[0x3A >> 1] = 0x0001;
                    regs_[0x3C >> 1] = bcd_word(disc_.track_count());
                    regs_[0x3E >> 1] = 0;
                    set_hi(0x40 >> 1, 0x00);
                    break;
                case 0x05: {  // where a track starts
                    const int number = hi(0x46 >> 1) * 10 + lo(0x46 >> 1);
                    const int index = number - 1;
                    const bool valid = index >= 0 && index < disc_.track_count();
                    const int msf = (valid ? disc_.track(index).start : 0) + 150;
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x05);
                    regs_[0x3A >> 1] = bcd_word((msf / 75) / 60);
                    regs_[0x3C >> 1] = bcd_word((msf / 75) % 60);
                    regs_[0x3E >> 1] = bcd_word(msf % 75);
                    if (valid && disc_.track(index).type != Disc::TrackType::audio) {
                        set_hi(0x3E >> 1, static_cast<std::uint8_t>(hi(0x3E >> 1) | 0x08));  // data track
                    }
                    set_hi(0x40 >> 1, static_cast<std::uint8_t>(number % 10));
                    break;
                }
                case 0x06:  // latest error: none
                    regs_[0x38 >> 1] = static_cast<std::uint16_t>((cdd_status_ << 8) | 0x06);
                    regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
                    set_hi(0x40 >> 1, 0x00);
                    break;
                default:
                    return;  // an unknown request is ignored outright
            }
            break;
        }

        case 0x03:  // play
            // "Seeking" with the track info marked invalid (0x0F) until the
            // drive is ready - Bari Arm's audio delay and Snatcher's Act 2
            // cutscene both depend on it.
            regs_[0x38 >> 1] = static_cast<std::uint16_t>((kSeek << 8) | 0x0F);
            regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
            regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kSeek + 0x0F) & 0x0F);
            cdd_pending_ = kPlay;
            return;

        case 0x04:  // seek
            regs_[0x38 >> 1] = static_cast<std::uint16_t>((kSeek << 8) | 0x0F);
            regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
            regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kSeek + 0x0F) & 0x0F);
            cdd_pending_ = kPause;
            return;

        case 0x06:  // pause
            set_hi(0x36 >> 1, 0x01);
            cdd_status_ = kPause;
            set_hi(0x38 >> 1, kPause);
            break;

        case 0x07:  // resume
            cdd_status_ = kPlay;
            set_hi(0x38 >> 1, kPlay);
            break;

        case 0x08:  // fast forward
            cdd_scan_offset_ = 30;
            cdd_status_ = kScan;
            set_hi(0x38 >> 1, kScan);
            break;

        case 0x09:  // rewind
            cdd_scan_offset_ = -30;
            cdd_status_ = kScan;
            set_hi(0x38 >> 1, kScan);
            break;

        case 0x0A:  // track jump: treated as a pause, as Genesis Plus GX does
            set_hi(0x36 >> 1, 0x01);
            cdd_status_ = kPause;
            set_hi(0x38 >> 1, kPause);
            break;

        case 0x0C:  // close the tray
            set_hi(0x36 >> 1, 0x01);
            cdd_status_ = disc_.loaded() ? kReadingToc : kNoDisc;
            regs_[0x38 >> 1] = static_cast<std::uint16_t>((kStop << 8) | 0x0F);
            regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
            regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kStop + 0x0F) & 0x0F);
            cdd_track_ = 0;
            cdd_lba_ = 0;
            return;

        case 0x0D:  // open the tray
            set_hi(0x36 >> 1, 0x01);
            cdd_status_ = kTrayOpen;
            regs_[0x38 >> 1] = static_cast<std::uint16_t>((kTrayOpen << 8) | 0x0F);
            regs_[0x3A >> 1] = regs_[0x3C >> 1] = regs_[0x3E >> 1] = 0;
            regs_[0x40 >> 1] = static_cast<std::uint16_t>(~(kTrayOpen + 0x0F) & 0x0F);
            cdd_track_ = 0;
            cdd_lba_ = 0;
            return;

        default:
            set_hi(0x38 >> 1, cdd_status_);
            break;
    }
    cdd_write_checksum();
}

}  // namespace mercury::segacd
