#include "mercury/gamegear/serial_eeprom.hpp"

namespace mercury::gamegear {

namespace {

std::uint16_t load_word(const std::uint8_t* memory, std::uint8_t word) noexcept {
    const std::size_t at = static_cast<std::size_t>(word & 0x3F) * 2;
    return static_cast<std::uint16_t>(memory[at] | (memory[at + 1] << 8));
}

// True when the word actually changed.
bool store_word(std::uint8_t* memory, std::uint8_t word, std::uint16_t value) noexcept {
    const std::size_t at = static_cast<std::size_t>(word & 0x3F) * 2;
    const auto low = static_cast<std::uint8_t>(value & 0xFF);
    const auto high = static_cast<std::uint8_t>(value >> 8);
    const bool changed = memory[at] != low || memory[at + 1] != high;
    memory[at] = low;
    memory[at + 1] = high;
    return changed;
}

}  // namespace

void SerialEeprom::reset() noexcept {
    phase_ = kIdle;
    select_ = false;
    clock_ = false;
    data_out_ = true;
    write_enabled_ = false;
    command_ = 0;
    bits_ = 0;
    shift_ = 0;
}

bool SerialEeprom::write_lines(std::uint8_t lines, std::uint8_t* memory) noexcept {
    const bool select = (lines & kSelect) != 0;
    const bool clock = (lines & kClock) != 0;
    bool changed = false;
    if (!select) {
        // Deselecting ends whatever command was in progress and puts the chip
        // in standby, where data out reads ready.
        if (select_) {
            phase_ = kIdle;
            data_out_ = true;
        }
    } else if (clock && !clock_) {
        // Everything happens on the rising edge, while selected.
        changed = clock_in((lines & kDataIn) != 0, memory);
    }
    select_ = select;
    clock_ = clock;
    return changed;
}

std::uint8_t SerialEeprom::read_lines() const noexcept {
    // Bit 1 reads high on this board whatever the game last wrote.
    return static_cast<std::uint8_t>((select_ ? kSelect : 0) | kClock | (data_out_ ? kDataIn : 0));
}

bool SerialEeprom::clock_in(bool bit, std::uint8_t* memory) noexcept {
    switch (phase_) {
        case kIdle:
            if (bit) {
                command_ = 0;
                bits_ = 0;
                phase_ = kCommand;
            }
            return false;

        case kCommand: {
            command_ = static_cast<std::uint8_t>((command_ << 1) | (bit ? 1 : 0));
            if (++bits_ < 8) {
                return false;
            }
            bits_ = 0;
            switch (command_ >> 6) {
                case 2:  // READ: a dummy zero first, then the word from the top
                    ++activity_.reads;
                    shift_ = load_word(memory, address());
                    data_out_ = false;
                    phase_ = kReading;
                    return false;
                case 1:  // WRITE
                    ++activity_.writes;
                    shift_ = 0;
                    phase_ = kWriting;
                    return false;
                case 3: {  // ERASE
                    ++activity_.erases;
                    phase_ = kDone;
                    return write_enabled_ && store_word(memory, address(), 0xFFFF);
                }
                default:
                    break;
            }
            // Opcode 00: the next two bits pick the command.
            switch ((command_ >> 4) & 3) {
                case 3:  // EWEN
                    ++activity_.enables;
                    write_enabled_ = true;
                    phase_ = kDone;
                    return false;
                case 0:  // EWDS
                    ++activity_.disables;
                    write_enabled_ = false;
                    phase_ = kDone;
                    return false;
                case 2: {  // ERAL
                    ++activity_.erase_all;
                    phase_ = kDone;
                    bool any = false;
                    if (write_enabled_) {
                        for (std::uint8_t word = 0; word < 64; ++word) {
                            any = store_word(memory, word, 0xFFFF) || any;
                        }
                    }
                    return any;
                }
                default:  // WRAL
                    ++activity_.write_all;
                    shift_ = 0;
                    phase_ = kWriting;
                    return false;
            }
        }

        case kWriting: {
            shift_ = static_cast<std::uint16_t>((shift_ << 1) | (bit ? 1 : 0));
            if (++bits_ < 16) {
                return false;
            }
            phase_ = kDone;
            if (!write_enabled_) {
                return false;
            }
            if ((command_ >> 6) == 1) {
                return store_word(memory, address(), shift_);
            }
            bool any = false;
            for (std::uint8_t word = 0; word < 64; ++word) {
                any = store_word(memory, word, shift_) || any;
            }
            return any;
        }

        case kReading:
            data_out_ = (shift_ & 0x8000) != 0;
            shift_ = static_cast<std::uint16_t>(shift_ << 1);
            if (++bits_ == 16) {
                // Holding select and clocking on reads the next word, wrapping
                // at the end of the chip.
                bits_ = 0;
                command_ = static_cast<std::uint8_t>((command_ & 0xC0) | ((command_ + 1) & 0x3F));
                shift_ = load_word(memory, address());
            }
            return false;

        default:
            return false;
    }
}

void SerialEeprom::sanitize_loaded() noexcept {
    if (phase_ > kDone) {
        phase_ = kIdle;
    }
    bits_ = static_cast<std::uint8_t>(bits_ & 0x0F);
}

}  // namespace mercury::gamegear
