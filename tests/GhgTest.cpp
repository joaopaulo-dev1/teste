#include "Tools/GhgProbe/GhgScan.hpp"
#include "Tools/GhgProbe/PngWriter.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void le16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void text(std::vector<std::uint8_t>& out, const char* s) { out.insert(out.end(), s, s + std::strlen(s)); }

// RESH header, NU20 (version 78, as in the retail files) with one position buffer (3 vertices, 4 x half) and one u16 index buffer, META trailer.
std::vector<std::uint8_t> makeGhg(std::uint16_t thirdW) {
    std::vector<std::uint8_t> resh;
    text(resh, ".CC4HSERHSER");
    resh.resize(40, 0);

    std::vector<std::uint8_t> objects;
    text(objects, "HSEM");
    be32(objects, 175);
    be32(objects, 0x502);
    be32(objects, 3);
    text(objects, "DXTV");
    be32(objects, 169);
    be32(objects, 1);
    objects.insert(objects.end(), {0x00, 0x06, 0x00});
    objects.insert(objects.end(), 6, 0);
    const std::uint16_t xyz[3][3] = {{0x0000, 0x0000, 0x0000}, {0x3c00, 0x0000, 0x0000}, {0x0000, 0x4000, 0xbc00}};
    for (int v = 0; v < 3; ++v) {
        for (int k = 0; k < 3; ++k) {
            le16(objects, xyz[v][k]);
        }
        le16(objects, v == 2 ? thirdW : 0x3c00);
    }
    be32(objects, 0x102);
    be32(objects, 3);
    be32(objects, 2);
    le16(objects, 0);
    le16(objects, 1);
    le16(objects, 2);

    std::vector<std::uint8_t> file;
    be32(file, static_cast<std::uint32_t>(resh.size()));
    file.insert(file.end(), resh.begin(), resh.end());
    be32(file, static_cast<std::uint32_t>(12 + objects.size()));
    be32(file, 1);
    text(file, "02UN");
    be32(file, 78);
    file.insert(file.end(), objects.begin(), objects.end());
    file.insert(file.end(), gamecore::ghg::kMetaTrailerBytes - 4, 0);
    text(file, "META");
    return file;
}

void testContainerAndBuffers() {
    const auto file = makeGhg(0x3c00);
    const auto container = gamecore::ghg::parseContainer(file);
    CHECK(container.valid);
    CHECK(container.reshSize == 40);
    CHECK(container.nu20Offset == 44);
    CHECK(container.nu20End == file.size() - gamecore::ghg::kMetaTrailerBytes);

    const auto scan = gamecore::ghg::scanObjects(file, container);
    CHECK(scan.vertexBuffers.size() == 1);
    CHECK(scan.indexBuffers.size() == 1);
    CHECK(scan.tagVersions.count("HSEM") == 1 && scan.tagVersions.at("HSEM") == 175);
    if (scan.vertexBuffers.size() == 1) {
        const auto& vb = scan.vertexBuffers.front();
        CHECK(vb.count == 3);
        CHECK(vb.version == 169);
        CHECK(vb.stride == 8);
        CHECK(vb.inlineData);
        std::vector<gamecore::ghg::Float3> positions;
        std::uint64_t wNotOne = 99;
        CHECK(gamecore::ghg::readPositions(file, vb, positions, wNotOne));
        CHECK(wNotOne == 0);
        CHECK(positions.size() == 3);
        if (positions.size() == 3) {
            CHECK(positions[1].x == 1.0f);
            CHECK(positions[2].y == 2.0f);
            CHECK(positions[2].z == -1.0f);
        }
    }
    if (scan.indexBuffers.size() == 1) {
        CHECK(scan.indexBuffers.front().count == 3);
        CHECK(scan.indexBuffers.front().indexSize == 2);
        CHECK(scan.indexBuffers.front().maxIndex == 2);
    }
}

void testWCheckCountsBadVertices() {
    const auto file = makeGhg(0x0000);
    const auto container = gamecore::ghg::parseContainer(file);
    const auto scan = gamecore::ghg::scanObjects(file, container);
    CHECK(scan.vertexBuffers.size() == 1);
    if (!scan.vertexBuffers.empty()) {
        std::vector<gamecore::ghg::Float3> positions;
        std::uint64_t wNotOne = 0;
        CHECK(gamecore::ghg::readPositions(file, scan.vertexBuffers.front(), positions, wNotOne));
        CHECK(wNotOne == 1);
    }
}

void testContainerRejectsWrongTrailer() {
    auto file = makeGhg(0x3c00);
    file.push_back(0);
    CHECK(!gamecore::ghg::parseContainer(file).valid);
    file.resize(10);
    CHECK(!gamecore::ghg::parseContainer(file).valid);
}

void testHalfToFloat() {
    CHECK(gamecore::ghg::halfToFloat(0x3c00) == 1.0f);
    CHECK(gamecore::ghg::halfToFloat(0xc000) == -2.0f);
    CHECK(gamecore::ghg::halfToFloat(0x3800) == 0.5f);
    CHECK(gamecore::ghg::halfToFloat(0x0000) == 0.0f);
    CHECK(std::fabs(gamecore::ghg::halfToFloat(0x0001) - 5.9604645e-8f) < 1e-12f);
    CHECK(std::isinf(gamecore::ghg::halfToFloat(0x7c00)));
}

void testPngWriter() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("gamecore-png-" + std::to_string(stamp) + ".png");
    std::vector<std::uint8_t> rgb(2 * 2 * 3, 200);
    CHECK(gamecore::image::writePngRgb(path, 2, 2, rgb));
    std::ifstream in(path, std::ios::binary);
    std::vector<char> head(8);
    in.read(head.data(), 8);
    in.close();
    CHECK(static_cast<unsigned char>(head[0]) == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G');
    CHECK(!gamecore::image::writePngRgb(path, 3, 2, rgb));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    testContainerAndBuffers();
    testWCheckCountsBadVertices();
    testContainerRejectsWrongTrailer();
    testHalfToFloat();
    testPngWriter();
    if (g_failed != 0) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "ghg tests passed\n";
    return 0;
}
