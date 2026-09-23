#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace gamecore::io {

// Random-access view. Callers never receive a writable pointer into the source.
struct ByteSource {
    std::uint64_t size = 0;
    std::size_t (*read)(void* ctx, std::uint64_t offset, std::uint8_t* dst, std::size_t count) = nullptr;
    void* ctx = nullptr;
};

inline bool readExact(const ByteSource& source, std::uint64_t offset, void* dst, std::size_t count) {
    if (source.read == nullptr || dst == nullptr) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (offset > source.size || count > source.size - offset) {
        return false;
    }
    const auto n = source.read(source.ctx, offset, static_cast<std::uint8_t*>(dst), count);
    return n == count;
}

inline bool readU16(const ByteSource& source, std::uint64_t offset, bool little, std::uint16_t& out) {
    std::uint8_t b[2];
    if (!readExact(source, offset, b, 2)) {
        return false;
    }
    out = little ? static_cast<std::uint16_t>(b[0] | (b[1] << 8))
                 : static_cast<std::uint16_t>((b[0] << 8) | b[1]);
    return true;
}

inline bool readU32(const ByteSource& source, std::uint64_t offset, bool little, std::uint32_t& out) {
    std::uint8_t b[4];
    if (!readExact(source, offset, b, 4)) {
        return false;
    }
    if (little) {
        out = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
              (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    } else {
        out = (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
              (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
    }
    return true;
}

inline bool readU64(const ByteSource& source, std::uint64_t offset, bool little, std::uint64_t& out) {
    std::uint8_t b[8];
    if (!readExact(source, offset, b, 8)) {
        return false;
    }
    out = 0;
    if (little) {
        for (int i = 7; i >= 0; --i) {
            out = (out << 8) | b[i];
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            out = (out << 8) | b[i];
        }
    }
    return true;
}

struct MemoryBytes {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = 0;
};

inline std::size_t memoryRead(void* ctx, std::uint64_t offset, std::uint8_t* dst, std::size_t count) {
    auto* mem = static_cast<const MemoryBytes*>(ctx);
    if (mem == nullptr || mem->data == nullptr || dst == nullptr) {
        return 0;
    }
    if (offset > mem->size || count > mem->size - offset) {
        return 0;
    }
    std::memcpy(dst, mem->data + static_cast<std::size_t>(offset), count);
    return count;
}

inline ByteSource sourceFromMemory(MemoryBytes& mem) {
    ByteSource source;
    source.size = mem.size;
    source.read = &memoryRead;
    source.ctx = &mem;
    return source;
}

struct SliceBytes {
    const ByteSource* parent = nullptr;
    std::uint64_t base = 0;
};

inline std::size_t sliceRead(void* ctx, std::uint64_t offset, std::uint8_t* dst, std::size_t count) {
    auto* slice = static_cast<const SliceBytes*>(ctx);
    if (slice == nullptr || slice->parent == nullptr || slice->parent->read == nullptr) {
        return 0;
    }
    return slice->parent->read(slice->parent->ctx, slice->base + offset, dst, count);
}

// Window [base, base + size) of a parent source; `storage` must outlive the returned source.
inline ByteSource sliceOf(const ByteSource& parent, std::uint64_t base, std::uint64_t size, SliceBytes& storage) {
    ByteSource source;
    if (base > parent.size) {
        return source;
    }
    storage.parent = &parent;
    storage.base = base;
    source.size = size > parent.size - base ? parent.size - base : size;
    source.read = &sliceRead;
    source.ctx = &storage;
    return source;
}

inline ByteSource sourceFromSpan(std::span<const std::uint8_t> bytes, MemoryBytes& storage) {
    storage.data = bytes.data();
    storage.size = static_cast<std::uint64_t>(bytes.size());
    return sourceFromMemory(storage);
}

}  // namespace gamecore::io
