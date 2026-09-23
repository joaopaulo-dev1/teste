#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/DatProbe/DatIndex.hpp"
#include "Tools/DatProbe/NameTable.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace gamecore::extract {

struct ExtractOptions {
    std::filesystem::path outRoot;
    std::set<std::string> extensions;
    bool dryRun = false;
};

struct ExtractStats {
    std::uint64_t written = 0;
    std::uint64_t reused = 0;
    std::uint64_t bytes = 0;
    std::uint64_t skippedCompressed = 0;
    std::uint64_t skippedFilter = 0;
    std::uint64_t rejectedPaths = 0;
    std::uint64_t errors = 0;
};

// Accepts only relative ASCII paths that are safe as Windows and POSIX file names.
bool sanitizeRelativePath(const std::string& path, std::filesystem::path& out, std::string& reason);

bool isStoredEntry(const dat::RawEntry& entry);

std::uint64_t storedBytes(const dat::DatIndex& index, const dat::NameTable& names, const std::set<std::string>& extensions);

// Copies stored (uncompressed) entries only. Compressed entries are counted and left untouched.
ExtractStats extractStored(const io::ByteSource& file,
                           const dat::DatIndex& index,
                           const dat::NameTable& names,
                           const ExtractOptions& options,
                           std::ostream& manifest,
                           std::vector<std::string>& errors);

}  // namespace gamecore::extract
