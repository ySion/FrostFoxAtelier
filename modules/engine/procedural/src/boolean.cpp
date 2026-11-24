#include "procedural/operations.hpp"
#include "detail/boolean_exact.hpp"
#include "detail/interpolate_attribute.hpp"
#include "detail/native_cook.hpp"
#include "detail/triangles.hpp"
#include "detail/topology.hpp"
#include "foundation/result.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace atelier::procedural {
namespace detail {
using namespace boolean_exact;

struct Lattice {
    math::Vector3d origin{};

    double step{1};

    GridPoint encode(math::Vector3 p) const {
        GridPoint q;
        for (int k = 0; k < 3; ++k)
            q[k] = std::llround((double(p[k]) - origin[k]) / step);
        return q;
    }

    math::Vector3 decode(const Point& p) const {
        math::Vector3 q;
        for (int k = 0; k < 3; ++k)
            q[k] = static_cast<float>(origin[k] + step * (double(p[k]) / double(p[3])));
        return q;
    }
};

struct Corner {
    Point p;

    Plane incoming;
};

using Cell = std::vector<Corner>;

// Indexed by the owning Geometry's existing point, primitive and corner
// arrays. Attribute-only COW copies share these exact numerical constructions.
struct BooleanBoundary {
    Lattice lattice;

    std::vector<Point> positions;

    std::vector<Plane> faces;

    std::vector<Plane> incoming;

    static const BooleanBoundary* get(const Geometry& mesh) { return mesh.boolean_boundary_.get(); }

    static void attach(Geometry& mesh, std::shared_ptr<BooleanBoundary> boundary) {
        mesh.boolean_boundary_ = std::move(boundary);
    }
};
}

bool Geometry::same_boolean_boundary(const Geometry& other) const noexcept {
    if (boolean_boundary_ == other.boolean_boundary_)
        return true;
    if (!boolean_boundary_ || !other.boolean_boundary_)
        return false;
    const auto& a = *boolean_boundary_;
    const auto& b = *other.boolean_boundary_;
    return a.lattice.origin == b.lattice.origin && a.lattice.step == b.lattice.step && a.positions == b.positions &&
           a.faces == b.faces && a.incoming == b.incoming;
}

std::size_t Geometry::boolean_boundary_bytes() const noexcept {
    if (!boolean_boundary_)
        return 0;
    const auto& b = *boolean_boundary_;
    return sizeof(b) + b.positions.capacity() * sizeof(detail::boolean_exact::Point) +
           (b.faces.capacity() + b.incoming.capacity()) * sizeof(detail::boolean_exact::Plane);
}

namespace {
using namespace detail;

using namespace detail::boolean_exact;

Result<Lattice> lattice(const Geometry& a, const Geometry& b) {
    math::Vector3d lo{};
    math::Vector3d hi{};
    bool first = true;
    for (const auto* mesh : {&a, &b}) {
        if (mesh->count(Layer::instance) || (!mesh->count(Layer::primitive) && mesh->count(Layer::point)))
            return failure("Boolean needs closed polygon surfaces or empty geometry.");
        for (auto id : mesh->vertex_points()) {
            const auto p = mesh->positions()[id];
            if (!math::is_finite(p))
                return failure("Boolean needs finite positions.");
            if (first)
                lo = hi = math::Vector3d(p);
            first = false;
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], double(p[k]));
                hi[k] = std::max(hi[k], double(p[k]));
            }
        }
    }
    // Keep the parent's lattice for direct chains while fresh positions fit it.
    // A larger domain or two independently quantized results start a new grid.
    for (auto mesh : {&a, &b})
        if (auto cached = BooleanBoundary::get(*mesh)) {
            const auto& grid = cached->lattice;
            bool fits = true;
            for (int k = 0; k < 3; ++k)
                fits &= lo[k] >= grid.origin[k] && hi[k] <= grid.origin[k] + grid.step * (1u << 25);
            if (fits)
                return grid;
        }
    const auto extent = hi - lo;
    const double span = std::max({extent.x, extent.y, extent.z});
    if (!span)
        return Lattice{lo, 1};
    int exponent;
    const auto fraction = std::frexp(span, &exponent);
    double step = std::ldexp(1., exponent - (fraction == .5 ? 26 : 25));
    math::Vector3d origin;
    for (;;) {
        bool fits = true;
        for (int k = 0; k < 3; ++k) {
            origin[k] = std::floor(lo[k] / step) * step;
            fits &= (hi[k] - origin[k]) / step <= (1u << 25);
        }
        if (fits)
            return Lattice{origin, step};
        step *= 2;
    }
}

struct Bounds {
    GridPoint lo;

    GridPoint hi;

    void include(const Bounds& b) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], b.lo[k]);
            hi[k] = std::max(hi[k], b.hi[k]);
        }
    }

    bool overlaps(const Bounds& b, int ray_axis) const {
        for (int k = 0; k < 3; ++k)
            if (hi[k] < b.lo[k] || (k != ray_axis && lo[k] > b.hi[k]))
                return false;
        return true;
    }
};

Point homogeneous(const GridPoint& p) {
    return {p[0], p[1], p[2], 1};
}

struct Face {
    Cell cell;

    Plane plane;

    Bounds box;

    std::uint32_t primitive;

    std::vector<std::uint32_t> corners;

    unsigned operand;

    std::uint32_t shell{};
};

GridPoint lower_grid(const Point& p) {
    GridPoint lo;
    for (int k = 0; k < 3; ++k) {
        auto v = std::int64_t(std::floor(double(p[k]) / double(p[3])));
        // On this bounded domain binary64 gives a candidate at most one
        // integer from the floor. Numerator and w can round in either direction.
        if (Wide(v) * p[3] > p[k])
            --v;
        else if (Wide(v + 1) * p[3] <= p[k])
            ++v;
        lo[k] = v;
    }
    return lo;
}

Bounds point_box(const Point& p) {
    if (p[3] == 1) {
        GridPoint q{std::int64_t(p[0]), std::int64_t(p[1]), std::int64_t(p[2])};
        return {q, q};
    }
    auto lo = lower_grid(p);
    auto hi = lo;
    for (int k = 0; k < 3; ++k)
        hi[k] += Wide(lo[k]) * p[3] != p[k];
    return {lo, hi};
}

}

struct detail::PreparedBooleanSurface {
    struct Node {
        Bounds box;

        std::size_t begin;

        std::size_t end;

        std::size_t right{};
    };

    std::vector<Face> faces;

    std::vector<std::size_t> order;

    std::vector<Node> tree;

    // For multi-shell operands, store the full bounds of the closed shells
    // represented by each BVH node. Their oriented crossings cancel outside.
    std::vector<Bounds> shells;

    std::vector<Bounds> enclosures;

