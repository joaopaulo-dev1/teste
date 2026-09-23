#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/AssetInspector/FormatProbe.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"
#include "Tools/DatProbe/DatListing.hpp"
#include "Tools/Lz2kProbe/Lz2kContainer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::filesystem::path> readArgs(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int i = 0; i < count; ++i) {
            args.emplace_back(wide[i]);
        }
        LocalFree(wide);
    }
#else
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
}

bool isDatLike(const std::filesystem::path& path) {
    std::string extension = gamecore::io::pathToUtf8(path.extension());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension.rfind(".dat", 0) == 0;
}

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out << ((i % 32) == 0 ? "\n      " : " ");
        }
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

struct SmallChunk {
    std::uint32_t rawSize = 0;
    std::uint32_t packedSize = 0;
    std::string where;
    std::vector<std::uint8_t> payloadHead;
};

struct ExtStats {
    std::uint64_t entries = 0;
    std::uint64_t raw = 0;
    std::uint64_t packed = 0;
};

struct Totals {
    std::uint64_t entries = 0;
    std::uint64_t valid = 0;
    std::map<std::string, std::uint64_t> failures;
    std::map<std::string, std::vector<std::string>> failureExamples;
    std::map<std::size_t, std::uint64_t> chunksPerEntry;
    std::uint64_t chunks = 0;
    std::map<std::uint32_t, std::uint64_t> nonLastRawSizes;
    std::uint64_t lastRawAbove32K = 0;
    std::uint64_t packedAtLeastRaw = 0;
    std::uint64_t packedEqualsRaw = 0;
    std::array<std::uint64_t, 256> firstPayloadByte{};
    std::uint64_t entropySamples = 0;
    double entropySum = 0.0;
    double entropyMin = 9.0;
    double entropyMax = 0.0;
    std::map<std::string, ExtStats> byExtension;
    std::vector<SmallChunk> smallest;
};

constexpr std::size_t kSmallestKept = 24;
constexpr std::size_t kPayloadHeadBytes = 128;
constexpr std::uint64_t kEntropySampleCap = 4000;
constexpr std::uint32_t kExpectedChunkRaw = 0x8000;

void keepSmallest(Totals& totals, SmallChunk candidate) {
    totals.smallest.push_back(std::move(candidate));
    std::sort(totals.smallest.begin(), totals.smallest.end(), [](const SmallChunk& a, const SmallChunk& b) {
        return a.rawSize != b.rawSize ? a.rawSize < b.rawSize : a.packedSize < b.packedSize;
    });
    if (totals.smallest.size() > kSmallestKept) {
        totals.smallest.pop_back();
    }
}

