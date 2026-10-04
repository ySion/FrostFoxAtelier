#include "ui_language/declaration.hpp"
#include "language/lexer.hpp"
#include <algorithm>
#include <charconv>
#include <optional>

namespace atelier::ui_language {
namespace {
using namespace language;
using namespace language::detail;
struct Fragment { std::size_t begin{}, end{}; SourcePosition position; };
struct Evaluation { Fragment source; std::uint32_t node{}, operation{}; std::optional<Property> property; };
struct Initializer { std::uint32_t slot{}; Fragment source; };

class Parser : private Tokens {
public:
    Parser(std::string_view name, std::string_view source) : Tokens(source), source_(source) { result_.source_name=name; }
    Declaration run() {
        // Helpers are ordinary Source functions. They share this component's input
        // accessors, but cannot mutate state; mutation belongs to event blocks.
        const auto helper_begin=token_.position;
        std::size_t braces{};
        while (token_.kind!=Token::Kind::end && (token_.text!="ui" || braces)) {
            if (token_.text=="{") ++braces;
            if (token_.text=="}") { if (!braces) reject(token_.position,"Unexpected closing brace."); --braces; }
            consume();
        }
        const Fragment helpers{helper_begin.offset,token_.position.offset,helper_begin};
        expect("ui"); result_.name=std::string(identifier().text); expect("(");
        if (!accept(")")) do {
            expect("in"); const auto kind=input_kind(true); const auto name=identifier();
            unique(name,result_.inputs);
            result_.inputs.push_back({std::string(name.text),kind});
            if (accept(")")) break;
            expect(",");
        } while (true);
        expect("{");
        while (accept("state")) {
            const auto kind=input_kind(false); const auto name=identifier(); unique(name,result_.states);
            expect("="); const auto initial=fragment(";"); expect(";");
            initializers_.push_back({static_cast<std::uint32_t>(result_.states.size()),initial});
            result_.states.push_back({std::string(name.text),kind});
        }
        while (!accept("}")) node(0,0);
        if (token_.kind!=Token::Kind::end) reject(token_.position,"Only one ui component is supported per module.");
        if (result_.nodes.empty()) reject(token_.position,"A ui component needs at least one node.");
        rewrite(helpers,false);
        append("\nvoid __ui_create() {\n",{});
        for (const auto& init:initializers_) {
            append("self.write_"+suffix(result_.states[init.slot].kind)+"("+std::to_string(init.slot)+", ",init.source.position);
            rewrite(init.source,false); append(");\n",init.source.position);
        }
        for (std::uint32_t i=0;i<result_.nodes.size();++i) {
            const auto& value=result_.nodes[i];
            append("self.create_node("+std::to_string(i)+", "+std::to_string(value.parent)+", "+
                std::to_string(static_cast<unsigned>(value.kind))+");\n",{});
        }
        append("}\n",{});
        for (const auto& evaluation:evaluations_) {
            append("void __ui_op_"+std::to_string(evaluation.operation)+"() {\n",evaluation.source.position);
            if (evaluation.property) {
                const auto property=*evaluation.property;
                const char* sink=property==Property::text || property==Property::classes?"set_text":
                    property==Property::edit?"set_edit":property>=Property::enabled?"set_bool":"set_number";
                append(std::string("self.")+sink+"("+std::to_string(evaluation.node)+", "+
                    std::to_string(static_cast<unsigned>(property))+", ",evaluation.source.position);
                const bool numeric=property>=Property::gap && property<=Property::padding_left;
                if (numeric) append("double(",evaluation.source.position);
                rewrite(evaluation.source,false);
                if (numeric) append(")",evaluation.source.position);
                append(");\n",evaluation.source.position);
            } else { rewrite(evaluation.source,true); append("\n",evaluation.source.position); }
            append("}\n",evaluation.source.position);
        }
        append("void __ui_dispatch(uint operation) {\nif (operation == 0) { __ui_create(); return; }\n",{});
        for (const auto& evaluation:evaluations_)
            append("if (operation == "+std::to_string(evaluation.operation)+") { __ui_op_"+
                std::to_string(evaluation.operation)+"(); return; }\n",evaluation.source.position);
        append("}\n",{});
        return std::move(result_);
    }
private:
    std::string_view source_;
    Declaration result_;
    std::vector<Evaluation> evaluations_;
    std::vector<Initializer> initializers_;

