#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gamecore::image {

// 8-bit RGB, rows top to bottom. Uses stored (uncompressed) deflate blocks, so any PNG reader accepts it.
bool writePngRgb(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 const std::vector<std::uint8_t>& rgb);

}  // namespace gamecore::image
