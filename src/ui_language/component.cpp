#include "ui_language/component.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "foundation/string.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <functional>
#include <stdexcept>
#include <windows.h>

namespace atelier::ui_language {
namespace {
constexpr std::size_t composition_limit=10000;
bool button_node(NodeKind kind) {
    return kind==NodeKind::button || kind==NodeKind::choice_button || kind==NodeKind::toggle_button || kind==NodeKind::checkbox || kind==NodeKind::foldout;
}
auto failure(std::string message) { return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)}); }
std::wstring wide(const String& value) {
    const auto text=value.view();
    if (text.empty()) return {};
    if (text.size()>4096*4) throw std::runtime_error("UI text exceeds the control limit.");
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!count || count>4096) throw std::runtime_error("Invalid or excessive UI text.");
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count);
    return result;
}
struct EditReference { std::uint32_t slot; };
struct TextReference { std::uint32_t slot; };
struct TextEditReference { std::uint32_t slot; };
struct KeyReference { std::uint32_t slot; };
struct ItemsReference { std::uint32_t slot; };
struct ActionReference { std::uint32_t slot; };
struct TableReference { std::uint32_t slot; };
struct CodeReference { std::uint32_t slot; };
struct TooltipReference { std::uint32_t slot; };
struct TreeRowReference { std::uint32_t slot; };
InputValue initial(InputKind kind) {
    switch (kind) {
    case InputKind::boolean:return false;
    case InputKind::number:return 0.0;
    case InputKind::edit:return EditBinding{};
    case InputKind::text:return std::wstring{};
    case InputKind::text_edit:return TextEditBinding{};
    case InputKind::key:return ui::ControlKey{};
    case InputKind::items:return ItemList{};
    case InputKind::action:return ActionBinding{};
    case InputKind::table:return TableBinding{};
    case InputKind::code:return CodeBinding{};
    case InputKind::tooltip:return TooltipBinding{};
    case InputKind::tree_row:return TreeRowBinding{};
    }
    throw std::logic_error("Unknown UI input kind.");
}
}
namespace detail {
struct Context;
struct NodeState {
    ui::Control control;
    ui::ControlKey label{},body{};
    EditBinding edit;
    TextEditBinding text_edit;
    ActionBinding action;
    TableBinding table;
    CodeBinding code;
    std::optional<float> minimum_width;
    std::vector<InputValue> arguments;
    std::vector<KeyBinding> aliases;
    ItemList items;
    bool mounted{true};
    std::unique_ptr<Instance> child;
    std::map<ui::ControlKey,std::unique_ptr<Instance>> repeated;
    NodeState* previous{}; // Borrowed accepted owner; transferred only at commit.
    std::size_t begin{},count{};
    bool dirty{true};
};
struct ScopeLifetime {std::uint64_t created{},retired{};};
struct DefinitionState;
struct InstanceData {
    Runtime* runtime{};
    ObjectRoot context;
    ui::ControlKey* next_key{};
    std::unique_ptr<DefinitionState> value;
    DefinitionState* executing{};
    std::unique_ptr<PreparedUpdate> pending;
    std::shared_ptr<ScopeLifetime> lifetime;
    Statistics statistics;
    bool open{true};
    Result<std::unique_ptr<PreparedUpdate>> prepare(std::shared_ptr<Component>,ui::ControlKey,
        std::span<const KeyBinding>,std::span<const InputValue>,std::size_t);
};
// Definition-dependent data can be replaced while its Instance/Context stays
// alive. A source candidate borrows child owners until the shared commit point.
struct DefinitionState {
    InstanceData& scope;
    std::shared_ptr<Component> component;
    source::Program::Lease code;
    ui::ControlKey parent{};
    std::vector<KeyBinding> aliases;
    std::vector<NodeState> nodes;
    std::vector<InputValue> inputs;
    std::vector<std::pair<std::size_t,InputValue>> input_undo;
    std::vector<std::variant<bool,double>> states;
    std::optional<std::vector<std::variant<bool,double>>> state_undo;
    std::vector<std::vector<bool>> dependencies;
    std::vector<bool> dirty,reads;
    std::vector<ui::Control> published;
    std::size_t subtree_nodes{};
    bool evaluating{},initializing{},handling_event{},event_checked{},publication_pending{true};
    explicit DefinitionState(InstanceData& owner):scope(owner) {}
    const Declaration& declaration() const { return component->declaration(); }
    void read(std::size_t index) { if (evaluating) reads.at(index)=true; }
    void invalidate(std::size_t index) {
        for (std::size_t i=0;i<dependencies.size();++i) if (dependencies[i][index]) dirty[i]=true;
    }
    Result<void> invoke(std::uint32_t operation,const Declaration* source=nullptr) {
        struct Execution {
            InstanceData& scope;DefinitionState* previous;
            ~Execution() {scope.executing=previous;}
        } guard{scope,scope.executing};
        scope.executing=this;
        const Scalar argument=operation;
        auto result=code.invoke(scope.context.id(),std::span{&argument,1});
        if (!result) return failure((source?*source:declaration()).diagnostic(result.error().message));
        return {};
    }
    Result<void> publish(std::span<const InputValue>);
};

// Source's self is this UI scope, never an Entity wrapper. All callbacks copy
// borrowed data and finish synchronously; none constructs/destroys Runtime
// objects, reenters JIT, or executes World/editor commands.
struct Context {
    InstanceData* owner{};
    DefinitionState& data() const {
        if (!owner || !owner->open || !owner->executing) throw std::runtime_error("UI component has no active invocation.");
        return *owner->executing;
    }
    bool read_bool(std::uint32_t index) const { auto& d=data(); d.read(index); return std::get<bool>(d.inputs.at(index)); }
    double read_number(std::uint32_t index) const { auto& d=data(); d.read(index); return std::get<double>(d.inputs.at(index)); }
    EditReference read_edit(std::uint32_t index) const {
        auto& d=data(); d.read(index); static_cast<void>(std::get<EditBinding>(d.inputs.at(index))); return {index};
    }
    TextReference read_text(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<std::wstring>(d.inputs.at(index)));return {index}; }
    TextEditReference read_text_edit(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<TextEditBinding>(d.inputs.at(index)));return {index}; }
    KeyReference read_key(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<ui::ControlKey>(d.inputs.at(index)));return {index}; }
    ItemsReference read_items(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<ItemList>(d.inputs.at(index)));return {index}; }
    ActionReference read_action(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<ActionBinding>(d.inputs.at(index)));return {index}; }
    TableReference read_table(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<TableBinding>(d.inputs.at(index)));return {index}; }
    CodeReference read_code(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<CodeBinding>(d.inputs.at(index)));return {index}; }
    TooltipReference read_tooltip(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<TooltipBinding>(d.inputs.at(index)));return {index}; }
    TreeRowReference read_tree_row(std::uint32_t index) const { auto& d=data();d.read(index);static_cast<void>(std::get<TreeRowBinding>(d.inputs.at(index)));return {index}; }
    KeyReference node_key(std::uint32_t slot) const {
        auto& d=data();static_cast<void>(d.nodes.at(slot));return {static_cast<std::uint32_t>(d.inputs.size()+slot)};
    }
    ui::ControlKey key(KeyReference reference) const {
        auto& d=data();return reference.slot<d.inputs.size()?std::get<ui::ControlKey>(d.inputs[reference.slot]):
            d.nodes.at(reference.slot-d.inputs.size()).control.key;
    }
    bool state_bool(std::uint32_t index) const { auto& d=data(); d.read(d.inputs.size()+index); return std::get<bool>(d.states.at(index)); }
    double state_number(std::uint32_t index) const { auto& d=data(); d.read(d.inputs.size()+index); return std::get<double>(d.states.at(index)); }
    void write_bool(std::uint32_t index,bool value) {
        auto& d=data();
        if (!d.initializing && !d.handling_event) throw std::runtime_error("A binding cannot modify component state.");
        auto& target=std::get<bool>(d.states.at(index));
        if (target!=value) {target=value;d.invalidate(d.inputs.size()+index);}
    }
    void write_number(std::uint32_t index,double value) {
        auto& d=data();
        if ((!d.initializing && !d.handling_event) || !std::isfinite(value)) throw std::runtime_error("Invalid component state write.");
        auto& target=std::get<double>(d.states.at(index));
        if (target!=value) {target=value;d.invalidate(d.inputs.size()+index);}
    }
    bool event_bool() const {
        auto& d=data(); if (!d.handling_event) throw std::runtime_error("No active UI event."); return d.event_checked;
    }
    void create_node(std::uint32_t slot,std::uint32_t parent,std::uint32_t kind) {
        auto& d=data(); if (!d.initializing) throw std::runtime_error("UI structure is created only at mount.");
        auto& node=d.nodes.at(slot).control;
        node.parent=parent?(d.declaration().nodes.at(parent-1).kind==NodeKind::parameter_row?d.nodes.at(parent-1).body:d.nodes.at(parent-1).control.key):d.parent;
        const auto type=static_cast<NodeKind>(kind);
        node.kind=type==NodeKind::text?ui::ControlKind::label:type==NodeKind::foldout?ui::ControlKind::foldout:
            type==NodeKind::text_field?ui::ControlKind::text_edit:type==NodeKind::tree?ui::ControlKind::tree:
            type==NodeKind::tree_item?ui::ControlKind::tree_item:(button_node(type) && type!=NodeKind::checkbox)?ui::ControlKind::button:
            type==NodeKind::list?ui::ControlKind::list:type==NodeKind::combo?ui::ControlKind::combo:
            type==NodeKind::code_editor?ui::ControlKind::code_editor:type==NodeKind::table?ui::ControlKind::table:type==NodeKind::checkbox?ui::ControlKind::checkbox:
            type==NodeKind::viewport?ui::ControlKind::viewport:type==NodeKind::tab?ui::ControlKind::tab:
            type==NodeKind::splitter?ui::ControlKind::splitter:type==NodeKind::popup?ui::ControlKind::popup:
            type==NodeKind::menu_item?ui::ControlKind::menu_item:ui::ControlKind::container;
        if (type==NodeKind::toggle_button) node.button_behavior=ui::ButtonBehavior::toggle;
        node.layout=type==NodeKind::row?ui_layout::NodeInput::row():type==NodeKind::foldout?ui_layout::NodeInput::box(0,28):
            type==NodeKind::text?ui_layout::NodeInput::box(0,0):type==NodeKind::overlay?ui_layout::NodeInput::overlay():ui_layout::NodeInput::column();
        node.layout.width=ui_layout::Axis::expand();
        if (type==NodeKind::text) node.layout.height=ui_layout::Axis::content();
        if (type==NodeKind::text_field) {node.layout=ui::single_line_layout();node.layout.width=ui_layout::Axis::expand();}
        if ((button_node(type) && type!=NodeKind::foldout) || type==NodeKind::combo) {node.layout=ui::single_line_layout();node.layout.width=ui_layout::Axis::content();}
        if (type==NodeKind::tree_item) node.layout.height=ui_layout::Axis::fixed(20);
    }
    NodeState& target(std::uint32_t slot) {
        auto& d=data(); if (!d.evaluating) throw std::runtime_error("UI properties are owned by bindings.");
        auto& node=d.nodes.at(slot);node.dirty=true;return node;
    }
    static void text(ui::Control& node,std::uint32_t property,std::wstring value) {
        if (static_cast<Property>(property)==Property::text || static_cast<Property>(property)==Property::empty_text) node.text=std::move(value);
        else if (const auto kind=static_cast<Property>(property);kind==Property::tooltip || kind==Property::tooltip_title || kind==Property::tooltip_identity) {
            const auto member=kind==Property::tooltip?&ui::Tooltip::description:
                kind==Property::tooltip_title?&ui::Tooltip::title:&ui::Tooltip::identity;
            if (node.tooltip && node.tooltip.get()->*member==value) return;
            auto tooltip=node.tooltip?*node.tooltip:ui::Tooltip{};
            tooltip.*member=std::move(value);
            node.tooltip=tooltip.title.empty() && tooltip.identity.empty() && tooltip.description.empty()?nullptr:
                std::make_shared<const ui::Tooltip>(std::move(tooltip));
        }
        else if (static_cast<Property>(property)==Property::menu_behavior) {
            if (value==L"command") node.menu_behavior=ui::MenuBehavior::command;
            else if (value==L"checkbox") node.menu_behavior=ui::MenuBehavior::checkbox;
            else if (value==L"radio") node.menu_behavior=ui::MenuBehavior::radio;
            else throw std::runtime_error("Menu behavior must be command, checkbox or radio.");
        }
        else if (static_cast<Property>(property)==Property::view) {
            if (value==L"details") node.table_layout.view=ui::TableView::details;
            else if (value==L"list") node.table_layout.view=ui::TableView::list;
            else if (value==L"tiles") node.table_layout.view=ui::TableView::tiles;
            else throw std::runtime_error("Table view must be details, list or tiles.");
        }
        else {
            if (value.size()>128 || value.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ")!=value.npos)
                throw std::runtime_error("Invalid UI style classes.");
            const std::string names(value.begin(),value.end());std::vector<std::string> classes;
            std::size_t begin{};
            while (begin<names.size()) {
                while (begin<names.size() && names[begin]==' ') ++begin;
                const auto end=names.find(' ',begin);
                if (begin<names.size()) classes.emplace_back(names.substr(begin,end==names.npos?names.size()-begin:end-begin));
                if (end==names.npos) break;
                begin=end+1;
            }
            node.classes=std::move(classes);
        }
    }
    void set_text(std::uint32_t slot,std::uint32_t property,const String& value) {text(target(slot).control,property,wide(value));}
    void set_text_reference(std::uint32_t slot,std::uint32_t property,TextReference reference) {
        text(target(slot).control,property,std::get<std::wstring>(data().inputs.at(reference.slot)));
    }
    void set_text_from_edit(std::uint32_t slot,std::uint32_t property,TextEditReference reference) {
        text(target(slot).control,property,std::get<TextEditBinding>(data().inputs.at(reference.slot)).text);
    }
    void set_text_edit(std::uint32_t slot,std::uint32_t,TextEditReference reference) {
        auto& d=data();target(slot).text_edit=std::get<TextEditBinding>(d.inputs.at(reference.slot));
    }
    void set_tree_row(std::uint32_t slot,std::uint32_t,TreeRowReference reference) {
        const auto& row=std::get<TreeRowBinding>(data().inputs.at(reference.slot));
        auto& node=target(slot).control;
        node.tree_index=row.index;node.tree_depth=row.depth;node.tree_branch=row.branch;
        node.tree_expanded=row.expanded;node.tree_primary=row.primary;node.tree_lines=row.lines;
    }
    void set_radio_group(std::uint32_t slot,std::uint32_t,KeyReference reference) { target(slot).control.radio_group=key(reference); }
    void set_context(std::uint32_t slot,std::uint32_t,KeyReference reference) { target(slot).control.context_target=key(reference); }
    void set_context_from_edit(std::uint32_t slot,std::uint32_t,TextEditReference reference) {
        target(slot).control.context_target=std::get<TextEditBinding>(data().inputs.at(reference.slot)).key;
    }
    void set_selection_group(std::uint32_t slot,std::uint32_t,KeyReference reference) {
        const auto group=key(reference);
        if (!group) throw std::runtime_error("ChoiceButton requires a nonzero selection group.");
        target(slot).control.selection_group=group;
    }
    void set_items(std::uint32_t slot,std::uint32_t,ItemsReference reference) {
        auto& d=data();target(slot).items=std::get<ItemList>(d.inputs.at(reference.slot));
    }
    void set_number(std::uint32_t slot,std::uint32_t property,double value) {
        const auto p=static_cast<Property>(property);
        if (p==Property::tree_rows) {
            if (!(value>=0 && value<=ui::maximum_tree_rows) || std::floor(value)!=value) throw std::runtime_error("Tree rows require a count within the supported range.");
            target(slot).control.tree_row_count=static_cast<std::uint32_t>(value);return;
        }
        const bool signed_value=p==Property::offset_x || p==Property::offset_y;
        if (!std::isfinite(value) || value<(signed_value?-32768:0) || value>32768) throw std::runtime_error("UI layout value is outside its supported range.");
        auto& state=target(slot);auto& node=state.control;
        if (static_cast<Property>(property)==Property::icon) {
            if (std::floor(value)!=value || value>static_cast<double>(ui::last_control_icon)) throw std::runtime_error("Invalid UI icon.");
            node.icon=static_cast<ui::ControlIcon>(static_cast<unsigned>(value));return;
        }
        auto& layout=node.layout;
        const auto scalar=static_cast<float>(value);
        switch (static_cast<Property>(property)) {
        case Property::offset_x:layout.offset_x=scalar;break;
        case Property::offset_y:layout.offset_y=scalar;break;
        case Property::anchor_x:layout.anchor_x=scalar;break;
        case Property::anchor_y:layout.anchor_y=scalar;break;
        case Property::pivot_x:layout.pivot_x=scalar;break;
        case Property::pivot_y:layout.pivot_y=scalar;break;
        case Property::flex_width:layout.width=ui_layout::Axis::flex(ui_layout::Basis::content(),0,1,scalar);break;
        case Property::grow_width:layout.width=ui_layout::Axis::flex(ui_layout::Basis::content(),1,1,scalar);break;
        case Property::red:case Property::green:case Property::blue: {
            if (value>1) throw std::runtime_error("Color channels require a value in [0, 1].");
            auto color=node.style.background.value_or(ui::Color{0,0,0,1});
            if (p==Property::red) color.r=scalar; else if (p==Property::green) color.g=scalar; else color.b=scalar;
            node.style.background=color;break;
        }
        case Property::value:node.value=value;break;
        case Property::minimum:node.minimum=value;break;
        case Property::maximum:node.maximum=value;break;
        case Property::step:node.drag_step=node.fine_step=value;break;
        case Property::gap: layout.gap=scalar;break;
        case Property::height:layout.height=ui_layout::Axis::fixed(scalar);break;
        case Property::width:layout.width=ui_layout::Axis::fixed(scalar);break;
        case Property::content_width:layout.width=ui_layout::Axis::content(scalar);break;
        case Property::shrink_width:layout.width=ui_layout::Axis::flex(ui_layout::Basis::fixed(scalar),0,1);break;
        case Property::content_height:layout.height=ui_layout::Axis::content(scalar);break;
        case Property::min_width:state.minimum_width=scalar;break;
        case Property::row_height:node.table_layout.row_height=scalar;break;
        case Property::header_height:node.table_layout.header_height=scalar;break;
        case Property::tile_width:node.table_layout.tile_width=scalar;break;
        case Property::tile_height:node.table_layout.tile_height=scalar;break;
        case Property::item_gap:node.table_layout.item_gap=scalar;break;
        case Property::padding_left:layout.padding.left=scalar;break;
        case Property::padding_top:layout.padding.top=scalar;break;
        case Property::padding_right:layout.padding.right=scalar;break;
        case Property::padding_bottom:layout.padding.bottom=scalar;break;
        default:throw std::runtime_error("Invalid numeric UI property.");
        }
    }
    void set_bool(std::uint32_t slot,std::uint32_t property,bool value) {
        auto& state=target(slot);auto& node=state.control;
        switch (static_cast<Property>(property)) {
        case Property::modified:node.modified=value;break;
        case Property::out_of_flow:node.layout.out_of_flow=value;break;
        case Property::vertical:node.drag_axis=value?ui::DragAxis::vertical:ui::DragAxis::horizontal;break;
        case Property::click_action:node.click_action=value;break;
        case Property::menu_branch:node.menu_branch=value;break;
        case Property::enabled:node.enabled=value;break;
        case Property::selected:node.selected=value;break;
        case Property::collapsed:node.layout.collapsed=value;break;
        case Property::caption:node.role=value?ui::Role::caption:ui::Role::normal;break;
        case Property::panel:node.role=value?ui::Role::panel:ui::Role::normal;break;
        case Property::expand_height:node.layout.height=value?ui_layout::Axis::expand():ui_layout::Axis::content();break;
        case Property::expand_width:node.layout.width=value?ui_layout::Axis::expand():ui_layout::Axis::content();break;
        case Property::draggable:node.tree_draggable=value;break;
        case Property::mounted:state.mounted=value;break;
        case Property::align_center:
            if (!node.layout.flow) node.layout.flow=ui_layout::FlexSpec{};
            node.layout.flow->align_items=ui_layout::Alignment::fit(value?.5f:0.f);break;
        case Property::wrap:
            if (!node.layout.flow) node.layout.flow=ui_layout::FlexSpec{};
            node.layout.flow->wrap=value?ui_layout::Wrap::wrap:ui_layout::Wrap::no_wrap;break;
        case Property::single_line:
            node.single_line=value;
            node.layout.height=ui_layout::Axis::content(value?ui::single_line_height:0.f);break;
        default:throw std::runtime_error("Invalid boolean UI property.");
        }
    }
    void set_edit(std::uint32_t slot,std::uint32_t,EditReference reference) {
        auto& d=data(); auto& node=target(slot);
        node.edit=std::get<EditBinding>(d.inputs.at(reference.slot));
    }
    void set_action(std::uint32_t slot,std::uint32_t,ActionReference reference) {
        auto& d=data();target(slot).action=std::get<ActionBinding>(d.inputs.at(reference.slot));
    }
    void set_model(std::uint32_t slot,std::uint32_t,TableReference reference) {
        auto& d=data();target(slot).table=std::get<TableBinding>(d.inputs.at(reference.slot));
    }
    void set_document(std::uint32_t slot,std::uint32_t,CodeReference reference) {
        target(slot).code=std::get<CodeBinding>(data().inputs.at(reference.slot));
    }
    void set_tooltip(std::uint32_t slot,std::uint32_t,TooltipReference reference) {
        auto& current=target(slot).control.tooltip;
        const auto& next=std::get<TooltipBinding>(data().inputs.at(reference.slot));
        if (current!=next && (!current || !next || *current!=*next)) current=next;
    }
    void set_argument_code(std::uint32_t slot,std::uint32_t index,CodeReference value) { argument(slot,index,std::get<CodeBinding>(data().inputs.at(value.slot))); }
    void set_argument_tooltip(std::uint32_t slot,std::uint32_t index,TooltipReference value) { argument(slot,index,std::get<TooltipBinding>(data().inputs.at(value.slot))); }
    void set_argument_tree_row(std::uint32_t slot,std::uint32_t index,TreeRowReference value) { argument(slot,index,std::get<TreeRowBinding>(data().inputs.at(value.slot))); }
    void argument(std::uint32_t slot,std::uint32_t index,InputValue value) {
        target(slot).arguments.at(index)=std::move(value);
    }
    void set_argument_bool(std::uint32_t slot,std::uint32_t index,bool value) { argument(slot,index,value); }
    void set_argument_number(std::uint32_t slot,std::uint32_t index,double value) { argument(slot,index,value); }
    void set_argument_text(std::uint32_t slot,std::uint32_t index,const String& value) { argument(slot,index,wide(value)); }
    void set_argument_key(std::uint32_t slot,std::uint32_t index,KeyReference value) { argument(slot,index,key(value)); }
    void set_argument_edit(std::uint32_t slot,std::uint32_t index,EditReference value) { argument(slot,index,std::get<EditBinding>(data().inputs.at(value.slot))); }
    void set_argument_text_reference(std::uint32_t slot,std::uint32_t index,TextReference value) { argument(slot,index,std::get<std::wstring>(data().inputs.at(value.slot))); }
    void set_argument_text_edit(std::uint32_t slot,std::uint32_t index,TextEditReference value) { argument(slot,index,std::get<TextEditBinding>(data().inputs.at(value.slot))); }
    void set_argument_items(std::uint32_t slot,std::uint32_t index,ItemsReference value) { argument(slot,index,std::get<ItemList>(data().inputs.at(value.slot))); }
    void set_argument_action(std::uint32_t slot,std::uint32_t index,ActionReference value) { argument(slot,index,std::get<ActionBinding>(data().inputs.at(value.slot))); }
    void set_argument_table(std::uint32_t slot,std::uint32_t index,TableReference value) { argument(slot,index,std::get<TableBinding>(data().inputs.at(value.slot))); }
    void set_argument_text_from_edit(std::uint32_t slot,std::uint32_t index,TextEditReference value) { argument(slot,index,std::get<TextEditBinding>(data().inputs.at(value.slot)).text); }

};
}

