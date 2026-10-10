#pragma once

#include "editor/session.hpp"
#include "renderer/renderer.hpp"
#include <tuple>
#include <unordered_map>

namespace atelier::application {
// An owning draw snapshot, not a registry of business objects. Opaque ObjectIds
// are mapped with full equality; their hashes are never used as renderer IDs.
class SceneSnapshot final {
public:
    std::vector<renderer::MeshInstance> meshes;
    std::vector<renderer::Light> lights;
    std::array<float,3> environment{};
    std::uint64_t identity{1}, revision{1};

    void reset() {
        ++identity;revision=1;next_key_=1;keys_.clear();meshes.clear();lights.clear();environment={};
    }
    Result<void> refresh(const editor::Session& document) {
        auto geometry=document.world().render_items();if(!geometry) return std::unexpected(geometry.error());
        auto illumination=document.world().light_items();if(!illumination) return std::unexpected(illumination.error());
        for(auto& [object,key]:keys_) { (void)object;key.seen=false; }
        bool changed=meshes.size()!=geometry->size();
        meshes.reserve(geometry->size());lights.reserve(illumination->size());
        const auto rgb=[](math::Vector3 value){return std::array{value.x,value.y,value.z};};
        for(std::size_t i=0;i<geometry->size();++i) {
            const auto& item=(*geometry)[i];
            renderer::MeshInstance value{item.world.elements,
                {item.material.base_color.x,item.material.base_color.y,item.material.base_color.z,1},
                document.is_selected(item.actor),item.geometry,
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
        std::array<double,3> sky_radiance{};
        std::size_t light_count=0;
        for(const auto& item:*illumination) {
            const auto& settings=item.settings;
            if(settings.kind==world::LightKind::sky) {
                // The World's single Sky Light is outside local-light tiling and
                // shadow budgets. Disabled components are absent here.
                for(std::size_t channel=0;channel<3;++channel)
                    sky_radiance[channel]=double(settings.color[channel])*settings.intensity;
                continue;
            }
            const auto i=light_count++;
            renderer::Light value;
            value.stable_key=key(item.component);
            switch(settings.kind) {
            case world::LightKind::point:value.kind=renderer::LightKind::point;break;
            case world::LightKind::rectangle:value.kind=renderer::LightKind::rectangle;break;
            case world::LightKind::directional:value.kind=renderer::LightKind::directional;break;
            case world::LightKind::sky:break; // Reduced above, never uploaded as a direct light.
            }
            value.position=rgb(item.position);value.direction=rgb(item.direction);value.right=rgb(item.right);value.up=rgb(item.up);
            value.color=rgb(settings.color);value.intensity=settings.intensity;value.range=settings.range;
            value.source_radius=settings.source_radius;value.width=settings.width;value.height=settings.height;
            value.two_sided=settings.two_sided;value.casts_shadow=settings.casts_shadow;
            if(i<lights.size()) {changed|=light_content(lights[i])!=light_content(value);lights[i]=value;}
            else {lights.push_back(value);changed=true;}
        }
        changed|=lights.size()!=light_count;lights.resize(light_count);
        std::array<float,3> next_environment{};
        for(std::size_t channel=0;channel<3;++channel) {
            if(!(sky_radiance[channel]>=0 && sky_radiance[channel]<=65504))
                return std::unexpected(Error{ErrorCode::argument_mismatch,"Sky Light radiance exceeds the HDR scene range."});
            next_environment[channel]=static_cast<float>(sky_radiance[channel]);
        }
        changed|=environment!=next_environment;environment=next_environment;
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
