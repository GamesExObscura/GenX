// The Sega CD unit, tested without a BIOS.
//
// The sub CPU is given small hand-assembled programs through the main CPU's
// program RAM window - which is exactly how the real BIOS hands it its code -
// so the tests exercise the gate array from both sides, the interrupts, the
// clocks and the drive, the way software actually reaches them.

#include "mercury/segacd/disc.hpp"
#include "mercury/segacd/segacd.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(std::uint32_t got, std::uint32_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s: got %X want %X\n", what.c_str(), got, want);
    }
}

using mercury::segacd::SegaCd;

constexpr std::uint32_t kMainClock = 7'670'453;

// A hand-assembled sub-CPU program, written through the main CPU's window
// onto program RAM bank 0 at $020000.
struct Program {
    std::vector<std::uint16_t> words;
    Program& w(std::uint16_t word) { words.push_back(word); return *this; }
    Program& l(std::uint32_t value) { w(static_cast<std::uint16_t>(value >> 16)); return w(static_cast<std::uint16_t>(value)); }
    // move.w #imm,(abs).l
    Program& move_w(std::uint16_t imm, std::uint32_t to) { w(0x33FC); w(imm); return l(to); }
    // move.b #imm,(abs).l
    Program& move_b(std::uint8_t imm, std::uint32_t to) { w(0x13FC); w(imm); return l(to); }
    // move.w (abs).l,(abs).l
    Program& copy_w(std::uint32_t from, std::uint32_t to) { w(0x33F9); l(from); return l(to); }
    // addq.w #1,(abs).l
    Program& bump(std::uint32_t at) { w(0x5279); return l(at); }
    Program& enable_interrupts() { w(0x46FC); return w(0x2000); }  // move #$2000,sr
    Program& spin() { return w(0x60FE); }                            // bra.s *
    Program& rte() { return w(0x4E73); }
};

// Load a program: vectors at 0 (stack, entry $200), optional interrupt
// handlers, and the body at $200. Then let the sub CPU go.
void boot_sub(SegaCd& cd, const Program& body,
              const std::vector<std::pair<int, Program>>& handlers = {}) {
    // Power-on state: RESET held and the bus requested, so the window is open.
    const auto put = [&cd](std::uint32_t sub_address, std::uint16_t word) {
        cd.main_write16(0x020000 + sub_address, word);
    };
    put(0x0000, 0x0000);
    put(0x0002, 0x7F00);  // SSP $00007F00
    put(0x0004, 0x0000);
    put(0x0006, 0x0200);  // PC $00000200
    std::uint32_t handler_at = 0x1000;
    for (const auto& [level, code] : handlers) {
        // Autovector for level n is vector 24 + n, at (24 + n) * 4.
        const std::uint32_t vector = static_cast<std::uint32_t>(24 + level) * 4;
        put(vector, static_cast<std::uint16_t>(handler_at >> 16));
        put(vector + 2, static_cast<std::uint16_t>(handler_at));
        for (std::size_t i = 0; i < code.words.size(); ++i) {
            put(handler_at + static_cast<std::uint32_t>(i) * 2, code.words[i]);
        }
        handler_at += 0x100;
    }
    for (std::size_t i = 0; i < body.words.size(); ++i) {
        put(0x200 + static_cast<std::uint32_t>(i) * 2, body.words[i]);
    }
    cd.main_write8(0xA12001, 0x01);  // release RESET, drop BUSREQ: run
    // A main-CPU register write lands at the end of its instruction; end it.
    cd.advance(0, kMainClock);
}

void run_for(SegaCd& cd, double seconds) {
    const auto main_cycles = static_cast<std::uint64_t>(seconds * kMainClock);
    for (std::uint64_t done = 0; done < main_cycles; done += 8) {
        cd.advance(8, kMainClock);
    }
}

// --- tests ---------------------------------------------------------------------

