#include "Tools/AssetInspector/GraphicsScan.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace gamecore::inspect {
namespace {

struct Pattern {
    const char* text;
    const char* api;
};

constexpr Pattern kPatterns[] = {
    {"d3d9.dll", "directx9"},
    {"d3d11.dll", "directx11"},
    {"d3d11createdevice", "directx11"},
    {"d3d12.dll", "directx12"},
    {"d3d12createdevice", "directx12"},
    {"dxgi.dll", "directxgi"},
    {"opengl32.dll", "opengl"},
    {"libglesv2", "opengles"},
    {"libegl", "opengles"},
    {"vulkan-1.dll", "vulkan"},
    {"vkcreateinstance", "vulkan"},
    {"metal.framework", "metal"},
    {"mtlcreatesystemdefaultdevice", "metal"},
};

std::size_t findInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return std::string_view::npos;
    }
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(static_cast<unsigned char>(haystack[i + j])) != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::vector<GraphicsEvidence> scanGraphicsApis(const io::ByteSource& source, std::uint64_t maxBytes) {
    std::vector<GraphicsEvidence> found;
    if (source.read == nullptr || source.size == 0) {
        return found;
    }
    const std::uint64_t limit = source.size < maxBytes ? source.size : maxBytes;
    constexpr std::size_t kOverlap = 64;
    std::string window;
    window.reserve((1u << 20) + kOverlap);
    std::vector<std::uint8_t> chunk(1u << 20);
    std::uint64_t offset = 0;
    while (offset < limit) {
        const std::size_t ask = static_cast<std::size_t>(
            (limit - offset) > chunk.size() ? chunk.size() : (limit - offset));
        const std::size_t got = source.read(source.ctx, offset, chunk.data(), ask);
        if (got == 0) {
            break;
        }
        window.append(reinterpret_cast<const char*>(chunk.data()), got);
        for (const Pattern& pattern : kPatterns) {
            bool already = false;
            for (const auto& item : found) {
                if (item.api == pattern.api && item.evidence == pattern.text) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (findInsensitive(window, pattern.text) != std::string_view::npos) {
                found.push_back(GraphicsEvidence{pattern.api, pattern.text});
            }
        }
        if (window.size() > kOverlap) {
            window.erase(0, window.size() - kOverlap);
        }
        offset += got;
    }

    const bool hasGles = std::any_of(found.begin(), found.end(), [](const GraphicsEvidence& item) {
        return item.api == "opengles";
    });
    if (hasGles) {
        found.erase(std::remove_if(found.begin(), found.end(),
                                    [](const GraphicsEvidence& item) { return item.api == "opengl"; }),
                    found.end());
    }
    return found;
}

}  // namespace gamecore::inspect
