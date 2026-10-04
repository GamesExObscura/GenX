#include "mercury/segacd/segacd.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mercury::segacd {

namespace {

// One tick of the timer and of the stopwatch: 384 sub-CPU cycles, 30.72us.
constexpr std::uint32_t kTimerTick = 384;

// The main CPU's view of Word RAM's "cell image" in 1M mode: $220000-$23FFFF
// is the main CPU's bank again, rearranged so that consecutive addresses walk
// down a column of 8x8 cells instead of along a line. A game draws a bitmap
// into the bank as lines and has the video chip DMA it out of the cell image
// as ready-made tiles. The layout is the hardware's, taken from Genesis Plus
// GX's table: four regions of decreasing height, each laid out column-first.
std::array<std::uint16_t, 0x8000> build_cell_table() {
    std::array<std::uint16_t, 0x8000> table{};
    for (std::uint32_t i = 0; i < 0x4000; ++i) {  // 64 x 32 cells
        table[i] = static_cast<std::uint16_t>(((i & 7) << 8) | (((i >> 8) & 0x3F) << 2) |
                                              (((i >> 3) & 0x1F) << 11));
    }
    for (std::uint32_t i = 0x4000; i < 0x6000; ++i) {  // 64 x 16
        table[i] = static_cast<std::uint16_t>(((i & 7) << 8) | (((i >> 7) & 0x3F) << 2) |
                                              (((i >> 3) & 0x0F) << 11));
    }
    for (std::uint32_t i = 0x6000; i < 0x7000; ++i) {  // 64 x 8
        table[i] = static_cast<std::uint16_t>((((i & 7) << 8) | (((i >> 6) & 0x3F) << 2) |
                                               (((i >> 3) & 0x07) << 11)) | 0x8000);
    }
    for (std::uint32_t i = 0x7000; i < 0x7800; ++i) {  // 64 x 4
        table[i] = static_cast<std::uint16_t>((((i & 7) << 8) | (((i >> 5) & 0x3F) << 2) |
                                               (((i >> 3) & 0x03) << 11)) | 0xC000);
    }
    for (std::uint32_t i = 0x7800; i < 0x8000; ++i) {  // 64 x 4
        table[i] = static_cast<std::uint16_t>((((i & 7) << 8) | (((i >> 5) & 0x3F) << 2) |
                                               (((i >> 3) & 0x03) << 11)) | 0xE000);
    }
    return table;
}

const std::array<std::uint16_t, 0x8000>& cell_table() {
    static const auto table = build_cell_table();
    return table;
}

// A write into the sub CPU's 1M "dot image" is a pair of 4-bit pixels, and
// the priority mode decides whether a new pixel may replace an old one.
std::uint8_t apply_priority(int mode, std::uint8_t previous, std::uint8_t incoming) noexcept {
    const auto pick = [](std::uint8_t keep_if_set, std::uint8_t other, std::uint8_t mask) {
        return static_cast<std::uint8_t>((keep_if_set & mask) != 0 ? (keep_if_set & mask)
                                                                   : (other & mask));
    };
    switch (mode & 3) {
        case 1:  // underwrite: only fill pixels that are still zero
            return static_cast<std::uint8_t>(pick(previous, incoming, 0x0F) |
                                             pick(previous, incoming, 0xF0));
        case 2:  // overwrite: a zero pixel in the new data leaves the old one
            return static_cast<std::uint8_t>(pick(incoming, previous, 0x0F) |
                                             pick(incoming, previous, 0xF0));
        case 3:  // not a valid mode: nothing changes
            return previous;
        default:
            return incoming;
    }
}

}  // namespace

SegaCd::SegaCd() {
    mercury::core::M68kBusCallbacks bus;
    bus.read16 = [this](std::uint32_t address) { return sub_read16(address); };
    bus.write16 = [this](std::uint32_t address, std::uint16_t value) { sub_write16(address, value); };
    bus.read8 = [this](std::uint32_t address) { return sub_read8(address); };
    bus.write8 = [this](std::uint32_t address, std::uint8_t value) { sub_write8(address, value); };
    sub_.attach_bus(std::move(bus));
    // Formatted, as a console leaves the factory. A saved file replaces it
    // through restore_backup_ram.
    format_backup_ram();
    reset(true);
}

// --- setup -------------------------------------------------------------------

bool SegaCd::load_bios(const std::vector<std::uint8_t>& image, std::string& error) {
    if (image.size() != kBiosSize) {
        error = "a Sega CD BIOS is 128KB; this file is " + std::to_string(image.size()) + " bytes";
        return false;
    }
    bios_ = image;
    bios_loaded_ = true;
    return true;
}

std::string SegaCd::bios_description() const {
    if (!bios_loaded_) {
        return "no BIOS";
    }
    // The BIOS carries a Mega Drive-style header at $100.
    auto field = [this](std::size_t from, std::size_t length) {
        std::string text(reinterpret_cast<const char*>(bios_.data() + from), length);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
        return text;
    };
    return field(0x120, 0x30) + " [" + field(0x180, 0x0E) + "] region " + field(0x1F0, 0x03);
}