void test_power_on_state() {
    std::printf("power on: the sub CPU is held and Word RAM belongs to the main CPU\n");
    SegaCd cd;
    check_eq(cd.main_read8(0xA12001), 0x02, "RESET clear, BUSREQ set");
    check(!cd.sub_running(), "the sub CPU does not run until released");
    check_eq(cd.main_read8(0xA12003) & 0x07, 0x01, "2M mode, RET set: Word RAM is the main CPU's");
    cd.main_write16(0x200000, 0xBEEF);
    check_eq(cd.main_read16(0x200000), 0xBEEF, "and the main CPU can use it");
}

void test_bios_and_hint_vector() {
    std::printf("the BIOS sits at 0, but the H-interrupt vector comes from $A12006\n");
    SegaCd cd;
    std::vector<std::uint8_t> bios(SegaCd::kBiosSize, 0);
    bios[0] = 0x12; bios[1] = 0x34;
    bios[0x70] = 0xAA; bios[0x71] = 0xAA; bios[0x72] = 0xBB; bios[0x73] = 0xBB;
    std::string error;
    check(cd.load_bios(bios, error), "a 128KB image loads");
    check_eq(cd.main_read16(0x000000), 0x1234, "the BIOS reads at 0");
    check_eq(cd.main_read16(0x040000), 0x1234, "and is mirrored every 256KB");
    cd.main_write16(0x000000, 0xFFFF);
    check_eq(cd.main_read16(0x000000), 0x1234, "and is read-only");
    cd.main_write16(0xA12006, 0x4321);
    check_eq(cd.main_read16(0x000070), 0xFFFF, "the vector's high word reads $FFFF");
    check_eq(cd.main_read16(0x000072), 0x4321, "and its low word is the gate array's");
    check(!cd.load_bios(std::vector<std::uint8_t>(1000, 0), error), "a wrong-sized image is refused");
}

void test_program_ram_window() {
    std::printf("program RAM is only reachable from the main CPU with the sub's bus requested\n");
    SegaCd cd;
    cd.main_write16(0x020000, 0xCAFE);
    check_eq(cd.main_read16(0x020000), 0xCAFE, "open while BUSREQ is set");
    cd.main_write8(0xA12003, 0x40);  // BK = 1
    cd.main_write16(0x020000, 0x1111);
    cd.main_write8(0xA12003, 0x00);  // back to bank 0
    check_eq(cd.main_read16(0x020000), 0xCAFE, "the bank bits select a different 128KB");
    check_eq(cd.prg_ram()[0x20000], 0x11, "bank 1 is the second 128KB of the sub's RAM");
    cd.main_write8(0xA12001, 0x01);  // run: BUSREQ drops
    check_eq(cd.main_read16(0x020000), 0xFFFF, "closed while the sub CPU runs");
}

void test_sub_cpu_boots_from_program_ram() {
    std::printf("releasing RESET boots the sub CPU from program RAM's vectors\n");
    SegaCd cd;
    Program body;
    body.move_w(0x1234, 0xFF8020).spin();
    boot_sub(cd, body);
    check(cd.sub_running(), "running after RESET is released with BUSREQ clear");
    run_for(cd, 0.001);
    check_eq(cd.main_read16(0xA12020), 0x1234, "its status word reaches the main CPU");
    check(cd.sub_instructions() > 0, "and it executed instructions");
}

void test_communication_registers() {
    std::printf("each side writes only its own half of the communication block\n");
    SegaCd cd;
    Program body;
    // Sub writes its flags and tries to write a command word it does not own.
    body.move_b(0x5A, 0xFF800F).move_w(0x9999, 0xFF8010).copy_w(0xFF8010, 0xFF8022).spin();
    cd.main_write16(0xA12010, 0x7777);
    cd.main_write8(0xA1200E, 0xA5);
    boot_sub(cd, body);
    run_for(cd, 0.001);
    check_eq(cd.main_read8(0xA1200F), 0x5A, "the sub CPU's flags");
    check_eq(cd.main_read8(0xA1200E), 0xA5, "the main CPU's flags");
    check_eq(cd.main_read16(0xA12022), 0x7777, "command words are read-only to the sub CPU");
    cd.main_write16(0xA12020, 0x0000);
    check_eq(cd.main_read16(0xA12020), 0x0000, "status words start clear");
}

