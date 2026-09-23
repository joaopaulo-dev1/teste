#include "Tools/DatProbe/DatIndex.hpp"

namespace gamecore::dat {
namespace {

std::uint32_t loadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

bool readDatIndex(const io::ByteSource& file,
                  std::uint64_t tableOffset,
                  std::uint64_t tableSize,
                  DatIndex& out,
                  std::string& error) {
    out = {};
    constexpr std::uint64_t kPreamble = 8;
    constexpr std::uint64_t kRecord = 16;
    if (tableSize < kPreamble || tableSize > (64ull << 20)) {
        error = "table size outside 8 B .. 64 MiB";
        return false;
    }
    std::vector<std::uint8_t> table(static_cast<std::size_t>(tableSize));
    if (!io::readExact(file, tableOffset, table.data(), table.size())) {
        error = "table unreadable";
        return false;
    }
    out.preamble = loadU32(table.data());
    const std::uint32_t count = loadU32(table.data() + 4);
    const std::uint64_t recordBytes = static_cast<std::uint64_t>(count) * kRecord;
    if (kPreamble + recordBytes > tableSize) {
        error = "declared count * 16 does not fit in the table";
        return false;
    }
    out.entries.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* record = table.data() + kPreamble + static_cast<std::size_t>(i) * kRecord;
        out.entries[i].a = loadU32(record);
        out.entries[i].b = loadU32(record + 4);
        out.entries[i].c = loadU32(record + 8);
        out.entries[i].flags = loadU32(record + 12);
    }
    const auto trailingStart = static_cast<std::size_t>(kPreamble + recordBytes);
    out.trailing.assign(table.begin() + static_cast<std::ptrdiff_t>(trailingStart), table.end());
    error.clear();
    return true;
}

}  // namespace gamecore::dat
