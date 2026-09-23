#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <string>
#include <vector>

namespace gamecore::inspect {

struct GraphicsEvidence {
    std::string api;
    std::string evidence;
};

std::vector<GraphicsEvidence> scanGraphicsApis(const io::ByteSource& source, std::uint64_t maxBytes);

}  // namespace gamecore::inspect