    void group_shells(const Geometry& mesh) {
        const auto components = detail::point_pieces(mesh);
        const auto component = [&](const Face& f) { return components[mesh.vertex_points()[f.corners.front()]]; };
        const auto first = component(faces.front());
        if (std::ranges::all_of(faces, [&](const Face& f) { return component(f) == first; }))
            return;
        Map<std::uint32_t, std::uint32_t> used;
        for (auto& face : faces) {
            const auto entry = used.try_emplace(component(face), static_cast<std::uint32_t>(shells.size()));
            face.shell = *entry.value;
            if (entry.inserted)
                shells.push_back(face.box);
            else
                shells[face.shell].include(face.box);
        }
    }

    std::size_t build(std::size_t begin, std::size_t end) {
        auto box = faces[order[begin]].box;
        for (auto i = begin + 1; i < end; ++i)
            box.include(faces[order[i]].box);
        const auto id = tree.size();
        tree.push_back({box, begin, end});
        if (!shells.empty())
            enclosures.push_back(shells[faces[order[begin]].shell]);
        if (end - begin > 8) {
            int axis = 0;
            for (int k = 1; k < 3; ++k)
                if (box.hi[k] - box.lo[k] > box.hi[axis] - box.lo[axis])
                    axis = k;
            auto mid = begin + (end - begin) / 2;
            std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end, [&](auto a, auto b) {
                const auto center = [&](auto i) { return faces[i].box.lo[axis] + faces[i].box.hi[axis]; };
                return std::pair{center(a), a} < std::pair{center(b), b};
            });
            build(begin, mid);
            tree[id].right = build(mid, end);
            if (!shells.empty()) {
                enclosures[id] = enclosures[id + 1];
                enclosures[id].include(enclosures[tree[id].right]);
            }
        } else if (!shells.empty()) {
            for (auto i = begin + 1; i < end; ++i)
                enclosures[id].include(shells[faces[order[i]].shell]);
        }
        return id;
    }

    template <class F>
    void query(std::size_t id, const Bounds& box, int axis, F& visit) const {
        const auto& node = tree[id];
        if (!node.box.overlaps(box, axis) || (!enclosures.empty() && !enclosures[id].overlaps(box, -1)))
            return;
        if (node.right) {
            query(id + 1, box, axis, visit);
            query(node.right, box, axis, visit);
        } else {
            for (auto i = node.begin; i < node.end; ++i) {
                const auto& f = faces[order[i]];
                if (f.box.overlaps(box, axis) && (shells.empty() || shells[f.shell].overlaps(box, -1)))
                    visit(f);
            }
        }
    }

    std::size_t bytes() const noexcept {
        auto total = sizeof(*this) + faces.capacity() * sizeof(Face) + order.capacity() * sizeof(std::size_t) +
                     tree.capacity() * sizeof(Node) + (shells.capacity() + enclosures.capacity()) * sizeof(Bounds);
        for (const auto& f : faces)
            total += f.cell.capacity() * sizeof(Corner) + f.corners.capacity() * sizeof(std::uint32_t);
        return total;
    }
};

namespace {

// Both grids use power-of-two world units. Change the homogeneous basis once
// per boundary, keeping each original half-space and vertex exactly equivalent.
struct GridChange {
    std::int64_t scale;

    std::int64_t divisor;

    GridPoint delta;

    static Result<GridChange> between(const Lattice& old, const Lattice& grid) {
        double unit = std::min(old.step, grid.step);
        double scale = old.step / unit;
        double divisor = grid.step / unit;
        if (std::max(scale, divisor) > (1u << 25))
            return failure("Boolean exact boundaries exceed the common grid's scale range.");
        GridChange change{std::int64_t(scale), std::int64_t(divisor), {}};
        for (int k = 0; k < 3; ++k) {
            const double d = (old.origin[k] - grid.origin[k]) / unit;
            if (std::abs(d) > 0x1p50 || d != std::trunc(d))
                return failure("Boolean cannot align the operands' exact lattice origins.");
            change.delta[k] = std::int64_t(d);
        }
        return change;
    }

    template <class T>
    static void reduce(std::array<T, 4>& v) {
        while (!((v[0] | v[1] | v[2] | v[3]) & 1))
            for (auto& component : v)
                component /= 2;
    }

    Result<Point> apply(const Point& p) const {
        Point q{};
        for (int k = 0; k < 3; ++k)
            q[k] = p[k] * scale + p[3] * delta[k];
        q[3] = p[3] * divisor;
        reduce(q);
        for (int k = 0; k < 4; ++k) {
            const Wide bound = Wide(1) << (k == 3 ? 160 : 190);
            if (q[k] <= -bound || q[k] >= bound)
                return failure("Boolean exact vertex exceeds this lattice's bit budget.");
        }
        // Bounds, BVH centers and the w==1 plane shortcut use native integers.
        // Enforce the Cartesian bound on exact coordinates after rebasing;
        // rounded display P alone cannot certify the quotient.
        const auto limit = q[3] << 50;
        for (int k = 0; k < 3; ++k)
            if (q[k] < -limit || q[k] > limit)
                return failure("Boolean exact coordinate exceeds the Cartesian grid range.");
        return q;
    }

    Result<Plane> apply(const Plane& p) const {
        std::array<Offset, 4> v{};
        v[3] = p.d * scale;
        for (int k = 0; k < 3; ++k) {
            v[k] = Offset(p.n[k]) * divisor;
            v[3] -= Offset(p.n[k]) * delta[k];
        }
        reduce(v);
        Plane q{};
        for (int k = 0; k < 4; ++k) {
            const Offset bound = Offset(1) << (k == 3 ? 81 : 52);
            if (v[k] <= -bound || v[k] >= bound)
                return failure("Boolean exact plane exceeds this lattice's bit budget.");
            if (k < 3)
                q.n[k] = std::int64_t(v[k]);
        }
        q.d = v[3];
        return q;
    }
};

// A planar face needs its authored fan only if removing a diagonal can
// change the attribute field. Constant columns are independent of the fan.
// On a parallelogram, scalar/vector data is affine exactly when opposite
// corners have equal sums. TwoSum retains the residue for distant exponents.
bool equal_sums(float a, float b, float c, float d) {
    const auto sum = [](double x, double y) {
        const double high = x + y;
        const double tail = high - x;
        return std::pair{high, (x - (high - tail)) + (y - tail)};
    };
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) && sum(a, b) == sum(c, d);
}

