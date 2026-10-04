#include "mercury/genesis/vdp.hpp"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace mercury::genesis {

namespace {

// Register indices, named because the numbers alone are unreadable and the
// meanings are not guessable.
constexpr std::size_t kRegModeSet1 = 0x00;
constexpr std::size_t kRegModeSet2 = 0x01;
constexpr std::size_t kRegPlaneANameBase = 0x02;
constexpr std::size_t kRegWindowNameBase = 0x03;
constexpr std::size_t kRegPlaneBNameBase = 0x04;
constexpr std::size_t kRegSpriteTableBase = 0x05;
constexpr std::size_t kRegBackdropColor = 0x07;
constexpr std::size_t kRegLineCounter = 0x0A;
constexpr std::size_t kRegModeSet3 = 0x0B;
constexpr std::size_t kRegModeSet4 = 0x0C;
constexpr std::size_t kRegHScrollBase = 0x0D;
constexpr std::size_t kRegAutoIncrement = 0x0F;
constexpr std::size_t kRegPlaneSize = 0x10;
constexpr std::size_t kRegWindowH = 0x11;
constexpr std::size_t kRegWindowV = 0x12;
constexpr std::size_t kRegDmaLengthLow = 0x13;
constexpr std::size_t kRegDmaLengthHigh = 0x14;
constexpr std::size_t kRegDmaSourceLow = 0x15;
constexpr std::size_t kRegDmaSourceMid = 0x16;
constexpr std::size_t kRegDmaSourceHigh = 0x17;

// Nine-bit colour: three bits per channel, in the low bit of each nibble pair.
// The part outputs seven distinct levels per channel, not eight, so the ramp is
// value * 255 / 7 rather than a shift.
// How long after the vertical interrupt flag is raised the request reaches the
// CPU. Roughly one instruction plus the 68000's exception acknowledge.
constexpr std::int32_t kVintAssertDelayCycles = 64;

// MERCURY_CRAM_DMA_LOG=<file> (optional MERCURY_CRAM_DMA_LOG_FRAMES=a-b): every
// DMA into colour RAM, every display on/off, and every drawn line with the
// palette DMA that was last in when it was drawn - the same three events, in
// the same format, as the reference oracle's GPGX_VDP_LOG, so the two can be
// laid side by side. A game that loads a palette per line (Mortal Kombat CD's
// video does, 31 colours a line) turns to horizontal colour noise if those
// land a line off, and only the line each one lands on can say whether they do.
// Where in a line the raster takes its picture of the VDP's state, for a game
// that runs with Vdp::set_render_at_hblank (Mortal Kombat CD - see there).
// Active display is the first 2560 of the line's 3420 master clocks here (the
// H counter reads 0 at the start of the line), so this is where the horizontal
// blank begins - the same point the status register's HBLANK flag rises.
constexpr std::uint32_t kHblankStartCycle = (Vdp::kM68kCyclesPerLine * 2560) / 3420;

// MERCURY_RENDER_AT_HBLANK=1 forces that timing on for every game and =0 off
// for every game, overriding the per-game switch. For A/B testing only.
int render_at_hblank_override() noexcept {
    static const int value = [] {
        const char* e = std::getenv("MERCURY_RENDER_AT_HBLANK");
        if (e == nullptr || *e == '\0') return -1;
        return e[0] != '0' ? 1 : 0;
    }();
    return value;
}

std::uint32_t g_cram_dma_src = 0;
std::uint32_t g_cram_dma_words = 0;
std::FILE* cram_dma_log(std::uint32_t frame) noexcept {
    static long from = -1;
    static long to = -1;
    static std::FILE* file = [] {
        const char* w = std::getenv("MERCURY_CRAM_DMA_LOG_FRAMES");
        if (w != nullptr && *w != '\0') {
            char* end = nullptr;
            from = std::strtol(w, &end, 10);
            to = (end != nullptr && *end == '-') ? std::strtol(end + 1, nullptr, 10) : from;
        }
        const char* e = std::getenv("MERCURY_CRAM_DMA_LOG");
        return e != nullptr && *e != '\0' ? std::fopen(e, "w") : nullptr;
    }();
    if (file == nullptr || (from >= 0 && (static_cast<long>(frame) < from || static_cast<long>(frame) > to))) {
        return nullptr;
    }
    return file;
}

// Words the VDP can move per scanline. Turning the slot budget into a per-word
// cost is what lets a transfer consume the right slice of a frame.
std::uint32_t dma_cycles_for(std::uint32_t words, bool wide, bool blanked) noexcept {
    // MERCURY_DMA_FREE=1 makes DMA instantaneous, to separate "the CPU is
    // stalled too long" from "the game asked for this".
    static const bool free_dma = [] {
        const char* e = std::getenv("MERCURY_DMA_FREE");
        return e != nullptr && e[0] != '0';
    }();
    if (free_dma) { return 0u; }
    // MERCURY_DMA_RATE overrides words-per-line, to find what rate puts the
    // game's mid-frame DMA back inside vblank where it belongs.
    static const long rate_override = [] {
        const char* e = std::getenv("MERCURY_DMA_RATE");
        return e == nullptr ? 0L : std::strtol(e, nullptr, 10);
    }();
    // The slots a DMA can steal depend on whether the VDP is fetching pattern
    // data. With the picture off - display disabled, or vblank - almost the
    // whole line is free: 205 slots in 40-cell, 167 in 32-cell. During active
    // display the VDP keeps nearly all of them and only 18 (or 16) get through,
    // which is why games blank the screen before a big transfer.
    //
    // These are SLOTS, and a slot carries a WORD - not bytes. An H40 line is
    // 3420 mclk at 16 mclk per slot, which is 213 slots, so 205 is the line
    // almost entirely given over to transfer. Reading the figure as bytes and
    // halving it makes every blanked DMA take twice as long as the hardware,
    // which is invisible in most games and fatal in one that has to finish a
    // transfer inside an 8-line blanking window: Lemmings 2 blanks, DMAs and
    // un-blanks from a 6-stage raster chain, and at half rate the un-blank
    // interrupt arrives while the handler is still running and is lost.
    // MERCURY_DMA_ACTIVE_FAST=1 restores the old always-fast behaviour.
    static const bool active_fast = [] {
        const char* e = std::getenv("MERCURY_DMA_ACTIVE_FAST");
        return e != nullptr && e[0] != '0';
    }();
    // The blanked figure is the verified one: correcting 102 to 205 fixed both
    // the raster flicker and the audio slowdown in Lemmings 2.
    //
    // The active-display figure is NOT applied. The hardware number is 18 slots
    // in 40-cell mode, but that is 5.7x SLOWER than the flat rate this
    // emulator used before, and imposing it broke NBA Live 98 - it faults with
    // an address error on entering the rules screen, reading a pointer of
    // $2010201 that is plainly garbage. The game boots identically either way,
    // so a boot sweep does not catch it.
    //
    // Being accurate about blanking gained a game; being accurate about active
    // display cost one, with nothing measured in its favour. So the old flat
    // rate stands here until the fault it exposes is understood - the game is
    // already noted as timing-fragile in game_quirks.cpp.
    // MERCURY_DMA_ACTIVE_SLOW=1 applies the hardware figure, to work on it.
    static const bool active_slow = [] {
        const char* e = std::getenv("MERCURY_DMA_ACTIVE_SLOW");
        return e != nullptr && e[0] != '0';
    }();
    std::uint32_t words_per_line;
    if (rate_override > 0) {
        words_per_line = static_cast<std::uint32_t>(rate_override);
    } else if (blanked || active_fast) {
        words_per_line = wide ? 205u : 167u;
    } else if (active_slow) {
        words_per_line = wide ? 18u : 16u;
    } else {
        words_per_line = wide ? 102u : 83u;
    }
    return (words * 488u) / words_per_line;
}

constexpr std::uint8_t kColorLevels[8] = {0, 36, 73, 109, 146, 182, 219, 255};

std::uint16_t plane_size_cells(std::uint8_t encoded) noexcept {
    switch (encoded & 0x03) {
        case 0x00: return 32;
        case 0x01: return 64;
        case 0x02: return 32;  // prohibited combination; hardware behaves as 32
        default: return 128;
    }
}

}  // namespace

void Vdp::reset() noexcept {
    std::fill(vram_.begin(), vram_.end(), std::uint8_t{0});
    cram_.fill(0);
    vsram_.fill(0);
    registers_.fill(0);
    // Power-on defaults that matter before software writes anything: display
    // off, no interrupts, increment of two.
    registers_[kRegAutoIncrement] = 0x02;
    first_word_written_ = false;
    pending_first_word_ = 0;
    address_ = 0;
    command_code_ = 0;
    read_buffer_ = 0;
    scanline_ = 0;
    cycles_into_line_ = 0;
    line_rendered_ = false;
    vscroll_latch_.fill(0);
    frames_elapsed_ = 0;
    line_counter_ = 0;
    vertical_interrupt_pending_ = false;
    horizontal_interrupt_pending_ = false;
    vint_enabled_effective_ = false;
    hint_enabled_effective_ = false;
    vint_enabled_pending_ = false;
    hint_enabled_pending_ = false;
    reg1_log_.clear();
    int_events_.clear();
    vint_flag_ = false;
    vint_assert_delay_ = 0;
    vblank_flag_ = false;
    sprite_overflow_ = false;
    sprite_collision_ = false;
    dma_busy_ = false;
    dma_fill_pending_ = false;
    odd_frame_ = false;
    hv_latch_ = 0;
    hv_latched_ = false;
    hv_latch_soft_ = false;
    external_interrupt_pending_ = false;
    std::fill(frame_pixels_.begin(), frame_pixels_.end(), std::uint8_t{0});
    dma_count_ = 0;
    dma_log_.clear();
    pending_dma_cycles_ = 0;
    register_write_count_ = 0;
    data_write_count_ = 0;
    hint_raise_count_ = 0;
    vram_write_count_ = 0;
    vram_write_buckets_.fill(0);
    cram_write_count_ = 0;
    vsram_write_count_ = 0;
    invalid_write_count_ = 0;
}

