#pragma once

#include "editor/session.hpp"
#include "renderer/renderer.hpp"
#include <tuple>
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
    std::uint64_t identity{1}, revision{1};

    void reset() {
        ++identity;revision=1;next_key_=1;keys_.clear();meshes.clear();mesh_entities.clear();lights.clear();
    }
    Result<void> refresh(const editor::Session& document) {
        auto geometry=document.presentation().render_items();if(!geometry) return std::unexpected(geometry.error());
        auto illumination=document.presentation().light_items();if(!illumination) return std::unexpected(illumination.error());
        for(auto& [object,key]:keys_) { (void)object;key.seen=false; }
        bool changed=meshes.size()!=geometry->size();
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
            if(i<meshes.size()) {
                const auto& previous=meshes[i];
                changed|=previous.stable_key!=value.stable_key || previous.geometry!=value.geometry || previous.color!=value.color ||
                    previous.material.roughness!=value.material.roughness || previous.material.metallic!=value.material.metallic ||
                    previous.material.emissive!=value.material.emissive || previous.surfaces!=value.surfaces;
                meshes[i]=std::move(value);
            } else meshes.push_back(std::move(value));
        }
        meshes.resize(geometry->size());
        const auto light_content=[](const renderer::Light& light) {
            return std::tuple{light.stable_key,light.kind,light.color,light.intensity,light.range,light.source_radius,
                light.width,light.height,light.two_sided,light.casts_shadow};
        };
        std::size_t light_count=0;
        for(const auto& item:*illumination) {
            const auto& settings=item.settings;
            const auto i=light_count++;
            renderer::Light value;
            value.stable_key=key(item.component);
            switch(settings.kind) {
            case presentation::LightKind::point:value.kind=renderer::LightKind::point;break;
            case presentation::LightKind::rectangle:value.kind=renderer::LightKind::rectangle;break;
            case presentation::LightKind::directional:value.kind=renderer::LightKind::directional;break;
            }
            value.position=rgb(item.position);value.direction=rgb(item.direction);value.right=rgb(item.right);value.up=rgb(item.up);
            value.color=rgb(settings.color);value.intensity=settings.intensity;value.range=settings.range;
            value.source_radius=settings.source_radius;value.width=settings.width;value.height=settings.height;
            value.two_sided=settings.two_sided;value.casts_shadow=settings.casts_shadow;
            if(i<lights.size()) {changed|=light_content(lights[i])!=light_content(value);lights[i]=value;}
            else {lights.push_back(value);changed=true;}
        }
        changed|=lights.size()!=light_count;lights.resize(light_count);
        std::erase_if(keys_,[](const auto& item){return !item.second.seen;});
        // TRS, selection and camera movement are temporal inputs, not scene cuts.
        // Geometry/material/light settings and presentation membership invalidate history.
        if(changed) ++revision;
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
