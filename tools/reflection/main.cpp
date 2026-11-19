#include "memory/fatal.hpp"
#include <llvm/Support/ErrorHandling.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclFriend.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/LiteralSupport.h>
#include <clang/Lex/MacroArgs.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace clang;

std::string key(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    llvm::SmallString<256> normalized(path);
    llvm::sys::path::remove_dots(normalized, true);
    return llvm::StringRef(normalized).lower();
}

std::string quote(llvm::StringRef text) {
    std::string result;
    llvm::raw_string_ostream stream(result);
    stream << llvm::json::Value(text);
    return result;
}

// Metadata owns decoded bytes, including embedded zeroes. Fixed-width octal
// escapes avoid C++ literal encoding and escape-boundary ambiguities.
std::string metadata_text(std::string_view text) {
    std::string result = "std::string{\"";
    for (const unsigned char c : text) {
        result += '\\';
        result += static_cast<char>('0' + (c >> 6));
        result += static_cast<char>('0' + ((c >> 3) & 7));
        result += static_cast<char>('0' + (c & 7));
    }
    return result + "\", " + std::to_string(text.size()) + "}";
}

llvm::Expected<std::string> required(const llvm::json::Object& object, llvm::StringRef name) {
    const auto value = object.getString(name);
    if (!value || value->empty())
        return llvm::createStringError("Missing configuration string: " + name.str());
    return value->str();
}

struct Header {
    std::string path;

    std::string include;

    std::string output;

    bool included{};
};

struct Config {
    std::string source;

    std::string database;

    std::string cpp;

    std::string depfile;

    std::string resource_dir;

    std::vector<Header> headers;
};

llvm::Expected<Config> read_config(const char* path) {
    auto file = llvm::MemoryBuffer::getFile(path);
    if (!file)
        return llvm::errorCodeToError(file.getError());
    auto json = llvm::json::parse((*file)->getBuffer());
    if (!json)
        return json.takeError();
    const auto* object = json->getAsObject();
    if (!object)
        return llvm::createStringError("Reflection configuration must be an object.");
    Config result;
    for (const auto& [name, destination] : {std::pair{"source", &result.source},
                                            {"database", &result.database},
                                            {"cpp", &result.cpp},
                                            {"depfile", &result.depfile},
                                            {"resource_dir", &result.resource_dir}}) {
        auto value = required(*object, name);
        if (!value)
            return value.takeError();
        *destination = std::move(*value);
    }
    const auto* headers = object->getArray("headers");
    if (!headers || headers->empty())
        return llvm::createStringError("No reflection headers configured.");
    std::set<std::string> paths;
    std::set<std::string> outputs;
    for (const auto& item : *headers) {
        const auto* header = item.getAsObject();
        if (!header)
            return llvm::createStringError("Invalid reflection header configuration.");
        Header value;
        for (const auto& [name, destination] :
             {std::pair{"path", &value.path}, {"include", &value.include}, {"output", &value.output}}) {
            auto text = required(*header, name);
            if (!text)
                return text.takeError();
            *destination = std::move(*text);
        }
        if (!paths.insert(key(value.path)).second || !outputs.insert(key(value.output)).second)
            return llvm::createStringError("Duplicate reflection header or output.");
        result.headers.push_back(std::move(value));
    }
    return result;
}

enum class MarkerKind { object, record, enumeration, enumerator, field, method, parameter, body };

struct Marker {
    MarkerKind kind;

    SourceLocation location;

    std::size_t header;

    unsigned next_offset;

    std::vector<Token> arguments;

    bool used{};
};

using Options = std::map<std::string, std::string>;

struct Field {
    std::string cpp_name;

    std::string name;

    std::string form;

    std::string target;

    bool hidden{};

    unsigned line{};

    std::optional<std::uint64_t> byte_offset{};

    std::string label{};

    std::string category{};

    std::string description{};

    // Designated FieldRange initializers, empty when the field declares no range.
    std::string range{};

    std::vector<std::pair<std::string, std::string>> metadata{};

    bool transient{};

    bool read_only{};
};

struct Method {
    std::string cpp_name;

    std::string name;

    std::string pointer;

    unsigned line{};

    bool preserves_borrows{};

    std::string description{};

    std::vector<std::string> parameters{};

    std::vector<std::optional<std::string>> defaults{};

    std::vector<std::vector<std::pair<std::string, std::string>>> parameter_metadata{};
};

struct Enumerator {
    std::string name;

    std::string label{};

    std::string description{};
};

struct Type {
    const CXXRecordDecl* declaration{};

    std::size_t header{};

    unsigned line{};

    bool object{};

    bool class_tag{};

    std::string qualified;

    std::string scope;

    std::string name;

    std::string runtime_name;

    std::string base;

    std::vector<Field> fields;

    std::vector<Method> methods;

    const EnumDecl* enumeration{};

    std::string underlying{};

    bool enum_flags{};

    std::vector<Enumerator> enumerators{};

    std::string description{};

    std::optional<std::uint64_t> base_offset{};

    std::string validator_option{};

    SourceLocation validator_location{};

    std::string validator{};

    std::vector<std::pair<std::string, std::string>> metadata{};
};

struct State {
    Config& config;

    CompilerInstance* compiler{};

    std::vector<Marker> markers;

    std::vector<Type> types;

    std::set<std::string> dependencies;

    std::set<const CXXRecordDecl*> external_objects;

    std::set<const CXXRecordDecl*> external_records;

    std::set<const EnumDecl*> external_enums;

    bool failed{};

    explicit State(Config& configuration) : config(configuration) {}

    void error(SourceLocation location, const std::string& message) {
        failed = true;
        auto& diagnostics = compiler->getDiagnostics();
        diagnostics.Report(location, diagnostics.getCustomDiagID(DiagnosticsEngine::Error, "%0")) << message;
    }

    std::optional<std::size_t> header_of(SourceLocation location) const {
        auto& sources = compiler->getSourceManager();
        if (location.isInvalid())
            return {};
        const auto path = key(sources.getFilename(sources.getExpansionLoc(location)).str());
        for (std::size_t i = 0; i < config.headers.size(); ++i)
            if (path == key(config.headers[i].path))
                return i;
        return {};
    }

    Marker* prefix(const Decl& declaration, MarkerKind kind) {
        const auto location = declaration.getBeginLoc();
        const auto header = header_of(location);
        if (!header)
            return nullptr;
        auto& sources = compiler->getSourceManager();
        const auto offset = sources.getFileOffset(sources.getExpansionLoc(location));
        for (auto& marker : markers)
            if (marker.header == *header && marker.next_offset == offset && marker.kind == kind) {
                if (marker.used) {
                    error(location,
                          "Each reflection marker must describe one declaration; split grouped declarations.");
                    return nullptr;
                }
                marker.used = true;
                return &marker;
            }
        return nullptr;
    }

