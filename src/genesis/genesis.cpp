#include "mercury/genesis/genesis.hpp"

#include "mercury/genesis/game_quirks.hpp"
#include "mercury/core/save_state.hpp"

#include <cstdlib>
#include <sstream>

namespace mercury::genesis {

namespace {

constexpr std::uint32_t kRomEnd = 0x3FFFFF;
constexpr std::uint32_t kZ80Base = 0xA00000;
constexpr std::uint32_t kZ80End = 0xA0FFFF;
constexpr std::uint32_t kIoBase = 0xA10000;
constexpr std::uint32_t kIoEnd = 0xA1001F;
constexpr std::uint32_t kZ80BusReq = 0xA11100;
constexpr std::uint32_t kZ80Reset = 0xA11200;
constexpr std::uint32_t kVdpBase = 0xC00000;
constexpr std::uint32_t kVdpEnd = 0xC0001F;
constexpr std::uint32_t kWorkRamBase = 0xE00000;
constexpr std::uint32_t kHiddenSramBase = 0x200000;
// A serial EEPROM answers on two byte addresses, not a 64KB window. Keeping
// the shadow that narrow is both closer to the hardware and far less likely to
// hide ROM the game needs.
constexpr std::uint32_t kHiddenSramEnd = 0x200001;

// The 32X's SH-2s run at 23.01MHz against the 68000's 7.67MHz, which is three
// to one almost exactly.
constexpr std::int64_t kSh2CyclesPer68k = 3;
// How far the 68000 may get ahead before the SH-2s are run to catch up.
//
// This was 64, chosen because it is what PicoDrive uses as its threshold.
// Ares syncs roughly four times more often - every 50 SH-2 cycles, which is
// about 17 of these - and carries a comment saying the value is tuned and
// naming the two cartridges that hang when it is wrong: Brutal Unleashed in
// its attract mode and Chaotix on its intro screen. Both are on the list of
// cartridges that do not run here, which is reason enough to match the
// tighter figure rather than the looser one.
const std::uint32_t kThirtyTwoXQuantum = [] {
    // MERCURY_32X_QUANTUM overrides it for a measurement. See the note above:
    // this is the granularity at which the two sides observe each other, and a
    // startup handshake is a race decided by it.
    const char* e = std::getenv("MERCURY_32X_QUANTUM");
    const long n = e == nullptr ? 64 : std::strtol(e, nullptr, 10);
    return n > 0 ? static_cast<std::uint32_t>(n) : 64u;
}();

}  // namespace

// The 68000's clock, NTSC and PAL: each console's master clock over seven.
// The Sega CD's sub CPU has its own 12.5MHz crystal whichever it is plugged
// into, so the ratio between the two depends on the console.
constexpr std::uint32_t kM68kClockNtsc = 7'670'453;
constexpr std::uint32_t kM68kClockPal = 7'600'489;

Genesis::Genesis()
    : thirtytwox_(std::make_unique<mercury::thirtytwox::ThirtyTwoX>()),
      segacd_(std::make_unique<mercury::segacd::SegaCd>()) {
    attach_buses();
    reset();
}

void Genesis::attach_buses() noexcept {
    mercury::core::M68kBusCallbacks m68k_bus;
    m68k_bus.read16 = [this](std::uint32_t address) { return read16(address); };
    m68k_bus.write16 = [this](std::uint32_t address, std::uint16_t value) { write16(address, value); };
    m68k_bus.read8 = [this](std::uint32_t address) { return read8(address); };
    m68k_bus.write8 = [this](std::uint32_t address, std::uint8_t value) { write8(address, value); };
    m68k_.attach_bus(std::move(m68k_bus));

    mercury::core::Z80BusCallbacks z80_bus;
    z80_bus.read8 = [this](std::uint16_t address) { return z80_read(address); };
    z80_bus.write8 = [this](std::uint16_t address, std::uint8_t value) { z80_write(address, value); };
    // The Mega Drive's Z80 has no I/O devices on its own port space.
    z80_bus.in = [](std::uint8_t) -> std::uint8_t { return 0xFF; };
    z80_bus.out = [](std::uint8_t, std::uint8_t) {};
    z80_.attach_bus(std::move(z80_bus));

    vdp_.set_memory_reader([this](std::uint32_t address) { return read16(address); });
}

void Genesis::set_pal(bool pal) noexcept {
    pal_ = pal;
    vdp_.set_pal(pal);
    // The adapter reports the console it is plugged into, and a 32X game
    // cross-checks the two. See thirtytwox::Vdp::kNotPal.
    thirtytwox_->vdp().set_ntsc(!pal);
}

void Genesis::reset() noexcept {
    reset_internal(/*power_on=*/true);
}

// The RESET button is not a power cycle: it pulses /RESET at the processors and
// the video chip, but nothing clears RAM, and a battery-backed save obviously
// survives. Games rely on the difference - HardBall III writes its save, prints
// "First time initialization complete. Press RESET to continue" and waits,
// because on the way back up it expects to find that data still in place.
void Genesis::soft_reset() noexcept {
    reset_internal(/*power_on=*/false);
}

void Genesis::reset_internal(bool power_on) noexcept {
    if (power_on) {
        std::fill(work_ram_.begin(), work_ram_.end(), std::uint8_t{0});
        std::fill(z80_ram_.begin(), z80_ram_.end(), std::uint8_t{0});
        vdp_.reset();
    } else {
        vdp_.soft_reset();
    }
    vdp_.set_pal(pal_);
    ym2612_.reset();
    psg_.reset();
    port1_.reset();
    port2_.reset();
    ext_.reset();
    port1_.set_kind(ControllerPort::Kind::pad3);
    port2_.set_kind(ControllerPort::Kind::pad3);
    ext_.set_kind(ControllerPort::Kind::none);
    z80_bus_requested_ = false;
    z80_reset_asserted_ = true;
    z80_bank_ = 0;
    z80_bank_bits_ = 0;
    z80_cycle_debt_ = 0;
    psg_cycle_debt_ = 0;
    z80_in_vblank_ = false;
    z80_interrupt_cycles_ = 0;
    if (power_on) {
        // Diagnostic counters measure a whole run; a reset in the middle of one
        // is part of what is being measured, so they are not cleared here.
        m68k_unimplemented_count_ = 0;
        m68k_unimplemented_opcodes_.clear();
        z80_unimplemented_count_ = 0;
        z80_last_bad_pc_ = 0;
        z80_instructions_ = 0;
        z80_interrupts_ = 0;
        m68k_hint_count_ = 0;
        m68k_vint_count_ = 0;
    }
    // A cartridge that declares battery RAM has it live from power-on. The
    // $A130F1 register exists to switch it back OUT of the way on carts where
    // it overlaps ROM - it is not an enable that software must set first, and
    // treating it as one fails the read-write test games run at boot.
    // Banks start as an identity map, so a cartridge that never touches the
    // mapper behaves exactly as a flat ROM.
    for (std::uint32_t slot = 0; slot < rom_banks_.size(); ++slot) {
        rom_banks_[slot] = slot * 0x80000u;
    }
    sram_enabled_ = cartridge_.header().has_sram;
    hidden_sram_active_ = false;
    eeprom_.reset();
    // Per-game behaviour switches, applied before the CPU comes out of reset so
    // the very first instruction already runs under them.
    const GameQuirks quirks = quirks_for(cartridge_.header());
    // A DISC HAS NO HEADER TO LOOK UP, so the lookup above returns the
    // defaults for one and this has to come from the disc instead. Applied
    // here rather than in load_cd because reset() runs after it and would
    // otherwise put the cartridge default straight back - which it was
    // silently doing, harmlessly only because the two happened to agree.
    core::M68kCpu::set_charge_ea_time(cd_mode_ ? disc_charges_ea_time()
                                               : quirks.charge_ea_time);
    vdp_.set_hint_raise_on_reload(quirks.hint_raise_on_reload);
    vdp_.set_hint_reload_on_write(quirks.hint_reload_on_write);
    vdp_.set_hint_run_in_vblank(quirks.hint_run_in_vblank);
    vdp_.set_blank_display_edges(quirks.blank_display_edges);
    vdp_.set_ignore_midscreen_blank(quirks.ignore_midscreen_blank);
    vdp_.set_render_at_hblank(cd_mode_ && disc_renders_at_hblank());
    if (std::getenv("MERCURY_QUIRK_LOG") != nullptr) {
        std::printf("quirks applied: ea=%d raise_on_reload=%d reload_on_write=%d run_in_vblank=%d edges=%d sram=%d(%06X-%06X)\n",
                    quirks.charge_ea_time ? 1 : 0, quirks.hint_raise_on_reload ? 1 : 0,
                    quirks.hint_reload_on_write ? 1 : 0, quirks.hint_run_in_vblank ? 1 : 0,
                    quirks.blank_display_edges ? 1 : 0, quirks.force_sram ? 1 : 0,
                    quirks.sram_start, quirks.sram_end);
    }
    forced_sram_ = quirks.force_sram;
    forced_sram_start_ = quirks.sram_start;
    forced_sram_end_ = quirks.sram_end;
    // Only Virtua Racing carries an SVP. Attaching the ROM is what arms the DSP;
    // for every other cartridge it stays detached and declines every address.
    if (cartridge_.loaded() && cartridge_.header().serial.find("MK-1229") != std::string::npos) {
        svp_.attach_rom(cartridge_.rom().data(), cartridge_.rom().size());
        // The whole cartridge lags, not just the DSP's DRAM. The SVP and the
        // 68000 share the cartridge bus - the board deliberately staggers them
        // with a delayed, inverted clock "to allow interleaved reads on the
        // external ROM by both the Mega Drive and the SVP" - so a VDP DMA burst
        // reading either the DRAM or the ROM gets the PREVIOUS word back.
        //
        // Virtua Racing compensates in both cases by programming its source two
        // bytes high, and it says so in its own data: every entry in the
        // needle-artwork table at 0x35A86 stores the VDP register values next
        // to the real address, and they differ by exactly 2 -
        //   95 FE 96 B1 97 0E -> 0x1D63FC, alongside 00 1D 63 FA.
        // Covering only the DRAM left every ROM transfer one word early, which
        // shifts each tile row into its neighbour's half and cuts the
        // tachometer needle into offset pieces.
        vdp_.set_dma_lag_range(0x000000, 0x3FFFFF);
    } else if (cd_mode_) {
        svp_.attach_rom(nullptr, 0);
        // Word RAM lags a VDP DMA by one word exactly as the SVP cartridge
        // does: the first word written is whatever was left on the bus, and
        // the last source word is never read. Genesis Plus GX applies the one
        // rule to both, for the same bus reason.
        vdp_.set_dma_lag_range(0x200000, 0x3FFFFF);
    } else {
        svp_.attach_rom(nullptr, 0);
        vdp_.set_dma_lag_range(0, 0);
    }
    svp_.reset();
    // The CD unit restarts with the console. Power-on clears its RAM; the
    // RESET button leaves RAM and the battery RAM alone.
    if (cd_mode_) {
        segacd_->reset(power_on);
    }
    svp_cycle_debt_ = 0;
    // Back to inert: ADEN clear, both SH-2s held. A reset has to put the
    // adapter back where a cartridge expects to find it, or a soft reset
    // would leave the 32X half-started.
    thirtytwox_->reset();
    // With a disc in, the slot is EMPTY and has to keep reading that way: the
    // 32X firmware picks its boot path off nCART, and re-attaching the (now
    // cleared) cartridge here would tell it a cartridge is present again.
    thirtytwox_->attach_rom(cd_mode_ ? nullptr : &cartridge_.rom());
    z80_.reset();
    // The 68000 takes SSP and PC from the first two vectors in the cartridge.
    m68k_.reset();
}

// Whether this disc wants the 68000's effective-address time charged.
//
// A disc has no cartridge header, so there is no quirk table to look it up
// in - and it matters more on a disc than on a cartridge, because the main
// CPU's speed is measured against a sub CPU and a 1x drive rather than only
// against the video chip.
//
// On by default, and that is the accurate setting: against the ProcessorTests
// vectors, off is wrong on 292,752 of 653,265 instructions and on is wrong on
// 59,143. It is also what makes The Lawnmower Man play its intro rather than
// sit on its title screen.
//
// HEART OF THE ALIEN IS THE ONE THAT DISAGREES, measured rather than
// suspected: over 2400 frames with it on the game holds its display up for
// 752 frames instead of 2161, reads 73 fewer sectors and never starts its CD
// audio at all - a black screen. With it off it plays. Two games wanting
// opposite settings is exactly what the cartridge quirk table exists for, so
// a disc gets the same treatment, matched on the title in the disc's own
// header rather than on a file name someone may have changed.
bool Genesis::disc_charges_ea_time() const {
    if (!segacd_->disc().loaded()) {
        return true;
    }
    return segacd_->disc().title().rfind("HEART OF THE ALIEN", 0) != 0;
}

// The discs whose video needs Vdp::set_render_at_hblank - see there. Only
// Mortal Kombat so far; the other five discs whose picture changed under it in
// the 2026-10-03 sweep came out even against Genesis Plus GX, so they keep the
// timing every other game uses.
bool Genesis::disc_renders_at_hblank() const {
    if (!segacd_->disc().loaded()) {
        return false;
    }
    return segacd_->disc().title().rfind("MORTAL KOMBAT", 0) == 0;
}

Genesis::LoadResult Genesis::load_cd(const std::filesystem::path& disc,
                                     const std::vector<std::uint8_t>& bios) {
    LoadResult result;
    std::string error;
    if (!segacd_->load_bios(bios, error)) {
        result.message = error;
        return result;
    }
    if (disc.empty()) {
        segacd_->eject_disc();
    } else if (!segacd_->insert_disc(disc, error)) {
        result.message = error;
        return result;
    }
    // No cartridge: the CD unit answers the whole bottom of the map. Taking
    // the cartridge out rather than merely shadowing it keeps the 32X, the
    // SVP and the battery code from finding a stale one.
    cartridge_ = Cartridge{};
    cd_mode_ = true;
    // The 68000's timing comes from the disc rather than a quirk table - see
    // disc_charges_ea_time - and reset() below is what applies it.
    // THE 32X IS STILL PLUGGED IN, and a few discs are "Sega CD 32X" titles
    // that use both add-ons at once (Night Trap, Supreme Warrior, Corpse
    // Killer, Fahrenheit disc 2). The adapter's registers are decoded either
    // way; what those discs need is for the SH-2s to have something to run.
    //
    // A cartridge carries a startup block in its header and GenX synthesises
    // the boot sequence from it. A disc has no such header - the game loads
    // its SH-2 code from the disc itself and expects the adapter's own
    // firmware to be running - so on a disc the REAL 32X BIOS is the only way
    // in, and without it the SH-2s stay parked. PicoDrive draws the same line
    // (it skips the cartridge initial-data-load when no cartridge is in).
    thirtytwox_->attach_rom(nullptr);
    thirtytwox_->attach_bios(disc);
    // The disc's region decides the console's timing, as a cartridge
    // header's does.
    set_pal(segacd_->disc().loaded() && segacd_->disc().region() == 'E');
    reset();
    std::ostringstream message;
    message << segacd_->bios_description();
    if (segacd_->disc().loaded()) {
        message << "\n" << segacd_->disc().title() << "\n" << segacd_->disc().describe();
    } else {
        message << "\nno disc";
    }
    result.loaded = true;
    result.message = message.str();
    return result;
}

Genesis::LoadResult Genesis::load_rom(const std::filesystem::path& path) {
    LoadResult result;
    std::string error;
    if (!cartridge_.load(path, error)) {
        result.message = error;
        return result;
    }
    // A cartridge takes the CD unit out of the picture again.
    cd_mode_ = false;
    // Region byte drives the machine's timing. 'E' and 'A' are the PAL markers;
    // everything else, including the common "JUE", runs at 60Hz here.
    const auto& region = cartridge_.header().region;
    const bool pal_only = !region.empty() &&
                          region.find('U') == std::string::npos &&
                          region.find('J') == std::string::npos &&
                          (region.find('E') != std::string::npos || region.find('A') != std::string::npos);
    set_pal(pal_only);
    // The adapter's own firmware, if it is on disk near the cartridge. Found
    // before the reset so the processors come out of it into real code rather
    // than into the synthesised stand-in. Absent, everything carries on as
    // before - see ThirtyTwoX::attach_bios.
    thirtytwox_->attach_bios(path);
    reset();
    // One cartridge, two buses. The SH-2s see the same ROM the 68000 does,
    // by reference rather than by copy - these are megabytes.
    thirtytwox_->attach_rom(&cartridge_.rom());
    result.loaded = true;
    result.message = cartridge_.describe();
    return result;
}

// --- 68000 address space ---------------------------------------------------

// A cart whose declared battery window is only a byte or two wide does not have
// parallel RAM at all - that is a serial EEPROM, which answers on one or two
// byte addresses. The header has no way to say so, so the width is the tell.
bool Genesis::serial_eeprom_cart() const noexcept {
    const auto& header = cartridge_.header();
    // The window also has to be where a device would actually sit. Some headers
    // carry a byte-wide window at a nonsense address inside the ROM - Sports
    // Talk Baseball declares $030D41 - and treating that as a device shadows
    // real program data.
    return header.has_sram && (header.sram_end - header.sram_start) < 2 &&
           header.sram_start >= kHiddenSramBase && header.sram_start <= 0x20FFFF;
}

std::uint32_t Genesis::map_rom(std::uint32_t address) const noexcept {
    return rom_banks_[(address >> 19) & 0x07] | (address & 0x7FFFF);
}

std::uint32_t Genesis::sram_offset(std::uint32_t address) const noexcept {
    const auto base = forced_sram_ ? forced_sram_start_ : cartridge_.header().sram_start;
    return (address - base) & 0xFFFF;
}

bool Genesis::in_sram(std::uint32_t address) const noexcept {
    // A cartridge whose header lies about its battery RAM gets the window from
    // the quirk table instead, and skips the header-derived tests entirely.
    if (forced_sram_) {
        return address >= forced_sram_start_ && address <= forced_sram_end_;
    }
    const auto& header = cartridge_.header();
    return sram_enabled_ && header.has_sram && !serial_eeprom_cart() &&
           address >= header.sram_start && address <= header.sram_end;
}

// Several carts - the Acclaim sports titles in particular - carry a serial
// EEPROM at $200000 but declare nothing in the header. They prove it is there
// at boot by writing a bit and reading it back, and halt in a crash handler if
// the value does not survive.
//
// Shadowing ROM at that address on every cartridge would corrupt any game that
// genuinely reads data there, so the window stays transparent until something
// WRITES to it. A write into ROM space is meaningless on a cart without a
// device, which makes it a safe signal that one is present.
bool Genesis::in_hidden_sram(std::uint32_t address) const noexcept {
    // A cart that declares the device gets it straight away; one that hides it
    // has to write there first, so the window cannot shadow ROM until then.
    if (serial_eeprom_cart()) {
        const auto& header = cartridge_.header();
        return address >= header.sram_start && address <= header.sram_start + 1;
    }
    return hidden_sram_active_ && !cartridge_.header().has_sram &&
           address >= kHiddenSramBase && address <= kHiddenSramEnd;
}

void Genesis::note_hidden_sram_write(std::uint32_t address) noexcept {
    if (!cartridge_.header().has_sram &&
        address >= kHiddenSramBase && address <= kHiddenSramEnd) {
        hidden_sram_active_ = true;
    }
}

// The two cartridge windows the 68000 gains once the adapter is switched on:
// a fixed 512KB at $880000 and a banked megabyte at $900000, the bank chosen
// by the adapter's bank register. Returns -1 when the address is not one of
// them, which - crucially - is always the case while ADEN is clear, so a
// Mega Drive cartridge never sees these at all.
std::int64_t Genesis::thirtytwox_rom_offset(std::uint32_t address) const noexcept {
    if (!thirtytwox_->adapter().enabled()) {
        return -1;
    }
    // Both windows are views ONTO A CARTRIDGE. A disc system has none, so on a
    // CD they do not exist at all rather than reading an empty vector as zeros.
    if (cd_mode_) {
        return -1;
    }
    if (address >= 0x880000 && address <= 0x8FFFFF) {
        return static_cast<std::int64_t>(address - 0x880000);
    }
    if (address >= 0x900000 && address <= 0x9FFFFF) {
        const auto bank = static_cast<std::uint32_t>(thirtytwox_->adapter().rom_bank());
        return static_cast<std::int64_t>(bank * 0x100000u + (address - 0x900000));
    }
    return -1;
}

std::uint16_t Genesis::read16(std::uint32_t address) noexcept {
    address &= 0xFFFFFF;
    // The Sega CD owns the bottom 4MB and its gate array while a disc system
    // is booted. One boolean for everyone else.
    if (cd_mode_ && mercury::segacd::SegaCd::main_decodes(address)) {
        return segacd_->main_read16(address);
    }
    // With a 32X switched on, the bottom 256 bytes are the adapter's own
    // 68000 ROM rather than the cartridge. First test in the function because
    // it is also the reset vector fetch; it answers only for a cartridge that
    // declared itself a 32X, so no Mega Drive game can reach it.
    if (std::uint16_t overlay = 0; thirtytwox_->m68k_boot_overlay(address, overlay)) {
        return overlay;
    }
    // Only hardware registers are worth tallying; ROM fetches and RAM traffic
    // would bury the signal. 0x300000+ is above every normal cartridge and is
    // where an SVP cart puts its DSP RAM, so it is tallied too.
    if (trace_reads_ && ((address >= 0x200000 && address < 0x210000) ||
                         (address >= 0x300000 && address <= kRomEnd) ||
                         (address >= 0xA00000 && address < 0xE00000))) {
        ++read_tally_[address];
    }
    // The SVP cartridge answers above the normal ROM window - DSP DRAM at
    // 0x300000 and its cell-arranged mirrors at 0x390000/0x3A0000 - so this has
    // to be tested before the ROM case. present() is false for every other cart,
    // which is what keeps this from touching the rest of the library.
    if (svp_.present() && Svp::handles(address)) {
        return svp_.read16(address);
    }
    if (address <= kRomEnd) {
        if (in_sram(address)) {
            const auto offset = sram_offset(address);
            return static_cast<std::uint16_t>((sram_[offset] << 8) | sram_[(offset + 1) & 0xFFFF]);
        }
        if (in_hidden_sram(address)) {
            const auto byte = eeprom_.read_byte();
            return static_cast<std::uint16_t>((byte << 8) | byte);
        }
        return cartridge_.read16(map_rom(address));
    }
    if (thirtytwox_->decodes_frame_buffer(address)) {
        return thirtytwox_->frame_buffer_read16(address);
    }
    if (const auto window = thirtytwox_rom_offset(address); window >= 0) {
        return cartridge_.read16(static_cast<std::uint32_t>(window));
    }
    if (address >= kWorkRamBase) {
        const auto offset = address & 0xFFFE;
        return static_cast<std::uint16_t>((work_ram_[offset & 0xFFFF] << 8) |
                                          work_ram_[(offset + 1) & 0xFFFF]);
    }
    if (address >= kVdpBase && address <= kVdpEnd) {
        const auto port = address & 0x1F;
        // Which port a game watches to pace itself. A title that drifts against
        // the raster is not being told to wait by whatever it reads here.
        if (port < 0x04) { ++vdp_data_reads_; return vdp_.read_data(); }
        if (port < 0x08) { ++vdp_status_reads_; return vdp_.read_control(); }
        if (port < 0x0C) {
            // Which code polls the raster counter, and what it sees. A game with
            // no V-interrupt has to find vblank here; if it never gets the
            // answer it wants, nothing keeps it locked to the frame.
            ++vdp_hv_reads_;
            const auto pc = m68k_.registers().pc;
            for (auto& e : hv_reader_pcs_) {
                if (e.second == 0 || e.first == pc) { e.first = pc; ++e.second; break; }
            }
            return vdp_.read_hv_counter();
        }
        return 0xFFFF;
    }
    // The 32X adapter's register block. Always decoded, even with the
    // adapter inert, because reading it is how a cartridge detects one - and
    // it sits in a gap no Mega Drive hardware uses, so nothing else changes.
    if (mercury::thirtytwox::Adapter::decodes(address)) {
        // A communication port read is the 68000 asking the SH-2s a question.
        // Let them run up to this moment first, or the answer is always one
        // quantum stale and a tight poll loop never sees it change.
        if (address >= mercury::thirtytwox::Adapter::kCommFirst) {
            thirtytwox_catch_up();
        }
        return thirtytwox_->adapter().read16(address);
    }
    // "MARS" at $A130EC - the 32X's codename, and the FIRST thing a 32X
    // cartridge looks for. The 240p Test Suite compares this longword before
    // anything else and drops into a failure path when it does not match,
    // which is what it did here: the 68000 sat restarting every few frames
    // and never reached its real startup. Only answered while the adapter is
    // enabled, so a Mega Drive cartridge reading $A130EC sees what it always
    // saw.
    // The 32X video registers and palette, as the Mega Drive sees them.
    if (mercury::thirtytwox::ThirtyTwoX::decodes_video(address)) {
        return thirtytwox_->video_read16(address);
    }
    if (address >= 0xA130EC && address <= 0xA130EF) {
        // Readable whenever an adapter is attached, which for GenX is always.
        // NOT gated on the adapter being switched on: this signature is how a
        // cartridge FINDS the adapter, and it looks before it sets ADEN.
        // Gating it meant a cartridge could never discover the 32X it was
        // plugged into, so it never switched it on, so the signature never
        // appeared. It sits in a gap no Mega Drive hardware uses.
        return address < 0xA130EE ? 0x4D41u   // "MA"
                                  : 0x5253u;  // "RS"
    }
    if (address >= kZ80Base && address <= kZ80End) {
        // A word read of Z80 space returns the same byte in both halves - the
        // Z80's bus is only eight bits wide.
        const auto value = z80_read(static_cast<std::uint16_t>(address & 0x7FFF));
        return static_cast<std::uint16_t>((value << 8) | value);
    }
    if (address >= kIoBase && address <= kIoEnd) {
        const auto value = read_io(address);
        return static_cast<std::uint16_t>((value << 8) | value);
    }
    if (address >= kZ80BusReq && address < (kZ80BusReq + 0x100)) {
        // Bit 8 is BUSACK: zero once the 68000 has been granted the Z80's bus.
        // It follows the bus REQUEST only. Folding the reset line in here as
        // well reports "granted" while the Z80 merely sits in reset, and a game
        // that releases the bus and then waits for the bit to go high - which
        // is the normal way to hand the bus back - waits forever.
        //
        // Only that one bit is driven. The rest of the word is whatever was
        // last on the bus - in practice the 68000's own prefetch - and games
        // depend on it in incompatible ways: some TST the whole byte and need
        // it non-zero, others BTST bit 9 and need that clear. No constant
        // satisfies both, but the instruction stream does, because it is what
        // the hardware actually returns.
        //
        // AND A Z80 HELD IN RESET NEVER ACKNOWLEDGES. BUSACK is the Z80's own
        // output, and in reset it answers nothing: the bit stays high however
        // long the bus has been requested. (The opposite fold - "granted"
        // while in reset - is the one described above that hung Flintstones.)
        // The Terminator on Sega CD requests the bus with the Z80 in reset
        // and waits for the bit to read high; reporting "granted" held it on
        // the SEGA screen for ever. Genesis Plus GX: bit clear only when the
        // bus is requested and the Z80 is out of reset.
        const auto prefetch = cartridge_.read16(m68k_.registers().pc & 0xFFFFFF);
        const bool granted = z80_bus_requested_ && !z80_reset_asserted_;
        return static_cast<std::uint16_t>((prefetch & 0xFEFF) | (granted ? 0x0000 : 0x0100));
    }
    // Genesis Plus GX returns a random value here as a hack labelled "fix Time
    // Killers", and returning the 68000 prefetch - what the hardware actually
    // leaves on an undriven bus, and what the Z80 bus-request register above
    // already does - was tried as the principled version of it. Measured: Time
    // Killers never reads an unmapped address at all, so it fixes nothing here.
    // Left as the constant rather than change bus behaviour library-wide for no
    // demonstrated gain.
    return 0xFFFF;
}

void Genesis::write16(std::uint32_t address, std::uint16_t value) noexcept {
    address &= 0xFFFFFF;
    if (cd_mode_ && mercury::segacd::SegaCd::main_decodes(address)) {
        segacd_->main_write16(address, value);
        return;
    }
    // The adapter's boot page, which is mapped over the bottom of the map
    // while the 32X is on. Almost all of it swallows writes; the horizontal
    // interrupt vector is real storage. See m68k_boot_overlay_write.
    if (thirtytwox_->m68k_boot_overlay_write(address, static_cast<std::uint8_t>(value >> 8))) {
        thirtytwox_->m68k_boot_overlay_write(address + 1, static_cast<std::uint8_t>(value));
        return;
    }
    if (mercury::thirtytwox::Adapter::decodes(address)) {
        // Sync before the write, so the SH-2s see the port's old value for
        // exactly as long as they really would. A FIFO push is the same kind
        // of conversation: the SH-2 side should have drained what it could
        // before the 68000 decides whether there is room.
        //
        // THE COMMAND INTERRUPT REGISTER NEEDS THIS MOST OF ALL. The command
        // interrupt is a LEVEL, not a latch: it stands for exactly as long as
        // the 68000 leaves its bit set in $A15102. Without a sync here the
        // 68000 could set that bit and clear it again inside one quantum - up
        // to 64 of its cycles, and unbounded if nothing else forces a catch-up
        // - and the SH-2s, which only run between quanta, would never sample
        // the line while it was high. The interrupt simply vanished.
        // Star Wars Arcade's master took 2 interrupts in 900 frames because of
        // it and sat in its startup handshake for ever.
        if (address >= mercury::thirtytwox::Adapter::kCommFirst ||
            address == mercury::thirtytwox::Adapter::kDreqFifo ||
            (address & ~1u) == mercury::thirtytwox::Adapter::kInterruptControl) {
            thirtytwox_catch_up();
        }
        thirtytwox_->adapter().write16(address, value);
        return;
    }
    if (mercury::thirtytwox::ThirtyTwoX::decodes_video(address)) {
        thirtytwox_->video_write16(address, value);
        return;
    }
    if (thirtytwox_->decodes_frame_buffer(address)) {
        thirtytwox_->frame_buffer_write16(address, value);
        return;
    }
    if (address >= kWorkRamBase) {
        const auto offset = address & 0xFFFE;
        // MERCURY_WATCH_WRITE=<hex address>: remember which code writes a given
        // work-RAM word. The sprite list is built in work RAM, so this finds
        // the loop that decides how many sprites to emit.
        static const std::uint32_t watched = [] {
            const char* e = std::getenv("MERCURY_WATCH_WRITE");
            return e == nullptr ? 0xFFFFFFFFu
                                : static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
        }();
        if ((address & 0xFFFFFE) == (watched & 0xFFFFFE)) {
            watch_write_pc_ = m68k_.registers().pc;
            ++watch_write_count_;
            // Every writer, not just the last: a raster chain that stays in
            // phase must be re-anchored by something, and the anchor is
            // whichever code touches the step counter besides the chain itself.
            {
                const auto pc = m68k_.registers().pc;
                for (auto& e : watch_writer_pcs_) {
                    if (e.second == 0 || e.first == pc) { e.first = pc; ++e.second; break; }
                }
            }
        }
        work_ram_[offset & 0xFFFF] = static_cast<std::uint8_t>(value >> 8);
        work_ram_[(offset + 1) & 0xFFFF] = static_cast<std::uint8_t>(value & 0xFF);
        return;
    }
    if (svp_.present() && Svp::handles(address)) {
        svp_.write16(address, value);
        return;
    }
    if (address >= kVdpBase && address <= kVdpEnd) {
        const auto port = address & 0x1F;
        if (port < 0x04) { vdp_.write_data(value); return; }
        if (port < 0x08) {
            // Which code writes the command that targets the tachometer needle
            // tiles. The game repeats one address thousands of times, so the
            // pointer it should be advancing lives near this PC.
            // Which code triggers the DMA that fills the needle tiles. The
            // source address it programs is computed from the needle angle, so
            // that computation lives just before this PC.
            // MERCURY_CTRL_WATCH=<hex control word>: report which code writes
            // a given VDP command or register. Finding the instruction that
            // blanks the display is the whole question for a raster artefact.
            static const long ctrl_watch = [] {
                const char* e = std::getenv("MERCURY_CTRL_WATCH");
                return e == nullptr ? -1L : std::strtol(e, nullptr, 16);
            }();
            if (ctrl_watch >= 0 && value == static_cast<std::uint16_t>(ctrl_watch)) {
                watch_write_pc_ = m68k_.registers().pc;
                ++watch_write_count_;
                // Which scanline each blanking path fires on. Two different
                // routines blank this game and only their line tells them apart.
                {
                    const auto ln = vdp_.scanline();
                    if (ln < watch_lines_.size()) ++watch_lines_[ln];
                    const auto pc = m68k_.registers().pc;
                    for (auto& e : ctrl_writer_pcs_) {
                        if (e.second == 0 || e.first == pc) { e.first = pc; ++e.second; break; }
                    }
                }
                // The routine that blanks the display lives in work RAM, so its
                // own PC says nothing about who decided to call it. Walk the
                // stack for the return addresses that led here.
                // Only the writes that land mid-screen are interesting; the ones
                // during vblank are the game behaving itself.
                const auto line_now = vdp_.scanline();
                if (watch_stack_.empty() && line_now >= 80 && line_now < 246) {
                    const auto sp = m68k_.registers().a[7];
                    watch_stack_line_ = vdp_.scanline();
                    const auto peek = [this](std::uint32_t a) {
                        return static_cast<std::uint32_t>(work_ram_[a & 0xFFFF]);
                    };
                    for (std::uint32_t i = 0; i < 12; ++i) {
                        const auto at = sp + i * 2;
                        watch_stack_.push_back((peek(at) << 24) | (peek(at + 1) << 16) |
                                               (peek(at + 2) << 8) | peek(at + 3));
                    }
                }
            }
            const auto before = vdp_.d_dma_count();
            const auto pc_here = m68k_.registers().pc;
            vdp_.write_control(value);
            if (vdp_.d_dma_count() != before) {
                needle_cmd_pc_ = pc_here;
                ++needle_cmd_count_;
            }
            return;
        }
        if (port >= 0x10 && port < 0x18) {
            // The PSG lives behind the VDP's address range on the 68000 side.
            psg_.write(static_cast<std::uint8_t>(value & 0xFF));
            return;
        }
        return;
    }
    if (address >= kZ80Base && address <= kZ80End) {
        z80_write(static_cast<std::uint16_t>(address & 0x7FFF), static_cast<std::uint8_t>(value >> 8));
        return;
    }
    if (address >= kIoBase && address <= kIoEnd) {
        write_io(address, static_cast<std::uint8_t>(value & 0xFF));
        return;
    }
    if (address >= kZ80BusReq && address < (kZ80BusReq + 0x100)) {
        z80_bus_requested_ = (value & 0x0100) != 0;
        return;
    }
    if (address >= kZ80Reset && address < (kZ80Reset + 0x100)) {
        const bool released = (value & 0x0100) != 0;
        if (released && z80_reset_asserted_) {
            z80_.reset();
            z80_.set_stack_pointer(0x0000);
        }
        z80_reset_asserted_ = !released;
        return;
    }
    if (address <= kRomEnd) {
        if (in_sram(address)) {
            const auto offset = sram_offset(address);
            sram_[offset] = static_cast<std::uint8_t>(value >> 8);
            sram_[(offset + 1) & 0xFFFF] = static_cast<std::uint8_t>(value & 0xFF);
            battery_dirty_ = true;
        } else {
            note_hidden_sram_write(address);
            if (in_hidden_sram(address)) {
                eeprom_.write_byte(static_cast<std::uint8_t>(value & 0xFF));
                battery_dirty_ = true;
            }
        }
        return;  // ROM is not writable
    }
    if (address == 0xA130F1) {
        sram_enabled_ = (value & 0x0001) != 0;
        return;
    }
}

std::uint8_t Genesis::read8(std::uint32_t address) noexcept {
    address &= 0xFFFFFF;
    if (cd_mode_ && mercury::segacd::SegaCd::main_decodes(address)) {
        return segacd_->main_read8(address);
    }
    // Same 68000 boot page as the word path - see read16.
    if (std::uint16_t overlay = 0; thirtytwox_->m68k_boot_overlay(address, overlay)) {
        return static_cast<std::uint8_t>((address & 1) != 0 ? (overlay & 0xFF) : (overlay >> 8));
    }
    if (trace_reads_ && ((address >= 0x200000 && address < 0x210000) ||
                         (address >= 0xA00000 && address < 0xE00000))) {
        ++read_tally_[address];
    }
    if (mercury::thirtytwox::Adapter::decodes(address)) {
        if (address >= mercury::thirtytwox::Adapter::kCommFirst) {
            thirtytwox_catch_up();  // see the word path
        }
        return thirtytwox_->adapter().read8(address);
    }
    if (mercury::thirtytwox::ThirtyTwoX::decodes_video(address)) {
        return thirtytwox_->video_read8(address);
    }
    if (address >= 0xA130EC && address <= 0xA130EF) {
        // See the word path: not gated, because this is the detection route.
        static constexpr char kSignature[] = "MARS";
        return static_cast<std::uint8_t>(kSignature[address - 0xA130EC]);
    }
    if (address >= kZ80Base && address <= kZ80End) {
        return z80_read(static_cast<std::uint16_t>(address & 0x7FFF));
    }
    if (address >= kIoBase && address <= kIoEnd) {
        return read_io(address);
    }
    if (svp_.present() && Svp::handles(address)) {
        const auto word = svp_.read16(address & ~std::uint32_t{1});
        return static_cast<std::uint8_t>((address & 1) ? (word & 0xFF) : (word >> 8));
    }
    if (thirtytwox_->decodes_frame_buffer(address)) {
        return thirtytwox_->frame_buffer_read8(address);
    }
    if (const auto window = thirtytwox_rom_offset(address & ~std::uint32_t{1}); window >= 0) {
        const auto word = cartridge_.read16(static_cast<std::uint32_t>(window));
        return static_cast<std::uint8_t>((address & 1) ? (word & 0xFF) : (word >> 8));
    }
    if (address >= kWorkRamBase) {
        return work_ram_[address & 0xFFFF];
    }
    if (address <= kRomEnd) {
        // Byte access to SRAM has to be handled here rather than folded into a
        // word read: most carts map battery RAM on odd bytes only, and a window
        // can be a single byte wide, so the containing word is often not in the
        // SRAM range even when the byte is.
        if (in_sram(address)) {
            return sram_[sram_offset(address)];
        }
        if (in_hidden_sram(address)) {
            return eeprom_.read_byte();
        }
        return cartridge_.read8(map_rom(address));
    }
    const auto word = read16(address & ~std::uint32_t{1});
    return static_cast<std::uint8_t>((address & 1) ? (word & 0xFF) : (word >> 8));
}

void Genesis::write8(std::uint32_t address, std::uint8_t value) noexcept {
    address &= 0xFFFFFF;
    if (cd_mode_ && mercury::segacd::SegaCd::main_decodes(address)) {
        segacd_->main_write8(address, value);
        return;
    }
    if (thirtytwox_->m68k_boot_overlay_write(address, value)) {
        return;  // see the word path
    }
    if (mercury::thirtytwox::Adapter::decodes(address)) {
        if (address >= mercury::thirtytwox::Adapter::kCommFirst ||
            (address & ~1u) == mercury::thirtytwox::Adapter::kDreqFifo ||
            (address & ~1u) == mercury::thirtytwox::Adapter::kInterruptControl) {
            thirtytwox_catch_up();  // see the word path, including the level note
        }
        thirtytwox_->adapter().write8(address, value);
        return;
    }
    if (mercury::thirtytwox::ThirtyTwoX::decodes_video(address)) {
        thirtytwox_->video_write8(address, value);
        return;
    }
    if (thirtytwox_->decodes_frame_buffer(address)) {
        thirtytwox_->frame_buffer_write8(address, value);
        return;
    }
    if (address >= kWorkRamBase) {
        work_ram_[address & 0xFFFF] = value;
        return;
    }
    if (address >= kZ80Base && address <= kZ80End) {
        z80_write(static_cast<std::uint16_t>(address & 0x7FFF), value);
        return;
    }
    if (address >= kIoBase && address <= kIoEnd) {
        write_io(address, value);
        return;
    }
    if (svp_.present() && Svp::handles(address)) {
        // A byte write lands in the matching half of the word; the DSP's
        // registers are only ever written as words in practice.
        const auto aligned = address & ~std::uint32_t{1};
        const auto word = svp_.read16(aligned);
        svp_.write16(aligned, (address & 1) != 0
                                  ? static_cast<std::uint16_t>((word & 0xFF00) | value)
                                  : static_cast<std::uint16_t>((word & 0x00FF) | (value << 8)));
        return;
    }
    if (address == 0xA130F1) {
        sram_enabled_ = (value & 0x01) != 0;
        return;
    }
    if (address >= 0xA130F3 && address <= 0xA130FF && (address & 1) != 0) {
        const auto slot = static_cast<std::uint32_t>((address - 0xA130F1) >> 1) & 0x07;
        if (slot != 0) {
            rom_banks_[slot] = static_cast<std::uint32_t>(value) * 0x80000u;
        }
        return;
    }
    if (address >= kVdpBase && address <= kVdpEnd) {
        const auto port = address & 0x1F;
        if (port >= 0x10 && port < 0x18) {
            psg_.write(value);
            return;
        }
        // A byte write to a 16-bit VDP port duplicates the byte into both
        // halves; software relies on this for the register writes it does with
        // MOVE.b.
        write16(address & ~std::uint32_t{1}, static_cast<std::uint16_t>((value << 8) | value));
        return;
    }
    if (address >= kZ80BusReq && address < (kZ80BusReq + 0x100)) {
        z80_bus_requested_ = (value & 0x01) != 0;
        return;
    }
    if (address >= kZ80Reset && address < (kZ80Reset + 0x100)) {
        const bool released = (value & 0x01) != 0;
        if (released && z80_reset_asserted_) {
            z80_.reset();
            z80_.set_stack_pointer(0x0000);
        }
        z80_reset_asserted_ = !released;
        return;
    }
    if (address <= kRomEnd) {
        if (in_sram(address)) {
            sram_[sram_offset(address)] = value;
            battery_dirty_ = true;
            return;
        }
        note_hidden_sram_write(address);
        if (in_hidden_sram(address)) {
            eeprom_.write_byte(value);
            battery_dirty_ = true;
            return;
        }
        // Nothing claimed it. Record where, so a game whose save silently
        // vanishes points at the window it actually wanted.
        if (trace_reads_) {
            ++dropped_rom_writes_[address];
        }
    }
}

// --- I/O -------------------------------------------------------------------

void Genesis::note_io(std::uint32_t reg, std::uint8_t value, bool write) noexcept {
    const auto slot = static_cast<std::uint8_t>(reg >> 1);
    if (write) {
        ++io_activity_.writes[slot];
        io_activity_.last_write[slot] = value;
    } else {
        ++io_activity_.reads[slot];
    }
    if (!io_trace_enabled_ || io_trace_.size() >= io_trace_limit_) {
        return;
    }
    // Frame and scanline as the timebase. A scanline is a few hundred 68000
    // cycles, which is coarse for a bit-banged line - but the ORDER of the
    // accesses is exact, and that is what says which pin is the clock and how
    // many bits go by before the game looks for an answer.
    io_trace_.push_back(IoTraceEntry{
        static_cast<std::uint64_t>(vdp_.frames_elapsed()) * 1000u + vdp_.scanline(),
        slot,
        value,
        write,
    });
}

const char* Genesis::io_register_name(std::size_t slot) noexcept {
    // The Mega Drive's I/O chip: a version register, then three controller
    // ports of data and control, then a UART for each of those three ports.
    static const char* kNames[16] = {
        "version",  "data1",    "data2",    "data3",
        "ctrl1",    "ctrl2",    "ctrl3",    "txdata1",
        "rxdata1",  "sctrl1",   "txdata2",  "rxdata2",
        "sctrl2",   "txdata3",  "rxdata3",  "sctrl3",
    };
    return slot < 16 ? kNames[slot] : "?";
}

std::uint8_t Genesis::read_io(std::uint32_t address) noexcept {
    // The I/O chip sits on the lower half of the data bus and does not decode
    // A0, so the even address and the odd one are the SAME register: $A10002
    // reads the port-1 data register just as $A10003 does. Matching only the
    // odd address makes every even access return zero, and a controller that
    // reads as zero is a controller with every button held down. Awesome Possum
    // reads port 1 as a word from $A10002, sees A+B+C+Start, takes that as its
    // soft-reset combo and restarts itself forever.
    const auto reg = (address & 0x1F) | 1;
    std::uint8_t value = 0x00;
    switch (reg) {
        case 0x01:
            // Version register: overseas + NTSC/PAL + "no Mega CD attached" +
            // hardware revision zero. Region code here has to agree with the
            // timing actually being run, or games refuse to start.
            value = static_cast<std::uint8_t>(0x80 | (pal_ ? 0x40 : 0x00) | 0x20);
            break;
        case 0x03: value = port1_.read_data(); break;
        case 0x05: value = port2_.read_data(); break;
        case 0x07: value = ext_.read_data(); break;
        case 0x09: value = port1_.control(); break;
        case 0x0B: value = port2_.control(); break;
        case 0x0D: value = ext_.control(); break;
        default: break;
    }
    note_io(reg, value, false);
    return value;
}

void Genesis::write_io(std::uint32_t address, std::uint8_t value) noexcept {
    // A0 is not decoded on writes either - see read_io.
    const auto reg = (address & 0x1F) | 1;
    note_io(reg, value, true);
    switch (reg) {
        case 0x03: port1_.write_data(value); return;
        case 0x05: port2_.write_data(value); return;
        case 0x07: ext_.write_data(value); return;
        case 0x09: port1_.write_control(value); return;
        case 0x0B: port2_.write_control(value); return;
        case 0x0D: ext_.write_control(value); return;
        default: return;
    }
}

// --- Z80 address space -----------------------------------------------------

std::uint8_t Genesis::z80_read(std::uint16_t address) noexcept {
    if (address < 0x4000) {
        return z80_ram_[address & 0x1FFF];
    }
    if (address < 0x6000) {
        // Only the YM2612 status is readable, and it answers at every address
        // in the block.
        return ym2612_.read_status();
    }
    if (address < 0x8000) {
        return 0xFF;
    }
    // The 32KB window into 68000 space. The bank register supplies the top nine
    // address bits, so the window can reach anywhere in the map - including the
    // VDP, which is how some drivers stream samples.
    const std::uint32_t target = (z80_bank_ << 15) | (address & 0x7FFF);
    return read8(target);
}

void Genesis::z80_write(std::uint16_t address, std::uint8_t value) noexcept {
    if (address < 0x4000) {
        z80_ram_[address & 0x1FFF] = value;
        return;
    }
    if (address < 0x6000) {
        ym2612_.write(static_cast<std::uint8_t>(address & 0x03), value);
        return;
    }
    if (address < 0x6100) {
        // Bank register: one bit per write, shifted in from the top. Nine
        // writes load the full bank.
        z80_bank_ = ((z80_bank_ >> 1) | ((value & 0x01) << 8)) & 0x1FF;
        return;
    }
    if (address >= 0x7F00 && address < 0x8000) {
        if ((address & 0xFF) == 0x11) {
            psg_.write(value);
        }
        return;
    }
    if (address >= 0x8000) {
        const std::uint32_t target = (z80_bank_ << 15) | (address & 0x7FFF);
        write8(target, value);
    }
}

// --- execution -------------------------------------------------------------

mercury::core::M68kStepResult Genesis::step() noexcept {
    m68k_.set_interrupt_level(vdp_.interrupt_level());
    auto result = m68k_.step();
    if (m68k_trace_) {
        m68k_trace_(result, m68k_.registers());
    }
    if (trace_reads_) {
        ++pc_tally_[result.pc_before];
    }
    if (result.unimplemented) {
        ++m68k_unimplemented_count_;
        ++m68k_unimplemented_opcodes_[result.opcode];
    }
    if (result.cycles == 0) {
        result.cycles = 4;  // never stall the clock on an unimplemented opcode
    }
    // A DMA started by this instruction holds the bus for the length of the
    // transfer. Charging it to the instruction stalls the 68000 for the right
    // amount of time and advances the video chip with it, which is what makes a
    // game's vblank budget come out the same as on hardware.
    const auto dma_stall_cycles = vdp_.take_pending_dma_cycles();
    result.cycles += dma_stall_cycles;
    // Level 2 belongs here as much as 6 and 4 do. Leaving it out meant a light
    // gun's external interrupt was raised and never retired, so the CPU took it
    // again on the very next instruction and the game stopped making progress -
    // T2 froze the instant a Menacer was selected. An interrupt this emulator
    // can raise must be an interrupt it can also acknowledge.
    // Counted outside the acknowledge, deliberately: these record what the CPU
    // DID, and folding them into the branch that retires the request makes them
    // blind to exactly the failure they are useful for - an interrupt taken
    // over and over because nothing ever retires it.
    if (result.interrupt_serviced == 6) {
        ++m68k_vint_count_;
    } else if (result.interrupt_serviced == 4) {
        ++m68k_hint_count_;
    } else if (result.interrupt_serviced == 2) {
        ++m68k_ext_count_;
    }
    if (result.interrupt_serviced == 6 || result.interrupt_serviced == 4 ||
        result.interrupt_serviced == 2) {
        // The CPU does not name the interrupt it is acknowledging; the VDP
        // retires whichever it is raising most urgently at that instant. That
        // can differ from the level the CPU accepted, and Fatal Rewind depends
        // on the difference.
        vdp_.acknowledge_interrupt();
    }

    // The Z80 runs from the same master clock divided by fifteen instead of
    // seven, so it gets 7/15 of the 68000's cycles. Carrying the remainder in
    // z80_cycle_debt_ keeps the ratio exact over a frame.
    if (!z80_reset_asserted_ && !z80_bus_requested_) {
        z80_cycle_debt_ += static_cast<std::int64_t>(result.cycles) * 7;
        while (z80_cycle_debt_ >= 15) {
            z80_.set_maskable_interrupt_line(z80_interrupt_cycles_ > 0);
            const auto z80_result = z80_.step();
            const auto spent = z80_result.cycles == 0 ? 4u : z80_result.cycles;
            z80_cycle_debt_ -= static_cast<std::int64_t>(spent) * 15;
            if (z80_interrupt_cycles_ > 0) {
                z80_interrupt_cycles_ -= static_cast<std::int32_t>(spent);
                if (z80_interrupt_cycles_ <= 0) {
                    z80_.set_maskable_interrupt_line(false);
                }
            }
            if (z80_result.cycles == 0) {
                // An unimplemented opcode would otherwise wedge the sound CPU
                // silently for the rest of the run. Count it and step past so
                // the failure is visible and recoverable.
                ++z80_unimplemented_count_;
                z80_last_bad_pc_ = z80_result.pc_before;
                break;
            }
            ++z80_instructions_;
        }
    }

    // The vertical interrupt is wired to both processors. Catching the edge
    // here rather than inside the VDP keeps the VDP unaware of the Z80.
    // active_scanlines(), not screen_height(): in interlace mode 2 the latter is
    // the woven framebuffer's height and twice the number of lines the raster
    // actually draws, which would put vblank off the end of the frame.
    const bool vblank_now = vdp_.scanline() >= vdp_.active_scanlines();
    if (vblank_now && !z80_in_vblank_) {
        // Held for about one scanline, which is 228 Z80 cycles.
        z80_interrupt_cycles_ = 228;
        ++z80_interrupts_;
    }
    z80_in_vblank_ = vblank_now;

    // The Sega CD's sub CPU runs to the same moment after every main-CPU
    // instruction, so a flag or command word one side writes is visible to
    // the other on its very next instruction. The two talk almost entirely by
    // polling those registers; any coarser interleave makes every handshake
    // wait out the slice.
    if (cd_mode_) {
        segacd_->advance(result.cycles, pal_ ? kM68kClockPal : kM68kClockNtsc);
    }

    ym2612_.advance_cycles(result.cycles);
    // The PSG's own clock is the Z80's, so it is fed in that domain.
    // The PSG runs in the Z80's clock domain, seven fifteenths of the 68000's.
    // That ratio has to be carried as a remainder, not truncated per call: an
    // instruction of four cycles truncates to one Z80 cycle instead of 1.87,
    // which runs the chip about fifteen percent slow. The audible result is not
    // a pitch error - it is that the PSG produces fewer samples per frame than
    // the FM does, so the tail of every frame has no PSG in it and every PSG
    // voice gets gated at 60Hz.
    psg_cycle_debt_ += static_cast<std::int64_t>(result.cycles) * 7;
    const auto psg_cycles = static_cast<std::uint32_t>(psg_cycle_debt_ / 15);
    psg_cycle_debt_ %= 15;
    if (psg_cycles != 0) {
        psg_.advance_cycles(psg_cycles);
    }
    // The DSP on an SVP cartridge runs at 23MHz against the 68000's 7.67MHz, so
    // three DSP cycles per 68000 cycle. Charging one cycle per instruction is a
    // simplification, but the game synchronises through a handshake word in DSP
    // RAM rather than by counting, so what matters is that the DSP keeps up.
    if (svp_.present()) {
        // 800 DSP cycles per scanline, and a scanline is 488 68000 cycles - the
        // rate Genesis Plus GX runs it at. The chip's nominal 23MHz against the
        // 68000's 7.67MHz suggests three to one, but running it that fast lets
        // the DSP get through "clear the tile buffer, render into it" more than
        // once between the 68000's reads, so the 68000 can DMA a buffer the DSP
        // has already blanked again.
        // 800 is what Genesis Plus GX uses (PicoDrive uses 850). Both were
        // measured here along with 1100, 1500 and 2000: 800 delivers the most
        // tile data to the 68000 of any of them, so this is not a value to
        // fiddle with hoping for more.
        // The real ratio: 23.01 MHz against the 68000's 7.67 MHz is 3 to 1, so
        // three DSP cycles per 68000 cycle, 1464 per scanline.
        //
        // This was 800 for a while, chosen by measurement back when a render
        // cost 1.2M DSP instructions and the DSP could outrun the tile
        // transfer. With the 32-bit ALU fixed a render costs 274K and the halt
        // register is honoured, so the DSP can be clocked properly again. 800,
        // 1464 and 3200 were re-measured across four points in the game and
        // came out within 0.13% of each other - the DSP is no longer the
        // constraint at any of them, so this is now just the accurate value.
        constexpr std::int64_t kSvpCyclesPerLine = 1464;

        constexpr std::int64_t kM68kCyclesPerLine = 488;
        // DMA stall time is INCLUDED. Excluding it - on the theory that the DSP
        // was clearing the tile buffer out from under a transfer - was tried and
        // made things worse: the 68000's non-zero DRAM reads fell from 428,554
        // to 350,948 over the same run. The DSP is short of cycles here, not
        // running ahead of the transfer.
        svp_cycle_debt_ += static_cast<std::int64_t>(result.cycles) * kSvpCyclesPerLine;
        (void)dma_stall_cycles;
        const auto dsp_cycles = svp_cycle_debt_ / kM68kCyclesPerLine;
        svp_cycle_debt_ %= kM68kCyclesPerLine;
        if (dsp_cycles > 0) {
            svp_.run(static_cast<int>(dsp_cycles));
        }
    }
    // DIAGNOSTIC MERCURY_M68K_SCALE=<percent>: scale how much time the rest of
    // the machine thinks an instruction took. A raster chain that drops
    // interrupts because its handler overruns will stop dropping them if the
    // CPU is actually faster than this emulator is charging it for.
    static const long cpu_scale = [] {
        const char* e = std::getenv("MERCURY_M68K_SCALE");
        return e == nullptr ? 100L : std::strtol(e, nullptr, 10);
    }();
    if (cpu_scale != 100) {
        result.cycles = static_cast<int>((static_cast<long>(result.cycles) * cpu_scale) / 100);
    }
    vdp_.advance_cycles(result.cycles);
    service_light_guns();
    // Clock the interrupt-enable pipeline once per instruction, after the
    // instruction that may have written the enable bits has completed.
    vdp_.commit_interrupt_enables();
    return result;
}

// A light gun sees the beam once a frame: when the raster reaches the line it
// is pointed at. That is the whole mechanism - the gun has no idea where it is
// aimed, it only knows WHEN it saw light, and the game works the position out
// from the HV counter it latched at that instant.
//
// Checked once per scanline rather than per instruction, which is both cheaper
// and closer to the truth: the photodiode fires once as the beam sweeps past.
void Genesis::service_light_guns() noexcept {
    const auto line = vdp_.scanline();
    if (line == light_gun_line_) {
        return;
    }
    light_gun_line_ = line;
    bool saw_beam = false;
    for (auto* port : {&port1_, &port2_}) {
        // Only the gun the console currently has SELECTED can report anything.
        // A Justifier's two guns share one signal path, so the one that is not
        // addressed is not looking at the screen as far as the console can
        // tell - which is how a game reads two positions from one port.
        if (!ControllerPort::is_light_gun(port->kind()) || !port->selected_on_screen()) {
            continue;
        }
        if (port->selected_aim_y() != line || line >= vdp_.active_scanlines()) {
            continue;
        }
        saw_beam = true;
        vdp_.latch_light_gun(port->light_gun_h(vdp_.wide_mode()), line);
    }
    if (!saw_beam) {
        // The gun freezes the HV counter on the line it fires whether or not the
        // game asked for a latch, so every other line has to hand the counter
        // back. Leaving it frozen randomises the raster for anything that polls
        // it - which is the bug this exact release fixes in Gunfight 3 in 1
        // upstream.
        vdp_.release_light_gun_latch();
    }
}

std::uint32_t Genesis::run_frame() noexcept {
    const auto start_frame = vdp_.frames_elapsed();
    std::uint32_t cycles = 0;
    // A frame is about 128,000 cycles; the cap is a safety net for a ROM that
    // wedges the VDP rather than a real limit.
    constexpr std::uint32_t kCycleCeiling = 4'000'000;
    while (vdp_.frames_elapsed() == start_frame && cycles < kCycleCeiling) {
        const auto result = step();
        const auto spent = result.cycles == 0 ? 4 : result.cycles;
        cycles += spent;
        // The 32X runs alongside rather than afterwards. See
        // thirtytwox_catch_up - at frame granularity the startup handshake
        // between the two sides can never complete.
        thirtytwox_quantum_ += spent;
        if (thirtytwox_quantum_ >= kThirtyTwoXQuantum) {
            thirtytwox_catch_up();
        }
    }
    // Six-button pads reset their bank counter once a frame goes by without a
    // strobe; doing it here keeps the timeout out of the hot path.
    port1_.tick_timeout();
    port2_.tick_timeout();
    ext_.tick_timeout();

    // The 32X, once a cartridge has switched it on. A frame of SH-2 time is
    // 23.01MHz over roughly 60Hz. Run() returns immediately while the adapter
    // is inert, which is the path every Mega Drive game takes.
    //
    // Once a frame rather than interleaved with the 68000: the two sides talk
    // through a communication port and a frame buffer rather than by racing
    // for the same bus, so this is nothing like the Gear Link's
    // instruction-by-instruction interleave. If a game turns out to need
    // finer granularity, THAT is the moment to pay for it.
    // Pay off whatever is left of the frame, then combine the two pictures.
    thirtytwox_catch_up();
    composite_thirtytwox();
    if (cd_mode_) {
        segacd_->end_frame();
    }
    return cycles;
}

void Genesis::thirtytwox_catch_up() noexcept {
    if (!thirtytwox_->active()) {
        // The path every Mega Drive game takes, thousands of times a frame.
        // It has to cost one pointer chase and a comparison.
        thirtytwox_quantum_ = 0;
        thirtytwox_owed_ = 0;
        return;
    }
    // The vertical blank edge, taken from the real beam rather than from a
    // fraction of the frame. The 32X swaps its frame buffer banks here, and
    // its games wait on the interrupt this raises.
    if (const bool blanking = vdp_.in_vblank(); blanking != thirtytwox_vblank_) {
        thirtytwox_vblank_ = blanking;
        if (blanking) {
            thirtytwox_->start_vblank();
        } else {
            thirtytwox_->end_vblank();
        }
    }
    // Scanline edges, for the 32X's own horizontal interrupt. Taken from the
    // real beam like the blanking edge above, rather than counted separately.
    //
    // EVERY LINE CROSSED, NOT JUST THE NEWEST ONE. This used to report a
    // single edge however far the beam had moved, and the 68000 routinely runs
    // past more than one line between two catch-ups - a long instruction, or a
    // stretch with no communication-port access to force a sync. The 32X
    // counts its horizontal interrupt in lines, so every line dropped here is
    // an interrupt the cartridge never gets: Blackthorne asks for one a line
    // and was given 159 of the 224 it should see, which left its master on a
    // different path through its own state machine than ares takes.
    if (const auto line = vdp_.scanline(); line != thirtytwox_last_line_) {
        const int total = static_cast<int>(vdp_.lines_per_frame());
        const int active = static_cast<int>(vdp_.active_scanlines());
        int crossed = static_cast<int>(line) - static_cast<int>(thirtytwox_last_line_);
        if (crossed < 0) {
            crossed += total;  // wrapped round the end of the frame
        }
        for (int step = 1; step <= crossed; ++step) {
            const int at = (static_cast<int>(thirtytwox_last_line_) + step) % total;
            thirtytwox_->scanline(at < active);
        }
        thirtytwox_last_line_ = line;
    }
    thirtytwox_owed_ += static_cast<std::int64_t>(thirtytwox_quantum_) * kSh2CyclesPer68k;
    thirtytwox_quantum_ = 0;
    if (thirtytwox_owed_ <= 0) {
        return;
    }
    // An instruction is not interruptible, so a slice can overshoot. Carrying
    // the difference keeps the two clocks from drifting apart over a frame.
    const auto spent = thirtytwox_->run(static_cast<std::uint32_t>(thirtytwox_owed_));
    thirtytwox_owed_ -= static_cast<std::int64_t>(spent);
}

void Genesis::composite_thirtytwox() noexcept {
    if (!thirtytwox_->active() || !thirtytwox_->vdp().produces_picture()) {
        return;
    }
    auto& rgb = vdp_.frame_pixels_for_overlay();
    const auto& backdrop = vdp_.frame_backdrop();
    const auto width = static_cast<int>(vdp_.screen_width());
    const auto height = static_cast<int>(vdp_.screen_height());
    if (rgb.size() < static_cast<std::size_t>(width) * height * 3) {
        return;
    }
    // The 32X layer is always 320 pixels across. A Mega Drive in 32-cell mode
    // is 256, and the two are combined one pixel to one pixel here, so the
    // right quarter of the 32X picture is lost in that mode. Real hardware
    // does not narrow the 32X layer either, and no 32X game runs its Mega
    // Drive side in 32-cell mode, so this only matters if one ever does.
    const int overlap = width < mercury::thirtytwox::Vdp::kWidth
                            ? width
                            : mercury::thirtytwox::Vdp::kWidth;
    std::array<mercury::thirtytwox::Vdp::Pixel, mercury::thirtytwox::Vdp::kWidth> line{};
    const bool have_backdrop =
        backdrop.size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (int y = 0; y < height; ++y) {
        thirtytwox_->vdp().render_line(y, line.data());
        mercury::thirtytwox::Vdp::composite_line(
            line.data(), rgb.data() + static_cast<std::size_t>(y) * width * 3,
            have_backdrop ? backdrop.data() + static_cast<std::size_t>(y) * width : nullptr,
            overlap);
    }
}

// --- reporting -------------------------------------------------------------

mercury::core::ModuleStatuses Genesis::module_statuses() const {
    using mercury::core::ModuleState;
    return {
        {"68000 CPU", ModuleState::validated,
         "653,225 of 653,265 SingleStepTests cases; every family bar four at 8065/8065"},
        {"Z80 sub-CPU", ModuleState::validated, "shared core with the Game Gear, zexdoc-clean"},
        {"VDP (Mode 5)", ModuleState::in_progress,
         "planes, window, sprites, DMA and both interrupt sources; no shadow/highlight yet"},
        {"YM2612", ModuleState::in_progress, "six FM channels, DAC and both timers"},
        {"SN76489 PSG", ModuleState::validated, "shared with the Game Gear"},
        {"Controllers", ModuleState::in_progress, "three- and six-button pads on both ports"},
        {"Cartridge", ModuleState::validated, ".bin/.md/.gen and .smd de-interleave, SRAM window"},
    };
}

namespace {
// Bumped whenever visit_state changes shape, so an old file is refused rather
// than read as though its fields still lined up.
constexpr std::uint32_t kStateMagic = 0x58'4E'45'47;  // "GENX"
// 2: the PSG changed from phase accumulators to hardware counters, so its
// channel structs are a different shape and a version 1 state would be read
// with every field one slot out.
// 3: the FM operator gained its SSG-EG inversion flag.
// 4: the VDP records whether an HV latch was taken by a light gun rather than
// by the game, because the two are released at different times.
// 5: the SVP, the 32X and the Sega CD, and a header that names the disc and
// firmware (Genesis::kStateVersion). A version 4 state still loads into a
// plain Mega Drive cartridge - it is exactly a version 5 state with no extra
// blocks - so nobody's existing saves stop working.
// 6: the VDP's raster samples (whether the current line is drawn, and the
// full-screen vertical scroll latched for the next), appended at the very end
// so versions 4 and 5 still load; those rebuild the two from the registers.
constexpr std::uint32_t kOldestLoadableVersion = 4;

std::uint64_t fold64(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        hash ^= static_cast<std::uint8_t>(value >> (i * 8));
        hash *= 1099511628211ull;
    }
    return hash;
}
}  // namespace