bool fan_independent(const Geometry& mesh, const std::vector<std::uint32_t>& corners, bool parallelogram) {
    for (const auto& attr : mesh.attributes()) {
        if (attr.name == "P" || (attr.layer != Layer::vertex && attr.layer != Layer::point))
            continue;
        const bool independent = std::visit(
            [&](const auto& data) {
                using T = typename std::decay_t<decltype(data)>::Value;
                const auto at = [&](std::uint32_t corner) -> const T& {
                    return data.array()[attr.layer == Layer::point ? mesh.vertex_points()[corner] : corner];
                };
                const auto& a = at(corners.front());
                if (std::ranges::all_of(corners, [&](auto c) { return at(c) == a; }))
                    return true;
                if (!parallelogram || attr.usage == Usage::normal)
                    return false;
                const auto& b = at(corners[1]);
                const auto& c = at(corners[2]);
                const auto& d = at(corners[3]);
                if constexpr (std::same_as<T, float>)
                    return equal_sums(a, c, b, d);
                else if constexpr (std::same_as<T, math::Vector2> || std::same_as<T, math::Vector3> ||
                                   std::same_as<T, math::Vector4>) {
                    for (std::size_t k = 0; k < T::dimensions; ++k)
                        if (!equal_sums(a[k], c[k], b[k], d[k]))
                            return false;
                    return true;
                } else
                    return false;
            },
            attr.data);
        if (!independent)
            return false;
    }
    return true;
}

Result<PreparedBooleanSurface> make_surface(const Geometry& mesh, const Lattice& grid, unsigned operand) {
    PreparedBooleanSurface result;
    auto cached = BooleanBoundary::get(mesh);
    if (cached) {
        ATELIER_TRY_BIND(change, GridChange::between(cached->lattice, grid));
        const bool same = cached->lattice.origin == grid.origin && cached->lattice.step == grid.step;
        result.faces.reserve(cached->faces.size());
        for (std::uint32_t i = 0; i < cached->faces.size(); ++i) {
            const auto range = mesh.primitive(i);
            Face f{{}, cached->faces[i], {}, i, {}, operand};
            f.cell.reserve(range.count);
            f.corners.reserve(range.count);
            for (std::uint32_t j = range.first; j < range.first + range.count; ++j) {
                f.cell.push_back({cached->positions[mesh.vertex_points()[j]], cached->incoming[j]});
                f.corners.push_back(j);
            }
            if (!same) {
                ATELIER_TRY_BIND(plane, change->apply(f.plane));
                f.plane = *plane;
                for (auto& c : f.cell) {
                    ATELIER_TRY_BIND(point, change->apply(c.p));
                    ATELIER_TRY_BIND(edge, change->apply(c.incoming));
                    c = {*point, *edge};
                }
            }
            f.box = point_box(f.cell.front().p);
            for (const auto& c : f.cell)
                f.box.include(point_box(c.p));
            result.faces.push_back(std::move(f));
        }
    } else {
        ATELIER_TRY_BIND(view, detail::TriangleView::create(mesh));
        ATELIER_TRY(detail::validate_shells(mesh));
        std::vector<GridPoint> points(mesh.positions().size());
        for (const auto id : mesh.vertex_points())
            points[id] = grid.encode(mesh.positions()[id]);
        result.faces.reserve(view->size());
        Offset six_volume = 0;
        for (std::uint32_t primitive = 0; primitive < mesh.count(Layer::primitive); ++primitive) {
            const auto range = mesh.primitive(primitive);
            const auto point = [&](std::uint32_t corner) -> const GridPoint& {
                return points[mesh.vertex_points()[corner]];
            };
            std::vector<std::uint32_t> corners(range.count);
            std::iota(corners.begin(), corners.end(), range.first);
            Plane support{};
            bool collinear_fan = false;
            for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
                const auto plane = through(point(corners[0]), point(corners[i]), point(corners[i + 1]));
                if (support.n == GridPoint{})
                    support = plane;
                collinear_fan |= plane.n == GridPoint{};
                six_volume -= plane.d;
            }
            auto invalid_face = [&](const char* reason) {
                return failure("Boolean operand " + std::to_string(operand) + ", primitive " +
                               std::to_string(primitive) + reason);
            };
            if (support.n == GridPoint{})
                return invalid_face(" is degenerate on the common 25-bit grid.");
            auto add_face = [&](std::vector<std::uint32_t> ids, Plane plane) -> Result<void> {
                if (plane.n == GridPoint{})
                    return invalid_face(" is degenerate on the common 25-bit grid.");
                auto position = [&](auto c) { return math::Vector3d(mesh.positions()[mesh.vertex_points()[c]]); };
                math::Vector3d area{};
                const auto origin = position(ids[0]);
                for (std::size_t i = 1; i + 1 < ids.size(); ++i)
                    area += math::cross(position(ids[i]) - origin, position(ids[i + 1]) - origin);
                if (area.x * double(plane.n[0]) + area.y * double(plane.n[1]) + area.z * double(plane.n[2]) <= 0)
                    return invalid_face(" reversed orientation on the common 25-bit grid.");
                const int axis = plane.axis();
                const auto& start = point(ids.front());
                Face f{{}, plane, {start, start}, primitive, std::move(ids), operand};
                f.cell.reserve(f.corners.size());
                auto prev = f.corners.back();
                for (auto c : f.corners) {
                    auto p = point(c);
                    const auto edge = edge_plane(point(prev), p, axis);
                    if (edge.n == GridPoint{})
                        return invalid_face(" is degenerate on the common 25-bit grid.");
                    f.cell.push_back({homogeneous(p), plane.n[axis] < 0 ? edge.flipped() : edge});
                    f.box.include({p, p});
                    prev = c;
                }
                for (std::size_t i = 0; i < f.cell.size(); ++i)
                    if (f.cell[i].incoming.side(f.cell[(i + 1) % f.cell.size()].p) < 0)
                        return invalid_face(" has a reversed corner on the common 25-bit grid.");
                result.faces.push_back(std::move(f));
                return {};
            };
            bool parallelogram = corners.size() == 4;
            if (parallelogram) {
                for (int k = 0; k < 3; ++k)
                    parallelogram &=
                        point(corners[0])[k] + point(corners[2])[k] == point(corners[1])[k] + point(corners[3])[k];
            }
            const bool planar = corners.size() > 3 && std::ranges::all_of(corners, [&](auto c) {
                                    return !support.side(homogeneous(point(c)));
                                });
            if (corners.size() == 3 || (planar && (collinear_fan || fan_independent(mesh, corners, parallelogram)))) {
                ATELIER_TRY(add_face(std::move(corners), support));
            } else {
                for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
                    const auto a = corners[0];
                    const auto b = corners[i];
                    const auto c = corners[i + 1];
                    ATELIER_TRY(add_face({a, b, c}, through(point(a), point(b), point(c))));
                }
            }
        }
        if (!result.faces.empty() && six_volume <= 0)
            return failure("Boolean requires outward-oriented solid boundaries.");
    }
    if (!result.faces.empty()) {
        result.group_shells(mesh);
        result.order.resize(result.faces.size());
        std::iota(result.order.begin(), result.order.end(), std::size_t{});
        result.build(0, result.order.size());
    }
    return result;
}

