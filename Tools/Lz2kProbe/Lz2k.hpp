#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::lz2k {

// Confirmed on the retail data by lz2k_decode_test: every compressed chunk is an LZH -lh5- stream
// (13-bit dictionary) that yields exactly raw_size bytes and ends on the last payload byte.
// Chunks with packed_size == raw_size are stored verbatim.
bool decompressEntry(const io::ByteSource& entry, std::uint64_t expectedRaw, std::vector<std::uint8_t>& out,
                     std::string& error);

}  // namespace gamecore::lz2k
