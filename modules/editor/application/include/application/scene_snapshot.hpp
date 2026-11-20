#pragma once

#include "application/timing.hpp"
#include "editor/session.hpp"
#include "editor/viewport_tool.hpp"
#include "foundation/result.hpp"
#include "renderer/renderer.hpp"
#include "logging/logging.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace atelier::application {
// Host-owned candidate, pending preparation and adopted draw group. refresh()
// extracts live values incrementally; prepare() uses only those frozen values.
// ObjectIds use full equality, never hashes as renderer identities. Release this
// owner (or reset it) before closing the Renderer used by prepare().
class SceneSnapshot final {
public:
    struct Draw {
        std::vector<renderer::MeshInstance> meshes;

        std::vector<renderer::Light> lights;

        std::uint64_t identity{};

        std::shared_ptr<const editor::ViewportScene> picking;

        std::shared_ptr<renderer::MaterialBatch> materials;

        std::uint64_t material_request{};
    };

    // Mesh instances and lights built so far; a frame without changes builds none.
    std::uint64_t built{};

    // Latest candidate, NOT the adopted group. Rendering must use draw().
    std::vector<renderer::MeshInstance> meshes;

    // Candidate geometry membership; the adopted picking group owns its own copy.
    std::vector<ObjectId> mesh_entities;

    std::vector<renderer::Light> lights;

    std::uint64_t identity{1};

    void reset() {
        ++identity;
        next_key_ = 1;
        mesh_keys_.clear();
        generator_keys_.clear();
        generator_slices_.clear();
        light_keys_.clear();
        meshes.clear();
        mesh_entities.clear();
        lights.clear();
        dense_.clear();
        static_mesh_count_ = 0;
        synced_.reset();
        mesh_surfaces_.clear();
        surfaces_.clear();
        locators_.clear();
        locator_sources_.clear();
        locator_cursor_ = 0;
        picking_.reset();
        pending_.reset();
        adopted_.reset();
        ++request_;
    }

