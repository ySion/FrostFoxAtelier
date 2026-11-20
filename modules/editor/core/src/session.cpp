#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/fields.hpp"
#include "editor/operator_parameters.hpp"
#include "editor/view_options.hpp"
#include "physics/physics.hpp"
#include "procedural/graph.hpp"
#include "source/program.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <type_traits>

namespace atelier::editor {
namespace {
auto invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}

Result<bool> read_only_path(Runtime& runtime, ObjectId object, std::span<const FieldId> path) {
    ATELIER_TRY_BIND(current, runtime.type_of(object));
    for (const auto step : path) {
        ATELIER_TRY_BIND(info, runtime.describe(*current));
        if ((*info)->kind == TypeKind::array) {
            current = *(*info)->element;
            continue;
        }
        const auto field = std::ranges::find((*info)->fields, step, &FieldInfo::id);
        if (field == (*info)->fields.end())
            return false;
        if (field->read_only)
            return true;
        current = field->value.type;
    }
    return false;
}

Result<void> validate_property_scalar(Runtime& runtime, const Scalar& value) {
    if (!detail::finite_scalar(value))
        return invalid("Property values must be finite.");
    if (const auto* enumerated = std::get_if<EnumValue>(&value)) {
        const auto type = runtime.describe(enumerated->type);
        if (!type || !(*type)->enumeration || !detail::valid_enum_bits(*(*type)->enumeration, enumerated->bits))
            return invalid("Choose a declared enum value so the scene remains serializable.");
    }
    return {};
}
}

Result<void> Session::initialize(Prepare prepare, SessionRole role) {
    assert(!presentation_);
    ATELIER_TRY_BIND(types, world::register_types(runtime_));
    types_ = *types;
    ATELIER_TRY_BIND(presentation_types, presentation::register_types(runtime_));
    presentation_types_ = *presentation_types;
    ATELIER_TRY_BIND(physics_types, physics::register_types(runtime_));
    physics_types_ = *physics_types;
    ATELIER_TRY(procedural::register_types(runtime_));
    ATELIER_TRY(world_.initialize(runtime_, types_));
    ATELIER_TRY_BIND(presentation, presentation::Scene::create(world_, presentation_types_));
    presentation_ = std::move(*presentation);
    parameters_ = {Parameter{"X", "m", types_.position_fields[0]}, Parameter{"Y", "m", types_.position_fields[1]},
                   Parameter{"Z", "m", types_.position_fields[2]}};
    static_assert(std::is_nothrow_move_constructible_v<Change> && std::is_nothrow_move_assignable_v<Change>);
    history_.reserve(history_limit + 1);
    if (role == SessionRole::editor)
        ATELIER_TRY(register_editor_types(runtime_));
    if (prepare) {
        ATELIER_TRY_BIND(ready, prepare(world_));
        prepared_ = std::move(*ready);
    }
    return {};
}

Session::~Session() {
    close_graphs();
    if (presentation_)
        (void)world_.close();
}

void Session::remember(Change change) {
    forward_change(change, true);
    ++revision_;
    if (saved_cursor_ && *saved_cursor_ > cursor_)
        saved_cursor_.reset();
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    auto released = release(change);
    history_.push_back(Entry{std::move(change), std::move(released)});
    ++recorded_;
    if (history_.size() > history_limit) {
        history_.erase(history_.begin());
        if (saved_cursor_) {
            if (*saved_cursor_)
                --*saved_cursor_;
            else
                saved_cursor_.reset();
        }
    }
    cursor_ = history_.size();
}

std::wstring_view Session::name(ObjectId entity) const noexcept {
    const auto at = names_.find(entity);
    return at == names_.end() ? std::wstring_view{} : at->second;
}

Result<void> Session::admit_entity_edit(ObjectId entity) const {
    ATELIER_TRY_BIND(state, world::inspect_entity(world_.runtime(), entity));
    if (!state->can_edit || !names_.contains(entity))
        return invalid("The Entity is not editable in this document.");
    ATELIER_TRY_BIND(members, world_.members());
    if (std::ranges::none_of(*members, [entity](const auto& member) { return member.id() == entity; }))
        return invalid("The Entity is not a member of this document.");
    return {};
}

Result<world::ComponentStatus> Session::admit_component_edit(ObjectId component) const {
    ATELIER_TRY_BIND(state, world::inspect_component(world_.runtime(), component));
    if (!state->valid || state->owner == ObjectId{})
        return invalid("The Component is not attached to an editable Entity.");
    ATELIER_TRY(admit_entity_edit(state->owner));
    return state;
}

Result<void> Session::validate_name(std::wstring_view name) {
    if (name.empty() || name.size() > 256 ||
        std::all_of(name.begin(), name.end(), [](wchar_t c) { return c == L' '; }) ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == 127; }))
        return invalid("Use a name with 1–256 visible characters.");
    return {};
}

namespace {
// The EnvironmentComponent field names of a World Settings option (a view option
// for which world_option holds), e.g. probe_volume_0_min_x -> volume0.minimum.x.
std::vector<std::string> environment_field(std::string_view option) {
    constexpr std::string_view volume = "probe_volume_";
    if (option.starts_with(volume) && option.size() > volume.size() + 2) {
        std::vector<std::string> names{"volume" + std::string(option.substr(volume.size(), 1))};
        const auto rest = option.substr(volume.size() + 2);
        if (rest.starts_with("min_")) {
            names.emplace_back("minimum");
            names.emplace_back(rest.substr(4));
        } else if (rest.starts_with("max_")) {
            names.emplace_back("maximum");
            names.emplace_back(rest.substr(4));
        } else
            names.emplace_back(rest);
        return names;
    }
    if (option.starts_with("sky_color_"))
        return {"sky_color", std::string(option.substr(10))};
    return {std::string(option)};
}
}

ObjectId Session::environment() const {
    const auto found = world_.query(presentation_types_.environment_component);
    return found && !found->empty() ? found->front() : ObjectId{};
}

Result<ObjectId> Session::main_camera() const {
    const auto existing = environment();
    if (existing == ObjectId{})
        return ObjectId{};
    ATELIER_TRY_BIND(field, runtime_.find_field(presentation_types_.environment_component, "main_camera"));
    ATELIER_TRY_BIND(value, runtime_.read_reference(existing, *field));
    return value->id();
}

Result<void> Session::set_main_camera(ObjectId target) {
    if (editing())
        return invalid("Finish the current edit before changing the main camera.");
    if (world_.running() || world_.closed())
        return invalid("Main camera authoring requires an open, stopped document.");
    ATELIER_TRY_BIND(reference, runtime_.null_reference(types_.entity, ValueForm::weak_reference));
    if (target != ObjectId{}) {
        ATELIER_TRY(admit_entity_edit(target));
        ATELIER_TRY_BIND(cameras, world_.members(presentation_->cameras()));
        if (std::ranges::count(*cameras, target, &world::CapabilityMember::entity) != 1)
            return invalid("Set as Main Camera requires an Entity with exactly one CameraComponent.");
        ATELIER_TRY_BIND(held, runtime_.hold(target, types_.entity));
        reference = runtime_.observe(held->reference());
        if (!reference)
            return std::unexpected(reference.error());
    }
    ATELIER_TRY_BIND(field, runtime_.find_field(presentation_types_.environment_component, "main_camera"));
    if (const auto existing = environment(); existing != ObjectId{}) {
        ATELIER_TRY_BIND(owner, admit_property_edit(existing));
        ATELIER_TRY_BIND(before, runtime_.read_reference(existing, *field));
        if (*before == *reference && !has_retained(existing))
            return {};
        ATELIER_TRY_BIND(held, runtime_.hold(existing));
        ReferenceFieldChange change{std::move(*held), *owner, {*field}, *before, *reference};
        ATELIER_TRY(apply_reference(change, true));
        remember(std::move(change));
        return {};
    }
    if (target == ObjectId{})
        return {};
    ATELIER_TRY_BIND(created, create_environment({}, {}));
    ATELIER_TRY_BIND(parts, world::components(runtime_, created->id()));
    ATELIER_TRY(runtime_.write_reference(parts->front().id(), *field, *reference));
    return publish_environment(std::move(*created));
}

