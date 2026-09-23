#include "Tools/AssetInspector/FormatProbe.hpp"

#include <cmath>
#include <vector>

namespace gamecore::inspect {
namespace {

bool startsWith(const io::ByteSource& source, const std::uint8_t* magic, std::size_t count) {
    if (source.size < count) {
        return false;
    }
    std::uint8_t buffer[32];
    if (count > sizeof(buffer) || !io::readExact(source, 0, buffer, count)) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (buffer[i] != magic[i]) {
            return false;
        }
    }
    return true;
}

bool isZlibHeader(std::uint8_t cmf, std::uint8_t flg) {
    if ((cmf & 0x0f) != 8) {
        return false;
    }
    if ((cmf >> 4) > 7) {
        return false;
    }
    return (static_cast<int>(cmf) * 256 + static_cast<int>(flg)) % 31 == 0;
}

FormatHit hit(const char* name, const char* family, const char* note) {
    FormatHit result;
    result.name = name;
    result.family = family;
    result.note = note;
    result.recognized = true;
    return result;
}

}  // namespace

FormatHit probeFormat(const io::ByteSource& source) {
    static constexpr std::uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    static constexpr std::uint8_t kGif87[] = {'G', 'I', 'F', '8', '7', 'a'};
    static constexpr std::uint8_t kGif89[] = {'G', 'I', 'F', '8', '9', 'a'};
    static constexpr std::uint8_t kDds[] = {'D', 'D', 'S', ' '};
    static constexpr std::uint8_t kKtx1[] = {0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    static constexpr std::uint8_t kKtx2[] = {0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    static constexpr std::uint8_t kOgg[] = {'O', 'g', 'g', 'S'};
    static constexpr std::uint8_t kFlac[] = {'f', 'L', 'a', 'C'};
    static constexpr std::uint8_t kId3[] = {'I', 'D', '3'};
    static constexpr std::uint8_t kGzip[] = {0x1f, 0x8b};
    static constexpr std::uint8_t kBzip[] = {'B', 'Z', 'h'};
    static constexpr std::uint8_t kXz[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
    static constexpr std::uint8_t kZstd[] = {0x28, 0xb5, 0x2f, 0xfd};
    static constexpr std::uint8_t k7z[] = {'7', 'z', 0xbc, 0xaf, 0x27, 0x1c};
    static constexpr std::uint8_t kZip[] = {'P', 'K', 0x03, 0x04};
    static constexpr std::uint8_t kZipEmpty[] = {'P', 'K', 0x05, 0x06};
    static constexpr std::uint8_t kLua[] = {0x1b, 'L', 'u', 'a'};
    static constexpr std::uint8_t kXml[] = {'<', '?', 'x', 'm', 'l'};

    if (startsWith(source, kPng, sizeof(kPng))) {
        return hit("png", "texture", "documented png signature; pixels are not decoded in milestone 1");
    }
    if (source.size >= 3) {
        std::uint8_t jpeg[3];
        if (io::readExact(source, 0, jpeg, 3) && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[2] == 0xff) {
            return hit("jpeg", "texture", "documented jpeg signature; pixels are not decoded in milestone 1");
        }
    }
    if (startsWith(source, kGif87, sizeof(kGif87)) || startsWith(source, kGif89, sizeof(kGif89))) {
        return hit("gif", "texture", "documented gif signature; pixels are not decoded in milestone 1");
    }
    if (source.size >= 2) {
        std::uint8_t bmp[2];
        if (io::readExact(source, 0, bmp, 2) && bmp[0] == 'B' && bmp[1] == 'M') {
            return hit("bmp", "texture", "documented bmp signature; pixels are not decoded in milestone 1");
        }
    }
    if (startsWith(source, kDds, sizeof(kDds))) {
        return hit("dds", "texture", "documented dds signature; pixel format is not decoded in milestone 1");
    }
    if (startsWith(source, kKtx2, sizeof(kKtx2))) {
        return hit("ktx2", "texture", "documented ktx 2.0 signature; pixels are not decoded in milestone 1");
    }
    if (startsWith(source, kKtx1, sizeof(kKtx1))) {
        return hit("ktx", "texture", "documented ktx 1.1 signature; pixels are not decoded in milestone 1");
    }
    if (source.size >= 12) {
        std::uint8_t riff[12];
        if (io::readExact(source, 0, riff, 12) && riff[0] == 'R' && riff[1] == 'I' && riff[2] == 'F' && riff[3] == 'F') {
            if (riff[8] == 'W' && riff[9] == 'A' && riff[10] == 'V' && riff[11] == 'E') {
                return hit("wav", "audio", "documented riff wave signature; samples are not decoded in milestone 1");
            }
            return hit("riff", "container", "documented riff container; form type is logged, payload is not parsed");
        }
    }
    if (startsWith(source, kOgg, sizeof(kOgg))) {
        return hit("ogg", "audio", "documented ogg signature; packets are not decoded in milestone 1");
    }
    if (startsWith(source, kFlac, sizeof(kFlac))) {
        return hit("flac", "audio", "documented flac signature; samples are not decoded in milestone 1");
    }
    if (startsWith(source, kId3, sizeof(kId3))) {
        return hit("id3", "audio", "documented id3 signature; frames are not decoded in milestone 1");
    }
    if (startsWith(source, kZip, sizeof(kZip)) || startsWith(source, kZipEmpty, sizeof(kZipEmpty))) {
        return hit("zip", "archive", "documented pkzip signature; central directory is listed, payloads are not rewritten");
    }
    if (startsWith(source, kGzip, sizeof(kGzip))) {
        return hit("gzip", "compression", "documented gzip signature; stream is not inflated in milestone 1");
    }
    if (startsWith(source, kBzip, sizeof(kBzip))) {
        return hit("bzip2", "compression", "documented bzip2 signature; stream is not inflated in milestone 1");
    }
    if (startsWith(source, kXz, sizeof(kXz))) {
        return hit("xz", "compression", "documented xz signature; stream is not inflated in milestone 1");
    }
    if (startsWith(source, kZstd, sizeof(kZstd))) {
        return hit("zstd", "compression", "documented zstd frame signature; stream is not inflated in milestone 1");
    }
    if (startsWith(source, k7z, sizeof(k7z))) {
        return hit("7z", "archive", "documented 7z signature; entries are not expanded in milestone 1");
    }
    if (source.size >= 2) {
        std::uint8_t zlib[2];
        if (io::readExact(source, 0, zlib, 2) && isZlibHeader(zlib[0], zlib[1])) {
            return hit("zlib", "compression", "documented zlib header; stream is not inflated in milestone 1");
        }
    }
    if (startsWith(source, kLua, sizeof(kLua))) {
        return hit("lua-bytecode", "script", "documented lua bytecode signature; instructions are not lifted");
    }
    if (startsWith(source, kXml, sizeof(kXml))) {
        return hit("xml", "document", "documented xml declaration; schema is not assumed");
    }

    FormatHit unknown;
    unknown.note = "no documented signature at offset 0; header logged, layout not guessed";
    return unknown;
}

double shannonEntropy(const io::ByteSource& source, std::uint64_t maxBytes) {
    if (source.read == nullptr) {
        return 0.0;
    }
    const std::uint64_t count = source.size < maxBytes ? source.size : maxBytes;
    if (count == 0) {
        return 0.0;
    }
    unsigned hist[256] = {};
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(count > (1u << 16) ? (1u << 16) : count));
    std::uint64_t consumed = 0;
    while (consumed < count) {
        const std::size_t chunk = static_cast<std::size_t>(
            (count - consumed) > buffer.size() ? buffer.size() : (count - consumed));
        const std::size_t got = source.read(source.ctx, consumed, buffer.data(), chunk);
        if (got == 0) {
            break;
        }
        for (std::size_t i = 0; i < got; ++i) {
            ++hist[buffer[i]];
        }
        consumed += got;
    }
    if (consumed == 0) {
        return 0.0;
    }
    double entropy = 0.0;
    const double total = static_cast<double>(consumed);
    for (unsigned bin : hist) {
        if (bin == 0) {
            continue;
        }
        const double p = static_cast<double>(bin) / total;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

std::string hexAsciiDump(const io::ByteSource& source, std::size_t maxBytes) {
    const std::size_t count =
        static_cast<std::size_t>(source.size < maxBytes ? source.size : static_cast<std::uint64_t>(maxBytes));
    std::vector<std::uint8_t> bytes(count);
    if (count > 0 && !io::readExact(source, 0, bytes.data(), count)) {
        return {};
    }
    std::string out;
    out.reserve(count * 4);
    for (std::size_t offset = 0; offset < count; offset += 16) {
        const std::size_t n = count - offset > 16 ? 16 : count - offset;
        char prefix[8];
        prefix[0] = "0123456789abcdef"[(offset >> 12) & 0xf];
        prefix[1] = "0123456789abcdef"[(offset >> 8) & 0xf];
        prefix[2] = "0123456789abcdef"[(offset >> 4) & 0xf];
        prefix[3] = "0123456789abcdef"[offset & 0xf];
        out.append(prefix, 4);
        out.append("  ");
        for (std::size_t i = 0; i < 16; ++i) {
            if (i < n) {
                const std::uint8_t value = bytes[offset + i];
                out.push_back("0123456789abcdef"[value >> 4]);
                out.push_back("0123456789abcdef"[value & 0xf]);
            } else {
                out.append("  ");
            }
            out.push_back(' ');
        }
        out.append(" ");
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char value = bytes[offset + i];
            out.push_back(value >= 32 && value < 127 ? static_cast<char>(value) : '.');
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace gamecore::inspect
