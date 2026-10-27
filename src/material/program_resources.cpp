#include "material/program_resources.hpp"
#include "material/resources.hpp"
#include "runtime/native.hpp"

#include <cstring>

namespace atelier::material {
namespace {
static_assert(std::is_nothrow_move_assignable_v<ReferenceField>);
auto failure(std::string message) { return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)}); }
bool valid_key(std::string_view value) { return value.size() <= 4096 && value.find('\0') == value.npos; }
bool finite(math::Vector4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
bool same(const SurfaceInput& a, const SurfaceInput& b) {
    return a.image == b.image && a.address == b.address && a.color == b.color &&
        a.empty_value.x == b.empty_value.x && a.empty_value.y == b.empty_value.y &&
        a.empty_value.z == b.empty_value.z && a.empty_value.w == b.empty_value.w;
}
ProgramSurfaceRef reuse(ProgramSurfaceRef old, ProgramSurfaceRef next) {
    if (old && old->definition() == next->definition() &&
        std::ranges::equal(old->parameters(), next->parameters()) &&
        std::ranges::equal(old->inputs(), next->inputs(), same)) return old;
    return next;
}
// The CPU contract is bilinear LOD0, RGB-only sRGB decode before interpolation.
// Empty values are already linear. UV/channel transformations belong to Source.
Result<math::Vector4> sample_input(const void* data, math::Vector2 uv) {
    const auto& input = *static_cast<const SurfaceInput*>(data);
    if (!input.image) return input.empty_value;
    const auto& image = *input.image;
    const std::array extent{int(image.width()), int(image.height())};
    std::array<int, 2> lower{};
    std::array<double, 2> weight{};
    for (std::size_t axis=0; axis<2; ++axis) {
        double point = uv[axis];
        if (input.address == Address::repeat) point -= std::floor(point);
        else if (input.address == Address::mirror) {
            point -= std::floor(point * .5) * 2;
            if (point > 1) point = 2 - point;
        } else point = std::clamp(point, 0., 1.);
        point = point * extent[axis] - .5;
        lower[axis] = static_cast<int>(std::floor(point));
        weight[axis] = point - lower[axis];
    }
    const auto coordinate = [&](int value, int size) {
        return input.address == Address::repeat ? (value + size) % size : std::clamp(value, 0, size - 1);
    };
    const auto texel = [&](int x, int y, std::size_t channel) {
        const auto index = std::size_t(coordinate(y, extent[1]) * extent[0] + coordinate(x, extent[0]));
        const double value = double(image.pixels()[index * 4 + channel]) / 255;
        return channel == 3 || input.color == TextureColor::linear ? value :
            value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
    };
    math::Vector4 result;
    for (std::size_t channel=0; channel<4; ++channel)
        result[channel] = static_cast<float>(std::lerp(
            std::lerp(texel(lower[0], lower[1], channel), texel(lower[0]+1, lower[1], channel), weight[0]),
            std::lerp(texel(lower[0], lower[1]+1, channel), texel(lower[0]+1, lower[1]+1, channel), weight[0]), weight[1]));
    return result;
}
Result<void> validate_binding(const TextureBinding& binding, const ImageRef& image) {
    if (!valid_key(binding.asset_key) || binding.address > Address::mirror)
        return failure("Invalid texture key or address mode.");
    if (binding.asset_key.empty() ? bool(image) : !image)
        return failure("A nonempty texture key requires exact image content; clearing requires both to be empty.");
    return {};
}
Result<bool> source_role(Runtime& runtime, ObjectId id) {
    auto type = runtime.type_of(id); if (!type) return std::unexpected(type.error());
    auto source = runtime.native_type<MaterialSource>(); if (!source) return std::unexpected(source.error());
    auto instance = runtime.native_type<MaterialInstance>(); if (!instance) return std::unexpected(instance.error());
    if (*type == *source) return true;
    if (*type == *instance) return false;
    return failure("Expected an actual material.Source or material.Instance, not the common resource role.");
}
template<class T> Result<void> exact_type(Runtime& runtime, ObjectId id) {
    auto type = runtime.type_of(id); if (!type) return std::unexpected(type.error());
    auto wanted = runtime.native_type<T>(); if (!wanted) return std::unexpected(wanted.error());
    if (*type != *wanted) return failure("Material operation does not match the object's actual role.");
    return {};
}
}

