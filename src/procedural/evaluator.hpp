#pragma once

#include "procedural/graph.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace atelier::source { class Program; }

namespace atelier::procedural {

struct NodeStatus {
    std::optional<Error> error;   // Set while the node or an input it reads fails.
    std::uint64_t evaluations{};  // Calls of evaluate.
    std::uint64_t cache_hits{};   // Results found in the cache instead.
    std::chrono::nanoseconds time{}; // Of the last evaluate call, with its argument setup.
};

// Brings a graph's display output up to date on the Runtime thread. A node runs again only
// when its type, public fields, connections, an input's result, a resource it read, for a
// node taking the context the graph seed, or for a Source node its code changed; results stay
// cached by those contents within a byte budget, so undoing an edit finds the earlier results.
// Write stamps select the objects to reread and the program revision the Source code to
// rebind; nothing is written to the graph or its nodes. Public fields may be scalars, enums,
// plain values (math included), String and Arrays of these.
class Evaluator {
public:
    // `program` supplies the evaluate methods of PointNode types and must outlive the
    // evaluator; without one those nodes fail.
    Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program = nullptr,
        std::size_t cache_bytes = std::size_t{256} << 20);
    ~Evaluator();
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;
    // Fails only when the graph cannot be read. Node failures go to status(), and output()
    // keeps the last result.
    Result<void> evaluate();
    // The display node's first output from the last evaluation that produced one; null while
    // the graph has no display node.
    [[nodiscard]] const Geometry* output() const noexcept;
    // Null for nodes not in the graph at the last evaluation.
    [[nodiscard]] const NodeStatus* status(ObjectId node) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
