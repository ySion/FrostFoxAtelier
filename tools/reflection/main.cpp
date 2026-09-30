#include <clang/AST/ASTConsumer.h>
#include <clang/AST/DeclCXX.h>
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
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
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
std::string required(const llvm::json::Object& object, llvm::StringRef name) {
    const auto value = object.getString(name);
    if (!value || value->empty()) throw std::runtime_error("Missing configuration string: " + name.str());
    return value->str();
}
struct Header { std::string path, include, output; bool included{}; };
struct Config {
    std::string source, database, cpp, depfile, resource_dir;
    std::vector<Header> headers;
};
Config read_config(const char* path) {
    auto file = llvm::MemoryBuffer::getFile(path);
    if (!file) throw std::runtime_error(file.getError().message());
    auto json = llvm::json::parse((*file)->getBuffer());
    if (!json) throw std::runtime_error(llvm::toString(json.takeError()));
    const auto* object = json->getAsObject();
    if (!object) throw std::runtime_error("Reflection configuration must be an object.");
    Config result{required(*object, "source"), required(*object, "database"), required(*object, "cpp"),
        required(*object, "depfile"), required(*object, "resource_dir"), {}};
    const auto* headers = object->getArray("headers");
    if (!headers || headers->empty()) throw std::runtime_error("No reflection headers configured.");
    std::set<std::string> paths, outputs;
    for (const auto& item : *headers) {
        const auto* header = item.getAsObject();
        if (!header) throw std::runtime_error("Invalid reflection header configuration.");
        Header value{required(*header, "path"), required(*header, "include"), required(*header, "output")};
        if (!paths.insert(key(value.path)).second || !outputs.insert(key(value.output)).second)
            throw std::runtime_error("Duplicate reflection header or output.");
        result.headers.push_back(std::move(value));
    }
    return result;
}

enum class MarkerKind { object, record, enumeration, field, method, body };
struct Marker {
    MarkerKind kind;
    SourceLocation location;
    std::size_t header;
    unsigned next_offset;
    std::vector<Token> arguments;
    bool used{};
};
using Options = std::map<std::string, std::string>;
struct Field { std::string cpp_name, name, form, target; bool hidden{}; unsigned line{}; bool direct_layout{}; };
struct Method { std::string cpp_name, name, pointer; unsigned line{}; bool preserves_borrows{}; };
struct Type {
    const CXXRecordDecl* declaration{};
    std::size_t header{};
    unsigned line{};
    bool object{}, class_tag{};
    std::string qualified, scope, name, runtime_name, base;
    std::vector<Field> fields;
    std::vector<Method> methods;
    const EnumDecl* enumeration{};
    std::string underlying{};
    std::vector<std::string> enumerators{};
};
struct State {
    Config& config;
    CompilerInstance* compiler{};
    std::vector<Marker> markers;
    std::vector<Type> types;
    std::set<std::string> dependencies;
    bool failed{};

    explicit State(Config& configuration) : config(configuration) {}

