#include "geometry/meshlet.hpp"
#include "foundation/sha256.hpp"
#include "memory/fatal.hpp"
#include "platform/windows_path.hpp"

#include <meshoptimizer.h>
#include <mimalloc.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <compare>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <thread>
#include <utility>

namespace atelier::geometry {
namespace {

constexpr std::uint32_t amsh_magic = 0x48534D41u; // 'AMSH'

constexpr std::uint32_t amsh_version = 1u;

std::atomic<std::uint64_t> g_amsh_tmp_seq{0u};

struct AmshHeader {
    std::uint32_t magic{};

    std::uint32_t version{};

    std::uint32_t lod_count{};

    std::uint32_t reserved{};

    std::uint64_t position_count{};

    std::uint64_t meshlet_count{};

    std::uint64_t meshlet_vert_count{};

    std::uint64_t meshlet_tri_bytes{};

    std::uint64_t blas_index_count{};

    std::uint64_t face_meta_count{};

    std::uint64_t lod_part_count{};

    std::array<MeshLodLevel, max_mesh_lods> lods{};
};

static_assert(sizeof(Vertex) == 48 && std::is_trivially_copyable_v<Vertex>);

static_assert(sizeof(MeshPart) == 12 && std::is_trivially_copyable_v<MeshPart>);

static_assert(sizeof(PartRange) == 12 && std::is_trivially_copyable_v<PartRange>);

static_assert(sizeof(MeshLodLevel) == 28 && std::is_trivially_copyable_v<MeshLodLevel>);

static_assert(sizeof(AmshHeader) == 184 && std::is_trivially_copyable_v<AmshHeader>);

void ensure_meshopt_allocator() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        meshopt_setAllocator(
            [](std::size_t size) -> void* {
                void* p = mi_malloc(std::max<std::size_t>(size, 1u));
                if (!p)
                    atelier::fatal("meshoptimizer host memory allocation exhausted");
                return p;
            },
            [](void* p) { mi_free(p); });
    });
}

float canonical_coord(float v) noexcept {
    return (v == 0.0f) ? 0.0f : v;
}