void Vdp::set_pal(bool pal) noexcept {
    pal_ = pal;
}

// MERCURY_VRAM_WATCH=<hex>: keep the last writes into the 128-byte window at
// that address, from either the data port or a DMA, with the frame they landed
// on. A nametable row that ends up holding the wrong tiles was either never
// written, written and then overwritten, or written with the wrong data - and
// only the order of the writes tells those three apart.
void Vdp::note_vram_write(std::uint32_t address, std::uint16_t value, bool via_dma) noexcept {  // NOLINT
    // MERCURY_VRAM_WATCH=<hex address>[,<hex size>] - the default window is one
    // tile. The SCANLINE is recorded as well as the frame: art that is complete
    // when the frame ends but was written after the raster had already passed
    // its rows renders half-drawn, and the frame number alone cannot show that.
    static const long watch = [] {
        const char* e = std::getenv("MERCURY_VRAM_WATCH");
        return e == nullptr ? -1L : std::strtol(e, nullptr, 16);
    }();
    static const std::uint32_t span = [] {
        const char* e = std::getenv("MERCURY_VRAM_WATCH");
        const char* comma = e == nullptr ? nullptr : std::strchr(e, ',');
        return comma == nullptr ? 0x20u : static_cast<std::uint32_t>(std::strtoul(comma + 1, nullptr, 16));
    }();
    if (watch < 0) { return; }
    // The VDP address is wider than the 64KB it indexes, and every write site
    // masks it. Filtering on the unmasked value silently drops the writes that
    // matter.
    address &= 0xFFFF;
    const auto base = static_cast<std::uint32_t>(watch);
    if (address < base || address >= base + span) { return; }
    ++vram_watch_count_;
    vram_watch_log_[vram_watch_pos_ % vram_watch_log_.size()] = {
        frames_elapsed_, scanline_, address, value, via_dma ? 1u : 0u};
    ++vram_watch_pos_;
}

// MERCURY_VSRAM_LOG=1: every vertical-scroll write with the frame and scanline
// it landed on. A plane drawn eight pixels off is either given the wrong value
// or given the right value too late, and only the line tells those apart.
void Vdp::note_raster_event(std::uint32_t kind, std::uint32_t value) noexcept {
    // =1 logs everything; =2 logs register 10 writes only. A game that takes an
    // interrupt on every line buries its own register writes in a 200-entry
    // flood, and the register writes are what decide the interval.
    static const int level = [] {
        const char* e = std::getenv("MERCURY_RASTER_LOG");
        return e == nullptr ? 0 : std::atoi(e);
    }();
    if (level == 0) { return; }
    if (kind == 5 && level < 3) { return; }        // every register write: opt in
    if (level >= 2 && (kind == 1 || kind == 2)) { return; }  // the H-int flood
    if (level >= 3 && kind != 5 && kind != 3) { return; }    // avoid duplicates
    ++raster_log_count_;
    raster_log_[raster_log_pos_ % raster_log_.size()] = {frames_elapsed_, scanline_, kind, value};
    ++raster_log_pos_;
}

void Vdp::note_vsram_write(std::uint32_t index, std::uint16_t value, bool via_dma) noexcept {
    static const bool on = [] {
        const char* e = std::getenv("MERCURY_VSRAM_LOG");
        return e != nullptr && e[0] != '0';
    }();
    if (!on || index > 1) { return; }
    ++vsram_log_count_;
    vsram_log_[vsram_log_pos_ % vsram_log_.size()] = {frames_elapsed_, scanline_, index, value,
                                                      via_dma ? 1u : 0u};
    ++vsram_log_pos_;
}

// --- geometry --------------------------------------------------------------

bool Vdp::wide_mode() const noexcept {
    // RS1, bit 0, is what selects 40-cell mode. RS0 in bit 7 picks the pixel
    // clock source and most software sets the two together, which makes
    // requiring both look safe - but a game that sets only RS1 then gets
    // rendered 256 wide with a 32-cell window stride, and a wrong stride turns
    // the whole background into scattered tile fragments.
    return (registers_[kRegModeSet4] & 0x01) != 0;
}

std::uint32_t Vdp::screen_width() const noexcept {
    return wide_mode() ? 320u : 256u;
}

std::uint32_t Vdp::active_scanlines() const noexcept {
    // The 30-cell (240 line) mode only exists on PAL machines; an NTSC console
    // with the bit set shows a corrupt display rather than 240 lines, so the
    // sane thing here is to keep 224.
    return (pal_ && (registers_[kRegModeSet2] & 0x08) != 0) ? 240u : 224u;
}

bool Vdp::interlace_double() const noexcept {
    return (registers_[kRegModeSet4] & 0x06) == 0x06;
}

std::uint32_t Vdp::screen_height() const noexcept {
    // What the framebuffer holds, which in interlace mode 2 is both fields
    // woven together - twice the scanline count. Anything to do with TIMING
    // wants active_scanlines() instead: the raster still draws 224 lines.
    return active_scanlines() * (interlace_double() ? 2u : 1u);
}

std::uint32_t Vdp::lines_per_frame() const noexcept {
    return pal_ ? kLinesPerFramePal : kLinesPerFrameNtsc;
}

bool Vdp::display_enabled() const noexcept {
    return (registers_[kRegModeSet2] & 0x40) != 0;
}

std::uint16_t Vdp::plane_width_cells() const noexcept {
    return plane_size_cells(registers_[kRegPlaneSize] & 0x03);
}

std::uint16_t Vdp::plane_height_cells() const noexcept {
    return plane_size_cells(static_cast<std::uint8_t>((registers_[kRegPlaneSize] >> 4) & 0x03));
}

// --- control port ----------------------------------------------------------

Vdp::Target Vdp::target_of(std::uint8_t code) const noexcept {
    // Only the low four bits pick the space; CD4/CD5 are the DMA controls.
    switch (code & 0x0F) {
        case 0x00: case 0x01: return Target::vram;
        case 0x03: case 0x08: return Target::cram;
        case 0x04: case 0x05: return Target::vsram;
        default: return Target::invalid;
    }
}

void Vdp::write_control(std::uint16_t value) noexcept {

    if (!first_word_written_ && (value & 0xE000) == 0x8000) {
        // Register write. Note this is only recognised when no command is half
        // way through: the second word of a command can legitimately look like
        // a register write.

        write_register(static_cast<std::uint8_t>((value >> 8) & 0x1F),
                       static_cast<std::uint8_t>(value & 0xFF));
        return;
    }

    if (!first_word_written_) {
        pending_first_word_ = value;
        first_word_written_ = true;
        // The low bits take effect immediately - software that only writes one
        // word and then hits the data port relies on it.
        address_ = (address_ & 0xC000) | (value & 0x3FFF);
        command_code_ = static_cast<std::uint8_t>((command_code_ & 0x3C) |
                                                  ((value >> 14) & 0x03));
        return;
    }

    first_word_written_ = false;
    address_ = static_cast<std::uint32_t>(((value & 0x0003) << 14) |
                                          (pending_first_word_ & 0x3FFF));
    command_code_ = static_cast<std::uint8_t>(((value >> 2) & 0x3C) |
                                              ((pending_first_word_ >> 14) & 0x03));

    // A DMA command with the enable bit clear is silently not a DMA. Count
    // those: if the game is asking for transfers this emulator quietly drops,
    // the tiles they carry never arrive and whatever they draw looks stale.
    if ((command_code_ & 0x20) != 0 && (registers_[kRegModeSet2] & 0x10) == 0) {
        ++dma_skipped_disabled_;
    }
    if ((command_code_ & 0x20) != 0 && (registers_[kRegModeSet2] & 0x10) != 0) {
        const auto mode = static_cast<std::uint8_t>(registers_[kRegDmaSourceHigh] & 0xC0);
        if (mode == 0x80) {
            // Fill waits for the data-port write that supplies the byte.
            dma_fill_pending_ = true;
        } else {
            run_dma();
        }
        return;
    }

    // Only commands that end up pointing into the sprite-tile region, so the
    // window covers the needle plotting rather than unrelated DMA setup.
    if (((address_ >> 12) & 0x0F) == 0x0D) {
        ctrl_log_[ctrl_log_pos_ % ctrl_log_.size()] = {pending_first_word_,
                                                        static_cast<std::uint16_t>(address_)};
        ++ctrl_log_pos_;
    }
    if ((command_code_ & 0x01) == 0) {
        // Reads are prefetched at command time.
        switch (target_of(command_code_)) {
            case Target::vram: read_buffer_ = read_vram16(address_); break;
            case Target::cram: read_buffer_ = cram_[(address_ >> 1) % kCramEntries]; break;
            case Target::vsram: read_buffer_ = vsram_[(address_ >> 1) % kVsramEntries]; break;
            default: break;
        }
    }
}

