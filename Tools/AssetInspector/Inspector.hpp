#pragma once

#include "Tools/AssetInspector/ZipLister.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gamecore::inspect {

struct FileReport {
    std::string sourcePath;
    std::string inspectedPath;
    std::uint64_t size = 0;
    std::string binaryKind = "none";
    std::string architecture = "unknown";
    std::string endianness = "unknown";
    std::vector<std::string> fatArchitectures;
    std::string formatName = "unknown";
    std::string formatFamily = "unknown";
    std::string formatNote;
    bool recognized = false;
    std::vector<std::string> libraries;
    std::vector<std::string> graphicsApis;
    std::vector<std::string> graphicsEvidence;
    std::vector<std::string> notes;
    double entropy = 0.0;
    std::string headerHex;
    ZipSummary zip;
};

struct InspectOptions {
    std::filesystem::path input;
    std::filesystem::path outputDirectory;
    bool copyFirst = false;
    bool writeJson = false;
    std::uint64_t maxFiles = 20000;
    std::uint64_t stringScanBytes = 4ull << 20;
};

struct InspectResult {
    std::vector<FileReport> files;
    std::vector<std::string> errors;
    bool hitFileCap = false;
};

bool inspectTree(const InspectOptions& options, InspectResult& result, std::string& error);
bool writeReports(const InspectOptions& options, const InspectResult& result, std::string& error);

}  // namespace gamecore::inspect
