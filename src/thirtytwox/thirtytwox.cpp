#include "mercury/thirtytwox/thirtytwox.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace mercury::thirtytwox {

namespace {
// The 32X startup block in the cartridge header, at fixed offsets in the
// 68000's view of the ROM. This is the whole of what the boot ROM needs to
// know, which is why GenX can be BIOS agnostic.
constexpr std::size_t kHeaderSource = 0x3D4;
constexpr std::size_t kHeaderDestination = 0x3D8;
constexpr std::size_t kHeaderSize = 0x3DC;
constexpr std::size_t kHeaderMasterEntry = 0x3E0;
constexpr std::size_t kHeaderSlaveEntry = 0x3E4;
constexpr std::size_t kHeaderMasterVbr = 0x3E8;
constexpr std::size_t kHeaderSlaveVbr = 0x3EC;
// The Mega Drive header's own checksum word. The boot ROM hands it to the
// cartridge in a communication port, and a game may compare it.
constexpr std::size_t kHeaderChecksum = 0x18E;

// What the boot ROM leaves in each processor before jumping to the game.
//
// GBR is the base for the @(disp,GBR) addressing modes, and every 32X program
// reaches its communication ports through it, so it is not optional.
constexpr std::uint32_t kGbr = 0x20004000;
// Stack pointers: the master at the very top of SDRAM, the slave 2KB below
// it, so the two do not collide.
constexpr std::uint32_t kMasterStack = 0x06040000;
constexpr std::uint32_t kSlaveStack = 0x0603F800;

// The handshake the boot ROM performs on the cartridge's behalf. The master
// publishes "M_OK" into communication port 0; the slave waits to see it, then
// publishes "S_OK" into port 2.
constexpr int kMasterOkPort = 0;
constexpr int kSlaveOkPort = 2;
constexpr int kChecksumPort = 4;
constexpr std::uint16_t kOkHigh = 0x4F4B;  // "OK"
constexpr std::uint16_t kMasterOkLow = 0x4D5F;  // "M_"
constexpr std::uint16_t kSlaveOkLow = 0x535F;   // "S_"

// How long the master waits before publishing its marker. The real stub
// counts down 0x10000 times through a three-instruction loop; this is that
// in cycles, and it only has to be long enough for the 68000 to finish
// clearing memory. Roughly half a frame.
// MERCURY_32X_STARTUP_DELAY overrides it (in cycles) for a measurement: when a
// cartridge deadlocks in a two-sided handshake, "does it come out differently
// if the SH-2s start earlier or later" is the question that says whether the
// deadlock is a race at all.
const std::uint32_t kStartupDelayCycles = [] {
    const char* e = std::getenv("MERCURY_32X_STARTUP_DELAY");
    const long n = e == nullptr ? 0 : std::strtol(e, nullptr, 10);
    return n > 0 ? static_cast<std::uint32_t>(n) : static_cast<std::uint32_t>(0x10000 * 3);
}();

// Where the 68000's exception vectors point once the adapter is on: into the
// cartridge's 32X-side code, six bytes per entry, which is one long jump.
constexpr std::uint32_t kVectorTrampolines = 0x880200;
constexpr std::uint32_t kTrampolineStride = 6;
// The vector table proper ends here; the rest of the page is the two small
// service routines a game can call.
constexpr std::size_t kVectorTableEnd = 0xC0;
// Level 4 - the Mega Drive's horizontal interrupt. The boot ROM leaves this
// one at zero rather than pointing it at a trampoline.
constexpr std::size_t kHorizontalVector = 0x70;

void poke16(std::array<std::uint8_t, 0x100>& page, std::size_t at, std::uint16_t value) {
    page[at] = static_cast<std::uint8_t>(value >> 8);
    page[at + 1] = static_cast<std::uint8_t>(value);
}

void poke32(std::array<std::uint8_t, 0x100>& page, std::size_t at, std::uint32_t value) {
    poke16(page, at, static_cast<std::uint16_t>(value >> 16));
    poke16(page, at + 2, static_cast<std::uint16_t>(value));
}

// The firmware images are told apart by size alone, and a BIOS folder holds
// nothing else of 256, 2048 or 1024 bytes. A GAME folder can - a cue sheet or
// a save that happens to be one of those sizes - and taking it for the boot
// page would boot garbage. Now that the game's own folder is searched, those
// are skipped by name.
bool known_non_firmware(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".cue" || extension == ".m3u" || extension == ".txt" ||
           extension == ".nfo" || extension == ".ini" || extension == ".srm" ||
           extension == ".sav" || extension == ".brm" || extension == ".eep";
}

std::uint32_t big_endian32(const std::vector<std::uint8_t>& rom, std::size_t at) {
    if (at + 3 >= rom.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(rom[at]) << 24) |
           (static_cast<std::uint32_t>(rom[at + 1]) << 16) |
           (static_cast<std::uint32_t>(rom[at + 2]) << 8) |
           static_cast<std::uint32_t>(rom[at + 3]);
}
}  // namespace