Result<const PreparedBooleanSurface*> prepare_operand(const Geometry& mesh, const Lattice& grid, unsigned operand,
                                                      PreparedBooleanSurface& scratch, BooleanStats* stats) {
    auto* cook = NativeCook::current;
    BooleanPreparationCache::Operand* repeated = nullptr;
    if (cook && cook->inputs[operand].get() == &mesh) {
        auto& slot = cook->preparation.operands[operand];
        if (slot.input.lock().get() == &mesh && slot.origin == grid.origin && slot.step == grid.step) {
            if (slot.prepared) {
                if (stats)
                    stats->prepared_input_hits |= 1u << operand;
                return slot.prepared.get();
            }
            repeated = &slot;
        } else {
            slot = {};
            slot.input = cook->inputs[operand];
            slot.origin = grid.origin;
            slot.step = grid.step;
        }
    }
    ATELIER_TRY_BIND(surface, make_surface(mesh, grid, operand));
    if (repeated) {
        const auto bytes = surface->bytes();
        if (bytes <= cook->budget - cook->preparation.bytes()) {
            repeated->prepared = std::make_shared<PreparedBooleanSurface>(std::move(*surface));
            repeated->bytes = bytes;
            return repeated->prepared.get();
        }
    }
    scratch = std::move(*surface);
    return &scratch;
}

// Traverse the boundary once for both children. Each new corner remains the
// intersection of three original support planes, independent of clipping depth.
// Live capacities owned by one face's arrangement. Count split children while
// the parent is still alive. Allocator metadata and transient reallocations
// inside a vector are outside this measurement.
struct FaceScratch {
    BooleanStats* stats;

    std::size_t corner_bytes{};

    std::size_t candidate_bytes{};

    void observe(std::size_t cell_bytes, std::size_t extra = 0) const {
        stats->peak_face_scratch_bytes = std::max<std::uint64_t>(stats->peak_face_scratch_bytes,
                                                                 cell_bytes + corner_bytes + candidate_bytes + extra);
    }
};

template <class Select>
void divide(std::vector<Cell>& cells, const Plane& face, const Plane& cut, FaceScratch& scratch, Select&& select) {
    const auto count = cells.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& old = cells[i];
        bool pos = false;
        bool neg = false;
        for (const auto& c : old) {
            int s = cut.side(c.p);
            pos |= s > 0;
            neg |= s < 0;
        }
        if (!(pos && neg) || !select(old))
            continue;
        Cell above;
        Cell below;
        int previous = cut.side(old.back().p);
        for (const auto& c : old) {
            int side = cut.side(c.p);
            if (previous * side < 0) {
                const auto p = intersection(face, c.incoming, cut);
                above.push_back({p, previous > 0 ? c.incoming : cut});
                below.push_back({p, previous < 0 ? c.incoming : cut.flipped()});
            }
            if (side >= 0)
                above.push_back({c.p, side == 0 && previous < 0 ? cut : c.incoming});
            if (side <= 0)
                below.push_back({c.p, side == 0 && previous > 0 ? cut.flipped() : c.incoming});
            previous = side;
        }
        if (scratch.stats) {
            const auto children = (above.capacity() + below.capacity()) * sizeof(Corner);
            scratch.observe(cells.capacity() * sizeof(Cell), children);
            scratch.corner_bytes -= old.capacity() * sizeof(Corner);
            scratch.corner_bytes += children;
            ++scratch.stats->splits;
        }
        cells[i] = std::move(above);
        cells.push_back(std::move(below));
        if (scratch.stats)
            scratch.observe(cells.capacity() * sizeof(Cell));
    }
}

std::pair<int, int> sides(const Cell& cell, const Plane& cut) {
    int lo = 1;
    int hi = -1;
    for (const auto& c : cell) {
        const int side = cut.side(c.p);
        lo = std::min(lo, side);
        hi = std::max(hi, side);
    }
    return {lo, hi};
}

bool coplanar_overlap(const Face& a, const Face& b) {
    for (auto [host, other] : {std::pair{&a, &b}, std::pair{&b, &a}})
        for (const auto& c : host->cell)
            if (sides(other->cell, c.incoming).second <= 0)
                return false;
    return true;
}

struct SectionEnd {
    Point p;

    const Plane* support;
};

using SectionEnds = InlineArray<SectionEnd, 4>;

// Keep the incident support planes at an on-plane vertex, including both
// adjacent edges. They determine where the later cut has to stop.
void section_ends(const Plane& plane, const Face& g, SectionEnds& ends) {
    const Corner* prev = &g.cell.back();
    int previous = plane.side(prev->p);
    for (const auto& c : g.cell) {
        const int side = plane.side(c.p);
        if (side * previous < 0)
            ends.add({intersection(plane, g.plane, c.incoming), &c.incoming});
        else {
            if (!side)
                ends.add({c.p, &c.incoming});
            if (!previous)
                ends.add({prev->p, &c.incoming});
        }
        prev = &c;
        previous = side;
    }
}

struct Section {
    Point first;

    Point last;

    // The caller has certified that the line crosses this convex cell's
    // interior. On that line each edge bounds a 1-D interval. A segment of
    // positive length misses the open interval iff one edge excludes both ends.
    bool enters(const Cell& cell) const {
        for (const auto& c : cell)
            if (c.incoming.side(first) <= 0 && c.incoming.side(last) <= 0)
                return false;
        return true;
    }
};

bool solid(bool a, bool b, BooleanOperation op) {
    switch (op) {
    case BooleanOperation::unite:
        return a || b;
    case BooleanOperation::intersect:
        return a && b;
    case BooleanOperation::subtract:
        return a && !b;
    }
    return false;
}

struct Patch {
    const Face* face;

    // Empty storage borrows the entire source face until publication.
    Cell clipped;

    bool reverse;

    const Cell& boundary() const { return clipped.empty() ? face->cell : clipped; }
};

// A positive convex combination p0 + e*p1 + e^2*p2 + ... (normalized)
// represents a point strictly inside the cell for infinitesimal positive e.
// Evaluating a plane there needs only the first nonzero corner sign.
int interior_side(const Cell& c, const Plane& p) {
    for (const auto& v : c)
        if (auto s = p.side(v.p))
            return s;
    return 0;
}

