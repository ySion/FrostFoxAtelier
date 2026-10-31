#include "editor/workspace.hpp"

namespace atelier::editor {
namespace {
constexpr ui::ControlKey choices = 1ULL << 38;
}
ui::ControlKey Workspace::bind_material(ObjectId object, std::string_view path, Binding::Kind kind, ObjectId component,
                                        std::string_view slot, std::size_t index) {
    const auto at = std::ranges::find_if(bindings_, [&](const auto& value) {
        return value.object == object && value.kind == kind && value.component == component && value.slot == slot &&
               value.index == index && value.material_path == path;
    });
    if (at != bindings_.end())
        return at->key;
    const auto key = next_binding_++;
    bindings_.push_back({key, object, {}, kind, component, std::string(slot), index, 0, std::string(path)});
    return key;
}
std::optional<Workspace::MaterialChoice> Workspace::material_choice(ui::ControlKey key) const {
    if (key < choices || key - choices >= material_choices_.size())
        return {};
    return material_choices_[static_cast<std::size_t>(key - choices)];
}
std::vector<ui::MenuItem> Workspace::material_menu(const Binding& target, const Session& session,
                                                   const Project* project) {
    std::vector<ui::MenuItem> result;
    const bool texture = target.kind == Binding::Kind::material_texture;
    const auto add = [&](std::wstring title, MaterialChoice choice, bool selected = false, bool separator = false) {
        const auto key = choices + material_choices_.size();
        material_choices_.push_back(std::move(choice));
        ui::MenuItem item{key, std::move(title), !session.world().running(), selected, {}, L"", separator};
        if (material_choices_.back().kind == MaterialChoice::Kind::assign || texture) {
            item.behavior = ui::MenuBehavior::radio;
            item.radio_group = target.key;
        }
        result.push_back(std::move(item));
    };
    std::string current;
    if (!target.material_path.empty()) {
        auto view = session.inspect_material(target.object);
        if (!view || view->source)
            return result;
        const auto local = std::ranges::find(view->entries, target.material_path, &material::Override::path);
        if (local != view->entries.end())
            add(L"Discard Local Entry", {MaterialChoice::Kind::discard, target.object, {}, {}, target.material_path});
        if (!texture || !project)
            return result;
        const auto declaration =
            std::ranges::find(view->declarations, target.material_path, &MaterialParameterView::path);
        if (declaration == view->declarations.end())
            return result;
        const auto* binding = std::get_if<material::TextureBinding>(&declaration->value);
        if (!binding)
            return result;
        current = binding->asset_key;
        if (local != view->entries.end())
            if (const auto* own = std::get_if<material::TextureBinding>(&local->value))
                current = own->asset_key;
        add(L"None (Explicit Clear)", {MaterialChoice::Kind::texture, target.object, {}, {}, target.material_path},
            current.empty());
    } else {
        auto bindings = presentation::read_material_bindings(session.world().runtime(), target.object);
        if (!bindings)
            return result;
        const auto at = std::ranges::find(*bindings, target.slot, &presentation::MaterialBinding::slot);
        ObjectId shown = at == bindings->end() ? ObjectId{} : at->resource.id();
        if (shown == ObjectId{})
            if (auto mesh = presentation::read_static_mesh(session.world().runtime(), target.object)) {
                const auto fallback =
                    std::ranges::find(mesh->mesh.defaults, target.slot, &presentation::MaterialBinding::slot);
                if (fallback != mesh->mesh.defaults.end())
                    shown = fallback->resource.id();
            }
        add(L"Use Mesh Default", {MaterialChoice::Kind::clear, target.object, target.slot, {}}, at == bindings->end());
        if (shown != ObjectId{}) {
            auto view = session.inspect_material(shown);
            if (view) {
                if (at != bindings->end())
                    current = view->asset_key;
                if (!view->asset_key.empty())
                    add(L"Inspect Shared Material",
                        {MaterialChoice::Kind::edit, target.object, target.slot, view->asset_key});
            }
        }
        add(L"Create Scene Material Instance", {MaterialChoice::Kind::create_scene, target.object, target.slot, {}});
    }
    if (!project)
        return result;
    bool first = true;
    for (const auto& entry : project->entries())
        if (entry.kind == (texture ? assets::AssetKind::texture : assets::AssetKind::material)) {
            add(entry.path.stem().wstring(),
                {texture ? MaterialChoice::Kind::texture : MaterialChoice::Kind::assign, target.object, target.slot,
                 entry.key, target.material_path},
                current == entry.key, first);
            first = false;
        }
    return result;
}
}
