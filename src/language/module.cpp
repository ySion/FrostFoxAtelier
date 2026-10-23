#include "language/module.hpp"
#include "language/lexer.hpp"
#include "language/declarations.hpp"
#include <algorithm>

namespace atelier::language {
void MappedSource::append(const MappedSource& source) {
    const auto base = text.size();
    text += source.text;
    for (auto piece : source.pieces) { piece.begin += base; piece.end += base; pieces.push_back(piece); }
}
void MappedSource::append(const SourceFile& source, std::uint32_t file, std::size_t begin, std::size_t end) {
    const auto base = text.size();
    text.append(source.source, begin, end - begin);
    pieces.push_back({base, text.size(), begin, file});
    text += '\n';
}
namespace {
using namespace detail;
void body(Tokens& tokens) {
    tokens.expect("{");
    std::size_t depth = 1;
    while (depth) {
        if (tokens.token_.kind == Token::Kind::end) reject(tokens.token_.position, "Expected '}' to close a function body.");
        if (tokens.accept("{")) ++depth;
        else if (tokens.accept("}")) --depth;
        else tokens.consume();
    }
}
// Consumes one expression up to a top-level ',' or `close`, which stay unconsumed.
void expression_until(Tokens& tokens, std::string_view close, SourcePosition owner, const char* message) {
    std::size_t depth = 0;
    while (depth || (tokens.token_.text != "," && tokens.token_.text != close)) {
        const auto text = tokens.token_.text;
        if (tokens.token_.kind == Token::Kind::end || text == ";" || text == "{" || text == "}") reject(owner, message);
        if (text == "(" || text == "[") ++depth;
        else if (text == ")" || text == "]") { if (!depth) reject(owner, message); --depth; }
        tokens.consume();
    }
}
// [Min = e, Max = e, LockMin, LockMax, HardMin = e, HardMax = e, Label = "...", Category = "...", DESC = "..."]
void field_attributes(Tokens& tokens, const SourceFile& unit, std::uint32_t file, ClassField& field) {
    constexpr std::array<std::string_view, 4> bounds{"Min", "Max", "HardMin", "HardMax"};
    std::vector<Token> seen;
    const auto find = [&](std::string_view name) { return std::ranges::find(seen, name, &Token::text); };
    do {
        const auto option = tokens.identifier();
        if (find(option.text) != seen.end()) reject(option.position, "Duplicate field attribute: " + std::string(option.text));
        seen.push_back(option);
        const auto bound = std::ranges::find(bounds, option.text);
        const bool lock = option.text == "LockMin" || option.text == "LockMax";
        if ((lock || bound != bounds.end()) && !field.range) field.range = option.position;
        if (lock) {
            if (tokens.token_.text == "=") reject(option.position, std::string(option.text) + " is a flag without a value.");
        } else if (option.text == "Label" || option.text == "Category" || option.text == "DESC") {
            tokens.expect("=");
            if (tokens.token_.kind != Token::Kind::string) reject(tokens.token_.position, std::string(option.text) + " requires a string literal.");
            auto text = string_value(tokens.consume());
            if (option.text != "DESC" && std::ranges::any_of(text, [](unsigned char c) { return c < 32; }))
                reject(option.position, std::string(option.text) + " must not contain control characters.");
            (option.text == "Label" ? field.label : option.text == "Category" ? field.category : field.description) = std::move(text);
        } else if (bound != bounds.end()) {
            tokens.expect("=");
            const auto begin = tokens.token_.position;
            expression_until(tokens, "]", option.position, "Expected ']' to close field attributes.");
            if (tokens.token_.position.offset == begin.offset) reject(option.position, std::string(option.text) + " requires a value.");
            field.bounds[static_cast<std::size_t>(bound - bounds.begin())].append(unit, file, begin.offset, tokens.token_.position.offset);
        } else reject(option.position, "Unknown field attribute: " + std::string(option.text));
    } while (tokens.accept(","));
    tokens.expect("]");
    // A locked side uses its UI bound as the hard bound too (Houdini strict range).
    for (std::size_t side = 0; side < 2; ++side) {
        const auto lock = find(side ? "LockMax" : "LockMin");
        if (lock == seen.end()) continue;
        if (find(bounds[side]) == seen.end() || find(bounds[side + 2]) != seen.end())
            reject(lock->position, std::string(lock->text) + " requires " + std::string(bounds[side]) + " and excludes " + std::string(bounds[side + 2]) + ".");
        field.bounds[side + 2] = field.bounds[side];
    }
}
// Records `name(arguments)` or `name::item` when that spelling is the whole initializer.
void initializer_shape(Tokens& tokens, const SourceFile& unit, std::uint32_t file, ClassField& field) {
    auto name = tokens.qualified_identifier();
    if (tokens.accept("::")) {
        if (tokens.token_.kind != Token::Kind::identifier) return;
        auto item = std::string(tokens.consume().text);
        if (tokens.token_.text == ";") { field.initializer_name = std::move(name); field.initializer_item = std::move(item); }
        return;
    }
    if (!tokens.accept("(")) return;
    std::vector<MappedSource> arguments;
    if (!tokens.accept(")")) for (;;) {
        const auto begin = tokens.token_.position;
        expression_until(tokens, ")", begin, "Expected ')' to close a field default.");
        arguments.emplace_back().append(unit, file, begin.offset, tokens.token_.position.offset);
        if (tokens.accept(")")) break;
        tokens.expect(",");
    }
    if (tokens.token_.text == ";") { field.initializer_name = std::move(name); field.initializer_arguments = std::move(arguments); }
}
std::string function_signature(Tokens& tokens, const TypeName& type, std::vector<std::string>& parameters) {
    std::string signature = type.name + ":" + std::to_string(type.array_depth) + "(";
    tokens.expect("(");
    if (!tokens.accept(")")) for (;;) {
        const bool constant = tokens.accept("const");
        auto parameter = parse_type_name(tokens);
        const bool reference = tokens.accept("&");
        parameters.emplace_back(tokens.identifier().text);
        signature += (constant ? "const " : "") + parameter.name + ":" + std::to_string(parameter.array_depth) + (reference ? "&;" : ";");
        if (tokens.accept(")")) break;
        tokens.expect(",");
    }
    return signature;
}
// The <module> of a well-formed engine/<module>/<file> path; empty for every other path.
std::string_view engine_module(std::string_view path) {
    constexpr std::string_view prefix = "engine/";
    if (!path.starts_with(prefix)) return {};
    const auto rest = path.substr(prefix.size());
    const auto name = rest.substr(0, rest.find('/'));
    if (name.empty() || name.size() == rest.size() || !letter(name.front()) || reserved(name) ||
        !std::ranges::all_of(name, [](char c) { return letter(c) || digit(c); }) ||
        name == "core" || name == "math" || name == "editor") return {};
    return name;
}
bool in_namespace(std::string_view name, std::string_view space) {
    return name.size() > space.size() && name.starts_with(space) && name[space.size()] == '.';
}
}
std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot& snapshot) {
    SourceModule result;
    if (snapshot.files.size() > 1024)
        return std::unexpected(ModuleDiagnostic{0, {{}, "A source snapshot supports at most 1024 files."}});
    std::vector<std::string_view> engine_modules;
    for (const auto& unit : snapshot.files)
        if (const auto name = engine_module(unit.path); !name.empty() && std::ranges::find(engine_modules, name) == engine_modules.end())
            engine_modules.push_back(name);
    for (std::uint32_t file = 0; file < snapshot.files.size(); ++file) {
        const auto& unit = snapshot.files[file];
        try {
            if (unit.path.empty() || unit.path.front() == '/' || unit.path.back() == '/' || unit.path.find_first_of("\\:\0", 0, 3) != std::string::npos)
                reject({}, "Source paths must be project-relative UTF-8 paths using '/'.");
            for (std::size_t begin = 0; begin < unit.path.size();) {
                const auto end = unit.path.find('/', begin);
                const auto part = std::string_view(unit.path).substr(begin, end == std::string::npos ? unit.path.size() - begin : end - begin);
                if (part.empty() || part == "." || part == "..") reject({}, "Source paths must be normalized project-relative paths.");
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            for (std::uint32_t previous = 0; previous < file; ++previous)
                if (snapshot.files[previous].path == unit.path) reject({}, "Duplicate source file path.");
            const auto own = engine_module(unit.path);
            if (own.empty() && unit.path.starts_with("engine/"))
                reject({}, "Engine source paths must be engine/<module>/<file>, where <module> is an identifier other than core, math or editor.");
            const auto declare = [&](const std::string& name, SourcePosition position) {
                if (!own.empty() && !in_namespace(name, own))
                    reject(position, "Engine sources under engine/" + std::string(own) + "/ may only declare names in the " +
                        std::string(own) + ". namespace: " + name);
                if (own.empty())
                    for (const auto space : engine_modules)
                        if (in_namespace(name, space))
                            reject(position, "Only engine sources under engine/" + std::string(space) + "/ may declare names in the " +
                                std::string(space) + ". namespace: " + name);
            };
            Tokens tokens(unit.source);
            while (tokens.token_.kind != Token::Kind::end) {
                const auto begin = tokens.token_.position;
                if (!tokens.accept("class")) {
                    // A top-level declaration ends at ';' or a complete body.
                    const bool function = tokens.token_.text != "struct" && tokens.token_.text != "enum" && tokens.token_.text != "const";
                    if (function) {
                        if (tokens.accept("async")) reject(begin, "Project exports must be synchronous.");
                        const auto type = tokens.accept("void") ? TypeName{"void"} : parse_type_name(tokens);
                        const auto position = tokens.token_.position;
                        FunctionDeclaration declaration{tokens.qualified_identifier(), {}, {}, file, position};
                        declare(declaration.name, position);
                        declaration.signature = function_signature(tokens, type, declaration.parameters);
                        result.functions.push_back(std::move(declaration));
                    } else {
                        const bool constant = tokens.accept("const");
                        if (constant) parse_type_name(tokens);
                        else tokens.consume();
                        const auto position = tokens.token_.position;
                        auto name = tokens.qualified_identifier();
                        declare(name, position);
                        if (constant) result.constants.push_back({std::move(name), file, position});
                        while (tokens.token_.text != ";" && tokens.token_.text != "{") {
                            if (tokens.token_.kind == Token::Kind::end) reject(begin, "Incomplete source declaration.");
                            tokens.consume();
                        }
                    }
                    if (tokens.token_.text == "{") body(tokens);
                    tokens.accept(";");
                    result.common.append(unit, file, begin.offset, tokens.token_.position.offset);
                    continue;
                }
                ClassDeclaration declaration;
                declaration.position = begin; declaration.file = file;
                const auto name_position = tokens.token_.position;
                declaration.name = tokens.qualified_identifier();
                declare(declaration.name, name_position);
                if (declaration.name == "String" || declaration.name == "Array" || declaration.name.starts_with("core.") ||
                    declaration.name.starts_with("math.") || declaration.name.starts_with("__source_"))
                    reject(begin, "Source classes cannot replace reserved types.");
                if (editor_reserved(declaration.name))
                    reject(begin, "Source classes cannot use the editor. prefix; it is reserved for editor types.");
                if (std::ranges::any_of(result.classes, [&](const auto& c) { return c.name == declaration.name; }))
                    reject(begin, "Duplicate source class name.");
                tokens.expect(":"); declaration.base = tokens.qualified_identifier(); tokens.expect("{");
                while (!tokens.accept("}")) {
                    ClassField field;
                    const auto attributes = tokens.token_.position;
                    const bool attributed = tokens.accept("[");
                    if (attributed) field_attributes(tokens, unit, file, field);
                    const auto start = tokens.token_.position;
                    if (tokens.accept("async")) reject(start, "Source class methods must be synchronous.");
                    const auto type = tokens.accept("void") ? TypeName{"void"} : parse_type_name(tokens);
                    const auto name = tokens.identifier();
                    if (name.text.starts_with("__source_")) reject(name.position, "Reserved source member name.");
                    if (std::ranges::any_of(declaration.fields, [&](const auto& f) { return f.name == name.text; }) ||
                        std::ranges::any_of(declaration.methods, [&](const auto& m) { return m.name == name.text; }))
                        reject(name.position, "Duplicate source class member.");
                    if (tokens.token_.text == "(") {
                        if (attributed) reject(attributes, "Attributes apply only to class fields.");
                        ClassMethod method{std::string(name.text), {}, start, {}, {}};
                        method.signature = function_signature(tokens, type, method.parameters);
                        body(tokens);
                        method.source.append(unit, file, start.offset, tokens.token_.position.offset);
                        declaration.methods.push_back(std::move(method));
                    } else {
                        if (type.array_depth || type.name == "void")
                            reject(start, "Source object fields require bool, int, uint, float, double, a float or double vector, or an enum.");
                        field.name = name.text; field.type = type.name; field.position = start;
                        if (tokens.accept("=")) {
                            const auto expression = tokens.token_.position;
                            if (tokens.token_.kind == Token::Kind::identifier && !reserved(tokens.token_.text))
                                initializer_shape(tokens, unit, file, field);
                            while (tokens.token_.text != ";") {
                                if (tokens.token_.kind == Token::Kind::end || tokens.token_.text == "{" || tokens.token_.text == "}")
                                    reject(start, "Expected ';' after a field initializer.");
                                tokens.consume();
                            }
                            field.initializer.append(unit, file, expression.offset, tokens.token_.position.offset);
                        }
                        tokens.expect(";"); declaration.fields.push_back(std::move(field));
                    }
                }
                tokens.accept(";");
                if (result.classes.size() == 1024) reject(begin, "A source snapshot supports at most 1024 classes.");
                result.classes.push_back(std::move(declaration));
            }
        } catch (const Rejected& error) { return std::unexpected(ModuleDiagnostic{file, error.diagnostic}); }
    }
    std::ranges::sort(result.classes, {}, &ClassDeclaration::name);
    return result;
}
}
