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
    float tangent[4]{}; // Tangent and handedness: MikkTSpace on publication, or authored (Tangents::authored).
};

void generate_tangents(std::span<Vertex>);
// Keeps each authored handedness; orthogonalizes XYZ against the unit normal and normalizes it.
// A zero tangent, or one parallel to its normal at any length, takes the same fallback as degenerate UVs.
void adopt_tangents(std::span<Vertex>);
enum class Tangents : std::uint8_t { generate, authored };

struct MeshPart {
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t material{};
};

struct MaterialSlot {
    std::string name;
};

struct Bounds {
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

// Immutable CPU mesh resource, independent of World, Runtime and GPU lifetime.
// Triangles use consecutive vertices. Sharing the resource never shares an Entity.
class Mesh final {
public:
    // One optional mask per triangle: bit i displays the edge opposite vertex i.
    // No masks means all triangle edges. Producers preserve polygon boundaries
    // during triangulation; the renderer never guesses connectivity from positions.
    explicit Mesh(std::vector<Vertex> vertices, std::vector<std::uint8_t> edge_masks = {},
                  std::vector<MaterialSlot> materials = {}, std::vector<MeshPart> parts = {},
                  Tangents tangents = Tangents::generate)
        : Mesh(std::move(vertices), std::move(edge_masks), std::move(materials), std::move(parts), tangents, false) {}
    // Restores an already published snapshot: validates the triangle partition and
    // finite, orthonormal tangent frames, but never renormalizes any float bits.
    // Unlike author construction, slots and parts must already be explicit.
    [[nodiscard]] static std::shared_ptr<const Mesh> restore_published(std::vector<Vertex> vertices,
                                                                       std::vector<std::uint8_t> edge_masks,
                                                                       std::vector<MaterialSlot> materials,
                                                                       std::vector<MeshPart> parts) {
        return std::shared_ptr<const Mesh>(new Mesh(std::move(vertices), std::move(edge_masks), std::move(materials),
                                                    std::move(parts), Tangents::authored, true));
    }

private:
    Mesh(std::vector<Vertex> vertices, std::vector<std::uint8_t> edge_masks, std::vector<MaterialSlot> materials,
         std::vector<MeshPart> parts, Tangents tangents, bool published)
        : vertices_(std::move(vertices)), edge_masks_(std::move(edge_masks)), materials_(std::move(materials)),
          parts_(std::move(parts)) {
        if (vertices_.empty() || vertices_.size() % 3 != 0 || vertices_.size() > std::numeric_limits<int>::max())
            throw std::invalid_argument("Mesh requires a nonempty triangle list.");
        if (!edge_masks_.empty() && edge_masks_.size() != vertices_.size() / 3)
            throw std::invalid_argument("Mesh requires one edge mask per triangle.");
        for (auto mask : edge_masks_)
            if (mask > 7)
                throw std::invalid_argument("Mesh edge mask uses only three bits.");
        if (published && (materials_.empty() || parts_.empty()))
            throw std::invalid_argument("Published mesh requires explicit material slots and parts.");
        if (materials_.empty())
            materials_.push_back({});
        if (parts_.empty())
            parts_.push_back({0, static_cast<std::uint32_t>(vertices_.size()), 0});
        std::uint64_t end = 0;
        for (const auto& part : parts_) {
            if (part.first_vertex != end || !part.vertex_count || part.vertex_count % 3 ||
                part.material >= materials_.size())
                throw std::invalid_argument("Mesh parts must partition complete triangles with valid materials.");
            end += part.vertex_count;
        }
        if (end != vertices_.size())
            throw std::invalid_argument("Mesh parts must cover every vertex exactly once.");
        std::copy_n(vertices_.front().position, 3, bounds_.minimum.begin());
        bounds_.maximum = bounds_.minimum;
        for (auto& vertex : vertices_) {
            for (float value : vertex.uv)
                if (!std::isfinite(value))
                    throw std::invalid_argument("Mesh UV must be finite.");
            for (float value : vertex.position)
                if (!std::isfinite(value))
                    throw std::invalid_argument("Mesh position must be finite.");
            for (std::size_t axis = 0; axis < 3; ++axis) {
                bounds_.minimum[axis] = std::min(bounds_.minimum[axis], vertex.position[axis]);
                bounds_.maximum[axis] = std::max(bounds_.maximum[axis], vertex.position[axis]);
            }
            for (float value : vertex.normal)
                if (!std::isfinite(value))
                    throw std::invalid_argument("Mesh normal must be finite.");
            const double length =
                std::hypot(double(vertex.normal[0]), double(vertex.normal[1]), double(vertex.normal[2]));
            if (length == 0)
                throw std::invalid_argument("Mesh normal must be nonzero.");
            if (published) {
                for (float value : vertex.tangent)
                    if (!std::isfinite(value))
                        throw std::invalid_argument("Published mesh tangent must be finite.");
                const auto* t = vertex.tangent;
                const auto* n = vertex.normal;
                const double tangent_length = std::hypot(double(t[0]), double(t[1]), double(t[2]));
                const double projection = double(t[0]) * n[0] + double(t[1]) * n[1] + double(t[2]) * n[2];
                // Published float frames include rounding from normalization/projection.
                if (std::abs(length - 1) > 1e-5 || std::abs(tangent_length - 1) > 1e-5 || std::abs(projection) > 1e-5 ||
                    (t[3] != 1 && t[3] != -1))
                    throw std::invalid_argument(
                        "Published mesh requires an orthonormal tangent frame and handedness of 1 or -1.");
            } else
                for (float& value : vertex.normal)
                    value = static_cast<float>(value / length);
        }
        if (!published) {
            if (tangents == Tangents::authored)
                adopt_tangents(vertices_);
            else
                generate_tangents(vertices_);
        }
    }

public:
    Mesh& operator=(const Mesh&) = delete;
    Mesh& operator=(Mesh&&) = delete;
    [[nodiscard]] std::span<const Vertex> vertices() const noexcept { return vertices_; }
    [[nodiscard]] const Bounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::span<const MaterialSlot> materials() const noexcept { return materials_; }
    [[nodiscard]] std::span<const MeshPart> parts() const noexcept { return parts_; }
    [[nodiscard]] std::uint32_t material_slot(std::size_t triangle) const noexcept {
        const auto vertex = triangle * 3;
        const auto at =
            std::upper_bound(parts_.begin(), parts_.end(), vertex,
                             [](std::size_t first, const MeshPart& part) { return first < part.first_vertex; });
        return std::prev(at)->material;
    }
    [[nodiscard]] std::uint8_t edge_mask(std::size_t triangle) const noexcept {
        return edge_masks_.empty() ? std::uint8_t{7} : edge_masks_[triangle];
    }

private:
    std::vector<Vertex> vertices_;
    std::vector<std::uint8_t> edge_masks_;
    std::vector<MaterialSlot> materials_;
    std::vector<MeshPart> parts_;
    Bounds bounds_;
};

using MeshRef = std::shared_ptr<const Mesh>;

// A captured source key and its exact immutable CPU content, without material or Runtime ownership.
struct MeshSource {
    std::string source;
    MeshRef resolved;
};

// Names, not slot positions, are the persistent correspondence. Empty imported
// names use the reserved slot/<index> spelling until assigned a named asset.
[[nodiscard]] inline std::string material_slot_name(const Mesh& mesh, std::size_t index) {
    const auto& slot = mesh.materials()[index];
    return slot.name.empty() ? "slot/" + std::to_string(index) : slot.name;
}

// A built-in resource; the cube has no special object type or rendering path.
[[nodiscard]] inline MeshRef cube_mesh() {
    static const auto resource = [] {
        constexpr float positions[8][3] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                                           {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
        constexpr int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                                     {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
        constexpr float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        constexpr int indices[6] = {0, 1, 2, 0, 2, 3};
        std::vector<Vertex> vertices(36);
        for (int face = 0; face < 6; ++face)
            for (int i = 0; i < 6; ++i) {
                auto& vertex = vertices[face * 6 + i];
                std::copy_n(positions[faces[face][indices[i]]], 3, vertex.position);
                std::copy_n(normals[face], 3, vertex.normal);
                constexpr float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
                std::copy_n(uv[indices[i]], 2, vertex.uv);
            }
        return std::make_shared<const Mesh>(std::move(vertices),
                                            std::vector<std::uint8_t>{5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3});
    }();
    return resource;
}

}