// Project onto the cyclic (u,v) plane normal to the ray. Evaluating the
// edges in boundary order permits an early exit on the first outside edge.
int projected_side(const Cell& sample, const Corner& from, const Corner& to, const Plane& face, int axis,
                   int direction) {
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    if (from.p[3] == 1 && to.p[3] == 1) {
        const auto x = std::int64_t(from.p[u]);
        const auto y = std::int64_t(from.p[v]);
        const auto dx = std::int64_t(to.p[u]) - x;
        const auto dy = std::int64_t(to.p[v]) - y;
        for (const auto& c : sample) {
            const auto& q = c.p;
            int side;
            if (q[3] == 1) {
                side = sign(Offset(dx) * (std::int64_t(q[v]) - y) - Offset(dy) * (std::int64_t(q[u]) - x));
            } else {
                const auto d = Offset(dy) * x - Offset(dx) * y;
                side = sign(Product(dx) * q[v] - Product(dy) * q[u] + Product(d) * q[3]);
            }
            if (side)
                return direction * side;
        }
        return 0;
    }
    // Cached rational boundaries carry their exact, inward edge support.
    // Eliminate the ray coordinate between that plane and the face plane.
    const auto& e = to.incoming;
    const Offset a = Offset(e.n[u]) * face.n[axis] - Offset(face.n[u]) * e.n[axis];
    const Offset b = Offset(e.n[v]) * face.n[axis] - Offset(face.n[v]) * e.n[axis];
    const Wide d = Wide(e.d) * face.n[axis] - Wide(face.d) * e.n[axis];
    for (const auto& c : sample) {
        const auto& q = c.p;
        if (auto side = sign(Product(a) * q[u] + Product(b) * q[v] + Product(d) * q[3]))
            return direction * side;
    }
    return 0;
}

struct Crossings {
    std::int64_t above{};

    std::int64_t at{};
};

Crossings winding(const Cell& cell, int axis, const PreparedBooleanSurface& target, BooleanStats* stats) {
    Crossings result;
    // The symbolic interior sample tends to the first corner. Any target
    // hit for arbitrarily small positive epsilon also meets the closed ray
    // bounds at this limit. Predicates still use the complete cell at ties.
    const auto ray = point_box(cell.front().p);
    if (target.tree.empty() || !target.tree.front().box.overlaps(ray, -1))
        return result;
    auto visit = [&](const Face& g) {
        const auto dir = sign(g.plane.n[axis]);
        if (!dir)
            return;
        if (stats)
            ++stats->ray_candidates;
        const auto side = interior_side(cell, g.plane);
        if (side == dir)
            return;
        const auto* prev = &g.cell.back();
        for (const auto& c : g.cell) {
            if (projected_side(cell, *prev, c, g.plane, axis, dir) < 0)
                return;
            prev = &c;
        }
        if (side)
            result.above += dir;
        else
            result.at += dir;
    };
    target.query(0, ray, axis, visit);
    return result;
}

// Decide which side of the source boundary contributes to the regularized solid.
std::optional<bool> output_orientation(const Face& f, int axis, Crossings winding, BooleanOperation op) {
    const auto [above, at] = winding;
    if (at && f.operand == 1)
        return {}; // A owns coincident boundaries.
    auto outside = above;
    auto inside = above + at;
    if (f.plane.n[axis] < 0)
        std::swap(outside, inside);
    const bool minus = f.operand ? solid(inside > 0, true, op) : solid(true, inside > 0, op);
    const bool plus = f.operand ? solid(outside > 0, false, op) : solid(false, outside > 0, op);
    if (minus == plus)
        return {};
    return plus;
}

bool interior_contains(const Cell& cell, const Point& point) {
    return std::ranges::all_of(cell, [&](const Corner& c) { return c.incoming.side(point) > 0; });
}

Result<void> arrange(const Geometry& mesh, const PreparedBooleanSurface& source, const PreparedBooleanSurface& other,
                     BooleanOperation op, std::vector<Patch>& patches, BooleanStats* stats) {
    enum class Location : std::uint8_t { unknown, outside, inside };
    std::vector<Location> locations(mesh.count(Layer::point));
    for (const auto& f : source.faces) {
        const int axis = f.plane.axis();
        if (other.tree.empty() || !other.tree.front().box.overlaps(f.box, -1)) {
            if (auto reverse = output_orientation(f, axis, {}, op))
                patches.push_back({&f, {}, *reverse});
            continue;
        }
        std::vector<Cell> cells;
        bool may_touch = false;
        struct Segment {
            const Face* face;

            bool ends_inside;

            Section interval;
        };
        std::vector<Segment> segments;
        SectionEnds ends;
        const auto all = [](const Cell&) { return true; };
        FaceScratch scratch{stats};
        const auto observe_sections = [&] {
            const auto spill = ends.capacity() > 4 ? ends.capacity() * sizeof(SectionEnd) : 0;
            scratch.candidate_bytes = segments.capacity() * sizeof(Segment) + spill;
            scratch.observe(cells.capacity() * sizeof(Cell));
        };
        auto split = [&](const Plane& cut, const auto& select) {
            if (cells.empty()) {
                cells.push_back(f.cell);
                if (stats) {
                    scratch.corner_bytes = cells.front().capacity() * sizeof(Corner);
                    scratch.observe(cells.capacity() * sizeof(Cell));
                }
            }
            divide(cells, f.plane, cut, scratch, select);
        };
        auto visit = [&](const Face& g) {
            if (stats)
                ++stats->candidate_pairs;
            const auto [lo, hi] = sides(f.cell, g.plane);
            if (lo > 0 || hi < 0)
                return;
            if (lo == 0 || hi == 0) {
                may_touch = true;
                if (!lo && !hi && coplanar_overlap(f, g))
                    for (const auto& c : g.cell)
                        split(c.incoming, all);
            } else {
                ends.clear();
                section_ends(f.plane, g, ends);
                if (stats && ends.capacity() > 4)
                    observe_sections();
                if (ends.empty())
                    return;
                may_touch = true;
                const auto [low, high] = std::ranges::minmax_element(ends, PointLess{}, &SectionEnd::p);
                if (!PointLess{}(low->p, high->p))
                    return;
                Section section{low->p, high->p};
                if (!section.enters(f.cell))
                    return;
                bool ends_inside = false;
                for (const auto& end : ends) {
                    if (!interior_contains(f.cell, end.p))
                        continue;
                    ends_inside = true;
                    split(*end.support, [&](const Cell& cell) { return interior_contains(cell, end.p); });
                }
                segments.push_back({&g, ends_inside, section});
                if (stats)
                    observe_sections();
            }
        };
        other.query(0, f.box, -1, visit);
        for (const auto& segment : segments) {
            const auto& g = *segment.face;
            split(g.plane, [&](const Cell& cell) { return !segment.ends_inside || segment.interval.enters(cell); });
        }
        if (cells.size() <= 1) {
            Crossings hit;
            if (may_touch) {
                hit = winding(f.cell, axis, other, stats);
            } else {
                // Every point of this closed face is disjoint from the other
                // boundary. The inside flag is constant, also at its original
                // vertices. Point indices preserve disconnected touching fans.
                auto known = Location::unknown;
                for (auto c : f.corners) {
                    known = locations[mesh.vertex_points()[c]];
                    if (known != Location::unknown)
                        break;
                }
                if (known == Location::unknown) {
                    const auto sample = winding(f.cell, axis, other, stats);
                    known = sample.above > 0 ? Location::inside : Location::outside;
                }
                for (auto c : f.corners)
                    locations[mesh.vertex_points()[c]] = known;
                hit.above = known == Location::inside;
            }
            if (auto reverse = output_orientation(f, axis, hit, op))
                patches.push_back({&f, {}, *reverse});
            continue;
        }
        const auto first_patch = patches.size();
        for (auto& c : cells)
            if (auto reverse = output_orientation(f, axis, winding(c, axis, other, stats), op))
                patches.push_back({&f, std::move(c), *reverse});
        if (patches.size() - first_patch == cells.size()) {
            const bool reversed = patches[first_patch].reverse;
            if (std::all_of(patches.begin() + first_patch, patches.end(),
                            [&](const Patch& p) { return p.reverse == reversed; })) {
                patches.resize(first_patch);
                patches.push_back({&f, {}, reversed});
            }
        }
    }
    return {};
}

