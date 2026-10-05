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
InputValue initial(InputKind kind) {
    switch (kind) {
    case InputKind::boolean:return false;
    case InputKind::number:return 0.0;
    case InputKind::edit:return EditBinding{};
    case InputKind::text:return std::wstring{};
    case InputKind::text_edit:return TextEditBinding{};
    case InputKind::key:return ui::ControlKey{};
    case InputKind::items:return ItemList{};
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
    std::shared_ptr<Component> type;
    std::vector<InputValue> arguments;
    ItemList items;
    bool mounted{true};
    std::unique_ptr<Instance> child;
    std::map<ui::ControlKey,std::unique_ptr<Instance>> repeated;
    const NodeState* previous{};
    std::size_t begin{},count{};
    bool dirty{true};
};
struct InstanceData {
    std::shared_ptr<Component> component;
    Runtime* runtime{};
    ObjectRoot context;
    source::Program::Lease code;
    ui::ControlKey parent{};
    ui::ControlKey* next_key{};
    std::unique_ptr<PreparedUpdate> pending;
    std::vector<NodeState> nodes;
    std::vector<InputValue> inputs;
    std::vector<std::pair<std::size_t,InputValue>> input_undo;
    std::vector<std::variant<bool,double>> states;
    std::optional<std::vector<std::variant<bool,double>>> state_undo;
    std::vector<std::vector<bool>> dependencies;
    std::vector<bool> dirty,reads;
    std::vector<ui::Control> published;
    Statistics statistics;
    std::size_t subtree_nodes{};
    bool evaluating{},initializing{},handling_event{},event_checked{},open{true},publication_pending{true};
    const Declaration& declaration() const { return component->declaration(); }
    void read(std::size_t index) { if (evaluating) reads.at(index)=true; }
    void invalidate(std::size_t index) {
        for (std::size_t i=0;i<dependencies.size();++i) if (dependencies[i][index]) dirty[i]=true;
    }
    Result<void> invoke(std::uint32_t operation) {
        const Scalar argument=operation;
        auto result=code.invoke(context.id(),std::span{&argument,1});
        if (!result) return failure(declaration().diagnostic(result.error().message));
        return {};
    }
    Result<std::unique_ptr<PreparedUpdate>> prepare(std::size_t budget);
};

// Source's self is this UI scope, never an Actor wrapper. All callbacks copy
// borrowed data and finish synchronously; none constructs/destroys Runtime
// objects, reenters JIT, or executes World/editor commands.
struct Context {
    InstanceData* owner{};
    InstanceData& data() const {
        if (!owner || !owner->open) throw std::runtime_error("UI component has been unmounted.");
        return *owner;
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
        node.parent=parent?d.nodes.at(parent-1).control.key:d.parent;
        const auto type=static_cast<NodeKind>(kind);
        node.kind=type==NodeKind::text?ui::ControlKind::label:type==NodeKind::foldout?ui::ControlKind::foldout:
            type==NodeKind::text_field?ui::ControlKind::text_edit:type==NodeKind::tree?ui::ControlKind::tree:
            type==NodeKind::tree_item?ui::ControlKind::tree_item:ui::ControlKind::container;
        node.layout=type==NodeKind::row?ui_layout::NodeInput::row():type==NodeKind::foldout?ui_layout::NodeInput::box(0,28):
            type==NodeKind::text?ui_layout::NodeInput::box(0,0):ui_layout::NodeInput::column();
        node.layout.width=ui_layout::Axis::expand();
        if (type==NodeKind::text) node.layout.height=ui_layout::Axis::content();
        if (type==NodeKind::text_field) {node.layout=ui::single_line_layout();node.layout.width=ui_layout::Axis::expand();}
        if (type==NodeKind::tree_item) node.layout.height=ui_layout::Axis::fixed(20);
    }
    NodeState& target(std::uint32_t slot) {
        auto& d=data(); if (!d.evaluating) throw std::runtime_error("UI properties are owned by bindings.");
        auto& node=d.nodes.at(slot);node.dirty=true;return node;
    }
    static void text(ui::Control& node,std::uint32_t property,std::wstring value) {
        if (static_cast<Property>(property)==Property::text) node.text=std::move(value);
        else if (static_cast<Property>(property)==Property::filter) node.tree_filter=std::move(value);
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
    void set_tree_parent(std::uint32_t slot,std::uint32_t,KeyReference reference) { target(slot).control.tree_parent=key(reference); }
    void set_items(std::uint32_t slot,std::uint32_t,ItemsReference reference) {
        auto& d=data();target(slot).items=std::get<ItemList>(d.inputs.at(reference.slot));
    }
    void set_number(std::uint32_t slot,std::uint32_t property,double value) {
        if (!std::isfinite(value) || value<0 || value>32768) throw std::runtime_error("UI layout value is outside its supported range.");
        auto& node=target(slot).control;
        if (static_cast<Property>(property)==Property::icon) {
            if (std::floor(value)!=value || value>static_cast<double>(ui::ControlIcon::folder)) throw std::runtime_error("Invalid UI icon.");
            node.icon=static_cast<ui::ControlIcon>(static_cast<unsigned>(value));return;
        }
        auto& layout=node.layout;
        const auto scalar=static_cast<float>(value);
        switch (static_cast<Property>(property)) {
        case Property::gap: layout.gap=scalar;break;
        case Property::height:layout.height=ui_layout::Axis::fixed(scalar);break;
        case Property::width:layout.width=ui_layout::Axis::fixed(scalar);break;
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
        case Property::enabled:node.enabled=value;break;
        case Property::selected:node.selected=value;break;
        case Property::collapsed:node.layout.collapsed=value;break;
        case Property::caption:node.role=value?ui::Role::caption:ui::Role::normal;break;
        case Property::expand_height:node.layout.height=value?ui_layout::Axis::expand():ui_layout::Axis::content();break;
        case Property::expanded:node.tree_initially_expanded=value;break;
        case Property::mounted:state.mounted=value;break;
        case Property::align_center:
            if (!node.layout.flow) node.layout.flow=ui_layout::FlexSpec{};
            node.layout.flow->align_items=ui_layout::Alignment::fit(value?.5f:0.f);break;
        case Property::single_line:node.layout.height=ui_layout::Axis::content(value?ui::single_line_height:0.f);break;
        default:throw std::runtime_error("Invalid boolean UI property.");
        }
    }
    void set_edit(std::uint32_t slot,std::uint32_t,EditReference reference) {
        auto& d=data(); auto& node=target(slot);
        node.edit=std::get<EditBinding>(d.inputs.at(reference.slot));
    }
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
        .method<&C::node_key>("node_key",true)
        .method<&C::set_text_reference>("set_text_reference",true)
        .method<&C::set_text_from_edit>("set_text_from_edit",true)
        .method<&C::set_text_edit>("set_text_edit",true)
        .method<&C::set_tree_parent>("set_tree_parent",true)
        .method<&C::set_items>("set_items",true)
        .method<&C::set_argument_bool>("set_argument_bool",true)
        .method<&C::set_argument_number>("set_argument_number",true)
        .method<&C::set_argument_text>("set_argument_text",true)
        .method<&C::set_argument_key>("set_argument_key",true)
        .method<&C::set_argument_edit>("set_argument_edit",true)
        .method<&C::set_argument_text_reference>("set_argument_text_reference",true)
        .method<&C::set_argument_text_edit>("set_argument_text_edit",true)
        .method<&C::set_argument_items>("set_argument_items",true)
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

// Preparation may evaluate values, but never removes an accepted scope. Child
// plans belong to their parent plan; a late failure drops the whole prepared
// tree. Only the root materializes its contiguous borrowed view before Session.
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
    InstanceData& data;
    std::vector<Change> changes;
    std::vector<ui::Control> reordered;
    std::uint64_t creations{},removals{};
    std::size_t subtree_nodes{};
    bool resized{},control_changes{},materialized{},committed{};
    explicit PreparedUpdate(InstanceData& value):data(value) {}
    ~PreparedUpdate() { if (materialized && !committed) swap_view(); }
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
    void materialize() noexcept { if (!materialized) {swap_view();materialized=true;} }
    void commit() noexcept {
        for (auto& change:changes) {
            for (auto& child:change.children) child->commit();
            auto& node=data.nodes[change.node];
            if (change.unmount) node.child.reset();
            if (change.child) node.child=std::move(change.child);
            for (const auto key:change.removals) node.repeated.erase(key);
            node.repeated.merge(change.additions); // Existing allocator and noexcept integral comparator; no allocation.
        }
        materialize();
        for (const auto& change:changes) if (change.controls_changed) data.nodes[change.node].count=change.next_count;
        std::size_t begin{};
        for (auto& node:data.nodes) {node.begin=begin;begin+=node.count;node.dirty=false;}
        data.statistics.node_creations+=creations;data.statistics.node_removals+=removals;
        if (control_changes) ++data.statistics.publications;
        data.subtree_nodes=subtree_nodes;
        data.input_undo.clear();data.state_undo.reset();data.publication_pending=false;committed=true;
    }
};
Result<std::unique_ptr<detail::PreparedUpdate>> detail::InstanceData::prepare(std::size_t budget) {
    if (nodes.size()>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
    for (std::size_t i=0;i<dirty.size();++i) if (dirty[i]) {
        std::fill(reads.begin(),reads.end(),false);evaluating=true;
        auto evaluated=invoke(declaration().bindings[i].operation);evaluating=false;
        ++statistics.binding_evaluations;
        if (!evaluated) return std::unexpected(evaluated.error());
        dependencies[i]=reads;dirty[i]=false;publication_pending=true;
    }
    if (!publication_pending && input_undo.empty() && !state_undo) {
        if (subtree_nodes>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
        return std::unique_ptr<PreparedUpdate>{};
    }
    auto update=std::make_unique<PreparedUpdate>(*this);
    auto remaining=budget-nodes.size();
    bool keys_changed{};
    for (std::size_t i=0;i<nodes.size();++i) {
        auto& node=nodes[i];
        if (!node.dirty) {
            auto retained=node.child?node.child->data_->subtree_nodes:0;
            for (const auto& [key,child]:node.repeated) retained+=child->data_->subtree_nodes;
            if (retained>remaining) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
            remaining-=retained;continue;
        }
        PreparedUpdate::Change change;change.node=i;
        auto& next=change.controls;
        const auto kind=declaration().nodes[i].kind;
        const auto append_new=[&](const Instance& child) {
            const auto controls=child.controls();next.insert(next.end(),controls.begin(),controls.end());
        };
        const auto append_existing=[&](Instance& child,std::span<const InputValue> values)->Result<void> {
            if (auto published=child.publish(values);!published) return published;
            auto prepared=child.data_->prepare(remaining);if (!prepared) return std::unexpected(prepared.error());
            remaining-=(*prepared?(*prepared)->subtree_nodes:child.data_->subtree_nodes);
            if (*prepared) {(*prepared)->append(next);change.children.push_back(std::move(*prepared));}
            else append_new(child);
            return {};
        };
        if (kind==NodeKind::component) {
            if (!node.mounted) change.unmount=node.child!=nullptr;
            else if (node.child) {
                if (auto result=append_existing(*node.child,node.arguments);!result) return std::unexpected(result.error());
            } else {
                auto child=Instance::create_bounded(node.type,node.control.parent,*next_key,{},node.arguments,
                    node.previous?node.previous->child.get():nullptr,remaining);
                if (!child) return std::unexpected(child.error());
                remaining-=(*child)->data_->subtree_nodes;
                change.child=std::move(*child);append_new(*change.child);
            }
        } else if (kind==NodeKind::repeat) {
            std::vector<ui::ControlKey> present;
            if (node.mounted && node.items.values) {
                if (node.items.values->size()>10000) return failure("UI repeat exceeds 10000 items.");
                present.reserve(node.items.values->size());
                for (const auto& item:*node.items.values) present.push_back(item.key);
                std::sort(present.begin(),present.end());
                if ((!present.empty() && !present.front()) || std::adjacent_find(present.begin(),present.end())!=present.end())
                    return failure("UI repeat requires distinct nonzero item keys.");
                for (const auto& item:*node.items.values) {
                    const auto at=node.repeated.find(item.key);
                    if (at==node.repeated.end()) {
                        const Instance* previous{};
                        if (node.previous) if (const auto old=node.previous->repeated.find(item.key);old!=node.previous->repeated.end()) previous=old->second.get();
                        const KeyBinding key{node.type->declaration().nodes.front().name,item.key};
                        auto child=Instance::create_bounded(node.type,node.control.parent,*next_key,std::span{&key,1},item.inputs,previous,remaining);
                        if (!child) return std::unexpected(child.error());
                        remaining-=(*child)->data_->subtree_nodes;
                        append_new(**child);change.additions.emplace(item.key,std::move(*child));
                    } else if (auto result=append_existing(*at->second,item.inputs);!result) return std::unexpected(result.error());
                    if (next.size()>10000) return failure("UI composition exceeds 10000 controls.");
                }
            }
            for (const auto& [key,child]:node.repeated) if (!std::binary_search(present.begin(),present.end(),key)) change.removals.push_back(key);
        } else if (kind==NodeKind::text_field) {
            auto control=node.control;control.key=node.text_edit.key;control.text=node.text_edit.text;control.enabled&=node.text_edit.enabled;
            if (!control.key) return failure("TextField requires a nonzero host edit key.");
            next.push_back(std::move(control));
        } else if (kind!=NodeKind::number_row) next.push_back(node.control);
        else if (!node.edit.fields.empty()) {
            try {
                ui::append_number_row(next,{node.control.key,node.label,node.body,node.edit.group,node.control.parent,
                    node.control.text,node.control.enabled && node.edit.enabled,node.edit.modified},node.edit.fields);
            } catch (const std::invalid_argument& error) {
                return failure(declaration().source_name+": "+declaration().nodes[i].name+": "+error.what());
            }
            next.front().layout.collapsed=node.control.layout.collapsed;
            next.front().classes.insert(next.front().classes.end(),node.control.classes.begin(),node.control.classes.end());
        }
        for (const auto& control:next) if (auto valid=ui::validate_control_properties(control);!valid)
            return failure(declaration().source_name+": "+declaration().nodes[i].name+
                (valid.error().code==ui::ErrorCode::invalid_style?": Invalid UI style.":": Invalid UI text or numeric properties."));
        const auto previous=std::span(published).subspan(node.begin,node.count);
        change.next_count=next.size();change.controls_changed=!std::ranges::equal(previous,next);
        update->resized|=next.size()!=node.count;update->control_changes|=change.controls_changed;
        if (!std::ranges::equal(previous,next,{},&ui::Control::key,&ui::Control::key)) {
            keys_changed=true;
            std::vector<ui::ControlKey> before,after;before.reserve(previous.size());after.reserve(next.size());
            for (const auto& item:previous) before.push_back(item.key);
            for (const auto& item:next) after.push_back(item.key);
            std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
            for (const auto key:after) if (!std::binary_search(before.begin(),before.end(),key)) ++update->creations;
            for (const auto key:before) if (!std::binary_search(after.begin(),after.end(),key)) ++update->removals;
        }
        if (change.controls_changed || change.child || change.unmount || !change.additions.empty() || !change.removals.empty() || !change.children.empty())
            update->changes.push_back(std::move(change));
    }
    if (keys_changed) {
        std::vector<ui::ControlKey> keys;std::size_t change{};
        for (std::size_t i=0;i<nodes.size();++i) {
            const auto* next=change<update->changes.size() && update->changes[change].node==i?&update->changes[change++]:nullptr;
            const auto values=next && next->controls_changed?std::span<const ui::Control>(next->controls):
                std::span<const ui::Control>(published).subspan(nodes[i].begin,nodes[i].count);
            if (keys.size()+values.size()>10000) return failure("UI composition exceeds 10000 controls.");
            for (const auto& control:values) keys.push_back(control.key);
        }
        std::sort(keys.begin(),keys.end());
        if ((!keys.empty() && !keys.front()) || std::adjacent_find(keys.begin(),keys.end())!=keys.end())
            return failure("UI composition requires distinct nonzero control keys.");
    }
    update->subtree_nodes=budget-remaining;
    if (update->resized) update->append(update->reordered);
    return update;
}

Instance::Instance(std::unique_ptr<detail::InstanceData> data):data_(std::move(data)) {}
Instance::~Instance() {close();}
Result<std::unique_ptr<Instance>> Instance::create(std::shared_ptr<Component> component,ui::ControlKey parent,
    ui::ControlKey& next_key,std::span<const KeyBinding> keys,std::span<const InputValue> inputs,const Instance* previous) {
    return create_bounded(std::move(component),parent,next_key,keys,inputs,previous,composition_limit);
}
Result<std::unique_ptr<Instance>> Instance::create_bounded(std::shared_ptr<Component> component,ui::ControlKey parent,
    ui::ControlKey& next_key,std::span<const KeyBinding> keys,std::span<const InputValue> inputs,const Instance* previous,std::size_t budget) {
    if (!component) return failure("A UI instance needs a compiled component.");
    if (component->declaration().nodes.size()>budget) return failure("UI composition exceeds 10000 instantiated declaration nodes.");
    auto data=std::make_unique<detail::InstanceData>(); data->component=std::move(component);
    data->runtime=&data->component->impl_->runtime;data->parent=parent;data->next_key=&next_key;
    const auto& declaration=data->declaration();
    const auto* old=previous && previous->data_->open && previous->data_->runtime==data->runtime &&
        previous->data_->declaration().name==declaration.name?previous->data_.get():nullptr;
    data->nodes.resize(declaration.nodes.size());
    const auto allocate=[&] {
        if (!next_key || next_key==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("UI control keys exhausted.");
        return next_key++;
    };
    for (std::size_t i=0;i<data->nodes.size();++i) {
        const auto at=std::ranges::find_if(keys,[&](const auto& key){return key.node==declaration.nodes[i].name;});
        auto& node=data->nodes[i];
        if (old) {
            const auto found=std::ranges::find_if(old->declaration().nodes,[&](const auto& value) {
                return value.name==declaration.nodes[i].name && value.kind==declaration.nodes[i].kind && value.component_type==declaration.nodes[i].component_type;
            });
            if (found!=old->declaration().nodes.end()) node.previous=&old->nodes[static_cast<std::size_t>(found-old->declaration().nodes.begin())];
        }
        node.control.key=at!=keys.end() && at->key?at->key:node.previous?node.previous->control.key:allocate();
        node.type=data->component->impl_->children[i];
        if (node.type) for (const auto& input:node.type->declaration().inputs) node.arguments.push_back(initial(input.kind));
        if (declaration.nodes[i].kind==NodeKind::number_row) {
            node.label=at!=keys.end() && at->label?at->label:node.previous?node.previous->label:allocate();
            node.body=at!=keys.end() && at->body?at->body:node.previous?node.previous->body:allocate();
        }
    }
    for (const auto& state:declaration.states)
        data->states.push_back(state.kind==InputKind::boolean?std::variant<bool,double>{false}:std::variant<bool,double>{0.0});
    data->dependencies.resize(declaration.bindings.size(),std::vector<bool>(declaration.inputs.size()+declaration.states.size()));
    data->dirty.resize(declaration.bindings.size(),true);data->reads.resize(declaration.inputs.size()+declaration.states.size());
    auto code=data->component->impl_->program->pin();if (!code) return std::unexpected(code.error());data->code=std::move(*code);
    auto context=data->runtime->create(*data->runtime->native_type<detail::Context>());
    if (!context) return std::unexpected(context.error());data->context=std::move(*context);
    auto result=std::unique_ptr<Instance>(new Instance(std::move(data)));
    auto& mounted=*result->data_;
    auto bound=mounted.runtime->with_native<detail::Context>(mounted.context.id(),[&](auto& value)->Result<void>{value.owner=&mounted;return {};});
    if (!bound) return std::unexpected(bound.error());
    if (auto published=result->publish(inputs);!published) return std::unexpected(published.error());
    result->data_->initializing=true;
    auto initialized=result->data_->invoke(0);result->data_->initializing=false;
    if (!initialized) return std::unexpected(initialized.error());
    if (old) for (std::size_t i=0;i<declaration.states.size();++i) {
        const auto& state=declaration.states[i];
        const auto found=std::ranges::find_if(old->declaration().states,[&](const auto& value){return value.name==state.name && value.kind==state.kind;});
        if (found!=old->declaration().states.end()) mounted.states[i]=old->states[static_cast<std::size_t>(found-old->declaration().states.begin())];
    }
    auto prepared=mounted.prepare(budget);if (!prepared) return std::unexpected(prepared.error());
    mounted.pending=std::move(*prepared);
    result->commit_controls(); // A newly constructed scope has never been exposed to Session.
    for (auto& node:mounted.nodes) node.previous=nullptr;
    return result;
}
Result<void> Instance::publish(std::span<const InputValue> inputs) {
    auto& d=*data_;if (!d.open) return failure("UI component is unmounted.");
    const auto& definitions=d.declaration().inputs;
    if (inputs.size()!=definitions.size()) return failure("UI component input count does not match its declaration.");
    for (std::size_t i=0;i<inputs.size();++i) {
        if (inputs[i].index()!=static_cast<std::size_t>(definitions[i].kind)) return failure("UI component input type mismatch: "+definitions[i].name);
        if (const auto* number=std::get_if<double>(&inputs[i]);number && !std::isfinite(*number)) return failure("UI numeric inputs must be finite.");
    }
    if (d.inputs.size()!=inputs.size()) {
        d.pending.reset();d.inputs.assign(inputs.begin(),inputs.end());return {};
    }
    std::vector<std::pair<std::size_t,InputValue>> changes;
    for (std::size_t i=0;i<inputs.size();++i) if (inputs[i]!=d.inputs[i]) changes.emplace_back(i,inputs[i]);
    if (changes.empty()) return {};
    d.input_undo.reserve(d.input_undo.size()+changes.size()); // Allocate before mutating the current view or inputs.
    d.pending.reset();
    for (auto& [index,value]:changes) {
        if (std::ranges::none_of(d.input_undo,[&](const auto& prior){return prior.first==index;}))
            d.input_undo.emplace_back(index,std::move(d.inputs[index]));
        d.inputs[index]=std::move(value);d.invalidate(index);
    }
    return {};
}
Result<void> Instance::update() {
    auto& d=*data_;if (!d.open) return failure("UI component is unmounted.");
    if (d.pending) return {};
    auto update=d.prepare(composition_limit);
    if (!update) {reject_controls();return std::unexpected(update.error());}
    d.pending=std::move(*update);if (d.pending) d.pending->materialize();
    return {};
}
Result<bool> Instance::event(const ui::Action& action) {
    auto& d=*data_;if (!d.open) return false;
    if (action.kind!=ui::ActionKind::activate) return false;
    if (d.pending) return failure("Commit or reject the prepared UI snapshot before accepting new input.");
    const auto at=std::ranges::find_if(d.declaration().events,[&](const auto& value){return d.nodes[value.node].control.key==action.key;});
    if (at==d.declaration().events.end()) {
        for (auto& node:d.nodes) {
            const auto dispatch=[&](Instance& child)->Result<bool> {
                auto handled=child.event(action);
                if (handled && *handled) {node.dirty=true;d.publication_pending=true;}
                return handled;
            };
            if (node.child) {auto handled=dispatch(*node.child);if (!handled || *handled) return handled;}
            for (auto& [key,child]:node.repeated) {auto handled=dispatch(*child);if (!handled || *handled) return handled;}
        }
        return false;
    }
    const auto* checked=std::get_if<bool>(&action.value);if (!checked) return failure("Foldout activation requires a bool value.");
    const auto before=d.states;d.handling_event=true;d.event_checked=*checked;
    auto handled=d.invoke(at->operation);d.handling_event=false;++d.statistics.event_evaluations;
    if (!handled) {d.states=before;return std::unexpected(handled.error());}
    if (d.states!=before && !d.state_undo) d.state_undo=before;
    return true;
}
std::span<const ui::Control> Instance::controls() const noexcept {return data_->published;}
Statistics Instance::statistics() const noexcept {
    auto result=data_->statistics;
    const auto add=[&](const Instance& child) {const auto stats=child.statistics();result.binding_evaluations+=stats.binding_evaluations;result.event_evaluations+=stats.event_evaluations;};
    for (const auto& node:data_->nodes) {if (node.child) add(*node.child);for (const auto& [key,child]:node.repeated) add(*child);}
    return result;
}
void Instance::commit_controls() noexcept {
    if (data_->pending) {data_->pending->commit();data_->pending.reset();}
}
void Instance::reject_controls() noexcept {
    auto& d=*data_;d.pending.reset();
    const bool changed=!d.input_undo.empty() || d.state_undo.has_value();
    for (auto& [index,value]:d.input_undo) d.inputs[index]=std::move(value);
    d.input_undo.clear();
    if (d.state_undo) {d.states=std::move(*d.state_undo);d.state_undo.reset();}
    if (changed) {std::fill(d.dirty.begin(),d.dirty.end(),true);d.publication_pending=true;for (auto& node:d.nodes) node.dirty=true;}
    for (auto& node:d.nodes) {
        if (node.child) node.child->reject_controls();
        for (auto& [key,child]:node.repeated) child->reject_controls();
    }
}
std::span<const InputValue> Instance::inputs() const noexcept {return data_->inputs;}
bool Instance::has_pending() const noexcept {return data_->pending!=nullptr;}
bool Instance::changes_controls() const noexcept {return data_->pending && data_->pending->control_changes;}

void Instance::close() noexcept {
    if (!data_ || !data_->open) return;
    auto& d=*data_;d.open=false;d.pending.reset();
    for (auto& node:d.nodes) {node.child.reset();node.repeated.clear();}
    static_cast<void>(d.runtime->with_native<detail::Context>(d.context.id(),[](auto& value)->Result<void>{value.owner=nullptr;return {};}));
    static_cast<void>(d.runtime->destroy(d.context.id()));d.context.reset();
    d.code={};d.statistics.node_removals+=d.published.size();d.published.clear();d.dependencies.clear();d.dirty.clear();
    d.nodes.clear();d.inputs.clear();d.input_undo.clear();d.states.clear();d.state_undo.reset();d.component.reset();
}
} // namespace atelier::ui_language