    std::optional<Options> options(const Marker& marker, std::initializer_list<llvm::StringRef> allowed,
                                   std::vector<std::pair<std::string, std::string>>* metadata = nullptr) {
        Options result;
        const auto& tokens = marker.arguments;
        std::size_t i = 0;
        while (i < tokens.size()) {
            if (!tokens[i].is(tok::identifier)) {
                error(tokens[i].getLocation(), "Expected a reflection option name.");
                return {};
            }
            const auto name = tokens[i++].getIdentifierInfo()->getName().str();
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() || result.contains(name)) {
                error(marker.location, "Unknown or duplicate reflection option: " + name);
                return {};
            }
            std::string value;
            if (name == "Meta") {
                if (i == tokens.size() || !tokens[i++].is(tok::l_paren)) {
                    error(marker.location, "Meta requires (\"key\", \"value\").");
                    return {};
                }
                std::array<std::string, 2> values;
                for (std::size_t part = 0; part < values.size(); ++part) {
                    if (i == tokens.size() || !tokens[i].is(tok::string_literal)) {
                        error(marker.location, "Meta requires an ordinary string literal key and value.");
                        return {};
                    }
                    StringLiteralParser literal(llvm::ArrayRef<Token>(&tokens[i++], 1), compiler->getPreprocessor());
                    if (literal.hadError) {
                        failed = true;
                        return {};
                    }
                    values[part] = literal.GetString().str();
                    if (i == tokens.size() || !tokens[i++].is(part == 0 ? tok::comma : tok::r_paren)) {
                        error(marker.location, "Meta requires exactly a key and value.");
                        return {};
                    }
                }
                metadata->emplace_back(std::move(values[0]), std::move(values[1]));
            } else if ((name == "In" || name == "Out" || name == "InOut" || name == "Group") && i < tokens.size() &&
                       tokens[i].is(tok::l_paren)) {
                ++i;
                if (i == tokens.size() || tokens[i].is(tok::r_paren)) {
                    error(marker.location, name + "(...) requires at least one argument.");
                    return {};
                }
                bool first = true;
                while (i < tokens.size()) {
                    if (first && tokens[i].is(tok::string_literal)) {
                        StringLiteralParser literal(llvm::ArrayRef<Token>(&tokens[i++], 1),
                                                    compiler->getPreprocessor());
                        if (literal.hadError) {
                            failed = true;
                            return {};
                        }
                        value = literal.GetString().str();
                        if (name != "Group" && value.empty()) {
                            error(marker.location, "Reflection option " + name + " requires a nonempty string.");
                            return {};
                        }
                        if (std::any_of(value.begin(), value.end(),
                                        [](unsigned char c) { return c < 32 || c == 127; })) {
                            error(marker.location, "Reflection option strings must contain no control characters.");
                            return {};
                        }
                        result.emplace("__explicit_role_name", "1");
                    } else if (tokens[i].is(tok::identifier)) {
                        const auto sub = tokens[i++].getIdentifierInfo()->getName().str();
                        if (std::find(allowed.begin(), allowed.end(), sub) == allowed.end() || result.contains(sub)) {
                            error(marker.location, "Unknown or duplicate reflection option: " + sub);
                            return {};
                        }
                        if (sub == "Optional") {
                            result.emplace(sub, "");
                        } else if ((sub == "From" || sub == "Layer") && i < tokens.size() && tokens[i].is(tok::equal)) {
                            ++i;
                            if (i == tokens.size() || !tokens[i].is(tok::string_literal)) {
                                error(marker.location,
                                      "Reflection option " + sub + " requires an ordinary string literal.");
                                return {};
                            }
                            StringLiteralParser literal(llvm::ArrayRef<Token>(&tokens[i++], 1),
                                                        compiler->getPreprocessor());
                            if (literal.hadError) {
                                failed = true;
                                return {};
                            }
                            auto sub_val = literal.GetString().str();
                            if (sub_val.empty() || std::any_of(sub_val.begin(), sub_val.end(),
                                                               [](unsigned char c) { return c < 32 || c == 127; })) {
                                error(marker.location, "Reflection option " + sub +
                                                           " requires a nonempty string without control characters.");
                                return {};
                            }
                            result.emplace(sub, std::move(sub_val));
                        } else {
                            error(marker.location, "Invalid option inside " + name + "(...): " + sub);
                            return {};
                        }
                    } else {
                        error(tokens[i].getLocation(), "Expected a string literal or option inside " + name + "(...).");
                        return {};
                    }
                    first = false;
                    if (i < tokens.size() && tokens[i].is(tok::r_paren)) {
                        ++i;
                        break;
                    }
                    if (i == tokens.size() || !tokens[i++].is(tok::comma) || i == tokens.size() ||
                        tokens[i].is(tok::r_paren)) {
                        error(marker.location, "Expected ',' or ')' inside " + name + "(...).");
                        return {};
                    }
                }
            } else if (i < tokens.size() && tokens[i].is(tok::equal)) {
                ++i;
                if (i == tokens.size() || !tokens[i].is(tok::string_literal)) {
                    error(marker.location, "Reflection option " + name + " requires an ordinary string literal.");
                    return {};
                }
                StringLiteralParser literal(llvm::ArrayRef<Token>(&tokens[i++], 1), compiler->getPreprocessor());
                if (literal.hadError) {
                    failed = true;
                    return {};
                }
                value = literal.GetString().str();
                const bool presentation = name == "Label" || name == "Category";
                const bool description = name == "DESC";
                const bool allow_empty = presentation || description || name == "Group";
                if (!allow_empty && value.empty()) {
                    error(marker.location, "Reflection option " + name + " requires a nonempty string.");
                    return {};
                }
                if (description) {
                    bool valid = llvm::json::isUTF8(value);
                    for (std::size_t at = 0; valid && at < value.size(); ++at) {
                        const auto c = static_cast<unsigned char>(value[at]);
                        valid = !((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127 ||
                                  (c == 0xc2 && at + 1 < value.size() &&
                                   static_cast<unsigned char>(value[at + 1]) >= 0x80 &&
                                   static_cast<unsigned char>(value[at + 1]) <= 0x9f));
                    }
                    if (!valid) {
                        error(marker.location,
                              "DESC requires valid UTF-8 without controls other than line breaks and tabs.");
                        return {};
                    }
                } else if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32; })) {
                    error(marker.location, "Reflection option strings must contain no control characters.");
                    return {};
                }
            } else if (name != "Hidden" && name != "Transient" && name != "ReadOnly" && name != "RunOver" &&
                       name != "Flags" && name != "BorrowSafe" && name != "LockMin" && name != "LockMax" &&
                       name != "Port" && name != "In" && name != "Out" && name != "InOut" && name != "Group" &&
                       name != "Optional") {
                error(marker.location, "Reflection option " + name + " requires a value.");
                return {};
            }
            if ((name == "Hidden" || name == "Transient" || name == "ReadOnly" || name == "RunOver" ||
                 name == "Flags" || name == "BorrowSafe" || name == "LockMin" || name == "LockMax" || name == "Port" ||
                 name == "Optional") &&
                !value.empty()) {
                error(marker.location, name + " is a flag, without a value.");
                return {};
            }
            if (name != "Meta")
                result.emplace(name, std::move(value));
            if (i < tokens.size()) {
                if (!tokens[i++].is(tok::comma) || i == tokens.size()) {
                    error(marker.location, "Expected another comma-separated reflection option.");
                    return {};
                }
            }
        }
        return result;
    }
};

// Pair a marker only with the immediately following declaration token. Comments
// and whitespace may intervene; directives or another declaration cannot.
unsigned after_trivia(llvm::StringRef buffer, unsigned offset) {
    std::size_t i = offset;
    while (i < buffer.size()) {
        if (std::isspace(static_cast<unsigned char>(buffer[i]))) {
            ++i;
            continue;
        }
        if (buffer.substr(i).starts_with("//")) {
            const auto end = buffer.find('\n', i);
            i = end == llvm::StringRef::npos ? buffer.size() : end + 1;
            continue;
        }
        if (buffer.substr(i).starts_with("/*")) {
            const auto end = buffer.find("*/", i + 2);
            i = end == llvm::StringRef::npos ? buffer.size() : end + 2;
            continue;
        }
        break;
    }
    return static_cast<unsigned>(i);
}

class Markers final : public PPCallbacks {
    State& state_;

public:
    explicit Markers(State& state) : state_(state) {}

    void MacroExpands(const Token& name, const MacroDefinition&, SourceRange range, const MacroArgs* args) override {
        const auto header = state_.header_of(range.getBegin());
        if (!header)
            return;
        const auto spelling = name.getIdentifierInfo()->getName();
        std::optional<MarkerKind> kind;
        if (spelling == "ACLASS")
            kind = MarkerKind::object;
        else if (spelling == "ASTRUCT")
            kind = MarkerKind::record;
        else if (spelling == "AENUM")
            kind = MarkerKind::enumeration;
        else if (spelling == "AMETA")
            kind = MarkerKind::enumerator;
        else if (spelling == "AFIELD")
            kind = MarkerKind::field;
        else if (spelling == "AFUNCTION")
            kind = MarkerKind::method;
        else if (spelling == "APARAM")
            kind = MarkerKind::parameter;
        else if (spelling == "AGENERATED_BODY")
            kind = MarkerKind::body;
        if (!kind)
            return;
        if (range.getBegin().isMacroID()) {
            state_.error(range.getBegin(), "Reflection markers must be written directly at the declaration.");
            return;
        }
        auto& sources = state_.compiler->getSourceManager();
        const auto end = Lexer::getLocForEndOfToken(range.getEnd(), 0, sources, state_.compiler->getLangOpts());
        const auto buffer = sources.getBufferData(sources.getFileID(range.getBegin()));
        Marker marker{*kind, range.getBegin(), *header, after_trivia(buffer, sources.getFileOffset(end)), {}, false};
        if (args) {
            const auto* tokens = args->getUnexpArgument(0);
            marker.arguments.assign(tokens, tokens + MacroArgs::getArgLength(tokens));
        }
        state_.markers.push_back(std::move(marker));
    }

    void FileChanged(SourceLocation location, FileChangeReason reason, SrcMgr::CharacteristicKind, FileID) override {
        if (reason != EnterFile || location.isInvalid())
            return;
        auto& sources = state_.compiler->getSourceManager();
        const auto file = sources.getFileEntryRefForID(sources.getFileID(location));
        if (!file)
            return;
        const auto path = file->getName().str();
        for (const auto& header : state_.config.headers)
            if (key(path) == key(header.output))
                return;
        state_.dependencies.insert(path);
    }

    void InclusionDirective(SourceLocation location, const Token&, llvm::StringRef, bool, CharSourceRange,
                            OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef, const Module*, bool,
                            SrcMgr::CharacteristicKind) override {
        const auto header = state_.header_of(location);
        if (header && file && key(file->getName().str()) == key(state_.config.headers[*header].output))
            state_.config.headers[*header].included = true;
    }
};

class Collector final : public RecursiveASTVisitor<Collector> {
    State& state_;

public:
    explicit Collector(State& state) : state_(state) {}

    bool TraverseDecl(Decl* declaration) {
        if (declaration && !isa<TranslationUnitDecl>(declaration) && !state_.header_of(declaration->getLocation()))
            return true;
        return RecursiveASTVisitor::TraverseDecl(declaration);
    }

    bool TraverseStmt(Stmt*) { return true; }