struct Piece {
    const Face* face;

    std::vector<std::uint32_t> vertices;

    std::vector<Plane> incoming;

    bool reverse;
};

struct MeshBuilder {
    std::vector<Point> points;

    std::vector<math::Vector3> decoded;

    std::vector<Piece> pieces;

    Result<void> sew(const std::vector<Patch>& patches, const Lattice& grid) {
        struct Support {
            const Plane* face;

            const Plane* edge;

            std::size_t fingerprint;

            bool operator==(const Support& b) const noexcept {
                return fingerprint == b.fingerprint && Line(*face, *edge) == Line(*b.face, *b.edge);
            }
        };
        struct SupportHash {
            using is_avalanching = void;

            std::size_t operator()(const Support& key) const noexcept { return key.fingerprint; }
        };
        struct LinePoints {
            int axis;

            InlineArray<std::uint32_t, 4> ids;
        };
        struct Endpoint {
            std::size_t line;

            std::uint32_t point;
        };
        std::vector<LinePoints> lines;
        std::vector<Endpoint> ends;
        std::size_t corner_count = 0;
        for (const auto& patch : patches)
            corner_count += patch.boundary().size();
        ends.reserve(corner_count);
        {
            // All patch boundaries stay immutable until sewing completes.
            Map<const Point*, std::uint32_t, PointRefHash, PointRefEqual> index;
            Map<Support, std::size_t, SupportHash> line_index;
            for (const auto& patch : patches) {
                const auto first = ends.size();
                for (const auto& c : patch.boundary()) {
                    auto point = index.try_emplace(&c.p, static_cast<std::uint32_t>(points.size()));
                    if (point.inserted) {
                        if (points.size() >= element_limit)
                            return failure("Boolean point limit exceeded.");
                        points.push_back(c.p);
                        decoded.push_back(grid.decode(c.p));
                    }
                    Line key(patch.face->plane, c.incoming);
                    const Support support{&patch.face->plane, &c.incoming, key.hash()};
                    auto line = line_index.try_emplace(support, lines.size());
                    if (line.inserted)
                        lines.push_back({key.axis, {}});
                    ends.push_back({*line.value, *point.value});
                }
                auto previous = ends.back().point;
                for (auto i = first; i < ends.size(); ++i) {
                    const auto [line, point] = ends[i];
                    auto& ids = lines[line].ids;
                    // Most supporting lines carry the same edge from two faces.
                    const bool same_edge = ids.size() == 2 && ((ids[0] == previous && ids[1] == point) ||
                                                               (ids[1] == previous && ids[0] == point));
                    if (!same_edge) {
                        ids.add(previous);
                        ids.add(point);
                    }
                    previous = point;
                }
            }
        }
        for (auto& [axis, ids] : lines) {
            if (ids.size() == 2 && ids[0] != ids[1])
                continue;
            std::ranges::sort(ids, [&](auto a, auto b) { return compare(points[a], points[b], axis) < 0; });
            ids.resize(std::unique(ids.begin(), ids.end()) - ids.begin());
        }
        pieces.reserve(patches.size());
        std::size_t first = 0;
        for (const auto& patch : patches) {
            Piece piece{patch.face, {}, {}, patch.reverse};
            const auto& c = patch.boundary();
            piece.vertices.reserve(c.size());
            piece.incoming.reserve(c.size());
            for (std::size_t i = 0; i < c.size(); ++i) {
                const auto j = (i + 1) % c.size();
                const auto& next = c[j];
                const auto& [line_axis, ids] = lines[ends[first + j].line];
                if (ids.size() == 2) {
                    piece.vertices.push_back(ends[first + i].point);
                    piece.incoming.push_back(next.incoming);
                    continue;
                }
                auto at = [&](const Point& p) {
                    return std::lower_bound(ids.begin(), ids.end(), p, [&](auto id, const Point& q) {
                        return compare(points[id], q, line_axis) < 0;
                    });
                };
                auto start = at(c[i].p);
                auto end = at(next.p);
                for (auto it = start; it != end; start < end ? ++it : --it) {
                    piece.vertices.push_back(*it);
                    piece.incoming.push_back(next.incoming); // Outgoing until the loop is complete.
                }
            }
            first += c.size();
            if (patch.reverse) {
                std::reverse(piece.vertices.begin(), piece.vertices.end());
                std::reverse(piece.incoming.begin(), piece.incoming.end());
            } else {
                std::rotate(piece.incoming.rbegin(), piece.incoming.rbegin() + 1, piece.incoming.rend());
            }
            // The rest of Geometry tessellates a convex polygon from its first
            // corner. Pick a fan whose visible triangles retain their orientation.
            const auto n = piece.vertices.size();
            const auto axis = piece.face->plane.axis();
            const int direction = sign(piece.face->plane.n[axis]) * (piece.reverse ? -1 : 1);
            for (std::size_t i = 0; i < n; ++i) {
                if (decoded[piece.vertices[i]] == decoded[piece.vertices[(i + 1) % n]]) {
                    return failure("Boolean operand " + std::to_string(piece.face->operand) + ", primitive " +
                                   std::to_string(piece.face->primitive) + ", boundary corner " + std::to_string(i) +
                                   " collapses onto its neighbor at float P precision.");
                }
            }
            auto admissible = [&](std::size_t start) {
                const auto& a = points[piece.vertices[start]];
                const auto p = math::Vector3d(decoded[piece.vertices[start]]);
                for (std::size_t i = 1; i + 1 < n; ++i) {
                    const auto& b = points[piece.vertices[(start + i) % n]];
                    const auto& c = points[piece.vertices[(start + i + 1) % n]];
                    const auto q = math::Vector3d(decoded[piece.vertices[(start + i) % n]]);
                    const auto r = math::Vector3d(decoded[piece.vertices[(start + i + 1) % n]]);
                    const auto area = direction * math::cross(q - p, r - p)[axis];
                    if (area < 0 || (area == 0 && determinant(a, b, c, axis)))
                        return false;
                }
                return true;
            };
            std::size_t start = 0;
            while (start < n && !admissible(start))
                ++start;
            if (start == n) {
                return failure("Boolean operand " + std::to_string(patch.face->operand) + ", primitive " +
                               std::to_string(patch.face->primitive) + " has no representable float triangle fan.");
            }
            std::rotate(piece.vertices.begin(), piece.vertices.begin() + start, piece.vertices.end());
            std::rotate(piece.incoming.begin(), piece.incoming.begin() + start, piece.incoming.end());
            pieces.push_back(std::move(piece));
        }
        return {};
    }
};

