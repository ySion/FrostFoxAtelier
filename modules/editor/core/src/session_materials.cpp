#include "editor/session.hpp"
#include "editor/project.hpp"
#include "assets/builtin_materials.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"
#include <cstring>
#include <unordered_set>

namespace atelier::editor {
namespace {
auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(text)});
}

auto stand_in(const MissingResource& item) {
    return std::unexpected(item.error);
}

// Explicit resource loads without a caller-owned issue list publish through the
// same diagnostic queue as scene loading, not a second logging/cache owner.
void report_resources(world::World& world, ObjectId object, std::span<const MissingResource> missing) noexcept {
    for (const auto& item : missing) {
        world::Diagnostic diagnostic;
        diagnostic.severity = world::Severity::warning;
        diagnostic.kind = item.kind;
        diagnostic.phase = world::DiagnosticPhase::load;
        diagnostic.object = object;
        diagnostic.value = item.key;
        diagnostic.message = item.error.message;
        diagnostic.source = item.error.source;
        diagnostic.error_code = item.error.code;
        world.report(std::move(diagnostic));
    }
}

bool same(const material::Override& a, const material::Override& b) {
    if (a.path != b.path || a.enabled != b.enabled || a.value.index() != b.value.index())
        return false;
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const auto& other = std::get<T>(b.value);
            if constexpr (std::same_as<T, material::TextureBinding>)
                return value.asset_key == other.asset_key && value.address == other.address;
            else
                return value == other;
        },
        a.value);
}

bool same(const std::optional<material::Override>& a, const std::optional<material::Override>& b) {
    return a.has_value() == b.has_value() && (!a || same(*a, *b));
}

std::optional<material::Override> entry_at(std::span<const material::Override> entries, std::string_view path) {
    const auto at = std::ranges::find(entries, path, &material::Override::path);
    return at == entries.end() ? std::nullopt : std::optional(*at);
}

material::LocalValue parameter_value(const source::MaterialParameter& parameter, std::span<const std::byte> bytes) {
    const auto read = [&]<class T>() {
        T value;
        std::memcpy(&value, bytes.data() + parameter.offset, sizeof(T));
        return material::LocalValue(value);
    };
    switch (parameter.components) {
    case 1:
        return read.template operator()<float>();
    case 2:
        return read.template operator()<math::Vector2>();
    case 3:
        return read.template operator()<math::Vector3>();
    case 4:
        return read.template operator()<math::Vector4>();
    default:
        fatal("Invalid compiled material parameter shape.");
    }
}
}

const MissingResource& Session::remember_unavailable(const assets::Reader& reader, assets::AssetKind kind,
                                                     std::string_view key, const Error& error) {
    const bool listed = reader.entry(key) != nullptr;
    return unavailable_
        .insert_or_assign({kind, std::string(key)}, MissingResource{std::string(key), error, std::nullopt,
                                                                    listed ? world::DiagnosticKind::unreadable_resource
                                                                           : world::DiagnosticKind::missing_resource})
        .first->second;
}

const MissingResource* Session::unavailable(assets::AssetKind kind, std::string_view key) const {
    const auto at = unavailable_.find(std::pair{kind, key});
    return at == unavailable_.end() ? nullptr : &at->second;
}

Result<void> Session::publish_texture(std::string_view key, material::ImageRef image) {
    if (world_.closed() || key.empty() || !image)
        return failure("Texture publication needs a live Session, asset key and complete image.");
    if (const auto at = textures_.find(std::string(key)); at != textures_.end()) {
        ATELIER_TRY_BIND(replaced, material::update_texture(runtime_, at->second.id(), image));
    } else {
        ATELIER_TRY_BIND(root, material::create_texture(runtime_, key, image));
        textures_.emplace(key, std::move(*root));
    }
    unavailable_.erase(std::pair{assets::AssetKind::texture, std::string(key)});
    ++revision_;
    forward_texture(key, std::move(image));
    return {};
}

Result<void> Session::reimport_texture(Project& project, std::string_view key, const std::filesystem::path& source) {
    if (world_.closed() || world_.running() || editing())
        return failure("Finish the active edit and stop the editor World before reimporting.");
    // Validate an existing Runtime target before touching the committed file.
    if (const auto at = textures_.find(std::string(key)); at != textures_.end()) {
        ATELIER_TRY_BIND(current, material::read_texture(runtime_, at->second.id()));
    }
    ATELIER_TRY_BIND(imported, project.reimport_texture(key, source));
    return publish_texture(imported->key, std::move(imported->image));
}

