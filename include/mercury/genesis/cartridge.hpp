#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mercury::genesis {

// How the ROM file stored its bytes. The 68000 itself is plainly big-endian;
// what varies is the FILE format, and getting that wrong scrambles every word
// in a way that looks like a broken CPU.
enum class RomLayout {
    linear,      // .bin/.gen/.md - bytes exactly as they sit on the cartridge
    interleaved, // .smd - Super Magic Drive copier output, de-interleaved on load
};

// Parsed from the 256-byte header at 0x100. Useful for identifying the ROM and
// for sizing SRAM, and a good early sanity check that the image loaded right:
// if the console name is garbage, the layout was probably misdetected.
struct RomHeader {
    std::string console;        // "SEGA MEGA DRIVE" / "SEGA GENESIS"
    std::string copyright;
    std::string domestic_title;
    std::string overseas_title;
    std::string serial;
    std::string region;
    std::uint32_t rom_start = 0;
    std::uint32_t rom_end = 0;
    std::uint32_t ram_start = 0;
    std::uint32_t ram_end = 0;
    bool has_sram = false;
    std::uint32_t sram_start = 0;
    std::uint32_t sram_end = 0;
    std::uint16_t checksum = 0;
};

class Cartridge {
public:
    [[nodiscard]] bool load(const std::filesystem::path& path, std::string& error);

    [[nodiscard]] bool loaded() const noexcept { return !rom_.empty(); }
    [[nodiscard]] const std::vector<std::uint8_t>& rom() const noexcept { return rom_; }

    // A GAME GENIE REALLY DOES CHANGE THE CARTRIDGE, as far as the console can
    // tell: the device sits in the slot and answers one address itself. So a
    // Mega Drive code is applied by writing the word into the image, not by
    // testing every read against a list - which is also why it costs the 68000
    // nothing at all. Returns what was there, so turning the code off puts it
    // back. Out of range does nothing and reports FFFF, an empty bus.
    std::uint16_t patch16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] const RomHeader& header() const noexcept { return header_; }
    [[nodiscard]] RomLayout layout() const noexcept { return layout_; }
    [[nodiscard]] std::size_t size() const noexcept { return rom_.size(); }

    // Big-endian word read with the cartridge's own bounds handling. Reads past
    // the end return FFFF, which is what an unpopulated bus gives.
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const noexcept;

    // Recomputed from the image; compare with header().checksum to spot a bad
    // dump or a botched de-interleave.
    [[nodiscard]] std::uint16_t compute_checksum() const noexcept;

    [[nodiscard]] std::string describe() const;

private:
    [[nodiscard]] std::uint32_t reset_vector() const noexcept;
    [[nodiscard]] bool plausible() const noexcept;
    void parse_header() noexcept;

    std::vector<std::uint8_t> rom_;
    RomHeader header_{};
    RomLayout layout_ = RomLayout::linear;
};

// Exposed for testing: turn one 16KB Super Magic Drive block into linear bytes.
// The block holds 8KB of even-offset bytes followed by 8KB of odd-offset bytes.
void deinterleave_smd_block(const std::uint8_t* source, std::uint8_t* destination) noexcept;

}  // namespace mercury::genesis
