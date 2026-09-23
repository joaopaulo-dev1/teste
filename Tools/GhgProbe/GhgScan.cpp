#include "Tools/GhgProbe/GhgScan.hpp"

#include <algorithm>
#include <cstring>

namespace gamecore::ghg {
namespace {

std::uint32_t be32(std::span<const std::uint8_t> b, std::uint64_t at) {
    const auto i = static_cast<std::size_t>(at);
    return (static_cast<std::uint32_t>(b[i]) << 24) | (static_cast<std::uint32_t>(b[i + 1]) << 16) |
           (static_cast<std::uint32_t>(b[i + 2]) << 8) | static_cast<std::uint32_t>(b[i + 3]);
}

std::uint16_t le16(std::span<const std::uint8_t> b, std::uint64_t at) {
    const auto i = static_cast<std::size_t>(at);
    return static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8));
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::uint64_t at) {
    const auto i = static_cast<std::size_t>(at);
    return static_cast<std::uint32_t>(b[i]) | (static_cast<std::uint32_t>(b[i + 1]) << 8) |
           (static_cast<std::uint32_t>(b[i + 2]) << 16) | (static_cast<std::uint32_t>(b[i + 3]) << 24);
}

bool isTagChar(std::uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

constexpr std::uint32_t kVertexMarker = 0x502;
constexpr std::uint32_t kIndexMarker = 0x102;

bool tryVertexBuffer(std::span<const std::uint8_t> b, std::uint64_t at, std::uint64_t end, VertexBuffer& vb) {
    if (at + 20 > end || be32(b, at) != kVertexMarker || std::memcmp(b.data() + at + 8, "DXTV", 4) != 0) {
        return false;
    }
    vb = {};
    vb.markerOffset = at;
    vb.count = be32(b, at + 4);
    vb.version = be32(b, at + 12);
    const std::uint32_t elementCount = be32(b, at + 16);
    if (elementCount == 0 || elementCount > 16 || vb.count == 0) {
        return false;
    }
    std::uint64_t pos = at + 20;
    if (pos + elementCount * 3ull + 6 > end) {
        return false;
    }
    bool allStreamZero = true;
    std::uint32_t stride = 0;
    for (std::uint32_t i = 0; i < elementCount; ++i, pos += 3) {
        VertexElement e;
        e.usage = b[static_cast<std::size_t>(pos)];
        e.stream = static_cast<std::uint8_t>(b[static_cast<std::size_t>(pos + 1)] >> 5);
        e.type = static_cast<std::uint8_t>(b[static_cast<std::size_t>(pos + 1)] & 0x1f);
        e.offset = b[static_cast<std::size_t>(pos + 2)];
        const unsigned size = elementTypeSize(e.type);
        if (size == 0) {
            vb.unknownType = true;
        }
        if (e.stream != 0) {
            allStreamZero = false;
        } else {
            stride = std::max<std::uint32_t>(stride, e.offset + size);
        }
        vb.elements.push_back(e);
    }
    pos += 6;
    vb.dataOffset = pos;
    vb.stride = stride;
    vb.inlineData = allStreamZero && !vb.unknownType && stride != 0 &&
                    static_cast<std::uint64_t>(vb.count) * stride <= end - pos;
    return true;
}

bool tryIndexBuffer(std::span<const std::uint8_t> b, std::uint64_t at, std::uint64_t end, IndexBuffer& ib) {
    if (at + 12 > end || be32(b, at) != kIndexMarker) {
        return false;
    }
    ib = {};
    ib.markerOffset = at;
    ib.count = be32(b, at + 4);
    ib.indexSize = be32(b, at + 8);
    ib.dataOffset = at + 12;
    if ((ib.indexSize != 2 && ib.indexSize != 4) || ib.count == 0 || ib.count % 3 != 0 ||
        static_cast<std::uint64_t>(ib.count) * ib.indexSize > end - ib.dataOffset) {
        return false;
    }
    for (std::uint32_t i = 0; i < ib.count; ++i) {
        const auto at2 = ib.dataOffset + static_cast<std::uint64_t>(i) * ib.indexSize;
        const std::uint32_t index = ib.indexSize == 2 ? le16(b, at2) : le32(b, at2);
        ib.maxIndex = std::max(ib.maxIndex, index);
    }
    return true;
}

}  // namespace