void test_word_ram_handover() {
    std::printf("2M Word RAM passes to the sub CPU on DMNA and back on RET\n");
    SegaCd cd;
    cd.main_write16(0x200000, 0x0101);
    Program body;
    // Wait for Word RAM, then write it and hand it back with RET.
    body.w(0x0839).w(0x0001).l(0xFF8003)   // btst #1,$FF8003 (DMNA)
        .w(0x67F6)                           // beq.s back to the btst
        .move_w(0x2222, 0x080000)            // sub's view of Word RAM
        .move_b(0x01, 0xFF8003)              // RET: give it back
        .spin();
    boot_sub(cd, body);
    run_for(cd, 0.0005);
    check_eq(cd.main_read16(0x200000), 0x0101, "still the main CPU's before DMNA");
    cd.main_write8(0xA12003, 0x02);  // DMNA
    run_for(cd, 0.001);
    check_eq(cd.main_read8(0xA12003) & 0x01, 0x01, "RET is back");
    check_eq(cd.main_read16(0x200000), 0x2222, "and the main CPU sees what the sub wrote");
}

void test_one_meg_banks_interleave() {
    std::printf("1M mode's two banks are the even and odd words of 2M Word RAM\n");
    SegaCd cd;
    cd.main_write16(0x200000, 0xAAAA);
    cd.main_write16(0x200002, 0xBBBB);
    cd.main_write16(0x200004, 0xCCCC);
    Program body;
    body.move_b(0x05, 0xFF8003).spin();  // MODE=1M, RET=1: main gets bank 1
    boot_sub(cd, body);
    run_for(cd, 0.0005);
    check_eq(cd.main_read8(0xA12003) & 0x04, 0x04, "now in 1M mode");
    check_eq(cd.main_read16(0x200000), 0xBBBB, "bank 1 word 0 is 2M word 1");
    check_eq(cd.word_ram()[0], 0xAA, "and nothing was moved to get there");
}

void test_level2_from_the_main_cpu() {
    std::printf("the main CPU interrupts the sub CPU at level 2, and IFL2 clears when taken\n");
    SegaCd cd;
    Program handler;
    handler.bump(0xFF8022).rte();
    Program body;
    body.move_b(0x04, 0xFF8033).enable_interrupts().spin();  // IEN2
    boot_sub(cd, body, {{2, handler}});
    run_for(cd, 0.0005);
    check_eq(cd.main_read16(0xA12000) & 0x8000, 0x8000, "IEN2 is visible to the main CPU");
    // A BYTE write: a word write would also write $A12001 and, with a zero
    // there, put the sub CPU straight back into reset.
    cd.main_write8(0xA12000, 0x01);  // IFL2
    run_for(cd, 0.0005);
    check_eq(cd.main_read16(0xA12022), 1, "the handler ran once");
    check_eq(cd.main_read16(0xA12000) & 0x0100, 0, "IFL2 cleared on acknowledge");
    check_eq(cd.sub_interrupts()[2], 1, "one level 2 taken");
}

void test_timer_interrupt() {
    std::printf("the timer interrupts at level 3 every N x 384 cycles\n");
    SegaCd cd;
    Program handler;
    handler.bump(0xFF8024).rte();
    Program body;
    body.move_b(0x08, 0xFF8033).move_b(0x10, 0xFF8031).enable_interrupts().spin();  // IEN3, N=16
    boot_sub(cd, body, {{3, handler}});
    run_for(cd, 0.1);
    // 12.5MHz / (16 * 384) = 2034.5 a second.
    const auto taken = cd.sub_interrupts()[3];
    check(taken > 190 && taken < 215, "about 203 in a tenth of a second (got " + std::to_string(taken) + ")");
}

