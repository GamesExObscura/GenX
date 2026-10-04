// Checks the hand-written inflate against real archives rather than against
// itself: every case here compares the extracted bytes to the original file,
// byte for byte. An inflate that is subtly wrong decodes the first few hundred
// bytes and then diverges, which a size check alone would not catch.
//
// Usage: zip_archive_tests <original-rom> <zip> [<zip> ...]

#include "mercury/core/zip_archive.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: zip_archive_tests <original> <zip> [<zip> ...]\n");
        return 2;
    }
    const auto expected = read_all(argv[1]);
    if (expected.empty()) {
        std::printf("FAIL: could not read the original %s\n", argv[1]);
        return 1;
    }
    std::printf("original %s: %zu bytes\n", argv[1], expected.size());

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        std::vector<std::uint8_t> got;
        std::string error;
        std::string entry;
        if (!mercury::core::read_rom_file(argv[i], got, error, &entry)) {
            std::printf("FAIL %s: %s\n", argv[i], error.c_str());
            ++failures;
            continue;
        }
        if (got.size() != expected.size()) {
            std::printf("FAIL %s: got %zu bytes, expected %zu\n", argv[i], got.size(),
                        expected.size());
            ++failures;
            continue;
        }
        std::size_t first_bad = expected.size();
        for (std::size_t k = 0; k < expected.size(); ++k) {
            if (got[k] != expected[k]) {
                first_bad = k;
                break;
            }
        }
        if (first_bad != expected.size()) {
            std::printf("FAIL %s: first mismatch at byte %zu (%02X vs %02X)\n", argv[i], first_bad,
                        got[first_bad], expected[first_bad]);
            ++failures;
            continue;
        }
        std::printf("PASS %s -> \"%s\", %zu bytes identical\n", argv[i], entry.c_str(), got.size());
    }

    // A file that is not an archive has to come through untouched.
    {
        std::vector<std::uint8_t> got;
        std::string error;
        if (!mercury::core::read_rom_file(argv[1], got, error) || got != expected) {
            std::printf("FAIL: a plain ROM did not survive read_rom_file\n");
            ++failures;
        } else {
            std::printf("PASS plain file passthrough\n");
        }
    }

    std::printf("%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