void Vdp::write_register(std::uint8_t index, std::uint8_t value) noexcept {
    if (index >= kRegisterCount) {
        return;
    }
    // Reg 1 carries the V-interrupt enable, and the exact frame/line a game
    // toggles it on is what decides whether a standing request is legitimate or
    // stale. Inferring that from the outside does not work; record it.
    // Where the display actually gets switched off, as a histogram of scanlines.
    // Lemmings 2 only blanks legally when V < 0x50 or V >= 0xF0; anything
    // outside that window is the game overrunning its own safe period.
    if (index == kRegModeSet2) {
        const bool was_on = (registers_[kRegModeSet2] & 0x40) != 0;
        const bool now_on = (value & 0x40) != 0;
        if (was_on && !now_on && scanline_ < blank_start_lines_.size()) {
            ++blank_start_lines_[scanline_];
        }
        if (was_on != now_on) {
            if (std::FILE* log = cram_dma_log(frames_elapsed_)) {
                std::fprintf(log, "R1 %u %u %u disp=%u\n", frames_elapsed_, scanline_, cycles_into_line_,
                             now_on ? 1u : 0u);
            }
        }
    }
    if (index == kRegModeSet2 && reg1_log_.size() < 24) {
        reg1_log_.push_back({frames_elapsed_, scanline_, value, vertical_interrupt_pending_});
    }
    // Clearing the HV latch enable releases the counter. Without this the
    // counter would stay frozen at the last latched value for the rest of the
    // run, and a game that latches once and then polls the raster normally
    // would never see it move again.
    if (index == kRegModeSet1 && (value & 0x02) == 0) {
        hv_latched_ = false;
        hv_latch_soft_ = false;
    }
    registers_[index] = value;
    ++register_write_count_;
    // MERCURY_RASTER_LOG=3 records EVERY register write with its frame and
    // scanline. When one register comes out with the wrong value on alternate
    // frames the question is what else the routine that writes it did, and a
    // trace of one register cannot answer that.
    note_raster_event(5, (static_cast<std::uint32_t>(index) << 8) | value);
    if (index == kRegLineCounter) {
        ++reg10_values_[value];
        note_raster_event(0, value);
    }
    // Window position, and the line it was written on. If a game moves the
    // window with its raster chain, the value sitting in the register when the
    // frame ends says nothing about where the window actually was.
    if (index == kRegWindowV) {
        ++reg18_values_[value];
        if (scanline_ < reg18_lines_.size()) ++reg18_lines_[scanline_];
        note_raster_event(4, value);
    }
    if (index == kRegLineCounter) {
        // Reload value only. Writing it does not disturb a countdown already in
        // progress - the same rule the Master System's R10 follows, and the
        // same bug if you get it wrong.
        // MERCURY_HINT_RELOAD_NOW=1 makes the write restart the countdown
        // instead, to test a raster chain that reprograms the interval from
        // inside its own handler and expects the new spacing at once.
        static const bool reload_now = [] {
            const char* e = std::getenv("MERCURY_HINT_RELOAD_NOW");
            return e != nullptr && e[0] != '0';
        }();
        if (reload_now || hint_reload_on_write_) {
            line_counter_ = value;
        }
        return;
    }
    // The enable bits do NOT touch the pending flags. The VDP sets VINT pending
    // at every vblank and HINT pending on every counter reload regardless of
    // whether the interrupt is enabled; the enable bits are only a mask applied
    // when deciding whether to assert the 68000's interrupt lines. Nothing in
    // software can clear a pending flag - only the 68000 acknowledging it can.
    // Clearing them here instead is the tempting shortcut that "fixes" Fatal
    // Rewind while breaking games that rely on the real behaviour.
}

std::uint16_t Vdp::read_control() noexcept {
    // Reading the control port abandons any half-written command.
    first_word_written_ = false;

    std::uint16_t status = 0x3400;  // unused bits float to this pattern
    status |= 0x0200;               // FIFO empty
    if (pal_) status |= 0x0001;
    if (dma_busy_) status |= 0x0002;
    // HBLANK is asserted outside the active picture area of the line.
    // Active display is 2560 of the line's 3420 master clocks in both H32 and
    // H40 - only the pixel clock differs - so HBLANK is the remaining 860.
    if (cycles_into_line_ >= (kM68kCyclesPerLine * 2560) / 3420) status |= 0x0004;
    if (vblank_flag_ || !display_enabled()) status |= 0x0008;
    // ODD frame, bit 4. The hardware only ever sets this in an INTERLACED mode;
    // on a progressive frame it reads zero every time. Reporting the real toggle
    // regardless hands any game that polls the bit an answer that flips every
    // frame, so a game branching on it runs two different code paths on
    // alternate frames - a whole class of 30Hz flicker with no other symptom.
    // (Found while looking at Steel Empire, which turned out not to be a bug at
    // all; this is a separate accuracy fix and is not for that game.)
    if (odd_frame_ && (registers_[kRegModeSet4] & 0x06) != 0) status |= 0x0010;
    if (sprite_collision_) status |= 0x0020;
    if (sprite_overflow_) status |= 0x0040;
    if (vint_flag_) status |= 0x0080;

    // The sticky flags are cleared by the read that reports them - including
    // the vertical interrupt flag, which is why it is tracked separately from
    // the interrupt line itself. Games poll this bit as a vblank wait with the
    // interrupt handler ALSO running; if the flag only mirrored the pending
    // interrupt it would be cleared by the acknowledge before the polling loop
    // ever saw it, and the loop would never end.
    vint_flag_ = false;
    sprite_collision_ = false;
    sprite_overflow_ = false;
    return status;
}

std::uint16_t Vdp::read_hv_counter() noexcept {
    if (hv_latched_) {
        return hv_latch_;
    }
    // V counter counts the visible lines then jumps backwards across the
    // blanking gap, which is why software can see the same value twice.
    std::uint32_t v = scanline_;
    if (!pal_) {
        if (v > 0xEA) v -= 6;
    } else {
        if (v > 0x102) v -= 56;
    }
    // H counter runs 0..0xB6 then 0xE4..0xFF in H40, 0..0x93 then 0xE9..0xFF in
    // H32, so it is a piecewise map of the position within the line.
    const std::uint32_t total = wide_mode() ? 211u : 171u;
    std::uint32_t h = (cycles_into_line_ * total) / kM68kCyclesPerLine;
    const std::uint32_t visible = wide_mode() ? 0xB7u : 0x94u;
    if (h >= visible) {
        h += wide_mode() ? (0xE4u - 0xB7u) : (0xE9u - 0x94u);
    }
    // Which V values a polling game actually sees. A wait loop that never
    // observes its target value falls through and the game stops being locked
    // to the frame, which shows up as a raster effect drifting up the screen.
    ++hv_v_histogram_[v & 0xFF];
    return static_cast<std::uint16_t>(((v & 0xFF) << 8) | (h & 0xFF));
}

// --- data port -------------------------------------------------------------

void Vdp::advance_address() noexcept {
    address_ = (address_ + registers_[kRegAutoIncrement]) & 0xFFFF;
}

std::uint16_t Vdp::read_data() noexcept {
    // Touching the data port abandons a half-written command, exactly as
    // reading the control port does. See the note in write_data().
    first_word_written_ = false;
    const auto value = read_buffer_;
    advance_address();
    switch (target_of(command_code_)) {
        case Target::vram: read_buffer_ = read_vram16(address_); break;
        case Target::cram: read_buffer_ = cram_[(address_ >> 1) % kCramEntries]; break;
        case Target::vsram: read_buffer_ = vsram_[(address_ >> 1) % kVsramEntries]; break;
        default: break;
    }
    return value;
}

void Vdp::write_data(std::uint16_t value) noexcept {
    // The pending-command latch is cleared by three things: the second control
    // word, a control-port read, and ANY access to the data port. Missing the
    // last one leaves the latch stuck after a game writes one control word and
    // then streams data - and the next control word it sends, typically a
    // register write, gets eaten as that command's second half. Since a
    // register write carries its data in the low byte, any value with bit 7 set
    // lands on CD5 and starts a DMA that was never asked for, with whatever
    // source and length the registers happen to hold.
    first_word_written_ = false;
    ++data_write_count_;
    if (dma_fill_pending_) {
        dma_fill_pending_ = false;
        dma_fill(value);
        return;
    }
    switch (target_of(command_code_)) {
        case Target::vram: {
            // A word write to an odd VRAM address stores the halves swapped.
            // Software does this deliberately to poke single bytes.
            ++vram_write_count_;
            ++vram_write_buckets_[(address_ >> 12) & 0x0F];
            // The needle is plotted into the sprite tiles by direct data-port
            // writes, so keep a rolling window of the most recent ones. If the
            // addresses run continuously the CPU is drawing a solid stroke and
            // something downstream scatters it; if they skip, the gaps are
            // already in what the CPU asked for.
            if (((address_ >> 12) & 0x0F) == 0x0D) {
                d_port_log_[d_port_log_pos_ % d_port_log_.size()] =
                    {address_ | (static_cast<std::uint32_t>(registers_[kRegAutoIncrement]) << 16),
                     value};
                ++d_port_log_pos_;
            }
            note_vram_write(address_, value, /*via_dma=*/false);
            const auto base = address_ & 0xFFFE;
            if ((address_ & 1) != 0) {
                vram_[base] = static_cast<std::uint8_t>(value & 0xFF);
                vram_[base + 1] = static_cast<std::uint8_t>(value >> 8);
            } else {
                vram_[base] = static_cast<std::uint8_t>(value >> 8);
                vram_[base + 1] = static_cast<std::uint8_t>(value & 0xFF);
            }
            break;
        }
        case Target::cram:
            ++cram_write_count_;
            cram_[(address_ >> 1) % kCramEntries] = static_cast<std::uint16_t>(value & 0x0EEE);
            break;
        case Target::vsram:
            ++vsram_write_count_;
            if (std::FILE* log = cram_dma_log(frames_elapsed_)) {
                std::fprintf(log, "V %u %u %u idx=%u val=%03X\n", frames_elapsed_, scanline_, cycles_into_line_,
                             static_cast<unsigned>((address_ >> 1) % kVsramEntries), value & 0x03FFu);
            }
            note_vsram_write((address_ >> 1) % kVsramEntries, static_cast<std::uint16_t>(value & 0x03FF), false);
            vsram_[(address_ >> 1) % kVsramEntries] = static_cast<std::uint16_t>(value & 0x03FF);
            break;
        default:
            ++invalid_write_count_;
            break;
    }
    advance_address();
}

// --- DMA -------------------------------------------------------------------

void Vdp::run_dma() noexcept {
    const auto mode = static_cast<std::uint8_t>(registers_[kRegDmaSourceHigh] & 0xC0);
    if (mode == 0xC0) {
        dma_copy();
    } else {
        dma_from_memory();
    }
}

