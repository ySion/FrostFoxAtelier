#pragma once

#include "Before.hpp"
#include "After.hpp"
#include "foundation/map.hpp"
#include "runtime/runtime.hpp"

namespace field_study {
using namespace atelier;

// Only the temporary runtime.cpp used by this study owns this type. Populate
// all layouts at registration so changing the probe leaves allocations, names,
// query sequence and the machine code for the error path identical.
struct Index {
    using Dense = detail::FieldStudyBefore<StringView, std::pair<StringView, FieldId>, Hash<StringView>, Equal<StringView>>;
    using Revised = detail::FieldStudyAfter<StringView, std::pair<StringView, FieldId>, Hash<StringView>, Equal<StringView>>;
    Map<StringView, FieldId> baseline;
    Dense owned;
    Dense indexed;
    Revised fixed;

    void reserve(size_t n) {
        baseline.reserve(n);
        owned.reserve(n);
        indexed.reserve(std::max(n, size_t{17}));
        fixed.reserve(n);
    }
    void try_emplace(StringView name, FieldId id) {
        baseline.try_emplace(name, id);
        owned.try_emplace(name, id);
        indexed.try_emplace(name, id);
        fixed.try_emplace(name, id);
    }

    using Probe = const FieldId* (*)(const Index&, StringView);
    static inline const Index* captured{};
    static Probe probe;
    const FieldId* find(StringView name) const { return probe(*this, name); }
};

inline bool omit_message{};
inline const FieldId* baseline(const Index& index, StringView key) { return index.baseline.find(key); }
inline Index::Probe Index::probe = field_study::baseline;
inline const FieldId* owned(const Index& index, StringView key) {
    const auto* entry = index.owned.find(key);
    return entry ? &entry->second : nullptr;
}
inline const FieldId* indexed(const Index& index, StringView key) {
    const auto* entry = index.indexed.find(key);
    return entry ? &entry->second : nullptr;
}
inline const FieldId* fixed(const Index& index, StringView key) {
    const auto* entry = index.fixed.find(key);
    return entry ? &entry->second : nullptr;
}
inline const FieldId* missing(const Index&, StringView) { return nullptr; }
}
