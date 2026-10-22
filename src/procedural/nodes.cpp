#include "procedural/nodes.hpp"
#include "procedural/bvh.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace atelier::procedural {
namespace {
// Node failures leave evaluate as exceptions; Runtime reports them as invocation_failed.
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
}

void MeshImport::evaluate(const Context& context, Geometry& output) const {
    output = import_mesh(*take(context.mesh(mesh)).mesh);
}

void Grid::evaluate(Geometry& output) const { output = take(grid(size, rows, columns)); }

void Line::evaluate(Geometry& output) const { output = take(line(start, end, count)); }

void Scatter::evaluate(const Context& context, const Geometry& surface, Geometry& points) const {
    points = take(scatter(surface, {.density = density, .seed = hash(context.seed, seed), .min_distance = min_distance}));
}

void Transform::evaluate(const Geometry& input, Geometry& output) const {
    output = input;
    take(transform(output, translate, rotate, scale));
}

void Promote::evaluate(const Geometry& input, Geometry& output) const {
    output = input;
    take(promote(output, name, to, reduce));
}

void Remove::evaluate(const Geometry& input, Geometry& output) const { output = take(input.remove_flagged(layer, flag)); }

void Merge::evaluate(const Geometries& inputs, Geometry& output) const {
    output = take(Geometry::merge({inputs.data(), inputs.size()}));
}

void Ray::evaluate(const Geometry& points, const Geometry& surface, Geometry& output) const {
    if (!std::isfinite(max_distance) || max_distance < 0) throw std::runtime_error("Ray max distance must be finite and non-negative.");
    const auto unit = math::try_normalize(direction);
    if (mode == RayMode::project && !unit) throw std::runtime_error("Ray direction must be finite and nonzero.");
    if (surface.count(Layer::primitive) > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Ray surfaces have fewer than 2^31 primitives.");
    const auto bvh = take(Bvh::build(surface));
    output = points;
    const auto count = output.count(Layer::point);
    auto& positions = *take(output.write<math::Vector3>("P", Layer::point));
    auto& primitives = *take(output.add<std::int32_t>("hitprim", Layer::point));
    auto& uvs = *take(output.add<math::Vector2>("hitprimuv", Layer::point));
    auto& distances = *take(output.add<float>("hitdist", Layer::point));
    auto* normals = normal ? take(output.add<math::Vector3>("N", Layer::point)) : nullptr;
    Array<bool> missed;
    missed.resize(remove_misses ? count : 0);
    const auto& corners = surface.positions();
    const auto vertices = surface.vertex_points();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto hit = mode == RayMode::project ? bvh.ray(positions[i], *unit, max_distance) : bvh.closest(positions[i], max_distance);
        if (!hit) {
            primitives[i] = -1;
            uvs[i] = {};
            distances[i] = -1;
            if (remove_misses) missed[i] = true;
            continue;
        }
        positions[i] = hit->position;
        primitives[i] = static_cast<std::int32_t>(hit->primitive);
        uvs[i] = hit->uv;
        distances[i] = hit->distance;
        if (normals) {
            const auto a = corners[vertices[3 * hit->primitive]];
            const auto b = corners[vertices[3 * hit->primitive + 1]], c = corners[vertices[3 * hit->primitive + 2]];
            if (const auto n = math::try_normalize(math::cross(b - a, c - a))) (*normals)[i] = *n;
        }
    }
    if (remove_misses) output = take(output.remove_flagged(Layer::point, std::span<const bool>{missed.data(), missed.size()}));
}

void CopyToPoints::evaluate(const Context& context, const Geometry& points, Geometry& output) const {
    std::vector<Reference> references;
    references.reserve(meshes.size());
    for (const auto& key : meshes) references.push_back(take(context.mesh(key)));
    output = take(copy_to_points(points, references, variant));
}

}