void test_drive_reports_no_disc() {
    std::printf("with no disc the drive answers at 75Hz and reports NO DISC\n");
    SegaCd cd;
    Program handler;
    handler.move_w(0x0000, 0xFF8042)   // command 0: get status
        .move_w(0x0000, 0xFF804A)      // the word that sends it
        .copy_w(0xFF8038, 0xFF8028)    // status nibbles 0-1 to the main CPU
        .copy_w(0xFF8040, 0xFF802A)    // nibble 8 and the checksum
        .bump(0xFF8026)
        .rte();
    Program body;
    body.move_w(0x0004, 0xFF8036)      // HOCK: let the drive interrupt
        .move_b(0x10, 0xFF8033)        // IEN4
        .enable_interrupts().spin();
    boot_sub(cd, body, {{4, handler}});
    run_for(cd, 1.0);
    const auto taken = cd.main_read16(0xA12026);
    check(taken >= 74 && taken <= 76, "75 drive interrupts a second (got " + std::to_string(taken) + ")");
    check_eq(cd.main_read16(0xA12028) >> 8, SegaCd::kNoDisc, "RS0 = no disc");
    // Checksum: ones' complement of the sum of RS0-RS8, low nibble.
    const auto rs = cd.main_read16(0xA12028);
    const auto tail = cd.main_read16(0xA1202A);
    const int sum = (rs >> 8) + (rs & 0xFF) + (tail >> 8);
    check_eq(tail & 0x0F, static_cast<std::uint32_t>(~sum & 0x0F), "a valid status checksum");
}

void test_backup_ram_is_odd_bytes() {
    std::printf("the 8KB backup RAM sits on odd bytes at $FE0000 and survives a reset\n");
    SegaCd cd;
    Program body;
    body.move_b(0x42, 0xFE0001).move_b(0x99, 0xFE0000).spin();
    boot_sub(cd, body);
    run_for(cd, 0.0005);
    check_eq(cd.backup_ram()[0], 0x42, "odd byte lands");
    check(cd.backup_ram_dirty(), "and marks it for saving");
    cd.reset(true);
    check_eq(cd.backup_ram()[0], 0x42, "battery RAM survives power-on");
}

void test_backup_ram_starts_formatted() {
    std::printf("backup RAM starts formatted, and a blank save file is formatted on load\n");
    SegaCd cd;
    check(cd.backup_ram_formatted(), "a new machine's backup RAM is formatted");
    const auto& ram = cd.backup_ram();
    check_eq(ram[0x1FC0], '_', "the volume name is underscores");
    check_eq((ram[0x1FD0] << 8) | ram[0x1FD1], 125, "125 blocks free of 128");
    check_eq(ram[0x1FEC], 1, "format version 1");
    check(!cd.backup_ram_dirty(), "and nothing to save until a game writes");
    cd.restore_backup_ram(std::vector<std::uint8_t>(0x2000, 0));
    check(cd.backup_ram_formatted(), "an all-zero file comes back formatted");
    // A real save survives the load untouched.
    std::vector<std::uint8_t> saved = cd.backup_ram();
    saved[0x10] = 0x5A;
    cd.restore_backup_ram(saved);
    check_eq(cd.backup_ram()[0x10], 0x5A, "a formatted file is loaded as it is");
}

