#include "Tools/DatProbe/DatListing.hpp"

#include "Tools/DatProbe/DatIndex.hpp"
#include "Tools/DatProbe/DatProbe.hpp"
#include "Tools/DatProbe/EntryProbe.hpp"
#include "Tools/DatProbe/NameTable.hpp"

namespace gamecore::dat {

const char* compressionName(std::uint32_t code) {
    switch (code) {
        case 0:
            return "stored";
        case 2:
            return "LZ2K";
        default:
            return "unknown";
    }
}

bool listDat(const io::ByteSource& file, DatListing& out, std::string& error) {
    out = {};
    out.fileSize = file.size;

    const auto header = probeDat(file);
    if (!header.headerRead) {
        error = "file too small for a DAT header";
        return false;
    }
    if (header.matchedTransform.empty()) {
        error = "table offset rule did not match: not a TT DAT or an unseen variant";
        return false;
    }

    DatIndex index;
    if (!readDatIndex(file, header.tableOffset, header.tableSize, index, error)) {
        return false;
    }
    const auto count = static_cast<std::uint32_t>(index.entries.size());
    const auto names = parseNameTable(index.trailing, count);
    if (!names.parsed) {
        error = "name table: " + names.failure;
        return false;
    }

    out.entries.resize(count);
    for (std::uint32_t id = 0; id < count; ++id) {
        const auto& raw = index.entries[id];
        auto& entry = out.entries[id];
        entry.id = id;
        entry.offset = entryOffset(raw, OffsetRule::Shl8OrHighByte);
        entry.storedSize = raw.b;
        entry.originalSize = raw.c;
        entry.compression = raw.flags & 0xffu;
        entry.inBounds = entry.offset <= file.size && raw.b <= file.size - entry.offset;

        const int node = id < names.fileIdToNode.size() ? names.fileIdToNode[id] : -1;
        if (node >= 0) {
            entry.path = fullPath(names, node);
        } else {
            entry.path = "(unnamed " + std::to_string(id) + ")";
            ++out.unnamedCount;
        }

        if (entry.compression == 0) {
            ++out.storedCount;
        } else if (entry.compression == 2) {
            ++out.compressedCount;
        } else {
            ++out.unknownCompressionCount;
        }
        if (!entry.inBounds) {
            ++out.outOfBoundsCount;
        }
        out.originalBytes += raw.c;
    }
    error.clear();
    return true;
}

}  // namespace gamecore::dat
