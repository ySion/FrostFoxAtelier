#pragma once

#include "procedural/graph.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace atelier::source {
class Program;
}

namespace atelier::procedural {

struct NodeStatus {
    std::optional<Error> error; // Set while the node or an input it reads fails.

    // Calls of process.
    std::uint64_t evaluations{};

    std::uint64_t cache_hits{}; // Results found in the cache instead.

    // The last actual node run, including argument setup and failures; empty if never run
    // or that run was unmeasured. Idle, cached and blocked nodes retain their previous time.
    std::optional<std::chrono::nanoseconds> time;
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
    // `program` supplies Source Node methods and must outlive the evaluator.
    // Native Node methods need no program.
    Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program = nullptr,
              std::size_t cache_bytes = std::size_t{256} << 20);

    ~Evaluator();

    Evaluator(const Evaluator&) = delete;

    Evaluator& operator=(const Evaluator&) = delete;

    // Fails only when the graph itself cannot be read or has invalid topology; node execution
    // failures go to status(). Any graph or display-chain failure clears output() while
    // preserving last_valid_output().
    // Timing is opt-in diagnostics only: it neither changes keys nor forces nodes to run.
    Result<void> evaluate(bool record_timings = false);

    // Evaluates `node` and its upstream dependencies without changing output() or
    // last_valid_output(), so an editor can inspect a selected node independent of the display flag.
    Result<void> evaluate_node(ObjectId node, bool record_timings = false);

    // The display node's first output when the latest evaluate() succeeded without display-chain
    // errors; null when the graph has no display node or the latest evaluation or display chain failed.
    [[nodiscard]] const Geometry* output() const noexcept;

    // The most recent valid display output while a display node is set, retained across graph
    // and node evaluation errors for editor fallback preview.
    [[nodiscard]] const Geometry* last_valid_output() const noexcept;
    // Keeps the displayed output alive through later cooks and asynchronous presentation.
    [[nodiscard]] std::shared_ptr<const Geometry> retain_last_valid_output() const noexcept;

    // Number of outputs produced by `node` on its latest successful evaluation, or 0 when
    // unevaluated, absent from the graph, or in an error state.
    [[nodiscard]] std::uint32_t node_output_count(ObjectId node) const noexcept;

    // Output `output_index` of `node` when its latest evaluation succeeded; null otherwise.
    [[nodiscard]] const Geometry* node_output(ObjectId node, std::uint32_t output_index = 0) const noexcept;

    // Null for nodes not in the graph at the last evaluation.
    [[nodiscard]] const NodeStatus* status(ObjectId node) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}