std::pair<float, float> encode_octahedral(float x, float y, float z) noexcept {
    const float inv_l1 = 1.0f / std::max(std::abs(x) + std::abs(y) + std::abs(z), 1e-12f);
    float ox = x * inv_l1;
    float oy = y * inv_l1;
    if (z < 0.0f) {
        const float tx = (1.0f - std::abs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        const float ty = (1.0f - std::abs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox = tx;
        oy = ty;
    }
    return {std::clamp(ox, -1.0f, 1.0f), std::clamp(oy, -1.0f, 1.0f)};
}

std::array<float, 3> decode_octahedral(float ox, float oy) noexcept {
    const float nz = 1.0f - std::abs(ox) - std::abs(oy);
    const float t = std::max(-nz, 0.0f);
    const float nx = ox + ((ox >= 0.0f) ? -t : t);
    const float ny = oy + ((oy >= 0.0f) ? -t : t);
    const float len = std::sqrt(std::max(nx * nx + ny * ny + nz * nz, 1e-24f));
    return {nx / len, ny / len, nz / len};
}

bool is_winding_flipped(const Vertex& v0, const Vertex& v1, const Vertex& v2) noexcept {
    const double ax = double(v1.position[0]) - v0.position[0];
    const double ay = double(v1.position[1]) - v0.position[1];
    const double az = double(v1.position[2]) - v0.position[2];
    const double bx = double(v2.position[0]) - v0.position[0];
    const double by = double(v2.position[1]) - v0.position[1];
    const double bz = double(v2.position[2]) - v0.position[2];
    const double cx = ay * bz - az * by;
    const double cy = az * bx - ax * bz;
    const double cz = ax * by - ay * bx;
    const double orient = cx * (double(v0.normal[0]) + v1.normal[0] + v2.normal[0]) +
                          cy * (double(v0.normal[1]) + v1.normal[1] + v2.normal[1]) +
                          cz * (double(v0.normal[2]) + v1.normal[2] + v2.normal[2]);
    return orient < 0.0;
}

// Per-connected-component closed 2-manifold check on position-welded LOD0 triangles:
// 1. Orients each triangle's half-edges consistently with its authored outward normal (`winding_flipped`).
// 2. Partitions welded vertices into connected components (shells) so an open leaf card never invalidates a closed trunk.
// 3. Verifies exact opposite half-edge pairing AND strictly positive enclosed 3D volume (rejecting zero-volume back-to-back cards).
std::vector<std::uint8_t> compute_pos_shell_closed(std::span<const PackedPosition> unique_pos,
                                                   std::span<const Vertex> unique_verts,
                                                   std::span<const unsigned int> pos_remap,
                                                   std::span<const unsigned int> lod0_indices) {
    const std::size_t unique_count = unique_pos.size();
    const std::size_t tri_count = lod0_indices.size() / 3u;
    std::vector<std::uint8_t> pos_closed(unique_count, 0u);
    if (unique_count == 0u || tri_count < 4u)
        return pos_closed;

    std::vector<std::uint32_t> dsu(unique_count);
    std::iota(dsu.begin(), dsu.end(), 0u);
    auto find_root = [&](std::uint32_t x) noexcept {
        std::uint32_t r = x;
        while (dsu[r] != r)
            r = dsu[r];
        while (dsu[x] != r) {
            const std::uint32_t next = dsu[x];
            dsu[x] = r;
            x = next;
        }
        return r;
    };
    auto unite = [&](std::uint32_t a, std::uint32_t b) noexcept {
        const std::uint32_t ra = find_root(a);
        const std::uint32_t rb = find_root(b);
        if (ra != rb)
            dsu[rb] = ra;
    };

    struct OrientedTri {
        std::uint32_t p0;

        std::uint32_t p1;

        std::uint32_t p2;
    };

    std::vector<OrientedTri> tris;
    tris.reserve(tri_count);
    for (std::size_t t = 0; t < tri_count; ++t) {
        const unsigned int u0 = lod0_indices[t * 3u + 0u];
        const unsigned int u1 = lod0_indices[t * 3u + 1u];
        const unsigned int u2 = lod0_indices[t * 3u + 2u];
        std::uint32_t p0 = pos_remap[u0];
        std::uint32_t p1 = pos_remap[u1];
        std::uint32_t p2 = pos_remap[u2];
        if (p0 == p1 || p1 == p2 || p2 == p0)
            continue;
        if (is_winding_flipped(unique_verts[u0], unique_verts[u1], unique_verts[u2]))
            std::swap(p1, p2);
        unite(p0, p1);
        unite(p1, p2);
        tris.push_back({p0, p1, p2});
    }
    if (tris.size() < 4u)
        return pos_closed;

    struct ShellEdge {
        std::uint32_t comp;

        std::uint32_t u;

        std::uint32_t v;

        auto operator<=>(const ShellEdge&) const noexcept = default;
    };

    struct ShellStats {
        double six_vol = 0.0;

        float min_p[3]{1e30f, 1e30f, 1e30f};

        float max_p[3]{-1e30f, -1e30f, -1e30f};

        std::uint32_t tri_cnt = 0u;

        bool valid_manifold = true;
    };

    std::vector<ShellEdge> fwd;
    std::vector<ShellEdge> rev;
    fwd.reserve(tris.size() * 3u);
    rev.reserve(tris.size() * 3u);
    std::vector<ShellStats> stats(unique_count);

    for (const OrientedTri& tr : tris) {
        const std::uint32_t comp = find_root(tr.p0);
        ShellStats& st = stats[comp];
        ++st.tri_cnt;

        const PackedPosition& origin = unique_pos[comp];
        const PackedPosition& a = unique_pos[tr.p0];
        const PackedPosition& b = unique_pos[tr.p1];
        const PackedPosition& c = unique_pos[tr.p2];
        const PackedPosition* const tri_pts[3] = {&a, &b, &c};
        for (const PackedPosition* vp : tri_pts) {
            st.min_p[0] = std::min(st.min_p[0], vp->x);
            st.min_p[1] = std::min(st.min_p[1], vp->y);
            st.min_p[2] = std::min(st.min_p[2], vp->z);
            st.max_p[0] = std::max(st.max_p[0], vp->x);
            st.max_p[1] = std::max(st.max_p[1], vp->y);
            st.max_p[2] = std::max(st.max_p[2], vp->z);
        }
        const double ax = double(a.x) - origin.x;
        const double ay = double(a.y) - origin.y;
        const double az = double(a.z) - origin.z;
        const double bx = double(b.x) - origin.x;
        const double by = double(b.y) - origin.y;
        const double bz = double(b.z) - origin.z;
        const double cx = double(c.x) - origin.x;
        const double cy = double(c.y) - origin.y;
        const double cz = double(c.z) - origin.z;
        st.six_vol += ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx);

        fwd.push_back({comp, tr.p0, tr.p1});
        fwd.push_back({comp, tr.p1, tr.p2});
        fwd.push_back({comp, tr.p2, tr.p0});
        rev.push_back({comp, tr.p1, tr.p0});
        rev.push_back({comp, tr.p2, tr.p1});
        rev.push_back({comp, tr.p0, tr.p2});
    }

    std::ranges::sort(fwd);
    std::ranges::sort(rev);

    for (std::size_t i = 0; i < fwd.size(); ++i) {
        if (i > 0u && fwd[i] == fwd[i - 1u])
            stats[fwd[i].comp].valid_manifold = false;
    }

    std::size_t fi = 0u;
    std::size_t ri = 0u;
    while (fi < fwd.size() || ri < rev.size()) {
        if (fi < fwd.size() && ri < rev.size() && fwd[fi] == rev[ri]) {
            ++fi;
            ++ri;
        } else if (ri >= rev.size() || (fi < fwd.size() && fwd[fi] < rev[ri])) {
            stats[fwd[fi].comp].valid_manifold = false;
            ++fi;
        } else {
            stats[rev[ri].comp].valid_manifold = false;
            ++ri;
        }
    }

    for (std::size_t i = 0; i < unique_count; ++i) {
        const std::uint32_t comp = find_root(static_cast<std::uint32_t>(i));
        const ShellStats& st = stats[comp];
        if (!st.valid_manifold || st.tri_cnt < 4u)
            continue;
        const double dx = double(st.max_p[0]) - st.min_p[0];
        const double dy = double(st.max_p[1]) - st.min_p[1];
        const double dz = double(st.max_p[2]) - st.min_p[2];
        const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (diag > 1e-6 && std::abs(st.six_vol) > 1e-7 * diag * diag * diag)
            pos_closed[i] = 1u;
    }
    return pos_closed;
}

struct TriKey {
    std::uint32_t a{};

    std::uint32_t b{};

    std::uint32_t c{};

    std::uint8_t mask{};

    std::strong_ordering operator<=>(const TriKey& o) const noexcept {
        if (a != o.a)
            return a <=> o.a;
        if (b != o.b)
            return b <=> o.b;
        return c <=> o.c;
    }

    bool operator==(const TriKey& o) const noexcept { return a == o.a && b == o.b && c == o.c; }
};

struct BucketLodOutput {
    std::vector<GpuMeshlet> meshlets;

    std::vector<std::uint32_t> meshlet_vertices;

    std::vector<std::uint8_t> meshlet_triangles;

    std::vector<std::uint32_t> blas_indices;

    std::vector<std::uint32_t> face_meta;

    std::size_t simplified_idx_count{};

    float world_error{};
};

BucketLodOutput
build_bucket_meshlets(std::span<const Vertex> unique_verts, std::span<const unsigned int> pos_remap,
                      std::span<const std::uint8_t> pos_closed, std::span<const PackedPosition> local_pos,
                      std::span<const std::uint32_t> local_to_global, std::span<const unsigned int> local_indices,
                      std::span<const TriKey> sorted_edge_lut, std::uint32_t material_slot, std::uint32_t lod_level) {
    BucketLodOutput out;
    if (local_indices.empty() || local_pos.empty())
        return out;

    std::vector<unsigned int> opt_indices(local_indices.size());
    meshopt_optimizeVertexCache(opt_indices.data(), local_indices.data(), local_indices.size(), local_pos.size());

    const std::size_t max_m =
        meshopt_buildMeshletsBound(opt_indices.size(), meshlet_max_vertices, meshlet_max_triangles);
    std::vector<meshopt_Meshlet> raw_meshlets(max_m);
    std::vector<unsigned int> ml_verts(opt_indices.size());
    std::vector<unsigned char> ml_tris(opt_indices.size() + max_m * 3u);

    const std::size_t m_count = meshopt_buildMeshlets(
        raw_meshlets.data(), ml_verts.data(), ml_tris.data(), opt_indices.data(), opt_indices.size(), &local_pos[0].x,
        local_pos.size(), sizeof(PackedPosition), meshlet_max_vertices, meshlet_max_triangles, 0.25f);
    if (m_count == 0u)
        return out;

    out.meshlets.reserve(m_count);
    out.meshlet_vertices.reserve(m_count * meshlet_max_vertices);
    out.meshlet_triangles.reserve(opt_indices.size() + m_count * 4u);
    out.blas_indices.reserve(opt_indices.size());
    out.face_meta.reserve(opt_indices.size() / 3u);

    for (std::size_t m = 0; m < m_count; ++m) {
        const meshopt_Meshlet& src = raw_meshlets[m];
        if (src.triangle_count == 0u || src.vertex_count == 0u)
            continue;

        unsigned int* v_slice = ml_verts.data() + src.vertex_offset;
        unsigned char* t_slice = ml_tris.data() + src.triangle_offset;
        meshopt_optimizeMeshlet(v_slice, t_slice, src.triangle_count, src.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            v_slice, t_slice, src.triangle_count, &local_pos[0].x, local_pos.size(), sizeof(PackedPosition));

        for (unsigned int v = 0u; v < src.vertex_count; ++v)
            v_slice[v] = local_to_global[v_slice[v]];

        const auto v_off = static_cast<std::uint32_t>(out.meshlet_vertices.size());
        out.meshlet_vertices.insert(out.meshlet_vertices.end(), v_slice, v_slice + src.vertex_count);

        while ((out.meshlet_triangles.size() & 3u) != 0u)
            out.meshlet_triangles.push_back(0u);
        const auto t_off = static_cast<std::uint32_t>(out.meshlet_triangles.size());
        const std::size_t tri_bytes = std::size_t(src.triangle_count) * 3u;
        out.meshlet_triangles.insert(out.meshlet_triangles.end(), t_slice, t_slice + tri_bytes);
        while ((out.meshlet_triangles.size() & 3u) != 0u)
            out.meshlet_triangles.push_back(0u);

        const auto local_tri_base = static_cast<std::uint32_t>(out.face_meta.size());
        std::array<std::array<float, 3>, meshlet_max_triangles> tri_normals{};
        double axis_sum_x = 0.0;
        double axis_sum_y = 0.0;
        double axis_sum_z = 0.0;
        for (unsigned int lt = 0; lt < src.triangle_count; ++lt) {
            const std::uint32_t gi0 = v_slice[t_slice[lt * 3u + 0u]];
            const std::uint32_t gi1 = v_slice[t_slice[lt * 3u + 1u]];
            const std::uint32_t gi2 = v_slice[t_slice[lt * 3u + 2u]];
            out.blas_indices.push_back(gi0);
            out.blas_indices.push_back(gi1);
            out.blas_indices.push_back(gi2);

            std::uint8_t mask = 7u;
            if (!sorted_edge_lut.empty()) {
                const TriKey probe{gi0, gi1, gi2, 7u};
                const auto it = std::ranges::lower_bound(sorted_edge_lut, probe);
                if (it != sorted_edge_lut.end() && it->a == gi0 && it->b == gi1 && it->c == gi2)
                    mask = it->mask;
            }
            const Vertex& v0 = unique_verts[gi0];
            const Vertex& v1 = unique_verts[gi1];
            const Vertex& v2 = unique_verts[gi2];
            const bool winding_flipped = is_winding_flipped(v0, v1, v2);
            const bool closed = (lod_level == 0u) && !pos_closed.empty() && (pos_closed[pos_remap[gi0]] != 0u);
            out.face_meta.push_back(pack_face_meta(material_slot, mask, closed, winding_flipped));

            // Exact `winding_flipped`-consistent local outward triangle normal matching `reconstruct_geom_normal`:
            const double e1x = double(v1.position[0]) - v0.position[0];
            const double e1y = double(v1.position[1]) - v0.position[1];
            const double e1z = double(v1.position[2]) - v0.position[2];
            const double e2x = double(v2.position[0]) - v0.position[0];
            const double e2y = double(v2.position[1]) - v0.position[1];
            const double e2z = double(v2.position[2]) - v0.position[2];
            const double esc1 = std::max(std::max(std::abs(e1x), std::abs(e1y)), std::abs(e1z));
            const double esc2 = std::max(std::max(std::abs(e2x), std::abs(e2y)), std::abs(e2z));
            const double esc = std::max(esc1, esc2);
            double nx = 0.0;
            double ny = 0.0;
            double nz = 0.0;
            if (esc > 0.0) {
                const double s1x = e1x / esc;
                const double s1y = e1y / esc;
                const double s1z = e1z / esc;
                const double s2x = e2x / esc;
                const double s2y = e2y / esc;
                const double s2z = e2z / esc;
                nx = s1y * s2z - s1z * s2y;
                ny = s1z * s2x - s1x * s2z;
                nz = s1x * s2y - s1y * s2x;
                if (winding_flipped) {
                    nx = -nx;
                    ny = -ny;
                    nz = -nz;
                }
            }
            double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (!(nlen > 0.0)) {
                nx = v0.normal[0];
                ny = v0.normal[1];
                nz = v0.normal[2];
                nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            }
            if (nlen > 0.0) {
                nx /= nlen;
                ny /= nlen;
                nz /= nlen;
            } else {
                nx = 0.0;
                ny = 0.0;
                nz = 1.0;
            }
            tri_normals[lt] = {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)};
            axis_sum_x += nx;
            axis_sum_y += ny;
            axis_sum_z += nz;
        }

        // Quantize cone axis to SNORM5 octahedral and measure `mindp` directly against the exact GPU-decoded axis!
        std::uint32_t cone_bits = 63u << 10u; // default cutoff_u6 = 63 (1.0 = cone cull disabled)
        const double axis_len = std::sqrt(axis_sum_x * axis_sum_x + axis_sum_y * axis_sum_y + axis_sum_z * axis_sum_z);
        if (axis_len > 1e-6 && src.triangle_count <= meshlet_max_triangles) {
            const auto [ox, oy] =
                encode_octahedral(static_cast<float>(axis_sum_x / axis_len), static_cast<float>(axis_sum_y / axis_len),
                                  static_cast<float>(axis_sum_z / axis_len));
            const auto uox = static_cast<std::uint32_t>(std::clamp<int>(int(std::lrint(ox * 15.0f)) + 15, 0, 30));
            const auto uoy = static_cast<std::uint32_t>(std::clamp<int>(int(std::lrint(oy * 15.0f)) + 15, 0, 30));
            const auto q_axis =
                decode_octahedral(float(int(uox) - 15) * (1.0f / 15.0f), float(int(uoy) - 15) * (1.0f / 15.0f));
            float mindp = 1.0f;
            for (unsigned int lt = 0; lt < src.triangle_count; ++lt) {
                const float dp =
                    tri_normals[lt][0] * q_axis[0] + tri_normals[lt][1] * q_axis[1] + tri_normals[lt][2] * q_axis[2];
                mindp = std::min(mindp, dp);
            }
            if (mindp > 0.05f) {
                const float sin_half = std::sqrt(std::max(0.0f, 1.0f - mindp * mindp));
                const auto cutoff_u6 =
                    std::min<std::uint32_t>(63u, static_cast<std::uint32_t>(std::ceil(sin_half * 63.0f)));
                cone_bits = (uox & 31u) | ((uoy & 31u) << 5u) | ((cutoff_u6 & 63u) << 10u);
            }
        }

        GpuMeshlet dst{};
        dst.vertex_offset = v_off;
        dst.triangle_offset = t_off;
        dst.global_tri_base = local_tri_base;
        dst.counts = (src.vertex_count & 0xFFu) | ((src.triangle_count & 0xFFu) << 8u) | (cone_bits << 16u);
        dst.center[0] = bounds.center[0];
        dst.center[1] = bounds.center[1];
        dst.center[2] = bounds.center[2];
        dst.radius = bounds.radius;
        out.meshlets.push_back(dst);
    }

    return out;
}