Result<void> Session::apply_reference(const ReferenceFieldChange& change, bool forward) {
    ATELIER_TRY_BIND(owner, admit_property_edit(change.root.id()));
    if (*owner != change.entity)
        return invalid("The reference field's owning Entity changed.");
    return runtime_.write_reference(change.root.id(), change.path, forward ? change.after : change.before);
}

Result<std::optional<Session::ReferenceFieldChange>> Session::main_camera_removal(std::span<const ObjectId> targets) {
    ATELIER_TRY_BIND(selected, main_camera());
    if (!world_.contains(*selected))
        return std::nullopt;
    for (auto node = *selected; node != ObjectId{};) {
        if (std::ranges::find(targets, node) != targets.end()) {
            const auto existing = environment();
            ATELIER_TRY_BIND(owner, admit_property_edit(existing));
            ATELIER_TRY_BIND(field, runtime_.find_field(presentation_types_.environment_component, "main_camera"));
            ATELIER_TRY_BIND(before, runtime_.read_reference(existing, *field));
            ATELIER_TRY_BIND(after, runtime_.null_reference(types_.entity, ValueForm::weak_reference));
            ATELIER_TRY_BIND(held, runtime_.hold(existing));
            return ReferenceFieldChange{std::move(*held), *owner, {*field}, *before, *after};
        }
        ATELIER_TRY_BIND(parent, world::parent_entity(runtime_, node));
        node = *parent;
    }
    return std::nullopt;
}

Result<Session::EnvironmentEdit> Session::environment_edit(const rendering::SceneLightingSettings& settings) const {
    ATELIER_TRY_BIND(current, presentation_->environment());
    renderer::ViewSettings before;
    renderer::ViewSettings after;
    static_cast<rendering::SceneLightingSettings&>(before.lit) = *current;
    static_cast<rendering::SceneLightingSettings&>(after.lit) = settings;
    ATELIER_TRY_BIND(fields, detail::author_fields(world_.runtime(), presentation_types_.environment_component));
    EnvironmentEdit edit;
    for (const auto& option : view_options()) {
        if (!world_option(option.option))
            continue;
        const auto value = view_value(after, option.option);
        if (value == view_value(before, option.option))
            continue;
        const auto names = environment_field(option.name);
        const auto field = std::ranges::find(*fields, names, &detail::ScalarField::names);
        if (field == fields->end())
            return invalid("World setting " + std::string(option.name) + " has no Environment field.");
        // The view option kind matches the field's Scalar type.
        Scalar leaf;
        switch (option.kind) {
        case ViewValueKind::boolean:
            leaf = value != 0;
            break;
        case ViewValueKind::enumeration:
            leaf = EnumValue{field->type, static_cast<std::uint32_t>(value)};
            break;
        case ViewValueKind::integer:
            leaf = static_cast<std::uint32_t>(value);
            break;
        case ViewValueKind::number:
            leaf = static_cast<float>(value);
            break;
        }
        edit.paths.push_back(field->path);
        edit.values.push_back(leaf);
    }
    return edit;
}

Result<ObjectId> Session::admit_environment_edit(ObjectId existing) const {
    // The World Settings show a disabled Environment's defaults, which an edit would silently diff against.
    ATELIER_TRY_BIND(status, world::inspect_component(world_.runtime(), existing));
    ATELIER_TRY_BIND(owner, world::effective_enabled(world_.runtime(), status->owner));
    if (!status->enabled || !*owner)
        return invalid("The Environment is disabled; enable it in Parameters before changing World settings.");
    return admit_property_edit(existing);
}

Result<void> Session::set_environment(const rendering::SceneLightingSettings& settings) {
    ATELIER_TRY_BIND(edit, environment_edit(settings));
    if (edit->paths.empty())
        return {};
    if (editing())
        return invalid("Finish the current property edit before changing World settings.");
    if (const auto existing = environment(); existing != ObjectId{}) {
        ATELIER_TRY_BIND(entity, admit_environment_edit(existing));
        ATELIER_TRY_BIND(root, runtime_.hold(existing));
        FieldGroupChange change{std::move(*root), *entity, edit->paths, 0, {}, edit->values};
        for (const auto& path : edit->paths) {
            ATELIER_TRY_BIND(value, world_.read(existing, path));
            change.before.push_back(*value);
        }
        ATELIER_TRY(world_.write(existing, edit->paths, edit->values));
        remember(std::move(change));
        return {};
    }
    // The first edit creates the Environment with its values: one history item, and
    // Play receives the complete component in one add.
    ATELIER_TRY_BIND(created, create_environment(edit->paths, edit->values));
    return publish_environment(std::move(*created));
}

Result<void> Session::set_sky_map(const assets::SkyAsset& asset) {
    if (editing())
        return invalid("Finish the current property edit before changing World settings.");
    presentation::SkyMapBinding after{asset.key, asset.map};
    if (const auto existing = environment(); existing != ObjectId{}) {
        ATELIER_TRY_BIND(entity, admit_environment_edit(existing));
        ATELIER_TRY_BIND(before, presentation::read_sky_map(runtime_, existing));
        if (before->source == after.source && before->resolved == after.resolved)
            return {};
        ATELIER_TRY_BIND(root, runtime_.hold(existing));
        ATELIER_TRY(presentation::set_sky_map_binding(runtime_, existing, after));
        remember(SkyMapChange{std::move(*root), *entity, std::move(*before), std::move(after)});
        return {};
    }
    if (after.source.empty() && !after.resolved)
        return {};
    ATELIER_TRY_BIND(created, create_environment({}, {}, &after));
    return publish_environment(std::move(*created));
}

Result<void> Session::publish_environment(ObjectRoot root) {
    EntityChange change;
    change.added = true;
    change.removals.emplace_back().root = std::move(root);
    const auto id = change.removals.front().id();
    names_.emplace(id, L"Environment");
    auto attached = world_.attach(id);
    if (!attached) {
        names_.erase(id);
        return std::unexpected(attached.error());
    }
    remember(std::move(change));
    if (attached->notification_error)
        return std::unexpected(*attached->notification_error);
    return {};
}

Result<ObjectRoot> Session::create_environment(std::span<const world::FieldPath> paths, std::span<const Scalar> values,
                                               const presentation::SkyMapBinding* sky) {
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    ATELIER_TRY_BIND(entity, world::create_entity(runtime_));
    ATELIER_TRY_BIND(component, world::create_component(runtime_, presentation_types_.environment_component));
    if (!paths.empty())
        ATELIER_TRY(world_.write(component->id(), paths, values));
    if (sky)
        ATELIER_TRY(presentation::set_sky_map_binding(runtime_, component->id(), *sky));
    const std::array parts{world::Attachment{component->id()}};
    ATELIER_TRY_BIND(composed, world_.attach_components(entity->id(), parts));
    return std::move(*entity);
}

Result<void> Session::rename(ObjectId entity, std::wstring name) {
    ATELIER_TRY(validate_name(name));
    ATELIER_TRY(admit_entity_edit(entity));
    const auto at = names_.find(entity);
    if (at == names_.end())
        return invalid("The Entity does not belong to this document.");
    if (at->second == name)
        return {};
    ATELIER_TRY_BIND(root, runtime_.hold(entity));
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    NameChange change{std::move(*root), at->second, name};
    at->second = std::move(name);
    remember(std::move(change));
    return {};
}

void Session::set_selection(Selection next) {
    if (selection_ != next || selected_entities_.size() != (next.entity == ObjectId{} ? 0u : 1u))
        ++revision_;
    selection_ = next;
    selected_entities_.clear();
    if (next.entity != ObjectId{})
        selected_entities_.push_back(next.entity);
}

void Session::prune_selection() noexcept {
    const auto members = world_.members();
    const auto contains = [&](ObjectId entity) {
        return members && std::ranges::any_of(*members, [&](const auto& member) { return member.id() == entity; });
    };
    const auto old_size = selected_entities_.size();
    std::erase_if(selected_entities_, [&](ObjectId id) { return !contains(id); });
    const bool lost_active = selection_.entity != ObjectId{} && !contains(selection_.entity);
    if (old_size != selected_entities_.size() || lost_active)
        ++revision_;
    if (lost_active) {
        selection_ = {};
        if (!selected_entities_.empty())
            selection_ = {selected_entities_.back(), selected_entities_.back()};
    }
}

