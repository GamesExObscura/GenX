#include "mercury/gamegear/mapper_state.hpp"

#include <sstream>

namespace mercury::gamegear {

namespace {

constexpr std::uint8_t kBankShiftMask = 0x03;
constexpr std::uint8_t kRamBankSelectMask = 0x04;
constexpr std::uint8_t kSlot2RamEnableMask = 0x08;
constexpr std::uint8_t kSystemRamOverrideMask = 0x10;
constexpr std::uint8_t kRomWriteEnableMask = 0x80;

}  // namespace

void MapperState::refresh_effective_slots() noexcept {
    const auto shift = static_cast<std::uint16_t>(bank_shift());
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        effective_slots_[index] =
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(slots_[index]) + shift);
    }
}

void MapperState::reset() {
    control_ = 0x00;
    slots_ = {0x00, 0x01, 0x02};
    effective_slots_ = {0x00, 0x01, 0x02};
}

bool MapperState::handles_register(std::uint16_t address) const noexcept {
    return address >= kControlRegisterAddress && address <= kSlot2RegisterAddress;
}

std::uint8_t MapperState::read_register(std::uint16_t address) const noexcept {
    switch (address) {
        case kControlRegisterAddress:
            return control_;
        case kSlot0RegisterAddress:
            return slots_[0];
        case kSlot1RegisterAddress:
            return slots_[1];
        case kSlot2RegisterAddress:
            return slots_[2];
        default:
            return 0xFF;
    }
}

void MapperState::write_register(std::uint16_t address, std::uint8_t value) noexcept {
    switch (address) {
        case kControlRegisterAddress:
            control_ = value;
            refresh_effective_slots();
            break;
        case kSlot0RegisterAddress:
            slots_[0] = value;
            refresh_effective_slots();
            break;
        case kSlot1RegisterAddress:
            slots_[1] = value;
            refresh_effective_slots();
            break;
        case kSlot2RegisterAddress:
            slots_[2] = value;
            refresh_effective_slots();
            break;
        default:
            break;
    }
}

std::array<std::uint8_t, 4> MapperState::registers() const noexcept {
    return {control_, slots_[0], slots_[1], slots_[2]};
}

std::uint8_t MapperState::control_register() const noexcept {
    return control_;
}

std::uint8_t MapperState::slot_register(std::size_t slot) const noexcept {
    return slot < slots_.size() ? slots_[slot] : 0xFF;
}

std::uint8_t MapperState::bank_shift() const noexcept {
    switch (control_ & kBankShiftMask) {
        case 0x01:
            return 0x18;
        case 0x02:
            return 0x10;
        case 0x03:
            return 0x08;
        default:
            return 0x00;
    }
}

std::uint16_t MapperState::effective_rom_bank_for_slot(std::size_t slot) const noexcept {
    return slot < effective_slots_.size() ? effective_slots_[slot] : 0xFFFF;
}

std::uint8_t MapperState::selected_ram_bank() const noexcept {
    return (control_ & kRamBankSelectMask) != 0 ? 1 : 0;
}

bool MapperState::slot2_ram_enabled() const noexcept {
    return (control_ & kSlot2RamEnableMask) != 0;
}

bool MapperState::system_ram_overridden() const noexcept {
    return (control_ & kSystemRamOverrideMask) != 0;
}

bool MapperState::rom_write_enabled() const noexcept {
    return (control_ & kRomWriteEnableMask) != 0;
}

std::string MapperState::describe() const {
    std::ostringstream out;
    out << "Mapper: ctrl="
        << static_cast<int>(control_)
        << ", slot banks ["
        << effective_rom_bank_for_slot(0) << ", "
        << effective_rom_bank_for_slot(1) << ", "
        << effective_rom_bank_for_slot(2) << "]"
        << ", slot2 RAM=" << (slot2_ram_enabled() ? "on" : "off")
        << ", system RAM override=" << (system_ram_overridden() ? "on" : "off");
    return out.str();
}

}  // namespace mercury::gamegear
