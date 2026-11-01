#include "procedural/instances.hpp"
#include "procedural/random.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace atelier::procedural {
namespace {

struct GroupKey {
    std::uint32_t reference;

    std::array<std::int32_t, 3> chunk;

    bool operator==(const GroupKey&) const = default;
};

struct GroupHash {
    std::size_t operator()(const GroupKey& key) const noexcept {
        return hash(key.reference,
                    hash(static_cast<std::uint32_t>(key.chunk[0]),
                         hash(static_cast<std::uint32_t>(key.chunk[1]), static_cast<std::uint32_t>(key.chunk[2]))));
    }
};

bool before(const Reference& a, const std::array<std::int32_t, 3>& ca, const Reference& b,
            const std::array<std::int32_t, 3>& cb) {
    if (const auto order = a.key <=> b.key; order != 0)
        return order < 0;
    if (a.mesh != b.mesh)
        return std::less<>{}(a.mesh.get(), b.mesh.get());
    return ca < cb;
}

}

Result<Publication> publish(const Geometry& geometry, float chunk_size, const Publication* previous) {
    if (!std::isfinite(chunk_size) || !(chunk_size > 0))
        return failure("Chunk size must be finite and positive.");
    const auto& indices = geometry.instance_references();
    const auto& transforms = geometry.instance_transforms();
    Publication result;
    std::unordered_map<GroupKey, std::vector<Affine>, GroupHash> groups;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto& m = transforms[i];
        if (!math::normal_transform({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]})) {
            ++result.dropped;
            continue;
        }
        GroupKey key{indices[i], {}};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double cell = std::floor(double(m[12 + axis]) / chunk_size);
            if (cell < std::numeric_limits<std::int32_t>::min() || cell > std::numeric_limits<std::int32_t>::max())
                return failure("An instance lies outside the chunk index range.");
            key.chunk[axis] = static_cast<std::int32_t>(cell);
        }
        groups[key].push_back({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]});
    }
    const auto references = geometry.references();
    result.collections.reserve(groups.size());
    for (auto& [key, values] : groups)
        result.collections.push_back(
            {references[key.reference], key.chunk, std::make_shared<const std::vector<Affine>>(std::move(values))});
    std::ranges::sort(result.collections, [](const Collection& a, const Collection& b) {
        return before(a.reference, a.chunk, b.reference, b.chunk);
    });
    if (!previous)
        return result;
    for (auto& collection : result.collections) {
        const auto at =
            std::ranges::lower_bound(previous->collections, collection, [](const Collection& a, const Collection& b) {
                return before(a.reference, a.chunk, b.reference, b.chunk);
            });
        if (at == previous->collections.end() || at->reference != collection.reference || at->chunk != collection.chunk)
            continue;
        const auto& old = *at->transforms;
        const auto& now = *collection.transforms;
        if (old.size() == now.size() && std::memcmp(old.data(), now.data(), now.size() * sizeof(Affine)) == 0)
            collection.transforms = at->transforms;
    }
    return result;
}

}
