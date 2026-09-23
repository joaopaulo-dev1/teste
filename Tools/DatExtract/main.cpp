#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/DatExtract/Extractor.hpp"
#include "Tools/DatProbe/DatIndex.hpp"
#include "Tools/DatProbe/DatProbe.hpp"
#include "Tools/DatProbe/NameTable.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

std::string lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool isDatLike(const std::filesystem::path& path) {
    return lower(gamecore::io::pathToUtf8(path.extension())).rfind(".dat", 0) == 0;
}

std::set<std::string> parseExtensions(const std::string& list) {
    std::set<std::string> out;
    std::string current;
    auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        if (current.front() != '.') {
            current.insert(current.begin(), '.');
        }
        out.insert(lower(current));
        current.clear();
    };
    for (char ch : list) {
        if (ch == ',') {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();
    return out;
}

bool isInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code ec;
    const auto c = std::filesystem::weakly_canonical(child, ec);
    const auto p = std::filesystem::weakly_canonical(parent, ec);
    const auto relative = c.lexically_relative(p);
    if (relative.empty()) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

void printUsage() {
    std::cerr << "dat_extract --input <dat-file-or-directory> --out <directory> [--ext .ogg,.wav] [--dry-run]\n"
              << "Copies stored (uncompressed) entries to <out>/<dat name>/<original path>.\n"
              << "Compressed entries are counted and skipped. The DAT files are opened read-only.\n";
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = readArgs(argc, argv);
    std::filesystem::path input;
    gamecore::extract::ExtractOptions base;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input" && i + 1 < args.size()) {
            input = args[++i];
        } else if (arg == "--out" && i + 1 < args.size()) {
            base.outRoot = args[++i];
        } else if (arg == "--ext" && i + 1 < args.size()) {
            base.extensions = parseExtensions(gamecore::io::pathToUtf8(args[++i]));
        } else if (arg == "--dry-run") {
            base.dryRun = true;
        } else {
            printUsage();
            return 1;
        }
    }
    if (input.empty() || base.outRoot.empty()) {
        printUsage();
        return 1;
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::path inputRoot = input;
    if (std::filesystem::is_regular_file(input, ec)) {
        files.push_back(input);
        inputRoot = input.parent_path();
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
    if (isInside(base.outRoot, inputRoot) || std::filesystem::weakly_canonical(base.outRoot, ec) ==
                                                 std::filesystem::weakly_canonical(inputRoot, ec)) {
        std::cerr << "refusing to write inside the game directory\n";
        return 1;
    }

    struct Loaded {
        std::filesystem::path path;
        gamecore::io::ReadOnlyFile file;
        gamecore::dat::DatIndex index;
        gamecore::dat::NameTable names;
    };
    std::vector<Loaded> loaded;
    loaded.reserve(files.size());
    std::uint64_t plannedBytes = 0;
    for (const auto& path : files) {
        Loaded item;
        item.path = path;
        std::string error;
        if (!gamecore::io::ReadOnlyFile::open(path, item.file, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto header = gamecore::dat::probeDat(item.file.source());
        if (header.matchedTransform.empty() ||
            !gamecore::dat::readDatIndex(item.file.source(), header.tableOffset, header.tableSize, item.index, error)) {
            std::cerr << "index unreadable: " << gamecore::io::pathToUtf8(path) << ' ' << error << '\n';
            return 1;
        }
        item.names = gamecore::dat::parseNameTable(item.index.trailing, static_cast<std::uint32_t>(item.index.entries.size()));
        if (!item.names.parsed || item.names.missingIds != 0 || item.names.duplicateIds != 0) {
            std::cerr << "name table failed validation: " << gamecore::io::pathToUtf8(path) << '\n';
            return 1;
        }
        plannedBytes += gamecore::extract::storedBytes(item.index, item.names, base.extensions);
        loaded.push_back(std::move(item));
    }

    std::filesystem::create_directories(base.outRoot, ec);
    if (!base.dryRun) {
        const auto space = std::filesystem::space(base.outRoot, ec);
        const std::uint64_t margin = plannedBytes / 20 + (256ull << 20);
        if (!ec && space.available < plannedBytes + margin) {
            std::cerr << "not enough free space: need " << ((plannedBytes + margin) >> 20) << " MiB, have "
                      << (space.available >> 20) << " MiB\n";
            return 1;
        }
    }
    std::cout << "planned=" << (plannedBytes >> 20) << " MiB across " << loaded.size() << " DAT files\n";

    std::ofstream log(base.outRoot / "extract_errors.log", std::ios::binary | std::ios::trunc);
    gamecore::extract::ExtractStats total;
    for (auto& item : loaded) {
        const std::string datName = gamecore::io::pathToUtf8(item.path.filename());
        gamecore::extract::ExtractOptions options = base;
        options.outRoot = base.outRoot / datName;
        std::filesystem::create_directories(options.outRoot, ec);
        std::ofstream manifest(options.outRoot / "manifest.tsv", std::ios::binary | std::ios::trunc);
        std::vector<std::string> errors;
        const auto stats =
            gamecore::extract::extractStored(item.file.source(), item.index, item.names, options, manifest, errors);
        for (const auto& message : errors) {
            log << datName << ": " << message << '\n';
        }
        std::cout << datName << ": written=" << stats.written << " reused=" << stats.reused
                  << " MiB=" << (stats.bytes >> 20) << " skipped_compressed=" << stats.skippedCompressed
                  << " skipped_filter=" << stats.skippedFilter << " rejected=" << stats.rejectedPaths
                  << " errors=" << stats.errors << '\n';
        total.written += stats.written;
        total.reused += stats.reused;
        total.bytes += stats.bytes;
        total.skippedCompressed += stats.skippedCompressed;
        total.rejectedPaths += stats.rejectedPaths;
        total.errors += stats.errors;
    }
    std::cout << "total: written=" << total.written << " reused=" << total.reused << " MiB=" << (total.bytes >> 20)
              << " skipped_compressed=" << total.skippedCompressed << " rejected=" << total.rejectedPaths
              << " errors=" << total.errors << '\n';
    return (total.errors == 0 && total.rejectedPaths == 0) ? 0 : 2;
}
