#include "procedural/operations.hpp"
#include "detail/boolean_exact.hpp"
#include "detail/triangles.hpp"
#include "detail/topology.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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
};

GridPoint lower_grid(const Point& p) {
    GridPoint lo;
    for (int k = 0; k < 3; ++k) {
        auto v = std::int64_t(std::floor(double(p[k]) / double(p[3])));
        if (Wide(v) * p[3] > p[k])
            --v;
        lo[k] = v;
    }
    return lo;
}

Bounds point_box(const Point& p) {
    auto lo = lower_grid(p);
    auto hi = lo;
    for (int k = 0; k < 3; ++k)
        hi[k] += Wide(lo[k]) * p[3] != p[k];
    return {lo, hi};
}

struct Surface {
    struct Node {
        Bounds box;

        std::size_t begin;
        std::size_t end;
        std::size_t right{};
    };

    std::vector<Face> faces;

    std::vector<std::size_t> order;

    std::vector<Node> tree;

    std::size_t build(std::size_t begin, std::size_t end) {
        auto box = faces[order[begin]].box;
        for (auto i = begin + 1; i < end; ++i)
            box.include(faces[order[i]].box);
        const auto id = tree.size();
        tree.push_back({box, begin, end});
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
        }
        return id;
    }

    template <class F>
    void query(std::size_t id, const Bounds& box, int axis, F& visit) const {
        const auto& node = tree[id];
        if (!node.box.overlaps(box, axis))
            return;
        if (node.right) {
            query(id + 1, box, axis, visit);
            query(node.right, box, axis, visit);
        } else {
            for (auto i = node.begin; i < node.end; ++i) {
                const auto& f = faces[order[i]];
                if (f.box.overlaps(box, axis))
                    visit(f);
            }
        }
    }
};

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

Result<Surface> make_surface(const Geometry& mesh, const Lattice& grid, unsigned operand) {
    Surface result;
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
            auto degenerate = [&] {
                return failure("Boolean operand " + std::to_string(operand) + ", primitive " +
                               std::to_string(primitive) + " is degenerate on the common 25-bit grid.");
            };
            if (support.n == GridPoint{})
                return degenerate();
            auto add_face = [&](std::vector<std::uint32_t> ids, Plane plane) -> Result<void> {
                if (plane.n == GridPoint{})
                    return degenerate();
                auto position = [&](auto c) { return math::Vector3d(mesh.positions()[mesh.vertex_points()[c]]); };
                math::Vector3d area{};
                const auto origin = position(ids[0]);
                for (std::size_t i = 1; i + 1 < ids.size(); ++i)
                    area += math::cross(position(ids[i]) - origin, position(ids[i + 1]) - origin);
                if (area.x * double(plane.n[0]) + area.y * double(plane.n[1]) + area.z * double(plane.n[2]) <= 0)
                    return failure("Boolean quantization reversed a source face.");
                const int axis = plane.axis();
                const auto& start = point(ids.front());
                Face f{{}, plane, {start, start}, primitive, std::move(ids), operand};
                f.cell.reserve(f.corners.size());
                auto prev = f.corners.back();
                for (auto c : f.corners) {
                    auto p = point(c);
                    const auto edge = edge_plane(point(prev), p, axis);
                    if (edge.n == GridPoint{})
                        return degenerate();
                    f.cell.push_back({homogeneous(p), plane.n[axis] < 0 ? edge.flipped() : edge});
                    f.box.include({p, p});
                    prev = c;
                }
                for (std::size_t i = 0; i < f.cell.size(); ++i)
                    if (f.cell[i].incoming.side(f.cell[(i + 1) % f.cell.size()].p) < 0)
                        return failure("Boolean quantization reversed a polygon corner.");
                result.faces.push_back(std::move(f));
                return {};
            };
            // Preserve the declared fan so cuts also retain its piecewise-linear
            // corner attributes. A planar face with a collinear fan piece must
            // retain the intermediate boundary corner and its authored value.
            if (collinear_fan &&
                std::ranges::all_of(corners, [&](auto c) { return !support.side(homogeneous(point(c))); })) {
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
        result.order.resize(result.faces.size());
        std::iota(result.order.begin(), result.order.end(), std::size_t{});
        result.build(0, result.order.size());
    }
    return result;
}