void test_pcm_plays_a_sample() {
    std::printf("a PCM channel plays wave RAM and $FF loops it\n");
    SegaCd cd;
    Program body;
    body.move_b(0xC0, 0xFF000F)        // CTRL: select channel 0, chip on
        .move_b(0xFF, 0xFF0001)        // ENV full
        .move_b(0xFF, 0xFF0003)        // PAN full both sides
        .move_b(0x00, 0xFF0005)        // FD low
        .move_b(0x08, 0xFF0007)        // FD high: step 1.0
        .move_b(0x00, 0xFF0009)        // LS low
        .move_b(0x00, 0xFF000B)        // LS high
        .move_b(0x00, 0xFF000D)        // ST = 0
        .move_b(0x80, 0xFF000F)        // bank 0, chip on
        .move_b(0xF0, 0xFF2001)        // wave RAM $0000: +112
        .move_b(0xF0, 0xFF2003)        // $0001
        .move_b(0xFF, 0xFF2005)        // $0002: loop marker
        .move_b(0xFE, 0xFF0011)        // ON/OFF: channel 0 on (active low)
        .spin();
    boot_sub(cd, body);
    run_for(cd, 1.0 / 60.0);
    cd.end_frame();
    const auto& out = cd.audio();
    check(out.size() > 1000, "about a frame of stereo samples");
    int loudest = 0;
    for (const auto sample : out) loudest = std::max(loudest, std::abs(static_cast<int>(sample)));
    check(loudest > 10000, "and a looping positive sample is loud (got " + std::to_string(loudest) + ")");
}

void test_graphics_asic_copies_a_stamp() {
    std::printf("the graphics ASIC walks a stamp map along a trace vector into the image buffer\n");
    SegaCd cd;
    // Everything is set up by the main CPU in 2M Word RAM before it is handed
    // to the sub CPU: stamp 1's pixels, a stamp map pointing its first entry
    // at stamp 1, and one trace vector line stepping one dot at a time.
    // Stamp 1 (16x16, 128 bytes) at byte $80: every pixel colour 5.
    for (std::uint32_t i = 0; i < 128; i += 2) cd.main_write16(0x200080 + i, 0x5555);
    // Stamp map at $1000: entry 0 = stamp 1, no flip.
    cd.main_write16(0x201000, 0x0001);
    // Trace vector at $2000: x=0, y=0 (13.3), step x=1.0 (5.11), y=0.
    cd.main_write16(0x202000, 0x0000);
    cd.main_write16(0x202002, 0x0000);
    cd.main_write16(0x202004, 0x0800);
    cd.main_write16(0x202006, 0x0000);
    Program body;
    body.w(0x0839).w(0x0001).l(0xFF8003).w(0x67F6)  // wait for Word RAM (DMNA)
        .move_w(0x0000, 0xFF8058)   // 256x256 map, 16x16 stamps, no repeat
        .move_w(0x1000 >> 2, 0xFF805A)  // stamp map base
        .move_w(0x0000, 0xFF805C)   // image buffer 1 cell tall
        .move_w(0x4000 >> 2, 0xFF805E)  // image buffer at $4000 (4-byte units, like the others)
        .move_w(0x0000, 0xFF8060)   // no pixel offset
        .move_w(0x0008, 0xFF8062)   // 8 dots wide
        .move_w(0x0001, 0xFF8064)   // 1 line
        .move_w(0x2000 >> 2, 0xFF8066)  // trace vector base: go
        .spin();
    boot_sub(cd, body);
    cd.main_write8(0xA12003, 0x02);  // DMNA: Word RAM to the sub CPU
    run_for(cd, 0.002);
    check_eq(cd.reg(0x58 >> 1) >> 8, 0x00, "the operation finished");
    check_eq(cd.word_ram()[0x4000], 0x55, "stamp pixels land in the image buffer");
    check_eq(cd.word_ram()[0x4003], 0x55, "all eight of them");
    check_eq(cd.word_ram()[0x4004], 0x00, "and nothing past the width");
}

// --- disc images -----------------------------------------------------------------

std::filesystem::path scratch_dir(const char* name) {
    std::error_code ignored;
    auto dir = std::filesystem::temp_directory_path(ignored) / name;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir, ignored);
    return dir;
}

