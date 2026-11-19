#include "language/module.hpp"
#include "language/lexer.hpp"
#include "language/declarations.hpp"
#include <cassert>
#include <algorithm>
#include <unordered_map>

namespace atelier::language {
void MappedSource::append(const MappedSource& source) {
    const auto base = text.size();
    text += source.text;
    for (auto piece : source.pieces) {
        piece.begin += base;
        piece.end += base;
        pieces.push_back(piece);
    }
}

void MappedSource::append(const SourceFile& source, std::uint32_t file, std::size_t begin, std::size_t end) {
    const auto base = text.size();
    text.append(source.source, begin, end - begin);
    pieces.push_back({base, text.size(), begin, file});
    text += '\n';
}

namespace {
using namespace detail;

std::expected<void, Diagnostic> body(Tokens& tokens) {
    ATELIER_TRY(tokens.expect("{"));
    std::size_t depth = 1;
    while (depth) {
        if (tokens.token_.kind == Token::Kind::end)
            return reject(tokens.token_.position, "Expected '}' to close a function body.");
        if (tokens.token_.text == "{")
            ++depth;
        else if (tokens.token_.text == "}")
            --depth;
        ATELIER_TRY(tokens.consume());
    }
    return {};
}

// Consumes one expression up to a top-level ',' or `close`, which stay unconsumed.
std::expected<void, Diagnostic> expression_until(Tokens& tokens, std::string_view close, SourcePosition owner,
                                                 const char* message) {
    std::size_t depth = 0;
    while (depth || (tokens.token_.text != "," && tokens.token_.text != close)) {
        const auto text = tokens.token_.text;
        if (tokens.token_.kind == Token::Kind::end || text == ";" || text == "{" || text == "}")
            return reject(owner, message);
        if (text == "(" || text == "[")
            ++depth;
        else if (text == ")" || text == "]") {
            if (!depth)
                return reject(owner, message);
            --depth;
        }
        ATELIER_TRY(tokens.consume());
    }
    return {};
}

// [Min = e, Max = e, LockMin, LockMax, HardMin = e, HardMax = e, Label = "...", Category = "...", DESC = "..."]
std::expected<void, Diagnostic> field_attributes(Tokens& tokens, const SourceFile& unit, std::uint32_t file,
                                                 ClassField& field) {
    constexpr std::array<std::string_view, 4> bounds{"Min", "Max", "HardMin", "HardMax"};
    std::vector<Token> seen;
    const auto find = [&](std::string_view name) { return std::ranges::find(seen, name, &Token::text); };
    for (;;) {
        ATELIER_TRY_BIND(parsed_option, tokens.identifier());
        const auto option = *parsed_option;
        if (option.text != "Meta" && find(option.text) != seen.end())
            return reject(option.position, "Duplicate field attribute: " + std::string(option.text));
        seen.push_back(option);
        const auto bound = std::ranges::find(bounds, option.text);
        const bool lock = option.text == "LockMin" || option.text == "LockMax";
        if ((lock || bound != bounds.end()) && !field.range)
            field.range = option.position;
        if (option.text == "Meta") {
            ATELIER_TRY(tokens.expect("("));
            std::array<std::string, 2> values;
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (tokens.token_.kind != Token::Kind::string)
                    return reject(tokens.token_.position, "Meta requires a string literal key and value.");
                ATELIER_TRY_BIND(token, tokens.consume());
                ATELIER_TRY_BIND(value, string_value(*token));
                values[i] = std::move(*value);
                ATELIER_TRY(tokens.expect(i == 0 ? "," : ")"));
            }
            field.metadata.emplace_back(std::move(values[0]), std::move(values[1]));
        } else if (lock || option.text == "Transient" || option.text == "ReadOnly") {
            if (tokens.token_.text == "=")
                return reject(option.position, std::string(option.text) + " is a flag without a value.");
            if (option.text == "Transient")
                field.transient = true;
            else if (option.text == "ReadOnly")
                field.read_only = true;
        } else if (option.text == "Label" || option.text == "Category" || option.text == "DESC") {
            ATELIER_TRY(tokens.expect("="));
            if (tokens.token_.kind != Token::Kind::string)
                return reject(tokens.token_.position, std::string(option.text) + " requires a string literal.");
            ATELIER_TRY_BIND(token, tokens.consume());
            ATELIER_TRY_BIND(text, string_value(*token));
            if (option.text == "DESC") {
                if (!normalize_description_text(*text))
                    return reject(option.position,
                                  "DESC must not contain control characters other than line breaks and tabs.");
            } else if (std::ranges::any_of(*text, [](unsigned char c) { return c < 32 || c == 127; })) {
                return reject(option.position, std::string(option.text) + " must not contain control characters.");
            }
            (option.text == "Label"      ? field.label
             : option.text == "Category" ? field.category
                                         : field.description) = std::move(*text);
        } else if (bound != bounds.end()) {
            ATELIER_TRY(tokens.expect("="));
            const auto begin = tokens.token_.position;
            ATELIER_TRY(expression_until(tokens, "]", option.position, "Expected ']' to close field attributes."));
            if (tokens.token_.position.offset == begin.offset)
                return reject(option.position, std::string(option.text) + " requires a value.");
            field.bounds[static_cast<std::size_t>(bound - bounds.begin())].append(unit, file, begin.offset,
                                                                                  tokens.token_.position.offset);
        } else
            return reject(option.position, "Unknown field attribute: " + std::string(option.text));
        ATELIER_TRY_BIND(more, tokens.accept(","));
        if (!*more)
            break;
    }
    ATELIER_TRY(tokens.expect("]"));
    // A locked side uses its UI bound as the hard bound too (Houdini strict range).
    for (std::size_t side = 0; side < 2; ++side) {
        const auto lock = find(side ? "LockMax" : "LockMin");
        if (lock == seen.end())
            continue;
        if (find(bounds[side]) == seen.end() || find(bounds[side + 2]) != seen.end())
            return reject(lock->position, std::string(lock->text) + " requires " + std::string(bounds[side]) +
                                              " and excludes " + std::string(bounds[side + 2]) + ".");
        field.bounds[side + 2] = field.bounds[side];
    }
    return {};
}

// Records `name(arguments)` or `name::item` when that spelling is the whole initializer.
std::expected<void, Diagnostic> initializer_shape(Tokens& tokens, const SourceFile& unit, std::uint32_t file,
                                                  ClassField& field) {
    ATELIER_TRY_BIND(name, tokens.qualified_identifier());
    ATELIER_TRY_BIND(enumerator, tokens.accept("::"));
    if (*enumerator) {
        if (tokens.token_.kind != Token::Kind::identifier)
            return {};
        ATELIER_TRY_BIND(item, tokens.consume());
        if (tokens.token_.text == ";") {
            field.initializer_name = std::move(*name);
            field.initializer_item = std::string(item->text);
        }
        return {};
    }
    ATELIER_TRY_BIND(call, tokens.accept("("));
    if (!*call)
        return {};
    std::vector<MappedSource> arguments;
    ATELIER_TRY_BIND(empty, tokens.accept(")"));
    if (!*empty)
        for (;;) {
            const auto begin = tokens.token_.position;
            ATELIER_TRY(expression_until(tokens, ")", begin, "Expected ')' to close a field default."));
            arguments.emplace_back().append(unit, file, begin.offset, tokens.token_.position.offset);
            ATELIER_TRY_BIND(closed, tokens.accept(")"));
            if (*closed)
                break;
            ATELIER_TRY(tokens.expect(","));
        }
    if (tokens.token_.text == ";") {
        field.initializer_name = std::move(*name);
        field.initializer_arguments = std::move(arguments);
    }
    return {};
}

std::expected<std::string, Diagnostic> function_signature(Tokens& tokens, const TypeName& type,
                                                          std::vector<std::string>& parameters,
                                                          TopLevel* declaration = nullptr) {
    std::string signature = type.name + ":" + std::to_string(type.array_depth) + "(";
    bool writable_parameter = false;
    ATELIER_TRY(tokens.expect("("));
    ATELIER_TRY_BIND(empty, tokens.accept(")"));
    if (!*empty)
        for (;;) {
            ATELIER_TRY_BIND(constant, tokens.accept("const"));
            ATELIER_TRY_BIND(parameter, parse_type_name(tokens));
            ATELIER_TRY_BIND(reference, tokens.accept("&"));
            writable_parameter |= *reference && !*constant;
            ATELIER_TRY_BIND(name, tokens.identifier());
            parameters.emplace_back(name->text);
            signature += (*constant ? "const " : "") + parameter->name + ":" + std::to_string(parameter->array_depth) +
                         (*reference ? "&;" : ";");
            ATELIER_TRY_BIND(closed, tokens.accept(")"));
            if (*closed)
                break;
            ATELIER_TRY(tokens.expect(","));
        }
    if (declaration)
        declaration->property =
            property_name(declaration->name, parameters.size(), type.name == "void", writable_parameter);
    return signature;
}

// The <module> of a well-formed engine/<module>/<file> path; empty for every other path.
std::string_view engine_module(std::string_view path) {
    constexpr std::string_view prefix = "engine/";
    if (!path.starts_with(prefix))
        return {};
    const auto rest = path.substr(prefix.size());
    const auto name = rest.substr(0, rest.find('/'));
    if (name.empty() || name.size() == rest.size() || !letter(name.front()) || reserved(name) ||
        !std::ranges::all_of(name, [](char c) { return letter(c) || digit(c); }) || name == "core" || name == "math" ||
        name == "editor")
        return {};
    return name;
}

bool in_namespace(std::string_view name, std::string_view space) {
    return name.size() > space.size() && name.starts_with(space) && name[space.size()] == '.';
}

using Names = std::unordered_multimap<std::string, std::uint32_t>;

// Every top-level name the text spells as an identifier or dotted chain, at each length of the chain.
// Property names include both accessors. Conservative extra dependencies also keep
// same-name declarations in one unit so the parser can diagnose collisions.
void spelled(std::string_view text, const Names& names, std::vector<std::uint32_t>& uses) {
    Lexer lexer(text);
    std::string chain;
    bool dotted = false;
    for (;;) {
        const auto parsed = lexer.next();
        // These fragments were tokenized successfully by the collector below.
        assert(parsed);
        const auto token = *parsed;
        if (token.kind == Token::Kind::end)
            break;
        if (token.kind == Token::Kind::identifier) {
            if (dotted)
                chain += '.';
            else
                chain.clear();
            chain += token.text;
            dotted = false;
            const auto include = [&](const std::string& name) {
                const auto [begin, end] = names.equal_range(name);
                for (auto entry = begin; entry != end; ++entry)
                    uses.push_back(entry->second);
            };
            include(chain);
            if (const auto alias = property_name(chain); !alias.empty())
                include(alias);
        } else {
            dotted = token.text == "." && !chain.empty();
            if (!dotted)
                chain.clear();
        }
    }
    std::ranges::sort(uses);
    const auto repeated = std::ranges::unique(uses);
    uses.erase(repeated.begin(), repeated.end());
}
}

