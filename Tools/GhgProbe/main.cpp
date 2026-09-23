#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/GhgProbe/GhgScan.hpp"
#include "Tools/GhgProbe/PngWriter.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

bool readWhole(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    gamecore::io::ReadOnlyFile file;
    if (!gamecore::io::ReadOnlyFile::open(path, file, error)) {
        return false;
    }
    out.resize(static_cast<std::size_t>(file.size()));
    if (!gamecore::io::readExact(file.source(), 0, out.data(), out.size())) {
        error = "short read";
        return false;
    }
    return true;
}

// Three orthographic views (X/Y front, Z/Y side, X/Z top) of every decoded position, plus edges of
// index buffers whose largest index fits the single position buffer they are paired with.
bool renderViews(const std::filesystem::path& ghg, const std::filesystem::path& png, std::ostream& log) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!readWhole(ghg, bytes, error)) {
        log << "render: " << error << '\n';
        return false;
    }
    const auto container = gamecore::ghg::parseContainer(bytes);
    const auto scan = gamecore::ghg::scanObjects(bytes, container);
    std::vector<std::vector<gamecore::ghg::Float3>> buffers;
    for (const auto& vb : scan.vertexBuffers) {
        std::vector<gamecore::ghg::Float3> positions;
        std::uint64_t wNotOne = 0;
        if (gamecore::ghg::readPositions(bytes, vb, positions, wNotOne)) {
            buffers.push_back(std::move(positions));
        }
    }
    if (buffers.empty()) {
        log << "render: no position buffers\n";
        return false;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& buffer : buffers) {
        for (const auto& p : buffer) {
            const float c[3] = {p.x, p.y, p.z};
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
    }
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-6f});
    constexpr std::uint32_t kView = 400;
    constexpr std::uint32_t kWidth = kView * 3;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kWidth) * kView * 3, 24);
    auto plot = [&](int view, float u, float v, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const int x = static_cast<int>(20 + u * (kView - 40)) + view * static_cast<int>(kView);
        const int y = static_cast<int>(kView - 20 - v * (kView - 40));
        if (x < view * static_cast<int>(kView) || x >= (view + 1) * static_cast<int>(kView) || y < 0 ||
            y >= static_cast<int>(kView)) {
            return;
        }
        const std::size_t i = (static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 3;
        rgb[i] = r;
        rgb[i + 1] = g;
        rgb[i + 2] = b;
    };
    auto project = [&](const gamecore::ghg::Float3& p, int view, float& u, float& v) {
        const float nx = (p.x - lo[0]) / extent;
        const float ny = (p.y - lo[1]) / extent;
        const float nz = (p.z - lo[2]) / extent;
        if (view == 0) {
            u = nx;
            v = ny;
        } else if (view == 1) {
            u = nz;
            v = ny;
        } else {
            u = nx;
            v = nz;
        }
    };
    auto line = [&](int view, const gamecore::ghg::Float3& a, const gamecore::ghg::Float3& b) {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        project(a, view, u0, v0);
        project(b, view, u1, v1);
        const int steps = std::max(2, static_cast<int>(std::hypot(u1 - u0, v1 - v0) * kView));
        for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            plot(view, u0 + (u1 - u0) * t, v0 + (v1 - v0) * t, 90, 140, 200);
        }
    };

    std::uint64_t edgesDrawn = 0;
    if (buffers.size() == 1) {
        const auto& positions = buffers.front();
        for (const auto& ib : scan.indexBuffers) {
            if (ib.maxIndex >= positions.size()) {
                continue;
            }
            for (std::uint32_t t = 0; t + 2 < ib.count; t += 3) {
                std::uint32_t idx[3];
                for (int k = 0; k < 3; ++k) {
                    const auto at = static_cast<std::size_t>(ib.dataOffset + (t + static_cast<std::uint32_t>(k)) * ib.indexSize);
                    idx[k] = ib.indexSize == 2 ? static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8))
                                               : static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8) |
                                                                            (bytes[at + 2] << 16) | (bytes[at + 3] << 24));
                }
                for (int view = 0; view < 3; ++view) {
                    line(view, positions[idx[0]], positions[idx[1]]);
                    line(view, positions[idx[1]], positions[idx[2]]);
                    line(view, positions[idx[2]], positions[idx[0]]);
                }
                ++edgesDrawn;
            }
        }
    }
    std::uint64_t points = 0;
    for (const auto& buffer : buffers) {
        for (const auto& p : buffer) {
            for (int view = 0; view < 3; ++view) {
                float u = 0, v = 0;
                project(p, view, u, v);
                plot(view, u, v, 255, 210, 60);
            }
            ++points;
        }
    }
    log << "render: " << gamecore::io::pathToUtf8(ghg.filename()) << " position_buffers=" << buffers.size()
        << " points=" << points << " triangles_drawn=" << edgesDrawn << " bounds=[" << lo[0] << ',' << lo[1] << ','
        << lo[2] << "]..[" << hi[0] << ',' << hi[1] << ',' << hi[2] << "]\n";
    return gamecore::image::writePngRgb(png, kWidth, kView, rgb);
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = readArgs(argc, argv);
    std::filesystem::path input;
    std::filesystem::path output;
    std::vector<std::filesystem::path> renders;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") {
            input = args[i + 1];
        } else if (args[i] == "--out") {
            output = args[i + 1];
        } else if (args[i] == "--render") {
            renders.push_back(args[i + 1]);
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "ghg_probe --input <extracted-directory> --out <report-directory> [--render <file.ghg>]...\n"
                  << "Read-only. Checks the .ghg container and vertex/index buffer hypotheses on every file.\n";
        return 1;
    }

    std::uint64_t files = 0, containerOk = 0, vbTotal = 0, vbInline = 0, vbUnknownType = 0, vbStreamed = 0;
    std::uint64_t positionBuffers = 0, positions = 0, wNotOne = 0, ibTotal = 0, ibFits = 0, filesWithGeometry = 0;
    std::map<std::string, std::uint64_t> failures;
    std::map<std::string, std::uint64_t> elementKinds;
    std::map<std::string, std::set<std::uint32_t>> tagVersions;
    std::map<std::string, std::uint64_t> tagCounts;
    std::map<std::uint32_t, std::uint64_t> strides;
    std::error_code ec;
    for (const auto& item : std::filesystem::recursive_directory_iterator(input, ec)) {
        if (!item.is_regular_file() || item.path().extension() != ".ghg") {
            continue;
        }
        ++files;
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!readWhole(item.path(), bytes, error)) {
            ++failures["unreadable: " + error];
            continue;
        }
        const auto container = gamecore::ghg::parseContainer(bytes);
        if (!container.valid) {
            ++failures[container.failure];
            continue;
        }
        ++containerOk;
        const auto scan = gamecore::ghg::scanObjects(bytes, container);
        for (const auto& [tag, version] : scan.tagVersions) {
            tagVersions[tag].insert(version);
        }
        for (const auto& [tag, count] : scan.tagCounts) {
            tagCounts[tag] += count;
        }
        std::uint32_t largestPositionBuffer = 0;
        for (const auto& vb : scan.vertexBuffers) {
            ++vbTotal;
            if (vb.unknownType) {
                ++vbUnknownType;
            }
            if (vb.inlineData) {
                ++vbInline;
                ++strides[vb.stride];
            } else {
                ++vbStreamed;
            }
            for (const auto& e : vb.elements) {
                ++elementKinds["usage " + std::to_string(e.usage) + " type " + std::to_string(e.type) + " stream " +
                               std::to_string(e.stream)];
            }
            std::vector<gamecore::ghg::Float3> pos;
            std::uint64_t bad = 0;
            if (gamecore::ghg::readPositions(bytes, vb, pos, bad)) {
                ++positionBuffers;
                positions += pos.size();
                wNotOne += bad;
                largestPositionBuffer = std::max<std::uint32_t>(largestPositionBuffer, vb.count);
            }
        }
        for (const auto& ib : scan.indexBuffers) {
            ++ibTotal;
            if (ib.maxIndex < largestPositionBuffer) {
                ++ibFits;
            }
        }
        if (largestPositionBuffer > 0 && !scan.indexBuffers.empty()) {
            ++filesWithGeometry;
        }
    }

    std::filesystem::create_directories(output, ec);
    std::ofstream report(output / "ghg_probe.txt", std::ios::binary | std::ios::trunc);
    report << "ghg_probe: read-only; container + vertex/index buffer hypotheses\n\n";
    report << "files: " << files << " container_ok: " << containerOk << '\n';
    for (const auto& [reason, count] : failures) {
        report << "  failure \"" << reason << "\": " << count << '\n';
    }
    report << "vertex_buffers: " << vbTotal << " inline: " << vbInline << " other_stream: " << vbStreamed
           << " unknown_element_type: " << vbUnknownType << '\n';
    report << "position_buffers (usage 0, 4 x half): " << positionBuffers << " vertices: " << positions
           << " w_not_1.0: " << wNotOne << '\n';
    report << "index_buffers: " << ibTotal << " max_index_below_largest_position_buffer: " << ibFits << '\n';
    report << "files_with_positions_and_indices: " << filesWithGeometry << '\n';
    report << "inline_strides (bytes: buffers):";
    for (const auto& [stride, count] : strides) {
        report << ' ' << stride << ':' << count;
    }
    report << "\nvertex_elements:\n";
    for (const auto& [kind, count] : elementKinds) {
        report << "  " << kind << ": " << count << '\n';
    }
    report << "object_tags (tag: count, versions):\n";
    for (const auto& [tag, count] : tagCounts) {
        report << "  " << tag << ": " << count << " v";
        for (const auto v : tagVersions[tag]) {
            report << ' ' << v;
        }
        report << '\n';
    }
    for (std::size_t i = 0; i < renders.size(); ++i) {
        auto png = output / (gamecore::io::pathToUtf8(renders[i].stem()) + ".png");
        if (!renderViews(renders[i], png, report)) {
            report << "render failed: " << gamecore::io::pathToUtf8(renders[i]) << '\n';
        }
    }
    std::cout << "files=" << files << " container_ok=" << containerOk << " vb=" << vbTotal << " inline=" << vbInline
              << " positions=" << positions << " w_not_1=" << wNotOne << " ib=" << ibTotal << " ib_fits=" << ibFits
              << '\n';
    return 0;
}