ThirtyTwoX::ThirtyTwoX() {
    bus_.attach_adapter(&adapter_);
    // Both ways round: the bus routes register access to the video layer, and
    // the video layer reads the frame buffer and palette back out of the bus.
    bus_.attach_vdp(&vdp_);
    vdp_.attach_bus(&bus_);
    bus_.attach_peripherals(&peripherals_[0], &peripherals_[1]);
    // Both ways round again: the bus reaches each processor's on-chip block,
    // and that block's DMA controller reaches back out to move memory.
    peripherals_[0].attach_bus(&bus_);
    peripherals_[1].attach_bus(&bus_);
    wire_bus(master_);
    wire_bus(slave_);
}

void ThirtyTwoX::wire_bus(mercury::core::Sh2Cpu& cpu) noexcept {
    // Both processors get the SAME callbacks into the same bus object,
    // because on hardware they are on one bus. Anything either writes, the
    // other sees immediately - which is how they coordinate, and also how a
    // 32X game deadlocks itself if it gets its locking wrong.
    mercury::core::Sh2BusCallbacks callbacks;
    callbacks.read8 = [this](std::uint32_t a) { return bus_.read8(a); };
    callbacks.read16 = [this](std::uint32_t a) { return bus_.read16(a); };
    callbacks.read32 = [this](std::uint32_t a) { return bus_.read32(a); };
    callbacks.write8 = [this](std::uint32_t a, std::uint8_t v) { bus_.write8(a, v); };
    callbacks.write16 = [this](std::uint32_t a, std::uint16_t v) { bus_.write16(a, v); };
    callbacks.write32 = [this](std::uint32_t a, std::uint32_t v) { bus_.write32(a, v); };
    cpu.attach(callbacks);
    // Then the direct path, which the processor prefers. Same bus, same
    // calls; the callbacks above stay attached underneath and are simply not
    // used. Measured on Star Wars Arcade the std::function layer was a few
    // percent of all host time on its own.
    shared_bus_.bus = &bus_;
    cpu.attach(&shared_bus_);
}

void ThirtyTwoX::reset() noexcept {
    adapter_.reset();
    bus_.reset();
    vdp_.reset();
    peripherals_[0].reset();
    peripherals_[1].reset();
    master_.reset();
    slave_.reset();
    released_ = false;
    master_delay_ = 0;
    slave_waiting_ = false;
    hint_countdown_ = 0;
    first_unimplemented_ = 0;
    first_unimplemented_pc_ = 0;
    first_unimplemented_slave_ = false;
    master_steps_ = 0;
    slave_steps_ = 0;
    unimplemented_ = 0;
    last_unimplemented_ = 0;
}

