#include "source/material_program.hpp"
#include "source/detail/bridge.hpp"
#include "runtime/detail/type_publication.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace atelier::source {
using namespace detail;
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Samples {
    std::span<const MaterialTexture> textures;
    char error[512]{};
};
std::uint32_t sample_texture(void* invocation, std::uint32_t slot, const float* uv, float* result) noexcept {
    auto& samples = *static_cast<Samples*>(invocation);
    try {
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) throw std::runtime_error("Material sample UV is not finite.");
        const auto texture = std::ranges::find(samples.textures, slot, &MaterialTexture::slot);
        if (texture == samples.textures.end() || !texture->sample) throw std::runtime_error("Missing material texture binding.");
        const auto value = take(texture->sample(texture->data, {uv[0], uv[1]}));
        for (std::size_t i = 0; i < 4; ++i)
            if (!std::isfinite(value[i])) throw std::runtime_error("Material sample result is not finite.");
        std::memcpy(result, &value, sizeof(value));
        return 0;
    } catch (const std::exception& error) { std::snprintf(samples.error, sizeof(samples.error), "%s", error.what()); }
    catch (...) { std::snprintf(samples.error, sizeof(samples.error), "%s", "Material sampler failed."); }
    return 1;
}
std::uint32_t sample_orientation(void* invocation, std::uint32_t slot, const float* uv, float* result) noexcept {
    auto& samples = *static_cast<Samples*>(invocation);
    try {
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) throw std::runtime_error("Material sample_orientation UV is not finite.");
        const auto texture = std::ranges::find(samples.textures, slot, &MaterialTexture::slot);
        if (texture == samples.textures.end()) throw std::runtime_error("Missing material texture binding.");
        if (!texture->orientation) throw std::runtime_error("Missing material sample_orientation callback.");
        const auto value = take(texture->orientation(texture->data, {uv[0], uv[1]}));
        for (std::size_t i = 0; i < 2; ++i)
            if (!std::isfinite(value[i])) throw std::runtime_error("Material sample_orientation result is not finite.");
        std::memcpy(result, &value, sizeof(value));
        return 0;
    } catch (const std::exception& error) { std::snprintf(samples.error, sizeof(samples.error), "%s", error.what()); }
    catch (...) { std::snprintf(samples.error, sizeof(samples.error), "%s", "Material sample_orientation failed."); }
    return 1;
}
bool finite_value(BusinessType type, const BusinessEnvironment& environment, const std::byte* bytes) {
    if (type == ValueKind::f32) {
        float value; std::memcpy(&value, bytes, sizeof(value));
        return std::isfinite(value);
    }
    if (type == ValueKind::boolean) return *bytes == std::byte{} || *bytes == std::byte{1};
    const auto& layout = environment.values[type.type_index];
    for (std::uint32_t i = 0; i < layout.field_count; ++i)
        if (!finite_value(layout.fields[i].type, environment, bytes + layout.fields[i].offset)) return false;
    return true;
}
void parameter_fields(BusinessType type, const BusinessEnvironment& environment, std::string path,
    std::size_t offset, std::vector<MaterialParameter>& fields) {
    if (type == ValueKind::f32) { fields.push_back({std::move(path), 1, offset}); return; }
    const auto& layout = environment.values[type.type_index];
    if (layout.math) { fields.push_back({std::move(path), static_cast<std::uint32_t>(layout.math.components()), offset}); return; }
    for (std::uint32_t i = 0; i < layout.field_count; ++i) {
        const auto& field = layout.fields[i];
        parameter_fields(field.type, environment, path.empty() ? field.name : path + "." + field.name,
            offset + field.offset, fields);
    }
}
}
struct MaterialProgram::State {
    std::unique_ptr<MaterialCode, decltype(&atelier_destroy_material)> code{nullptr, atelier_destroy_material};
    MaterialLayout context, output, parameters, resources;
    std::vector<MaterialParameter> fields;
    std::vector<MaterialTextureInput> textures;
    std::vector<std::byte> defaults;
    std::vector<std::byte> resource_values;
    bool supports_coverage{};
};
MaterialProgram::MaterialProgram(std::shared_ptr<const State> state) : state_(std::move(state)) {}
MaterialLayout MaterialProgram::context_layout() const noexcept { return state_->context; }
MaterialLayout MaterialProgram::output_layout() const noexcept { return state_->output; }
MaterialLayout MaterialProgram::parameter_layout() const noexcept { return state_->parameters; }
std::span<const MaterialParameter> MaterialProgram::parameters() const noexcept { return state_->fields; }
std::span<const std::byte> MaterialProgram::default_parameters() const noexcept { return state_->defaults; }
std::span<const MaterialTextureInput> MaterialProgram::textures() const noexcept { return state_->textures; }
Result<std::string> MaterialProgram::hlsl(std::string_view name_space) const {
    return generate_hlsl(name_space, false);
}
Result<std::string> MaterialProgram::hlsl_footprint(std::string_view name_space) const {
    return generate_hlsl(name_space, true);
}
Result<std::string> MaterialProgram::hlsl_coverage(std::string_view name_space) const {
    return generate_hlsl(name_space, false, true);
}
Result<std::string> MaterialProgram::generate_hlsl(std::string_view name_space, bool footprint, bool coverage) const {
    const auto letter = [](char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (name_space.empty() || !letter(name_space.front()) || !std::ranges::all_of(name_space,
        [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Material HLSL namespace requires an ASCII identifier."});
    if (coverage && !state_->supports_coverage)
        return std::unexpected(Error{ErrorCode::invalid_type, "Material coverage requires a top-level float coverage field in Output."});
    std::string result = footprint ?
        "float4 atelier_material_sample_grad(uint environment,uint local_slot,float2 uv,float2 d0,float2 d1);\n#define ATELIER_MATERIAL_FOOTPRINT 1\n" :
        "float4 atelier_material_sample(uint environment,uint local_slot,float2 uv);\n#define ATELIER_MATERIAL_FOOTPRINT 0\n";
    result += coverage ? "#define ATELIER_MATERIAL_COVERAGE 1\n" : "#define ATELIER_MATERIAL_COVERAGE 0\n";
    result += "float2 atelier_material_sample_orientation(uint environment,uint local_slot,float2 uv);\n";
    return result + "namespace " + std::string(name_space) + " {\n" +
        atelier_material_code_view(state_->code.get()).hlsl +
        "}\n#undef ATELIER_MATERIAL_FOOTPRINT\n#undef ATELIER_MATERIAL_COVERAGE\n";
}

Result<void> MaterialProgram::evaluate(std::span<const std::byte> context, std::span<const std::byte> parameters,
    std::span<std::byte> output, std::span<const MaterialTexture> textures) const {
    const auto state = state_; // Samplers may release the caller's last outer owner.
    if (context.size() != state->context.size || parameters.size() != state->parameters.size || output.size() != state->output.size)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Material input/output byte sizes do not match the captured layouts."});
    const auto view = atelier_material_code_view(state->code.get());
    for (const auto& input : state->textures) {
        const auto found = std::ranges::find(textures, input.slot, &MaterialTexture::slot);
        if (found == textures.end() || !found->sample)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Missing material texture input: " + input.key});
        if (std::ranges::count(textures, input.slot, &MaterialTexture::slot) != 1)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Duplicate material texture input: " + input.key});
    }
    const auto info = atelier_business_info(view.shade);
    if (!finite_value(info.signature.parameters[0].type, info.environment, context.data()) ||
        !finite_value(info.signature.parameters[1].type, info.environment, parameters.data()))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Material inputs must be finite values."});
    atelier::detail::ValueStorage context_storage, parameter_storage, output_storage, resource_storage;
    auto* context_data = context_storage.prepare(context.size(), state->context.alignment);
    auto* parameter_data = parameter_storage.prepare(parameters.size(), state->parameters.alignment);
    auto* output_data = output_storage.prepare(output.size(), state->output.alignment);
    std::memcpy(context_data, context.data(), context.size());
    std::memcpy(parameter_data, parameters.data(), parameters.size());
    BusinessValue arguments[3]{
        {info.signature.parameters[0].type, 0, ArgumentStorage::value, context_data},
        {info.signature.parameters[1].type, 0, ArgumentStorage::value, parameter_data}};
    if (view.resources != UINT32_MAX) {
        auto* data = resource_storage.prepare(state->resources.size, state->resources.alignment);
        std::memcpy(data, state->resource_values.data(), state->resource_values.size());
        arguments[2] = {info.signature.parameters[2].type, 0, ArgumentStorage::value, data};
    }
    BusinessValue result{info.signature.result, 0, ArgumentStorage::value, output_data};
    Samples samples{textures};
    char error[1024]{};
    if (atelier_invoke_business(view.shade, &samples, arguments, info.signature.parameter_count, &result, error, sizeof(error)))
        return std::unexpected(Error{ErrorCode::invocation_failed, *samples.error ? samples.error : error});
    if (!finite_value(info.signature.result, info.environment, output_data))
        return std::unexpected(Error{ErrorCode::invocation_failed, "Material output is not finite."});
    std::memcpy(output.data(), output_data, output.size());
    return {};
}

Result<std::shared_ptr<const MaterialProgram>> compile_material(Runtime& runtime, TypeId context, TypeId output,
    TypeId texture_input, std::string_view source_name, std::string_view source) {
    const auto fail = [&](Error error) {
        error.message = std::string(source_name) + ": " + error.message;
        return std::unexpected(std::move(error));
    };
    try {
        if (source.size() > UINT32_MAX) return fail(Error{ErrorCode::capacity_exceeded, "Material source is too large."});
        auto declarations = language::collect_declarations(source);
        if (!declarations) {
            const auto& diagnostic = declarations.error();
            return std::unexpected(Error{ErrorCode::invalid_definition, std::string(source_name) + ":" +
                std::to_string(diagnostic.position.line) + ":" + std::to_string(diagnostic.position.column) + ": " + diagnostic.message});
        }
        const auto original = bind_type(runtime, {});
        auto definitions = type_definitions(*declarations, original);
        if (!definitions) return fail(definitions.error());
        auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
        if (!publication) return fail(publication.error());
        const auto binding = bind_type(runtime, {}, (*publication)->catalog());
        const auto context_type = value_kind(binding, context), output_type = value_kind(binding, output);
        const auto texture_type = value_kind(binding, texture_input);
        if (context_type.kind != ValueKind::record || output_type.kind != ValueKind::record || texture_type.kind != ValueKind::record)
            return fail(Error{ErrorCode::invalid_type, "Material Context/Output/TextureInput must be registered plain records in this Runtime."});
        // Capture ordinary plain defaults while the compile catalog is alive.
        // Preview Source records compose the existing value defaults without
        // ever passing preview TypeIds to live Runtime operations.
        std::vector<std::vector<std::byte>> initial(binding.values.size());
        std::vector<bool> captured(binding.values.size());
        struct Defaults {
            std::function<const std::byte*(std::uint32_t)> capture;
            std::exception_ptr failure;
        } defaults;
        defaults.capture = [&](std::uint32_t index) -> const std::byte* {
            if (captured[index]) return initial[index].data();
            captured[index] = true;
            const auto& value = binding.values[index];
            initial[index].resize(value.size);
            if (runtime.describe(value.type)) {
                atelier::detail::ValueStorage storage;
                auto bytes = std::span(storage.prepare(value.size, value.alignment), value.size);
                take(runtime.initialize_value(value.type, bytes));
                std::memcpy(initial[index].data(), bytes.data(), bytes.size());
                take(runtime.destroy_value(value.type, bytes));
            } else for (const auto& field : value.fields) {
                if (field.type.kind != ValueKind::record) continue;
                defaults.capture(field.type.type_index);
                const auto& child = initial[field.type.type_index];
                if (!child.empty()) std::memcpy(initial[index].data() + field.offset, child.data(), child.size());
            }
            return initial[index].data();
        };
        const auto capture = [](void* state, std::uint32_t type) noexcept -> const std::byte* {
            auto& defaults = *static_cast<Defaults*>(state);
            try { return defaults.capture(type); }
            catch (...) { defaults.failure = std::current_exception(); return nullptr; }
        };
        EnvironmentView environment(binding);
        // Material locals never call native lifecycle operations: plain default
        // bytes are captured on demand, then construction/copy lowers to value copies.
        for (auto& value : environment.values) value.can_own = value.plain_value;
        const auto catalog = environment.view(nullptr);
        std::string name(source_name);
        auto state = std::make_shared<MaterialProgram::State>();
        MaterialCode* code{};
        char error[2048]{};
        if (atelier_compile_material(name.c_str(), source.data(), static_cast<std::uint32_t>(source.size()), &catalog,
            &defaults, capture, context_type.type_index, output_type.type_index, texture_type.type_index,
            sample_texture, sample_orientation, &code, error, sizeof(error))) {
            if (defaults.failure) std::rethrow_exception(defaults.failure);
            return std::unexpected(Error{ErrorCode::invalid_definition, error});
        }
        state->code.reset(code);
        const auto view = atelier_material_code_view(code);
        const auto info = atelier_business_info(view.shade);
        const auto& c = binding.values[context_type.type_index];
        const auto& o = binding.values[output_type.type_index];
        const auto& p = binding.values[view.parameters];
        state->supports_coverage = std::ranges::any_of(o.fields, [](const auto& field) {
            return field.name == "coverage" && field.type == ValueKind::f32;
        });
        if (!std::ranges::any_of(*declarations, [&](const auto& declaration) { return declaration.name == p.name && !declaration.enumeration; }))
            return fail(Error{ErrorCode::invalid_definition, "Material Params must be declared by this source."});
        if (view.resources != UINT32_MAX) {
            const auto& resources = binding.values[view.resources];
            if (!std::ranges::any_of(*declarations, [&](const auto& declaration) { return declaration.name == resources.name && !declaration.enumeration; }))
                return fail(Error{ErrorCode::invalid_definition, "Material Resources must be declared by this source."});
            state->resources = {resources.size, resources.alignment};
            state->resource_values.resize(resources.size);
            for (const auto& input : std::span(view.textures, view.texture_count)) {
                state->textures.push_back({input.key, input.slot});
                std::memcpy(state->resource_values.data() + input.offset, &input.slot, sizeof(input.slot));
            }
        }
        state->context = {c.size, c.alignment}; state->output = {o.size, o.alignment}; state->parameters = {p.size, p.alignment};
        parameter_fields({ValueKind::record, view.parameters}, info.environment, {}, 0, state->fields);
        atelier::detail::ValueStorage storage;
        auto* bytes = storage.prepare(p.size, p.alignment);
        BusinessValue result{{ValueKind::record, view.parameters}, 0, ArgumentStorage::value, bytes};
        Samples samples{};
        if (atelier_invoke_business(view.defaults, &samples, nullptr, 0, &result, error, sizeof(error)))
            return fail(Error{ErrorCode::invocation_failed, error});
        if (!finite_value(result.kind, info.environment, bytes))
            return fail(Error{ErrorCode::invalid_definition, "Material defaults must be finite."});
        state->defaults.assign(bytes, bytes + p.size);
        return std::shared_ptr<const MaterialProgram>(new MaterialProgram(std::move(state)));
    } catch (const std::exception& error) { return fail(Error{ErrorCode::invalid_definition, error.what()}); }
}
}
