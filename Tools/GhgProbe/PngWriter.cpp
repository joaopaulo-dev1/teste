#include "Tools/GhgProbe/PngWriter.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>

namespace gamecore::image {
namespace {

std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes) {
    static const auto table = makeCrcTable();
    std::uint32_t c = 0xffffffffu;
    for (const auto b : bytes) {
        c = table[(c ^ b) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

void putBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void writeChunk(std::ofstream& file, const char* type, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    std::vector<std::uint8_t> head;
    putBe32(head, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> tail;
    putBe32(tail, crc32(body));
    file.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    file.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    file.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
}

}  // namespace

bool writePngRgb(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 const std::vector<std::uint8_t>& rgb) {
    if (width == 0 || height == 0 || rgb.size() != static_cast<std::size_t>(width) * height * 3) {
        return false;
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 3 + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto row = rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * width * 3);
        raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(width) * 3);
    }

    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(n & 0xff));
        zlib.push_back(static_cast<std::uint8_t>(n >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~n & 0xff));
        zlib.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xff));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    putBe32(zlib, (b << 16) | a);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    file.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    std::vector<std::uint8_t> ihdr;
    putBe32(ihdr, width);
    putBe32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    writeChunk(file, "IHDR", ihdr);
    writeChunk(file, "IDAT", zlib);
    writeChunk(file, "IEND", {});
    return static_cast<bool>(file);
}

}  // namespace gamecore::image
