#include "memory/fatal.hpp"
#include <llvm/Support/ErrorHandling.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/thread.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace clang;

namespace {

llvm::cl::OptionCategory category("atelier_style");

llvm::cl::opt<bool> fix_option("fix", llvm::cl::desc("Rewrite fixable findings in place"), llvm::cl::cat(category));

llvm::cl::list<std::string> rule_option("rule", llvm::cl::desc("declarations or blank-lines (default: both)"),
                                        llvm::cl::cat(category));

llvm::cl::opt<unsigned> jobs_option("j", llvm::cl::desc("Parallel translation units"), llvm::cl::init(0),
                                    llvm::cl::cat(category));

struct Finding {
    std::string path;

    unsigned line{};

    unsigned column{};

    std::string rule;

    std::string message;

    bool fixable{};

    auto operator<=>(const Finding&) const = default;
};

struct Edit {
    unsigned offset{};

    unsigned length{};

    std::string text;

    auto operator<=>(const Edit&) const = default;
};

struct Report {
    std::vector<Finding> findings;

    std::vector<std::pair<std::string, Edit>> edits;
};

struct Settings {
    std::set<std::string> checked;

    bool fix{};

    bool declarations{};

    bool blank_lines{};
};

std::string real_path(llvm::StringRef path) {
    llvm::SmallString<260> result;
    if (llvm::sys::fs::real_path(path, result))
        return path.str();
    return std::string(result.str());
}

std::string key_of(llvm::StringRef path) {
    auto key = real_path(path);
    for (auto& c : key)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

// `*` stays within one path segment; `**` crosses segments, as in .clang-format-ignore.
bool glob(llvm::StringRef pattern, llvm::StringRef path) {
    if (pattern.empty())
        return path.empty();
    if (pattern.starts_with("**")) {
        auto rest = pattern.drop_front(2);
        if (rest.starts_with("/") && glob(rest.drop_front(), path))
            return true;
        for (size_t i = 0; i <= path.size(); ++i)
            if (glob(rest, path.drop_front(i)))
                return true;
        return false;
    }
    if (pattern.front() == '*') {
        for (size_t i = 0; i <= path.size(); ++i) {
            if (glob(pattern.drop_front(), path.drop_front(i)))
                return true;
            if (i < path.size() && path[i] == '/')
                break;
        }
        return false;
    }
    if (path.empty() || (pattern.front() == '?' ? path.front() == '/' : pattern.front() != path.front()))
        return false;
    return glob(pattern.drop_front(), path.drop_front());
}

// The formatter's ignore file decides which sources belong to the project style.
bool ignored(const std::string& key) {
    llvm::StringRef directory = llvm::sys::path::parent_path(key, llvm::sys::path::Style::posix);
    while (!directory.empty()) {
        std::ifstream file(directory.str() + "/.clang-format-ignore");
        if (file) {
            const auto relative = llvm::StringRef(key).drop_front(directory.size() + 1);
            bool result = false;
            for (std::string line; std::getline(file, line);) {
                auto pattern = llvm::StringRef(line).trim();
                if (pattern.empty() || pattern.starts_with("#"))
                    continue;
                const bool negated = pattern.consume_front("!");
                if (glob(pattern.ltrim().lower(), relative))
                    result = !negated;
            }
            return result;
        }
        const auto parent = llvm::sys::path::parent_path(directory, llvm::sys::path::Style::posix);
        if (parent == directory)
            break;
        directory = parent;
    }
    return false;
}

// Accepts declaration specifiers shared by every declarator: no pointer, reference,
// array or declarator parentheses outside template arguments and alignas/decltype.
bool shared_specifier(llvm::StringRef specifier) {
    int angle = 0;
    int paren = 0;
    for (size_t i = 0; i < specifier.size(); ++i) {
        const char c = specifier[i];
        if (paren > 0) {
            paren += c == '(' ? 1 : c == ')' ? -1 : 0;
        } else if (c == '<') {
            ++angle;
        } else if (c == '>') {
            --angle;
        } else if (c == '(' && angle == 0) {
            const auto before = specifier.take_front(i).rtrim();
            if (!before.ends_with("alignas") && !before.ends_with("decltype") && !before.ends_with("__declspec"))
                return false;
            paren = 1;
        } else if (angle == 0 && llvm::StringRef("*&^[)").contains(c)) {
            return false;
        }
    }
    return angle == 0 && paren == 0;
}

bool implicit_instantiation(const Decl* declaration) {
    if (const auto* record = dyn_cast<ClassTemplateSpecializationDecl>(declaration))
        return !record->isExplicitInstantiationOrSpecialization();
    if (const auto* function = dyn_cast<FunctionDecl>(declaration))
        return function->getTemplateSpecializationKind() == TSK_ImplicitInstantiation;
    if (const auto* variable = dyn_cast<VarDecl>(declaration))
        return variable->getTemplateSpecializationKind() == TSK_ImplicitInstantiation;
    return false;
}

class Visitor final : public RecursiveASTVisitor<Visitor> {
public:
    Visitor(ASTContext& ast, const Settings& settings, Report& report)
        : ast_(ast), sources_(ast.getSourceManager()), settings_(settings), report_(report) {}

    bool TraverseDecl(Decl* declaration) {
        if (declaration && !isa<TranslationUnitDecl>(declaration) && !owned(declaration->getBeginLoc()))
            return true;
        return RecursiveASTVisitor::TraverseDecl(declaration);
    }

    bool VisitTranslationUnitDecl(TranslationUnitDecl* scope) {
        check_scope(scope);
        return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl* scope) {
        check_scope(scope);
        return true;
    }

    bool VisitLinkageSpecDecl(LinkageSpecDecl* scope) {
        check_scope(scope);
        return true;
    }

    bool VisitCXXRecordDecl(CXXRecordDecl* record) {
        if (record->isThisDeclarationADefinition() && !record->isLambda() && !record->isImplicit())
            check_scope(record);
        return true;
    }

    // Initialisers of for/if/switch keep several variables: splitting them widens their scope.
    bool VisitCompoundStmt(CompoundStmt* block) {
        if (!settings_.declarations)
            return true;
        for (auto* statement : block->body()) {
            const auto* declaration = dyn_cast<DeclStmt>(statement);
            if (!declaration || declaration->isSingleDecl() || declaration->getBeginLoc().isMacroID() ||
                !owned(declaration->getBeginLoc()))
                continue;
            std::vector<const DeclaratorDecl*> variables;
            bool only_variables = true;
            for (const auto* item : declaration->decls()) {
                if (const auto* variable = dyn_cast<VarDecl>(item))
                    variables.push_back(variable);
                else
                    only_variables = false;
            }
            if (only_variables)
                multiple_variables(variables);
        }
        return true;
    }

private:
    struct Unit {
        SourceLocation begin;

        SourceLocation end;

        std::vector<const DeclaratorDecl*> variables;

        bool tag{};

        bool macro{};

        bool barrier{};
    };

    bool owned(SourceLocation location) {
        const auto file = sources_.getFileID(sources_.getExpansionLoc(location));
        if (file == sources_.getMainFileID())
            return true;
        const auto found = owned_.find(file);
        if (found != owned_.end())
            return found->second;
        bool result = false;
        if (const auto entry = sources_.getFileEntryRefForID(file)) {
            const auto key = key_of(entry->getName());
            result = llvm::StringRef(key).ends_with(".inl") && settings_.checked.contains(key);
        }
        owned_.emplace(file, result);
        return result;
    }

    std::string path_of(FileID file) {
        const auto entry = sources_.getFileEntryRefForID(file);
        return entry ? real_path(entry->getName()) : std::string{};
    }

    llvm::StringRef line_text(FileID file, unsigned line) {
        const auto buffer = sources_.getBufferData(file);
        const auto text = buffer.drop_front(sources_.getFileOffset(sources_.translateLineCol(file, line, 1)));
        return text.take_until([](char c) { return c == '\n'; });
    }

    llvm::StringRef newline(FileID file) { return sources_.getBufferData(file).contains("\r\n") ? "\r\n" : "\n"; }

    void add(SourceLocation location, llvm::StringRef rule, llvm::StringRef message, std::optional<Edit> edit) {
        const auto expansion = sources_.getExpansionLoc(location);
        const auto path = path_of(sources_.getFileID(expansion));
        report_.findings.push_back({path, sources_.getExpansionLineNumber(expansion),
                                    sources_.getExpansionColumnNumber(expansion), rule.str(), message.str(),
                                    edit.has_value()});
        if (edit && settings_.fix)
            report_.edits.emplace_back(path, std::move(*edit));
    }

    void check_scope(DeclContext* scope) {
        std::vector<Unit> units;
        for (auto* declaration : scope->decls()) {
            if (declaration->isImplicit() || isa<EmptyDecl>(declaration) || implicit_instantiation(declaration))
                continue;
            if (const auto* record = dyn_cast<CXXRecordDecl>(declaration); record && record->isLambda())
                continue;
            const auto range = sources_.getExpansionRange(declaration->getSourceRange());
            Unit unit;
            unit.begin = range.getBegin();
            unit.end = range.getEnd();
            unit.barrier = isa<AccessSpecDecl>(declaration) || unit.begin.isInvalid() || !owned(unit.begin);
            unit.macro = declaration->getBeginLoc().isMacroID();
            if (!unit.barrier && !units.empty() && !units.back().barrier && units.back().begin == unit.begin) {
                if (sources_.isBeforeInTranslationUnit(units.back().end, unit.end))
                    units.back().end = unit.end;
            } else {
                units.push_back(unit);
            }
            auto& current = units.back();
            if (isa<FieldDecl>(declaration) || isa<VarDecl>(declaration))
                current.variables.push_back(cast<DeclaratorDecl>(declaration));
            else if (isa<TagDecl>(declaration))
                current.tag = true;
        }
        for (size_t i = 0; i < units.size(); ++i) {
            const auto& unit = units[i];
            if (settings_.declarations && !unit.barrier && !unit.macro && unit.variables.size() > 1) {
                if (unit.tag)
                    add(unit.begin, "declarations", "declare one variable per declaration", std::nullopt);
                else
                    multiple_variables(unit.variables);
            }
            if (settings_.blank_lines && i > 0)
                separate(units[i - 1], unit);
        }
    }

    void separate(const Unit& previous, const Unit& next) {
        if (previous.barrier || next.barrier)
            return;
        const auto file = sources_.getFileID(previous.end);
        if (file != sources_.getFileID(next.begin) || !sources_.isBeforeInTranslationUnit(previous.end, next.begin))
            return;
        const auto end_line = sources_.getExpansionLineNumber(previous.end);
        const auto next_line = sources_.getExpansionLineNumber(next.begin);
        if (end_line == next_line) {
            const auto separator = newline(file).str() + newline(file).str();
            add(next.begin, "blank-lines", "declarations share a line",
                Edit{sources_.getFileOffset(next.begin), 0, separator});
            return;
        }
        for (auto line = end_line + 1; line < next_line; ++line)
            if (line_text(file, line).trim().empty())
                return;
        // A trailing comment continued on the following lines stays with the previous declaration.
        const auto indent = sources_.getExpansionColumnNumber(previous.begin) - 1;
        auto line = end_line + 1;
        for (; line < next_line; ++line) {
            const auto text = line_text(file, line);
            const auto content = text.ltrim();
            if (!content.starts_with("//") || text.size() - content.size() <= indent)
                break;
        }
        const auto offset = sources_.getFileOffset(sources_.translateLineCol(file, line, 1));
        add(next.begin, "blank-lines", "separate adjacent declarations with one blank line",
            Edit{offset, 0, newline(file).str()});
    }

    void multiple_variables(const std::vector<const DeclaratorDecl*>& variables) {
        add(variables.front()->getBeginLoc(), "declarations", "declare one variable per declaration", split(variables));
    }

    // Repeats the shared declaration specifiers for each declarator; anything
    // that may bind to only one declarator is left for a manual rewrite.
    std::optional<Edit> split(const std::vector<const DeclaratorDecl*>& variables) {
        for (const auto* variable : variables)
            if (variable->getBeginLoc().isMacroID() || variable->getLocation().isMacroID() ||
                variable->getEndLoc().isMacroID())
                return std::nullopt;
        const auto file = sources_.getFileID(variables.front()->getBeginLoc());
        const auto buffer = sources_.getBufferData(file);
        const auto start = sources_.getFileOffset(variables.front()->getBeginLoc());
        auto line_start = start;
        while (line_start > 0 && buffer[line_start - 1] != '\n')
            --line_start;
        const auto indent = buffer.slice(line_start, start);
        if (!indent.trim().empty())
            return std::nullopt;
        // Pointer and reference operators before the first name belong to its declarator.
        const auto specifier =
            buffer.slice(start, sources_.getFileOffset(variables.front()->getLocation())).rtrim(" \t\r\n*&");
        if (specifier.empty() || !shared_specifier(specifier))
            return std::nullopt;
        std::string text;
        unsigned previous_end = start + static_cast<unsigned>(specifier.size());
        for (size_t i = 0; i < variables.size(); ++i) {
            const auto name = sources_.getFileOffset(variables[i]->getLocation());
            const auto end = sources_.getFileOffset(
                Lexer::getLocForEndOfToken(variables[i]->getEndLoc(), 0, sources_, ast_.getLangOpts()));
            if (name < previous_end)
                return std::nullopt;
            auto operators = buffer.slice(previous_end, name).trim();
            if (i > 0) {
                if (!operators.consume_front(","))
                    return std::nullopt;
                operators = operators.trim();
                text += newline(file);
                text += indent;
            }
            if (operators.find_first_not_of("*& \t") != llvm::StringRef::npos)
                return std::nullopt;
            text += specifier;
            text += ' ';
            text += operators;
            text += buffer.slice(name, end);
            text += ';';
            previous_end = end;
        }
        const auto rest = buffer.drop_front(previous_end);
        const auto semicolon = rest.find_first_not_of(" \t\r\n");
        if (semicolon == llvm::StringRef::npos || rest[semicolon] != ';')
            return std::nullopt;
        return Edit{start, static_cast<unsigned>(previous_end + semicolon + 1 - start), text};
    }

    ASTContext& ast_;

    SourceManager& sources_;

    const Settings& settings_;

    Report& report_;

    std::map<FileID, bool> owned_;
};

class Consumer final : public ASTConsumer {
public:
    Consumer(const Settings& settings, Report& report) : settings_(settings), report_(report) {}

    void HandleTranslationUnit(ASTContext& ast) override {
        if (ast.getDiagnostics().hasErrorOccurred())
            return;
        Visitor(ast, settings_, report_).TraverseAST(ast);
    }

private:
    const Settings& settings_;

    Report& report_;
};

class Action final : public ASTFrontendAction {
public:
    Action(const Settings& settings, Report& report) : settings_(settings), report_(report) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<Consumer>(settings_, report_);
    }

private:
    const Settings& settings_;

    Report& report_;
};

class Factory final : public tooling::FrontendActionFactory {
public:
    Factory(const Settings& settings, Report& report) : settings_(settings), report_(report) {}

