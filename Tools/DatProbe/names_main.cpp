#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatProbe/ContentCheck.hpp"
#include "Tools/DatProbe/DatIndex.hpp"
#include "Tools/DatProbe/DatProbe.hpp"
#include "Tools/DatProbe/NameTable.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
        std::cerr << "dat_names --input <file-or-directory> --out <report-directory>\n"
                  << "Read-only. Parses the name tree after the entry records and cross-checks names against content.\n";
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
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "no DAT files found\n";
        return 1;
    }

    const auto listingDir = output / "listing";
    std::filesystem::create_directories(listingDir, ec);
    std::ofstream report(output / "dat_names.txt", std::ios::binary | std::ios::trunc);
    if (!report) {
        std::cerr << "cannot write report\n";
        return 1;
    }
    report << "dat_names: read-only; node layout is a hypothesis checked below, file ids map to entry records\n\n";

    std::map<std::string, gamecore::dat::ExtensionStats> global;
    int failures = 0;
    for (const auto& path : files) {
        const std::string fileName = gamecore::io::pathToUtf8(path.filename());
        gamecore::io::ReadOnlyFile file;
        std::string error;
        if (!gamecore::io::ReadOnlyFile::open(path, file, error)) {
            report << "file: " << fileName << "\nerror: " << error << "\n\n";
            ++failures;
            continue;
        }
        const auto source = file.source();
        const auto header = gamecore::dat::probeDat(source);
        report << "file: " << fileName << '\n';
        if (header.matchedTransform.empty()) {
            report << "error: table offset rule did not match\n\n";
            ++failures;
            continue;
        }
        gamecore::dat::DatIndex index;
        if (!gamecore::dat::readDatIndex(source, header.tableOffset, header.tableSize, index, error)) {
            report << "error: " << error << "\n\n";
            ++failures;
            continue;
        }
        const auto names = gamecore::dat::parseNameTable(index.trailing, static_cast<std::uint32_t>(index.entries.size()));
        if (!names.parsed) {
            report << "error: " << names.failure << "\n\n";
            ++failures;
            continue;
        }
        report << "entries: " << index.entries.size() << '\n';
        report << "nodes: " << names.nodeCount << " (files " << names.fileNodes << ", directories "
               << names.directoryNodes << ", sentinel 1)\n";
        report << "names_size: " << names.namesSize << " bytes_after_names: " << names.bytesAfterNames << '\n';
        report << "check.bad_name_offsets: " << names.badNameOffsets << '\n';
        report << "check.parent_out_of_range: " << names.parentOutOfRange
               << " parent_not_earlier: " << names.parentNotEarlier << " cycles: " << names.cycles << '\n';
        report << "check.prev_out_of_range: " << names.prevOutOfRange << " extra_nonzero: " << names.extraNonZero
               << '\n';
        report << "check.file_ids: out_of_range " << names.idsOutOfRange << " duplicate " << names.duplicateIds
               << " missing " << names.missingIds << " (files must equal entries: "
               << (names.fileNodes == index.entries.size() ? "yes" : "no") << ")\n";

        const auto stats = gamecore::dat::crossCheck(source, index, names, 2000);
        std::vector<std::pair<std::uint64_t, std::string>> byCount;
        for (const auto& [ext, value] : stats) {
            byCount.emplace_back(value.count, ext);
            auto& merged = global[ext];
            merged.count += value.count;
            merged.stored += value.stored;
            merged.compressed += value.compressed;
            merged.totalOriginalBytes += value.totalOriginalBytes;
            for (const auto& [label, n] : value.detected) {
                merged.detected[label] += n;
            }
        }
        std::sort(byCount.rbegin(), byCount.rend());
        report << "extensions (count stored compressed | labels of content at the mapped entry):\n";
        for (std::size_t i = 0; i < byCount.size() && i < 25; ++i) {
            const auto& ext = byCount[i].second;
            const auto& value = stats.at(ext);
            report << "  " << ext << ' ' << value.count << ' ' << value.stored << ' ' << value.compressed << " |";
            for (const auto& [label, n] : value.detected) {
                report << ' ' << label << '=' << n;
            }
            report << '\n';
        }

        std::ofstream listing(listingDir / (fileName + ".tsv"), std::ios::binary | std::ios::trunc);
        listing << "id\tpath\toffset\tstored\toriginal\tcompression\n";
        for (std::size_t id = 0; id < names.fileIdToNode.size(); ++id) {
            const int node = names.fileIdToNode[id];
            const auto& entry = index.entries[id];
            listing << id << '\t' << (node < 0 ? std::string("(unnamed)") : gamecore::dat::fullPath(names, node))
                    << '\t' << gamecore::dat::entryOffset(entry, gamecore::dat::OffsetRule::Shl8OrHighByte) << '\t'
                    << entry.b << '\t' << entry.c << '\t' << (entry.flags & 0xffu) << '\n';
        }
        report << "sample_paths:\n";
        for (std::size_t id = 0, shown = 0; id < names.fileIdToNode.size() && shown < 12; id += 1 + names.fileIdToNode.size() / 12) {
            if (names.fileIdToNode[id] >= 0) {
                report << "  " << id << ' ' << gamecore::dat::fullPath(names, names.fileIdToNode[id]) << '\n';
                ++shown;
            }
        }
        report << '\n';
    }

    std::vector<std::pair<std::uint64_t, std::string>> totals;
    for (const auto& [ext, value] : global) {
        totals.emplace_back(value.totalOriginalBytes, ext);
    }
    std::sort(totals.rbegin(), totals.rend());
    report << "all_files_by_original_bytes (ext count stored compressed MiB | labels):\n";
    for (std::size_t i = 0; i < totals.size() && i < 40; ++i) {
        const auto& ext = totals[i].second;
        const auto& value = global.at(ext);
        report << "  " << ext << ' ' << value.count << ' ' << value.stored << ' ' << value.compressed << ' '
               << (value.totalOriginalBytes >> 20) << " |";
        for (const auto& [label, n] : value.detected) {
            report << ' ' << label << '=' << n;
        }
        report << '\n';
    }
    std::cout << "dat files=" << files.size() << " failures=" << failures << " extensions=" << global.size() << '\n';
    return failures == 0 ? 0 : 2;
}
