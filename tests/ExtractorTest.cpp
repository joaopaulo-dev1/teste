#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/DatExtract/Extractor.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::cerr << "FAIL " << __LINE__ << ": " #cond << std::endl; \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

void testSanitize() {
    std::filesystem::path out;
    std::string reason;
    CHECK(gamecore::extract::sanitizeRelativePath("audio/vo/dx_line.ogg", out, reason));
    CHECK(out == std::filesystem::path("audio") / "vo" / "dx_line.ogg");
    CHECK(!gamecore::extract::sanitizeRelativePath("../evil.txt", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("a/../../b", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("/abs/path", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("c:/windows/x", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("dir/con.txt", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("dir//x", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("trailing./x", out, reason));
    CHECK(!gamecore::extract::sanitizeRelativePath("", out, reason));
}

gamecore::dat::NameNode node(std::int16_t link, std::uint16_t parent, const char* name) {
    gamecore::dat::NameNode n;
    n.link = link;
    n.parent = parent;
    n.name = name;
    return n;
}

void testExtractStoredOnly() {
    // Data file: stored "hello" at 0x100, a packed blob at 0x200.
    std::vector<std::uint8_t> bytes(0x300, 0);
    std::memcpy(bytes.data() + 0x100, "hello", 5);
    std::memcpy(bytes.data() + 0x200, "LZ2Kxxxx", 8);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);

    gamecore::dat::DatIndex index;
    index.entries.resize(3);
    index.entries[0] = {0x1, 5, 5, 0x00000000u};
    index.entries[1] = {0x2, 8, 64, 0x00000002u};
    index.entries[2] = {0x1, 5, 5, 0x00000000u};

    gamecore::dat::NameTable names;
    names.nodes = {node(1, 0xffff, ""), node(4, 0, ""), node(0, 1, "greeting.txt"), node(-1, 1, "packed.tex"),
                   node(-2, 1, "..")};
    names.fileIdToNode = {2, 3, 4};
    names.parsed = true;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("gamecore-extract-" + std::to_string(stamp));
    gamecore::extract::ExtractOptions options;
    options.outRoot = root;

    CHECK(gamecore::extract::plannedBytes(index, names, {}, false) == 10);
    CHECK(gamecore::extract::plannedBytes(index, names, {".txt"}, false) == 5);
    CHECK(gamecore::extract::plannedBytes(index, names, {}, true) == 74);

    std::ostringstream manifest;
    std::vector<std::string> errors;
    const auto stats = gamecore::extract::extractEntries(source, index, names, options, manifest, errors);
    CHECK(stats.written == 1);
    CHECK(stats.skippedCompressed == 1);
    CHECK(stats.rejectedPaths == 1);
    CHECK(stats.errors == 0);
    CHECK(errors.size() == 1);

    std::ifstream in(root / "greeting.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(content == "hello");
    CHECK(!std::filesystem::exists(root / "packed.tex"));
    CHECK(manifest.str().find("greeting.txt") != std::string::npos);

    std::ostringstream second;
    errors.clear();
    const auto again = gamecore::extract::extractEntries(source, index, names, options, second, errors);
    CHECK(again.reused == 1);
    CHECK(again.written == 0);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
}

void testExtractDecompressesLz2k() {
    // LZ2K entry at 0x100 with one verbatim chunk "world"; a broken LZ2K entry at 0x200.
    std::vector<std::uint8_t> bytes(0x300, 0);
    std::memcpy(bytes.data() + 0x100, "LZ2K", 4);
    putU32(bytes, 0x104, 5);
    putU32(bytes, 0x108, 5);
    std::memcpy(bytes.data() + 0x10c, "world", 5);
    std::memcpy(bytes.data() + 0x200, "LZ2K", 4);
    putU32(bytes, 0x204, 99);
    putU32(bytes, 0x208, 4);
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);

    gamecore::dat::DatIndex index;
    index.entries.resize(2);
    index.entries[0] = {0x1, 17, 5, 0x00000002u};
    index.entries[1] = {0x2, 16, 99, 0x00000002u};

    gamecore::dat::NameTable names;
    names.nodes = {node(1, 0xffff, ""), node(3, 0, ""), node(0, 1, "word.txt"), node(-1, 1, "broken.tex")};
    names.fileIdToNode = {2, 3};
    names.parsed = true;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("gamecore-lz2k-" + std::to_string(stamp));
    gamecore::extract::ExtractOptions options;
    options.outRoot = root;
    options.decompress = true;

    std::ostringstream manifest;
    std::vector<std::string> errors;
    const auto stats = gamecore::extract::extractEntries(source, index, names, options, manifest, errors);
    CHECK(stats.written == 1);
    CHECK(stats.decompressed == 1);
    CHECK(stats.errors == 1);
    CHECK(stats.bytes == 5);
    CHECK(errors.size() == 1);

    std::ifstream in(root / "word.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(content == "world");
    CHECK(!std::filesystem::exists(root / "broken.tex"));
    CHECK(!std::filesystem::exists(root / "broken.tex.part"));
    CHECK(manifest.str().find("decompressed") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
    testSanitize();
    testExtractStoredOnly();
    testExtractDecompressesLz2k();
    if (g_failed != 0) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "extractor tests passed\n";
    return 0;
}
