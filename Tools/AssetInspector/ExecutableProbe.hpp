#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <string>
#include <vector>

namespace gamecore::inspect {

enum class BinaryKind {
    None,
    DosStub,
    Pe,
    Elf,
    MachO,
    FatMachO,
};

enum class CpuArch {
    Unknown,
    X86,
    X64,
    Arm,
    Arm64,
    PowerPC,
    PowerPC64,
    Universal,
};

struct ExecutableInfo {
    BinaryKind kind = BinaryKind::None;
    CpuArch arch = CpuArch::Unknown;
    bool littleEndian = true;
    bool endianKnown = false;
    std::vector<std::string> libraries;
    std::vector<std::string> notes;
    std::vector<CpuArch> fatArchitectures;
};

const char* toString(BinaryKind kind);
const char* toString(CpuArch arch);

ExecutableInfo probeExecutable(const io::ByteSource& source);

}  // namespace gamecore::inspect
