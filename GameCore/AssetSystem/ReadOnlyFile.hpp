#pragma once

#include "GameCore/AssetSystem/ByteSource.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace gamecore::io {

// Opens a path with read-only OS flags. There is no write(), truncate(), or map-writable API.
class ReadOnlyFile {
public:
    ReadOnlyFile() = default;
    ~ReadOnlyFile();

    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    ReadOnlyFile(ReadOnlyFile&& other) noexcept;
    ReadOnlyFile& operator=(ReadOnlyFile&& other) noexcept;

    static bool open(const std::filesystem::path& path, ReadOnlyFile& out, std::string& error);

    std::uint64_t size() const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    bool valid() const noexcept;

    std::size_t readAt(std::uint64_t offset, std::uint8_t* dst, std::size_t count) const;
    ByteSource source() const;

    // Stream a copy. The source handle stays GENERIC_READ / O_RDONLY.
    static bool copyTo(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::string& error);

private:
    void close() noexcept;

    std::filesystem::path path_;
    std::uint64_t size_ = 0;
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

std::string pathToUtf8(const std::filesystem::path& path);

}  // namespace gamecore::io
