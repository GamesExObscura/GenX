#include "mercury/segacd/bios.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

#include "mercury/core/path_text.hpp"

namespace mercury::segacd {

namespace {

using core::path_text;

constexpr std::uintmax_t kBiosSize = 0x20000;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The region a file name admits to, if any.
char region_from_name(const std::filesystem::path& path) {
    const auto name = lower(path_text(path.filename()));
    const auto has = [&name](const char* word) { return name.find(word) != std::string::npos; };
    if (has("(usa") || has("(us)") || has("us_") || has("_us") || has("sega cd") || has("segacd") ||
        has("usa")) {
        return 'U';
    }
    if (has("(europe") || has("(eu)") || has("eu_") || has("_eu") || has("europe")) {
        return 'E';
    }
    if (has("(japan") || has("(jp)") || has("jp_") || has("_jp") || has("japan")) {
        return 'J';
    }
    return 0;
}

// The first of U / E / J in the header's region field. A BIOS lists only its
// own.
char header_region(const std::vector<std::uint8_t>& image) {
    for (std::size_t i = 0; i < 3 && 0x1F0 + i < image.size(); ++i) {
        const char c = static_cast<char>(std::toupper(image[0x1F0 + i]));
        if (c == 'U' || c == 'E' || c == 'J') {
            return c;
        }
    }
    return 0;
}

}  // namespace

char bios_console_region(const std::vector<std::uint8_t>& image) {
    // Each BIOS carries exactly one of these, the text of its own region
    // error. Southeast Asia is tested first: its line is a different sentence
    // from Japan's ("...FOR EXCLUSIVE USE IN JAPAN AND / SOUTHEAST ASIA.").
    const std::string_view text(reinterpret_cast<const char*>(image.data()), image.size());
    if (text.find("FOR EXCLUSIVE USE IN SOUTHEAST ASIA") != std::string_view::npos) return 'A';
    if (text.find("FOR EXCLUSIVE USE IN JAPAN") != std::string_view::npos) return 'J';
    if (text.find("FOR USE IN NTSC REGIONS OTHER THAN") != std::string_view::npos) return 'U';
    if (text.find("PAL/SECAM-COMPATIBLE") != std::string_view::npos) return 'E';
    return header_region(image);
}

std::vector<BiosImage> scan_bios_folder(const std::filesystem::path& folder) {
    std::vector<BiosImage> found;
    std::error_code ignored;
    if (folder.empty() || !std::filesystem::is_directory(folder, ignored)) {
        return found;
    }
    // INTO SUBFOLDERS TOO, two levels. BIOS packs arrive sorted by who names
    // them how - "RetroArch naming", "PicoDrive naming", "Extras" - and a
    // person pointing at the pack's top folder means all of it.
    constexpr int kMaxDepth = 2;
    std::vector<int> depth_of;  // parallel to `found`, for the ordering below
    std::filesystem::recursive_directory_iterator walk(
        folder, std::filesystem::directory_options::skip_permission_denied, ignored);
    for (auto it = walk; !ignored && it != std::filesystem::recursive_directory_iterator();
         it.increment(ignored)) {
        const auto& entry = *it;
        if (entry.is_directory(ignored)) {
            if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ignored) || entry.file_size(ignored) != kBiosSize) {
            continue;
        }
        // The whole image, not just the header: which console it belongs to is
        // read from its region message (bios_console_region).
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes(kBiosSize, 0);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in) {
            continue;
        }
        // A Sega CD BIOS carries a Mega Drive header. Its system name says
        // "SEGA", which also rules out a 128KB file that is something else.
        const std::string system(reinterpret_cast<const char*>(bytes.data()) + 0x100, 16);
        if (system.find("SEGA") == std::string::npos) {
            continue;
        }
        BiosImage image;
        image.path = entry.path();
        image.region = header_region(bytes);
        if (image.region == 0) {
            image.region = region_from_name(entry.path());
        }
        image.console = bios_console_region(bytes);
        if (image.console == 0) {
            image.console = image.region;
        }
        std::string title(reinterpret_cast<const char*>(bytes.data()) + 0x120, 0x30);
        while (!title.empty() && (title.back() == ' ' || title.back() == '\0')) title.pop_back();
        image.description = title;
        found.push_back(image);
        depth_of.push_back(it.depth());
    }
    // A stable order, so which image is picked does not depend on how the
    // file system happened to list them. The standard dump names first -
    // RetroArch's BIOS_CD_U/E/J and PicoDrive's us_scd / eu_mcd / jp_mcd -
    // because a pack's "Extras" can hold revisions that do not run (one
    // "Model 2 v2.00" image in the user's pack never starts its sub CPU).
    // Then a file in the folder itself before one in a subfolder, then path.
    const auto standard = [](const std::filesystem::path& path) {
        const auto name = lower(path_text(path.filename()));
        return name.rfind("bios_cd_", 0) == 0 || name.rfind("us_scd", 0) == 0 ||
               name.rfind("eu_mcd", 0) == 0 || name.rfind("jp_mcd", 0) == 0;
    };
    std::vector<std::size_t> order(found.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const bool sa = standard(found[a].path);
        const bool sb = standard(found[b].path);
        if (sa != sb) return sa;
        if (depth_of[a] != depth_of[b]) return depth_of[a] < depth_of[b];
        return found[a].path < found[b].path;
    });
    std::vector<BiosImage> sorted;
    sorted.reserve(found.size());
    for (const std::size_t i : order) sorted.push_back(found[i]);
    return sorted;
}