void append_bucket_output(CookedMeshlets& cooked, BucketLodOutput& bo, std::uint32_t material_slot) {
    if (bo.meshlets.empty() || bo.face_meta.empty())
        return;

    const auto v_base = static_cast<std::uint32_t>(cooked.meshlet_vertices.size());
    while ((cooked.meshlet_triangles.size() & 3u) != 0u)
        cooked.meshlet_triangles.push_back(0u);
    const auto t_base = static_cast<std::uint32_t>(cooked.meshlet_triangles.size());
    const auto tri_base = static_cast<std::uint32_t>(cooked.face_meta.size());

    cooked.meshlet_vertices.insert(cooked.meshlet_vertices.end(), bo.meshlet_vertices.begin(),
                                   bo.meshlet_vertices.end());
    cooked.meshlet_triangles.insert(cooked.meshlet_triangles.end(), bo.meshlet_triangles.begin(),
                                    bo.meshlet_triangles.end());
    cooked.blas_indices.insert(cooked.blas_indices.end(), bo.blas_indices.begin(), bo.blas_indices.end());
    cooked.face_meta.insert(cooked.face_meta.end(), bo.face_meta.begin(), bo.face_meta.end());

    cooked.meshlets.reserve(cooked.meshlets.size() + bo.meshlets.size());
    for (GpuMeshlet m : bo.meshlets) {
        m.vertex_offset += v_base;
        m.triangle_offset += t_base;
        m.global_tri_base += tri_base;
        cooked.meshlets.push_back(m);
    }

    cooked.lod_parts.push_back(PartRange{
        .material_slot = material_slot,
        .global_tri_base = tri_base,
        .triangle_count = static_cast<std::uint32_t>(bo.face_meta.size()),
    });

    bo.meshlets.clear();
    bo.meshlets.shrink_to_fit();
    bo.meshlet_vertices.clear();
    bo.meshlet_vertices.shrink_to_fit();
    bo.meshlet_triangles.clear();
    bo.meshlet_triangles.shrink_to_fit();
    bo.blas_indices.clear();
    bo.blas_indices.shrink_to_fit();
    bo.face_meta.clear();
    bo.face_meta.shrink_to_fit();
}

