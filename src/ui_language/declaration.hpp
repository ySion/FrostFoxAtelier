#pragma once

#include "language/source.hpp"
#include <string>
#include <vector>

namespace atelier::ui_language {

enum class InputKind { boolean, number, edit };
struct Input { std::string name; InputKind kind; };
struct State { std::string name; InputKind kind; };
enum class NodeKind { column, row, text, foldout, number_row };
enum class Property { text, classes, gap, height, width, padding_left, enabled, selected, collapsed, caption, edit };
struct Node { std::string name; NodeKind kind; std::uint32_t parent{}; }; // Parent slot + 1; zero is the host parent.
struct Binding { std::uint32_t node{}, operation{}; Property property{}; language::SourcePosition position; };
struct Event { std::uint32_t node{}, operation{}; };
struct SourceMapping { std::size_t begin{}, end{}; language::SourcePosition origin; };

// UI structure and source positions only. Expressions and handlers are typed and
// compiled by the ordinary Source compiler, in one module per component type.
struct Declaration {
    std::string name, source_name, generated_source;
    std::vector<Input> inputs;
    std::vector<State> states;
    std::vector<Node> nodes;
    std::vector<Binding> bindings;
    std::vector<Event> events;
    std::vector<SourceMapping> mappings;
    std::string diagnostic(std::string_view compiler_message) const;
};

[[nodiscard]] std::expected<Declaration, language::Diagnostic> lower(
    std::string_view source_name, std::string_view source);

} // namespace atelier::ui_language
