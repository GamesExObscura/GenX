#pragma once

// LZMA decoding, enough of it to read a CHD.
//
// Written rather than pulled in, like the DEFLATE and the CHD container
// beside it. Only what a CHD needs: LZMA1, raw, no container header, no end
// marker, with the properties fixed by the encoder chdman used and the
// output size known in advance because it is always one hunk.
//
// The whole dictionary is the output buffer. A CHD hunk is 19584 bytes and
// chdman gives the encoder a dictionary at least that big, so a match can
// never reach back past the start of the hunk and there is no window to
// wrap - which removes the fiddliest part of an LZMA decoder.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mercury::core {

// Decode `size` bytes into `out`, which is resized to exactly that.
//
// lc, lp and pb are the usual LZMA context/position parameters. A CHD does
// not store them: chdman builds the encoder with its defaults and writes no
// properties byte, so the caller passes what those defaults are.
[[nodiscard]] bool lzma_decode(const std::uint8_t* data, std::size_t data_size, std::size_t size,
                               int lc, int lp, int pb, std::vector<std::uint8_t>& out,
                               std::string& error);

}  // namespace mercury::core
