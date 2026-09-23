#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/AssetInspector/FormatProbe.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"
#include "Tools/DatProbe/DatListing.hpp"
#include "Tools/Lz2kProbe/Lz2kContainer.hpp"
#include "Tools/Lz2kProbe/LzhDecoder.hpp"

#include <algorithm>
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

std::string hexAscii(const std::vector<std::uint8_t>& bytes, std::size_t limit) {
    std::ostringstream out;
    const std::size_t n = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < n; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]) << ' ';
    }
    out << " |";
    for (std::size_t i = 0; i < n; ++i) {
        const auto ch = bytes[i];
        out << (ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '.');
    }
    out << '|';
    return out.str();
}

struct ParamStats {
    std::uint64_t chunks = 0;
    std::uint64_t ok = 0;
    std::uint64_t exactByteEnd = 0;
    std::map<std::int64_t, std::uint64_t> slackBytes;
    std::map<std::string, std::uint64_t> failures;
    std::map<std::string, std::string> failureExample;
    std::uint64_t entriesOk = 0;
    std::uint64_t entries = 0;
};

struct Sniff {
    std::string where;
    std::vector<std::uint8_t> head;
    std::string format;
};

}  // namespace

int main(int argc, char** argv) {
    const auto args = readArgs(argc, argv);
    std::filesystem::path input;
    std::filesystem::path output;
    std::uint64_t stride = 25;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") {
            input = args[i + 1];
        } else if (args[i] == "--out") {
            output = args[i + 1];
        } else if (args[i] == "--stride") {
            stride = std::max<std::uint64_t>(1, std::stoull(gamecore::io::pathToUtf8(args[i + 1])));
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "lz2k_decode_test --input <dat-file-or-directory> --out <report-directory> [--stride N]\n"
                  << "Read-only. Runs the public LZH parameter sets against every Nth LZ2K entry and reports exact fits.\n";
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

    const gamecore::lz2k::LzhParams paramSets[] = {gamecore::lz2k::kLh5, gamecore::lz2k::kLh6, gamecore::lz2k::kLh7};
    std::map<std::string, ParamStats> stats;
    std::uint64_t storedChunks = 0;
    std::uint64_t seen = 0;
    std::map<std::string, std::vector<Sniff>> sniffs;

    for (const auto& path : files) {
        const std::string datName = gamecore::io::pathToUtf8(path.filename());
        gamecore::io::ReadOnlyFile file;
        std::string error;
        gamecore::dat::DatListing listing;
        if (!gamecore::io::ReadOnlyFile::open(path, file, error) ||
            !gamecore::dat::listDat(file.source(), listing, error)) {
            std::cerr << datName << ": " << error << '\n';
            continue;
        }
        const auto source = file.source();
        for (const auto& entry : listing.entries) {
            if (entry.compression != 2 || !entry.inBounds) {
                continue;
            }
            if ((seen++ % stride) != 0) {
                continue;
            }
            std::vector<std::uint8_t> packed(entry.storedSize);
            if (!gamecore::io::readExact(source, entry.offset, packed.data(), packed.size())) {
                continue;
            }
            gamecore::io::MemoryBytes mem;
            const auto entrySource = gamecore::io::sourceFromSpan(packed, mem);
            const auto layout = gamecore::lz2k::parseLayout(entrySource, entry.originalSize);
            if (!layout.valid) {
                continue;
            }
            const std::string where = datName + ":" + entry.path;
            for (const auto& params : paramSets) {
                auto& s = stats[params.name];
                ++s.entries;
                bool entryOk = true;
                std::vector<std::uint8_t> whole;
                whole.reserve(entry.originalSize);
                for (std::size_t c = 0; c < layout.chunks.size(); ++c) {
                    const auto& chunk = layout.chunks[c];
                    const std::span<const std::uint8_t> payload(packed.data() + chunk.payloadOffset(), chunk.packedSize);
                    std::vector<std::uint8_t> raw(chunk.rawSize);
                    if (chunk.packedSize == chunk.rawSize) {
                        if (&params == &paramSets[0]) {
                            ++storedChunks;
                        }
                        std::copy(payload.begin(), payload.end(), raw.begin());
                        whole.insert(whole.end(), raw.begin(), raw.end());
                        continue;
                    }
                    ++s.chunks;
                    const auto result = gamecore::lz2k::decodeLzh(payload, raw, params);
                    if (result.ok) {
                        ++s.ok;
                        const std::int64_t slack = static_cast<std::int64_t>(chunk.packedSize) -
                                                   static_cast<std::int64_t>((result.bitsConsumed + 7) / 8);
                        ++s.slackBytes[slack];
                        if (slack == 0) {
                            ++s.exactByteEnd;
                        }
                        whole.insert(whole.end(), raw.begin(), raw.end());
                    } else {
                        entryOk = false;
                        ++s.failures[result.failure];
                        if (s.failureExample.find(result.failure) == s.failureExample.end()) {
                            std::ostringstream ex;
                            ex << where << " chunk " << c << " raw=" << chunk.rawSize << " packed=" << chunk.packedSize
                               << " produced=" << result.produced << " blocks=" << result.blocks
                               << " bits=" << result.bitsConsumed;
                            s.failureExample[result.failure] = ex.str();
                        }
                    }
                }
                if (entryOk && whole.size() == entry.originalSize) {
                    ++s.entriesOk;
                    const auto ext = gamecore::dat::lowerExtension(entry.path);
                    auto& list = sniffs[std::string(params.name) + " " + ext];
                    if (list.size() < 3) {
                        Sniff sniff;
                        sniff.where = where;
                        sniff.head.assign(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(whole.size(), 48)));
                        gamecore::io::MemoryBytes wm;
                        const auto ws = gamecore::io::sourceFromSpan(whole, wm);
                        sniff.format = gamecore::inspect::probeFormat(ws).name;
                        list.push_back(std::move(sniff));
                    }
                }
            }
        }
        std::cout << datName << " sampled_so_far=" << (seen + stride - 1) / stride << '\n';
    }

    std::filesystem::create_directories(output, ec);
    std::ofstream report(output / "lz2k_decode_test.txt", std::ios::binary | std::ios::trunc);
    report << "lz2k_decode_test: read-only; public LZH parameter sets vs. LZ2K chunks (every " << stride
           << "th LZ2K entry)\n";
    report << "ok = produced exactly raw_size without reading past packed_size\n";
    report << "chunks with packed == raw copied verbatim: " << storedChunks << "\n\n";
    for (const auto& params : paramSets) {
        const auto& s = stats[params.name];
        report << params.name << " (dict_bits " << params.dictBits << ", pos_bits " << params.posBits << ")\n";
        report << "  entries " << s.entries << " entries_ok " << s.entriesOk << '\n';
        report << "  chunks " << s.chunks << " ok " << s.ok << " ok_ending_on_last_byte " << s.exactByteEnd << '\n';
        report << "  slack_bytes (packed - bytes consumed: chunks):";
        for (const auto& [slack, count] : s.slackBytes) {
            report << ' ' << slack << ':' << count;
        }
        report << '\n';
        for (const auto& [reason, count] : s.failures) {
            report << "  failure \"" << reason << "\": " << count << "\n    e.g. " << s.failureExample.at(reason) << '\n';
        }
        report << '\n';
    }
    report << "content of fully decoded entries (first 48 bytes):\n";
    for (const auto& [key, list] : sniffs) {
        for (const auto& sniff : list) {
            report << "  [" << key << "] " << sniff.where << " format=" << sniff.format << "\n      "
                   << hexAscii(sniff.head, 48) << '\n';
        }
    }
    for (const auto& params : paramSets) {
        const auto& s = stats[params.name];
        std::cout << params.name << ": chunks " << s.chunks << " ok " << s.ok << " exact_end " << s.exactByteEnd
                  << " entries_ok " << s.entriesOk << "/" << s.entries << '\n';
    }
    return 0;
}