namespace {
std::uint64_t fnv(std::uint64_t hash, const std::uint8_t* data, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
}  // namespace

std::uint64_t SegaCd::bios_id() const noexcept {
    if (!bios_loaded_) {
        return 0;
    }
    const auto hash = fnv(1469598103934665603ull, bios_.data(), bios_.size());
    return hash == 0 ? 1 : hash;
}

std::uint64_t SegaCd::disc_id() const noexcept {
    if (!disc_.loaded()) {
        return 0;
    }
    const auto& header = disc_.header();
    std::uint64_t hash = fnv(1469598103934665603ull, header.data(), header.size());
    const std::int32_t shape[2] = {disc_.track_count(), disc_.lead_out()};
    hash = fnv(hash, reinterpret_cast<const std::uint8_t*>(shape), sizeof(shape));
    return hash == 0 ? 1 : hash;
}

char SegaCd::bios_region() const noexcept {
    if (!bios_loaded_) {
        return 0;
    }
    for (int i = 0; i < 3; ++i) {
        const char c = static_cast<char>(bios_[0x1F0 + static_cast<std::size_t>(i)]);
        if (c == 'U' || c == 'E' || c == 'J') {
            return c;
        }
    }
    return 0;
}

bool SegaCd::insert_disc(const std::filesystem::path& image, std::string& error) {
    if (!disc_.open(image, error)) {
        return false;
    }
    cdd_reset();
    return true;
}

void SegaCd::eject_disc() {
    disc_.close();
    cdd_reset();
}

void SegaCd::restore_backup_ram(const std::vector<std::uint8_t>& bytes) noexcept {
    std::fill(backup_ram_.begin(), backup_ram_.end(), std::uint8_t{0});
    std::copy_n(bytes.begin(), std::min(bytes.size(), backup_ram_.size()), backup_ram_.begin());
    // A file that never held a format holds no saves either, so formatting
    // it loses nothing - and leaving it blank stops games at "Internal RAM
    // is not initialized".
    if (!backup_ram_formatted()) {
        format_backup_ram();
    }
    backup_ram_dirty_ = false;
}

// THE DIRECTORY A FORMATTED BACKUP RAM ENDS WITH.
//
// A console's backup RAM is formatted at the factory, and every game reads
// this block before it will save. Blank, Sonic CD stops on "Internal RAM is
// not initialized - use the control panel to format the RAM". It is what the
// BIOS's own format command writes: zeros for the save area, then 64 bytes at
// the very end -
//   +$00  eleven '_' (the empty volume name), three zeros, then $00 $40
//   +$10  the free block count, four times over as big-endian words: the
//         8KB is 128 blocks of 64 bytes, less three for the directory
//   +$20  "SEGA_CD_ROM", a zero, then format version $01 and two zeros
//   +$30  "RAM_CARTRIDGE___"
bool SegaCd::backup_ram_formatted() const noexcept {
    static constexpr char kSignature[] = "SEGA_CD_ROM";
    const std::size_t at = backup_ram_.size() - 0x20;
    return std::equal(kSignature, kSignature + 11, backup_ram_.begin() + static_cast<std::ptrdiff_t>(at));
}

void SegaCd::format_backup_ram() noexcept {
    std::fill(backup_ram_.begin(), backup_ram_.end(), std::uint8_t{0});
    std::uint8_t* tail = backup_ram_.data() + backup_ram_.size() - 0x40;
    std::fill(tail, tail + 11, static_cast<std::uint8_t>('_'));
    tail[0x0F] = 0x40;
    const auto free_blocks = static_cast<std::uint16_t>(backup_ram_.size() / 64 - 3);
    for (int copy = 0; copy < 4; ++copy) {
        tail[0x10 + copy * 2] = static_cast<std::uint8_t>(free_blocks >> 8);
        tail[0x11 + copy * 2] = static_cast<std::uint8_t>(free_blocks);
    }
    static constexpr char kSystem[] = "SEGA_CD_ROM";
    std::copy(kSystem, kSystem + 11, tail + 0x20);
    tail[0x2C] = 0x01;
    static constexpr char kMedia[] = "RAM_CARTRIDGE___";
    std::copy(kMedia, kMedia + 16, tail + 0x30);
}

void SegaCd::sanitize_loaded() noexcept {
    // The PCM window is one of sixteen 4KB banks and the register picks one of
    // eight channels - both index memory directly, so a file's word on them
    // is never taken as given.
    pcm_bank_ &= 0xF000;
    pcm_selected_ &= 0x07;
    // Track numbers run 0..track_count (the last meaning "in the lead-out").
    // Disc::track() checks its index and throws, which in this noexcept code
    // ends the program.
    if (cdd_track_ < 0) cdd_track_ = 0;
    if (cdd_track_ > disc_.track_count()) cdd_track_ = disc_.track_count();
    // A seek costs at most a couple of hundred 75Hz ticks.
    if (cdd_latency_ < 0 || cdd_latency_ > 10'000) cdd_latency_ = 0;
    // Counters that the frame loop works down one step at a time: past these
    // limits they are not a state the hardware can be in, only a hang.
    cdd_phase_ %= 500'000;
    const auto bound = [](std::int64_t& value, std::int64_t limit) {
        if (value > limit || value < -limit) value = 0;
    };
    bound(budget_, 1'000'000'000'000);
    bound(timer_left_, 100'000'000);
    // Divides by this; 0 is a crash, not a speed.
    if (gfx_.cycles_per_line == 0) gfx_.cycles_per_line = 1;
    if (gfx_.stamp_shift > 31) gfx_.stamp_shift = 15;
    if (gfx_.map_shift > 31) gfx_.map_shift = 4;
    // The PCM clock is caught up to now_ at the end of every frame, so a real
    // state never has it more than a frame apart; anything further would make
    // the next catch-up generate samples for ever. (The graphics and CDC DMA
    // clocks are NOT bounded here: both legitimately sit far behind while
    // idle, and both catch-ups are already capped - by the lines left in the
    // job and by the byte counter - so neither can spin.)
    constexpr std::uint64_t kWindow = 50'000'000;
    const std::uint64_t pcm_distance = pcm_clock_ > now_ ? pcm_clock_ - now_ : now_ - pcm_clock_;
    if (pcm_distance > kWindow) pcm_clock_ = now_;
    // The resampler's position within the next frame's samples: a fraction
    // under a couple of samples. Negative or huge, it becomes an index.
    if (!(pcm_resample_phase_ >= 0.0 && pcm_resample_phase_ < 4.0)) pcm_resample_phase_ = 0.0;
}

void SegaCd::reset(bool power_on) {
    if (power_on) {
        std::fill(prg_ram_.begin(), prg_ram_.end(), std::uint8_t{0});
        std::fill(word_ram_.begin(), word_ram_.end(), std::uint8_t{0});
        now_ = 0;
    }
    regs_.fill(0);
    // Power-on values on the main side: the sub CPU held (RESET clear, bus
    // request set), and Word RAM in 2M mode belonging to the main CPU.
    regs_[0x00] = 0x0002;
    regs_[0x01] = 0x0001;
    hint_vector_ = 0xFFFF;
    led_ = 0;
    dmna_pending_ = false;
    sub_word_ram_wait_ = false;
    deferred_.clear();
    budget_ = 0;

    // Sub-side defaults - Genesis Plus GX's reset values, which match what
    // the BIOS expects to find: the CDC host data and DMA address read as all
    // ones, the drive status checksum is valid for an all-zero status, and
    // the command registers are all ones until the BIOS writes one.
    regs_[0x08 >> 1] = 0xFFFF;
    regs_[0x0A >> 1] = 0xFFFF;
    regs_[0x36 >> 1] = 0x0100;
    regs_[0x40 >> 1] = 0x000F;
    for (std::uint32_t at = 0x42; at <= 0x4A; at += 2) {
        regs_[at >> 1] = 0xFFFF;
    }
    pending_ = 0;
    timer_left_ = 0;
    stopwatch_base_ = now_;
    cdd_phase_ = 0;
    cdd_reset();
    cdc_reset();
    gfx_reset();
    pcm_reset();
    // Held in reset until the main CPU releases it, which is when the vectors
    // it boots from have been copied into program RAM. Resetting it here as
    // well only makes its registers tidy.
    sub_.reset();
    sub_.set_interrupt_level(0);
    audio_.clear();
    cdda_.clear();
    pcm_native_.clear();
}

bool SegaCd::sub_running() const noexcept {
    // RESET released and the bus not requested.
    return (lo(0x00) & 0x01) != 0 && (lo(0x00) & 0x02) == 0;
}

// --- the main CPU's memory ----------------------------------------------------

std::uint32_t SegaCd::cell_image_offset(std::uint32_t address) const noexcept {
    return static_cast<std::uint32_t>(cell_table()[(address >> 2) & 0x7FFF]) | (address & 0x10003);
}

std::uint16_t SegaCd::main_read16(std::uint32_t address) noexcept {
    // A processor always sees its own writes in program order, so anything
    // it queued lands before it reads. (Between instructions the queue is
    // already empty; this only matters inside one, and to a test harness.)
    if (!deferred_.empty()) flush_main_writes();
    if (main_decodes_registers(address)) {
        // Registers are read as words, not as two bytes: reading the CDC
        // data register moves the transfer on, and must only happen once.
        return main_register_read16(address);
    }
    return static_cast<std::uint16_t>((main_read8(address & ~1u) << 8) | main_read8(address | 1u));
}

std::uint8_t SegaCd::main_read8(std::uint32_t address) noexcept {
    if (!deferred_.empty()) flush_main_writes();  // see main_read16
    if (main_decodes_registers(address)) {
        return main_register_read8(address);
    }
    const std::uint32_t a = address & 0x3FFFFF;
    if (a < 0x200000) {
        if ((a & 0x20000) == 0) {
            // The BIOS, mirrored every 256KB. The low word of the horizontal
            // interrupt vector comes from the gate array rather than the ROM,
            // so a game can point it at its own handler; the high word reads
            // $FFFF so the handler is always in work RAM.
            const std::uint32_t offset = a & 0x1FFFF;
            if (offset >= 0x70 && offset <= 0x73) {
                if (offset < 0x72) return 0xFF;
                return offset == 0x72 ? static_cast<std::uint8_t>(hint_vector_ >> 8)
                                      : static_cast<std::uint8_t>(hint_vector_);
            }
            return bios_[offset];
        }
        // The program RAM window: one 128KB bank of the sub CPU's 512KB,
        // chosen by BK0-1. Only reachable while the sub CPU's bus is
        // requested - Dungeon Explorer depends on that.
        if ((lo(0x00) & 0x02) == 0) {
            return 0xFF;
        }
        const std::uint32_t bank = (lo(0x01) >> 6) & 3;
        return prg_ram_[bank * 0x20000 + (a & 0x1FFFF)];
    }
    // Word RAM, mirrored up to $3FFFFF.
    if (!one_meg_mode()) {
        return main_owns_word_ram() ? word_ram_[a & 0x3FFFF] : 0xFF;
    }
    if ((a & 0x20000) == 0) {
        return word_ram_[bank_offset(main_bank(), a & 0x1FFFF)];
    }
    return word_ram_[bank_offset(main_bank(), cell_image_offset(a) & 0x1FFFF)];
}

// A MAIN-CPU REGISTER WRITE LANDS AT THE END OF ITS INSTRUCTION.
//
// The main CPU runs a whole instruction and the sub CPU is then brought up to
// the same moment, so anything the main CPU wrote used to be visible to the
// sub from the START of that instruction - twenty-odd cycles before the write
// happens on hardware. Most of the time nobody can tell. A handshake can:
// Dragon's Lair's main CPU sets DMNA with BSET and loops until a second BSET
// reads it back set, while its sub CPU polls DMNA and switches Word RAM to 1M
// the moment it sees it - which clears DMNA. On hardware the sub needs ~68 of
// its cycles to get there and the main's re-read comes ~22 main cycles after
// its write, so the main wins. Seen twenty cycles early, the sub won, and the
// main spun on a bit that could never come back.
//
// So register writes from the main side wait until the sub has run through
// the instruction's time (flushed at the end of advance). Reads are
// unchanged. MERCURY_SCD_IMMEDIATE_WRITES=1 puts them back to immediate, for
// bisecting.
namespace {
const bool kImmediateMainWrites = [] {
    const char* e = std::getenv("MERCURY_SCD_IMMEDIATE_WRITES");
    return e != nullptr && *e != '0';
}();
}  // namespace

void SegaCd::flush_main_writes() noexcept {
    // Indexed, not iterated: a write can itself queue nothing, but keep the
    // loop honest if one ever does.
    for (std::size_t i = 0; i < deferred_.size(); ++i) {
        const DeferredWrite w = deferred_[i];
        if (w.word) {
            main_register_write16(w.address, w.value);
        } else {
            main_register_write8(w.address, static_cast<std::uint8_t>(w.value));
        }
    }
    deferred_.clear();
}

void SegaCd::main_write16(std::uint32_t address, std::uint16_t value) noexcept {
    if (main_decodes_registers(address)) {
        if (kImmediateMainWrites) {
            main_register_write16(address, value);
        } else {
            deferred_.push_back({address, value, true});
        }
        return;
    }
    main_write8(address & ~1u, static_cast<std::uint8_t>(value >> 8));
    main_write8(address | 1u, static_cast<std::uint8_t>(value));
}

void SegaCd::main_write8(std::uint32_t address, std::uint8_t value) noexcept {
    if (main_decodes_registers(address)) {
        if (kImmediateMainWrites) {
            main_register_write8(address, value);
        } else {
            deferred_.push_back({address, value, false});
        }
        return;
    }
    if (!deferred_.empty()) flush_main_writes();  // a bank select must land first
    const std::uint32_t a = address & 0x3FFFFF;
    if (a < 0x200000) {
        if ((a & 0x20000) == 0 || (lo(0x00) & 0x02) == 0) {
            return;  // the BIOS is ROM; the window is closed without BUSREQ
        }
        const std::uint32_t bank = (lo(0x01) >> 6) & 3;
        prg_ram_[bank * 0x20000 + (a & 0x1FFFF)] = value;
        return;
    }
    if (!one_meg_mode()) {
        if (main_owns_word_ram()) {
            word_ram_[a & 0x3FFFF] = value;
        }
        return;
    }
    if ((a & 0x20000) == 0) {
        word_ram_[bank_offset(main_bank(), a & 0x1FFFF)] = value;
    } else {
        word_ram_[bank_offset(main_bank(), cell_image_offset(a) & 0x1FFFF)] = value;
    }
}

// --- the gate array, main side ------------------------------------------------

std::uint16_t SegaCd::main_register_read16(std::uint32_t address) noexcept {
    const std::uint32_t index = address & 0x3F;  // $A12000-$A1203F, mirrored
    switch (index & 0x3E) {
        case 0x02:
            // PM0-1 belong to the sub side and read as zero here.
            return static_cast<std::uint16_t>(regs_[0x01] & 0xFFC7);
        case 0x04:
            // The CDC register address is the sub side's; only the mode byte.
            return static_cast<std::uint16_t>((hi(0x02) & 0xC7) << 8);
        case 0x06:
            return hint_vector_;
        case 0x08:
            return cdc_host_read(true);
        case 0x0C: {
            const auto ticks = (now_ - stopwatch_base_) / kTimerTick;
            return static_cast<std::uint16_t>((regs_[0x06] + ticks) & 0xFFF);
        }
        default:
            break;
    }
    if (index < 0x30) {
        return regs_[index >> 1];
    }
    return 0xFFFF;
}

std::uint8_t SegaCd::main_register_read8(std::uint32_t address) noexcept {
    const std::uint32_t index = address & 0x3F;
    switch (index) {
        case 0x03: return static_cast<std::uint8_t>(lo(0x01) & 0xC7);
        case 0x04: return static_cast<std::uint8_t>(hi(0x02) & 0xC7);
        case 0x05: return 0x00;
        case 0x06: return static_cast<std::uint8_t>(hint_vector_ >> 8);
        case 0x07: return static_cast<std::uint8_t>(hint_vector_);
        case 0x08: case 0x09: {
            const auto word = cdc_host_read(true);
            return (index & 1) != 0 ? static_cast<std::uint8_t>(word) : static_cast<std::uint8_t>(word >> 8);
        }
        case 0x0C: case 0x0D: {
            const auto word = main_register_read16(address & ~1u);
            return (index & 1) != 0 ? static_cast<std::uint8_t>(word) : static_cast<std::uint8_t>(word >> 8);
        }
        default:
            break;
    }
    if (index < 0x30) {
        return (index & 1) != 0 ? lo(index >> 1) : hi(index >> 1);
    }
    return 0xFF;
}

void SegaCd::main_register_write16(std::uint32_t address, std::uint16_t value) noexcept {
    const std::uint32_t index = address & 0x3E;
    switch (index) {
        case 0x00:
            write_sub_control(static_cast<std::uint8_t>(value));
            if ((value & 0x0100) != 0) {
                request_level2();
            }
            return;
        case 0x02:
            set_hi(0x01, static_cast<std::uint8_t>(value >> 8));  // write protect
            main_write_memory_mode(static_cast<std::uint8_t>(value));
            return;
        case 0x06:
            hint_vector_ = value;
            return;
        case 0x08:
            // A write reads the data register too - verified on hardware by
            // Krikzz's mcd-verificator, per Genesis Plus GX.
            (void)cdc_host_read(true);
            return;
        case 0x0E:
            // Only the main CPU's own flag byte changes - and it takes the
            // word's LOW byte, D8-D15 being ignored. Genesis Plus GX's note
            // cites Mortal Kombat. This took the high byte, which a game
            // writing its flags as a word would find quietly zeroed.
            set_hi(0x07, static_cast<std::uint8_t>(value));
            return;
        default:
            break;
    }
    if (index >= 0x10 && index < 0x20) {
        regs_[index >> 1] = value;  // command words, main to sub
    }
}

void SegaCd::main_register_write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t index = address & 0x3F;
    switch (index) {
        case 0x00:
            // IFL2: writing a one interrupts the sub CPU; a zero does nothing.
            if ((value & 0x01) != 0) {
                request_level2();
            }
            return;
        case 0x01:
            write_sub_control(value);
            return;
        case 0x02:
            set_hi(0x01, value);
            return;
        case 0x03:
            main_write_memory_mode(value);
            return;
        case 0x06:
            hint_vector_ = static_cast<std::uint16_t>((hint_vector_ & 0x00FF) | (value << 8));
            return;
        case 0x07:
            hint_vector_ = static_cast<std::uint16_t>((hint_vector_ & 0xFF00) | value);
            return;
        case 0x0E:
        case 0x0F:
            // The byte lane is ignored: either address writes the main
            // flags. Space Ace and Dragon's Lair write the odd one.
            set_hi(0x07, value);
            return;
        default:
            break;
    }
    if (index >= 0x10 && index < 0x20) {
        if ((index & 1) != 0) set_lo(index >> 1, value); else set_hi(index >> 1, value);
    }
}

void SegaCd::write_sub_control(std::uint8_t value) noexcept {
    if ((value & 0x01) != 0) {
        // Reset is taken on the RELEASE of /RESET, 0 to 1.
        if ((lo(0x00) & 0x01) == 0) {
            sub_.reset();
        }
        set_lo(0x00, static_cast<std::uint8_t>(value & 0x03));
    } else {
        // Held in reset. The bus request reads back set while it is.
        set_lo(0x00, 0x02);
    }
}

void SegaCd::request_level2() noexcept {
    if ((lo(0x19) & 0x04) == 0) {
        return;  // the sub side has level 2 masked
    }
    set_hi(0x00, static_cast<std::uint8_t>(hi(0x00) | 0x01));  // IFL2
    pending_ |= 1u << 2;
    update_sub_interrupt();
}

// MERCURY_SCD_MODE_LOG=N: the first N writes to the memory mode register
// ($A12003 / $FF8003), from either side, with the register before and after,
// the pending-swap flag, the sub CPU's PC and the time in frames. Word RAM
// ownership is a handshake between two processors, and a game that hangs in
// it is waiting on a bit the other side never set - this says which, and who
// wrote what in what order.
void SegaCd::log_memory_mode(char side, std::uint8_t value, std::uint8_t before) const {
    static const long limit = [] {
        const char* e = std::getenv("MERCURY_SCD_MODE_LOG");
        return e == nullptr ? 0L : std::strtol(e, nullptr, 10);
    }();
    static long logged = 0;
    if (logged >= limit) return;
    ++logged;
    std::fprintf(stderr, "[scd mode] f%.1f %c wrote %02X: %02X -> %02X pending=%d subpc=%06X\n",
                 static_cast<double>(now_) / (static_cast<double>(kSubClockHz) / 60.0), side, value,
                 before, lo(0x01), dmna_pending_ ? 1 : 0,
                 static_cast<unsigned>(sub_.registers().pc));
}

void SegaCd::main_write_memory_mode(std::uint8_t value) noexcept {
    const std::uint8_t before = lo(0x01);
    main_write_memory_mode_inner(value);
    // Handing Word RAM over releases a sub CPU stopped on it.
    if (!sub_word_ram_locked()) sub_word_ram_wait_ = false;
    log_memory_mode('M', value, before);
}

void SegaCd::sub_write_memory_mode(std::uint8_t value) noexcept {
    const std::uint8_t before = lo(0x01);
    sub_write_memory_mode_inner(value);
    if (!sub_word_ram_locked()) sub_word_ram_wait_ = false;
    log_memory_mode('S', value, before);
}

void SegaCd::main_write_memory_mode_inner(std::uint8_t value) noexcept {
    const std::uint8_t current = lo(0x01);
    if (one_meg_mode()) {
        if ((value & 0x02) != 0) {
            // Asks for the banks to be swapped; the sub CPU completes it.
            dmna_pending_ = true;
            set_lo(0x01, static_cast<std::uint8_t>((current & ~0xC0) | (value & 0xC0)));
        } else {
            // Writing zero to DMNA in 1M mode SETS it.
            set_lo(0x01, static_cast<std::uint8_t>((current & ~0xC2) | ((value | 0x02) & 0xC2)));
        }
        return;
    }
    if ((value & 0x02) != 0) {
        // 2M: give Word RAM to the sub CPU. RET clears, DMNA sets.
        dmna_pending_ = true;
        set_lo(0x01, static_cast<std::uint8_t>((current & ~0xC3) | (value & 0xC2)));
        return;
    }
    set_lo(0x01, static_cast<std::uint8_t>((current & ~0xC0) | (value & 0xC0)));
}

void SegaCd::sub_write_memory_mode_inner(std::uint8_t value) noexcept {
    const std::uint8_t current = lo(0x01);
    if (((value ^ current) & 0x05) != 0) {
        if ((value & 0x04) != 0) {
            // Into (or within) 1M mode. RET chooses who gets which bank; RET
            // set also completes a swap the main CPU asked for.
            if ((value & 0x01) != 0) {
                dmna_pending_ = false;
            }
            set_lo(0x01, static_cast<std::uint8_t>((current & ~0x1F) | (value & 0x1D)));
            return;
        }
        std::uint8_t data = value;
        if ((current & 0x04) != 0) {
            // Back to 2M. RET comes back set unless the main CPU had asked
            // for Word RAM to go to the sub side.
            data = static_cast<std::uint8_t>(data | (dmna_pending_ ? 0 : 1));
            if ((data & 0x01) == 0) {
                set_lo(0x01, static_cast<std::uint8_t>((current & ~0x1F) | (data & 0x1F) | 0x02));
                return;
            }
        }
        if ((data & 0x01) != 0) {
            // 2M, RET set: Word RAM goes back to the main CPU.
            dmna_pending_ = false;
            set_lo(0x01, static_cast<std::uint8_t>((current & ~0x1F) | (data & 0x1D)));
            return;
        }
    }
    // Only the priority mode and MODE bits move.
    set_lo(0x01, static_cast<std::uint8_t>((current & ~0x1C) | (value & 0x1C)));
}

// --- the sub CPU's memory -----------------------------------------------------

// WORD RAM THE SUB CPU DOES NOT OWN STOPS IT DEAD.
//
// In 2M mode the whole of Word RAM belongs to one side at a time. When it is
// the main CPU's, the sub's address decoder does not assert /DTACK for
// $080000-$0BFFFF, so the sub CPU simply waits - mid-instruction - until the
// main hands the memory over. It is not an open-bus read.
//
// GenX used to let the read through and carry on with whatever bytes were
// there. Nothing looked wrong on screen, but the sub ran ahead of where the
// hardware would have let it: measured on The Lawnmower Man, GenX's sub
// executed 1.7x the instructions per frame that Genesis Plus GX's did, its
// BIOS sector ring filled, the BIOS stopped taking sectors and the game sat
// on its title screen while the attract should have been running.
//
// Modelled the way the hardware behaves rather than by predicting the
// access: the instruction runs, and if it touched locked Word RAM the sub's
// registers (PC included) are put back and the processor is held until
// ownership changes, when it runs the instruction again.
bool SegaCd::sub_word_ram_locked() const noexcept {
    return !one_meg_mode() && main_owns_word_ram();
}

void SegaCd::note_sub_word_ram_wait() noexcept { sub_word_ram_wait_ = true; }

std::uint16_t SegaCd::sub_read16(std::uint32_t address) noexcept {
    const std::uint32_t a = address & 0xFFFFFE;
    const std::uint32_t region = (a >> 16) & 0x0F;  // mirrored every megabyte
    if (region < 0x08) {
        const std::uint32_t o = a & 0x7FFFF;
        return static_cast<std::uint16_t>((prg_ram_[o] << 8) | prg_ram_[o + 1]);
    }
    if (region < 0x0C) {
        if (!one_meg_mode()) {
            if (sub_word_ram_locked()) {
                note_sub_word_ram_wait();
                return 0xFFFF;
            }
            const std::uint32_t o = a & 0x3FFFF;
            return static_cast<std::uint16_t>((word_ram_[o] << 8) | word_ram_[o + 1]);
        }
        // 1M dot image: one byte of the bank holds two pixels, and a word
        // read returns them as two bytes.
        const auto pair = word_ram_[bank_offset(sub_bank(), (a >> 1) & 0x1FFFF)];
        return static_cast<std::uint16_t>((pair & 0x0F) | ((pair << 4) & 0x0F00));
    }
    if (region < 0x0E) {
        if (!one_meg_mode()) {
            return 0xFFFF;
        }
        const std::uint32_t o = a & 0x1FFFF;
        return static_cast<std::uint16_t>((word_ram_[bank_offset(sub_bank(), o)] << 8) |
                                          word_ram_[bank_offset(sub_bank(), o + 1)]);
    }
    if (region == 0x0E) {
        return static_cast<std::uint16_t>(0xFF00 | backup_ram_[(a >> 1) & 0x1FFF]);
    }
    if ((a & 0x8000) == 0) {
        return pcm_read((a >> 1) & 0x1FFF);
    }
    return sub_register_read16(a);
}

std::uint8_t SegaCd::sub_read8(std::uint32_t address) noexcept {
    const std::uint32_t a = address & 0xFFFFFF;
    const std::uint32_t region = (a >> 16) & 0x0F;
    if (region < 0x08) {
        return prg_ram_[a & 0x7FFFF];
    }
    if (region < 0x0C) {
        if (!one_meg_mode()) {
            if (sub_word_ram_locked()) {
                note_sub_word_ram_wait();
                return 0xFF;
            }
            return word_ram_[a & 0x3FFFF];
        }
        const auto pair = word_ram_[bank_offset(sub_bank(), (a >> 1) & 0x1FFFF)];
        return (a & 1) != 0 ? static_cast<std::uint8_t>(pair & 0x0F) : static_cast<std::uint8_t>(pair >> 4);
    }
    if (region < 0x0E) {
        return one_meg_mode() ? word_ram_[bank_offset(sub_bank(), a & 0x1FFFF)] : 0xFF;
    }
    if (region == 0x0E) {
        return (a & 1) != 0 ? backup_ram_[(a >> 1) & 0x1FFF] : 0xFF;
    }
    if ((a & 0x8000) == 0) {
        return (a & 1) != 0 ? pcm_read((a >> 1) & 0x1FFF) : 0xFF;
    }
    return sub_register_read8(a);
}

void SegaCd::sub_write16(std::uint32_t address, std::uint16_t value) noexcept {
    const std::uint32_t a = address & 0xFFFFFE;
    const std::uint32_t region = (a >> 16) & 0x0F;
    if (region < 0x08) {
        sub_write8(a, static_cast<std::uint8_t>(value >> 8));
        sub_write8(a | 1u, static_cast<std::uint8_t>(value));
        return;
    }
    if (region < 0x0C && !one_meg_mode() && sub_word_ram_locked()) {
        note_sub_word_ram_wait();
        return;
    }
    if (region < 0x0C && one_meg_mode()) {
        // A word write to the dot image carries two pixels in its low nibbles.
        const std::uint32_t o = bank_offset(sub_bank(), (a >> 1) & 0x1FFFF);
        const auto incoming = static_cast<std::uint8_t>((value & 0x0F) | ((value >> 4) & 0xF0));
        word_ram_[o] = apply_priority((lo(0x01) >> 3) & 3, word_ram_[o], incoming);
        return;
    }
    if (region == 0x0E) {
        backup_ram_[(a >> 1) & 0x1FFF] = static_cast<std::uint8_t>(value);
        backup_ram_dirty_ = true;
        return;
    }
    if (region == 0x0F) {
        if ((a & 0x8000) == 0) {
            pcm_write((a >> 1) & 0x1FFF, static_cast<std::uint8_t>(value));
        } else {
            sub_register_write16(a, value);
        }
        return;
    }
    sub_write8(a, static_cast<std::uint8_t>(value >> 8));
    sub_write8(a | 1u, static_cast<std::uint8_t>(value));
}

void SegaCd::sub_write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t a = address & 0xFFFFFF;
    const std::uint32_t region = (a >> 16) & 0x0F;
    if (region < 0x08) {
        // The bottom of program RAM can be write-protected from the sub side,
        // in 512-byte steps. The BIOS keeps its own code there.
        const std::uint32_t o = a & 0x7FFFF;
        if (o >= (static_cast<std::uint32_t>(hi(0x01)) << 9)) {
            prg_ram_[o] = value;
        }
        return;
    }
    if (region < 0x0C) {
        if (!one_meg_mode()) {
            if (sub_word_ram_locked()) {
                note_sub_word_ram_wait();
                return;
            }
            word_ram_[a & 0x3FFFF] = value;
            return;
        }
        const std::uint32_t o = bank_offset(sub_bank(), (a >> 1) & 0x1FFFF);
        const auto previous = word_ram_[o];
        const auto merged = (a & 1) != 0
                                ? static_cast<std::uint8_t>((previous & 0xF0) | (value & 0x0F))
                                : static_cast<std::uint8_t>((previous & 0x0F) | (value << 4));
        word_ram_[o] = apply_priority((lo(0x01) >> 3) & 3, previous, merged);
        return;
    }
    if (region < 0x0E) {
        if (one_meg_mode()) {
            word_ram_[bank_offset(sub_bank(), a & 0x1FFFF)] = value;
        }
        return;
    }
    if (region == 0x0E) {
        if ((a & 1) != 0) {
            backup_ram_[(a >> 1) & 0x1FFF] = value;
            backup_ram_dirty_ = true;
        }
        return;
    }
    if ((a & 0x8000) == 0) {
        if ((a & 1) != 0) {
            pcm_write((a >> 1) & 0x1FFF, value);
        }
        return;
    }
    sub_register_write8(a, value);
}

