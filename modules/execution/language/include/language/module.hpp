#pragma once
#include "language/declarations.hpp"
#include "language/source.hpp"
#include "language/snapshot.hpp"

#include <array>
#include <optional>
#include <span>

namespace atelier::language {
struct SourcePiece {
    std::size_t begin{};

    std::size_t end{};

    std::size_t offset{};

    std::uint32_t file{};
};

struct MappedSource {
    std::string text;

    std::vector<SourcePiece> pieces;

    void append(const MappedSource&);

    void append(const SourceFile&, std::uint32_t file, std::size_t begin, std::size_t end);
};

struct ModuleDiagnostic {
    std::uint32_t file{};

    Diagnostic diagnostic;
};

// Syntax only; types, defaults and bounds resolve when the snapshot is prepared.
struct ClassField {
    std::string name;

    std::string type;

    MappedSource initializer;

    SourcePosition position;

    // An initializer spelled exactly `name(arguments)` or `name::item` also keeps that shape:
    // math vector and enum fields require it, scalar fields fold the whole initializer.
    std::string initializer_name;

    std::string initializer_item;

    std::vector<MappedSource> initializer_arguments;

    // Present only when the entire initializer is one decoded string literal.
    std::optional<std::string> initializer_text;

    // Attributes. LockMin/LockMax already copy their UI bound to the hard side.
    std::string label;

    std::string category;

    std::string description;

    std::array<MappedSource, 4> bounds; // ui_min, ui_max, hard_min, hard_max

    std::optional<SourcePosition> range; // the first range attribute

    std::vector<std::pair<std::string, std::string>> metadata;

    bool transient{};

    bool read_only{};
};

// Signatures omit parameter names, so renaming an ordinary parameter stays compatible.
struct ClassMethod {
    std::string name;

    MappedSource source;

    SourcePosition position;

    std::string signature;

    TypeName result_type;

    std::vector<MethodParameter> parameters;
};

struct FunctionDeclaration {
    std::string name;

    std::string signature;

    std::vector<std::string> parameters;
};

// A top-level struct, enum, constant or function. uses names every other top-level
// declaration its text spells, so a compile unit needs only the closure of its own uses.
struct TopLevel {
    enum class Kind { type, constant, function } kind;

    std::string name;

    std::string engine; // the engine module of its file, empty in project files

    std::uint32_t file{};

    SourcePosition position;

    MappedSource source;

    std::vector<std::uint32_t> uses;

    std::string property; // eligible accessor projection for the complete project type-name check
};

struct ClassDeclaration {
    std::string name;

    std::string base;

    std::string description;

    std::uint32_t file{};

    SourcePosition position;

    std::vector<ClassField> fields;

    std::vector<ClassMethod> methods;

    std::vector<std::uint32_t> uses; // top-level declarations its methods spell
};

// File collection and class structure use the ordinary lexer. Function bodies,
// signatures and constant expressions are checked by the one typed Source parser.
struct SourceModule {
    std::vector<std::string> engine_modules;

    std::vector<TopLevel> declarations; // snapshot order

    std::vector<TypeDeclaration> types; // sorted by name

    std::vector<FunctionDeclaration> functions; // project-file functions, the exported ones

    std::vector<ClassDeclaration> classes;

    // The declarations reachable from roots through uses, in snapshot order.
    [[nodiscard]] MappedSource closure(std::span<const std::uint32_t> roots) const;
};

// Files under engine/<module>/ declare only <module>.* names; other files declare none in a module present in the snapshot.
[[nodiscard]] std::expected<SourceModule, ModuleDiagnostic> collect_module(const SourceSnapshot&);
}