CookedMeshletsRef build_cooked_meshlets(const Mesh& mesh) {
    ensure_meshopt_allocator();

    const std::span<const Vertex> raw_verts = mesh.vertices();
    const std::span<const MeshPart> parts = mesh.parts();
    const std::span<const std::uint8_t> raw_edge_masks = mesh.edge_masks();
    const std::size_t raw_count = raw_verts.size();

    auto cooked = std::make_shared<CookedMeshlets>();

    // Exact bitwise vertex deduplication preserving authored split normals/tangents/UVs (after canonicalizing -0.0f).
    std::vector<Vertex> clean_raw(raw_count);
    for (std::size_t i = 0; i < raw_count; ++i) {
        Vertex v = raw_verts[i];
        v.position[0] = canonical_coord(v.position[0]);
        v.position[1] = canonical_coord(v.position[1]);
        v.position[2] = canonical_coord(v.position[2]);
        clean_raw[i] = v;
    }

    std::vector<unsigned int> remap(raw_count);
    const std::size_t unique_count =
        meshopt_generateVertexRemap(remap.data(), nullptr, raw_count, clean_raw.data(), raw_count, sizeof(Vertex));

    std::vector<Vertex> unique_verts(unique_count);
    std::vector<unsigned int> lod0_indices(raw_count);
    meshopt_remapVertexBuffer(unique_verts.data(), clean_raw.data(), raw_count, sizeof(Vertex), remap.data());
    meshopt_remapIndexBuffer(lod0_indices.data(), nullptr, raw_count, remap.data());
    clean_raw.clear();
    clean_raw.shrink_to_fit();
    remap.clear();
    remap.shrink_to_fit();

    cooked->positions.resize(unique_count);
    cooked->attributes.resize(unique_count);
    for (std::size_t i = 0; i < unique_count; ++i) {
        const Vertex& v = unique_verts[i];
        cooked->positions[i] = {v.position[0], v.position[1], v.position[2]};
        cooked->attributes[i] = {
            .oct_normal = pack_oct_normal_snorm16(v.normal[0], v.normal[1], v.normal[2]),
            .oct_tangent_sign = pack_oct_tangent_sign(v.tangent[0], v.tangent[1], v.tangent[2], v.tangent[3]),
            .u = v.uv[0],
            .v = v.uv[1],
        };
    }

    // Coalesce authored MeshParts by `material` slot (preserving first-seen slot order) so DCC multi-submesh
    // assets like Sponza (405 parts / 28 materials) cluster, simplify, and build BLAS sub-geometries per material!
    struct MaterialBucket {
        std::uint32_t material_slot{};

        std::vector<unsigned int> lod0_indices;

        std::vector<std::uint8_t> lod0_edge_masks;
    };

    std::vector<MaterialBucket> buckets;
    buckets.reserve(std::min(parts.size(), mesh.materials().size()));
    std::vector<std::uint32_t> slot_to_bucket(mesh.materials().size(), UINT32_MAX);
    for (const MeshPart& part : parts) {
        if (part.vertex_count == 0u)
            continue;
        std::uint32_t b = slot_to_bucket[part.material];
        if (b == UINT32_MAX) {
            b = static_cast<std::uint32_t>(buckets.size());
            slot_to_bucket[part.material] = b;
            buckets.push_back(
                MaterialBucket{.material_slot = part.material, .lod0_indices = {}, .lod0_edge_masks = {}});
        }
        auto& dst = buckets[b];
        dst.lod0_indices.insert(dst.lod0_indices.end(), lod0_indices.data() + part.first_vertex,
                                lod0_indices.data() + part.first_vertex + part.vertex_count);
        if (!raw_edge_masks.empty()) {
            const std::size_t first_tri = part.first_vertex / 3u;
            const std::size_t tri_cnt = part.vertex_count / 3u;
            dst.lod0_edge_masks.insert(dst.lod0_edge_masks.end(), raw_edge_masks.data() + first_tri,
                                       raw_edge_masks.data() + first_tri + tri_cnt);
        }
    }

    // Single position-only remap shared by per-shell closed-manifold detection AND inter-material seam locking!
    std::vector<unsigned int> pos_remap(unique_count);
    const std::size_t unique_pos_count = meshopt_generateVertexRemap(
        pos_remap.data(), nullptr, unique_count, cooked->positions.data(), unique_count, sizeof(PackedPosition));
    std::vector<PackedPosition> welded_pos(unique_pos_count);
    meshopt_remapVertexBuffer(welded_pos.data(), cooked->positions.data(), unique_count, sizeof(PackedPosition),
                              pos_remap.data());

    const std::vector<std::uint8_t> pos_closed =
        compute_pos_shell_closed(welded_pos, unique_verts, pos_remap, lod0_indices);
    welded_pos.clear();
    welded_pos.shrink_to_fit();
    lod0_indices.clear();
    lod0_indices.shrink_to_fit();

    std::vector<std::uint32_t> pos_owner_bucket(unique_pos_count, UINT32_MAX);
    std::vector<std::uint8_t> pos_boundary(unique_pos_count, 0u);
    for (std::size_t b = 0; b < buckets.size(); ++b) {
        const auto b_id = static_cast<std::uint32_t>(b);
        for (const unsigned int u_idx : buckets[b].lod0_indices) {
            const unsigned int p_rep = pos_remap[u_idx];
            if (pos_owner_bucket[p_rep] == UINT32_MAX)
                pos_owner_bucket[p_rep] = b_id;
            else if (pos_owner_bucket[p_rep] != b_id)
                pos_boundary[p_rep] = 1u;
        }
    }
    pos_owner_bucket.clear();
    pos_owner_bucket.shrink_to_fit();

    const std::size_t total_lod0_tris = raw_count / 3u;
    const bool build_lods = (total_lod0_tris > meshlet_max_triangles);
    const float mesh_scale =
        build_lods ? meshopt_simplifyScale(&cooked->positions[0].x, unique_count, sizeof(PackedPosition)) : 0.0f;

    struct BucketAllLods {
        std::array<BucketLodOutput, max_mesh_lods> lods;
    };

    std::vector<BucketAllLods> bucket_results(buckets.size());

    auto cook_bucket = [&](std::size_t b) {
        const MaterialBucket& bucket = buckets[b];
        BucketAllLods& res = bucket_results[b];
        const std::size_t idx_count = bucket.lod0_indices.size();
        if (idx_count == 0u)
            return;

        // Compact local vertex table via power-of-2 open-addressed hash map: O(bucket_indices) time & memory.
        const std::size_t max_local_v = std::min(unique_count, idx_count);
        const std::size_t hash_cap = std::bit_ceil(std::max<std::size_t>(16u, max_local_v * 2u));
        const std::size_t hash_mask = hash_cap - 1u;
        std::vector<std::uint32_t> ht_keys(hash_cap, UINT32_MAX);
        std::vector<std::uint32_t> ht_vals(hash_cap, 0u);

        std::vector<std::uint32_t> local_to_global;
        std::vector<PackedPosition> local_pos;
        std::vector<unsigned char> local_lock;
        std::vector<unsigned int> local_lod0(idx_count);
        local_to_global.reserve(max_local_v);
        local_pos.reserve(max_local_v);
        if (build_lods)
            local_lock.reserve(max_local_v);

        for (std::size_t k = 0; k < idx_count; ++k) {
            const std::uint32_t gi = bucket.lod0_indices[k];
            std::size_t slot = (std::size_t(gi) * 2654435761u) & hash_mask;
            std::uint32_t li = 0u;
            for (;;) {
                const std::uint32_t cur = ht_keys[slot];
                if (cur == gi) {
                    li = ht_vals[slot];
                    break;
                }
                if (cur == UINT32_MAX) {
                    li = static_cast<std::uint32_t>(local_to_global.size());
                    ht_keys[slot] = gi;
                    ht_vals[slot] = li;
                    local_to_global.push_back(gi);
                    local_pos.push_back(cooked->positions[gi]);
                    if (build_lods)
                        local_lock.push_back(pos_boundary[pos_remap[gi]]);
                    break;
                }
                slot = (slot + 1u) & hash_mask;
            }
            local_lod0[k] = li;
        }
        ht_keys.clear();
        ht_keys.shrink_to_fit();
        ht_vals.clear();
        ht_vals.shrink_to_fit();

        std::vector<TriKey> edge_lut;
        if (!bucket.lod0_edge_masks.empty()) {
            const std::size_t src_tris = idx_count / 3u;
            edge_lut.reserve(src_tris);
            for (std::size_t t = 0; t < src_tris; ++t) {
                edge_lut.push_back({bucket.lod0_indices[t * 3u + 0u], bucket.lod0_indices[t * 3u + 1u],
                                    bucket.lod0_indices[t * 3u + 2u], bucket.lod0_edge_masks[t]});
            }
            std::ranges::sort(edge_lut);
        }

        res.lods[0] = build_bucket_meshlets(unique_verts, pos_remap, pos_closed, local_pos, local_to_global, local_lod0,
                                            edge_lut, bucket.material_slot, 0u);
        res.lods[0].simplified_idx_count = idx_count;
        edge_lut.clear();
        edge_lut.shrink_to_fit();

        if (!build_lods)
            return;

        const std::size_t local_v_count = local_to_global.size();
        std::vector<float> local_attr(local_v_count * 9u);
        for (std::size_t i = 0; i < local_v_count; ++i) {
            const Vertex& v = unique_verts[local_to_global[i]];
            local_attr[i * 9u + 0u] = v.normal[0];
            local_attr[i * 9u + 1u] = v.normal[1];
            local_attr[i * 9u + 2u] = v.normal[2];
            local_attr[i * 9u + 3u] = v.uv[0];
            local_attr[i * 9u + 4u] = v.uv[1];
            local_attr[i * 9u + 5u] = v.tangent[0];
            local_attr[i * 9u + 6u] = v.tangent[1];
            local_attr[i * 9u + 7u] = v.tangent[2];
            local_attr[i * 9u + 8u] = v.tangent[3];
        }

        constexpr float attr_weights[9] = {0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 1.0f};
        constexpr std::array<float, 3> target_ratios = {0.50f, 0.22f, 0.08f};
        constexpr std::array<float, 3> target_errors = {0.015f, 0.05f, 0.15f};
        const float local_scale = meshopt_simplifyScale(&local_pos[0].x, local_v_count, sizeof(PackedPosition));

        std::vector<unsigned int> simplified;
        for (std::uint32_t lod = 1u; lod < max_mesh_lods; ++lod) {
            if (idx_count <= 36u) {
                res.lods[lod] = build_bucket_meshlets(unique_verts, pos_remap, {}, local_pos, local_to_global,
                                                      local_lod0, {}, bucket.material_slot, lod);
                res.lods[lod].simplified_idx_count = idx_count;
                res.lods[lod].world_error = 0.0f;
                continue;
            }

            const float ratio = target_ratios[lod - 1u];
            const float max_world_err = target_errors[lod - 1u] * mesh_scale;
            const float local_target_err =
                (local_scale > 1e-12f) ? std::min(1.0f, max_world_err / local_scale) : target_errors[lod - 1u];
            const std::size_t target_idx_count =
                std::max<std::size_t>(12u, (std::size_t(double(idx_count) * ratio) / 3u) * 3u);

            simplified.resize(idx_count);
            float part_rel_err = 0.0f;
            const std::size_t got = meshopt_simplifyWithAttributes(
                simplified.data(), local_lod0.data(), idx_count, &local_pos[0].x, local_v_count, sizeof(PackedPosition),
                local_attr.data(), sizeof(float) * 9u, attr_weights, 9u, local_lock.data(), target_idx_count,
                local_target_err, meshopt_SimplifyLockBorder, &part_rel_err);
            simplified.resize(got);

            res.lods[lod] = build_bucket_meshlets(unique_verts, pos_remap, {}, local_pos, local_to_global, simplified,
                                                  {}, bucket.material_slot, lod);
            res.lods[lod].simplified_idx_count = got;
            res.lods[lod].world_error = part_rel_err * local_scale;
        }
    };

    if (buckets.size() > 1u && total_lod0_tris >= 4096u) {
        std::atomic<std::size_t> next_bucket{0u};
        const auto hw_threads = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
        const std::size_t worker_count = std::min(buckets.size(), std::min<std::size_t>(8u, hw_threads));
        auto worker_fn = [&] {
            for (;;) {
                const std::size_t b = next_bucket.fetch_add(1u, std::memory_order_relaxed);
                if (b >= buckets.size())
                    break;
                cook_bucket(b);
            }
        };
        std::vector<std::jthread> workers;
        workers.reserve(worker_count - 1u);
        [&]() noexcept {
            for (std::size_t t = 1u; t < worker_count; ++t)
                workers.emplace_back(worker_fn);
        }();
        worker_fn();
    } else {
        for (std::size_t b = 0; b < buckets.size(); ++b)
            cook_bucket(b);
    }

    unique_verts.clear();
    unique_verts.shrink_to_fit();
    pos_remap.clear();
    pos_remap.shrink_to_fit();
    for (auto& bucket : buckets) {
        bucket.lod0_indices.clear();
        bucket.lod0_indices.shrink_to_fit();
        bucket.lod0_edge_masks.clear();
        bucket.lod0_edge_masks.shrink_to_fit();
    }

    cooked->blas_indices.reserve(total_lod0_tris * 5u + 16u);
    cooked->face_meta.reserve(total_lod0_tris * 2u + 8u);

    // Deterministic merge of LOD 0 in material-bucket order.
    {
        MeshLodLevel& l0 = cooked->lods[0];
        l0.first_meshlet = 0u;
        l0.first_part = 0u;
        l0.first_global_tri = 0u;
        l0.error = 0.0f;
        for (std::size_t b = 0; b < buckets.size(); ++b)
            append_bucket_output(*cooked, bucket_results[b].lods[0], buckets[b].material_slot);
        l0.meshlet_count = static_cast<std::uint32_t>(cooked->meshlets.size());
        l0.part_count = static_cast<std::uint32_t>(cooked->lod_parts.size());
        l0.triangle_count = static_cast<std::uint32_t>(cooked->face_meta.size());
    }

    // Deterministic merge of Discrete Simplified Meshlet LODs (LOD 1..3).
    if (build_lods) {
        float prev_error = 0.0f;
        std::size_t prev_total = raw_count;
        for (std::uint32_t lod = 1u; lod < max_mesh_lods; ++lod) {
            std::size_t simplified_total_indices = 0u;
            std::size_t level_meshlet_count = 0u;
            float max_world_err = 0.0f;
            for (std::size_t b = 0; b < buckets.size(); ++b) {
                const BucketLodOutput& bo = bucket_results[b].lods[lod];
                simplified_total_indices += bo.simplified_idx_count;
                level_meshlet_count += bo.meshlets.size();
                max_world_err = std::max(max_world_err, bo.world_error);
            }
            if (simplified_total_indices == 0u || simplified_total_indices * 10u >= prev_total * 9u ||
                level_meshlet_count == 0u || level_meshlet_count > cooked->lods[0].meshlet_count) {
                break;
            }
            prev_total = simplified_total_indices;

            const float monotonic_err = std::max(prev_error + 1e-5f, max_world_err);
            prev_error = monotonic_err;

            const std::size_t saved_meshlets = cooked->meshlets.size();
            const std::size_t saved_parts = cooked->lod_parts.size();
            const std::size_t saved_faces = cooked->face_meta.size();

            MeshLodLevel& level = cooked->lods[cooked->lod_count];
            level.first_meshlet = static_cast<std::uint32_t>(saved_meshlets);
            level.first_part = static_cast<std::uint32_t>(saved_parts);
            level.first_global_tri = static_cast<std::uint32_t>(saved_faces);
            level.error = monotonic_err;

            for (std::size_t b = 0; b < buckets.size(); ++b)
                append_bucket_output(*cooked, bucket_results[b].lods[lod], buckets[b].material_slot);

            level.meshlet_count = static_cast<std::uint32_t>(cooked->meshlets.size() - saved_meshlets);
            level.part_count = static_cast<std::uint32_t>(cooked->lod_parts.size() - saved_parts);
            level.triangle_count = static_cast<std::uint32_t>(cooked->face_meta.size() - saved_faces);
            ++cooked->lod_count;
        }
    }

    cooked->meshlet_triangles.insert(cooked->meshlet_triangles.end(), 4u, 0u);
    return cooked;
}