std::uint64_t ThirtyTwoX::firmware_id() const noexcept {
    if (!bios_loaded_) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (int which = 0; which < 2; ++which) {
        for (const auto byte : bus_.boot_rom(which)) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    return hash == 0 ? 1 : hash;
}

void ThirtyTwoX::attach_rom(const std::vector<std::uint8_t>* rom) noexcept {
    bus_.attach_rom(rom);
    rom_ = rom;
    startup_ = Startup{};
    // One source of truth for nCART: a slot with no ROM in it is an empty
    // slot, and the firmware needs to be told - see Adapter::kSh2NotCart.
    adapter_.set_cartridge_present(rom != nullptr && !rom->empty());
    if (rom == nullptr || rom->size() < kHeaderSlaveEntry + 4) {
        return;
    }
    startup_.source = big_endian32(*rom, kHeaderSource);
    startup_.destination = big_endian32(*rom, kHeaderDestination);
    startup_.size = big_endian32(*rom, kHeaderSize);
    startup_.master_entry = big_endian32(*rom, kHeaderMasterEntry);
    startup_.slave_entry = big_endian32(*rom, kHeaderSlaveEntry);
    startup_.master_vbr = big_endian32(*rom, kHeaderMasterVbr);
    startup_.slave_vbr = big_endian32(*rom, kHeaderSlaveVbr);

    // The destination is an OFFSET INTO 32X RAM, not an absolute SH-2
    // address. The real 240p Test Suite declares 0x00000000 and expects its
    // code at the start of SDRAM, with entry points at 0x06000240 and
    // 0x06000468 - which only makes sense if the base is added. Taken
    // literally, 0 lands in the boot ROM region and the copy goes nowhere.
    // A destination that is already an SH-2 address is left alone.
    if (startup_.destination < 0x06000000u) {
        startup_.destination += 0x06000000u;
    }

    // Detection, and it takes two rules because ONE IS NOT ENOUGH.
    //
    // Almost every 32X cartridge declares "SEGA 32X" in the console-name
    // field, and for a long time that was the whole test. Sega's own MARS
    // Check Program does not: it declares "SEGA MEGA DRIVE" and is as much a
    // 32X cartridge as any of them. A rule built on the name alone refuses
    // the one cartridge written to exercise the hardware.
    //
    // So the name is one rule and the startup block is the other. A real
    // block points both entry points and both vector bases into somewhere an
    // SH-2 could actually run - 32X RAM or the cartridge window - and asks
    // for a copy that fits inside the cartridge. Measured across the whole
    // library: 32 of 32 32X cartridges accepted, and of 693 Mega Drive
    // cartridges none mistaken for one. The near miss was TinHead, whose
    // header area holds a repeating filler pattern that lands two plausible
    // entry points by chance but asks to copy 32MB out of a 1MB cartridge.
    static constexpr char kConsoleName[] = "SEGA 32X";
    bool named = rom->size() > 0x108;
    for (std::size_t index = 0; named && index < 8; ++index) {
        named = (*rom)[0x100 + index] == static_cast<std::uint8_t>(kConsoleName[index]);
    }

    const auto reachable = [](std::uint32_t address) {
        return (address >= 0x06000000u && address <= 0x0603FFFFu) ||
               (address >= 0x02000000u && address <= 0x023FFFFFu);
    };
    const bool header_is_real =
        reachable(startup_.master_entry) && reachable(startup_.slave_entry) &&
        reachable(startup_.master_vbr) && reachable(startup_.slave_vbr) &&
        startup_.size > 0 && startup_.size <= rom->size() && startup_.source < rom->size();

    startup_.valid = named || header_is_real;

    // Switch the adapter on for a 32X cartridge, here and now.
    //
    // On hardware the 68000 boots into the 32X's own BIOS, which sets ADEN,
    // releases the SH-2s and only then jumps to the cartridge. GenX has no
    // BIOS, so without this the cartridge's 68000 code sits waiting for a
    // 32X that never wakes - measured on the 240p Test Suite, which loops at
    // $808 for ever with the adapter inert.
    //
    // Only for a cartridge that declares itself a 32X. A Mega Drive game
    // leaves the adapter exactly as it was: off.
    if (startup_.valid) {
        build_m68k_boot_page();
        // WHO TURNS THE ADAPTER ON.
        //
        // It used to be turned on here, at load, because a cartridge left
        // waiting for a 32X that never woke went nowhere. That was the wrong
        // fix for a different bug, and it costs a cartridge its own startup:
        // the adapter being on at reset swaps the bottom of the 68000's map
        // to the boot page, so the processor resets into the 32X-side code
        // instead of into the cartridge's Mega Drive code.
        //
        // That matters because the 32X-side entry expects registers the Mega
        // Drive side sets up. Sega's own Mars Check Program begins its 32X
        // entry with an instruction using A6 as a base pointer, and A6 is set
        // by its Mega Drive reset code - which never ran. The result was a
        // dispatch through a zero frame pointer, a jump into the cartridge's
        // string table, and half a million illegal instructions.
        //
        // Hardware starts inert and the cartridge switches the adapter on
        // itself, which is what happens here now. Two things had to be right
        // before that could work: the MARS signature has to be readable
        // BEFORE ADEN, because that is how a cartridge finds the adapter at
        // all; and the Mega Drive's own windows onto the 32X video registers
        // and palette have to exist, because a cartridge polls the frame
        // buffer control register through them to pace its startup.
        //
        // With those two in place the 240p Test Suite runs the accurate path
        // end to end, which is what proves the handoff now works.
        //
        // The seven cartridges that regressed when this was first taken out
        // all shared one cause, as expected: the adapter was not reporting
        // which console it was plugged into, so their PAL/NTSC cross-check
        // failed and they halted. See thirtytwox::Vdp::kNotPal.
        //
        // MERCURY_32X_FORCE_ADEN goes back to switching it on here.
        if (std::getenv("MERCURY_32X_FORCE_ADEN") != nullptr) {
            adapter_.write16(Adapter::kControl, Adapter::kAden | Adapter::kNotRes);
        }
    }
}

// What is in a folder, by size, without loading it. attach_bios below uses the
// same walk, so a settings dialog reporting "found" can never disagree with
// what the loader will do.
ThirtyTwoX::BiosScan ThirtyTwoX::scan_bios_folder(const std::filesystem::path& folder) {
    BiosScan scan;
    std::error_code ignored;
    if (folder.empty() || !std::filesystem::is_directory(folder, ignored)) {
        return scan;
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
        if (!entry.is_regular_file(ignored) || known_non_firmware(entry.path())) {
            continue;
        }
        switch (entry.file_size(ignored)) {
            case 0x100: scan.genesis_page = true; break;
            case 0x800: scan.master_rom = true; break;
            case 0x400: scan.slave_rom = true; break;
            default: break;
        }
    }
    return scan;
}

bool ThirtyTwoX::attach_bios(const std::filesystem::path& rom_path) {
    bios_loaded_ = false;

    // Where to look: the folder the user configured first, then the
    // environment override, then a folder called BIOS beside the cartridge or
    // above it. A library is usually laid out as <root>/Games/<title>/<rom>
    // with the firmware in <root>/BIOS, so walking up a few levels finds it
    // without the user having to say where - but only for that layout, which
    // is why the configured folder wins outright.
    std::vector<std::filesystem::path> candidates;
    if (!bios_folder_.empty()) {
        candidates.push_back(bios_folder_);
    }
    if (const char* configured = std::getenv("MERCURY_32X_BIOS"); configured != nullptr) {
        candidates.emplace_back(configured);
    }
    std::error_code ignored;
    // The game's own folder, before the walk: the BIOS tab tells people GenX
    // looks for the files in their game folder, and three files dropped in
    // beside the cartridge are the plainest reading of that. It only counts
    // if all three sizes are there, so an ordinary game folder falls through.
    std::filesystem::path here = rom_path.parent_path();
    if (!here.empty()) {
        candidates.push_back(here);
    }
    for (int level = 0; level < 4 && !here.empty(); ++level) {
        candidates.push_back(here / "BIOS");
        candidates.push_back(here / "bios");
        const auto up = here.parent_path();
        if (up == here) {
            break;
        }
        here = up;
    }

    for (const auto& folder : candidates) {
        if (!std::filesystem::is_directory(folder, ignored)) {
            continue;
        }
        std::vector<std::uint8_t> genesis_page, master_rom, slave_rom;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
            if (!entry.is_regular_file(ignored)) {
                continue;
            }
            if (known_non_firmware(entry.path())) {
                continue;
            }
            const auto size = entry.file_size(ignored);
            // Size is the only reliable label. 256 / 2048 / 1024 are the three
            // images and nothing else in a BIOS folder is those sizes.
            std::vector<std::uint8_t>* target = nullptr;
            if (size == 0x100) target = &genesis_page;
            else if (size == 0x800) target = &master_rom;
            else if (size == 0x400) target = &slave_rom;
            if (target == nullptr || !target->empty()) {
                continue;
            }
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) {
                continue;
            }
            target->assign(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
        }
        if (genesis_page.size() != 0x100 || master_rom.size() != 0x800 ||
            slave_rom.size() != 0x400) {
            continue;
        }
        std::copy(genesis_page.begin(), genesis_page.end(), m68k_boot_page_.begin());
        bus_.load_boot_rom(0, master_rom);
        bus_.load_boot_rom(1, slave_rom);
        bios_loaded_ = true;
        return true;
    }
    return false;
}