    void error(SourceLocation location, const std::string& message) {
        failed = true;
        auto& diagnostics = compiler->getDiagnostics();
        diagnostics.Report(location, diagnostics.getCustomDiagID(DiagnosticsEngine::Error, "%0")) << message;
    }
    std::optional<std::size_t> header_of(SourceLocation location) const {
        auto& sources = compiler->getSourceManager();
        if (location.isInvalid()) return {};
        const auto path = key(sources.getFilename(sources.getExpansionLoc(location)).str());
        for (std::size_t i = 0; i < config.headers.size(); ++i)
            if (path == key(config.headers[i].path)) return i;
        return {};
    }
    Marker* prefix(const Decl& declaration, MarkerKind kind) {
        const auto location = declaration.getBeginLoc();
        const auto header = header_of(location);
        if (!header) return nullptr;
        auto& sources = compiler->getSourceManager();
        const auto offset = sources.getFileOffset(sources.getExpansionLoc(location));
        for (auto& marker : markers)
            if (marker.header == *header && marker.next_offset == offset && marker.kind == kind) {
                if (marker.used) {
                    error(location, "Each reflection marker must describe one declaration; split grouped declarations.");
                    return nullptr;
                }
                marker.used = true;
                return &marker;
            }
        return nullptr;
    }
    std::optional<Options> options(const Marker& marker, std::initializer_list<llvm::StringRef> allowed) {
        Options result;
        const auto& tokens = marker.arguments;
        std::size_t i = 0;
        while (i < tokens.size()) {
            if (!tokens[i].is(tok::identifier)) { error(tokens[i].getLocation(), "Expected a reflection option name."); return {}; }
            const auto name = tokens[i++].getIdentifierInfo()->getName().str();
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() || result.contains(name)) {
                error(marker.location, "Unknown or duplicate reflection option: " + name); return {};
            }
            std::string value;
            if (i < tokens.size() && tokens[i].is(tok::equal)) {
                ++i;
                if (i == tokens.size() || !tokens[i].is(tok::string_literal)) {
                    error(marker.location, "Reflection option " + name + " requires an ordinary string literal."); return {};
                }
                StringLiteralParser literal(llvm::ArrayRef<Token>(&tokens[i++], 1), compiler->getPreprocessor());
                if (literal.hadError) { failed = true; return {}; }
                value = literal.GetString().str();
                if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32; })) {
                    error(marker.location, "Reflection option strings must be nonempty and contain no control characters."); return {};
                }
            } else if (name != "Hidden" && name != "BorrowSafe") {
                error(marker.location, "Reflection option " + name + " requires a value."); return {};
            }
            if ((name == "Hidden" || name == "BorrowSafe") && !value.empty()) { error(marker.location, name + " is a flag, without a value."); return {}; }
            result.emplace(name, std::move(value));
            if (i < tokens.size()) {
                if (!tokens[i++].is(tok::comma) || i == tokens.size()) {
                    error(marker.location, "Expected another comma-separated reflection option."); return {};
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
        if (std::isspace(static_cast<unsigned char>(buffer[i]))) { ++i; continue; }
        if (buffer.substr(i).starts_with("//")) {
            const auto end = buffer.find('\n', i); i = end == llvm::StringRef::npos ? buffer.size() : end + 1; continue;
        }
        if (buffer.substr(i).starts_with("/*")) {
            const auto end = buffer.find("*/", i + 2); i = end == llvm::StringRef::npos ? buffer.size() : end + 2; continue;
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
        if (!header) return;
        const auto spelling = name.getIdentifierInfo()->getName();
        std::optional<MarkerKind> kind;
        if (spelling == "ACLASS") kind = MarkerKind::object;
        else if (spelling == "ASTRUCT") kind = MarkerKind::record;
        else if (spelling == "AENUM") kind = MarkerKind::enumeration;
        else if (spelling == "AFIELD") kind = MarkerKind::field;
        else if (spelling == "AFUNCTION") kind = MarkerKind::method;
        else if (spelling == "AGENERATED_BODY") kind = MarkerKind::body;
        if (!kind) return;
        if (range.getBegin().isMacroID()) { state_.error(range.getBegin(), "Reflection markers must be written directly at the declaration."); return; }
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
        if (reason != EnterFile || location.isInvalid()) return;
        auto& sources = state_.compiler->getSourceManager();
        const auto file = sources.getFileEntryRefForID(sources.getFileID(location));
        if (!file) return;
        const auto path = file->getName().str();
        for (const auto& header : state_.config.headers) if (key(path) == key(header.output)) return;
        state_.dependencies.insert(path);
    }
    void InclusionDirective(SourceLocation location, const Token&, llvm::StringRef, bool, CharSourceRange,
        OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef, const Module*, bool, SrcMgr::CharacteristicKind) override {
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
        if (declaration && !isa<TranslationUnitDecl>(declaration) && !state_.header_of(declaration->getLocation())) return true;
        return RecursiveASTVisitor::TraverseDecl(declaration);
    }
    bool TraverseStmt(Stmt*) { return true; }
    bool VisitEnumDecl(EnumDecl* declaration) {
        auto* marker = state_.prefix(*declaration, MarkerKind::enumeration);
        if (!marker) return true;
        const auto options = state_.options(*marker, {"Name"});
        if (!options) return true;
        const auto* definition = declaration->getDefinition();
        if (!definition || !definition->isScoped() || definition->getName().empty() ||
            (!isa<NamespaceDecl>(definition->getDeclContext()) && !definition->getDeclContext()->isTranslationUnit())) {
            state_.error(marker->location, "AENUM requires a complete namespace-level enum class."); return true;
        }
        for (auto* context = definition->getDeclContext(); !context->isTranslationUnit(); context = context->getParent())
            if (const auto* scope = dyn_cast<NamespaceDecl>(context); scope && scope->isAnonymousNamespace()) {
                state_.error(marker->location, "Reflected enums cannot have anonymous namespace linkage."); return true;
            }
        const auto underlying = definition->getIntegerType().getCanonicalType();
        if (state_.compiler->getASTContext().getTypeSize(underlying) != 32 ||
            (!underlying->isSpecificBuiltinType(BuiltinType::Int) && !underlying->isSpecificBuiltinType(BuiltinType::UInt))) {
            state_.error(marker->location, "AENUM requires an int32_t or uint32_t underlying type."); return true;
        }
        const auto qualified = definition->getQualifiedNameAsString();
        std::string runtime_name = qualified;
        for (auto at = runtime_name.find("::"); at != std::string::npos; at = runtime_name.find("::")) runtime_name.replace(at, 2, ".");
        if (options->contains("Name")) runtime_name = options->at("Name");
        for (const auto& type : state_.types) {
            if (type.enumeration == definition) {
                if (type.runtime_name != runtime_name) state_.error(marker->location, "Conflicting enum names.");
                return true;
            }
            if (type.runtime_name == runtime_name) { state_.error(marker->location, "Duplicate reflected type name: " + runtime_name); return true; }
        }
        const auto owner = state_.header_of(definition->getLocation());
        if (!owner) { state_.error(marker->location, "Enum definition must belong to a configured header."); return true; }
        Type type;
        type.header = *owner;
        type.line = state_.compiler->getSourceManager().getExpansionLineNumber(definition->getLocation());
        type.enumeration = definition;
        type.qualified = "::" + qualified;
        const auto scope = qualified.rfind("::");
        type.scope = scope == std::string::npos ? "" : qualified.substr(0, scope);
        type.name = definition->getNameAsString();
        type.runtime_name = std::move(runtime_name);
        type.underlying = underlying->isSignedIntegerType() ? "::std::int32_t" : "::std::uint32_t";
        for (const auto* item : definition->enumerators()) type.enumerators.push_back(item->getNameAsString());
        state_.types.push_back(std::move(type));
        return true;
    }
    bool VisitCXXRecordDecl(CXXRecordDecl* declaration) {
        if (declaration->isImplicit()) return true;
        auto* marker = state_.prefix(*declaration, MarkerKind::object);
        const bool object = marker != nullptr;
        if (!marker) marker = state_.prefix(*declaration, MarkerKind::record);
        if (!marker) return true;
        const auto options = state_.options(*marker, {"Name"});
        if (!options) return true;
        auto* definition = declaration->getDefinition();
        if (!definition || definition->isDependentType() || definition->isUnion() || definition->isLocalClass() ||
            definition->getName().empty() || isa<CXXRecordDecl>(definition->getDeclContext())) {
            state_.error(marker->location, "Reflected types require a complete, named, non-template namespace-level class or struct."); return true;
        }
        const auto owner = state_.header_of(definition->getLocation());
        if (!owner) { state_.error(marker->location, "Reflected definition must belong to a configured header."); return true; }
        for (auto* context = definition->getDeclContext(); !context->isTranslationUnit(); context = context->getParent())
            if (const auto* scope = dyn_cast<NamespaceDecl>(context); scope && scope->isAnonymousNamespace()) {
                state_.error(marker->location, "Reflected types cannot have anonymous namespace linkage."); return true;
            }
        const auto qualified = definition->getQualifiedNameAsString();
        std::string runtime_name = qualified;
        for (auto pos = runtime_name.find("::"); pos != std::string::npos; pos = runtime_name.find("::")) runtime_name.replace(pos, 2, ".");
        if (const auto name = options->find("Name"); name != options->end()) runtime_name = name->second;
        for (const auto& type : state_.types) {
            if (type.declaration == definition) {
                if (type.object != object || type.runtime_name != runtime_name)
                    state_.error(marker->location, "Conflicting reflection markers on redeclarations.");
                return true;
            }
            if (type.runtime_name == runtime_name) { state_.error(marker->location, "Duplicate reflected type name: " + runtime_name); return true; }
        }
        const auto scope_end = qualified.rfind("::");
        state_.types.push_back({definition, *owner,
            state_.compiler->getSourceManager().getExpansionLineNumber(definition->getLocation()), object, definition->isClass(),
            "::" + qualified, scope_end == std::string::npos ? "" : qualified.substr(0, scope_end), definition->getNameAsString(),
            std::move(runtime_name), {}, {}, {}});
        return true;
    }
};

const Type* reflected(State& state, const CXXRecordDecl* record) {
    if (!record) return nullptr;
    const auto* definition = record->getDefinition();
    for (const auto& type : state.types) if (type.declaration == definition) return &type;
    return nullptr;
}
std::optional<std::string> scalar(QualType type, State& state, ASTContext& context, bool allow_void = false) {
    if (type.hasLocalQualifiers()) return {};
    type = type.getCanonicalType();
    if (allow_void && type->isVoidType()) return "void";
    if (const auto* enumeration = type->getAs<EnumType>())
        for (const auto& reflected_type : state.types)
            if (reflected_type.enumeration == enumeration->getDecl()->getDefinition()) return reflected_type.qualified;
    if (type->isSpecificBuiltinType(BuiltinType::Int) && context.getTypeSize(type) == 32) return "::std::int32_t";
    if (type->isSpecificBuiltinType(BuiltinType::Float)) return "float";
    if (type->isSpecificBuiltinType(BuiltinType::UInt) && context.getTypeSize(type) == 32) return "::std::uint32_t";
    return {};
}

const ClassTemplateSpecializationDecl* array_specialization(QualType type) {
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);
    return specialization && specialization->getSpecializedTemplate()->getQualifiedNameAsString() == "atelier::Array"
        ? specialization : nullptr;
}