Result<void> Session::select(ObjectId object) {
    Selection next;
    if (object != ObjectId{}) {
        ATELIER_TRY_BIND(entity, admit_property_edit(object));
        next = {*entity, object};
    }
    if (next == selection_ && selected_entities_.size() == (next.entity == ObjectId{} ? 0u : 1u))
        return {};
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    set_selection(next);
    return {};
}

Result<void> Session::select_entities(std::span<const ObjectId> entities, ObjectId active) {
    std::vector<ObjectId> next;
    next.reserve(entities.size());
    for (const auto entity : entities) {
        if (std::ranges::find(next, entity) != next.end())
            continue;
        ATELIER_TRY(admit_entity_edit(entity));
        next.push_back(entity);
    }
    if (active == ObjectId{} || std::ranges::find(next, active) == next.end())
        active = next.empty() ? ObjectId{} : next.back();
    else {
        std::erase(next, active);
        next.push_back(active);
    }
    if (next == selected_entities_ && selection_ == Selection{active, active})
        return {};
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    selected_entities_ = std::move(next);
    selection_ = {active, active};
    ++revision_;
    return {};
}

Result<world::Commit> Session::add_entity(TypeId actual) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    ATELIER_TRY_BIND(root, world::create_entity(runtime_, actual == TypeId{} ? types_.entity : actual));
    return attach_created(std::move(*root), L"Entity");
}

Result<void> Session::set_transform(ObjectId entity, world::Trs value) {
    ATELIER_TRY(admit_entity_edit(entity));
    ATELIER_TRY_BIND(canonical, world::canonical_transform(value));
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(spatial, world::has_transform(runtime_, entity));
    std::optional<world::Trs> before;
    if (*spatial) {
        ATELIER_TRY_BIND(transform, world::read_transform(runtime_, entity));
        before = *transform;
    }
    ATELIER_TRY_BIND(before_origin, world::space_reference(runtime_, entity));
    if (before && *before == *canonical)
        return {};
    ATELIER_TRY_BIND(root, runtime_.hold(entity));
    ATELIER_TRY(world_.set_transform(entity, *canonical));
    ATELIER_TRY_BIND(after_origin, world::space_reference(runtime_, entity));
    remember(TransformPresenceChange{std::move(*root), before, *canonical, *before_origin, *after_origin});
    return {};
}

Result<void> Session::remove_transform(ObjectId entity) {
    ATELIER_TRY(admit_entity_edit(entity));
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(spatial, world::has_transform(runtime_, entity));
    if (!*spatial)
        return {};
    ATELIER_TRY_BIND(before, world::read_transform(runtime_, entity));
    ATELIER_TRY_BIND(before_origin, world::space_reference(runtime_, entity));
    ATELIER_TRY_BIND(root, runtime_.hold(entity));
    ATELIER_TRY(world_.remove_transform(entity));
    ATELIER_TRY_BIND(after_origin, world::space_reference(runtime_, entity));
    remember(TransformPresenceChange{std::move(*root), *before, {}, *before_origin, *after_origin});
    return {};
}

Result<world::Commit> Session::set_space_reference(ObjectId entity, std::optional<ObjectId> origin,
                                                   world::ReparentRule rule) {
    ATELIER_TRY(admit_entity_edit(entity));
    if (origin && *origin != ObjectId{})
        ATELIER_TRY(admit_entity_edit(*origin));
    ATELIER_TRY(cancel_edit());
    SpaceReferenceChange change;
    ATELIER_TRY_BIND(root, runtime_.hold(entity));
    change.root = std::move(*root);
    ATELIER_TRY_BIND(before_origin, world::space_reference(runtime_, entity));
    change.before_origin = *before_origin;
    ATELIER_TRY_BIND(spatial, world::has_transform(runtime_, entity));
    change.spatial = *spatial;
    if (change.spatial) {
        ATELIER_TRY_BIND(before, world::read_transform(runtime_, entity));
        change.before = *before;
    }
    ATELIER_TRY_BIND(changed, world_.set_space_reference(entity, origin, rule));
    if (changed->changed) {
        ATELIER_TRY_BIND(after_origin, world::space_reference(runtime_, entity));
        change.after_origin = *after_origin;
        if (change.spatial) {
            ATELIER_TRY_BIND(after, world::read_transform(runtime_, entity));
            change.after = *after;
        }
        set_selection({entity, entity});
        remember(std::move(change));
    }
    return changed;
}

Result<world::Commit> Session::reparent(ObjectId entity, ObjectId parent, world::ReparentRule rule, std::size_t index) {
    return reparent(std::span{&entity, 1}, parent, rule, index);
}

Result<world::Commit> Session::reparent(std::span<const ObjectId> entities, ObjectId parent, world::ReparentRule rule,
                                        std::size_t index) {
    // The input may be selected_entities(), which publication updates below.
    const std::vector<ObjectId> targets(entities.begin(), entities.end());
    if (parent != ObjectId{})
        ATELIER_TRY(admit_entity_edit(parent));
    const auto sibling_index = [&](ObjectId target, ObjectId owner) -> Result<std::size_t> {
        std::size_t result{};
        if (owner != ObjectId{}) {
            ATELIER_TRY_BIND(children, world::children(runtime_, owner));
            for (const auto& child : *children) {
                if (child.id() == target)
                    return result;
                ++result;
            }
        } else {
            ATELIER_TRY_BIND(members, world_.members());
            for (const auto& member : *members) {
                ATELIER_TRY_BIND(parent, world::parent_entity(runtime_, member.id()));
                if (*parent == ObjectId{}) {
                    if (member.id() == target)
                        return result;
                    ++result;
                }
            }
        }
        return result;
    };
    ReparentChange change;
    change.moves.reserve(targets.size());
    const auto publish = [&]() -> Result<world::Commit> {
        for (const auto entity : targets) {
            ATELIER_TRY(admit_entity_edit(entity));
            ATELIER_TRY(cancel_edit());
            ReparentChange::Move move;
            ATELIER_TRY_BIND(root, runtime_.hold(entity));
            move.root = std::move(*root);
            ATELIER_TRY_BIND(before_parent, world::parent_entity(runtime_, entity));
            move.before_parent = *before_parent;
            ATELIER_TRY_BIND(before_index, sibling_index(entity, move.before_parent));
            move.before_index = *before_index;
            std::vector<ObjectId> pending{entity};
            while (!pending.empty()) {
                const auto object = pending.back();
                pending.pop_back();
                ATELIER_TRY_BIND(spatial, world::has_transform(runtime_, object));
                if (*spatial) {
                    ATELIER_TRY_BIND(before, world::read_transform(runtime_, object));
                    ATELIER_TRY_BIND(held, runtime_.hold(object));
                    move.transforms.push_back({std::move(*held), *before, *before});
                }
                if (rule == world::ReparentRule::keep_world) {
                    ATELIER_TRY_BIND(children, world::children(runtime_, object));
                    for (const auto& child : *children)
                        pending.push_back(child.id());
                }
            }
            ATELIER_TRY_BIND(changed, world_.reparent(entity, parent, rule, index));
            if (changed->changed) {
                ATELIER_TRY_BIND(after_parent, world::parent_entity(runtime_, entity));
                move.after_parent = *after_parent;
                ATELIER_TRY_BIND(after_index, sibling_index(entity, move.after_parent));
                move.after_index = *after_index;
                for (auto& transform : move.transforms) {
                    ATELIER_TRY_BIND(after, world::read_transform(runtime_, transform.root.id()));
                    transform.after = *after;
                }
                // Live forwarding needs this move's root pose, not the final pose after later moves.
                std::erase_if(move.transforms, [entity](const auto& transform) {
                    return transform.root.id() != entity && transform.before == transform.after;
                });
                set_selection({entity, entity});
                change.moves.push_back(std::move(move));
            }
            if (changed->notification_error)
                return world::Commit{!change.moves.empty(), std::move(changed->notification_error)};
            if (index != world::append)
                ++index;
        }
        return world::Commit{!change.moves.empty(), {}};
    };
    const auto changed = publish();
    // World has already published these moves, even when a later move was rejected.
    if (!change.moves.empty())
        remember(std::move(change));
    return changed;
}

