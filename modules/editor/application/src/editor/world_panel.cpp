#include "editor/world_panel.hpp"
#include "editor/workspace.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <format>

namespace atelier::editor {
namespace {
std::wstring title(StringView name) {
    std::wstring result;
    bool capital = true;
    for (char c : name) {
        if (c == '_') {
            result += L' ';
            capital = true;
        } else {
            result += wchar_t(capital && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            capital = false;
        }
    }
    return result;
}

std::wstring choice_title(const ViewOptionInfo& option, double value) {
    const auto idx = static_cast<std::size_t>(value);
    if (option.option == ViewOption::antialiasing) {
        constexpr std::array names{L"Off", L"SMAA", L"TAA"};
        return names[std::min(idx, names.size() - 1)];
    }
    if (option.option == ViewOption::gi_quality) {
        constexpr std::array names{L"High", L"Ultra"};
        return names[std::min(idx, names.size() - 1)];
    }
    if (option.option == ViewOption::sky_mode) {
        constexpr std::array names{L"None", L"Physical Atmosphere", L"HDRI Sky Map"};
        return names[std::min(idx, names.size() - 1)];
    }
    if (option.option == ViewOption::tonemapper) {
        constexpr std::array names{L"Khronos PBR Neutral", L"AgX"};
        return names[std::min(idx, names.size() - 1)];
    }
    return title(option.choices[idx]);
}
} // namespace

double WorldPanel::displayed(const renderer::ViewSettings& view, ViewOption option) const {
    return edit_ && edit_->key == world_control::field(option) ? edit_->value : view_value(view, option);
}

Result<bool> WorldPanel::apply(const ui::Action& action, renderer::ViewSettings& candidate) {
    using namespace world_control;
    if (action.key == performance_table)
        return performance_.apply(action);
    if (action.kind == ui::ActionKind::activate && action.key >= choice_first &&
        action.key < choice_first + (static_cast<unsigned>(ViewOption::last) + 1) * 32) {
        const auto offset = action.key - choice_first;
        const auto* option = view_option(static_cast<ViewOption>(offset / 32));
        if (!option)
            return false;
        const auto value = offset % 32;
        if (option->kind != ViewValueKind::enumeration || value >= option->choices.size())
            return false;
        ATELIER_TRY_BIND(changed, set_view_option(candidate, option->option, double(value)));
        return true;
    }
    if (action.key < field_first || action.key >= choice_first)
        return false;
    const auto* option = view_option(static_cast<ViewOption>(action.key - field_first));
    if (!option)
        return false;
    if (action.kind == ui::ActionKind::activate && option->kind == ViewValueKind::boolean) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked)
            return false;
        ATELIER_TRY_BIND(changed, set_view_option(candidate, option->option, *checked ? 1 : 0));
        return true;
    }
    if (option->kind == ViewValueKind::boolean || option->kind == ViewValueKind::enumeration)
        return false;
    if (action.kind == ui::ActionKind::edit_begin) {
        if (!edit_ || edit_->key != action.key)
            edit_ = Edit{action.key, view_value(candidate, option->option)};
        return true;
    }
    if (!edit_ || edit_->key != action.key)
        return true;
    if (action.kind == ui::ActionKind::edit_cancel) {
        cancel();
        return true;
    }
    if (action.kind != ui::ActionKind::edit_preview && action.kind != ui::ActionKind::edit_commit)
        return true;
    const auto* value = std::get_if<double>(&action.value);
    if (!value)
        return false;
    auto proposed = candidate;
    auto changed = set_view_option(proposed, option->option, *value);
    if (!changed) {
        if (action.kind == ui::ActionKind::edit_commit)
            cancel();
        return std::unexpected(changed.error());
    }
    edit_->value = view_value(proposed, option->option);
    if (action.kind == ui::ActionKind::edit_commit) {
        cancel();
        candidate = proposed;
    }
    return true;
}

Array<ui::MenuItem> WorldPanel::menu(ui::ControlKey source, const renderer::ViewSettings& view) const {
    using namespace world_control;
    Array<ui::MenuItem> items;
    if (source < field_first || source >= choice_first)
        return items;
    const auto* option = view_option(static_cast<ViewOption>(source - field_first));
    if (!option || option->kind != ViewValueKind::enumeration)
        return items;
    for (unsigned i = 0; i < option->choices.size(); ++i) {
        const auto value = double(i);
        ui::MenuItem item{choice(option->option, i),
                          choice_title(*option, value),
                          true,
                          view_value(view, option->option) == value,
                          {}};
        item.behavior = ui::MenuBehavior::radio;
        item.radio_group = source;
        items.add(std::move(item));
    }
    return items;
}