MappedSource SourceModule::closure(std::span<const std::uint32_t> roots) const {
    std::vector<bool> included(declarations.size());
    std::vector<std::uint32_t> pending(roots.begin(), roots.end());
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (included[index])
            continue;
        included[index] = true;
        pending.insert(pending.end(), declarations[index].uses.begin(), declarations[index].uses.end());
    }
    MappedSource text;
    for (std::size_t i = 0; i < declarations.size(); ++i)
        if (included[i])
            text.append(declarations[i].source);
    return text;
}

std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot& snapshot) {
    SourceModule result;
    if (snapshot.files.size() > 1024)
        return std::unexpected(ModuleDiagnostic{0, {{}, "A source snapshot supports at most 1024 files."}});
    std::vector<std::string_view> engine_modules;
    for (const auto& unit : snapshot.files)
        if (const auto name = engine_module(unit.path);
            !name.empty() && std::ranges::find(engine_modules, name) == engine_modules.end())
            engine_modules.push_back(name);
    for (std::uint32_t file = 0; file < snapshot.files.size(); ++file) {
        const auto& unit = snapshot.files[file];
        const auto collected = [&]() -> std::expected<void, Diagnostic> {
            if (unit.path.empty() || unit.path.front() == '/' || unit.path.back() == '/' ||
                unit.path.find_first_of("\\:\0", 0, 3) != std::string::npos)
                return reject({}, "Source paths must be project-relative UTF-8 paths using '/'.");
            for (std::size_t begin = 0; begin < unit.path.size();) {
                const auto end = unit.path.find('/', begin);
                const auto part = std::string_view(unit.path).substr(
                    begin, end == std::string::npos ? unit.path.size() - begin : end - begin);
                if (part.empty() || part == "." || part == "..")
                    return reject({}, "Source paths must be normalized project-relative paths.");
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            for (std::uint32_t previous = 0; previous < file; ++previous)
                if (snapshot.files[previous].path == unit.path)
                    return reject({}, "Duplicate source file path.");
            const auto own = engine_module(unit.path);
            if (own.empty() && unit.path.starts_with("engine/"))
                return reject(
                    {},
                    "Engine source paths must be engine/<module>/<file>, where <module> is an identifier other than core, math or editor.");
            const auto declare = [&](const std::string& name,
                                     SourcePosition position) -> std::expected<void, Diagnostic> {
                if (!own.empty() && !in_namespace(name, own))
                    return reject(position, "Engine sources under engine/" + std::string(own) +
                                                "/ may only declare names in the " + std::string(own) +
                                                ". namespace: " + name);
                if (own.empty())
                    for (const auto space : engine_modules)
                        if (in_namespace(name, space))
                            return reject(position, "Only engine sources under engine/" + std::string(space) +
                                                        "/ may declare names in the " + std::string(space) +
                                                        ". namespace: " + name);
                return {};
            };
            ATELIER_TRY_BIND(created, Tokens::create(unit.source));
            auto& tokens = *created;
            while (tokens.token_.kind != Token::Kind::end) {
                const auto begin = tokens.token_.position;
                if (tokens.token_.text == "[" || tokens.token_.text == "enum" || tokens.token_.text == "struct") {
                    ATELIER_TRY_BIND(parsed_type, parse_type_declaration(tokens));
                    ATELIER_TRY(declare(parsed_type->name, parsed_type->position));
                    TopLevel declaration{TopLevel::Kind::type,
                                         parsed_type->name,
                                         std::string(own),
                                         file,
                                         parsed_type->position,
                                         {},
                                         {},
                                         {}};
                    declaration.source.append(unit, file, begin.offset, tokens.token_.position.offset);
                    result.declarations.push_back(std::move(declaration));
                    continue;
                }
                ATELIER_TRY_BIND(object, tokens.accept("class"));
                if (!*object) {
                    ATELIER_TRY_BIND(constant, tokens.accept("const"));
                    if (*constant) {
                        TopLevel declaration{TopLevel::Kind::constant, {}, std::string(own), file, {}, {}, {}, {}};
                        ATELIER_TRY(parse_type_name(tokens));
                        declaration.position = tokens.token_.position;
                        ATELIER_TRY_BIND(name, tokens.qualified_identifier());
                        declaration.name = std::move(*name);
                        ATELIER_TRY(declare(declaration.name, declaration.position));
                        while (tokens.token_.text != ";") {
                            if (tokens.token_.kind == Token::Kind::end || tokens.token_.text == "{" ||
                                tokens.token_.text == "}")
                                return reject(tokens.token_.position, "Expected ';' to close a module constant.");
                            ATELIER_TRY(tokens.consume());
                        }
                        ATELIER_TRY(tokens.expect(";"));
                        declaration.source.append(unit, file, begin.offset, tokens.token_.position.offset);
                        result.declarations.push_back(std::move(declaration));
                        continue;
                    }
                    TopLevel declaration{TopLevel::Kind::function, {}, std::string(own), file, {}, {}, {}, {}};
                    ATELIER_TRY_BIND(async, tokens.accept("async"));
                    if (*async)
                        return reject(begin, "Project exports must be synchronous.");
                    ATELIER_TRY_BIND(void_result, tokens.accept("void"));
                    ATELIER_TRY_BIND(type, *void_result ? std::expected<TypeName, Diagnostic>{TypeName{"void"}}
                                                        : parse_type_name(tokens));
                    declaration.position = tokens.token_.position;
                    ATELIER_TRY_BIND(name, tokens.qualified_identifier());
                    declaration.name = std::move(*name);
                    ATELIER_TRY(declare(declaration.name, declaration.position));
                    FunctionDeclaration exported{declaration.name, {}, {}};
                    ATELIER_TRY_BIND(signature, function_signature(tokens, *type, exported.parameters, &declaration));
                    exported.signature = std::move(*signature);
                    // Engine functions serve Source calls only; project functions are the global entries.
                    if (own.empty())
                        result.functions.push_back(std::move(exported));
                    ATELIER_TRY(body(tokens));
                    declaration.source.append(unit, file, begin.offset, tokens.token_.position.offset);
                    result.declarations.push_back(std::move(declaration));
                    continue;
                }
                ClassDeclaration declaration;
                declaration.position = begin;
                declaration.file = file;
                const auto name_position = tokens.token_.position;
                ATELIER_TRY_BIND(class_name, tokens.qualified_identifier());
                declaration.name = std::move(*class_name);
                ATELIER_TRY(declare(declaration.name, name_position));
                if (declaration.name == "String" || declaration.name == "Array" ||
                    declaration.name.starts_with("core.") || declaration.name.starts_with("math.") ||
                    declaration.name.starts_with("__source_"))
                    return reject(begin, "Source classes cannot replace reserved types.");
                if (editor_reserved(declaration.name))
                    return reject(begin,
                                  "Source classes cannot use the editor. prefix; it is reserved for editor types.");
                if (std::ranges::any_of(result.classes, [&](const auto& c) { return c.name == declaration.name; }))
                    return reject(begin, "Duplicate source class name.");
                ATELIER_TRY(tokens.expect(":"));
                ATELIER_TRY_BIND(base, tokens.qualified_identifier());
                declaration.base = std::move(*base);
                ATELIER_TRY(tokens.expect("{"));
                for (;;) {
                    ATELIER_TRY_BIND(closed, tokens.accept("}"));
                    if (*closed)
                        break;
                    ClassField field;
                    const auto attributes = tokens.token_.position;
                    ATELIER_TRY_BIND(attributed, tokens.accept("["));
                    if (*attributed) {
                        ATELIER_TRY(field_attributes(tokens, unit, file, field));
                    }
                    const auto start = tokens.token_.position;
                    ATELIER_TRY_BIND(async, tokens.accept("async"));
                    if (*async)
                        return reject(start, "Source class methods must be synchronous.");
                    ATELIER_TRY_BIND(void_result, tokens.accept("void"));
                    ATELIER_TRY_BIND(type, *void_result ? std::expected<TypeName, Diagnostic>{TypeName{"void"}}
                                                        : parse_type_name(tokens));
                    ATELIER_TRY_BIND(name, tokens.identifier());
                    if (name->text.starts_with("__source_"))
                        return reject(name->position, "Reserved source member name.");
                    if (std::ranges::any_of(declaration.fields, [&](const auto& f) { return f.name == name->text; }) ||
                        std::ranges::any_of(declaration.methods, [&](const auto& m) { return m.name == name->text; }))
                        return reject(name->position, "Duplicate source class member.");
                    if (tokens.token_.text == "(") {
                        if (*attributed)
                            return reject(attributes, "Attributes apply only to class fields.");
                        ClassMethod method{std::string(name->text), {}, start, {}, {}};
                        ATELIER_TRY_BIND(signature, function_signature(tokens, *type, method.parameters));
                        method.signature = std::move(*signature);
                        ATELIER_TRY(body(tokens));
                        method.source.append(unit, file, start.offset, tokens.token_.position.offset);
                        declaration.methods.push_back(std::move(method));
                    } else {
                        if (type->array_depth || type->name == "void")
                            return reject(
                                start,
                                "Source object fields require bool, int, uint, float, double, a float or double vector, an enum, or String.");
                        field.name = name->text;
                        field.type = type->name;
                        field.position = start;
                        ATELIER_TRY_BIND(initializer, tokens.accept("="));
                        if (*initializer) {
                            const auto expression = tokens.token_.position;
                            if (tokens.token_.kind == Token::Kind::string) {
                                ATELIER_TRY_BIND(token, tokens.consume());
                                ATELIER_TRY_BIND(text, string_value(*token));
                                if (tokens.token_.text == ";")
                                    field.initializer_text = std::move(*text);
                            } else if (tokens.token_.kind == Token::Kind::identifier && !reserved(tokens.token_.text)) {
                                ATELIER_TRY(initializer_shape(tokens, unit, file, field));
                            }
                            while (tokens.token_.text != ";") {
                                if (tokens.token_.kind == Token::Kind::end || tokens.token_.text == "{" ||
                                    tokens.token_.text == "}")
                                    return reject(start, "Expected ';' after a field initializer.");
                                ATELIER_TRY(tokens.consume());
                            }
                            field.initializer.append(unit, file, expression.offset, tokens.token_.position.offset);
                        }
                        ATELIER_TRY(tokens.expect(";"));
                        declaration.fields.push_back(std::move(field));
                    }
                }
                ATELIER_TRY(tokens.accept(";"));
                if (result.classes.size() == 1024)
                    return reject(begin, "A source snapshot supports at most 1024 classes.");
                result.classes.push_back(std::move(declaration));
            }
            return {};
        }();
        if (!collected)
            return std::unexpected(ModuleDiagnostic{file, collected.error()});
    }
    Names names;
    for (std::uint32_t i = 0; i < result.declarations.size(); ++i) {
        const auto& declaration = result.declarations[i];
        names.emplace(declaration.name, i);
        if (declaration.kind == TopLevel::Kind::function)
            if (auto alias = property_name(declaration.name); !alias.empty())
                names.emplace(std::move(alias), i);
    }
    for (auto& declaration : result.declarations)
        spelled(declaration.source.text, names, declaration.uses);
    // A method spells its own name, so a top-level declaration sharing it joins the class unit,
    // where the parser rejects the clash as before.
    for (auto& cls : result.classes)
        for (const auto& method : cls.methods)
            spelled(method.source.text, names, cls.uses);
    std::ranges::sort(result.classes, {}, &ClassDeclaration::name);
    return result;
}
}
