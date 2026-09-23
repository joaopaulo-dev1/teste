#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/DatProbe/DatIndex.hpp"
#include "Tools/DatProbe/NameTable.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace gamecore::dat {

struct ExtensionStats {
    std::uint64_t count = 0;
    std::uint64_t stored = 0;
    std::uint64_t compressed = 0;
    std::uint64_t totalOriginalBytes = 0;
    std::map<std::string, std::uint64_t> detected;
};

std::string lowerExtension(const std::string& name);

// Labels what sits at an entry's offset without decompressing anything:
// a documented format name, "text", the 4-byte lead of a compressed block, or "bin".
std::string labelEntry(const io::ByteSource& file, const RawEntry& entry);

std::map<std::string, ExtensionStats> crossCheck(const io::ByteSource& file,
                                                 const DatIndex& index,
                                                 const NameTable& names,
                                                 std::uint64_t labelCapPerExtension);

}  // namespace gamecore::dat
