#include "mercury/gamegear/sms_peripherals.hpp"

#include <array>

namespace mercury::gamegear {

namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrcTable = make_crc_table();

struct KnownCartridge {
    std::uint32_t crc;
    SmsPeripheral peripheral;
};

// The releases that need something other than the pad, identified by the
// CRC32s of the No-Intro dumps (the same list Genesis Plus GX keeps).
constexpr KnownCartridge kKnown[] = {
    // Light Phaser
    {0xFBE5CFBB, SmsPeripheral::light_phaser},  // Missile Defense 3-D
    {0xE79BB689, SmsPeripheral::light_phaser},  // Missile Defense 3-D (BIOS)
    {0x43DEF05D, SmsPeripheral::light_phaser},  // Missile Defense 3-D (proto)
    {0x56DCB2D4, SmsPeripheral::light_phaser},  // 3-D Gunner (proto)
    {0x861B6E79, SmsPeripheral::light_phaser},  // Assault City (Light Phaser version)
    {0x5FC74D2A, SmsPeripheral::light_phaser},  // Gangster Town
    {0xE167A561, SmsPeripheral::light_phaser},  // Hang-On / Safari Hunt
    {0x91E93385, SmsPeripheral::light_phaser},  // Hang-On / Safari Hunt (BIOS)
    {0xE8EA842C, SmsPeripheral::light_phaser},  // Marksman Shooting / Trap Shooting
    {0xE8215C2E, SmsPeripheral::light_phaser},  // Marksman Shooting / Trap Shooting / Safari Hunt
    {0x205CAAE8, SmsPeripheral::light_phaser},  // Operation Wolf
    {0x23283F37, SmsPeripheral::light_phaser},  // Operation Wolf (alt)
    {0xDA5A7013, SmsPeripheral::light_phaser},  // Rambo III
    {0x79AC8E7F, SmsPeripheral::light_phaser},  // Rescue Mission
    {0x4B051022, SmsPeripheral::light_phaser},  // Shooting Gallery
    {0xA908CFF5, SmsPeripheral::light_phaser},  // Space Gun
    {0x5359762D, SmsPeripheral::light_phaser},  // Wanted
    {0x0CA95637, SmsPeripheral::light_phaser},  // Laser Ghost
    // Paddle Control
    {0xF9DBB533, SmsPeripheral::paddle},  // Alex Kidd BMX Trial
    {0xA6FA42D0, SmsPeripheral::paddle},  // Galactic Protector
    {0x29BC7FAD, SmsPeripheral::paddle},  // Megumi Rescue
    {0x315917D4, SmsPeripheral::paddle},  // Woody Pop
};

// CARTRIDGES THAT NEED A EUROPEAN 50Hz CONSOLE (issue #12), by CRC32.
//
// Back to the Future Part III (Europe), 2D48C1D3, VERIFIED: at $200C it waits
// for the frame flag, burns about 65,000 cycles in a counted loop, and reads
// the flag again. A 60Hz frame (262 x 228 = 59,736 cycles) has already ended
// by then and the game jumps to itself at $202C with interrupts off - a black
// screen for good. A 50Hz frame (313 x 228 = 71,364) has not, and it runs.
//
// The rest are Genesis Plus GX's "games requiring PAL timings", added with the
// Mega Drive list at the user's request (2026-11-15) without the ROMs to test.
constexpr std::uint32_t kNeedsPal[] = {
    0x2D48C1D3,  // Back to the Future Part III
    0x72420F38,  // The Addams Family
    0x1CBB7BF1,  // Battlemaniacs (Brazil)
    0x1B10A951,  // Bram Stoker's Dracula
    0xC0E25D62,  // California Games II
    0x45C50294,  // Jogos de Verao II (Brazil)
    0xC9DBF936,  // Home Alone
    0xA109A6FE,  // Power Strike II
    0x0047B615,  // Predator 2
    0xF42E145C,  // Quest for the Shaven Yak Starring Ren Hoek & Stimpy (Brazil)
    0x9F951756,  // RoboCop 3
    0xF8176918,  // Sensible Soccer
    0x1575581D,  // Shadow of the Beast
    0x96B3F29E,  // Sonic Blast (Brazil)
    0x5B3B922C,  // Sonic the Hedgehog 2 (version 0)
    0xD6F2BFCA,  // Sonic the Hedgehog 2 (version 1)
    0xCA1D3752,  // Space Harrier (50Hz)
    0x85CFC9C9,  // Taito Chase H.Q.
    0x332A847D,  // NBA Jam (prototype)
};

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

SmsCartridgeDevice sms_device_for_rom(const std::vector<std::uint8_t>& rom) noexcept {
    SmsCartridgeDevice device;
    if (rom.empty()) {
        return device;
    }
    // A dump with a 512-byte copier header in front is the same cartridge.
    std::size_t skip = (rom.size() % 0x4000) == 512 ? 512 : 0;
    const auto crc = crc32(rom.data() + skip, rom.size() - skip);
    for (const auto& known : kKnown) {
        if (known.crc == crc) {
            device.peripheral = known.peripheral;
            break;
        }
    }
    // Gangster Town and Wanted read the latch four counter steps earlier than
    // the rest of the library.
    if (crc == 0x5FC74D2A || crc == 0x5359762D) {
        device.phaser_x_offset = 16;
    }
    for (const auto pal_crc : kNeedsPal) {
        if (pal_crc == crc) {
            device.needs_pal = true;
            break;
        }
    }
    return device;
}

}  // namespace mercury::gamegear
