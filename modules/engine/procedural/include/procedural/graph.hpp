#pragma once

#include "foundation/map.hpp"
#include "procedural/geometry.hpp"
#include "runtime/reflection.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "procedural/graph.gen.hpp"

namespace atelier::procedural {

namespace detail {
struct Access;
}

class Evaluator;

// Host resources a graph reads by persistent key; recoverable failures return Result.
// revision() changes whenever some key may resolve differently, for example after a reload.
class Resources {
public:
    virtual ~Resources() = default;

    [[nodiscard]] virtual Result<geometry::MeshRef> mesh(StringView key) const noexcept = 0;

    [[nodiscard]] virtual std::uint64_t revision() const noexcept = 0;
};

// Every member shares one access level, so the type stays standard layout and Source reads
// `seed` at its published offset.
ASTRUCT(Name = "procedural.Context", DESC = "一次求值交给节点的上下文：图的随机种子，以及按键解析资源的入口。")
struct Context {
    AGENERATED_BODY(Context)

public:
    [[nodiscard]] std::uint32_t seed() const noexcept { return seed_; }

    // Resolves a mesh and records it as something the evaluating node's result depends on.
    [[nodiscard]] Result<Reference> mesh(StringView key) const noexcept;

private:
    friend class Evaluator;

    AFIELD(Name = "seed", DESC = "图的随机种子。节点把它与自己的 seed 参数混合。")
    std::uint32_t seed_{};

    const Resources* resources_{};

    std::vector<Reference>* dependencies_{};
};

// Input port `input` reads output `output` of the node in the same position of inputs_.
struct Connection {
    std::uint32_t input;

    std::uint32_t output;
};

struct FrozenOutput {
    std::vector<Geometry> outputs;

    std::vector<Reference> dependencies;
};

// The unified base of all native and Source procedural graph nodes. Public fields are
// authored parameters (an optional `[RunOver]` `procedural.Layer` field selects the active
// element domain). Its synchronous void `process` method declares ports and column bindings:
// - `const procedural.Context&`: evaluation context (seed and host resources).
// - `const procedural.Geometry&`, `const procedural.Geometries&`, `procedural.Geometry&`:
//   whole-geometry single input, multi-input and output ports (named by parameter name).
// - `[In]`, `[Out]`, `[InOut]`, `[Group]` `Array<T>&` / `const Array<T>&` column streams:
//   imply primary `"input"` -> `"output"` COW geometry ports (combinable with extra reference
//   geometry ports). Read borrows observe the input snapshot; `[Group]` receives an empty
//   view when its group name is empty (all elements selected); `Optional` `[In]` receives an
//   empty view when the input lacks that attribute.
ACLASS(Name = "procedural.Node", Meta("source.contract_method", "process"),
       DESC = "程序化图中的统一节点基类。公开字段是面板参数，端口与属性列流来自 process 方法签名。")
class Node {
    AGENERATED_BODY(Node)

public:
    Node() = default;

    Node(const Node&) = delete;

    Node& operator=(const Node&) = delete;

