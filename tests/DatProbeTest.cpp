#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/DatProbe/DatProbe.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"
#include "Tools/DatProbe/DatListing.hpp"
#include "Tools/DatProbe/EntryProbe.hpp"
#include "Tools/DatProbe/NameTable.hpp"

#include <string>

#include <cstring>

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int g_failed = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "FAIL " << __LINE__ << ": " #cond << std::endl;        \
            ++g_failed;                                                         \
        }                                                                       \
    } while (0)

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
}

std::vector<std::uint8_t> makeSynthetic(std::uint64_t tableOffset, std::uint32_t tableSize) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(tableOffset + tableSize), 0);
    const auto encoded = static_cast<std::uint32_t>(~((tableOffset - 0x100) >> 8));
    putU32(bytes, 0, encoded);
    putU32(bytes, 4, tableSize);
    for (std::size_t i = 0x100; i < static_cast<std::size_t>(tableOffset); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 37u);
    }
    putU32(bytes, static_cast<std::size_t>(tableOffset), 0x12345678u);
    return bytes;
}

void testMatchesEncodedOffset() {
    const auto bytes = makeSynthetic(0x3100, 0x90);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    const auto result = gamecore::dat::probeDat(source);
    CHECK(result.headerRead);
    CHECK(result.matchedTransform == "negated<<8");
    CHECK(result.tableOffset == 0x3100);
    CHECK(result.tableSize == 0x90);
    CHECK(!result.tableLeadWords.empty());
    CHECK(result.tableLeadWords[0] == 0x12345678u);
    CHECK(result.zeroPaddingBytes >= 56);
}

void testNoMatchWhenTableDoesNotReachEof() {
    auto bytes = makeSynthetic(0x3100, 0x90);
    bytes.resize(bytes.size() + 17, 0xee);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    const auto result = gamecore::dat::probeDat(source);
    CHECK(result.matchedTransform.empty());
}

void testTruncatedHeader() {
    const std::vector<std::uint8_t> bytes = {1, 2, 3};
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    const auto result = gamecore::dat::probeDat(source);
    CHECK(!result.headerRead);
}

void testEntryAnalysis() {
    // Data: stored PNG at 0x100 (16 bytes), packed blob at 0x110 (8 bytes -> 32), overlapping blob at 0x114.
    constexpr std::uint64_t tableOffset = 0x200;
    std::vector<std::uint8_t> table(8 + 3 * 16 + 6, 0);
    putU32(table, 0, 0xfffffffau);
    putU32(table, 4, 3);
    const std::uint32_t records[3][4] = {
        {0x100, 16, 16, 0x90000000u},
        {0x110, 8, 32, 0x95000002u},
        {0x114, 4, 4, 0x11000000u},
    };
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t f = 0; f < 4; ++f) {
            putU32(table, 8 + r * 16 + f * 4, records[r][f]);
        }
    }
    std::memcpy(table.data() + 8 + 48, "a.tex\0", 6);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(tableOffset), 0);
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::memcpy(bytes.data() + 0x100, png, sizeof(png));
    std::memcpy(bytes.data() + 0x110, "LZ2K", 4);
    bytes.insert(bytes.end(), table.begin(), table.end());

    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    const auto analysis = gamecore::dat::analyzeEntries(source, tableOffset, table.size(), 100);
    CHECK(analysis.parsed);
    CHECK(analysis.declaredCount == 3);
    CHECK(analysis.trailingBytes == 6);
    CHECK(analysis.equalAndLowZero == 2);
    CHECK(analysis.differentAndLowNonZero == 1);
    CHECK(analysis.trailingPrintableRatio == 1.0);
    CHECK(analysis.rules.size() == 3);
    const auto& raw = analysis.rules[0];
    CHECK(raw.rule == gamecore::dat::OffsetRule::Raw);
    CHECK(raw.outOfBounds == 0);
    CHECK(raw.overlaps == 1);
    CHECK(raw.storedRecognized == 1);
    CHECK(raw.storedFormats.count("png") == 1);
    CHECK(raw.compressedLeadBytes.size() == 1);
    CHECK(raw.compressedLeadBytes.begin()->first.find("LZ2K") != std::string::npos);
    const auto& shifted = analysis.rules[1];
    CHECK(shifted.outOfBounds == 3);
}

void testShiftedOffsetRule() {
    gamecore::dat::RawEntry entry;
    entry.a = 0x12;
    entry.flags = 0x34000002u;
    CHECK(gamecore::dat::entryOffset(entry, gamecore::dat::OffsetRule::Raw) == 0x12);
    CHECK(gamecore::dat::entryOffset(entry, gamecore::dat::OffsetRule::Shl8) == 0x1200);
    CHECK(gamecore::dat::entryOffset(entry, gamecore::dat::OffsetRule::Shl8OrHighByte) == 0x1234);
}

void putU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

constexpr std::size_t kNameNodes = 5;