    // Play suppresses selection; this is part of the frozen request, not a
    // mutation of an already adopted group's instances.
    Result<void> refresh(const editor::Session& document, bool show_selection = true) {
        const auto& scene = document.presentation();
        // The slot map below indexes the synchronized Scene's cache; another one needs reset().
        if (synced_ && synced_->scene != scene.identity())
            return std::unexpected(Error{ErrorCode::argument_mismatch,
                                         "A scene snapshot was refreshed from another document without reset()."});
        auto version = [&] {
            timing::Scope extracting{phases::extract};
            return scene.update(document.presented_poses());
        }();
        if (!version)
            return std::unexpected(version.error());
        const auto selection = show_selection ? document.selected_entities() : std::span<const ObjectId>{};
        const bool reselect = !synced_ || !std::ranges::equal(synced_->selection, selection);
        bool changed = !synced_ || synced_->version != *version || reselect;
        const auto slots = scene.mesh_slots();
        const auto gen_slots = scene.generator_slots();
        const auto since = synced_ ? synced_->version : 0;
        // Slots keep their component until the layout changes; a slot that starts or
        // stops drawing changes the dense order, so the instances are rebuilt.
        bool relayout = !synced_ || scene.mesh_layout() > since;
        bool bindings_changed = relayout;
        bool generators_changed = relayout;
        for (std::size_t slot = 0; changed && !relayout && slot < slots.size(); ++slot) {
            if (slots[slot].changed <= since)
                continue;
            const auto at = dense_[slot];
            if ((at == absent) == slots[slot].item.has_value()) {
                relayout = bindings_changed = generators_changed = true;
                break;
            }
            if (at != absent) {
                auto& value = meshes[at];
                bindings_changed |= mesh_surfaces_[at] != slots[slot].item->surfaces;
                mesh_surfaces_[at] = slots[slot].item->surfaces;
                const auto mapping = value.material_slots;
                value = instance(slots[slot], value.stable_key, value.selected);
                value.material_slots = mapping;
            }
        }
        for (std::size_t g = 0; changed && !generators_changed && g < gen_slots.size(); ++g) {
            if (gen_slots[g].changed > since) {
                generators_changed = true;
                break;
            }
        }
        if (relayout) {
            for (auto& [object, key] : mesh_keys_) {
                (void)object;
                key.seen = false;
            }
            meshes.clear();
            mesh_entities.clear();
            mesh_surfaces_.clear();
            dense_.assign(slots.size(), absent);
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (!slots[slot].item)
                    continue;
                const auto& item = *slots[slot].item;
                dense_[slot] = static_cast<std::uint32_t>(meshes.size());
                mesh_entities.push_back(item.entity);
                mesh_surfaces_.push_back(item.surfaces);
                meshes.push_back(instance(slots[slot], key(mesh_keys_, item.component), false));
            }
            static_mesh_count_ = meshes.size();
            std::erase_if(mesh_keys_, [](const auto& item) { return !item.second.seen; });
            generator_slices_.assign(gen_slots.size(), {});
        }
        if (generators_changed && (!gen_slots.empty() || meshes.size() > static_mesh_count_)) {
            if (generator_slices_.size() != gen_slots.size())
                generator_slices_.assign(gen_slots.size(), {});
            meshes.resize(static_mesh_count_);
            mesh_entities.resize(static_mesh_count_);
            mesh_surfaces_.resize(static_mesh_count_);
            for (auto& [_, gk] : generator_keys_)
                gk.seen = false;
            for (std::size_t g = 0; g < gen_slots.size(); ++g) {
                const auto& gslot = gen_slots[g];
                auto& slice = generator_slices_[g];
                if (!relayout && gslot.changed <= since) {
                    if (slice.present) {
                        if (auto it = generator_keys_.find(slice.component); it != generator_keys_.end())
                            it->second.seen = true;
                        mesh_entities.insert(mesh_entities.end(), slice.meshes.size(), slice.entity);
                        mesh_surfaces_.insert(mesh_surfaces_.end(), slice.mesh_surfaces.begin(),
                                              slice.mesh_surfaces.end());
                        meshes.insert(meshes.end(), slice.meshes.begin(), slice.meshes.end());
                    }
                    continue;
                }
                if (!gslot.item) {
                    if (slice.present)
                        bindings_changed = true;
                    slice = {};
                    continue;
                }
                const auto& gen = *gslot.item;
                const math::Matrix4 gen_world = gslot.presented ? *gslot.presented : gen.world;
                const bool same_surface_set = [&] {
                    if (!slice.present || slice.component != gen.component ||
                        slice.surface_materials != gen.surface_materials ||
                        slice.collections.size() != gen.collections.size())
                        return false;
                    for (std::size_t c = 0; c < gen.collections.size(); ++c)
                        if (slice.collections[c].surfaces != gen.collections[c].surfaces)
                            return false;
                    return true;
                }();
                std::unordered_map<std::uint64_t, std::shared_ptr<const std::vector<std::uint32_t>>> prev_slots;
                if (same_surface_set) {
                    prev_slots.reserve(slice.meshes.size());
                    for (const auto& prev_m : slice.meshes)
                        if (prev_m.material_slots)
                            prev_slots.emplace(prev_m.stable_key, prev_m.material_slots);
                } else {
                    bindings_changed = true;
                }

                auto& gkeys = generator_keys_[gen.component];
                gkeys.seen = true;
                if (gkeys.surface_key == 0)
                    gkeys.surface_key = next_key_++;
                for (auto& [_, ck] : gkeys.chunks)
                    ck.seen = false;

                std::vector<renderer::MeshInstance> next_meshes;
                std::vector<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> next_surfaces;
                if (gen.surface_geometry && gen.surface_materials) {
                    ++built;
                    std::shared_ptr<const std::vector<std::uint32_t>> kept_slots;
                    if (const auto it = prev_slots.find(gkeys.surface_key); it != prev_slots.end())
                        kept_slots = it->second;
                    else
                        bindings_changed = true;
                    next_surfaces.push_back(gen.surface_materials);
                    next_meshes.push_back(
                        {gen_world, false, gen.surface_geometry, gkeys.surface_key, std::move(kept_slots)});
                }
                for (const auto& col : gen.collections) {
                    if (!col.geometry || !col.transforms || !col.surfaces)
                        continue;
                    auto& chunk_entry = gkeys.chunks[col.transforms.get()];
                    std::vector<std::uint64_t> duplicate_keys;
                    const std::vector<std::uint64_t>* active_keys = nullptr;
                    if (chunk_entry.seen) {
                        duplicate_keys.resize(col.transforms->size());
                        for (auto& k : duplicate_keys)
                            k = next_key_++;
                        active_keys = &duplicate_keys;
                    } else {
                        chunk_entry.owner = col.transforms;
                        if (chunk_entry.keys.size() != col.transforms->size()) {
                            chunk_entry.keys.resize(col.transforms->size());
                            for (auto& k : chunk_entry.keys)
                                k = next_key_++;
                        }
                        chunk_entry.seen = true;
                        active_keys = &chunk_entry.keys;
                    }
                    const auto& xforms = *col.transforms;
                    for (std::size_t i = 0; i < xforms.size(); ++i) {
                        const auto inst_world = presentation::compose_instance_world(xforms[i], gen_world);
                        if (!inst_world)
                            continue;
                        ++built;
                        const std::uint64_t inst_key = (*active_keys)[i];
                        std::shared_ptr<const std::vector<std::uint32_t>> kept_slots;
                        if (const auto it = prev_slots.find(inst_key); it != prev_slots.end())
                            kept_slots = it->second;
                        else
                            bindings_changed = true;
                        next_surfaces.push_back(col.surfaces);
                        next_meshes.push_back({*inst_world, false, col.geometry, inst_key, std::move(kept_slots)});
                    }
                }
                std::erase_if(gkeys.chunks, [](const auto& item) { return !item.second.seen; });
                if (same_surface_set && next_meshes.size() != slice.meshes.size())
                    bindings_changed = true;

                slice.present = true;
                slice.entity = gen.entity;
                slice.component = gen.component;
                slice.surface_materials = gen.surface_materials;
                slice.collections = gen.collections;
                slice.meshes = std::move(next_meshes);
                slice.mesh_surfaces = std::move(next_surfaces);

                mesh_entities.insert(mesh_entities.end(), slice.meshes.size(), slice.entity);
                mesh_surfaces_.insert(mesh_surfaces_.end(), slice.mesh_surfaces.begin(), slice.mesh_surfaces.end());
                meshes.insert(meshes.end(), slice.meshes.begin(), slice.meshes.end());
            }
            std::erase_if(generator_keys_, [](const auto& item) { return !item.second.seen; });
        }
        if (bindings_changed)
            ATELIER_TRY(bind_materials());
        if (relayout || generators_changed || reselect)
            ATELIER_TRY(highlight(document, selection));
        if (!synced_ || scene.lights_changed() > since) {
            for (auto& [object, key] : light_keys_) {
                (void)object;
                key.seen = false;
            }
            lights.clear();
            for (const auto& item : scene.light_snapshot())
                lights.push_back(light(item, key(light_keys_, item.component)));
            std::erase_if(light_keys_, [](const auto& item) { return !item.second.seen; });
        }
        auto locators = refresh_locators(
            document, relayout || reselect || !synced_ || synced_->world_revision != document.world().revision(),
            selection);
        if (!locators)
            return std::unexpected(locators.error());
        changed |= *locators;
        if (changed) {
            auto picking = std::make_shared<editor::ViewportScene>();
            picking->scene_identity = scene.identity();
            picking->materials = surfaces_;
            picking->locators = locators_;
            picking->meshes.reserve(meshes.size());
            for (std::size_t i = 0; i < meshes.size(); ++i)
                picking->meshes.push_back(
                    {mesh_entities[i], meshes[i].world, meshes[i].geometry, meshes[i].material_slots});
            picking_ = std::move(picking);
            ++request_;
            synced_ =
                Synced{scene.identity(), *version, document.world().revision(), {selection.begin(), selection.end()}};
        }
        return {};
    }

    // Pure CPU candidate owners, useful to consumers that do not submit a GPU
    // frame. A viewport host must instead use draw()->picking, and retain it
    // only after the corresponding render returned accepted.
    const std::shared_ptr<const editor::ViewportScene>& picking() const noexcept { return picking_; }

    // The old complete group survives pending/failed replacements. The returned
    // borrow lasts until prepare()/reset(); accepted picking owns CPU data only.
    const Draw* draw() const noexcept { return adopted_ ? &adopted_->draw : nullptr; }

    renderer::Result<renderer::Preparation> prepare(renderer::Renderer& renderer) {
        if (!picking_)
            return std::unexpected(
                renderer::Error{{.code = rhi::ErrorCode::invalid_argument,
                                 .message = "Refresh the scene before preparing its material group."}});
        if (adopted_ && adopted_->request == request_)
            return renderer::Preparation{0, 0, true};
        if (!pending_ || pending_->request != request_) {
            // Transforms, selection or lights may change while code compiles.
            // Reuse preparation only for the exact same Surface table; replace
            // the entire frozen draw request, never graft live values at adoption.
            std::shared_ptr<renderer::MaterialBatch> batch;
            const auto matches = [&](const std::optional<Prepared>& group) {
                return group && group->draw.materials &&
                       std::ranges::equal(group->draw.materials->surfaces(), surfaces_);
            };
            std::uint64_t preparation{};
            if (matches(pending_)) {
                batch = pending_->draw.materials;
                preparation = pending_->draw.material_request;
            } else if (matches(adopted_)) {
                batch = adopted_->draw.materials;
                preparation = adopted_->draw.material_request;
            }
            // Keep the previous code jobs alive through submission, so a new
            // parameter/image table can share compilation rather than restart it.
            [[maybe_unused]] const auto previous = pending_ ? pending_->draw.materials : nullptr;
            pending_ = Prepared{request_, Draw{meshes, lights, identity, picking_, std::move(batch), preparation}, {}};
            if (!surfaces_.empty() && !pending_->draw.materials) {
                pending_->draw.material_request = request_;
                material_event("material.requested");
                auto submitted = renderer.prepare_materials(surfaces_);
                if (!submitted) {
                    pending_->failure = submitted.error();
                    material_event("material.rejected", &submitted.error());
                } else
                    pending_->draw.materials = std::make_shared<renderer::MaterialBatch>(std::move(*submitted));
            }
        }
        if (pending_->failure)
            return std::unexpected(*pending_->failure);
        auto progress = pending_->draw.materials
                            ? renderer.preparation(*pending_->draw.materials)
                            : renderer::Result<renderer::Preparation>{renderer::Preparation{0, 0, true}};
        if (!progress) {
            pending_->failure = progress.error();
            material_event("material.rejected", &progress.error());
            return progress;
        }
        if (progress->ready) {
            if (pending_->draw.material_request &&
                (!adopted_ || adopted_->draw.material_request != pending_->draw.material_request))
                material_event("material.adopted");
            adopted_ = std::move(pending_);
            pending_.reset();
        }
        return progress;
    }

    // Called only after the host accepts the rendered frame. Transforms and
    // selection can replace draw requests without changing the material group.
    void accept(std::uint64_t frame, bool fallback, renderer::Shading shading) noexcept try {
        if (!adopted_)
            return;
        const auto& draw = adopted_->draw;
        const auto identity = std::tuple{draw.picking->scene_identity, draw.material_request, fallback, shading};
        if (accepted_material_ == identity)
            return;
        accepted_material_ = identity;
        logging::Entry entry{.category = "material",
                             .event_code = "material.view_accepted",
                             .message = "Material group reached an accepted view."};
        entry.fields = {{"document", draw.picking->scene_identity},
                        {"draw_request", adopted_->request},
                        {"request_generation", draw.material_request},
                        {"frame", frame},
                        {"fallback", fallback},
                        {"shading", static_cast<std::uint64_t>(shading)}};
        static_cast<void>(logging::submit(std::move(entry)));
    } catch (...) {
        std::fputs("Material accepted-frame diagnostic capture failed.\n", stderr);
    }

