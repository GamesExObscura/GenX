#include "mercury/gamegear/game_gear.hpp"

#include "mercury/core/save_state.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace mercury::gamegear {

namespace {

// Value the Master System / Game Gear BIOS leaves in SP before jumping to the
// cartridge.
constexpr std::uint16_t kPowerOnStackPointer = 0xDFF0;

}  // namespace

GameGear::GameGear() {
    initialize_runtime();
}

mercury::core::RomLoadResult GameGear::load_rom(const std::filesystem::path& path) {
    auto result = mercury::core::load_rom_image(path);
    if (!result.ok) {
        return result;
    }

    // A Master System cartridge run on Game Gear hardware puts the VDP in SMS
    // mode: 6-bit single-byte CRAM entries and the full 256x192 picture rather
    // than the handheld's 160x144 crop. The file extension is what tells the
    // two apart here.
    // For a zipped cartridge the .zip on the outside says nothing; the name of
    // the file inside it is what carries the .sms or .gg.
    auto extension = result.image.archive_entry.empty()
                         ? path.extension().string()
                         : std::filesystem::path(result.image.archive_entry).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    vdp_.set_display_mode(extension == ".sms" ? Vdp::DisplayMode::master_system
                                              : Vdp::DisplayMode::game_gear);

    if (!cartridge_.load(result.image)) {
        return {
            .ok = false,
            .image = {},
            .error = "Cartridge rejected the ROM image.",
        };
    }

    reset();
    // Per-cartridge serial quirk, applied after reset because reset clears it.
    system_control_.set_tx_holding_register(wants_tx_holding_register(result.image.bytes));
    return result;
}

// Does this cartridge need a transmit holding register on the link port?
//
// A write to $03 while a byte is still going out is normally thrown away, here
// and in Gearsystem. Two games in the library write $03 without checking the
// busy bit first, and they need opposite things:
//
//   Pac-Man sends a literal 3 from one routine and its per-frame delta byte
//   from another, three bit periods apart, at the moment its link state
//   changes. Exactly ONE write is refused in a whole session, and that one
//   byte puts its index/value protocol permanently out of step: the receiver
//   latches message type 7 and the two-player lobby waits for a type 4 that
//   can never arrive. With the byte held and sent, both consoles play.
//
//   R.B.I. Baseball '94 writes $03 about three times a frame and has roughly
//   one write refused each frame, over a thousand a session. Buffer those and
//   it jumps to its own halt-on-error trap - the two-byte self-loop at $02E0 -
//   after eight bytes, long before its title screen. It gates its sends on the
//   receive-ready bit, so the extra traffic is what breaks it.
//
// No hardware was available to settle which is right, and R.B.I. actively
// reports a bad stream while Pac-Man does not, so the default stays "drop" and
// the buffer is turned on for the one cartridge that needs it. Identified by
// the Sega header's product code at $7FFC-$7FFE, which is fixed for a release
// and, unlike this cartridge's header checksum, is not zero.
bool GameGear::wants_tx_holding_register(const std::vector<std::uint8_t>& rom) noexcept {
    constexpr std::size_t kHeaderProductCode = 0x7FFC;
    if (rom.size() < kHeaderProductCode + 3) {
        return false;
    }
    // "TMR SEGA" has to be there, or these are just three arbitrary bytes.
    static constexpr char kMagic[] = "TMR SEGA";
    for (std::size_t index = 0; index < 8; ++index) {
        if (rom[0x7FF0 + index] != static_cast<std::uint8_t>(kMagic[index])) {
            return false;
        }
    }
    return rom[kHeaderProductCode] == 0x18 && rom[kHeaderProductCode + 1] == 0x40 &&
           rom[kHeaderProductCode + 2] == 0x10;
}

void GameGear::reset() noexcept {
    cartridge_.reset();
    system_control_.reset();
    vdp_.reset();
    psg_.reset();
    bus_.reset();
    cpu_.reset();
    // On real hardware the BIOS sets up a stack before handing control to the
    // cartridge, and games are entitled to rely on that. Leaving SP at the
    // Z80's 0xFFFF puts the stack directly on top of the mapper registers at
    // FFFC-FFFF, so any game that uses PUSH/POP as a timing delay before
    // setting its own SP - Ecco the Dolphin does exactly this around its VDP
    // writes - silently rewrites its own bank registers. 0xDFF0 is the
    // conventional Master System value.
    cpu_.set_stack_pointer(kPowerOnStackPointer);
    synchronize_interrupts();
}

mercury::core::Z80StepResult GameGear::step() noexcept {
    synchronize_interrupts();
    auto result = cpu_.step();
    const auto applied_io_wait_cycles = cpu_.take_applied_io_wait_cycles();
    result.cycles += cpu_.take_pending_wait_cycles();
    if (result.executed && !result.unimplemented && result.cycles > 0) {
        const auto remaining_cycles =
            result.cycles > applied_io_wait_cycles
                ? result.cycles - applied_io_wait_cycles
                : 0;
        if (remaining_cycles != 0) {
            vdp_.advance_cycles(remaining_cycles);
            psg_.advance_cycles(remaining_cycles);
            system_control_.advance_serial(remaining_cycles);
        }
    }
    synchronize_interrupts();
    return result;
}

void GameGear::initialize_runtime() noexcept {
    cartridge_.reset();
    system_control_.reset();
    vdp_.reset();
    psg_.reset();
    bus_.reset();
    bus_.attach(cartridge_, system_control_, vdp_, psg_);
    cpu_.attach_bus(bus_.make_z80_callbacks());
    cpu_.reset();
}

const GameGearBus& GameGear::bus() const noexcept {
    return bus_;
}

const Cartridge& GameGear::cartridge() const noexcept {
    return cartridge_;
}

