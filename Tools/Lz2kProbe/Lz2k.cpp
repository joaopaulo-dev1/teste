#include "Tools/Lz2kProbe/Lz2k.hpp"

#include "Tools/Lz2kProbe/Lz2kContainer.hpp"
#include "Tools/Lz2kProbe/LzhDecoder.hpp"

#include <algorithm>
#include <span>

namespace gamecore::lz2k {

bool decompressEntry(const io::ByteSource& entry, std::uint64_t expectedRaw, std::vector<std::uint8_t>& out,
                     std::string& error) {
    out.clear();
    const auto layout = parseLayout(entry, expectedRaw);
    if (!layout.valid) {
        error = "LZ2K framing: " + layout.failure;
        return false;
    }
    out.resize(static_cast<std::size_t>(expectedRaw));
    std::vector<std::uint8_t> payload;
    std::size_t written = 0;
    for (std::size_t i = 0; i < layout.chunks.size(); ++i) {
        const auto& chunk = layout.chunks[i];
        payload.resize(chunk.packedSize);
        if (!io::readExact(entry, chunk.payloadOffset(), payload.data(), payload.size())) {
            error = "LZ2K chunk " + std::to_string(i) + " unreadable";
            out.clear();
            return false;
        }
        const std::span<std::uint8_t> target(out.data() + written, chunk.rawSize);
        if (chunk.packedSize == chunk.rawSize) {
            std::copy(payload.begin(), payload.end(), target.begin());
        } else {
            const auto result = decodeLzh(payload, target, kLh5);
            if (!result.ok) {
                error = "LZ2K chunk " + std::to_string(i) + ": " + result.failure;
                out.clear();
                return false;
            }
        }
        written += chunk.rawSize;
    }
    error.clear();
    return true;
}

}  // namespace gamecore::lz2k