std::uint8_t Genesis::state_media() const noexcept {
    std::uint8_t media = 0;
    if (cd_mode_) {
        media |= kMediaCd;
    }
    // A 32X cartridge always, and anything else once it has switched the
    // adapter on - which is how a Sega CD 32X disc looks.
    if (thirtytwox_->startup().valid || thirtytwox_->adapter().enabled()) {
        media |= kMedia32x;
    }
    if (svp_.present()) {
        media |= kMediaSvp;
    }
    return media;
}

std::uint64_t Genesis::media_id() const noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    hash = fold64(hash, cd_mode_ ? 1 : 0);
    if (cd_mode_) {
        hash = fold64(hash, segacd_->disc_id());
        hash = fold64(hash, segacd_->bios_id());
    }
    if (thirtytwox_->startup().valid) {
        hash = fold64(hash, thirtytwox_->firmware_id());
    }
    return hash;
}

bool Genesis::battery_is_eeprom() const noexcept {
    return serial_eeprom_cart();
}

bool Genesis::battery_backed() const noexcept {
    // The Sega CD's internal 8KB, which every disc saves into.
    if (cd_mode_) {
        return true;
    }
    // The header's own flag, the quirk table's forced window for the carts whose
    // headers lie about it, and "the game has actually driven the EEPROM" - a
    // cart can carry one without the header admitting it.
    return cartridge_.header().has_sram || forced_sram_ || eeprom_.ever_used();
}

