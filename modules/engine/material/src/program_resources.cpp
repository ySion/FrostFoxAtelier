#include "material/program_resources.hpp"
#include "material/resources.hpp"
#include "runtime/native.hpp"

#include <cstring>

namespace atelier::material {
namespace {
static_assert(std::is_nothrow_move_assignable_v<ReferenceField>);

auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}

bool valid_key(std::string_view value) {
    return value.size() <= 4096 && value.find('\0') == value.npos;
}

bool finite(math::Vector4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

bool same(const SurfaceInput& a, const SurfaceInput& b) {
    return a.image == b.image && a.address == b.address && a.color == b.color && a.empty_value.x == b.empty_value.x &&
           a.empty_value.y == b.empty_value.y && a.empty_value.z == b.empty_value.z &&
           a.empty_value.w == b.empty_value.w;
}

ProgramSurfaceRef reuse(ProgramSurfaceRef old, ProgramSurfaceRef next) {
    if (old && old->definition() == next->definition() &&
        old->uses_default_material() == next->uses_default_material() &&
        std::ranges::equal(old->parameters(), next->parameters()) &&
        std::ranges::equal(old->diagnostics(), next->diagnostics()) &&
        std::ranges::equal(old->inputs(), next->inputs(), same))
        return old;
    return next;
}

// The CPU contract is bilinear LOD0, RGB-only sRGB decode before interpolation.
// Empty values are already linear. UV/channel transformations belong to Source.
Result<math::Vector4> sample_input(const void* data, math::Vector2 uv) {
    const auto& input = *static_cast<const SurfaceInput*>(data);
    if (!input.image)
        return input.empty_value;
    const auto& image = *input.image;
    const std::array extent{int(image.width()), int(image.height())};
    std::array<int, 2> lower{};
    std::array<double, 2> weight{};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        double point = uv[axis];
        if (input.address == Address::repeat)
            point -= std::floor(point);
        else if (input.address == Address::mirror) {
            point -= std::floor(point * .5) * 2;
            if (point > 1)
                point = 2 - point;
        } else
            point = std::clamp(point, 0., 1.);
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
        return channel == 3 || input.color == TextureColor::linear ? value
               : value <= .04045                                   ? value / 12.92
                                                                   : std::pow((value + .055) / 1.055, 2.4);
    };
    math::Vector4 result;
    for (std::size_t channel = 0; channel < 4; ++channel)
        result[channel] = static_cast<float>(std::lerp(
            std::lerp(texel(lower[0], lower[1], channel), texel(lower[0] + 1, lower[1], channel), weight[0]),
            std::lerp(texel(lower[0], lower[1] + 1, channel), texel(lower[0] + 1, lower[1] + 1, channel), weight[0]),
            weight[1]));
    return result;
}

Result<math::Vector2> input_orientation(const void* data, math::Vector2 uv) {
    const auto& input = *static_cast<const SurfaceInput*>(data);
    math::Vector2 result{1, 1};
    if (input.address == Address::mirror)
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const double coordinate = uv[axis];
            result[axis] = coordinate - std::floor(coordinate * .5) * 2 > 1 ? -1.f : 1.f;
        }
    return result;
}

Result<void> validate_binding(const TextureBinding& binding, const ImageRef& image) {
    if (!valid_key(binding.asset_key) || binding.address > Address::mirror)
        return failure("Invalid texture key or address mode.");
    if (binding.asset_key.empty() && image)
        return failure("An explicitly cleared texture cannot carry image content.");
    return {};
}

Result<bool> source_role(Runtime& runtime, ObjectId id) {
    ATELIER_TRY_BIND(type, runtime.type_of(id));
    ATELIER_TRY_BIND(source, runtime.native_type<MaterialSource>());
    ATELIER_TRY_BIND(instance, runtime.native_type<MaterialInstance>());
    if (*type == *source)
        return true;
    if (*type == *instance)
        return false;
    return failure("Expected an actual material.Source or material.Instance, not the common resource role.");
}

template <class T>
Result<void> exact_type(Runtime& runtime, ObjectId id) {
    ATELIER_TRY_BIND(type, runtime.type_of(id));
    ATELIER_TRY_BIND(wanted, runtime.native_type<T>());
    if (*type != *wanted)
        return failure("Material operation does not match the object's actual role.");
    return {};
}
}