namespace detail {
struct ProgramAccess {
    static ProgramSurfaceRef surface(DefinitionRef definition, std::vector<std::byte> parameters,
        std::vector<SurfaceInput> inputs) {
        return ProgramSurfaceRef(new ProgramSurface(std::move(definition), std::move(parameters), std::move(inputs)));
    }
    static Result<void> register_objects(Runtime& runtime) {
        auto resource = NativeRegistration<Material>{runtime, "material.Resource", TypeKind::object}
            .hidden_field<&Material::textures_>("textures", {TypeReference::native<TextureResource>(), ValueForm::strong_sequence}).build();
        auto source = NativeRegistration<MaterialSource>{runtime, "material.Source", TypeKind::object}
            .base<Material>().build();
        auto instance = NativeRegistration<MaterialInstance>{runtime, "material.Instance", TypeKind::object}
            .base<Material>()
            .hidden_field<&MaterialInstance::parent_>("parent", {TypeReference::native<Material>(), ValueForm::strong_reference}).build();
        auto registered = runtime.register_types({std::move(resource), std::move(source), std::move(instance)});
        if (!registered) return std::unexpected(registered.error());
        return {};
    }
    static Result<ImageRef> image(Runtime& runtime, ObjectId id, std::string_view key) {
        return runtime.with_native<const TextureResource>(id, [&](const auto& value) -> Result<ImageRef> {
            if (value.source_.view().std_view() != key) return failure("Texture root does not match the authored key.");
            return value.image_;
        });
    }
    struct Textures { std::vector<ImageRef> images; std::vector<ObjectReference> references; };
    template<class Binding> static Result<Textures> textures(Runtime& runtime, std::span<const ObjectRoot> roots, Binding binding) {
        auto type = runtime.native_type<TextureResource>(); if (!type) return std::unexpected(type.error());
        auto empty = runtime.null_reference(*type); if (!empty) return std::unexpected(empty.error());
        Textures result;
        result.images.resize(roots.size()); result.references.assign(roots.size(), *empty);
        for (std::size_t i=0; i<roots.size(); ++i) {
            if (!roots[i].registered()) continue;
            const auto* input = binding(i);
            if (!input || input->asset_key.empty()) return failure("A numeric or explicitly cleared input cannot own a texture root.");
            auto held = runtime.hold(roots[i].reference()); if (!held) return std::unexpected(held.error());
            auto content = image(runtime, held->id(), input->asset_key); if (!content) return std::unexpected(content.error());
            result.images[i] = std::move(*content); result.references[i] = held->reference();
        }
        return result;
    }
    // Called only under a const borrow of value. Immutable texture objects need
    // no write stamp; checking their formal identity catches explicit destroy.
    static Result<void> validate_images(Runtime& runtime, const Material& value, std::span<const Override> entries = {}) {
        auto size = runtime.sequence_size(value.texture_field_); if (!size) return std::unexpected(size.error());
        if (*size != value.images_.size()) return failure("Material resource edges do not match its captured inputs.");
        for (std::size_t i=0; i<*size; ++i) {
            if (!entries.empty() && !entries[i].enabled) continue;
            auto reference = runtime.sequence_read(value.texture_field_, i); if (!reference) return std::unexpected(reference.error());
            if (reference->is_null()) {
                if (value.images_[i]) return failure("A required material texture was destroyed.");
                continue;
            }
            auto checked = runtime.with_native<const TextureResource>(reference->id(), [&](const auto& texture) -> Result<void> {
                if (texture.image_ != value.images_[i]) return failure("Material texture content changed outside publication.");
                return {};
            });
            if (!checked) return checked;
        }
        return {};
    }
    static Result<std::vector<ObjectRoot>> roots(Runtime& runtime, const Material& value) {
        auto size = runtime.sequence_size(value.texture_field_); if (!size) return std::unexpected(size.error());
        std::vector<ObjectRoot> result(*size);
        for (std::size_t i=0; i<*size; ++i) {
            auto reference = runtime.sequence_read(value.texture_field_, i); if (!reference) return std::unexpected(reference.error());
            if (reference->is_null()) continue;
            auto held = runtime.hold(*reference); if (!held) return std::unexpected(held.error());
            result[i] = std::move(*held);
        }
        return result;
    }
    static Result<ObjectReference> parent(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<ObjectReference> {
            if (!value.published_) return failure("Material instance is not initialized.");
            return runtime.read_reference(value.parent_field_);
        });
    }
    static Result<void> check_parent(Runtime& runtime, ObjectId target, ObjectId current) {
        for (;;) {
            if (current == target) return failure("Material inheritance must be acyclic.");
            auto source = source_role(runtime, current); if (!source) return std::unexpected(source.error());
            if (*source) return {};
            auto next = parent(runtime, current); if (!next) return std::unexpected(next.error());
            if (next->is_null()) return failure("Material instance lost its parent.");
            current = next->id();
        }
    }
    template<class T, class State> static Result<ObjectRoot> create(Runtime& runtime, std::string_view key, const State& state) {
        if (auto ready = runtime.check_code_publication(); !ready) return std::unexpected(ready.error());
        if (!valid_key(key) || key.starts_with("builtin/")) return failure("Project material creation cannot claim an engine-reserved key.");
        auto type = runtime.native_type<T>(); if (!type) return std::unexpected(type.error());
        auto root = runtime.create(*type); if (!root) return std::unexpected(root.error());
        auto initialized = runtime.with_native<T>(root->id(), [&](auto& value) -> Result<void> {
            value.asset_key_ = key;
            return {};
        });
        if (!initialized) return std::unexpected(initialized.error());
        Result<void> written;
        if constexpr (std::same_as<T, MaterialSource>) written = update_source(runtime, root->id(), state);
        else written = update_instance(runtime, root->id(), state);
        if (!written) return std::unexpected(written.error());
        return std::move(*root);
    }
    static Result<std::string> key(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const Material>(id, [](const auto& value) -> Result<std::string> {
            if (!value.published_) return failure("Material has no valid publication.");
            return value.asset_key_;
        });
    }
    static Result<SourceState> read_source(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialSource>(id, [&](const auto& value) -> Result<SourceState> {
            if (!value.published_) return failure("Material source is not initialized.");
            auto textures = roots(runtime, value); if (!textures) return std::unexpected(textures.error());
            return SourceState{value.definition_, std::move(*textures)};
        });
    }
    static Result<InstanceState> read_instance(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<InstanceState> {
            if (!value.published_) return failure("Material instance is not initialized.");
            auto reference = runtime.read_reference(value.parent_field_); if (!reference) return std::unexpected(reference.error());
            auto held = runtime.hold(*reference); if (!held) return std::unexpected(held.error());
            auto textures = roots(runtime, value); if (!textures) return std::unexpected(textures.error());
            return InstanceState{std::move(*held), value.entries_, std::move(*textures)};
        });
    }
    static Result<std::vector<AuthorDiagnostic>> diagnostics(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialInstance>(id, [](const auto& value) -> Result<std::vector<AuthorDiagnostic>> {
            if (!value.published_) return failure("Material instance is not initialized.");
            return value.diagnostics_;
        });
    }
    static Result<void> update_source(Runtime& runtime, ObjectId id, const SourceState& state) {
        if (!state.definition || state.default_textures.size() != state.definition->textures().size())
            return failure("Source defaults require one exact root slot per declared texture.");
        auto prepared = textures(runtime, state.default_textures, [&](std::size_t i) { return &state.definition->textures()[i].binding; });
        if (!prepared) return std::unexpected(prepared.error());
        auto surface = resolve_source(state.definition, prepared->images); if (!surface) return std::unexpected(surface.error());
        auto definition = state.definition;
        return runtime.with_native<MaterialSource>(id, [&](auto& value) -> Result<void> {
            auto field = runtime.reference_field(id, value.textures_); if (!field) return std::unexpected(field.error());
            auto size = runtime.sequence_size(*field); if (!size) return std::unexpected(size.error());
            const std::array<ReferenceChange, 1> changes{SequenceSplice{*field, 0, *size, prepared->references}};
            *surface = reuse(value.published_, std::move(*surface));
            auto written = runtime.apply_reference_changes(changes); if (!written) return written;
            // No allocation, validation or callbacks after the reference commit.
            value.texture_field_ = std::move(*field);
            value.images_.swap(prepared->images);
            value.definition_.swap(definition);
            value.published_.swap(*surface);
            return {};
        });
    }
    static Result<void> update_instance(Runtime& runtime, ObjectId id, const InstanceState& state) {
        if (!state.parent.registered() || state.textures.size() != state.entries.size())
            return failure("An instance needs a parent and one root slot per author entry.");
        auto held = runtime.hold(state.parent.reference()); if (!held) return std::unexpected(held.error());
        if (auto valid = check_parent(runtime, id, held->id()); !valid) return valid;
        auto parent_surface = capture_program(runtime, held->id()); if (!parent_surface) return std::unexpected(parent_surface.error());
        auto prepared = textures(runtime, state.textures, [&](std::size_t i) { return std::get_if<TextureBinding>(&state.entries[i].value); });
        if (!prepared) return std::unexpected(prepared.error());
        auto resolved = resolve_instance(*parent_surface, state.entries, prepared->images); if (!resolved) return std::unexpected(resolved.error());
        auto entries = state.entries;
        return runtime.with_native<MaterialInstance>(id, [&](auto& value) -> Result<void> {
            auto field = runtime.reference_field(id, value.textures_); if (!field) return std::unexpected(field.error());
            auto size = runtime.sequence_size(*field); if (!size) return std::unexpected(size.error());
            auto parent_field = runtime.reference_field(id, value.parent_); if (!parent_field) return std::unexpected(parent_field.error());
            const std::array<ReferenceChange, 2> changes{SequenceSplice{*field, 0, *size, prepared->references},
                ReferenceWrite{*parent_field, held->reference()}};
            resolved->surface = reuse(value.published_, std::move(resolved->surface));
            auto written = runtime.apply_reference_changes(changes); if (!written) return written;
            value.texture_field_ = std::move(*field);
            value.parent_field_ = std::move(*parent_field);
            value.images_.swap(prepared->images);
            value.entries_.swap(entries);
            value.parent_surface_.swap(*parent_surface);
            value.diagnostics_.swap(resolved->diagnostics);
            value.published_.swap(resolved->surface);
            return {};
        });
    }
    static Result<ProgramSurfaceRef> capture(Runtime& runtime, ObjectId id, bool source) {
        if (source) return runtime.with_native<const MaterialSource>(id, [&](const auto& value) -> Result<ProgramSurfaceRef> {
            if (!value.published_) return failure("Material source is not initialized.");
            if (auto valid = validate_images(runtime, value); !valid) return std::unexpected(valid.error());
            return value.published_;
        });
        ProgramSurfaceRef next_parent;
        std::optional<InstanceResolution> refreshed;
        auto captured = runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<ProgramSurfaceRef> {
            if (!value.published_) return failure("Material instance is not initialized.");
            auto reference = runtime.read_reference(value.parent_field_); if (!reference) return std::unexpected(reference.error());
            if (reference->is_null()) return failure("Material instance lost its parent.");
            auto parent_surface = capture_program(runtime, reference->id()); if (!parent_surface) return std::unexpected(parent_surface.error());
            if (auto valid = validate_images(runtime, value, value.entries_); !valid) return std::unexpected(valid.error());
            if (*parent_surface == value.parent_surface_) return value.published_;
            auto resolved = resolve_instance(*parent_surface, value.entries_, value.images_); if (!resolved) return std::unexpected(resolved.error());
            resolved->surface = reuse(value.published_, std::move(resolved->surface));
            next_parent = std::move(*parent_surface);
            refreshed = std::move(*resolved);
            return refreshed->surface;
        });
        if (!captured || !refreshed) return captured;
        // End this object's const borrow before publishing the prepared cache.
        // An outer mesh borrow remains permitted; no publication admission here.
        return runtime.with_native<MaterialInstance>(id, [&](auto& value) -> Result<ProgramSurfaceRef> {
            value.parent_surface_.swap(next_parent);
            value.diagnostics_.swap(refreshed->diagnostics);
            value.published_.swap(refreshed->surface);
            return value.published_;
        });
    }
};
}

