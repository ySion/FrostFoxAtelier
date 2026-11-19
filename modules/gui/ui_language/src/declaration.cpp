#include "ui_language/declaration.hpp"
#include "language/lexer.hpp"
#include "foundation/result.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace atelier::ui_language {
namespace {
using namespace language;

using namespace language::detail;

struct Fragment {
    SourcePosition position;

    SourcePosition end_position;

    std::vector<Token> tokens;
};

struct Evaluation {
    Fragment source;

    std::uint32_t node{};

    std::optional<Property> property;

    InputKind kind{InputKind::boolean};

    std::uint32_t argument{};

    std::size_t binding_index{};

    std::size_t event_index{};
};

struct Initializer {
    std::uint32_t slot{};

    Fragment source;
};

bool is_numeric_property(Property property) {
    return property == Property::gap || property == Property::height || property == Property::width ||
           property == Property::padding_left || property == Property::icon || property == Property::tree_rows ||
           property == Property::padding_top || property == Property::padding_right ||
           property == Property::padding_bottom || property == Property::content_width ||
           property == Property::content_height || property == Property::min_width ||
           property == Property::shrink_width || (property >= Property::row_height && property <= Property::item_gap) ||
           (property >= Property::offset_x && property <= Property::step);
}

bool is_text_property(Property property) {
    return property == Property::text || property == Property::classes || property == Property::view ||
           property == Property::empty_text || property == Property::menu_behavior ||
           property == Property::tooltip_title || property == Property::tooltip_identity;
}

std::optional<bool> try_bool_literal(std::span<const Token> tokens) {
    if (tokens.size() == 1 && (tokens[0].text == "true" || tokens[0].text == "false"))
        return tokens[0].text == "true";
    return std::nullopt;
}

std::optional<double> try_number_literal(std::span<const Token> tokens, bool double_only) {
    if (tokens.empty() || tokens.size() > 2)
        return std::nullopt;
    bool negative = false;
    std::size_t index = 0;
    if (tokens.size() == 2) {
        if (tokens[0].text == "-")
            negative = true;
        else if (tokens[0].text != "+")
            return std::nullopt;
        index = 1;
    }
    const auto& token = tokens[index];
    if (token.kind != Token::Kind::number || token.text.empty())
        return std::nullopt;
    auto text = token.text;
    const char suffix = text.back();
    const bool is_double = suffix == 'd' || suffix == 'D';
    const bool is_float = suffix == 'f' || suffix == 'F';
    if (double_only && !is_double)
        return std::nullopt;
    if (is_double || is_float)
        text.remove_suffix(1);
    if (is_double) {
        double value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
            return std::nullopt;
        return negative ? -value : value;
    }
    if (is_float || text.find_first_of(".eE") != std::string_view::npos) {
        float value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
            return std::nullopt;
        return static_cast<double>(negative ? -value : value);
    }
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    if (negative)
        return value <= 2147483648u ? std::optional{static_cast<double>(-static_cast<std::int64_t>(value))}
                                    : std::nullopt;
    return static_cast<double>(value);
}

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
    bool declaration_start = true;
    while (tokens.token_.kind != Token::Kind::end && (tokens.token_.text != "ui" || braces)) {
        if (!braces && declaration_start &&
            (tokens.token_.text == "struct" || tokens.token_.text == "enum" || tokens.token_.text == "["))
            return reject(tokens.token_.position, "UI modules do not support Source struct or enum declarations.");
        ATELIER_TRY_BIND(token, tokens.consume());
        if (token->text == "{") {
            ++braces;
            declaration_start = false;
        } else if (token->text == "}") {
            if (!braces)
                return reject(token->position, "Unexpected closing brace.");
            --braces;
            if (!braces)
                declaration_start = true;
        } else if (!braces)
            declaration_start = token->text == ";";
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

std::expected<std::shared_ptr<const PreparedHelpers>, Diagnostic> prepare_helpers(std::string_view source,
                                                                                  std::size_t end) {
    auto prepared = std::make_shared<PreparedHelpers>();
    ATELIER_TRY_BIND(created, Tokens::create(source.substr(0, end)));
    auto& tokens = *created;
    std::string_view previous;
    while (tokens.token_.kind != Token::Kind::end) {
        ATELIER_TRY_BIND(parsed_token, tokens.consume());
        const auto token = *parsed_token;
        const bool member = previous == "." || previous == "::";
        if (!member && (token.text == "self" || token.text == "new" || token.text == "yield" || token.text == "async" ||
                        token.text.starts_with("__ui_")))
            return reject(
                token.position,
                "UI helpers use explicit parameters; native context access and asynchronous effects are not permitted.");
        const auto begin = prepared->generated_source.size();
        prepared->generated_source += token.text;
        prepared->mappings.push_back({begin, prepared->generated_source.size(), token.position});
        prepared->generated_source += ' ';
        previous = token.text;
    }
    return prepared->generated_source.empty() ? std::shared_ptr<const PreparedHelpers>{} : std::move(prepared);
}

class Parser : private Tokens {
public:
    Parser(std::string_view name, const Header& header, const std::vector<Header>& all,
           std::shared_ptr<const PreparedHelpers> helpers)
        : Tokens(header.body), headers_(all) {
        result_.source_name = name;
        result_.name = header.name;
        result_.inputs = header.inputs;
        result_.helpers = std::move(helpers);
    }

    std::expected<Declaration, Diagnostic> run() {
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
            State declared{std::string(name->text), *kind, {}};
            // Only the leading literals can run before the JIT initializer. A dynamic
            // initializer may read a later state, which must still hold its type default.
            if (initializers_.empty()) {
                if (*kind == InputKind::boolean) {
                    if (const auto value = try_bool_literal(initial->tokens))
                        declared.initial_value = *value;
                } else if (const auto value = try_number_literal(initial->tokens, true)) {
                    declared.initial_value = *value;
                }
            }
            if (std::holds_alternative<std::monostate>(declared.initial_value))
                initializers_.push_back({static_cast<std::uint32_t>(result_.states.size()), std::move(*initial)});
            result_.states.push_back(std::move(declared));
        }
        for (;;) {
            ATELIER_TRY_BIND(closed, accept("}"));
            if (*closed)
                break;
            ATELIER_TRY(node(0, 0));
        }
        if (result_.nodes.empty())
            return reject(token_.position, "A ui component needs at least one node.");
        result_.has_jit_init = !initializers_.empty();
        std::vector<std::size_t> jit_evaluations;
        jit_evaluations.reserve(evaluations_.size());
        const std::uint32_t op_base = result_.has_jit_init ? 1u : 0u;
        for (std::size_t i = 0; i < evaluations_.size(); ++i) {
            const auto& eval = evaluations_[i];
            if (!eval.property) {
                const auto op = static_cast<std::uint32_t>(op_base + jit_evaluations.size());
                result_.events[eval.event_index].operation = op;
                jit_evaluations.push_back(i);
                continue;
            }
            ATELIER_TRY_BIND(classified, classify_binding(eval));
            if (*classified)
                continue;
            const auto op = static_cast<std::uint32_t>(op_base + jit_evaluations.size());
            result_.bindings[eval.binding_index].operation = op;
            jit_evaluations.push_back(i);
        }
        jit_evaluations_ = std::move(jit_evaluations);
        const std::size_t operation_count = op_base + jit_evaluations_.size();
        result_.needs_jit = operation_count > 0;
        if (result_.needs_jit) {
            if (result_.helpers) {
                result_.generated_source = result_.helpers->generated_source;
                result_.mappings = result_.helpers->mappings;
            }
            append("\nvoid __ui_dispatch(uint __ui_operation) {\n", {});
            append("if (__ui_operation >= " + std::to_string(operation_count) + ") { return; }\n", {});
            ATELIER_TRY(emit_dispatch(0, operation_count));
            append("}\n", {});
        }
        return std::move(result_);
    }

private:
    Declaration result_;

    const std::vector<Header>& headers_;

    std::vector<Evaluation> evaluations_;

    std::vector<std::size_t> jit_evaluations_;

    std::vector<Initializer> initializers_;

    std::optional<DirectSource> try_direct_source(std::span<const Token> tokens) const {
        if (tokens.size() != 3 || tokens[1].text != "." || tokens[2].kind != Token::Kind::identifier)
            return std::nullopt;
        const auto member = tokens[2].text;
        if (tokens[0].text == "input") {
            for (std::uint32_t i = 0; i < result_.inputs.size(); ++i)
                if (result_.inputs[i].name == member)
                    return DirectSource{DirectSource::Kind::input, i, result_.inputs[i].kind};
            return std::nullopt;
        }
        if (tokens[0].text == "state") {
            for (std::uint32_t i = 0; i < result_.states.size(); ++i)
                if (result_.states[i].name == member)
                    return DirectSource{DirectSource::Kind::state, i, result_.states[i].kind};
            return std::nullopt;
        }
        if (tokens[0].text == "node") {
            for (std::uint32_t i = 0; i < result_.nodes.size(); ++i) {
                const auto& item = result_.nodes[i];
                if (item.name != member)
                    continue;
                if (item.kind == NodeKind::component || item.kind == NodeKind::repeat ||
                    item.kind == NodeKind::text_field)
                    return std::nullopt;
                return DirectSource{DirectSource::Kind::node_key, i, InputKind::key};
            }
        }
        return std::nullopt;
    }

    static bool compatible_direct(Property property, InputKind argument_kind, NodeKind node_kind,
                                  const DirectSource& source) {
        if (property == Property::argument)
            return source.input_kind == argument_kind ||
                   (argument_kind == InputKind::text && source.kind == DirectSource::Kind::input &&
                    source.input_kind == InputKind::text_edit);
        if (property == Property::tooltip)
            return source.kind == DirectSource::Kind::input &&
                   (source.input_kind == InputKind::tooltip || source.input_kind == InputKind::text ||
                    source.input_kind == InputKind::text_edit);
        if (is_text_property(property))
            return source.kind == DirectSource::Kind::input &&
                   (source.input_kind == InputKind::text || source.input_kind == InputKind::text_edit);
        if (is_numeric_property(property))
            return (source.kind == DirectSource::Kind::input || source.kind == DirectSource::Kind::state) &&
                   source.input_kind == InputKind::number;
        if (property == Property::edit)
            return source.kind == DirectSource::Kind::input &&
                   source.input_kind == (node_kind == NodeKind::text_field ? InputKind::text_edit : InputKind::edit);
        if (property == Property::action)
            return source.kind == DirectSource::Kind::input && source.input_kind == InputKind::action;
        if (property == Property::model)
            return source.kind == DirectSource::Kind::input && source.input_kind == InputKind::table;
        if (property == Property::document)
            return source.kind == DirectSource::Kind::input && source.input_kind == InputKind::code;
        if (property == Property::tree_row)
            return source.kind == DirectSource::Kind::input && source.input_kind == InputKind::tree_row;
        if (property == Property::items)
            return source.kind == DirectSource::Kind::input && source.input_kind == InputKind::items;
        if (property == Property::context)
            return source.kind == DirectSource::Kind::node_key ||
                   (source.kind == DirectSource::Kind::input &&
                    (source.input_kind == InputKind::key || source.input_kind == InputKind::text_edit));
        if (property == Property::selection_group || property == Property::radio_group)
            return source.kind == DirectSource::Kind::node_key ||
                   (source.kind == DirectSource::Kind::input && source.input_kind == InputKind::key);
        return (source.kind == DirectSource::Kind::input || source.kind == DirectSource::Kind::state) &&
               source.input_kind == InputKind::boolean;
    }

    std::expected<bool, Diagnostic> classify_binding(const Evaluation& eval) {
        auto& binding = result_.bindings[eval.binding_index];
        const auto property = *eval.property;
        const auto node_kind = result_.nodes[eval.node].kind;
        const std::span<const Token> tokens = eval.source.tokens;
        if (const auto direct = try_direct_source(tokens)) {
            if (compatible_direct(property, eval.kind, node_kind, *direct)) {
                binding.mode = Binding::Mode::direct;
                binding.direct = *direct;
                return true;
            }
            return false;
        }
        const bool accepts_text = is_text_property(property) || property == Property::tooltip ||
                                  (property == Property::argument && eval.kind == InputKind::text);
        if (accepts_text && tokens.size() == 1 && tokens[0].kind == Token::Kind::string) {
            ATELIER_TRY_BIND(decoded, string_value(tokens[0]));
            binding.mode = Binding::Mode::literal;
            binding.literal = std::move(*decoded);
            return true;
        }
        const bool accepts_number =
            is_numeric_property(property) || (property == Property::argument && eval.kind == InputKind::number);
        if (accepts_number) {
            if (const auto value = try_number_literal(tokens, false)) {
                binding.mode = Binding::Mode::literal;
                binding.literal = *value;
                return true;
            }
            return false;
        }
        const bool accepts_bool =
            (property == Property::argument && eval.kind == InputKind::boolean) ||
            (property != Property::argument && !accepts_text && !accepts_number && property != Property::edit &&
             property != Property::action && property != Property::model && property != Property::document &&
             property != Property::tree_row && property != Property::items && property != Property::context &&
             property != Property::selection_group && property != Property::radio_group);
        if (accepts_bool) {
            if (const auto value = try_bool_literal(tokens)) {
                binding.mode = Binding::Mode::literal;
                binding.literal = *value;
                return true;
            }
        }
        return false;
    }

    std::expected<void, Diagnostic> emit_operation(std::size_t index) {
        if (result_.has_jit_init && index == 0) {
            for (const auto& init : initializers_) {
                append("self.write_" + suffix(result_.states[init.slot].kind) + "(" + std::to_string(init.slot) + ", ",
                       init.source.position);
                ATELIER_TRY(rewrite(init.source, false));
                append(");\n", init.source.position);
            }
            return {};
        }
        const auto& evaluation = evaluations_[jit_evaluations_[index - (result_.has_jit_init ? 1u : 0u)]];
        if (evaluation.property) {
            const auto property = *evaluation.property;
            const auto reference = reference_kind(evaluation.source);
            const bool text = is_text_property(property) ||
                              (property == Property::tooltip && reference != InputKind::tooltip) ||
                              (property == Property::argument && evaluation.kind == InputKind::text);
            const bool numeric = is_numeric_property(property) ||
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
        std::vector<Token> tokens;
        std::size_t parentheses{};
        std::size_t brackets{};
        std::size_t braces{};
        while (token_.kind != Token::Kind::end) {
            if (!parentheses && !brackets && !braces && token_.text == terminator) {
                if (terminator == ";" && tokens.empty())
                    return reject(begin, "Expected a UI expression.");
                return Fragment{begin, token_.position, std::move(tokens)};
            }
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
            ATELIER_TRY_BIND(parsed_token, consume());
            tokens.push_back(*parsed_token);
        }
        return reject(begin, "Unterminated UI expression or handler.");
    }

    std::optional<InputKind> reference_kind(const Fragment& source) const {
        if (source.tokens.size() != 3 || source.tokens[0].text != "input" || source.tokens[1].text != ".")
            return std::nullopt;
        const auto at = std::ranges::find_if(result_.inputs,
                                             [&](const auto& input) { return input.name == source.tokens[2].text; });
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
                const auto event_index = result_.events.size();
                result_.events.push_back({slot, 0, event->position});
                evaluations_.push_back({std::move(*body), slot, {}, InputKind::boolean, 0, 0, event_index});
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
                    "width, expand_width, content_width, shrink_width, flex_width and grow_width are mutually exclusive width strategies.");
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
                    return reject(
                        property.position,
                        "selected requires Foldout, TreeItem, ChoiceButton, ToggleButton, Combo, Checkbox, Tab, MenuItem, Button or a layout container.");
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
            auto expression = std::move(*parsed_expression);
            ATELIER_TRY(expect(";"));
            const auto reference = reference_kind(expression);
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
                if (expression.tokens.size() != 3 || expression.tokens[0].text != "node" ||
                    expression.tokens[1].text != "." || expression.tokens[2].kind != Token::Kind::identifier)
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
                const auto at = std::ranges::find_if(
                    result_.inputs, [&](const auto& item) { return item.name == expression.tokens[2].text; });
                if (value == Property::items)
                    result_.nodes[slot].component_type = at->item_type;
                else if (at->item_type != child->inputs[argument].item_type)
                    return reject(expression.position, "Component Items argument has a different element type.");
            }
            const auto binding_index = result_.bindings.size();
            result_.bindings.push_back(
                {slot, 0, value, value_kind, argument, Binding::Mode::jit, {}, {}, property.position});
            evaluations_.push_back({std::move(expression), slot, value, value_kind, argument, binding_index, 0});
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

    std::expected<void, Diagnostic> rewrite(const Fragment& fragment, bool event, bool checked = false) {
        const auto& tokens = fragment.tokens;
        std::size_t cursor{};
        const auto expect_dot = [&]() -> std::expected<void, Diagnostic> {
            if (cursor >= tokens.size() || tokens[cursor].text != ".")
                return reject(cursor < tokens.size() ? tokens[cursor].position : fragment.end_position,
                              "Expected '.'.");
            ++cursor;
            return {};
        };
        const auto expect_identifier = [&]() -> std::expected<Token, Diagnostic> {
            if (cursor >= tokens.size() || tokens[cursor].kind != Token::Kind::identifier)
                return reject(cursor < tokens.size() ? tokens[cursor].position : fragment.end_position,
                              "Expected an identifier.");
            return tokens[cursor++];
        };
        std::optional<std::size_t> assignment;
        std::size_t parentheses{};
        std::string_view previous;
        while (cursor < tokens.size()) {
            const auto token = tokens[cursor++];
            const auto origin = token.position;
            const bool member = previous == "." || previous == "::";
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
            if (!member && (token.text == "self" || token.text == "new" || token.text == "yield" ||
                            token.text == "async" || token.text.starts_with("__ui_")))
                return reject(
                    token.position,
                    "UI expressions use typed inputs/state; native context access and asynchronous effects are not permitted.");
            if (!member && (token.text == "input" || token.text == "state")) {
                if (cursor >= tokens.size() || tokens[cursor].text != ".")
                    return reject(token.position,
                                  "input, state and event are reserved UI identifiers; use a member after '.'.");
                ++cursor;
                ATELIER_TRY_BIND(name, expect_identifier());
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
                if (cursor < tokens.size() && tokens[cursor].text == "=") {
                    ++cursor;
                    if (!state || !event || assignment)
                        return reject(token.position,
                                      "Only event blocks may assign component state; inputs are read-only.");
                    assignment = parentheses;
                    append("self.write_" + suffix(kind) + "(" + std::to_string(*index) + ", ", origin);
                    previous = "=";
                } else {
                    append("self." + std::string(state ? "state_" : "read_") + suffix(kind) + "(" +
                               std::to_string(*index) + ") ",
                           origin);
                    previous = name->text;
                }
            } else if (!member && token.text == "node") {
                ATELIER_TRY(expect_dot());
                ATELIER_TRY_BIND(name, expect_identifier());
                const auto at =
                    std::ranges::find_if(result_.nodes, [&](const auto& item) { return item.name == name->text; });
                if (at == result_.nodes.end())
                    return reject(name->position, "Unknown declaration node.");
                if (at->kind == NodeKind::component || at->kind == NodeKind::repeat || at->kind == NodeKind::text_field)
                    return reject(name->position,
                                  "node references require a concrete node with a declaration-owned key.");
                append("self.node_key(" + std::to_string(at - result_.nodes.begin()) + ") ", origin);
                previous = name->text;
            } else if (!member && token.text == "event") {
                if (!event)
                    return reject(token.position, "event is reserved and only available inside an event handler.");
                if (!checked)
                    return reject(token.position, "Button and ChoiceButton activation do not expose event values.");
                ATELIER_TRY(expect_dot());
                ATELIER_TRY_BIND(name, expect_identifier());
                if (name->text != "checked")
                    return reject(name->position, "This event exposes checked: bool.");
                append("self.event_bool() ", origin);
                previous = name->text;
            } else {
                append(token.text, origin);
                result_.generated_source += ' ';
                previous = token.text;
            }
        }
        if (assignment)
            return reject(fragment.end_position, "State assignment requires a statement terminator.");
        return {};
    }
};
}

std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(std::string_view name,
                                                                           std::string_view source) {
    ATELIER_TRY_BIND(all, headers(source));
    ATELIER_TRY_BIND(helpers, prepare_helpers(source, all->front().begin.offset));
    std::vector<Declaration> result;
    result.reserve(all->size());
    for (const auto& header : *all) {
        ATELIER_TRY_BIND(declaration, Parser(name, header, *all, *helpers).run());
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