Result<ObjectRoot> Session::texture_resource(assets::Reader& reader, std::string_view key,
                                             std::vector<MissingResource>* missing, std::size_t input) {
    if (const auto at = textures_.find(std::string(key)); at != textures_.end()) {
        if (const auto* item = unavailable(assets::AssetKind::texture, key)) {
            if (!missing)
                return stand_in(*item);
            missing->push_back(*item);
            missing->back().input = input;
        }
        return at->second;
    }
    auto asset = reader.texture(key);
    if (!asset && !missing)
        return std::unexpected(asset.error());
    ATELIER_TRY_BIND(root, material::create_texture(runtime_, key, asset ? asset->image : material::ImageRef{}));
    textures_.emplace(std::string(key), *root);
    if (!asset) {
        missing->push_back(remember_unavailable(reader, assets::AssetKind::texture, key, asset.error()));
        missing->back().input = input;
    }
    return std::move(*root);
}

Result<ObjectRoot> Session::material_resource(assets::Reader& reader, std::string_view key,
                                              std::vector<MissingResource>* missing) {
    if (std::ranges::find(loading_materials_, key) != loading_materials_.end())
        return failure("Material parent cycle at " + std::string(key));
    if (const auto at = materials_.find(std::string(key)); at != materials_.end()) {
        if (const auto* item = unavailable(assets::AssetKind::material, key)) {
            if (!missing)
                return stand_in(*item);
            missing->push_back(*item);
            return at->second.root;
        }
        ATELIER_TRY_BIND(surface, material::capture_program(runtime_, at->second.root.id()));
        if ((*surface)->uses_default_material()) {
            if (at->second.error) {
                if (!missing)
                    return std::unexpected(*at->second.error);
                missing->push_back(
                    {std::string(key), *at->second.error, std::nullopt, world::DiagnosticKind::unreadable_resource});
            } else if (const auto* author = std::get_if<assets::MaterialInstanceAsset>(&at->second.author.author)) {
                ATELIER_TRY_BIND(parent, material_resource(reader, author->parent_key, missing));
            }
        }
        return at->second.root;
    }
    auto asset = reader.material(key);
    if (asset)
        return store_material(reader, *asset, {}, missing);
    // Malformed/unsupported author records are structural failures, not missing assets.
    const auto* listed = reader.entry(key);
    if (!missing || key.empty() || key.starts_with("builtin/") ||
        (listed && (listed->kind != assets::AssetKind::material || !asset.error().message.starts_with("Cannot read "))))
        return std::unexpected(asset.error());
    ATELIER_TRY_BIND(root, material::create_source(runtime_, key, {}));
    MaterialDocument document;
    document.root = *root;
    document.author.key = key;
    document.author.author = assets::MaterialSourceAsset{};
    document.diagnostics.push_back({{}, asset.error().message});
    materials_.emplace(key, std::move(document));
    missing->push_back(remember_unavailable(reader, assets::AssetKind::material, key, asset.error()));
    return std::move(*root);
}

Result<ObjectRoot> Session::open_material(Project& project, std::string_view key) {
    if (const auto* item = unavailable(assets::AssetKind::material, key))
        return stand_in(*item);
    ATELIER_TRY_BIND(asset, project.assets().material(key));
    std::vector<MissingResource> missing;
    auto result = material_resource(project.assets(), *asset, &missing);
    if (result)
        report_resources(world_, result->id(), missing);
    return result;
}

Result<ObjectRoot> Session::material_resource(assets::Reader& reader, const assets::MaterialAsset& asset,
                                              std::vector<MissingResource>* missing) {
    if (!asset.key.empty() && materials_.contains(asset.key))
        return material_resource(reader, asset.key, missing);
    return publish_material(reader, asset, {}, missing);
}

Result<ObjectRoot> Session::publish_material(assets::Reader& reader, const assets::MaterialAsset& asset,
                                             std::optional<source::SourceFile> exact_source,
                                             std::vector<MissingResource>* missing) {
    ATELIER_TRY_BIND(root, store_material(reader, asset, std::move(exact_source), missing));
    ++revision_;
    forward_material(root->id());
    return std::move(*root);
}