std::string compute_mesh_cache_digest(const Mesh& mesh) noexcept {
    foundation::Sha256 sha;
    const std::uint32_t header_words[4] = {
        amsh_version,
        meshlet_max_vertices,
        meshlet_max_triangles,
        max_mesh_lods,
    };
    sha.add(atelier::as_bytes(Span{header_words}));

    const auto verts = mesh.vertices();
    const auto vert_count = static_cast<std::uint64_t>(verts.size());
    sha.add(atelier::as_bytes(Span{&vert_count, 1u}));
    sha.add(atelier::as_bytes(verts));

    const auto masks = mesh.edge_masks();
    const auto mask_count = static_cast<std::uint64_t>(masks.size());
    sha.add(atelier::as_bytes(Span{&mask_count, 1u}));
    if (!masks.empty())
        sha.add(atelier::as_bytes(Span{masks.data(), masks.size()}));

    const auto parts = mesh.parts();
    const auto part_count = static_cast<std::uint64_t>(parts.size());
    sha.add(atelier::as_bytes(Span{&part_count, 1u}));
    sha.add(atelier::as_bytes(parts));

    const String fin = sha.finish();
    return std::string(fin.view().std_view());
}

template <class T>
bool read_vec(std::ifstream& in, std::vector<T>& dst, std::uint64_t count) {
    dst.resize(static_cast<std::size_t>(count));
    if (count == 0u)
        return true;
    const auto bytes = static_cast<std::streamsize>(count * sizeof(T));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(dst.data()), bytes));
}

