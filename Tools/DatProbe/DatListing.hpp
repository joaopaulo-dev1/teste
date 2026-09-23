#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::dat {

struct ListedEntry {
    std::uint32_t id = 0;
    std::string path;
    std::uint64_t offset = 0;
    std::uint32_t storedSize = 0;
    std::uint32_t originalSize = 0;
    std::uint32_t compression = 0;
    bool inBounds = true;
};

struct DatListing {
    std::uint64_t fileSize = 0;
    std::vector<ListedEntry> entries;
    std::uint64_t storedCount = 0;
    std::uint64_t compressedCount = 0;
    std::uint64_t unknownCompressionCount = 0;
    std::uint64_t unnamedCount = 0;
    std::uint64_t outOfBoundsCount = 0;
    std::uint64_t originalBytes = 0;
};

// Codes observed on the 13 retail files: 0 = stored, 2 = LZ2K. Anything else is reported, not guessed.
const char* compressionName(std::uint32_t code);

// Header -> entry records -> name tree, read-only. Fails instead of guessing on an unknown variant.
bool listDat(const io::ByteSource& file, DatListing& out, std::string& error);

}  // namespace gamecore::dat