struct Sample {
    const Piece* piece;

    std::array<std::uint32_t, 3> corners;

    std::array<double, 3> weights;
};

Result<Sample> sample(const Piece& piece, const Point& p) {
    const auto& f = *piece.face;
    const int axis = f.plane.axis();
    const auto& a = f.cell[0].p;
    // A corner of a previously published polygon can lie on a straight edge.
    // Preserve its authored value even when it belongs to a zero-area fan piece.
    for (std::size_t i = 0; i < f.cell.size(); ++i)
        if (same_point(f.cell[i].p, p))
            return Sample{&piece, {f.corners[i], f.corners[i], f.corners[i]}, {1., 0., 0.}};
    for (std::size_t j = 0; j < f.cell.size(); ++j) {
        if (f.cell[j].incoming.side(p))
            continue;
        const auto i = (j + f.cell.size() - 1) % f.cell.size();
        const auto& x = f.cell[i].p;
        const auto& y = f.cell[j].p;
        for (int k = 0; k < 3; ++k) {
            const Product span = Product(y[k]) * x[3] - Product(x[k]) * y[3];
            if (!span)
                continue;
            if (compare(p, x, k) * compare(p, y, k) > 0)
                break;
            const Product dist = Product(p[k]) * x[3] - Product(x[k]) * p[3];
            const auto t = (double(dist) / double(span)) * (double(y[3]) / double(p[3]));
            return Sample{&piece, {f.corners[i], f.corners[j], f.corners[j]}, {1. - t, t, 0.}};
        }
    }
    for (std::size_t i = 1; i + 1 < f.cell.size(); ++i) {
        const auto& b = f.cell[i].p;
        const auto& c = f.cell[i + 1].p;
        auto total = determinant(a, b, c, axis);
        if (!total)
            continue;
        std::array<Triple, 3> d{determinant(p, b, c, axis), determinant(a, p, c, axis), determinant(a, b, p, axis)};
        if (std::ranges::any_of(d, [&](auto t) { return sign(t) * sign(total) < 0; }))
            continue;
        Sample result{&piece, {f.corners[0], f.corners[i], f.corners[i + 1]}, {}};
        std::array<Wide, 3> w{a[3], b[3], c[3]};
        for (int k = 0; k < 3; ++k)
            result.weights[k] = (double(d[k]) / double(total)) * (double(w[k]) / double(p[3]));
        return result;
    }
    return failure("Boolean boundary sample lies outside its exact source polygon.");
}

Result<void> attributes(Geometry& out, const std::array<const Geometry*, 2>& inputs, const std::vector<Piece>& pieces,
                        const std::vector<Sample>& corners, const std::vector<std::uint32_t>& donors) {
    for (unsigned side = 0; side < 2; ++side) {
        for (const auto& attr : inputs[side]->attributes()) {
            const auto name = attr.name.view();
            if (name == "P")
                continue;
            const auto* a = inputs[0]->find(name);
            const auto* b = inputs[1]->find(name);
            if (a && side)
                continue;
            if (a && b && (a->layer != b->layer || a->usage != b->usage || a->data.index() != b->data.index()))
                return failure("Boolean attribute schemas differ for " + std::string(name.std_view()) + ".");
            if (name == "boolean_operand" || name == "boolean_primitive") {
                if (attr.layer != Layer::primitive || !std::holds_alternative<SharedArray<std::uint32_t>>(attr.data))
                    return failure("Boolean provenance requires primitive uint columns.");
                continue;
            }
            if (attr.layer == Layer::detail) {
                ATELIER_TRY(out.set(attr.name, attr.layer, attr.data, attr.usage));
                continue;
            }
            auto transfer = std::visit(
                [&](const auto& column) -> Result<void> {
                    using Column = std::decay_t<decltype(column)>;
                    using T = typename Column::Value;
                    std::array<const Array<T>*, 2> data{a ? &std::get<Column>(a->data).array() : nullptr,
                                                        b ? &std::get<Column>(b->data).array() : nullptr};
                    Array<T> values;
                    const auto count = out.count(attr.layer);
                    values.reserve(count);
                    for (std::uint32_t i = 0; i < count; ++i) {
                        if (attr.layer == Layer::primitive) {
                            const auto& piece = pieces[i];
                            const auto* src = data[piece.face->operand];
                            auto value = src ? (*src)[piece.face->primitive] : T{};
                            if constexpr (std::same_as<T, math::Vector3>)
                                if (attr.usage == Usage::normal && piece.reverse)
                                    value = -value;
                            values.add(std::move(value));
                        } else {
                            const bool at_point = attr.layer == Layer::point;
                            const auto& sample = corners[at_point ? donors[i] : i];
                            const auto& f = *sample.piece->face;
                            auto ids = sample.corners;
                            if (at_point)
                                for (auto& id : ids)
                                    id = inputs[f.operand]->vertex_points()[id];
                            values.add(data[f.operand]
                                           ? detail::interpolate_attribute(*data[f.operand], ids, sample.weights,
                                                                           attr.usage, sample.piece->reverse)
                                           : T{});
                        }
                    }
                    return out.set(attr.name, attr.layer, Column{std::move(values)}, attr.usage);
                },
                attr.data);
            if (!transfer)
                return std::unexpected(transfer.error());
        }
    }
    Array<std::uint32_t> operands;
    Array<std::uint32_t> primitives;
    for (const auto& piece : pieces) {
        operands.add(piece.face->operand);
        primitives.add(piece.face->primitive);
    }
    ATELIER_TRY(out.set(String::literal<"boolean_operand">(), Layer::primitive, SharedArray{std::move(operands)}));
    return out.set(String::literal<"boolean_primitive">(), Layer::primitive, SharedArray{std::move(primitives)});
}

// Build point identities by pairing opposite halfedges. Independent shells
// that touch at a vertex occupy distinct point fans at the same coordinate.
struct Fans {
    std::vector<std::uint32_t> parent;

    explicit Fans(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }

    std::uint32_t root(std::uint32_t v) {
        while (v != parent[v]) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    }

    void join(std::uint32_t a, std::uint32_t b) { parent[root(a)] = root(b); }
};