void probeEntry(const gamecore::io::ByteSource& dat, const std::string& datName,
                const gamecore::dat::ListedEntry& entry, Totals& totals) {
    ++totals.entries;
    gamecore::io::SliceBytes sliceStorage;
    const auto slice = gamecore::io::sliceOf(dat, entry.offset, entry.storedSize, sliceStorage);
    const auto layout = gamecore::lz2k::parseLayout(slice, entry.originalSize);
    const std::string where = datName + ":" + entry.path;
    if (!layout.valid) {
        ++totals.failures[layout.failure];
        auto& examples = totals.failureExamples[layout.failure];
        if (examples.size() < 5) {
            examples.push_back(where + " stored=" + std::to_string(entry.storedSize) +
                               " original=" + std::to_string(entry.originalSize) +
                               " chunks=" + std::to_string(layout.chunks.size()) +
                               " raw_total=" + std::to_string(layout.rawTotal) +
                               " trailing=" + std::to_string(layout.trailingBytes));
        }
        return;
    }
    ++totals.valid;
    ++totals.chunksPerEntry[layout.chunks.size()];
    auto& ext = totals.byExtension[gamecore::dat::lowerExtension(entry.path)];
    ++ext.entries;
    ext.raw += entry.originalSize;
    ext.packed += entry.storedSize;

    for (std::size_t i = 0; i < layout.chunks.size(); ++i) {
        const auto& chunk = layout.chunks[i];
        ++totals.chunks;
        const bool last = i + 1 == layout.chunks.size();
        if (!last) {
            ++totals.nonLastRawSizes[chunk.rawSize];
        } else if (chunk.rawSize > kExpectedChunkRaw) {
            ++totals.lastRawAbove32K;
        }
        if (chunk.packedSize >= chunk.rawSize) {
            ++totals.packedAtLeastRaw;
        }
        if (chunk.packedSize == chunk.rawSize) {
            ++totals.packedEqualsRaw;
        }

        std::uint8_t first = 0;
        if (gamecore::io::readExact(slice, chunk.payloadOffset(), &first, 1)) {
            ++totals.firstPayloadByte[first];
        }

        gamecore::io::SliceBytes payloadStorage;
        const auto payload = gamecore::io::sliceOf(slice, chunk.payloadOffset(), chunk.packedSize, payloadStorage);
        if (totals.entropySamples < kEntropySampleCap) {
            const double entropy = gamecore::inspect::shannonEntropy(payload, 1u << 16);
            ++totals.entropySamples;
            totals.entropySum += entropy;
            totals.entropyMin = std::min(totals.entropyMin, entropy);
            totals.entropyMax = std::max(totals.entropyMax, entropy);
        }

        const bool candidate = totals.smallest.size() < kSmallestKept || chunk.rawSize < totals.smallest.back().rawSize;
        if (candidate) {
            SmallChunk small;
            small.rawSize = chunk.rawSize;
            small.packedSize = chunk.packedSize;
            small.where = where + " chunk " + std::to_string(i);
            small.payloadHead.resize(std::min<std::size_t>(chunk.packedSize, kPayloadHeadBytes));
            if (gamecore::io::readExact(payload, 0, small.payloadHead.data(), small.payloadHead.size())) {
                keepSmallest(totals, std::move(small));
            }
        }
    }
}

