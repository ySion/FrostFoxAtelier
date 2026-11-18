#include "ui_language/declaration.hpp"
#include "language/lexer.hpp"
#include "foundation/result.hpp"
#include <algorithm>
#include <optional>
#include <utility>

namespace atelier::ui_language {
namespace {
using namespace language;

using namespace language::detail;

struct Fragment {
    std::size_t begin{};

    std::size_t end{};

    SourcePosition position;
};

struct Evaluation {
    Fragment source;

    std::uint32_t node{};

    std::optional<Property> property;

    InputKind kind{InputKind::boolean};

    std::uint32_t argument{};
};

struct Initializer {
    std::uint32_t slot{};

    Fragment source;
};

constexpr std::pair<std::string_view, NodeKind> builtin_nodes[]{{"Column", NodeKind::column},
                                                                {"Row", NodeKind::row},
                                                                {"Text", NodeKind::text},
                                                                {"Foldout", NodeKind::foldout},
                                                                {"NumberRow", NodeKind::number_row},
                                                                {"TextField", NodeKind::text_field},
                                                                {"Tree", NodeKind::tree},
                                                                {"TreeItem", NodeKind::tree_item},
                                                                {"Repeat", NodeKind::repeat},
                                                                {"Button", NodeKind::button},
                                                                {"List", NodeKind::list},
                                                                {"Combo", NodeKind::combo},
                                                                {"ChoiceButton", NodeKind::choice_button},
                                                                {"ToggleButton", NodeKind::toggle_button},
                                                                {"Table", NodeKind::table},
                                                                {"Checkbox", NodeKind::checkbox},
                                                                {"ParameterRow", NodeKind::parameter_row},
                                                                {"Viewport", NodeKind::viewport},
                                                                {"Tab", NodeKind::tab},
                                                                {"Splitter", NodeKind::splitter},
                                                                {"Popup", NodeKind::popup},
                                                                {"MenuItem", NodeKind::menu_item},
                                                                {"Overlay", NodeKind::overlay},
                                                                {"CodeEditor", NodeKind::code_editor}};

std::optional<NodeKind> builtin_node(std::string_view name) {
    const auto at = std::ranges::find_if(builtin_nodes, [&](const auto& item) { return item.first == name; });
    return at == std::end(builtin_nodes) ? std::nullopt : std::optional{at->second};
}

bool height_strategy(std::string_view name) {
    return name == "height" || name == "expand_height" || name == "single_line" || name == "content_height";
}

bool width_strategy(std::string_view name) {
    return name == "width" || name == "expand_width" || name == "content_width" || name == "shrink_width" ||
           name == "flex_width" || name == "grow_width";
}

bool button_node(NodeKind kind) {
    return kind == NodeKind::button || kind == NodeKind::choice_button || kind == NodeKind::toggle_button ||
           kind == NodeKind::checkbox || kind == NodeKind::foldout;
}

bool container_node(NodeKind kind) {
    return kind == NodeKind::column || kind == NodeKind::row || kind == NodeKind::tree || kind == NodeKind::list ||
           kind == NodeKind::parameter_row || kind == NodeKind::popup || kind == NodeKind::overlay;
}

struct Header {
    std::string name;

    std::vector<Input> inputs;

    SourcePosition begin;