    bool VisitEnumDecl(EnumDecl* declaration) {
        auto* marker = state_.prefix(*declaration, MarkerKind::enumeration);
        if (!marker)
            return true;
        const auto options = state_.options(*marker, {"Name", "DESC", "Flags"});
        if (!options)
            return true;
        const auto* definition = declaration->getDefinition();
        if (!definition || !definition->isScoped() || definition->getName().empty() ||
            (!isa<NamespaceDecl>(definition->getDeclContext()) && !definition->getDeclContext()->isTranslationUnit())) {
            state_.error(marker->location, "AENUM requires a complete namespace-level enum class.");
            return true;
        }
        for (auto* context = definition->getDeclContext(); !context->isTranslationUnit();
             context = context->getParent())
            if (const auto* scope = dyn_cast<NamespaceDecl>(context); scope && scope->isAnonymousNamespace()) {
                state_.error(marker->location, "Reflected enums cannot have anonymous namespace linkage.");
                return true;
            }
        const auto underlying = definition->getIntegerType().getCanonicalType();
        if (state_.compiler->getASTContext().getTypeSize(underlying) != 32 ||
            (!underlying->isSpecificBuiltinType(BuiltinType::Int) &&
             !underlying->isSpecificBuiltinType(BuiltinType::UInt))) {
            state_.error(marker->location, "AENUM requires an int32_t or uint32_t underlying type.");
            return true;
        }
        const bool enum_flags = options->contains("Flags");
        if (enum_flags && underlying->isSignedIntegerType()) {
            state_.error(marker->location, "AENUM(Flags) requires a uint32_t underlying type.");
            return true;
        }
        const auto qualified = definition->getQualifiedNameAsString();
        std::string runtime_name = qualified;
        for (auto at = runtime_name.find("::"); at != std::string::npos; at = runtime_name.find("::"))
            runtime_name.replace(at, 2, ".");
        if (options->contains("Name"))
            runtime_name = options->at("Name");
        const auto description = options->contains("DESC") ? options->at("DESC") : std::string{};
        for (const auto& type : state_.types) {
            if (type.enumeration == definition) {
                if (type.runtime_name != runtime_name || type.description != description ||
                    type.enum_flags != enum_flags)
                    state_.error(marker->location, "Conflicting reflection markers on enum redeclarations.");
                return true;
            }
            if (type.runtime_name == runtime_name) {
                state_.error(marker->location, "Duplicate reflected type name: " + runtime_name);
                return true;
            }
        }
        const auto owner = state_.header_of(definition->getLocation());
        if (!owner) {
            state_.error(marker->location, "Enum definition must belong to a configured header.");
            return true;
        }
        Type type;
        type.header = *owner;
        type.line = state_.compiler->getSourceManager().getExpansionLineNumber(definition->getLocation());
        type.enumeration = definition;
        type.qualified = "::" + qualified;
        const auto scope = qualified.rfind("::");
        type.scope = scope == std::string::npos ? "" : qualified.substr(0, scope);
        type.name = definition->getNameAsString();
        type.runtime_name = std::move(runtime_name);
        type.description = description;
        type.underlying = underlying->isSignedIntegerType() ? "::std::int32_t" : "::std::uint32_t";
        type.enum_flags = enum_flags;
        for (const auto* item : definition->enumerators()) {
            Enumerator enumerator{item->getNameAsString()};
            if (auto* item_marker = state_.prefix(*item, MarkerKind::enumerator)) {
                const auto item_options = state_.options(*item_marker, {"Label", "DESC"});
                if (!item_options)
                    return true;
                if (item_options->contains("Label"))
                    enumerator.label = item_options->at("Label");
                if (item_options->contains("DESC"))
                    enumerator.description = item_options->at("DESC");
            }
            type.enumerators.push_back(std::move(enumerator));
        }
        state_.types.push_back(std::move(type));
        return true;
    }

    bool VisitCXXRecordDecl(CXXRecordDecl* declaration) {
        if (declaration->isImplicit())
            return true;
        auto* marker = state_.prefix(*declaration, MarkerKind::object);
        const bool object = marker != nullptr;
        if (!marker)
            marker = state_.prefix(*declaration, MarkerKind::record);
        if (!marker)
            return true;
        std::vector<std::pair<std::string, std::string>> metadata;
        const auto options = state_.options(*marker, {"Name", "DESC", "Validate", "Meta"}, &metadata);
        if (!options)
            return true;
        std::ranges::sort(metadata);
        auto* definition = declaration->getDefinition();
        if (!definition || definition->isDependentType() || definition->isUnion() || definition->isLocalClass() ||
            definition->getName().empty() || isa<CXXRecordDecl>(definition->getDeclContext())) {
            state_.error(marker->location,
                         "Reflected types require a complete, named, non-template namespace-level class or struct.");
            return true;
        }
        const auto owner = state_.header_of(definition->getLocation());
        if (!owner) {
            state_.error(marker->location, "Reflected definition must belong to a configured header.");
            return true;
        }
        for (auto* context = definition->getDeclContext(); !context->isTranslationUnit();
             context = context->getParent())
            if (const auto* scope = dyn_cast<NamespaceDecl>(context); scope && scope->isAnonymousNamespace()) {
                state_.error(marker->location, "Reflected types cannot have anonymous namespace linkage.");
                return true;
            }
        const auto qualified = definition->getQualifiedNameAsString();
        std::string runtime_name = qualified;
        for (auto pos = runtime_name.find("::"); pos != std::string::npos; pos = runtime_name.find("::"))
            runtime_name.replace(pos, 2, ".");
        if (const auto name = options->find("Name"); name != options->end())
            runtime_name = name->second;
        const auto description = options->contains("DESC") ? options->at("DESC") : std::string{};
        const auto validator_option = options->contains("Validate") ? options->at("Validate") : std::string{};
        for (const auto& type : state_.types) {
            if (type.declaration == definition) {
                if (type.object != object || type.runtime_name != runtime_name || type.description != description ||
                    type.validator_option != validator_option || type.metadata != metadata)
                    state_.error(marker->location, "Conflicting reflection markers on redeclarations.");
                return true;
            }
            if (type.runtime_name == runtime_name) {
                state_.error(marker->location, "Duplicate reflected type name: " + runtime_name);
                return true;
            }
        }
        const auto scope_end = qualified.rfind("::");
        state_.types.push_back({definition,
                                *owner,
                                state_.compiler->getSourceManager().getExpansionLineNumber(definition->getLocation()),
                                object,
                                definition->isClass(),
                                "::" + qualified,
                                scope_end == std::string::npos ? "" : qualified.substr(0, scope_end),
                                definition->getNameAsString(),
                                std::move(runtime_name),
                                {},
                                {},
                                {}});
        state_.types.back().description = description;
        state_.types.back().validator_option = validator_option;
        state_.types.back().validator_location = marker->location;
        state_.types.back().metadata = std::move(metadata);
        return true;
    }
};

const Type* reflected(State& state, const CXXRecordDecl* record) {
    if (!record)
        return nullptr;
    const auto* definition = record->getDefinition();
    for (const auto& type : state.types)
        if (type.declaration == definition)
            return &type;
    return nullptr;
}

// A type reflected by another generation group carries AGENERATED_BODY, which
// befriends its Definition. Registration checks that it is a native object type
// registered before this group.
bool generated_elsewhere(const CXXRecordDecl* record) {
    // A template specialization would be named without its arguments.
    if (!record || isa<ClassTemplateSpecializationDecl>(record) || !(record = record->getDefinition()))
        return false;
    for (const auto* declaration : record->friends()) {
        const auto* friend_type = declaration->getFriendType();
        const auto* definition =
            friend_type
                ? dyn_cast_or_null<ClassTemplateSpecializationDecl>(friend_type->getType()->getAsCXXRecordDecl())
                : nullptr;
        if (!definition ||
            definition->getSpecializedTemplate()->getQualifiedNameAsString() != "atelier::reflection::Definition")
            continue;
        const auto& arguments = definition->getTemplateArgs();
        if (arguments.size() != 1 || arguments[0].getKind() != TemplateArgument::Type)
            continue;
        const auto* subject = arguments[0].getAsType()->getAsCXXRecordDecl();
        if (subject && subject->getCanonicalDecl() == record->getCanonicalDecl())
            return true;
    }
    return false;
}

std::optional<std::string> scalar(QualType type, State& state, ASTContext& context, bool allow_void = false) {
    if (type.hasLocalQualifiers())
        return {};
    type = type.getCanonicalType();
    if (allow_void && type->isVoidType())
        return "void";
    if (const auto* enumeration = type->getAs<EnumType>()) {
        for (const auto& reflected_type : state.types)
            if (reflected_type.enumeration == enumeration->getDecl()->getDefinition())
                return reflected_type.qualified;
        if (enumeration->getDecl()->getDefinition() &&
            state.external_enums.contains(enumeration->getDecl()->getCanonicalDecl()))
            return TypeName::getFullyQualifiedName(type, context, context.getPrintingPolicy(), true);
    }
    if (type->isSpecificBuiltinType(BuiltinType::Int) && context.getTypeSize(type) == 32)
        return "::std::int32_t";
    if (type->isSpecificBuiltinType(BuiltinType::Float))
        return "float";
    if (type->isSpecificBuiltinType(BuiltinType::Double))
        return "double";
    if (type->isSpecificBuiltinType(BuiltinType::Bool))
        return "bool";
    if (type->isSpecificBuiltinType(BuiltinType::UInt) && context.getTypeSize(type) == 32)
        return "::std::uint32_t";
    return {};
}