Result<world::Commit> Session::create_static_mesh(const assets::MeshAsset& asset, math::Vector3 position,
                                                  Project* project) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    std::vector<presentation::MaterialBinding> defaults;
    if (project) {
        ATELIER_TRY_BIND(resolved, mesh_defaults(project->assets(), asset));
        defaults = std::move(*resolved);
    }
    ATELIER_TRY_BIND(root, presentation::create_static_mesh_entity(
                               world_, {asset.mesh, {position, {}, {1.f, 1.f, 1.f}}, asset.key, std::move(defaults)}));
    return attach_created(std::move(*root), asset.name);
}

Result<std::pair<std::string, std::wstring>> Session::resolve_procedural_graph_asset(std::string_view graph_key,
                                                                                     Project* project) const {
    if (is_scratch_graph(graph_key))
        return std::unexpected(
            Error{ErrorCode::invalid_definition,
                  "Save the scratch procedural graph as a Project asset before binding it to a Scene Entity."});
    std::string resolved_key(graph_key);
    std::wstring label = L"Procedural Generator";
    if (!graph_key.empty() && project && !has_graph(graph_key)) {
        ATELIER_TRY_BIND(asset, project->assets().procedural_graph(graph_key));
        resolved_key = asset->key;
        if (!asset->name.empty())
            label = asset->name;
    } else if (!graph_key.empty() && project) {
        if (const auto* entry = project->assets().entry(graph_key)) {
            resolved_key = entry->key;
            if (const auto stem = entry->path.stem().wstring(); !stem.empty())
                label = stem;
        }
    }
    return std::pair{std::move(resolved_key), std::move(label)};
}

Result<world::Commit> Session::create_procedural_generator(std::string_view graph_key, math::Vector3 position,
                                                           Project* project) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    ATELIER_TRY_BIND(resolved, resolve_procedural_graph_asset(graph_key, project));
    ATELIER_TRY_BIND(root, presentation::create_procedural_generator_entity(
                               world_, {{position, {}, {1.f, 1.f, 1.f}}, resolved->first}));
    ATELIER_TRY_BIND(attached, attach_created(std::move(*root), resolved->second));
    if (attached->changed)
        ATELIER_TRY(sync_procedural_generators(project));
    return std::move(*attached);
}

Result<void> Session::set_procedural_graph(Project* project, ObjectId component, std::string_view graph_key) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(state, admit_component_edit(component));
    ATELIER_TRY_BIND(resolved, resolve_procedural_graph_asset(graph_key, project));
    ATELIER_TRY_BIND(current, presentation::read_procedural_source(runtime_, component));
    if (current->source == resolved->first)
        return {};
    ATELIER_TRY_BIND(root, runtime_.hold(component));
    auto before = std::move(current->source);
    ATELIER_TRY(presentation::set_procedural_binding(runtime_, component, {resolved->first, nullptr, {}}));
    remember(ProceduralGraphChange{std::move(*root), state->owner, std::move(before), std::move(resolved->first)});
    return sync_procedural_generators(project);
}

Result<world::Commit> Session::create_light(presentation::LightKind kind, math::Vector3 position) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    ATELIER_TRY_BIND(entity, world::create_entity(runtime_));
    ATELIER_TRY_BIND(light, world::create_component(runtime_, presentation_types_.light_component));
    auto configured = runtime_.with_native<presentation::LightComponent>(light->id(), [&](auto& value) -> Result<void> {
        value.settings.kind = kind;
        return presentation::validate_light(runtime_, value.settings);
    });
    if (!configured)
        return std::unexpected(configured.error());
    ATELIER_TRY_BIND(positioned, world_.set_transform(entity->id(), {position, {}, {1, 1, 1}}));
    const std::array parts{world::Attachment{light->id()}};
    ATELIER_TRY_BIND(attached, world_.attach_components(entity->id(), parts));
    if (attached->notification_error)
        return std::unexpected(*attached->notification_error);
    const auto label = kind == presentation::LightKind::point       ? L"Point Light"
                       : kind == presentation::LightKind::rectangle ? L"Rectangle Light"
                                                                    : L"Directional Light";
    return attach_created(std::move(*entity), label);
}

Result<world::Commit> Session::create_camera(math::Vector3 position) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() >= maximum_entities)
        return invalid("Editor Entity limit reached.");
    ATELIER_TRY_BIND(entity, world::create_entity(runtime_));
    ATELIER_TRY_BIND(camera, world::create_component(runtime_, presentation_types_.camera_component));
    ATELIER_TRY(world_.set_transform(entity->id(), {position, {}, {1, 1, 1}}));
    ATELIER_TRY_BIND(composed, world_.attach_components(entity->id(), std::array{world::Attachment{camera->id()}}));
    return attach_created(std::move(*entity), L"Camera");
}

Result<world::Commit> Session::attach_created(ObjectRoot root, std::wstring_view label) {
    EntityChange change;
    change.added = true;
    change.removals.emplace_back().root = std::move(root);
    const auto id = change.removals.front().id();
    names_.emplace(id, std::wstring(label) + L" " + std::to_wstring(next_name_++));
    auto attached = world_.attach(id);
    if (!attached) {
        names_.erase(id);
        return attached;
    }
    if (attached->changed) {
        set_selection({id, id});
        remember(std::move(change));
    }
    return attached;
}

Result<world::Commit> Session::erase_entities(std::span<const ObjectId> entities) {
    if (entities.empty())
        return world::Commit{};
    const std::vector<ObjectId> requested(entities.begin(), entities.end());
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    std::vector<ObjectId> targets;
    for (const auto entity : requested) {
        ATELIER_TRY(admit_entity_edit(entity));
        ATELIER_TRY_BIND(parent, world::parent_entity(runtime_, entity));
        bool included{};
        while (*parent != ObjectId{}) {
            if (std::ranges::find(requested, *parent) != requested.end()) {
                included = true;
                break;
            }
            ATELIER_TRY(parent = world::parent_entity(runtime_, *parent));
        }
        if (!included)
            targets.push_back(entity);
    }
    ATELIER_TRY_BIND(clear, main_camera_removal(targets));
    if (*clear)
        ATELIER_TRY(apply_reference(**clear, true));
    auto removed = world_.detach_batch(targets);
    if (!removed) {
        if (*clear)
            if (auto restored = apply_reference(**clear, false); !restored)
                return std::unexpected(restored.error());
        return std::unexpected(removed.error());
    }
    EntityChange change;
    change.cleared_camera = std::move(*clear);
    change.removals = std::move(*removed);
    world::Commit result;
    for (auto& removal : change.removals) {
        result.changed |= removal.commit.changed;
        if (removal.commit.notification_error && !result.notification_error)
            result.notification_error = std::move(removal.commit.notification_error);
        removal.commit.notification_error.reset();
    }
    if (result.changed) {
        remember(std::move(change));
        prune_selection();
    }
    return result;
}

Result<world::Commit> Session::add_component(ObjectId entity, TypeId type) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY(admit_entity_edit(entity));
    ATELIER_TRY_BIND(supported, runtime_.is_a(type, types_.component));
    if (!*supported)
        return invalid("Adding a component requires a Component type.");
    const auto members = world::components(runtime_, entity);
    if (!members)
        return std::unexpected(members.error());
    if (members->size() >= maximum_components)
        return invalid("Editor Component limit reached.");
    ATELIER_TRY_BIND(root, world::create_component(runtime_, type));
    ComponentChange change;
    change.added = true;
    change.removal.root = std::move(*root);
    change.removal.owner = entity;
    const auto spatial = world::component_requires_space(runtime_, change.removal.id());
    if (!spatial)
        return std::unexpected(spatial.error());
    ATELIER_TRY_BIND(has_space, world::has_transform(runtime_, entity));
    if (*spatial && !*has_space) {
        ATELIER_TRY_BIND(positioned, world_.set_transform(entity, {}));
        change.created_transform = true;
    }
    const std::array attachments{world::Attachment{change.removal.id()}};
    auto attached = world_.attach_components(entity, attachments);
    if (!attached) {
        if (change.created_transform)
            (void)world_.remove_transform(entity);
        return attached;
    }
    if (attached->changed) {
        set_selection({entity, change.removal.id()});
        remember(std::move(change));
    }
    return attached;
}