// --- the gate array, sub side --------------------------------------------------

namespace {
// The font generator: a 16-bit bit pattern at $4E and two colours at $4C turn
// into four 4-bit pixels per word at $50-$57, so text can be drawn into a
// bitmap without shifting nibbles in software.
std::uint16_t font_word(std::uint16_t bits_register, std::uint8_t colours, std::uint32_t address) {
    std::uint8_t bits = static_cast<std::uint8_t>(
        (bits_register >> (((address & 6) ^ 6) << 1)) << 2);
    std::uint16_t data = static_cast<std::uint16_t>((colours >> (bits & 4)) & 0x0F);
    bits = static_cast<std::uint8_t>(bits >> 1);
    data = static_cast<std::uint16_t>(data | (((colours >> (bits & 4)) << 4) & 0xF0));
    bits = static_cast<std::uint8_t>(bits >> 1);
    data = static_cast<std::uint16_t>(data | (((colours >> (bits & 4)) << 8) & 0xF00));
    bits = static_cast<std::uint8_t>(bits >> 1);
    data = static_cast<std::uint16_t>(data | (((colours >> (bits & 4)) << 12) & 0xF000));
    return data;
}
}  // namespace

std::uint16_t SegaCd::sub_register_read16(std::uint32_t address) noexcept {
    std::uint32_t index = address & 0x1FE;
    switch (index) {
        case 0x00: return static_cast<std::uint16_t>((led_ << 8) | 0x01);
        case 0x02: return static_cast<std::uint16_t>(regs_[0x01] & 0xFF1F);  // BK0-1 hidden
        case 0x06: return cdc_register_read();
        case 0x08: return cdc_host_read(false);
        case 0x0C: {
            const auto ticks = (now_ - stopwatch_base_) / kTimerTick;
            return static_cast<std::uint16_t>((regs_[0x06] + ticks) & 0xFFF);
        }
        default:
            break;
    }
    if (index >= 0x50 && index <= 0x56) {
        return font_word(regs_[0x4E >> 1], lo(0x4C >> 1), index);
    }
    if ((index & 0x100) != 0) {
        index &= 0x17F;  // the subcode buffer, 64 words, mirrored
    }
    return regs_[index >> 1];
}