bool contains_reference_storage(QualType type, ASTContext& context, std::set<const CXXRecordDecl*>& visited) {
    type = type.getCanonicalType();
    if (const auto* array = context.getAsArrayType(type)) return contains_reference_storage(array->getElementType(), context, visited);
    const auto* record = type->getAsCXXRecordDecl();
    if (!record) return false;
    const auto name = record->getQualifiedNameAsString();
    if (name == "atelier::ReferenceSlot" || name == "atelier::ReferenceSequence" || name == "atelier::ObjectReference") return true;
    record = record->getDefinition();
    if (!record || !visited.insert(record).second) return false;
    if (const auto* array = array_specialization(type))
        return contains_reference_storage(array->getTemplateArgs()[0].getAsType(), context, visited);
    for (const auto* field : record->fields()) if (contains_reference_storage(field->getType(), context, visited)) return true;
    for (const auto& base : record->bases()) if (contains_reference_storage(base.getType(), context, visited)) return true;
    return false;
}
bool contains_reference_storage(QualType type, ASTContext& context) {
    std::set<const CXXRecordDecl*> visited;
    return contains_reference_storage(type, context, visited);
}

bool value_field(QualType type, State& state, ASTContext& context) {
    if (type.hasLocalQualifiers()) return false;
    if (scalar(type, state, context)) return true;
    const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
    if (!record) return false;
    if (record->getQualifiedNameAsString() == "atelier::String") return true;
    if (const auto* array = array_specialization(type))
        return !contains_reference_storage(type, context) && value_field(array->getTemplateArgs()[0].getAsType(), state, context);
    const auto* nested = reflected(state, record);
    return nested && !nested->object;
}

