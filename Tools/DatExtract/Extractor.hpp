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
    bool decompress = false;
};

struct ExtractStats {
    std::uint64_t written = 0;
    std::uint64_t reused = 0;
    std::uint64_t bytes = 0;
    std::uint64_t decompressed = 0;
    std::uint64_t skippedCompressed = 0;
    std::uint64_t skippedFilter = 0;
    std::uint64_t rejectedPaths = 0;
    std::uint64_t errors = 0;
};

// Accepts only relative ASCII paths that are safe as Windows and POSIX file names.
bool sanitizeRelativePath(const std::string& path, std::filesystem::path& out, std::string& reason);

bool isStoredEntry(const dat::RawEntry& entry);
bool isLz2kEntry(const dat::RawEntry& entry);

// Bytes that extraction will write: stored entries, plus LZ2K entries at their original size when
// `includeLz2k` is set.
std::uint64_t plannedBytes(const dat::DatIndex& index, const dat::NameTable& names,
                           const std::set<std::string>& extensions, bool includeLz2k);

// Copies stored entries; with options.decompress also writes LZ2K entries decompressed.
// Anything else is counted as skipped and left untouched.
ExtractStats extractEntries(const io::ByteSource& file,
                           const dat::DatIndex& index,
                           const dat::NameTable& names,
                           const ExtractOptions& options,
                           std::ostream& manifest,
                           std::vector<std::string>& errors);

}  // namespace gamecore::extract
