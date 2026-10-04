#include "mercury/genesis/cartridge.hpp"

#include "mercury/core/zip_archive.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace mercury::genesis {

namespace {

constexpr std::size_t kHeaderOffset = 0x100;
constexpr std::size_t kSmdHeaderBytes = 512;
constexpr std::size_t kSmdBlockBytes = 16 * 1024;

std::string trimmed(const std::vector<std::uint8_t>& rom, std::size_t offset, std::size_t length) {
    if (offset + length > rom.size()) {
        return {};
    }
    std::string text(reinterpret_cast<const char*>(rom.data() + offset), length);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\0");
    return text.substr(first, last - first + 1);
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& rom, std::size_t offset) {
    if (offset + 4 > rom.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(rom[offset]) << 24) |
           (static_cast<std::uint32_t>(rom[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(rom[offset + 2]) << 8) |
           static_cast<std::uint32_t>(rom[offset + 3]);
}

// A .smd image is 512 header bytes followed by whole 16KB blocks. The header's
// third byte is 0x03 for a Genesis dump, but the size/shape test below is the
// reliable signal: a linear ROM is a power-of-two multiple of 512, an .smd is
// 512 + N*16384.
bool looks_like_smd(const std::vector<std::uint8_t>& raw, const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (raw.size() <= kSmdHeaderBytes) {
        return false;
    }
    const auto payload = raw.size() - kSmdHeaderBytes;
    const bool block_aligned = (payload % kSmdBlockBytes) == 0;
    if (extension == ".smd") {
        return block_aligned;
    }
    // Some dumps carry the interleave without the extension. Require both the
    // block alignment and the copier's marker bytes before assuming it, so a
    // normal .bin is never mangled.
    return block_aligned && raw[8] == 0xAA && raw[9] == 0xBB;
}

}  // namespace

void deinterleave_smd_block(const std::uint8_t* source, std::uint8_t* destination) noexcept {
    // First half of the block is every even byte of the original, second half
    // every odd byte. Undo by weaving them back together.
    constexpr std::size_t half = kSmdBlockBytes / 2;
    for (std::size_t i = 0; i < half; ++i) {
        destination[i * 2 + 1] = source[i];
        destination[i * 2 + 0] = source[half + i];
    }
}

bool Cartridge::load(const std::filesystem::path& path, std::string& error) {
    // read_rom_file unwraps a .zip transparently, so everything below sees the
    // cartridge image whether or not it arrived compressed.
    std::vector<std::uint8_t> raw;
    if (!mercury::core::read_rom_file(path, raw, error)) {
        return false;
    }
    const auto size = raw.size();
    if (size < 0x200) {
        error = "file is too small to be a Mega Drive ROM";
        return false;
    }

    if (looks_like_smd(raw, path)) {
        layout_ = RomLayout::interleaved;
        const auto payload = raw.size() - kSmdHeaderBytes;
        rom_.assign(payload, 0);
        const auto blocks = payload / kSmdBlockBytes;
        for (std::size_t block = 0; block < blocks; ++block) {
            deinterleave_smd_block(raw.data() + kSmdHeaderBytes + block * kSmdBlockBytes,
                                   rom_.data() + block * kSmdBlockBytes);
        }
    } else {
        layout_ = RomLayout::linear;
        rom_ = std::move(raw);
    }

    parse_header();
    if (plausible()) {
        return true;
    }

    // The layout guess was wrong, or the image is not a Mega Drive ROM. Try the
    // other interleaving before giving up - the size test alone cannot always
    // tell them apart, and a wrong guess scrambles every word.
    if (layout_ == RomLayout::linear && raw.size() > kSmdHeaderBytes &&
        ((raw.size() - kSmdHeaderBytes) % kSmdBlockBytes) == 0) {
        std::vector<std::uint8_t> attempt(raw.size() - kSmdHeaderBytes, 0);
        const auto blocks = attempt.size() / kSmdBlockBytes;
        for (std::size_t block = 0; block < blocks; ++block) {
            deinterleave_smd_block(raw.data() + kSmdHeaderBytes + block * kSmdBlockBytes,
                                   attempt.data() + block * kSmdBlockBytes);
        }
        auto keep = std::move(rom_);
        rom_ = std::move(attempt);
        layout_ = RomLayout::interleaved;
        parse_header();
        if (plausible()) {
            return true;
        }
        rom_ = std::move(keep);
        layout_ = RomLayout::linear;
        parse_header();
    }

    error = "not a recognisable Mega Drive image: no SEGA signature at 0x100 and "
            "the reset vector (" + [&] {
                std::ostringstream out;
                out << std::hex << std::uppercase << reset_vector();
                return out.str();
            }() + ") does not point into the ROM";
    return false;
}

// Early Electronic Arts carts - Budokan, Populous and friends - predate the
// standard header and carry no SEGA signature at all, so requiring one rejects
// perfectly good ROMs. The reset vector is the better test: it has to be even
// and point inside the image, which a scrambled interleave essentially never
// manages.
std::uint32_t Cartridge::reset_vector() const noexcept {
    return read_be32(rom_, 0x04);
}

bool Cartridge::plausible() const noexcept {
    if (header_.console.find("SEGA") != std::string::npos) {
        return true;
    }
    const auto pc = reset_vector();
    const auto sp = read_be32(rom_, 0x00);
    const bool pc_ok = (pc & 1) == 0 && pc >= 0x200 && pc < rom_.size();
    // The stack lives in work RAM at FF0000-FFFFFF, or occasionally in the
    // mirror at E00000; either way the top byte is set.
    const bool sp_ok = (sp & 1) == 0 && (sp & 0xFF0000) >= 0xE00000;
    return pc_ok && sp_ok;
}

void Cartridge::parse_header() noexcept {
    header_ = RomHeader{};
    if (rom_.size() < kHeaderOffset + 0x100) {
        return;
    }
    header_.console = trimmed(rom_, kHeaderOffset + 0x00, 16);
    header_.copyright = trimmed(rom_, kHeaderOffset + 0x10, 16);
    header_.domestic_title = trimmed(rom_, kHeaderOffset + 0x20, 48);
    header_.overseas_title = trimmed(rom_, kHeaderOffset + 0x50, 48);
    header_.serial = trimmed(rom_, kHeaderOffset + 0x80, 14);
    header_.checksum = static_cast<std::uint16_t>((rom_[kHeaderOffset + 0x8E] << 8) |
                                                   rom_[kHeaderOffset + 0x8F]);
    header_.rom_start = read_be32(rom_, kHeaderOffset + 0xA0);
    header_.rom_end = read_be32(rom_, kHeaderOffset + 0xA4);
    header_.ram_start = read_be32(rom_, kHeaderOffset + 0xA8);
    header_.ram_end = read_be32(rom_, kHeaderOffset + 0xAC);

    // "RA" at 0x1B0 marks external SRAM, with its window in the next two longs.
    if (rom_[kHeaderOffset + 0xB0] == 'R' && rom_[kHeaderOffset + 0xB1] == 'A') {
        header_.has_sram = true;
        header_.sram_start = read_be32(rom_, kHeaderOffset + 0xB4);
        header_.sram_end = read_be32(rom_, kHeaderOffset + 0xB8);
    }
    header_.region = trimmed(rom_, kHeaderOffset + 0xF0, 3);
}

std::uint16_t Cartridge::compute_checksum() const noexcept {
    // Sum of every word from 0x200 to the end, 16-bit wrapping. The header's
    // own checksum field is excluded because it sits below 0x200.
    std::uint16_t sum = 0;
    for (std::size_t offset = 0x200; offset + 1 < rom_.size(); offset += 2) {
        sum = static_cast<std::uint16_t>(sum + ((rom_[offset] << 8) | rom_[offset + 1]));
    }
    return sum;
}

std::uint16_t Cartridge::read16(std::uint32_t address) const noexcept {
    // In 64 bits: `address + 1` in 32 wraps 0xFFFFFFFF round to zero, which
    // passes the test and reads four gigabytes past the end of the ROM. No
    // game can produce that address, but a crafted save state could.
    if (std::uint64_t{address} + 1 >= rom_.size()) {
        return 0xFFFF;
    }
    return static_cast<std::uint16_t>((rom_[address] << 8) | rom_[address + 1]);
}

std::uint8_t Cartridge::read8(std::uint32_t address) const noexcept {
    if (address >= rom_.size()) {
        return 0xFF;
    }
    return rom_[address];
}

std::uint16_t Cartridge::patch16(std::uint32_t address, std::uint16_t value) noexcept {
    if (std::uint64_t{address} + 1 >= rom_.size()) {  // 64 bits: see read16
        return 0xFFFF;
    }
    const auto was = static_cast<std::uint16_t>((rom_[address] << 8) | rom_[address + 1]);
    rom_[address] = static_cast<std::uint8_t>(value >> 8);
    rom_[address + 1] = static_cast<std::uint8_t>(value);
    return was;
}

std::string Cartridge::describe() const {
    std::ostringstream out;
    out << "Cartridge: " << (header_.overseas_title.empty() ? header_.domestic_title
                                                            : header_.overseas_title)
        << "\n  console   : " << header_.console
        << "\n  serial    : " << header_.serial
        << "\n  region    : " << header_.region
        << "\n  layout    : " << (layout_ == RomLayout::interleaved ? "interleaved (.smd, de-interleaved on load)"
                                                                    : "linear (.bin)")
        << "\n  size      : " << rom_.size() << " bytes"
        << "\n  rom range : " << std::hex << std::uppercase << std::setw(6) << std::setfill('0')
        << header_.rom_start << "-" << std::setw(6) << header_.rom_end << std::dec
        << "\n  sram      : " << (header_.has_sram ? "yes" : "no")
        << "\n  checksum  : header " << std::hex << std::setw(4) << std::setfill('0') << header_.checksum
        << ", computed " << std::setw(4) << compute_checksum() << std::dec;
    return out.str();
}

}  // namespace mercury::genesis
