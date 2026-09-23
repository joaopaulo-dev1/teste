#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace gamecore::ghg {

// Container layout confirmed on all 502 retail .ghg files:
//   u32be resh_size; u8 resh[resh_size];                      ("4CC." "RESH" "RESH" ... dependency list)
//   u32be nu20_size; u32be 1; "02UN"; u8 objects[nu20_size-8]; (serialised objects: reversed tag + u32be version)
//   u8 meta[128];                                              (conversion timestamps, ends with "META")
struct Container {
    bool valid = false;
    std::string failure;
    std::uint32_t reshSize = 0;
    std::uint64_t nu20Offset = 0;
    std::uint64_t nu20End = 0;
};

constexpr std::uint64_t kMetaTrailerBytes = 128;

Container parseContainer(std::span<const std::uint8_t> file);

// Vertex declaration element as stored after a "DXTV" tag: usage, (stream << 5 | type), offset.
struct VertexElement {
    std::uint8_t usage = 0;
    std::uint8_t stream = 0;
    std::uint8_t type = 0;
    std::uint8_t offset = 0;
};

// Byte sizes of element types seen in the data; 0 marks a type whose size is not established.
unsigned elementTypeSize(std::uint8_t type);

// Hypothesis under test: u32be 0x502; u32be vertex_count; "DXTV"; u32be version; u32be element_count;
// element_count x 3 bytes; 6 bytes; then, when every element is in stream 0, vertex_count * stride bytes (LE).
struct VertexBuffer {
    std::uint64_t markerOffset = 0;
    std::uint32_t count = 0;
    std::uint32_t version = 0;
    std::vector<VertexElement> elements;
    std::uint32_t stride = 0;
    bool inlineData = false;
    bool unknownType = false;
    std::uint64_t dataOffset = 0;
};

// Hypothesis under test: u32be 0x102; u32be index_count; u32be index_size; index_count * index_size bytes (LE).
struct IndexBuffer {
    std::uint64_t markerOffset = 0;
    std::uint32_t count = 0;
    std::uint32_t indexSize = 0;
    std::uint64_t dataOffset = 0;
    std::uint32_t maxIndex = 0;
};

struct Scan {
    std::vector<VertexBuffer> vertexBuffers;
    std::vector<IndexBuffer> indexBuffers;
    std::map<std::string, std::uint32_t> tagVersions;
    std::map<std::string, std::uint64_t> tagCounts;
};

Scan scanObjects(std::span<const std::uint8_t> file, const Container& container);

float halfToFloat(std::uint16_t half);

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Positions of an inline buffer whose usage-0 element is type 6 (4 x half). Returns false otherwise.
bool readPositions(std::span<const std::uint8_t> file, const VertexBuffer& vb, std::vector<Float3>& out,
                   std::uint64_t& wNotOne);

}  // namespace gamecore::ghg
