#pragma once

#include "editor/session.hpp"
#include "renderer/renderer.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace atelier::application {
// An owning draw snapshot, not a registry of business objects. Opaque ObjectIds
// are mapped with full equality; their hashes are never used as renderer IDs.
// It follows presentation::Scene's cached extraction: a refresh rebuilds only the
// mesh slots, lights and selection that changed since the previous refresh.
class SceneSnapshot final {
public:
    // Mesh instances and lights built so far; a frame without changes builds none.
    std::uint64_t built{};
    std::vector<renderer::MeshInstance> meshes;
    // Same-frame geometry membership, shared with viewport locator overlays.
    std::vector<ObjectId> mesh_entities;
    std::vector<renderer::Light> lights;
    std::uint64_t identity{1};

    void reset() {
        ++identity;next_key_=1;mesh_keys_.clear();light_keys_.clear();meshes.clear();mesh_entities.clear();lights.clear();
        dense_.clear();synced_.reset();
    }
    Result<void> refresh(const editor::Session& document) {
        const auto& scene=document.presentation();
        auto version=scene.update();if(!version) return std::unexpected(version.error());
        const auto selection=document.selected_entities();
        const bool reselect=!synced_ || !std::ranges::equal(synced_->selection,selection);
        if(synced_ && synced_->version==*version && !reselect) return {};
        const auto slots=scene.mesh_slots();
        const auto since=synced_?synced_->version:0;
        // Slots keep their component until the layout changes; a slot that starts or
        // stops drawing changes the dense order, so the instances are rebuilt.
        bool relayout=!synced_ || scene.mesh_layout()>since;
        for(std::size_t slot=0;!relayout && slot<slots.size();++slot) {
            if(slots[slot].changed<=since) continue;
            const auto at=dense_[slot];
            if((at==absent)==slots[slot].item.has_value()) {relayout=true;break;}
            if(at!=absent) {auto& value=meshes[at];value=instance(*slots[slot].item,value.stable_key,value.selected);}
        }
        if(relayout) {
            for(auto& [object,key]:mesh_keys_) { (void)object;key.seen=false; }
            meshes.clear();mesh_entities.clear();dense_.assign(slots.size(),absent);
            for(std::size_t slot=0;slot<slots.size();++slot) {
                if(!slots[slot].item) continue;
                const auto& item=*slots[slot].item;
                dense_[slot]=static_cast<std::uint32_t>(meshes.size());
                mesh_entities.push_back(item.entity);
                meshes.push_back(instance(item,key(mesh_keys_,item.component),false));
            }
            std::erase_if(mesh_keys_,[](const auto& item){return !item.second.seen;});
        }
        if(relayout || reselect) if(auto selected=highlight(document,selection);!selected) return selected;
        if(!synced_ || scene.lights_changed()>since) {
            for(auto& [object,key]:light_keys_) { (void)object;key.seen=false; }
            lights.clear();
            for(const auto& item:scene.light_snapshot()) lights.push_back(light(item,key(light_keys_,item.component)));
            std::erase_if(light_keys_,[](const auto& item){return !item.second.seen;});
        }
        synced_=Synced{*version,{selection.begin(),selection.end()}};
        return {};
    }
private:
    static constexpr std::uint32_t absent=std::numeric_limits<std::uint32_t>::max();
    struct Key { std::uint64_t value{};bool seen{}; };
    struct Synced { std::uint64_t version;std::vector<ObjectId> selection; };
    std::unordered_map<ObjectId,Key> mesh_keys_,light_keys_;
    std::uint64_t next_key_{1};
    std::vector<std::uint32_t> dense_; // Mesh slot to its index in `meshes`, or absent.
    std::optional<Synced> synced_;
    std::uint64_t key(std::unordered_map<ObjectId,Key>& keys,ObjectId object) {
        auto [at,added]=keys.try_emplace(object);
        if(added) at->second.value=next_key_++;
        at->second.seen=true;return at->second.value;
    }
    static std::array<float,3> rgb(math::Vector3 value) {return {value.x,value.y,value.z};}
    renderer::MeshInstance instance(const presentation::RenderItem& item,std::uint64_t key,bool selected) {
        ++built;
        renderer::MeshInstance value{item.world.elements,
            {item.material.base_color.x,item.material.base_color.y,item.material.base_color.z,1},
            selected,item.geometry,{item.material.roughness,item.material.metallic,rgb(item.material.emissive)},key};
        value.surfaces=item.surfaces;
        return value;
    }
    renderer::Light light(const presentation::LightRenderItem& item,std::uint64_t key) {
        ++built;
        const auto& settings=item.settings;
        renderer::Light value;
        value.stable_key=key;
        switch(settings.kind) {
        case presentation::LightKind::point:value.kind=renderer::LightKind::point;break;
        case presentation::LightKind::rectangle:value.kind=renderer::LightKind::rectangle;break;
        case presentation::LightKind::directional:value.kind=renderer::LightKind::directional;break;
        }
        value.position=rgb(item.position);value.direction=rgb(item.direction);value.right=rgb(item.right);value.up=rgb(item.up);
        value.color=rgb(settings.color);value.intensity=settings.intensity;value.range=settings.range;
        value.source_radius=settings.source_radius;value.width=settings.width;value.height=settings.height;
        value.two_sided=settings.two_sided;value.casts_shadow=settings.casts_shadow;
        return value;
    }
    // A mesh is highlighted when its Entity or an ancestor is selected.
    Result<void> highlight(const editor::Session& document,std::span<const ObjectId> selection) {
        if(selection.empty()) {for(auto& mesh:meshes) mesh.selected=false;return {};}
        const auto members=document.world().members();if(!members) return std::unexpected(members.error());
        std::unordered_set<ObjectId> selected(selection.begin(),selection.end());
        std::unordered_map<ObjectId,bool> inherited;inherited.reserve(members->size());
        // The World directory is in composition preorder: each parent is already available.
        for(const auto& member:*members) {
            const auto parent=world::parent_entity(document.world().runtime(),member.id());
            if(!parent) return std::unexpected(parent.error());
            inherited.emplace(member.id(),selected.contains(member.id()) || (*parent!=ObjectId{} && inherited.at(*parent)));
        }
        for(std::size_t i=0;i<meshes.size();++i) meshes[i].selected=inherited.at(mesh_entities[i]);
        return {};
    }
};
}