Result<ObjectRoot> Session::store_material(assets::Reader& reader, const assets::MaterialAsset& asset,
                                           std::optional<source::SourceFile> exact_source,
                                           std::vector<MissingResource>* missing) {
    if (world_.closed())
        return failure("Material publication requires a live Session.");
    if (!asset.key.empty()) {
        if (std::ranges::find(loading_materials_, asset.key) != loading_materials_.end())
            return failure("Material parent cycle at " + asset.key);
    }
    loading_materials_.push_back(asset.key);
    struct Loading {
        std::vector<std::string>& keys;

        ~Loading() { keys.pop_back(); }
    } loading{loading_materials_};
    const auto encoded = assets::material_format::encode(asset);
    if (!encoded)
        return failure(encoded.error().describe());
    if (asset.key.starts_with("builtin/")) {
        const auto installed = assets::material_assets();
        const auto at = std::ranges::find(installed, asset.key, &assets::MaterialAsset::key);
        if (at == installed.end())
            return failure("An installed material key cannot be invented or its author data replaced.");
        const auto expected = assets::material_format::encode(*at);
        if (!expected)
            return failure(expected.error().describe());
        if (*expected != *encoded)
            return failure("An installed material key cannot be invented or its author data replaced.");
        if (exact_source) {
            const auto* author = std::get_if<assets::MaterialSourceAsset>(&at->author);
            const auto source = author ? assets::builtin_material_source(author->source_path) : std::nullopt;
            if (!source || source->path != exact_source->path || source->source != exact_source->source)
                return failure("An installed Material Source snapshot cannot replace the installed text.");
        }
    }
    MaterialDocument document;
    document.author = asset;
    if (const auto at = materials_.find(asset.key); at != materials_.end()) {
        if (at->second.author.author.index() != asset.author.index())
            return failure("Material publication cannot change its MS/MI role.");
        document.root = at->second.root;
    }
    if (const auto* author = std::get_if<assets::MaterialSourceAsset>(&asset.author)) {
        if (asset.key.empty())
            return failure("A scene-local material must be an instance, not a Source.");
        if (exact_source && exact_source->path != author->source_path)
            return failure("Material Source snapshot path does not match its author record.");
        material::SourceState state;
        auto source_file = exact_source ? Result<source::SourceFile>(std::move(*exact_source))
                                        : reader.source(platform::utf8_to_utf16_validated(author->source_path));
        if (!source_file)
            document.error = std::move(source_file.error());
        else {
            document.source_file = std::move(*source_file);
            Runtime compiler;
            ATELIER_TRY(material::register_program_types(compiler));
            ATELIER_TRY_BIND(context_type, compiler.native_type<material::Context>());
            ATELIER_TRY_BIND(output_type, compiler.native_type<material::Output>());
            ATELIER_TRY_BIND(texture_type, compiler.native_type<material::TextureInput>());
            auto program = source::compile_material(compiler, *context_type, *output_type, *texture_type,
                                                    document.source_file->path, document.source_file->source);
            if (!program)
                document.error = std::move(program.error());
            else {
                auto definition = material::make_definition(std::move(*program), author->policy, author->inputs);
                if (!definition)
                    document.error = std::move(definition.error());
                else {
                    state.definition = std::move(*definition);
                    for (const auto& input : state.definition->textures()) {
                        ObjectRoot root;
                        if (!input.binding.asset_key.empty()) {
                            ATELIER_TRY_BIND(texture, texture_resource(reader, input.binding.asset_key, missing,
                                                                       state.default_textures.size()));
                            root = std::move(*texture);
                        }
                        state.default_textures.push_back(std::move(root));
                    }
                }
            }
        }
        if (document.error) {
            auto& error = *document.error;
            // Only associate a saved file when the cause identifies this
            // exact input; a business callback may refer to another source.
            if (error.source && document.source_file && error.source->disk_path.empty() &&
                error.source->path == document.source_file->path &&
                error.source->version == document.source_file->version)
                error.source->disk_path = document.source_file->disk_path;
            document.diagnostics.push_back({author->source_path, error.message});
            if (!missing)
                return std::unexpected(error);
            missing->push_back({asset.key, error, std::nullopt, world::DiagnosticKind::unreadable_resource});
        }
        // A cold fallback (or an absent declaration) may have left enabled
        // child bindings unresolved. Prepare those roots against the new
        // declaration without replacing child authors, history or images.
        std::vector<std::pair<ObjectRoot, material::InstanceState>> children;
        if (document.root.registered() && state.definition) {
            ATELIER_TRY_BIND(instance_type, runtime_.native_type<material::MaterialInstance>());
            const auto prepare_child = [&](ObjectId object) -> Result<void> {
                const auto type = runtime_.type_of(object);
                if (!type)
                    return {};
                ATELIER_TRY_BIND(is_instance, runtime_.is_a(*type, *instance_type));
                if (!*is_instance)
                    return {};
                ATELIER_TRY_BIND(child, material::read_instance(runtime_, object));
                auto parent = child->parent;
                while (parent.registered() && parent.id() != document.root.id()) {
                    const auto parent_type = runtime_.type_of(parent.id());
                    if (!parent_type)
                        return {};
                    ATELIER_TRY_BIND(parent_is_instance, runtime_.is_a(*parent_type, *instance_type));
                    if (!*parent_is_instance)
                        return {};
                    ATELIER_TRY_BIND(parent_state, material::read_instance(runtime_, parent.id()));
                    parent = std::move(parent_state->parent);
                }
                if (!parent.registered())
                    return {};
                bool changed = false;
                for (std::size_t i = 0; i < child->entries.size(); ++i) {
                    const auto& entry = child->entries[i];
                    const auto* texture = std::get_if<material::TextureBinding>(&entry.value);
                    if (!entry.enabled || !texture || texture->asset_key.empty() || child->textures[i].registered())
                        continue;
                    if (std::ranges::find(state.definition->textures(), entry.path, &material::TextureDefault::path) ==
                        state.definition->textures().end())
                        continue;
                    ATELIER_TRY_BIND(root, texture_resource(reader, texture->asset_key, missing, i));
                    child->textures[i] = std::move(*root);
                    changed = true;
                }
                if (changed) {
                    ATELIER_TRY_BIND(root, runtime_.hold(object));
                    children.emplace_back(std::move(*root), std::move(*child));
                }
                return {};
            };
            for (const auto& [key, child] : materials_) {
                static_cast<void>(key);
                ATELIER_TRY(prepare_child(child.root.id()));
            }
            for (const auto& [object, parent] : scene_material_parents_) {
                static_cast<void>(parent);
                ATELIER_TRY(prepare_child(object));
            }
        }
        if (document.root.registered())
            ATELIER_TRY(material::update_source(runtime_, document.root.id(), state));
        else {
            ATELIER_TRY_BIND(root, material::create_source(runtime_, asset.key, state));
            document.root = std::move(*root);
        }
        for (const auto& [child, prepared] : children)
            ATELIER_TRY(material::update_instance(runtime_, child.id(), prepared));
    } else {
        if (exact_source)
            return failure("A Material Instance cannot carry Source text.");
        const auto& instance_author = std::get<assets::MaterialInstanceAsset>(asset.author);
        if (instance_author.parent_key.empty())
            return failure("A material asset instance requires its parent's asset key.");
        material::InstanceState state;
        ATELIER_TRY_BIND(parent_root, material_resource(reader, instance_author.parent_key, missing));
        state.parent = std::move(*parent_root);
        state.entries = instance_author.entries;
        ATELIER_TRY_BIND(parent, material::capture_program(runtime_, state.parent.id()));
        for (const auto& entry : state.entries) {
            const auto* texture = std::get_if<material::TextureBinding>(&entry.value);
            const bool declared =
                !(*parent)->uses_default_material() &&
                std::ranges::find((*parent)->definition()->textures(), entry.path, &material::TextureDefault::path) !=
                    (*parent)->definition()->textures().end();
            ObjectRoot root;
            if (entry.enabled && declared && texture && !texture->asset_key.empty()) {
                ATELIER_TRY_BIND(loaded, texture_resource(reader, texture->asset_key, missing, state.textures.size()));
                root = std::move(*loaded);
            }
            state.textures.push_back(std::move(root));
        }
        if (document.root.registered())
            ATELIER_TRY(material::update_instance(runtime_, document.root.id(), state));
        else {
            ATELIER_TRY_BIND(root, material::create_instance(runtime_, asset.key, state));
            document.root = std::move(*root);
        }
        document.saved = instance_author.entries;
    }
    auto root = document.root;
    if (asset.key.empty())
        scene_material_parents_.emplace(root.id(), std::get<assets::MaterialInstanceAsset>(asset.author).parent_key);
    else {
        // Recursive parent loads can rehash the map; only look it up again now.
        if (const auto at = materials_.find(asset.key); at != materials_.end()) {
            document.history = std::move(at->second.history);
            document.cursor = at->second.cursor;
        }
        materials_.insert_or_assign(asset.key, std::move(document));
        unavailable_.erase(std::pair{assets::AssetKind::material, asset.key});
    }
    return root;
}

