#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gamecore::dat {

// Hypothesised record: 16 bytes after an 8-byte table preamble.
// Field names stay neutral until the checks below confirm a role.
struct RawEntry {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t flags = 0;
};

enum class OffsetRule {
    Raw,
    Shl8,
    Shl8OrHighByte,
};

const char* toString(OffsetRule rule);
std::uint64_t entryOffset(const RawEntry& entry, OffsetRule rule);

struct OffsetRuleResult {
    OffsetRule rule = OffsetRule::Raw;
    std::uint64_t outOfBounds = 0;
    std::uint64_t belowHeader = 0;
    std::uint64_t overlaps = 0;
    std::uint64_t coveredBytes = 0;
    std::uint64_t maxGap = 0;
    std::uint64_t storedSampled = 0;
    std::uint64_t storedRecognized = 0;
    std::map<std::string, std::uint64_t> storedFormats;
    std::uint64_t compressedSampled = 0;
    std::map<std::string, std::uint64_t> compressedLeadBytes;
};

struct EntryAnalysis {
    bool parsed = false;
    std::string failure;
    std::uint32_t preamble = 0;
    std::uint32_t declaredCount = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t trailingBytes = 0;
    std::uint64_t dataRegionBytes = 0;

    std::uint64_t equalAndLowZero = 0;
    std::uint64_t equalAndLowNonZero = 0;
    std::uint64_t differentAndLowZero = 0;
    std::uint64_t differentAndLowNonZero = 0;
    std::uint64_t bGreaterThanC = 0;
    std::map<std::uint32_t, std::uint64_t> lowByteHistogram;
    std::map<std::uint32_t, std::uint64_t> middleBytesHistogram;
    std::uint64_t distinctHighBytes = 0;

    std::vector<OffsetRuleResult> rules;

    double trailingPrintableRatio = 0.0;
    std::string trailingHead;
    std::vector<std::uint32_t> trailingLeadWords;
};

EntryAnalysis analyzeEntries(const io::ByteSource& file, std::uint64_t tableOffset, std::uint64_t tableSize,
                             std::uint64_t sampleCap);

}  // namespace gamecore::dat