    template<class T> void unique(Token name,const std::vector<T>& items) {
        if (items.size()>=1024 || name.text.starts_with("__ui_") ||
            std::ranges::any_of(items,[&](const auto& item){return item.name==name.text;}))
            reject(name.position,"Duplicate, reserved or excessive UI declaration.");
    }
    InputKind input_kind(bool edit) {
        if (accept("bool")) return InputKind::boolean;
        if (accept("double")) return InputKind::number;
        const auto token=identifier();
        if (token.text=="EditBinding" && edit) return InputKind::edit;
        reject(token.position,"Expected bool, double or an EditBinding input.");
    }
    static std::string suffix(InputKind kind) {
        return kind==InputKind::boolean?"bool":kind==InputKind::number?"number":"edit";
    }
    Fragment fragment(std::string_view terminator) {
        const auto begin=token_.position;
        std::size_t parentheses{},brackets{},braces{};
        while (token_.kind!=Token::Kind::end) {
            if (!parentheses && !brackets && !braces && token_.text==terminator)
                return {begin.offset,token_.position.offset,begin};
            if (token_.text=="(") ++parentheses;
            else if (token_.text==")") { if (!parentheses) reject(token_.position,"Unbalanced expression."); --parentheses; }
            else if (token_.text=="[") ++brackets;
            else if (token_.text=="]") { if (!brackets) reject(token_.position,"Unbalanced expression."); --brackets; }
            else if (token_.text=="{") ++braces;
            else if (token_.text=="}") { if (!braces) reject(token_.position,"Expected expression terminator."); --braces; }
            consume();
        }
        reject(begin,"Unterminated UI expression or handler.");
    }
    void node(std::uint32_t parent,std::size_t depth) {
        if (depth>=64) reject(token_.position,"UI nesting exceeds 64 levels.");
        const auto type=identifier();
        const auto kind=type.text=="Column"?NodeKind::column:type.text=="Row"?NodeKind::row:
            type.text=="Text"?NodeKind::text:type.text=="Foldout"?NodeKind::foldout:NodeKind::number_row;
        if (kind==NodeKind::number_row && type.text!="NumberRow") reject(type.position,"Unknown UI node type.");
        const auto name=identifier(); unique(name,result_.nodes);
        const auto slot=static_cast<std::uint32_t>(result_.nodes.size());
        result_.nodes.push_back({std::string(name.text),kind,parent}); expect("{");
        std::vector<std::string_view> properties;
        bool has_event{};
        while (!accept("}")) {
            if (accept("on")) {
                const auto event=identifier();
                if (event.text!="activate" || kind!=NodeKind::foldout || has_event)
                    reject(event.position,"Expected one activate handler on a Foldout.");
                has_event=true; expect("{"); const auto body=fragment("}"); expect("}");
                const auto operation=static_cast<std::uint32_t>(evaluations_.size()+1);
                evaluations_.push_back({body,slot,operation,{}}); result_.events.push_back({slot,operation});
                continue;
            }
            const auto saved_lexer=lexer_; const auto saved_token=token_;
            const auto property=identifier();
            if (!accept(":")) {
                lexer_=saved_lexer; token_=saved_token;
                if (kind!=NodeKind::column && kind!=NodeKind::row) reject(type.position,"Only containers accept child nodes.");
                node(slot+1,depth+1); continue;
            }
            if (std::ranges::find(properties,property.text)!=properties.end()) reject(property.position,"Duplicate UI property.");
            properties.push_back(property.text);
            constexpr std::string_view names[]{"text","classes","gap","height","width","padding_left","enabled","selected","collapsed","caption","edit"};
            const auto at=std::ranges::find(names,property.text);
            if (at==std::end(names)) reject(property.position,"Unknown UI property.");
            const auto value=static_cast<Property>(at-std::begin(names));
            if (value==Property::edit && kind!=NodeKind::number_row) reject(property.position,"edit requires a NumberRow.");
            if (value==Property::selected && kind!=NodeKind::foldout) reject(property.position,"selected requires a Foldout.");
            if (value==Property::caption && kind!=NodeKind::text) reject(property.position,"caption requires Text.");
            if (value==Property::text && (kind==NodeKind::column || kind==NodeKind::row)) reject(property.position,"Use a Text child to display container text.");
            if (kind==NodeKind::number_row && value>=Property::gap && value<=Property::padding_left)
                reject(property.position,"NumberRow uses the shared parameter layout; put outer layout on its parent.");
            const auto expression=fragment(";"); expect(";");
            const auto operation=static_cast<std::uint32_t>(evaluations_.size()+1);
            evaluations_.push_back({expression,slot,operation,value});
            result_.bindings.push_back({slot,operation,value,property.position});
        }
        if (kind==NodeKind::number_row && std::ranges::find(properties,"edit")==properties.end())
            reject(name.position,"A NumberRow requires a typed edit binding.");
    }
    void append(std::string_view value,SourcePosition origin) {
        const auto begin=result_.generated_source.size(); result_.generated_source+=value;
        if (!value.empty()) result_.mappings.push_back({begin,result_.generated_source.size(),origin});
    }
    void rewrite(Fragment fragment,bool event) {
        const auto position=[&](SourcePosition local) {
            local.offset+=fragment.begin;
            local.column+=local.line==1?fragment.position.column-1:0;
            local.line+=fragment.position.line-1;
            return local;
        };
        try {
            Tokens tokens(source_.substr(fragment.begin,fragment.end-fragment.begin));
            std::optional<std::size_t> assignment;
            std::size_t parentheses{};
            while (tokens.token_.kind!=Token::Kind::end) {
                const auto token=tokens.consume(); const auto origin=position(token.position);
                // Source permits assignments in for's step, whose terminator is ')'.
                // Calls within the value must close before the generated write call.
                if (assignment && parentheses==*assignment && (token.text==";" || token.text==")")) {
                    append(")",origin);assignment.reset();
                }
                if (token.text=="(") ++parentheses;
                else if (token.text==")") {
                    if (!parentheses) reject(token.position,"Unbalanced expression.");
                    --parentheses;
                }
                if (token.text=="self" || token.text=="new" || token.text=="yield" || token.text=="async" || token.text.starts_with("__ui_"))
                    reject(token.position,"UI expressions use typed inputs/state; native context access and asynchronous effects are not permitted.");
                if (token.text=="input" || token.text=="state") {
                    if (!tokens.accept(".")) reject(token.position,"input, state and event are reserved UI identifiers; use a member after '.'.");
                    const auto name=tokens.identifier();
                    const auto state=token.text=="state";
                    std::optional<std::uint32_t> index; InputKind kind{};
                    if (state) {
                        for (std::uint32_t i=0;i<result_.states.size();++i) if (result_.states[i].name==name.text) { index=i; kind=result_.states[i].kind; }
                    } else for (std::uint32_t i=0;i<result_.inputs.size();++i) if (result_.inputs[i].name==name.text) { index=i; kind=result_.inputs[i].kind; }
                    if (!index) reject(token.position,"Unknown UI input or state.");
                    if (tokens.accept("=")) {
                        if (!state || !event || assignment) reject(token.position,"Only event blocks may assign component state; inputs are read-only.");
                        assignment=parentheses;
                        append("self.write_"+suffix(kind)+"("+std::to_string(*index)+", ",origin);
                    } else append("self."+std::string(state?"state_":"read_")+suffix(kind)+"("+std::to_string(*index)+") ",origin);
                } else if (token.text=="event") {
                    if (!event) reject(token.position,"event is reserved and only available inside an event handler.");
                    tokens.expect("."); const auto name=tokens.identifier();
                    if (name.text!="checked") reject(name.position,"This event exposes checked: bool.");
                    append("self.event_bool() ",origin);
                } else {
                    append(token.text,origin); append(" ",origin);
                }
            }
            if (assignment) reject(tokens.token_.position,"State assignment requires a statement terminator.");
        } catch (const Rejected& error) {
            reject(position(error.diagnostic.position),error.diagnostic.message);
        }
    }
};
}

std::expected<Declaration, language::Diagnostic> lower(std::string_view name,std::string_view source) {
    try { return Parser(name,source).run(); }
    catch (const language::detail::Rejected& error) { return std::unexpected(error.diagnostic); }
}

std::string Declaration::diagnostic(std::string_view message) const {
    // Program appends a request tag. Preserve its message, replace the generated
    // line/column with the originating declaration token when available.
    auto colon=message.find("]:");
    if (colon==message.npos) return std::string(message);
    colon+=2;
    std::size_t line{},column{};
    auto first=std::from_chars(message.data()+colon,message.data()+message.size(),line);
    if (first.ec!=std::errc{} || first.ptr==message.data()+message.size() || *first.ptr!=':') return std::string(message);
    auto second=std::from_chars(first.ptr+1,message.data()+message.size(),column);
    if (second.ec!=std::errc{}) return std::string(message);
    std::size_t offset{},current_line=1;
    while (current_line<line && offset<generated_source.size()) if (generated_source[offset++]=='\n') ++current_line;
    offset+=column?column-1:0;
    const auto at=std::ranges::find_if(mappings,[&](const auto& item){return offset>=item.begin && offset<item.end;});
    if (at==mappings.end()) return std::string(message);
    return source_name+":"+std::to_string(at->origin.line)+":"+std::to_string(at->origin.column)+
        std::string(second.ptr,message.data()+message.size());
}
} // namespace atelier::ui_language
