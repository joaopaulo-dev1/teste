#include "Tools/DatProbe/ContentCheck.hpp"

#include "Tools/AssetInspector/FormatProbe.hpp"

#include <cctype>

namespace gamecore::dat {

std::string lowerExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return "(none)";
    }
    std::string extension = name.substr(dot);
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

std::string labelEntry(const io::ByteSource& file, const RawEntry& entry) {
    const std::uint64_t offset = entryOffset(entry, OffsetRule::Shl8OrHighByte);
    if (entry.b == 0) {
        return "empty";
    }
    const bool stored = (entry.flags & 0xffu) == 0 && entry.b == entry.c;
    std::uint8_t lead[64];
    const std::size_t want = entry.b < sizeof(lead) ? entry.b : sizeof(lead);
    if (!io::readExact(file, offset, lead, want)) {
        return "unreadable";
    }
    if (!stored) {
        std::string label = "packed:";
        for (std::size_t i = 0; i < 4 && i < want; ++i) {
            const unsigned char ch = lead[i];
            label.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
        }
        return label;
    }
    io::SliceBytes storage;
    const io::ByteSource slice = io::sliceOf(file, offset, entry.b, storage);
    const auto hit = inspect::probeFormat(slice);
    if (hit.recognized) {
        return hit.name;
    }
    bool printable = true;
    for (std::size_t i = 0; i < want; ++i) {
        const unsigned char ch = lead[i];
        const bool whitespace = ch == '\n' || ch == '\r' || ch == '\t';
        // UTF-8 BOM and high bytes are allowed so localised text still counts as text.
        if (!(whitespace || (ch >= 32 && ch != 127))) {
            printable = false;
            break;
        }
    }
    return printable ? "text" : "bin";
}

std::map<std::string, ExtensionStats> crossCheck(const io::ByteSource& file,
                                                 const DatIndex& index,
                                                 const NameTable& names,
                                                 std::uint64_t labelCapPerExtension) {
    std::map<std::string, ExtensionStats> stats;
    for (std::size_t id = 0; id < names.fileIdToNode.size() && id < index.entries.size(); ++id) {
        const int node = names.fileIdToNode[id];
        if (node < 0) {
            continue;
        }
        const RawEntry& entry = index.entries[id];
        ExtensionStats& ext = stats[lowerExtension(names.nodes[static_cast<std::size_t>(node)].name)];
        ++ext.count;
        ext.totalOriginalBytes += entry.c;
        const bool stored = (entry.flags & 0xffu) == 0 && entry.b == entry.c;
        if (stored) {
            ++ext.stored;
        } else {
            ++ext.compressed;
        }
        if (ext.count <= labelCapPerExtension) {
            ++ext.detected[labelEntry(file, entry)];
        }
    }
    return stats;
}

}  // namespace gamecore::dat