namespace detail {
struct ProgramAccess {
    static ProgramSurfaceRef surface(DefinitionRef definition, std::vector<std::byte> parameters,
                                     std::vector<SurfaceInput> inputs, std::vector<AuthorDiagnostic> diagnostics,
                                     bool default_material = false) {
        return ProgramSurfaceRef(new ProgramSurface(std::move(definition), std::move(parameters), std::move(inputs),
                                                    std::move(diagnostics), default_material));
    }

    static Result<ProgramSurfaceRef> missing_parent() {
        static const auto prepared = []() -> Result<ProgramSurfaceRef> {
            ATELIER_TRY_BIND(fallback, default_program_surface());
            const auto parameters = (*fallback)->parameters();
            return surface((*fallback)->definition(), {parameters.begin(), parameters.end()}, {},
                           {{"$parent", "Material parent is unavailable; using the engine default material."}}, true);
        }();
        return prepared;
    }

    static Result<void> register_objects(Runtime& runtime) {
        auto resource = NativeRegistration<Material>{runtime, "material.Resource", TypeKind::object}
                            .hidden_field<&Material::textures_>(
                                "textures", {TypeReference::native<TextureResource>(), ValueForm::strong_sequence})
                            .build();
        auto source =
            NativeRegistration<MaterialSource>{runtime, "material.Source", TypeKind::object}.base<Material>().build();
        auto instance = NativeRegistration<MaterialInstance>{runtime, "material.Instance", TypeKind::object}
                            .base<Material>()
                            .hidden_field<&MaterialInstance::parent_>(
                                "parent", {TypeReference::native<Material>(), ValueForm::strong_reference})
                            .build();
        ATELIER_TRY(runtime.register_types({std::move(resource), std::move(source), std::move(instance)}));
        return {};
    }

    static Result<ImageRef> image(Runtime& runtime, ObjectId id, std::string_view key) {
        return runtime.with_native<const TextureResource>(id, [&](const auto& value) -> Result<ImageRef> {
            if (value.source_.view().std_view() != key)
                return failure("Texture root does not match the authored key.");
            return value.image_;
        });
    }

    struct Textures {
        std::vector<ImageRef> images;

        std::vector<ObjectReference> references;

        std::vector<ObjectId> ids;
    };

    template <class Binding, class PreviousBinding>
    static Result<Textures> textures(Runtime& runtime, std::span<const ObjectRoot> roots, Binding binding,
                                     const Material& previous, PreviousBinding previous_binding) {
        ATELIER_TRY_BIND(type, runtime.native_type<TextureResource>());
        ATELIER_TRY_BIND(empty, runtime.null_reference(*type));
        Textures result;
        result.images.resize(roots.size());
        result.references.assign(roots.size(), *empty);
        result.ids.resize(roots.size());
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (!roots[i].registered()) {
                if (!roots[i].reference().is_null())
                    return failure("Texture root belongs to a closed Runtime.");
                continue;
            }
            const auto* input = binding(i);
            if (!input || input->asset_key.empty())
                return failure("A numeric or explicitly cleared input cannot own a texture root.");
            ATELIER_TRY_BIND(held, runtime.hold(roots[i].reference()));
            auto content = image(runtime, held->id(), input->asset_key);
            if (!content) {
                if (content.error().code != ErrorCode::invalid_object)
                    return std::unexpected(content.error());
                const auto found = std::ranges::find(previous.texture_ids_, held->id());
                if (found == previous.texture_ids_.end())
                    return std::unexpected(content.error());
                const auto* old = previous_binding(static_cast<std::size_t>(found - previous.texture_ids_.begin()));
                if (!old || old->asset_key != input->asset_key)
                    return failure("Destroyed texture root does not match the authored key.");
                // A previously verified root/key can stay authored after destroy.
                // Its old pixels are never supplied to the new resolution.
            } else
                result.images[i] = std::move(*content);
            result.references[i] = held->reference();
            result.ids[i] = held->id();
        }
        return result;
    }