std::vector<std::uint8_t> Genesis::battery_snapshot() const {
    if (cd_mode_) {
        return segacd_->backup_ram();
    }
    if (battery_is_eeprom()) {
        const auto& storage = eeprom_.storage();
        return std::vector<std::uint8_t>(storage.begin(), storage.end());
    }
    return sram_;
}

void Genesis::restore_battery(const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    if (cd_mode_) {
        segacd_->restore_backup_ram(bytes);
        return;
    }
    if (battery_is_eeprom()) {
        eeprom_.restore_storage(bytes.data(), bytes.size());
    } else {
        const auto count = bytes.size() < sram_.size() ? bytes.size() : sram_.size();
        std::copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count), sram_.begin());
    }
    // Loading is not a change the host needs to write back out.
    battery_dirty_ = false;
}

std::vector<std::uint8_t> Genesis::save_state_blob() const {
    std::vector<std::uint8_t> blob;
    mercury::core::StateWriter writer(blob);
    writer(kStateMagic);
    writer(kStateVersion);
    // Identity of the cartridge this state belongs to. Loading a state onto a
    // different game would restore RAM and registers that mean nothing there.
    std::uint16_t checksum = cartridge_.header().checksum;
    std::uint32_t rom_size = static_cast<std::uint32_t>(cartridge_.rom().size());
    writer(checksum);
    writer(rom_size);
    // What else is in the machine, and which disc and firmware it is running.
    // Separate fields rather than media_id() so a refusal can say which one
    // is different.
    std::uint8_t media = state_media();
    std::uint64_t disc = cd_mode_ ? segacd_->disc_id() : 0;
    std::uint64_t cd_bios = cd_mode_ ? segacd_->bios_id() : 0;
    std::uint64_t x32_firmware = (media & kMedia32x) != 0 ? thirtytwox_->firmware_id() : 0;
    writer(media);
    writer(disc);
    writer(cd_bios);
    writer(x32_firmware);
    // visit_state is not const - it is the same function for both directions -
    // and saving genuinely does not modify anything it touches.
    const_cast<Genesis*>(this)->visit_state(writer, kStateVersion, media);
    return blob;
}

