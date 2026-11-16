#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mercury::gamegear {

// What can sit in a Master System's controller port 1 besides the ordinary
// pad. (A Game Gear has no controller ports; these only mean anything when a
// Master System cartridge is running.)
//
//   Light Phaser - the trigger is TL (button 1). The light sensor pulls TH low
//     while the beam passes near where the gun points, and the VDP latches its
//     H counter on that edge; the game reads the latch to find the shot.
//   Paddle Control - the Japanese HPD-200. Eight bits of position sent four
//     at a time on the direction lines, with TR low for the low nibble; the
//     button is TL.
enum class SmsPeripheral : std::uint8_t { control_pad, light_phaser, paddle };

struct SmsCartridgeDevice {
    SmsPeripheral peripheral = SmsPeripheral::control_pad;
    // The H counter the Phaser's latch lands on, minus half the aimed X: the
    // delay between the beam passing the sight and the latch, in counter
    // steps. Most games are made for 20; two expect 16.
    int phaser_x_offset = 20;
    // Made for a European 50Hz console and refusing a 60Hz one in code (issue
    // #12): run it on PAL timing. The header can't say so - "export" covers
    // both.
    bool needs_pal = false;
};

[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

// The device a Master System cartridge was made for, by the CRC32 of its ROM.
// Every other cartridge gets the control pad.
[[nodiscard]] SmsCartridgeDevice sms_device_for_rom(const std::vector<std::uint8_t>& rom) noexcept;

}  // namespace mercury::gamegear