struct Component::Impl {
    Runtime& runtime;
    Declaration declaration;
    std::shared_ptr<source::Program> program;
    std::vector<std::shared_ptr<Component>> children;
    Impl(Runtime& value,Declaration source):runtime(value),declaration(std::move(source)) {}
};
Component::Component(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Component::~Component()=default;
const Declaration& Component::declaration() const noexcept {return impl_->declaration;}

Result<TypeId> register_types(Runtime& runtime) {
    if (auto type=runtime.native_type<detail::Context>();type) return *type;
    std::vector<TypeDefinition> definitions;
    if (!runtime.native_type<String>()) definitions.push_back(string_definition(runtime));
    definitions.push_back(NativeRegistration<EditReference>(runtime,"ui.EditReference",TypeKind::record)
        .field<&EditReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(EditReference,slot)).build());
    definitions.push_back(NativeRegistration<TextReference>(runtime,"ui.TextReference",TypeKind::record)
        .field<&TextReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(TextReference,slot)).build());
    definitions.push_back(NativeRegistration<TextEditReference>(runtime,"ui.TextEditReference",TypeKind::record)
        .field<&TextEditReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(TextEditReference,slot)).build());
    definitions.push_back(NativeRegistration<KeyReference>(runtime,"ui.KeyReference",TypeKind::record)
        .field<&KeyReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(KeyReference,slot)).build());
    definitions.push_back(NativeRegistration<ItemsReference>(runtime,"ui.ItemsReference",TypeKind::record)
        .field<&ItemsReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(ItemsReference,slot)).build());
    definitions.push_back(NativeRegistration<ActionReference>(runtime,"ui.ActionReference",TypeKind::record)
        .field<&ActionReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(ActionReference,slot)).build());
    definitions.push_back(NativeRegistration<TableReference>(runtime,"ui.TableReference",TypeKind::record)
        .field<&TableReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(TableReference,slot)).build());
    definitions.push_back(NativeRegistration<CodeReference>(runtime,"ui.CodeReference",TypeKind::record)
        .field<&CodeReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(CodeReference,slot)).build());
    definitions.push_back(NativeRegistration<TooltipReference>(runtime,"ui.TooltipReference",TypeKind::record)
        .field<&TooltipReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(TooltipReference,slot)).build());
    definitions.push_back(NativeRegistration<TreeRowReference>(runtime,"ui.TreeRowReference",TypeKind::record)
        .field<&TreeRowReference::slot>("slot",{TypeReference::native<std::uint32_t>()},offsetof(TreeRowReference,slot)).build());
    using C=detail::Context;
    definitions.push_back(NativeRegistration<C>(runtime,"ui.ComponentContext",TypeKind::object)
        .method<&C::read_bool>("read_bool",true).method<&C::read_number>("read_number",true).method<&C::read_edit>("read_edit",true)
        .method<&C::state_bool>("state_bool",true).method<&C::state_number>("state_number",true)
        .method<&C::write_bool>("write_bool",true).method<&C::write_number>("write_number",true)
        .method<&C::event_bool>("event_bool",true).method<&C::create_node>("create_node",true)
        .method<&C::set_text>("set_text",true).method<&C::set_number>("set_number",true)
        .method<&C::set_bool>("set_bool",true).method<&C::set_edit>("set_edit",true)
        .method<&C::read_text>("read_text",true)
        .method<&C::read_text_edit>("read_text_edit",true)
        .method<&C::read_key>("read_key",true)
        .method<&C::read_items>("read_items",true)
        .method<&C::read_action>("read_action",true)
        .method<&C::read_table>("read_table",true)
        .method<&C::read_code>("read_code",true)
        .method<&C::set_document>("set_document",true)
        .method<&C::set_argument_code>("set_argument_code",true)
        .method<&C::read_tooltip>("read_tooltip",true)
        .method<&C::set_tooltip>("set_tooltip",true)
        .method<&C::set_argument_tooltip>("set_argument_tooltip",true)
        .method<&C::read_tree_row>("read_tree_row",true)
        .method<&C::set_tree_row>("set_tree_row",true)
        .method<&C::set_argument_tree_row>("set_argument_tree_row",true)
        .method<&C::node_key>("node_key",true)
        .method<&C::set_text_reference>("set_text_reference",true)
        .method<&C::set_text_from_edit>("set_text_from_edit",true)
        .method<&C::set_text_edit>("set_text_edit",true)
        .method<&C::set_context>("set_context",true)
        .method<&C::set_context_from_edit>("set_context_from_edit",true)
        .method<&C::set_radio_group>("set_radio_group",true)
        .method<&C::set_selection_group>("set_selection_group",true)
        .method<&C::set_items>("set_items",true)
        .method<&C::set_action>("set_action",true)
        .method<&C::set_model>("set_model",true)
        .method<&C::set_argument_bool>("set_argument_bool",true)
        .method<&C::set_argument_number>("set_argument_number",true)
        .method<&C::set_argument_text>("set_argument_text",true)
        .method<&C::set_argument_key>("set_argument_key",true)
        .method<&C::set_argument_edit>("set_argument_edit",true)
        .method<&C::set_argument_text_reference>("set_argument_text_reference",true)
        .method<&C::set_argument_text_edit>("set_argument_text_edit",true)
        .method<&C::set_argument_items>("set_argument_items",true)
        .method<&C::set_argument_action>("set_argument_action",true)
        .method<&C::set_argument_table>("set_argument_table",true)
        .method<&C::set_argument_text_from_edit>("set_argument_text_from_edit",true)
        .build());
    auto registered=runtime.register_types(std::move(definitions));
    if (!registered) return std::unexpected(registered.error());
    return registered->back();
}
bool ItemList::operator==(const ItemList& other) const {
    if (values==other.values) return true;
    const std::span<const Item> a=values?std::span<const Item>(*values):std::span<const Item>{};
    const std::span<const Item> b=other.values?std::span<const Item>(*other.values):std::span<const Item>{};
    return std::ranges::equal(a,b);
}
std::shared_ptr<Component> Module::find(std::string_view name) const {
    const auto found=std::ranges::find_if(components_,[&](const auto& item){return item->declaration().name==name;});
    return found==components_.end()?nullptr:*found;
}
Result<std::shared_ptr<Module>> Module::compile(Runtime& runtime,std::string_view name,std::string_view source,const Module* previous) {
    auto declarations=lower_module(name,source);
    if (!declarations) return failure(std::string(name)+":"+std::to_string(declarations.error().position.line)+":"+
        std::to_string(declarations.error().position.column)+": "+declarations.error().message);
    const auto type=runtime.native_type<detail::Context>();
    if (!type) return failure("Register UI language types during Runtime bootstrap before creating objects.");
    auto module=std::make_shared<Module>();std::vector<unsigned> states(declarations->size());
    module->components_.resize(declarations->size());
    std::function<Result<std::shared_ptr<Component>>(std::size_t)> compile=[&](std::size_t index)->Result<std::shared_ptr<Component>> {
        if (states[index]==2) return module->components_[index];
        if (states[index]==1) return failure(std::string(name)+": Recursive UI composition is not supported.");
        states[index]=1;const auto& declaration=(*declarations)[index];
        std::vector<std::shared_ptr<Component>> children(declaration.nodes.size());
        for (std::size_t i=0;i<declaration.nodes.size();++i) if (!declaration.nodes[i].component_type.empty()) {
            const auto& child_name=declaration.nodes[i].component_type;
            const auto at=std::ranges::find_if(*declarations,[&](const auto& item){return item.name==child_name;});
            if (at==declarations->end()) return failure(std::string(name)+": Unknown child component "+child_name);
            auto child=compile(static_cast<std::size_t>(at-declarations->begin()));if (!child) return std::unexpected(child.error());
            if (declaration.nodes[i].kind==NodeKind::repeat &&
                (std::ranges::count_if((*child)->declaration().nodes,[](const auto& node){return node.parent==0;})!=1 ||
                 (*child)->declaration().nodes.front().kind==NodeKind::component || (*child)->declaration().nodes.front().kind==NodeKind::repeat))
                return failure(std::string(name)+": Repeated components require one concrete root node.");
            children[i]=std::move(*child);
        }
        auto impl=std::make_unique<Component::Impl>(runtime,declaration);impl->children=std::move(children);
        const auto prior=previous?previous->find(declaration.name):nullptr;
        if (prior && &prior->impl_->runtime==&runtime && prior->declaration().generated_source==declaration.generated_source) {
            // Share only immutable code. Source maps and child edges belong to
            // this revision even when its parent dispatch did not change.
            impl->program=prior->impl_->program;++module->compilation_.reused;
        } else {
            impl->program=std::make_shared<source::Program>(runtime,*type);
            auto request=impl->program->request(impl->declaration.source_name,impl->declaration.generated_source,"__ui_dispatch");
            if (!request) return std::unexpected(request.error());
            auto candidate=source::Program::prepare(*request);
            if (!candidate) return failure(impl->declaration.diagnostic(candidate.error().message));
            auto published=impl->program->try_publish(*candidate);if (!published) return std::unexpected(published.error());
            auto signature=impl->program->bind<void,std::uint32_t>();if (!signature) return std::unexpected(signature.error());
            ++module->compilation_.compiled;
        }
        module->components_[index]=std::shared_ptr<Component>(new Component(std::move(impl)));states[index]=2;
        return module->components_[index];
    };
    for (std::size_t i=0;i<declarations->size();++i) if (auto result=compile(i);!result) return std::unexpected(result.error());
    return module;
}
Result<std::shared_ptr<Component>> Component::compile(Runtime& runtime,std::string_view name,std::string_view source,std::string_view entry) {
    auto module=Module::compile(runtime,name,source);if (!module) return std::unexpected(module.error());
    if (entry.empty()) {
        if ((*module)->components_.size()!=1) return failure("Choose a named component from this UI module.");
        return (*module)->components_.front();
    }
    auto result=(*module)->find(entry);
    if (!result) return failure(std::string(name)+": Unknown component "+std::string(entry));
    return result;
}

