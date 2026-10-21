#pragma once

#include "editor/session.hpp"
#include "renderer/renderer.hpp"
#include <unordered_map>
#include <unordered_set>

namespace atelier::application {
// An owning draw snapshot, not a registry of business objects. Opaque ObjectIds
// are mapped with full equality; their hashes are never used as renderer IDs.
class SceneSnapshot final {
public:
    std::vector<renderer::MeshInstance> meshes;
    // Same-frame geometry membership, shared with viewport locator overlays.
    std::vector<ObjectId> mesh_entities;
    std::vector<renderer::Light> lights;
    std::uint64_t identity{1};

    void reset() {
        ++identity;next_key_=1;keys_.clear();meshes.clear();mesh_entities.clear();lights.clear();
    }
    Result<void> refresh(const editor::Session& document) {
        auto geometry=document.world().render_items();if(!geometry) return std::unexpected(geometry.error());
        auto illumination=document.world().light_items();if(!illumination) return std::unexpected(illumination.error());
        for(auto& [object,key]:keys_) { (void)object;key.seen=false; }
        meshes.clear();lights.clear();
        meshes.reserve(geometry->size());lights.reserve(illumination->size());
        mesh_entities.clear();mesh_entities.reserve(geometry->size());
        const auto selection=document.selected_entities();
        std::unordered_map<ObjectId,bool> inherited_selection;
        if(!selection.empty()) {
            const auto members=document.world().members();if(!members) return std::unexpected(members.error());
            std::unordered_set<ObjectId> selected(selection.begin(),selection.end());
            inherited_selection.reserve(members->size());
            // The World directory is in composition preorder: each parent is already available.
            for(const auto& member:*members) {
                const auto parent=world::parent_entity(document.world().runtime(),member.id());
                if(!parent) return std::unexpected(parent.error());
                inherited_selection.emplace(member.id(),selected.contains(member.id()) ||
                    (*parent!=ObjectId{} && inherited_selection.at(*parent)));
            }
        }
        const auto rgb=[](math::Vector3 value){return std::array{value.x,value.y,value.z};};
        for(std::size_t i=0;i<geometry->size();++i) {
            const auto& item=(*geometry)[i];
            mesh_entities.push_back(item.entity);
            renderer::MeshInstance value{item.world.elements,
                {item.material.base_color.x,item.material.base_color.y,item.material.base_color.z,1},
                !selection.empty() && inherited_selection.at(item.entity),item.geometry,
                {item.material.roughness,item.material.metallic,rgb(item.material.emissive)},key(item.component)};
            value.surfaces=item.surfaces;
            meshes.push_back(std::move(value));
        }
        for(const auto& item:*illumination) {
            const auto& settings=item.settings;
            renderer::Light value;
            value.stable_key=key(item.component);
            switch(settings.kind) {
            case world::LightKind::point:value.kind=renderer::LightKind::point;break;
            case world::LightKind::rectangle:value.kind=renderer::LightKind::rectangle;break;
            case world::LightKind::directional:value.kind=renderer::LightKind::directional;break;
            }
            value.position=rgb(item.position);value.direction=rgb(item.direction);value.right=rgb(item.right);value.up=rgb(item.up);
            value.color=rgb(settings.color);value.intensity=settings.intensity;value.range=settings.range;
            value.source_radius=settings.source_radius;value.width=settings.width;value.height=settings.height;
            value.two_sided=settings.two_sided;value.casts_shadow=settings.casts_shadow;
            lights.push_back(value);
        }
        std::erase_if(keys_,[](const auto& item){return !item.second.seen;});
        return {};
    }
private:
    struct Key { std::uint64_t value{};bool seen{}; };
    std::unordered_map<ObjectId,Key> keys_;
    std::uint64_t next_key_{1};
    std::uint64_t key(ObjectId object) {
        auto [at,added]=keys_.try_emplace(object);
        if(added) at->second.value=next_key_++;
        at->second.seen=true;return at->second.value;
    }
};
}