    std::unique_ptr<FrontendAction> create() override { return std::make_unique<Action>(settings_, report_); }

private:
    const Settings& settings_;

    Report& report_;
};

// A source built into several targets is checked once.
class FirstCommand final : public tooling::CompilationDatabase {
public:
    explicit FirstCommand(const tooling::CompilationDatabase& base) : base_(base) {}

    std::vector<tooling::CompileCommand> getCompileCommands(llvm::StringRef path) const override {
        auto commands = base_.getCompileCommands(path);
        if (commands.size() > 1)
            commands.resize(1);
        return commands;
    }

private:
    const tooling::CompilationDatabase& base_;
};

bool apply(const std::string& path, const std::set<Edit>& edits) {
    std::string text;
    {
        // Volatile: a mapped view would keep the file locked while it is rewritten.
        auto buffer = llvm::MemoryBuffer::getFile(path, false, false, true);
        if (!buffer) {
            llvm::errs() << path << ": cannot read\n";
            return false;
        }
        text = (*buffer)->getBuffer().str();
    }
    auto limit = static_cast<unsigned>(text.size());
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        if (edit->offset + edit->length > limit) {
            llvm::errs() << path << ": overlapping edits at offset " << edit->offset << '\n';
            return false;
        }
        text.replace(edit->offset, edit->length, edit->text);
        limit = edit->offset;
    }
    std::error_code error;
    llvm::raw_fd_ostream out(path, error, llvm::sys::fs::OF_None);
    if (error) {
        llvm::errs() << path << ": " << error.message() << '\n';
        return false;
    }
    out << text;
    return true;
}

} // namespace

