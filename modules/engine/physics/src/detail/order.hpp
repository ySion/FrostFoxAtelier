#pragma once

#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "runtime/runtime.hpp"
#include <algorithm>

namespace atelier::physics::detail {
// A lookup table's stable traversal order, independent of its hash policy.
// Entries receive a fresh revision on creation/rebuild. Invalidate on insert/erase,
// not on World tree edits. Rebuild lazily so unchanged fixed steps allocate nothing.
class InsertionOrder final {
public:
    void invalidate() noexcept { stale_ = true; }

    // Erasure may invalidate while a caller walks this snapshot; get() must not be
    // called again during that walk. No snapshot survives another get() after edits.
    template <class T>
    Span<const ObjectId> get(const Map<ObjectId, T>& entries) {
        if (stale_) {
            keys_.clear();
            keys_.reserve(entries.size());
            for (const auto [id, entry] : entries) {
                (void)entry;
                keys_.add(id);
            }
            std::ranges::sort(keys_,
                              [&](ObjectId a, ObjectId b) { return entries.at(a).revision < entries.at(b).revision; });
            stale_ = false;
        }
        return keys_;
    }

private:
    Array<ObjectId> keys_;

    bool stale_{true};
};
}
