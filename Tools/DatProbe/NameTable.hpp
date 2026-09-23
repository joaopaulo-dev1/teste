#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gamecore::dat {

// Hypothesised node (12 bytes), derived from DLC5.DAT and checked by parseNameTable:
//   s16 link        directory: index of last child; file: -file_id (so <= 0)
//   u16 prevSibling 0 when first in its directory
//   u32 nameOffset  into the name block
//   u16 parent      node index; node 0 is a sentinel with parent 0xffff
//   u16 extra       reported, not interpreted
struct NameNode {
    std::int16_t link = 0;
    std::uint16_t prevSibling = 0;
    std::uint32_t nameOffset = 0;
    std::uint16_t parent = 0;
    std::uint16_t extra = 0;
    std::string name;
};

struct NameTable {
    bool parsed = false;
    std::string failure;
    std::uint32_t nodeCount = 0;
    std::uint32_t namesSize = 0;
    std::uint64_t bytesAfterNames = 0;
    std::vector<NameNode> nodes;
    std::vector<int> fileIdToNode;

    std::uint64_t badNameOffsets = 0;
    std::uint64_t parentOutOfRange = 0;
    std::uint64_t parentNotEarlier = 0;
    std::uint64_t prevOutOfRange = 0;
    std::uint64_t cycles = 0;
    std::uint64_t fileNodes = 0;
    std::uint64_t directoryNodes = 0;
    std::uint64_t duplicateIds = 0;
    std::uint64_t idsOutOfRange = 0;
    std::uint64_t missingIds = 0;
    std::uint64_t extraNonZero = 0;
};

NameTable parseNameTable(std::span<const std::uint8_t> trailing, std::uint32_t entryCount);

// Joins names from the root down, skipping the sentinel and empty names.
std::string fullPath(const NameTable& table, int nodeIndex);

}  // namespace gamecore::dat
