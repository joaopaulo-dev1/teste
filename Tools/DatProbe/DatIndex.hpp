#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/DatProbe/EntryProbe.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::dat {

// Layout confirmed on 13 files by EntryProbe:
//   u32 preamble, u32 count, count x 16-byte RawEntry, then the trailing region.
struct DatIndex {
    std::uint32_t preamble = 0;
    std::vector<RawEntry> entries;
    std::vector<std::uint8_t> trailing;
};

bool readDatIndex(const io::ByteSource& file,
                  std::uint64_t tableOffset,
                  std::uint64_t tableSize,
                  DatIndex& out,
                  std::string& error);

}  // namespace gamecore::dat
