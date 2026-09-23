#include "GameCore/AssetSystem/ByteSource.hpp"
#include "Tools/Lz2kProbe/Lz2k.hpp"
#include "Tools/Lz2kProbe/Lz2kContainer.hpp"
#include "Tools/Lz2kProbe/LzhDecoder.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
    }
}

void appendChunk(std::vector<std::uint8_t>& bytes, std::uint32_t raw, std::uint32_t packed) {
    bytes.insert(bytes.end(), {'L', 'Z', '2', 'K'});
    appendU32(bytes, raw);
    appendU32(bytes, packed);
    for (std::uint32_t i = 0; i < packed; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(i * 29u + 7u));
    }
}

gamecore::lz2k::Layout parse(const std::vector<std::uint8_t>& bytes, std::uint64_t expectedRaw) {
    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(bytes, storage);
    return gamecore::lz2k::parseLayout(source, expectedRaw);
}

void testTwoChunks() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 0x8000, 40);
    appendChunk(bytes, 100, 9);
    const auto layout = parse(bytes, 0x8000 + 100);
    CHECK(layout.valid);
    CHECK(layout.failure.empty());
    CHECK(layout.chunks.size() == 2);
    CHECK(layout.rawTotal == 0x8000 + 100);
    CHECK(layout.trailingBytes == 0);
    if (layout.chunks.size() == 2) {
        CHECK(layout.chunks[0].headerOffset == 0);
        CHECK(layout.chunks[0].payloadOffset() == 12);
        CHECK(layout.chunks[1].headerOffset == 12 + 40);
        CHECK(layout.chunks[1].rawSize == 100);
        CHECK(layout.chunks[1].packedSize == 9);
    }
}

void testRawMismatch() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 500, 20);
    const auto layout = parse(bytes, 501);
    CHECK(!layout.valid);
    CHECK(layout.failure.find("add up") != std::string::npos);
}

void testTrailingBytes() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 500, 20);
    bytes.insert(bytes.end(), {1, 2, 3, 4, 5});
    const auto layout = parse(bytes, 500);
    CHECK(!layout.valid);
    CHECK(layout.trailingBytes == 5);
}

void testBadMagic() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 500, 20);
    bytes[0] = 'X';
    const auto layout = parse(bytes, 500);
    CHECK(!layout.valid);
    CHECK(layout.chunks.empty());
}

void testPayloadPastEnd() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 500, 20);
    bytes.resize(bytes.size() - 1);
    const auto layout = parse(bytes, 500);
    CHECK(!layout.valid);
    CHECK(layout.failure.find("past") != std::string::npos);
}

void testZeroSizes() {
    std::vector<std::uint8_t> bytes;
    appendChunk(bytes, 0, 4);
    const auto layout = parse(bytes, 0);
    CHECK(!layout.valid);
}

class BitWriter {
public:
    void put(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            acc_ = static_cast<std::uint8_t>((acc_ << 1) | ((value >> i) & 1u));
            if (++used_ == 8) {
                bytes_.push_back(acc_);
                acc_ = 0;
                used_ = 0;
            }
        }
    }

    std::vector<std::uint8_t> finish() {
        if (used_ != 0) {
            bytes_.push_back(static_cast<std::uint8_t>(acc_ << (8 - used_)));
            acc_ = 0;
            used_ = 0;
        }
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint8_t acc_ = 0;
    unsigned used_ = 0;
};

// Hand-built -lh5- block: "ABC" as literals, then a length-3 match at distance 1 -> "ABCCCC".
// Temp code: symbols 3 and 11 at length 1 (codes 0 and 1). Char code: symbol 256 (match len 3) at
// length 1 (code 0), literals at length 9 (codes 256 + byte). Position code: single symbol 0.
std::vector<std::uint8_t> makeLh5Stream() {
    BitWriter w;
    w.put(4, 16);
    w.put(12, 5);
    for (unsigned i = 0; i < 12; ++i) {
        w.put(i == 3 || i == 11 ? 1u : 0u, 3);
        if (i == 2) {
            w.put(0, 2);
        }
    }
    w.put(257, 9);
    for (unsigned i = 0; i < 256; ++i) {
        w.put(1, 1);
    }
    w.put(0, 1);
    w.put(0, 4);
    w.put(0, 4);
    for (const char ch : std::string("ABC")) {
        w.put(256u + static_cast<unsigned char>(ch), 9);
    }
    w.put(0, 1);
    return w.finish();
}

void testLh5Decode() {
    const auto stream = makeLh5Stream();
    std::vector<std::uint8_t> out(6);
    const auto result = gamecore::lz2k::decodeLzh(stream, out, gamecore::lz2k::kLh5);
    CHECK(result.ok);
    CHECK(result.blocks == 1);
    CHECK(result.codesLeftInBlock == 0);
    CHECK((result.bitsConsumed + 7) / 8 == stream.size());
    CHECK(std::string(out.begin(), out.end()) == "ABCCCC");
}

void testLh5RejectsShortOutputBudget() {
    const auto stream = makeLh5Stream();
    std::vector<std::uint8_t> out(5);
    const auto result = gamecore::lz2k::decodeLzh(stream, out, gamecore::lz2k::kLh5);
    CHECK(!result.ok);
    CHECK(result.failure.find("past the declared raw size") != std::string::npos);
}

void testLh5RejectsTruncatedInput() {
    auto stream = makeLh5Stream();
    stream.resize(10);
    std::vector<std::uint8_t> out(6);
    const auto result = gamecore::lz2k::decodeLzh(stream, out, gamecore::lz2k::kLh5);
    CHECK(!result.ok);
}

void testDecompressEntryMixesStoredAndPacked() {
    const auto stream = makeLh5Stream();
    std::vector<std::uint8_t> entry;
    entry.insert(entry.end(), {'L', 'Z', '2', 'K'});
    appendU32(entry, 6);
    appendU32(entry, static_cast<std::uint32_t>(stream.size()));
    entry.insert(entry.end(), stream.begin(), stream.end());
    entry.insert(entry.end(), {'L', 'Z', '2', 'K'});
    appendU32(entry, 3);
    appendU32(entry, 3);
    entry.insert(entry.end(), {'x', 'y', 'z'});

    gamecore::io::MemoryBytes storage;
    const auto source = gamecore::io::sourceFromSpan(entry, storage);
    std::vector<std::uint8_t> out;
    std::string error;
    CHECK(gamecore::lz2k::decompressEntry(source, 9, out, error));
    CHECK(error.empty());
    CHECK(std::string(out.begin(), out.end()) == "ABCCCCxyz");

    CHECK(!gamecore::lz2k::decompressEntry(source, 10, out, error));
    CHECK(out.empty());
    CHECK(!error.empty());
}

}  // namespace

int main() {
    testLh5Decode();
    testLh5RejectsShortOutputBudget();
    testLh5RejectsTruncatedInput();
    testDecompressEntryMixesStoredAndPacked();
    testTwoChunks();
    testRawMismatch();
    testTrailingBytes();
    testBadMagic();
    testPayloadPastEnd();
    testZeroSizes();
    if (g_failed != 0) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "lz2k tests passed\n";
    return 0;
}
