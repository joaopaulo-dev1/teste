#include "Tools/DatProbe/EntryProbe.hpp"

#include "Tools/AssetInspector/FormatProbe.hpp"

#include <algorithm>
#include <set>

namespace gamecore::dat {
namespace {

constexpr std::uint64_t kHeaderBytes = 0x100;
constexpr std::uint64_t kRecordSize = 16;
constexpr std::uint64_t kPreambleSize = 8;

std::uint32_t loadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string leadBytesKey(const io::ByteSource& file, std::uint64_t offset) {
    std::uint8_t lead[4];
    if (!io::readExact(file, offset, lead, sizeof(lead))) {
        return "(unreadable)";
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    key.reserve(20);
    for (std::uint8_t byte : lead) {
        key.push_back(kHex[byte >> 4]);
        key.push_back(kHex[byte & 0xf]);
        key.push_back(' ');
    }
    key.push_back('|');
    for (std::uint8_t byte : lead) {
        key.push_back(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.');
    }
    key.push_back('|');
    return key;
}

bool isStored(const RawEntry& entry) { return (entry.flags & 0xffu) == 0 && entry.b == entry.c; }

OffsetRuleResult evaluateRule(const io::ByteSource& file,
                              const std::vector<RawEntry>& entries,
                              std::uint64_t tableOffset,
                              OffsetRule rule,
                              std::uint64_t sampleCap) {
    OffsetRuleResult result;
    result.rule = rule;

    struct Span {
        std::uint64_t start;
        std::uint64_t end;
    };
    std::vector<Span> spans;
    spans.reserve(entries.size());
    for (const RawEntry& entry : entries) {
        const std::uint64_t start = entryOffset(entry, rule);
        const std::uint64_t end = start + entry.b;
        if (end > tableOffset) {
            ++result.outOfBounds;
            continue;
        }
        if (start < kHeaderBytes && entry.b != 0) {
            ++result.belowHeader;
        }
        if (entry.b != 0) {
            spans.push_back(Span{start, end});
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& x, const Span& y) { return x.start < y.start; });
    std::uint64_t cursor = 0;
    bool first = true;
    for (const Span& span : spans) {
        if (!first && span.start < cursor) {
            ++result.overlaps;
            if (span.end > cursor) {
                result.coveredBytes += span.end - cursor;
                cursor = span.end;
            }
            continue;
        }
        if (!first) {
            result.maxGap = std::max(result.maxGap, span.start - cursor);
        }
        result.coveredBytes += span.end - span.start;
        cursor = span.end;
        first = false;
    }

    for (const RawEntry& entry : entries) {
        const std::uint64_t start = entryOffset(entry, rule);
        if (entry.b < 4 || start + entry.b > tableOffset) {
            continue;
        }
        if (isStored(entry)) {
            if (result.storedSampled >= sampleCap) {
                continue;
            }
            io::SliceBytes storage;
            const io::ByteSource slice = io::sliceOf(file, start, entry.b, storage);
            const auto hit = inspect::probeFormat(slice);
            if (hit.recognized) {
                ++result.storedRecognized;
                ++result.storedFormats[hit.name];
            } else {
                ++result.storedFormats[leadBytesKey(file, start)];
            }
            ++result.storedSampled;
        } else if (result.compressedSampled < sampleCap) {
            ++result.compressedLeadBytes[leadBytesKey(file, start)];
            ++result.compressedSampled;
        }
    }
    return result;
}

}  // namespace

const char* toString(OffsetRule rule) {
    switch (rule) {
        case OffsetRule::Raw:
            return "a";
        case OffsetRule::Shl8:
            return "a<<8";
        case OffsetRule::Shl8OrHighByte:
            return "(a<<8)|flags>>24";
    }
    return "a";
}

std::uint64_t entryOffset(const RawEntry& entry, OffsetRule rule) {
    switch (rule) {
        case OffsetRule::Raw:
            return entry.a;
        case OffsetRule::Shl8:
            return static_cast<std::uint64_t>(entry.a) << 8;
        case OffsetRule::Shl8OrHighByte:
            return (static_cast<std::uint64_t>(entry.a) << 8) | (entry.flags >> 24);
    }
    return entry.a;
}

EntryAnalysis analyzeEntries(const io::ByteSource& file, std::uint64_t tableOffset, std::uint64_t tableSize,
                             std::uint64_t sampleCap) {
    EntryAnalysis analysis;
    if (tableSize < kPreambleSize || tableSize > (64ull << 20)) {
        analysis.failure = "table size outside 8 B .. 64 MiB";
        return analysis;
    }
    std::vector<std::uint8_t> table(static_cast<std::size_t>(tableSize));
    if (!io::readExact(file, tableOffset, table.data(), table.size())) {
        analysis.failure = "table unreadable";
        return analysis;
    }
    analysis.preamble = loadU32(table.data());
    analysis.declaredCount = loadU32(table.data() + 4);
    analysis.recordBytes = static_cast<std::uint64_t>(analysis.declaredCount) * kRecordSize;
    if (kPreambleSize + analysis.recordBytes > tableSize) {
        analysis.failure = "declared count * 16 does not fit in the table";
        return analysis;
    }
    analysis.trailingBytes = tableSize - kPreambleSize - analysis.recordBytes;
    analysis.dataRegionBytes = tableOffset > kHeaderBytes ? tableOffset - kHeaderBytes : 0;

    std::vector<RawEntry> entries(analysis.declaredCount);
    std::set<std::uint32_t> highBytes;
    for (std::uint32_t i = 0; i < analysis.declaredCount; ++i) {
        const std::uint8_t* record = table.data() + kPreambleSize + static_cast<std::size_t>(i) * kRecordSize;
        RawEntry& entry = entries[i];
        entry.a = loadU32(record);
        entry.b = loadU32(record + 4);
        entry.c = loadU32(record + 8);
        entry.flags = loadU32(record + 12);

        const std::uint32_t low = entry.flags & 0xffu;
        const bool lowZero = low == 0;
        const bool equal = entry.b == entry.c;
        if (equal && lowZero) {
            ++analysis.equalAndLowZero;
        } else if (equal) {
            ++analysis.equalAndLowNonZero;
        } else if (lowZero) {
            ++analysis.differentAndLowZero;
        } else {
            ++analysis.differentAndLowNonZero;
        }
        if (entry.b > entry.c) {
            ++analysis.bGreaterThanC;
        }
        ++analysis.lowByteHistogram[low];
        ++analysis.middleBytesHistogram[(entry.flags >> 8) & 0xffffu];
        highBytes.insert(entry.flags >> 24);
    }
    analysis.distinctHighBytes = highBytes.size();

    for (OffsetRule rule : {OffsetRule::Raw, OffsetRule::Shl8, OffsetRule::Shl8OrHighByte}) {
        analysis.rules.push_back(evaluateRule(file, entries, tableOffset, rule, sampleCap));
    }

    if (analysis.trailingBytes > 0) {
        const std::uint8_t* tail = table.data() + kPreambleSize + analysis.recordBytes;
        std::uint64_t printable = 0;
        for (std::uint64_t i = 0; i < analysis.trailingBytes; ++i) {
            const std::uint8_t byte = tail[i];
            if ((byte >= 32 && byte < 127) || byte == 0) {
                ++printable;
            }
        }
        analysis.trailingPrintableRatio =
            static_cast<double>(printable) / static_cast<double>(analysis.trailingBytes);
        io::MemoryBytes tailStorage{tail, analysis.trailingBytes};
        const io::ByteSource tailSource = io::sourceFromMemory(tailStorage);
        analysis.trailingHead = inspect::hexAsciiDump(tailSource, 512);
        for (std::uint64_t i = 0; i < 16 && (i + 1) * 4 <= analysis.trailingBytes; ++i) {
            analysis.trailingLeadWords.push_back(loadU32(tail + i * 4));
        }
    }
    analysis.parsed = true;
    return analysis;
}

}  // namespace gamecore::dat