    Tokens body;
};

std::expected<Input, Diagnostic> parse_input(Tokens& tokens) {
    Input value;
    if (tokens.token_.text == "bool") {
        ATELIER_TRY(tokens.consume());
        value.kind = InputKind::boolean;
    } else if (tokens.token_.text == "double") {
        ATELIER_TRY(tokens.consume());
        value.kind = InputKind::number;
    } else {
        ATELIER_TRY_BIND(parsed_type, tokens.identifier());
        const auto type = *parsed_type;
        if (type.text == "EditBinding")
            value.kind = InputKind::edit;
        else if (type.text == "Text")
            value.kind = InputKind::text;
        else if (type.text == "TextEditBinding")
            value.kind = InputKind::text_edit;
        else if (type.text == "ActionBinding")
            value.kind = InputKind::action;
        else if (type.text == "TableBinding")
            value.kind = InputKind::table;
        else if (type.text == "CodeBinding")
            value.kind = InputKind::code;
        else if (type.text == "TooltipBinding")
            value.kind = InputKind::tooltip;
        else if (type.text == "TreeRowBinding")
            value.kind = InputKind::tree_row;
        else if (type.text == "Key")
            value.kind = InputKind::key;
        else if (type.text == "Items") {
            value.kind = InputKind::items;
            ATELIER_TRY(tokens.expect("<"));
            ATELIER_TRY_BIND(item, tokens.identifier());
            value.item_type = item->text;
            ATELIER_TRY(tokens.expect(">"));
        } else
            return reject(type.position, "Unknown UI input type.");
    }
    ATELIER_TRY_BIND(name, tokens.identifier());
    value.name = name->text;
    if (name->text == "input" || name->text == "state" || name->text == "event" || name->text == "node" ||
        name->text == "mounted" || name->text.starts_with("__ui_"))
        return reject(name->position, "Reserved UI input name.");
    return value;
}

std::expected<std::vector<Header>, Diagnostic> headers(std::string_view source) {
    ATELIER_TRY_BIND(created, Tokens::create(source));
    auto& tokens = *created;
    std::size_t braces{};
    while (tokens.token_.kind != Token::Kind::end && (tokens.token_.text != "ui" || braces)) {
        ATELIER_TRY_BIND(token, tokens.consume());
        if (token->text == "{")
            ++braces;
        else if (token->text == "}") {
            if (!braces)
                return reject(token->position, "Unexpected closing brace.");
            --braces;
        }
    }
    std::vector<Header> result;
    while (tokens.token_.kind != Token::Kind::end) {
        ATELIER_TRY_BIND(begin, tokens.expect("ui"));
        ATELIER_TRY_BIND(name, tokens.identifier());
        if (builtin_node(name->text))
            return reject(name->position, "UI component names cannot replace built-in node types.");
        if (result.size() >= 64 ||
            std::ranges::any_of(result, [&](const auto& item) { return item.name == name->text; }))
            return reject(begin->position, "Duplicate or excessive UI component type.");
        std::vector<Input> inputs;
        ATELIER_TRY(tokens.expect("("));
        ATELIER_TRY_BIND(empty, tokens.accept(")"));
        if (!*empty)
            for (;;) {
                ATELIER_TRY(tokens.expect("in"));
                ATELIER_TRY_BIND(input, parse_input(tokens));
                if (inputs.size() >= 1024 ||
                    std::ranges::any_of(inputs, [&](const auto& item) { return item.name == input->name; }))
                    return reject(tokens.token_.position, "Duplicate or excessive UI input.");
                inputs.push_back(std::move(*input));
                ATELIER_TRY_BIND(closed, tokens.accept(")"));
                if (*closed)
                    break;
                ATELIER_TRY(tokens.expect(","));
            }
        ATELIER_TRY(tokens.expect("{"));
        result.push_back({std::string(name->text), std::move(inputs), begin->position, tokens});
        braces = 1;
        while (braces && tokens.token_.kind != Token::Kind::end) {
            ATELIER_TRY_BIND(token, tokens.consume());
            if (token->text == "{")
                ++braces;
            else if (token->text == "}")
                --braces;
        }
        if (braces)
            return reject(begin->position, "Unterminated UI component.");
    }
    if (result.empty())
        return reject(tokens.token_.position, "Expected a ui component.");
    for (const auto& header : result)
        for (const auto& input : header.inputs)
            if (input.kind == InputKind::items &&
                std::ranges::none_of(result, [&](const auto& item) { return item.name == input.item_type; }))
                return reject(header.begin, "Unknown item component type: " + input.item_type);
    return result;
}

class Parser : private Tokens {
public:
    Parser(std::string_view name, std::string_view source, const Header& header, const std::vector<Header>& all)
        : Tokens(header.body), source_(source), headers_(all) {
        result_.source_name = name;
        result_.name = header.name;
        result_.inputs = header.inputs;
    }

    std::expected<Declaration, Diagnostic> run() {
        const Fragment helpers{0, headers_.front().begin.offset, {}};
        for (;;) {
            ATELIER_TRY_BIND(state, accept("state"));
            if (!*state)
                break;
            ATELIER_TRY_BIND(kind, state_kind());
            ATELIER_TRY_BIND(name, identifier());
            ATELIER_TRY(unique(*name, result_.states));
            ATELIER_TRY(expect("="));
            ATELIER_TRY_BIND(initial, fragment(";"));
            ATELIER_TRY(expect(";"));
            initializers_.push_back({static_cast<std::uint32_t>(result_.states.size()), *initial});
            result_.states.push_back({std::string(name->text), *kind});
        }
        for (;;) {
            ATELIER_TRY_BIND(closed, accept("}"));
            if (*closed)
                break;
            ATELIER_TRY(node(0, 0));
        }
        if (result_.nodes.empty())
            return reject(token_.position, "A ui component needs at least one node.");
        ATELIER_TRY(rewrite(helpers, false));
        const std::size_t operation_count = evaluations_.size() + 1;
        append("\nvoid __ui_dispatch(uint __ui_operation) {\n", {});
        append("if (__ui_operation >= " + std::to_string(operation_count) + ") { return; }\n", {});
        ATELIER_TRY(emit_dispatch(0, operation_count));
        append("}\n", {});
        return std::move(result_);
    }

private:
    std::string_view source_;