std::optional<std::string> math_value(QualType type, ASTContext& context) {
    if (type.hasLocalQualifiers())
        return {};
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);
    if (!specialization)
        return {};
    const auto name = specialization->getSpecializedTemplate()->getQualifiedNameAsString();
    const bool vector = name == "atelier::math::BasicVector";
    if (!vector && name != "atelier::math::BasicQuaternion" && name != "atelier::math::BasicMatrix4")
        return {};
    const auto& arguments = specialization->getTemplateArgs();
    if (arguments.size() != (vector ? 2u : 1u) || arguments[0].getKind() != TemplateArgument::Type)
        return {};
    const auto scalar_type = arguments[0].getAsType().getCanonicalType();
    if (!scalar_type->isSpecificBuiltinType(BuiltinType::Float) &&
        !scalar_type->isSpecificBuiltinType(BuiltinType::Double))
        return {};
    if (vector && (arguments[1].getKind() != TemplateArgument::Integral || arguments[1].getAsIntegral() < 2 ||
                   arguments[1].getAsIntegral() > 4))
        return {};
    return TypeName::getFullyQualifiedName(type.getCanonicalType(), context, context.getPrintingPolicy(), true);
}

const ClassTemplateSpecializationDecl* array_specialization(QualType type) {
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);
    return specialization && specialization->getSpecializedTemplate()->getQualifiedNameAsString() == "atelier::Array"
               ? specialization
               : nullptr;
}

// Range bounds use the exact leaf scalar; math vectors apply them per component and
// Arrays of scalars or vectors per element.
std::optional<std::string> range_leaf(QualType type, ASTContext& context) {
    if (const auto* array = array_specialization(type))
        type = array->getTemplateArgs()[0].getAsType();
    type = type.getCanonicalType();
    if (const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(type->getAsCXXRecordDecl())) {
        if (!math_value(type, context) ||
            specialization->getSpecializedTemplate()->getQualifiedNameAsString() != "atelier::math::BasicVector")
            return {};
        type = specialization->getTemplateArgs()[0].getAsType().getCanonicalType();
    }
    if (type->isSpecificBuiltinType(BuiltinType::Float))
        return "float";
    if (type->isSpecificBuiltinType(BuiltinType::Double))
        return "double";
    if (type->isSpecificBuiltinType(BuiltinType::Int) && context.getTypeSize(type) == 32)
        return "std::int32_t";
    if (type->isSpecificBuiltinType(BuiltinType::UInt) && context.getTypeSize(type) == 32)
        return "std::uint32_t";
    return {};
}

template <class T>
std::string scalar_literal(T value) {
    if constexpr (std::is_floating_point_v<T>) {
        char digits[64];
        const auto end = std::to_chars(digits, digits + sizeof digits, std::abs(value), std::chars_format::hex).ptr;
        return std::string("Scalar{") + (std::signbit(value) ? "-0x" : "0x") + std::string(digits, end) +
               (std::is_same_v<T, float> ? "f}" : "}");
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return value == std::numeric_limits<std::int32_t>::min()
                   ? "Scalar{std::int32_t{-2147483647 - 1}}"
                   : "Scalar{std::int32_t{" + std::to_string(value) + "}}";
    } else {
        return "Scalar{std::uint32_t{" + std::to_string(value) + "u}}";
    }
}

// Hex floats keep generated bounds bit-exact; integers reject fractions and overflow.
template <class T>
std::optional<std::string> range_bound(const std::string& text) {
    T value{};
    const auto* last = text.data() + text.size();
    if (const auto parsed = std::from_chars(text.data(), last, value); parsed.ec != std::errc{} || parsed.ptr != last)
        return {};
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value))
            return {};
    }
    return scalar_literal(value);
}

bool contains_reference_storage(QualType type, ASTContext& context, std::set<const CXXRecordDecl*>& visited) {
    type = type.getCanonicalType();
    if (const auto* array = context.getAsArrayType(type))
        return contains_reference_storage(array->getElementType(), context, visited);
    const auto* record = type->getAsCXXRecordDecl();
    if (!record)
        return false;
    const auto name = record->getQualifiedNameAsString();
    if (name == "atelier::ReferenceSlot" || name == "atelier::ReferenceSequence" || name == "atelier::ObjectReference")
        return true;
    record = record->getDefinition();
    if (!record || !visited.insert(record).second)
        return false;
    if (const auto* array = array_specialization(type))
        return contains_reference_storage(array->getTemplateArgs()[0].getAsType(), context, visited);
    for (const auto* field : record->fields())
        if (contains_reference_storage(field->getType(), context, visited))
            return true;
    for (const auto& base : record->bases())
        if (contains_reference_storage(base.getType(), context, visited))
            return true;
    return false;
}

bool contains_reference_storage(QualType type, ASTContext& context) {
    std::set<const CXXRecordDecl*> visited;
    return contains_reference_storage(type, context, visited);
}

bool value_field(QualType type, State& state, ASTContext& context) {
    if (type.hasLocalQualifiers())
        return false;
    if (scalar(type, state, context) || math_value(type, context))
        return true;
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    if (!record)
        return false;
    if (record->getQualifiedNameAsString() == "atelier::String")
        return true;
    if (const auto* array = array_specialization(type))
        return !contains_reference_storage(type, context) &&
               value_field(array->getTemplateArgs()[0].getAsType(), state, context);
    const auto* nested = reflected(state, record);
    if (nested)
        return !nested->object;
    return record->getDefinition() && state.external_records.contains(record->getCanonicalDecl());
}

std::optional<std::string> record_value(QualType type, State& state, ASTContext& context) {
    if (type.hasLocalQualifiers())
        return {};
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    const auto* value = reflected(state, record);
    if (value) {
        if (value->object)
            return {};
    } else if (!record || !record->getDefinition() || !state.external_records.contains(record->getCanonicalDecl())) {
        return {};
    }
    // Runtime registration owns the recursive plain-value eligibility check.
    return TypeName::getFullyQualifiedName(type.getCanonicalType(), context, context.getPrintingPolicy(), true);
}

std::optional<QualType> unwrap_result(QualType type) {
    const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(type->getAsCXXRecordDecl());
    if (!specialization || specialization->getSpecializedTemplate()->getQualifiedNameAsString() != "std::expected")
        return {};
    const auto& arguments = specialization->getTemplateArgs();
    if (arguments.size() != 2 || arguments[0].getKind() != TemplateArgument::Type ||
        arguments[1].getKind() != TemplateArgument::Type)
        return {};
    const auto* error = arguments[1].getAsType()->getAsCXXRecordDecl();
    if (!error || error->getQualifiedNameAsString() != "atelier::Error")
        return {};
    return arguments[0].getAsType();
}

bool valid_validator_input(QualType parameter, const CXXRecordDecl* owner) {
    if (!parameter->isLValueReferenceType())
        return false;
    const auto pointee = parameter->getPointeeType();
    if (!pointee.isConstQualified() || pointee.isVolatileQualified())
        return false;
    const auto* record = pointee->getAsCXXRecordDecl();
    return record && record->getCanonicalDecl() == owner->getCanonicalDecl();
}

