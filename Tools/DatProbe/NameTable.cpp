#include "Tools/DatProbe/NameTable.hpp"

#include <algorithm>

namespace gamecore::dat {
namespace {

constexpr std::size_t kNodeSize = 12;
constexpr std::uint16_t kNoParent = 0xffff;

std::uint16_t loadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t loadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

NameTable parseNameTable(std::span<const std::uint8_t> trailing, std::uint32_t entryCount) {
    NameTable table;
    if (trailing.size() < 8) {
        table.failure = "trailing region shorter than 8 bytes";
        return table;
    }
    table.nodeCount = loadU32(trailing.data());
    const std::uint64_t nodeBytes = static_cast<std::uint64_t>(table.nodeCount) * kNodeSize;
    if (table.nodeCount == 0 || 4 + nodeBytes + 4 > trailing.size()) {
        table.failure = "node count * 12 does not fit";
        return table;
    }
    const std::size_t namesSizeAt = static_cast<std::size_t>(4 + nodeBytes);
    table.namesSize = loadU32(trailing.data() + namesSizeAt);
    const std::size_t namesAt = namesSizeAt + 4;
    if (namesAt + table.namesSize > trailing.size()) {
        table.failure = "name block size exceeds the trailing region";
        return table;
    }
    table.bytesAfterNames = trailing.size() - namesAt - table.namesSize;
    const std::uint8_t* names = trailing.data() + namesAt;

    table.nodes.resize(table.nodeCount);
    for (std::uint32_t i = 0; i < table.nodeCount; ++i) {
        const std::uint8_t* raw = trailing.data() + 4 + static_cast<std::size_t>(i) * kNodeSize;
        NameNode& node = table.nodes[i];
        node.link = static_cast<std::int16_t>(loadU16(raw));
        node.prevSibling = loadU16(raw + 2);
        node.nameOffset = loadU32(raw + 4);
        node.parent = loadU16(raw + 8);
        node.extra = loadU16(raw + 10);
        if (i == 0) {
            continue;
        }
        if (node.nameOffset >= table.namesSize) {
            ++table.badNameOffsets;
        } else {
            const auto* begin = names + node.nameOffset;
            const auto* end = names + table.namesSize;
            const auto* terminator = std::find(begin, end, std::uint8_t{0});
            if (terminator == end) {
                ++table.badNameOffsets;
            } else {
                node.name.assign(reinterpret_cast<const char*>(begin), static_cast<std::size_t>(terminator - begin));
            }
        }
        if (node.parent >= table.nodeCount) {
            ++table.parentOutOfRange;
        } else if (node.parent >= i) {
            ++table.parentNotEarlier;
        }
        if (node.prevSibling >= table.nodeCount) {
            ++table.prevOutOfRange;
        }
        if (node.extra != 0) {
            ++table.extraNonZero;
        }
    }

    for (std::uint32_t i = 1; i < table.nodeCount; ++i) {
        std::uint32_t cursor = i;
        std::uint32_t steps = 0;
        while (cursor != 0 && cursor < table.nodeCount && steps <= table.nodeCount) {
            const std::uint16_t parent = table.nodes[cursor].parent;
            if (parent == kNoParent) {
                break;
            }
            cursor = parent;
            ++steps;
        }
        if (steps > table.nodeCount) {
            ++table.cycles;
        }
    }

    // A node is a directory when some other node names it as parent. Files are the rest.
    std::vector<bool> hasChildren(table.nodeCount, false);
    for (std::uint32_t i = 1; i < table.nodeCount; ++i) {
        const std::uint16_t parent = table.nodes[i].parent;
        if (parent < table.nodeCount) {
            hasChildren[parent] = true;
        }
    }
    table.fileIdToNode.assign(entryCount, -1);
    for (std::uint32_t i = 1; i < table.nodeCount; ++i) {
        if (hasChildren[i]) {
            ++table.directoryNodes;
            continue;
        }
        ++table.fileNodes;
        const int id = -static_cast<int>(table.nodes[i].link);
        if (id < 0 || static_cast<std::uint32_t>(id) >= entryCount) {
            ++table.idsOutOfRange;
            continue;
        }
        if (table.fileIdToNode[static_cast<std::size_t>(id)] != -1) {
            ++table.duplicateIds;
            continue;
        }
        table.fileIdToNode[static_cast<std::size_t>(id)] = static_cast<int>(i);
    }
    table.missingIds = static_cast<std::uint64_t>(
        std::count(table.fileIdToNode.begin(), table.fileIdToNode.end(), -1));
    table.parsed = true;
    return table;
}

std::string fullPath(const NameTable& table, int nodeIndex) {
    std::vector<const std::string*> parts;
    std::uint32_t cursor = static_cast<std::uint32_t>(nodeIndex);
    std::uint32_t guard = 0;
    while (cursor != 0 && cursor < table.nodes.size() && guard++ <= table.nodes.size()) {
        const NameNode& node = table.nodes[cursor];
        if (!node.name.empty()) {
            parts.push_back(&node.name);
        }
        if (node.parent == kNoParent) {
            break;
        }
        cursor = node.parent;
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) {
            path.push_back('/');
        }
        path += **it;
    }
    return path;
}

}  // namespace gamecore::dat
