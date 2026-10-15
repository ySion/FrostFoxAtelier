#include "ui_language/declaration.hpp"
#include "language/lexer.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace atelier::ui_language {
namespace {
using namespace language;
using namespace language::detail;
struct Fragment { std::size_t begin{}, end{}; SourcePosition position; };
struct Evaluation { Fragment source; std::uint32_t node{}, operation{}; std::optional<Property> property; InputKind kind{InputKind::boolean}; std::uint32_t argument{}; };
struct Initializer { std::uint32_t slot{}; Fragment source; };
constexpr std::pair<std::string_view,NodeKind> builtin_nodes[]{
    {"Column",NodeKind::column},{"Row",NodeKind::row},{"Text",NodeKind::text},
    {"Foldout",NodeKind::foldout},{"NumberRow",NodeKind::number_row},{"TextField",NodeKind::text_field},
    {"Tree",NodeKind::tree},{"TreeItem",NodeKind::tree_item},{"Repeat",NodeKind::repeat},{"Button",NodeKind::button},
    {"List",NodeKind::list},{"Combo",NodeKind::combo},{"ChoiceButton",NodeKind::choice_button},{"ToggleButton",NodeKind::toggle_button},
    {"Table",NodeKind::table},{"Checkbox",NodeKind::checkbox},{"ParameterRow",NodeKind::parameter_row},
    {"Viewport",NodeKind::viewport},{"Tab",NodeKind::tab},{"Splitter",NodeKind::splitter},
    {"Popup",NodeKind::popup},{"MenuItem",NodeKind::menu_item},{"Overlay",NodeKind::overlay},{"CodeEditor",NodeKind::code_editor}};
std::optional<NodeKind> builtin_node(std::string_view name) {
    const auto at=std::ranges::find_if(builtin_nodes,[&](const auto& item){return item.first==name;});
    return at==std::end(builtin_nodes)?std::nullopt:std::optional{at->second};
}
bool height_strategy(std::string_view name) {
    return name=="height" || name=="expand_height" || name=="single_line" || name=="content_height";
}
bool width_strategy(std::string_view name) {
    return name=="width" || name=="expand_width" || name=="content_width" || name=="shrink_width" || name=="flex_width" || name=="grow_width";
}
bool button_node(NodeKind kind) {
    return kind==NodeKind::button || kind==NodeKind::choice_button || kind==NodeKind::toggle_button || kind==NodeKind::checkbox || kind==NodeKind::foldout;
}
bool container_node(NodeKind kind) {
    return kind==NodeKind::column || kind==NodeKind::row || kind==NodeKind::tree || kind==NodeKind::list || kind==NodeKind::parameter_row || kind==NodeKind::popup || kind==NodeKind::overlay;
}

struct Header {
    std::string name;
    std::vector<Input> inputs;
    SourcePosition begin, body;
};
Input parse_input(Tokens& tokens) {
    Input value;
    if (tokens.accept("bool")) value.kind=InputKind::boolean;
    else if (tokens.accept("double")) value.kind=InputKind::number;
    else {
        const auto type=tokens.identifier();
        if (type.text=="EditBinding") value.kind=InputKind::edit;
        else if (type.text=="Text") value.kind=InputKind::text;
        else if (type.text=="TextEditBinding") value.kind=InputKind::text_edit;
        else if (type.text=="ActionBinding") value.kind=InputKind::action;
        else if (type.text=="TableBinding") value.kind=InputKind::table;
        else if (type.text=="CodeBinding") value.kind=InputKind::code;
        else if (type.text=="Key") value.kind=InputKind::key;
        else if (type.text=="Items") {
            value.kind=InputKind::items;tokens.expect("<");value.item_type=tokens.identifier().text;tokens.expect(">");
        } else reject(type.position,"Unknown UI input type.");
    }
    const auto name=tokens.identifier();value.name=name.text;
    if (name.text=="input" || name.text=="state" || name.text=="event" || name.text=="node" || name.text=="mounted" || name.text.starts_with("__ui_"))
        reject(name.position,"Reserved UI input name.");
    return value;
}
std::vector<Header> headers(std::string_view source) {
    Tokens tokens(source);std::size_t braces{};
    while (tokens.token_.kind!=Token::Kind::end && (tokens.token_.text!="ui" || braces)) {
        const auto token=tokens.consume();
        if (token.text=="{") ++braces;
        else if (token.text=="}") {if (!braces) reject(token.position,"Unexpected closing brace.");--braces;}
    }
    std::vector<Header> result;
    while (tokens.token_.kind!=Token::Kind::end) {
        Header header;header.begin=tokens.expect("ui").position;const auto name=tokens.identifier();header.name=name.text;
        if (builtin_node(name.text)) reject(name.position,"UI component names cannot replace built-in node types.");
        if (result.size()>=64 || std::ranges::any_of(result,[&](const auto& item){return item.name==header.name;}))
            reject(header.begin,"Duplicate or excessive UI component type.");
        tokens.expect("(");
        if (!tokens.accept(")")) do {
            tokens.expect("in");auto input=parse_input(tokens);
            if (header.inputs.size()>=1024 || std::ranges::any_of(header.inputs,[&](const auto& item){return item.name==input.name;}))
                reject(tokens.token_.position,"Duplicate or excessive UI input.");
            header.inputs.push_back(std::move(input));
            if (tokens.accept(")")) break;
            tokens.expect(",");
        } while (true);
        tokens.expect("{");header.body=tokens.token_.position;braces=1;
        while (braces && tokens.token_.kind!=Token::Kind::end) {
            const auto token=tokens.consume();if (token.text=="{") ++braces;else if (token.text=="}") --braces;
        }
        if (braces) reject(header.begin,"Unterminated UI component.");
        result.push_back(std::move(header));
    }
    if (result.empty()) reject(tokens.token_.position,"Expected a ui component.");
    for (const auto& header:result) for (const auto& input:header.inputs) if (input.kind==InputKind::items &&
        std::ranges::none_of(result,[&](const auto& item){return item.name==input.item_type;}))
        reject(header.begin,"Unknown item component type: "+input.item_type);
    return result;
}
class Parser : private Tokens {
public:
    Parser(std::string_view name,std::string_view source,const Header& header,const std::vector<Header>& all)
        :Tokens(source),source_(source),headers_(all) {
        result_.source_name=name;result_.name=header.name;result_.inputs=header.inputs;
        while (token_.position.offset<header.body.offset) consume();
    }
    Declaration run() {
        const Fragment helpers{0,headers_.front().begin.offset,{}};
        while (accept("state")) {
            const auto kind=state_kind(); const auto name=identifier(); unique(name,result_.states);
            expect("="); const auto initial=fragment(";"); expect(";");
            initializers_.push_back({static_cast<std::uint32_t>(result_.states.size()),initial});
            result_.states.push_back({std::string(name.text),kind});
        }
        while (!accept("}")) node(0,0);
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
                const auto reference=reference_kind(evaluation.source);
                const bool text=property==Property::text || property==Property::filter || property==Property::classes ||
                    property==Property::view || property==Property::empty_text || property==Property::menu_behavior ||
                    (property==Property::argument && evaluation.kind==InputKind::text);
                const bool numeric=property==Property::gap || property==Property::height || property==Property::width ||
                    property==Property::padding_left || property==Property::icon ||
                    property==Property::padding_top || property==Property::padding_right || property==Property::padding_bottom ||
                    property==Property::content_width || property==Property::content_height || property==Property::min_width || property==Property::shrink_width ||
                    (property>=Property::row_height && property<=Property::item_gap) ||
                    (property>=Property::offset_x && property<=Property::step) ||
                    (property==Property::argument && evaluation.kind==InputKind::number);
                std::string sink;
                if (property==Property::argument) sink="set_argument_"+suffix(evaluation.kind);
                else if (text) sink="set_text";
                else if (property==Property::edit) sink=result_.nodes[evaluation.node].kind==NodeKind::text_field?"set_text_edit":"set_edit";
                else if (property==Property::action) sink="set_action";
                else if (property==Property::model) sink="set_model";
                else if (property==Property::document) sink="set_document";
                else if (property==Property::context) sink=reference==InputKind::text_edit?"set_context_from_edit":"set_context";
                else if (property==Property::items) sink="set_items";
                else if (property==Property::tree_parent) sink="set_tree_parent";
                else if (property==Property::radio_group) sink="set_radio_group";
                else if (property==Property::selection_group) sink="set_selection_group";
                else sink=numeric?"set_number":"set_bool";
                if (text && reference==InputKind::text) sink+="_reference";
                else if (text && reference==InputKind::text_edit) sink+="_from_edit";
                append("self."+sink+"("+std::to_string(evaluation.node)+", "+
                    std::to_string(property==Property::argument?evaluation.argument:static_cast<unsigned>(property))+", ",evaluation.source.position);
                if (numeric) append("double(",evaluation.source.position);
                rewrite(evaluation.source,false);
                if (numeric) append(")",evaluation.source.position);
                append(");\n",evaluation.source.position);
            } else {
                const auto kind=result_.nodes[evaluation.node].kind;
                rewrite(evaluation.source,true,kind==NodeKind::foldout || kind==NodeKind::toggle_button || kind==NodeKind::checkbox);
                append("\n",evaluation.source.position);
            }
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
    const std::vector<Header>& headers_;
    std::vector<Evaluation> evaluations_;
    std::vector<Initializer> initializers_;

