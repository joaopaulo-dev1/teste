#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gamecore::ir {

// Host-side intermediate representation. Fields stay empty until a measured converter fills them.
// No loader in this header claims a proprietary layout.

enum class TextureEncoding : std::uint8_t {
    Unknown = 0,
    Rgba8,
};

struct Mesh {
    std::string name;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<std::uint32_t> indices;

    bool validate(std::string& reason) const {
        if (positions.size() % 3 != 0) {
            reason = "positions are not a multiple of 3";
            return false;
        }
        const auto vertexCount = static_cast<std::uint32_t>(positions.size() / 3);
        if (vertexCount == 0) {
            reason = "mesh has no vertices";
            return false;
        }
        if (!normals.empty() && normals.size() != positions.size()) {
            reason = "normals do not match positions";
            return false;
        }
        if (!uvs.empty() && uvs.size() / 2 != positions.size() / 3) {
            reason = "uvs do not match vertex count";
            return false;
        }
        if (indices.size() < 3 || indices.size() % 3 != 0) {
            reason = "indices are not a triangle list";
            return false;
        }
        for (std::uint32_t index : indices) {
            if (index >= vertexCount) {
                reason = "index out of range";
                return false;
            }
        }
        reason.clear();
        return true;
    }
};

struct Texture {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureEncoding encoding = TextureEncoding::Unknown;
    std::vector<std::uint8_t> rgba8;

    bool validate(std::string& reason) const {
        if (width == 0 || height == 0) {
            reason = "texture dimensions are zero";
            return false;
        }
        if (encoding == TextureEncoding::Rgba8) {
            const auto expected = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ull;
            if (rgba8.size() != expected) {
                reason = "rgba8 byte count does not match dimensions";
                return false;
            }
        }
        reason.clear();
        return true;
    }
};

struct Material {
    std::string name;
    int albedoTexture = -1;
    float baseColor[4] = {1.f, 1.f, 1.f, 1.f};

    bool validate(std::string& reason) const {
        for (float channel : baseColor) {
            if (!std::isfinite(channel)) {
                reason = "material color is not finite";
                return false;
            }
        }
        reason.clear();
        return true;
    }
};

struct Skeleton {
    std::vector<std::string> jointNames;
    std::vector<int> parents;
    std::vector<float> inverseBindMatrices;

    bool validate(std::string& reason) const {
        if (jointNames.size() != parents.size()) {
            reason = "joint name count does not match parent count";
            return false;
        }
        if (!inverseBindMatrices.empty() && inverseBindMatrices.size() != jointNames.size() * 16) {
            reason = "inverse bind matrices are not 4x4 per joint";
            return false;
        }
        const int count = static_cast<int>(parents.size());
        for (int i = 0; i < count; ++i) {
            const int parent = parents[static_cast<std::size_t>(i)];
            if (parent < -1 || parent >= count || parent == i) {
                reason = "parent index is invalid";
                return false;
            }
        }
        std::vector<int> color(static_cast<std::size_t>(count), 0);
        for (int start = 0; start < count; ++start) {
            int cursor = start;
            while (cursor != -1) {
                if (color[static_cast<std::size_t>(cursor)] == start + 1) {
                    reason = "skeleton contains a cycle";
                    return false;
                }
                if (color[static_cast<std::size_t>(cursor)] != 0) {
                    break;
                }
                color[static_cast<std::size_t>(cursor)] = start + 1;
                cursor = parents[static_cast<std::size_t>(cursor)];
            }
        }
        reason.clear();
        return true;
    }
};

struct AnimationClip {
    std::string name;
    float durationSeconds = 0.f;
    int jointCount = 0;

    bool validate(std::string& reason) const {
        if (!(durationSeconds >= 0.f) || !std::isfinite(durationSeconds)) {
            reason = "animation duration is invalid";
            return false;
        }
        if (jointCount < 0) {
            reason = "joint count is negative";
            return false;
        }
        reason.clear();
        return true;
    }
};

struct AudioClip {
    std::string name;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::vector<float> pcm;

    bool validate(std::string& reason) const {
        if (sampleRate == 0 || channels == 0) {
            reason = "audio rate or channel count is zero";
            return false;
        }
        if (!pcm.empty() && pcm.size() % channels != 0) {
            reason = "pcm frame size does not match channels";
            return false;
        }
        reason.clear();
        return true;
    }
};

struct CollisionShape {
    std::string name;
    float aabbMin[3] = {};
    float aabbMax[3] = {};

    bool validate(std::string& reason) const {
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(aabbMin[axis]) || !std::isfinite(aabbMax[axis]) || aabbMin[axis] > aabbMax[axis]) {
                reason = "aabb is invalid";
                return false;
            }
        }
        reason.clear();
        return true;
    }
};

struct Entity {
    std::string name;
    int mesh = -1;
    int material = -1;
    int skeleton = -1;
    int animation = -1;
    int audio = -1;
    int collision = -1;
};

struct Scene {
    std::string name;
    std::vector<Entity> entities;
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::vector<Skeleton> skeletons;
    std::vector<AnimationClip> animations;
    std::vector<AudioClip> audioClips;
    std::vector<CollisionShape> collisions;

    bool validate(std::string& reason) const {
        auto inRange = [](int index, std::size_t count) {
            return index < 0 || static_cast<std::size_t>(index) < count;
        };
        for (const Entity& entity : entities) {
            if (!inRange(entity.mesh, meshes.size()) || !inRange(entity.material, materials.size()) ||
                !inRange(entity.skeleton, skeletons.size()) || !inRange(entity.animation, animations.size()) ||
                !inRange(entity.audio, audioClips.size()) || !inRange(entity.collision, collisions.size())) {
                reason = "entity references an asset that is not in the scene";
                return false;
            }
        }
        for (const Mesh& mesh : meshes) {
            if (!mesh.validate(reason)) {
                return false;
            }
        }
        for (const Texture& texture : textures) {
            if (!texture.validate(reason)) {
                return false;
            }
        }
        for (const Material& material : materials) {
            if (!material.validate(reason)) {
                return false;
            }
        }
        for (const Skeleton& skeleton : skeletons) {
            if (!skeleton.validate(reason)) {
                return false;
            }
        }
        reason.clear();
        return true;
    }
};

}  // namespace gamecore::ir