SourceDefinition::SourceDefinition(std::shared_ptr<const source::MaterialProgram> program, Policy policy,
    std::vector<TextureDefault> textures) : program_(std::move(program)), policy_(policy), textures_(std::move(textures)) {}

Result<DefinitionRef> make_definition(std::shared_ptr<const source::MaterialProgram> program, Policy policy,
    std::span<const TextureDefault> textures) {
    try {
        if (!program || policy.coverage > Coverage::mask) return failure("Invalid material program or coverage policy.");
        if (program->context_layout().size != sizeof(Context) || program->context_layout().alignment != alignof(Context) ||
            program->output_layout().size != sizeof(Output) || program->output_layout().alignment != alignof(Output))
            return failure("Program does not use the material Context/Output layout.");
        if (textures.size() != program->textures().size()) return failure("All declared texture defaults must be specified exactly once.");
        std::vector<TextureDefault> ordered;
        ordered.reserve(textures.size());
        for (const auto& input : program->textures()) {
            auto found = std::ranges::find(textures, input.key, &TextureDefault::path);
            if (found == textures.end() || std::ranges::count(textures, input.key, &TextureDefault::path) != 1)
                return failure("Missing or repeated material texture default: " + input.key);
            if (!valid_key(found->binding.asset_key) || found->binding.address > Address::mirror ||
                found->color > TextureColor::srgb || !finite(found->empty_value))
                return failure("Invalid material texture default: " + input.key);
            ordered.push_back(*found);
        }
        return DefinitionRef(new SourceDefinition(std::move(program), policy, std::move(ordered)));
    } catch (const std::exception& error) { return failure(error.what()); }
}

