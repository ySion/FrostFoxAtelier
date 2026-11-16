#include "source/material_program.hpp"
#include "source/detail/bridge.hpp"
#include "runtime/detail/type_publication.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>

namespace atelier::source {
using namespace detail;

namespace {
struct Samples {
    static constexpr std::size_t fast_slots = 16;

    const MaterialTexture* indexed[fast_slots];

    bool has_indexed{};

    std::span<const MaterialTexture> textures;

    // The backend stops at the first failed callback. Owning errors stay on
    // the host side; only the status crosses the C ABI.
    std::optional<Error> error{};

    const MaterialTexture* lookup(std::uint32_t slot) const noexcept {
        if (has_indexed && slot < fast_slots && indexed[slot])
            return indexed[slot];
        const auto found = std::ranges::find(textures, slot, &MaterialTexture::slot);
        return found == textures.end() ? nullptr : &*found;
    }
};

bool finite_float_bits(const void* address) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, address, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

std::uint32_t sample_texture(void* invocation, std::uint32_t slot, const float* uv, float* result) noexcept {
    auto& samples = *static_cast<Samples*>(invocation);
    if (!finite_float_bits(&uv[0]) || !finite_float_bits(&uv[1])) {
        samples.error = Error{ErrorCode::invocation_failed, "Material sample UV is not finite."};
        return 1;
    }
    const auto* texture = samples.lookup(slot);
    // Source can only copy injected texture tokens; evaluate verified each binding.
    assert(texture && texture->sample);
    auto value = texture->sample(texture->data, {uv[0], uv[1]});
    if (!value) {
        samples.error = std::move(value.error());
        return 1;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const float lane = (*value)[i];
        if (!finite_float_bits(&lane)) {
            samples.error = Error{ErrorCode::invocation_failed, "Material sample result is not finite."};
            return 1;
        }
    }
    std::memcpy(result, &*value, sizeof(*value));
    return 0;
}

std::uint32_t sample_orientation(void* invocation, std::uint32_t slot, const float* uv, float* result) noexcept {
    auto& samples = *static_cast<Samples*>(invocation);
    if (!finite_float_bits(&uv[0]) || !finite_float_bits(&uv[1])) {
        samples.error = Error{ErrorCode::invocation_failed, "Material sample_orientation UV is not finite."};
        return 1;
    }
    const auto* texture = samples.lookup(slot);
    assert(texture);
    if (!texture->orientation) {
        samples.error = Error{ErrorCode::invocation_failed, "Missing material sample_orientation callback."};
        return 1;
    }
    auto value = texture->orientation(texture->data, {uv[0], uv[1]});
    if (!value) {
        samples.error = std::move(value.error());
        return 1;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const float lane = (*value)[i];
        if (!finite_float_bits(&lane)) {
            samples.error = Error{ErrorCode::invocation_failed, "Material sample_orientation result is not finite."};
            return 1;
        }
    }
    std::memcpy(result, &*value, sizeof(*value));
    return 0;
}

struct CheckedLayout {
    std::vector<std::uint32_t> floats;

    std::vector<std::uint32_t> booleans;

    bool valid(const std::byte* bytes) const noexcept {
        for (const auto offset : floats)
            if (!finite_float_bits(bytes + offset))
                return false;
        for (const auto offset : booleans)
            if (static_cast<unsigned char>(bytes[offset]) > 1u)
                return false;
        return true;
    }
};

void collect_checked_layout(BusinessType type, const BusinessEnvironment& environment, std::uint32_t offset,
                            CheckedLayout& out) {
    if (type == ValueKind::f32) {
        out.floats.push_back(offset);
        return;
    }
    if (type == ValueKind::boolean) {
        out.booleans.push_back(offset);
        return;
    }
    if (type.kind != ValueKind::record)
        return;
    const auto& layout = environment.values[type.type_index];
    for (std::uint32_t i = 0; i < layout.field_count; ++i)
        collect_checked_layout(layout.fields[i].type, environment,
                               offset + static_cast<std::uint32_t>(layout.fields[i].offset), out);
}

void parameter_fields(BusinessType type, const BusinessEnvironment& environment, std::string path, std::size_t offset,
                      std::vector<MaterialParameter>& fields) {
    if (type == ValueKind::f32) {
        fields.push_back({std::move(path), 1, offset});
        return;
    }
    const auto& layout = environment.values[type.type_index];
    if (layout.math) {
        fields.push_back({std::move(path), static_cast<std::uint32_t>(layout.math.components()), offset});
        return;
    }
    for (std::uint32_t i = 0; i < layout.field_count; ++i) {
        const auto& field = layout.fields[i];
        parameter_fields(field.type, environment, path.empty() ? field.name : path + "." + field.name,
                         offset + field.offset, fields);
    }
}
}

struct MaterialProgram::State {
    std::unique_ptr<MaterialCode, decltype(&atelier_destroy_material)> code{nullptr, atelier_destroy_material};