void Vdp::dma_from_memory() noexcept {
    if (!read_memory_) {
        return;
    }
    ++dma_count_;
    std::uint32_t length = static_cast<std::uint32_t>(registers_[kRegDmaLengthHigh] << 8) |
                           registers_[kRegDmaLengthLow];
    if (length == 0) {
        length = 0x10000;  // a zero length means the full 64K, not nothing
    }
    // Where DMA actually lands, by 4KB of VRAM. The transfer log only keeps its
    // first entries, so a region filled later in the run is invisible in it.
    dma_dest_buckets_[(address_ >> 12) & 0x0F] += length;

    // The source is a word address: the register pair holds bits 23-1.
    std::uint32_t source = (static_cast<std::uint32_t>(registers_[kRegDmaSourceHigh] & 0x7F) << 17) |
                           (static_cast<std::uint32_t>(registers_[kRegDmaSourceMid]) << 9) |
                           (static_cast<std::uint32_t>(registers_[kRegDmaSourceLow]) << 1);
    // For transfers into the sprite-tile region specifically, remember where
    // they came from: a source inside the SVP DRAM window behaves differently
    // from one in work RAM, and that decides which rules apply to it.
    if (((address_ >> 12) & 0x0F) == 0x0D) {
        dma_d_sources_[(source >> 16) & 0xFF] += length;
        ++d_dma_count_;
        d_dma_log_[d_dma_log_pos_ % d_dma_log_.size()] = {address_, source, length};
        ++d_dma_log_pos_;
        last_d_inc_ = registers_[kRegAutoIncrement];
        last_d_source_ = source;
        last_d_dest_ = address_;
        last_d_length_ = length;
    }
    const auto target = target_of(command_code_);
    // MERCURY_CRAM_DMA_LOG=<file>: every DMA into colour RAM, with the frame,
    // the scanline it starts on and how far into that line. A game that loads
    // a palette per line (Mortal Kombat CD's video does, 31 colours a line)
    // turns to horizontal colour noise if those land a line off, and only the
    // line each one lands on can say whether they do.
    if (target == Target::cram) {
        if (std::FILE* cram_log = cram_dma_log(frames_elapsed_)) {
            std::fprintf(cram_log, "D %u %u %u src=%06X len=%u cram=%u disp=%u\n", frames_elapsed_, scanline_,
                         cycles_into_line_, source, length, (address_ >> 1) & 0x3F,
                         display_enabled() ? 1u : 0u);
        }
        if (source != g_cram_dma_src + 2 * g_cram_dma_words) {
            g_cram_dma_src = source;
            g_cram_dma_words = 0;
        }
        g_cram_dma_words += length;
    }
    // A rolling window, not the first 64: the transfers that set a screen up are
    // useful, but so are the ones a game repeats every frame, and capping at the
    // first 64 hides those completely behind the boot sequence.
    dma_log_.push_back({address_, source, length, command_code_, 'm'});
    if (dma_log_.size() > 64) {
        dma_log_.erase(dma_log_.begin());
    }
    // The SVP's DRAM is slow enough that a DMA burst reading it gets the
    // PREVIOUS word back, not the one just addressed - the transfer lags its
    // source by one word. Virtua Racing knows this and programs its source two
    // bytes high to compensate, which is why its tile transfers read 0x300002
    // rather than 0x300000, and why its own self-test writes VRAM words 0 and 2
    // by hand afterwards and then expects word 4 to line up.
    //
    // Without the lag every tile lands one word - four pixels - off, which is
    // exactly the ghosting on its rendered text. The range is only ever set for
    // a cartridge that has an SVP, so no other game can be affected.
    static const bool lag_disabled = [] {
        const char* e = std::getenv("MERCURY_NO_DMA_LAG");
        return e != nullptr && e[0] != '0';
    }();
    const bool lagged_source = !lag_disabled && dma_lag_end_ > dma_lag_start_ &&
                               source >= dma_lag_start_ && source <= dma_lag_end_;
    std::uint16_t lag_latch = lagged_source ? read_memory_((source - 2) & 0xFFFFFF) : 0;
    for (std::uint32_t i = 0; i < length; ++i) {
        auto word = read_memory_(source & 0xFFFFFF);
        if (lagged_source) {
            const auto delivered = lag_latch;
            lag_latch = word;
            word = delivered;
        }
        switch (target) {
            case Target::vram: {
                note_vram_write(address_, word, /*via_dma=*/true);
                const auto base = address_ & 0xFFFE;
                if ((address_ & 1) != 0) {
                    vram_[base] = static_cast<std::uint8_t>(word & 0xFF);
                    vram_[base + 1] = static_cast<std::uint8_t>(word >> 8);
                } else {
                    vram_[base] = static_cast<std::uint8_t>(word >> 8);
                    vram_[base + 1] = static_cast<std::uint8_t>(word & 0xFF);
                }
                break;
            }
            case Target::cram:
                cram_[(address_ >> 1) % kCramEntries] = static_cast<std::uint16_t>(word & 0x0EEE);
                break;
            case Target::vsram:
                note_vsram_write((address_ >> 1) % kVsramEntries, static_cast<std::uint16_t>(word & 0x03FF), true);
                vsram_[(address_ >> 1) % kVsramEntries] = static_cast<std::uint16_t>(word & 0x03FF);
                break;
            default:
                break;
        }
        advance_address();
        // The source pointer wraps inside its 128KB bank rather than carrying
        // into the high bits - a real and frequently-hit quirk.
        source = (source & 0xFE0000) | ((source + 2) & 0x01FFFF);
    }
    registers_[kRegDmaLengthLow] = 0;
    registers_[kRegDmaLengthHigh] = 0;
    pending_dma_cycles_ += dma_cycles_for(length, wide_mode(), !display_enabled() || vblank_flag_);
}

void Vdp::dma_fill(std::uint16_t value) noexcept {
    ++dma_count_;
    std::uint32_t length = static_cast<std::uint32_t>(registers_[kRegDmaLengthHigh] << 8) |
                           registers_[kRegDmaLengthLow];
    if (length == 0) {
        length = 0x10000;
    }
    // The fill writes the HIGH byte of the data word to successive addresses,
    // after the initial word write lands normally.
    const auto fill_byte = static_cast<std::uint8_t>(value >> 8);
    const auto base = address_ & 0xFFFE;
    vram_[base] = static_cast<std::uint8_t>(value >> 8);
    vram_[base + 1] = static_cast<std::uint8_t>(value & 0xFF);
    note_vram_write(base, value, /*via_dma=*/true);
    for (std::uint32_t i = 0; i < length; ++i) {
        advance_address();
        note_vram_write(address_ & 0xFFFF, fill_byte, /*via_dma=*/true);
        vram_[address_ & 0xFFFF] = fill_byte;
    }
    registers_[kRegDmaLengthLow] = 0;
    registers_[kRegDmaLengthHigh] = 0;
    pending_dma_cycles_ += dma_cycles_for(length, wide_mode(), !display_enabled() || vblank_flag_);
}

void Vdp::dma_copy() noexcept {
    ++dma_count_;
    std::uint32_t length = static_cast<std::uint32_t>(registers_[kRegDmaLengthHigh] << 8) |
                           registers_[kRegDmaLengthLow];
    if (length == 0) {
        length = 0x10000;
    }
    std::uint32_t source = (static_cast<std::uint32_t>(registers_[kRegDmaSourceMid]) << 8) |
                           registers_[kRegDmaSourceLow];
    for (std::uint32_t i = 0; i < length; ++i) {
        note_vram_write(address_ & 0xFFFF, vram_[source & 0xFFFF], /*via_dma=*/true);
        vram_[address_ & 0xFFFF] = vram_[source & 0xFFFF];
        source = (source + 1) & 0xFFFF;
        advance_address();
    }
    registers_[kRegDmaLengthLow] = 0;
    registers_[kRegDmaLengthHigh] = 0;
}

// --- timing ----------------------------------------------------------------

int Vdp::interrupt_level() const noexcept {
    // Deliberately the DELAYED copies of the enable bits. See
    // commit_interrupt_enables().
    if (vertical_interrupt_pending_ && vint_enabled_effective_) {
        return 6;
    }
    if (horizontal_interrupt_pending_ && hint_enabled_effective_) {
        return 4;
    }
    // Level 2 is the external interrupt, raised by a transition on a controller
    // port's TH line. Only a light gun drives it, and only when the game has
    // asked for it, so this is dead code for every other cartridge.
    if (external_interrupt_pending_ && external_interrupt_enabled()) {
        return 2;
    }
    return 0;
}

bool Vdp::external_interrupt_enabled() const noexcept {
    return (registers_[kRegModeSet3] & 0x08) != 0;
}

bool Vdp::hv_latch_enabled() const noexcept {
    // Mode register 1 bit 1 (M3). While it is set the HV counter stops
    // following the raster and reports whatever was latched.
    return (registers_[kRegModeSet1] & 0x02) != 0;
}

// The gun has seen the beam. Two separate things happen: the HV counter freezes
// at the aim point, and the external interrupt is raised.
//
// The counter freezes EVEN IF the game never set M3. That looks wrong and is
// not - it is what Genesis Plus GX does, labelled there as a hack for games
// that "do not lock the HV Counter but instead apply a larger offset value",
// and it is the difference between a light gun game working and half working.
// Gating the freeze on M3 was the first attempt here, and T2: The Arcade Game
// showed exactly the symptom that predicts: its own crosshair tracked
// VERTICALLY and not horizontally. Y reaches the game through the interrupt -
// it fires on the gun's line, and the handler can read the live V counter - so
// Y worked. X has no path except the latched H counter, so with the freeze
// gated off, X never moved. A latch the game did not ask for has to be released
// again on the next line, which is what release_light_gun_latch does.
void Vdp::latch_light_gun(std::uint32_t h, std::uint32_t y) noexcept {
    ++light_gun_latches_;
    if (external_interrupt_enabled()) {
        ++light_gun_interrupts_;
    }
    if (hv_latch_enabled()) {
        ++light_gun_hv_latches_;
    }
    hv_latch_ = static_cast<std::uint16_t>(((y & 0xFF) << 8) | (h & 0xFF));
    hv_latched_ = true;
    hv_latch_soft_ = !hv_latch_enabled();
    external_interrupt_pending_ = true;
}