Result<std::vector<presentation::MaterialBinding>>
Session::mesh_defaults(assets::Reader& reader, const assets::MeshAsset& asset, std::vector<MissingResource>* missing) {
    std::vector<MissingResource> unreported;
    std::vector<presentation::MaterialBinding> result;
    result.reserve(asset.materials.size());
    for (const auto& item : asset.materials) {
        ATELIER_TRY_BIND(root, material_resource(reader, item.key, missing ? missing : &unreported));
        report_resources(world_, root->id(), unreported);
        unreported.clear();
        result.push_back({item.slot, std::move(*root), false});
    }
    return result;
}

Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>>
Session::mesh_surfaces(assets::Reader& reader, const assets::MeshAsset& asset, std::vector<MissingResource>* missing) {
    ATELIER_TRY_BIND(defaults, mesh_defaults(reader, asset, missing));
    auto result = std::make_shared<std::vector<material::ProgramSurfaceRef>>();
    if (!asset.mesh)
        return result;
    for (std::size_t i = 0; i < asset.mesh->materials().size(); ++i) {
        const auto at = std::ranges::find(*defaults, presentation::material_slot_name(*asset.mesh, i),
                                          &presentation::MaterialBinding::slot);
        ATELIER_TRY_BIND(surface, at == defaults->end() ? material::default_world_surface()
                                                        : material::capture_program(runtime_, at->resource.id()));
        result->push_back(std::move(*surface));
    }
    return result;
}

Result<assets::MaterialAsset> Session::material_asset(ObjectId object) const {
    ATELIER_TRY_BIND(key, material::material_key(world_.runtime(), object));
    assets::MaterialAsset result;
    result.key = *key;
    if (!key->empty()) {
        const auto at = materials_.find(*key);
        if (at == materials_.end() || at->second.root.id() != object)
            return failure("Material has no author document.");
        result = at->second.author;
        if (std::holds_alternative<assets::MaterialSourceAsset>(result.author))
            return result;
    } else if (const auto at = scene_material_parents_.find(object); at != scene_material_parents_.end())
        result.author = assets::MaterialInstanceAsset{at->second, {}};
    else
        return failure("Scene material has no author identity.");
    ATELIER_TRY_BIND(state, material::read_instance(world_.runtime(), object));
    std::get<assets::MaterialInstanceAsset>(result.author).entries = std::move(state->entries);
    return result;
}

