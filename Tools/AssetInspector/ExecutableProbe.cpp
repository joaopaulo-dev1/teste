#include "Tools/AssetInspector/ExecutableProbe.hpp"

#include <cstring>

namespace gamecore::inspect {
namespace {

void addUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    for (const auto& existing : values) {
        if (existing == value) {
            return;
        }
    }
    values.push_back(std::move(value));
}

CpuArch archFromPeMachine(std::uint16_t machine) {
    switch (machine) {
        case 0x014c:
            return CpuArch::X86;
        case 0x8664:
            return CpuArch::X64;
        case 0x01c0:
        case 0x01c2:
        case 0x01c4:
            return CpuArch::Arm;
        case 0xaa64:
            return CpuArch::Arm64;
        case 0x01f2:
            return CpuArch::PowerPC;
        default:
            return CpuArch::Unknown;
    }
}

CpuArch archFromElfMachine(std::uint16_t machine) {
    switch (machine) {
        case 3:
            return CpuArch::X86;
        case 62:
            return CpuArch::X64;
        case 40:
            return CpuArch::Arm;
        case 183:
            return CpuArch::Arm64;
        case 20:
            return CpuArch::PowerPC;
        case 21:
            return CpuArch::PowerPC64;
        default:
            return CpuArch::Unknown;
    }
}

CpuArch archFromMachCpu(std::uint32_t cputype) {
    constexpr std::uint32_t abi64 = 0x01000000u;
    const std::uint32_t base = cputype & 0xffu;
    const bool is64 = (cputype & abi64) != 0;
    switch (base) {
        case 7:
            return is64 ? CpuArch::X64 : CpuArch::X86;
        case 12:
            return is64 ? CpuArch::Arm64 : CpuArch::Arm;
        case 18:
            return is64 ? CpuArch::PowerPC64 : CpuArch::PowerPC;
        default:
            return CpuArch::Unknown;
    }
}

bool rvaToOffset(const io::ByteSource& source,
                 std::uint64_t sectionTable,
                 std::uint16_t sectionCount,
                 std::uint32_t rva,
                 std::uint64_t& fileOffset) {
    const std::uint16_t capped = sectionCount > 96 ? 96 : sectionCount;
    for (std::uint16_t i = 0; i < capped; ++i) {
        const std::uint64_t section = sectionTable + static_cast<std::uint64_t>(i) * 40ull;
        std::uint32_t virtualSize = 0;
        std::uint32_t virtualAddress = 0;
        std::uint32_t rawSize = 0;
        std::uint32_t rawPointer = 0;
        if (!io::readU32(source, section + 8, true, virtualSize) ||
            !io::readU32(source, section + 12, true, virtualAddress) ||
            !io::readU32(source, section + 16, true, rawSize) ||
            !io::readU32(source, section + 20, true, rawPointer)) {
            return false;
        }
        const std::uint32_t span = virtualSize > rawSize ? virtualSize : rawSize;
        if (rva < virtualAddress || span == 0 || rva - virtualAddress >= span) {
            continue;
        }
        const std::uint32_t delta = rva - virtualAddress;
        if (delta >= rawSize) {
            return false;
        }
        fileOffset = static_cast<std::uint64_t>(rawPointer) + delta;
        return true;
    }
    return false;
}

std::string readCString(const io::ByteSource& source, std::uint64_t offset, std::size_t cap) {
    std::string value;
    value.reserve(32);
    for (std::size_t i = 0; i < cap; ++i) {
        std::uint8_t byte = 0;
        if (!io::readExact(source, offset + i, &byte, 1)) {
            break;
        }
        if (byte == 0) {
            return value;
        }
        if (byte < 32 || byte > 126) {
            return {};
        }
        value.push_back(static_cast<char>(byte));
    }
    return {};
}

void readPeImports(const io::ByteSource& source,
                   std::uint64_t optionalOffset,
                   std::uint16_t optionalSize,
                   std::uint16_t sectionCount,
                   std::uint16_t magic,
                   std::vector<std::string>& libraries,
                   std::vector<std::string>& notes) {
    // PE32 and PE32+ put NumberOfRvaAndSizes at different offsets. Using the wrong one
    // silently drops the import table on 32-bit images.
    std::uint32_t countOffset = 0;
    std::uint32_t importOffset = 0;
    std::uint32_t minimumOptional = 0;
    if (magic == 0x20b) {
        countOffset = 108;
        importOffset = 120;
        minimumOptional = 128;
    } else if (magic == 0x10b) {
        countOffset = 92;
        importOffset = 104;
        minimumOptional = 112;
    } else {
        notes.push_back("pe optional magic is not pe32 or pe32+; imports skipped");
        return;
    }
    if (optionalSize < minimumOptional) {
        notes.push_back("pe optional header has no import directory");
        return;
    }
    std::uint32_t directoryCount = 0;
    if (!io::readU32(source, optionalOffset + countOffset, true, directoryCount)) {
        notes.push_back("pe data-directory count unreadable");
        return;
    }
    if (directoryCount < 2) {
        notes.push_back("pe import directory absent");
        return;
    }
    const std::uint32_t cappedDirs = directoryCount > 16 ? 16 : directoryCount;
    const std::uint32_t directoryBase = magic == 0x20b ? 112u : 96u;
    if (optionalSize < directoryBase + cappedDirs * 8) {
        notes.push_back("pe optional header truncated before import directory");
        return;
    }
    std::uint32_t importRva = 0;
    std::uint32_t importSize = 0;
    if (!io::readU32(source, optionalOffset + importOffset, true, importRva) ||
        !io::readU32(source, optionalOffset + importOffset + 4, true, importSize)) {
        return;
    }
    if (importRva == 0 || importSize < 20) {
        notes.push_back("pe has no import table");
        return;
    }
    const std::uint64_t sectionTable = optionalOffset + optionalSize;
    std::uint64_t descriptor = 0;
    if (!rvaToOffset(source, sectionTable, sectionCount, importRva, descriptor)) {
        notes.push_back("pe import rva did not map into a section");
        return;
    }
    for (int i = 0; i < 256; ++i) {
        std::uint32_t nameRva = 0;
        std::uint8_t header[20];
        if (!io::readExact(source, descriptor, header, 20)) {
            break;
        }
        bool allZero = true;
        for (std::uint8_t byte : header) {
            if (byte != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            break;
        }
        nameRva = static_cast<std::uint32_t>(header[12]) | (static_cast<std::uint32_t>(header[13]) << 8) |
                  (static_cast<std::uint32_t>(header[14]) << 16) | (static_cast<std::uint32_t>(header[15]) << 24);
        std::uint64_t nameOffset = 0;
        if (nameRva != 0 && rvaToOffset(source, sectionTable, sectionCount, nameRva, nameOffset)) {
            addUnique(libraries, readCString(source, nameOffset, 260));
        }
        descriptor += 20;
    }
}

ExecutableInfo probePe(const io::ByteSource& source) {
    ExecutableInfo info;
    std::uint32_t lfanew = 0;
    if (!io::readU32(source, 0x3c, true, lfanew)) {
        info.kind = BinaryKind::DosStub;
        info.notes.push_back("mz header truncated");
        return info;
    }
    if (lfanew < 0x40 || static_cast<std::uint64_t>(lfanew) > 1024ull * 1024ull) {
        info.kind = BinaryKind::DosStub;
        info.notes.push_back("mz e_lfanew is outside the inspected range");
        return info;
    }
    std::uint8_t signature[4];
    if (!io::readExact(source, lfanew, signature, 4) || signature[0] != 'P' || signature[1] != 'E' ||
        signature[2] != 0 || signature[3] != 0) {
        info.kind = BinaryKind::DosStub;
        info.notes.push_back("mz without a pe signature");
        return info;
    }
    std::uint16_t machine = 0;
    std::uint16_t sections = 0;
    std::uint16_t optionalSize = 0;
    std::uint16_t characteristics = 0;
    const std::uint64_t fileHeader = static_cast<std::uint64_t>(lfanew) + 4ull;
    if (!io::readU16(source, fileHeader, true, machine) || !io::readU16(source, fileHeader + 2, true, sections) ||
        !io::readU16(source, fileHeader + 16, true, optionalSize) ||
        !io::readU16(source, fileHeader + 18, true, characteristics)) {
        info.kind = BinaryKind::DosStub;
        info.notes.push_back("pe file header truncated");
        return info;
    }
    info.kind = BinaryKind::Pe;
    info.arch = archFromPeMachine(machine);
    info.littleEndian = true;
    info.endianKnown = true;
    if ((characteristics & 0x2000) != 0) {
        info.notes.push_back("pe dll");
    } else {
        info.notes.push_back("pe image");
    }
    if (info.arch == CpuArch::Unknown) {
        info.notes.push_back("pe machine code is not one of x86/x64/arm/arm64/powerpc");
    }
    const std::uint64_t optional = fileHeader + 20ull;
    std::uint16_t magic = 0;
    if (io::readU16(source, optional, true, magic)) {
        if (magic == 0x10b) {
            info.notes.push_back("pe32");
        } else if (magic == 0x20b) {
            info.notes.push_back("pe32+");
        }
    }
    readPeImports(source, optional, optionalSize, sections, magic, info.libraries, info.notes);
    return info;
}

bool mapElfVirtual(const io::ByteSource& source,
                   std::uint64_t phoff,
                   std::uint16_t phentsize,
                   std::uint16_t phnum,
                   bool elf64,
                   bool little,
                   std::uint64_t virtualAddress,
                   std::uint64_t& fileOffset) {
    const std::uint16_t capped = phnum > 64 ? 64 : phnum;
    if (phentsize < (elf64 ? 56 : 32)) {
        return false;
    }
    for (std::uint16_t i = 0; i < capped; ++i) {
        const std::uint64_t ph = phoff + static_cast<std::uint64_t>(i) * phentsize;
        std::uint32_t type = 0;
        if (!io::readU32(source, ph, little, type) || type != 1) {
            continue;
        }
        std::uint64_t offset = 0;
        std::uint64_t vaddr = 0;
        std::uint64_t filesz = 0;
        if (elf64) {
            if (!io::readU64(source, ph + 8, little, offset) || !io::readU64(source, ph + 16, little, vaddr) ||
                !io::readU64(source, ph + 32, little, filesz)) {
                return false;
            }
        } else {
            std::uint32_t offset32 = 0;
            std::uint32_t vaddr32 = 0;
            std::uint32_t filesz32 = 0;
            if (!io::readU32(source, ph + 4, little, offset32) || !io::readU32(source, ph + 8, little, vaddr32) ||
                !io::readU32(source, ph + 16, little, filesz32)) {
                return false;
            }
            offset = offset32;
            vaddr = vaddr32;
            filesz = filesz32;
        }
        if (virtualAddress < vaddr || virtualAddress - vaddr >= filesz) {
            continue;
        }
        fileOffset = offset + (virtualAddress - vaddr);
        return true;
    }
    return false;
}

void readElfNeeded(const io::ByteSource& source, bool elf64, bool little, std::vector<std::string>& libraries,
                   std::vector<std::string>& notes) {
    std::uint64_t phoff = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    if (elf64) {
        if (!io::readU64(source, 32, little, phoff) || !io::readU16(source, 54, little, phentsize) ||
            !io::readU16(source, 56, little, phnum)) {
            return;
        }
    } else {
        std::uint32_t phoff32 = 0;
        if (!io::readU32(source, 28, little, phoff32) || !io::readU16(source, 42, little, phentsize) ||
            !io::readU16(source, 44, little, phnum)) {
            return;
        }
        phoff = phoff32;
    }
    if (phoff == 0 || phnum == 0) {
        notes.push_back("elf has no program headers");
        return;
    }
    const std::uint16_t capped = phnum > 64 ? 64 : phnum;
    bool foundDynamic = false;
    for (std::uint16_t i = 0; i < capped; ++i) {
        const std::uint64_t ph = phoff + static_cast<std::uint64_t>(i) * phentsize;
        std::uint32_t type = 0;
        if (!io::readU32(source, ph, little, type) || type != 2) {
            continue;
        }
        foundDynamic = true;
        std::uint64_t dynOffset = 0;
        std::uint64_t dynSize = 0;
        if (elf64) {
            std::uint64_t dynVaddr = 0;
            if (!io::readU64(source, ph + 8, little, dynOffset) || !io::readU64(source, ph + 16, little, dynVaddr) ||
                !io::readU64(source, ph + 32, little, dynSize)) {
                break;
            }
            std::uint64_t mapped = 0;
            if (mapElfVirtual(source, phoff, phentsize, phnum, true, little, dynVaddr, mapped)) {
                dynOffset = mapped;
            }
        } else {
            std::uint32_t dynOffset32 = 0;
            std::uint32_t dynVaddr = 0;
            std::uint32_t dynSize32 = 0;
            if (!io::readU32(source, ph + 4, little, dynOffset32) ||
                !io::readU32(source, ph + 8, little, dynVaddr) ||
                !io::readU32(source, ph + 16, little, dynSize32)) {
                break;
            }
            dynOffset = dynOffset32;
            dynSize = dynSize32;
            std::uint64_t mapped = 0;
            if (mapElfVirtual(source, phoff, phentsize, phnum, false, little, dynVaddr, mapped)) {
                dynOffset = mapped;
            }
        }
        const std::uint64_t stride = elf64 ? 16ull : 8ull;
        if (dynSize < stride || dynSize / stride > 4096) {
            notes.push_back("elf dynamic table size is not usable");
            break;
        }
        std::uint64_t strtab = 0;
        std::uint64_t strsz = 0;
        std::vector<std::uint64_t> needed;
        const std::uint64_t count = dynSize / stride;
        for (std::uint64_t entry = 0; entry < count; ++entry) {
            const std::uint64_t at = dynOffset + entry * stride;
            std::int64_t tag = 0;
            std::uint64_t value = 0;
            if (elf64) {
                std::uint64_t tagRaw = 0;
                if (!io::readU64(source, at, little, tagRaw) || !io::readU64(source, at + 8, little, value)) {
                    break;
                }
                tag = static_cast<std::int64_t>(tagRaw);
            } else {
                std::uint32_t tagRaw = 0;
                std::uint32_t value32 = 0;
                if (!io::readU32(source, at, little, tagRaw) || !io::readU32(source, at + 4, little, value32)) {
                    break;
                }
                tag = static_cast<std::int32_t>(tagRaw);
                value = value32;
            }
            if (tag == 0) {
                break;
            }
            if (tag == 1) {
                needed.push_back(value);
            } else if (tag == 5) {
                strtab = value;
            } else if (tag == 10) {
                strsz = value;
            }
        }
        std::uint64_t strOffset = 0;
        if (strtab == 0 || !mapElfVirtual(source, phoff, phentsize, phnum, elf64, little, strtab, strOffset)) {
            notes.push_back("elf dt_strtab did not map into a load segment");
            break;
        }
        if (strsz > 1u << 20) {
            strsz = 1u << 20;
        }
        for (std::uint64_t nameOffset : needed) {
            if (strsz != 0 && nameOffset >= strsz) {
                continue;
            }
            addUnique(libraries, readCString(source, strOffset + nameOffset, 260));
        }
        break;
    }
    if (!foundDynamic) {
        notes.push_back("elf has no pt_dynamic");
    }
}

ExecutableInfo probeElf(const io::ByteSource& source) {
    ExecutableInfo info;
    std::uint8_t ident[6];
    if (!io::readExact(source, 0, ident, 6)) {
        return info;
    }
    const bool elf64 = ident[4] == 2;
    const bool elf32 = ident[4] == 1;
    if (!elf64 && !elf32) {
        info.notes.push_back("elf class is neither 32 nor 64");
        return info;
    }
    if (ident[5] != 1 && ident[5] != 2) {
        info.notes.push_back("elf data encoding is neither little nor big endian");
        return info;
    }
    const bool little = ident[5] == 1;
    std::uint16_t machine = 0;
    if (!io::readU16(source, 18, little, machine)) {
        info.notes.push_back("elf header truncated before e_machine");
        return info;
    }
    info.kind = BinaryKind::Elf;
    info.arch = archFromElfMachine(machine);
    info.littleEndian = little;
    info.endianKnown = true;
    info.notes.push_back(elf64 ? "elf64" : "elf32");
    if (info.arch == CpuArch::Unknown) {
        info.notes.push_back("elf e_machine is not one of x86/x64/arm/arm64/powerpc");
    }
    readElfNeeded(source, elf64, little, info.libraries, info.notes);
    return info;
}

void readMachLibraries(const io::ByteSource& source,
                       std::uint64_t commandOffset,
                       std::uint32_t commandCount,
                       std::uint32_t commandBytes,
                       bool little,
                       std::vector<std::string>& libraries,
                       std::vector<std::string>& notes) {
    if (commandBytes > 8u << 20) {
        notes.push_back("mach-o sizeofcmds is unreasonably large");
        return;
    }
    const std::uint64_t regionEnd = commandOffset + commandBytes;
    if (regionEnd < commandOffset || regionEnd > source.size) {
        notes.push_back("mach-o load-command region exceeds the file");
        return;
    }
    std::uint64_t cursor = commandOffset;
    const std::uint32_t capped = commandCount > 4096 ? 4096 : commandCount;
    for (std::uint32_t i = 0; i < capped && cursor + 8 <= regionEnd; ++i) {
        std::uint32_t cmd = 0;
        std::uint32_t cmdsize = 0;
        if (!io::readU32(source, cursor, little, cmd) || !io::readU32(source, cursor + 4, little, cmdsize)) {
            break;
        }
        if (cmdsize < 8 || cursor + cmdsize > regionEnd) {
            notes.push_back("mach-o load command size is invalid");
            break;
        }
        const std::uint32_t opcode = cmd & 0x7fffffffu;
        const bool loadDylib = opcode == 0x0c || opcode == 0x18 || opcode == 0x1f || opcode == 0x23;
        if (loadDylib && cmdsize >= 24) {
            std::uint32_t nameOffset = 0;
            if (io::readU32(source, cursor + 8, little, nameOffset) && nameOffset < cmdsize) {
                addUnique(libraries, readCString(source, cursor + nameOffset, cmdsize - nameOffset));
            }
        }
        cursor += cmdsize;
    }
}

ExecutableInfo probeMach(const io::ByteSource& source, bool little, bool is64) {
    ExecutableInfo info;
    info.kind = BinaryKind::MachO;
    info.littleEndian = little;
    info.endianKnown = true;
    std::uint32_t cputype = 0;
    std::uint32_t ncmds = 0;
    std::uint32_t sizeofcmds = 0;
    if (!io::readU32(source, 4, little, cputype) || !io::readU32(source, 16, little, ncmds) ||
        !io::readU32(source, 20, little, sizeofcmds)) {
        info.notes.push_back("mach-o header truncated");
        return info;
    }
    info.arch = archFromMachCpu(cputype);
    info.notes.push_back(is64 ? "mach-o 64" : "mach-o 32");
    if (info.arch == CpuArch::Unknown) {
        info.notes.push_back("mach-o cputype is not one of x86/x64/arm/arm64/powerpc");
    }
    const std::uint64_t commands = is64 ? 32ull : 28ull;
    readMachLibraries(source, commands, ncmds, sizeofcmds, little, info.libraries, info.notes);
    return info;
}

ExecutableInfo probeFat(const io::ByteSource& source, bool little) {
    ExecutableInfo info;
    info.kind = BinaryKind::FatMachO;
    info.arch = CpuArch::Universal;
    info.littleEndian = little;
    info.endianKnown = true;
    std::uint32_t count = 0;
    if (!io::readU32(source, 4, little, count)) {
        info.notes.push_back("fat mach-o header truncated");
        return info;
    }
    if (count == 0 || count > 8) {
        info.notes.push_back("fat mach-o architecture count is outside 1..8");
        return info;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t cputype = 0;
        const std::uint64_t arch = 8ull + static_cast<std::uint64_t>(i) * 20ull;
        if (!io::readU32(source, arch, little, cputype)) {
            break;
        }
        info.fatArchitectures.push_back(archFromMachCpu(cputype));
    }
    info.notes.push_back("fat mach-o; slices listed separately");
    return info;
}

}  // namespace

const char* toString(BinaryKind kind) {
    switch (kind) {
        case BinaryKind::None:
            return "none";
        case BinaryKind::DosStub:
            return "dos-stub";
        case BinaryKind::Pe:
            return "pe";
        case BinaryKind::Elf:
            return "elf";
        case BinaryKind::MachO:
            return "mach-o";
        case BinaryKind::FatMachO:
            return "fat-mach-o";
    }
    return "none";
}

const char* toString(CpuArch arch) {
    switch (arch) {
        case CpuArch::Unknown:
            return "unknown";
        case CpuArch::X86:
            return "x86";
        case CpuArch::X64:
            return "x64";
        case CpuArch::Arm:
            return "arm";
        case CpuArch::Arm64:
            return "arm64";
        case CpuArch::PowerPC:
            return "powerpc";
        case CpuArch::PowerPC64:
            return "powerpc64";
        case CpuArch::Universal:
            return "universal";
    }
    return "unknown";
}

ExecutableInfo probeExecutable(const io::ByteSource& source) {
    ExecutableInfo info;
    if (source.size < 4) {
        return info;
    }
    std::uint8_t magic[4];
    if (!io::readExact(source, 0, magic, 4)) {
        return info;
    }
    if (magic[0] == 'M' && magic[1] == 'Z') {
        return probePe(source);
    }
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        return probeElf(source);
    }
    if (magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) {
        return probeMach(source, true, true);
    }
    if (magic[0] == 0xce && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) {
        return probeMach(source, true, false);
    }
    if (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xcf) {
        return probeMach(source, false, true);
    }
    if (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xce) {
        return probeMach(source, false, false);
    }
    if (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba && magic[3] == 0xbe) {
        return probeFat(source, false);
    }
    if (magic[0] == 0xbe && magic[1] == 0xba && magic[2] == 0xfe && magic[3] == 0xca) {
        return probeFat(source, true);
    }
    return info;
}

}  // namespace gamecore::inspect