mercury::core::Z80Cpu& GameGear::cpu() noexcept {
    return cpu_;
}

const mercury::core::Z80Cpu& GameGear::cpu() const noexcept {
    return cpu_;
}

InputState& GameGear::input_state() noexcept {
    return system_control_.input_state();
}

const InputState& GameGear::input_state() const noexcept {
    return system_control_.input_state();
}

const MemoryMap& GameGear::memory_map() const noexcept {
    return memory_map_;
}

Psg& GameGear::psg() noexcept {
    return psg_;
}

const Psg& GameGear::psg() const noexcept {
    return psg_;
}

const SystemControl& GameGear::system_control() const noexcept {
    return system_control_;
}

const Vdp& GameGear::vdp() const noexcept {
    return vdp_;
}

mercury::core::Z80BusCallbacks GameGear::make_z80_callbacks() noexcept {
    return bus_.make_z80_callbacks();
}

mercury::core::ModuleStatuses GameGear::module_statuses() const {
    return {
        {"ROM loader", mercury::core::ModuleState::scaffolded, "File loading helper exists."},
        {"Mapper state", mercury::core::ModuleState::scaffolded, "FFFC-FFFF mapper state and helper logic are implemented."},
        {"Cartridge", mercury::core::ModuleState::scaffolded, "Banked cartridge reads are routed through mapper state."},
        {"Game Gear bus", mercury::core::ModuleState::scaffolded, "Memory-space and I/O-space routing now has concrete behavior."},
        {"Input state", mercury::core::ModuleState::scaffolded, "Typed button state now derives the active-low joystick ports."},
        {"Memory map", mercury::core::ModuleState::scaffolded, "Game Gear address regions are documented."},
        {"Z80 integration", mercury::core::ModuleState::scaffolded, "Shared Z80 wrapper can reset, attach to the bus, and step a starter opcode subset."},
        {"VDP", mercury::core::ModuleState::scaffolded, "VRAM/CRAM-facing module boundary is defined, with a basic frame interrupt timebase."},
        {"PSG", mercury::core::ModuleState::scaffolded, "Tone/noise/stereo boundary is defined."},
        {"Input/system control", mercury::core::ModuleState::scaffolded, "Port ownership is defined."},
    };
}

std::string GameGear::describe() const {
    std::ostringstream out;
    out << "Game Gear system skeleton\n"
        << "- " << bus_.describe() << '\n'
        << "- " << cpu_.describe() << '\n'
        << "- " << memory_map_.describe() << '\n'
        << "- " << vdp_.describe() << '\n'
        << "- " << psg_.describe() << '\n'
        << "- " << system_control_.describe() << '\n'
        << "- " << cartridge_.describe();
    return out.str();
}

void GameGear::synchronize_interrupts() noexcept {
    cpu_.set_maskable_interrupt_line(vdp_.interrupt_requested());

    // The Gear-to-Gear cable's interrupt is wired to the Z80's NMI, the same
    // line the Master System's pause button uses. A completed reception with
    // the link interrupt enabled is what raises it.
    if (system_control_.take_link_nmi()) {
        cpu_.request_non_maskable_interrupt();
    }

    // On the Master System the START button is on the console, not the pad,
    // and it is wired to the Z80's NMI line rather than to a port bit - which
    // is why a title screen reading "PRESS START BUTTON" cannot be started
    // with button 1 or 2. The Game Gear has no such line (its START is just
    // port 00 bit 7), so this only applies to cartridges running in SMS mode.
    // Edge triggered: holding the button must not retrigger the handler.
    if (vdp_.display_mode() == Vdp::DisplayMode::master_system) {
        const bool pause_pressed = system_control_.input_state().is_pressed(Button::start);
        if (pause_pressed && !pause_button_was_pressed_) {
            cpu_.request_non_maskable_interrupt();
        }
        pause_button_was_pressed_ = pause_pressed;
    }
}

namespace {
// Kept distinct from the Mega Drive's magic so a state cannot be loaded into
// the wrong machine. The version (GameGear::kStateVersion) is bumped whenever
// visit_state changes shape.
constexpr std::uint32_t kStateMagic = 0x58'4E'47'47;  // "GGNX"
// The oldest layout still readable: version 4 only appended to version 3.
constexpr std::uint32_t kOldestLoadableVersion = 3;
}  // namespace

std::vector<std::uint8_t> GameGear::save_state_blob() const {
    std::vector<std::uint8_t> blob;
    mercury::core::StateWriter writer(blob);
    writer(kStateMagic);
    writer(kStateVersion);
    std::uint32_t rom_size = static_cast<std::uint32_t>(cartridge_.rom_size_bytes());
    writer(rom_size);
    // visit_state is one function for both directions, so it cannot be const;
    // saving genuinely does not modify anything it touches.
    const_cast<GameGear*>(this)->visit_state(writer);
    return blob;
}

bool GameGear::load_state_blob(const std::vector<std::uint8_t>& blob, std::string& error) {
    mercury::core::StateReader reader(blob.data(), blob.size());
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t rom_size = 0;
    reader(magic);
    reader(version);
    reader(rom_size);
    if (!reader.ok() || magic != kStateMagic) {
        error = "not a GenX Game Gear save state";
        return false;
    }
    if (version < kOldestLoadableVersion || version > kStateVersion) {
        error = "save state was written by a different build";
        return false;
    }
    if (rom_size != static_cast<std::uint32_t>(cartridge_.rom_size_bytes())) {
        error = "save state belongs to a different cartridge";
        return false;
    }
    visit_state(reader, version);
    if (!reader.ok()) {
        error = "save state is truncated or corrupt";
        return false;
    }
    return true;
}

}  // namespace mercury::gamegear

