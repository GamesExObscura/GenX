#include "mercury/core/path_text.hpp"

#include <exception>

namespace mercury::core {

std::string path_text(const std::filesystem::path& path) {
    try {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    } catch (const std::exception&) {
        // Only a name that is not valid text at all gets here. Keep what is
        // plain ASCII, which is enough to recognise an extension or the word
        // "bios", and mark the rest.
        std::string text;
        for (const auto c : path.native()) {
            text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
        }
        return text;
    }
}

}  // namespace mercury::core