Result<std::vector<assets::MaterialAsset>> Session::material_assets() const {
    std::vector<assets::MaterialAsset> result;
    result.reserve(materials_.size());
    for (const auto& [key, document] : materials_) {
        if (unavailable(assets::AssetKind::material, key))
            continue;
        ATELIER_TRY_BIND(asset, material_asset(document.root.id()));
        result.push_back(std::move(*asset));
    }
    return result;
}

std::vector<source::SourceFile> Session::material_sources() const {
    std::vector<source::SourceFile> result;
    for (const auto& [key, document] : materials_) {
        static_cast<void>(key);
        if (document.source_file)
            result.push_back(*document.source_file);
    }
    return result;
}

Result<MaterialView> Session::inspect_material(ObjectId object) const {
    auto& runtime = world_.runtime();
    ATELIER_TRY_BIND(asset, material_asset(object));
    ATELIER_TRY_BIND(surface, material::capture_program(runtime, object));
    MaterialView view;
    view.asset_key = asset->key;
    view.scene = asset->key.empty();
    view.source = std::holds_alternative<assets::MaterialSourceAsset>(asset->author);
    view.default_material = (*surface)->uses_default_material();
    view.diagnostics.assign((*surface)->diagnostics().begin(), (*surface)->diagnostics().end());
    if (const auto* instance = std::get_if<assets::MaterialInstanceAsset>(&asset->author)) {
        view.parent_key = instance->parent_key;
        view.entries = instance->entries;
    }
    std::vector<material::InstanceState> chain;
    std::unordered_set<ObjectId> visited;
    auto current = object;
    for (;;) {
        if (!visited.insert(current).second)
            return failure("Material parent cycle during inspection.");
        const auto type = runtime.type_of(current);
        if (!type) {
            if (current != object && type.error().code == ErrorCode::invalid_object)
                break; // A destroyed, previously validated parent uses the default material.
            return std::unexpected(type.error());
        }
        ATELIER_TRY_BIND(key, material::material_key(runtime, current));
        if (const auto at = materials_.find(*key); at != materials_.end()) {
            view.diagnostics.insert(view.diagnostics.end(), at->second.diagnostics.begin(),
                                    at->second.diagnostics.end());
            if (at->second.source_file)
                view.source_file = at->second.source_file;
        }
        ATELIER_TRY_BIND(source_type, runtime.native_type<material::MaterialSource>());
        ATELIER_TRY_BIND(is_source, runtime.is_a(*type, *source_type));
        if (*is_source)
            break;
        ATELIER_TRY_BIND(state, material::read_instance(runtime, current));
        const auto parent = state->parent.id();
        chain.push_back(std::move(*state));
        if (parent == ObjectId{})
            break;
        current = parent;
    }
    if (view.default_material)
        return view;
    for (const auto& parameter : (*surface)->program().parameters())
        view.declarations.push_back({parameter.key, parameter.components,
                                     parameter_value(parameter, (*surface)->parameters()),
                                     parameter_value(parameter, (*surface)->program().default_parameters())});
    for (const auto& input : (*surface)->definition()->textures()) {
        auto binding = input.binding;
        for (auto at = chain.rbegin(); at != chain.rend(); ++at)
            for (std::size_t i = 0; i < at->entries.size(); ++i) {
                const auto& entry = at->entries[i];
                if (entry.path != input.path || !entry.enabled)
                    continue;
                const auto* value = std::get_if<material::TextureBinding>(&entry.value);
                if (!value)
                    continue;
                if (value->asset_key.empty())
                    binding = *value;
                else if (at->textures[i].registered()) {
                    auto texture = material::read_texture(runtime, at->textures[i].id());
                    if (!texture && texture.error().code != ErrorCode::invalid_object)
                        return std::unexpected(texture.error());
                    if (texture && texture->image)
                        binding = *value;
                }
            }
        view.declarations.push_back({input.path, 0, binding, input.binding});
    }
    return view;
}

std::vector<ObjectId> Session::material_objects() const {
    std::vector<ObjectId> result;
    result.reserve(materials_.size());
    for (const auto& [key, document] : materials_) {
        static_cast<void>(key);
        result.push_back(document.root.id());
    }
    return result;
}

Result<Session::MaterialDocument*> Session::admit_material_edit(ObjectId object) {
    if (world_.closed() || world_.running())
        return failure("Stop the World before editing a material.");
    ATELIER_TRY_BIND(asset, material_asset(object));
    if (asset->key.starts_with("builtin/"))
        return failure("Installed material authors are read-only; create a Material Instance.");
    if (std::holds_alternative<assets::MaterialSourceAsset>(asset->author))
        return failure("Edit the Material Source file or create a Material Instance.");
    if (const auto* item = unavailable(assets::AssetKind::material, asset->key))
        return stand_in(*item);
    // material_asset validated both the catalog key and its object identity.
    return asset->key.empty() ? nullptr : &materials_.find(asset->key)->second;
}