std::uint8_t SegaCd::sub_register_read8(std::uint32_t address) noexcept {
    std::uint32_t index = address & 0x1FF;
    switch (index) {
        case 0x00: return led_;
        case 0x01: return 0x01;  // reset status: always done
        case 0x03: return static_cast<std::uint8_t>(lo(0x01) & 0x1F);
        case 0x07: return cdc_register_read();
        case 0x08: case 0x09: case 0x0C: case 0x0D: {
            const auto word = sub_register_read16(index & ~1u);
            return (index & 1) != 0 ? static_cast<std::uint8_t>(word) : static_cast<std::uint8_t>(word >> 8);
        }
        default:
            break;
    }
    if (index >= 0x50 && index <= 0x57) {
        const auto word = font_word(regs_[0x4E >> 1], lo(0x4C >> 1), index);
        return (index & 1) != 0 ? static_cast<std::uint8_t>(word) : static_cast<std::uint8_t>(word >> 8);
    }
    if ((index & 0x100) != 0) {
        index &= 0x17F;
    }
    return (index & 1) != 0 ? lo(index >> 1) : hi(index >> 1);
}

void SegaCd::sub_register_write16(std::uint32_t address, std::uint16_t value) noexcept {
    const std::uint32_t index = address & 0x1FE;
    switch (index) {
        case 0x00:
            led_ = static_cast<std::uint8_t>(value >> 8);
            if ((value & 0x01) == 0) {
                sub_register_write8(0x01, 0x00);
            }
            return;
        case 0x02:
            sub_write_memory_mode(static_cast<std::uint8_t>(value));
            return;
        case 0x04:
            // Writing the transfer mode re-arms the destination and zeroes
            // the DMA address - both verified on hardware (mcd-verificator).
            regs_[0x02] = static_cast<std::uint16_t>(value & 0x070F);
            if (cdc_.dma != 0) cdc_run_dma(now_);
            cdc_start_transfer();
            regs_[0x05] = 0;
            return;
        case 0x06:
            cdc_register_write(static_cast<std::uint8_t>(value));
            return;
        case 0x08:
            (void)cdc_host_read(false);
            return;
        case 0x0C: {
            // Any write clears the stopwatch.
            const auto ticks = (now_ - stopwatch_base_) / kTimerTick;
            stopwatch_base_ += ticks * kTimerTick;
            regs_[0x06] = 0;
            return;
        }
        case 0x0E:
            set_lo(0x07, static_cast<std::uint8_t>(value));  // sub flags only
            return;
        case 0x30:
            sub_register_write8(0x31, static_cast<std::uint8_t>(value));
            return;
        case 0x32:
            sub_register_write8(0x33, static_cast<std::uint8_t>(value));
            return;
        case 0x34:
            // CD audio fader: a 12-bit volume in the top of the word, where
            // 0x400 is full level.
            fader_ = static_cast<std::uint16_t>(value >> 4);
            regs_[0x1A] = value;
            return;
        case 0x36:
            // Only bit 2 is writable: HOCK, which lets the drive interrupt.
            set_lo(0x1B, static_cast<std::uint8_t>(value & 0x04));
            return;
        case 0x4A:
            // The last command word: the drive takes the command now.
            regs_[0x25] = 0;
            cdd_command();
            return;
        case 0x66:
            // The trace vector base: writing it starts a graphics operation.
            regs_[0x66 >> 1] = value;
            gfx_start(value);
            return;
        default:
            break;
    }
    if (index >= 0x10 && index < 0x20) {
        return;  // the main CPU's command words are read-only here
    }
    regs_[(index >> 1) & 0xFF] = value;
}

