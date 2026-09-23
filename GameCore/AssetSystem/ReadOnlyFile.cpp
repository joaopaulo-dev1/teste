#include "GameCore/AssetSystem/ReadOnlyFile.hpp"

#include <fstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gamecore::io {
namespace {

std::size_t fileReadThunk(void* ctx, std::uint64_t offset, std::uint8_t* dst, std::size_t count) {
    return static_cast<const ReadOnlyFile*>(ctx)->readAt(offset, dst, count);
}

}  // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
    return path.string();
#endif
}

ReadOnlyFile::~ReadOnlyFile() { close(); }

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : path_(std::move(other.path_)), size_(other.size_) {
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.size_ = 0;
}

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close();
    path_ = std::move(other.path_);
    size_ = other.size_;
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.size_ = 0;
    return *this;
}

void ReadOnlyFile::close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
    handle_ = nullptr;
#else
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
#endif
    size_ = 0;
}

bool ReadOnlyFile::valid() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
}

bool ReadOnlyFile::open(const std::filesystem::path& path, ReadOnlyFile& out, std::string& error) {
    out.close();
    out.path_.clear();
#if defined(_WIN32)
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "read-only open failed: " + pathToUtf8(path);
        return false;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length) || length.QuadPart < 0) {
        CloseHandle(handle);
        error = "size query failed: " + pathToUtf8(path);
        return false;
    }
    out.handle_ = handle;
    out.size_ = static_cast<std::uint64_t>(length.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "read-only open failed: " + pathToUtf8(path);
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        error = "size query failed: " + pathToUtf8(path);
        return false;
    }
    out.fd_ = fd;
    out.size_ = static_cast<std::uint64_t>(st.st_size);
#endif
    out.path_ = path;
    error.clear();
    return true;
}

std::size_t ReadOnlyFile::readAt(std::uint64_t offset, std::uint8_t* dst, std::size_t count) const {
    if (!valid() || dst == nullptr || count == 0) {
        return 0;
    }
    if (offset > size_) {
        return 0;
    }
    const std::uint64_t available = size_ - offset;
    if (static_cast<std::uint64_t>(count) > available) {
        count = static_cast<std::size_t>(available);
    }
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), dst, static_cast<DWORD>(count), &got, &overlapped)) {
        return 0;
    }
    return static_cast<std::size_t>(got);
#else
    const ssize_t got = ::pread(fd_, dst, count, static_cast<off_t>(offset));
    if (got < 0) {
        return 0;
    }
    return static_cast<std::size_t>(got);
#endif
}

ByteSource ReadOnlyFile::source() const {
    ByteSource source;
    source.size = size_;
    source.read = &fileReadThunk;
    source.ctx = const_cast<ReadOnlyFile*>(this);
    return source;
}

bool ReadOnlyFile::copyTo(const std::filesystem::path& source,
                          const std::filesystem::path& destination,
                          std::string& error) {
    std::error_code ec;
    const auto srcCanon = std::filesystem::weakly_canonical(source, ec);
    const auto dstParent = destination.parent_path();
    if (!dstParent.empty()) {
        std::filesystem::create_directories(dstParent, ec);
        if (ec) {
            error = "failed to create copy directory: " + pathToUtf8(dstParent);
            return false;
        }
    }
    const auto dstCanon = std::filesystem::weakly_canonical(destination, ec);
    if (!ec && !srcCanon.empty() && srcCanon == dstCanon) {
        error = "refusing to copy a file onto itself";
        return false;
    }

    ReadOnlyFile in;
    if (!open(source, in, error)) {
        return false;
    }

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to create copy: " + pathToUtf8(destination);
        return false;
    }

    std::vector<std::uint8_t> buffer(1u << 20);
    std::uint64_t offset = 0;
    while (offset < in.size()) {
        const std::size_t n = in.readAt(offset, buffer.data(), buffer.size());
        if (n == 0) {
            error = "short read while copying: " + pathToUtf8(source);
            return false;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n));
        if (!out) {
            error = "short write while copying: " + pathToUtf8(destination);
            return false;
        }
        offset += static_cast<std::uint64_t>(n);
    }
    return true;
}

}  // namespace gamecore::io