Result<world::Commit> Session::remove_component(ObjectId component) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY(admit_component_edit(component));
    ATELIER_TRY_BIND(removed, world_.remove_component(component));
    auto result = std::move(removed->commit);
    if (result.changed) {
        if (selection_.object == component)
            set_selection({removed->owner, removed->owner});
        remember(ComponentChange{false, std::move(*removed)});
    }
    return result;
}

Result<ObjectId> Session::admit_property_edit(ObjectId object) const {
    if (world_.running() || world_.closed())
        return invalid("Properties require an open, stopped document.");
    if (is_graph_object(object))
        return object;
    ATELIER_TRY_BIND(type, runtime_.type_of(object));
    ATELIER_TRY_BIND(entity, runtime_.is_a(*type, types_.entity));
    if (*entity) {
        ATELIER_TRY(admit_entity_edit(object));
        return object;
    }
    ATELIER_TRY_BIND(component, admit_component_edit(object));
    return component->owner;
}

Result<std::vector<ComponentType>> Session::component_types() const {
    std::vector<ComponentType> result;
    for (const auto type : runtime_.types()) {
        ATELIER_TRY_BIND(info, runtime_.describe(type));
        if ((*info)->kind != TypeKind::object)
            continue;
        ATELIER_TRY_BIND(matches, runtime_.is_a(type, types_.component));
        if (*matches)
            result.push_back({type, std::string((*info)->name)});
    }
    return result;
}

Result<std::vector<Property>> Session::properties(ObjectId object) const {
    ATELIER_TRY(admit_property_edit(object));
    ATELIER_TRY_BIND(type, runtime_.type_of(object));
    const bool graph_object = is_graph_object(object);
    ATELIER_TRY_BIND(fields, graph_object ? detail::author_fields(world_.runtime(), *type, true)
                                          : detail::scalar_fields(world_.runtime(), *type));
    std::vector<Property> result;
    ATELIER_TRY_BIND(entity, runtime_.is_a(*type, types_.entity));
    bool spatial{};
    if (*entity) {
        ATELIER_TRY_BIND(has_space, world::has_transform(world_.runtime(), object));
        spatial = *has_space;
    }
    for (auto& field : *fields) {
        if (field.form != ValueForm::inline_value || field.kind == TypeKind::array)
            continue;
        if (*entity && !spatial && detail::transform_path(types_, field.path))
            continue;
        if (field.kind == TypeKind::string) {
            String value;
            ATELIER_TRY(runtime_.read(object, field.path, ValueOutput{field.type, &value}));
            Property prop{detail::dotted(field.names),
                          std::move(field.path),
                          field.type,
                          Scalar{0.f},
                          std::move(field.label),
                          std::move(field.category),
                          std::move(field.description),
                          field.range,
                          field.transient,
                          field.read_only,
                          true,
                          std::string(value.view().std_view()),
                          {}};
            result.push_back(std::move(prop));
            continue;
        }
        ATELIER_TRY_BIND(value, read_property(object, field.path));
        result.push_back({detail::dotted(field.names), std::move(field.path), field.type, std::move(*value),
                          std::move(field.label), std::move(field.category), std::move(field.description), field.range,
                          field.transient, field.read_only});
    }
    return result;
}

Result<Scalar> Session::read_property(ObjectId object, std::span<const FieldId> path) const {
    ATELIER_TRY(admit_property_edit(object));
    return is_graph_object(object) ? runtime_.read(object, path) : world_.read(object, path);
}

Result<void> Session::configure_body(ObjectId component, physics::BodySettings settings) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(state, admit_component_edit(component));
    ATELIER_TRY_BIND(physical, runtime_.is_a(state->type, types_.primitive_component));
    if (!*physical)
        return invalid("Selected Component is not a physics body.");
    ATELIER_TRY_BIND(before, physics::read_settings(runtime_, component));
    if (*before == settings)
        return {};
    ATELIER_TRY_BIND(held, runtime_.hold(component));
    ATELIER_TRY(physics::configure(runtime_, component, settings));
    remember(BodyChange{std::move(*held), state->owner, std::move(*before), std::move(settings)});
    return {};
}

Result<void> Session::begin_edit(ObjectId object, std::span<const FieldId> path, std::uint64_t control_session) {
    if (editing())
        return invalid("A field edit is already active.");
    if (!control_session)
        return invalid("A field edit requires a control session.");
    ATELIER_TRY_BIND(entity, admit_property_edit(object));
    ATELIER_TRY_BIND(read_only, read_only_path(runtime_, object, path));
    if (*read_only)
        return invalid("Read-only fields cannot be edited in the inspector.");
    const bool graph_object = is_graph_object(object);
    ATELIER_TRY_BIND(before, graph_object ? runtime_.read(object, path) : world_.read(object, path));
    ATELIER_TRY_BIND(root, runtime_.hold(object));
    std::variant<world::Trs, Scalar> value{*before};
    if (!graph_object && detail::transform_path(types_, path)) {
        ATELIER_TRY_BIND(transform, world::read_transform(runtime_, object));
        value = *transform;
    }
    active_.emplace(FieldChange{std::move(*root), *entity, {path.begin(), path.end()}, control_session, value, value});
    return {};
}

Result<void> Session::begin_group_edit(ObjectId object, std::span<const world::FieldPath> paths,
                                       std::uint64_t control_session) {
    if (editing())
        return invalid("A field edit is already active.");
    if (!control_session || paths.empty())
        return invalid("A group edit requires a control session and fields.");
    for (std::size_t i = 1; i < paths.size(); ++i)
        if (std::ranges::find(paths.begin(), paths.begin() + static_cast<std::ptrdiff_t>(i), paths[i]) !=
            paths.begin() + static_cast<std::ptrdiff_t>(i))
            return invalid("A group edit names the same field more than once.");
    ATELIER_TRY_BIND(entity, admit_property_edit(object));
    const bool graph_object = is_graph_object(object);
    ATELIER_TRY_BIND(root, runtime_.hold(object));
    FieldGroupChange change{std::move(*root), *entity, {paths.begin(), paths.end()}, control_session, {}, {}};
    for (const auto& path : paths) {
        if (!graph_object && detail::transform_path(types_, path))
            return invalid("Transform fields use the transform edit.");
        ATELIER_TRY_BIND(read_only, read_only_path(runtime_, object, path));
        if (*read_only)
            return invalid("Read-only fields cannot be edited in the inspector.");
        ATELIER_TRY_BIND(value, graph_object ? runtime_.read(object, path) : world_.read(object, path));
        change.before.push_back(*value);
    }
    change.after = change.before;
    group_.emplace(std::move(change));
    return {};
}

Result<void> Session::preview_group_edit(std::uint64_t control_session, std::span<const Scalar> values) {
    if (!group_ || group_->control_session != control_session)
        return invalid("No matching group edit.");
    if (values.size() != group_->paths.size())
        return invalid("A group edit needs one value per field.");
    ATELIER_TRY_BIND(entity, admit_property_edit(group_->root.id()));
    if (*entity != group_->entity)
        return invalid("The property's owning Entity changed.");
    ATELIER_TRY(write_group(group_->root.id(), group_->paths, values));
    ++revision_;
    if (!is_graph_object(group_->root.id()))
        forward_edit();
    return {};
}