// The counter has to be free-running again on every other line, or a game that
// polls the raster normally sees it stopped. Only the gun's own latch is
// released: one the game took by setting M3 is the game's to clear.
void Vdp::release_light_gun_latch() noexcept {
    if (hv_latch_soft_) {
        hv_latch_soft_ = false;
        hv_latched_ = false;
    }
}

// The 68000 latches the external interrupt lines partway through an
// instruction, BEFORE the write that changes these enable bits reaches the VDP.
// So from software's point of view, enabling a VDP interrupt whose request is
// already pending does not take effect until one instruction later. Games rely
// on that gap to get one more instruction done first: Fatal Rewind enables
// HINT and then VINT on consecutive instructions and needs its HINT handler to
// run before any VINT, and Sesame Street: Counting Cafe needs a CLR.B to land
// before its VINT so the handler takes the branch that leaves VINT enabled.
//
// Modelled as a two-stage pipeline clocked once per instruction rather than by
// polling mid-instruction, which would mean sub-instruction timing everywhere.
void Vdp::commit_interrupt_enables() noexcept {
    vint_enabled_effective_ = vint_enabled_pending_;
    hint_enabled_effective_ = hint_enabled_pending_;
    vint_enabled_pending_ = (registers_[kRegModeSet2] & 0x20) != 0;
    hint_enabled_pending_ = (registers_[kRegModeSet1] & 0x10) != 0;
}

// The 68000 never says WHICH interrupt it is acknowledging - it asserts VPA and
// the VDP clears the highest-level request it is raising at that moment, which
// need not be the one the CPU thinks it is handling. If a vblank starts between
// the CPU accepting an HINT and sending the acknowledge, the VDP retires the
// VINT instead and the CPU takes the same HINT again on the way out. Fatal
// Rewind depends on getting two HINTs and no VINT out of exactly that race.
//
// This deliberately reads the CURRENT enable bits, not the delayed ones: the
// delay models when the CPU notices the lines, not when the VDP applies them.
void Vdp::acknowledge_interrupt() noexcept {
    if (vertical_interrupt_pending_ && (registers_[kRegModeSet2] & 0x20) != 0) {
        vertical_interrupt_pending_ = false;
        note_int_event("ACK retired VINT");
        note_raster_event(3, registers_[kRegLineCounter]);
        return;
    }
    if (horizontal_interrupt_pending_ && (registers_[kRegModeSet1] & 0x10) != 0) {
        horizontal_interrupt_pending_ = false;
        note_int_event("ACK retired HINT");
        return;
    }
    if (external_interrupt_pending_ && external_interrupt_enabled()) {
        external_interrupt_pending_ = false;
        note_int_event("ACK retired EXT");
    }
}

bool Vdp::hblank_timing() const noexcept {
    const int forced = render_at_hblank_override();
    return forced >= 0 ? forced != 0 : render_at_hblank_;
}

void Vdp::sanitize_loaded() noexcept {
    // A position within a line is under one line. Anything larger makes the
    // next advance_cycles walk millions of lines in one call.
    cycles_into_line_ %= kM68kCyclesPerLine;
    // And the line itself inside a frame: the 32X's per-line catch-up walks
    // from its last line to this one.
    if (scanline_ >= lines_per_frame()) {
        scanline_ = 0;
    }
    // The longest stall real DMA can charge is a 64KB fill during active
    // display, about two million cycles; a state claiming billions is a hang.
    constexpr std::uint32_t kMaxPendingDmaCycles = 4'000'000;
    if (pending_dma_cycles_ > kMaxPendingDmaCycles) {
        pending_dma_cycles_ = kMaxPendingDmaCycles;
    }
    if (vint_assert_delay_ > kVintAssertDelayCycles || vint_assert_delay_ < 0) {
        vint_assert_delay_ = 0;
    }
}

void Vdp::derive_raster_samples() noexcept {
    // A state from before version 6 did not record these. Whether the line has
    // been drawn follows from where in it the state was taken; the latch can
    // only be taken as VSRAM now, which is wrong solely when a game wrote the
    // scroll between this line's blank and the save - and then for one line.
    line_rendered_ = hblank_timing() && cycles_into_line_ >= kHblankStartCycle;
    vscroll_latch_[0] = vsram_[0];
    vscroll_latch_[1] = vsram_[1];
}

bool Vdp::advance_cycles(std::uint32_t m68k_cycles) noexcept {
    bool frame_completed = false;
    if (vint_assert_delay_ > 0) {
        vint_assert_delay_ -= static_cast<std::int32_t>(m68k_cycles);
        if (vint_assert_delay_ <= 0) {
            vint_assert_delay_ = 0;
            vertical_interrupt_pending_ = true;
        }
    }
    cycles_into_line_ += m68k_cycles;
    // Only for a game with set_render_at_hblank (see there): draw the line when
    // its horizontal blank begins (kHblankStartCycle), and in the same instant
    // sample the full-screen vertical scroll the NEXT line will use. Every
    // other game is drawn at the end of the line, below, exactly as before.
    const auto render_if_due = [this]() {
        if (hblank_timing() && !line_rendered_ && cycles_into_line_ >= kHblankStartCycle) {
            if (scanline_ < active_scanlines()) {
                render_line(scanline_);
            }
            vscroll_latch_[0] = vsram_[0];
            vscroll_latch_[1] = vsram_[1];
            line_rendered_ = true;
        }
    };
    render_if_due();
    while (cycles_into_line_ >= kM68kCyclesPerLine) {
        cycles_into_line_ -= kM68kCyclesPerLine;

        // Scanlines, not framebuffer height: interlace mode 2 doubles the latter
        // but the raster still draws 224 lines per field.
        const auto active = active_scanlines();
        // Only reached undrawn with MERCURY_RENDER_AT_HBLANK=0: otherwise the
        // render_if_due above has always drawn it on the way past the blank.
        if (scanline_ < active && !line_rendered_) {
            render_line(scanline_);
        }
        line_rendered_ = false;

        // Line counter: decrements across the active area plus the first
        // blanked line, reloading from register 10 everywhere else.
        // Lines 0..active INCLUSIVE count; everything above reloads. Narrowing
        // this to 0..active-1 to match the reference implementation's
        // active-display loop was tried and it breaks Lemmings 2, which runs
        // with V-int disabled and drives everything off H-interrupts.
        // MERCURY_HINT_VBLANK_RUN=1 keeps the counter running through vblank
        // instead of holding it reloaded. A chain that needs a whole number of
        // interrupts per frame gets a different number under each rule, so the
        // game itself is the measurement.
        static const bool vblank_run = [] {
            const char* e = std::getenv("MERCURY_HINT_VBLANK_RUN");
            return e != nullptr && e[0] != '0';
        }();
        // MERCURY_HINT_ACTIVE_EXCL=1 counts lines 0..active-1 instead of
        // 0..active, which moves every raster split down by one interval.
        static const bool active_excl = [] {
            const char* e = std::getenv("MERCURY_HINT_ACTIVE_EXCL");
            return e != nullptr && e[0] != '0';
        }();
        const std::uint32_t count_top = active_excl ? active - 1 : active;
        if (scanline_ <= count_top || vblank_run || hint_run_in_vblank_) {
            // How many lines actually run the counter, and what reload value
            // they see. If these do not match expectations the fault is in the
            // surrounding loop, not in the counter arithmetic.
            ++line_counter_ticks_;
            line_counter_reload_sum_ += registers_[kRegLineCounter];
            // The request is raised on the line BEFORE the counter expires, so
            // the handler runs during the line it was meant for and reads that
            // line from the V counter. Raising it on the expiry line instead
            // makes every handler see the following line - which silently
            // shifts the parity of the values software can observe, and a game
            // watching for one specific odd line then never sees it.
            // Reload AND raise on the same line, using the reload value as it
            // stands BEFORE the handler runs. This is not cosmetic: a handler
            // that reprograms register 10 - which is how raster chains walk
            // down the screen - should not have its new value take effect until
            // the interval after next. Reloading on the following line instead
            // applies the new value one interval early and silently costs the
            // game an interrupt per frame. Time Killers' chain needs five and
            // was getting four.
            bool expired = false;
            if (line_counter_ == 0) {
                line_counter_ = registers_[kRegLineCounter];
                if (hint_raise_on_reload_) {
                    expired = true;
                }
            } else {
                --line_counter_;
            }
            // Deliberately AFTER the reload, not inside the else: a reload value
            // of zero leaves the counter at zero, which must raise on every
            // line. Folding this into the else silently stops a game that sets
            // register 10 to zero from ever getting an H-interrupt at all.
            if (!hint_raise_on_reload_ && line_counter_ == 0) {
                expired = true;
            }
            if (expired) {
                // Counted separately from the interrupts the CPU actually
                // takes: the request is a single level-triggered line, so if
                // software does not acknowledge one before the next expiry the
                // second is simply lost. A large gap between raised and
                // serviced is the signature of a game whose raster chain never
                // gets far enough through its stages.
                ++hint_raise_count_;
                if (scanline_ < hint_raise_lines_.size()) {
                    ++hint_raise_lines_[scanline_];
                }
                note_raster_event(1, registers_[kRegLineCounter]);
                // Raising a request that is already standing destroys the
                // earlier one - there is a single flag, not a queue. Games that
                // count interrupts rather than reading the line number cannot
                // survive that, so it is worth knowing where it happens.
                if (horizontal_interrupt_pending_) {
                    ++hint_lost_count_;
                    if (scanline_ < hint_lost_lines_.size()) {
                        ++hint_lost_lines_[scanline_];
                    }
                }
                horizontal_interrupt_pending_ = true;
            }
        } else {
            line_counter_ = registers_[kRegLineCounter];
            // An extra H-int at the vblank transition was tried here TWICE for
            // Time Killers - once before and once after the interrupt
            // acknowledge semantics were corrected - and measured to change
            // nothing both times. Not carried.
        }

        ++scanline_;
        if (scanline_ == active) {
            vblank_flag_ = true;
            vint_flag_ = true;
            // The status flag goes up immediately, but the CPU cannot answer
            // the interrupt instantly - it finishes the instruction in progress
            // and then spends the best part of fifty cycles acknowledging. That
            // gap is not cosmetic: games poll this flag as a vblank wait while
            // an interrupt handler is also installed, and if the interrupt is
            // taken with zero delay the handler''s own status read consumes the
            // flag every frame and the polling loop never terminates.
            //
            // Gating this on the enable bit - so a frame spent with V-int off
            // leaves no standing request - was tried TWICE for Fatal Rewind and
            // measured to change nothing both times. The reg1 write log proves
            // why: that game disables V-int at frame 280 (which clears the
            // pending request) and re-enables it at frame 283 with nothing
            // pending, so the interrupt that kills it is freshly latched and
            // entirely legitimate. Do not try this a third time.
            vint_assert_delay_ = kVintAssertDelayCycles;
        }
        if (scanline_ >= lines_per_frame()) {
            scanline_ = 0;
            vblank_flag_ = false;
            odd_frame_ = !odd_frame_;
            ++frames_elapsed_;
            frame_completed = true;
            begin_frame();
        }
        // A long step can carry past the next line's blank as well.
        render_if_due();
    }
    return frame_completed;
}

