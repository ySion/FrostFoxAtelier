#include "ui_language/component.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "foundation/string.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <windows.h>

namespace atelier::ui_language {
namespace {
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
}
namespace detail {
struct Context;
struct NodeState {
    ui::Control control;
    ui::ControlKey label{},body{};
    EditBinding edit;
    std::size_t begin{},count{};
    bool dirty{true};
};
struct InstanceData {
    std::shared_ptr<Component> component;
    Runtime* runtime{};
    ObjectRoot context;
    source::Program::Lease code;
    ui::ControlKey parent{};
    std::vector<NodeState> nodes;
    std::vector<InputValue> inputs;
    std::vector<std::variant<bool,double>> states;
    std::vector<std::vector<bool>> dependencies;
    std::vector<bool> dirty,reads;
    std::vector<ui::Control> published;
    Statistics statistics;
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
    Result<void> rebuild();
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
        node.kind=type==NodeKind::text?ui::ControlKind::label:type==NodeKind::foldout?ui::ControlKind::foldout:ui::ControlKind::container;
        node.layout=type==NodeKind::row?ui_layout::NodeInput::row():type==NodeKind::foldout?ui_layout::NodeInput::box(0,28):
            type==NodeKind::text?ui_layout::NodeInput::box(0,0):ui_layout::NodeInput::column();
        node.layout.width=ui_layout::Axis::expand();
        if (type==NodeKind::text) node.layout.height=ui_layout::Axis::content();
    }
    NodeState& target(std::uint32_t slot) {
        auto& d=data(); if (!d.evaluating) throw std::runtime_error("UI properties are owned by bindings.");
        auto& node=d.nodes.at(slot);node.dirty=true;return node;
    }
    void set_text(std::uint32_t slot,std::uint32_t property,const String& value) {
        auto& node=target(slot).control;
        if (static_cast<Property>(property)==Property::text) node.text=wide(value);
        else {
            const auto text=value.view().std_view();
            if (text.size()>128 || text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ")!=text.npos)
                throw std::runtime_error("Invalid UI style classes.");
            std::vector<std::string> classes;
            std::size_t begin{};
            while (begin<text.size()) {
                while (begin<text.size() && text[begin]==' ') ++begin;
                const auto end=text.find(' ',begin);
                if (begin<text.size()) classes.emplace_back(text.substr(begin,end==text.npos?text.size()-begin:end-begin));
                if (end==text.npos) break;
                begin=end+1;
            }
            node.classes=std::move(classes);
        }
    }
    void set_number(std::uint32_t slot,std::uint32_t property,double value) {
        if (!std::isfinite(value) || value<0 || value>32768) throw std::runtime_error("UI layout value is outside its supported range.");
        auto& layout=target(slot).control.layout;
        const auto scalar=static_cast<float>(value);
        switch (static_cast<Property>(property)) {
        case Property::gap: layout.gap=scalar;break;
        case Property::height:layout.height=ui_layout::Axis::fixed(scalar);break;
        case Property::width:layout.width=ui_layout::Axis::fixed(scalar);break;
        case Property::padding_left:layout.padding.left=scalar;break;
        default:throw std::runtime_error("Invalid numeric UI property.");
        }
    }
    void set_bool(std::uint32_t slot,std::uint32_t property,bool value) {
        auto& node=target(slot).control;
        switch (static_cast<Property>(property)) {
        case Property::enabled:node.enabled=value;break;
        case Property::selected:node.selected=value;break;
        case Property::collapsed:node.layout.collapsed=value;break;
        case Property::caption:node.role=value?ui::Role::caption:ui::Role::normal;break;
        default:throw std::runtime_error("Invalid boolean UI property.");
        }
    }
    void set_edit(std::uint32_t slot,std::uint32_t,EditReference reference) {
        auto& d=data(); auto& node=target(slot);
        node.edit=std::get<EditBinding>(d.inputs.at(reference.slot));
    }
};
}

struct Component::Impl {
    Runtime& runtime;
    Declaration declaration;
    source::Program program;
    Impl(Runtime& value,Declaration source,TypeId type):runtime(value),declaration(std::move(source)),program(value,type) {}
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
    using C=detail::Context;
    definitions.push_back(NativeRegistration<C>(runtime,"ui.ComponentContext",TypeKind::object)
        .method<&C::read_bool>("read_bool",true).method<&C::read_number>("read_number",true).method<&C::read_edit>("read_edit",true)
        .method<&C::state_bool>("state_bool",true).method<&C::state_number>("state_number",true)
        .method<&C::write_bool>("write_bool",true).method<&C::write_number>("write_number",true)
        .method<&C::event_bool>("event_bool",true).method<&C::create_node>("create_node",true)
        .method<&C::set_text>("set_text",true).method<&C::set_number>("set_number",true)
        .method<&C::set_bool>("set_bool",true).method<&C::set_edit>("set_edit",true).build());
    auto registered=runtime.register_types(std::move(definitions));
    if (!registered) return std::unexpected(registered.error());
    return registered->back();
}
Result<std::shared_ptr<Component>> Component::compile(Runtime& runtime,std::string_view name,std::string_view source) {
    auto declaration=lower(name,source);
    if (!declaration) return failure(std::string(name)+":"+std::to_string(declaration.error().position.line)+":"+
        std::to_string(declaration.error().position.column)+": "+declaration.error().message);
    const auto type=runtime.native_type<detail::Context>();
    if (!type) return failure("Register UI language types during Runtime bootstrap before creating objects.");
    auto impl=std::make_unique<Impl>(runtime,std::move(*declaration),*type);
    auto request=impl->program.request(impl->declaration.source_name,impl->declaration.generated_source,"__ui_dispatch");
    if (!request) return std::unexpected(request.error());
    auto candidate=source::Program::prepare(*request);
    if (!candidate) return failure(impl->declaration.diagnostic(candidate.error().message));
    auto published=impl->program.try_publish(*candidate);
    if (!published) return std::unexpected(published.error());
    auto signature=impl->program.bind<void,std::uint32_t>();
    if (!signature) return std::unexpected(signature.error());
    return std::shared_ptr<Component>(new Component(std::move(impl)));
}

Result<void> detail::InstanceData::rebuild() {
    struct Change { std::size_t node;std::vector<ui::Control> controls; };
    std::vector<Change> changes;
    bool resized{};
    for (std::size_t i=0;i<nodes.size();++i) {
        const auto& node=nodes[i];
        if (!node.dirty) continue;
        std::vector<ui::Control> next;
        if (declaration().nodes[i].kind!=NodeKind::number_row) next.push_back(node.control);
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
        if (std::ranges::equal(previous,next)) continue;
        resized|=next.size()!=node.count;
        changes.push_back({i,std::move(next)});
    }
    // Prepare all owning copies before changing the published view. Ordinary
    // property changes replace only their node/parameter-row slice.
    std::vector<ui::Control> reordered;
    if (resized) {
        std::size_t change{};
        for (std::size_t i=0;i<nodes.size();++i) {
            const auto values=change<changes.size() && changes[change].node==i?std::span<const ui::Control>(changes[change++].controls):
                std::span<const ui::Control>(published).subspan(nodes[i].begin,nodes[i].count);
            reordered.insert(reordered.end(),values.begin(),values.end());
        }
    }
    for (const auto& change:changes) {
        const auto& node=nodes[change.node];const auto previous=std::span(published).subspan(node.begin,node.count);
        for (const auto& item:change.controls) if (std::ranges::none_of(previous,[&](const auto& old){return item.key==old.key;})) ++statistics.node_creations;
        for (const auto& item:previous) if (std::ranges::none_of(change.controls,[&](const auto& value){return item.key==value.key;})) ++statistics.node_removals;
    }
    static_assert(std::is_nothrow_move_assignable_v<ui::Control>);
    for (auto& change:changes) {
        auto& node=nodes[change.node];node.count=change.controls.size();
        if (!resized) std::move(change.controls.begin(),change.controls.end(),published.begin()+static_cast<std::ptrdiff_t>(node.begin));
    }
    if (resized) {
        published=std::move(reordered);std::size_t begin{};
        for (auto& node:nodes) {node.begin=begin;begin+=node.count;}
    }
    for (auto& node:nodes) node.dirty=false;
    if (!changes.empty()) ++statistics.publications;
    return {};
}

Instance::Instance(std::unique_ptr<detail::InstanceData> data):data_(std::move(data)) {}
Instance::~Instance() {close();}
Result<std::unique_ptr<Instance>> Instance::create(std::shared_ptr<Component> component,ui::ControlKey parent,
    ui::ControlKey& next_key,std::span<const KeyBinding> keys,std::span<const InputValue> inputs) {
    if (!component) return failure("A UI instance needs a compiled component.");
    auto data=std::make_unique<detail::InstanceData>(); data->component=std::move(component);
    data->runtime=&data->component->impl_->runtime;data->parent=parent;
    const auto& declaration=data->declaration();
    data->nodes.resize(declaration.nodes.size());
    const auto allocate=[&] {
        if (!next_key || next_key==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("UI control keys exhausted.");
        return next_key++;
    };
    for (std::size_t i=0;i<data->nodes.size();++i) {
        const auto at=std::ranges::find_if(keys,[&](const auto& key){return key.node==declaration.nodes[i].name;});
        auto& node=data->nodes[i];node.control.key=at!=keys.end() && at->key?at->key:allocate();
        if (declaration.nodes[i].kind==NodeKind::number_row) {
            node.label=at!=keys.end() && at->label?at->label:allocate();
            node.body=at!=keys.end() && at->body?at->body:allocate();
        }
    }
    for (const auto& state:declaration.states)
        data->states.push_back(state.kind==InputKind::boolean?std::variant<bool,double>{false}:std::variant<bool,double>{0.0});
    data->dependencies.resize(declaration.bindings.size(),std::vector<bool>(declaration.inputs.size()+declaration.states.size()));
    data->dirty.resize(declaration.bindings.size(),true);data->reads.resize(declaration.inputs.size()+declaration.states.size());
    auto code=data->component->impl_->program.pin();if (!code) return std::unexpected(code.error());data->code=std::move(*code);
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
    if (auto updated=result->update();!updated) return std::unexpected(updated.error());
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
    if (std::ranges::equal(inputs,d.inputs)) return {};
    std::vector<InputValue> next(inputs.begin(),inputs.end());
    if (d.inputs.size()==next.size()) for (std::size_t i=0;i<next.size();++i) if (next[i]!=d.inputs[i]) d.invalidate(i);
    d.inputs=std::move(next);return {};
}
Result<void> Instance::update() {
    auto& d=*data_;if (!d.open) return failure("UI component is unmounted.");
    bool changed{};
    for (std::size_t i=0;i<d.dirty.size();++i) if (d.dirty[i]) {
        std::fill(d.reads.begin(),d.reads.end(),false);d.evaluating=true;
        auto evaluated=d.invoke(d.declaration().bindings[i].operation);d.evaluating=false;
        ++d.statistics.binding_evaluations;
        if (!evaluated) return evaluated;
        d.dependencies[i]=d.reads;d.dirty[i]=false;changed=true;
    }
    d.publication_pending|=changed;
    if (!d.publication_pending) return {};
    auto published=d.rebuild();
    if (published) d.publication_pending=false;
    return published;
}
Result<bool> Instance::event(const ui::Action& action) {
    auto& d=*data_;if (!d.open) return false;
    if (action.kind!=ui::ActionKind::activate) return false;
    const auto at=std::ranges::find_if(d.declaration().events,[&](const auto& value){return d.nodes[value.node].control.key==action.key;});
    if (at==d.declaration().events.end()) return false;
    const auto* checked=std::get_if<bool>(&action.value);if (!checked) return failure("Foldout activation requires a bool value.");
    const auto before=d.states;d.handling_event=true;d.event_checked=*checked;
    auto handled=d.invoke(at->operation);d.handling_event=false;++d.statistics.event_evaluations;
    if (!handled) {d.states=before;return std::unexpected(handled.error());}
    return true;
}
std::span<const ui::Control> Instance::controls() const noexcept {return data_->published;}
Statistics Instance::statistics() const noexcept {return data_->statistics;}
void Instance::close() noexcept {
    if (!data_ || !data_->open) return;
    auto& d=*data_;d.open=false;
    static_cast<void>(d.runtime->with_native<detail::Context>(d.context.id(),[](auto& value)->Result<void>{value.owner=nullptr;return {};}));
    static_cast<void>(d.runtime->destroy(d.context.id()));d.context.reset();
    d.code={};d.statistics.node_removals+=d.published.size();d.published.clear();d.dependencies.clear();d.dirty.clear();
}
} // namespace atelier::ui_language