    MaterialCodeView view{};

    BusinessEntry shade_entry{};

    BusinessType context_type{};

    BusinessType parameter_type{};

    BusinessType resource_type{};

    CheckedLayout context_check;

    CheckedLayout parameter_check;

    CheckedLayout output_check;

    MaterialLayout context;

    MaterialLayout output;

    MaterialLayout parameters;

    MaterialLayout resources;

    std::vector<MaterialParameter> fields;

    std::vector<MaterialTextureInput> textures;

    std::vector<std::byte> defaults;

    std::vector<std::byte> resource_values;

    bool supports_coverage{};
};

MaterialProgram::MaterialProgram(std::shared_ptr<const State> state) : state_(std::move(state)) {}

MaterialLayout MaterialProgram::context_layout() const noexcept {
    return state_->context;
}

MaterialLayout MaterialProgram::output_layout() const noexcept {
    return state_->output;
}

MaterialLayout MaterialProgram::parameter_layout() const noexcept {
    return state_->parameters;
}

std::span<const MaterialParameter> MaterialProgram::parameters() const noexcept {
    return state_->fields;
}

std::span<const std::byte> MaterialProgram::default_parameters() const noexcept {
    return state_->defaults;
}

std::span<const MaterialTextureInput> MaterialProgram::textures() const noexcept {
    return state_->textures;
}

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
    if (name_space.empty() || !letter(name_space.front()) ||
        !std::ranges::all_of(name_space, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Material HLSL namespace requires an ASCII identifier."});
    if (coverage && !state_->supports_coverage)
        return std::unexpected(
            Error{ErrorCode::invalid_type, "Material coverage requires a top-level float coverage field in Output."});
    std::string result =
        footprint
            ? "float4 atelier_material_sample_grad(uint environment,uint local_slot,float2 uv,float2 d0,float2 d1);\n#define ATELIER_MATERIAL_FOOTPRINT 1\n"
            : "float4 atelier_material_sample(uint environment,uint local_slot,float2 uv);\n#define ATELIER_MATERIAL_FOOTPRINT 0\n";
    result += coverage ? "#define ATELIER_MATERIAL_COVERAGE 1\n" : "#define ATELIER_MATERIAL_COVERAGE 0\n";
    result += "float2 atelier_material_sample_orientation(uint environment,uint local_slot,float2 uv);\n";
    return result + "namespace " + std::string(name_space) + " {\n" + state_->view.hlsl +
           "}\n#undef ATELIER_MATERIAL_FOOTPRINT\n#undef ATELIER_MATERIAL_COVERAGE\n";
}

Result<void> MaterialProgram::evaluate(std::span<const std::byte> context, std::span<const std::byte> parameters,
                                       std::span<std::byte> output,
                                       std::span<const MaterialTexture> textures) const noexcept {
    const auto state = state_; // Samplers may release the caller's last outer owner.
    if (context.size() != state->context.size || parameters.size() != state->parameters.size ||
        output.size() != state->output.size)
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Material input/output byte sizes do not match the captured layouts."});
    Samples samples{};
    samples.textures = textures;
    if (!state->textures.empty()) {
        samples.has_indexed = true;
        for (const auto& input : state->textures) {
            const MaterialTexture* match = nullptr;
            std::size_t count = 0;
            for (const auto& item : textures) {
                if (item.slot == input.slot) {
                    if (!match)
                        match = &item;
                    ++count;
                }
            }
            if (!match || !match->sample)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "Missing material texture input: " + input.key});
            if (count != 1)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "Duplicate material texture input: " + input.key});
            if (input.slot < Samples::fast_slots)
                samples.indexed[input.slot] = match;
        }
    }
    if (!state->context_check.valid(context.data()) || !state->parameter_check.valid(parameters.data()))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Material inputs must be finite values."});
    atelier::detail::ValueStorage output_storage;
    auto* output_data = output_storage.prepare(output.size(), state->output.alignment);
    std::byte empty{};
    BusinessValue arguments[3]{
        {state->context_type, 0, ArgumentStorage::value, context.empty() ? &empty : context.data()},
        {state->parameter_type, 0, ArgumentStorage::value, parameters.empty() ? &empty : parameters.data()}};
    if (state->view.resources != UINT32_MAX) {
        arguments[2] = {state->resource_type, 0, ArgumentStorage::value,
                        state->resource_values.empty() ? &empty : state->resource_values.data()};
    }
    BusinessFailure failure{};
    if (state->shade_entry(&samples, arguments, output_data, &failure) != 0) {
        if (samples.error)
            return std::unexpected(std::move(*samples.error));
        const auto* reason = failure_message(failure.reason);
        return std::unexpected(Error{ErrorCode::invocation_failed, reason ? reason : "Material evaluation failed."});
    }
    if (!state->output_check.valid(output_data))
        return std::unexpected(Error{ErrorCode::invocation_failed, "Material output is not finite."});
    std::memcpy(output.data(), output_data, output.size());
    return {};
}