Result<void> Session::write_group(ObjectId object, std::span<const world::FieldPath> paths,
                                  std::span<const Scalar> values) {
    if (paths.empty() || values.size() != paths.size())
        return invalid("A group edit needs one value per field.");
    std::vector<Scalar> previous;
    previous.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        ATELIER_TRY(validate_property_scalar(runtime_, values[i]));
        ATELIER_TRY_BIND(value, read_property(object, paths[i]));
        if (value->index() != values[i].index() ||
            (std::holds_alternative<EnumValue>(*value) &&
             std::get<EnumValue>(*value).type != std::get<EnumValue>(values[i]).type))
            return invalid("A group edit value changed its field type.");
        previous.push_back(*value);
    }
    if (!is_graph_object(object))
        return world_.write(object, paths, values);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (auto written = runtime_.write(object, paths[i], values[i]); !written) {
            for (std::size_t j = 0; j < i; ++j)
                (void)runtime_.write(object, paths[j], previous[j]);
            return written;
        }
    }
    return {};
}

Result<void> Session::apply_group(const FieldGroupChange& change, bool forward) {
    ATELIER_TRY_BIND(entity, admit_property_edit(change.root.id()));
    if (*entity != change.entity)
        return invalid("The property's owning Entity changed.");
    const auto& values = forward ? change.after : change.before;
    return write_group(change.root.id(), change.paths, values);
}

Result<void> Session::set_properties(ObjectId object, std::span<const PropertyPatch> patch) {
    ATELIER_TRY_BIND(entity, admit_property_edit(object));
    const bool graph_object = is_graph_object(object);
    for (const auto& leaf : patch) {
        if (!graph_object && object == *entity && !leaf.path.empty() &&
            detail::transform_field(types_, leaf.path.front()))
            return invalid("Transform fields require scene.transform_set/clear/remove.");
        ATELIER_TRY_BIND(read_only, read_only_path(runtime_, object, leaf.path));
        if (*read_only)
            return invalid("Read-only fields cannot be edited in the inspector.");
        ATELIER_TRY(validate_property_scalar(runtime_, leaf.value));
    }
    if (patch.empty())
        return {};
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(root, runtime_.hold(object));
    FieldGroupChange change{std::move(*root), *entity, {}, 0, {}, {}};
    for (const auto& leaf : patch) {
        ATELIER_TRY_BIND(before, read_property(object, leaf.path));
        change.paths.push_back(leaf.path);
        change.before.push_back(*before);
        change.after.push_back(leaf.value);
    }
    if (!graph_object && change.before == change.after)
        return {};
    ATELIER_TRY(write_group(object, change.paths, change.after));
    if (graph_object)
        ATELIER_TRY(remember_graph_scalar_edit(std::move(change.root), std::move(change.paths),
                                               std::move(change.before), std::move(change.after)));
    else
        remember(std::move(change));
    return {};
}

bool Session::owns_edit(std::uint64_t control_session) const noexcept {
    return (group_ && group_->control_session == control_session) ||
           (active_ && active_->control_session == control_session) ||
           (transform_ && transform_->control_session == control_session) || material_editing(control_session);
}

Result<void> Session::begin_transform(std::span<const ObjectId> entities, std::uint64_t control_session,
                                      bool amend_creation) {
    if (editing())
        return invalid("A field edit is already active.");
    if (!control_session)
        return invalid("A transform edit requires a control session.");
    if (entities.empty())
        return invalid("Select an Entity with a Transform.");
    TransformChange change{{}, control_session, selection_};
    change.entries.reserve(entities.size());
    for (const auto entity : entities) {
        if (std::ranges::any_of(change.entries, [&](const auto& entry) { return entry.root.id() == entity; }))
            return invalid("A transform edit names an Entity twice.");
        ATELIER_TRY_BIND(state, admit_entity_edit(entity));
        ATELIER_TRY_BIND(value, world::read_transform(runtime_, entity));
        ATELIER_TRY_BIND(root, runtime_.hold(entity));
        change.entries.push_back({std::move(*root), entity, *value, *value});
    }
    if (amend_creation) {
        const auto* creation =
            cursor_ && cursor_ == history_.size() ? std::get_if<EntityChange>(&history_.back().change) : nullptr;
        if (!creation || !creation->added || saved_cursor_ == cursor_ ||
            std::ranges::any_of(change.entries, [&](const auto& entry) {
                return std::ranges::none_of(creation->removals,
                                            [&](const auto& item) { return item.id() == entry.entity; });
            }))
            return invalid("Initial placement must belong to the current unsaved creation.");
        change.amend_creation = true;
    }
    transform_ = std::move(change);
    return {};
}

Result<world::Trs> Session::original_transform(std::uint64_t control_session, ObjectId component) const {
    if (!transform_ || transform_->control_session != control_session)
        return invalid("No matching transform edit.");
    for (const auto& entry : transform_->entries)
        if (entry.root.id() == component)
            return entry.before;
    return invalid("The Entity does not belong to this transform edit.");
}

Result<void> Session::preview_transform(std::uint64_t control_session, std::span<const world::Trs> values) {
    if (!transform_ || transform_->control_session != control_session)
        return invalid("No matching transform edit.");
    for (const auto& value : values)
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (value.position[axis] < parameters_[axis].minimum || value.position[axis] > parameters_[axis].maximum ||
                std::abs(value.scale[axis]) > maximum_scale)
                return invalid("Transform value is outside the editor range.");
    auto written = write_transforms(*transform_, values);
    if (written)
        forward_edit();
    return written;
}

Result<void> Session::write_transforms(const TransformChange& change, std::span<const world::Trs> values) {
    if (values.size() != change.entries.size())
        return invalid("Transform preview does not match its targets.");
    std::vector<world::Trs> previous;
    previous.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& entry = change.entries[i];
        const auto canonical = world::canonical_transform(values[i]);
        if (!canonical)
            return std::unexpected(canonical.error());
        ATELIER_TRY_BIND(entity, admit_property_edit(entry.root.id()));
        if (*entity != entry.entity)
            return invalid("The transform's owning Entity changed.");
        ATELIER_TRY_BIND(value, world::read_transform(runtime_, entry.root.id()));
        previous.push_back(*value);
    }
    if (!std::ranges::equal(previous, values))
        ++revision_;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (auto written = world_.set_transform(change.entries[i].root.id(), values[i]); !written) {
            // A refused member cannot leave earlier members at a new preview.
            for (std::size_t j = 0; j < i; ++j) {
                auto restored = world_.set_transform(change.entries[j].root.id(), previous[j]);
                if (!restored)
                    return restored;
            }
            return written;
        }
    }
    return {};
}

Result<void> Session::set_property(ObjectId object, std::span<const FieldId> path, Scalar value,
                                   std::uint64_t control_session) {
    ATELIER_TRY_BIND(begun, begin_edit(object, path, control_session));
    auto changed = preview_edit(control_session, std::move(value));
    if (changed)
        changed = commit_edit(control_session);
    if (changed)
        return {};
    auto restored = cancel_edit();
    return restored ? changed : restored;
}

Result<void> Session::preview_edit(std::uint64_t control_session, float value) {
    return preview_edit(control_session, Scalar{value});
}

Result<void> Session::preview_edit(std::uint64_t control_session, Scalar value) {
    if (!active_ || active_->control_session != control_session)
        return invalid("No matching field edit.");
    ATELIER_TRY_BIND(entity, admit_property_edit(active_->root.id()));
    if (*entity != active_->entity)
        return invalid("The property's owning Entity changed.");
    ATELIER_TRY(validate_property_scalar(runtime_, value));
    const bool graph_object = is_graph_object(active_->root.id());
    if (!graph_object) {
        const auto parameter = std::find_if(parameters_.begin(), parameters_.end(), [&](const auto& item) {
            return std::ranges::equal(item.path, active_->path);
        });
        if (parameter != parameters_.end()) {
            const auto* number = std::get_if<float>(&value);
            if (!number || *number < parameter->minimum || *number > parameter->maximum)
                return invalid("Field value is outside the editor range.");
        }
        for (const auto& path : types_.scale_fields)
            if (std::ranges::equal(path, active_->path)) {
                const auto* number = std::get_if<float>(&value);
                if (!number || std::abs(*number) > maximum_scale)
                    return invalid("Scale is outside the editor range.");
            }
    }
    const auto before = graph_object ? runtime_.read(active_->root.id(), active_->path)
                                     : world_.read(active_->root.id(), active_->path);
    if (!before)
        return std::unexpected(before.error());
    auto result = graph_object ? runtime_.write(active_->root.id(), active_->path, value)
                               : world_.write(active_->root.id(), active_->path, value);
    if (result && *before != value)
        ++revision_;
    if (result && !graph_object)
        forward_edit();
    return result;
}