ProgramSurface::ProgramSurface(DefinitionRef definition, std::vector<std::byte> parameters, std::vector<SurfaceInput> inputs)
    : definition_(std::move(definition)), parameters_(std::move(parameters)), inputs_(std::move(inputs)) {
    samplers_.reserve(inputs_.size());
    for (std::size_t i=0; i<inputs_.size(); ++i) samplers_.push_back({program().textures()[i].slot, &inputs_[i], sample_input});
}
Result<Output> ProgramSurface::evaluate(const Context& context) const {
    Output output;
    auto result = program().evaluate(std::as_bytes(std::span{&context, 1}), parameters_,
        std::as_writable_bytes(std::span{&output, 1}), samplers_);
    if (!result) return std::unexpected(result.error());
    return output;
}
Result<ProgramSurfaceRef> resolve_source(DefinitionRef definition, std::span<const ImageRef> images) {
    try {
        if (!definition || images.size() != definition->textures().size()) return failure("Source image table does not match its definition.");
        std::vector<SurfaceInput> inputs;
        inputs.reserve(images.size());
        for (std::size_t i=0; i<images.size(); ++i) {
            const auto& input = definition->textures()[i];
            if (auto valid = validate_binding(input.binding, images[i]); !valid)
                return failure(input.path + ": " + valid.error().message);
            inputs.push_back({images[i], input.binding.address, input.color, input.empty_value});
        }
        const auto defaults = definition->program().default_parameters();
        return detail::ProgramAccess::surface(std::move(definition), {defaults.begin(), defaults.end()}, std::move(inputs));
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<InstanceResolution> resolve_instance(ProgramSurfaceRef parent, std::span<const Override> entries,
    std::span<const ImageRef> images) {
    try {
        if (!parent || images.size() != entries.size()) return failure("Instance resolution requires a parent candidate and one image slot per author entry.");
        if (entries.empty()) return InstanceResolution{std::move(parent), {}};
        std::vector<std::byte> parameters(parent->parameters().begin(), parent->parameters().end());
        std::vector<SurfaceInput> inputs(parent->inputs().begin(), parent->inputs().end());
        std::vector<AuthorDiagnostic> diagnostics;
        const auto numeric = parent->program().parameters();
        const auto textures = parent->program().textures();
        for (std::size_t i=0; i<entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.path.empty() || !valid_key(entry.path) || std::ranges::find(entries.first(i), entry.path, &Override::path) != entries.first(i).end())
                return failure("Author override paths must be nonempty and unique.");
            const auto parameter = std::ranges::find(numeric, entry.path, &source::MaterialParameter::key);
            const auto texture = std::ranges::find(textures, entry.path, &source::MaterialTextureInput::key);
            std::string issue;
            const auto* binding = std::get_if<TextureBinding>(&entry.value);
            if (binding) {
                if (!valid_key(binding->asset_key) || binding->address > Address::mirror)
                    return failure(entry.path + ": invalid authored texture binding.");
                if (texture == textures.end()) issue = "Texture declaration is absent or has a different type.";
                else if (auto valid = validate_binding(*binding, images[i]); !valid) issue = valid.error().message;
                if (issue.empty() && entry.enabled) {
                    auto& input = inputs[static_cast<std::size_t>(texture - textures.begin())];
                    input.image = images[i]; input.address = binding->address;
                }
            } else {
                if (images[i]) return failure(entry.path + ": numeric author entry cannot carry image content.");
                std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (!std::same_as<T, TextureBinding>) {
                        constexpr std::size_t count = sizeof(T) / sizeof(float);
                        bool valid = true;
                        if constexpr (std::same_as<T, float>) valid = std::isfinite(value);
                        else for (std::size_t c=0; c<count; ++c) valid &= std::isfinite(value[c]);
                        if (!valid) issue = "Numeric author values must be finite.";
                        else if (parameter == numeric.end() || parameter->components != count)
                            issue = "Numeric declaration is absent or has a different type.";
                        else if (entry.enabled) std::memcpy(parameters.data() + parameter->offset, &value, sizeof(T));
                    }
                }, entry.value);
            }
            if (!issue.empty()) {
                if (entry.enabled) return failure(entry.path + ": " + issue);
                diagnostics.push_back({entry.path, std::move(issue)});
            }
        }
        auto next = detail::ProgramAccess::surface(parent->definition(), std::move(parameters), std::move(inputs));
        return InstanceResolution{reuse(std::move(parent), std::move(next)), std::move(diagnostics)};
    } catch (const std::exception& error) { return failure(error.what()); }
}