void members(State& state, Type& type, ASTContext& context) {
    if (type.enumeration) return;
    auto& sources = state.compiler->getSourceManager();
    const auto* record = type.declaration;
    if (record->getNumBases() > (type.object ? 1u : 0u)) {
        state.error(record->getLocation(), "This reflection slice supports one object base and no record inheritance."); return;
    }
    for (const auto& base : record->bases()) {
        const auto* parent = reflected(state, base.getType()->getAsCXXRecordDecl());
        if (!parent || !parent->object || base.isVirtual() || base.getAccessSpecifier() != AS_public) {
            state.error(base.getBeginLoc(), "A reflected base must be a public, non-virtual reflected object in this generation group."); return;
        }
        type.base = parent->qualified;
    }
    unsigned bodies = 0;
    const auto begin = sources.getFileOffset(record->getBeginLoc());
    const auto end = sources.getFileOffset(record->getEndLoc());
    for (auto& marker : state.markers) {
        if (marker.kind != MarkerKind::body || marker.header != type.header) continue;
        const auto offset = sources.getFileOffset(marker.location);
        if (offset <= begin || offset >= end) continue;
        marker.used = true; ++bodies;
        if (marker.arguments.size() != 1 || !marker.arguments.front().is(tok::identifier) ||
            marker.arguments.front().getIdentifierInfo()->getName() != type.name)
            state.error(marker.location, "AGENERATED_BODY must name its containing type.");
    }
    if (bodies != 1) state.error(record->getLocation(), "Each reflected type requires exactly one AGENERATED_BODY(TypeName).");
    std::set<std::string> names;
    for (const auto* field : record->fields()) {
        auto* marker = state.prefix(*field, MarkerKind::field);
        const auto canonical = field->getType().getCanonicalType();
        const auto* field_record = canonical->getAsCXXRecordDecl();
        const auto record_name = field_record ? field_record->getQualifiedNameAsString() : "";
        const bool slot = record_name == "atelier::ReferenceSlot", sequence = record_name == "atelier::ReferenceSequence";
        if (!marker) {
            if (contains_reference_storage(field->getType(), context))
                state.error(field->getLocation(), "Runtime reference storage, including inline records and arrays, requires an explicit AFIELD marker.");
            continue;
        }
        const auto options = state.options(*marker, {"Name", "Hidden", "Strong", "Weak", "Sequence"});
        if (!options) continue;
        if (field->isBitField() || field->isAnonymousStructOrUnion() || field->getType().isConstQualified() || field->getType().isVolatileQualified()) {
            state.error(field->getLocation(), "AFIELD requires a named, mutable, addressable data member."); continue;
        }
        Field result{field->getNameAsString(), field->getNameAsString(), {}, {}, options->contains("Hidden"), sources.getExpansionLineNumber(field->getLocation())};
        const auto alignment = context.getTypeAlign(field->getType());
        const auto& layout = context.getASTRecordLayout(record);
        result.direct_layout = record->isStandardLayout() &&
            layout.getAlignment().getQuantity() * context.getCharWidth() >= alignment &&
            layout.getFieldOffset(field->getFieldIndex()) % alignment == 0;
        if (options->contains("Name")) result.name = options->at("Name");
        unsigned forms = 0;
        for (const auto* form : {"Strong", "Weak", "Sequence"}) if (options->contains(form)) {
            ++forms; result.target = options->at(form);
            result.form = std::string(form) == "Strong" ? "strong_reference" : std::string(form) == "Weak" ? "weak_reference" : "strong_sequence";
        }
        if ((slot && (forms != 1 || result.form == "strong_sequence")) ||
            (sequence && (forms != 1 || result.form != "strong_sequence" || !type.object)) || (!slot && !sequence && forms != 0)) {
            state.error(field->getLocation(), "Reference storage requires exactly its matching Strong, Weak or Sequence target; sequences belong to objects."); continue;
        }
        if (slot || sequence) {
            const auto target = std::find_if(state.types.begin(), state.types.end(), [&](const Type& candidate) { return candidate.runtime_name == result.target && candidate.object; });
            if (target == state.types.end() && result.target != "core.Object") {
                state.error(field->getLocation(), "Reference target must name a reflected object in this group or core.Object: " + result.target); continue;
            }
        } else {
            if (!value_field(field->getType(), state, context)) {
                state.error(field->getLocation(), "AFIELD requires a supported scalar/enum/record, String, reference-free Array<T>, or explicit Runtime reference storage. Borrowed views cannot be persistent fields."); continue;
            }
        }
        if (!names.insert(result.name).second) state.error(field->getLocation(), "Duplicate reflected member name: " + result.name);
        type.fields.push_back(std::move(result));
    }
    for (const auto* method : record->methods()) {
        auto* marker = state.prefix(*method, MarkerKind::method);
        if (!marker) continue;
        const auto options = state.options(*marker, {"Name", "BorrowSafe"});
        if (!options) continue;
        const auto result = scalar(method->getReturnType(), state, context, true);
        if (!result || method->isStatic() || method->isVariadic() || method->isDeleted() || method->isVolatile() ||
            method->getRefQualifier() != RQ_None || method->getDescribedFunctionTemplate() ||
            isa<CXXConstructorDecl>(method) || isa<CXXDestructorDecl>(method) || method->isOverloadedOperator() || isa<CXXConversionDecl>(method)) {
            state.error(method->getLocation(), "AFUNCTION requires a supported non-static member with a scalar or void result."); continue;
        }
        Method value{method->getNameAsString(), method->getNameAsString(), *result + " (Self::*)(", sources.getExpansionLineNumber(method->getLocation())};
        if (options->contains("Name")) value.name = options->at("Name");
        value.preserves_borrows = options->contains("BorrowSafe");
        bool valid = true;
        for (unsigned i = 0; i < method->getNumParams(); ++i) {
            auto parameter_type = method->getParamDecl(i)->getType();
            const bool reference = parameter_type->isLValueReferenceType();
            if (reference) parameter_type = parameter_type->getPointeeType();
            const bool read_only = parameter_type.isConstQualified();
            auto parameter = parameter_type.isVolatileQualified() ? std::optional<std::string>{} :
                scalar(parameter_type.getUnqualifiedType(), state, context);
            if (!parameter && reference && !parameter_type.isVolatileQualified() &&
                value_field(parameter_type.getUnqualifiedType(), state, context) && !contains_reference_storage(parameter_type, context))
                parameter = TypeName::getFullyQualifiedName(parameter_type.getCanonicalType().getUnqualifiedType(),
                    context, context.getPrintingPolicy(), true);
            if (parameter && reference) *parameter = (read_only ? "const " : "") + *parameter + "&";
            if (!parameter) { state.error(method->getParamDecl(i)->getLocation(), "Reflected parameters require scalar/enum values or supported native values passed as const T& or T&."); valid = false; break; }
            if (i) value.pointer += ", ";
            value.pointer += *parameter;
        }
        if (!valid) continue;
        value.pointer += ")";
        if (method->isConst()) value.pointer += " const";
        if (method->getType()->castAs<FunctionProtoType>()->isNothrow()) value.pointer += " noexcept";
        if (!names.insert(value.name).second) state.error(method->getLocation(), "Reflected overloads require distinct Name aliases: " + value.name);
        type.methods.push_back(std::move(value));
    }
}

