#include "material/resources.hpp"

namespace atelier::material {
namespace detail {
struct Access {
    static String& source(TextureResource& value) {return value.source_;}
    static ImageRef& image(TextureResource& value) {return value.image_;}
    static String& source(MaterialResource& value) {return value.source_;}
    static ReferenceSequence& textures(MaterialResource& value) {return value.textures_;}
    static ReferenceSlot& parent(MaterialResource& value) {return value.parent_;}
    static std::uint32_t& overrides(MaterialResource& value) {return value.overrides_;}
    static Description& description(MaterialResource& value) {return value.description_;}
    static SurfaceRef& published(MaterialResource& value) {return value.published_;}
};
}
namespace {
using detail::Access;
auto samplers(MaterialResource& value) {
    return std::array{&value.base_color_texture,&value.roughness_texture,&value.metallic_texture,
        &value.normal_texture,&value.emissive_texture,&value.opacity_texture};
}
Description describe(MaterialResource& value) {
    auto result=Access::description(value);const auto& p=value.parameters;auto& out=result.parameters;
    out.base_color={p.base_color.x,p.base_color.y,p.base_color.z,p.opacity};out.roughness=p.roughness;out.metallic=p.metallic;
    out.emissive={p.emissive.x,p.emissive.y,p.emissive.z};out.normal_scale=p.normal_scale;
    out.normal_y_sign=p.normal_y_sign==NormalY::positive?1.f:p.normal_y_sign==NormalY::negative?-1.f:0.f;
    out.coverage=p.mask?Coverage::mask:Coverage::opaque;out.alpha_cutoff=p.alpha_cutoff;out.double_sided=p.double_sided;out.emission_two_sided=p.emission_two_sided;
    const auto fields=samplers(value);
    for(std::size_t i=0;i<input_count;++i) {const auto& s=*fields[i];auto& t=result.textures[i];t.scale={s.scale.x,s.scale.y};
        t.offset={s.offset.x,s.offset.y};t.rotation=s.rotation;t.address=static_cast<Address>(s.address);t.channel=static_cast<std::uint32_t>(s.channel);}
    return result;
}
void assign(MaterialResource& value,const Description& description) {
    const auto& p=description.parameters;auto& out=value.parameters;
    out.base_color={p.base_color[0],p.base_color[1],p.base_color[2]};out.opacity=p.base_color[3];out.roughness=p.roughness;out.metallic=p.metallic;
    out.emissive={p.emissive[0],p.emissive[1],p.emissive[2]};out.normal_scale=p.normal_scale;out.normal_y_sign=p.normal_y_sign==1?NormalY::positive:NormalY::negative;
    out.mask=p.coverage==Coverage::mask;out.alpha_cutoff=p.alpha_cutoff;out.double_sided=p.double_sided;out.emission_two_sided=p.emission_two_sided;
    const auto fields=samplers(value);
    for(std::size_t i=0;i<input_count;++i) {auto& s=*fields[i];const auto& t=description.textures[i];s.scale={t.scale[0],t.scale[1]};
        s.offset={t.offset[0],t.offset[1]};s.rotation=t.rotation;s.address=static_cast<SamplingAddress>(t.address);s.channel=static_cast<SamplingChannel>(t.channel);}
}
Description inherited(Description base,const Description& own,std::uint32_t mask) {
    auto& p=base.parameters;const auto& o=own.parameters;
    if(mask&1u) for(std::size_t i=0;i<3;++i) p.base_color[i]=o.base_color[i];
    if(mask&(1u<<1)) p.base_color[3]=o.base_color[3];
    if(mask&(1u<<2)) p.roughness=o.roughness;
    if(mask&(1u<<3)) p.metallic=o.metallic;
    if(mask&(1u<<4)) p.emissive=o.emissive;
    if(mask&(1u<<5)) p.normal_scale=o.normal_scale;
    if(mask&(1u<<6)) p.normal_y_sign=o.normal_y_sign;
    if(mask&(1u<<7)) p.coverage=o.coverage;
    if(mask&(1u<<8)) p.alpha_cutoff=o.alpha_cutoff;
    if(mask&(1u<<9)) p.double_sided=o.double_sided;
    if(mask&(1u<<10)) p.emission_two_sided=o.emission_two_sided;
    return base;
}
auto failure(std::string message) {return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)});}
Result<void> validate_source(std::string_view source) {
    if(source.size()>4096 || source.find('\0')!=source.npos) return failure("Invalid material resource source.");
    return {};
}
}
Result<void> register_types(Runtime& runtime) {
    if(runtime.native_type<MaterialResource>() && runtime.native_type<TextureResource>()) return {};
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<NormalY>::build(runtime));
    definitions.push_back(reflection::Definition<ParameterValues>::build(runtime));
    definitions.push_back(reflection::Definition<SamplingAddress>::build(runtime));
    definitions.push_back(reflection::Definition<SamplingChannel>::build(runtime));
    definitions.push_back(reflection::Definition<SamplingValues>::build(runtime));
    definitions.push_back(reflection::Definition<TextureResource>::build(runtime));
    definitions.push_back(reflection::Definition<MaterialResource>::build(runtime));
    auto result=runtime.register_types(std::move(definitions));
    if(!result) return std::unexpected(result.error());
    return {};
}
Result<ObjectRoot> create_texture(Runtime& runtime,std::string_view source,ImageRef image) {
    if(auto ready=runtime.check_code_publication();!ready) return std::unexpected(ready.error());
    if(auto valid=validate_source(source);!valid) return std::unexpected(valid.error());
    if(!image && source.empty()) return failure("A texture without image content requires the persistent source it stands in for.");
    auto type=runtime.native_type<TextureResource>();if(!type) return std::unexpected(type.error());
    auto root=runtime.create(*type);if(!root) return std::unexpected(root.error());
    auto written=runtime.with_native<TextureResource>(root->id(),[&](auto& value)->Result<void> {
        Access::source(value)=String(StringView(source));Access::image(value)=std::move(image);return {};
    });
    if(!written) return std::unexpected(written.error());
    return std::move(*root);
}
Result<TextureState> read_texture(Runtime& runtime,ObjectId id) {
    return runtime.with_native<TextureResource>(id,[](auto& value)->Result<TextureState> {
        return TextureState{std::string(Access::source(value).view().std_view()),Access::image(value)};
    });
}
Result<ObjectRoot> create_material(Runtime& runtime,const State& state) {
    if(auto ready=runtime.check_code_publication();!ready) return std::unexpected(ready.error());
    auto type=runtime.native_type<MaterialResource>();if(!type) return std::unexpected(type.error());
    auto root=runtime.create(*type);if(!root) return std::unexpected(root.error());
    auto written=update_material(runtime,root->id(),state);if(!written) return std::unexpected(written.error());
    return std::move(*root);
}
Result<ObjectRoot> create_instance(Runtime& runtime,const ObjectRoot& parent) {
    auto held=runtime.hold(parent.reference());if(!held) return std::unexpected(held.error());
    auto state=read_material(runtime,held->id());if(!state) return std::unexpected(state.error());
    if(state->parent.registered() || state->source.empty()) return failure("A material instance requires one persistent parent material.");
    state->source.clear();state->parent=std::move(*held);state->overrides=0;
    return create_material(runtime,*state);
}
Result<State> read_material(Runtime& runtime,ObjectId id) {
    auto captured=capture(runtime,id);if(!captured) return std::unexpected(captured.error());
    return runtime.with_native<MaterialResource>(id,[&](auto& value)->Result<State> {
        State result;result.source=std::string(Access::source(value).view().std_view());
        result.description=describe(value);
        result.overrides=Access::overrides(value);
        auto parent_field=runtime.reference_field(id,Access::parent(value));if(!parent_field) return std::unexpected(parent_field.error());
        auto parent=runtime.read_reference(*parent_field);if(!parent) return std::unexpected(parent.error());
        if(!parent->is_null()) {auto root=runtime.hold(*parent);if(!root) return std::unexpected(root.error());result.parent=std::move(*root);}
        auto field=runtime.reference_field(id,Access::textures(value));if(!field) return std::unexpected(field.error());
        auto size=runtime.sequence_size(*field);if(!size) return std::unexpected(size.error());
        if(*size!=input_count) return failure("Material texture reference table is incomplete.");
        for(std::size_t i=0;i<input_count;++i) {
            auto reference=runtime.sequence_read(*field,i);if(!reference) return std::unexpected(reference.error());
            if(reference->is_null()) continue;
            auto root=runtime.hold(*reference);if(!root) return std::unexpected(root.error());
            result.textures[i]=std::move(*root);
        }
        if(result.parent.registered()) {auto inherited=read_material(runtime,result.parent.id());if(!inherited) return std::unexpected(inherited.error());result.textures=std::move(inherited->textures);}
        return result;
    });
}
Result<void> update_material(Runtime& runtime,ObjectId id,const State& state) {
    if(auto ready=runtime.check_code_publication();!ready) return ready;
    if(auto valid=validate_source(state.source);!valid) return valid;
    try {
        if(state.overrides & ~parameter_mask) return failure("Unknown material override bits.");
        auto material_type=runtime.native_type<MaterialResource>();if(!material_type) return std::unexpected(material_type.error());
        auto parent_reference=runtime.null_reference(*material_type);if(!parent_reference) return std::unexpected(parent_reference.error());
        SurfaceRef parent_surface;
        if(state.parent.registered()) {
            auto held=runtime.hold(state.parent.reference());if(!held) return std::unexpected(held.error());
            if(held->id()==id || !state.source.empty()) return failure("Material instances cannot replace an asset or inherit themselves.");
            auto parent_state=read_material(runtime,held->id());if(!parent_state) return std::unexpected(parent_state.error());
            if(parent_state->parent.registered() || parent_state->source.empty()) return failure("Only one material instance inheritance level is supported.");
            auto captured=capture(runtime,held->id());if(!captured) return std::unexpected(captured.error());
            parent_surface=*captured;parent_reference=held->reference();
        } else if(state.overrides) return failure("Parameter overrides require a parent material.");
        auto texture_type=runtime.native_type<TextureResource>();if(!texture_type) return std::unexpected(texture_type.error());
        auto empty=runtime.null_reference(*texture_type);if(!empty) return std::unexpected(empty.error());
        auto description=state.description;
        std::array<ObjectReference,input_count> references;
        for(std::size_t i=0;i<input_count;++i) {
            references[i]=*empty;
            if(state.textures[i].registered()) {
                // hold verifies Runtime provenance before an opaque ObjectId is used.
                auto owned=runtime.hold(state.textures[i].reference());if(!owned) return std::unexpected(owned.error());
                auto texture=read_texture(runtime,owned->id());if(!texture) return std::unexpected(texture.error());
                if(!state.source.empty() && texture->source.empty()) return failure("Persistent materials require persistent texture references.");
                references[i]=owned->reference();description.textures[i].image=texture->image;
            } else if(!state.source.empty() && description.textures[i].image) {
                return failure("Persistent materials require persistent texture references.");
            }
        }
        if(parent_surface) {description=inherited(parent_surface->description(),description,state.overrides);references.fill(*empty);}
        auto surface=std::make_shared<const Surface>(description);
        String source{StringView(state.source)};
        return runtime.with_native<MaterialResource>(id,[&](auto& value)->Result<void> {
            if(!Access::source(value).empty() && Access::source(value).view()!=source.view())
                return failure("A material update cannot replace its persistent identity.");
            auto field=runtime.reference_field(id,Access::textures(value));if(!field) return std::unexpected(field.error());
            auto size=runtime.sequence_size(*field);if(!size) return std::unexpected(size.error());
            auto parent_field=runtime.reference_field(id,Access::parent(value));if(!parent_field) return std::unexpected(parent_field.error());
            const std::array<ReferenceChange,2> changes{SequenceSplice{*field,0,*size,references},ReferenceWrite{*parent_field,*parent_reference}};
            auto written=runtime.apply_reference_changes(changes);if(!written) return written;
            Access::overrides(value)=state.overrides;
            Access::source(value)=std::move(source);assign(value,description);Access::description(value)=std::move(description);
            if(!Access::published(value) || Access::published(value)->description()!=surface->description())
                Access::published(value)=std::move(surface);
            return {};
        });
    } catch(const std::exception& error) {return failure(error.what());}
}
static Result<SurfaceRef> capture_depth(Runtime& runtime,ObjectId id,unsigned depth) {
    return runtime.with_native<MaterialResource>(id,[&](auto& value)->Result<SurfaceRef> {
        if(!Access::published(value)) return failure("Material resource has not published validated content.");
        // Direct native/source field writes use the same parameter record. A
        // snapshot validates and publishes it once when its content changes.
        auto description=describe(value);
        auto field=runtime.reference_field(id,Access::parent(value));if(!field) return std::unexpected(field.error());
        auto parent=runtime.read_reference(*field);if(!parent) return std::unexpected(parent.error());
        if(!parent->is_null()) {
            if(depth) return failure("Material parent cycles and multilevel inheritance are not supported.");
            auto base=capture_depth(runtime,parent->id(),depth+1);if(!base) return std::unexpected(base.error());
            description=inherited((*base)->description(),description,Access::overrides(value));
        }
        if(Access::published(value)->description()!=description) {
            try {Access::published(value)=std::make_shared<const Surface>(description);}
            catch(const std::exception& error) {return failure(error.what());}
        }
        assign(value,description);Access::description(value)=description;
        return Access::published(value);
    });
}
Result<SurfaceRef> capture(Runtime& runtime,ObjectId id) {return capture_depth(runtime,id,0);}
Result<void> write_property(Runtime& runtime,ObjectId id,std::span<const FieldId> path,Scalar value) {
    if(auto ready=runtime.check_code_publication();!ready) return ready;
    auto state=read_material(runtime,id);if(!state) return std::unexpected(state.error());
    auto before=runtime.read(id,path);if(!before) return std::unexpected(before.error());
    if(state->parent.registered()) {
        auto bit=override_bit(runtime,path);if(!bit) return std::unexpected(bit.error());
        auto marked=runtime.with_native<MaterialResource>(id,[&](auto& item)->Result<void>{Access::overrides(item)|=*bit;return {};});
        if(!marked) return marked;
    }
    auto written=runtime.write(id,path,std::move(value));
    if(!written) {auto restored=update_material(runtime,id,*state);return restored?written:restored;}
    auto result=capture(runtime,id);
    if(!result) {auto restored=update_material(runtime,id,*state);if(!restored) return restored;return std::unexpected(result.error());}
    return {};
}
Result<std::uint32_t> override_bit(Runtime& runtime,std::span<const FieldId> path) {
    auto owner=runtime.native_type<MaterialResource>(),parameters=runtime.native_type<ParameterValues>();
    if(!owner) return std::unexpected(owner.error());if(!parameters) return std::unexpected(parameters.error());
    auto field=runtime.find_field(*owner,"parameters");if(!field) return std::unexpected(field.error());
    if(path.size()<2 || path.front()!=*field) return failure("Instance overrides support material parameters; texture sampling is inherited.");
    for(std::size_t i=0;i<parameter_names.size();++i) {
        auto candidate=runtime.find_field(*parameters,parameter_names[i]);if(!candidate) return std::unexpected(candidate.error());
        if(path[1]==*candidate) return 1u<<i;
    }
    return failure("Unknown material parameter override.");
}
Result<void> set_override(Runtime& runtime,ObjectId id,std::span<const FieldId> path,bool enabled) {
    auto state=read_material(runtime,id);if(!state) return std::unexpected(state.error());
    if(!state->parent.registered()) return failure("Parameter overrides require a material instance.");
    auto bit=override_bit(runtime,path);if(!bit) return std::unexpected(bit.error());
    state->overrides=enabled?state->overrides|*bit:state->overrides&~*bit;
    return update_material(runtime,id,*state);
}
}