Result<Geometry> publish(const MeshBuilder& mesh, const Lattice& grid, const std::array<const Geometry*, 2>& inputs) {
    const auto& pieces = mesh.pieces;
    struct Edge {
        std::uint32_t lo;

        std::uint32_t hi;

        std::uint32_t corner;
    };
    std::vector<Edge> edges;
    std::vector<std::uint32_t> ids;
    std::vector<std::uint32_t> next;
    std::vector<std::uint32_t> owners;
    Array<std::uint32_t> offsets{0};
    for (std::uint32_t i = 0; i < pieces.size(); ++i) {
        const auto& loop = pieces[i].vertices;
        if (ids.size() + loop.size() > element_limit)
            return failure("Boolean corner limit exceeded.");
        const auto start = std::uint32_t(ids.size());
        for (std::size_t j = 0; j < loop.size(); ++j) {
            auto a = loop[j];
            auto b = loop[(j + 1) % loop.size()];
            edges.push_back({std::min(a, b), std::max(a, b), std::uint32_t(ids.size())});
            ids.push_back(a);
            owners.push_back(i);
            next.push_back(start + std::uint32_t((j + 1) % loop.size()));
        }
        offsets.add(std::uint32_t(ids.size()));
    }
    Fans fans(ids.size());
    std::ranges::sort(edges, {}, [](const Edge& e) { return std::pair{e.lo, e.hi}; });
    auto pair = [&](std::uint32_t a, std::uint32_t b) -> Result<void> {
        if (ids[a] != ids[next[b]])
            return failure("Boolean produced inconsistent boundary winding.");
        fans.join(a, next[b]);
        fans.join(next[a], b);
        return {};
    };
    for (std::size_t i = 0; i < edges.size();) {
        auto end = i + 1;
        while (end < edges.size() && edges[i].lo == edges[end].lo && edges[i].hi == edges[end].hi)
            ++end;
        if (end - i == 2) {
            ATELIER_TRY(pair(edges[i].corner, edges[i + 1].corner));
        } else if (end - i >= 4 && (end - i) % 2 == 0) {
            // Keep tangentially meeting shells separate. Check shell keys in order so multiple
            // invalid groups on one edge report the same first error.
            const auto shell_key = [&](const Edge& edge) {
                const auto& face = *pieces[owners[edge.corner]].face;
                return std::pair{face.operand, face.shell};
            };
            std::ranges::stable_sort(std::span{edges}.subspan(i, end - i), {}, shell_key);
            for (auto j = i; j < end;) {
                const auto first = j++;
                const auto key = shell_key(edges[first]);
                while (j < end && shell_key(edges[j]) == key)
                    ++j;
                if (j - first != 2)
                    return failure("Boolean boundary has an ambiguous non-manifold edge.");
                ATELIER_TRY(pair(edges[first].corner, edges[first + 1].corner));
            }
        } else
            return failure("Boolean result has an open or non-manifold boundary.");
        i = end;
    }
    Array<math::Vector3> positions;
    Array<std::uint32_t> vertices;
    std::vector<Sample> samples;
    std::vector<std::uint32_t> donors;
    std::vector<std::uint32_t> root_ids(edges.size(), UINT32_MAX);
    auto boundary = std::make_shared<BooleanBoundary>();
    boundary->lattice = grid;
    boundary->faces.reserve(pieces.size());
    boundary->positions.reserve(mesh.points.size());
    boundary->incoming.reserve(edges.size());
    for (const auto& piece : pieces) {
        boundary->faces.push_back(piece.reverse ? piece.face->plane.flipped() : piece.face->plane);
        for (std::size_t i = 0; i < piece.vertices.size(); ++i) {
            const auto v = piece.vertices[i];
            boundary->incoming.push_back(piece.incoming[i]);
            ATELIER_TRY_BIND(value, sample(piece, mesh.points[v]));
            const auto corner = static_cast<std::uint32_t>(vertices.size());
            samples.push_back(*value);
            auto& point = root_ids[fans.root(corner)];
            if (point == UINT32_MAX) {
                point = static_cast<std::uint32_t>(positions.size());
                positions.add(mesh.decoded[v]);
                boundary->positions.push_back(mesh.points[v]);
                donors.push_back(corner);
            }
            vertices.add(point);
        }
    }
    ATELIER_TRY_BIND(result, Geometry::polygons(std::move(positions), std::move(vertices), offsets));
    ATELIER_TRY(detail::validate_shells(*result));
    ATELIER_TRY(attributes(*result, inputs, pieces, samples, donors));
    // An empty result has no construction to preserve. Carrying its grid into
    // the next cook would make an unrelated fresh operand inherit its precision.
    if (!pieces.empty())
        BooleanBoundary::attach(*result, std::move(boundary));
    return std::move(*result);
}

template <class F>
auto stage(BooleanStats* stats, std::chrono::nanoseconds BooleanStats::* elapsed, const char* name, F&& run) {
    using Clock = std::chrono::steady_clock;
    const auto start = stats ? Clock::now() : Clock::time_point{};
    auto result = run();
    if (stats)
        stats->*elapsed += Clock::now() - start;
    if (!result)
        result.error().message = std::string(name) + ": " + result.error().message;
    return result;
}
}

Result<Geometry> solid_boolean(const Geometry& a, const Geometry& b, BooleanOperation operation, BooleanStats* stats) {
    if (stats)
        *stats = {};
    if (operation != BooleanOperation::unite && operation != BooleanOperation::intersect &&
        operation != BooleanOperation::subtract)
        return failure("Unknown Boolean operation.");
    ATELIER_TRY_BIND(grid,
                     stage(stats, &BooleanStats::prepare, "Boolean prepare lattice", [&] { return lattice(a, b); }));
    PreparedBooleanSurface scratch[2];
    ATELIER_TRY_BIND(left, stage(stats, &BooleanStats::prepare, "Boolean prepare A",
                                 [&] { return prepare_operand(a, *grid, 0, scratch[0], stats); }));
    if (stats)
        stats->input_faces = (*left)->faces.size();
    ATELIER_TRY_BIND(right, stage(stats, &BooleanStats::prepare, "Boolean prepare B",
                                  [&] { return prepare_operand(b, *grid, 1, scratch[1], stats); }));
    if (stats)
        stats->input_faces += (*right)->faces.size();
    std::vector<Patch> patches;
    ATELIER_TRY(stage(stats, &BooleanStats::arrange, "Boolean arrange A",
                      [&] { return arrange(a, **left, **right, operation, patches, stats); }));
    ATELIER_TRY(stage(stats, &BooleanStats::arrange, "Boolean arrange B",
                      [&] { return arrange(b, **right, **left, operation, patches, stats); }));
    MeshBuilder mesh;
    ATELIER_TRY(stage(stats, &BooleanStats::sew, "Boolean sew", [&] { return mesh.sew(patches, *grid); }));
    return stage(stats, &BooleanStats::publish, "Boolean publish", [&] { return publish(mesh, *grid, {&a, &b}); });
}
}