void ThirtyTwoX::build_m68k_boot_page() noexcept {
    if (bios_loaded_) {
        return;  // the real page is already in place and is not to be guessed over
    }
    m68k_boot_page_.fill(0);
    if (rom_ == nullptr) {
        return;
    }

    // Longword 0 is the initial stack pointer. Keep the cartridge's own,
    // rather than the zero the real boot ROM leaves there: every 32X program
    // sets its own stack early on, so the value is not load bearing, and a
    // real stack pointer is the safer of the two if one ever is not set.
    const std::uint32_t cartridge_stack = big_endian32(*rom_, 0);
    poke32(m68k_boot_page_, 0, cartridge_stack);

    // Every other vector, reset included, points at its own six-byte slot in
    // the cartridge. Vector n lands at $880200 + (n-1)*6.
    for (std::size_t vector = 1; vector * 4 < kVectorTableEnd; ++vector) {
        poke32(m68k_boot_page_, vector * 4,
               kVectorTrampolines + static_cast<std::uint32_t>(vector - 1) * kTrampolineStride);
    }
    poke32(m68k_boot_page_, kHorizontalVector, 0);

    // Past the table sit two routines a game may call into. Fill the whole
    // area with RTS first, so a call to anything not implemented returns
    // harmlessly instead of executing zeros.
    for (std::size_t at = kVectorTableEnd; at < m68k_boot_page_.size(); at += 2) {
        poke16(m68k_boot_page_, at, 0x4E75);  // rts
    }

    // $D4 is the one service worth having: set the cartridge's ROM banking.
    // The caller points A0 at eight bytes and the routine writes them into
    // the bank registers from $A130F1 upward, every other byte, then clears
    // RV so the Mega Drive sees the cartridge again. Written out here from
    // that contract rather than lifted from anywhere.
    std::size_t at = 0xD4;
    poke16(m68k_boot_page_, at, 0x48E7);          // movem.l d0/a1,-(sp)
    poke16(m68k_boot_page_, at + 2, 0x8040);
    poke16(m68k_boot_page_, at + 4, 0x227C);      // movea.l #$A130F1,a1
    poke32(m68k_boot_page_, at + 6, 0x00A130F1);
    poke16(m68k_boot_page_, at + 10, 0x7007);     // moveq #7,d0
    poke16(m68k_boot_page_, at + 12, 0x12D8);     // loop: move.b (a0)+,(a1)+
    poke16(m68k_boot_page_, at + 14, 0x5289);     // addq.l #1,a1
    poke16(m68k_boot_page_, at + 16, 0x51C8);     // dbra d0,loop
    poke16(m68k_boot_page_, at + 18, 0xFFFA);
    poke16(m68k_boot_page_, at + 20, 0x0239);     // andi.b #$FE,($A15107).l
    poke16(m68k_boot_page_, at + 22, 0x00FE);
    poke32(m68k_boot_page_, at + 24, 0x00A15107);
    poke16(m68k_boot_page_, at + 28, 0x4CDF);     // movem.l (sp)+,d0/a1
    poke16(m68k_boot_page_, at + 30, 0x0201);
    // and falls through to the RTS that already fills the rest.
}