void SegaCd::sub_register_write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t index = address & 0x1FF;
    switch (index) {
        case 0x00:
            led_ = value;
            return;
        case 0x01:
            if ((value & 0x01) == 0) {
                // The sub CPU resetting its own peripherals. The command and
                // status words survive - see msu-md-sample.bin, per Genesis
                // Plus GX - but everything from the timer up is cleared.
                regs_[0x02] = 0;
                regs_[0x06] = 0;
                std::fill(regs_.begin() + (0x30 >> 1), regs_.end(), std::uint16_t{0});
                regs_[0x08 >> 1] = 0xFFFF;
                regs_[0x0A >> 1] = 0xFFFF;
                regs_[0x36 >> 1] = 0x0100;
                regs_[0x40 >> 1] = 0x000F;
                for (std::uint32_t at = 0x42; at <= 0x4A; at += 2) {
                    regs_[at >> 1] = 0xFFFF;
                }
                pending_ = 0;
                timer_left_ = 0;
                stopwatch_base_ = now_;
                update_sub_interrupt();
                cdd_reset();
                cdc_reset();
                gfx_reset();
                pcm_reset();
            }
            return;
        case 0x02:
        case 0x03:
            // The byte lane is ignored (mcd-verificator).
            sub_write_memory_mode(value);
            return;
        case 0x04:
            set_hi(0x02, static_cast<std::uint8_t>(value & 0x07));
            if (cdc_.dma != 0) cdc_run_dma(now_);
            cdc_start_transfer();
            regs_[0x05] = 0;
            return;
        case 0x05:
            set_lo(0x02, static_cast<std::uint8_t>(value & 0x0F));
            return;
        case 0x07:
            cdc_register_write(value);
            return;
        case 0x0E:
        case 0x0F:
            set_lo(0x07, value);
            return;
        case 0x30:
        case 0x31:
            // The timer: one tick is 384 cycles. Zero stops it.
            set_lo(0x18, value);
            timer_left_ = static_cast<std::int64_t>(value) * kTimerTick;
            return;
        case 0x33:
            set_lo(0x19, value);
            // IEN2 is also visible to the main CPU, in bit 15 of $A12000.
            set_hi(0x00, static_cast<std::uint8_t>((hi(0x00) & 0x7F) | ((value & 0x04) << 5)));
            // Masking level 1 drops one that was pending (Batman Returns).
            pending_ &= static_cast<std::uint8_t>(0xFD | (value & 0x02));
            update_sub_interrupt();
            return;
        case 0x4C:
        case 0x4D:
            set_lo(0x26, value);
            return;
        default:
            break;
    }
    const std::uint32_t word = index & 0x1FE;
    if (word >= 0x10 && word < 0x20) {
        return;
    }
    // Word-only registers take a byte write as that byte in both halves.
    if ((word >= 0x08 && word <= 0x0D) || (word >= 0x34 && word <= 0x35) ||
        (word >= 0x5A && word <= 0x67)) {
        sub_register_write16(word, static_cast<std::uint16_t>((value << 8) | value));
        return;
    }
    if ((index & 1) != 0) set_lo((index >> 1) & 0xFF, value); else set_hi((index >> 1) & 0xFF, value);
}