std::optional<std::string> resolve_validator(State& state, const Type& type, ASTContext& context) {
    llvm::StringRef text = type.validator_option;
    const bool rooted = text.consume_front("::");
    llvm::SmallVector<llvm::StringRef, 4> parts;
    text.split(parts, "::");
    const auto valid_identifier = [](llvm::StringRef part) {
        return !part.empty() && (std::isalpha(static_cast<unsigned char>(part[0])) || part[0] == '_') &&
               std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    };
    if (parts.empty() ||
        std::any_of(parts.begin(), parts.end(), [&](llvm::StringRef p) { return !valid_identifier(p); })) {
        state.error(type.validator_location,
                    "Validate requires a valid member or function name: " + type.validator_option);
        return {};
    }
    const auto find_id = [&](llvm::StringRef name) -> const IdentifierInfo* {
        const auto found = context.Idents.find(name);
        return found == context.Idents.end() ? nullptr : found->second;
    };
    const auto void_result = [](QualType ret) {
        const auto inner = unwrap_result(ret);
        return !ret.getCanonicalType().hasLocalQualifiers() && inner && (*inner)->isVoidType();
    };
    if (!rooted && parts.size() == 1) {
        if (const auto* id = find_id(parts[0])) {
            const CXXMethodDecl* matched{};
            std::size_t callables = 0;
            for (const auto* decl : type.declaration->lookup(id)) {
                if (isa<FunctionTemplateDecl>(decl))
                    ++callables;
                if (const auto* method = dyn_cast<CXXMethodDecl>(decl)) {
                    ++callables;
                    if (!method->isDeleted() && !method->isVariadic() && void_result(method->getReturnType()) &&
                        ((!method->isStatic() && method->isConst() && !method->isVolatile() &&
                          method->getRefQualifier() == RQ_None && method->getNumParams() == 0) ||
                         (method->isStatic() && method->getNumParams() == 1 &&
                          valid_validator_input(method->getParamDecl(0)->getType(), type.declaration)))) {
                        matched = method;
                    }
                }
            }
            if (callables > 0) {
                if (!matched || callables != 1) {
                    state.error(type.validator_location,
                                "Validate member must be an unambiguous const() or static(const " + type.name +
                                    "&) method returning Result<void>: " + type.validator_option);
                    return {};
                }
                return "&Self::" + parts[0].str();
            }
        }
    }
    for (const DeclContext* start = rooted ? context.getTranslationUnitDecl() : type.declaration->getDeclContext();
         start; start = rooted ? nullptr : start->getParent()) {
        const DeclContext* scope = start->getPrimaryContext();
        bool resolved = true;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            const auto* id = find_id(parts[i]);
            const DeclContext* next = nullptr;
            if (id) {
                for (const auto* decl : scope->lookup(id)) {
                    if (const auto* ns = dyn_cast<NamespaceDecl>(decl)) {
                        next = ns->getPrimaryContext();
                        break;
                    }
                    if (const auto* rec = dyn_cast<CXXRecordDecl>(decl); rec && rec->getDefinition()) {
                        next = rec->getDefinition();
                        break;
                    }
                }
            }
            if (!next) {
                resolved = false;
                break;
            }
            scope = next;
        }
        if (!resolved)
            continue;
        const auto* id = find_id(parts.back());
        if (!id)
            continue;
        const FunctionDecl* matched{};
        std::size_t callables = 0;
        for (const auto* decl : scope->lookup(id)) {
            if (isa<FunctionTemplateDecl>(decl))
                ++callables;
            if (const auto* fn = dyn_cast<FunctionDecl>(decl)) {
                ++callables;
                const auto* method = dyn_cast<CXXMethodDecl>(fn);
                if (!fn->isDeleted() && !fn->isVariadic() && (!method || method->isStatic()) &&
                    fn->getNumParams() == 1 && void_result(fn->getReturnType()) &&
                    valid_validator_input(fn->getParamDecl(0)->getType(), type.declaration)) {
                    matched = fn;
                }
            }
        }
        if (callables > 0) {
            if (!matched || callables != 1) {
                state.error(type.validator_location, "Validate function must unambiguously take const " + type.name +
                                                         "& and return Result<void>: " + type.validator_option);
                return {};
            }
            return "&::" + matched->getQualifiedNameAsString();
        }
    }
    state.error(type.validator_location, "Unresolved type validator: " + type.validator_option);
    return {};
}

void collect_external_reflected(State& state, ASTContext& context) {
    const auto find_id = [&](llvm::StringRef name) -> const IdentifierInfo* {
        const auto found = context.Idents.find(name);
        return found == context.Idents.end() ? nullptr : found->second;
    };
    const auto* atelier_id = find_id("atelier");
    const auto* reflection_id = find_id("reflection");
    const auto* reflected_id = find_id("Reflected");
    if (!atelier_id || !reflection_id || !reflected_id)
        return;
    for (const auto* top : context.getTranslationUnitDecl()->lookup(atelier_id)) {
        const auto* ns_atelier = dyn_cast<NamespaceDecl>(top);
        if (!ns_atelier)
            continue;
        for (const auto* sub : ns_atelier->getPrimaryContext()->lookup(reflection_id)) {
            const auto* ns_reflection = dyn_cast<NamespaceDecl>(sub);
            if (!ns_reflection)
                continue;
            for (const auto* decl : ns_reflection->getPrimaryContext()->lookup(reflected_id)) {
                const auto* tmpl = dyn_cast<ClassTemplateDecl>(decl);
                if (!tmpl)
                    continue;
                for (const auto* spec : tmpl->getCanonicalDecl()->specializations()) {
                    if (!spec->isExplicitSpecialization())
                        continue;
                    const auto& args = spec->getTemplateArgs();
                    if (args.size() != 2 || args[0].getKind() != TemplateArgument::Type ||
                        args[1].getKind() != TemplateArgument::Integral)
                        continue;
                    const auto* enum_type = args[1].getIntegralType()->getAs<EnumType>();
                    if (!enum_type)
                        continue;
                    llvm::StringRef kind;
                    for (const auto* item : enum_type->getDecl()->enumerators())
                        if (llvm::APSInt::isSameValue(item->getInitVal(), args[1].getAsIntegral()))
                            kind = item->getName();
                    const auto target = args[0].getAsType().getCanonicalType();
                    if (kind == "enumeration") {
                        if (const auto* enumeration = target->getAs<EnumType>())
                            state.external_enums.insert(enumeration->getDecl()->getCanonicalDecl());
                    } else if (const auto* record = target->getAsCXXRecordDecl()) {
                        if (kind == "object")
                            state.external_objects.insert(record->getCanonicalDecl());
                        else if (kind == "record")
                            state.external_records.insert(record->getCanonicalDecl());
                    }
                }
                return;
            }
        }
    }
}

