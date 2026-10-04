#include "mercury/genesis/eeprom.hpp"

namespace mercury::genesis {

void SerialEeprom::reset() noexcept {
    storage_.fill(0xFF);
    phase_ = Phase::idle;
    scl_ = true;
    sda_ = true;
    acknowledging_ = false;
    reading_ = false;
    shift_ = 0;
    out_bit_ = 1;
    counter_ = 0;
    address_ = 0;
    used_ = false;
}

void SerialEeprom::begin_transfer() noexcept {
    phase_ = Phase::device;
    counter_ = 0;
    shift_ = 0;
    acknowledging_ = false;
    reading_ = false;
}

void SerialEeprom::finish_byte(bool) noexcept {
    switch (phase_) {
        case Phase::device:
            // 1010xxxR. The low bit picks direction; the rest of the address is
            // not checked, because a cart only ever has the one device on it.
            reading_ = (shift_ & 0x01) != 0;
            phase_ = reading_ ? Phase::read_data : Phase::address;
            break;
        case Phase::address:
            address_ = shift_;
            phase_ = Phase::write_data;
            break;
        case Phase::write_data:
            storage_[address_ % kSize] = shift_;
            address_ = static_cast<std::uint8_t>(address_ + 1);
            break;
        case Phase::read_data:
            address_ = static_cast<std::uint8_t>(address_ + 1);
            break;
        case Phase::idle:
        default:
            break;
    }
    shift_ = 0;
}

void SerialEeprom::write_byte(std::uint8_t value) noexcept {
    used_ = true;
    const bool sda = (value & 0x01) != 0;
    const bool scl = (value & 0x02) != 0;

    if (scl_ && scl) {
        // Both edges of interest happen while the clock is held high.
        if (sda_ && !sda) {
            begin_transfer();
        } else if (!sda_ && sda) {
            phase_ = Phase::idle;
            acknowledging_ = false;
        }
    } else if (!scl_ && scl) {
        // Rising edge: the bit on the line is valid and gets sampled.
        if (phase_ != Phase::idle) {
            const bool device_talks = (phase_ == Phase::read_data);
            if (counter_ < 8) {
                if (!device_talks) {
                    shift_ = static_cast<std::uint8_t>((shift_ << 1) | (sda ? 1 : 0));
                }
                ++counter_;
            } else if (counter_ == 8) {
                ++counter_;  // this is the acknowledge clock
            }
        }
    } else if (scl_ && !scl) {
        // Falling edge. The acknowledge has to be ASSERTED here, one clock
        // after the eighth bit, and held through the ninth clock - the master
        // samples it while the clock is high, so releasing it on the ninth
        // rising edge means it is never seen.
        const bool device_talks = (phase_ == Phase::read_data);
        if (counter_ == 8) {
            acknowledging_ = !device_talks;
        } else if (counter_ == 9) {
            counter_ = 0;
            acknowledging_ = false;
            finish_byte(sda);
        }
        if (phase_ == Phase::read_data && counter_ < 8) {
            const auto byte = storage_[address_ % kSize];
            out_bit_ = static_cast<std::uint8_t>((byte >> (7 - counter_)) & 1);
        }
    }

    scl_ = scl;
    sda_ = sda;
}

bool SerialEeprom::device_drives_low() const noexcept {
    if (acknowledging_) {
        return true;
    }
    if (phase_ == Phase::read_data && counter_ < 8 && out_bit_ == 0) {
        return true;
    }
    return false;
}

std::uint8_t SerialEeprom::read_byte() const noexcept {
    // Wired-AND: the game sees a low if either side is pulling the line down.
    // Everything else on the byte floats high, which matters because some games
    // test the whole byte rather than isolating bit 0.
    const bool level = sda_ && !device_drives_low();
    return static_cast<std::uint8_t>(level ? 0xFF : 0xFE);
}

}  // namespace mercury::genesis