Result<void> Session::commit_edit(std::uint64_t control_session) {
    if (material_editing(control_session))
        return commit_material_edit(control_session);
    if (!owns_edit(control_session))
        return invalid("No matching field edit.");
    if (group_) {
        const bool graph_object = is_graph_object(group_->root.id());
        for (std::size_t i = 0; i < group_->paths.size(); ++i) {
            ATELIER_TRY_BIND(value, graph_object ? runtime_.read(group_->root.id(), group_->paths[i])
                                                 : world_.read(group_->root.id(), group_->paths[i]));
            group_->after[i] = *value;
        }
        if (graph_object || group_->before != group_->after) {
            if (graph_object)
                ATELIER_TRY(remember_graph_scalar_edit(std::move(group_->root), std::move(group_->paths),
                                                       std::move(group_->before), std::move(group_->after)));
            else
                remember(std::move(*group_));
        }
        group_.reset();
        return {};
    }
    if (transform_) {
        bool changed{};
        for (auto& entry : transform_->entries) {
            ATELIER_TRY_BIND(value, world::read_transform(runtime_, entry.root.id()));
            entry.after = *value;
            changed |= entry.after != entry.before;
        }
        if (changed && !transform_->amend_creation)
            remember(std::move(*transform_));
        transform_.reset();
        live_leaves_.clear();
        return {};
    }
    ATELIER_TRY_BIND(entity, admit_property_edit(active_->root.id()));
    if (*entity != active_->entity)
        return invalid("The property's owning Entity changed.");
    const bool graph_object = is_graph_object(active_->root.id());
    if (std::holds_alternative<world::Trs>(active_->before)) {
        ATELIER_TRY_BIND(value, world::read_transform(runtime_, active_->root.id()));
        active_->after = *value;
    } else {
        ATELIER_TRY_BIND(value, graph_object ? runtime_.read(active_->root.id(), active_->path)
                                             : world_.read(active_->root.id(), active_->path));
        active_->after = *value;
    }
    if (graph_object || active_->before != active_->after) {
        if (graph_object)
            ATELIER_TRY(remember_graph_scalar_edit(std::move(active_->root), {std::move(active_->path)},
                                                   {std::get<Scalar>(active_->before)},
                                                   {std::get<Scalar>(active_->after)}));
        else
            remember(std::move(*active_));
    }
    active_.reset();
    live_leaves_.clear();
    return {};
}

Result<void> Session::apply_field(const FieldChange& change, bool forward) {
    ATELIER_TRY_BIND(entity, admit_property_edit(change.root.id()));
    if (*entity != change.entity)
        return invalid("The property's owning Entity changed.");
    const auto& value = forward ? change.after : change.before;
    if (const auto* transform = std::get_if<world::Trs>(&value))
        return world_.set_transform(change.root.id(), *transform);
    return is_graph_object(change.root.id()) ? runtime_.write(change.root.id(), change.path, std::get<Scalar>(value))
                                             : world_.write(change.root.id(), change.path, std::get<Scalar>(value));
}

Result<void> Session::cancel_edit() {
    ATELIER_TRY(cancel_material_edit());
    if (group_) {
        const bool graph_object = is_graph_object(group_->root.id());
        ATELIER_TRY_BIND(restored, apply_group(*group_, false));
        ++revision_;
        if (!graph_object)
            forward_edit();
        group_.reset();
    }
    if (transform_) {
        // Restore every target even if one fails; report the first failure.
        Result<void> first;
        for (const auto& entry : transform_->entries) {
            const auto current = world::read_transform(runtime_, entry.root.id());
            if (!current) {
                if (first)
                    first = std::unexpected(current.error());
                continue;
            }
            auto restored = world_.set_transform(entry.root.id(), entry.before);
            if (restored && *current != entry.before)
                ++revision_;
            if (!restored && first)
                first = restored;
        }
        if (!first)
            return first;
        forward_edit();
        transform_.reset();
        live_leaves_.clear();
    }
    if (!active_)
        return {};
    const bool graph_object = is_graph_object(active_->root.id());
    bool changed{};
    if (const auto* before = std::get_if<world::Trs>(&active_->before)) {
        const auto current = world::read_transform(runtime_, active_->root.id());
        if (!current)
            return std::unexpected(current.error());
        changed = *current != *before;
    } else {
        const auto current = graph_object ? runtime_.read(active_->root.id(), active_->path)
                                          : world_.read(active_->root.id(), active_->path);
        if (!current)
            return std::unexpected(current.error());
        changed = *current != std::get<Scalar>(active_->before);
    }
    ATELIER_TRY_BIND(restored, apply_field(*active_, false));
    if (changed)
        ++revision_;
    if (!graph_object)
        forward_edit();
    active_.reset();
    live_leaves_.clear();
    return {};
}

Result<void> Session::reset_position(ObjectId component) {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(before, world::read_transform(runtime_, component));
    ATELIER_TRY_BIND(state, admit_entity_edit(component));
    ATELIER_TRY_BIND(root, runtime_.hold(component));
    auto after = *before;
    after.position = {parameters_[0].default_value, parameters_[1].default_value, parameters_[2].default_value};
    if (after == *before)
        return {};
    FieldChange change{std::move(*root), component, {}, 0, *before, after};
    ATELIER_TRY_BIND(applied, world_.set_transform(component, after));
    remember(std::move(change));
    return {};
}