private:
    static constexpr std::uint32_t absent = std::numeric_limits<std::uint32_t>::max();

    struct Key {
        std::uint64_t value{};

        bool seen{};
    };

    struct Synced {
        std::uint64_t scene;

        std::uint64_t version;

        std::uint64_t world_revision;

        std::vector<ObjectId> selection;
    };

    struct Prepared {
        std::uint64_t request;

        Draw draw;

        std::optional<renderer::Error> failure;
    };

    std::optional<std::tuple<std::uint64_t, std::uint64_t, bool, renderer::Shading>> accepted_material_;

    struct ChunkKeys {
        std::shared_ptr<const std::vector<procedural::Affine>> owner;

        std::vector<std::uint64_t> keys;

        bool seen{};
    };

    struct GeneratorKeys {
        std::uint64_t surface_key{};

        std::unordered_map<const void*, ChunkKeys> chunks;

        bool seen{};
    };

    struct GeneratorSlice {
        bool present{};

        ObjectId entity{};

        ObjectId component{};

        std::shared_ptr<const std::vector<material::ProgramSurfaceRef>> surface_materials;

        std::vector<presentation::GeneratorRenderCollection> collections;

        std::vector<renderer::MeshInstance> meshes;

        std::vector<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> mesh_surfaces;
    };

    std::unordered_map<ObjectId, Key> mesh_keys_;

    std::unordered_map<ObjectId, GeneratorKeys> generator_keys_;

    std::vector<GeneratorSlice> generator_slices_;

    std::unordered_map<ObjectId, Key> light_keys_;

    std::uint64_t next_key_{1};

    std::vector<std::uint32_t> dense_; // Mesh slot to its index in `meshes`, or absent.

    std::size_t static_mesh_count_{};

    std::optional<Synced> synced_;

    std::vector<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> mesh_surfaces_;

    std::vector<material::ProgramSurfaceRef> surfaces_;

    std::vector<editor::ViewportScene::Locator> locators_;

    std::unordered_set<ObjectId> locator_sources_;

    std::uint64_t locator_cursor_{};

    std::uint64_t request_{};

    std::shared_ptr<const editor::ViewportScene> picking_;

    std::optional<Prepared> pending_;

    std::optional<Prepared> adopted_;

    void material_event(std::string_view code, const renderer::Error* error = nullptr) const noexcept {
        try {
            logging::Entry entry{.severity = error ? logging::Severity::error : logging::Severity::info,
                                 .category = "material",
                                 .event_code = std::string(code),
                                 .message = error ? error->cause.message : "Material preparation transition."};
            entry.fields = {{"document", picking_->scene_identity},
                            {"draw_request", pending_->request},
                            {"surfaces", std::uint64_t{surfaces_.size()}}};
            if (pending_->draw.material_request)
                entry.fields.push_back({"request_generation", pending_->draw.material_request});
            if (error) {
                entry.fields.push_back({"terminal", error->terminal});
                entry.fields.push_back({"error_code", static_cast<std::uint64_t>(error->cause.code)});
            }
            static_cast<void>(logging::submit(std::move(entry)));
        } catch (...) {
            std::fputs("Material diagnostic capture failed.\n", stderr);
        }
    }

    std::uint64_t key(std::unordered_map<ObjectId, Key>& keys, ObjectId object) {
        auto [at, added] = keys.try_emplace(object);
        if (added)
            at->second.value = next_key_++;
        at->second.seen = true;
        return at->second.value;
    }

    // A mesh is drawn at its presented matrix while a simulation interpolates it.
    renderer::MeshInstance instance(const presentation::MeshSlot& slot, std::uint64_t key, bool selected) {
        const auto& item = *slot.item;
        ++built;
        renderer::MeshInstance value;
        value.world = slot.presented ? *slot.presented : item.world;
        value.selected = selected;
        value.geometry = item.geometry;
        value.stable_key = key;
        return value;
    }

    Result<void> bind_materials() {
        std::vector<material::ProgramSurfaceRef> surfaces;
        std::unordered_map<const material::ProgramSurface*, std::uint32_t> indices;
        std::unordered_map<const std::vector<material::ProgramSurfaceRef>*,
                           std::shared_ptr<const std::vector<std::uint32_t>>>
            slot_cache;
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const auto& source = mesh_surfaces_[i];
            if (!source || source->size() != meshes[i].geometry->materials().size())
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "A scene mesh needs one captured Surface per material slot."});
            std::shared_ptr<const std::vector<std::uint32_t>> mapped;
            if (const auto cached_it = slot_cache.find(source.get()); cached_it != slot_cache.end()) {
                mapped = cached_it->second;
            } else {
                std::vector<std::uint32_t> slots;
                slots.reserve(source->size());
                for (const auto& surface : *source) {
                    if (!surface || surfaces.size() >= absent)
                        return std::unexpected(Error{ErrorCode::invalid_definition, "Invalid scene material table."});
                    const auto [at, added] =
                        indices.try_emplace(surface.get(), static_cast<std::uint32_t>(surfaces.size()));
                    if (added)
                        surfaces.push_back(surface);
                    slots.push_back(at->second);
                }
                if (meshes[i].material_slots && *meshes[i].material_slots == slots)
                    mapped = meshes[i].material_slots;
                else
                    mapped = std::make_shared<const std::vector<std::uint32_t>>(std::move(slots));
                slot_cache.emplace(source.get(), mapped);
            }
            if (!meshes[i].material_slots || *meshes[i].material_slots != *mapped)
                meshes[i].material_slots = std::move(mapped);
        }
        std::size_t gen_offset = static_mesh_count_;
        for (auto& slice : generator_slices_) {
            if (!slice.present)
                continue;
            for (std::size_t k = 0; k < slice.meshes.size() && gen_offset + k < meshes.size(); ++k)
                slice.meshes[k].material_slots = meshes[gen_offset + k].material_slots;
            gen_offset += slice.meshes.size();
        }
        surfaces_ = std::move(surfaces);
        return {};
    }

    Result<bool> refresh_locators(const editor::Session& document, bool rebuild, std::span<const ObjectId> selection) {
        auto& runtime = document.world().runtime();
        if (!rebuild)
            rebuild = std::ranges::any_of(
                locator_sources_, [&](ObjectId object) { return runtime.changed_since(object, locator_cursor_); });
        bool changed{};
        if (rebuild) {
            const auto members = document.world().members();
            if (!members)
                return std::unexpected(members.error());
            const std::unordered_set<ObjectId> rendered(mesh_entities.begin(), mesh_entities.end());
            const std::unordered_set<ObjectId> selected(selection.begin(), selection.end());
            std::vector<editor::ViewportScene::Locator> locators;
            locator_sources_.clear();
            for (const auto& member : *members)
                if (!rendered.contains(member.id())) {
                    // Include nonspatial members: a later transform write may make
                    // them visible without changing capability layout.
                    for (auto node = member.id(); node != ObjectId{};) {
                        if (!locator_sources_.insert(node).second)
                            break;
                        const auto parent = world::spatial_parent(runtime, node);
                        if (!parent)
                            break;
                        node = *parent;
                    }
                    const auto spatial = world::has_transform(runtime, member.id());
                    if (!spatial || !*spatial)
                        continue;
                    const auto matrix = world::world_matrix(runtime, member.id());
                    if (!matrix)
                        continue;
                    locators.push_back(
                        {member.id(), {(*matrix)[12], (*matrix)[13], (*matrix)[14]}, selected.contains(member.id())});
                }
            changed = locators_ != locators;
            locators_ = std::move(locators);
        }
        const auto cursor = runtime.change_cursor();
        if (!cursor)
            return std::unexpected(cursor.error());
        locator_cursor_ = *cursor;
        return changed;
    }

    renderer::Light light(const presentation::LightRenderItem& item, std::uint64_t key) {
        ++built;
        const auto& settings = item.settings;
        renderer::Light value;
        value.stable_key = key;
        switch (settings.kind) {
        case presentation::LightKind::point:
            value.kind = renderer::LightKind::point;
            break;
        case presentation::LightKind::rectangle:
            value.kind = renderer::LightKind::rectangle;
            break;
        case presentation::LightKind::directional:
            value.kind = renderer::LightKind::directional;
            break;
        }
        value.position = item.position;
        value.direction = item.direction;
        value.right = item.right;
        value.up = item.up;
        value.color = settings.color;
        value.intensity = settings.intensity;
        value.range = settings.range;
        value.source_radius = settings.source_radius;
        value.width = settings.width;
        value.height = settings.height;
        value.two_sided = settings.two_sided;
        value.casts_shadow = settings.casts_shadow;
        return value;
    }

    // A mesh is highlighted when its Entity or an ancestor is selected.
    Result<void> highlight(const editor::Session& document, std::span<const ObjectId> selection) {
        if (selection.empty()) {
            for (auto& mesh : meshes)
                mesh.selected = false;
            return {};
        }
        const auto members = document.world().members();
        if (!members)
            return std::unexpected(members.error());
        std::unordered_set<ObjectId> selected(selection.begin(), selection.end());
        std::unordered_map<ObjectId, bool> inherited;
        inherited.reserve(members->size());
        // The World directory is in composition preorder: each parent is already available.
        for (const auto& member : *members) {
            const auto parent = world::parent_entity(document.world().runtime(), member.id());
            if (!parent)
                return std::unexpected(parent.error());
            inherited.emplace(member.id(),
                              selected.contains(member.id()) || (*parent != ObjectId{} && inherited.at(*parent)));
        }
        for (std::size_t i = 0; i < meshes.size(); ++i)
            meshes[i].selected = inherited.at(mesh_entities[i]);
        return {};
    }
};
}