class Consumer final : public ASTConsumer {
    State& state_;
public:
    explicit Consumer(State& state) : state_(state) {}
    void HandleTranslationUnit(ASTContext& context) override {
        if (state_.compiler->getDiagnostics().hasErrorOccurred()) return;
        Collector(state_).TraverseDecl(context.getTranslationUnitDecl());
        for (auto& type : state_.types) members(state_, type, context);
        for (const auto& type : state_.types) {
            std::set<std::string> names;
            for (const auto& field : type.fields) names.insert(field.name);
            for (const auto& method : type.methods) names.insert(method.name);
            auto base = type.base;
            while (!base.empty()) {
                const auto parent = std::find_if(state_.types.begin(), state_.types.end(), [&](const Type& candidate) { return candidate.qualified == base; });
                if (parent == state_.types.end()) break;
                for (const auto& field : parent->fields) if (!names.insert(field.name).second)
                    state_.error(type.declaration->getLocation(), "Inherited reflected member name cannot be shadowed: " + field.name);
                for (const auto& method : parent->methods) if (!names.insert(method.name).second)
                    state_.error(type.declaration->getLocation(), "Inherited reflected member name cannot be shadowed: " + method.name);
                base = parent->base;
            }
        }
        for (const auto& marker : state_.markers) if (!marker.used)
            state_.error(marker.location, "Reflection marker does not immediately precede a supported declaration in a reflected type.");
        for (const auto& header : state_.config.headers) if (!header.included)
            state_.error({}, "Reflected header must directly include its generated header: " + header.include);
        if (state_.types.empty()) state_.error({}, "No reflected type definitions found in the configured headers.");
    }
};
class Action final : public ASTFrontendAction {
    State& state_;
public:
    explicit Action(State& state) : state_(state) {}
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, llvm::StringRef) override {
        state_.compiler = &compiler;
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

void write_changed(const std::string& path, const std::string& text) {
    const auto current = llvm::MemoryBuffer::getFile(path);
    if (current && (*current)->getBuffer() == text) return;
    if (const auto error = llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path))) throw std::runtime_error(error.message());
    const auto temporary = path + ".tmp";
    std::error_code error;
    { llvm::raw_fd_ostream file(temporary, error); if (error) throw std::runtime_error(error.message()); file << text; file.flush(); if (file.has_error()) throw std::runtime_error("Cannot write generated file: " + path); }
    if (const auto renamed = llvm::sys::fs::rename(temporary, path)) throw std::runtime_error(renamed.message());
}
std::string dependency_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string result;
    for (const auto c : path) {
        if (c == '$') result += '$';
        else if (c == ' ' || c == '#' || c == ':') result += '\\';
        result += c;
    }
    return result;
}
void emit(const State& state) {
    std::vector<std::string> headers(state.config.headers.size(), "// Generated by atelier_reflect. Do not edit.\n#pragma once\n#include \"runtime/reflection.hpp\"\n\n");
    std::string cpp = "// Generated by atelier_reflect. Do not edit.\n#include \"runtime/native.hpp\"\n";
    for (const auto& header : state.config.headers) cpp += "#include " + quote(header.include) + "\n";
    cpp += "\nnamespace atelier::reflection {\n";
    for (const auto& type : state.types) {
        auto& header = headers[type.header];
        if (!type.scope.empty()) header += "namespace " + type.scope + " { ";
        if (type.enumeration) header += "enum class " + type.name + " : " + type.underlying + ";";
        else header += std::string(type.class_tag ? "class " : "struct ") + type.name + ";";
        if (!type.scope.empty()) header += " }";
        header += "\nnamespace atelier::reflection {\ntemplate<> TypeDefinition Definition<" + type.qualified + ">::build(Runtime&);\n}\n";
        const auto location = [&](unsigned line) { return "\n#line " + std::to_string(line) + " " + quote(state.config.headers[type.header].include) + "\n"; };
        cpp += location(type.line) + "template<> TypeDefinition Definition<" + type.qualified + ">::build(Runtime& runtime) {\n    using Self = " + type.qualified + ";\n";
        cpp += "    return NativeRegistration<Self>{runtime, " + quote(type.runtime_name) + ", TypeKind::" + (type.enumeration ? "enumeration" : type.object ? "object" : "record") + "}";
        for (const auto& name : type.enumerators)
            cpp += "\n        .enumerator(" + quote(name) + ", Self::" + name + ")";
        if (!type.base.empty()) cpp += "\n        .base<" + type.base + ">()";
        for (const auto& field : type.fields) {
            cpp += location(field.line) + "        ." + (field.hidden ? "hidden_field" : "field") + "<&Self::" + field.cpp_name + ">(" + quote(field.name);
            if (!field.form.empty()) cpp += ", {TypeReference{" + quote(field.target) + "}, ValueForm::" + field.form + "}";
            else if (!field.hidden && field.direct_layout)
                cpp += ", {TypeReference::native<decltype(Self::" + field.cpp_name + ")>()}";
            if (!field.hidden && field.direct_layout)
                cpp += ", offsetof(Self, " + field.cpp_name + ")";
            cpp += ")";
        }
        for (const auto& method : type.methods)
            cpp += location(method.line) + "        .method<static_cast<" + method.pointer + ">(&Self::" + method.cpp_name + ")>(" + quote(method.name) + (method.preserves_borrows ? ", true)" : ")");
        cpp += "\n        .build();\n}\n";
    }
    cpp += "}\n";
    std::string dependencies = dependency_path(state.config.cpp) + ":";
    for (const auto& path : state.dependencies) dependencies += " \\\n  " + dependency_path(path);
    dependencies += "\n";
    for (std::size_t i = 0; i < headers.size(); ++i) write_changed(state.config.headers[i].output, headers[i]);
    write_changed(state.config.cpp, cpp);
    write_changed(state.config.depfile, dependencies);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: atelier_reflect <reflection.json>");
        auto config = read_config(argv[1]);
        std::string error;
        auto database = clang::tooling::CompilationDatabase::loadFromDirectory(config.database, error);
        if (!database) throw std::runtime_error(error);
        if (database->getCompileCommands(config.source).size() != 1)
            throw std::runtime_error("SOURCE must select exactly one real compile command: " + config.source);
        State state{config};
        clang::tooling::ClangTool tool(*database, {config.source});
        // Metadata producers have a fixed declaration in reflection.hpp, so a
        // real TU can be checked without reading stale generated specializations.
        for (const auto& header : config.headers) tool.mapVirtualFile(header.output, "#pragma once\n");
        tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
            {"-resource-dir", config.resource_dir.c_str()}, clang::tooling::ArgumentInsertPosition::BEGIN));
        Factory factory(state);
        const auto result = tool.run(&factory);
        if (result != 0 || state.failed) return 1;
        emit(state);
        llvm::outs() << "Generated " << state.types.size() << " reflected types.\n";
        return 0;
    } catch (const std::exception& error) {
        llvm::errs() << "atelier_reflect: " << error.what() << '\n';
        return 1;
    }
}