namespace {
// The Mega Drive's frame buffer window lines up byte for byte with the
// processors' own, so one subtraction turns an address on that side into an
// address on this one - and $860000 lands in the overwrite image for free,
// because $04020000 is the same distance along.
constexpr std::uint32_t kFrameBufferOnTheBus = 0x04000000;
}  // namespace

// On hardware the Mega Drive may only reach the frame buffer when it owns the
// video chip and the memory is not busy with a swap or a fill.
//
// GENX DOES NOT ENFORCE THAT, deliberately. Enforcing it was tried and cost
// Virtua Fighter its entire picture, taking it from a full screen back to
// nothing, while helping no cartridge at all. Something about how GenX
// arrives at those two conditions is wrong - most likely the swap is counted
// as pending for far longer than it really is - and until that is understood,
// a gate built on them refuses writes the hardware would have taken.
//
// The STATUS is still reported honestly through the frame buffer control
// register, so software that asks gets the right answer. Only the refusal is
// left out, which is the half that can do harm.
bool ThirtyTwoX::frame_buffer_reachable() const noexcept {
    return true;
}

std::uint16_t ThirtyTwoX::frame_buffer_read16(std::uint32_t address) const noexcept {
    if (!frame_buffer_reachable()) {
        return 0;
    }
    return bus_.read16(kFrameBufferOnTheBus + (address - kFrameBufferFirst));
}

void ThirtyTwoX::frame_buffer_write16(std::uint32_t address, std::uint16_t value) noexcept {
    if (!frame_buffer_reachable()) {
        return;
    }
    bus_.write16(kFrameBufferOnTheBus + (address - kFrameBufferFirst), value);
}

std::uint8_t ThirtyTwoX::frame_buffer_read8(std::uint32_t address) const noexcept {
    if (!frame_buffer_reachable()) {
        return 0;
    }
    return bus_.read8(kFrameBufferOnTheBus + (address - kFrameBufferFirst));
}

void ThirtyTwoX::frame_buffer_write8(std::uint32_t address, std::uint8_t value) noexcept {
    if (!frame_buffer_reachable()) {
        return;
    }
    bus_.write8(kFrameBufferOnTheBus + (address - kFrameBufferFirst), value);
}

std::uint16_t ThirtyTwoX::video_read16(std::uint32_t address) const noexcept {
    const std::uint32_t even = address & ~1u;
    if (even >= kPaletteFirst) {
        return bus_.palette()[(even & 0x1FFu) / 2];
    }
    return vdp_.read_register(even & 0x0Eu);
}

void ThirtyTwoX::video_write16(std::uint32_t address, std::uint16_t value) noexcept {
    // Only while the Mega Drive owns the video chip. With FM set the SH-2s
    // have it and a write from this side is ignored rather than fought over.
    if (adapter_.sh2_owns_vdp()) {
        return;
    }
    const std::uint32_t even = address & ~1u;
    if (even >= kPaletteFirst) {
        bus_.set_palette_entry((even & 0x1FFu) / 2, value);
        return;
    }
    vdp_.write_register(even & 0x0Eu, value);
}

