#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mercury::genesis {

// Serial (I2C) EEPROM, as fitted to a number of Mega Drive carts in place of
// parallel battery RAM. Two pins live in one byte of ROM space - SDA on bit 0,
// SCL on bit 1 - and the game bit-bangs the protocol by hand.
//
// The one detail that has to be right is that I2C is an OPEN DRAIN bus: neither
// side ever drives the line high, they only pull it low against a pull-up. So
// the level a game reads back is the wired-AND of what it is driving and what
// the device is driving. That single rule covers both behaviours games rely on:
//
//   * A cart with no device, or a device that is not talking, reads back
//     whatever the game itself last wrote - which is exactly the write-a-bit,
//     read-it-back test several titles use to detect that saving is available.
//   * During an acknowledge the game releases the line (writes 1) and the
//     device pulls it low, so the game reads 0. Model the line as "whatever was
//     last written" and the acknowledge never arrives; model it as always-high
//     and the detection test fails instead.
class SerialEeprom {
public:
    static constexpr std::size_t kSize = 256;

    void reset() noexcept;

    // Called on a write to the EEPROM byte. Bit 0 is SDA, bit 1 is SCL.
    void write_byte(std::uint8_t value) noexcept;
    // The level the game reads back: the wired-AND described above.
    [[nodiscard]] std::uint8_t read_byte() const noexcept;
    [[nodiscard]] bool ever_used() const noexcept { return used_; }
    // The stored bytes, for writing a battery file out and reading one back in.
    [[nodiscard]] const std::array<std::uint8_t, kSize>& storage() const noexcept {
        return storage_;
    }
    void restore_storage(const std::uint8_t* data, std::size_t size) noexcept {
        std::memcpy(storage_.data(), data, size < kSize ? size : kSize);
        used_ = true;
    }

private:
    enum class Phase : std::uint8_t { idle, device, address, write_data, read_data };

    void begin_transfer() noexcept;
    void finish_byte(bool master_sda) noexcept;
    [[nodiscard]] bool device_drives_low() const noexcept;

  public:
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(storage_);
        ar(phase_);
        ar(scl_);
        ar(sda_);
        ar(acknowledging_);
        ar(reading_);
        ar(shift_);
        ar(out_bit_);
        ar(counter_);
        ar(address_);
        ar(used_);
    }

  private:
    std::array<std::uint8_t, kSize> storage_{};
    Phase phase_ = Phase::idle;
    bool scl_ = true;
    bool sda_ = true;
    bool acknowledging_ = false;
    bool reading_ = false;
    std::uint8_t shift_ = 0;
    std::uint8_t out_bit_ = 1;
    std::uint8_t counter_ = 0;
    std::uint8_t address_ = 0;
    bool used_ = false;
};

}  // namespace mercury::genesis
