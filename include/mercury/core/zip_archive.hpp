#pragma once

// Reading ROMs straight out of a .zip.
//
// Written rather than pulled in: the project has no third-party dependencies and
// this needs exactly one thing from zlib - raw DEFLATE decoding - which is a few
// hundred lines. Compression is not needed, only decompression, and only the
// two stored/deflated methods that every zip tool actually emits.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mercury::core {

// True when the buffer starts with a local file header or an empty archive's
// end-of-central-directory record.
[[nodiscard]] bool looks_like_zip(const std::vector<std::uint8_t>& data) noexcept;

// Raw DEFLATE (RFC 1951), no zlib or gzip wrapper. expected_size is the
// uncompressed size the archive declares; it is used to size the output up
// front and to catch a stream that decodes to the wrong length.
[[nodiscard]] bool inflate_raw(const std::uint8_t* data, std::size_t size,
                               std::size_t expected_size, std::vector<std::uint8_t>& out,
                               std::string& error);

// Pulls the entry from the archive that looks most like a ROM: the largest one
// whose extension is a known cartridge type, or failing that simply the largest.
// entry_name comes back so the caller can tell what it actually opened.
[[nodiscard]] bool extract_rom_from_zip(const std::vector<std::uint8_t>& archive,
                                        std::vector<std::uint8_t>& out, std::string& entry_name,
                                        std::string& error);

// Reads a file, and if it turns out to be a zip, replaces the contents with the
// ROM inside it. Every cartridge loader goes through this so that zip support is
// one implementation rather than one per machine.
// entry_name, when given, comes back holding the name of the file inside the
// archive - empty when the path was not an archive. The Master System / Game
// Gear split is decided by extension, so a zipped .sms has to be told apart by
// what is inside it rather than by the .zip on the outside.
[[nodiscard]] bool read_rom_file(const std::filesystem::path& path,
                                 std::vector<std::uint8_t>& out, std::string& error,
                                 std::string* entry_name = nullptr);

}  // namespace mercury::core
