#include "Tools/AssetInspector/Inspector.hpp"

#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/AssetInspector/ExecutableProbe.hpp"
#include "Tools/AssetInspector/FormatProbe.hpp"
#include "Tools/AssetInspector/GraphicsScan.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <string_view>

namespace gamecore::inspect {
namespace {

bool isInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto relative = child.lexically_normal().lexically_relative(parent.lexically_normal());
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

void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::string lowerCopy(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

void addApiFromLibrary(std::vector<std::string>& apis, std::vector<std::string>& evidence, std::string_view library) {
    const std::string lower = lowerCopy(library);
    auto add = [&](const char* api, const char* token) {
        if (lower.find(token) != std::string::npos) {
            addUnique(apis, api);
            addUnique(evidence, std::string(token) + " import");
        }
    };
    add("directx9", "d3d9.dll");
    add("directx11", "d3d11.dll");
    add("directx12", "d3d12.dll");
    add("directxgi", "dxgi.dll");
    add("opengl", "opengl32.dll");
    add("opengles", "libglesv2");
    add("opengles", "libegl");
    add("vulkan", "vulkan-1.dll");
    add("metal", "metal.framework");
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 32) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[ch >> 4]);
                    out.push_back(kHex[ch & 0xf]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

void writeStringArray(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << jsonEscape(values[i]) << '"';
    }
    out << ']';
}

FileReport inspectOpenFile(const io::ReadOnlyFile& file, const InspectOptions& options, const std::filesystem::path& source) {
    FileReport report;
    report.sourcePath = io::pathToUtf8(source);
    report.inspectedPath = io::pathToUtf8(file.path());
    report.size = file.size();
    const io::ByteSource bytes = file.source();

    const ExecutableInfo executable = probeExecutable(bytes);
    report.binaryKind = toString(executable.kind);
    report.architecture = toString(executable.arch);
    report.endianness = executable.endianKnown ? (executable.littleEndian ? "little" : "big") : "unknown";
    report.libraries = executable.libraries;
    report.notes = executable.notes;
    for (CpuArch arch : executable.fatArchitectures) {
        report.fatArchitectures.emplace_back(toString(arch));
    }

    const bool executableRecognized = executable.kind == BinaryKind::Pe || executable.kind == BinaryKind::Elf ||
                                      executable.kind == BinaryKind::MachO || executable.kind == BinaryKind::FatMachO;
    const FormatHit format = probeFormat(bytes);
    if (executableRecognized && !format.recognized) {
        report.formatName = report.binaryKind;
        report.formatFamily = "executable";
        report.formatNote = "documented executable container; architecture and linked libraries were parsed";
    } else {
        report.formatName = format.name;
        report.formatFamily = format.family;
        report.formatNote = format.note;
    }
    if (!report.formatNote.empty()) {
        report.notes.push_back(report.formatNote);
    }

    report.recognized = executableRecognized || format.recognized;

    if (format.name == "zip") {
        report.zip = summarizeZip(bytes);
        if (!report.zip.note.empty()) {
            report.notes.push_back(report.zip.note);
        }
    }

    for (const GraphicsEvidence& evidence : scanGraphicsApis(bytes, options.stringScanBytes)) {
        addUnique(report.graphicsApis, evidence.api);
        addUnique(report.graphicsEvidence, evidence.evidence);
    }
    for (const std::string& library : report.libraries) {
        addApiFromLibrary(report.graphicsApis, report.graphicsEvidence, library);
    }

    report.entropy = shannonEntropy(bytes, 1ull << 16);
    if (!report.recognized) {
        report.headerHex = hexAsciiDump(bytes, 64);
        report.notes.push_back("unknown layout logged; no proprietary specification was assumed");
        if (report.entropy > 7.5) {
            report.notes.push_back(
                "high entropy payload; compressed or encrypted bytes are not inflated or decrypted");
        }
    }
    return report;
}

void inspectOne(const std::filesystem::path& source, const InspectOptions& options, InspectResult& result) {
    std::filesystem::path inspected = source;
    if (options.copyFirst) {
        std::filesystem::path relative = source.filename();
        if (std::filesystem::is_directory(options.input)) {
            std::error_code relativeEc;
            relative = std::filesystem::relative(source, options.input, relativeEc);
            if (relativeEc) {
                result.errors.push_back("failed to relativize " + io::pathToUtf8(source));
                return;
            }
        }
        for (const auto& part : relative) {
            if (part == "..") {
                result.errors.push_back("refusing copy that escapes the workspace: " + io::pathToUtf8(source));
                return;
            }
        }
        const auto destination = options.outputDirectory / "copies" / relative;
        if (!isInside(destination, options.outputDirectory / "copies")) {
            result.errors.push_back("refusing copy that escapes the workspace: " + io::pathToUtf8(source));
            return;
        }
        std::string copyError;
        if (!io::ReadOnlyFile::copyTo(source, destination, copyError)) {
            result.errors.push_back(copyError);
            return;
        }
        inspected = destination;
    }

    io::ReadOnlyFile file;
    std::string openError;
    if (!io::ReadOnlyFile::open(inspected, file, openError)) {
        result.errors.push_back(openError);
        return;
    }
    result.files.push_back(inspectOpenFile(file, options, source));
}

void bump(std::map<std::string, std::uint64_t>& histogram, const std::string& key) {
    ++histogram[key];
}

}  // namespace

bool inspectTree(const InspectOptions& options, InspectResult& result, std::string& error) {
    result = {};
    std::error_code ec;
    if (!std::filesystem::exists(options.input, ec)) {
        error = "input path does not exist: " + io::pathToUtf8(options.input);
        return false;
    }
    std::filesystem::create_directories(options.outputDirectory, ec);
    if (ec) {
        error = "failed to create output directory: " + io::pathToUtf8(options.outputDirectory);
        return false;
    }

    const auto status = std::filesystem::status(options.input, ec);
    if (ec) {
        error = "failed to stat input: " + io::pathToUtf8(options.input);
        return false;
    }
    if (std::filesystem::is_regular_file(status)) {
        inspectOne(options.input, options, result);
        error.clear();
        return true;
    }
    if (!std::filesystem::is_directory(status)) {
        error = "input is neither a file nor a directory: " + io::pathToUtf8(options.input);
        return false;
    }

    const auto optionsFlags = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(options.input, optionsFlags, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (result.files.size() + result.errors.size() >= options.maxFiles) {
            result.hitFileCap = true;
            break;
        }
        const auto& entry = *it;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;
        }
        if (isInside(entry.path(), options.outputDirectory)) {
            continue;
        }
        inspectOne(entry.path(), options, result);
    }
    if (ec) {
        result.errors.push_back("directory walk stopped: " + ec.message());
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const FileReport& a, const FileReport& b) { return a.sourcePath < b.sourcePath; });
    error.clear();
    return true;
}

