#pragma once
#include "language/source.hpp"
#include "language/snapshot.hpp"

namespace atelier::language {
struct SourcePiece { std::size_t begin{}, end{}, offset{}; std::uint32_t file{}; };
struct MappedSource {
    std::string text;
    std::vector<SourcePiece> pieces;
    void append(const MappedSource&);
    void append(const SourceFile&, std::uint32_t file, std::size_t begin, std::size_t end);
};
struct ModuleDiagnostic { std::uint32_t file{}; Diagnostic diagnostic; };
struct ClassField { std::string name, type; MappedSource initializer; SourcePosition position; };
struct ClassMethod { std::string name; MappedSource source; SourcePosition position; std::string signature; };
struct FunctionDeclaration { std::string name, signature; };
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
    std::vector<ClassDeclaration> classes;
};
[[nodiscard]] std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot&);
}