Result<void> Session::begin_material_edit(ObjectId object, std::uint64_t control) {
    if (!control)
        return failure("A material edit requires a control session.");
    if (material_edit_ && material_edit_->root.id() == object && material_edit_->control == control)
        return {};
    ATELIER_TRY_BIND(edit, prepare_material_edit(object, control));
    material_edit_ = std::move(*edit);
    return {};
}

Result<Session::MaterialEdit> Session::prepare_material_edit(ObjectId object, std::uint64_t control) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(document, admit_material_edit(object));
    ATELIER_TRY_BIND(held, runtime_.hold(object));
    return MaterialEdit{std::move(*held), control, *document, {}};
}

Result<void> Session::apply_material_entries(ObjectId object, std::span<const MaterialEntryChange> changes,
                                             bool forward, Project* project) {
    ATELIER_TRY_BIND(state, material::read_instance(runtime_, object));
    for (const auto& change : changes) {
        const auto& value = forward ? change.after : change.before;
        const auto at = std::ranges::find(state->entries, change.path, &material::Override::path);
        const auto index = static_cast<std::size_t>(at - state->entries.begin());
        if (!value) {
            if (at != state->entries.end()) {
                state->entries.erase(at);
                state->textures.erase(state->textures.begin() + static_cast<std::ptrdiff_t>(index));
            }
            continue;
        }
        ObjectRoot texture;
        if (const auto* binding = std::get_if<material::TextureBinding>(&value->value)) {
            if (const auto cached = textures_.find(binding->asset_key); cached != textures_.end())
                texture = cached->second;
            else if (at != state->entries.end())
                if (const auto* previous = std::get_if<material::TextureBinding>(&at->value);
                    previous && previous->asset_key == binding->asset_key)
                    texture = state->textures[index];
            if (project && value->enabled && !binding->asset_key.empty() && !texture.registered()) {
                ATELIER_TRY_BIND(surface, material::capture_program(runtime_, object));
                if (!(*surface)->uses_default_material() &&
                    std::ranges::find((*surface)->definition()->textures(), change.path,
                                      &material::TextureDefault::path) != (*surface)->definition()->textures().end()) {
                    std::vector<MissingResource> missing;
                    ATELIER_TRY_BIND(resolved, texture_resource(project->assets(), binding->asset_key, &missing));
                    report_resources(world_, object, missing);
                    texture = std::move(*resolved);
                }
            }
        }
        if (at == state->entries.end()) {
            state->entries.push_back(*value);
            state->textures.push_back(std::move(texture));
        } else {
            *at = *value;
            state->textures[index] = std::move(texture);
        }
    }
    return material::update_instance(runtime_, object, *state);
}

Result<void> Session::preview_material_entry(std::uint64_t control, material::Override entry, Project* project) {
    if (!material_edit_ || material_edit_->control != control)
        return failure("No matching material edit.");
    return preview_material_entry(*material_edit_, {entry.path, {}, std::move(entry)}, project);
}

Result<void> Session::preview_material_entry(MaterialEdit& edit, MaterialEntryChange change, Project* project) {
    const auto object = edit.root.id();
    const auto& entry = change.after;
    if (entry) {
        if (project && entry->enabled)
            if (const auto* texture = std::get_if<material::TextureBinding>(&entry->value);
                texture && !texture->asset_key.empty()) {
                ATELIER_TRY_BIND(view, inspect_material(object));
                const auto field = std::ranges::find(view->declarations, change.path, &MaterialParameterView::path);
                if (field != view->declarations.end() && !field->components) {
                    ATELIER_TRY(texture_resource(project->assets(), texture->asset_key));
                }
            }
        const auto encoded = assets::material_format::encode_entries(std::span(&*entry, 1));
        if (!encoded)
            return failure(encoded.error().describe());
    }
    const auto at = std::ranges::find(edit.entries, change.path, &MaterialEntryChange::path);
    if (at == edit.entries.end()) {
        ATELIER_TRY_BIND(state, material::read_instance(runtime_, object));
        change.before = entry_at(state->entries, change.path);
    }
    ATELIER_TRY(apply_material_entries(object, std::span(&change, 1), true));
    if (at == edit.entries.end())
        edit.entries.push_back(std::move(change));
    else
        at->after = std::move(change.after);
    ++revision_;
    forward_material(object);
    return {};
}

Result<void> Session::commit_material_edit(std::uint64_t control) {
    if (!material_edit_ || material_edit_->control != control)
        return failure("No matching material edit.");
    record_material_edit(*material_edit_);
    material_edit_.reset();
    return {};
}