template <class T>
bool write_vec(std::ofstream& out, const std::vector<T>& src) {
    if (src.empty())
        return true;
    const auto bytes = static_cast<std::streamsize>(src.size() * sizeof(T));
    return static_cast<bool>(out.write(reinterpret_cast<const char*>(src.data()), bytes));
}

CookedMeshletsRef try_read_amsh(const std::filesystem::path& file) {
    const auto win_path = platform::windows_file_path(file);
    if (!win_path)
        return nullptr;

    std::ifstream in(*win_path, std::ios::binary | std::ios::ate);
    if (!in.good())
        return nullptr;
    const auto end_pos = in.tellg();
    if (end_pos < static_cast<std::streamoff>(sizeof(AmshHeader)))
        return nullptr;
    const auto file_bytes = static_cast<std::uint64_t>(end_pos);
    in.seekg(0);

    AmshHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)))
        return nullptr;

    constexpr std::uint64_t max_elems = 1ull << 29u;
    if (hdr.magic != amsh_magic || hdr.version != amsh_version || hdr.reserved != 0u || hdr.lod_count == 0u ||
        hdr.lod_count > max_mesh_lods || hdr.position_count == 0u || hdr.position_count > max_elems ||
        hdr.meshlet_count == 0u || hdr.meshlet_count > max_elems || hdr.meshlet_vert_count > max_elems ||
        hdr.meshlet_tri_bytes < 4ull || (hdr.meshlet_tri_bytes & 3ull) != 0ull || hdr.meshlet_tri_bytes > max_elems ||
        hdr.face_meta_count == 0u || hdr.face_meta_count > max_elems ||
        hdr.blas_index_count != hdr.face_meta_count * 3ull || hdr.lod_part_count == 0u ||
        hdr.lod_part_count > max_elems) {
        return nullptr;
    }

    const std::uint64_t expected_bytes =
        sizeof(AmshHeader) + hdr.position_count * sizeof(PackedPosition) +
        hdr.position_count * sizeof(PackedVertexAttributes) + hdr.meshlet_count * sizeof(GpuMeshlet) +
        hdr.meshlet_vert_count * sizeof(std::uint32_t) + hdr.meshlet_tri_bytes +
        hdr.blas_index_count * sizeof(std::uint32_t) + hdr.face_meta_count * sizeof(std::uint32_t) +
        hdr.lod_part_count * sizeof(PartRange);
    if (expected_bytes != file_bytes)
        return nullptr;

    auto cooked = std::make_shared<CookedMeshlets>();
    cooked->lods = hdr.lods;
    cooked->lod_count = hdr.lod_count;
    if (!read_vec(in, cooked->positions, hdr.position_count) || !read_vec(in, cooked->attributes, hdr.position_count) ||
        !read_vec(in, cooked->meshlets, hdr.meshlet_count) ||
        !read_vec(in, cooked->meshlet_vertices, hdr.meshlet_vert_count) ||
        !read_vec(in, cooked->meshlet_triangles, hdr.meshlet_tri_bytes) ||
        !read_vec(in, cooked->blas_indices, hdr.blas_index_count) ||
        !read_vec(in, cooked->face_meta, hdr.face_meta_count) || !read_vec(in, cooked->lod_parts, hdr.lod_part_count)) {
        return nullptr;
    }

    for (std::uint32_t l = 0u; l < cooked->lod_count; ++l) {
        const MeshLodLevel& lv = cooked->lods[l];
        if (!std::isfinite(lv.error) || lv.error < 0.0f ||
            std::uint64_t(lv.first_meshlet) + lv.meshlet_count > hdr.meshlet_count ||
            std::uint64_t(lv.first_part) + lv.part_count > hdr.lod_part_count ||
            std::uint64_t(lv.first_global_tri) + lv.triangle_count > hdr.face_meta_count) {
            return nullptr;
        }
    }
    for (const PartRange& pr : cooked->lod_parts) {
        if (std::uint64_t(pr.global_tri_base) + pr.triangle_count > hdr.face_meta_count)
            return nullptr;
    }
    for (const GpuMeshlet& m : cooked->meshlets) {
        const std::uint32_t v_cnt = m.counts & 0xFFu;
        const std::uint32_t t_cnt = (m.counts >> 8u) & 0xFFu;
        if (v_cnt == 0u || v_cnt > meshlet_max_vertices || t_cnt == 0u || t_cnt > meshlet_max_triangles ||
            std::uint64_t(m.vertex_offset) + v_cnt > hdr.meshlet_vert_count || (m.triangle_offset & 3u) != 0u ||
            std::uint64_t(m.triangle_offset) + std::uint64_t(t_cnt) * 3ull + 4ull > hdr.meshlet_tri_bytes ||
            std::uint64_t(m.global_tri_base) + t_cnt > hdr.face_meta_count || !std::isfinite(m.radius) ||
            m.radius < 0.0f) {
            return nullptr;
        }
    }
    for (const std::uint32_t vi : cooked->meshlet_vertices) {
        if (vi >= hdr.position_count)
            return nullptr;
    }
    for (const std::uint32_t vi : cooked->blas_indices) {
        if (vi >= hdr.position_count)
            return nullptr;
    }

    return cooked;
}