void Genesis::sanitize_loaded() noexcept {
    // Bank registers hold whole 512KB banks, 0-255, and slot 0 never moves -
    // the same values a write to $A130F3-FF can produce, and no others.
    rom_banks_[0] = 0;
    for (auto& bank : rom_banks_) {
        bank &= 0xFFu * 0x80000u;
    }
    z80_bank_ &= 0x1FF;
    // Cycle debts stay within one instruction's worth between steps; a huge
    // one runs the Z80, the PSG or the DSP for billions of steps in one go.
    const auto bound = [](std::int64_t& debt) {
        constexpr std::int64_t kLimit = 1'000'000;
        if (debt > kLimit || debt < -kLimit) debt = 0;
    };
    bound(z80_cycle_debt_);
    bound(psg_cycle_debt_);
    bound(svp_cycle_debt_);
    if (z80_interrupt_cycles_ < 0 || z80_interrupt_cycles_ > 228) {
        z80_interrupt_cycles_ = 0;
    }
    // The 32X's share of the clock: a few hundred cycles owed either way
    // between catch-ups, and a line number inside one frame. Billions run the
    // SH-2s, or walk the per-line loop, for minutes.
    if (thirtytwox_quantum_ > 1'000'000) thirtytwox_quantum_ = 0;
    if (thirtytwox_owed_ > 10'000'000 || thirtytwox_owed_ < -10'000'000) thirtytwox_owed_ = 0;
    if (thirtytwox_last_line_ >= vdp_.lines_per_frame()) thirtytwox_last_line_ = 0;
}