void Session::record_material_edit(MaterialEdit& edit) {
    std::erase_if(edit.entries, [](const auto& entry) { return same(entry.before, entry.after); });
    if (edit.entries.empty())
        return;
    MaterialValueChange change{std::move(edit.root), std::move(edit.entries)};
    if (!edit.document)
        remember(std::move(change));
    else {
        auto& document = *edit.document;
        document.history.erase(document.history.begin() + static_cast<std::ptrdiff_t>(document.cursor),
                               document.history.end());
        document.history.push_back(std::move(change));
        ++document.cursor;
        if (document.history.size() > history_limit) {
            document.history.erase(document.history.begin());
            --document.cursor;
        }
        ++revision_;
    }
}

Result<void> Session::change_material_entry(MaterialEdit& edit, MaterialEntryChange change, Project* project) {
    // Publication is the last fallible step. A one-shot edit owns no earlier preview to restore.
    ATELIER_TRY(preview_material_entry(edit, std::move(change), project));
    record_material_edit(edit);
    return {};
}

Result<void> Session::set_material_entry(ObjectId object, material::Override entry, Project* project) {
    ATELIER_TRY_BIND(edit, prepare_material_edit(object));
    return change_material_entry(*edit, {entry.path, {}, std::move(entry)}, project);
}

Result<void> Session::cancel_material_edit() {
    if (!material_edit_)
        return {};
    if (!material_edit_->entries.empty()) {
        ATELIER_TRY(apply_material_entries(material_edit_->root.id(), material_edit_->entries, false));
        forward_material(material_edit_->root.id());
    }
    material_edit_.reset();
    ++revision_;
    return {};
}

bool Session::material_editing(std::uint64_t control) const noexcept {
    return material_edit_ && material_edit_->control == control;
}

bool Session::is_material(ObjectId object) const {
    const auto type = runtime_.type_of(object);
    return type && is_material_type(*type).value_or(false);
}

Result<bool> Session::is_material_type(TypeId type) const {
    ATELIER_TRY_BIND(base, runtime_.native_type<material::Material>());
    return runtime_.is_a(type, *base);
}

bool Session::material_can_undo(std::string_view key, bool redo) const {
    const auto at = materials_.find(std::string(key));
    return at != materials_.end() && (redo ? at->second.cursor < at->second.history.size() : at->second.cursor > 0);
}

ObjectId Session::loaded_material(std::string_view key) const {
    const auto at = materials_.find(std::string(key));
    return at == materials_.end() ? ObjectId{} : at->second.root.id();
}

bool Session::material_dirty(std::string_view key) const {
    const auto at = materials_.find(std::string(key));
    if (at == materials_.end())
        return false;
    if (material_edit_ && material_edit_->root.id() == at->second.root.id())
        return true;
    if (std::holds_alternative<assets::MaterialSourceAsset>(at->second.author.author))
        return false;
    const auto state = material::read_instance(world_.runtime(), at->second.root.id());
    return !state || at->second.saved.size() != state->entries.size() ||
           !std::ranges::all_of(at->second.saved, [&](const auto& entry) {
               return same(std::optional(entry), entry_at(state->entries, entry.path));
           });
}

std::vector<std::string> Session::dirty_materials() const {
    std::vector<std::string> result;
    for (const auto& [key, value] : materials_) {
        static_cast<void>(value);
        if (material_dirty(key))
            result.push_back(key);
    }
    return result;
}

Result<void> Session::material_undo(std::string_view key, bool redo, Project* project) {
    ATELIER_TRY(cancel_edit());
    const auto at = materials_.find(std::string(key));
    if (at == materials_.end())
        return failure("Material asset is not open.");
    ATELIER_TRY(admit_material_edit(at->second.root.id()));
    auto& doc = at->second;
    if (!material_can_undo(key, redo))
        return {};
    auto result =
        apply_material_entries(doc.root.id(), doc.history[redo ? doc.cursor : doc.cursor - 1].entries, redo, project);
    if (result) {
        if (redo)
            ++doc.cursor;
        else
            --doc.cursor;
        ++revision_;
        forward_material(doc.root.id());
    }
    return result;
}

Result<void> Session::save_material(Project& project, std::string_view key) {
    if (material_edit_)
        return failure("Finish the active material edit before saving.");
    const auto at = materials_.find(std::string(key));
    if (at == materials_.end())
        return failure("Material asset is not open.");
    if (const auto* item = unavailable(assets::AssetKind::material, key))
        return stand_in(*item);
    ATELIER_TRY_BIND(asset, material_asset(at->second.root.id()));
    ATELIER_TRY_BIND(saved, project.save_material(*asset));
    if (const auto* instance = std::get_if<assets::MaterialInstanceAsset>(&asset->author))
        at->second.saved = instance->entries;
    ++revision_;
    return {};
}