// Traverse the boundary once for both children. Each new corner remains the
// intersection of three original support planes, independent of clipping depth.
void divide(std::vector<Cell>& cells, const Plane& face, const Plane& cut) {
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
        if (!(pos && neg))
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
        cells[i] = std::move(above);
        cells.push_back(std::move(below));
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

bool crosses_interior(const Face& f, const Face& g) {
    // Clip the infinite line to the convex boundary of g. Collinear corners can
    // lie on the line: retaining its two extremes also handles a grazing edge.
    Line line(f.plane, g.plane);
    Point low{};
    Point high{};
    bool found = false;
    auto include = [&](const Point& p) {
        if (!found || compare(p, low, line.axis) < 0)
            low = p;
        if (!found || compare(p, high, line.axis) > 0)
            high = p;
        found = true;
    };
    int prev = f.plane.side(g.cell.back().p);
    for (const auto& c : g.cell) {
        int side = f.plane.side(c.p);
        if (!side)
            include(c.p);
        if (prev * side < 0)
            include(intersection(f.plane, g.plane, c.incoming));
        prev = side;
    }
    if (!found || !compare(low, high, line.axis))
        return false;
    for (const auto& c : f.cell) {
        const auto a = c.incoming.side(low);
        const auto b = c.incoming.side(high);
        if (a <= 0 && b <= 0)
            return false;
        if (a * b < 0)
            (a < 0 ? low : high) = intersection(f.plane, g.plane, c.incoming);
    }
    return compare(low, high, line.axis) < 0;
}

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

    Cell cell;

    bool reverse;
};

struct ProjectedEdge {
    std::array<Offset, 3> n;

    Wide d;

    int side(const Point& p) const {
        return sign(Product(n[0]) * p[0] + Product(n[1]) * p[1] + Product(n[2]) * p[2] + Product(d) * p[3]);
    }
};

struct Crossing {
    const Face* face;

    std::vector<ProjectedEdge> edges;

    int direction;
};

// A positive convex combination p0 + e*p1 + e^2*p2 + ... (normalized)
// represents a point strictly inside the cell for infinitesimal positive e.
// Evaluating a plane there needs only the first nonzero corner sign.
template <class P>
int interior_side(const Cell& c, const P& p) {
    for (const auto& v : c)
        if (auto s = p.side(v.p))
            return s;
    return 0;
}