Result<world::Commit> Session::replay(Change& change, bool forward, Project* project) {
    return std::visit(
        [&](auto& item) -> Result<world::Commit> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, NameChange>) {
                ATELIER_TRY(admit_entity_edit(item.root.id()));
                names_.at(item.root.id()) = forward ? item.after : item.before;
                set_selection({item.root.id(), item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, TransformPresenceChange>) {
                ATELIER_TRY(admit_entity_edit(item.root.id()));
                const auto& value = forward ? item.after : item.before;
                ATELIER_TRY_BIND(changed, value ? world_.set_transform(item.root.id(), *value)
                                                : world_.remove_transform(item.root.id()));
                if (value) {
                    ATELIER_TRY_BIND(restored, world_.set_space_reference(
                                                   item.root.id(), forward ? item.after_origin : item.before_origin,
                                                   world::ReparentRule::keep_local));
                }
                set_selection({item.root.id(), item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, SpaceReferenceChange>) {
                ATELIER_TRY(admit_entity_edit(item.root.id()));
                ATELIER_TRY_BIND(changed, world_.set_space_reference(item.root.id(),
                                                                     forward ? item.after_origin : item.before_origin,
                                                                     world::ReparentRule::keep_local));
                if (item.spatial)
                    ATELIER_TRY(world_.set_transform(item.root.id(), forward ? item.after : item.before));
                set_selection({item.root.id(), item.root.id()});
                return world::Commit{true, changed->notification_error};
            } else if constexpr (std::is_same_v<T, ReparentChange>) {
                world::Commit result;
                for (std::size_t i = 0; i < item.moves.size(); ++i) {
                    const auto& move = item.moves[forward ? i : item.moves.size() - 1 - i];
                    ATELIER_TRY(admit_entity_edit(move.root.id()));
                    auto changed = world_.reparent(move.root.id(), forward ? move.after_parent : move.before_parent,
                                                   world::ReparentRule::keep_local,
                                                   forward ? move.after_index : move.before_index);
                    if (!changed)
                        return changed;
                    for (const auto& transform : move.transforms) {
                        ATELIER_TRY_BIND(restored, world_.set_transform(transform.root.id(),
                                                                        forward ? transform.after : transform.before));
                    }
                    result.changed = true;
                    if (!result.notification_error)
                        result.notification_error = std::move(changed->notification_error);
                    set_selection({move.root.id(), move.root.id()});
                }
                return result;
            } else if constexpr (std::is_same_v<T, FieldGroupChange>) {
                ATELIER_TRY_BIND(written, apply_group(item, forward));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, FieldChange>) {
                ATELIER_TRY_BIND(written, apply_field(item, forward));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, ReferenceFieldChange>) {
                ATELIER_TRY_BIND(written, apply_reference(item, forward));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, TransformChange>) {
                std::vector<world::Trs> values;
                values.reserve(item.entries.size());
                for (const auto& entry : item.entries)
                    values.push_back(forward ? entry.after : entry.before);
                ATELIER_TRY_BIND(written, write_transforms(item, values));
                std::vector<ObjectId> entities;
                for (const auto& entry : item.entries) {
                    entities.push_back(entry.entity);
                }
                // Retain the document-history policy: the edited objects become the selection.
                ATELIER_TRY(select_entities(entities, entities.empty() ? ObjectId{} : entities.back()));
                if (item.inspection.entity == selection_.entity) {
                    const auto owner = admit_property_edit(item.inspection.object);
                    if (owner && *owner == selection_.entity)
                        selection_.object = item.inspection.object;
                }
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, MaterialValueChange>) {
                ATELIER_TRY_BIND(admitted, admit_material_edit(item.root.id()));
                ATELIER_TRY_BIND(changed, apply_material_entries(item.root.id(), item.entries, forward, project));
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, MaterialBindingChange>) {
                ATELIER_TRY_BIND(state, admit_component_edit(item.root.id()));
                if (state->owner != item.entity)
                    return invalid("The material binding's owning Entity changed.");
                ATELIER_TRY_BIND(changed, presentation::set_material_bindings(runtime_, item.root.id(),
                                                                              forward ? item.after : item.before));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, SkyMapChange>) {
                ATELIER_TRY_BIND(state, admit_component_edit(item.root.id()));
                if (state->owner != item.entity)
                    return invalid("The sky map's owning Entity changed.");
                ATELIER_TRY_BIND(changed, presentation::set_sky_map_binding(runtime_, item.root.id(),
                                                                            forward ? item.after : item.before));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, ProceduralGraphChange>) {
                ATELIER_TRY_BIND(state, admit_component_edit(item.root.id()));
                if (state->owner != item.entity)
                    return invalid("The procedural generator's owning Entity changed.");
                ATELIER_TRY(presentation::set_procedural_binding(runtime_, item.root.id(),
                                                                 {forward ? item.after : item.before, nullptr, {}}));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, BodyChange>) {
                ATELIER_TRY_BIND(state, admit_component_edit(item.root.id()));
                if (state->owner != item.entity)
                    return invalid("The physics body's owning Entity changed.");
                ATELIER_TRY_BIND(changed,
                                 physics::configure(runtime_, item.root.id(), forward ? item.after : item.before));
                set_selection({item.entity, item.root.id()});
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, RetainedChange>) {
                return world::Commit{true, {}};
            } else if constexpr (std::is_same_v<T, EntityChange>) {
                world::Commit result;
                const auto merge = [&](world::Commit& commit) {
                    result.changed |= commit.changed;
                    if (commit.notification_error && !result.notification_error)
                        result.notification_error = std::move(commit.notification_error);
                    commit.notification_error.reset();
                };
                if (item.added == forward) {
                    ATELIER_TRY_BIND(restored, world_.restore(item.removals));
                    if (item.cleared_camera)
                        ATELIER_TRY(apply_reference(*item.cleared_camera, false));
                    merge(*restored);
                    std::vector<ObjectId> restored_ids;
                    for (const auto& removal : item.removals)
                        restored_ids.push_back(removal.id());
                    if (result.changed) {
                        ATELIER_TRY(
                            select_entities(restored_ids, restored_ids.empty() ? ObjectId{} : restored_ids.back()));
                    }
                    return result;
                }
                std::vector<ObjectId> targets;
                for (const auto& removal : item.removals)
                    targets.push_back(removal.id());
                if (item.cleared_camera)
                    ATELIER_TRY(apply_reference(*item.cleared_camera, true));
                auto removed = world_.detach_batch(targets);
                if (!removed) {
                    if (item.cleared_camera)
                        if (auto restored = apply_reference(*item.cleared_camera, false); !restored)
                            return std::unexpected(restored.error());
                    return std::unexpected(removed.error());
                }
                item.removals = std::move(*removed);
                for (auto& removal : item.removals)
                    merge(removal.commit);
                if (result.changed)
                    prune_selection();
                return result;
            } else {
                const bool attach = item.added == forward;
                if (attach) {
                    if (item.created_transform) {
                        ATELIER_TRY_BIND(added, world_.set_transform(item.removal.owner, {}));
                    }
                    auto restored = world_.restore(item.removal);
                    if (!restored) {
                        if (item.created_transform)
                            (void)world_.remove_transform(item.removal.owner);
                        return restored;
                    }
                    if (restored->changed)
                        set_selection({item.removal.owner, item.removal.id()});
                    return restored;
                }
                ATELIER_TRY_BIND(removed, world_.remove_component(item.removal.id()));
                item.removal = std::move(*removed);
                if (item.created_transform && item.removal.commit.changed) {
                    ATELIER_TRY_BIND(removed_space, world_.remove_transform(item.removal.owner));
                }
                if (item.removal.commit.changed && selection_.object == item.removal.id())
                    set_selection({item.removal.owner, item.removal.owner});
                auto result = std::move(item.removal.commit);
                item.removal.commit.notification_error.reset();
                return result;
            }
        },
        change);
}

Result<world::Commit> Session::undo(Project* project) {
    if (cursor_)
        ++revision_;
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    if (!cursor_)
        return world::Commit{};
    auto result = replay(history_[cursor_ - 1].change, false, project);
    if (result) {
        restore(history_[cursor_ - 1].retained, false);
        --cursor_;
        forward_change(history_[cursor_].change, false);
        ATELIER_TRY(sync_procedural_generators(project));
    }
    return result;
}

Result<world::Commit> Session::redo(Project* project) {
    if (cursor_ < history_.size())
        ++revision_;
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    if (cursor_ == history_.size())
        return world::Commit{};
    auto result = replay(history_[cursor_].change, true, project);
    if (result) {
        restore(history_[cursor_].retained, true);
        ++cursor_;
        forward_change(history_[cursor_ - 1].change, true);
        ATELIER_TRY(sync_procedural_generators(project));
    }
    return result;
}

Result<world::Commit> Session::close() {
    ATELIER_TRY_BIND(cancelled, cancel_edit());
    ATELIER_TRY_BIND(closed, world_.close());
    physics_.reset();
    history_.clear();
    names_.clear();
    retained_.clear();
    retained_document_ = {};
    cursor_ = 0;
    set_selection({});
    materials_.clear();
    close_graphs();
    scene_material_parents_.clear();
    textures_.clear();
    unavailable_.clear();
    engine_program_.reset();
    project_code_ = nullptr;
    prepared_.reset();
    return closed;
}

Result<world::Commit> Session::start() {
    if (editing())
        return invalid("Finish the active edit before starting the World.");
    if (world_.running() || world_.closed())
        return world_.start();
    ATELIER_TRY_BIND(scene, physics::PhysicsScene::create(world_, physics_types_, presentation::read_mesh_source));
    physics_ = std::move(*scene);
    auto started = world_.start();
    if (!world_.running())
        physics_.reset();
    return started;
}

Result<void> Session::update(float amount) {
    auto result = world_.update(amount);
    if (!result)
        (void)stop();
    return result;
}

Result<world::Commit> Session::stop() {
    ATELIER_TRY_BIND(result, world_.stop());
    if (!world_.running())
        physics_.reset();
    return result;
}

Result<void> Session::collect_step() {
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        ATELIER_TRY_BIND(started, runtime_.begin_collection());
    }
    ATELIER_TRY_BIND(progress, runtime_.collect_step({32}));
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        std::erase_if(names_, [&](const auto& item) { return !runtime_.type_of(item.first); });
        std::erase_if(retained_, [&](const auto& item) { return !runtime_.type_of(item.first); });
        std::erase_if(scene_material_parents_, [&](const auto& item) { return !runtime_.type_of(item.first); });
    }
    return {};
}
}