std::uint8_t ThirtyTwoX::video_read8(std::uint32_t address) const noexcept {
    const std::uint16_t word = video_read16(address & ~1u);
    return (address & 1u) != 0 ? static_cast<std::uint8_t>(word)
                               : static_cast<std::uint8_t>(word >> 8);
}

void ThirtyTwoX::video_write8(std::uint32_t address, std::uint8_t value) noexcept {
    const std::uint32_t even = address & ~1u;
    const std::uint16_t word = video_read16(even);
    const std::uint16_t merged =
        (address & 1u) != 0
            ? static_cast<std::uint16_t>((word & 0xFF00u) | value)
            : static_cast<std::uint16_t>((word & 0x00FFu) |
                                         (static_cast<std::uint16_t>(value) << 8));
    video_write16(even, merged);
}

bool ThirtyTwoX::m68k_boot_overlay_write(std::uint32_t address, std::uint8_t value) noexcept {
    if (!adapter_.enabled() || !startup_.valid || address >= m68k_boot_page_.size()) {
        return false;
    }
    // Only the four bytes of the horizontal interrupt vector are storage.
    // Everything else in the page is the adapter's own and a write to it goes
    // nowhere - claiming the access, though, because the page is what is
    // mapped there.
    if ((address & ~3u) == kWritableVector) {
        m68k_boot_page_[address] = value;
    }
    return true;
}

bool ThirtyTwoX::m68k_boot_overlay(std::uint32_t address, std::uint16_t& out) const noexcept {
    if (!adapter_.enabled() || !startup_.valid || address >= m68k_boot_page_.size()) {
        return false;
    }
    const std::size_t at = address & ~std::size_t{1};
    out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(m68k_boot_page_[at]) << 8) |
                                     m68k_boot_page_[at + 1]);
    return true;
}

void ThirtyTwoX::run_boot_stub() noexcept {
    if (bios_loaded_) {
        // Nothing to stand in for. Both processors have just reset out of
        // their own boot ROMs, and the firmware does the copy, the registers
        // and the ready handshake itself - which is the whole point of
        // having it.
        return;
    }
    if (!startup_.valid || rom_ == nullptr) {
        return;
    }
    // Copy the SH-2 program out of the cartridge into the 32X's RAM, which is
    // what the boot ROM spends its time doing. Through the bus rather than
    // straight into the array, so the destination is decoded exactly as the
    // processors would decode it and a cartridge asking for somewhere odd
    // lands somewhere odd rather than out of bounds.
    for (std::uint32_t offset = 0; offset < startup_.size; ++offset) {
        const std::size_t from = startup_.source + offset;
        const std::uint8_t byte = from < rom_->size() ? (*rom_)[from] : 0;
        bus_.write8(startup_.destination + offset, byte);
    }

    // Then set up each processor the way the boot ROM leaves it. Only the
    // entry points used to be set here, and that was the whole of the master
    // running to an invalid address: with no boot ROM, reset reads its stack
    // pointer out of an empty vector table and gets zero, so the first
    // subroutine call pushed a return address into nothing and the matching
    // return jumped there.
    auto prepare = [](mercury::core::Sh2Cpu& cpu, std::uint32_t entry, std::uint32_t vbr,
                      std::uint32_t stack) {
        auto& r = cpu.registers();
        r.pc = entry;
        r.vbr = vbr;
        r.gbr = kGbr;
        r.r[15] = stack;
    };
    prepare(master_, startup_.master_entry, startup_.master_vbr, kMasterStack);
    prepare(slave_, startup_.slave_entry, startup_.slave_vbr, kSlaveStack);

    // Neither runs yet. The master waits out the startup delay; the slave
    // waits for the master's marker. See the note on master_delay_.
    master_delay_ = kStartupDelayCycles;
    slave_waiting_ = true;
    master_.set_halted(true);
    slave_.set_halted(true);

    if (rom_->size() > kHeaderChecksum + 1) {
        adapter_.comm_write(kChecksumPort,
                            static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>((*rom_)[kHeaderChecksum]) << 8) |
                                (*rom_)[kHeaderChecksum + 1]));
    }
}

void ThirtyTwoX::start_vblank() noexcept {
    if (!adapter_.sh2_running()) {
        return;
    }
    vdp_.start_vblank();
    adapter_.raise_sh2_interrupt(Adapter::kIrqVint);
}