    // Position-only sections for Source kernels which author many pieces from one guide.
    AFUNCTION(
        BorrowSafe,
        DESC =
            "在给定折线图元上批量取出区间路径。每对数是从起点量取的米数；positions 和 offsets 按请求顺序返回，区间之间不连线。")
    [[nodiscard]] Result<void> read_curve_slices(const Geometry& curves, std::uint32_t primitive,
                                                 const Array<math::Vector2>& intervals, Array<math::Vector3>& positions,
                                                 Array<std::uint32_t>& offsets) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "向指定显式输出端口提交变长纯点集。")
    [[nodiscard]] Result<void> emit_points(const Array<math::Vector3>& positions,
                                           std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "向指定显式输出端口提交一条或多条开放折线拓扑。")
    [[nodiscard]] Result<void> emit_polylines(const Array<math::Vector3>& positions,
                                              const Array<std::uint32_t>& vertex_points,
                                              const Array<std::uint32_t>& offsets,
                                              std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "向指定显式输出端口提交经公共凸面与平面容差校验的多边形面片拓扑。")
    [[nodiscard]] Result<void> emit_polygons(const Array<math::Vector3>& positions,
                                             const Array<std::uint32_t>& vertex_points,
                                             const Array<std::uint32_t>& offsets,
                                             std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "向指定显式输出端口提交经公共凸面与平面容差校验的四边形面片序列。")
    [[nodiscard]] Result<void> emit_quads(const Array<math::Vector3>& positions,
                                          const Array<std::uint32_t>& quad_points,
                                          std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 bool 属性列。")
    [[nodiscard]] Result<void> set_bool(const String& name, Layer layer, const Array<bool>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 int 属性列。")
    [[nodiscard]] Result<void> set_int(const String& name, Layer layer, const Array<std::int32_t>& values,
                                       std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 uint 属性列。")
    [[nodiscard]] Result<void> set_uint(const String& name, Layer layer, const Array<std::uint32_t>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 float 属性列。")
    [[nodiscard]] Result<void> set_float(const String& name, Layer layer, const Array<float>& values,
                                         std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 vector2 属性列。")
    [[nodiscard]] Result<void> set_vec2(const String& name, Layer layer, const Array<math::Vector2>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 vector3 属性列。")
    [[nodiscard]] Result<void> set_vec3(const String& name, Layer layer, const Array<math::Vector3>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 vector4 属性列。")
    [[nodiscard]] Result<void> set_vec4(const String& name, Layer layer, const Array<math::Vector4>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

    AFUNCTION(BorrowSafe, DESC = "在指定显式输出端口的几何层上写入或替换 quaternion 属性列。")
    [[nodiscard]] Result<void> set_quat(const String& name, Layer layer, const Array<math::Quaternion>& values,
                                        std::uint32_t output_port = 0u) const noexcept;

private:
    friend struct detail::Access;

    AFIELD(Name = "inputs", Hidden, Sequence = "procedural.Node")
    ReferenceSequence inputs_; // Upstream node of each connection, ordered by input port.

    std::vector<Connection> connections_; // Parallel to inputs_.

    bool bypassed_{};

    bool frozen_{};

    std::shared_ptr<const FrozenOutput> frozen_output_;
};

ACLASS(Name = "procedural.Graph", DESC = "程序化图：一组节点及其连接，display 节点的输出是图的结果。")
class Graph {
    AGENERATED_BODY(Graph)

public:
    Graph() = default;

    Graph(const Graph&) = delete;

    Graph& operator=(const Graph&) = delete;

    AFIELD(DESC = "图的随机种子，经 Context 交给每个节点。")
    std::uint32_t seed{};

private:
    friend struct detail::Access;

    AFIELD(Name = "nodes", Hidden, Sequence = "procedural.Node")
    ReferenceSequence nodes_;

    AFIELD(Name = "display", Hidden, Strong = "procedural.Node")
    ReferenceSlot display_;
};

// Publishes Geometry, Graph, Node and the built-in nodes. Repeating it changes nothing.
Result<void> register_types(Runtime& runtime);

// Port schema of a concrete `procedural.Node` type or bound `procedural.AssetNode` instance.
struct Ports {
    std::vector<std::string> inputs; // Input port names.

    std::vector<bool> multiple; // Per input port: takes any number of connections.

    std::vector<std::string> outputs; // Output port names.

    bool operator==(const Ports&) const = default;
};

struct AssetSubgraph {
    std::string asset_key;

    Ports ports;

    ObjectRoot graph;

    std::vector<ObjectId> input_nodes;

    std::vector<ObjectId> output_nodes;

    std::optional<Error> bind_error;

    std::uint64_t revision{};
};

// Named column bindings. Opaque geometry kernels may additionally transport arbitrary columns.
enum class AttributeAccess { read, write, edit, group };

struct AttributeRule {
    std::string port, name, selector; // A selector names the public field supplying the column name.
    std::string layer, layer_selector, type;
    AttributeAccess access{};
    std::string_view missing;
    std::string purpose{};
    std::string_view unless_present{}, needs_present{};
    // A mode-dependent rule applies when the reflected enum field matches (or excludes) its named value.
    struct Condition {
        std::string_view field{}, value{};
        bool exclude{};
        bool operator==(const Condition&) const = default;
    };
    Condition when{};
    bool operator==(const AttributeRule&) const = default;
};

// Documentation for explicit geometry ports; collected on type inspection only.
struct PortDescription {
    std::string port;

    bool output{};

    std::string description;
};

struct NodeInterface {
    Ports ports;
    std::vector<AttributeRule> attributes;
    std::vector<PortDescription> port_descriptions;
};

Result<NodeInterface> node_interface(Runtime& runtime, TypeId node);
std::string_view layer_name(Layer layer) noexcept;
std::string_view access_name(AttributeAccess access) noexcept;

// The ports of a node type, or why it cannot be a node; an unnamed port parameter fails.
Result<Ports> ports(Runtime& runtime, TypeId node);

// The live ports of a node instance (resolving dynamic subgraph ports on AssetNode).
Result<Ports> ports(Runtime& runtime, ObjectId node);

Result<AssetSubgraph> asset_subgraph(Runtime& runtime, ObjectId node);

Result<void> set_asset_subgraph(Runtime& runtime, ObjectId node, AssetSubgraph subgraph);

struct Input {
    ObjectId from;

    std::uint32_t output;

    std::uint32_t input;

    bool operator==(const Input&) const = default;
};

// A membership snapshot. Refresh after changing the graph's members or display.
// Does not root objects; the caller keeps the graph alive.
class Contents {
public:
    [[nodiscard]] std::span<const ObjectId> nodes() const noexcept { return nodes_; }

    [[nodiscard]] ObjectId display() const noexcept { return display_; }

    [[nodiscard]] bool contains(ObjectId node) const noexcept { return members_.contains(node); }

    // Recheck cached connections after a membership change, without rereading the node.
    [[nodiscard]] Result<void> check(std::span<const Input> connections) const;

private:
    friend struct detail::Access;

    std::vector<ObjectId> nodes_;

    Map<ObjectId, std::uint8_t> members_;

    ObjectId display_;
};

Result<Contents> contents(Runtime& runtime, ObjectId graph);

// Connections into a member of this snapshot. Checks membership and reference/connection
// lengths, not output port numbers; consumers check against their current source ports.
Result<std::vector<Input>> inputs(Runtime& runtime, const Contents& graph, ObjectId node);

// Graph edits. Each writes only the objects it changes: the graph for its node list and
// display, a node for the connections into it. The graph holds its nodes and a node holds
// its inputs, so a removed node lives while something else roots it, such as edit history.
Result<ObjectRoot> add_node(Runtime& runtime, ObjectId graph, TypeId type);

// Re-attaches an existing detached node root (such as from edit history) with empty inputs.
Result<void> restore_node(Runtime& runtime, ObjectId graph, const ObjectRoot& node);

// Also disconnects the node's outputs and clears it as the display node.
Result<void> remove_node(Runtime& runtime, ObjectId graph, ObjectId node);

// Both nodes must be in the graph. A single input port replaces its connection; a cycle fails.
Result<void> connect(Runtime& runtime, ObjectId graph, ObjectId from, std::uint32_t output, ObjectId to,
                     std::uint32_t input);

Result<void> disconnect(Runtime& runtime, ObjectId from, std::uint32_t output, ObjectId to, std::uint32_t input);

// A null node clears the display.
Result<void> set_display(Runtime& runtime, ObjectId graph, ObjectId node);

// A bypassed node skips process() and forwards its first connected input geometry (or empty Geometry) to output 0.
Result<bool> bypassed(Runtime& runtime, ObjectId node);

Result<void> set_bypassed(Runtime& runtime, ObjectId node, bool value);

// A frozen node serves its locked cooked Geometry snapshot without re-evaluating upstream changes until unfrozen.
Result<bool> frozen(Runtime& runtime, ObjectId node);

Result<std::shared_ptr<const FrozenOutput>> frozen_output(Runtime& runtime, ObjectId node);

Result<void> set_frozen(Runtime& runtime, ObjectId node, bool value, std::shared_ptr<const FrozenOutput> snapshot = {});

}