int main(int argc, const char** argv) {
    atelier::install_fatal_handler();
    llvm::install_fatal_error_handler([](void*, const char*, bool) { atelier::fatal("llvm fatal error"); });
    auto parser = tooling::CommonOptionsParser::create(
        argc, argv, category, llvm::cl::OneOrMore,
        "Checks declarations that clang-format cannot normalise: one variable per declaration, and one blank\n"
        "line between adjacent declarations outside function bodies. Files excluded by .clang-format-ignore are\n"
        "skipped; .inl files are checked through the listed translation units that include them.\n");
    if (!parser) {
        llvm::errs() << llvm::toString(parser.takeError()) << '\n';
        return 2;
    }
    Settings settings;
    settings.fix = fix_option;
    for (const auto& rule : rule_option) {
        if (rule == "declarations")
            settings.declarations = true;
        else if (rule == "blank-lines")
            settings.blank_lines = true;
        else {
            llvm::errs() << "unknown rule: " << rule << '\n';
            return 2;
        }
    }
    if (rule_option.empty())
        settings.declarations = settings.blank_lines = true;

    std::vector<std::string> units;
    for (const auto& path : parser->getSourcePathList()) {
        const auto key = key_of(path);
        if (ignored(key) || !settings.checked.insert(key).second)
            continue;
        if (!llvm::StringRef(key).ends_with(".inl"))
            units.push_back(real_path(path));
    }

    std::vector<Report> reports(units.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    const FirstCommand compilations(parser->getCompilations());
    const auto work = [&] {
        for (size_t i; (i = next++) < units.size();) {
            tooling::ClangTool tool(compilations, {units[i]});
            tool.appendArgumentsAdjuster(tooling::getInsertArgumentAdjuster(
                {"-resource-dir", ATELIER_STYLE_RESOURCE_DIR, "-w"}, tooling::ArgumentInsertPosition::BEGIN));
            Factory factory(settings, reports[i]);
            if (tool.run(&factory) != 0)
                failed = true;
        }
    };
    const auto jobs =
        std::max(1u, std::min<unsigned>(jobs_option ? jobs_option : std::thread::hardware_concurrency() / 2,
                                        static_cast<unsigned>(units.size())));
    // Parsing and AST traversal run on these threads; the 1 MiB default overflows on deep expressions.
    std::vector<llvm::thread> threads;
    for (unsigned i = 0; i < jobs; ++i)
        threads.emplace_back(std::optional<unsigned>(16u << 20), work);
    for (auto& thread : threads)
        thread.join();

    std::set<Finding> findings;
    std::map<std::string, std::set<Edit>> edits;
    for (auto& report : reports) {
        findings.insert(report.findings.begin(), report.findings.end());
        for (auto& [path, edit] : report.edits)
            edits[path].insert(std::move(edit));
    }
    size_t remaining = 0;
    for (const auto& finding : findings) {
        if (settings.fix && finding.fixable)
            continue;
        ++remaining;
        llvm::outs() << finding.path << ':' << finding.line << ':' << finding.column << ": " << finding.rule << ": "
                     << finding.message << (finding.fixable ? "" : " (manual)") << '\n';
    }
    if (settings.fix) {
        for (const auto& [path, file_edits] : edits)
            if (!apply(path, file_edits))
                failed = true;
        llvm::errs() << "atelier_style: fixed " << findings.size() - remaining << " findings in " << edits.size()
                     << " files\n";
    }
    llvm::errs() << "atelier_style: " << units.size() << " translation units, " << remaining << " findings\n";
    return failed || remaining ? 1 : 0;
}