void Vdp::begin_frame() noexcept {
    // Reloaded from register 10 during vblank, as the hardware does. Forcing it
    // to zero here instead hands every frame an extra interrupt on line zero,
    // which wrecks games that ask for exactly one split per frame with a large
    // reload value.
    // Do NOT make this conditional on the counter running through vblank. It
    // was tried: skipping the reload gives the phase a six-stage chain wants,
    // but it also fixes the parity of every raise line, and Lemmings 2 hunts
    // for one specific ODD value of the V counter (0xCF) to lock its chain. With
    // the reload skipped the handler only ever observes even V, the hunt never
    // catches, and the game sits in its 2-line search mode forever.
    line_counter_ = registers_[kRegLineCounter];
}

// --- rendering -------------------------------------------------------------

std::uint16_t Vdp::read_vram16(std::uint32_t address) const noexcept {
    const auto base = address & 0xFFFE;
    return static_cast<std::uint16_t>((vram_[base] << 8) | vram_[base + 1]);
}

void Vdp::unpack_color(std::uint16_t entry, std::uint8_t* rgb) noexcept {
    // 0000 BBB0 GGG0 RRR0
    rgb[0] = kColorLevels[(entry >> 1) & 0x07];
    rgb[1] = kColorLevels[(entry >> 5) & 0x07];
    rgb[2] = kColorLevels[(entry >> 9) & 0x07];
}

std::uint16_t Vdp::horizontal_scroll(std::uint32_t line, bool plane_b) const noexcept {
    const std::uint32_t base = static_cast<std::uint32_t>(registers_[kRegHScrollBase] & 0x3F) << 10;
    std::uint32_t offset = 0;
    switch (registers_[kRegModeSet3] & 0x03) {
        case 0x00: offset = 0; break;                       // whole screen
        case 0x01: offset = (line & 0x07) * 4; break;       // prohibited; first cell row
        case 0x02: offset = (line & ~0x07u) * 4; break;     // per cell row
        default: offset = line * 4; break;                  // per line
    }
    return static_cast<std::uint16_t>(read_vram16(base + offset + (plane_b ? 2 : 0)) & 0x03FF);
}

std::uint16_t Vdp::vertical_scroll(std::uint32_t column, bool plane_b) const noexcept {
    // MERCURY_VSCROLL_ADJ=<signed>: shift both planes vertically. Purely a
    // diagnostic - if one value makes two scenes with different scroll values
    // both correct, the fault is a constant offset in this path; if each scene
    // wants a different value, it is not.
    static const long adj = [] {
        const char* e = std::getenv("MERCURY_VSCROLL_ADJ");
        return e == nullptr ? 0L : std::strtol(e, nullptr, 10);
    }();
    if ((registers_[kRegModeSet3] & 0x04) == 0) {
        // Full screen: the value latched in the blank before this line (see
        // advance_cycles), not whatever VSRAM holds by the time it is drawn.
        const std::size_t which = plane_b ? 1 : 0;
        const std::uint16_t value = hblank_timing() ? vscroll_latch_[which] : vsram_[which];
        return static_cast<std::uint16_t>((value + adj) & 0x03FF);
    }
    // Per two cells: one entry pair every 16 pixels.
    const std::size_t index = ((column / 16) * 2 + (plane_b ? 1 : 0)) % kVsramEntries;
    return static_cast<std::uint16_t>(vsram_[index] & 0x03FF);
}

bool Vdp::window_covers(std::uint32_t cell_x, std::uint32_t line) const noexcept {
    // MERCURY_NO_WINDOW=1: never draw the window. When a game's own code plainly
    // asks for a window and the result looks wrong, the first thing to settle is
    // whether the picture is right WITHOUT it - that separates "the window
    // should not be here" from "the window's content is wrong".
    static const bool off = [] {
        const char* e = std::getenv("MERCURY_NO_WINDOW");
        return e != nullptr && e[0] != '0';
    }();
    if (off) { return false; }
    const auto wh = static_cast<std::uint32_t>(registers_[kRegWindowH] & 0x1F) * 2;
    const auto wv = static_cast<std::uint32_t>(registers_[kRegWindowV] & 0x1F) * 8;
    const bool from_right = (registers_[kRegWindowH] & 0x80) != 0;
    const bool from_bottom = (registers_[kRegWindowV] & 0x80) != 0;
    const bool in_v = from_bottom ? (line >= wv) : (line < wv);
    const bool in_h = from_right ? (cell_x >= wh) : (cell_x < wh);
    return in_v || in_h;
}

Vdp::PlanePixel Vdp::fetch_plane_pixel(std::uint32_t name_base, std::uint32_t plane_x,
                                       std::uint32_t plane_y) const noexcept {
    PlanePixel pixel;
    // Interlace mode 2 patterns are 8x16 rather than 8x8: the index addresses
    // 64-byte units instead of 32-byte ones and only ten of its bits are used,
    // and a cell spans sixteen of the doubled lines. plane_y is already in that
    // doubled space when this is reached.
    const bool im2 = interlace_double();
    const std::uint32_t cell_rows = im2 ? 16u : 8u;
    const auto width_cells = plane_width_cells();
    const auto cell_x = (plane_x / 8);
    const auto cell_y = (plane_y / cell_rows);
    const auto entry_address = name_base + ((cell_y * width_cells + cell_x) * 2);
    const auto entry = read_vram16(entry_address & 0xFFFF);

    const auto tile = static_cast<std::uint32_t>(entry & (im2 ? 0x03FFu : 0x07FFu));
    const bool hflip = (entry & 0x0800) != 0;
    const bool vflip = (entry & 0x1000) != 0;
    pixel.palette = static_cast<std::uint8_t>((entry >> 13) & 0x03);
    pixel.priority = (entry & 0x8000) != 0;

    std::uint32_t row = plane_y % cell_rows;
    std::uint32_t col = plane_x % 8;
    if (vflip) row = cell_rows - 1 - row;
    if (hflip) col = 7 - col;

    const auto pattern = im2 ? (tile << 6) : (tile << 5);
    const auto byte = vram_[(pattern + row * 4 + col / 2) & 0xFFFF];
    pixel.index = static_cast<std::uint8_t>((col & 1) ? (byte & 0x0F) : (byte >> 4));
    return pixel;
}