void WorldPanel::observe(const renderer::FrameStats& stats) {
    readouts_[0] = std::format(L"{} primary · {} NEE", stats.scene_lit_gi_primary_rays,
                               stats.scene_lit_gi_visibility_rays[0]);
    readouts_[1] = std::format(L"{} allocated · {} reused", stats.scene_lit_vsm_allocated_pages,
                               stats.scene_lit_vsm_reused_pages);
    readouts_[2] = std::format(L"{} static · {} dynamic · {} CoW", stats.scene_lit_vsm_static_renders,
                               stats.scene_lit_vsm_dynamic_renders, stats.scene_lit_vsm_cow_copies);
    readouts_[3] = std::format(L"{} active tiles · {} max · {} overflow", stats.scene_lit_direct_cluster_cells,
                               stats.scene_lit_direct_cluster_max_sources, stats.scene_lit_direct_cluster_overflows);
    readouts_[4] = std::format(L"{} analytic · {} shadow-casting", stats.scene_lit_light_sources,
                               stats.scene_lit_shadow_sources);
    readouts_[5] = std::format(L"{} refl rays · {} resets", stats.scene_lit_reflection_work_items,
                               stats.scene_lit_history_resets);
}

void WorldPanel::append(ui_language::ControlOutput controls, ui::ControlKey parent, ui_language::LiveDocument& document,
                        const renderer::ViewSettings& view, bool editable, std::wstring_view main_camera,
                        ui_language::ActionBinding clear_camera) const {
    using namespace ui_language;
    namespace key = world_control;
    static const renderer::ViewSettings defaults;
    help_.begin();
    auto groups = std::make_shared<Array<Item>>();
    const auto group = [&](ui::ControlKey id, std::wstring name, ui::ControlIcon icon,
                           std::shared_ptr<Array<Item>> rows) {
        groups->add({id, {std::move(name), static_cast<double>(icon), ItemList{std::move(rows)}}});
    };
    const auto row = [&](Array<Item>& rows, Span<const ViewOption> options, std::wstring label = std::wstring{}) {
        const auto* option = view_option(options.front());
        const auto id = key::field(option->option);
        if (label.empty())
            label = platform::utf8_to_utf16_validated(option->label);
        const bool boolean = option->kind == ViewValueKind::boolean;
        const bool enumeration = option->kind == ViewValueKind::enumeration;
        const bool enabled = editable || !world_option(option->option);
        EditBinding edit{0, enabled, false, {}};
        if (!boolean && !enumeration)
            for (std::size_t i = 0; i < options.size(); ++i) {
                const auto* item = view_option(options[i]);
                const auto value = displayed(view, item->option);
                const auto initial = view_value(defaults, item->option);
                edit.modified |= value != initial;
                const bool integer = item->kind == ViewValueKind::integer;
                constexpr std::array names{L"R", L"G", L"B"};
                edit.fields.add(
                    {key::field(item->option), options.size() == 1 ? L"" : names[i], value, initial, item->minimum,
                     item->maximum, integer ? 1. : .05, integer ? 1. : .005,
                     options.size() == 1 ? ui::VectorAxis::none : static_cast<ui::VectorAxis>(i + 1)});
                edit.fields.back().single_precision = !integer;
                edit.fields.back().integral = integer;
            }
        std::wstring identity = world_option(option->option) ? L"Scene · " : L"Viewport · ";
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (i)
                identity += L" / ";
            identity += platform::utf8_to_utf16_validated(view_option(options[i])->name);
        }
        constexpr std::array kinds{L"bool", L"number", L"integer", L"enum"};
        identity += L" · ";
        identity += kinds[static_cast<std::size_t>(option->kind)];
        if (options.size() > 1)
            identity += L"[" + std::to_wstring(options.size()) + L"]";
        PropertyRow property;
        property.key = key::root + 8192 + static_cast<unsigned>(option->option);
        property.title = std::move(label);
        property.edit = std::move(edit);
        property.kind = boolean       ? PropertyRow::Kind::boolean
                        : enumeration ? PropertyRow::Kind::enumeration
                                      : PropertyRow::Kind::number;
        property.checked = view_value(view, option->option) != 0;
        property.enabled = enabled;
        property.change = {id, enabled};
        property.target = id;
        property.inherit = {id, false};
        if (enumeration)
            property.choice = choice_title(*option, view_value(view, option->option));
        property.help = help_.get(property.key, property.title, identity, option->description);
        rows.add(std::move(property).item());
    };
    const auto scalar = [&](Array<Item>& rows, ViewOption option) { row(rows, {&option, 1}); };

    auto direct = std::make_shared<Array<Item>>();
    scalar(*direct, ViewOption::shadows);
    scalar(*direct, ViewOption::contact_shadow_length);
    scalar(*direct, ViewOption::direct_candidates);
    group(key::root + 100, L"Direct Lighting & Shadows", ui::ControlIcon::sun_light, std::move(direct));

    auto sky_fog = std::make_shared<Array<Item>>();
    scalar(*sky_fog, ViewOption::sky_mode);
    scalar(*sky_fog, ViewOption::sky_intensity);
    scalar(*sky_fog, ViewOption::sky_rotation);
    const std::array sky_options{ViewOption::sky_color_x, ViewOption::sky_color_y, ViewOption::sky_color_z};
    row(*sky_fog, sky_options);
    scalar(*sky_fog, ViewOption::fog_enabled);
    scalar(*sky_fog, ViewOption::fog_density);
    scalar(*sky_fog, ViewOption::fog_height_falloff);
    scalar(*sky_fog, ViewOption::fog_anisotropy);
    group(key::root + 101, L"Sky & Volumetric Fog", ui::ControlIcon::sky_light, std::move(sky_fog));
    auto lighting_groups = groups;

    groups = std::make_shared<Array<Item>>();
    auto gi = std::make_shared<Array<Item>>();
    scalar(*gi, ViewOption::indirect_lighting);
    scalar(*gi, ViewOption::gi_quality);
    scalar(*gi, ViewOption::indirect_gain);
    scalar(*gi, ViewOption::ssilvb);
    scalar(*gi, ViewOption::ao_radius);
    group(key::root + 102, L"3-Layer Global Illumination", ui::ControlIcon::shade_lit, std::move(gi));

    auto refl = std::make_shared<Array<Item>>();
    scalar(*refl, ViewOption::reflections);
    scalar(*refl, ViewOption::reflection_distance);
    scalar(*refl, ViewOption::reflection_roughness);
    group(key::root + 103, L"3-Tier Coat-Aware Reflections", ui::ControlIcon::shade_lit, std::move(refl));
    auto gi_groups = groups;

    groups = std::make_shared<Array<Item>>();
    auto post = std::make_shared<Array<Item>>();
    scalar(*post, ViewOption::exposure_ev);
    scalar(*post, ViewOption::tonemapper);
    scalar(*post, ViewOption::bloom);
    scalar(*post, ViewOption::antialiasing);
    group(key::root + 104, L"Post-Processing & Display", ui::ControlIcon::sliders, std::move(post));
    auto post_groups = groups;

    groups = std::make_shared<Array<Item>>();
    auto debug = std::make_shared<Array<Item>>();
    scalar(*debug, ViewOption::direct_cluster_inspect);
    group(key::root + 105, L"Diagnostics", ui::ControlIcon::overlays, std::move(debug));
    auto diagnostics = groups;

    auto actual = std::make_shared<Array<Item>>();
    constexpr std::array labels{L"Screen probes",     L"VSM pages",     L"VSM updates",
                                L"Light clusters",    L"Scene lights",  L"Reflections & history"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        TextEditBinding readout_val{key::readout_first + i,
                                    readouts_[i].empty() ? std::wstring(L"—") : readouts_[i]};
        readout_val.read_only = true;
        actual->add({key::root + 128 + i, {std::wstring(labels[i]), std::move(readout_val)}});
    }
    TextEditBinding camera_val{key::main_camera_text, Workspace::single_line_field_text(main_camera)};
    camera_val.read_only = true;
    const std::array<InputValue, 11> inputs{ItemList{std::move(lighting_groups)},
                                            ItemList{std::move(gi_groups)},
                                            ItemList{std::move(post_groups)},
                                            ItemList{std::move(diagnostics)},
                                            ItemList{std::move(actual)},
                                            editable,
                                            TableBinding{performance_.table(), performance_.selection()},
                                            performance_.status(),
                                            performance_.resources(),
                                            std::move(camera_val),
                                            clear_camera};
    const std::array keys{KeyBinding{"scroll", key::root + 80},
                          KeyBinding{"lighting_tab", key::lighting_tab},
                          KeyBinding{"gi_tab", key::gi_tab},
                          KeyBinding{"post_tab", key::post_tab},
                          KeyBinding{"diagnostics_tab", key::diagnostics_tab},
                          KeyBinding{"performance_tab", key::performance_tab},
                          KeyBinding{"performance_table", key::performance_table},
                          KeyBinding{"performance_status", key::performance_status},
                          KeyBinding{"performance_resources", key::performance_resources}};
    static_cast<void>(document.append("WorldPanel", parent, keys, inputs, controls));
    help_.end();
}
} // namespace atelier::editor