bool load_bios_for(char region, const std::filesystem::path& configured,
                   const std::filesystem::path& disc, std::vector<std::uint8_t>& image,
                   std::string& error, std::filesystem::path* chosen) {
    std::vector<std::filesystem::path> folders;
    if (!configured.empty()) {
        folders.push_back(configured);
    }
    // Then the disc's own folder: the BIOS tab tells people GenX looks for
    // the BIOS in their game folder, and a file dropped in beside the disc is
    // the plainest reading of that. Only a 128KB file with a Sega header
    // counts (see scan_bios_folder), so a game folder with none falls through.
    //
    // Then any folder with BIOS in its name beside the disc or above it - a
    // library laid out <root>/Games/<title>/<disc>.cue with <root>/Sega CD
    // BIOS next to Games is found without being told.
    std::filesystem::path here = disc.empty() ? std::filesystem::path() : disc.parent_path();
    if (!here.empty()) {
        folders.push_back(here);
    }
    std::error_code ignored;
    for (int level = 0; level < 4 && !here.empty(); ++level) {
        for (const auto& entry : std::filesystem::directory_iterator(here, ignored)) {
            if (entry.is_directory(ignored) &&
                lower(path_text(entry.path().filename())).find("bios") != std::string::npos) {
                folders.push_back(entry.path());
            }
        }
        const auto up = here.parent_path();
        if (up == here) break;
        here = up;
    }

    for (const auto& folder : folders) {
        const auto images = scan_bios_folder(folder);
        if (images.empty()) {
            continue;
        }
        // The disc's own region first; with no disc (or no match) any image
        // will do for the BIOS's own menu, preferring USA.
        //
        // A BIOS carries two facts. The header's region letter is the discs
        // it boots: the JVC X'Eye's says U, and it plays US discs and drops a
        // Japanese one into its CD player. Its region message is the console
        // it must run on (bios_console_region), which GenX then becomes. So
        // the disc is matched against the header, preferring an image whose
        // console is the same region - a real Japanese unit over the
        // Southeast Asian PAL one, which also says J and also boots Japanese
        // discs, but at 50Hz. Picking that one first was the bug: PicoDrive's
        // standard jp_mcd1_9112.bin name is the Asian BIOS, and standard
        // names are tried first.
        const BiosImage* pick = nullptr;
        for (const auto& candidate : images) {
            if (region != 0 && candidate.region == region && candidate.console == region) {
                pick = &candidate;
                break;
            }
        }
        for (const auto& candidate : images) {
            if (pick == nullptr && region != 0 && candidate.region == region) {
                pick = &candidate;
                break;
            }
        }
        if (pick == nullptr && region != 0) {
            error = std::string("found a Sega CD BIOS in ") + path_text(folder) +
                    " but none for this disc's region (" + region +
                    ") - a disc only boots on its own region's BIOS";
            continue;
        }
        if (pick == nullptr) {
            for (const auto& candidate : images) {
                if (candidate.region == 'U' && candidate.console == 'U') { pick = &candidate; break; }
            }
            for (const auto& candidate : images) {
                if (pick == nullptr && candidate.region == 'U') { pick = &candidate; break; }
            }
            if (pick == nullptr) pick = &images.front();
        }
        std::ifstream in(pick->path, std::ios::binary);
        image.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (image.size() != kBiosSize) {
            error = "could not read " + path_text(pick->path);
            continue;
        }
        if (chosen != nullptr) *chosen = pick->path;
        error.clear();
        return true;
    }
    if (error.empty()) {
        error = "no Sega CD BIOS found - set its folder on the BIOS tab in Configure";
    }
    return false;
}

}  // namespace mercury::segacd