void members(State& state, Type& type, ASTContext& context) {
    if (type.enumeration)
        return;
    auto& sources = state.compiler->getSourceManager();
    const auto* record = type.declaration;
    if (record->getNumBases() > (type.object ? 1u : 0u)) {
        state.error(record->getLocation(), "This reflection slice supports one object base and no record inheritance.");
        return;
    }
    const auto& layout = context.getASTRecordLayout(record);
    for (const auto& base : record->bases()) {
        const auto* declaration = base.getType()->getAsCXXRecordDecl();
        const auto* parent = reflected(state, declaration);
        const auto* canonical_base = declaration ? declaration->getCanonicalDecl() : nullptr;
        const bool external = !parent && canonical_base &&
                              (state.external_objects.contains(canonical_base) ||
                               (!state.external_records.contains(canonical_base) && generated_elsewhere(declaration)));
        if ((parent ? !parent->object : !external) || base.isVirtual() || base.getAccessSpecifier() != AS_public) {
            state.error(base.getBeginLoc(), "A reflected base must be a public, non-virtual reflected object type.");
            return;
        }
        const auto* base_definition = declaration->getDefinition();
        const auto alignment = context.getASTRecordLayout(base_definition).getAlignment();
        const auto offset = layout.getBaseClassOffset(base_definition);
        if (layout.getAlignment() < alignment || !offset.isMultipleOf(alignment)) {
            state.error(base.getBeginLoc(), "Reflected type " + type.name + " guarantees alignment " +
                                                std::to_string(layout.getAlignment().getQuantity()) + ", but base " +
                                                base_definition->getQualifiedNameAsString() + " at offset " +
                                                std::to_string(offset.getQuantity()) + " requires alignment " +
                                                std::to_string(alignment.getQuantity()) + ".");
            return;
        }
        type.base = parent ? parent->qualified : "::" + base_definition->getQualifiedNameAsString();
        type.base_offset = static_cast<std::uint64_t>(offset.getQuantity());
    }
    if (!type.validator_option.empty()) {
        if (const auto resolved = resolve_validator(state, type, context))
            type.validator = *resolved;
    }
    unsigned bodies = 0;
    const auto begin = sources.getFileOffset(record->getBeginLoc());
    const auto end = sources.getFileOffset(record->getEndLoc());
    for (auto& marker : state.markers) {
        if (marker.kind != MarkerKind::body || marker.header != type.header)
            continue;
        const auto offset = sources.getFileOffset(marker.location);
        if (offset <= begin || offset >= end)
            continue;
        marker.used = true;
        ++bodies;
        if (marker.arguments.size() != 1 || !marker.arguments.front().is(tok::identifier) ||
            marker.arguments.front().getIdentifierInfo()->getName() != type.name)
            state.error(marker.location, "AGENERATED_BODY must name its containing type.");
    }
    if (bodies != 1)
        state.error(record->getLocation(), "Each reflected type requires exactly one AGENERATED_BODY(TypeName).");
    std::set<std::string> names;
    for (const auto* field : record->fields()) {
        auto* marker = state.prefix(*field, MarkerKind::field);
        const auto canonical = field->getType().getCanonicalType();
        const auto* field_record = canonical->getAsCXXRecordDecl();
        const auto record_name = field_record ? field_record->getQualifiedNameAsString() : "";
        const bool slot = record_name == "atelier::ReferenceSlot";
        const bool sequence = record_name == "atelier::ReferenceSequence";
        if (!marker) {
            if (contains_reference_storage(field->getType(), context))
                state.error(
                    field->getLocation(),
                    "Runtime reference storage, including inline records and arrays, requires an explicit AFIELD marker.");
            continue;
        }
        std::vector<std::pair<std::string, std::string>> metadata;
        const auto options =
            state.options(*marker,
                          {"Name", "Label", "Category", "DESC", "Hidden", "Transient", "ReadOnly", "RunOver", "Strong",
                           "Weak", "Sequence", "Min", "Max", "LockMin", "LockMax", "HardMin", "HardMax", "Meta"},
                          &metadata);
        if (!options)
            continue;
        if (options->contains("RunOver"))
            metadata.emplace_back("procedural.role", "run_over");
        if (field->isBitField() || field->isAnonymousStructOrUnion() || field->getType().isConstQualified() ||
            field->getType().isVolatileQualified()) {
            state.error(field->getLocation(), "AFIELD requires a named, mutable, addressable data member.");
            continue;
        }
        Field result{field->getNameAsString(),
                     field->getNameAsString(),
                     {},
                     {},
                     options->contains("Hidden"),
                     sources.getExpansionLineNumber(field->getLocation())};
        result.transient = options->contains("Transient");
        result.read_only = options->contains("ReadOnly");
        if (result.hidden && (result.transient || result.read_only)) {
            state.error(field->getLocation(), "Hidden fields cannot combine with Transient or ReadOnly.");
            continue;
        }
        const auto char_bits = context.getCharWidth();
        const auto alignment = context.getTypeAlign(field->getType());
        const auto bit_offset = layout.getFieldOffset(field->getFieldIndex());
        if (bit_offset % char_bits == 0 && layout.getAlignment().getQuantity() * char_bits >= alignment &&
            bit_offset % alignment == 0) {
            result.byte_offset = bit_offset / char_bits;
        }
        if (options->contains("Name"))
            result.name = options->at("Name");
        if (options->contains("Label"))
            result.label = options->at("Label");
        if (options->contains("Category"))
            result.category = options->at("Category");
        if (options->contains("DESC"))
            result.description = options->at("DESC");
        result.metadata = std::move(metadata);
        unsigned forms = 0;
        for (const auto* form : {"Strong", "Weak", "Sequence"})
            if (options->contains(form)) {
                ++forms;
                result.target = options->at(form);
                result.form = std::string(form) == "Strong" ? "strong_reference"
                              : std::string(form) == "Weak" ? "weak_reference"
                                                            : "strong_sequence";
            }
        if ((slot && (forms != 1 || result.form == "strong_sequence")) ||
            (sequence && (forms != 1 || result.form != "strong_sequence" || !type.object)) ||
            (!slot && !sequence && forms != 0)) {
            state.error(
                field->getLocation(),
                "Reference storage requires exactly its matching Strong, Weak or Sequence target; sequences belong to objects.");
            continue;
        }
        if (slot || sequence) {
            // Targets outside this group resolve by name when the Runtime publishes the batch.
            const auto target = std::find_if(state.types.begin(), state.types.end(), [&](const Type& candidate) {
                return candidate.runtime_name == result.target;
            });
            if (result.target.empty() || (target != state.types.end() && !target->object)) {
                state.error(field->getLocation(), "Reference target must name an object type: " + result.target);
                continue;
            }
        } else {
            if (!value_field(field->getType(), state, context)) {
                state.error(
                    field->getLocation(),
                    "AFIELD requires a supported scalar/enum/record, String, reference-free Array<T>, or explicit Runtime reference storage. Borrowed views cannot be persistent fields.");
                continue;
            }
        }
        if (std::ranges::any_of(std::array{"Min", "Max", "LockMin", "LockMax", "HardMin", "HardMax"},
                                [&](const char* name) { return options->contains(name); })) {
            const auto leaf = range_leaf(field->getType(), context);
            if (!leaf) {
                state.error(
                    field->getLocation(),
                    "Field ranges require a float, double, int32, uint32 or math vector field, or an Array of them.");
                continue;
            }
            bool valid = true;
            for (const std::string side : {"Min", "Max"})
                if (options->contains("Lock" + side) &&
                    (!options->contains(side) || options->contains("Hard" + side))) {
                    state.error(field->getLocation(),
                                "Lock" + side + " requires " + side + " and excludes Hard" + side + ".");
                    valid = false;
                }
            // Runtime publication owns ordering, UI-within-hard and default checks.
            const auto bound = [&](const char* member, const std::string& option) {
                if (!valid || !options->contains(option))
                    return;
                const auto& text = options->at(option);
                const auto parsed = *leaf == "float"          ? range_bound<float>(text)
                                    : *leaf == "double"       ? range_bound<double>(text)
                                    : *leaf == "std::int32_t" ? range_bound<std::int32_t>(text)
                                                              : range_bound<std::uint32_t>(text);
                if (!parsed) {
                    state.error(field->getLocation(),
                                option + " requires a finite decimal " + *leaf + " value: " + text);
                    valid = false;
                    return;
                }
                result.range += std::string(result.range.empty() ? "" : ", ") + "." + member + " = " + *parsed;
            };
            bound("ui_min", "Min");
            bound("ui_max", "Max");
            bound("hard_min", options->contains("LockMin") ? "Min" : "HardMin");
            bound("hard_max", options->contains("LockMax") ? "Max" : "HardMax");
            if (!valid)
                continue;
        }
        if (!names.insert(result.name).second)
            state.error(field->getLocation(), "Duplicate reflected member name: " + result.name);
        type.fields.push_back(std::move(result));
    }
    for (const auto* method : record->methods()) {
        auto* marker = state.prefix(*method, MarkerKind::method);
        if (!marker)
            continue;
        const auto options = state.options(*marker, {"Name", "BorrowSafe", "DESC"});
        if (!options)
            continue;
        if (!type.object) {
            state.error(method->getLocation(), "AFUNCTION is supported on ACLASS object types only.");
            continue;
        }
        auto result_type = method->getReturnType();
        bool fallible = false;
        if (const auto unwrapped = unwrap_result(result_type)) {
            result_type = *unwrapped;
            fallible = true;
        }
        auto result = scalar(result_type, state, context, true);
        if (!result)
            result = math_value(result_type, context);
        if (!result)
            result = record_value(result_type, state, context);
        const bool contextual = method->isStatic();
        bool receiver_read_only = false;
        if (contextual) {
            bool ok = result && fallible && !method->isVariadic() && !method->isDeleted() &&
                      !method->getDescribedFunctionTemplate() && !method->isOverloadedOperator() &&
                      method->getNumParams() >= 3;
            if (ok) {
                const auto p0 = method->getParamDecl(0)->getType();
                const auto p1 = method->getParamDecl(1)->getType();
                const auto p2 = method->getParamDecl(2)->getType();
                const auto* r0 = p0->isLValueReferenceType() ? p0->getPointeeType()->getAsCXXRecordDecl() : nullptr;
                const auto* r1 =
                    (!p1->isReferenceType() && !p1.hasLocalQualifiers()) ? p1->getAsCXXRecordDecl() : nullptr;
                const auto* r2 = p2->isLValueReferenceType() ? p2->getPointeeType()->getAsCXXRecordDecl() : nullptr;
                ok = r0 && !p0->getPointeeType().isConstQualified() && !p0->getPointeeType().isVolatileQualified() &&
                     r0->getQualifiedNameAsString() == "atelier::Runtime" && r1 &&
                     r1->getQualifiedNameAsString() == "atelier::ObjectId" && r2 &&
                     !p2->getPointeeType().isVolatileQualified() &&
                     r2->getCanonicalDecl() == record->getCanonicalDecl();
                if (ok)
                    receiver_read_only = p2->getPointeeType().isConstQualified();
            }
            if (!ok) {
                state.error(
                    method->getLocation(),
                    "Static AFUNCTION requires contextual signature Result<R>(Runtime&, ObjectId, [const] Self&, ...).");
                continue;
            }
        } else if (!result || method->isVariadic() || method->isDeleted() || method->isVolatile() ||
                   method->getRefQualifier() != RQ_None || method->getDescribedFunctionTemplate() ||
                   isa<CXXConstructorDecl>(method) || isa<CXXDestructorDecl>(method) ||
                   method->isOverloadedOperator() || isa<CXXConversionDecl>(method)) {
            state.error(
                method->getLocation(),
                "AFUNCTION requires a supported member with a scalar, void or plain record result, optionally in runtime Result.");
            continue;
        }
        if (fallible)
            *result = "::atelier::Result<" + *result + ">";
        Method value{method->getNameAsString(), method->getNameAsString(),
                     contextual ? *result + " (*)(::atelier::Runtime&, ::atelier::ObjectId, " +
                                      (receiver_read_only ? "const Self&" : "Self&")
                                : *result + " (Self::*)(",
                     sources.getExpansionLineNumber(method->getLocation())};
        if (options->contains("Name"))
            value.name = options->at("Name");
        value.preserves_borrows = options->contains("BorrowSafe");
        if (options->contains("DESC"))
            value.description = options->at("DESC");
        bool valid = true;
        const unsigned first_param = contextual ? 3u : 0u;
        for (unsigned i = first_param; i < method->getNumParams(); ++i) {
            const auto* param_decl = method->getParamDecl(i);
            std::vector<std::pair<std::string, std::string>> param_meta;
            if (auto* param_marker = state.prefix(*param_decl, MarkerKind::parameter)) {
                const auto param_options = state.options(
                    *param_marker, {"Port", "In", "Out", "InOut", "Group", "From", "Layer", "Optional", "Meta"},
                    &param_meta);
                if (!param_options) {
                    valid = false;
                    break;
                }
                constexpr std::array<std::pair<const char*, const char*>, 5> roles{
                    {{"Port", "port"}, {"In", "in"}, {"Out", "out"}, {"InOut", "inout"}, {"Group", "group"}}};
                std::string role;
                std::string explicit_name;
                bool has_explicit_name = false;
                for (const auto& [option_name, role_name] : roles) {
                    const auto found = param_options->find(option_name);
                    if (found == param_options->end())
                        continue;
                    if (!role.empty()) {
                        state.error(param_marker->location, "APARAM supports at most one procedural role.");
                        valid = false;
                        break;
                    }
                    role = role_name;
                    if (!found->second.empty() || param_options->contains("__explicit_role_name") ||
                        std::string_view{option_name} == "Group") {
                        if (param_options->contains("__explicit_role_name"))
                            has_explicit_name = true;
                        for (std::size_t t = 0; t + 1 < param_marker->arguments.size(); ++t)
                            if (param_marker->arguments[t].is(tok::identifier) &&
                                param_marker->arguments[t].getIdentifierInfo()->getName() == option_name &&
                                param_marker->arguments[t + 1].is(tok::equal))
                                has_explicit_name = true;
                        explicit_name = found->second;
                    }
                }
                if (!valid)
                    break;
                if (!role.empty()) {
                    if (role == "port" && (param_options->contains("From") || param_options->contains("Layer") ||
                                           param_options->contains("Optional"))) {
                        state.error(param_marker->location, "Port parameters do not accept From, Layer or Optional.");
                        valid = false;
                        break;
                    }
                    if (has_explicit_name && param_options->contains("From")) {
                        state.error(param_marker->location,
                                    "Specify either a literal attribute name or From, not both.");
                        valid = false;
                        break;
                    }
                    if (param_options->contains("Optional") && role != "in") {
                        state.error(param_marker->location, "Optional applies only to In parameters.");
                        valid = false;
                        break;
                    }
                    param_meta.emplace_back("procedural.role", role);
                    if (has_explicit_name)
                        param_meta.emplace_back("procedural.name", std::move(explicit_name));
                    if (param_options->contains("From"))
                        param_meta.emplace_back("procedural.from", param_options->at("From"));
                    if (param_options->contains("Layer"))
                        param_meta.emplace_back("procedural.layer", param_options->at("Layer"));
                    if (param_options->contains("Optional"))
                        param_meta.emplace_back("procedural.optional", "true");
                } else if (param_options->contains("From") || param_options->contains("Layer") ||
                           param_options->contains("Optional")) {
                    state.error(param_marker->location,
                                "From, Layer and Optional require an In, Out, InOut or Group role.");
                    valid = false;
                    break;
                }
            }
            auto parameter_type = param_decl->getType();
            const bool reference = parameter_type->isLValueReferenceType();
            if (reference)
                parameter_type = parameter_type->getPointeeType();
            const bool read_only = parameter_type.isConstQualified();
            const auto scalar_param = parameter_type.isVolatileQualified()
                                          ? std::optional<std::string>{}
                                          : scalar(parameter_type.getUnqualifiedType(), state, context);
            auto parameter = scalar_param;
            if (!parameter && !parameter_type.isVolatileQualified())
                parameter = math_value(parameter_type.getUnqualifiedType(), context);
            if (!parameter && !reference && !parameter_type.isVolatileQualified())
                parameter = record_value(parameter_type.getUnqualifiedType(), state, context);
            if (!parameter && reference && !parameter_type.isVolatileQualified() &&
                value_field(parameter_type.getUnqualifiedType(), state, context) &&
                !contains_reference_storage(parameter_type, context))
                parameter = TypeName::getFullyQualifiedName(parameter_type.getCanonicalType().getUnqualifiedType(),
                                                            context, context.getPrintingPolicy(), true);
            if (parameter && reference)
                *parameter = (read_only ? "const " : "") + *parameter + "&";
            if (!parameter) {
                state.error(param_decl->getLocation(),
                            "Reflected parameters require scalar/enum/plain record values or const T&/T& borrows.");
                valid = false;
                break;
            }
            std::optional<std::string> default_expr;
            if (param_decl->hasDefaultArg()) {
                if (!scalar_param || (reference && !read_only) || param_decl->hasUnparsedDefaultArg() ||
                    param_decl->hasUninstantiatedDefaultArg()) {
                    state.error(param_decl->getLocation(),
                                "Reflected default arguments require a value or const& scalar/enum parameter.");
                    valid = false;
                    break;
                }
                Expr::EvalResult evaluated;
                if (!param_decl->getDefaultArg()->EvaluateAsRValue(evaluated, context) || evaluated.HasSideEffects) {
                    state.error(param_decl->getLocation(),
                                "Reflected default arguments require a constant scalar or enum expression.");
                    valid = false;
                    break;
                }
                const auto canonical = parameter_type.getCanonicalType().getUnqualifiedType();
                if (canonical->isSpecificBuiltinType(BuiltinType::Bool)) {
                    default_expr = evaluated.Val.getInt().getBoolValue() ? "Scalar{true}" : "Scalar{false}";
                } else if (canonical->isSpecificBuiltinType(BuiltinType::Float) ||
                           canonical->isSpecificBuiltinType(BuiltinType::Double)) {
                    const bool is_float = canonical->isSpecificBuiltinType(BuiltinType::Float);
                    const auto& fp = evaluated.Val.getFloat();
                    if (!fp.isFinite()) {
                        state.error(param_decl->getLocation(), "Reflected floating default arguments must be finite.");
                        valid = false;
                        break;
                    }
                    default_expr =
                        is_float ? scalar_literal(fp.convertToFloat()) : scalar_literal(fp.convertToDouble());
                } else {
                    const auto int_type =
                        canonical->isEnumeralType()
                            ? canonical->getAs<EnumType>()->getDecl()->getIntegerType().getCanonicalType()
                            : canonical;
                    default_expr =
                        int_type->isSignedIntegerType()
                            ? scalar_literal(static_cast<std::int32_t>(evaluated.Val.getInt().getSExtValue()))
                            : scalar_literal(static_cast<std::uint32_t>(evaluated.Val.getInt().getZExtValue()));
                }
            }
            if (contextual || i > first_param)
                value.pointer += ", ";
            value.pointer += *parameter;
            value.parameters.push_back(param_decl->getName().str());
            value.defaults.push_back(std::move(default_expr));
            value.parameter_metadata.push_back(std::move(param_meta));
        }
        if (!valid)
            continue;
        value.pointer += ")";
        if (!contextual && method->isConst())
            value.pointer += " const";
        const auto* prototype = method->getType()->castAs<FunctionProtoType>();
        if (isUnresolvedExceptionSpec(prototype->getExceptionSpecType()) && state.compiler->hasSema())
            prototype = state.compiler->getSema().ResolveExceptionSpec(method->getLocation(), prototype);
        if (prototype && prototype->isNothrow())
            value.pointer += " noexcept";
        if (!names.insert(value.name).second)
            state.error(method->getLocation(), "Reflected overloads require distinct Name aliases: " + value.name);
        type.methods.push_back(std::move(value));
    }
}