Result<void> Session::set_material_texture(Project& project, ObjectId object, std::string_view path,
                                           std::string_view key) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(view, inspect_material(object));
    const auto field = std::ranges::find(view->declarations, path, &MaterialParameterView::path);
    if (field == view->declarations.end() || field->components)
        return failure("Unknown material texture declaration.");
    if (!key.empty()) {
        ATELIER_TRY_BIND(texture, texture_resource(project.assets(), key));
    }
    auto value = std::get<material::TextureBinding>(field->value);
    if (const auto previous = entry_at(view->entries, path);
        previous && std::holds_alternative<material::TextureBinding>(previous->value))
        value = std::get<material::TextureBinding>(previous->value);
    value.asset_key = key;
    return set_material_entry(object, {std::string(path), std::move(value), true});
}

Result<void> Session::replace_material_bindings(ObjectId object, std::vector<presentation::MaterialBinding> after) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(component, admit_component_edit(object));
    ATELIER_TRY_BIND(before, presentation::read_material_bindings(runtime_, object));
    const auto equal = [](const auto& a, const auto& b) {
        return a.slot == b.slot && a.resource.id() == b.resource.id() && a.overridden == b.overridden;
    };
    if (std::ranges::equal(*before, after, equal))
        return {};
    ATELIER_TRY_BIND(root, runtime_.hold(object));
    ATELIER_TRY_BIND(changed, presentation::set_material_bindings(runtime_, object, after));
    remember(MaterialBindingChange{std::move(*root), component->owner, std::move(*before), std::move(after)});
    return {};
}

Result<void> Session::assign_material(Project& project, ObjectId object, std::string_view slot, std::string_view key) {
    ATELIER_TRY_BIND(resource, material_resource(project.assets(), key));
    ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime_, object));
    const auto at = std::ranges::find(*bindings, slot, &presentation::MaterialBinding::slot);
    if (at == bindings->end())
        bindings->push_back({std::string(slot), std::move(*resource), true});
    else {
        at->resource = std::move(*resource);
        at->overridden = true;
    }
    return replace_material_bindings(object, std::move(*bindings));
}

Result<void> Session::clear_material(ObjectId object, std::string_view slot) {
    ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime_, object));
    ATELIER_TRY_BIND(type, runtime_.type_of(object));
    ATELIER_TRY_BIND(is_generator, runtime_.is_a(*type, presentation_types_.procedural_generator_component));
    std::erase_if(*bindings,
                  [&](const auto& binding) { return binding.slot == slot && (*is_generator || binding.overridden); });
    return replace_material_bindings(object, std::move(*bindings));
}

Result<ObjectRoot> Session::create_scene_material(ObjectId component, std::string_view slot) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(state, admit_component_edit(component));
    ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime_, component));
    const auto at = std::ranges::find(*bindings, slot, &presentation::MaterialBinding::slot);
    ObjectRoot parent;
    if (at != bindings->end())
        parent = at->resource;
    else {
        ATELIER_TRY_BIND(is_mesh, runtime_.is_a(state->type, presentation_types_.static_mesh_component));
        if (*is_mesh) {
            ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(runtime_, component));
            const auto fallback = std::ranges::find(mesh->mesh.defaults, slot, &presentation::MaterialBinding::slot);
            if (fallback != mesh->mesh.defaults.end())
                parent = fallback->resource;
        }
        if (parent.id() == ObjectId{}) {
            assets::Reader installed;
            ATELIER_TRY_BIND(grid, material_resource(installed, "builtin/material/default-grid"));
            parent = std::move(*grid);
        }
    }
    ATELIER_TRY_BIND(parent_key, material::material_key(runtime_, parent.id()));
    ATELIER_TRY_BIND(instance, material::create_instance(runtime_, {}, material::InstanceState{parent, {}, {}}));
    scene_material_parents_.emplace(instance->id(), *parent_key);
    if (at == bindings->end())
        bindings->push_back({std::string(slot), *instance, true});
    else {
        at->resource = *instance;
        at->overridden = true;
    }
    auto replaced = replace_material_bindings(component, std::move(*bindings));
    if (!replaced) {
        scene_material_parents_.erase(instance->id());
        return std::unexpected(replaced.error());
    }
    return std::move(*instance);
}

Result<void> Session::set_material_override(ObjectId object, std::string_view path, bool enabled, Project* project) {
    ATELIER_TRY_BIND(edit, prepare_material_edit(object));
    ATELIER_TRY_BIND(view, inspect_material(object));
    auto entry = entry_at(view->entries, path);
    if (!entry) {
        if (!enabled)
            return {};
        const auto field = std::ranges::find(view->declarations, path, &MaterialParameterView::path);
        if (field == view->declarations.end())
            return failure("Unknown material declaration.");
        entry = material::Override{std::string(path), field->value, true};
    }
    entry->enabled = enabled;
    return change_material_entry(*edit, {std::string(path), {}, std::move(entry)}, project);
}

Result<void> Session::discard_material_override(ObjectId object, std::string_view path) {
    ATELIER_TRY_BIND(edit, prepare_material_edit(object));
    return change_material_entry(*edit, {std::string(path), {}, {}}, nullptr);
}
}
