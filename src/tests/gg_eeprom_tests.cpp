// Game Gear baseball cartridges: the 93C46 serial EEPROM, driven the way the
// games drive it - through the cartridge, one wire change per write to $8000.
//
// What has to hold, and why each one matters:
//   - a word written with writes enabled reads back, MSB first, after the
//     dummy zero - that is the whole save;
//   - writes are refused until EWEN and again after EWDS, or a stray command
//     at power-on could scribble over a season;
//   - $FFFC bit 7 resets the logic and forbids writes, but keeps the words;
//   - only $8000 is the chip: the rest of slot 2 stays ROM, and $FFFC never
//     reaches the Sega mapper on these carts;
//   - a cartridge WITHOUT the chip still pages battery RAM in with $FFFC
//     bit 3, exactly as before;
//   - a save state taken in the middle of a read resumes mid-read.

#include "mercury/core/save_state.hpp"
#include "mercury/gamegear/cartridge.hpp"

#include <cstdio>
#include <vector>

namespace {

using mercury::gamegear::Cartridge;
using mercury::gamegear::SerialEeprom;

int failures = 0;

void check(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void check_word(const char* what, unsigned actual, unsigned expected) {
    if (actual != expected) {
        std::printf("FAIL %-52s got %04X, expected %04X\n", what, actual, expected);
        ++failures;
        return;
    }
    std::printf("ok   %-52s %04X\n", what, actual);
}

// 128 KB, every byte of bank n reading n, with the given product code in the
// Sega header - which is how the EEPROM carts are recognised.
mercury::core::RomImage make_rom(std::uint32_t product) {
    mercury::core::RomImage image;
    image.bytes.resize(8 * Cartridge::kBankSize);
    for (std::size_t i = 0; i < image.bytes.size(); ++i) {
        image.bytes[i] = static_cast<std::uint8_t>(i / Cartridge::kBankSize);
    }
    image.bytes[0x7FFC] = static_cast<std::uint8_t>(product & 0xFF);
    image.bytes[0x7FFD] = static_cast<std::uint8_t>((product >> 8) & 0xFF);
    image.bytes[0x7FFE] = static_cast<std::uint8_t>((product >> 16) & 0xFF);
    return image;
}

constexpr std::uint8_t kCs = SerialEeprom::kSelect;
constexpr std::uint8_t kClk = SerialEeprom::kClock;
constexpr std::uint8_t kDi = SerialEeprom::kDataIn;

// One bit: clock low with the data set up, then the rising edge. Returns data
// out as the game would read it straight after the edge.
int clock_bit(Cartridge& cart, int bit) {
    const auto di = static_cast<std::uint8_t>(bit ? kDi : 0);
    cart.write(0x8000, static_cast<std::uint8_t>(kCs | di));
    cart.write(0x8000, static_cast<std::uint8_t>(kCs | kClk | di));
    return cart.read(0x8000) & kDi;
}

void deselect(Cartridge& cart) { cart.write(0x8000, 0); }

void command(Cartridge& cart, int opcode, int address) {
    clock_bit(cart, 1);  // start
    for (int i = 1; i >= 0; --i) {
        clock_bit(cart, (opcode >> i) & 1);
    }
    for (int i = 5; i >= 0; --i) {
        clock_bit(cart, (address >> i) & 1);
    }
}

unsigned read_word_bits(Cartridge& cart) {
    unsigned value = 0;
    for (int i = 0; i < 16; ++i) {
        value = (value << 1) | static_cast<unsigned>(clock_bit(cart, 0));
    }
    return value;
}

void write_word(Cartridge& cart, int address, unsigned value) {
    command(cart, 1, address);
    for (int i = 15; i >= 0; --i) {
        clock_bit(cart, (value >> i) & 1);
    }
    deselect(cart);
}

void enable_writes(Cartridge& cart) {
    command(cart, 0, 0x30);
    deselect(cart);
}

void disable_writes(Cartridge& cart) {
    command(cart, 0, 0x00);
    deselect(cart);
}

unsigned stored(const Cartridge& cart, int address) {
    const auto& bytes = cart.battery_bytes();
    return static_cast<unsigned>(bytes[address * 2] | (bytes[address * 2 + 1] << 8));
}

unsigned read_word(Cartridge& cart, int address, int* dummy = nullptr) {
    // The dummy zero comes out on the edge that clocks the last address bit.
    clock_bit(cart, 1);
    clock_bit(cart, 1);
    clock_bit(cart, 0);
    int last = 0;
    for (int i = 5; i >= 0; --i) {
        last = clock_bit(cart, (address >> i) & 1);
    }
    if (dummy != nullptr) {
        *dummy = last;
    }
    const auto value = read_word_bits(cart);
    deselect(cart);
    return value;
}

}  // namespace

int main() {
    Cartridge cart;
    check("World Series Baseball header is recognised", cart.load(make_rom(0x012439)) &&
                                                             cart.uses_serial_eeprom());

    // Before $FFFC bit 3, $8000 is plain ROM (bank 2 reads 2).
    check_word("$8000 is ROM until the chip is selected", cart.read(0x8000), 0x02);
    cart.write(0xFFFC, 0x08);
    check_word("selected and idle: select low, clock 1, ready", cart.read(0x8000), 0x03);
    check_word("rest of slot 2 is still ROM", cart.read(0x8001), 0x02);
    check_word("so is the top of slot 2", cart.read(0xBFFF), 0x02);

    // Blank chip reads FFFF.
    int dummy = 1;
    check_word("blank word 5", read_word(cart, 5, &dummy), 0xFFFF);
    check("READ starts with a dummy zero", dummy == 0);

    // Writes refused before EWEN.
    write_word(cart, 5, 0x1234);
    check_word("WRITE before EWEN is refused", stored(cart, 5), 0xFFFF);
    check("a refused write leaves the save clean", !cart.battery_dirty());

    enable_writes(cart);
    write_word(cart, 5, 0xBEEF);
    check_word("WRITE after EWEN lands (stored low byte first)", stored(cart, 5), 0xBEEF);
    check("the write marks the save dirty", cart.battery_dirty());
    check_word("and reads back", read_word(cart, 5), 0xBEEF);
    write_word(cart, 6, 0x0F0F);

    // Holding select and clocking on runs into the next word.
    command(cart, 2, 5);
    const auto first = read_word_bits(cart);
    const auto second = read_word_bits(cart);
    deselect(cart);
    check_word("sequential read, word 5", first, 0xBEEF);
    check_word("sequential read, word 6", second, 0x0F0F);

    // ERASE, then EWDS blocks the next write.
    command(cart, 3, 6);
    deselect(cart);
    check_word("ERASE word 6", stored(cart, 6), 0xFFFF);
    disable_writes(cart);
    write_word(cart, 7, 0xAAAA);
    check_word("WRITE after EWDS is refused", stored(cart, 7), 0xFFFF);

    // WRAL and ERAL.
    enable_writes(cart);
    command(cart, 0, 0x10);
    for (int i = 15; i >= 0; --i) {
        clock_bit(cart, (0x5A5A >> i) & 1);
    }
    deselect(cart);
    check_word("WRAL fills word 0", stored(cart, 0), 0x5A5A);
    check_word("WRAL fills word 63", stored(cart, 63), 0x5A5A);
    command(cart, 0, 0x20);
    deselect(cart);
    check_word("ERAL clears word 63", stored(cart, 63), 0xFFFF);

    // $FFFC bit 7: logic reset, writes forbidden, words kept. And $FFFC's low
    // bits must not reach the Sega mapper's bank offset on this board.
    write_word(cart, 9, 0x4321);
    cart.write(0xFFFC, 0x8B);
    check_word("bit 7 keeps the stored words", stored(cart, 9), 0x4321);
    write_word(cart, 9, 0x0000);
    check_word("bit 7 forbids writes again", stored(cart, 9), 0x4321);
    check_word("$FFFC low bits do not shift the ROM banks", cart.read(0x8001), 0x02);

    // Deselect at $FFFC: $8000 is ROM again.
    cart.write(0xFFFC, 0x00);
    check_word("bit 3 clear puts $8000 back to ROM", cart.read(0x8000), 0x02);

    // A save state in the middle of a read resumes in the middle of the read.
    {
        cart.write(0xFFFC, 0x08);
        enable_writes(cart);
        write_word(cart, 20, 0xC3A5);
        command(cart, 2, 20);
        unsigned head = 0;
        for (int i = 0; i < 5; ++i) {
            head = (head << 1) | static_cast<unsigned>(clock_bit(cart, 0));
        }
        std::vector<std::uint8_t> blob;
        mercury::core::StateWriter writer(blob);
        cart.visit_state(writer);
        cart.visit_eeprom_state(writer);

        Cartridge copy;
        copy.load(make_rom(0x012439));
        mercury::core::StateReader reader(blob.data(), blob.size());
        copy.visit_state(reader);
        copy.visit_eeprom_state(reader);
        check("state read back cleanly", reader.ok());
        unsigned tail = 0;
        for (int i = 0; i < 11; ++i) {
            tail = (tail << 1) | static_cast<unsigned>(clock_bit(copy, 0));
        }
        check_word("a read split across a save state", (head << 11) | tail, 0xC3A5);
    }

    // A cartridge without the chip: $FFFC bit 3 still pages battery RAM in.
    Cartridge plain;
    plain.load(make_rom(0x002501));
    check("an ordinary header is not taken for an EEPROM cart", !plain.uses_serial_eeprom());
    plain.write(0xFFFC, 0x08);
    plain.write(0x8123, 0x5A);
    check_word("ordinary cart: battery RAM at $8000-$BFFF", plain.read(0x8123), 0x5A);

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