    template<class T> void unique(Token name,const std::vector<T>& items) {
        if (items.size()>=1024 || name.text.starts_with("__ui_") ||
            std::ranges::any_of(items,[&](const auto& item){return item.name==name.text;}))
            reject(name.position,"Duplicate, reserved or excessive UI declaration.");
    }
    InputKind state_kind() {
        if (accept("bool")) return InputKind::boolean;
        if (accept("double")) return InputKind::number;
        const auto token=identifier();
        reject(token.position,"Component state requires bool or double.");
    }
    static std::string suffix(InputKind kind) {
        constexpr std::string_view names[]{"bool","number","edit","text","text_edit","key","items","action","table","code"};
        return std::string(names[static_cast<std::size_t>(kind)]);
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
    std::optional<InputKind> reference_kind(Fragment source) const {
        Tokens tokens(source_.substr(source.begin,source.end-source.begin));
        if (!tokens.accept("input") || !tokens.accept(".")) return {};
        const auto name=tokens.consume();
        if (tokens.token_.kind!=Token::Kind::end) return {};
        const auto at=std::ranges::find_if(result_.inputs,[&](const auto& input){return input.name==name.text;});
        return at==result_.inputs.end()?std::nullopt:std::optional{at->kind};
    }
    void node(std::uint32_t parent,std::size_t depth) {
        if (depth>=64) reject(token_.position,"UI nesting exceeds 64 levels.");
        const auto type=identifier();
        const auto builtin=builtin_node(type.text);
        const auto child=std::ranges::find_if(headers_,[&](const auto& value){return value.name==type.text;});
        const auto kind=builtin.value_or(NodeKind::component);
        if (kind==NodeKind::component && child==headers_.end()) reject(type.position,"Unknown UI node or component type.");
        const auto name=identifier();unique(name,result_.nodes);
        const auto slot=static_cast<std::uint32_t>(result_.nodes.size());
        result_.nodes.push_back({std::string(name.text),kind,parent,kind==NodeKind::component?std::string(type.text):std::string{}});expect("{");
        std::vector<std::string_view> properties;bool has_event{};
        while (!accept("}")) {
            if (accept("on")) {
                const auto event=identifier();
                if (event.text!="activate" || (kind!=NodeKind::foldout && !button_node(kind)) || has_event)
                    reject(event.position,"Expected one activate handler on a Foldout or button.");
                has_event=true;expect("{");const auto body=fragment("}");expect("}");
                const auto operation=static_cast<std::uint32_t>(evaluations_.size()+1);
                evaluations_.push_back({body,slot,operation,{}});result_.events.push_back({slot,operation});continue;
            }
            const auto saved_lexer=lexer_;const auto saved_token=token_;
            const auto property=identifier();
            if (!accept(":")) {
                lexer_=saved_lexer;token_=saved_token;
                if (!container_node(kind)) reject(type.position,"Only layout containers, List and Tree accept child nodes.");
                node(slot+1,depth+1);continue;
            }
            if (std::ranges::find(properties,property.text)!=properties.end()) reject(property.position,"Duplicate UI property or argument.");
            if (kind!=NodeKind::component && height_strategy(property.text) && std::ranges::any_of(properties,height_strategy))
                reject(property.position,"height, expand_height, content_height and single_line are mutually exclusive height strategies.");
            if (kind!=NodeKind::component && width_strategy(property.text) && std::ranges::any_of(properties,width_strategy))
                reject(property.position,"width, expand_width, content_width and shrink_width are mutually exclusive width strategies.");
            if (kind!=NodeKind::component && (property.text=="panel" || property.text=="caption") &&
                std::ranges::any_of(properties,[](auto name){return name=="panel" || name=="caption";}))
                reject(property.position,"panel and caption are mutually exclusive roles.");
            properties.push_back(property.text);
            Property value{};InputKind value_kind{InputKind::boolean};std::uint32_t argument{};
            if (kind==NodeKind::component && property.text!="mounted") {
                const auto at=std::ranges::find_if(child->inputs,[&](const auto& input){return input.name==property.text;});
                if (at==child->inputs.end()) reject(property.position,"Unknown component input.");
                value=Property::argument;value_kind=at->kind;argument=static_cast<std::uint32_t>(at-child->inputs.begin());
            } else {
                constexpr std::string_view names[]{"text","classes","gap","height","width","padding_left","enabled","selected","collapsed","caption","edit",
                    "expand_height","icon","tree_parent","initially_expanded","filter","mounted","items",
                    "padding_top","padding_right","padding_bottom","align_center","single_line","action",
                    "selection_group","content_width","content_height","expand_width","min_width","wrap","panel","model","context","shrink_width",
                    "view","row_height","header_height","tile_width","tile_height","item_gap","empty_text",
                    "modified","out_of_flow","offset_x","offset_y","anchor_x","anchor_y","pivot_x","pivot_y",
                    "flex_width","grow_width","red","green","blue","value","minimum","maximum","step","vertical","click_action",
                    "menu_branch","menu_behavior","radio_group","document","draggable"};
                const auto at=std::ranges::find(names,property.text);
                if (at==std::end(names)) reject(property.position,"Unknown UI property.");
                value=static_cast<Property>(at-std::begin(names));
                if ((kind==NodeKind::component && value!=Property::mounted) ||
                    (kind==NodeKind::repeat && value!=Property::mounted && value!=Property::items))
                    reject(property.position,"This structural scope accepts only mounted and its typed input.");
                if (value==Property::edit && kind!=NodeKind::number_row && kind!=NodeKind::text_field) reject(property.position,"edit requires NumberRow or TextField.");
                if (value==Property::action && ((!button_node(kind) && kind!=NodeKind::table) || kind==NodeKind::foldout)) reject(property.position,"action requires a button, Checkbox or Table.");
                if (value==Property::document && kind!=NodeKind::code_editor) reject(property.position,"document requires CodeEditor.");
                if (value==Property::model && kind!=NodeKind::table) reject(property.position,"model requires Table.");
                if (value>=Property::view && value<=Property::empty_text && kind!=NodeKind::table)
                    reject(property.position,"Table presentation properties require Table.");
                if (value==Property::context && kind==NodeKind::number_row)
                    reject(property.position,"NumberRow context is owned by its EditBinding.");
                if (value==Property::selection_group && kind!=NodeKind::choice_button) reject(property.position,"selection_group requires ChoiceButton.");
                if (value==Property::selected && kind!=NodeKind::foldout && kind!=NodeKind::tree_item &&
                    kind!=NodeKind::choice_button && kind!=NodeKind::toggle_button && kind!=NodeKind::combo && kind!=NodeKind::checkbox && kind!=NodeKind::tab &&
                    kind!=NodeKind::menu_item && kind!=NodeKind::button && !container_node(kind))
                    reject(property.position,"selected requires Foldout, TreeItem, ChoiceButton, ToggleButton or Combo.");
                if ((value==Property::menu_branch || value==Property::menu_behavior || value==Property::radio_group) && kind!=NodeKind::menu_item)
                    reject(property.position,"Menu properties require MenuItem.");
                if ((value==Property::value || value==Property::minimum || value==Property::maximum || value==Property::step || value==Property::vertical || value==Property::click_action) && kind!=NodeKind::splitter)
                    reject(property.position,"Drag properties require Splitter.");
                if (value==Property::caption && kind!=NodeKind::text) reject(property.position,"caption requires Text.");
                if ((value==Property::align_center || value==Property::wrap) && !container_node(kind))
                    reject(property.position,"align_center and wrap require a layout container.");
                if (value==Property::text && ((container_node(kind) && kind!=NodeKind::parameter_row) || kind==NodeKind::text_field || kind==NodeKind::table || kind==NodeKind::code_editor))
                    reject(property.position,"Use Text or a typed text edit binding.");
                if (kind==NodeKind::number_row && value!=Property::text && value!=Property::classes &&
                    value!=Property::enabled && value!=Property::collapsed && value!=Property::edit)
                    reject(property.position,"NumberRow layout and field state are owned by the shared parameter row and EditBinding.");
                if (kind==NodeKind::parameter_row && value!=Property::text && value!=Property::classes &&
                    value!=Property::enabled && value!=Property::collapsed && value!=Property::modified)
                    reject(property.position,"ParameterRow owns its shared layout; put outer layout on its parent.");
                if ((value==Property::tree_parent || value==Property::expanded || value==Property::draggable) && kind!=NodeKind::tree_item) reject(property.position,"Tree relation properties require TreeItem.");
                if (value==Property::filter && kind!=NodeKind::tree) reject(property.position,"filter requires Tree.");
                if (value==Property::mounted && kind!=NodeKind::component && kind!=NodeKind::repeat) reject(property.position,"mounted requires a component or Repeat scope.");
                if (value==Property::items && kind!=NodeKind::repeat) reject(property.position,"items requires Repeat.");
            }
            const auto expression=fragment(";");expect(";");
            if ((value==Property::selection_group || value==Property::context || value==Property::radio_group) && reference_kind(expression)!=InputKind::key &&
                !(value==Property::context && reference_kind(expression)==InputKind::text_edit)) {
                Tokens tokens(source_.substr(expression.begin,expression.end-expression.begin));
                if (!tokens.accept("node") || !tokens.accept(".") || tokens.token_.kind!=Token::Kind::identifier)
                    reject(expression.position,std::string(property.text)+" requires node.name or a direct Key input.");
                tokens.consume();
                if (tokens.token_.kind!=Token::Kind::end)
                    reject(expression.position,std::string(property.text)+" requires node.name or a direct Key input.");
            }
            if ((value==Property::action || (value==Property::argument && value_kind==InputKind::action)) &&
                reference_kind(expression)!=InputKind::action)
                reject(expression.position,"Action bindings require a direct ActionBinding input.");
            if ((value==Property::model || (value==Property::argument && value_kind==InputKind::table)) &&
                reference_kind(expression)!=InputKind::table)
                reject(expression.position,"Table models require a direct TableBinding input.");
            if ((value==Property::document || (value==Property::argument && value_kind==InputKind::code)) && reference_kind(expression)!=InputKind::code)
                reject(expression.position,"CodeEditor requires a direct CodeBinding input.");
            if (value==Property::argument && value_kind==InputKind::items) {
                if (reference_kind(expression)!=InputKind::items) reject(expression.position,"Items arguments require a direct typed input.");
                Tokens tokens(source_.substr(expression.begin,expression.end-expression.begin));tokens.expect("input");tokens.expect(".");const auto input=tokens.identifier();
                const auto at=std::ranges::find_if(result_.inputs,[&](const auto& item){return item.name==input.text;});
                if (at->item_type!=child->inputs[argument].item_type) reject(expression.position,"Component Items argument has a different element type.");
            }
            if (value==Property::items) {
                if (reference_kind(expression)!=InputKind::items) reject(expression.position,"Repeat requires a direct Items input.");
                Tokens tokens(source_.substr(expression.begin,expression.end-expression.begin));tokens.expect("input");tokens.expect(".");const auto input=tokens.identifier();
                const auto at=std::ranges::find_if(result_.inputs,[&](const auto& item){return item.name==input.text;});
                result_.nodes[slot].component_type=at->item_type;
            }
            const auto operation=static_cast<std::uint32_t>(evaluations_.size()+1);
            evaluations_.push_back({expression,slot,operation,value,value_kind,argument});
            result_.bindings.push_back({slot,operation,value,property.position});
        }
        if ((kind==NodeKind::number_row || kind==NodeKind::text_field) && std::ranges::find(properties,"edit")==properties.end())
            reject(name.position,"Editable controls require a typed edit binding.");
        if (kind==NodeKind::code_editor && std::ranges::find(properties,"document")==properties.end())
            reject(name.position,"CodeEditor requires a typed document binding.");
        if (kind==NodeKind::table && std::ranges::find(properties,"model")==properties.end())
            reject(name.position,"Table requires a typed model binding.");
        if (button_node(kind) && has_event && std::ranges::find(properties,"action")!=properties.end())
            reject(name.position,"A button cannot combine action with a local activate handler.");
        if (kind==NodeKind::choice_button && std::ranges::find(properties,"selection_group")==properties.end())
            reject(name.position,"ChoiceButton requires an explicit selection_group.");
        if (kind==NodeKind::repeat && result_.nodes[slot].component_type.empty()) reject(name.position,"Repeat requires items.");
        if (kind==NodeKind::component) for (const auto& input:child->inputs)
            if (std::ranges::find(properties,input.name)==properties.end()) reject(name.position,"Missing component input: "+input.name);
    }
    void append(std::string_view value,SourcePosition origin) {
        const auto begin=result_.generated_source.size(); result_.generated_source+=value;
        if (!value.empty()) result_.mappings.push_back({begin,result_.generated_source.size(),origin});
    }
    void rewrite(Fragment fragment,bool event,bool checked=false) {
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
                } else if (token.text=="node") {
                    tokens.expect(".");const auto name=tokens.identifier();
                    const auto at=std::ranges::find_if(result_.nodes,[&](const auto& item){return item.name==name.text;});
                    if (at==result_.nodes.end()) reject(name.position,"Unknown declaration node.");
                    if (at->kind==NodeKind::component || at->kind==NodeKind::repeat || at->kind==NodeKind::text_field)
                        reject(name.position,"node references require a concrete node with a declaration-owned key.");
                    append("self.node_key("+std::to_string(at-result_.nodes.begin())+") ",origin);
                } else if (token.text=="event") {
                    if (!event) reject(token.position,"event is reserved and only available inside an event handler.");
                    if (!checked) reject(token.position,"Button and ChoiceButton activation do not expose event values.");
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

std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(std::string_view name,std::string_view source) {
    try {
        const auto all=headers(source);std::vector<Declaration> result;
        for (const auto& header:all) result.push_back(Parser(name,source,header,all).run());
        return result;
    } catch (const language::detail::Rejected& error) {return std::unexpected(error.diagnostic);}
}
std::expected<Declaration, language::Diagnostic> lower(std::string_view name,std::string_view source) {
    auto all=lower_module(name,source);
    if (!all) return std::unexpected(all.error());
    if (all->size()!=1) return std::unexpected(language::Diagnostic{{},"Choose a named component from this UI module."});
    return std::move(all->front());
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
