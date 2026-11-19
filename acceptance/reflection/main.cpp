#include "../helpers.hpp"
#include "reflection/fixture.hpp"
#include "runtime/native.hpp"
#include <cstdio>

namespace {
struct Base {
    double value{};
};

#pragma pack(push, 1)
struct Packed : Base {
    bool active{};
};
#pragma pack(pop)

static_assert(alignof(Packed) < alignof(Base));
}

void verify_reflection() {
    using namespace atelier;
    using namespace acceptance::helpers;
    using namespace acceptance::reflection;
    namespace upstream = acceptance::upstream;
    TypeInfo retained;
    TypeInfo retained_derived;
    {
        Runtime runtime;
        take(NativeRegistration<Base>{runtime, "reflection.AlignedBase", TypeKind::object}.publish());
        for (const auto offset : {std::optional<std::size_t>{}, std::optional<std::size_t>{0}}) {
            const auto packed =
                NativeRegistration<Packed>{runtime, "reflection.Packed", TypeKind::object}.base<Base>(offset).publish();
            require(!packed && packed.error().code == ErrorCode::invalid_definition &&
                        packed.error().message.contains("requires alignment 8"),
                    "Native registration accepted an underaligned base or lost its alignment diagnostic.");
        }
        const auto mode_def = NativeRegistration<upstream::Mode>{runtime, "upstream.Mode", TypeKind::enumeration}
                                  .enumerator("idle", upstream::Mode::idle)
                                  .enumerator("ready", upstream::Mode::ready)
                                  .build();
        const auto payload_def = NativeRegistration<upstream::Payload>{runtime, "upstream.Payload", TypeKind::record}
                                     .field<&upstream::Payload::scale>("scale", {TypeReference::native<float>()},
                                                                       offsetof(upstream::Payload, scale))
                                     .build();
        auto derived_def = atelier::reflection::Definition<DerivedCounter>::build(runtime);
        {
            TypeDefinition colliding_described;
            colliding_described.name = "reflection.CollidingDescribed";
            colliding_described.kind = TypeKind::object;
            MethodDefinition callable{"process", {}, {}, true};
            callable.signature.receiver = ReceiverDefinition{TypeReference{"reflection.CollidingDescribed"}, true};
            callable.invoke = [](CallContext&) noexcept -> Result<void> { return {}; };
            colliding_described.methods.push_back(std::move(callable));
            DescribedMethodDefinition described{"process", {}};
            described.signature.receiver = ReceiverDefinition{TypeReference{"reflection.CollidingDescribed"}, false};
            colliding_described.described_methods.push_back(std::move(described));
            const auto colliding = runtime.register_type(std::move(colliding_described));
            require(!colliding && colliding.error().code == ErrorCode::duplicate_name,
                    "Runtime accepted same-name callable and described methods on one type.");
        }
        const auto quat_type = take(runtime.native_type<math::Quaternion>());
        MethodDefinition partial_method{"partial_rotation", {}, {}, true};
        partial_method.signature.receiver = ReceiverDefinition{TypeReference::native<DerivedCounter>(), true};
        partial_method.signature.result = ValueDefinition{quat_type};
        partial_method.invoke = [](CallContext& ctx) noexcept -> Result<void> {
            static_cast<math::Quaternion*>(ctx.result_value.data)->x = 0.5f;
            ctx.result_written = true;
            return {};
        };
        derived_def.methods.push_back(std::move(partial_method));
        const auto types = take(
            runtime.register_types({mode_def, payload_def, atelier::reflection::Definition<LayerMask>::build(runtime),
                                    atelier::reflection::Definition<Sample>::build(runtime),
                                    atelier::reflection::Definition<Counter>::build(runtime), std::move(derived_def)}));
        const auto* mask_info = take(runtime.describe(types[2]));
        require(mask_info->enumeration && mask_info->enumeration->flags &&
                    mask_info->enumeration->metadata.size() == 3 &&
                    mask_info->enumeration->metadata[1].label == "静态层" &&
                    mask_info->enumeration->metadata[1].description == "地形与建筑" &&
                    mask_info->enumeration->metadata[2].label == "动态层" &&
                    mask_info->enumeration->metadata[2].description.empty(),
                "Reflected Flags enum or AMETA enumerator metadata did not publish.");
        retained = *take(runtime.describe(types[3]));
        const auto* derived_info = take(runtime.describe(types[5]));
        retained_derived = *derived_info;
        static_assert(!std::is_standard_layout_v<DerivedCounter>);
        require(derived_info->base_offset == 0u && derived_info->fields.size() == 4 &&
                    derived_info->fields[0].offset.has_value() && !derived_info->fields[0].transient &&
                    !derived_info->fields[0].read_only && derived_info->fields[3].transient &&
                    derived_info->fields[3].read_only,
                "Non-standard-layout reflected object did not publish direct offsets or field flags.");
        auto object = take(runtime.create(types[5]));
        require(Counter::live == 1, "Generated constructor did not run.");
        const FieldId step = derived_info->fields[0].id;
        take(runtime.write(object.id(), std::span{&step, 1}, Scalar{4.5f}));
        require(std::get<float>(take(runtime.read(object.id(), std::span{&step, 1}))) == 4.5f,
                "Direct-offset reflected object field round-trip failed.");
        require(!take(runtime.validate(object.id())).has_value(), "Valid reflected object failed validation.");
        take(runtime.write(object.id(), std::span{&step, 1}, Scalar{0.0f}));
        require(take(runtime.validate(object.id())).has_value(), "Reflected Validate hook did not reject step=0.");
        take(runtime.write(object.id(), std::span{&step, 1}, Scalar{2.0f}));
        const MethodId advance = take(runtime.find_method(types[5], "advance"));
        const upstream::Payload input{3.0f};
        const std::array args{Argument{Scalar{EnumValue{types[0], 0u}}}, Argument::borrow(input)};
        Scalar call_out{};
        take(runtime.call(object.id(), advance, args, &call_out));
        require(std::get<float>(call_out) == 13.0f, "Contextual AFUNCTION with omitted default arguments failed.");
        const std::array custom_args{Argument{Scalar{EnumValue{types[0], 1u}}}, Argument::borrow(input),
                                     Argument{Scalar{0.5f}}, Argument{Scalar{EnumValue{types[2], 0u}}}};
        take(runtime.call(object.id(), advance, custom_args, &call_out));
        require(std::get<float>(call_out) == 13.0f, "Explicit enum/scalar overrides on AFUNCTION failed.");
        const MethodId partial_rotation = take(runtime.find_method(types[5], "partial_rotation"));
        math::Quaternion rotation{9.0f, 9.0f, 9.0f, 9.0f};
        take(runtime.call(object.id(), partial_rotation, std::span<const Scalar>{}, ValueOutput{quat_type, &rotation}));
        require(rotation == math::Quaternion{0.5f, 0.0f, 0.0f, 1.0f},
                "Plain record staging did not start from a live default-constructed value.");
        take(runtime.destroy(object.id()));
        require(Counter::live == 0, "Generated destructor did not run.");
    }
    const std::vector<std::pair<std::string, std::string>> expected{{"test.binding", "evaluate.values"},
                                                                    {"test.note", std::string{"分组\0尾", 10}}};
    const MetadataList expected_type_meta{{"source.contract_method", "advance"}};
    const MetadataList expected_run_over{{"procedural.role", "run_over"}};
    const MetadataList expected_param{{"procedural.layer", "point"},
                                      {"procedural.name", "P"},
                                      {"procedural.optional", "true"},
                                      {"procedural.role", "in"},
                                      {"test.param", "ok"}};
    require(retained.fields[0].metadata == expected && retained_derived.metadata == expected_type_meta &&
                retained_derived.fields[1].metadata == expected_run_over &&
                retained_derived.methods[0].parameter_metadata.size() == 4 &&
                retained_derived.methods[0].parameter_metadata[0].empty() &&
                retained_derived.methods[0].parameter_metadata[1] == expected_param,
            "Generated type/field/parameter metadata did not outlive Runtime.");
    std::puts("Reflection consumer: PASS (generated producer and owned metadata)");
}