Result<void> register_resource_types(Runtime& runtime) {
    if (runtime.native_type<MaterialSource>() && runtime.native_type<MaterialInstance>()) return {};
    if (auto registered = register_types(runtime); !registered) return registered;
    if (auto registered = register_program_types(runtime); !registered) return registered;
    return detail::ProgramAccess::register_objects(runtime);
}
Result<ObjectRoot> create_source(Runtime& runtime, std::string_view key, const SourceState& state) {
    try { return detail::ProgramAccess::create<MaterialSource>(runtime, key, state); }
    catch (const std::exception& error) { return failure(error.what()); }
}
Result<ObjectRoot> create_instance(Runtime& runtime, std::string_view key, const InstanceState& state) {
    try { return detail::ProgramAccess::create<MaterialInstance>(runtime, key, state); }
    catch (const std::exception& error) { return failure(error.what()); }
}
Result<std::string> material_key(Runtime& runtime, ObjectId id) {
    if (auto role = source_role(runtime, id); !role) return std::unexpected(role.error());
    return detail::ProgramAccess::key(runtime, id);
}
Result<SourceState> read_source(Runtime& runtime, ObjectId id) {
    if (auto valid = exact_type<MaterialSource>(runtime, id); !valid) return std::unexpected(valid.error());
    return detail::ProgramAccess::read_source(runtime, id);
}
Result<InstanceState> read_instance(Runtime& runtime, ObjectId id) {
    if (auto valid = exact_type<MaterialInstance>(runtime, id); !valid) return std::unexpected(valid.error());
    return detail::ProgramAccess::read_instance(runtime, id);
}
Result<std::vector<AuthorDiagnostic>> read_diagnostics(Runtime& runtime, ObjectId id) {
    if (auto valid = exact_type<MaterialInstance>(runtime, id); !valid) return std::unexpected(valid.error());
    return detail::ProgramAccess::diagnostics(runtime, id);
}
Result<void> update_source(Runtime& runtime, ObjectId id, const SourceState& state) {
    if (auto ready = runtime.check_code_publication(); !ready) return ready;
    if (auto valid = exact_type<MaterialSource>(runtime, id); !valid) return valid;
    try { return detail::ProgramAccess::update_source(runtime, id, state); }
    catch (const std::exception& error) { return failure(error.what()); }
}
Result<void> update_instance(Runtime& runtime, ObjectId id, const InstanceState& state) {
    if (auto ready = runtime.check_code_publication(); !ready) return ready;
    if (auto valid = exact_type<MaterialInstance>(runtime, id); !valid) return valid;
    try { return detail::ProgramAccess::update_instance(runtime, id, state); }
    catch (const std::exception& error) { return failure(error.what()); }
}
Result<ProgramSurfaceRef> capture_program(Runtime& runtime, ObjectId id) {
    auto role = source_role(runtime, id); if (!role) return std::unexpected(role.error());
    return detail::ProgramAccess::capture(runtime, id, *role);
}
} // namespace atelier::material
