#include "GameCore/AssetSystem/ByteSource.hpp"
#include "GameCore/AssetSystem/Intermediate.hpp"
#include "GameCore/AssetSystem/ReadOnlyFile.hpp"
#include "Tools/AssetInspector/ExecutableProbe.hpp"
#include "Tools/AssetInspector/FormatProbe.hpp"
#include "Tools/AssetInspector/GraphicsScan.hpp"
#include "Tools/AssetInspector/Inspector.hpp"
#include "Tools/AssetInspector/ZipLister.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void check(bool condition, const char* text, int line) {
    if (!condition) {
        std::cerr << "FAIL " << line << ": " << text << std::endl;
        ++g_failed;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void writeU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

gamecore::io::ByteSource asSource(const std::vector<std::uint8_t>& bytes, gamecore::io::MemoryBytes& storage) {
    storage.data = bytes.data();
    storage.size = static_cast<std::uint64_t>(bytes.size());
    return gamecore::io::sourceFromMemory(storage);
}

std::vector<std::uint8_t> makePe64ImportingD3d11() {
    std::vector<std::uint8_t> bytes(0x600, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    writeU32(bytes, 0x3c, 0x80);
    bytes[0x80] = 'P';
    bytes[0x81] = 'E';
    writeU16(bytes, 0x84, 0x8664);
    writeU16(bytes, 0x86, 1);
    writeU16(bytes, 0x94, 240);
    writeU16(bytes, 0x96, 0x22);
    writeU16(bytes, 0x98, 0x20b);
    writeU32(bytes, 0x104, 16);
    writeU32(bytes, 0x110, 0x1000);
    writeU32(bytes, 0x114, 40);
    const char sectionName[] = ".rdata";
    std::memcpy(&bytes[0x188], sectionName, sizeof(sectionName));
    writeU32(bytes, 0x190, 0x40);
    writeU32(bytes, 0x194, 0x1000);
    writeU32(bytes, 0x198, 0x200);
    writeU32(bytes, 0x19c, 0x400);
    writeU32(bytes, 0x40c, 0x1028);
    const char dll[] = "d3d11.dll";
    std::memcpy(&bytes[0x428], dll, sizeof(dll));
    return bytes;
}

std::vector<std::uint8_t> makeElf64Aarch64() {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    bytes[18] = static_cast<std::uint8_t>(183);
    bytes[19] = 0;
    return bytes;
}

std::vector<std::uint8_t> makeMachOArm64Metal() {
    const char* path = "/System/Library/Frameworks/Metal.framework/Metal";
    const std::size_t pathLen = std::strlen(path) + 1;
    const std::uint32_t cmdsize = static_cast<std::uint32_t>((24u + pathLen + 7u) & ~7u);
    std::vector<std::uint8_t> bytes(32u + cmdsize, 0);
    bytes[0] = 0xcf;
    bytes[1] = 0xfa;
    bytes[2] = 0xed;
    bytes[3] = 0xfe;
    bytes[4] = 0x0c;
    bytes[7] = 0x01;
    bytes[16] = 1;
    bytes[20] = static_cast<std::uint8_t>(cmdsize & 0xff);
    bytes[21] = static_cast<std::uint8_t>((cmdsize >> 8) & 0xff);
    bytes[32] = 0x0c;
    bytes[36] = static_cast<std::uint8_t>(cmdsize & 0xff);
    bytes[37] = static_cast<std::uint8_t>((cmdsize >> 8) & 0xff);
    bytes[40] = 24;
    std::memcpy(&bytes[56], path, pathLen);
    return bytes;
}

std::vector<std::uint8_t> makeFatMachO() {
    std::vector<std::uint8_t> bytes(48, 0);
    bytes[0] = 0xca;
    bytes[1] = 0xfe;
    bytes[2] = 0xba;
    bytes[3] = 0xbe;
    bytes[7] = 2;
    bytes[8] = 0x01;
    bytes[11] = 0x0c;
    bytes[28] = 0x01;
    bytes[31] = 0x07;
    return bytes;
}

struct ZipItem {
    std::string name;
    std::string data;
    std::uint16_t flags = 0;
};

std::vector<std::uint8_t> makeZip(const std::vector<ZipItem>& items) {
    std::vector<std::uint8_t> local;
    struct Meta {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        std::string name;
        std::uint16_t flags = 0;
    };
    std::vector<Meta> metas;
    for (const ZipItem& item : items) {
        Meta meta;
        meta.offset = static_cast<std::uint32_t>(local.size());
        meta.size = static_cast<std::uint32_t>(item.data.size());
        meta.name = item.name;
        meta.flags = item.flags;
        appendU32(local, 0x04034b50u);
        appendU16(local, 20);
        appendU16(local, item.flags);
        appendU16(local, 0);
        appendU16(local, 0);
        appendU16(local, 0);
        appendU32(local, 0);
        appendU32(local, meta.size);
        appendU32(local, meta.size);
        appendU16(local, static_cast<std::uint16_t>(item.name.size()));
        appendU16(local, 0);
        local.insert(local.end(), item.name.begin(), item.name.end());
        local.insert(local.end(), item.data.begin(), item.data.end());
        metas.push_back(meta);
    }
    const auto cdOffset = static_cast<std::uint32_t>(local.size());
    std::vector<std::uint8_t> central;
    for (const Meta& meta : metas) {
        appendU32(central, 0x02014b50u);
        appendU16(central, 20);
        appendU16(central, 20);
        appendU16(central, meta.flags);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU32(central, 0);
        appendU32(central, meta.size);
        appendU32(central, meta.size);
        appendU16(central, static_cast<std::uint16_t>(meta.name.size()));
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU32(central, 0);
        appendU32(central, meta.offset);
        central.insert(central.end(), meta.name.begin(), meta.name.end());
    }
    std::vector<std::uint8_t> out = local;
    out.insert(out.end(), central.begin(), central.end());
    appendU32(out, 0x06054b50u);
    appendU16(out, 0);
    appendU16(out, 0);
    appendU16(out, static_cast<std::uint16_t>(items.size()));
    appendU16(out, static_cast<std::uint16_t>(items.size()));
    appendU32(out, static_cast<std::uint32_t>(central.size()));
    appendU32(out, cdOffset);
    appendU16(out, 0);
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& needle) {
    for (const auto& value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

void testExecutables() {
    const auto pe = makePe64ImportingD3d11();
    gamecore::io::MemoryBytes storage;
    const auto peInfo = gamecore::inspect::probeExecutable(asSource(pe, storage));
    CHECK(peInfo.kind == gamecore::inspect::BinaryKind::Pe);
    CHECK(peInfo.arch == gamecore::inspect::CpuArch::X64);
    CHECK(contains(peInfo.libraries, "d3d11.dll"));

    const auto graphics = gamecore::inspect::scanGraphicsApis(asSource(pe, storage), pe.size());
    bool sawDx11 = false;
    for (const auto& item : graphics) {
        if (item.api == "directx11") {
            sawDx11 = true;
        }
    }
    CHECK(sawDx11);

    const auto elf = makeElf64Aarch64();
    const auto elfInfo = gamecore::inspect::probeExecutable(asSource(elf, storage));
    CHECK(elfInfo.kind == gamecore::inspect::BinaryKind::Elf);
    CHECK(elfInfo.arch == gamecore::inspect::CpuArch::Arm64);

    const auto macho = makeMachOArm64Metal();
    const auto machoInfo = gamecore::inspect::probeExecutable(asSource(macho, storage));
    CHECK(machoInfo.kind == gamecore::inspect::BinaryKind::MachO);
    CHECK(machoInfo.arch == gamecore::inspect::CpuArch::Arm64);
    CHECK(!machoInfo.libraries.empty());
    const auto metalGraphics = gamecore::inspect::scanGraphicsApis(asSource(macho, storage), macho.size());
    bool sawMetal = false;
    for (const auto& item : metalGraphics) {
        if (item.api == "metal") {
            sawMetal = true;
        }
    }
    CHECK(sawMetal);

    const auto fat = makeFatMachO();
    const auto fatInfo = gamecore::inspect::probeExecutable(asSource(fat, storage));
    CHECK(fatInfo.kind == gamecore::inspect::BinaryKind::FatMachO);
    CHECK(fatInfo.arch == gamecore::inspect::CpuArch::Universal);
    CHECK(fatInfo.fatArchitectures.size() == 2);
    CHECK(fatInfo.fatArchitectures[0] == gamecore::inspect::CpuArch::Arm64);
    CHECK(fatInfo.fatArchitectures[1] == gamecore::inspect::CpuArch::X64);
}

void testFormats() {
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 0};
    std::vector<std::uint8_t> pngBytes(png, png + sizeof(png));
    gamecore::io::MemoryBytes storage;
    const auto pngHit = gamecore::inspect::probeFormat(asSource(pngBytes, storage));
    CHECK(pngHit.recognized);
    CHECK(pngHit.name == "png");
    CHECK(pngHit.family == "texture");

    std::vector<std::uint8_t> unknown(48, 0xab);
    unknown[0] = 'Q';
    unknown[1] = 'Z';
    unknown[2] = '9';
    unknown[3] = 0x01;
    const auto unknownHit = gamecore::inspect::probeFormat(asSource(unknown, storage));
    CHECK(!unknownHit.recognized);
    const auto dump = gamecore::inspect::hexAsciiDump(asSource(unknown, storage), 16);
    CHECK(dump.find("51 5a 39 01") != std::string::npos);
    CHECK(dump.find("QZ9.") != std::string::npos);
    const double entropy = gamecore::inspect::shannonEntropy(asSource(unknown, storage), 64);
    CHECK(entropy > 0.0);
    CHECK(entropy < 2.0);

    const auto zipBytes = makeZip({
        {"textures/hero.dds", "DDS "},
        {"scripts/boot.lua", "print(1)"},
        {"secret.bin", "nope", 1},
    });
    const auto zipHit = gamecore::inspect::probeFormat(asSource(zipBytes, storage));
    CHECK(zipHit.name == "zip");
    const auto zip = gamecore::inspect::summarizeZip(asSource(zipBytes, storage));
    if (!zip.parsed || zip.entryCount != 3) {
        std::cerr << "zip parsed=" << zip.parsed << " count=" << zip.entryCount << " note=" << zip.note << std::endl;
    }
    CHECK(zip.parsed);
    CHECK(zip.entryCount == 3);
    CHECK(zip.encryptedCount == 1);
    const auto extensionCount = [&](const char* key) {
        const auto it = zip.extensions.find(key);
        return it == zip.extensions.end() ? 0ull : it->second;
    };
    CHECK(extensionCount(".dds") == 1);
    CHECK(extensionCount(".lua") == 1);
    CHECK(extensionCount(".bin") == 1);
}

void testIntermediate() {
    gamecore::ir::Mesh mesh;
    mesh.positions = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    mesh.indices = {0, 1, 2};
    std::string reason;
    CHECK(mesh.validate(reason));

    mesh.indices = {0, 1, 9};
    CHECK(!mesh.validate(reason));

    gamecore::ir::Skeleton skeleton;
    skeleton.jointNames = {"root", "child"};
    skeleton.parents = {-1, 0};
    CHECK(skeleton.validate(reason));
    skeleton.parents = {1, 0};
    CHECK(!skeleton.validate(reason));

    gamecore::ir::Scene scene;
    scene.meshes.push_back(gamecore::ir::Mesh{});
    scene.meshes[0].positions = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    scene.meshes[0].indices = {0, 1, 2};
    scene.entities.push_back(gamecore::ir::Entity{});
    scene.entities[0].mesh = 0;
    CHECK(scene.validate(reason));
    scene.entities[0].mesh = 3;
    CHECK(!scene.validate(reason));
}

void testReadOnlyRoundTrip() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("gamecore-inspector-" + std::to_string(stamp));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "input");
    const auto source = root / "input" / "sample.png";
    const auto unknown = root / "input" / "mystery.bin";
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    {
        std::ofstream pngOut(source, std::ios::binary);
        pngOut.write(reinterpret_cast<const char*>(png), sizeof(png));
        std::ofstream mystery(unknown, std::ios::binary);
        // 16 of each byte is uniform over 256 symbols, so the 64 KiB window
        // measures entropy near 8. The first two bytes stay visible in the hex log.
        std::string payload(4096, '\0');
        for (int i = 0; i < 4096; ++i) {
            payload[static_cast<std::size_t>(i)] = static_cast<char>(i & 0xff);
        }
        payload[0] = static_cast<char>(0xab);
        payload[1] = static_cast<char>(0xab);
        mystery.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    std::vector<std::uint8_t> before(sizeof(png));
    {
        gamecore::io::ReadOnlyFile file;
        std::string error;
        CHECK(gamecore::io::ReadOnlyFile::open(source, file, error));
        CHECK(file.readAt(0, before.data(), before.size()) == before.size());
        const auto copy = root / "copied.png";
        CHECK(gamecore::io::ReadOnlyFile::copyTo(source, copy, error));
        gamecore::io::ReadOnlyFile copied;
        CHECK(gamecore::io::ReadOnlyFile::open(copy, copied, error));
        std::vector<std::uint8_t> after(sizeof(png));
        CHECK(copied.readAt(0, after.data(), after.size()) == after.size());
        CHECK(before == after);
    }

    gamecore::inspect::InspectOptions options;
    options.input = root / "input";
    options.outputDirectory = root / "out";
    options.copyFirst = true;
    options.writeJson = true;
    gamecore::inspect::InspectResult result;
    std::string error;
    CHECK(gamecore::inspect::inspectTree(options, result, error));
    CHECK(error.empty());
    CHECK(result.files.size() == 2);
    CHECK(gamecore::inspect::writeReports(options, result, error));

    std::vector<std::uint8_t> afterInspect(sizeof(png));
    {
        gamecore::io::ReadOnlyFile file;
        CHECK(gamecore::io::ReadOnlyFile::open(source, file, error));
        CHECK(file.readAt(0, afterInspect.data(), afterInspect.size()) == afterInspect.size());
    }
    CHECK(before == afterInspect);

    bool sawPng = false;
    bool sawUnknown = false;
    for (const auto& file : result.files) {
        if (file.formatName == "png") {
            sawPng = true;
            CHECK(file.recognized);
            CHECK(file.inspectedPath.find("copies") != std::string::npos);
        }
        if (!file.recognized) {
            sawUnknown = true;
            CHECK(file.headerHex.find("ab ab") != std::string::npos);
            bool highEntropyNote = false;
            for (const auto& note : file.notes) {
                if (note.find("high entropy") != std::string::npos) {
                    highEntropyNote = true;
                }
            }
            CHECK(highEntropyNote);
        }
    }
    CHECK(sawPng);
    CHECK(sawUnknown);

    std::ifstream unknownLog(root / "out" / "unknown.log");
    std::string log((std::istreambuf_iterator<char>(unknownLog)), std::istreambuf_iterator<char>());
    CHECK(log.find("mystery.bin") != std::string::npos);
    CHECK(log.find("ab ab") != std::string::npos);
    std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
    try {
        testExecutables();
        testFormats();
        testIntermediate();
        testReadOnlyRoundTrip();
    } catch (const std::exception& ex) {
        std::cerr << "exception: " << ex.what() << std::endl;
        return 1;
    }
    if (g_failed != 0) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "inspector tests passed\n";
    return 0;
}