class Consumer final : public ASTConsumer {
    State& state_;

public:
    explicit Consumer(State& state) : state_(state) {}

    void HandleTranslationUnit(ASTContext& context) override {
        if (state_.compiler->getDiagnostics().hasErrorOccurred())
            return;
        collect_external_reflected(state_, context);
        Collector(state_).TraverseDecl(context.getTranslationUnitDecl());
        for (auto& type : state_.types)
            members(state_, type, context);
        for (const auto& type : state_.types) {
            std::set<std::string> names;
            for (const auto& field : type.fields)
                names.insert(field.name);
            for (const auto& method : type.methods)
                names.insert(method.name);
            auto base = type.base;
            while (!base.empty()) {
                const auto parent = std::find_if(state_.types.begin(), state_.types.end(),
                                                 [&](const Type& candidate) { return candidate.qualified == base; });
                if (parent == state_.types.end())
                    break;
                for (const auto& field : parent->fields)
                    if (!names.insert(field.name).second)
                        state_.error(type.declaration->getLocation(),
                                     "Inherited reflected member name cannot be shadowed: " + field.name);
                for (const auto& method : parent->methods)
                    if (!names.insert(method.name).second)
                        state_.error(type.declaration->getLocation(),
                                     "Inherited reflected member name cannot be shadowed: " + method.name);
                base = parent->base;
            }
        }
        for (const auto& marker : state_.markers)
            if (!marker.used)
                state_.error(
                    marker.location,
                    "Reflection marker does not immediately precede a supported declaration in a reflected type.");
        for (const auto& header : state_.config.headers)
            if (!header.included)
                state_.error({}, "Reflected header must directly include its generated header: " + header.include);
        if (state_.types.empty())
            state_.error({}, "No reflected type definitions found in the configured headers.");
    }
};

class Action final : public ASTFrontendAction {
    State& state_;

public:
    explicit Action(State& state) : state_(state) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, llvm::StringRef) override {
        state_.compiler = &compiler;
        compiler.getFrontendOpts().SkipFunctionBodies = true;
        // Skipping function bodies hides references to private fields and file-local helpers;
        // real C++ compilation of the translation unit still enforces unused-symbol warnings.
        compiler.getDiagnostics().setSeverityForGroup(diag::Flavor::WarningOrError, "unused", diag::Severity::Ignored);
        compiler.getPreprocessor().addPPCallbacks(std::make_unique<Markers>(state_));
        return std::make_unique<Consumer>(state_);
    }
};

class Factory final : public tooling::FrontendActionFactory {
    State& state_;

public:
    explicit Factory(State& state) : state_(state) {}

    std::unique_ptr<FrontendAction> create() override { return std::make_unique<Action>(state_); }
};