Result<void> arrange(const Surface& source, const Surface& other, BooleanOperation op, std::vector<Patch>& patches) {
    for (const auto& f : source.faces) {
        const int axis = f.plane.axis();
        std::vector<Cell> cells{f.cell};
        std::vector<Crossing> rays;
        auto visit = [&](const Face& g) {
            if (const int dir = sign(g.plane.n[axis])) {
                Crossing ray{&g, {}, dir};
                for (const auto& c : g.cell) {
                    // Eliminate the ray coordinate between the face and edge
                    // equations. The resulting test is positive inside.
                    const auto& e = c.incoming;
                    const auto& p = g.plane;
                    ProjectedEdge test{};
                    for (int k = 0; k < 3; ++k)
                        test.n[k] = dir * (Offset(e.n[k]) * p.n[axis] - Offset(p.n[k]) * e.n[axis]);
                    test.d = dir * (Wide(e.d) * p.n[axis] - Wide(p.d) * e.n[axis]);
                    ray.edges.push_back(test);
                }
                rays.push_back(std::move(ray));
            }
            if (!g.box.overlaps(f.box, -1))
                return;
            const auto [lo, hi] = sides(f.cell, g.plane);
            if (!lo && !hi && coplanar_overlap(f, g)) {
                for (const auto& c : g.cell)
                    divide(cells, f.plane, c.incoming);
            } else if (lo < 0 && hi > 0) {
                if (crosses_interior(f, g))
                    divide(cells, f.plane, g.plane);
            }
        };
        if (!other.tree.empty())
            other.query(0, f.box, axis, visit);
        const auto first_patch = patches.size();
        for (auto& c : cells) {
            int above = 0;
            int at = 0;
            for (const auto& ray : rays) {
                if (std::ranges::any_of(ray.edges,
                                        [&](const ProjectedEdge& edge) { return interior_side(c, edge) < 0; }))
                    continue;
                const int side = interior_side(c, ray.face->plane);
                if (side == -ray.direction)
                    above += ray.direction;
                if (!side)
                    at += ray.direction;
            }
            // Along +axis, strict crossings classify immediately above this
            // boundary. Including t=0 crossings classifies immediately below.
            if (at && f.operand == 1)
                continue; // A owns coincident boundaries.
            int outside = above;
            int inside = above + at;
            if (f.plane.n[axis] < 0)
                std::swap(outside, inside);
            const bool minus = f.operand ? solid(inside > 0, true, op) : solid(true, inside > 0, op);
            const bool plus = f.operand ? solid(outside > 0, false, op) : solid(false, outside > 0, op);
            if (minus != plus)
                patches.push_back({&f, std::move(c), plus});
        }
        if (cells.size() > 1 && patches.size() - first_patch == cells.size()) {
            const bool reversed = patches[first_patch].reverse;
            if (std::all_of(patches.begin() + first_patch, patches.end(),
                            [&](const Patch& p) { return p.reverse == reversed; })) {
                patches.resize(first_patch);
                patches.push_back({&f, f.cell, reversed});
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
    std::map<Point, std::uint32_t, PointLess> index;

    std::vector<Point> points;

    std::map<Line, std::vector<std::uint32_t>> lines;

    std::vector<Piece> pieces;

    Result<void> sew(const std::vector<Patch>& patches, const Lattice& grid) {
        for (const auto& patch : patches) {
            const auto& c = patch.cell;
            for (std::size_t i = 0; i < c.size(); ++i) {
                const auto& next = c[(i + 1) % c.size()];
                auto [it, added] = index.try_emplace(c[i].p, static_cast<std::uint32_t>(points.size()));
                if (added) {
                    if (points.size() >= element_limit)
                        return failure("Boolean point limit exceeded.");
                    points.push_back(c[i].p);
                }
                // Both incident support lines need this endpoint, including when
                // it is a T-junction on the opposite face.
                lines[Line(patch.face->plane, c[i].incoming)].push_back(it->second);
                lines[Line(patch.face->plane, next.incoming)].push_back(it->second);
            }
        }
        for (auto& [line, ids] : lines) {
            std::ranges::sort(ids, [&](auto a, auto b) { return compare(points[a], points[b], line.axis) < 0; });
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        for (const auto& patch : patches) {
            Piece piece{patch.face, {}, {}, patch.reverse};
            const auto& c = patch.cell;
            for (std::size_t i = 0; i < c.size(); ++i) {
                const auto& next = c[(i + 1) % c.size()];
                Line key(patch.face->plane, next.incoming);
                const auto& ids = lines.find(key)->second;
                auto at = [&](const Point& p) {
                    return std::lower_bound(ids.begin(), ids.end(), p, [&](auto id, const Point& q) {
                        return compare(points[id], q, key.axis) < 0;
                    });
                };
                auto start = at(c[i].p);
                auto end = at(next.p);
                for (auto it = start; it != end; start < end ? ++it : --it) {
                    piece.vertices.push_back(*it);
                    piece.incoming.push_back(next.incoming); // Outgoing until the loop is complete.
                }
            }
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
            auto admissible = [&](std::size_t start) {
                const auto& a = points[piece.vertices[start]];
                const auto p = math::Vector3d(grid.decode(a));
                for (std::size_t i = 1; i + 1 < n; ++i) {
                    const auto& b = points[piece.vertices[(start + i) % n]];
                    const auto& c = points[piece.vertices[(start + i + 1) % n]];
                    const auto q = math::Vector3d(grid.decode(b));
                    const auto r = math::Vector3d(grid.decode(c));
                    const auto area = direction * math::cross(q - p, r - p)[axis];
                    if (area < 0 || (area == 0 && determinant(a, b, c, axis)))
                        return false;
                }
                return true;
            };
            std::size_t start = 0;
            while (start < n && !admissible(start))
                ++start;
            if (start == n)
                return failure("Boolean operand " + std::to_string(patch.face->operand) + ", primitive " +
                               std::to_string(patch.face->primitive) + " has no representable float triangle fan.");
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
        if (!compare(f.cell[i].p, p, 0) && !compare(f.cell[i].p, p, 1) && !compare(f.cell[i].p, p, 2))
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

template <class T>
T interpolate(const Array<T>& values, const std::array<std::uint32_t, 3>& ids, const Sample& s, Usage usage) {
    if constexpr (std::same_as<T, float> || std::same_as<T, math::Vector2> || std::same_as<T, math::Vector3> ||
                  std::same_as<T, math::Vector4>) {
        if constexpr (std::same_as<T, float>)
            return static_cast<float>(values[ids[0]] * s.weights[0] + values[ids[1]] * s.weights[1] +
                                      values[ids[2]] * s.weights[2]);
        else {
            T v{};
            for (std::size_t k = 0; k < T::dimensions; ++k)
                v[k] = static_cast<float>(values[ids[0]][k] * s.weights[0] + values[ids[1]][k] * s.weights[1] +
                                          values[ids[2]][k] * s.weights[2]);
            if constexpr (std::same_as<T, math::Vector3>) {
                if (usage == Usage::normal) {
                    v = math::try_normalize(v).value_or(T{});
                    if (s.piece->reverse)
                        v = -v;
                }
            }
            return v;
        }
    } else {
        auto best = 0;
        for (int i = 1; i < 3; ++i)
            if (s.weights[i] > s.weights[best] || (s.weights[i] == s.weights[best] && ids[i] < ids[best]))
                best = i;
        return values[ids[best]];
    }
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
                            values.add(data[f.operand] ? interpolate(*data[f.operand], ids, sample, attr.usage) : T{});
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
    std::array<std::optional<Array<std::uint32_t>>, 2> shell_ids;
    for (std::size_t i = 0; i < edges.size();) {
        auto end = i + 1;
        while (end < edges.size() && edges[i].lo == edges[end].lo && edges[i].hi == edges[end].hi)
            ++end;
        if (end - i == 2) {
            ATELIER_TRY(pair(edges[i].corner, edges[i + 1].corner));
        } else if (end - i >= 4 && (end - i) % 2 == 0) {
            // Group a tangential contact by the actual input shell. This
            // also preserves shells meeting along an edge on a later cook.
            std::map<std::pair<unsigned, std::uint32_t>, std::vector<std::uint32_t>> uses;
            for (auto j = i; j < end; ++j) {
                const auto c = edges[j].corner;
                const auto& f = *pieces[owners[c]].face;
                const auto& input = *inputs[f.operand];
                auto& ids = shell_ids[f.operand];
                if (!ids)
                    ids = detail::point_pieces(input);
                uses[{f.operand, (*ids)[input.vertex_points()[f.corners[0]]]}].push_back(c);
            }
            for (const auto& [key, use] : uses) {
                if (use.size() != 2)
                    return failure("Boolean boundary has an ambiguous non-manifold edge.");
                ATELIER_TRY(pair(use[0], use[1]));
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
    std::vector<math::Vector3> decoded;
    decoded.reserve(mesh.points.size());
    for (const auto& p : mesh.points)
        decoded.push_back(grid.decode(p));
    auto boundary = std::make_shared<BooleanBoundary>();
    boundary->lattice = grid;
    boundary->faces.reserve(pieces.size());
    boundary->positions.reserve(mesh.points.size());
    boundary->incoming.reserve(edges.size());
    for (const auto& piece : pieces) {
        boundary->faces.push_back(piece.reverse ? piece.face->plane.flipped() : piece.face->plane);
        for (std::size_t i = 0; i < piece.vertices.size(); ++i) {
            const auto v = piece.vertices[i];
            const auto following = piece.vertices[(i + 1) % piece.vertices.size()];
            if (decoded[v] == decoded[following])
                return failure("Boolean boundary edge collapses at float P precision.");
            boundary->incoming.push_back(piece.incoming[i]);
            ATELIER_TRY_BIND(value, sample(piece, mesh.points[v]));
            const auto corner = static_cast<std::uint32_t>(vertices.size());
            samples.push_back(*value);
            auto& point = root_ids[fans.root(corner)];
            if (point == UINT32_MAX) {
                point = static_cast<std::uint32_t>(positions.size());
                positions.add(decoded[v]);
                boundary->positions.push_back(mesh.points[v]);
                donors.push_back(corner);
            }
            vertices.add(point);
        }
    }
    ATELIER_TRY_BIND(result, Geometry::polygons(std::move(positions), std::move(vertices), offsets));
    ATELIER_TRY(detail::validate_shells(*result));
    ATELIER_TRY(attributes(*result, inputs, pieces, samples, donors));
    BooleanBoundary::attach(*result, std::move(boundary));
    return std::move(*result);
}
}

Result<Geometry> solid_boolean(const Geometry& a, const Geometry& b, BooleanOperation operation) {
    if (operation != BooleanOperation::unite && operation != BooleanOperation::intersect &&
        operation != BooleanOperation::subtract)
        return failure("Unknown Boolean operation.");
    ATELIER_TRY_BIND(grid, lattice(a, b));
    ATELIER_TRY_BIND(left, make_surface(a, *grid, 0));
    ATELIER_TRY_BIND(right, make_surface(b, *grid, 1));
    std::vector<Patch> patches;
    ATELIER_TRY(arrange(*left, *right, operation, patches));
    ATELIER_TRY(arrange(*right, *left, operation, patches));
    MeshBuilder mesh;
    ATELIER_TRY(mesh.sew(patches, *grid));
    return publish(mesh, *grid, {&a, &b});
}
}
