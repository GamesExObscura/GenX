#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mercury::genesis {

// The 64KB SPI serial EEPROM (a 25-series part) on WaterMelon's T-5740
// cartridge board - Pier Solar's save memory. Unlike the I2C EEPROMs some
// Sega carts use, SPI has separate lines in each direction, so there is no
// wired-AND to model: the game drives four lines through one register and
// reads the chip's output on another.
//
//   write: bit 0 data in, bit 1 clock, bit 2 /HOLD, bit 3 /CS (both active low)
//   read:  bit 0 data out
//
// Everything is clocked on the rising edge and shifted most significant bit
// first: an 8-bit instruction, then for READ and WRITE a 16-bit address, then
// data. The game reads each output bit after the rising edge that presents
// it, the way Genesis Plus GX models the part, which runs the game.
//
// The memory array is not kept here. The board's 64KB lives in the
// console's battery RAM buffer, so it reaches the .srm file and save states
// by the same road as every other cartridge's save.
class SpiEeprom {
public:
    static constexpr std::size_t kSize = 0x10000;

    // Power-on: deselected, output high, waiting for an instruction.
    void reset() noexcept;
    // One write to the control register. Returns true when the memory array
    // changed, so the host knows the save needs writing out.
    bool write(std::uint8_t lines, std::vector<std::uint8_t>& memory) noexcept;
    [[nodiscard]] bool data_out() const noexcept { return out_; }

    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(phase_);
        ar(clock_);
        ar(out_);
        ar(status_);
        ar(instruction_);
        ar(buffer_);
        ar(address_);
        ar(bits_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    void sanitize_loaded() noexcept;

private:
    enum class Phase : std::uint8_t { standby, instruction, address, write_data, read_data };

    Phase phase_ = Phase::instruction;
    bool clock_ = false;
    bool out_ = true;
    // Status register: bit 1 write enable latch, bits 2-3 block protection.
    std::uint8_t status_ = 0;
    std::uint8_t instruction_ = 0;
    std::uint8_t buffer_ = 0;
    std::uint16_t address_ = 0;
    std::uint8_t bits_ = 0;
};

}  // namespace mercury::genesis
