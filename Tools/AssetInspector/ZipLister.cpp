#include "Tools/AssetInspector/ZipLister.hpp"

#include <cctype>
#include <string_view>
#include <vector>

namespace gamecore::inspect {
namespace {

constexpr std::uint32_t kEocdSig = 0x06054b50u;
constexpr std::uint32_t kEocd64LocatorSig = 0x07064b50u;
constexpr std::uint32_t kEocd64Sig = 0x06064b50u;
constexpr std::uint32_t kCentralSig = 0x02014b50u;
constexpr std::size_t kSampleCap = 100;
constexpr std::uint64_t kEntryCap = 10000;

std::string extensionOf(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    const auto base = slash == std::string_view::npos ? name : name.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= base.size()) {
        return "(none)";
    }
    std::string extension(base.substr(dot));
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

std::string nameFromBytes(const std::uint8_t* data, std::uint16_t length) {
    std::string name;
    name.reserve(length);
    for (std::uint16_t i = 0; i < length; ++i) {
        const unsigned char byte = data[i];
        if (byte == 0) {
            break;
        }
        name.push_back(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?');
    }
    return name;
}

struct DirectoryLocation {
    std::uint64_t offset = 0;
    std::uint64_t entryCount = 0;
    bool zip64 = false;
};

bool locateDirectory(const io::ByteSource& source, DirectoryLocation& location, std::string& note) {
    if (source.size < 22 || source.read == nullptr) {
        note = "file is smaller than a zip end-of-central-directory record";
        return false;
    }
    const std::uint64_t window = source.size < 65557ull ? source.size : 65557ull;
    const std::uint64_t start = source.size - window;
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
    if (source.read(source.ctx, start, tail.data(), tail.size()) != tail.size()) {
        note = "failed to read the zip tail";
        return false;
    }

    bool found = false;
    std::uint64_t eocd = 0;
    for (std::size_t i = tail.size() - 22; ; --i) {
        const std::uint32_t sig = static_cast<std::uint32_t>(tail[i]) |
                                  (static_cast<std::uint32_t>(tail[i + 1]) << 8) |
                                  (static_cast<std::uint32_t>(tail[i + 2]) << 16) |
                                  (static_cast<std::uint32_t>(tail[i + 3]) << 24);
        if (sig == kEocdSig) {
            const std::uint16_t commentLength = static_cast<std::uint16_t>(tail[i + 20] | (tail[i + 21] << 8));
            if (i + 22ull + commentLength == tail.size()) {
                eocd = start + i;
                found = true;
                break;
            }
        }
        if (i == 0) {
            break;
        }
    }
    if (!found) {
        note = "zip local header present but end-of-central-directory was not found";
        return false;
    }

    std::uint16_t diskEntries = 0;
    std::uint16_t totalEntries = 0;
    std::uint32_t cdSize = 0;
    std::uint32_t cdOffset = 0;
    if (!io::readU16(source, eocd + 8, true, diskEntries) || !io::readU16(source, eocd + 10, true, totalEntries) ||
        !io::readU32(source, eocd + 12, true, cdSize) || !io::readU32(source, eocd + 16, true, cdOffset)) {
        note = "zip end-of-central-directory is truncated";
        return false;
    }
    location.entryCount = totalEntries;
    location.offset = cdOffset;

    const bool needs64 = totalEntries == 0xffff || cdSize == 0xffffffffu || cdOffset == 0xffffffffu;
    if (!needs64) {
        return true;
    }
    if (eocd < 20) {
        note = "zip64 fields are saturated and the locator does not fit";
        return false;
    }
    std::uint32_t locatorSig = 0;
    std::uint64_t eocd64 = 0;
    if (!io::readU32(source, eocd - 20, true, locatorSig) || locatorSig != kEocd64LocatorSig ||
        !io::readU64(source, eocd - 12, true, eocd64)) {
        note = "zip64 end-of-central-directory locator is missing";
        return false;
    }
    std::uint32_t eocd64Sig = 0;
    std::uint64_t entries64 = 0;
    std::uint64_t offset64 = 0;
    if (!io::readU32(source, eocd64, true, eocd64Sig) || eocd64Sig != kEocd64Sig ||
        !io::readU64(source, eocd64 + 32, true, entries64) || !io::readU64(source, eocd64 + 48, true, offset64)) {
        note = "zip64 end-of-central-directory is truncated";
        return false;
    }
    location.zip64 = true;
    location.entryCount = entries64;
    location.offset = offset64;
    return true;
}

}  // namespace

ZipSummary summarizeZip(const io::ByteSource& source) {
    ZipSummary summary;
    DirectoryLocation directory;
    if (!locateDirectory(source, directory, summary.note)) {
        return summary;
    }
    summary.zip64 = directory.zip64;
    summary.parsed = true;
    std::uint64_t cursor = directory.offset;
    const std::uint64_t expected = directory.entryCount;
    std::uint64_t seen = 0;
    while (seen < expected && seen < kEntryCap) {
        std::uint32_t sig = 0;
        if (!io::readU32(source, cursor, true, sig) || sig != kCentralSig) {
            summary.note = "central directory ended before the recorded entry count";
            break;
        }
        std::uint16_t flags = 0;
        std::uint16_t nameLength = 0;
        std::uint16_t extraLength = 0;
        std::uint16_t commentLength = 0;
        if (!io::readU16(source, cursor + 8, true, flags) || !io::readU16(source, cursor + 28, true, nameLength) ||
            !io::readU16(source, cursor + 30, true, extraLength) ||
            !io::readU16(source, cursor + 32, true, commentLength)) {
            summary.note = "central directory entry is truncated";
            break;
        }
        std::vector<std::uint8_t> nameBytes(nameLength);
        if (nameLength > 0 && !io::readExact(source, cursor + 46, nameBytes.data(), nameLength)) {
            summary.note = "central directory name is truncated";
            break;
        }
        const std::string name = nameFromBytes(nameBytes.data(), nameLength);
        if ((flags & 0x1) != 0) {
            ++summary.encryptedCount;
        }
        if (!name.empty() && name.back() != '/' && name.back() != '\\') {
            ++summary.extensions[extensionOf(name)];
        }
        if (summary.sampleNames.size() < kSampleCap) {
            summary.sampleNames.push_back(name);
        }
        const std::uint64_t advance = 46ull + nameLength + extraLength + commentLength;
        if (cursor + advance < cursor) {
            summary.note = "central directory offset overflow";
            break;
        }
        cursor += advance;
        ++seen;
    }
    summary.entryCount = seen;
    if (expected > kEntryCap) {
        summary.truncated = true;
        summary.entryCount = expected;
        if (summary.note.empty()) {
            summary.note = "entry listing truncated at 10000; histogram covers the listed prefix";
        }
    }
    if (summary.encryptedCount > 0 && summary.note.empty()) {
        summary.note = "encrypted entries were counted and not unwrapped";
    }
    return summary;
}

}  // namespace gamecore::inspect