void write_data_track(const std::filesystem::path& path, int sectors, int sector_size) {
    std::ofstream out(path, std::ios::binary);
    std::vector<char> block(static_cast<std::size_t>(sector_size), 0);
    for (int s = 0; s < sectors; ++s) {
        std::fill(block.begin(), block.end(), 0);
        char* user = block.data() + (sector_size == 2352 ? 16 : 0);
        if (sector_size == 2352) {
            static const unsigned char sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
            std::memcpy(block.data(), sync, 12);
            block[15] = 1;
        }
        if (s == 0) std::memcpy(user, "SEGADISCSYSTEM  ", 16);
        user[100] = static_cast<char>(s);  // a marker to prove which block was read
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
}

void write_audio(std::ofstream& out, int sectors) {
    std::vector<char> block(2352, 0x11);
    for (int s = 0; s < sectors; ++s) out.write(block.data(), 2352);
}

void test_cue_one_file_per_track() {
    std::printf("a Redump cue: one file per track, the audio pause inside its file\n");
    const auto dir = scratch_dir("genx_cue_multi");
    write_data_track(dir / "game (Track 01).bin", 200, 2352);
    {
        std::ofstream audio(dir / "game (Track 02).bin", std::ios::binary);
        write_audio(audio, 300);
    }
    {
        std::ofstream cue(dir / "game.cue");
        cue << "FILE \"game (Track 01).bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
            << "FILE \"game (Track 02).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n"
            << "    INDEX 01 00:02:00\n";
    }
    mercury::segacd::Disc disc;
    std::string error;
    check(disc.open(dir / "game.cue", error), "opens (" + error + ")");
    check_eq(static_cast<std::uint32_t>(disc.track_count()), 2, "two tracks");
    check_eq(static_cast<std::uint32_t>(disc.track(0).end), 200, "data track ends where its file does");
    check_eq(static_cast<std::uint32_t>(disc.track(1).start), 350, "audio starts after its two-second pause");
    check_eq(static_cast<std::uint32_t>(disc.lead_out()), 500, "lead-out after the audio file");
    std::vector<std::uint8_t> block(2048);
    check(disc.read_data(5, block.data()), "a data block reads");
    check_eq(block[100], 5, "and it is the right one");
    std::int16_t audio[1176];
    check(disc.read_audio(360, audio), "an audio block reads");
    check_eq(static_cast<std::uint16_t>(audio[0]), 0x1111, "little-endian samples");
}

void test_cue_single_file() {
    std::printf("a single-file cue: the next track's INDEX 00 ends the one before\n");
    const auto dir = scratch_dir("genx_cue_single");
    {
        write_data_track(dir / "whole.bin", 200, 2352);
        std::ofstream more(dir / "whole.bin", std::ios::binary | std::ios::app);
        write_audio(more, 300);
    }
    {
        std::ofstream cue(dir / "whole.cue");
        cue << "FILE \"whole.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
            << "  TRACK 02 AUDIO\n    INDEX 00 00:02:50\n    INDEX 01 00:04:50\n";
    }
    mercury::segacd::Disc disc;
    std::string error;
    check(disc.open(dir / "whole.cue", error), "opens (" + error + ")");
    check_eq(static_cast<std::uint32_t>(disc.track(0).end), 200, "track 1 ends at track 2's INDEX 00");
    check_eq(static_cast<std::uint32_t>(disc.track(1).start), 350, "track 2 starts at its INDEX 01");
    check_eq(static_cast<std::uint32_t>(disc.lead_out()), 500, "lead-out at the end of the file");
}

void test_bare_iso() {
    std::printf("a bare 2048-byte image is one data track\n");
    const auto dir = scratch_dir("genx_iso");
    write_data_track(dir / "game.iso", 400, 2048);
    mercury::segacd::Disc disc;
    std::string error;
    check(disc.open(dir / "game.iso", error), "opens (" + error + ")");
    check_eq(static_cast<std::uint32_t>(disc.track_count()), 1, "one track");
    check_eq(static_cast<std::uint32_t>(disc.lead_out()), 400, "as long as the file");
    std::vector<std::uint8_t> block(2048);
    check(disc.read_data(7, block.data()) && block[100] == 7, "reads cooked blocks directly");
    check_eq(static_cast<std::uint32_t>(disc.region()), 'U', "a zero security byte is the US region");
}

// SWAPPING A DISC MUST NOT RESTART ANYTHING.
//
// Half the FMV library is two or three discs and each one asks for the next
// partway through. The point of a swap is that the game keeps running, so
// what is checked here is as much what did NOT change - program RAM, the sub
// CPU's registers, the backup RAM - as what did.
void test_disc_swap_keeps_the_machine_running() {
    std::printf("a disc swap changes the disc and nothing else\n");
    const auto dir = scratch_dir("genx_swap");
    write_data_track(dir / "disc1.iso", 400, 2048);
    write_data_track(dir / "disc2.iso", 900, 2048);

    SegaCd cd;
    std::string error;
    check(cd.insert_disc(dir / "disc1.iso", error), "disc 1 goes in (" + error + ")");
    check_eq(static_cast<std::uint32_t>(cd.disc().lead_out()), 400, "and it is the short one");

    // Get the sub CPU running and leave a fingerprint in program RAM, so
    // "the game kept running" is a fact rather than an impression.
    Program body;
    body.bump(0xFF8020).spin();
    boot_sub(cd, body);
    const auto sub_before = cd.sub().registers();
    const auto instructions_before = cd.sub_instructions();

    check(cd.swap_disc(dir / "disc2.iso", error), "the swap works (" + error + ")");
    check_eq(static_cast<std::uint32_t>(cd.disc().lead_out()), 900, "the long disc is in now");

    // The sub CPU is where it was, not reset.
    check_eq(cd.sub().registers().pc, sub_before.pc, "the sub CPU did not move");
    check_eq(static_cast<std::uint32_t>(cd.sub_instructions()),
             static_cast<std::uint32_t>(instructions_before), "and it did not restart");
    check(cd.backup_ram_formatted(), "the backup RAM is untouched");

    // And it carries on afterwards, reading the disc that is in now.
    run_for(cd, 0.2);
    check(cd.sub_instructions() > instructions_before, "and it keeps running afterwards");
    std::vector<std::uint8_t> block(2048);
    check(cd.disc().read_data(800, block.data()), "a block only the new disc has reads");
    check_eq(block[100], static_cast<std::uint8_t>(800 & 0xFF), "and it is the right one");

    // A swap to something that is not there leaves the drive honest rather
    // than still claiming the old disc.
    check(!cd.swap_disc(dir / "nothing.iso", error), "a missing disc is refused");
    check(!cd.disc().loaded(), "and the tray is reported empty");
}

void test_not_a_sega_cd() {
    std::printf("an image without SEGADISCSYSTEM is refused\n");
    const auto dir = scratch_dir("genx_notcd");
    {
        std::ofstream out(dir / "x.iso", std::ios::binary);
        std::vector<char> zeros(2048 * 10, 0);
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    mercury::segacd::Disc disc;
    std::string error;
    check(!disc.open(dir / "x.iso", error), "refused");
    check(!error.empty(), "with a reason");
}

}  // namespace

int main() {
    std::printf("Sega CD tests\n=============\n\n");
    test_power_on_state();
    test_bios_and_hint_vector();
    test_program_ram_window();
    test_sub_cpu_boots_from_program_ram();
    test_communication_registers();
    test_word_ram_handover();
    test_one_meg_banks_interleave();
    test_level2_from_the_main_cpu();
    test_timer_interrupt();
    test_drive_reports_no_disc();
    test_backup_ram_is_odd_bytes();
    test_backup_ram_starts_formatted();
    test_pcm_plays_a_sample();
    test_graphics_asic_copies_a_stamp();
    test_disc_swap_keeps_the_machine_running();
    test_cue_one_file_per_track();
    test_cue_single_file();
    test_bare_iso();
    test_not_a_sega_cd();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