    Declaration result_;

    const std::vector<Header>& headers_;

    std::vector<Evaluation> evaluations_;

    std::vector<Initializer> initializers_;

    std::expected<void, Diagnostic> emit_operation(std::size_t index) {
        if (index == 0) {
            for (const auto& init : initializers_) {
                append("self.write_" + suffix(result_.states[init.slot].kind) + "(" + std::to_string(init.slot) + ", ",
                       init.source.position);
                ATELIER_TRY(rewrite(init.source, false));
                append(");\n", init.source.position);
            }
            for (std::uint32_t i = 0; i < result_.nodes.size(); ++i) {
                const auto& value = result_.nodes[i];
                append("self.create_node(" + std::to_string(i) + ", " + std::to_string(value.parent) + ", " +
                           std::to_string(static_cast<unsigned>(value.kind)) + ");\n",
                       {});
            }
            return {};
        }
        const auto& evaluation = evaluations_[index - 1];
        if (evaluation.property) {
            const auto property = *evaluation.property;
            ATELIER_TRY_BIND(parsed_reference, reference_kind(evaluation.source));
            const auto reference = *parsed_reference;
            const bool text = property == Property::text || property == Property::classes ||
                              property == Property::view || property == Property::empty_text ||
                              property == Property::menu_behavior || property == Property::tooltip_title ||
                              property == Property::tooltip_identity ||
                              (property == Property::tooltip && reference != InputKind::tooltip) ||
                              (property == Property::argument && evaluation.kind == InputKind::text);
            const bool numeric = property == Property::gap || property == Property::height ||
                                 property == Property::width || property == Property::padding_left ||
                                 property == Property::icon || property == Property::tree_rows ||
                                 property == Property::padding_top || property == Property::padding_right ||
                                 property == Property::padding_bottom || property == Property::content_width ||
                                 property == Property::content_height || property == Property::min_width ||
                                 property == Property::shrink_width ||
                                 (property >= Property::row_height && property <= Property::item_gap) ||
                                 (property >= Property::offset_x && property <= Property::step) ||
                                 (property == Property::argument && evaluation.kind == InputKind::number);
            std::string sink;
            if (property == Property::argument)
                sink = "set_argument_" + suffix(evaluation.kind);
            else if (text)
                sink = "set_text";
            else if (property == Property::edit)
                sink = result_.nodes[evaluation.node].kind == NodeKind::text_field ? "set_text_edit" : "set_edit";
            else if (property == Property::action)
                sink = "set_action";
            else if (property == Property::model)
                sink = "set_model";
            else if (property == Property::document)
                sink = "set_document";
            else if (property == Property::tooltip)
                sink = "set_tooltip";
            else if (property == Property::context)
                sink = reference == InputKind::text_edit ? "set_context_from_edit" : "set_context";
            else if (property == Property::items)
                sink = "set_items";
            else if (property == Property::tree_row)
                sink = "set_tree_row";
            else if (property == Property::radio_group)
                sink = "set_radio_group";
            else if (property == Property::selection_group)
                sink = "set_selection_group";
            else
                sink = numeric ? "set_number" : "set_bool";
            if (text && reference == InputKind::text)
                sink += "_reference";
            else if (text && reference == InputKind::text_edit)
                sink += "_from_edit";
            append("self." + sink + "(" + std::to_string(evaluation.node) + ", " +
                       std::to_string(property == Property::argument ? evaluation.argument
                                                                     : static_cast<unsigned>(property)) +
                       ", ",
                   evaluation.source.position);
            if (numeric)
                append("double(", evaluation.source.position);
            ATELIER_TRY(rewrite(evaluation.source, false));
            if (numeric)
                append(")", evaluation.source.position);
            append(");\n", evaluation.source.position);
            return {};
        }
        const auto kind = result_.nodes[evaluation.node].kind;
        ATELIER_TRY(
            rewrite(evaluation.source, true,
                    kind == NodeKind::foldout || kind == NodeKind::toggle_button || kind == NodeKind::checkbox));
        append("\n", evaluation.source.position);
        return {};
    }

    std::expected<void, Diagnostic> emit_dispatch(std::size_t first, std::size_t last) {
        if (last - first == 1)
            return emit_operation(first);
        const std::size_t middle = first + (last - first) / 2;
        append("if (__ui_operation < " + std::to_string(middle) + ") {\n", {});
        ATELIER_TRY(emit_dispatch(first, middle));
        append("} else {\n", {});
        ATELIER_TRY(emit_dispatch(middle, last));
        append("}\n", {});
        return {};
    }

