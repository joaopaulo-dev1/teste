#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::lz2k {

// Hypothesised framing, read from DLC5.DAT samples and checked by parseLayout:
//   repeated { char magic[4] = "LZ2K"; u32 rawSize; u32 packedSize; u8 payload[packedSize]; }
// The payload bitstream itself is not interpreted here.
struct Chunk {
    std::uint64_t headerOffset = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t packedSize = 0;

    std::uint64_t payloadOffset() const { return headerOffset + 12; }
};

struct Layout {
    bool valid = false;
    std::string failure;
    std::vector<Chunk> chunks;
    std::uint64_t rawTotal = 0;
    std::uint64_t consumed = 0;
    std::uint64_t trailingBytes = 0;
};

constexpr std::uint64_t kChunkHeaderBytes = 12;
constexpr std::size_t kMaxChunks = 1u << 20;

// `expectedRaw` is the entry's original size from the DAT record; the layout is valid only
// when the chunk raw sizes add up to it and the chunks cover the entry exactly.
Layout parseLayout(const io::ByteSource& entry, std::uint64_t expectedRaw);

}  // namespace gamecore::lz2k