    static Result<bool> available(Runtime& runtime, const ObjectReference& reference, ObjectId expected) {
        if (reference.id() != expected)
            return failure("Material resource identity changed outside publication.");
        if (reference.is_null())
            return false;
        auto type = runtime.type_of(expected);
        if (type)
            return true;
        // Only a previously validated identity may be interpreted as destroyed.
        // Foreign/new invalid roots and all other Runtime errors remain errors.
        if (type.error().code == ErrorCode::invalid_object)
            return false;
        return std::unexpected(type.error());
    }

    // Called under a const borrow. Without dependency collection, steady captures
    // allocate only when a texture publishes new content or is lost.
    static Result<std::optional<std::vector<ImageRef>>> current_images(Runtime& runtime, const Material& value,
                                                                       std::unordered_set<ObjectId>* dependencies) {
        ATELIER_TRY_BIND(size, runtime.sequence_size(value.texture_field_));
        if (*size != value.images_.size() || *size != value.texture_ids_.size())
            return failure("Material resource edges do not match its captured inputs.");
        std::optional<std::vector<ImageRef>> images;
        for (std::size_t i = 0; i < *size; ++i) {
            ATELIER_TRY_BIND(reference, runtime.sequence_read(value.texture_field_, i));
            ATELIER_TRY_BIND(live, available(runtime, *reference, value.texture_ids_[i]));
            if (!*live) {
                if (value.images_[i]) {
                    if (!images)
                        images = value.images_;
                    (*images)[i].reset();
                }
                continue;
            }
            auto checked =
                runtime.with_native<const TextureResource>(reference->id(), [&](const auto& texture) -> Result<void> {
                    if (dependencies)
                        dependencies->insert(reference->id());
                    // Texture identity/key are immutable; update_texture is its sole
                    // content writer. Existing ProgramSurfaces retain the old pixels.
                    if (texture.image_ != value.images_[i]) {
                        if (!images)
                            images = value.images_;
                        (*images)[i] = texture.image_;
                    }
                    return {};
                });
            if (!checked)
                return std::unexpected(checked.error());
        }
        return images;
    }

    static Result<std::vector<ObjectRoot>> roots(Runtime& runtime, const Material& value) {
        ATELIER_TRY_BIND(size, runtime.sequence_size(value.texture_field_));
        std::vector<ObjectRoot> result(*size);
        for (std::size_t i = 0; i < *size; ++i) {
            ATELIER_TRY_BIND(reference, runtime.sequence_read(value.texture_field_, i));
            if (reference->is_null())
                continue;
            ATELIER_TRY_BIND(held, runtime.hold(*reference));
            result[i] = std::move(*held);
        }
        return result;
    }

