#pragma once
#include "language/source.hpp"
#include "language/snapshot.hpp"

#include <array>
#include <optional>

namespace atelier::language {
struct SourcePiece { std::size_t begin{}, end{}, offset{}; std::uint32_t file{}; };
struct MappedSource {
    std::string text;
    std::vector<SourcePiece> pieces;
    void append(const MappedSource&);
    void append(const SourceFile&, std::uint32_t file, std::size_t begin, std::size_t end);
};
struct ModuleDiagnostic { std::uint32_t file{}; Diagnostic diagnostic; };
// Syntax only; types, defaults and bounds resolve when the snapshot is prepared.
struct ClassField {
    std::string name, type; MappedSource initializer; SourcePosition position;
    // An initializer spelled exactly `name(arguments)` or `name::item` also keeps that shape:
    // math vector and enum fields require it, scalar fields fold the whole initializer.
    std::string initializer_name, initializer_item;
    std::vector<MappedSource> initializer_arguments;
    // Attributes. LockMin/LockMax already copy their UI bound to the hard side.
    std::string label, category, description;
    std::array<MappedSource, 4> bounds; // ui_min, ui_max, hard_min, hard_max
    std::optional<SourcePosition> range; // the first range attribute
};
// Signatures omit parameter names, so renaming a parameter stays compatible.
struct ClassMethod { std::string name; MappedSource source; SourcePosition position; std::string signature; std::vector<std::string> parameters; };
struct FunctionDeclaration { std::string name, signature; std::vector<std::string> parameters; std::uint32_t file{}; SourcePosition position; };
struct ConstantDeclaration { std::string name; std::uint32_t file{}; SourcePosition position; };
struct ClassDeclaration {
    std::string name, base;
    std::uint32_t file{};
    SourcePosition position;
    std::vector<ClassField> fields;
    std::vector<ClassMethod> methods;
};
// File collection and class structure use the ordinary lexer. Function bodies,
// signatures and constant expressions are checked by the one typed Source parser.
struct SourceModule {
    MappedSource common;
    std::vector<FunctionDeclaration> functions;
    std::vector<ConstantDeclaration> constants;
    std::vector<ClassDeclaration> classes;
};
// Files under engine/<module>/ declare only <module>.* names; other files declare none in a module present in the snapshot.
[[nodiscard]] std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot&);
}
