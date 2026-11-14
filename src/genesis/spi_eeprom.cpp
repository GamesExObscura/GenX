#include "mercury/genesis/spi_eeprom.hpp"

namespace mercury::genesis {

namespace {

constexpr std::uint8_t kDataIn = 0x01;
constexpr std::uint8_t kClock = 0x02;
constexpr std::uint8_t kHoldReleased = 0x04;  // /HOLD high: the chip listens
constexpr std::uint8_t kDeselected = 0x08;    // /CS high

constexpr std::uint8_t kWriteStatus = 0x01;
constexpr std::uint8_t kWrite = 0x02;
constexpr std::uint8_t kRead = 0x03;
constexpr std::uint8_t kWriteDisable = 0x04;
constexpr std::uint8_t kReadStatus = 0x05;
constexpr std::uint8_t kWriteEnable = 0x06;

constexpr std::uint8_t kWriteEnableLatch = 0x02;
constexpr std::uint8_t kBlockProtect = 0x0C;
// Sequential writes stay inside one 128-byte page and wrap within it.
constexpr std::uint16_t kPageMask = 0x7F;

// Whether the block-protection bits allow a write to this address: the top
// quarter, the top half, or everything.
bool writable(std::uint8_t status, std::uint16_t address) noexcept {
    switch ((status & kBlockProtect) >> 2) {
        case 1: return address < 0xC000;
        case 2: return address < 0x8000;
        case 3: return false;
        default: return true;
    }
}

}  // namespace

void SpiEeprom::reset() noexcept {
    phase_ = Phase::instruction;
    clock_ = false;
    out_ = true;
    status_ = 0;
    instruction_ = 0;
    buffer_ = 0;
    address_ = 0;
    bits_ = 0;
}

void SpiEeprom::sanitize_loaded() noexcept {
    if (phase_ > Phase::read_data) {
        phase_ = Phase::standby;
    }
    bits_ &= 0x0F;
    status_ &= kWriteEnableLatch | kBlockProtect;
}

bool SpiEeprom::write(std::uint8_t lines, std::vector<std::uint8_t>& memory) noexcept {
    bool changed = false;
    const bool rising = (lines & kClock) != 0 && !clock_;
    const auto bit = static_cast<std::uint8_t>(lines & kDataIn);

    // With /HOLD low the chip ignores the bus altogether - an edge that
    // arrives meanwhile is simply lost.
    if ((lines & kHoldReleased) != 0) {
        if ((lines & kDeselected) != 0) {
            // Deselecting ends whatever was in progress and floats the output
            // high; the next selection starts with a fresh instruction.
            phase_ = Phase::instruction;
            out_ = true;
            instruction_ = 0;
            bits_ = 0;
        } else if (rising) {
            switch (phase_) {
                case Phase::instruction:
                    instruction_ = static_cast<std::uint8_t>((instruction_ << 1) | bit);
                    if (++bits_ == 8) {
                        bits_ = 0;
                        switch (instruction_) {
                            case kWriteStatus:
                                buffer_ = 0;
                                phase_ = Phase::write_data;
                                break;
                            case kWrite:
                            case kRead:
                                address_ = 0;
                                phase_ = Phase::address;
                                break;
                            case kWriteDisable:
                                status_ &= static_cast<std::uint8_t>(~kWriteEnableLatch);
                                phase_ = Phase::standby;
                                break;
                            case kReadStatus:
                                buffer_ = status_;
                                phase_ = Phase::read_data;
                                break;
                            case kWriteEnable:
                                status_ |= kWriteEnableLatch;
                                phase_ = Phase::standby;
                                break;
                            default:
                                phase_ = Phase::standby;  // not a command this part has
                                break;
                        }
                    }
                    break;

                case Phase::address:
                    address_ = static_cast<std::uint16_t>((address_ << 1) | bit);
                    if (++bits_ == 16) {
                        bits_ = 0;
                        if (instruction_ == kRead) {
                            buffer_ = memory[address_ % memory.size()];
                            phase_ = Phase::read_data;
                        } else {
                            buffer_ = 0;
                            phase_ = Phase::write_data;
                        }
                    }
                    break;

                case Phase::write_data:
                    buffer_ = static_cast<std::uint8_t>((buffer_ << 1) | bit);
                    if (++bits_ == 8) {
                        bits_ = 0;
                        if (instruction_ == kWriteStatus) {
                            // Only the protection bits are writable; the
                            // write-enable latch keeps its own state.
                            status_ = static_cast<std::uint8_t>((status_ & kWriteEnableLatch) |
                                                                (buffer_ & kBlockProtect));
                            phase_ = Phase::standby;
                        } else {
                            if ((status_ & kWriteEnableLatch) != 0 && writable(status_, address_)) {
                                auto& cell = memory[address_ % memory.size()];
                                changed = cell != buffer_;
                                cell = buffer_;
                            }
                            buffer_ = 0;
                            address_ = static_cast<std::uint16_t>((address_ & ~kPageMask) |
                                                                  ((address_ + 1) & kPageMask));
                        }
                    }
                    break;

                case Phase::read_data:
                    out_ = ((buffer_ >> (7 - bits_)) & 1) != 0;
                    if (++bits_ == 8) {
                        bits_ = 0;
                        // A memory read runs on into the next byte for as long
                        // as the clock keeps going; a status read repeats.
                        if (instruction_ == kRead) {
                            address_ = static_cast<std::uint16_t>(address_ + 1);
                            buffer_ = memory[address_ % memory.size()];
                        }
                    }
                    break;

                case Phase::standby:
                default:
                    break;  // waits for /CS to go high
            }
        }
    }
    clock_ = (lines & kClock) != 0;
    return changed;
}

}  // namespace mercury::genesis