    template <class T>
    std::expected<void, Diagnostic> unique(Token name, const std::vector<T>& items) {
        if (items.size() >= 1024 || name.text.starts_with("__ui_") ||
            std::ranges::any_of(items, [&](const auto& item) { return item.name == name.text; }))
            return reject(name.position, "Duplicate, reserved or excessive UI declaration.");
        return {};
    }

    std::expected<InputKind, Diagnostic> state_kind() {
        ATELIER_TRY_BIND(boolean, accept("bool"));
        if (*boolean)
            return InputKind::boolean;
        ATELIER_TRY_BIND(number, accept("double"));
        if (*number)
            return InputKind::number;
        ATELIER_TRY_BIND(token, identifier());
        return reject(token->position, "Component state requires bool or double.");
    }

    static std::string suffix(InputKind kind) {
        constexpr std::string_view names[]{"bool",  "number", "edit",  "text", "text_edit", "key",
                                           "items", "action", "table", "code", "tooltip",   "tree_row"};
        return std::string(names[static_cast<std::size_t>(kind)]);
    }

    std::expected<Fragment, Diagnostic> fragment(std::string_view terminator) {
        const auto begin = token_.position;
        std::size_t parentheses{};
        std::size_t brackets{};
        std::size_t braces{};
        while (token_.kind != Token::Kind::end) {
            if (!parentheses && !brackets && !braces && token_.text == terminator)
                return Fragment{begin.offset, token_.position.offset, begin};
            if (token_.text == "(")
                ++parentheses;
            else if (token_.text == ")") {
                if (!parentheses)
                    return reject(token_.position, "Unbalanced expression.");
                --parentheses;
            } else if (token_.text == "[")
                ++brackets;
            else if (token_.text == "]") {
                if (!brackets)
                    return reject(token_.position, "Unbalanced expression.");
                --brackets;
            } else if (token_.text == "{")
                ++braces;
            else if (token_.text == "}") {
                if (!braces)
                    return reject(token_.position, "Expected expression terminator.");
                --braces;
            }
            ATELIER_TRY(consume());
        }
        return reject(begin, "Unterminated UI expression or handler.");
    }

    std::expected<std::optional<InputKind>, Diagnostic> reference_kind(Fragment source) const {
        ATELIER_TRY_BIND(created, Tokens::create(source_.substr(source.begin, source.end - source.begin)));
        auto& tokens = *created;
        ATELIER_TRY_BIND(input, tokens.accept("input"));
        if (!*input)
            return {};
        ATELIER_TRY_BIND(dot, tokens.accept("."));
        if (!*dot)
            return {};
        ATELIER_TRY_BIND(name, tokens.consume());
        if (tokens.token_.kind != Token::Kind::end)
            return {};
        const auto at =
            std::ranges::find_if(result_.inputs, [&](const auto& input) { return input.name == name->text; });
        return at == result_.inputs.end() ? std::nullopt : std::optional{at->kind};
    }