void ThirtyTwoX::scanline(bool active) noexcept {
    if (!adapter_.sh2_running()) {
        return;
    }
    // The countdown lives with the register, because writing the period is
    // what restarts it.
    if (!active && !adapter_.hint_during_vblank()) {
        // Outside the picture with HEN clear, the counter is held reloaded
        // rather than counting, so the first line of the next frame is a
        // whole period away.
        adapter_.set_hint_countdown(static_cast<int>(adapter_.sh2_hcount()) + 1);
        return;
    }
    const int left = adapter_.hint_countdown() - 1;
    if (left > 0) {
        adapter_.set_hint_countdown(left);
        return;
    }
    // The register holds the period LESS ONE, so a zero there means every
    // line, not never.
    adapter_.set_hint_countdown(static_cast<int>(adapter_.sh2_hcount()) + 1);
    adapter_.raise_sh2_interrupt(Adapter::kIrqHint);
}

void ThirtyTwoX::end_vblank() noexcept {
    if (!adapter_.sh2_running()) {
        return;
    }
    vdp_.end_vblank();
}

std::uint32_t ThirtyTwoX::run(std::uint32_t cycles) noexcept {
    if (!adapter_.enabled()) {
        // Inert. Not merely idle - this is the path every Mega Drive game
        // takes, every frame, so it has to cost nothing.
        return cycles;
    }

    if (!adapter_.sh2_running()) {
        released_ = false;
        return cycles;
    }
    if (!released_) {
        // ADEN and nRES are both up. This is where the boot sequence happens -
        // not at construction, because the cartridge has to be mapped first.
        // Each processor reads its reset vector out of ITS OWN boot ROM, so
        // the bus has to know who is asking before the read happens. Get this
        // wrong with real firmware loaded and the slave starts the master's
        // program.
        bus_.set_active_sh2(0);
        master_.reset();
        bus_.set_active_sh2(1);
        slave_.reset();
        // The on-chip blocks come out of reset with the processors, and their
        // reset values are load bearing - see Sh2Peripherals.
        peripherals_[0].reset();
        peripherals_[1].reset();
        // GenX has no boot ROM, so the vector table at address 0 reads as
        // zero and reset leaves both processors at 0. The stub replaces what
        // the BIOS would have done, from the cartridge's own header.
        run_boot_stub();
        released_ = true;
    }
    // The fetch windows point into the cartridge, and a cartridge can be
    // swapped between slices. Cheap to drop - one bus question per processor
    // per slice - and it means a window can never outlive what it points at.
    master_.invalidate_fetch_window();
    slave_.invalidate_fetch_window();


    // Run them turn about. They are on one bus and a real pair interleave at
    // the bus-cycle level; alternating instructions is the same trade the
    // Gear Link makes between two Game Gears, and for the same reason - it is
    // deterministic and close enough that neither can get a bus cycle ahead
    // of what the other could have seen.
    // Interrupts are levels, so the line is presented before every instruction
    // at whatever level it stands at NOW - including zero.
    //
    // This used to only ever raise the processor's request, never lower it,
    // and that manufactured interrupts. A handler runs with the mask up, the
    // still-pending source is offered again and latched, the handler
    // acknowledges the source - and the latch survives the acknowledge. On
    // return from exception the mask drops and the processor takes the stale
    // latch: a second handler run with no event behind it.
    //
    // Knuckles' Chaotix shows exactly what that costs. Its slave's PWM handler
    // acknowledges at once and then does its sample work on every OTHER
    // interrupt, by rotating an alternating bit pattern. The phantom entry
    // flipped the pattern, so the work ran on every real interrupt, the sample
    // queue filled faster than it drains, and the slave spent over half its
    // time spinning on the queue-full flag - which ares never once reports.
    // The tally said the same thing on its own: 132,144 PWM interrupts raised
    // and 263,842 taken.
    const auto offer = [this](mercury::core::Sh2Cpu& cpu, int which) {
        int level = adapter_.pending_sh2_level(which);
        int vector = level > 0 ? static_cast<int>(Adapter::vector_for_level(level)) : 0;
        // TWO SETS OF INTERRUPTS REACH AN SH-2, not one. The adapter's five
        // are only half of it: the processor's own on-chip blocks interrupt it
        // too, with a priority and a vector software chooses. Highest level
        // wins, which is what the interrupt controller does.
        const auto& chip = peripherals_[which != 0 ? 1 : 0];
        const int on_chip = chip.on_chip_interrupt_level();
        if (on_chip > level) {
            level = on_chip;
            vector = chip.on_chip_interrupt_vector();
        }
        cpu.set_interrupt_level(level, vector);
    };

    // The startup handshake, resolved before any instruction runs this slice.
    if (master_delay_ > 0) {
        master_delay_ = master_delay_ > cycles ? master_delay_ - cycles : 0;
        if (master_delay_ == 0) {
            adapter_.comm_write(kMasterOkPort, kMasterOkLow);
            adapter_.comm_write(kMasterOkPort + 1, kOkHigh);
            master_.set_halted(false);
        }
    }
    if (slave_waiting_ && adapter_.comm_read(kMasterOkPort) == kMasterOkLow &&
        adapter_.comm_read(kMasterOkPort + 1) == kOkHigh) {
        adapter_.comm_write(kSlaveOkPort, kSlaveOkLow);
        adapter_.comm_write(kSlaveOkPort + 1, kOkHigh);
        slave_.set_halted(false);
        slave_waiting_ = false;
    }

    // WHICHEVER PROCESSOR IS BEHIND IN TIME RUNS NEXT.
    //
    // This used to alternate them one instruction each, which quietly makes
    // the pair equal in INSTRUCTION COUNT rather than in time: a processor
    // executing three-cycle instructions kept perfect pace with one executing
    // one-cycle instructions, so the two drifted apart by however much their
    // instruction mixes differed. On a machine whose two halves coordinate
    // through tight spin-waits, that drift is the difference between a
    // handshake completing and a deadlock.
    //
    // Each now carries its own position and the one with the lower position
    // steps, which is what two processors sharing a bus actually do.
    std::uint32_t master_position = 0;
    std::uint32_t slave_position = 0;
    std::uint32_t audio_paid = 0;
    while (master_position < cycles || slave_position < cycles) {
        if (master_position <= slave_position) {
            bus_.set_active_sh2(0);
            offer(master_, 0);
            const auto m = master_.step();
            if (sh2_trace_) sh2_trace_(0, m, master_.registers());
            peripherals_[0].tick(m.cycles);
            // The 68000 -> SH-2 FIFO's request line, sampled between
            // instructions the way the DMA controller samples its pin. One
            // word per instruction is well inside what the hardware manages,
            // and it lets the FIFO drain as the 68000 fills it.
            if (adapter_.dreq_line()) {
                peripherals_[0].step_external_request(0);
            }
            master_steps_ += m.executed ? 1 : 0;
            if (m.unimplemented) {
                if (unimplemented_ == 0) {
                    first_unimplemented_ = m.opcode;
                    first_unimplemented_pc_ = m.pc_before;
                    first_unimplemented_slave_ = false;
                }
                ++unimplemented_;
                last_unimplemented_ = m.opcode;
            }
            master_position += m.cycles != 0 ? m.cycles : 1;
            vdp_.tick_fill(m.cycles);
        } else {
            bus_.set_active_sh2(1);
            offer(slave_, 1);
            const auto s = slave_.step();
            if (sh2_trace_) sh2_trace_(1, s, slave_.registers());
            peripherals_[1].tick(s.cycles);
            if (adapter_.dreq_line()) {
                peripherals_[1].step_external_request(0);  // see the master's
            }
            slave_steps_ += s.executed ? 1 : 0;
            if (s.unimplemented) {
                if (unimplemented_ == 0) {
                    first_unimplemented_ = s.opcode;
                    first_unimplemented_pc_ = s.pc_before;
                    first_unimplemented_slave_ = true;
                }
                ++unimplemented_;
                last_unimplemented_ = s.opcode;
            }
            slave_position += s.cycles != 0 ? s.cycles : 1;
        }
        // The audio clock is shared, so it follows the pair's common time -
        // the position BOTH have reached - rather than either one alone.
        const std::uint32_t common =
            master_position < slave_position ? master_position : slave_position;
        if (common > audio_paid) {
            if (adapter_.pwm().tick(common - audio_paid)) {
                adapter_.raise_sh2_interrupt(Adapter::kIrqPwm);
            }
            audio_paid = common;
        }
    }
    return master_position < slave_position ? master_position : slave_position;
}

std::string ThirtyTwoX::describe() const {
    std::ostringstream out;
    out << adapter_.describe() << "; " << bus_.describe() << "; "
        << (bios_loaded_ ? "REAL BIOS" : "BIOS agnostic (synthesised startup)") << "; ";
    if (startup_.valid) {
        out << "header start: copy " << startup_.size << " bytes to " << std::hex
            << startup_.destination << ", master at " << startup_.master_entry << ", slave at "
            << startup_.slave_entry << std::dec << "; ";
    } else {
        out << "no 32X startup header; ";
    }
    out << "master " << master_steps_ << " instructions, slave " << slave_steps_;
    if (unimplemented_ != 0) {
        out << "; " << unimplemented_ << " unimplemented (last 0x" << std::hex
            << last_unimplemented_ << ")";
    }
    return out.str();
}

}  // namespace mercury::thirtytwox
