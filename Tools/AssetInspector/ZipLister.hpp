#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gamecore::inspect {

struct ZipSummary {
    bool parsed = false;
    bool zip64 = false;
    bool truncated = false;
    std::uint64_t entryCount = 0;
    std::uint64_t encryptedCount = 0;
    std::map<std::string, std::uint64_t> extensions;
    std::vector<std::string> sampleNames;
    std::string note;
};

ZipSummary summarizeZip(const io::ByteSource& source);

}  // namespace gamecore::inspect
