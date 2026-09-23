#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace gamecore::lz2k {

// Parameters of the public LZH block scheme (Okumura's ar002 / LHA -lh5-..-lh7-).
// Which set LZ2K uses, if any, is decided by running every set against the data.
struct LzhParams {
    const char* name = "lh6";
    unsigned dictBits = 15;
    unsigned posBits = 5;

    unsigned posCodes() const { return dictBits + 1; }
};

inline constexpr LzhParams kLh5{"lh5", 13, 4};
inline constexpr LzhParams kLh6{"lh6", 15, 5};
inline constexpr LzhParams kLh7{"lh7", 16, 5};

struct DecodeResult {
    bool ok = false;
    std::string failure;
    std::size_t produced = 0;
    std::uint64_t bitsConsumed = 0;
    std::uint32_t blocks = 0;
    std::uint32_t codesLeftInBlock = 0;
};

// Fills `out` completely or reports why it could not. Never reads outside `in` (missing bits read as zero
// and are reported through bitsConsumed > in.size() * 8).
DecodeResult decodeLzh(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, const LzhParams& params);

}  // namespace gamecore::lz2k
