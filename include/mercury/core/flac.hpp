#pragma once

// FLAC decoding, enough of it to read the audio tracks of a CHD.
//
// Written rather than pulled in, like everything else in this corner. Only
// what a CHD needs, which is less than the whole format: no metadata blocks,
// no seek tables, no stream header at all. A cdfl hunk begins directly with
// a FLAC audio frame - checked by looking, the first two bytes of every one
// of them are the FF F8 sync - and the frames are always 44.1kHz, sixteen
// bit, two channels, because they are a compact disc.
//
// The subcode half of the hunk follows the last FLAC frame, so the caller
// needs to know where that ended; `consumed` says.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mercury::core {

// Decode `sample_frames` stereo pairs and write them as BIG-ENDIAN 16-bit
// interleaved bytes - which is how a CHD stores CD audio, the opposite way
// round from a .bin file. `out` is resized to sample_frames * 4.
[[nodiscard]] bool flac_decode_cd(const std::uint8_t* data, std::size_t size,
                                  std::size_t sample_frames, std::vector<std::uint8_t>& out,
                                  std::size_t& consumed, std::string& error);

}  // namespace mercury::core