    static Result<ObjectReference> parent(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<ObjectReference> {
            if (!value.published_)
                return failure("Material instance is not initialized.");
            ATELIER_TRY_BIND(reference, runtime.read_reference(value.parent_field_));
            ATELIER_TRY_BIND(live, available(runtime, *reference, value.parent_id_));
            if (!*live)
                return runtime.null_reference(reference->description().type);
            return *reference;
        });
    }

    static Result<void> check_parent(Runtime& runtime, ObjectId target, ObjectId current) {
        for (;;) {
            if (current == target)
                return failure("Material inheritance must be acyclic.");
            ATELIER_TRY_BIND(source, source_role(runtime, current));
            if (*source)
                return {};
            ATELIER_TRY_BIND(next, parent(runtime, current));
            if (next->is_null())
                return {}; // The initialized ancestor resolves to the engine default.
            current = next->id();
        }
    }

    template <class T, class State>
    static Result<ObjectRoot> create(Runtime& runtime, std::string_view key, const State& state) {
        ATELIER_TRY(runtime.check_code_publication());
        // The publishing asset directory owns key authority. Native creation
        // also serves installed engine Sources, not only project author files.
        if (!valid_key(key))
            return failure("Invalid material resource key.");
        ATELIER_TRY_BIND(type, runtime.native_type<T>());
        ATELIER_TRY_BIND(root, runtime.create(*type));
        auto initialized = runtime.with_native<T>(root->id(), [&](auto& value) -> Result<void> {
            value.asset_key_ = key;
            return {};
        });
        if (!initialized)
            return std::unexpected(initialized.error());
        Result<void> written;
        if constexpr (std::same_as<T, MaterialSource>)
            written = update_source(runtime, root->id(), state);
        else
            written = update_instance(runtime, root->id(), state);
        if (!written)
            return std::unexpected(written.error());
        return std::move(*root);
    }

    static Result<std::string> key(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const Material>(id, [](const auto& value) -> Result<std::string> {
            if (!value.published_)
                return failure("Material has no valid publication.");
            return value.asset_key_;
        });
    }

    static Result<SourceState> read_source(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialSource>(id, [&](const auto& value) -> Result<SourceState> {
            if (!value.published_)
                return failure("Material source is not initialized.");
            ATELIER_TRY_BIND(textures, roots(runtime, value));
            return SourceState{value.definition_, std::move(*textures)};
        });
    }

    static Result<InstanceState> read_instance(Runtime& runtime, ObjectId id) {
        return runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<InstanceState> {
            if (!value.published_)
                return failure("Material instance is not initialized.");
            ATELIER_TRY_BIND(reference, runtime.read_reference(value.parent_field_));
            ATELIER_TRY_BIND(held, runtime.hold(*reference));
            ATELIER_TRY_BIND(textures, roots(runtime, value));
            return InstanceState{std::move(*held), value.entries_, std::move(*textures)};
        });
    }

    static Result<void> update_source(Runtime& runtime, ObjectId id, const SourceState& state) {
        if (state.default_textures.size() != (state.definition ? state.definition->textures().size() : 0))
            return failure("Source defaults require one exact root slot per declared texture.");
        auto prepared = runtime.with_native<const MaterialSource>(id, [&](const auto& value) -> Result<Textures> {
            return textures(
                runtime, state.default_textures,
                [&](std::size_t i) { return &state.definition->textures()[i].binding; }, value,
                [&](std::size_t i) { return &value.definition_->textures()[i].binding; });
        });
        if (!prepared)
            return std::unexpected(prepared.error());
        ATELIER_TRY_BIND(surface, resolve_source(state.definition, prepared->images));
        auto definition = state.definition;
        return runtime.with_native<MaterialSource>(id, [&](auto& value) -> Result<void> {
            ATELIER_TRY_BIND(field, runtime.reference_field(id, value.textures_));
            ATELIER_TRY_BIND(size, runtime.sequence_size(*field));
            const std::array<ReferenceChange, 1> changes{SequenceSplice{*field, 0, *size, prepared->references}};
            *surface = reuse(value.published_, std::move(*surface));
            ATELIER_TRY(runtime.apply_reference_changes(changes));
            // No allocation, validation or callbacks after the reference commit.
            value.texture_field_ = std::move(*field);
            value.texture_ids_.swap(prepared->ids);
            value.images_.swap(prepared->images);
            value.definition_.swap(definition);
            value.published_.swap(*surface);
            return {};
        });
    }

    static Result<void> update_instance(Runtime& runtime, ObjectId id, const InstanceState& state) {
        if (state.textures.size() != state.entries.size())
            return failure("An instance needs one root slot per author entry.");
        auto reference = state.parent.reference();
        if (!state.parent.registered()) {
            if (!reference.is_null())
                return failure("Parent root belongs to a closed Runtime.");
            ATELIER_TRY_BIND(type, runtime.native_type<Material>());
            ATELIER_TRY_BIND(empty, runtime.null_reference(*type));
            reference = *empty;
        }
        ATELIER_TRY_BIND(held, runtime.hold(reference));
        auto parent_surface =
            runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<ProgramSurfaceRef> {
                if (reference.is_null()) {
                    if (!value.published_)
                        return failure("A new material instance requires a valid parent.");
                    return missing_parent();
                }
                auto type = runtime.type_of(reference.id());
                if (!type) {
                    if (value.published_ && reference.id() == value.parent_id_ &&
                        type.error().code == ErrorCode::invalid_object)
                        return missing_parent();
                    return std::unexpected(type.error());
                }
                ATELIER_TRY(check_parent(runtime, id, reference.id()));
                return capture_program(runtime, reference.id());
            });
        if (!parent_surface)
            return std::unexpected(parent_surface.error());
        auto prepared = runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<Textures> {
            return textures(
                runtime, state.textures,
                [&](std::size_t i) { return std::get_if<TextureBinding>(&state.entries[i].value); }, value,
                [&](std::size_t i) { return std::get_if<TextureBinding>(&value.entries_[i].value); });
        });
        if (!prepared)
            return std::unexpected(prepared.error());
        ATELIER_TRY_BIND(resolved, resolve_instance(*parent_surface, state.entries, prepared->images));
        auto entries = state.entries;
        return runtime.with_native<MaterialInstance>(id, [&](auto& value) -> Result<void> {
            ATELIER_TRY_BIND(field, runtime.reference_field(id, value.textures_));
            ATELIER_TRY_BIND(size, runtime.sequence_size(*field));
            ATELIER_TRY_BIND(parent_field, runtime.reference_field(id, value.parent_));
            const std::array<ReferenceChange, 2> changes{SequenceSplice{*field, 0, *size, prepared->references},
                                                         ReferenceWrite{*parent_field, held->reference()}};
            *resolved = reuse(value.published_, std::move(*resolved));
            ATELIER_TRY(runtime.apply_reference_changes(changes));
            value.texture_field_ = std::move(*field);
            value.parent_field_ = std::move(*parent_field);
            value.parent_id_ = held->id();
            value.texture_ids_.swap(prepared->ids);
            value.images_.swap(prepared->images);
            value.entries_.swap(entries);
            value.parent_surface_.swap(*parent_surface);
            value.published_.swap(*resolved);
            return {};
        });
    }

    static Result<ProgramSurfaceRef> capture(Runtime& runtime, ObjectId id,
                                             std::unordered_set<ObjectId>* dependencies = nullptr) {
        ATELIER_TRY_BIND(source, source_role(runtime, id));
        ProgramSurfaceRef next_parent;
        ProgramSurfaceRef refreshed;
        std::optional<std::vector<ImageRef>> next_images;
        if (*source) {
            auto captured =
                runtime.with_native<const MaterialSource>(id, [&](const auto& value) -> Result<ProgramSurfaceRef> {
                    if (!value.published_)
                        return failure("Material source is not initialized.");
                    if (dependencies)
                        dependencies->insert(id);
                    ATELIER_TRY_BIND(images, current_images(runtime, value, dependencies));
                    if (!*images)
                        return value.published_;
                    ATELIER_TRY_BIND(resolved, resolve_source(value.definition_, **images));
                    next_images = std::move(*images);
                    refreshed = reuse(value.published_, std::move(*resolved));
                    return refreshed;
                });
            if (!captured || !refreshed)
                return captured;
            return runtime.with_native<MaterialSource>(id, [&](auto& value) -> Result<ProgramSurfaceRef> {
                value.images_.swap(*next_images);
                value.published_.swap(refreshed);
                return value.published_;
            });
        }
        auto captured =
            runtime.with_native<const MaterialInstance>(id, [&](const auto& value) -> Result<ProgramSurfaceRef> {
                if (!value.published_)
                    return failure("Material instance is not initialized.");
                if (dependencies)
                    dependencies->insert(id);
                ATELIER_TRY_BIND(reference, runtime.read_reference(value.parent_field_));
                ATELIER_TRY_BIND(live, available(runtime, *reference, value.parent_id_));
                ATELIER_TRY_BIND(parent_surface,
                                 *live ? capture(runtime, reference->id(), dependencies) : missing_parent());
                ATELIER_TRY_BIND(images, current_images(runtime, value, dependencies));
                if (*parent_surface == value.parent_surface_ && !*images)
                    return value.published_;
                ATELIER_TRY_BIND(resolved,
                                 resolve_instance(*parent_surface, value.entries_, *images ? **images : value.images_));
                next_images = std::move(*images);
                next_parent = std::move(*parent_surface);
                refreshed = reuse(value.published_, std::move(*resolved));
                return refreshed;
            });
        if (!captured || !refreshed)
            return captured;
        // End this object's const borrow before publishing the prepared cache.
        // An outer mesh borrow remains permitted; no publication admission here.
        return runtime.with_native<MaterialInstance>(id, [&](auto& value) -> Result<ProgramSurfaceRef> {
            if (next_images)
                value.images_.swap(*next_images);
            value.parent_surface_.swap(next_parent);
            value.published_.swap(refreshed);
            return value.published_;
        });
    }
};
}