    std::expected<void, Diagnostic> node(std::uint32_t parent, std::size_t depth) {
        if (depth >= 64)
            return reject(token_.position, "UI nesting exceeds 64 levels.");
        ATELIER_TRY_BIND(parsed_type, identifier());
        const auto type = *parsed_type;
        const auto builtin = builtin_node(type.text);
        const auto child = std::ranges::find_if(headers_, [&](const auto& value) { return value.name == type.text; });
        const auto kind = builtin.value_or(NodeKind::component);
        if (kind == NodeKind::component && child == headers_.end())
            return reject(type.position, "Unknown UI node or component type.");
        ATELIER_TRY_BIND(parsed_name, identifier());
        const auto name = *parsed_name;
        ATELIER_TRY(unique(name, result_.nodes));
        const auto slot = static_cast<std::uint32_t>(result_.nodes.size());
        result_.nodes.push_back({std::string(name.text), kind, parent,
                                 kind == NodeKind::component ? std::string(type.text) : std::string{}});
        ATELIER_TRY(expect("{"));
        std::vector<std::string_view> properties;
        bool has_event{};
        bool tooltip_binding{};
        for (;;) {
            ATELIER_TRY_BIND(closed, accept("}"));
            if (*closed)
                break;
            ATELIER_TRY_BIND(handler, accept("on"));
            if (*handler) {
                ATELIER_TRY_BIND(event, identifier());
                if (event->text != "activate" || (kind != NodeKind::foldout && !button_node(kind)) || has_event)
                    return reject(event->position, "Expected one activate handler on a Foldout or button.");
                has_event = true;
                ATELIER_TRY(expect("{"));
                ATELIER_TRY_BIND(body, fragment("}"));
                ATELIER_TRY(expect("}"));
                const auto operation = static_cast<std::uint32_t>(evaluations_.size() + 1);
                evaluations_.push_back({*body, slot, {}});
                result_.events.push_back({slot, operation, event->position});
                continue;
            }
            const auto saved_lexer = lexer_;
            const auto saved_token = token_;
            ATELIER_TRY_BIND(parsed_property, identifier());
            const auto property = *parsed_property;
            ATELIER_TRY_BIND(binding, accept(":"));
            if (!*binding) {
                lexer_ = saved_lexer;
                token_ = saved_token;
                if (!container_node(kind))
                    return reject(type.position, "Only layout containers, List and Tree accept child nodes.");
                ATELIER_TRY(node(slot + 1, depth + 1));
                continue;
            }
            if (std::ranges::find(properties, property.text) != properties.end())
                return reject(property.position, "Duplicate UI property or argument.");
            if (kind != NodeKind::component && height_strategy(property.text) &&
                std::ranges::any_of(properties, height_strategy))
                return reject(
                    property.position,
                    "height, expand_height, content_height and single_line are mutually exclusive height strategies.");
            if (kind != NodeKind::component && width_strategy(property.text) &&
                std::ranges::any_of(properties, width_strategy))
                return reject(
                    property.position,
                    "width, expand_width, content_width and shrink_width are mutually exclusive width strategies.");
            if (kind != NodeKind::component && (property.text == "panel" || property.text == "caption") &&
                std::ranges::any_of(properties, [](auto name) { return name == "panel" || name == "caption"; }))
                return reject(property.position, "panel and caption are mutually exclusive roles.");
            properties.push_back(property.text);
            Property value{};
            InputKind value_kind{InputKind::boolean};
            std::uint32_t argument{};
            if (kind == NodeKind::component && property.text != "mounted") {
                const auto at =
                    std::ranges::find_if(child->inputs, [&](const auto& input) { return input.name == property.text; });
                if (at == child->inputs.end())
                    return reject(property.position, "Unknown component input.");
                value = Property::argument;
                value_kind = at->kind;
                argument = static_cast<std::uint32_t>(at - child->inputs.begin());
            } else {
                constexpr std::string_view names[]{"text",          "classes",
                                                   "gap",           "height",
                                                   "width",         "padding_left",
                                                   "enabled",       "selected",
                                                   "collapsed",     "caption",
                                                   "edit",          "expand_height",
                                                   "icon",          "row",
                                                   "rows",          "mounted",
                                                   "items",         "padding_top",
                                                   "padding_right", "padding_bottom",
                                                   "align_center",  "single_line",
                                                   "action",        "selection_group",
                                                   "content_width", "content_height",
                                                   "expand_width",  "min_width",
                                                   "wrap",          "panel",
                                                   "model",         "context",
                                                   "shrink_width",  "view",
                                                   "row_height",    "header_height",
                                                   "tile_width",    "tile_height",
                                                   "item_gap",      "empty_text",
                                                   "modified",      "out_of_flow",
                                                   "offset_x",      "offset_y",
                                                   "anchor_x",      "anchor_y",
                                                   "pivot_x",       "pivot_y",
                                                   "flex_width",    "grow_width",
                                                   "red",           "green",
                                                   "blue",          "value",
                                                   "minimum",       "maximum",
                                                   "step",          "vertical",
                                                   "click_action",  "menu_branch",
                                                   "menu_behavior", "radio_group",
                                                   "document",      "tooltip",
                                                   "tooltip_title", "tooltip_identity",
                                                   "draggable"};
                const auto at = std::ranges::find(names, property.text);
                if (at == std::end(names))
                    return reject(property.position, "Unknown UI property.");
                value = static_cast<Property>(at - std::begin(names));
                if ((kind == NodeKind::component && value != Property::mounted) ||
                    (kind == NodeKind::repeat && value != Property::mounted && value != Property::items))
                    return reject(property.position, "This structural scope accepts only mounted and its typed input.");
                if (value == Property::edit && kind != NodeKind::number_row && kind != NodeKind::text_field)
                    return reject(property.position, "edit requires NumberRow or TextField.");
                if (value == Property::action &&
                    ((!button_node(kind) && kind != NodeKind::table) || kind == NodeKind::foldout))
                    return reject(property.position, "action requires a button, Checkbox or Table.");
                if (value == Property::document && kind != NodeKind::code_editor)
                    return reject(property.position, "document requires CodeEditor.");
                if (value == Property::model && kind != NodeKind::table)
                    return reject(property.position, "model requires Table.");
                if (value >= Property::view && value <= Property::empty_text && kind != NodeKind::table)
                    return reject(property.position, "Table presentation properties require Table.");
                if (value == Property::context && kind == NodeKind::number_row)
                    return reject(property.position, "NumberRow context is owned by its EditBinding.");
                if (value == Property::selection_group && kind != NodeKind::choice_button)
                    return reject(property.position, "selection_group requires ChoiceButton.");
                if (value == Property::selected && kind != NodeKind::foldout && kind != NodeKind::tree_item &&
                    kind != NodeKind::choice_button && kind != NodeKind::toggle_button && kind != NodeKind::combo &&
                    kind != NodeKind::checkbox && kind != NodeKind::tab && kind != NodeKind::menu_item &&
                    kind != NodeKind::button && !container_node(kind))
                    return reject(property.position,
                                  "selected requires Foldout, TreeItem, ChoiceButton, ToggleButton or Combo.");
                if ((value == Property::menu_branch || value == Property::menu_behavior ||
                     value == Property::radio_group) &&
                    kind != NodeKind::menu_item)
                    return reject(property.position, "Menu properties require MenuItem.");
                if ((value == Property::value || value == Property::minimum || value == Property::maximum ||
                     value == Property::step || value == Property::vertical || value == Property::click_action) &&
                    kind != NodeKind::splitter)
                    return reject(property.position, "Drag properties require Splitter.");
                if (value == Property::caption && kind != NodeKind::text)
                    return reject(property.position, "caption requires Text.");
                if ((value == Property::align_center || value == Property::wrap) && !container_node(kind))
                    return reject(property.position, "align_center and wrap require a layout container.");
                if (value == Property::text &&
                    ((container_node(kind) && kind != NodeKind::parameter_row) || kind == NodeKind::text_field ||
                     kind == NodeKind::table || kind == NodeKind::code_editor))
                    return reject(property.position, "Use Text or a typed text edit binding.");
                if (kind == NodeKind::number_row && value != Property::text && value != Property::classes &&
                    value != Property::enabled && value != Property::collapsed && value != Property::edit &&
                    value != Property::tooltip && value != Property::tooltip_title &&
                    value != Property::tooltip_identity)
                    return reject(
                        property.position,
                        "NumberRow layout and field state are owned by the shared parameter row and EditBinding.");
                if (kind == NodeKind::parameter_row && value != Property::text && value != Property::classes &&
                    value != Property::enabled && value != Property::collapsed && value != Property::modified &&
                    value != Property::tooltip && value != Property::tooltip_title &&
                    value != Property::tooltip_identity)
                    return reject(property.position,
                                  "ParameterRow owns its shared layout; put outer layout on its parent.");
                if ((value == Property::tree_row || value == Property::draggable) && kind != NodeKind::tree_item)
                    return reject(property.position, "row and draggable require TreeItem.");
                if (value == Property::tree_rows && kind != NodeKind::tree)
                    return reject(property.position, "rows requires Tree.");
                if (value == Property::mounted && kind != NodeKind::component && kind != NodeKind::repeat)
                    return reject(property.position, "mounted requires a component or Repeat scope.");
                if (value == Property::items && kind != NodeKind::repeat)
                    return reject(property.position, "items requires Repeat.");
            }
            ATELIER_TRY_BIND(parsed_expression, fragment(";"));
            const auto expression = *parsed_expression;
            ATELIER_TRY(expect(";"));
            ATELIER_TRY_BIND(parsed_reference, reference_kind(expression));
            const auto reference = *parsed_reference;
            if (value == Property::tooltip && reference == InputKind::tooltip)
                tooltip_binding = true;
            if (tooltip_binding && (std::ranges::find(properties, "tooltip_title") != properties.end() ||
                                    std::ranges::find(properties, "tooltip_identity") != properties.end()))
                return reject(
                    property.position,
                    "TooltipBinding owns its title, identity and description; do not combine it with tooltip text properties.");
            if (value == Property::argument && value_kind == InputKind::tooltip && reference != InputKind::tooltip)
                return reject(expression.position, "Tooltip arguments require a direct TooltipBinding input.");
            if ((value == Property::selection_group || value == Property::context || value == Property::radio_group) &&
                reference != InputKind::key && !(value == Property::context && reference == InputKind::text_edit)) {
                ATELIER_TRY_BIND(created,
                                 Tokens::create(source_.substr(expression.begin, expression.end - expression.begin)));
                auto& tokens = *created;
                ATELIER_TRY_BIND(node_reference, tokens.accept("node"));
                ATELIER_TRY_BIND(member, *node_reference ? tokens.accept(".") : std::expected<bool, Diagnostic>{false});
                if (!*member || tokens.token_.kind != Token::Kind::identifier)
                    return reject(expression.position,
                                  std::string(property.text) + " requires node.name or a direct Key input.");
                ATELIER_TRY(tokens.consume());
                if (tokens.token_.kind != Token::Kind::end)
                    return reject(expression.position,
                                  std::string(property.text) + " requires node.name or a direct Key input.");
            }
            if ((value == Property::action || (value == Property::argument && value_kind == InputKind::action)) &&
                reference != InputKind::action)
                return reject(expression.position, "Action bindings require a direct ActionBinding input.");
            if ((value == Property::model || (value == Property::argument && value_kind == InputKind::table)) &&
                reference != InputKind::table)
                return reject(expression.position, "Table models require a direct TableBinding input.");
            if ((value == Property::tree_row || (value == Property::argument && value_kind == InputKind::tree_row)) &&
                reference != InputKind::tree_row)
                return reject(expression.position, "Tree rows require a direct TreeRowBinding input.");
            if ((value == Property::document || (value == Property::argument && value_kind == InputKind::code)) &&
                reference != InputKind::code)
                return reject(expression.position, "CodeEditor requires a direct CodeBinding input.");
            if ((value == Property::argument && value_kind == InputKind::items) || value == Property::items) {
                if (reference != InputKind::items)
                    return reject(expression.position, value == Property::items
                                                           ? "Repeat requires a direct Items input."
                                                           : "Items arguments require a direct typed input.");
                ATELIER_TRY_BIND(created,
                                 Tokens::create(source_.substr(expression.begin, expression.end - expression.begin)));
                auto& tokens = *created;
                ATELIER_TRY(tokens.expect("input"));
                ATELIER_TRY(tokens.expect("."));
                ATELIER_TRY_BIND(input, tokens.identifier());
                const auto at =
                    std::ranges::find_if(result_.inputs, [&](const auto& item) { return item.name == input->text; });
                if (value == Property::items)
                    result_.nodes[slot].component_type = at->item_type;
                else if (at->item_type != child->inputs[argument].item_type)
                    return reject(expression.position, "Component Items argument has a different element type.");
            }
            const auto operation = static_cast<std::uint32_t>(evaluations_.size() + 1);
            evaluations_.push_back({expression, slot, value, value_kind, argument});
            result_.bindings.push_back({slot, operation, value, property.position});
        }
        if ((kind == NodeKind::number_row || kind == NodeKind::text_field) &&
            std::ranges::find(properties, "edit") == properties.end())
            return reject(name.position, "Editable controls require a typed edit binding.");
        if (kind == NodeKind::code_editor && std::ranges::find(properties, "document") == properties.end())
            return reject(name.position, "CodeEditor requires a typed document binding.");
        if (kind == NodeKind::table && std::ranges::find(properties, "model") == properties.end())
            return reject(name.position, "Table requires a typed model binding.");
        if (button_node(kind) && has_event && std::ranges::find(properties, "action") != properties.end())
            return reject(name.position, "A button cannot combine action with a local activate handler.");
        if (kind == NodeKind::choice_button && std::ranges::find(properties, "selection_group") == properties.end())
            return reject(name.position, "ChoiceButton requires an explicit selection_group.");
        if (kind == NodeKind::repeat && result_.nodes[slot].component_type.empty())
            return reject(name.position, "Repeat requires items.");
        if (kind == NodeKind::component)
            for (const auto& input : child->inputs)
                if (std::ranges::find(properties, input.name) == properties.end())
                    return reject(name.position, "Missing component input: " + input.name);
        return {};
    }

