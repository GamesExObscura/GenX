#include "mercury/gamegear/cartridge.hpp"

#include <sstream>
#include <utility>

namespace mercury::gamegear {

Cartridge::MapperKind Cartridge::detect_mapper(const mercury::core::RomImage& image) noexcept {
    // Codemasters cartridges carry their own header at 0x7FE0 because the
    // stock BIOS checksum routine cannot cope with their mapper. The two
    // 16-bit words at 0x7FE6 and 0x7FE8 are a checksum and its complement, so
    // they sum to zero; Sega-mapper ROMs leave the whole area blank, which is
    // why a non-zero checksum is required as well. Verified across a 225-ROM
    // library: this matches exactly the four Codemasters titles and nothing
    // else.
    if (image.bytes.size() < 0x8000) {
        return MapperKind::sega;
    }

    const auto word_at = [&image](std::size_t offset) {
        return static_cast<std::uint16_t>(image.bytes[offset] |
                                          (static_cast<std::uint16_t>(image.bytes[offset + 1]) << 8));
    };

    const auto checksum = word_at(0x7FE6);
    const auto complement = word_at(0x7FE8);
    if (checksum != 0 &&
        static_cast<std::uint16_t>(checksum + complement) == 0) {
        return MapperKind::codemasters;
    }

    return MapperKind::sega;
}

bool Cartridge::detect_serial_eeprom(const mercury::core::RomImage& image) noexcept {
    // These carts carry a 93C46 (128 byte, 3-wire serial) chip rather than
    // battery-backed SRAM. The game bit-bangs a clock and data line through
    // writes to 0x8000 and reads the response back; presenting plain RAM
    // there makes it read its own clock line and hang before booting.
    // Matched on the Sega header product code at 0x7FFC-0x7FFE.
    if (image.bytes.size() < 0x8000) {
        return false;
    }

    const auto product = static_cast<std::uint32_t>(image.bytes[0x7FFC]) |
                         (static_cast<std::uint32_t>(image.bytes[0x7FFD]) << 8) |
                         (static_cast<std::uint32_t>(image.bytes[0x7FFE]) << 16);

    switch (product) {
        case 0x012439:  // World Series Baseball
        case 0x002537:  // World Series Baseball '95
        case 0x002418:  // The Majors - Pro Baseball
            return true;
        default:
            return false;
    }
}

bool Cartridge::load(mercury::core::RomImage image) {
    if (image.empty()) {
        return false;
    }

    rom_ = std::move(image);
    kind_ = detect_mapper(rom_);
    uses_serial_eeprom_ = detect_serial_eeprom(rom_);
    codemasters_checksum_ =
        kind_ == MapperKind::codemasters
            ? static_cast<std::uint16_t>(rom_.bytes[0x7FE6] |
                                         (static_cast<std::uint16_t>(rom_.bytes[0x7FE7]) << 8))
            : 0;
    // Uninitialised cartridge RAM reads as 0xFF on hardware - a cart with no
    // RAM chip fitted floats the bus, and blank battery RAM reads the same.
    // Zero-filling here changes what games see before they write anything:
    // World Series Baseball enables slot-2 RAM and reads it back, and hangs
    // if those reads come back 0x00.
    cart_ram_.fill(0xFF);
    reset();
    return true;
}

void Cartridge::reset() {
    mapper_.reset();
    codemasters_banks_ = {0x00, 0x01, 0x00};
    codemasters_ram_enabled_ = false;
    codemasters_ram_.fill(0x00);
    // Note: cart_ram_ is deliberately NOT cleared on reset - it models
    // battery-backed save RAM, which survives a power cycle. The same goes for
    // the EEPROM's words, which live in it; only the chip's logic restarts.
    reset_eeprom_logic();
}

bool Cartridge::loaded() const noexcept {
    return !rom_.empty();
}

std::size_t Cartridge::rom_size_bytes() const noexcept {
    return rom_.size_bytes();
}

std::size_t Cartridge::rom_bank_count() const noexcept {
    if (!loaded()) {
        return 0;
    }

    return (rom_size_bytes() + kBankSize - 1) / kBankSize;
}

const MapperState& Cartridge::mapper() const noexcept {
    return mapper_;
}

std::uint8_t Cartridge::read(std::uint16_t address) const noexcept {
    if (!loaded()) {
        return 0xFF;
    }

    if (kind_ == MapperKind::codemasters) {
        // No fixed first kilobyte here: slot 0 is banked in full.
        if (address <= 0x3FFF) {
            return read_rom(codemasters_banks_[0], address);
        }
        if (address <= 0x7FFF) {
            return read_rom(codemasters_banks_[1], static_cast<std::uint16_t>(address - 0x4000));
        }
        if (address <= 0xBFFF) {
            // Optional 8 KB of on-cart RAM overlays the top half of slot 2.
            if (codemasters_ram_enabled_ && address >= 0xA000) {
                return codemasters_ram_[static_cast<std::size_t>(address - 0xA000)];
            }
            return read_rom(codemasters_banks_[2], static_cast<std::uint16_t>(address - 0x8000));
        }
        return 0xFF;
    }

    if (address <= 0x03FF) {
        return read_rom(0, address);
    }

    if (address <= 0x3FFF) {
        return read_rom(mapper_.effective_rom_bank_for_slot(0), static_cast<std::uint16_t>(address - 0x0000));
    }

    if (address <= 0x7FFF) {
        return read_rom(mapper_.effective_rom_bank_for_slot(1), static_cast<std::uint16_t>(address - 0x4000));
    }

    if (address <= 0xBFFF) {
        // The EEPROM answers at $8000 alone; the rest of slot 2 stays ROM.
        if (uses_serial_eeprom_ && eeprom_selected_ && address == 0x8000) {
            return eeprom_.read_lines();
        }
        // Never true on an EEPROM cart: its $FFFC writes never reach the mapper.
        if (mapper_.slot2_ram_enabled()) {
            // Battery-backed cartridge RAM overrides ROM banking in slot 2.
            const auto offset = static_cast<std::size_t>(mapper_.selected_ram_bank()) * kBankSize +
                                static_cast<std::size_t>(address - 0x8000);
            return offset < cart_ram_.size() ? cart_ram_[offset] : 0xFF;
        }
        return read_rom(mapper_.effective_rom_bank_for_slot(2), static_cast<std::uint16_t>(address - 0x8000));
    }

    return 0xFF;
}

void Cartridge::write(std::uint16_t address, std::uint8_t value) noexcept {
    if (kind_ == MapperKind::codemasters) {
        switch (address) {
            case 0x0000:
                codemasters_banks_[0] = value;
                return;
            case 0x4000:
                // Bit 7 of the slot 1 latch pages the on-cart RAM in over
                // A000-BFFF; the low bits still select the ROM bank.
                codemasters_ram_enabled_ = (value & 0x80) != 0;
                codemasters_banks_[1] = static_cast<std::uint8_t>(value & 0x7F);
                return;
            case 0x8000:
                codemasters_banks_[2] = value;
                return;
            default:
                break;
        }

        if (codemasters_ram_enabled_ && address >= 0xA000 && address <= 0xBFFF) {
            codemasters_ram_[static_cast<std::size_t>(address - 0xA000)] = value;
        }
        return;
    }

    // On an EEPROM cart, $FFFC is the chip's control register (bit 3 selects
    // it at $8000, bit 7 resets its logic) and $8000 is its three wires.
    // $FFFD-$FFFF are the ordinary Sega bank registers.
    if (uses_serial_eeprom_) {
        if (address == MapperState::kControlRegisterAddress) {
            eeprom_selected_ = (value & 0x08) != 0;
            if ((value & 0x80) != 0) {
                eeprom_.reset();
            }
            return;
        }
        if (address == 0x8000 && eeprom_selected_) {
            if (eeprom_.write_lines(value, cart_ram_.data())) {
                battery_dirty_ = true;
            }
            return;
        }
    }

    // Writes into slot 2 land in cartridge RAM whenever it is paged in.
    if (address >= 0x8000 && address <= 0xBFFF && mapper_.slot2_ram_enabled()) {
        const auto offset = static_cast<std::size_t>(mapper_.selected_ram_bank()) * kBankSize +
                            static_cast<std::size_t>(address - 0x8000);
        if (offset < cart_ram_.size()) {
            cart_ram_[offset] = value;
            battery_dirty_ = true;
        }
        return;
    }

    if (mapper_.handles_register(address)) {
        mapper_.write_register(address, value);
    }
}

std::array<std::uint8_t, 4> Cartridge::mapper_registers() const noexcept {
    return mapper_.registers();
}

std::array<std::size_t, 3> Cartridge::visible_banks() const noexcept {
    const auto bank_count = rom_bank_count();
    if (bank_count == 0) {
        return {0, 0, 0};
    }

    const auto normalize_bank = [bank_count](std::uint16_t value) -> std::size_t {
        return static_cast<std::size_t>(value % bank_count);
    };

    if (kind_ == MapperKind::codemasters) {
        return {
            normalize_bank(codemasters_banks_[0]),
            normalize_bank(codemasters_banks_[1]),
            normalize_bank(codemasters_banks_[2]),
        };
    }

    return {
        normalize_bank(mapper_.effective_rom_bank_for_slot(0)),
        normalize_bank(mapper_.effective_rom_bank_for_slot(1)),
        normalize_bank(mapper_.effective_rom_bank_for_slot(2)),
    };
}

std::string Cartridge::describe() const {
    std::ostringstream out;
    out << "Cartridge: ";

    if (!loaded()) {
        out << "no ROM loaded";
        return out.str();
    }

    const auto banks = visible_banks();

    out << rom_size_bytes() << " bytes, "
        << rom_bank_count() << " logical banks, "
        << "visible banks [" << banks[0] << ", " << banks[1] << ", " << banks[2] << "], "
        << mapper_.describe();
    return out.str();
}

std::uint8_t Cartridge::read_rom(std::uint16_t bank, std::uint16_t offset) const noexcept {
    const auto bank_count = rom_bank_count();
    if (bank_count == 0) {
        return 0xFF;
    }

    const auto normalized_bank = static_cast<std::size_t>(bank % bank_count);
    const auto rom_index = normalized_bank * kBankSize + offset;

    if (rom_index >= rom_.bytes.size()) {
        return 0xFF;
    }

    return rom_.bytes[rom_index];
}

}  // namespace mercury::gamegear
