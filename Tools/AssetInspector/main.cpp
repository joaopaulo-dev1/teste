#include "Tools/AssetInspector/Inspector.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
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

void printUsage() {
    std::cerr
        << "asset_inspector --input <file-or-directory> --out <report-directory> [--copy] [--json] [--max-files N]\n"
        << "Reads originals with a read-only handle. --copy streams a workspace copy under <out>/copies and inspects that copy.\n"
        << "Documented containers and executables are identified. Unknown layouts are hex-dumped and not guessed.\n";
}

bool parseCount(const std::string& text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#if defined(_WIN32)
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArgs == nullptr) {
        std::cerr << "failed to read the command line\n";
        return 1;
    }
    args.reserve(static_cast<std::size_t>(wideCount));
    for (int i = 0; i < wideCount; ++i) {
        args.emplace_back(wideArgs[i]);
    }
    LocalFree(wideArgs);
    (void)argc;
    (void)argv;
#else
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif

    gamecore::inspect::InspectOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::filesystem::path arg = args[i];
        auto needValue = [&](std::filesystem::path& value) {
            if (i + 1 >= args.size()) {
                return false;
            }
            value = args[++i];
            return true;
        };
        if (arg == "--input") {
            if (!needValue(options.input)) {
                printUsage();
                return 1;
            }
        } else if (arg == "--out") {
            if (!needValue(options.outputDirectory)) {
                printUsage();
                return 1;
            }
        } else if (arg == "--copy") {
            options.copyFirst = true;
        } else if (arg == "--json") {
            options.writeJson = true;
        } else if (arg == "--max-files") {
            std::filesystem::path value;
            if (!needValue(value) || !parseCount(value.string(), options.maxFiles) || options.maxFiles == 0) {
                printUsage();
                return 1;
            }
        } else if (arg == "--help") {
            printUsage();
            return 0;
        } else {
            printUsage();
            return 1;
        }
    }
    if (options.input.empty() || options.outputDirectory.empty()) {
        printUsage();
        return 1;
    }

    gamecore::inspect::InspectResult result;
    std::string error;
    if (!gamecore::inspect::inspectTree(options, result, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!gamecore::inspect::writeReports(options, result, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "files=" << result.files.size() << " errors=" << result.errors.size()
              << " report=" << options.outputDirectory.string() << "/report.txt\n";
    return result.errors.empty() ? 0 : 2;
}
