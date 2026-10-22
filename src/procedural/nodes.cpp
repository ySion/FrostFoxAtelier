#include "procedural/nodes.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"

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

void CopyToPoints::evaluate(const Context& context, const Geometry& points, Geometry& output) const {
    std::vector<Reference> references;
    references.reserve(meshes.size());
    for (const auto& key : meshes) references.push_back(take(context.mesh(key)));
    output = take(copy_to_points(points, references, variant));
}

}