void Vdp::render_line(std::uint32_t line) noexcept {
    const auto width = screen_width();
    // The raster draws 224 (or 240) lines whatever the interlace setting; only
    // where they LAND in the framebuffer changes.
    const auto height = active_scanlines();
    if (line >= height) {
        return;
    }
    // Interlace mode 2: this scanline is one of the doubled lines, which field
    // deciding whether it is the even or the odd one. Both fields are woven into
    // the same buffer, so the picture is steady rather than flickering at 30Hz.
    const bool im2 = interlace_double();
    const std::uint32_t field = (im2 && odd_frame_) ? 1u : 0u;
    const std::uint32_t raster_line = im2 ? (line * 2 + field) : line;
    const std::uint32_t output_row = raster_line;

    if (line < kMaxHeight) {
        line_records_[line] = LineRecord{display_enabled(), horizontal_scroll(line, false),
                                        vertical_scroll(0, false), registers_[kRegModeSet2]};
    }
    if (std::FILE* log = cram_dma_log(frames_elapsed_)) {
        std::fprintf(log, "L %u %u %u src=%06X words=%u disp=%u vs=%03X,%03X r11=%02X r12=%02X r16=%02X latch=%03X,%03X hs=%03X,%03X\n",
                     frames_elapsed_, line, cycles_into_line_, g_cram_dma_src, g_cram_dma_words,
                     display_enabled() ? 1u : 0u, vsram_[0] & 0x3FFu, vsram_[1] & 0x3FFu,
                     registers_[kRegModeSet3], registers_[kRegModeSet4], registers_[kRegPlaneSize],
                     vscroll_latch_[0] & 0x3FFu, vscroll_latch_[1] & 0x3FFu,
                     horizontal_scroll(line, false), horizontal_scroll(line, true));
    }

    std::uint8_t backdrop_rgb[3] = {0, 0, 0};
    unpack_color(cram_[registers_[kRegBackdropColor] & 0x3F], backdrop_rgb);

    // DIAGNOSTIC MERCURY_FORCE_BLANK_EDGES=1: force the first and last eight
    // visible lines to the backdrop. Both planes wrap inside those bands in this
    // game, and the question is whether hardware simply does not display them.
    // If forcing it matches the reference, blanking is the mechanism; if it does
    // not, the fault is in what the planes hold and blanking is a dead end.
    static const bool blank_edges = [] {
        const char* e = std::getenv("MERCURY_FORCE_BLANK_EDGES");
        return e != nullptr && e[0] != '0';
    }();
    // Which edge bands are not displayed depends on what the game has set up,
    // and Lemmings 2 uses three different arrangements:
    //
    //   intro    planes 512px tall, no window  -> 8 lines blanked top AND bottom
    //   menu     planes 256px tall, no window  -> nothing blanked; the menu
    //                                             draws a border down to line 223
    //   gameplay planes 256px tall, window from line 168 -> 16 lines blanked at
    //                                             the bottom, nothing at the top
    //
    // The intro is the case where both planes wrap inside the picture and show
    // leftover artwork at the seams. In gameplay nothing wraps, but the panel
    // lives in the window and its bottom two rows are transparent, so plane B -
    // the level - shows through where the hardware shows backdrop. The menu has
    // neither problem, and blanking anything there eats its border.
    // Blank an edge band only where a plane actually wraps into it. The wrap
    // line follows from the scroll and the plane height, so this asks the game's
    // own state rather than guessing per screen:
    //
    //   intro village   A=296 of 512 -> wraps at 216, bottom band leftover
    //                   B=1016       -> wraps at 8,   top band leftover
    //   intro indoor    both 1016 of 512 -> wrap at 8, top only
    //   menu            both 1016 of 256 -> wrap at 8, top only; the bottom is
    //                   the menu's border and must NOT be touched
    //   gameplay        A=8 B=20 of 256 -> no wrap anywhere near an edge
    //
    // Gameplay's band is a separate thing: the panel is in the window from line
    // 168 down and its last two rows are transparent, so plane B shows through.
    // Lemmings 2 scrolls its full-screen pictures by -8 and leaves the rows the
    // artwork does not reach filled with a border tile. How many rows that is
    // depends on the picture, and the plane height tells the two cases apart:
    //
    //   512px planes  intro pictures are 272 rows -> 8 filler rows, bottom only
    //   256px planes  menu picture is 200 rows    -> 24 filler rows, 8 top + 16
    //                                                bottom, both border tiles
    //
    // A plane whose wrap lands within 8 lines of the top is one of these
    // shifted pictures; a plane that wraps near the bottom has run past the end
    // of its artwork there instead. Gameplay wraps at neither end and gets its
    // band from the window instead.
    const std::uint32_t plane_px = static_cast<std::uint32_t>(plane_height_cells()) * 8u;
    const auto wrap_line = [&](std::uint32_t scroll) {
        const auto m = scroll % plane_px;
        return m == 0 ? 0u : plane_px - m;
    };
    const auto wrap_a = wrap_line(vertical_scroll(0, false));
    const auto wrap_b = wrap_line(vertical_scroll(0, true));
    const bool tall_planes = plane_height_cells() == 64;
    const bool top_leftover = (wrap_a != 0 && wrap_a <= 8) || (wrap_b != 0 && wrap_b <= 8);
    const bool bottom_leftover = (wrap_a >= height - 8 && wrap_a < height) ||
                                 (wrap_b >= height - 8 && wrap_b < height);
    const bool bottom_window =
        (registers_[kRegWindowV] & 0x80) != 0 &&
        static_cast<std::uint32_t>(registers_[kRegWindowV] & 0x1F) * 8 < height;
    std::uint32_t bottom_band = 0;
    if (bottom_window) {
        bottom_band = 16u;
    } else if (tall_planes) {
        bottom_band = bottom_leftover ? 8u : 0u;
    } else if (top_leftover) {
        bottom_band = 16u;
    }
    const bool edges_on = blank_edges || blank_display_edges_;
    const bool forced_edge =
        edges_on && ((top_leftover && line < 8) || (bottom_band != 0 && line + bottom_band >= height));
    // Record what this decision was made from, so a screen that blanks when it
    // should not can be diagnosed from the machine rather than from argument.
    if (line == 0) {
        edge_dbg_ = {plane_px, wrap_a, wrap_b,
                     static_cast<std::uint32_t>(top_leftover), bottom_band,
                     static_cast<std::uint32_t>(bottom_window),
                     static_cast<std::uint32_t>(edges_on)};
    }

    // See GameQuirks::ignore_midscreen_blank. The edge bands above are still
    // honoured; this only covers a blank that lands inside the picture.
    const bool keep_drawing = ignore_midscreen_blank_ && !display_enabled() && !forced_edge;

    auto* out = frame_pixels_.data() + static_cast<std::size_t>(output_row) * kMaxWidth * 3;
    if ((!display_enabled() && !keep_drawing) || forced_edge) {
        auto* blank_backdrop =
            frame_backdrop_.data() + static_cast<std::size_t>(output_row) * kMaxWidth;
        for (std::uint32_t x = 0; x < width; ++x) {
            out[x * 3 + 0] = backdrop_rgb[0];
            out[x * 3 + 1] = backdrop_rgb[1];
            out[x * 3 + 2] = backdrop_rgb[2];
            // A blanked line is backdrop all the way across, so a 32X layer
            // behind it is fully visible. That is how a 32X game that leaves
            // the Mega Drive side switched off gets a full-screen picture.
            blank_backdrop[x] = 1;
        }
        return;
    }

    const std::uint32_t plane_a_base = static_cast<std::uint32_t>(registers_[kRegPlaneANameBase] & 0x38) << 10;
    const std::uint32_t plane_b_base = static_cast<std::uint32_t>(registers_[kRegPlaneBNameBase] & 0x07) << 13;
    const std::uint32_t window_base = wide_mode()
        ? (static_cast<std::uint32_t>(registers_[kRegWindowNameBase] & 0x3E) << 10)
        : (static_cast<std::uint32_t>(registers_[kRegWindowNameBase] & 0x3F) << 10);

    const std::uint32_t plane_w_px = static_cast<std::uint32_t>(plane_width_cells()) * 8;
    // A cell is sixteen doubled lines tall in interlace mode 2, so the plane
    // wraps at twice the height it otherwise would.
    const std::uint32_t plane_h_px =
        static_cast<std::uint32_t>(plane_height_cells()) * (im2 ? 16u : 8u);
    const auto hscroll_a = horizontal_scroll(line, false);
    const auto hscroll_b = horizontal_scroll(line, true);

    // Per-pixel layers, resolved together at the end. Keeping them apart is
    // what makes the priority order legible.
    std::array<PlanePixel, kMaxWidth> layer_a{};
    std::array<PlanePixel, kMaxWidth> layer_b{};

    for (std::uint32_t x = 0; x < width; ++x) {
        const auto cell_x = x / 8;
        const bool in_window = window_covers(cell_x, line);
        if (in_window) {
            // The window does not scroll and uses its own, always-full-width
            // name table: 64 cells across in H40, 32 in H32.
            const auto window_cells = wide_mode() ? 64u : 32u;
            const std::uint32_t cell_rows = im2 ? 16u : 8u;
            const auto entry_address =
                window_base + (((raster_line / cell_rows) * window_cells + cell_x) * 2);
            const auto entry = read_vram16(entry_address & 0xFFFF);
            const auto tile = static_cast<std::uint32_t>(entry & (im2 ? 0x03FFu : 0x07FFu));
            const bool hflip = (entry & 0x0800) != 0;
            const bool vflip = (entry & 0x1000) != 0;
            std::uint32_t row = raster_line % cell_rows;
            std::uint32_t col = x % 8;
            if (vflip) row = cell_rows - 1 - row;
            if (hflip) col = 7 - col;
            const auto pattern = im2 ? (tile << 6) : (tile << 5);
            const auto byte = vram_[(pattern + row * 4 + col / 2) & 0xFFFF];
            layer_a[x].index = static_cast<std::uint8_t>((col & 1) ? (byte & 0x0F) : (byte >> 4));
            layer_a[x].palette = static_cast<std::uint8_t>((entry >> 13) & 0x03);
            layer_a[x].priority = (entry & 0x8000) != 0;
        } else {
            const auto vscroll_a = vertical_scroll(x, false);
            const std::uint32_t ax = (x + plane_w_px - (hscroll_a % plane_w_px)) % plane_w_px;
            // The doubled line, not the scanline: the scroll value is written by
            // the game in the same doubled space.
            const std::uint32_t ay = (raster_line + vscroll_a) % plane_h_px;
            layer_a[x] = fetch_plane_pixel(plane_a_base, ax, ay);
        }
        const auto vscroll_b = vertical_scroll(x, true);
        const std::uint32_t bx = (x + plane_w_px - (hscroll_b % plane_w_px)) % plane_w_px;
        const std::uint32_t by = (raster_line + vscroll_b) % plane_h_px;
        layer_b[x] = fetch_plane_pixel(plane_b_base, bx, by);
    }

    // --- sprites ---
    // Walked through the link field rather than by index: the list order is
    // what decides which sprite wins, and it is not the table order.
    std::array<PlanePixel, kMaxWidth> layer_s{};
    const std::uint32_t sprite_table = static_cast<std::uint32_t>(registers_[kRegSpriteTableBase] & 0x7F) << 9;
    const std::uint32_t sprite_capacity = wide_mode() ? kMaxSpritesH40 : kMaxSpritesH32;
    // DIAGNOSTIC: which layer is a given artefact in? Sprite limits were ruled
    // out for the tachometer needle, so this only isolates the layer now.
    static const bool no_sprites = [] {
        const char* e = std::getenv("MERCURY_NO_SPRITES");
        return e != nullptr && e[0] != '0';
    }();
    const std::uint32_t per_line_limit = no_sprites ? 0u : (wide_mode() ? 20u : 16u);
    const std::uint32_t pixel_limit = wide_mode() ? 320u : 256u;

    std::uint32_t entry_index = 0;
    std::uint32_t drawn = 0;
    std::uint32_t pixels_used = 0;
    for (std::uint32_t visited = 0; visited < sprite_capacity; ++visited) {
        // In interlace mode 2 a sprite's vertical position and height are in the
        // doubled space: ten bits of Y instead of nine, an offset of 256 instead
        // of 128, and a cell sixteen lines tall instead of eight.
        const std::uint32_t y_mask = im2 ? 0x03FFu : 0x01FFu;
        const std::int32_t y_offset = im2 ? 256 : 128;
        const std::uint32_t cell_height = im2 ? 16u : 8u;

        const auto base = sprite_table + entry_index * 8;
        const auto y_raw = static_cast<std::int32_t>(read_vram16(base) & y_mask);
        const auto size_byte = vram_[(base + 2) & 0xFFFF];
        const auto link = static_cast<std::uint32_t>(vram_[(base + 3) & 0xFFFF] & 0x7F);
        const auto attribute = read_vram16(base + 4);
        const auto x_raw = static_cast<std::int32_t>(read_vram16(base + 6) & 0x01FF);

        const std::uint32_t cells_h = ((size_byte >> 2) & 0x03) + 1;
        const std::uint32_t cells_v = (size_byte & 0x03) + 1;
        const auto sprite_y = y_raw - y_offset;
        const auto sprite_x = x_raw - 128;
        const auto sprite_h = static_cast<std::int32_t>(cells_v * cell_height);

        const auto row_in_sprite = static_cast<std::int32_t>(raster_line) - sprite_y;
        if (row_in_sprite >= 0 && row_in_sprite < sprite_h) {
            if (drawn >= per_line_limit || pixels_used >= pixel_limit) {
                sprite_overflow_ = true;
                break;
            }
            ++drawn;
            pixels_used += cells_h * 8;

            const auto tile = static_cast<std::uint32_t>(attribute & (im2 ? 0x03FFu : 0x07FFu));
            const bool hflip = (attribute & 0x0800) != 0;
            const bool vflip = (attribute & 0x1000) != 0;
            const auto palette = static_cast<std::uint8_t>((attribute >> 13) & 0x03);
            const bool priority = (attribute & 0x8000) != 0;

            auto sprite_row = static_cast<std::uint32_t>(row_in_sprite);
            if (vflip) sprite_row = cells_v * cell_height - 1 - sprite_row;

            for (std::uint32_t px = 0; px < cells_h * 8; ++px) {
                const auto screen_x = sprite_x + static_cast<std::int32_t>(px);
                if (screen_x < 0 || screen_x >= static_cast<std::int32_t>(width)) {
                    continue;
                }
                auto sprite_col = px;
                if (hflip) sprite_col = cells_h * 8 - 1 - sprite_col;

                // Sprite tiles run DOWN each column before moving right, which
                // is the opposite of every plane tilemap.
                const auto cell_col = sprite_col / 8;
                const auto cell_row = sprite_row / cell_height;
                const auto tile_index = tile + cell_col * cells_v + cell_row;
                const auto row = sprite_row % cell_height;
                const auto col = sprite_col % 8;
                const auto pattern = im2 ? (tile_index << 6) : (tile_index << 5);
                const auto byte = vram_[(pattern + row * 4 + col / 2) & 0xFFFF];
                const auto index = static_cast<std::uint8_t>((col & 1) ? (byte & 0x0F) : (byte >> 4));
                if (index == 0) {
                    continue;
                }
                auto& slot = layer_s[static_cast<std::uint32_t>(screen_x)];
                if (slot.index != 0) {
                    sprite_collision_ = true;
                    continue;  // an earlier sprite in the list already owns it
                }
                slot.index = index;
                slot.palette = palette;
                slot.priority = priority;
            }
        }

        if (link == 0 || link >= sprite_capacity) {
            break;  // link zero terminates the list
        }
        entry_index = link;
    }

    // MERCURY_ONLY_PLANE=a|b|s: draw one layer and leave the rest empty. When
    // the framebuffer and the nametable disagree about what is on a scanline,
    // this is the shortest way to find out which layer the pixels came from -
    // reading VRAM only says what the layer SHOULD hold.
    static const char only_layer = [] {
        const char* e = std::getenv("MERCURY_ONLY_PLANE");
        return e == nullptr ? '\0' : e[0];
    }();
    if (only_layer != '\0') {
        if (only_layer != 'a') layer_a.fill(PlanePixel{});
        if (only_layer != 'b') layer_b.fill(PlanePixel{});
        if (only_layer != 's') layer_s.fill(PlanePixel{});
    }

    // --- priority resolution ---
    // High-priority sprite, then high A, high B, then the low-priority pass in
    // the same order. Anything still transparent shows the backdrop.
    const bool left_column_blanked = (registers_[kRegModeSet1] & 0x20) != 0;
    // Shadow/highlight, register 12 bit 3. With it on, a pixel is DARKENED by
    // default and only restored to full brightness where a high-priority layer
    // covers it, and two sprite colours stop being colours at all: palette 3
    // index 15 darkens whatever is underneath and index 14 brightens it, with
    // neither drawing a pixel of its own. Ignoring the mode is why a game's
    // shadow sprites come out as solid blocks of whatever palette 3 entry 15
    // happens to hold - white, in Mortal Kombat's case, instead of a dark
    // translucent shadow under the fighters.
    const bool shadow_highlight = (registers_[kRegModeSet4] & 0x08) != 0;
    auto* backdrop_out =
        frame_backdrop_.data() + static_cast<std::size_t>(output_row) * kMaxWidth;
    for (std::uint32_t x = 0; x < width; ++x) {
        std::uint16_t color = cram_[registers_[kRegBackdropColor] & 0x3F];
        const auto& a = layer_a[x];
        const auto& b = layer_b[x];
        const auto& s = layer_s[x];

        int shade = 0;  // -1 darkened, 0 normal, +1 brightened
        bool sprite_is_operator = false;
        if (shadow_highlight) {
            const bool background_priority =
                (a.index != 0 && a.priority) || (b.index != 0 && b.priority);
            if (!background_priority) {
                shade = -1;
            }
            if (s.index != 0 && s.palette == 3 && (s.index == 14 || s.index == 15)) {
                sprite_is_operator = true;
                // The two operators cancel each other rather than stacking: a
                // darkened pixel brightened once returns to normal.
                shade = (s.index == 15) ? ((shade > 0) ? 0 : -1) : ((shade < 0) ? 0 : 1);
            } else if (s.index != 0 && s.priority) {
                shade = 0;  // a high-priority sprite pixel is never darkened
            }
        }
        const bool sprite_visible = (s.index != 0) && !sprite_is_operator;

        // DIAGNOSTIC MERCURY_SPRITES_WIN=1: let every sprite beat the planes,
        // to test whether a LOW-priority sprite is being punched through by
        // high-priority plane pixels behind it.
        static const bool sprites_win = [] {
            const char* e = std::getenv("MERCURY_SPRITES_WIN");
            return e != nullptr && e[0] != '0';
        }();
        if (sprite_visible && (s.priority || sprites_win)) {
            color = cram_[(s.palette * 16 + s.index) % kCramEntries];
        } else if (a.index != 0 && a.priority) {
            color = cram_[(a.palette * 16 + a.index) % kCramEntries];
        } else if (b.index != 0 && b.priority) {
            color = cram_[(b.palette * 16 + b.index) % kCramEntries];
        } else if (sprite_visible) {
            color = cram_[(s.palette * 16 + s.index) % kCramEntries];
        } else if (a.index != 0) {
            color = cram_[(a.palette * 16 + a.index) % kCramEntries];
        } else if (b.index != 0) {
            color = cram_[(b.palette * 16 + b.index) % kCramEntries];
        }

        std::uint8_t rgb[3];
        unpack_color(color, rgb);
        if (shade < 0) {
            for (auto& channel : rgb) {
                channel = static_cast<std::uint8_t>(channel / 2);
            }
        } else if (shade > 0) {
            for (auto& channel : rgb) {
                channel = static_cast<std::uint8_t>(128 + channel / 2);
            }
        }
        if (left_column_blanked && x < 8) {
            rgb[0] = backdrop_rgb[0];
            rgb[1] = backdrop_rgb[1];
            rgb[2] = backdrop_rgb[2];
        }
        out[x * 3 + 0] = rgb[0];
        out[x * 3 + 1] = rgb[1];
        out[x * 3 + 2] = rgb[2];
        // Was anything actually drawn here, or is this the backdrop showing?
        //
        // Purely additive - it changes no pixel and no decision above. The
        // 32X needs it: its layer shows through the Mega Drive picture only
        // where the Mega Drive drew nothing, and by this point `color` has
        // already been resolved to RGB with no way to tell the two apart.
        // A blanked left column counts as backdrop, because that is what is
        // on the screen there.
        const bool drew_something = (sprite_visible || a.index != 0 || b.index != 0) &&
                                    !(left_column_blanked && x < 8);
        backdrop_out[x] = drew_something ? 0u : 1u;
    }
}

