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
bool scalar(std::string_view name) {
    return name == "bool" || name == "int" || name == "uint" || name == "float" || name == "double";
}
std::string function_signature(Tokens& tokens, const TypeName& type) {
    std::string signature = type.name + ":" + std::to_string(type.array_depth) + "(";
    tokens.expect("(");
    if (!tokens.accept(")")) for (;;) {
        const bool constant = tokens.accept("const");
        auto parameter = parse_type_name(tokens);
        const bool reference = tokens.accept("&");
        tokens.identifier();
        signature += (constant ? "const " : "") + parameter.name + ":" + std::to_string(parameter.array_depth) + (reference ? "&;" : ";");
        if (tokens.accept(")")) break;
        tokens.expect(",");
    }
    return signature;
}
}
std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot& snapshot) {
    SourceModule result;
    if (snapshot.files.size() > 1024)
        return std::unexpected(ModuleDiagnostic{0, {{}, "A source snapshot supports at most 1024 files."}});
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
            Tokens tokens(unit.source);
            while (tokens.token_.kind != Token::Kind::end) {
                const auto begin = tokens.token_.position;
                if (!tokens.accept("class")) {
                    // A top-level declaration ends at ';' or a complete body.
                    const bool function = tokens.token_.text != "struct" && tokens.token_.text != "enum" && tokens.token_.text != "const";
                    if (function) {
                        if (tokens.accept("async")) reject(begin, "Project exports must be synchronous.");
                        const auto type = tokens.accept("void") ? TypeName{"void"} : parse_type_name(tokens);
                        const auto name = tokens.identifier();
                        result.functions.push_back({std::string(name.text), function_signature(tokens, type)});
                    } else while (tokens.token_.text != ";" && tokens.token_.text != "{") {
                        if (tokens.token_.kind == Token::Kind::end) reject(begin, "Incomplete source declaration.");
                        tokens.consume();
                    }
                    if (tokens.token_.text == "{") body(tokens);
                    tokens.accept(";");
                    result.common.append(unit, file, begin.offset, tokens.token_.position.offset);
                    continue;
                }
                ClassDeclaration declaration;
                declaration.position = begin; declaration.file = file;
                declaration.name = tokens.qualified_identifier();
                if (declaration.name == "String" || declaration.name == "Array" || declaration.name.starts_with("core.") ||
                    declaration.name.starts_with("math.") || declaration.name.starts_with("__source_"))
                    reject(begin, "Source classes cannot replace reserved types.");
                if (editor_reserved(declaration.name))
                    reject(begin, "Source classes cannot use the editor. prefix; it is reserved for editor types.");
                if (std::ranges::any_of(result.classes, [&](const auto& c) { return c.name == declaration.name; }))
                    reject(begin, "Duplicate source class name.");
                tokens.expect(":"); declaration.base = tokens.qualified_identifier(); tokens.expect("{");
                while (!tokens.accept("}")) {
                    const auto start = tokens.token_.position;
                    if (tokens.accept("async")) reject(start, "Source class methods must be synchronous.");
                    const auto type = tokens.accept("void") ? TypeName{"void"} : parse_type_name(tokens);
                    const auto name = tokens.identifier();
                    if (name.text.starts_with("__source_")) reject(name.position, "Reserved source member name.");
                    if (std::ranges::any_of(declaration.fields, [&](const auto& f) { return f.name == name.text; }) ||
                        std::ranges::any_of(declaration.methods, [&](const auto& m) { return m.name == name.text; }))
                        reject(name.position, "Duplicate source class member.");
                    if (tokens.token_.text == "(") {
                        auto signature = function_signature(tokens, type);
                        body(tokens);
                        ClassMethod method{std::string(name.text), {}, start, std::move(signature)};
                        method.source.append(unit, file, start.offset, tokens.token_.position.offset);
                        declaration.methods.push_back(std::move(method));
                    } else {
                        if (type.array_depth || !scalar(type.name)) reject(start, "Source object fields currently require bool, int, uint, float or double.");
                        ClassField field{std::string(name.text), type.name, {}, start};
                        if (tokens.accept("=")) {
                            const auto expression = tokens.token_.position;
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
