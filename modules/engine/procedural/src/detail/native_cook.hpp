#pragma once

#include "foundation/span.hpp"
#include "math/vector.hpp"

#include <array>
#include <memory>
#include <utility>

namespace atelier::procedural {
class Geometry;

class Node;

struct BooleanStats;

namespace detail {
struct PreparedBooleanSurface;

// Two slots shared by all Boolean nodes in one evaluator. Each slot observes
// one immutable published geometry at a time; the key never keeps it alive.
struct BooleanPreparationCache {
    struct Operand {
        std::weak_ptr<const Geometry> input;

        math::Vector3d origin{};

        double step{};

        std::shared_ptr<const PreparedBooleanSurface> prepared;

        std::size_t bytes{};
    };

    std::array<Operand, 2> operands;

    std::size_t bytes() const noexcept { return operands[0].bytes + operands[1].bytes; }
};

// Reflection keeps the same native SOP signature as a direct call. Nested
// evaluations restore the caller's scope when they finish.
struct NativeCook {
    static inline thread_local NativeCook* current{};

    std::shared_ptr<const BooleanStats>& boolean_stats;

    BooleanPreparationCache& preparation;

    std::size_t budget;

    std::array<std::shared_ptr<const Geometry>, 2> inputs;

    const Node* active_node{};

    Span<Geometry> outputs{};

    bool uses_primary_stream{};

    NativeCook(std::shared_ptr<const BooleanStats>& stats, BooleanPreparationCache& cache, std::size_t bytes,
               const Node* node = nullptr, Span<Geometry> out_span = {}, bool primary_stream = false) noexcept
        : boolean_stats(stats), preparation(cache), budget(bytes), active_node(node), outputs(out_span),
          uses_primary_stream(primary_stream), previous(std::exchange(current, this)) {}

    ~NativeCook() { current = previous; }

    NativeCook(const NativeCook&) = delete;

    NativeCook& operator=(const NativeCook&) = delete;

private:
    NativeCook* previous;
};
}
}