// Tree: sentinel(0) -> root "" (1) -> "a.txt" (file id 1), dir "sub" (3) -> "b.ogg" (file id 0)
std::vector<std::uint8_t> makeNameTrailing() {
    const std::string namesBlob = std::string("\0\0a.txt\0\0sub\0\0b.ogg\0", 20);
    struct Node {
        std::int16_t link;
        std::uint16_t prev;
        std::uint32_t name;
        std::uint16_t parent;
    };
    const Node nodes[] = {
        {1, 0, 0xdeadbeefu, 0xffff},
        {3, 0, 0, 0},
        {-1, 0, 2, 1},
        {4, 2, 9, 1},
        {0, 0, 14, 3},
    };
    constexpr std::size_t kNodes = sizeof(nodes) / sizeof(nodes[0]);
    static_assert(kNodes == kNameNodes);
    std::vector<std::uint8_t> trailing(4 + kNodes * 12 + 4 + namesBlob.size() + 8, 0);
    putU32(trailing, 0, static_cast<std::uint32_t>(kNodes));
    for (std::size_t i = 0; i < kNodes; ++i) {
        const std::size_t at = 4 + i * 12;
        putU16(trailing, at, static_cast<std::uint16_t>(nodes[i].link));
        putU16(trailing, at + 2, nodes[i].prev);
        putU32(trailing, at + 4, nodes[i].name);
        putU16(trailing, at + 8, nodes[i].parent);
    }
    const std::size_t namesSizeAt = 4 + kNodes * 12;
    putU32(trailing, namesSizeAt, static_cast<std::uint32_t>(namesBlob.size()));
    std::memcpy(trailing.data() + namesSizeAt + 4, namesBlob.data(), namesBlob.size());
    return trailing;
}

void testNameTable() {
    const auto trailing = makeNameTrailing();
    const auto table = gamecore::dat::parseNameTable(trailing, 2);
    CHECK(table.parsed);
    CHECK(table.nodeCount == kNameNodes);
    CHECK(table.bytesAfterNames == 8);
    CHECK(table.badNameOffsets == 0);
    CHECK(table.fileNodes == 2);
    CHECK(table.directoryNodes == 2);
    CHECK(table.missingIds == 0);
    CHECK(table.duplicateIds == 0);
    CHECK(table.fileIdToNode[0] == 4);
    CHECK(table.fileIdToNode[1] == 2);
    CHECK(gamecore::dat::fullPath(table, 4) == "sub/b.ogg");
    CHECK(gamecore::dat::fullPath(table, 2) == "a.txt");
    CHECK(gamecore::dat::lowerExtension("B.OGG") == ".ogg");
}

std::vector<std::uint8_t> makeFullDat() {
    constexpr std::uint64_t tableOffset = 0x200;
    const auto trailing = makeNameTrailing();
    std::vector<std::uint8_t> table(8 + 2 * 16, 0);
    putU32(table, 0, 0xfffffffau);
    putU32(table, 4, 2);
    const std::uint32_t records[2][4] = {
        {0x1, 4, 4, 0x00000000u},
        {0x1, 4, 16, 0x10000002u},
    };
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t f = 0; f < 4; ++f) {
            putU32(table, 8 + r * 16 + f * 4, records[r][f]);
        }
    }
    table.insert(table.end(), trailing.begin(), trailing.end());

    auto bytes = makeSynthetic(tableOffset, static_cast<std::uint32_t>(table.size()));
    std::memcpy(bytes.data() + 0x100, "OggS", 4);
    std::memcpy(bytes.data() + 0x110, "LZ2K", 4);
    std::memcpy(bytes.data() + static_cast<std::size_t>(tableOffset), table.data(), table.size());
    return bytes;
}

void testListDat() {
    const auto bytes = makeFullDat();
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    gamecore::dat::DatListing listing;
    std::string error;
    CHECK(gamecore::dat::listDat(source, listing, error));
    CHECK(error.empty());
    CHECK(listing.entries.size() == 2);
    CHECK(listing.storedCount == 1);
    CHECK(listing.compressedCount == 1);
    CHECK(listing.unnamedCount == 0);
    CHECK(listing.outOfBoundsCount == 0);
    CHECK(listing.originalBytes == 20);
    if (listing.entries.size() == 2) {
        CHECK(listing.entries[0].path == "sub/b.ogg");
        CHECK(listing.entries[0].offset == 0x100);
        CHECK(listing.entries[0].compression == 0);
        CHECK(listing.entries[1].path == "a.txt");
        CHECK(listing.entries[1].offset == 0x110);
        CHECK(listing.entries[1].originalSize == 16);
        CHECK(std::string(gamecore::dat::compressionName(listing.entries[1].compression)) == "LZ2K");
    }
}

void testListDatRejectsForeignFile() {
    const std::vector<std::uint8_t> bytes(0x400, 0x5a);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    gamecore::dat::DatListing listing;
    std::string error;
    CHECK(!gamecore::dat::listDat(source, listing, error));
    CHECK(!error.empty());
    CHECK(listing.entries.empty());
}

void testEntryCountTooLarge() {
    std::vector<std::uint8_t> table(8, 0);
    putU32(table, 0, 0xfffffffau);
    putU32(table, 4, 1000);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(table, storage);
    const auto analysis = gamecore::dat::analyzeEntries(source, 0, table.size(), 10);
    CHECK(!analysis.parsed);
}

}  // namespace

int main() {
    testEntryAnalysis();
    testShiftedOffsetRule();
    testNameTable();
    testEntryCountTooLarge();
    testMatchesEncodedOffset();
    testNoMatchWhenTableDoesNotReachEof();
    testTruncatedHeader();
    testListDat();
    testListDatRejectsForeignFile();
    if (g_failed != 0) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "dat probe tests passed\n";
    return 0;
}
