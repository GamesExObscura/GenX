#pragma once

#include <cstddef>
#include <cstdint>

namespace mercury::gamegear {

// THE 93C46 ON SEGA'S GAME GEAR BASEBALL CARTRIDGES.
//
// World Series Baseball, World Series Baseball '95 and The Majors: Pro
// Baseball save to a 93C46 serial EEPROM - 64 words of 16 bits - instead of
// battery RAM. The game bit-bangs it through one address:
//
//   write $8000   bit 0 = data in, bit 1 = clock, bit 2 = chip select
//   read  $8000   bit 0 = data out, bit 1 reads 1, bit 2 = chip select
//
// and the cartridge's $FFFC register is the chip's control, not the Sega
// mapper's: bit 3 puts the chip on $8000, bit 7 resets its serial logic.
// Board wiring and register meanings checked against Genesis Plus GX
// (cart_hw/eeprom_93c.c, sms_cart.c) - read for the facts, not copied. The
// command set is the standard Microwire 93C46 one.
//
// A command is a start bit (data in high on a rising clock while selected),
// two opcode bits and six address bits, most significant first:
//
//   10 aaaaaa  READ   a dummy 0, then the word, then the following words
//   01 aaaaaa  WRITE  sixteen data bits follow
//   11 aaaaaa  ERASE  the word becomes FFFF
//   00 11xxxx  EWEN   allow writes      00 00xxxx  EWDS  forbid writes
//   00 10xxxx  ERAL   erase everything  00 01xxxx  WRAL  write every word
//
// Writes and erases complete at once: data out reads 1 (ready) as soon as the
// chip is selected again, so a game that waits for ready never waits.
//
// The chip owns no memory of its own. It works on the 128 bytes it is handed
// - the cartridge's battery store - so the save file, save states and netplay
// carry the contents with nothing added. Word n is bytes 2n (low) and 2n+1.
class SerialEeprom {
public:
    static constexpr std::size_t kBytes = 128;
    static constexpr std::uint8_t kDataIn = 0x01;
    static constexpr std::uint8_t kClock = 0x02;
    static constexpr std::uint8_t kSelect = 0x04;

    // Power-on and $FFFC bit 7: serial logic idle, writes forbidden. The
    // contents are untouched - that is the whole point of the chip.
    void reset() noexcept;

    // The game drives the three input lines. Returns true when the stored
    // words changed, so the host knows the save needs writing out.
    bool write_lines(std::uint8_t lines, std::uint8_t* memory) noexcept;
    [[nodiscard]] std::uint8_t read_lines() const noexcept;

    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(phase_);
        ar(select_);
        ar(clock_);
        ar(data_out_);
        ar(write_enabled_);
        ar(command_);
        ar(bits_);
        ar(shift_);
        if constexpr (Archive::loading()) {
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else (netplay, shared files). The
    // phase and bit count are brought back into range; addresses are masked
    // at every use, so no value can reach past the 128 bytes.
    void sanitize_loaded() noexcept;

    // How the game has used the chip, for the probe. Not part of the state:
    // it is diagnostics, and it would make two otherwise identical machines
    // hash differently.
    struct Activity {
        std::uint32_t reads = 0;
        std::uint32_t writes = 0;
        std::uint32_t erases = 0;
        std::uint32_t enables = 0;
        std::uint32_t disables = 0;
        std::uint32_t erase_all = 0;
        std::uint32_t write_all = 0;
    };
    [[nodiscard]] const Activity& activity() const noexcept { return activity_; }

private:
    // Plain bytes rather than an enum class, so the archive stores them as-is.
    enum Phase : std::uint8_t {
        kIdle = 0,      // waiting for a start bit
        kCommand = 1,   // collecting opcode and address
        kWriting = 2,   // collecting sixteen data bits
        kReading = 3,   // shifting words out
        kDone = 4,      // command finished; waits for select to drop
    };

    bool clock_in(bool bit, std::uint8_t* memory) noexcept;
    [[nodiscard]] std::uint8_t address() const noexcept { return command_ & 0x3F; }

    std::uint8_t phase_ = kIdle;
    bool select_ = false;
    bool clock_ = false;
    bool data_out_ = true;
    bool write_enabled_ = false;
    std::uint8_t command_ = 0;
    std::uint8_t bits_ = 0;
    std::uint16_t shift_ = 0;
    Activity activity_{};
};

}  // namespace mercury::gamegear