Result<std::shared_ptr<const MaterialProgram>> compile_material(Runtime& runtime, TypeId context, TypeId output,
                                                                TypeId texture_input, std::string_view source_name,
                                                                std::string_view source) noexcept {
    const auto fail = [&](Error error) {
        error.message = std::string(source_name) + ": " + error.message;
        return std::unexpected(std::move(error));
    };
    if (source.size() > UINT32_MAX)
        return fail(Error{ErrorCode::capacity_exceeded, "Material source is too large."});
    auto declarations = language::collect_declarations(source);
    if (!declarations) {
        const auto& diagnostic = declarations.error();
        return std::unexpected(Error{ErrorCode::invalid_definition,
                                     std::string(source_name) + ":" + std::to_string(diagnostic.position.line) + ":" +
                                         std::to_string(diagnostic.position.column) + ": " + diagnostic.message});
    }
    const auto original = bind_type(runtime, {});
    if (!original)
        return fail(original.error());
    auto definitions = type_definitions(*declarations, *original);
    if (!definitions)
        return fail(definitions.error());
    auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
    if (!publication)
        return fail(publication.error());
    const auto binding = bind_type(runtime, {}, (*publication)->catalog());
    if (!binding)
        return fail(binding.error());
    const auto context_type = value_kind(*binding, context);
    const auto output_type = value_kind(*binding, output);
    const auto texture_type = value_kind(*binding, texture_input);
    if (context_type.kind != ValueKind::record || output_type.kind != ValueKind::record ||
        texture_type.kind != ValueKind::record || binding->values[context_type.type_index].math ||
        binding->values[output_type.type_index].math)
        return fail(Error{ErrorCode::invalid_type,
                          "Material Context/Output/TextureInput must be registered plain records in this Runtime."});
    // Capture ordinary plain defaults while the compile catalog is alive.
    // Preview Source records compose the existing value defaults without
    // ever passing preview TypeIds to live Runtime operations.
    std::vector<std::vector<std::byte>> initial(binding->values.size());
    std::vector<bool> captured(binding->values.size());
    struct Defaults {
        std::function<Result<const std::byte*>(std::uint32_t)> capture;

        std::optional<Error> error;
    } defaults;
    defaults.capture = [&](std::uint32_t index) noexcept -> Result<const std::byte*> {
        if (captured[index])
            return initial[index].data();
        captured[index] = true;
        const auto& value = binding->values[index];
        initial[index].resize(value.size);
        if (runtime.describe(value.type)) {
            atelier::detail::ValueStorage storage;
            auto bytes = std::span(storage.prepare(value.size, value.alignment), value.size);
            ATELIER_TRY(runtime.initialize_value(value.type, bytes));
            std::memcpy(initial[index].data(), bytes.data(), bytes.size());
            ATELIER_TRY(runtime.destroy_value(value.type, bytes));
        } else
            for (const auto& field : value.fields) {
                if (field.type.kind != ValueKind::record)
                    continue;
                ATELIER_TRY_BIND(result, defaults.capture(field.type.type_index));
                const auto& child = initial[field.type.type_index];
                if (!child.empty())
                    std::memcpy(initial[index].data() + field.offset, child.data(), child.size());
            }
        return initial[index].data();
    };
    const auto capture = [](void* state, std::uint32_t type) noexcept -> const std::byte* {
        auto& defaults = *static_cast<Defaults*>(state);
        auto result = defaults.capture(type);
        if (result)
            return *result;
        defaults.error = std::move(result.error());
        return nullptr;
    };
    EnvironmentView environment(*binding);
    // Material locals never call native lifecycle operations: plain default
    // bytes are captured on demand, then construction/copy lowers to value copies.
    for (auto& value : environment.values)
        value.can_own = value.plain_value;
    const auto catalog = environment.view(nullptr);
    std::string name(source_name);
    auto state = std::make_shared<MaterialProgram::State>();
    MaterialCode* code{};
    char error[2048]{};
    if (atelier_compile_material(name.c_str(), source.data(), static_cast<std::uint32_t>(source.size()), &catalog,
                                 &defaults, capture, context_type.type_index, output_type.type_index,
                                 texture_type.type_index, sample_texture, sample_orientation, &code, error,
                                 sizeof(error))) {
        if (defaults.error)
            return std::unexpected(std::move(*defaults.error));
        return std::unexpected(Error{ErrorCode::invalid_definition, error});
    }
    state->code.reset(code);
    const auto view = atelier_material_code_view(code);
    const auto info = atelier_business_info(view.shade);
    state->view = view;
    state->shade_entry = reinterpret_cast<BusinessEntry>(info.entry_address);
    state->context_type = info.signature.parameters[0].type;
    state->parameter_type = info.signature.parameters[1].type;
    if (view.resources != UINT32_MAX)
        state->resource_type = info.signature.parameters[2].type;
    collect_checked_layout(state->context_type, info.environment, 0, state->context_check);
    collect_checked_layout(state->parameter_type, info.environment, 0, state->parameter_check);
    collect_checked_layout(info.signature.result, info.environment, 0, state->output_check);
    const auto& c = binding->values[context_type.type_index];
    const auto& o = binding->values[output_type.type_index];
    const auto& p = binding->values[view.parameters];
    state->supports_coverage = std::ranges::any_of(
        o.fields, [](const auto& field) { return field.name == "coverage" && field.type == ValueKind::f32; });
    if (!std::ranges::any_of(*declarations, [&](const auto& declaration) {
            return declaration.name == p.name && !declaration.enumeration;
        }))
        return fail(Error{ErrorCode::invalid_definition, "Material Params must be declared by this source."});
    if (view.resources != UINT32_MAX) {
        const auto& resources = binding->values[view.resources];
        if (!std::ranges::any_of(*declarations, [&](const auto& declaration) {
                return declaration.name == resources.name && !declaration.enumeration;
            }))
            return fail(Error{ErrorCode::invalid_definition, "Material Resources must be declared by this source."});
        state->resources = {resources.size, resources.alignment};
        state->resource_values.resize(resources.size);
        for (const auto& input : std::span(view.textures, view.texture_count)) {
            state->textures.push_back({input.key, input.slot});
            std::memcpy(state->resource_values.data() + input.offset, &input.slot, sizeof(input.slot));
        }
    }
    state->context = {c.size, c.alignment};
    state->output = {o.size, o.alignment};
    state->parameters = {p.size, p.alignment};
    parameter_fields({ValueKind::record, view.parameters}, info.environment, {}, 0, state->fields);
    atelier::detail::ValueStorage storage;
    auto* bytes = storage.prepare(p.size, p.alignment);
    BusinessValue result{{ValueKind::record, view.parameters}, 0, ArgumentStorage::value, bytes};
    Samples samples{};
    if (atelier_invoke_business(view.defaults, &samples, nullptr, 0, &result, error, sizeof(error)))
        return fail(Error{ErrorCode::invocation_failed, error});
    if (!state->parameter_check.valid(bytes))
        return fail(Error{ErrorCode::invalid_definition, "Material defaults must be finite."});
    state->defaults.assign(bytes, bytes + p.size);
    return std::shared_ptr<const MaterialProgram>(new MaterialProgram(std::move(state)));
}
}
