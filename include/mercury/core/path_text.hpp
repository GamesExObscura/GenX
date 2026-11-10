#pragma once

#include <filesystem>
#include <string>

namespace mercury::core {

// A path as UTF-8 text, for error messages, logs and matching file names.
//
// Use this, never path::string(). On Windows string() converts to the old
// ANSI code page and THROWS for any character that page cannot hold - a
// separate combining accent ("e" + U+0301), Japanese, an emoji. One such
// folder name beside a Sega CD disc used to abort GenX outright, because the
// BIOS search lists the folders around the disc by name.
//
// UTF-8 holds every character a name can have. The only names it cannot
// represent are broken ones (half of a UTF-16 pair), and those come back with
// '?' in place of anything that is not plain ASCII rather than an exception.
[[nodiscard]] std::string path_text(const std::filesystem::path& path);

}  // namespace mercury::core
