#include "editor/workspace.hpp"

namespace atelier::editor {
namespace {constexpr ui::ControlKey choices=1ULL<<38;}
ui::ControlKey Workspace::bind_material(ObjectId object,std::span<const FieldId> path,Binding::Kind kind,ObjectId component,std::string_view slot,std::size_t index) {
    const auto at=std::ranges::find_if(bindings_,[&](const auto& value){return value.object==object && value.kind==kind && value.component==component &&
        value.slot==slot && value.index==index && std::ranges::equal(value.path,path);});
    if(at!=bindings_.end()) return at->key;
    const auto key=next_binding_++;bindings_.push_back({key,object,{path.begin(),path.end()},kind,component,std::string(slot),index});return key;
}
std::optional<Workspace::MaterialChoice> Workspace::material_choice(ui::ControlKey key) const {
    if(key<choices || key-choices>=material_choices_.size()) return {};
    return material_choices_[static_cast<std::size_t>(key-choices)];
}
std::vector<ui::MenuItem> Workspace::material_menu(const Binding& target,const Session& session,const Project* project) {
    std::vector<ui::MenuItem> result;if(!project) return result;
    const bool texture=target.kind==Binding::Kind::material_texture;
    const auto add=[&](std::wstring title,MaterialChoice choice,bool selected=false,bool separator=false) {
        const auto key=choices+material_choices_.size();material_choices_.push_back(std::move(choice));
        ui::MenuItem item{key,std::move(title),!session.world().running(),selected,{},L"",separator};
        if(material_choices_.back().kind==MaterialChoice::Kind::assign || texture) {item.behavior=ui::MenuBehavior::radio;item.radio_group=target.key;}
        result.push_back(std::move(item));
    };
    std::string current;
    if(texture) {
        auto state=session.material_asset(target.object);
        if(!state || target.index>=material::input_count) return result;
        current=state->textures[target.index];
        add(L"None",{MaterialChoice::Kind::texture,target.object,{}, {},target.index},current.empty());
    } else {
        auto bindings=presentation::read_material_bindings(session.world().runtime(),target.object);if(!bindings) return result;
        const auto at=std::ranges::find(*bindings,target.slot,&presentation::MaterialBinding::slot);
        if(at!=bindings->end()) {
            auto state=material::read_material(session.world().runtime(),at->resource.id());
            if(state) {if(state->parent.registered()) state=material::read_material(session.world().runtime(),state->parent.id());if(state) current=state->source;}
        }
        std::string shown=current; // The mesh default when no explicit material is bound.
        if(current.empty()) if(auto mesh=presentation::read_static_mesh(session.world().runtime(),target.object)) {
            const auto fallback=std::ranges::find(mesh->mesh.defaults,target.slot,&presentation::MaterialBinding::slot);
            if(fallback!=mesh->mesh.defaults.end()) if(auto state=material::read_material(session.world().runtime(),fallback->resource.id())) shown=state->source;
        }
        add(L"Use Mesh Default",{MaterialChoice::Kind::clear,target.object,target.slot,{}},current.empty());
        if(!shown.empty()) add(L"Edit Shared Material",{MaterialChoice::Kind::edit,target.object,target.slot,shown});
    }
    bool first=true;
    for(const auto& entry:project->entries()) if(entry.kind==(texture?AssetKind::texture:AssetKind::material)) {
        add(entry.path.stem().wstring(),{texture?MaterialChoice::Kind::texture:MaterialChoice::Kind::assign,target.object,target.slot,entry.key,target.index},
            current==entry.key,first);first=false;
    }
    return result;
}
}