Container parseContainer(std::span<const std::uint8_t> file) {
    Container c;
    if (file.size() < 4 + 12 + kMetaTrailerBytes) {
        c.failure = "file too small";
        return c;
    }
    c.reshSize = be32(file, 0);
    if (file.size() < 8 + 12) {
        c.failure = "file too small";
        return c;
    }
    const std::uint64_t nu20 = 4ull + c.reshSize;
    if (nu20 + 12 > file.size() || std::memcmp(file.data() + 8, "HSERHSER", 8) != 0) {
        c.failure = "missing RESH header";
        return c;
    }
    const std::uint32_t nuSize = be32(file, nu20);
    if (be32(file, nu20 + 4) != 1 || std::memcmp(file.data() + nu20 + 8, "02UN", 4) != 0) {
        c.failure = "missing NU20 block";
        return c;
    }
    if (nu20 + 4 + nuSize + kMetaTrailerBytes != file.size()) {
        c.failure = "NU20 size does not end 128 bytes before EOF";
        return c;
    }
    c.nu20Offset = nu20;
    c.nu20End = nu20 + 4 + nuSize;
    c.valid = true;
    return c;
}

unsigned elementTypeSize(std::uint8_t type) {
    switch (type) {
        case 5:  // 2 x half
        case 7:  // 4 bytes, interpretation open
        case 8:  // 4 x unorm8
        case 9:  // colour, 4 x unorm8
            return 4;
        case 6:  // 4 x half
            return 8;
        default:
            return 0;
    }
}

Scan scanObjects(std::span<const std::uint8_t> file, const Container& container) {
    Scan scan;
    if (!container.valid) {
        return scan;
    }
    const std::uint64_t end = container.nu20End;
    std::uint64_t pos = container.nu20Offset + 12;
    while (pos + 8 <= end) {
        VertexBuffer vb;
        if (tryVertexBuffer(file, pos, end, vb)) {
            const std::uint64_t next = vb.inlineData ? vb.dataOffset + static_cast<std::uint64_t>(vb.count) * vb.stride
                                                     : vb.dataOffset;
            scan.vertexBuffers.push_back(std::move(vb));
            ++scan.tagCounts["DXTV"];
            pos = next;
            continue;
        }
        IndexBuffer ib;
        if (tryIndexBuffer(file, pos, end, ib)) {
            scan.indexBuffers.push_back(ib);
            pos = ib.dataOffset + static_cast<std::uint64_t>(ib.count) * ib.indexSize;
            continue;
        }
        const auto i = static_cast<std::size_t>(pos);
        if (isTagChar(file[i]) && isTagChar(file[i + 1]) && isTagChar(file[i + 2]) && isTagChar(file[i + 3])) {
            const std::string tag(reinterpret_cast<const char*>(file.data() + i), 4);
            const std::uint32_t version = be32(file, pos + 4);
            if (version < 0x1000) {
                ++scan.tagCounts[tag];
                scan.tagVersions[tag] = version;
                pos += 8;
                continue;
            }
        }
        ++pos;
    }
    return scan;
}

float halfToFloat(std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
    std::uint32_t exponent = (half >> 10) & 0x1fu;
    std::uint32_t mantissa = half & 0x3ffu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool readPositions(std::span<const std::uint8_t> file, const VertexBuffer& vb, std::vector<Float3>& out,
                   std::uint64_t& wNotOne) {
    out.clear();
    wNotOne = 0;
    if (!vb.inlineData) {
        return false;
    }
    const VertexElement* position = nullptr;
    for (const auto& e : vb.elements) {
        if (e.usage == 0 && e.stream == 0 && e.type == 6) {
            position = &e;
        }
    }
    if (position == nullptr) {
        return false;
    }
    out.reserve(vb.count);
    for (std::uint32_t v = 0; v < vb.count; ++v) {
        const std::uint64_t at = vb.dataOffset + static_cast<std::uint64_t>(v) * vb.stride + position->offset;
        Float3 p;
        p.x = halfToFloat(le16(file, at));
        p.y = halfToFloat(le16(file, at + 2));
        p.z = halfToFloat(le16(file, at + 4));
        if (le16(file, at + 6) != 0x3c00) {
            ++wNotOne;
        }
        out.push_back(p);
    }
    return true;
}

}  // namespace gamecore::ghg
