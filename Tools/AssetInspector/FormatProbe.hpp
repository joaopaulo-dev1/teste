#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <string>

namespace gamecore::inspect {

struct FormatHit {
    std::string name = "unknown";
    std::string family = "unknown";
    std::string note;
    bool recognized = false;
};

FormatHit probeFormat(const io::ByteSource& source);

double shannonEntropy(const io::ByteSource& source, std::uint64_t maxBytes);

std::string hexAsciiDump(const io::ByteSource& source, std::size_t maxBytes);

}  // namespace gamecore::inspect
