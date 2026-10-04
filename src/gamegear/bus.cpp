#include "mercury/gamegear/bus.hpp"

#include <sstream>

#include "mercury/gamegear/cartridge.hpp"
#include "mercury/gamegear/psg.hpp"
#include "mercury/gamegear/system_control.hpp"
#include "mercury/gamegear/vdp.hpp"

namespace mercury::gamegear {

namespace {

// The ~29 T-state figure quoted for active-display VRAM/CRAM accesses is a
// spacing requirement on the PROGRAM, not a stall the hardware imposes. The
// SMS/GG VDP has no wait-state line: write the data port too fast during
// active display and the write is silently LOST, the CPU is never held up.
//
// Charging the CPU 18 cycles per data-port write made every VRAM upload loop
// ~36% slower than hardware. Games that upload with interrupts disabled then
// overrun into the active display and lose the line interrupts that arrive
// while they are still copying - Rise of the Robots drives its whole arena
// from a 96-split-per-frame line-interrupt table and was losing ~40% of them,
// which is what made its background flicker and jump.
constexpr std::uint32_t kVdpActiveDisplayWaitCycles = 0;

// T-states from the start of an I/O instruction to the point where it samples
// the bus. IN A,(n) is 4 (opcode) + 3 (operand) before its I/O machine cycle;
// IN r,(C) is 4 + 4. Seven is a good single value for both, and it is what
// lets a status-port poll observe a VDP flag that flips part-way through the
// instruction instead of always losing to the interrupt.
constexpr std::uint32_t kIoReadPhaseCycles = 7;

}  // namespace

void GameGearBus::attach(Cartridge& cartridge, SystemControl& system_control, Vdp& vdp, Psg& psg) noexcept {
    cartridge_ = &cartridge;
    system_control_ = &system_control;
    vdp_ = &vdp;
    psg_ = &psg;
}

void GameGearBus::reset() noexcept {
    work_ram_.fill(0x00);
}

std::uint8_t GameGearBus::read8(std::uint16_t address) const noexcept {
    if (!ready()) {
        return 0xFF;
    }

    std::uint8_t value;
    if (address <= 0xBFFF || cartridge_->mapper().system_ram_overridden()) {
        value = cartridge_->read(address);
    } else {
        value = read_work_ram(address);
    }
    // A Game Genie sits on the bus and answers instead of the cartridge, so
    // the substitution happens here, after whatever really lives at the
    // address has answered - the check byte in a nine-digit code is compared
    // against exactly that. Null pointer when no codes are on, which is the
    // usual case and is the whole cost of the feature.
    if (cheats_ != nullptr) {
        value = cheats_->substitute(address, value);
    }
    return value;
}

void GameGearBus::write8(std::uint16_t address, std::uint8_t value) noexcept {
    if (!ready()) {
        return;
    }

    if (address <= 0xBFFF) {
        cartridge_->write(address, value);
        return;
    }

    if (!cartridge_->mapper().system_ram_overridden() || address >= MapperState::kControlRegisterAddress) {
        write_work_ram(address, value);
    }

    if (address >= MapperState::kControlRegisterAddress) {
        cartridge_->write(address, value);
    }
}

std::uint8_t GameGearBus::in(std::uint8_t port) noexcept {
    if (!ready()) {
        return 0xFF;
    }

    if (port >= 0x40 && port <= 0x7F) {
        return (port & 0x01) == 0 ? vdp_->read_v_counter() : vdp_->read_h_counter();
    }

    if (port >= 0x80 && port <= 0xBF) {
        return (port & 0x01) == 0 ? vdp_->read_data() : vdp_->read_status();
    }

    // The I/O chip decodes ranges, not exact addresses: everything from C0-FF
    // is the pair of joypad ports, even -> port A, odd -> port B. DC/DD are
    // just the addresses games conventionally use. Decoding only the exact
    // four addresses left every mirror reading FFh, a permanent "no buttons
    // pressed" - Prince of Persia reads DE.
    if (port >= 0xC0) {
        return (port & 0x01) == 0 ? system_control_->read_port(0xC0)
                                  : system_control_->read_port(0xC1);
    }

    switch (port) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
            return system_control_->read_port(port);
        default:
            return 0xFF;
    }
}

void GameGearBus::out(std::uint8_t port, std::uint8_t value) noexcept {
    if (!ready()) {
        return;
    }

    if (port >= 0x40 && port <= 0x7F) {
        psg_->write(value);
        return;
    }

    if (port >= 0x80 && port <= 0xBF) {
        if ((port & 0x01) == 0) {
            vdp_->write_data(value);
        } else {
            vdp_->write_control(value);
        }
        return;
    }

    switch (port) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x05:
        case 0x06:
            system_control_->write_port(port, value);
            break;
        default:
            break;
    }
}

std::string GameGearBus::describe() const {
    std::ostringstream out;
    out << "Bus: separates Z80 memory space from I/O space, "
        << kWorkRamSize << " bytes work RAM, "
        << (ready() ? "devices attached" : "devices detached");
    return out.str();
}

mercury::core::Z80BusCallbacks GameGearBus::make_z80_callbacks() noexcept {
    mercury::core::Z80BusCallbacks callbacks;
    callbacks.read8 = [this](std::uint16_t address) { return read8(address); };
    callbacks.write8 = [this](std::uint16_t address, std::uint8_t value) { write8(address, value); };
    callbacks.in = [this](std::uint8_t port) { return in(port); };
    callbacks.out = [this](std::uint8_t port, std::uint8_t value) { out(port, value); };
    callbacks.io_wait_cycles = [this](std::uint8_t port, bool is_write) { return io_wait_cycles(port, is_write); };
    callbacks.io_read_phase_cycles = []() { return kIoReadPhaseCycles; };
    callbacks.advance_io_timing = [this](std::uint32_t cycles) {
        if (vdp_ != nullptr) {
            vdp_->advance_cycles(cycles);
        }
        if (psg_ != nullptr) {
            psg_->advance_cycles(cycles);
        }
        // The link cable rides the same mid-instruction advance as the VDP, so
        // a game that polls the serial status port sees the transmitter clear
        // at the point in the instruction where it actually samples the bus.
        if (system_control_ != nullptr) {
            system_control_->advance_serial(cycles);
        }
    };
    return callbacks;
}

bool GameGearBus::ready() const noexcept {
    return cartridge_ != nullptr && system_control_ != nullptr && vdp_ != nullptr && psg_ != nullptr;
}

std::uint8_t GameGearBus::read_work_ram(std::uint16_t address) const noexcept {
    const auto offset = static_cast<std::size_t>(address & 0x1FFF);
    return work_ram_[offset];
}

void GameGearBus::write_work_ram(std::uint16_t address, std::uint8_t value) noexcept {
    const auto offset = static_cast<std::size_t>(address & 0x1FFF);
    work_ram_[offset] = value;
}

std::uint32_t GameGearBus::io_wait_cycles(std::uint8_t port, bool /*is_write*/) const noexcept {
    if (!ready()) {
        return 0;
    }

    if (port >= 0x80 &&
        port <= 0xBF &&
        (port & 0x01) == 0 &&
        vdp_->access_requires_wait()) {
        return kVdpActiveDisplayWaitCycles;
    }

    return 0;
}

}  // namespace mercury::gamegear
