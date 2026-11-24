#pragma once

#include "procedural/graph.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace atelier::source {
class Program;
}

namespace atelier::procedural {

struct BooleanStats;

// Shared LRU node-evaluation & Boolean-preparation cache pool across a main graph, its nested
// AssetNode subgraphs, and multiple scene ProceduralGeneratorComponent instances.
class CachePool {
public:
    explicit CachePool(std::size_t budget_bytes = std::size_t{256} << 20);

    ~CachePool();

    CachePool(const CachePool&) = delete;

    CachePool& operator=(const CachePool&) = delete;

private:
    friend class Evaluator;

    struct Impl;

    std::unique_ptr<Impl> impl_;
};

struct AttributeBinding {
    AttributeRule rule;
    std::string name{}, layer{};
    std::string_view status{};
    ObjectId upstream{}, column_source{};
};

struct NodeBindings {
    std::string_view mode;
    std::vector<AttributeBinding> attributes;
};

struct NodeStatus {
    std::optional<Error> error; // Set while the node or an input it reads fails.

    // Calls of process.
    std::uint64_t evaluations{};

    std::uint64_t cache_hits{}; // Results found in the cache instead.

    // The last actual node run, including argument setup and failures; empty if never run
    // or that run was unmeasured. Idle, cached and blocked nodes retain their previous time.
    std::optional<std::chrono::nanoseconds> time;

    // Snapshot from the last actual Boolean invocation, including partial work on failure.
    std::shared_ptr<const BooleanStats> boolean_stats;
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
    inline static constexpr std::uint32_t max_subgraph_depth = 8;

    struct ExternalInput {
        ObjectId input_node{};

        Geometry geometry{};

        std::uint64_t serial{}; // Non-zero upstream Entry::serial when connected; 0 when unconnected.

        std::uint32_t output{}; // Upstream output port index within that Entry.
    };

    // `program` supplies Source Node methods and must outlive the evaluator.
    // Native Node methods need no program.
    Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program = nullptr,
              std::size_t cache_bytes = std::size_t{256} << 20);

    Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program,
              std::shared_ptr<CachePool> cache_pool, std::uint32_t subgraph_depth = 0);

    ~Evaluator();

    Evaluator(const Evaluator&) = delete;

    Evaluator& operator=(const Evaluator&) = delete;

    [[nodiscard]] std::shared_ptr<CachePool> cache_pool() const noexcept;

    // Supplies external Geometry inputs for `procedural.GraphInput` nodes when evaluating an AssetNode subgraph.
    void set_external_inputs(std::span<const ExternalInput> inputs);

    void clear_external_inputs() noexcept;

    // Fails only when the graph itself cannot be read or has invalid topology; node execution
    // failures go to status(). Any graph or display-chain failure clears output() while
    // preserving last_valid_output(). When `seed_override` is engaged, nodes reading the
    // context receive `*seed_override` without mutating the graph's authored seed or
    // overwriting last_valid_output().
    // Timing is opt-in diagnostics only: it neither changes keys nor forces nodes to run.
    Result<void> evaluate(bool record_timings = false, std::optional<std::uint32_t> seed_override = {});

    // Evaluates `node` and its upstream dependencies without changing output() or
    // last_valid_output(), so an editor can inspect a selected node independent of the display flag.
    Result<void> evaluate_node(ObjectId node, bool record_timings = false,
                               std::optional<std::uint32_t> seed_override = {});

    // Evaluates an explicit set of target nodes (e.g. ordered GraphOutput nodes) in one pass
    // without changing output() or last_valid_output().
    Result<void> evaluate_targets(std::span<const ObjectId> targets, bool record_timings = false,
                                  std::optional<std::uint32_t> seed_override = {});

    // The display node's first output when the latest evaluate() succeeded without display-chain
    // errors; null when the graph has no display node or the latest evaluation or display chain failed.
    [[nodiscard]] const Geometry* output() const noexcept;

    // Keeps the current output() alive through later cooks and asynchronous presentation.
    [[nodiscard]] std::shared_ptr<const Geometry> retain_output() const noexcept;

    // The most recent valid display output while a display node is set, retained across graph
    // and node evaluation errors for editor fallback preview.
    [[nodiscard]] const Geometry* last_valid_output() const noexcept;

    // Keeps the displayed output alive through later cooks and asynchronous presentation.
    [[nodiscard]] std::shared_ptr<const Geometry> retain_last_valid_output() const noexcept;

    // True when the graph currently designates a display node.
    [[nodiscard]] bool has_display_node() const noexcept;

    // Root-cause error on the display chain when the latest evaluate() had a node failure.
    [[nodiscard]] std::optional<Error> display_error() const;

    // Deduplicated host resources read by the display node and its upstream chain on their
    // latest evaluations, including cached nodes and zero-instance MeshImport/variant reads.
    [[nodiscard]] std::vector<Reference> display_dependencies() const;

    // Number of outputs produced by `node` on its latest successful evaluation, or 0 when
    // unevaluated, absent from the graph, or in an error state.
    [[nodiscard]] std::uint32_t node_output_count(ObjectId node) const noexcept;

    // Output `output_index` of `node` when its latest evaluation succeeded; null otherwise.
    [[nodiscard]] const Geometry* node_output(ObjectId node, std::uint32_t output_index = 0) const noexcept;

    [[nodiscard]] std::shared_ptr<const Geometry> retain_node_output(ObjectId node,
                                                                     std::uint32_t output_index = 0) const noexcept;

    // Cook only the target branch and explain the named columns of the resulting snapshot.
    [[nodiscard]] Result<NodeBindings> inspect_bindings(ObjectId node);

    // Root-cause error on `node` or its upstream chain when its latest evaluation failed.
    [[nodiscard]] std::optional<Error> target_error(ObjectId node) const;

    // Deduplicated host resources read by `targets` and their active upstream chains.
    [[nodiscard]] std::vector<Reference> target_dependencies(std::span<const ObjectId> targets) const;

    // Cooks the current inputs, then captures all outputs and their effective resource dependencies.
    [[nodiscard]] Result<std::shared_ptr<const FrozenOutput>>
    freeze_snapshot(ObjectId node, std::optional<std::uint32_t> seed_override = {});

    // Session freezes that cut off upstream evaluation on the current display branch.
    [[nodiscard]] std::vector<ObjectId> display_freezes() const;

    // Null for nodes not in the graph at the last evaluation.
    [[nodiscard]] const NodeStatus* status(ObjectId node) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}