SourceDefinition::SourceDefinition(std::shared_ptr<const source::MaterialProgram> program, Policy policy,
                                   std::vector<TextureDefault> textures)
    : program_(std::move(program)), policy_(policy), textures_(std::move(textures)) {}

Result<DefinitionRef> make_definition(std::shared_ptr<const source::MaterialProgram> program, Policy policy,
                                      std::span<const TextureDefault> textures) {
    try {
        if (!program || policy.coverage > Coverage::mask)
            return failure("Invalid material program or coverage policy.");
        if (program->context_layout().size != sizeof(Context) ||
            program->context_layout().alignment != alignof(Context) ||
            program->output_layout().size != sizeof(Output) || program->output_layout().alignment != alignof(Output))
            return failure("Program does not use the material Context/Output layout.");
        if (textures.size() != program->textures().size())
            return failure("All declared texture defaults must be specified exactly once.");
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
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

ProgramSurface::ProgramSurface(DefinitionRef definition, std::vector<std::byte> parameters,
                               std::vector<SurfaceInput> inputs, std::vector<AuthorDiagnostic> diagnostics,
                               bool default_material)
    : definition_(std::move(definition)), parameters_(std::move(parameters)), inputs_(std::move(inputs)),
      diagnostics_(std::move(diagnostics)), default_material_(default_material) {
    samplers_.reserve(inputs_.size());
    for (std::size_t i = 0; i < inputs_.size(); ++i)
        samplers_.push_back({program().textures()[i].slot, &inputs_[i], sample_input, input_orientation});
}

Result<Output> ProgramSurface::evaluate(const Context& context) const {
    Output output;
    ATELIER_TRY(program().evaluate(std::as_bytes(std::span{&context, 1}), parameters_,
                                   std::as_writable_bytes(std::span{&output, 1}), samplers_));
    return output;
}

std::string_view default_world_source() noexcept {
    return R"(
struct Params { float cell_size; vector3 light; vector3 dark; };
Params defaults() {
    Params p; p.cell_size=1.0;
    p.light=vector3(0.32,0.32,0.32); p.dark=vector3(0.18,0.18,0.18);
    return p;
}
material.Output shade(material.Context context,Params p) {
    vector3 n=context.geometric_normal_world;
    vector2 plane=vector2(context.position_world.x,context.position_world.z);
    if (math.abs(n.z)>math.abs(n.x) && math.abs(n.z)>math.abs(n.y)) {
        plane=vector2(context.position_world.x,context.position_world.y);
    } else {
        if (math.abs(n.x)>math.abs(n.y)) { plane=vector2(context.position_world.z,context.position_world.y); }
    }
    float size=math.max(math.abs(p.cell_size),0.0001);
    vector2 cell=math.floor(plane/size);
    material.Output result; result.base_color=p.light;
    if (math.fract((cell.x+cell.y)*0.5)>=0.5) { result.base_color=p.dark; }
    result.roughness=0.8; result.normal_world=context.normal_world;
    return result;
}
)";
}

Result<ProgramSurfaceRef> default_world_surface() {
    try {
        static const auto prepared = []() -> Result<ProgramSurfaceRef> {
            Runtime compiler;
            ATELIER_TRY(register_program_types(compiler));
            ATELIER_TRY_BIND(context, compiler.native_type<Context>());
            ATELIER_TRY_BIND(output, compiler.native_type<Output>());
            ATELIER_TRY_BIND(texture, compiler.native_type<TextureInput>());
            ATELIER_TRY_BIND(program,
                             source::compile_material(compiler, *context, *output, *texture,
                                                      "builtin/material/default-grid.source", default_world_source()));
            ATELIER_TRY_BIND(definition, make_definition(std::move(*program), default_world_policy, {}));
            return resolve_source(std::move(*definition), {});
        }();
        return prepared;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<ProgramSurfaceRef> default_program_surface() {
    try {
        static const auto prepared = []() -> Result<ProgramSurfaceRef> {
            Runtime compiler;
            ATELIER_TRY(register_program_types(compiler));
            ATELIER_TRY_BIND(context, compiler.native_type<Context>());
            ATELIER_TRY_BIND(output, compiler.native_type<Output>());
            ATELIER_TRY_BIND(texture, compiler.native_type<TextureInput>());
            auto program =
                source::compile_material(compiler, *context, *output, *texture, "engine/default-material.source", R"(
struct Params { float marker; };
Params defaults() { Params p; p.marker=0.0; return p; }
material.Output shade(material.Context c, Params p) {
    material.Output o;
    o.base_color=vector3(1.0,0.0,1.0); o.emissive=vector3(0.0,0.0,0.0);
    o.roughness=0.65; o.metallic=0.0; o.coverage=1.0; o.normal_world=c.normal_world;
    return o;
})");
            if (!program)
                return std::unexpected(program.error());
            ATELIER_TRY_BIND(definition, make_definition(std::move(*program), {}, {}));
            const auto parameters = (*definition)->program().default_parameters();
            return detail::ProgramAccess::surface(
                std::move(*definition), {parameters.begin(), parameters.end()}, {},
                {{"", "Material definition is unavailable; using the engine default material."}}, true);
        }();
        return prepared;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<ProgramSurfaceRef> resolve_source(DefinitionRef definition, std::span<const ImageRef> images) {
    try {
        if (images.size() != (definition ? definition->textures().size() : 0))
            return failure("Source image table does not match its definition.");
        if (!definition)
            return default_program_surface();
        std::vector<SurfaceInput> inputs;
        std::vector<AuthorDiagnostic> diagnostics;
        inputs.reserve(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) {
            const auto& input = definition->textures()[i];
            if (auto valid = validate_binding(input.binding, images[i]); !valid)
                return failure(input.path + ": " + valid.error().message);
            if (!input.binding.asset_key.empty() && !images[i])
                diagnostics.push_back(
                    {input.path, "Texture '" + input.binding.asset_key +
                                     "' has no available image content; using the MS typed empty/default sample."});
            inputs.push_back({images[i], input.binding.address, input.color, input.empty_value});
        }
        const auto defaults = definition->program().default_parameters();
        return detail::ProgramAccess::surface(std::move(definition), {defaults.begin(), defaults.end()},
                                              std::move(inputs), std::move(diagnostics));
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<ProgramSurfaceRef> resolve_instance(ProgramSurfaceRef parent, std::span<const Override> entries,
                                           std::span<const ImageRef> images) {
    try {
        if (!parent || images.size() != entries.size())
            return failure("Instance resolution requires a parent candidate and one image slot per author entry.");
        if (entries.empty())
            return parent;
        std::vector<std::byte> parameters(parent->parameters().begin(), parent->parameters().end());
        std::vector<SurfaceInput> inputs(parent->inputs().begin(), parent->inputs().end());
        std::vector<AuthorDiagnostic> diagnostics(parent->diagnostics().begin(), parent->diagnostics().end());
        // The engine default is a complete replacement, not an author interface
        // that accidentally accepts an old entry with a matching field name.
        const auto numeric = parent->uses_default_material() ? std::span<const source::MaterialParameter>{}
                                                             : parent->program().parameters();
        const auto textures = parent->uses_default_material() ? std::span<const source::MaterialTextureInput>{}
                                                              : parent->program().textures();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.path.empty() || !valid_key(entry.path) ||
                std::ranges::find(entries.first(i), entry.path, &Override::path) != entries.first(i).end())
                return failure("Author override paths must be nonempty and unique.");
            const auto parameter = std::ranges::find(numeric, entry.path, &source::MaterialParameter::key);
            const auto texture = std::ranges::find(textures, entry.path, &source::MaterialTextureInput::key);
            std::string issue;
            const auto* binding = std::get_if<TextureBinding>(&entry.value);
            if (binding) {
                if (auto valid = validate_binding(*binding, images[i]); !valid)
                    return failure(entry.path + ": " + valid.error().message);
                if (texture == textures.end())
                    issue = "Texture declaration is absent or has a different type.";
                else if (!binding->asset_key.empty() && !images[i])
                    issue = "Texture '" + binding->asset_key + "' has no available image content.";
                if (issue.empty() && entry.enabled) {
                    auto& input = inputs[static_cast<std::size_t>(texture - textures.begin())];
                    input.image = images[i];
                    input.address = binding->address;
                }
            } else {
                if (images[i])
                    return failure(entry.path + ": numeric author entry cannot carry image content.");
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (!std::same_as<T, TextureBinding>) {
                            constexpr std::size_t count = sizeof(T) / sizeof(float);
                            bool valid = true;
                            if constexpr (std::same_as<T, float>)
                                valid = std::isfinite(value);
                            else
                                for (std::size_t c = 0; c < count; ++c)
                                    valid &= std::isfinite(value[c]);
                            if (!valid)
                                issue = "Numeric author values must be finite.";
                            else if (parameter == numeric.end() || parameter->components != count)
                                issue = "Numeric declaration is absent or has a different type.";
                            else if (entry.enabled)
                                std::memcpy(parameters.data() + parameter->offset, &value, sizeof(T));
                        }
                    },
                    entry.value);
            }
            if (!issue.empty()) {
                if (!entry.enabled)
                    issue += " Override is disabled; using the current inherited value.";
                else if (texture != textures.end()) {
                    const auto& input = inputs[static_cast<std::size_t>(texture - textures.begin())];
                    issue += input.image ? " Using the current inherited texture input."
                                         : " Using the MS typed empty/default sample.";
                } else if (parameter != numeric.end())
                    issue += " Using the current inherited/default parameter value.";
                else
                    issue += " Override is ignored by the current declaration.";
                diagnostics.push_back({entry.path, std::move(issue)});
            }
        }
        auto next = detail::ProgramAccess::surface(parent->definition(), std::move(parameters), std::move(inputs),
                                                   std::move(diagnostics), parent->uses_default_material());
        return reuse(std::move(parent), std::move(next));
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> register_resource_types(Runtime& runtime) {
    if (runtime.native_type<MaterialSource>() && runtime.native_type<MaterialInstance>())
        return {};
    // Prepare fallback code on the cold registration path, never on steady capture.
    ATELIER_TRY(default_program_surface());
    ATELIER_TRY(register_texture_types(runtime));
    ATELIER_TRY(register_program_types(runtime));
    return detail::ProgramAccess::register_objects(runtime);
}

Result<ObjectRoot> create_source(Runtime& runtime, std::string_view key, const SourceState& state) {
    try {
        return detail::ProgramAccess::create<MaterialSource>(runtime, key, state);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<ObjectRoot> create_instance(Runtime& runtime, std::string_view key, const InstanceState& state) {
    try {
        return detail::ProgramAccess::create<MaterialInstance>(runtime, key, state);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<std::string> material_key(Runtime& runtime, ObjectId id) {
    ATELIER_TRY(source_role(runtime, id));
    return detail::ProgramAccess::key(runtime, id);
}

Result<SourceState> read_source(Runtime& runtime, ObjectId id) {
    ATELIER_TRY(exact_type<MaterialSource>(runtime, id));
    return detail::ProgramAccess::read_source(runtime, id);
}

Result<InstanceState> read_instance(Runtime& runtime, ObjectId id) {
    ATELIER_TRY(exact_type<MaterialInstance>(runtime, id));
    return detail::ProgramAccess::read_instance(runtime, id);
}

Result<void> update_source(Runtime& runtime, ObjectId id, const SourceState& state) {
    ATELIER_TRY(runtime.check_code_publication());
    ATELIER_TRY(exact_type<MaterialSource>(runtime, id));
    try {
        return detail::ProgramAccess::update_source(runtime, id, state);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> update_instance(Runtime& runtime, ObjectId id, const InstanceState& state) {
    ATELIER_TRY(runtime.check_code_publication());
    ATELIER_TRY(exact_type<MaterialInstance>(runtime, id));
    try {
        return detail::ProgramAccess::update_instance(runtime, id, state);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<ProgramSurfaceRef> capture_program(Runtime& runtime, ObjectId id) {
    return detail::ProgramAccess::capture(runtime, id);
}

Result<ProgramCapture> capture_program_with_dependencies(Runtime& runtime, ObjectId id) {
    ProgramCapture result;
    ATELIER_TRY_BIND(surface, detail::ProgramAccess::capture(runtime, id, &result.dependencies));
    result.surface = std::move(*surface);
    return result;
}
} // namespace atelier::material
