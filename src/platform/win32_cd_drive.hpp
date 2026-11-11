#pragma once

// Real discs in the PC's optical drives, for the Sega CD.
//
// Windows only, and kept out of the core on purpose: the core reads every
// disc through mercury::segacd::DiscSource and knows nothing of drives. This
// supplies that source for a drive, and the opener the front end hands to
// Disc::set_source_opener at startup. After that, the path "E:\" opens the
// disc in drive E: anywhere a disc image path would be accepted.

#include <filesystem>
#include <string>
#include <vector>

#include "mercury/segacd/disc_source.hpp"

namespace mercury::platform {

struct CdDrive {
    wchar_t letter = 0;  // 'E'
    bool has_disc = false;
    // The disc's volume label when there is one ("FINALFIGHT").
    std::wstring label;
};

// Every optical drive on the machine, A to Z, with whether a disc is in it.
// Asking can spin a drive up, so this is for when a menu is opened, not for
// every frame.
[[nodiscard]] std::vector<CdDrive> list_cd_drives();

// The path that stands for a drive's disc: "E:\".
[[nodiscard]] std::filesystem::path cd_drive_path(wchar_t letter);

// The drive letter a path names, or 0 when it is not an optical drive's root
// ("E:\" or "E:" for a drive Windows reports as a CD-ROM).
[[nodiscard]] wchar_t cd_drive_letter(const std::filesystem::path& path);

// For mercury::segacd::Disc::set_source_opener.
[[nodiscard]] segacd::DiscSourceOpener cd_drive_opener();

}  // namespace mercury::platform
