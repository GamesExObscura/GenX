#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mercury/core/rom_image.hpp"
#include "mercury/gamegear/mapper_state.hpp"
#include "mercury/gamegear/serial_eeprom.hpp"

namespace mercury::gamegear {

class Cartridge {
public:
    static constexpr std::size_t kBankSize = 16 * 1024;
    static constexpr std::size_t kCodemastersRamSize = 8 * 1024;
    // Sega mapper supports up to 32 KB of battery-backed cartridge RAM,
    // exposed as two 16 KB banks at 8000-BFFF.
    static constexpr std::size_t kCartRamSize = 32 * 1024;

    // Only two mappers were ever official. Sega's uses the registers at
    // FFFC-FFFF; Codemasters' own cartridges instead latch banks from writes
    // to 0000/4000/8000 and have no fixed first kilobyte.
    enum class MapperKind {
        sega,
        codemasters,
    };

    bool load(mercury::core::RomImage image);
    void reset();

    [[nodiscard]] bool loaded() const noexcept;
    // The cartridge's bytes as loaded, for anything that has to identify the
    // game itself rather than its file name - netplay's fingerprint, so that
    // two people cannot start different cartridges and only find out when the
    // games drift apart.
    [[nodiscard]] const std::vector<std::uint8_t>& rom() const noexcept { return rom_.bytes; }
    [[nodiscard]] std::size_t rom_size_bytes() const noexcept;
    [[nodiscard]] std::size_t rom_bank_count() const noexcept;
    [[nodiscard]] const MapperState& mapper() const noexcept;
    [[nodiscard]] MapperKind mapper_kind() const noexcept { return kind_; }

    // The Codemasters header's checksum word at 0x7FE6, or zero on a cartridge
    // that has no such header. It is a checksum of the ROM contents, so it is
    // the same in every dump of a given release and different between
    // releases - which makes it a usable identity for the handful of
    // per-cartridge behaviours that need to know WHICH game is in the slot.
    [[nodiscard]] std::uint16_t codemasters_checksum() const noexcept {
        return codemasters_checksum_;
    }

    [[nodiscard]] std::uint8_t read(std::uint16_t address) const noexcept;
    void write(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::array<std::uint8_t, 4> mapper_registers() const noexcept;
    [[nodiscard]] std::array<std::size_t, 3> visible_banks() const noexcept;
    [[nodiscard]] std::string describe() const;

private:
    [[nodiscard]] std::uint8_t read_rom(std::uint16_t bank, std::uint16_t offset) const noexcept;
    [[nodiscard]] static MapperKind detect_mapper(const mercury::core::RomImage& image) noexcept;
    [[nodiscard]] static bool detect_serial_eeprom(const mercury::core::RomImage& image) noexcept;

  public:
    // Cartridge RAM is battery-backed on the carts that have it, so it survives
    // a power cycle on hardware and has to reach a file here. cart_ram_ is
    // already deliberately not cleared on reset for the same reason.
    [[nodiscard]] bool battery_dirty() const noexcept { return battery_dirty_; }
    void clear_battery_dirty() noexcept { battery_dirty_ = false; }
    [[nodiscard]] const std::array<std::uint8_t, kCartRamSize>& battery_bytes() const noexcept {
        return cart_ram_;
    }
    void restore_battery(const std::uint8_t* data, std::size_t size) noexcept {
        std::memcpy(cart_ram_.data(), data, size < kCartRamSize ? size : kCartRamSize);
        battery_dirty_ = false;
    }

    // rom_ and kind_ come from the file that was loaded, not from a state; the
    // banking registers and the cartridge's own RAM are what change as it runs.
    template <typename Archive>
    void visit_state(Archive& ar) {
        mapper_.visit_state(ar);
        ar(codemasters_banks_);
        ar(codemasters_ram_enabled_);
        ar(codemasters_ram_);
        ar(cart_ram_);
    }
    // The serial EEPROM's own logic, added in Game Gear state version 4 and
    // kept apart so it can go at the very end of the state, where version 3
    // files simply stop. Its stored words are already in cart_ram_ above.
    template <typename Archive>
    void visit_eeprom_state(Archive& ar) {
        eeprom_.visit_state(ar);
        ar(eeprom_selected_);
    }
    // A version 3 state has no EEPROM logic in it: start that from idle.
    void reset_eeprom_logic() noexcept {
        eeprom_.reset();
        eeprom_selected_ = false;
    }
    [[nodiscard]] bool uses_serial_eeprom() const noexcept { return uses_serial_eeprom_; }
    [[nodiscard]] const SerialEeprom& serial_eeprom() const noexcept { return eeprom_; }

  private:
    mercury::core::RomImage rom_;
    MapperState mapper_;
    MapperKind kind_ = MapperKind::sega;
    // Codemasters latches: slots 0/1/2 power up as banks 0/1/0.
    std::array<std::uint8_t, 3> codemasters_banks_{0x00, 0x01, 0x00};
    bool codemasters_ram_enabled_ = false;
    std::array<std::uint8_t, kCodemastersRamSize> codemasters_ram_{};
    std::array<std::uint8_t, kCartRamSize> cart_ram_{};
    bool battery_dirty_ = false;
    // A handful of baseball carts fit a 93C46 serial EEPROM instead of
    // battery SRAM (see serial_eeprom.hpp). On those, $FFFC is the chip's
    // control register rather than the Sega mapper's, so they never page
    // cartridge RAM in. The chip's 128 bytes live at the start of cart_ram_.
    bool uses_serial_eeprom_ = false;
    SerialEeprom eeprom_;
    // $FFFC bit 3: the chip answers at $8000.
    bool eeprom_selected_ = false;
    std::uint16_t codemasters_checksum_ = 0;
};

}  // namespace mercury::gamegear
