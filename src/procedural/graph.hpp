#pragma once

#include "procedural/geometry.hpp"
#include "runtime/reflection.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "procedural/graph.gen.hpp"

namespace atelier::procedural {

namespace detail {
struct Access;
}

class Evaluator;

// Host resources a graph reads by persistent key. revision() changes whenever some key
// may resolve differently, for example after a reload.
class Resources {
public:
    virtual ~Resources() = default;

    [[nodiscard]] virtual Result<geometry::MeshRef> mesh(StringView key) const = 0;

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
    [[nodiscard]] Result<Reference> mesh(StringView key) const;

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

// The base of graph nodes. Public fields of a node type are its parameters. Its const
// method evaluate declares the ports, in parameter order: const procedural.Context& is
// the context, const procedural.Geometry& an input port, const procedural.Geometries&
// an input port taking any number of connections, and procedural.Geometry& an output.
ACLASS(Name = "procedural.Node", DESC = "程序化图中的节点。公开字段是参数，端口来自 evaluate 方法的签名。")
class Node {
    AGENERATED_BODY(Node)

public:
    Node() = default;

    Node(const Node&) = delete;

    Node& operator=(const Node&) = delete;

private:
    friend struct detail::Access;

    AFIELD(Name = "inputs", Hidden, Sequence = "procedural.Node")
    ReferenceSequence inputs_; // Upstream node of each connection, ordered by input port.

    std::vector<Connection> connections_; // Parallel to inputs_.
};

// The base of Source attribute nodes, which rewrite point columns of their input `input` into
// their output `output`. A Source subclass declares a synchronous `void evaluate(...)`: a
// `const procedural.Context&` parameter takes the context, `Array<T>& name` writes the point
// column `name` (added with T's default value, zero or the identity rotation, when missing) and
// `const Array<T>& name` reads one that must exist. Borrowed columns keep their length, so the
// point count is the input's.
ACLASS(Name = "procedural.PointNode",
       DESC = "自有语言属性节点的基类：按 evaluate 的参数名读写输入几何的点属性列，点数不变。")
class PointNode : public Node {
    AGENERATED_BODY(PointNode)
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

enum class Slot : std::uint8_t { context, input, inputs, output };

// Port names are evaluate's parameter names, read again for every published version. A
// PointNode type has the fixed ports `input` and `output`; its evaluate is Source code, bound
// by the evaluator, so `evaluate` is null and `parameters` empty.
struct Ports {
    MethodId evaluate;

    std::vector<Slot> parameters; // Of evaluate, in order.

    std::vector<std::string> inputs; // Input port names.

    std::vector<bool> multiple; // Per input port: takes any number of connections.

    std::vector<std::string> outputs; // Output port names.
};

// The ports of a node type, or why it cannot be a node; an unnamed port parameter fails.
Result<Ports> ports(Runtime& runtime, TypeId node);

struct Input {
    ObjectId from;

    std::uint32_t output;

    std::uint32_t input;

    bool operator==(const Input&) const = default;
};

// Connections into `node`, ordered by input port; a multiple port keeps connection order.
Result<std::vector<Input>> inputs(Runtime& runtime, ObjectId node);

struct Contents {
    std::vector<ObjectId> nodes;

    ObjectId display; // Null when unset.
};

Result<Contents> contents(Runtime& runtime, ObjectId graph);

// Graph edits. Each writes only the objects it changes: the graph for its node list and
// display, a node for the connections into it. The graph holds its nodes and a node holds
// its inputs, so a removed node lives while something else roots it, such as edit history.
Result<ObjectRoot> add_node(Runtime& runtime, ObjectId graph, TypeId type);

// Also disconnects the node's outputs and clears it as the display node.
Result<void> remove_node(Runtime& runtime, ObjectId graph, ObjectId node);

// Both nodes must be in the graph. A single input port replaces its connection; a cycle fails.
Result<void> connect(Runtime& runtime, ObjectId graph, ObjectId from, std::uint32_t output, ObjectId to,
                     std::uint32_t input);

Result<void> disconnect(Runtime& runtime, ObjectId from, std::uint32_t output, ObjectId to, std::uint32_t input);

// A null node clears the display.
Result<void> set_display(Runtime& runtime, ObjectId graph, ObjectId node);

}