void write_amsh_atomic(const std::filesystem::path& cache_dir, const std::filesystem::path& file,
                       const CookedMeshlets& cooked) {
    const auto win_dir = platform::windows_file_path(cache_dir);
    const auto win_file = platform::windows_file_path(file);
    if (!win_dir || !win_file)
        return;

    std::error_code ec;
    std::filesystem::create_directories(*win_dir, ec);
    if (ec)
        return;

    const auto pid = static_cast<unsigned long>(_getpid());
    const auto seq = ++g_amsh_tmp_seq;
    const std::filesystem::path tmp_raw =
        file.wstring() + L"." + std::to_wstring(pid) + L"." + std::to_wstring(seq) + L".tmp";
    const auto win_tmp = platform::windows_file_path(tmp_raw);
    if (!win_tmp)
        return;

    AmshHeader hdr{};
    hdr.magic = amsh_magic;
    hdr.version = amsh_version;
    hdr.lod_count = cooked.lod_count;
    hdr.reserved = 0u;
    hdr.position_count = cooked.positions.size();
    hdr.meshlet_count = cooked.meshlets.size();
    hdr.meshlet_vert_count = cooked.meshlet_vertices.size();
    hdr.meshlet_tri_bytes = cooked.meshlet_triangles.size();
    hdr.blas_index_count = cooked.blas_indices.size();
    hdr.face_meta_count = cooked.face_meta.size();
    hdr.lod_part_count = cooked.lod_parts.size();
    hdr.lods = cooked.lods;

    std::ofstream out(*win_tmp, std::ios::binary | std::ios::trunc);
    const bool ok = static_cast<bool>(out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr))) &&
                    write_vec(out, cooked.positions) && write_vec(out, cooked.attributes) &&
                    write_vec(out, cooked.meshlets) && write_vec(out, cooked.meshlet_vertices) &&
                    write_vec(out, cooked.meshlet_triangles) && write_vec(out, cooked.blas_indices) &&
                    write_vec(out, cooked.face_meta) && write_vec(out, cooked.lod_parts);
    out.close();
    if (ok) {
        std::filesystem::rename(*win_tmp, *win_file, ec);
        if (ec)
            std::filesystem::remove(*win_tmp, ec);
    } else {
        std::filesystem::remove(*win_tmp, ec);
    }
}