// --- interrupts ---------------------------------------------------------------

void SegaCd::update_sub_interrupt() noexcept {
    // Bit n of the pending set is level n; the mask register uses the same
    // numbering. The highest level both pending and enabled is what the CPU
    // sees.
    const auto live = static_cast<std::uint32_t>((pending_ & lo(0x19)) >> 1);
    sub_.set_interrupt_level(live == 0 ? 0 : std::bit_width(live));
}

void SegaCd::raise(int level) noexcept {
    pending_ |= static_cast<std::uint8_t>(1u << level);
    update_sub_interrupt();
}

void SegaCd::acknowledge(int level) noexcept {
    pending_ &= static_cast<std::uint8_t>(~(1u << level));
    if (level == 2) {
        set_hi(0x00, static_cast<std::uint8_t>(hi(0x00) & ~0x01));  // IFL2
    }
    update_sub_interrupt();
}

// --- time ------------------------------------------------------------------------

void SegaCd::advance(std::uint32_t main_cycles, std::uint32_t main_clock_hz) noexcept {
    if (main_clock_hz == 0) {
        return;
    }
    budget_ += static_cast<std::int64_t>(main_cycles) * kSubClockHz;
    while (budget_ > 0) {
        std::uint32_t spent = 0;
        if (sub_running() && sub_word_ram_wait_) {
            // Held on /DTACK: the processor is mid-instruction and nothing it
            // does is visible until the main CPU hands Word RAM over. Time
            // still passes for the drive, the timers and the PCM chip.
            spent = 4;
            clock(spent);
            budget_ -= static_cast<std::int64_t>(spent) * main_clock_hz;
            continue;
        }
        if (sub_running()) {
            // Kept so an instruction that turns out to touch Word RAM the sub
            // does not own can be put back exactly as it was.
            const auto before = sub_.registers();
            const auto result = sub_.step();
            if (sub_word_ram_wait_) {
                sub_.mutable_registers() = before;
                spent = result.cycles != 0 ? result.cycles : 4;
                clock(spent);
                budget_ -= static_cast<std::int64_t>(spent) * main_clock_hz;
                continue;
            }
            spent = result.cycles != 0 ? result.cycles : 4;
            if (result.executed) ++sub_instructions_;
            // MERCURY_SCD_PROFILE - see sub_pc_counts.
            static const bool profiling = [] {
                const char* e = std::getenv("MERCURY_SCD_PROFILE");
                return e != nullptr && *e != '0';
            }();
            if (profiling) {
                static const std::uint64_t from = [] {
                    const char* e = std::getenv("MERCURY_SCD_PROFILE_FROM");
                    return e == nullptr ? 0ull : std::strtoull(e, nullptr, 10) * (kSubClockHz / 60);
                }();
                if (now_ >= from) ++sub_pc_counts_[result.pc_before];
            }
            // MERCURY_SCD_TRACE=<file> with MERCURY_SCD_TRACE_FRAMES=a-b:
            // every sub-CPU PC inside that frame window, in order - the same
            // shape the GPGX oracle host writes, for diffing the two flows
            // instruction by instruction.
            static std::FILE* window_trace = [] {
                const char* e = std::getenv("MERCURY_SCD_TRACE");
                return e != nullptr && *e != '\0' ? std::fopen(e, "w") : nullptr;
            }();
            if (window_trace != nullptr && result.executed) {
                static const auto window = [] {
                    const char* e = std::getenv("MERCURY_SCD_TRACE_FRAMES");
                    std::pair<int, int> range{0, 0};
                    if (e != nullptr) {
                        range.first = std::atoi(e);
                        const char* dash = std::strchr(e, '-');
                        range.second = dash != nullptr ? std::atoi(dash + 1) : range.first;
                    }
                    return range;
                }();
                const int frame = static_cast<int>(now_ / (kSubClockHz / 60));
                if (frame >= window.first && frame <= window.second) {
                    std::fprintf(window_trace, "%06X f%d\n", result.pc_before & 0xFFFFFF, frame);
                }
            }
            // MERCURY_SCD_FIRST_TRACE=<file>: every sub-CPU PC the first time
            // it runs, with the frame, in order. The same shape the GPGX
            // oracle host writes, so the two orders can be diffed to find
            // where the sub programs part company.
            static std::FILE* first_trace = [] {
                const char* e = std::getenv("MERCURY_SCD_FIRST_TRACE");
                return e != nullptr && *e != '\0' ? std::fopen(e, "w") : nullptr;
            }();
            if (first_trace != nullptr && result.executed &&
                sub_first_seen_.insert(result.pc_before & 0xFFFFFF).second) {
                std::fprintf(first_trace, "%06X f%d\n", result.pc_before & 0xFFFFFF,
                             static_cast<int>(now_ / (kSubClockHz / 60)));
            }
            if (result.unimplemented) ++sub_unimplemented_;
            if (result.interrupt_serviced > 0 && result.interrupt_serviced < 7) {
                ++sub_interrupts_[static_cast<std::size_t>(result.interrupt_serviced)];
                acknowledge(result.interrupt_serviced);
            }
        } else {
            // Held: nothing to interleave with, so the rest of the slice goes
            // by in one step. The drive and timers still run.
            spent = static_cast<std::uint32_t>((budget_ + main_clock_hz - 1) / main_clock_hz);
        }
        clock(spent);
        budget_ -= static_cast<std::int64_t>(spent) * main_clock_hz;
    }
    // The instruction is over: what the main CPU wrote lands now.
    if (!deferred_.empty()) {
        flush_main_writes();
    }
}