std::string Vdp::describe() const {
    std::ostringstream out;
    out << "VDP (315-5313, Mode 5)"
        << "\n  mode      : " << (wide_mode() ? "H40 320x" : "H32 256x") << screen_height()
        << (pal_ ? " PAL" : " NTSC")
        << "\n  display   : " << (display_enabled() ? "on" : "off")
        << "\n  planes    : " << plane_width_cells() << "x" << plane_height_cells() << " cells"
        << "\n  interrupts: vint " << ((registers_[kRegModeSet2] & 0x20) ? "on" : "off")
        << ", hint " << ((registers_[kRegModeSet1] & 0x10) ? "on" : "off")
        << " (every " << static_cast<int>(registers_[kRegLineCounter]) << " lines)"
        << "\n  counters  : frame " << frames_elapsed_ << ", line " << scanline_
        << "\n  traffic   : " << register_write_count_ << " register writes, "
        << data_write_count_ << " data writes, " << dma_count_ << " DMA transfers"
        << "\n  bases     : A " << std::hex << std::setw(4) << std::setfill('0')
        << ((registers_[kRegPlaneANameBase] & 0x38) << 10)
        << ", B " << std::setw(4) << ((registers_[kRegPlaneBNameBase] & 0x07) << 13)
        << ", sprites " << std::setw(4) << ((registers_[kRegSpriteTableBase] & 0x7F) << 9)
        << std::dec;
    return out.str();
}

}  // namespace mercury::genesis








































