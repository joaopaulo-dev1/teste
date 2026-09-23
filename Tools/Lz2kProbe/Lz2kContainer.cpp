#include "Tools/Lz2kProbe/Lz2kContainer.hpp"

#include <cstring>

namespace gamecore::lz2k {

Layout parseLayout(const io::ByteSource& entry, std::uint64_t expectedRaw) {
    Layout layout;
    std::uint64_t pos = 0;
    while (pos < entry.size) {
        if (entry.size - pos < kChunkHeaderBytes) {
            break;
        }
        std::uint8_t header[kChunkHeaderBytes];
        if (!io::readExact(entry, pos, header, sizeof(header))) {
            layout.failure = "chunk header unreadable";
            return layout;
        }
        if (std::memcmp(header, "LZ2K", 4) != 0) {
            if (layout.chunks.empty()) {
                layout.failure = "no LZ2K magic at entry start";
                return layout;
            }
            break;
        }
        Chunk chunk;
        chunk.headerOffset = pos;
        chunk.rawSize = static_cast<std::uint32_t>(header[4]) | (static_cast<std::uint32_t>(header[5]) << 8) |
                        (static_cast<std::uint32_t>(header[6]) << 16) | (static_cast<std::uint32_t>(header[7]) << 24);
        chunk.packedSize = static_cast<std::uint32_t>(header[8]) | (static_cast<std::uint32_t>(header[9]) << 8) |
                           (static_cast<std::uint32_t>(header[10]) << 16) |
                           (static_cast<std::uint32_t>(header[11]) << 24);
        if (chunk.rawSize == 0 || chunk.packedSize == 0) {
            layout.failure = "chunk with zero raw or packed size";
            return layout;
        }
        if (chunk.packedSize > entry.size - pos - kChunkHeaderBytes) {
            layout.failure = "chunk payload runs past the entry";
            return layout;
        }
        if (layout.chunks.size() >= kMaxChunks) {
            layout.failure = "chunk count above limit";
            return layout;
        }
        layout.chunks.push_back(chunk);
        layout.rawTotal += chunk.rawSize;
        pos += kChunkHeaderBytes + chunk.packedSize;
    }
    layout.consumed = pos;
    layout.trailingBytes = entry.size - pos;

    if (layout.chunks.empty()) {
        layout.failure = "entry too small for a chunk header";
    } else if (layout.rawTotal != expectedRaw) {
        layout.failure = "chunk raw sizes do not add up to the record's original size";
    } else if (layout.trailingBytes != 0) {
        layout.failure = "bytes left after the last chunk";
    } else {
        layout.valid = true;
    }
    return layout;
}

}  // namespace gamecore::lz2k