// One plan covers data, definition and topology changes. Accepted child owners
// remain available until Session has consumed its cleanup effects.
struct detail::PreparedUpdate {
    struct Change {
        std::size_t node{},next_count{};
        std::vector<ui::Control> controls;
        std::unique_ptr<Instance> child;
        std::map<ui::ControlKey,std::unique_ptr<Instance>> additions;
        std::vector<ui::ControlKey> removals;
        std::vector<std::unique_ptr<PreparedUpdate>> children;
        bool unmount{},controls_changed{};
    };
    InstanceData& scope;
    std::unique_ptr<DefinitionState> replacement;
    DefinitionState& data;
    std::shared_ptr<Component> definition;
    std::vector<Change> changes;
    std::vector<ui::Control> reordered;
    std::uint64_t creations{},removals{};
    std::size_t subtree_nodes{};
    bool resized{},control_changes{},materialized{},committed{};
    PreparedUpdate(InstanceData& owner,std::unique_ptr<DefinitionState> next,std::shared_ptr<Component> type)
        :scope(owner),replacement(std::move(next)),data(replacement?*replacement:*owner.value),definition(std::move(type)) {}
    ~PreparedUpdate() {if (materialized && !committed) swap_view();}
    void append(std::vector<ui::Control>& output) const {
        if (materialized) {output.insert(output.end(),data.published.begin(),data.published.end());return;}
        std::size_t change{};
        for (std::size_t i=0;i<data.nodes.size();++i) {
            const auto* next=change<changes.size() && changes[change].node==i?&changes[change++]:nullptr;
            const auto values=next && next->controls_changed?std::span<const ui::Control>(next->controls):
                std::span<const ui::Control>(data.published).subspan(data.nodes[i].begin,data.nodes[i].count);
            output.insert(output.end(),values.begin(),values.end());
        }
    }
    void swap_view() noexcept {
        if (resized) data.published.swap(reordered);
        else for (auto& change:changes) if (change.controls_changed) {
            auto at=data.published.begin()+static_cast<std::ptrdiff_t>(data.nodes[change.node].begin);
            std::swap_ranges(change.controls.begin(),change.controls.end(),at);
        }
    }
    void materialize() noexcept {if (!materialized) {swap_view();materialized=true;}}
    void commit() noexcept {
        // Borrowed owners move only now; removed nodes stay in the old definition
        // until every child plan and descriptor change has been committed.
        for (auto& node:data.nodes) if (node.previous) {
            node.child=std::move(node.previous->child);node.repeated=std::move(node.previous->repeated);node.previous=nullptr;
        }
        for (auto& change:changes) {
            for (auto& child:change.children) child->commit();
            auto& node=data.nodes[change.node];
            if (change.unmount) node.child.reset();
            if (change.child) node.child=std::move(change.child);
            for (const auto key:change.removals) node.repeated.erase(key);
            node.repeated.merge(change.additions);
        }
        materialize();
        for (const auto& change:changes) if (change.controls_changed) data.nodes[change.node].count=change.next_count;
        std::size_t begin{};
        for (auto& node:data.nodes) {node.begin=begin;begin+=node.count;node.dirty=false;}
        scope.statistics.node_creations+=creations;scope.statistics.node_removals+=removals;
        if (control_changes) ++scope.statistics.publications;
        data.subtree_nodes=subtree_nodes;data.component=std::move(definition);
        data.input_undo.clear();data.state_undo.reset();data.publication_pending=false;
        committed=true;
        if (replacement) scope.value=std::move(replacement);
    }
};
Result<void> detail::DefinitionState::publish(std::span<const InputValue> values) {
    const auto& definitions=declaration().inputs;
    if (values.size()!=definitions.size()) return failure("UI component input count does not match its declaration.");
    for (std::size_t i=0;i<values.size();++i) {
        if (values[i].index()!=static_cast<std::size_t>(definitions[i].kind)) return failure("UI component input type mismatch: "+definitions[i].name);
        if (const auto* number=std::get_if<double>(&values[i]);number && !std::isfinite(*number)) return failure("UI numeric inputs must be finite.");
        if (const auto* action=std::get_if<ActionBinding>(&values[i]);action && !action->target)
            return failure("UI action bindings require a nonzero host target: "+definitions[i].name);
        if (const auto* code=std::get_if<CodeBinding>(&values[i]);code && !code->document)
            return failure("CodeBinding requires a live document.");
        if (const auto* table=std::get_if<TableBinding>(&values[i]);table && !table->data)
            return failure("UI table bindings require an immutable data snapshot: "+definitions[i].name);
    }
    if (inputs.size()!=values.size()) {inputs.assign(values.begin(),values.end());return {};}
    std::vector<std::pair<std::size_t,InputValue>> changes;
    for (std::size_t i=0;i<values.size();++i) if (values[i]!=inputs[i]) changes.emplace_back(i,values[i]);
    input_undo.reserve(input_undo.size()+changes.size());
    for (auto& [index,value]:changes) {
        if (std::ranges::none_of(input_undo,[&](const auto& old){return old.first==index;})) input_undo.emplace_back(index,std::move(inputs[index]));
        inputs[index]=std::move(value);invalidate(index);
    }
    return {};
}
Result<std::unique_ptr<detail::PreparedUpdate>> detail::InstanceData::prepare(std::shared_ptr<Component> definition,
    ui::ControlKey parent,std::span<const KeyBinding> aliases,std::span<const InputValue> inputs,std::size_t budget) {
    if (!definition || &definition->impl_->runtime!=runtime || (value && value->declaration().name!=definition->declaration().name))
        return failure("A UI scope keeps its Runtime and component type.");
    const auto& declaration=definition->declaration();
    if (declaration.nodes.size()>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
    const bool source_changed=value && definition!=value->component;
    const bool aliases_changed=!value || !std::ranges::equal(aliases,value->aliases);
    if (source_changed || aliases_changed) {
        std::vector<std::string_view> paths;paths.reserve(aliases.size());
        for (const auto& alias:aliases) {
            const std::string_view path=alias.node;
            if (std::ranges::find(paths,path)!=paths.end()) return failure("Duplicate UI key alias path: "+alias.node);
            paths.push_back(path);
            auto remaining=path;const auto* component=definition.get();
            for (;;) {
                const auto dot=remaining.find('.');const auto name=remaining.substr(0,dot);
                const auto& nodes=component->declaration().nodes;
                const auto node=std::ranges::find_if(nodes,[&](const auto& item){return item.name==name;});
                if (node==nodes.end()) return failure("Unknown UI key alias path: "+alias.node);
                if (dot==remaining.npos) {
                    if (node->kind==NodeKind::component || node->kind==NodeKind::repeat ||
                        (node->kind==NodeKind::text_field && path.find('.')!=path.npos))
                        return failure("UI key alias requires a concrete node with a declaration-owned key: "+alias.node);
                    break;
                }
                if (node->kind!=NodeKind::component)
                    return failure("UI key alias paths may cross only static component instances: "+alias.node);
                component=component->impl_->children[static_cast<std::size_t>(node-nodes.begin())].get();
                remaining.remove_prefix(dot+1);
            }
        }
    }
    const auto same_shape=[&] {
        const auto& before=value->declaration();
        return before.inputs==declaration.inputs && before.states==declaration.states && before.nodes==declaration.nodes &&
            std::ranges::equal(before.bindings,declaration.bindings,[](const auto& a,const auto& b){return a.node==b.node && a.operation==b.operation && a.property==b.property;}) &&
            std::ranges::equal(before.events,declaration.events,[](const auto& a,const auto& b){return a.node==b.node && a.operation==b.operation;});
    };
    const bool replace=!value || parent!=value->parent || aliases_changed ||
        (source_changed && (definition->impl_->program!=value->component->impl_->program || !same_shape()));
    std::unique_ptr<DefinitionState> next;
    if (replace) {
        next=std::make_unique<DefinitionState>(*this);next->component=definition;next->parent=parent;
        next->aliases.assign(aliases.begin(),aliases.end());
        auto code=definition->impl_->program->pin();if (!code) return std::unexpected(code.error());next->code=std::move(*code);
        next->nodes.resize(declaration.nodes.size());
        const auto allocate=[&] {
            if (!*next_key || *next_key==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("UI control keys exhausted.");
            return (*next_key)++;
        };
        for (std::size_t i=0;i<next->nodes.size();++i) {
            auto& node=next->nodes[i];const auto& declared=declaration.nodes[i];
            const auto alias=std::ranges::find_if(aliases,[&](const auto& key){return key.node==declared.name;});
            if (value) {
                const auto& before=value->declaration().nodes;
                const auto old=std::ranges::find_if(before,[&](const auto& n){return n.name==declared.name && n.kind==declared.kind && n.component_type==declared.component_type;});
                if (old!=before.end()) node.previous=&value->nodes[static_cast<std::size_t>(old-before.begin())];
            }
            node.control.key=alias!=aliases.end() && alias->key?alias->key:node.previous?node.previous->control.key:allocate();
            if (const auto& type=definition->impl_->children[i]) for (const auto& input:type->declaration().inputs) node.arguments.push_back(initial(input.kind));
            if (declared.kind==NodeKind::component) {
                const auto prefix=declared.name+'.';
                for (const auto& key:aliases) if (key.node.starts_with(prefix))
                    node.aliases.push_back({key.node.substr(prefix.size()),key.key,key.label,key.body});
            }
            if (declared.kind==NodeKind::number_row || declared.kind==NodeKind::parameter_row) {
                node.label=alias!=aliases.end() && alias->label?alias->label:node.previous?node.previous->label:allocate();
                node.body=alias!=aliases.end() && alias->body?alias->body:node.previous?node.previous->body:allocate();
            }
        }
        for (const auto& state:declaration.states) next->states.push_back(state.kind==InputKind::boolean?std::variant<bool,double>{false}:std::variant<bool,double>{0.0});
        next->dependencies.resize(declaration.bindings.size(),std::vector<bool>(declaration.inputs.size()+declaration.states.size()));
        next->dirty.resize(declaration.bindings.size(),true);next->reads.resize(declaration.inputs.size()+declaration.states.size());
        if (auto result=next->publish(inputs);!result) return std::unexpected(result.error());
        next->initializing=true;auto initialized=next->invoke(0);next->initializing=false;
        if (!initialized) return std::unexpected(initialized.error());
        if (value) for (std::size_t i=0;i<declaration.states.size();++i) {
            const auto& old=value->declaration().states;const auto& state=declaration.states[i];
            const auto at=std::ranges::find_if(old,[&](const auto& s){return s.name==state.name && s.kind==state.kind;});
            if (at!=old.end()) next->states[i]=value->states[static_cast<std::size_t>(at-old.begin())];
        }
    } else if (auto result=value->publish(inputs);!result) return std::unexpected(result.error());
    auto& data=next?*next:*value;
    for (std::size_t i=0;i<data.dirty.size();++i) if (data.dirty[i]) {
        std::fill(data.reads.begin(),data.reads.end(),false);data.evaluating=true;
        auto evaluated=data.invoke(declaration.bindings[i].operation,&declaration);data.evaluating=false;++statistics.binding_evaluations;
        if (!evaluated) return std::unexpected(evaluated.error());
        data.dependencies[i]=data.reads;data.dirty[i]=false;data.publication_pending=true;
    }
    if (!replace && !source_changed && !data.publication_pending && data.input_undo.empty() && !data.state_undo) {
        if (data.subtree_nodes>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
        return std::unique_ptr<PreparedUpdate>{};
    }
    auto update=std::make_unique<PreparedUpdate>(*this,std::move(next),definition);
    auto remaining=budget-data.nodes.size();bool keys_changed{};
    for (std::size_t i=0;i<data.nodes.size();++i) {
        auto& node=data.nodes[i];auto& owners=node.previous?*node.previous:node;
        if (!node.dirty && !source_changed) {
            auto retained=node.child?node.child->data_->value->subtree_nodes:0;
            for (const auto& [key,child]:node.repeated) retained+=child->data_->value->subtree_nodes;
            if (retained>remaining) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
            remaining-=retained;continue;
        }
        PreparedUpdate::Change change;change.node=i;auto& output=change.controls;
        const auto kind=declaration.nodes[i].kind;const auto& type=definition->impl_->children[i];
        if (node.minimum_width) node.control.layout.width.minimum=*node.minimum_width;
        const auto append_existing=[&](Instance& child,std::span<const InputValue> values,std::span<const KeyBinding> keys,ui::ControlKey parent)->Result<void> {
            auto prepared=child.data_->prepare(type,parent,keys,values,remaining);
            if (!prepared) return std::unexpected(prepared.error());
            remaining-=(*prepared?(*prepared)->subtree_nodes:child.data_->value->subtree_nodes);
            if (*prepared) {(*prepared)->append(output);change.children.push_back(std::move(*prepared));}
            else {const auto controls=child.controls();output.insert(output.end(),controls.begin(),controls.end());}
            return {};
        };
        const auto append_new=[&](std::unique_ptr<Instance>& child) {
            remaining-=child->data_->value->subtree_nodes;
            const auto controls=child->controls();output.insert(output.end(),controls.begin(),controls.end());
        };
        if (kind==NodeKind::component) {
            if (!node.mounted) change.unmount=owners.child!=nullptr;
            else if (owners.child) {
                if (auto result=append_existing(*owners.child,node.arguments,node.aliases,node.control.parent);!result) return std::unexpected(result.error());
            } else {
                auto child=Instance::create_bounded(type,node.control.parent,*next_key,node.aliases,node.arguments,remaining,lifetime);
                if (!child) return std::unexpected(child.error());change.child=std::move(*child);append_new(change.child);
            }
        } else if (kind==NodeKind::repeat) {
            std::vector<ui::ControlKey> present;
            if (node.mounted && node.items.values) {
                if (node.items.values->size()>composition_limit) return failure("UI repeat exceeds 10000 items.");
                present.reserve(node.items.values->size());for (const auto& item:*node.items.values) present.push_back(item.key);
                std::sort(present.begin(),present.end());
                if ((!present.empty() && !present.front()) || std::adjacent_find(present.begin(),present.end())!=present.end())
                    return failure("UI repeat requires distinct nonzero item keys.");
                for (const auto& item:*node.items.values) {
                    auto keys=item.keys;
                    keys.push_back({type->declaration().nodes.front().name,item.key});
                    const auto parent=item.parent.value_or(node.control.parent);
                    const auto at=owners.repeated.find(item.key);
                    if (at==owners.repeated.end()) {
                        auto child=Instance::create_bounded(type,parent,*next_key,keys,item.inputs,remaining,lifetime);
                        if (!child) return std::unexpected(child.error());append_new(*child);change.additions.emplace(item.key,std::move(*child));
                    } else if (auto result=append_existing(*at->second,item.inputs,keys,parent);!result) return std::unexpected(result.error());
                    if (output.size()>composition_limit) return failure("UI composition exceeds 10000 controls.");
                }
            }
            for (const auto& [key,child]:owners.repeated) if (!std::binary_search(present.begin(),present.end(),key)) change.removals.push_back(key);
        } else if (button_node(kind)) {
            auto control=node.control;control.action_target=node.action.target;control.enabled&=node.action.enabled;
            output.push_back(std::move(control));
        } else if (kind==NodeKind::code_editor) {
            auto control=node.control;control.code=node.code.document;control.enabled&=node.code.enabled;
            control.read_only=node.code.read_only;
            output.push_back(std::move(control));
        } else if (kind==NodeKind::table) {
            auto control=node.control;control.table=node.table.data;control.table_selection=node.table.selection;
            control.action_target=node.action.target;control.enabled&=node.table.enabled && node.action.enabled;
            output.push_back(std::move(control));
        } else if (kind==NodeKind::text_field) {
            auto control=node.control;control.key=node.text_edit.key;control.text=node.text_edit.text;control.enabled&=node.text_edit.enabled;
            if (!control.key) return failure("TextField requires a nonzero host edit key.");output.push_back(std::move(control));
        } else if (kind==NodeKind::parameter_row) {
            ui::append_parameter_row(output,{node.control.key,node.label,node.body,0,node.control.parent,
                node.control.text,node.control.enabled,node.control.modified});
            output.front().layout.collapsed=node.control.layout.collapsed;
            output.front().tooltip=node.control.tooltip;
            output.front().classes.insert(output.front().classes.end(),node.control.classes.begin(),node.control.classes.end());
        } else if (kind!=NodeKind::number_row) output.push_back(node.control);
        else if (!node.edit.fields.empty()) {
            try {ui::append_number_row(output,{node.control.key,node.label,node.body,node.edit.group,node.control.parent,
                node.control.text,node.control.enabled && node.edit.enabled,node.edit.modified},node.edit.fields);}
            catch (const std::invalid_argument& error) {return failure(declaration.source_name+": "+declaration.nodes[i].name+": "+error.what());}
            output.front().layout.collapsed=node.control.layout.collapsed;
            output.front().tooltip=node.control.tooltip;
            output.front().classes.insert(output.front().classes.end(),node.control.classes.begin(),node.control.classes.end());
        }
        for (const auto& control:output) if (auto valid=ui::validate_control_properties(control);!valid)
            return failure(declaration.source_name+": "+declaration.nodes[i].name+
                (valid.error().code==ui::ErrorCode::invalid_style?": Invalid UI style.":": Invalid UI text or numeric properties."));
        const auto previous=std::span(data.published).subspan(node.begin,node.count);
        change.next_count=output.size();change.controls_changed=!std::ranges::equal(previous,output);
        update->resized|=output.size()!=node.count;update->control_changes|=change.controls_changed;
        if (!std::ranges::equal(previous,output,{},&ui::Control::key,&ui::Control::key)) {
            keys_changed=true;
            std::vector<ui::ControlKey> before,after;
            for (const auto& item:previous) before.push_back(item.key);
            for (const auto& item:output) after.push_back(item.key);
            std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
            for (const auto key:after) if (!std::binary_search(before.begin(),before.end(),key)) ++update->creations;
            for (const auto key:before) if (!std::binary_search(after.begin(),after.end(),key)) ++update->removals;
        }
        if (change.controls_changed || change.child || change.unmount || !change.additions.empty() || !change.removals.empty() || !change.children.empty())
            update->changes.push_back(std::move(change));
    }
    if (keys_changed) {
        std::vector<ui::ControlKey> keys;std::size_t change{};
        for (std::size_t i=0;i<data.nodes.size();++i) {
            const auto* next=change<update->changes.size() && update->changes[change].node==i?&update->changes[change++]:nullptr;
            const auto values=next && next->controls_changed?std::span<const ui::Control>(next->controls):
                std::span<const ui::Control>(data.published).subspan(data.nodes[i].begin,data.nodes[i].count);
            if (keys.size()+values.size()>composition_limit) return failure("UI composition exceeds 10000 controls.");
            for (const auto& control:values) keys.push_back(control.key);
        }
        std::sort(keys.begin(),keys.end());
        if ((!keys.empty() && !keys.front()) || std::adjacent_find(keys.begin(),keys.end())!=keys.end())
            return failure("UI composition requires distinct nonzero control keys.");
    }
    update->subtree_nodes=budget-remaining;
    if (update->resized) update->append(update->reordered);
    if (replace && value) {
        // A changed definition can reorder/remove declared slots. Account by
        // accepted control identities, not by the new slots' initially empty slices.
        const auto candidate=update->resized?std::span<const ui::Control>(update->reordered):std::span<const ui::Control>(data.published);
        update->control_changes=!std::ranges::equal(value->published,candidate);
        std::vector<ui::ControlKey> before,after;
        for (const auto& control:value->published) before.push_back(control.key);
        for (const auto& control:candidate) after.push_back(control.key);
        std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());update->creations=0;update->removals=0;
        for (const auto key:after) if (!std::binary_search(before.begin(),before.end(),key)) ++update->creations;
        for (const auto key:before) if (!std::binary_search(after.begin(),after.end(),key)) ++update->removals;
    }
    return update;
}

Instance::Instance(std::unique_ptr<detail::InstanceData> data):data_(std::move(data)) {}
Instance::~Instance() {close();}
Result<std::unique_ptr<Instance>> Instance::create(std::shared_ptr<Component> component,ui::ControlKey parent,
    ui::ControlKey& next_key,std::span<const KeyBinding> keys,std::span<const InputValue> inputs) {
    return create_bounded(std::move(component),parent,next_key,keys,inputs,composition_limit,std::make_shared<detail::ScopeLifetime>());
}
Result<std::unique_ptr<Instance>> Instance::create_bounded(std::shared_ptr<Component> component,ui::ControlKey parent,
    ui::ControlKey& next_key,std::span<const KeyBinding> keys,std::span<const InputValue> inputs,std::size_t budget,std::shared_ptr<detail::ScopeLifetime> lifetime) {
    if (!component) return failure("A UI instance needs a compiled component.");
    if (component->declaration().nodes.size()>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
    auto data=std::make_unique<detail::InstanceData>();data->runtime=&component->impl_->runtime;data->next_key=&next_key;data->lifetime=std::move(lifetime);
    auto context=data->runtime->create(*data->runtime->native_type<detail::Context>());
    if (!context) return std::unexpected(context.error());data->context=std::move(*context);++data->lifetime->created;
    auto result=std::unique_ptr<Instance>(new Instance(std::move(data)));auto& scope=*result->data_;
    auto bound=scope.runtime->with_native<detail::Context>(scope.context.id(),[&](auto& value)->Result<void>{value.owner=&scope;return {};});
    if (!bound) return std::unexpected(bound.error());
    auto prepared=scope.prepare(std::move(component),parent,keys,inputs,budget);if (!prepared) return std::unexpected(prepared.error());
    scope.pending=std::move(*prepared);result->commit_controls();return result;
}
Result<void> Instance::update(std::span<const InputValue> values,std::shared_ptr<Component> definition) {
    auto& scope=*data_;if (!scope.open) return failure("UI component is unmounted.");
    if (!definition) definition=scope.value->component;
    const bool source_changed=definition!=scope.value->component;
    if (scope.pending && scope.pending->definition==definition && std::ranges::equal(scope.pending->data.inputs,values)) return {};
    // LiveDocument may reuse the prepared inputs. Keep that borrow alive if a
    // different definition supersedes its owning candidate before commit.
    std::vector<InputValue> retained;
    if (scope.pending && scope.pending->replacement && values.data()==scope.pending->data.inputs.data()) {
        retained.assign(values.begin(),values.end());values=retained;
    }
    scope.pending.reset();
    auto prepared=scope.prepare(std::move(definition),scope.value->parent,scope.value->aliases,values,composition_limit);
    if (!prepared) {reject_controls(source_changed);return std::unexpected(prepared.error());}
    scope.pending=std::move(*prepared);if (scope.pending) scope.pending->materialize();return {};
}
Result<void> Instance::update() {
    if (!data_->open) return failure("UI component is unmounted.");
    return data_->pending?Result<void>{}:update(data_->value->inputs);
}
Result<bool> Instance::event(const ui::Action& action) {
    auto& scope=*data_;if (!scope.open || action.target || action.kind!=ui::ActionKind::activate) return false;
    if (scope.pending) return failure("Commit or reject the prepared UI snapshot before accepting new input.");
    auto& data=*scope.value;
    const auto at=std::ranges::find_if(data.declaration().events,[&](const auto& value){return data.nodes[value.node].control.key==action.key;});
    if (at==data.declaration().events.end()) {
        for (auto& node:data.nodes) {
            const auto dispatch=[&](Instance& child)->Result<bool> {
                auto handled=child.event(action);if (handled && *handled) {node.dirty=true;data.publication_pending=true;}return handled;
            };
            if (node.child) {auto handled=dispatch(*node.child);if (!handled || *handled) return handled;}
            for (auto& [key,child]:node.repeated) {auto handled=dispatch(*child);if (!handled || *handled) return handled;}
        }
        return false;
    }
    bool checked{};
    const auto kind=data.declaration().nodes[at->node].kind;
    if (kind==NodeKind::foldout || kind==NodeKind::toggle_button || kind==NodeKind::checkbox) {
        const auto* value=std::get_if<bool>(&action.value);
        if (!value) return failure(kind==NodeKind::foldout?"Foldout activation requires a bool value.":"ToggleButton activation requires a bool value.");
        checked=*value;
    } else if (!std::holds_alternative<std::monostate>(action.value)) return failure("Button activation does not accept a value.");
    const auto before=data.states;data.handling_event=true;data.event_checked=checked;
    auto handled=data.invoke(at->operation);data.handling_event=false;++scope.statistics.event_evaluations;
    if (!handled) {data.states=before;return std::unexpected(handled.error());}
    if (data.states!=before && !data.state_undo) data.state_undo=before;return true;
}
std::span<const ui::Control> Instance::controls() const noexcept {
    if (data_->pending) return data_->pending->data.published;
    return data_->value?std::span<const ui::Control>(data_->value->published):std::span<const ui::Control>{};
}
Statistics Instance::statistics() const noexcept {
    auto result=data_->statistics;result.instance=data_->context.id();
    result.scope_creations=data_->lifetime->created;result.scope_retirements=data_->lifetime->retired;
    const auto add=[&](const Instance& child) {const auto stats=child.statistics();result.binding_evaluations+=stats.binding_evaluations;result.event_evaluations+=stats.event_evaluations;};
    if (data_->value) for (const auto& node:data_->value->nodes) {if (node.child) add(*node.child);for (const auto& [key,child]:node.repeated) add(*child);}
    return result;
}
void Instance::commit_controls() noexcept {if (data_->pending) {data_->pending->commit();data_->pending.reset();}}
void Instance::reject_controls() noexcept {reject_controls(false);}
void Instance::reject_controls(bool retain_events) noexcept {
    auto& scope=*data_;scope.pending.reset();if (!scope.value) return;auto& data=*scope.value;
    const bool changed=!data.input_undo.empty() || (!retain_events && data.state_undo.has_value());
    for (auto& [index,value]:data.input_undo) data.inputs[index]=std::move(value);data.input_undo.clear();
    if (!retain_events && data.state_undo) {data.states=std::move(*data.state_undo);data.state_undo.reset();}
    if (changed) {std::fill(data.dirty.begin(),data.dirty.end(),true);data.publication_pending=true;for (auto& node:data.nodes) node.dirty=true;}
    for (auto& node:data.nodes) {if (node.child) node.child->reject_controls(retain_events);for (auto& [key,child]:node.repeated) child->reject_controls(retain_events);}
}
std::span<const InputValue> Instance::inputs() const noexcept {return data_->pending?data_->pending->data.inputs:data_->value->inputs;}
bool Instance::has_pending() const noexcept {return data_->pending!=nullptr;}
bool Instance::changes_controls() const noexcept {return data_->pending && data_->pending->control_changes;}
void Instance::close() noexcept {
    if (!data_ || !data_->open) return;auto& scope=*data_;scope.open=false;scope.pending.reset();
    if (scope.value) scope.statistics.node_removals+=scope.value->published.size();scope.value.reset();
    static_cast<void>(scope.runtime->with_native<detail::Context>(scope.context.id(),[](auto& value)->Result<void>{value.owner=nullptr;return {};}));
    static_cast<void>(scope.runtime->destroy(scope.context.id()));scope.context.reset();++scope.lifetime->retired;
}
} // namespace atelier::ui_language