void SegaCd::clock(std::uint32_t cycles) noexcept {
    now_ += cycles;

    if (timer_left_ > 0) {
        timer_left_ -= cycles;
        if (timer_left_ <= 0) {
            const auto reload = static_cast<std::int64_t>(lo(0x18)) * kTimerTick;
            timer_left_ = reload > 0 ? timer_left_ + reload : 0;
            if ((lo(0x19) & 0x08) != 0) {
                raise(3);
            }
        }
    }

    // The drive runs at 75Hz: 12.5MHz / 75 is 166,666 and two thirds, so the
    // phase counts in thirds of a cycle to stay exact.
    cdd_phase_ += static_cast<std::uint64_t>(cycles) * 3;
    while (cdd_phase_ >= 500'000) {
        cdd_phase_ -= 500'000;
        cdd_tick();
        if ((lo(0x1B) & 0x04) != 0) {
            // Pending even while masked, as Genesis Plus GX has it: enabling
            // level 4 later takes the interrupt that was waiting.
            pending_ |= 1u << 4;
            update_sub_interrupt();
        }
    }

    if (cdc_.dma != 0) {
        cdc_run_dma(now_);
    }
    if (gfx_busy()) {
        gfx_update();
    }
}

void SegaCd::end_frame() noexcept {
    pcm_catch_up();
    // Resample the chip's output (12.5MHz / 384 = 32,552Hz) to the host
    // rate, linearly, carrying the phase across frames.
    constexpr double kStep = (static_cast<double>(kSubClockHz) / kTimerTick) / kAudioRate;
    const std::size_t frames = pcm_native_.size() / 2;
    std::vector<std::int32_t> pcm;
    if (frames > 0) {
        while (pcm_resample_phase_ < static_cast<double>(frames)) {
            const auto at = static_cast<std::size_t>(pcm_resample_phase_);
            const double fraction = pcm_resample_phase_ - static_cast<double>(at);
            for (int side = 0; side < 2; ++side) {
                const std::int32_t a = at == 0 ? pcm_last_[side] : pcm_native_[(at - 1) * 2 + side];
                const std::int32_t b = pcm_native_[at * 2 + side];
                pcm.push_back(a + static_cast<std::int32_t>((b - a) * fraction));
            }
            pcm_resample_phase_ += kStep;
        }
        pcm_resample_phase_ -= static_cast<double>(frames);
        pcm_last_[0] = pcm_native_[(frames - 1) * 2];
        pcm_last_[1] = pcm_native_[(frames - 1) * 2 + 1];
        pcm_native_.clear();
    }
    // THE TWO STREAMS ARRIVE IN DIFFERENT SIZED LUMPS AND THAT IS THE WHOLE
    // PROBLEM.
    //
    // Both are 44.1kHz in the long run. The PCM chip is resampled from its
    // own clock and lands on 734-736 frames per video frame, which is what
    // the host wants (44100/60 = 735). CD audio does not: it arrives one
    // 2352-byte block at a time, 588 frames each, 75 blocks a second - so a
    // video frame sees 588 or 1176 of it and never 735.
    //
    // This used to emit max(pcm, cdda) frames and mix pairwise from index
    // zero, which meant that on a 588 frame the last ~147 frames of music
    // were simply dropped and the next block started from the beginning of
    // the next video frame - about a fifth of the track thrown away, in a
    // regular pattern, which is heard as a buzz over everything. On a 1176
    // frame it did the opposite and made the block 441 frames too long.
    // Measured with MERCURY_SCD_AUDIO_LOG: "pcm=736 cdda=588 / pcm=734
    // cdda=1176" over and over.
    //
    // So the output length is the PCM count - the one that tracks real time -
    // and CD audio is a queue that is drawn from at that rate, keeping what
    // is left for the next frame. Supply and demand are equal on average, so
    // the queue neither starves nor grows.
    const std::size_t pcm_frames = pcm.size() / 2;
    const std::size_t cd_frames = cdda_.size() / 2;
    // MERCURY_SCD_AUDIO_LOG=N: how many sample frames each source had this
    // video frame. The two arrive in completely different sized lumps and
    // that is what this is for - see the note below.
    {
        static const long limit = [] {
            const char* set = std::getenv("MERCURY_SCD_AUDIO_LOG");
            return set != nullptr ? std::strtol(set, nullptr, 10) : 0;
        }();
        static long said = 0;
        static long seen = 0;
        // Every 60th, so a long run shows the TREND rather than the first
        // second. A queue that grows without bound is a rate mismatch
        // between the drive and the video frame, and that is the thing this
        // has to be able to tell apart from ordinary lumpiness.
        if (said < limit && (seen++ % 60) == 0) {
            ++said;
            std::fprintf(stderr, "[scd audio] frame %ld pcm=%zu cdda=%zu trims=%llu\n", seen - 1,
                         pcm_frames, cd_frames,
                         static_cast<unsigned long long>(cdda_overruns_));
        }
    }
    const std::size_t count = pcm_frames;
    // Only draw from the queue when a whole frame's worth is in it. Taking a
    // short read instead would put a hole in the music every time the block
    // boundary fell inside a video frame, which is the fault this replaced;
    // waiting costs at most one frame of delay when a track starts.
    const std::size_t cd_used = cd_frames >= count ? count : 0;
    for (std::size_t i = 0; i < count; ++i) {
        for (int side = 0; side < 2; ++side) {
            std::int32_t sum = pcm[i * 2 + side];
            if (i < cd_used) sum += cdda_[i * 2 + side];
            audio_.push_back(static_cast<std::int16_t>(std::clamp(sum, -32768, 32767)));
        }
    }
    cdda_.erase(cdda_.begin(), cdda_.begin() + static_cast<std::ptrdiff_t>(cd_used) * 2);
    // A bound, so a drive that outruns the host cannot grow this without
    // limit. FOUR blocks, not two: the drive reads ahead in bursts rather
    // than handing over a steady 1.25 blocks a frame, and measured over a
    // minute of Sonic CD the queue swings between about one and three blocks
    // on its own. A two-block cap clipped that twice a minute, and each clip
    // is a skip in the music; four never fires in normal play and costs 9KB
    // and about 30ms of delay on the disc's audio, which is under two frames.
    constexpr std::size_t kMaxCarryFrames = 588 * 4;
    if (cdda_.size() / 2 > kMaxCarryFrames) {
        cdda_.erase(cdda_.begin(),
                    cdda_.end() - static_cast<std::ptrdiff_t>(kMaxCarryFrames) * 2);
        ++cdda_overruns_;
    }
}

