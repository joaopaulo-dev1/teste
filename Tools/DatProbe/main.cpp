#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatProbe/DatProbe.hpp"
#include "Tools/DatProbe/EntryProbe.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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

bool isDatLike(const std::filesystem::path& path) {
    std::string extension = gamecore::io::pathToUtf8(path.extension());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension.rfind(".dat", 0) == 0;
}

std::string hex32(std::uint32_t value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(kHex[(value >> shift) & 0xf]);
    }
    return out;
}

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
        std::cerr << "dat_probe --input <file-or-directory> --out <report-directory>\n"
                  << "Read-only. Tests arithmetic readings of header field 0 against the file size.\n";
        return 1;
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        files.push_back(input);
    } else if (std::filesystem::is_directory(input, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(input, ec)) {
            if (entry.is_regular_file() && isDatLike(entry.path())) {
                files.push_back(entry.path());
            }
        }
    } else {
        std::cerr << "input not found\n";
        return 1;
    }
    std::sort(files.begin(), files.end());

    std::filesystem::create_directories(output, ec);
    std::ofstream report(output / "dat_probe.txt", std::ios::binary | std::ios::trunc);
    if (!report) {
        std::cerr << "cannot write report\n";
        return 1;
    }
    report << "dat_probe: read-only; candidates are arithmetic readings of field0, accepted only if offset+field1 == file size\n\n";

    std::map<std::string, int> matchesByTransform;
    int probed = 0;
    int errors = 0;
    for (const auto& path : files) {
        gamecore::io::ReadOnlyFile file;
        std::string error;
        if (!gamecore::io::ReadOnlyFile::open(path, file, error)) {
            report << "error: " << error << "\n\n";
            ++errors;
            continue;
        }
        const auto source = file.source();
        const auto result = gamecore::dat::probeDat(source);
        ++probed;
        report << "file: " << gamecore::io::pathToUtf8(path.filename()) << '\n';
        report << "size: " << result.fileSize << '\n';
        if (!result.headerRead) {
            report << "header: truncated\n\n";
            continue;
        }
        report << "field0: " << hex32(result.field0) << " (signed " << static_cast<std::int32_t>(result.field0) << ")\n";
        report << "field1: " << hex32(result.field1) << " (" << result.field1 << ")\n";
        report << "zero_bytes_after_header: " << result.zeroPaddingBytes << '\n';
        for (const auto& candidate : result.candidates) {
            report << "  candidate " << std::left << std::setw(22) << candidate.transform << " offset=" << std::setw(12)
                   << candidate.offset << " in_file=" << candidate.inFile << " fits=" << candidate.tableFits
                   << " ends_at_eof=" << candidate.endsAtEof << '\n';
        }
        if (result.matchedTransform.empty()) {
            report << "match: none\n";
        } else {
            ++matchesByTransform[result.matchedTransform];
            report << "match: " << result.matchedTransform << " table_offset=" << result.tableOffset
                   << " table_size=" << result.tableSize << '\n';
            report << "table_entropy64k: " << result.tableEntropy << '\n';
            report << "table_lead_u32:";
            for (auto word : result.tableLeadWords) {
                report << ' ' << hex32(word);
            }
            report << "\ntable_head:\n" << result.tableHead;

            const auto entries =
                gamecore::dat::analyzeEntries(source, result.tableOffset, result.tableSize, 4000);
            if (!entries.parsed) {
                report << "entries: not parsed (" << entries.failure << ")\n";
            } else {
                report << "entries.preamble: " << hex32(entries.preamble) << '\n';
                report << "entries.count: " << entries.declaredCount << '\n';
                report << "entries.record_bytes: " << entries.recordBytes
                       << " trailing_bytes: " << entries.trailingBytes << '\n';
                report << "check3.b_eq_c&low0: " << entries.equalAndLowZero
                       << " b_eq_c&low!=0: " << entries.equalAndLowNonZero
                       << " b_ne_c&low0: " << entries.differentAndLowZero
                       << " b_ne_c&low!=0: " << entries.differentAndLowNonZero
                       << " b_gt_c: " << entries.bGreaterThanC << '\n';
                report << "check3.low_byte_histogram:";
                for (const auto& [value, count] : entries.lowByteHistogram) {
                    report << ' ' << value << '=' << count;
                }
                report << "\ncheck3.middle_16bit_values: " << entries.middleBytesHistogram.size()
                       << " distinct_high_bytes: " << entries.distinctHighBytes << '\n';
                auto topN = [](const std::map<std::string, std::uint64_t>& histogram, std::size_t n) {
                    std::vector<std::pair<std::uint64_t, std::string>> sorted;
                    sorted.reserve(histogram.size());
                    for (const auto& [key, count] : histogram) {
                        sorted.emplace_back(count, key);
                    }
                    std::sort(sorted.rbegin(), sorted.rend());
                    if (sorted.size() > n) {
                        sorted.resize(n);
                    }
                    return sorted;
                };
                for (const auto& rule : entries.rules) {
                    const char* name = gamecore::dat::toString(rule.rule);
                    report << "rule " << name << ":\n";
                    report << "  check1.end_past_table: " << rule.outOfBounds
                           << " start_below_header: " << rule.belowHeader << '\n';
                    report << "  check2.overlaps: " << rule.overlaps << " covered_bytes: " << rule.coveredBytes
                           << " of " << entries.dataRegionBytes << " max_gap: " << rule.maxGap << '\n';
                    report << "  check4.stored_sampled: " << rule.storedSampled
                           << " recognized: " << rule.storedRecognized << " distinct_leads: "
                           << rule.storedFormats.size() << '\n';
                    for (const auto& [count, key] : topN(rule.storedFormats, 10)) {
                        report << "    stored " << key << ' ' << count << '\n';
                    }
                    report << "  check4.compressed_sampled: " << rule.compressedSampled
                           << " distinct_leads: " << rule.compressedLeadBytes.size() << '\n';
                    for (const auto& [count, key] : topN(rule.compressedLeadBytes, 10)) {
                        report << "    compressed_lead " << key << ' ' << count << '\n';
                    }
                }
                report << "check5.trailing_printable_ratio: " << entries.trailingPrintableRatio << '\n';
                report << "check5.trailing_lead_u32:";
                for (auto word : entries.trailingLeadWords) {
                    report << ' ' << hex32(word);
                }
                report << "\ncheck5.trailing_head:\n" << entries.trailingHead;
            }
        }
        report << "file_tail:\n" << result.fileTail << '\n';
    }

    report << "summary: probed=" << probed << " errors=" << errors << '\n';
    for (const auto& [transform, count] : matchesByTransform) {
        report << "  " << transform << " matched " << count << '/' << probed << '\n';
    }
    std::cout << "probed=" << probed << " errors=" << errors;
    for (const auto& [transform, count] : matchesByTransform) {
        std::cout << " [" << transform << "=" << count << "]";
    }
    std::cout << '\n';
    return errors == 0 ? 0 : 2;
}