    void append(std::string_view value, SourcePosition origin) {
        const auto begin = result_.generated_source.size();
        result_.generated_source += value;
        if (!value.empty())
            result_.mappings.push_back({begin, result_.generated_source.size(), origin});
    }

    std::expected<void, Diagnostic> rewrite(Fragment fragment, bool event, bool checked = false) {
        const auto position = [&](SourcePosition local) {
            local.offset += fragment.begin;
            local.column += local.line == 1 ? fragment.position.column - 1 : 0;
            local.line += fragment.position.line - 1;
            return local;
        };
        auto rewritten = [&]() -> std::expected<void, Diagnostic> {
            ATELIER_TRY_BIND(created, Tokens::create(source_.substr(fragment.begin, fragment.end - fragment.begin)));
            auto& tokens = *created;
            std::optional<std::size_t> assignment;
            std::size_t parentheses{};
            while (tokens.token_.kind != Token::Kind::end) {
                ATELIER_TRY_BIND(parsed_token, tokens.consume());
                const auto token = *parsed_token;
                const auto origin = position(token.position);
                // Source permits assignments in for's step, whose terminator is ')'.
                // Calls within the value must close before the generated write call.
                if (assignment && parentheses == *assignment && (token.text == ";" || token.text == ")")) {
                    append(")", origin);
                    assignment.reset();
                }
                if (token.text == "(")
                    ++parentheses;
                else if (token.text == ")") {
                    if (!parentheses)
                        return reject(token.position, "Unbalanced expression.");
                    --parentheses;
                }
                if (token.text == "self" || token.text == "new" || token.text == "yield" || token.text == "async" ||
                    token.text.starts_with("__ui_"))
                    return reject(
                        token.position,
                        "UI expressions use typed inputs/state; native context access and asynchronous effects are not permitted.");
                if (token.text == "input" || token.text == "state") {
                    ATELIER_TRY_BIND(dot, tokens.accept("."));
                    if (!*dot)
                        return reject(token.position,
                                      "input, state and event are reserved UI identifiers; use a member after '.'.");
                    ATELIER_TRY_BIND(name, tokens.identifier());
                    const auto state = token.text == "state";
                    std::optional<std::uint32_t> index;
                    InputKind kind{};
                    if (state) {
                        for (std::uint32_t i = 0; i < result_.states.size(); ++i)
                            if (result_.states[i].name == name->text) {
                                index = i;
                                kind = result_.states[i].kind;
                            }
                    } else
                        for (std::uint32_t i = 0; i < result_.inputs.size(); ++i)
                            if (result_.inputs[i].name == name->text) {
                                index = i;
                                kind = result_.inputs[i].kind;
                            }
                    if (!index)
                        return reject(token.position, "Unknown UI input or state.");
                    ATELIER_TRY_BIND(write, tokens.accept("="));
                    if (*write) {
                        if (!state || !event || assignment)
                            return reject(token.position,
                                          "Only event blocks may assign component state; inputs are read-only.");
                        assignment = parentheses;
                        append("self.write_" + suffix(kind) + "(" + std::to_string(*index) + ", ", origin);
                    } else
                        append("self." + std::string(state ? "state_" : "read_") + suffix(kind) + "(" +
                                   std::to_string(*index) + ") ",
                               origin);
                } else if (token.text == "node") {
                    ATELIER_TRY(tokens.expect("."));
                    ATELIER_TRY_BIND(name, tokens.identifier());
                    const auto at =
                        std::ranges::find_if(result_.nodes, [&](const auto& item) { return item.name == name->text; });
                    if (at == result_.nodes.end())
                        return reject(name->position, "Unknown declaration node.");
                    if (at->kind == NodeKind::component || at->kind == NodeKind::repeat ||
                        at->kind == NodeKind::text_field)
                        return reject(name->position,
                                      "node references require a concrete node with a declaration-owned key.");
                    append("self.node_key(" + std::to_string(at - result_.nodes.begin()) + ") ", origin);
                } else if (token.text == "event") {
                    if (!event)
                        return reject(token.position, "event is reserved and only available inside an event handler.");
                    if (!checked)
                        return reject(token.position, "Button and ChoiceButton activation do not expose event values.");
                    ATELIER_TRY(tokens.expect("."));
                    ATELIER_TRY_BIND(name, tokens.identifier());
                    if (name->text != "checked")
                        return reject(name->position, "This event exposes checked: bool.");
                    append("self.event_bool() ", origin);
                } else {
                    append(token.text, origin);
                    append(" ", origin);
                }
            }
            if (assignment)
                return reject(tokens.token_.position, "State assignment requires a statement terminator.");
            return {};
        }();
        if (!rewritten) {
            rewritten.error().position = position(rewritten.error().position);
        }
        return rewritten;
    }
};
}

std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(std::string_view name,
                                                                           std::string_view source) {
    ATELIER_TRY_BIND(all, headers(source));
    std::vector<Declaration> result;
    for (const auto& header : *all) {
        ATELIER_TRY_BIND(declaration, Parser(name, source, header, *all).run());
        result.push_back(std::move(*declaration));
    }
    return result;
}

std::expected<Declaration, language::Diagnostic> lower(std::string_view name, std::string_view source) {
    ATELIER_TRY_BIND(all, lower_module(name, source));
    if (all->size() != 1)
        return std::unexpected(language::Diagnostic{{}, "Choose a named component from this UI module."});
    return std::move(all->front());
}

} // namespace atelier::ui_language
