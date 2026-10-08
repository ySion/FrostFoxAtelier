#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace atelier::geometry {

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2]{};
};

// Immutable tightly packed RGBA8 source, with top-left image origin. Color
// interpretation belongs to the material slot (sRGB diffuse, linear opacity).
class Image final {
public:
    Image(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {
        if (!width || !height || width > 8192 || height > 8192 ||
            std::uint64_t(width)*height*4 != pixels_.size())
            throw std::invalid_argument("Invalid material image dimensions or RGBA storage.");
    }
    Image& operator=(const Image&) = delete;
    Image& operator=(Image&&) = delete;
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }
private:
    std::uint32_t width_, height_;
    std::vector<std::uint8_t> pixels_;
};
using ImageRef = std::shared_ptr<const Image>;
struct Material {
    std::string name;
    std::array<float,4> diffuse{1,1,1,1};
    ImageRef color{}, opacity{};
};
struct MeshPart {
    std::uint32_t first_vertex{}, vertex_count{}, material{};
};

// Immutable CPU mesh resource, independent of World, Runtime and GPU lifetime.
// Triangles use consecutive vertices. Sharing the resource never shares an Actor.
class Mesh final {
public:
    // One optional mask per triangle: bit i displays the edge opposite vertex i.
    // No masks means all triangle edges. Producers preserve polygon boundaries
    // during triangulation; the renderer never guesses connectivity from positions.
    explicit Mesh(std::vector<Vertex> vertices, std::vector<std::uint8_t> edge_masks = {},
                  std::vector<Material> materials = {}, std::vector<MeshPart> parts = {})
        : vertices_(std::move(vertices)), edge_masks_(std::move(edge_masks)),
          materials_(std::move(materials)), parts_(std::move(parts)) {
        if (vertices_.empty() || vertices_.size() % 3 != 0 ||
            vertices_.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("Mesh requires a nonempty triangle list.");
        if (!edge_masks_.empty() && edge_masks_.size() != vertices_.size() / 3)
            throw std::invalid_argument("Mesh requires one edge mask per triangle.");
        for (auto mask : edge_masks_)
            if (mask > 7) throw std::invalid_argument("Mesh edge mask uses only three bits.");
        if (materials_.empty()) materials_.push_back({});
        if (parts_.empty()) parts_.push_back({0,static_cast<std::uint32_t>(vertices_.size()),0});
        std::uint64_t end=0;
        for (const auto& part:parts_) {
            if (part.first_vertex!=end || !part.vertex_count || part.vertex_count%3 || part.material>=materials_.size())
                throw std::invalid_argument("Mesh parts must partition complete triangles with valid materials.");
            end+=part.vertex_count;
        }
        if (end!=vertices_.size()) throw std::invalid_argument("Mesh parts must cover every vertex exactly once.");
        for (const auto& material:materials_) for (float value:material.diffuse)
            if (!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("Invalid diffuse material factor.");
        for (auto& vertex : vertices_) {
            for (float value : vertex.uv)
                if (!std::isfinite(value)) throw std::invalid_argument("Mesh UV must be finite.");
            for (float value : vertex.position)
                if (!std::isfinite(value)) throw std::invalid_argument("Mesh position must be finite.");
            for (float value : vertex.normal)
                if (!std::isfinite(value)) throw std::invalid_argument("Mesh normal must be finite.");
            const double length = std::hypot(double(vertex.normal[0]), double(vertex.normal[1]), double(vertex.normal[2]));
            if (length == 0) throw std::invalid_argument("Mesh normal must be nonzero.");
            for (float& value : vertex.normal) value = static_cast<float>(value / length);
        }
    }
    Mesh& operator=(const Mesh&) = delete;
    Mesh& operator=(Mesh&&) = delete;
    [[nodiscard]] std::span<const Vertex> vertices() const noexcept { return vertices_; }
    [[nodiscard]] std::span<const Material> materials() const noexcept { return materials_; }
    [[nodiscard]] std::span<const MeshPart> parts() const noexcept { return parts_; }
    [[nodiscard]] bool has_materials() const noexcept {
        return std::ranges::any_of(materials_, [](const auto& m) { return !m.name.empty(); });
    }
    [[nodiscard]] std::uint8_t edge_mask(std::size_t triangle) const noexcept {
        return edge_masks_.empty() ? std::uint8_t{7} : edge_masks_[triangle];
    }
private:
    std::vector<Vertex> vertices_;
    std::vector<std::uint8_t> edge_masks_;
    std::vector<Material> materials_;
    std::vector<MeshPart> parts_;
};

using MeshRef = std::shared_ptr<const Mesh>;

// A built-in resource; the cube has no special object type or rendering path.
[[nodiscard]] inline MeshRef cube_mesh() {
    static const auto resource = [] {
        constexpr float positions[8][3] = {
            {-.5f,-.5f,-.5f}, {.5f,-.5f,-.5f}, {.5f,.5f,-.5f}, {-.5f,.5f,-.5f},
            {-.5f,-.5f,.5f}, {.5f,-.5f,.5f}, {.5f,.5f,.5f}, {-.5f,.5f,.5f}};
        constexpr int faces[6][4] = {{0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {3,2,6,7}, {4,5,1,0}};
        constexpr float normals[6][3] = {{0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}, {0,1,0}, {0,-1,0}};
        constexpr int indices[6] = {0,1,2,0,2,3};
        std::vector<Vertex> vertices(36);
        for (int face = 0; face < 6; ++face) for (int i = 0; i < 6; ++i) {
            auto& vertex = vertices[face * 6 + i];
            std::copy_n(positions[faces[face][indices[i]]], 3, vertex.position);
            std::copy_n(normals[face], 3, vertex.normal);
        }
        return std::make_shared<const Mesh>(std::move(vertices),
            std::vector<std::uint8_t>{5,3,5,3,5,3,5,3,5,3,5,3});
    }();
    return resource;
}

}
