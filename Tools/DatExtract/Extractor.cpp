#include "Tools/DatExtract/Extractor.hpp"

#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"
#include "Tools/Lz2kProbe/Lz2k.hpp"

#include <array>
#include <cctype>
#include <fstream>

namespace gamecore::extract {
namespace {

bool isReservedDeviceName(const std::string& component) {
    std::string stem = component.substr(0, component.find('.'));
    for (char& ch : stem) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    static constexpr std::array<const char*, 4> kFixed = {"CON", "PRN", "AUX", "NUL"};
    for (const char* name : kFixed) {
        if (stem == name) {
            return true;
        }
    }
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' &&
        stem[3] <= '9') {
        return true;
    }
    return false;
}

bool sameSizeFileExists(const std::filesystem::path& path, std::uint64_t size) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) == size && !ec;
}

}  // namespace

bool sanitizeRelativePath(const std::string& path, std::filesystem::path& out, std::string& reason) {
    out.clear();
    if (path.empty() || path.size() > 240) {
        reason = "empty or longer than 240 characters";
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        reason = "absolute path";
        return false;
    }
    std::string component;
    auto flush = [&]() {
        if (component.empty() || component == "." || component == "..") {
            reason = "empty, '.' or '..' component";
            return false;
        }
        if (component.back() == ' ' || component.back() == '.') {
            reason = "component ends with space or dot";
            return false;
        }
        if (isReservedDeviceName(component)) {
            reason = "reserved device name";
            return false;
        }
        out /= component;
        component.clear();
        return true;
    };
    for (char ch : path) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '/' || ch == '\\') {
            if (!flush()) {
                out.clear();
                return false;
            }
            continue;
        }
        if (byte < 0x20 || byte > 0x7e || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' ||
            ch == '?' || ch == '*') {
            reason = "character not allowed in a portable file name";
            out.clear();
            return false;
        }
        component.push_back(ch);
    }
    if (!flush()) {
        out.clear();
        return false;
    }
    reason.clear();
    return true;
}

bool isStoredEntry(const dat::RawEntry& entry) { return (entry.flags & 0xffu) == 0 && entry.b == entry.c; }

bool isLz2kEntry(const dat::RawEntry& entry) { return (entry.flags & 0xffu) == 2; }

std::uint64_t plannedBytes(const dat::DatIndex& index, const dat::NameTable& names,
                           const std::set<std::string>& extensions, bool includeLz2k) {
    std::uint64_t total = 0;
    for (std::size_t id = 0; id < names.fileIdToNode.size() && id < index.entries.size(); ++id) {
        const int node = names.fileIdToNode[id];
        if (node < 0) {
            continue;
        }
        const auto& entry = index.entries[id];
        if (!extensions.empty() &&
            extensions.count(dat::lowerExtension(names.nodes[static_cast<std::size_t>(node)].name)) == 0) {
            continue;
        }
        if (isStoredEntry(entry)) {
            total += entry.b;
        } else if (includeLz2k && isLz2kEntry(entry)) {
            total += entry.c;
        }
    }
    return total;
}

ExtractStats extractEntries(const io::ByteSource& file,
                            const dat::DatIndex& index,
                            const dat::NameTable& names,
                            const ExtractOptions& options,
                            std::ostream& manifest,
                            std::vector<std::string>& errors) {
    ExtractStats stats;
    std::vector<std::uint8_t> buffer(1u << 20);
    std::vector<std::uint8_t> unpacked;
    manifest << "id\tpath\tbytes\tlabel\tstatus\n";
    for (std::size_t id = 0; id < names.fileIdToNode.size() && id < index.entries.size(); ++id) {
        const int node = names.fileIdToNode[id];
        if (node < 0) {
            continue;
        }
        const dat::RawEntry& entry = index.entries[id];
        const std::string& leaf = names.nodes[static_cast<std::size_t>(node)].name;
        if (!options.extensions.empty() && options.extensions.count(dat::lowerExtension(leaf)) == 0) {
            ++stats.skippedFilter;
            continue;
        }
        const bool stored = isStoredEntry(entry);
        const bool packed = !stored && options.decompress && isLz2kEntry(entry);
        if (!stored && !packed) {
            ++stats.skippedCompressed;
            continue;
        }
        const std::uint64_t outSize = stored ? entry.b : entry.c;
        const std::string logical = dat::fullPath(names, node);
        std::filesystem::path relative;
        std::string reason;
        if (!sanitizeRelativePath(logical, relative, reason)) {
            ++stats.rejectedPaths;
            errors.push_back("rejected path '" + logical + "': " + reason);
            continue;
        }
        const std::filesystem::path target = options.outRoot / relative;
        const std::uint64_t offset = dat::entryOffset(entry, dat::OffsetRule::Shl8OrHighByte);
        if (options.dryRun) {
            const std::string label = stored ? dat::labelEntry(file, entry) : std::string("lz2k");
            manifest << id << '\t' << logical << '\t' << outSize << '\t' << label << "\tdry-run\n";
            ++stats.written;
            stats.bytes += outSize;
            continue;
        }
        if (sameSizeFileExists(target, outSize)) {
            manifest << id << '\t' << logical << '\t' << outSize << "\t-\treused\n";
            ++stats.reused;
            continue;
        }

        std::string label;
        if (packed) {
            io::SliceBytes sliceStorage;
            const auto slice = io::sliceOf(file, offset, entry.b, sliceStorage);
            std::string failure;
            if (!lz2k::decompressEntry(slice, entry.c, unpacked, failure)) {
                ++stats.errors;
                errors.push_back("decompress failed for " + logical + ": " + failure);
                continue;
            }
            io::MemoryBytes unpackedStorage;
            const auto unpackedSource = io::sourceFromSpan(unpacked, unpackedStorage);
            dat::RawEntry view;
            view.b = entry.c;
            view.c = entry.c;
            label = dat::labelEntry(unpackedSource, view);
        } else {
            label = dat::labelEntry(file, entry);
        }
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            ++stats.errors;
            errors.push_back("cannot create directory for " + logical);
            continue;
        }
        std::filesystem::path partial = target;
        partial += ".part";
        bool ok = true;
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) {
                ok = false;
            }
            if (ok && packed) {
                out.write(reinterpret_cast<const char*>(unpacked.data()), static_cast<std::streamsize>(unpacked.size()));
                ok = static_cast<bool>(out);
            }
            std::uint64_t copied = 0;
            while (ok && stored && copied < entry.b) {
                const std::size_t chunk = static_cast<std::size_t>(
                    entry.b - copied < buffer.size() ? entry.b - copied : buffer.size());
                if (!io::readExact(file, offset + copied, buffer.data(), chunk)) {
                    ok = false;
                    break;
                }
                out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
                if (!out) {
                    ok = false;
                    break;
                }
                copied += chunk;
            }
        }
        if (ok) {
            std::filesystem::rename(partial, target, ec);
            ok = !ec;
        }
        if (!ok) {
            std::filesystem::remove(partial, ec);
            ++stats.errors;
            errors.push_back("write failed for " + logical);
            continue;
        }
        manifest << id << '\t' << logical << '\t' << outSize << '\t' << label << (packed ? "\tdecompressed\n" : "\twritten\n");
        ++stats.written;
        if (packed) {
            ++stats.decompressed;
        }
        stats.bytes += outSize;
    }
    return stats;
}

}  // namespace gamecore::extract