bool Genesis::load_state_blob(const std::vector<std::uint8_t>& blob, std::string& error) {
    mercury::core::StateReader reader(blob.data(), blob.size());
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint16_t checksum = 0;
    std::uint32_t rom_size = 0;
    reader(magic);
    reader(version);
    reader(checksum);
    reader(rom_size);
    if (!reader.ok() || magic != kStateMagic) {
        error = "not a GenX save state";
        return false;
    }
    if (version < kOldestLoadableVersion || version > kStateVersion) {
        error = "save state was written by a different build";
        return false;
    }
    if (checksum != cartridge_.header().checksum ||
        rom_size != static_cast<std::uint32_t>(cartridge_.rom().size())) {
        error = "save state belongs to a different cartridge";
        return false;
    }
    std::uint8_t media = 0;
    std::uint64_t disc = 0;
    std::uint64_t cd_bios = 0;
    std::uint64_t x32_firmware = 0;
    if (version >= 5) {
        reader(media);
        reader(disc);
        reader(cd_bios);
        reader(x32_firmware);
        if (!reader.ok()) {
            error = "save state is truncated or corrupt";
            return false;
        }
    } else if (state_media() != 0) {
        // A version 4 state never held the SVP, the 32X or the Sega CD, so it
        // cannot put this machine back where it was.
        error = "save state is from a build that could not save this game's extra hardware";
        return false;
    }
    if (version >= 5) {
        if (((media & kMediaCd) != 0) != cd_mode_) {
            error = cd_mode_ ? "save state is from a cartridge, not a Sega CD disc"
                             : "save state is from a Sega CD disc, not a cartridge";
            return false;
        }
        if (((media & kMediaSvp) != 0) != svp_.present()) {
            error = "save state belongs to a different cartridge";
            return false;
        }
        if (cd_mode_ && disc != segacd_->disc_id()) {
            error = "save state belongs to a different disc";
            return false;
        }
        if (cd_mode_ && cd_bios != segacd_->bios_id()) {
            error = "save state was made with a different Sega CD BIOS";
            return false;
        }
        if ((media & kMedia32x) != 0 && x32_firmware != thirtytwox_->firmware_id()) {
            error = x32_firmware == 0 ? "save state was made without the 32X BIOS files"
                                      : "save state was made with different 32X BIOS files";
            return false;
        }
    }
    visit_state(reader, version, media);
    if (!reader.ok()) {
        error = "save state is truncated or corrupt";
        return false;
    }
    return true;
}

std::string Genesis::describe() const {
    std::ostringstream out;
    out << "Mercury - Sega Mega Drive\n"
        << (cartridge_.loaded() ? cartridge_.describe() : std::string("No cartridge loaded"))
        << "\n\n" << vdp_.describe()
        << "\n\nRegion    : " << (pal_ ? "PAL 50Hz" : "NTSC 60Hz")
        << "\nZ80       : " << (z80_reset_asserted_ ? "held in reset"
                                                    : (z80_bus_requested_ ? "bus granted to 68000" : "running"))
        << ", bank " << std::hex << (z80_bank_ << 15) << std::dec
        << "\nPort 1    : " << port1_.describe()
        << "\nPort 2    : " << port2_.describe();
    return out.str();
}

}  // namespace mercury::genesis




















