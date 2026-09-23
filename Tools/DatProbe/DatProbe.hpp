#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::dat {

// Each candidate is one arithmetic reading of header field 0 as a table offset.
// None is assumed correct; a candidate is only accepted when the data agrees.
struct OffsetCandidate {
    std::string transform;
    std::uint64_t offset = 0;
    bool inFile = false;
    bool tableFits = false;
    bool endsAtEof = false;
};

struct DatProbeResult {
    std::uint64_t fileSize = 0;
    bool headerRead = false;
    std::uint32_t field0 = 0;
    std::uint32_t field1 = 0;
    bool headerPaddingZero = false;
    std::uint64_t zeroPaddingBytes = 0;
    std::vector<OffsetCandidate> candidates;
    std::string matchedTransform;
    std::uint64_t tableOffset = 0;
    std::uint64_t tableSize = 0;
    double tableEntropy = 0.0;
    std::vector<std::uint32_t> tableLeadWords;
    std::string tableHead;
    std::string fileTail;
};

std::vector<OffsetCandidate> offsetCandidates(std::uint32_t field0, std::uint32_t field1, std::uint64_t fileSize);

DatProbeResult probeDat(const io::ByteSource& source);

}  // namespace gamecore::dat
