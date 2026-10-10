#include "world/detail.hpp"
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;
std::string material_slot_name(const geometry::Mesh& mesh,std::size_t index) {
    const auto& slot=mesh.materials()[index];
    return slot.name.empty()?"slot/"+std::to_string(index):slot.name;
}
Result<std::vector<MaterialBinding>> read_material_bindings(Runtime& runtime,ObjectId id) {
    return runtime.with_native<StaticMeshComponent>(id,[&](auto& value)->Result<std::vector<MaterialBinding>> {
        auto field=runtime.reference_field(id,Access::materials(value));if(!field) return std::unexpected(field.error());
        std::vector<MaterialBinding> result;result.reserve(Access::material_slots(value).size());
        for(std::size_t i=0;i<Access::material_slots(value).size();++i) {
            auto reference=runtime.sequence_read(*field,i);if(!reference) return std::unexpected(reference.error());
            auto root=runtime.hold(*reference);if(!root) return std::unexpected(root.error());
            result.push_back({Access::material_slots(value)[i],std::move(*root),Access::material_overrides(value)[i]});
        }
        return result;
    });
}
Result<void> set_material_bindings(Runtime& runtime,ObjectId id,std::span<const MaterialBinding> bindings) {
    if(auto ready=runtime.check_code_publication();!ready) return ready;
    auto component=inspect_component(runtime,id);if(!component) return std::unexpected(component.error());
    if(!component->valid) return error(ErrorCode::invalid_member,"Material bindings need a live mesh component.");
    auto data=detail::component_data(runtime,id);if(!data) return std::unexpected(data.error());
    if(data->owner!=ObjectId{}) {
        auto actor=detail::actor_data(runtime,data->owner);if(!actor) return std::unexpected(actor.error());
        if(actor->transitioning || (actor->world && actor->world->running())) return error(ErrorCode::busy,"Stop the World before changing material slots.");
        if(actor->world) if(auto ready=Access::state(*actor->world).admit();!ready) return ready;
    }
    return runtime.with_native<StaticMeshComponent>(id,[&](auto& value)->Result<void> {
        const auto& mesh=Access::geometry(value);
        if(bindings.size()>(mesh?mesh->materials().size():0)) return error(ErrorCode::argument_mismatch,"Too many material slot bindings.");
        std::vector<std::string> names;std::vector<bool> flags;std::vector<ObjectReference> references;
        names.reserve(bindings.size());flags.reserve(bindings.size());references.reserve(bindings.size());
        std::unordered_set<std::string> unique;
        for(const auto& binding:bindings) {
            if(!unique.insert(binding.slot).second) return error(ErrorCode::argument_mismatch,"Duplicate material slot binding.");
            std::size_t matches{};
            for(std::size_t i=0;i<mesh->materials().size();++i) matches+=material_slot_name(*mesh,i)==binding.slot;
            if(matches!=1) return error(ErrorCode::argument_mismatch,"Material slot is missing or ambiguous: "+binding.slot);
            auto held=runtime.hold(binding.resource.reference());if(!held) return std::unexpected(held.error());
            auto surface=material::capture(runtime,held->id());if(!surface) return std::unexpected(surface.error());
            names.push_back(binding.slot);flags.push_back(binding.overridden);references.push_back(held->reference());
        }
        auto field=runtime.reference_field(id,Access::materials(value));if(!field) return std::unexpected(field.error());
        auto size=runtime.sequence_size(*field);if(!size) return std::unexpected(size.error());
        const ReferenceChange change=SequenceSplice{*field,0,*size,references};
        auto written=runtime.apply_reference_changes({&change,1});if(!written) return written;
        Access::material_slots(value)=std::move(names);Access::material_overrides(value)=std::move(flags);
        Access::surfaces(value).reset();return {};
    });
}
Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> material_surfaces(Runtime& runtime,ObjectId id) {
    using Snapshot=std::shared_ptr<const std::vector<material::SurfaceRef>>;
    return runtime.with_native<StaticMeshComponent>(id,[&](auto& value)->Result<Snapshot> {
        const auto& mesh=Access::geometry(value);if(!mesh) return Snapshot{};
        auto& cached=Access::surfaces(value);
        const bool base_changed=!cached || Access::surface_geometry(value)!=mesh || Access::surface_parameters(value)!=value.material;
        if(!base_changed && Access::material_slots(value).empty()) return cached;
        std::optional<ReferenceField> field;
        if(!Access::material_slots(value).empty()) {
            auto references=runtime.reference_field(id,Access::materials(value));if(!references) return std::unexpected(references.error());
            field=std::move(*references);
        }
        std::shared_ptr<std::vector<material::SurfaceRef>> next;
        const auto writable=[&]() -> auto& {
            if(!next) next=base_changed?std::make_shared<std::vector<material::SurfaceRef>>(mesh->materials().size()):
                std::make_shared<std::vector<material::SurfaceRef>>(*cached);
            return *next;
        };
        for(std::size_t i=0;i<mesh->materials().size();++i) {
            material::SurfaceRef surface;
            const auto name=material_slot_name(*mesh,i);
            const auto at=std::ranges::find(Access::material_slots(value),name);
            if(at!=Access::material_slots(value).end()) {
                const auto index=static_cast<std::size_t>(at-Access::material_slots(value).begin());
                auto reference=runtime.sequence_read(*field,index);if(!reference) return std::unexpected(reference.error());
                auto captured=material::capture(runtime,reference->id());if(!captured) return std::unexpected(captured.error());
                surface=*captured;
            } else if(base_changed) {
                auto description=mesh->materials()[i].surface->description();
                const auto& p=value.material;
                for(unsigned channel=0;channel<3;++channel) description.parameters.base_color[channel]*=p.base_color[channel];
                description.parameters.roughness=p.roughness;description.parameters.metallic=p.metallic;
                description.parameters.emissive={p.emissive.x,p.emissive.y,p.emissive.z};
                try {surface=std::make_shared<const material::Surface>(std::move(description));}
                catch(const std::exception& failure) {return error(ErrorCode::argument_mismatch,failure.what());}
            } else surface=(*cached)[i];
            if(base_changed || (*cached)[i]!=surface) writable()[i]=std::move(surface);
        }
        if(next) {cached=std::move(next);Access::surface_geometry(value)=mesh;Access::surface_parameters(value)=value.material;}
        return cached;
    });
}
}
