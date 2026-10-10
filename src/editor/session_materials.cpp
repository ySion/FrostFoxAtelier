#include "editor/session.hpp"
#include "editor/project.hpp"

namespace atelier::editor {
namespace {
auto failure(std::string text) {return std::unexpected(Error{ErrorCode::invalid_definition,std::move(text)});}
bool same(const material::State& a,const material::State& b) {
    if(a.source!=b.source || a.description!=b.description || a.parent.reference()!=b.parent.reference() || a.overrides!=b.overrides) return false;
    for(std::size_t i=0;i<a.textures.size();++i) if(a.textures[i].reference()!=b.textures[i].reference()) return false;
    return true;
}
}
Result<ObjectRoot> Session::texture_resource(Project& project,std::string_view key) {
    if(const auto at=textures_.find(std::string(key));at!=textures_.end()) return at->second;
    auto asset=project.texture(key);if(!asset) return std::unexpected(asset.error());
    auto root=material::create_texture(runtime_,key,asset->image);if(!root) return std::unexpected(root.error());
    textures_.emplace(std::string(key),*root);return std::move(*root);
}
Result<ObjectRoot> Session::material_resource(Project& project,std::string_view key) {
    if(const auto at=materials_.find(std::string(key));at!=materials_.end()) return at->second.root;
    auto asset=project.material(key);if(!asset) return std::unexpected(asset.error());
    return material_resource(project,*asset);
}
Result<ObjectRoot> Session::material_resource(Project& project,const MaterialAsset& asset) {
    if(!asset.key.empty()) if(const auto at=materials_.find(asset.key);at!=materials_.end()) return at->second.root;
    material::State state;state.source=asset.key;state.description=asset.description;
    for(std::size_t i=0;i<state.textures.size();++i) if(!asset.textures[i].empty()) {
        auto texture=texture_resource(project,asset.textures[i]);if(!texture) return std::unexpected(texture.error());
        state.textures[i]=std::move(*texture);auto captured=material::read_texture(runtime_,state.textures[i].id());
        if(!captured) return std::unexpected(captured.error());state.description.textures[i].image=captured->image;
    }
    auto root=material::create_material(runtime_,state);if(!root) return std::unexpected(root.error());
    if(!asset.key.empty()) materials_.emplace(asset.key,MaterialDocument{*root,state,{state},0});
    return std::move(*root);
}
Result<MaterialAsset> Session::material_asset(ObjectId object) const {
    auto& runtime=world_.runtime();auto state=material::read_material(runtime,object);if(!state) return std::unexpected(state.error());
    MaterialAsset result;result.key=state->source;result.description=state->description;
    for(std::size_t i=0;i<state->textures.size();++i) if(state->textures[i].registered()) {
        auto texture=material::read_texture(runtime,state->textures[i].id());if(!texture) return std::unexpected(texture.error());
        if(texture->source.empty()) return failure("A material texture has no persistent source.");
        result.textures[i]=texture->source;
    }
    return result;
}
Result<std::vector<MaterialAsset>> Session::material_assets() const {
    std::vector<MaterialAsset> result;result.reserve(materials_.size());
    for(const auto& [key,document]:materials_) {static_cast<void>(key);auto asset=material_asset(document.root.id());if(!asset) return std::unexpected(asset.error());result.push_back(std::move(*asset));}
    return result;
}
std::vector<ObjectId> Session::material_objects() const {
    std::vector<ObjectId> result;result.reserve(materials_.size());
    for(const auto& [key,document]:materials_) {static_cast<void>(key);result.push_back(document.root.id());}return result;
}
Result<void> Session::begin_material_edit(ObjectId object,std::uint64_t control) {
    if(world_.closed() || world_.running()) return failure("Stop the World before editing a material asset.");
    if(material_edit_ && material_edit_->root.id()==object && material_edit_->control==control) return {};
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    auto state=material::read_material(runtime_,object);if(!state) return std::unexpected(state.error());
    if(state->parent.registered() || state->source.empty()) {
        ObjectId owner,component;
        const auto members=world_.members();if(!members) return std::unexpected(members.error());
        for(const auto& actor:*members) {
            auto parts=world::components(runtime_,actor.id());if(!parts) return std::unexpected(parts.error());
            for(const auto& part:*parts) {
                auto bindings=world::read_material_bindings(runtime_,part.id());if(!bindings) continue;
                for(const auto& binding:*bindings) if(binding.resource.id()==object) {
                    if(component!=ObjectId{} && component!=part.id()) return failure("A material instance cannot be edited across multiple components.");
                    owner=actor.id();component=part.id();
                }
            }
        }
        if(component==ObjectId{}) return failure("The material instance is not attached to an editable component.");
        if(auto admitted=admit_component_edit(component);!admitted) return std::unexpected(admitted.error());
        auto held=runtime_.hold(object);if(!held) return std::unexpected(held.error());
        material_edit_=MaterialEdit{std::move(*held),std::move(*state),control,{},owner,component};return {};
    }
    const auto at=materials_.find(state->source);
    if(at==materials_.end() || at->second.root.id()!=object) return failure("Open the persistent material asset before editing it.");
    material_edit_=MaterialEdit{at->second.root,std::move(*state),control,{},{},{}};return {};
}
Result<void> Session::preview_material(std::uint64_t control,const material::State& state) {
    if(!material_edit_ || material_edit_->control!=control) return failure("No matching material edit.");
    if(state.source!=material_edit_->before.source) return failure("A material edit cannot replace its identity.");
    auto result=material::update_material(runtime_,material_edit_->root.id(),state);if(result) ++revision_;return result;
}
Result<void> Session::commit_material_edit(std::uint64_t control) {
    if(!material_edit_ || material_edit_->control!=control) return failure("No matching material edit.");
    auto state=material::read_material(runtime_,material_edit_->root.id());if(!state) return std::unexpected(state.error());
    if(material_edit_->component!=ObjectId{}) {
        if(!same(*state,material_edit_->before)) remember(MaterialValueChange{material_edit_->root,material_edit_->actor,
            material_edit_->component,material_edit_->before,std::move(*state)});
        material_edit_.reset();return {};
    }
    auto& document=materials_.at(state->source);
    if(!same(*state,material_edit_->before)) {
        document.history.erase(document.history.begin()+static_cast<std::ptrdiff_t>(document.cursor+1),document.history.end());
        document.history.push_back(std::move(*state));++document.cursor;
        if(document.history.size()>history_limit+1) {document.history.erase(document.history.begin());--document.cursor;}
        ++revision_;
    }
    material_edit_.reset();return {};
}
Result<void> Session::cancel_material_edit() {
    if(!material_edit_) return {};
    auto restored=material::update_material(runtime_,material_edit_->root.id(),material_edit_->before);
    if(!restored) return restored;material_edit_.reset();++revision_;return {};
}
bool Session::material_editing(std::uint64_t control) const noexcept {return material_edit_ && material_edit_->control==control;}
bool Session::is_material(ObjectId object) const {
    const auto type=runtime_.type_of(object),base=runtime_.native_type<material::MaterialResource>();
    if(!type || !base) return false;const auto matches=runtime_.is_a(*type,*base);return matches && *matches;
}
bool Session::material_can_undo(std::string_view key,bool redo) const {
    const auto at=materials_.find(std::string(key));
    return at!=materials_.end() && (redo?at->second.cursor+1<at->second.history.size():at->second.cursor>0);
}
ObjectId Session::loaded_material(std::string_view key) const {
    const auto at=materials_.find(std::string(key));return at==materials_.end()?ObjectId{}:at->second.root.id();
}
bool Session::material_dirty(std::string_view key) const {
    const auto at=materials_.find(std::string(key));if(at==materials_.end()) return false;
    if(material_edit_ && material_edit_->root.id()==at->second.root.id()) return true;
    return !same(at->second.saved,at->second.history[at->second.cursor]);
}
std::vector<std::string> Session::dirty_materials() const {
    std::vector<std::string> result;for(const auto& [key,value]:materials_) {static_cast<void>(value);if(material_dirty(key)) result.push_back(key);}return result;
}
Result<void> Session::material_undo(std::string_view key,bool redo) {
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    const auto at=materials_.find(std::string(key));if(at==materials_.end()) return failure("Material asset is not open.");
    auto& doc=at->second;
    if((!redo && !doc.cursor) || (redo && doc.cursor+1==doc.history.size())) return {};
    const auto next=redo?doc.cursor+1:doc.cursor-1;
    auto result=material::update_material(runtime_,doc.root.id(),doc.history[next]);if(result) {doc.cursor=next;++revision_;}return result;
}
Result<void> Session::save_material(Project& project,std::string_view key) {
    if(material_edit_) return failure("Finish the active material edit before saving.");
    const auto at=materials_.find(std::string(key));if(at==materials_.end()) return failure("Material asset is not open.");
    auto asset=material_asset(at->second.root.id());if(!asset) return std::unexpected(asset.error());
    auto state=material::read_material(runtime_,at->second.root.id());if(!state) return std::unexpected(state.error());
    auto saved=project.save_material(*asset);if(!saved) return saved;
    at->second.saved=std::move(*state);++revision_;return {};
}
Result<void> Session::set_material_texture(Project& project,ObjectId object,material::Input input,std::string_view key) {
    const auto index=static_cast<std::size_t>(input);if(index>=material::input_count) return failure("Unknown material texture input.");
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    auto state=material::read_material(runtime_,object);if(!state) return std::unexpected(state.error());
    if(state->parent.registered()) return failure("Texture inputs are edited on the shared material asset.");
    ObjectRoot texture;
    if(!key.empty()) {auto loaded=texture_resource(project,key);if(!loaded) return std::unexpected(loaded.error());texture=std::move(*loaded);}
    state->textures[index]=std::move(texture);state->description.textures[index].image.reset();
    constexpr auto control=std::numeric_limits<std::uint64_t>::max();
    if(auto begun=begin_material_edit(object,control);!begun) return begun;
    auto changed=preview_material(control,*state);
    if(changed) changed=commit_material_edit(control);
    if(!changed) {auto restored=cancel_material_edit();if(!restored) return restored;}
    return changed;
}
Result<void> Session::replace_material_bindings(ObjectId object,std::vector<world::MaterialBinding> after) {
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    auto component=admit_component_edit(object);if(!component) return std::unexpected(component.error());
    auto before=world::read_material_bindings(runtime_,object);if(!before) return std::unexpected(before.error());
    const auto equal=[](const auto& a,const auto& b){return a.slot==b.slot && a.resource.reference()==b.resource.reference() && a.overridden==b.overridden;};
    if(std::ranges::equal(*before,after,equal)) return {};
    auto root=runtime_.hold(object);if(!root) return std::unexpected(root.error());
    auto changed=world::set_material_bindings(runtime_,object,after);if(!changed) return changed;
    remember(MaterialBindingChange{std::move(*root),component->owner,std::move(*before),std::move(after)});return {};
}
Result<void> Session::assign_material(Project& project,ObjectId object,std::string_view slot,std::string_view key) {
    auto resource=material_resource(project,key);if(!resource) return std::unexpected(resource.error());
    auto bindings=world::read_material_bindings(runtime_,object);if(!bindings) return std::unexpected(bindings.error());
    const auto at=std::ranges::find(*bindings,slot,&world::MaterialBinding::slot);
    if(at==bindings->end()) bindings->push_back({std::string(slot),std::move(*resource),true});
    else {at->resource=std::move(*resource);at->overridden=true;}
    return replace_material_bindings(object,std::move(*bindings));
}
Result<void> Session::clear_material(ObjectId object,std::string_view slot) {
    auto bindings=world::read_material_bindings(runtime_,object);if(!bindings) return std::unexpected(bindings.error());
    std::erase_if(*bindings,[&](const auto& binding){return binding.slot==slot && binding.overridden;});
    return replace_material_bindings(object,std::move(*bindings));
}
Result<void> Session::set_material_override(ObjectId component,std::string_view slot,std::span<const FieldId> path,bool enabled) {
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    auto bindings=world::read_material_bindings(runtime_,component);if(!bindings) return std::unexpected(bindings.error());
    const auto at=std::ranges::find(*bindings,slot,&world::MaterialBinding::slot);
    if(at==bindings->end()) return failure("Assign a shared material before creating parameter overrides.");
    auto state=material::read_material(runtime_,at->resource.id());if(!state) return std::unexpected(state.error());
    auto bit=material::override_bit(runtime_,path);if(!bit) return std::unexpected(bit.error());
    if(!state->parent.registered()) {
        if(!enabled) return {};
        auto instance=material::create_instance(runtime_,at->resource);if(!instance) return std::unexpected(instance.error());
        auto marked=material::set_override(runtime_,instance->id(),path,true);if(!marked) return marked;
        at->resource=std::move(*instance);return replace_material_bindings(component,std::move(*bindings));
    }
    if(bool(state->overrides & *bit)==enabled) return {};
    constexpr auto control=std::numeric_limits<std::uint64_t>::max()-1;
    if(auto begun=begin_material_edit(at->resource.id(),control);!begun) return begun;
    auto changed=material::set_override(runtime_,at->resource.id(),path,enabled);
    if(changed) changed=commit_material_edit(control);
    if(!changed) {auto restored=cancel_material_edit();if(!restored) return restored;}
    return changed;
}
}
