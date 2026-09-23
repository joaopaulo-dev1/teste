#include "Tools/DatProbe/DatProbe.hpp"

#include "Tools/AssetInspector/FormatProbe.hpp"

namespace gamecore::dat {
namespace {

OffsetCandidate evaluate(const char* name, std::uint64_t offset, std::uint32_t field1, std::uint64_t fileSize) {
    OffsetCandidate candidate;
    candidate.transform = name;
    candidate.offset = offset;
    candidate.inFile = offset >= 8 && offset < fileSize;
    candidate.tableFits = candidate.inFile && static_cast<std::uint64_t>(field1) <= fileSize - offset;
    candidate.endsAtEof = candidate.tableFits && offset + field1 == fileSize;
    return candidate;
}

std::uint64_t countLeadingZeroPadding(const io::ByteSource& source, std::uint64_t start, std::uint64_t cap) {
    std::uint8_t buffer[4096];
    std::uint64_t zeros = 0;
    while (zeros < cap) {
        const std::uint64_t want = cap - zeros < sizeof(buffer) ? cap - zeros : sizeof(buffer);
        if (!io::readExact(source, start + zeros, buffer, static_cast<std::size_t>(want))) {
            break;
        }
        for (std::uint64_t i = 0; i < want; ++i) {
            if (buffer[i] != 0) {
                return zeros + i;
            }
        }
        zeros += want;
    }
    return zeros;
}

}  // namespace

std::vector<OffsetCandidate> offsetCandidates(std::uint32_t field0, std::uint32_t field1, std::uint64_t fileSize) {
    const std::uint32_t inverted = ~field0;
    const std::uint64_t negated = (0x100000000ull - field0) & 0xffffffffull;
    std::vector<OffsetCandidate> candidates;
    candidates.reserve(8);
    candidates.push_back(evaluate("raw", field0, field1, fileSize));
    candidates.push_back(evaluate("negated", negated, field1, fileSize));
    candidates.push_back(evaluate("inverted", inverted, field1, fileSize));
    candidates.push_back(evaluate("raw<<8", static_cast<std::uint64_t>(field0) << 8, field1, fileSize));
    candidates.push_back(evaluate("negated<<8", negated << 8, field1, fileSize));
    // negated<<8 already covers (inverted<<8)+0x100, since -v == ~v + 1 in 32-bit arithmetic.
    candidates.push_back(evaluate("inverted<<8", static_cast<std::uint64_t>(inverted) << 8, field1, fileSize));
    candidates.push_back(evaluate("size-field1", fileSize >= field1 ? fileSize - field1 : 0, field1, fileSize));
    return candidates;
}

DatProbeResult probeDat(const io::ByteSource& source) {
    DatProbeResult result;
    result.fileSize = source.size;
    if (!io::readU32(source, 0, true, result.field0) || !io::readU32(source, 4, true, result.field1)) {
        return result;
    }
    result.headerRead = true;
    result.zeroPaddingBytes = countLeadingZeroPadding(source, 8, 1u << 16);
    result.headerPaddingZero = result.zeroPaddingBytes >= 56;
    result.candidates = offsetCandidates(result.field0, result.field1, source.size);

    // "size-field1" matches by construction, so it is a reference line and never the answer.
    for (const OffsetCandidate& candidate : result.candidates) {
        if (candidate.endsAtEof && candidate.transform != "size-field1") {
            result.matchedTransform = candidate.transform;
            result.tableOffset = candidate.offset;
            result.tableSize = result.field1;
            break;
        }
    }

    const std::uint64_t tailSize = source.size < 128 ? source.size : 128;
    io::SliceBytes tailStorage;
    const io::ByteSource tail = io::sliceOf(source, source.size - tailSize, tailSize, tailStorage);
    result.fileTail = inspect::hexAsciiDump(tail, static_cast<std::size_t>(tailSize));

    if (result.matchedTransform.empty()) {
        return result;
    }
    io::SliceBytes tableStorage;
    const io::ByteSource table = io::sliceOf(source, result.tableOffset, result.tableSize, tableStorage);
    result.tableEntropy = inspect::shannonEntropy(table, 1ull << 16);
    result.tableHead = inspect::hexAsciiDump(table, 256);
    for (std::uint64_t i = 0; i < 16 && (i + 1) * 4 <= table.size; ++i) {
        std::uint32_t word = 0;
        if (!io::readU32(table, i * 4, true, word)) {
            break;
        }
        result.tableLeadWords.push_back(word);
    }
    return result;
}

}  // namespace gamecore::dat