// --- reporting ---------------------------------------------------------------------

std::string SegaCd::describe() const {
    std::ostringstream out;
    out << "Sega CD: " << bios_description() << "; sub CPU "
        << (sub_running() ? "running" : ((lo(0x00) & 0x01) == 0 ? "held in reset" : "bus requested"))
        << ", " << sub_instructions_ << " instructions, pc=" << std::hex
        << sub_.registers().pc << std::dec << "; Word RAM "
        << (one_meg_mode() ? "1M" : "2M") << (one_meg_mode() ? "" : (main_owns_word_ram() ? " (main)" : " (sub)"))
        << "; drive status " << static_cast<int>(cdd_status_) << " at block " << cdd_lba_
        << ", " << cdd_commands_ << " commands, " << sectors_decoded_ << " sectors decoded";
    out << "; interrupts";
    for (int level = 1; level <= 6; ++level) {
        out << " L" << level << "=" << sub_interrupts_[static_cast<std::size_t>(level)];
    }
    // The decoder's own registers, in the same order the GPGX oracle host
    // prints them, so a "the game stopped taking sectors" can be compared
    // side by side.
    {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "; cdc ifstat=%02X ifctrl=%02X ctrl=%02X%02X stat=%02X%02X%02X%02X "
                      "dbc=%04X dac=%04X pt=%04X wa=%04X irq=%02X",
                      cdc_.ifstat, cdc_.ifctrl, cdc_.ctrl[0], cdc_.ctrl[1], cdc_.stat[0],
                      cdc_.stat[1], cdc_.stat[2], cdc_.stat[3], cdc_.dbc, cdc_.dac, cdc_.pt,
                      cdc_.wa, cdc_.irq);
        out << line;
    }
    return out.str();
}

}  // namespace mercury::segacd