void writeReport(std::ostream& report, const Totals& totals, int datFiles, int datFailures) {
    report << "lz2k_probe: read-only; checks the chunk framing hypothesis, does not decode payloads\n";
    report << "framing: repeated { 'LZ2K', u32 raw_size, u32 packed_size, payload[packed_size] }\n\n";
    report << "dat_files: " << datFiles << " unreadable: " << datFailures << '\n';
    report << "lz2k_entries: " << totals.entries << " framing_valid: " << totals.valid
           << " framing_invalid: " << (totals.entries - totals.valid) << '\n';
    for (const auto& [reason, count] : totals.failures) {
        report << "  failure \"" << reason << "\": " << count << '\n';
        for (const auto& example : totals.failureExamples.at(reason)) {
            report << "    " << example << '\n';
        }
    }

    report << "\nchunks_total: " << totals.chunks << '\n';
    report << "chunks_per_entry (count: entries):\n";
    std::uint64_t over16 = 0;
    for (const auto& [count, entries] : totals.chunksPerEntry) {
        if (count <= 16) {
            report << "  " << count << ": " << entries << '\n';
        } else {
            over16 += entries;
        }
    }
    report << "  >16: " << over16 << " (max " << (totals.chunksPerEntry.empty() ? 0 : totals.chunksPerEntry.rbegin()->first)
           << ")\n";

    report << "non_last_chunk_raw_sizes (size: chunks):\n";
    for (const auto& [size, count] : totals.nonLastRawSizes) {
        report << "  0x" << std::hex << size << std::dec << ": " << count << '\n';
    }
    report << "last_chunk_raw_above_0x8000: " << totals.lastRawAbove32K << '\n';
    report << "chunks_packed_ge_raw: " << totals.packedAtLeastRaw << " packed_eq_raw: " << totals.packedEqualsRaw << '\n';

    report << std::fixed << std::setprecision(3);
    report << "payload_entropy_bits_per_byte (first " << totals.entropySamples << " chunks): mean "
           << (totals.entropySamples == 0 ? 0.0 : totals.entropySum / static_cast<double>(totals.entropySamples))
           << " min " << totals.entropyMin << " max " << totals.entropyMax << '\n';

    std::vector<std::pair<std::uint64_t, int>> firstBytes;
    for (int b = 0; b < 256; ++b) {
        if (totals.firstPayloadByte[static_cast<std::size_t>(b)] != 0) {
            firstBytes.emplace_back(totals.firstPayloadByte[static_cast<std::size_t>(b)], b);
        }
    }
    std::sort(firstBytes.rbegin(), firstBytes.rend());
    report << "first_payload_byte (distinct " << firstBytes.size() << "; top 24 value: chunks):\n";
    for (std::size_t i = 0; i < firstBytes.size() && i < 24; ++i) {
        report << "  0x" << std::hex << std::setw(2) << std::setfill('0') << firstBytes[i].second << std::dec
               << std::setfill(' ') << ": " << firstBytes[i].first << '\n';
    }

    std::vector<std::pair<std::uint64_t, std::string>> exts;
    for (const auto& [ext, stats] : totals.byExtension) {
        exts.emplace_back(stats.raw, ext);
    }
    std::sort(exts.rbegin(), exts.rend());
    report << "by_extension (entries raw_MiB packed_MiB packed/raw):\n";
    for (std::size_t i = 0; i < exts.size() && i < 30; ++i) {
        const auto& stats = totals.byExtension.at(exts[i].second);
        report << "  " << std::left << std::setw(16) << exts[i].second << std::right << ' ' << stats.entries << ' '
               << (stats.raw >> 20) << ' ' << (stats.packed >> 20) << ' '
               << (stats.raw == 0 ? 0.0 : static_cast<double>(stats.packed) / static_cast<double>(stats.raw)) << '\n';
    }

    report << "\nsmallest_chunks (payload head, up to " << kPayloadHeadBytes << " bytes):\n";
    for (const auto& small : totals.smallest) {
        report << "  raw=" << small.rawSize << " packed=" << small.packedSize << "  " << small.where << '\n';
        report << "      " << hexBytes(small.payloadHead) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = readArgs(argc, argv);
    std::filesystem::path input;
    std::filesystem::path output;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") {
            input = args[i + 1];
        } else if (args[i] == "--out") {
            output = args[i + 1];
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "lz2k_probe --input <dat-file-or-directory> --out <report-directory>\n"
                  << "Read-only. Validates LZ2K chunk framing on every compressed entry; does not decompress.\n";
        return 1;
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        files.push_back(input);
    } else if (std::filesystem::is_directory(input, ec)) {
        for (const auto& item : std::filesystem::directory_iterator(input, ec)) {
            if (item.is_regular_file() && isDatLike(item.path())) {
                files.push_back(item.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "no DAT files found\n";
        return 1;
    }

    Totals totals;
    int datFailures = 0;
    for (const auto& path : files) {
        const std::string datName = gamecore::io::pathToUtf8(path.filename());
        gamecore::io::ReadOnlyFile file;
        std::string error;
        gamecore::dat::DatListing listing;
        if (!gamecore::io::ReadOnlyFile::open(path, file, error) ||
            !gamecore::dat::listDat(file.source(), listing, error)) {
            std::cerr << datName << ": " << error << '\n';
            ++datFailures;
            continue;
        }
        const auto source = file.source();
        for (const auto& entry : listing.entries) {
            if (entry.compression == 2 && entry.inBounds) {
                probeEntry(source, datName, entry, totals);
            }
        }
        std::cout << datName << " lz2k_entries_so_far=" << totals.entries << " valid=" << totals.valid << '\n';
    }

    std::filesystem::create_directories(output, ec);
    const auto reportPath = output / "lz2k_probe.txt";
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    if (!report) {
        std::cerr << "cannot write " << gamecore::io::pathToUtf8(reportPath) << '\n';
        return 1;
    }
    writeReport(report, totals, static_cast<int>(files.size()), datFailures);
    std::cout << "entries=" << totals.entries << " valid=" << totals.valid << " chunks=" << totals.chunks
              << " report=" << gamecore::io::pathToUtf8(reportPath) << '\n';
    return (datFailures == 0 && totals.valid == totals.entries) ? 0 : 2;
}
