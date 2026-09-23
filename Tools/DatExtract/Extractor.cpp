#include "Tools/DatExtract/Extractor.hpp"

#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"

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

std::uint64_t storedBytes(const dat::DatIndex& index, const dat::NameTable& names,
                          const std::set<std::string>& extensions) {
    std::uint64_t total = 0;
    for (std::size_t id = 0; id < names.fileIdToNode.size() && id < index.entries.size(); ++id) {
        const int node = names.fileIdToNode[id];
        if (node < 0 || !isStoredEntry(index.entries[id])) {
            continue;
        }
        if (!extensions.empty() &&
            extensions.count(dat::lowerExtension(names.nodes[static_cast<std::size_t>(node)].name)) == 0) {
            continue;
        }
        total += index.entries[id].b;
    }
    return total;
}

ExtractStats extractStored(const io::ByteSource& file,
                           const dat::DatIndex& index,
                           const dat::NameTable& names,
                           const ExtractOptions& options,
                           std::ostream& manifest,
                           std::vector<std::string>& errors) {
    ExtractStats stats;
    std::vector<std::uint8_t> buffer(1u << 20);
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
        if (!isStoredEntry(entry)) {
            ++stats.skippedCompressed;
            continue;
        }
        const std::string logical = dat::fullPath(names, node);
        std::filesystem::path relative;
        std::string reason;
        if (!sanitizeRelativePath(logical, relative, reason)) {
            ++stats.rejectedPaths;
            errors.push_back("rejected path '" + logical + "': " + reason);
            continue;
        }
        const std::filesystem::path target = options.outRoot / relative;
        const std::string label = dat::labelEntry(file, entry);
        if (options.dryRun) {
            manifest << id << '\t' << logical << '\t' << entry.b << '\t' << label << "\tdry-run\n";
            ++stats.written;
            stats.bytes += entry.b;
            continue;
        }
        if (sameSizeFileExists(target, entry.b)) {
            manifest << id << '\t' << logical << '\t' << entry.b << '\t' << label << "\treused\n";
            ++stats.reused;
            continue;
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
            const std::uint64_t offset = dat::entryOffset(entry, dat::OffsetRule::Shl8OrHighByte);
            std::uint64_t copied = 0;
            while (ok && copied < entry.b) {
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
            errors.push_back("copy failed for " + logical);
            continue;
        }
        manifest << id << '\t' << logical << '\t' << entry.b << '\t' << label << "\twritten\n";
        ++stats.written;
        stats.bytes += entry.b;
    }
    return stats;
}

}  // namespace gamecore::extract
