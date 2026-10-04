#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mercury::gamegear {

class MapperState {
public:
    static constexpr std::uint16_t kControlRegisterAddress = 0xFFFC;
    static constexpr std::uint16_t kSlot0RegisterAddress = 0xFFFD;
    static constexpr std::uint16_t kSlot1RegisterAddress = 0xFFFE;
    static constexpr std::uint16_t kSlot2RegisterAddress = 0xFFFF;

    void reset();

    [[nodiscard]] bool handles_register(std::uint16_t address) const noexcept;
    [[nodiscard]] std::uint8_t read_register(std::uint16_t address) const noexcept;
    void write_register(std::uint16_t address, std::uint8_t value) noexcept;

    [[nodiscard]] std::array<std::uint8_t, 4> registers() const noexcept;
    [[nodiscard]] std::uint8_t control_register() const noexcept;
    [[nodiscard]] std::uint8_t slot_register(std::size_t slot) const noexcept;
    [[nodiscard]] std::uint8_t bank_shift() const noexcept;
    [[nodiscard]] std::uint16_t effective_rom_bank_for_slot(std::size_t slot) const noexcept;
    [[nodiscard]] std::uint8_t selected_ram_bank() const noexcept;
    [[nodiscard]] bool slot2_ram_enabled() const noexcept;
    [[nodiscard]] bool system_ram_overridden() const noexcept;
    [[nodiscard]] bool rom_write_enabled() const noexcept;
    [[nodiscard]] std::string describe() const;

private:
    void refresh_effective_slots() noexcept;

  public:
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(control_);
        ar(slots_);
        ar(effective_slots_);
    }

  private:
    std::uint8_t control_ = 0x00;
    std::array<std::uint8_t, 3> slots_{0x00, 0x01, 0x02};
    std::array<std::uint16_t, 3> effective_slots_{0x00, 0x01, 0x02};
};

}  // namespace mercury::gamegear