bool writeReports(const InspectOptions& options, const InspectResult& result, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(options.outputDirectory, ec);
    if (ec) {
        error = "failed to create output directory";
        return false;
    }

    const auto reportPath = options.outputDirectory / "report.txt";
    const auto unknownPath = options.outputDirectory / "unknown.log";
    const auto summaryPath = options.outputDirectory / "summary.txt";
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    std::ofstream unknown(unknownPath, std::ios::binary | std::ios::trunc);
    std::ofstream summary(summaryPath, std::ios::binary | std::ios::trunc);
    if (!report || !unknown || !summary) {
        error = "failed to create report files in " + io::pathToUtf8(options.outputDirectory);
        return false;
    }

    report << "inspector: read-only\n";
    report << "originals: opened without write access and never rewritten\n";
    report << "format_policy: documented signatures only; unknown payloads are logged and not guessed\n";
    report << "encryption_policy: encrypted zip entries are counted and not unwrapped\n\n";

    std::map<std::string, std::uint64_t> byFormat;
    std::map<std::string, std::uint64_t> byArch;
    std::map<std::string, std::uint64_t> byGraphics;
    std::map<std::string, std::uint64_t> byExtension;
    std::uint64_t unknownCount = 0;

    for (const FileReport& file : result.files) {
        report << "file: " << file.sourcePath << '\n';
        report << "inspected: " << file.inspectedPath << '\n';
        report << "size: " << file.size << '\n';
        report << "binary: " << file.binaryKind << '\n';
        report << "arch: " << file.architecture << '\n';
        report << "endian: " << file.endianness << '\n';
        if (!file.fatArchitectures.empty()) {
            report << "fat_arch:";
            for (const auto& arch : file.fatArchitectures) {
                report << ' ' << arch;
            }
            report << '\n';
        }
        report << "format: " << file.formatName << '\n';
        report << "family: " << file.formatFamily << '\n';
        report << "recognized: " << (file.recognized ? "yes" : "no") << '\n';
        report << "entropy64k: " << file.entropy << '\n';
        if (!file.libraries.empty()) {
            report << "libraries:";
            for (const auto& library : file.libraries) {
                report << ' ' << library;
            }
            report << '\n';
        }
        if (!file.graphicsApis.empty()) {
            report << "graphics:";
            for (const auto& api : file.graphicsApis) {
                report << ' ' << api;
            }
            report << '\n';
        }
        if (!file.graphicsEvidence.empty()) {
            report << "graphics_evidence:";
            for (const auto& evidence : file.graphicsEvidence) {
                report << ' ' << evidence;
            }
            report << '\n';
        }
        if (file.zip.parsed) {
            report << "zip_entries_seen: " << file.zip.entryCount << '\n';
            report << "zip_encrypted: " << file.zip.encryptedCount << '\n';
            report << "zip_extensions:";
            for (const auto& [ext, count] : file.zip.extensions) {
                report << ' ' << ext << '=' << count;
                byExtension[ext] += count;
            }
            report << '\n';
            if (!file.zip.sampleNames.empty()) {
                report << "zip_sample:\n";
                for (const auto& name : file.zip.sampleNames) {
                    report << "  " << name << '\n';
                }
            }
        }
        for (const auto& note : file.notes) {
            report << "note: " << note << '\n';
        }
        if (!file.recognized) {
            ++unknownCount;
            unknown << "file: " << file.sourcePath << '\n';
            unknown << "size: " << file.size << '\n';
            unknown << "entropy64k: " << file.entropy << '\n';
            unknown << file.headerHex << '\n';
            report << "header:\n" << file.headerHex;
        }
        report << '\n';

        bump(byFormat, file.recognized ? file.formatName : "unknown");
        if (file.architecture != "unknown") {
            bump(byArch, file.architecture);
        }
        for (const auto& api : file.graphicsApis) {
            bump(byGraphics, api);
        }
    }

    for (const auto& message : result.errors) {
        report << "error: " << message << '\n';
    }

    summary << "files: " << result.files.size() << '\n';
    summary << "unknown: " << unknownCount << '\n';
    summary << "open_errors: " << result.errors.size() << '\n';
    summary << "hit_file_cap: " << (result.hitFileCap ? "yes" : "no") << '\n';
    summary << "by_format:\n";
    for (const auto& [name, count] : byFormat) {
        summary << "  " << name << ' ' << count << '\n';
    }
    summary << "by_arch:\n";
    for (const auto& [name, count] : byArch) {
        summary << "  " << name << ' ' << count << '\n';
    }
    summary << "by_graphics:\n";
    for (const auto& [name, count] : byGraphics) {
        summary << "  " << name << ' ' << count << '\n';
    }
    summary << "zip_extensions:\n";
    for (const auto& [name, count] : byExtension) {
        summary << "  " << name << ' ' << count << '\n';
    }

    if (options.writeJson) {
        std::ofstream json(options.outputDirectory / "report.json", std::ios::binary | std::ios::trunc);
        if (!json) {
            error = "failed to create report.json";
            return false;
        }
        json << "[\n";
        for (std::size_t i = 0; i < result.files.size(); ++i) {
            const FileReport& file = result.files[i];
            json << "  {\"path\":\"" << jsonEscape(file.sourcePath) << "\",\"size\":" << file.size
                 << ",\"binary\":\"" << jsonEscape(file.binaryKind) << "\",\"arch\":\""
                 << jsonEscape(file.architecture) << "\",\"format\":\"" << jsonEscape(file.formatName)
                 << "\",\"family\":\"" << jsonEscape(file.formatFamily) << "\",\"recognized\":"
                 << (file.recognized ? "true" : "false") << ",\"entropy\":" << file.entropy << ",\"graphics\":";
            writeStringArray(json, file.graphicsApis);
            json << ",\"libraries\":";
            writeStringArray(json, file.libraries);
            json << '}';
            if (i + 1 != result.files.size()) {
                json << ',';
            }
            json << '\n';
        }
        json << "]\n";
    }
    return true;
}

}  // namespace gamecore::inspect