CookedMeshletsRef load_or_cook_raw(const Mesh& mesh, const std::filesystem::path& cache_dir) {
    if (!cache_dir.empty()) {
        const std::string digest = compute_mesh_cache_digest(mesh);
        const std::filesystem::path file = cache_dir / (digest + "-v" + std::to_string(amsh_version) + ".amsh");
        if (auto cached = try_read_amsh(file))
            return cached;
        auto cooked = build_cooked_meshlets(mesh);
        write_amsh_atomic(cache_dir, file, *cooked);
        return cooked;
    }
    return build_cooked_meshlets(mesh);
}

} // namespace

std::uint32_t pack_oct_normal_snorm16(float nx, float ny, float nz) noexcept {
    const auto [ox, oy] = encode_octahedral(nx, ny, nz);
    const auto qx = static_cast<std::int16_t>(std::lrint(ox * 32767.0f));
    const auto qy = static_cast<std::int16_t>(std::lrint(oy * 32767.0f));
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(qx)) |
           (static_cast<std::uint32_t>(static_cast<std::uint16_t>(qy)) << 16u);
}

std::array<float, 3> unpack_oct_normal_snorm16(std::uint32_t packed) noexcept {
    const auto qx = static_cast<std::int16_t>(packed & 0xFFFFu);
    const auto qy = static_cast<std::int16_t>((packed >> 16u) & 0xFFFFu);
    const float ox = std::clamp(static_cast<float>(qx) / 32767.0f, -1.0f, 1.0f);
    const float oy = std::clamp(static_cast<float>(qy) / 32767.0f, -1.0f, 1.0f);
    return decode_octahedral(ox, oy);
}

std::uint32_t pack_oct_tangent_sign(float tx, float ty, float tz, float sign) noexcept {
    const auto [ox, oy] = encode_octahedral(tx, ty, tz);
    const auto qx = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(ox * 32767.0f)));
    const auto qy15 =
        static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(oy * 16383.0f)))) &
        0x7FFFu;
    const std::uint32_t sign_bit = (sign < 0.0f) ? (1u << 31u) : 0u;
    return static_cast<std::uint32_t>(qx) | (qy15 << 16u) | sign_bit;
}

const std::shared_ptr<const CookedMeshlets>& Mesh::cooked_meshlets(const std::filesystem::path& cache_dir) const {
    std::scoped_lock lock(cooked_mutex_);
    if (!cooked_meshlets_)
        cooked_meshlets_ = load_or_cook_raw(*this, cache_dir);
    return cooked_meshlets_;
}

const std::shared_ptr<const CookedMeshlets>& Mesh::cooked_meshlets() const {
    return cooked_meshlets({});
}

bool Mesh::cooked_triangle(std::uint32_t global_triangle_id, std::array<math::Vector3, 3>& positions,
                           std::array<math::Vector3, 3>& normals) const {
    const auto& c = cooked_meshlets();
    const std::size_t base = std::size_t(global_triangle_id) * 3u;
    if (!c || base + 2u >= c->blas_indices.size())
        return false;
    for (std::size_t k = 0u; k < 3u; ++k) {
        const std::uint32_t vi = c->blas_indices[base + k];
        if (vi >= c->positions.size() || vi >= c->attributes.size())
            return false;
        const PackedPosition& p = c->positions[vi];
        positions[k] = {p.x, p.y, p.z};
        const auto n = unpack_oct_normal_snorm16(c->attributes[vi].oct_normal);
        normals[k] = {n[0], n[1], n[2]};
    }
    return true;
}

CookedMeshletsRef cook_meshlets(const Mesh& mesh) {
    return mesh.cooked_meshlets();
}

CookedMeshletsRef load_or_cook_meshlets(const Mesh& mesh, const std::filesystem::path& cache_dir) {
    return mesh.cooked_meshlets(cache_dir);
}

} // namespace atelier::geometry