llvm::Error write_changed(const std::string& path, const std::string& text) {
    const auto current = llvm::MemoryBuffer::getFile(path);
    if (current && (*current)->getBuffer() == text)
        return llvm::Error::success();
    if (const auto error = llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path)))
        return llvm::errorCodeToError(error);
    const auto temporary = path + ".tmp";
    std::error_code error;
    {
        llvm::raw_fd_ostream file(temporary, error);
        if (error)
            return llvm::errorCodeToError(error);
        file << text;
        file.flush();
        if (file.has_error()) {
            // The error is returned below; do not let the stream destructor report it as fatal.
            file.clear_error();
            return llvm::createStringError("Cannot write generated file: " + path);
        }
    }
    return llvm::errorCodeToError(llvm::sys::fs::rename(temporary, path));
}

std::string dependency_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string result;
    for (const auto c : path) {
        if (c == '$')
            result += '$';
        else if (c == ' ' || c == '#' || c == ':')
            result += '\\';
        result += c;
    }
    return result;
}

llvm::Error emit(const State& state) {
    std::vector<std::string> headers(
        state.config.headers.size(),
        "// Generated by atelier_reflect. Do not edit.\n#pragma once\n#include \"runtime/reflection.hpp\"\n\n");
    std::string cpp = "// Generated by atelier_reflect. Do not edit.\n#include \"runtime/native.hpp\"\n";
    for (const auto& header : state.config.headers)
        cpp += "#include " + quote(header.include) + "\n";
    cpp += "\nnamespace atelier::reflection {\n";
    for (const auto& type : state.types) {
        auto& header = headers[type.header];
        if (!type.scope.empty())
            header += "namespace " + type.scope + " { ";
        if (type.enumeration)
            header += "enum class " + type.name + " : " + type.underlying + ";";
        else
            header += std::string(type.class_tag ? "class " : "struct ") + type.name + ";";
        if (!type.scope.empty())
            header += " }";
        const char* kind = type.enumeration ? "enumeration" : type.object ? "object" : "record";
        header += "\nnamespace atelier::reflection {\ntemplate<> TypeDefinition Definition<" + type.qualified +
                  ">::build(Runtime&);\ntemplate<> struct Reflected<" + type.qualified + ", TypeKind::" + kind +
                  "> {};\n}\n";
        const auto location = [&](unsigned line) {
            return "\n#line " + std::to_string(line) + " " + quote(state.config.headers[type.header].include) + "\n";
        };
        cpp += location(type.line) + "template<> TypeDefinition Definition<" + type.qualified +
               ">::build(Runtime& runtime) {\n    using Self = " + type.qualified + ";\n";
        cpp += "    return NativeRegistration<Self>{runtime, " + quote(type.runtime_name) + ", TypeKind::" + kind;
        if (!type.description.empty())
            cpp += ", " + quote(type.description);
        cpp += "}";
        if (!type.metadata.empty()) {
            cpp += "\n        .type_metadata({";
            for (std::size_t i = 0; i < type.metadata.size(); ++i) {
                const auto& [key, value] = type.metadata[i];
                cpp += (i ? ", " : "") + std::string{"{"} + metadata_text(key) + ", " + metadata_text(value) + "}";
            }
            cpp += "})";
        }
        if (type.enum_flags)
            cpp += "\n        .flags()";
        for (const auto& item : type.enumerators) {
            cpp += "\n        .enumerator(" + quote(item.name) + ", Self::" + item.name;
            if (!item.label.empty() || !item.description.empty()) {
                cpp += ", " + quote(item.label);
                if (!item.description.empty())
                    cpp += ", " + quote(item.description);
            }
            cpp += ")";
        }
        if (!type.base.empty()) {
            cpp += "\n        .base<" + type.base + ">(";
            if (type.base_offset)
                cpp += std::to_string(*type.base_offset) + "u";
            cpp += ")";
        }
        if (!type.validator.empty())
            cpp += "\n        .validate<" + type.validator + ">()";
        for (const auto& field : type.fields) {
            const bool presentation = !field.label.empty() || !field.category.empty() || !field.description.empty();
            cpp += location(field.line) + "        ." + (field.hidden ? "hidden_field" : "field") +
                   "<&Self::" + field.cpp_name + ">(" + quote(field.name);
            if (!field.form.empty())
                cpp += ", {TypeReference{" + quote(field.target) + "}, ValueForm::" + field.form + "}";
            else if (field.byte_offset || presentation)
                cpp += ", {TypeReference::native<decltype(Self::" + field.cpp_name + ")>()}";
            if (field.byte_offset)
                cpp += ", " + std::to_string(*field.byte_offset) + "u";
            else if (presentation)
                cpp += ", std::nullopt";
            if (presentation)
                cpp += ", " + quote(field.label) + ", " + quote(field.category);
            if (!field.description.empty())
                cpp += ", " + quote(field.description);
            cpp += ")";
            if (field.transient)
                cpp += "\n        .transient()";
            if (field.read_only)
                cpp += "\n        .read_only()";
            if (!field.range.empty())
                cpp += "\n        .range({" + field.range + "})";
            if (!field.metadata.empty()) {
                cpp += "\n        .metadata({";
                for (std::size_t i = 0; i < field.metadata.size(); ++i) {
                    const auto& [key, value] = field.metadata[i];
                    cpp += (i ? ", " : "") + std::string{"{"} + metadata_text(key) + ", " + metadata_text(value) + "}";
                }
                cpp += "})";
            }
        }
        for (const auto& method : type.methods) {
            cpp += location(method.line) + "        .method<static_cast<" + method.pointer +
                   ">(&Self::" + method.cpp_name + ")>(" + quote(method.name);
            if (method.preserves_borrows || !method.description.empty())
                cpp += method.preserves_borrows ? ", true" : ", false";
            if (!method.description.empty())
                cpp += ", " + quote(method.description);
            cpp += ")";
            if (!method.parameters.empty()) {
                cpp += "\n        .parameter_names({";
                for (std::size_t i = 0; i < method.parameters.size(); ++i)
                    cpp += (i ? ", " : "") + quote(method.parameters[i]);
                cpp += "})";
            }
            if (std::any_of(method.defaults.begin(), method.defaults.end(),
                            [](const auto& item) { return item.has_value(); })) {
                cpp += "\n        .parameter_defaults({";
                for (std::size_t i = 0; i < method.defaults.size(); ++i)
                    cpp += (i ? ", " : "") + (method.defaults[i] ? *method.defaults[i] : "std::nullopt");
                cpp += "})";
            }
            if (std::any_of(method.parameter_metadata.begin(), method.parameter_metadata.end(),
                            [](const auto& entries) { return !entries.empty(); })) {
                cpp += "\n        .parameter_metadata({";
                for (std::size_t i = 0; i < method.parameter_metadata.size(); ++i) {
                    cpp += (i ? ", {" : "{");
                    for (std::size_t j = 0; j < method.parameter_metadata[i].size(); ++j) {
                        const auto& [key, val] = method.parameter_metadata[i][j];
                        cpp +=
                            (j ? ", " : "") + std::string{"{"} + metadata_text(key) + ", " + metadata_text(val) + "}";
                    }
                    cpp += "}";
                }
                cpp += "})";
            }
        }
        cpp += "\n        .build();\n}\n";
    }
    cpp += "}\n";
    std::string dependencies = dependency_path(state.config.cpp) + ":";
    for (const auto& path : state.dependencies)
        dependencies += " \\\n  " + dependency_path(path);
    dependencies += "\n";
    for (std::size_t i = 0; i < headers.size(); ++i)
        if (auto error = write_changed(state.config.headers[i].output, headers[i]))
            return error;
    if (auto error = write_changed(state.config.cpp, cpp))
        return error;
    return write_changed(state.config.depfile, dependencies);
}
}

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    llvm::install_fatal_error_handler([](void*, const char*, bool) { atelier::fatal("llvm fatal error"); });
    auto generated = [&]() -> llvm::Expected<int> {
        if (argc != 2)
            return llvm::createStringError("Usage: atelier_reflect <reflection.json>");
        auto config = read_config(argv[1]);
        if (!config)
            return config.takeError();
        std::string error;
        auto database = clang::tooling::CompilationDatabase::loadFromDirectory(config->database, error);
        if (!database)
            return llvm::createStringError(error);
        if (database->getCompileCommands(config->source).size() != 1)
            return llvm::createStringError("SOURCE must select exactly one real compile command: " + config->source);
        State state{*config};
        clang::tooling::ClangTool tool(*database, {config->source});
        // Metadata producers have a fixed declaration in reflection.hpp, so a
        // real TU can be checked without reading stale generated specializations.
        for (const auto& header : config->headers)
            tool.mapVirtualFile(header.output, "#pragma once\n");
        tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
            {"-resource-dir", config->resource_dir.c_str()}, clang::tooling::ArgumentInsertPosition::BEGIN));
        Factory factory(state);
        const auto result = tool.run(&factory);
        if (result != 0 || state.failed)
            return 1;
        if (auto failure = emit(state))
            return failure;
        llvm::outs() << "Generated " << state.types.size() << " reflected types.\n";
        return 0;
    }();
    if (!generated) {
        llvm::errs() << "atelier_reflect: " << llvm::toString(generated.takeError()) << '\n';
        return 1;
    }
    return *generated;
}
