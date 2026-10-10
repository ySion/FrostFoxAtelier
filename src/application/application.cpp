#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/presentation.hpp"
#include "atelier/build_info.hpp"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace atelier::application {
namespace {
Result<void> completion(Result<void> result) { return result; }
Result<void> completion(Result<world::Commit> result) {
    if (!result) return std::unexpected(result.error());
    if (result->notification_error) {
        auto error = std::move(*result->notification_error);
        error.message = "Operation committed; lifecycle notification failed: " + error.message;
        return std::unexpected(std::move(error));
    }
    return {};
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
template<class T> T graphics(renderer::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().cause.message);
    return std::move(*result);
}
std::wstring widen(const std::string& value) { return {value.begin(), value.end()}; }
Result<Scalar> scalar_value(const Scalar& prototype,double value) {
    return std::visit([&](const auto& original)->Result<Scalar> {
        using T=std::decay_t<decltype(original)>;
        const auto invalid=[] {return std::unexpected(Error{ErrorCode::invalid_definition,"The value is outside this property's type or range."});};
        if(!std::isfinite(value)) return invalid();
        if constexpr(std::same_as<T,EnumValue>) {
            return invalid(); // Enumerations use named choices, never numeric editing.
        } else if constexpr(std::same_as<T,bool>) {
            if(value!=0 && value!=1) return invalid();return Scalar{value!=0};
        } else {
            if(value<static_cast<double>(std::numeric_limits<T>::lowest()) || value>static_cast<double>(std::numeric_limits<T>::max())) return invalid();
            if constexpr(std::is_integral_v<T>) if(std::trunc(value)!=value) return invalid();
            return Scalar{static_cast<T>(value)};
        }
    },prototype);
}
bool live_table_context(const ui::UiFrame& frame,const ui::ContextMenuRequest& request,bool require_selection) {
    if (!request.table) return true;
    const auto* table=frame.find_table(request.source);
    if (!table || !table->data || (require_selection && table->selection.row!=request.table->row)) return false;
    const auto& rows=table->data->rows;
    const auto row=std::ranges::find(rows,request.table->row,&ui::TableRow::key);
    return row!=rows.end() && row->enabled &&
        std::ranges::find(table->data->columns,request.table->column,&ui::TableColumn::key)!=table->data->columns.end();
}
ui::PixelExtent client_extent(HWND window) {
    if (IsIconic(window)) return {};
    RECT bounds{}; GetClientRect(window, &bounds);
    return {static_cast<std::uint32_t>(bounds.right), static_cast<std::uint32_t>(bounds.bottom)};
}
}
Application::Application(bool validation,bool activate,std::filesystem::path examples_workspace)
    : examples_workspace_(std::move(examples_workspace)),ui_(editor_fonts()),gpu_validation_(validation) {
    auto style=ui_.style(); style.font.family=L"Sarasa UI SC"; style.font.size=12.5f; style.caption_scale=1; style.title_scale=7.f/6;
    if (!ui_.set_style(style)) throw std::runtime_error("Cannot publish editor typography");
    style_sheet_=editor_styles();
    if (!ui_.set_style_sheet(style_sheet_)) throw std::runtime_error("Cannot publish editor style sheet");
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    renderer_ = graphics(renderer::Renderer::open(window_.handle(), options));
    popup_=std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_);
    submenus_.push_back(std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_));
    window_.show(activate);
}
Application::~Application() {
    try { close(); }
    catch (const std::exception& error) { std::fprintf(stderr, "Editor shutdown: %s\n", error.what()); }
}
void Application::status(Result<void> result, const wchar_t* success) {
    workspace_.status = result ? success : widen(result.error().message);
}
void Application::status(Result<world::Commit> result, const wchar_t* success) {
    status(completion(std::move(result)), success);
}

Result<void> Application::focus_selected() { return frame_actors(true); }
Result<void> Application::frame_actors(bool selected_only) {
    // Bounds of the rendered geometry (Blender View Selected / View All); locators count as points.
    auto& runtime=document_->world().runtime();
    math::Vector3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},high=-low;
    bool any{};
    const auto include=[&](math::Vector3 p) {for(int i=0;i<3;++i) {low[i]=std::min(low[i],p[i]);high[i]=std::max(high[i],p[i]);} any=true;};
    const auto wanted=[&](ObjectId actor){return !selected_only || document_->is_selected(actor) || actor==document_->selection().actor;};
    auto items=document_->world().render_items();if(!items) return std::unexpected(items.error());
    std::vector<ObjectId> rendered;
    for(const auto& item:*items) {
        rendered.push_back(item.actor);if(!wanted(item.actor)) continue;
        for(const auto& vertex:item.geometry->vertices())
            include(math::transform_point(math::Vector3{vertex.position[0],vertex.position[1],vertex.position[2]},item.world));
    }
    auto members=document_->world().members();if(!members) return std::unexpected(members.error());
    for(const auto& member:*members) {
        if(!wanted(member.id()) || std::ranges::find(rendered,member.id())!=rendered.end()) continue;
        auto root=world::root_component(runtime,member.id());if(!root || !*root) continue;
        auto matrix=world::world_matrix(runtime,**root);if(!matrix) continue;
        include({(*matrix)[12],(*matrix)[13],(*matrix)[14]});
    }
    if(!any) return std::unexpected(Error{ErrorCode::invalid_definition,selected_only?"Select an Actor with a root SceneComponent to frame.":"The scene has nothing to frame."});
    camera_.frame((low+high)*.5f,math::length(high-low)*.5f);camera_cut_=true;
    return {};
}
Result<void> Application::configure_view(math::Vector3 eye,math::Vector3 target,renderer::Shading shading,bool fit_ddgi_volume) {
    auto lighting=workspace_.view.lit;
    if(fit_ddgi_volume) {
        auto items=document_->world().render_items();if(!items) return std::unexpected(items.error());
        if(items->empty()) return std::unexpected(Error{ErrorCode::invalid_definition,"DDGI volume fitting requires rendered geometry."});
        math::Vector3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},high=-low;
        for(const auto& item:*items) for(const auto& vertex:item.geometry->vertices()) {
            const auto point=math::transform_point(math::Vector3{vertex.position[0],vertex.position[1],vertex.position[2]},item.world);
            for(int axis=0;axis<3;++axis) {low[axis]=std::min(low[axis],point[axis]);high[axis]=std::max(high[axis],point[axis]);}
        }
        std::uint32_t probes=1;
        for(int axis=0;axis<3;++axis) {
            lighting.probe_min[axis]=std::floor(low[axis]-.5f);
            lighting.probe_max[axis]=std::ceil(high[axis]+.5f);
            // Aim for two-meter cells, bounded to 4096 probes and a four-frame sweep.
            lighting.probe_counts[axis]=static_cast<std::uint32_t>(std::clamp(
                std::ceil((lighting.probe_max[axis]-lighting.probe_min[axis])/2.f)+1.f,2.f,16.f));
            probes*=lighting.probe_counts[axis];
        }
        lighting.probe_update_budget=(probes+3)/4;
    }
    if(shading>renderer::Shading::scene_lit || !camera_.look_at(eye,target))
        return std::unexpected(Error{ErrorCode::invalid_definition,"Invalid initial camera or shading mode."});
    workspace_.view.shading=shading;workspace_.view.lit=lighting;camera_cut_=true;invalidate_surface();return {};
}
Result<void> Application::action(const ui::Action& source) {
    // UI instance keys and host command/target keys occupy different roles.
    // Resolve once at the existing dispatch boundary; business checks stay below.
    std::optional<ui::Action> routed;
    if (source.target) routed.emplace(source.kind,source.target,source.value);
    const auto& event=routed?*routed:source;
    using namespace editor::control;
    const auto invalid = [] { return std::unexpected(Error{ErrorCode::invalid_definition, "UI action does not match its live binding or value type."}); };
    const auto done = [&](auto result, const wchar_t* message) -> Result<void> {
        auto finished = completion(std::move(result));
        if (finished) workspace_.status = message;
        return finished;
    };
    if (event.kind==ui::ActionKind::context_menu) {
        const auto* request=std::get_if<ui::ContextMenuRequest>(&event.value);
        if (!request) return invalid();
        request_menu(*request); return {};
    }
    if (event.kind==ui::ActionKind::dismiss) { close_menu(); return {}; }
    if (event.kind==ui::ActionKind::activate && (event.key==menu_bar_handle || event.key==shelf_handle)) {
        close_menu();
        if (event.key==shelf_handle) {
            const auto* collapsed=std::get_if<bool>(&event.value);
            if (!collapsed) return invalid();
            workspace_.shelf_collapsed=*collapsed;
        } else workspace_.menu_bar_collapsed=!workspace_.menu_bar_collapsed;
        return {};
    }
    if (event.kind==ui::ActionKind::activate && is_menu(event.key)) {
        request_menu({event.key,0,0,0}); return {};
    }
    if (event.kind==ui::ActionKind::activate && event.key==menu_group_reset) {
        if (!workspace_.menu) return {};
        return action({ui::ActionKind::reset,workspace_.menu->group?workspace_.menu->group:workspace_.menu->source,{}});
    }
    if (event.kind==ui::ActionKind::activate && workspace_.source_editor.select(event.key)) {
        close_menu(false);workspace_.dock.select(editor::source_control::tab);pending_source_focus_=true;return {};
    }
    if (event.kind==ui::ActionKind::activate && event.key==editor::source_control::problem) {
        if (workspace_.source_editor.locate_problem()) pending_source_focus_=true;return {};
    }
    if(event.kind==ui::ActionKind::activate) if(const auto type=workspace_.component_choice(event.key)) {
        if(document_->selection()!=workspace_.menu_selection()) return invalid();
        return done(document_->add_component(*type),L"Component added");
    }
    if(event.kind==ui::ActionKind::activate) if(const auto choice=workspace_.property_choice(event.key)) {
        if(document_->selection()!=workspace_.menu_selection()) return invalid();
        return done(document_->set_property(choice->object,choice->path,Scalar{choice->value},choice->field),L"Property changed");
    }
    if (ui_.presented() && workspace_.dock.apply(event,*ui_.presented())) return {};
    if (workspace_.workbench.apply(event)) return {};
    if (workspace_.assets.apply(event)) {
        if(event.key==editor::browser_control::results && event.kind==ui::ActionKind::select) workspace_.inspecting_asset=true;
        return {};
    }
    if (workspace_.apply_view(event)) return {};
    if (!source.target) {
        const auto local_event=workspace_.apply_ui(source);
        if (!local_event) return std::unexpected(local_event.error());
        if (*local_event) return {};
    }
    if (event.kind==ui::ActionKind::activate && (event.key==editor::browser_control::open || event.key==editor::browser_control::add)) {
        const auto selected=workspace_.assets.selected();
        if (!selected) return {};
        if (const auto* cell=std::get_if<ui::TableSelection>(&event.value);
            cell && workspace_.assets.row_key(selected->key)!=cell->row) return {};
        if (event.key==editor::browser_control::add && selected->kind==editor::AssetKind::mesh)
            return execute(editor::Command::create_static_mesh,selected->key);
        if (selected->kind==editor::AssetKind::folder) workspace_.assets.navigate(selected->path);
        else if (selected->kind==editor::AssetKind::scene) return open_scene(selected->path);
        else if (selected->kind==editor::AssetKind::source && project_) {
            auto path=project_->resolve(selected->path);if (!path) return std::unexpected(path.error());
            return open_source(*path);
        }
        return {};
    }
    if (event.key==parameter_search_toggle && event.kind==ui::ActionKind::activate) {
        const auto* checked=std::get_if<bool>(&event.value);
        if (!checked) return invalid();
        if (workspace_.parameter_search_visible==*checked) return {};
        workspace_.parameter_search_visible=*checked;
        pending_parameter_search_focus_=*checked;
        return {};
    }
    if (event.kind==ui::ActionKind::activate) {
        if (const auto direction=navigation_direction(event.key)) return done(workspace_.navigate(*document_,direction),L"Selection history");
        if (event.key==world_context) {workspace_.inspecting_asset=false;return done(document_->select({}),L"World context");}
    }
    if (event.kind==ui::ActionKind::select && event.key==tree_world)
        return done(workspace_.select_tree(*document_,event.key),L"World context");
    // The rail snap toggle is a second view of the shared snapping command.
    if (event.kind==ui::ActionKind::activate && event.key==viewport_rail_snap) return execute(editor::Command::snap_toggle);
    if (event.kind==ui::ActionKind::activate && (event.key==shelf_create || event.key==shelf_view)) {
        workspace_.shelf_page=event.key; return {};
    }
    if (event.key == parameter_search || event.key==tree_filter || event.key==structure_filter) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value) return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel) {
            (event.key==tree_filter?workspace_.tree_query:event.key==structure_filter?workspace_.structure_query:workspace_.query) = *value;
            workspace_.status = event.kind == ui::ActionKind::edit_cancel ? L"Filter cancelled" :
                event.key==tree_filter?L"Tree filtered":L"Parameters filtered";
        }
        return {};
    }
    // A retired control's cleanup must not cancel a different live edit.
    if (event.kind == ui::ActionKind::edit_cancel)
        return document_->owns_edit(event.key) ? done(document_->cancel_edit(), L"Edit cancelled") : Result<void>{};
    if (const auto* binding = workspace_.binding(event.key)) {
        if (event.kind==ui::ActionKind::select && (binding->kind==editor::Workspace::Binding::Kind::object || binding->kind==editor::Workspace::Binding::Kind::structure) && binding->path.empty())
            return done(workspace_.select_tree(*document_,event.key,std::get_if<ui::SelectionModifiers>(&event.value)?std::get<ui::SelectionModifiers>(event.value):ui::SelectionModifiers{},ui_.presented().get()),L"Tree selection updated");
        if (binding->kind==editor::Workspace::Binding::Kind::navigation) {
            if (event.kind==ui::ActionKind::activate) {workspace_.inspecting_asset=false;return done(document_->select(binding->object),L"Actor selected");}
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::name) {
            if (event.kind==ui::ActionKind::edit_commit)
                return done(document_->rename(binding->object,std::get<std::wstring>(event.value)),L"Actor renamed / Undo restores its name");
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::position_default) {
            if (event.kind!=ui::ActionKind::reset) return invalid();
            return done(document_->reset_position(binding->object),L"Position restored to default / Undo restores the vector");
        }
        if(binding->kind==editor::Workspace::Binding::Kind::rotation_default || binding->kind==editor::Workspace::Binding::Kind::scale_default) {
            if(event.kind!=ui::ActionKind::reset) return invalid();
            auto value=world::read_transform(document_->world().runtime(),binding->object);if(!value) return std::unexpected(value.error());
            if(binding->kind==editor::Workspace::Binding::Kind::rotation_default) value->rotation={};else value->scale={1,1,1};
            const std::array objects{binding->object};const std::array values{*value};
            auto result=document_->begin_transform(objects,event.key);if(!result) return result;
            result=document_->preview_transform(event.key,values);
            if(result) result=document_->commit_edit(event.key);
            if(!result) {const auto cancelled=document_->cancel_edit();if(!cancelled) return cancelled;}
            return done(result,L"Transform tuple restored");
        }
        if(binding->kind==editor::Workspace::Binding::Kind::rotation) {
            if(event.kind==ui::ActionKind::edit_begin) {
                const std::array objects{binding->object};return document_->begin_transform(objects,event.key);
            }
            if(event.kind!=ui::ActionKind::edit_preview && event.kind!=ui::ActionKind::edit_commit) return {};
            const auto* number=std::get_if<double>(&event.value);if(!number || !std::isfinite(*number) || std::abs(*number)>360000) return invalid();
            auto value=document_->original_transform(event.key,binding->object);if(!value) return std::unexpected(value.error());
            auto angles=editor::euler_degrees(value->rotation);
            for(std::size_t axis=0;axis<3;++axis) if(std::ranges::equal(binding->path,document_->types().rotation_fields[axis])) angles[axis]=float(*number);
            value->rotation=editor::from_euler_degrees(angles);const std::array values{*value};
            auto result=document_->preview_transform(event.key,values);
            if(result && event.kind==ui::ActionKind::edit_commit) result=document_->commit_edit(event.key);
            return done(result,L"Rotation updated");
        }
        if (binding->path.empty()) {
            return invalid();
        }
        if(event.kind==ui::ActionKind::activate) if(const auto* checked=std::get_if<bool>(&event.value)) {
            return done(document_->set_property(binding->object,binding->path,Scalar{*checked},event.key),L"Property changed");
        }
        const auto* value = std::get_if<double>(&event.value);
        if (!value) return invalid();
        auto prototype=document_->read_property(binding->object,binding->path);if(!prototype) return std::unexpected(prototype.error());
        auto typed=scalar_value(*prototype,*value);if(!typed) return std::unexpected(typed.error());
        switch (event.kind) {
        case ui::ActionKind::edit_begin:
            return done(document_->begin_edit(binding->object, binding->path, event.key), L"Editing field / Esc cancels");
        case ui::ActionKind::edit_preview:
            return done(document_->preview_edit(event.key, *typed), L"Field preview / Esc cancels");
        case ui::ActionKind::edit_commit: {
            auto preview = document_->preview_edit(event.key, *typed);
            if (!preview) return preview;
            return done(document_->commit_edit(event.key), L"Field committed");
        }
        default: return {};
        }
    }
    if (event.kind != ui::ActionKind::activate) return {}; // Includes retired field/Actor bindings.
    if (const auto* command=editor::command_definition(event.key)) return execute(command->id);
    return {};
}
Result<void> Application::execute(editor::Command command,std::string_view asset_key) {
    using enum editor::Command;
    if (!workspace_.command_enabled(command,*document_)) return {};
    if(auto cancelled=viewport_tool_.cancel(*document_);!cancelled) return cancelled;
    if(!workspace_.menu) window_.capture(false);
    Result<void> result;
    switch (command) {
    case example_sponza:case example_scene_lit:case example_physic:return open_example(command);
    case tool_select: case tool_transform: case tool_move: case tool_rotate: case tool_scale:
        workspace_.viewport_settings.tool=static_cast<editor::TransformTool>(static_cast<unsigned>(command)-static_cast<unsigned>(tool_select));
        workspace_.status=std::wstring(editor::command_definition(static_cast<std::uint64_t>(command))->title);break;
    case pivot_median:workspace_.viewport_settings.pivot=editor::PivotMode::median;break;
    case pivot_cursor:workspace_.viewport_settings.pivot=editor::PivotMode::cursor;break;
    case cursor_origin:viewport_tool_.cursor={};viewport_tool_.cursor_rotation={};break;
    case snap_toggle:workspace_.viewport_settings.grid_snap=!workspace_.viewport_settings.grid_snap;break;
    case snap_grid_01:case snap_grid_02:case snap_grid_05:case snap_grid_1: {
        constexpr std::array steps{.1f,.2f,.5f,1.f};
        workspace_.viewport_settings.grid_spacing=steps[static_cast<unsigned>(command)-static_cast<unsigned>(snap_grid_01)];break;
    }
    case selection_grid:case selection_cursor: result=viewport_tool_.snap_selection(*document_,workspace_.viewport_settings,command==selection_cursor);break;
    case cursor_selected:result=viewport_tool_.cursor_to_selection(*document_);break;
    case cursor_grid:viewport_tool_.cursor_to_grid(workspace_.viewport_settings);break;
    case cursor_surface:workspace_.viewport_settings.cursor_surface=!workspace_.viewport_settings.cursor_surface;break;
    case view_perspective:case view_top:case view_front:case view_right:
        camera_.set_view(static_cast<editor::CameraView>(static_cast<unsigned>(command)-static_cast<unsigned>(view_perspective)));camera_cut_=true;break;
    case viewport_options:workspace_.viewport_options_collapsed=!workspace_.viewport_options_collapsed;break;
    case open_source: case edit_ui_source: case save_document: case close_source: case reload_source: case save_all:
    case new_source: case new_project_ui: case edit_project_ui:
        return source_command(command,asset_key);
    case build_project:case rebuild_project:case run_project:case stop_project:case show_project_ui:case show_output:
        return execution_command(command);
    case show_code: workspace_.dock.select(editor::source_control::tab);break;
    case new_project: case open_project: case save_scene: case save_scene_as: case open_scene: case import_mesh: case refresh_assets:
        workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
        return project_command(command,asset_key);
    case exit: PostMessageW(window(),WM_CLOSE,0,0); break;
    case create_actor: workspace_.inspecting_asset=false;result=completion(document_->add_actor()); break;
    case create_point_light:case create_rectangle_light:case create_directional_light:case create_sky_light: {
        workspace_.inspecting_asset=false;
        const auto kind=command==create_point_light?world::LightKind::point:
            command==create_rectangle_light?world::LightKind::rectangle:
            command==create_directional_light?world::LightKind::directional:world::LightKind::sky;
        result=completion(document_->create_light(kind,viewport_tool_.cursor));break;
    }
    case create_static_mesh: {
        workspace_.inspecting_asset=false;
        const auto catalog=editor::mesh_assets();
        if (asset_key.empty()) asset_key=catalog.front().key;
        if (project_) {
            auto asset=project_->mesh(asset_key);if (!asset) return std::unexpected(asset.error());
            result=completion(document_->create_static_mesh(*asset,viewport_tool_.cursor));
        } else {
            const auto* asset=editor::mesh_asset(asset_key);
            if (!asset) return std::unexpected(Error{ErrorCode::invalid_definition,"Unknown mesh asset."});
            result=completion(document_->create_static_mesh(*asset,viewport_tool_.cursor));
        }
        break;
    }
    case erase_selected: workspace_.inspecting_asset=false;result=completion(document_->erase_selected()); break;
    case erase_actors: workspace_.inspecting_asset=false;result=completion(document_->erase_actors()); break;
    case duplicate_selected: workspace_.inspecting_asset=false;result=completion(document_->duplicate_selected()); break;
    case undo: case redo:
        workspace_.inspecting_asset=false;
        if (workspace_.source_history()) {
            workspace_.source_editor.active()->document->edit().history(command==redo);pending_source_focus_=true;
        } else if (workspace_.sample_history()) { if (command==undo) workspace_.workbench.undo(); else workspace_.workbench.redo(); }
        else result=completion(command==undo?document_->undo():document_->redo());
        break;
    case focus: result=focus_selected();if(result) camera_cut_=true;break;
    case reset_camera: camera_.reset();camera_cut_=true;break;
    case show_scene: workspace_.dock.select(editor::control::scene_tab); break;
    case show_parameters: workspace_.dock.select(editor::control::parameters_tab); break;
    case show_controls: workspace_.dock.select(editor::control::workbench); break;
    case show_actors: workspace_.dock.select(editor::control::actors_tab); break;
    case show_structure: workspace_.dock.select(editor::control::structure_tab); break;
    case show_assets: workspace_.dock.select(editor::control::assets_tab); break;
    case maximize_pane: workspace_.dock.maximize(workspace_.dock.focused_tab()); break;
    case previous_tab: case next_tab: workspace_.dock.cycle(command==previous_tab?-1:1); break;
    case close_tab:
        if (workspace_.source_history() && workspace_.source_editor.active()) return source_command(close_source,{});
        workspace_.dock.close(workspace_.dock.focused_tab());break;
    case reset_layout: workspace_.reset_layout(); break;
    case about: {
        close_menu(false);
        const auto details=L"FrostFox Atelier\n\nBuild: "+editor::wide(build_info::revision)+
            L"\nConfiguration: "+editor::wide(build_info::configuration)+L"\n\nProject:\n"+
            (project_?project_->root().wstring():L"No project open")+L"\n\nScene:\n"+
            (scene_path_.empty()?L"Untitled":scene_path_.generic_wstring());
        MessageBoxW(window(),details.c_str(),L"About FrostFox Atelier",MB_OK|MB_ICONINFORMATION);
        break;
    }
    }
    if (result) workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
    return result;
}
bool Application::effects(const ui::Reply& reply) {
    std::wstring first_error;
    bool business_failed = false;
    const auto remember = [&](std::wstring message) { if (first_error.empty()) first_error = std::move(message); };
    const auto apply = [&](auto&& self, const ui::Reply& current, bool cleanup) -> void {
        if (current.error) {
            const auto& error = *current.error;
            if (error.code == ui::ErrorCode::platform_failed)
                remember(L"Input operation failed (Windows " + std::to_wstring(error.native_code) + L").");
            else if (error.code == ui::ErrorCode::capacity_exceeded) remember(L"Input is too long.");
            else remember(L"Input must be valid single-line text.");
        }
        for (const auto& effect : current.effects) {
            if (business_failed && !cleanup) break;
            if (const auto* command = std::get_if<ui::Action>(&effect)) {
                if (cleanup && command->kind != ui::ActionKind::edit_cancel && command->kind!=ui::ActionKind::dismiss && command->kind!=ui::ActionKind::drag_cancel) continue;
                const auto result = action(*command);
                if (!result) {
                    if (cleanup) first_error += L"; rollback failed: " + widen(result.error().message);
                    else { remember(widen(result.error().message)); business_failed = true; }
                }
            } else {
                const auto& request = std::get<ui::PlatformRequest>(effect);
                ui::Event response;
                if (const auto* clipboard = std::get_if<ui::ClipboardRequest>(&request)) {
                    if (cleanup) continue;
                    response.kind = ui::EventKind::clipboard_result;
                    const auto target = ui_.text_input();
                    response.data = target && target->token == clipboard->token ? window_.clipboard(*clipboard) :
                        ui::ClipboardResult{clipboard->kind, clipboard->operation_id, clipboard->token, false, ERROR_INVALID_STATE, {}};
                } else {
                    const auto& ime = std::get<ui::ImeRequest>(request);
                    if (cleanup && ime.kind != ui::ImeKind::cancel) continue;
                    auto result = window_.ime(ime);
                    if (!result.success) {
                        const auto message = L"IME operation failed (Windows " + std::to_wstring(result.native_error) + L").";
                        if ((cleanup || ime.kind == ui::ImeKind::cancel) && !first_error.empty()) first_error += L"; " + message;
                        else remember(message);
                    }
                    response.kind = ui::EventKind::ime_result; response.data = std::move(result);
                }
                self(self, ui_.input(response), cleanup);
            }
        }
        if (current.capture == ui::Capture::release) window_.capture(false);
        else if (current.capture == ui::Capture::acquire && !business_failed) window_.capture(true);
        if (current.cursor) window_.cursor(*current.cursor);
    };
    apply(apply, reply, false);
    if (business_failed) {
        apply(apply, ui_.cancel_interaction(), true);
        const auto cancelled = document_->cancel_edit();
        if (!cancelled) first_error += L"; rollback failed: " + widen(cancelled.error().message);
        window_.capture(false);
    }
    const auto native_error = window_.text_input(ui_.text_input());
    if (native_error) remember(L"Text input unavailable (Windows " + std::to_wstring(native_error) + L").");
    const bool success=first_error.empty();
    if (!success) workspace_.status = std::move(first_error);
    return success;
}
void Application::sync_text_input() {
    const auto error = window_.text_input(ui_.text_input());
    if (error) workspace_.status = L"Text input unavailable (Windows " + std::to_wstring(error) + L").";
}
void Application::cancel_interaction(const ui::Reply& reply) {
    // Session distinguishes native focus/capture loss from a forced UI cancellation.
    // Application-owned gestures must end for either kind before capture is released.
    orbiting_ = false;
    close_menu(false);
    effects(reply);
    const auto cancelled = document_->cancel_edit();
    if (!cancelled) workspace_.status += L"; rollback failed: " + widen(cancelled.error().message);
    const auto tool_cancelled=viewport_tool_.cancel(*document_);
    if(!tool_cancelled) workspace_.status=L"Viewport rollback failed: "+widen(tool_cancelled.error().message);
    window_.capture(false);
    window_.cursor(ui::Cursor::arrow);
}
void Application::invalidate_surface() {
    ++surface_revision_;
    cancel_interaction(ui_.cancel_interaction());
}
bool Application::pointer_ready() const noexcept {
    const auto& frame = ui_.presented();
    return frame && frame->drawing->extent == client_extent(window_.handle()) &&
        frame->drawing->surface_revision == surface_revision_ && frame->drawing->dpi_scale == window_.dpi_scale();
}
bool Application::over_viewport(float x, float y) const noexcept {
    if (!pointer_ready()) return false;
    const auto* viewport = ui_.presented()->find(editor::control::viewport);
    return viewport && x >= viewport->bounds.x && y >= viewport->bounds.y &&
        x < viewport->bounds.x + viewport->bounds.width && y < viewport->bounds.y + viewport->bounds.height;
}
void Application::request_menu(const ui::ContextMenuRequest& request) {
    const auto* source=ui_.presented()?ui_.presented()->find(request.source):nullptr;
    if (!source || !source->enabled || !live_table_context(*ui_.presented(),request,false)) return;
    if(auto cancelled=viewport_tool_.cancel(*document_);!cancelled) {status(cancelled,L"");return;}
    if (!editor::control::is_menu(request.source)) menu_return_focus_=ui_.focused_control();
    // The native menu owns subsequent mouse-up events. Retire the previous
    // gesture through its UI/business transaction before transferring capture.
    effects(ui_.cancel_interaction());
    for (auto& submenu:submenus_) submenu->hide();
    menu_path_.clear();
    workspace_.menu=request; menu_incarnation_=source->incarnation;
    menu_requested_=std::chrono::steady_clock::now();
    pie_release_pending_=pie_release_select_=pie_pressed_=false;pie_hot_=0;
    menu_context_target_=source->context_target; menu_context_group_=source->context_group;
    menu_table_layout_=source->table_layout;
    orbiting_=false; window_.capture(false);
}
void Application::close_menu(bool restore_focus) {
    pending_menu_=0;
    pie_release_pending_=pie_release_select_=pie_pressed_=false;pie_hot_=0;
    const bool was_open=workspace_.menu.has_value() || menu_window();
    if (!was_open) return;
    workspace_.menu.reset(); workspace_.menu_items.clear();
    for (auto& submenu:submenus_) submenu->hide();
    menu_path_.clear();
    if (popup_) popup_->hide();
    if (was_open) {
        window_.capture(false);
        if (restore_focus) effects(ui_.focus(menu_return_focus_));
    }
    menu_return_focus_=0; menu_incarnation_=0;
    menu_context_target_=menu_context_group_=0;
    menu_table_layout_={};
}
void Application::update_menu() {
    if (!workspace_.menu || workspace_.menu_items.empty()) { close_menu(); return; }
    const auto request=*workspace_.menu;
    const auto* source=ui_.presented()->find(request.source);
    if (!source || source->incarnation!=menu_incarnation_ || !source->enabled ||
        source->context_target!=menu_context_target_ || source->context_group!=menu_context_group_ ||
        source->table_layout!=menu_table_layout_ ||
        !live_table_context(*ui_.presented(),request,true) ||
        source->bounds.y>=source->clip.bottom || source->bounds.y+source->bounds.height<=source->clip.top) { close_menu(); return; }
    if (workspace_.pie_menu()) {
        if (popup_) popup_->hide();
        window_.capture(true);
        if (ui_.presented()->popup==editor::control::viewport_shading_pie) {
            effects(ui_.focus(pie_hot_));
            if (pie_release_pending_) {
                pie_release_pending_=false;
                if (pie_release_select_) {
                    ui::Event release;release.kind=ui::EventKind::key_up;
                    pie_input(release,'S');
                }
            }
        }
        return;
    }
    const bool below=editor::control::is_menu(source->key) || source->kind==ui::ControlKind::combo;
    POINT points[2];
    if (below) {
        points[0]={static_cast<LONG>(source->bounds.x),static_cast<LONG>(source->bounds.y)};
        points[1]={static_cast<LONG>(source->bounds.x+source->bounds.width),static_cast<LONG>(source->bounds.y+source->bounds.height)};
    } else points[0]=points[1]={static_cast<LONG>(request.x),static_cast<LONG>(request.y)};
    MapWindowPoints(window(),nullptr,points,2);
    if (!popup_) popup_=std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_);
    popup_->update(source->key,{points[0].x,points[0].y,points[1].x,points[1].y},below,workspace_.menu_items,ui_.style(),style_sheet_,workspace_.ui_component("PopupMenu"),
        request.group==editor::control::viewport_delete_menu);
    if (!popup_->visible()) {
        workspace_.status=L"UI: "+editor::wide(std::string(popup_->diagnostic()));close_menu(false);return;
    }
    window_.capture(true); popup_->render();
    auto items=std::span<const ui::MenuItem>(workspace_.menu_items);
    auto* parent=popup_.get();
    for (std::size_t depth=0;depth<menu_path_.size();++depth) {
        const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==menu_path_[depth];});
        const auto* box=parent->presented()?parent->presented()->find(menu_path_[depth]):nullptr;
        if (at==items.end() || at->children.empty() || !at->enabled || !box) { menu_path_.resize(depth); break; }
        if (depth==submenus_.size()) submenus_.push_back(std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_));
        POINT corners[2]{{static_cast<LONG>(box->bounds.x),static_cast<LONG>(box->bounds.y)},
                         {static_cast<LONG>(box->bounds.x+box->bounds.width),static_cast<LONG>(box->bounds.y+box->bounds.height)}};
        MapWindowPoints(parent->window(),nullptr,corners,2);
        auto& child=*submenus_[depth];
        child.update(at->key,{corners[0].x,corners[0].y,corners[1].x,corners[1].y},false,at->children,ui_.style(),style_sheet_,workspace_.ui_component("PopupMenu"));
        if (!child.visible()) {
            workspace_.status=L"UI: "+editor::wide(std::string(child.diagnostic()));menu_path_.resize(depth);break;
        }
        child.render(); parent=&child; items=at->children;
    }
    for (std::size_t depth=menu_path_.size();depth<submenus_.size();++depth) submenus_[depth]->hide();
}
bool Application::menu_input(const ui::Event& input,WPARAM native_key) {
    if (!workspace_.menu) return false;
    if (workspace_.pie_menu()) return pie_input(input,native_key);
    const auto source=workspace_.menu->source;
    const bool top=editor::control::is_menu(source);
    auto menu_at=ui::ControlKey{};
    if (ui_.presented()) for (const auto& box:ui_.presented()->controls)
        if (editor::control::is_menu(box.key) && input.x>=box.bounds.x && input.x<box.bounds.x+box.bounds.width &&
            input.y>=box.bounds.y && input.y<box.bounds.y+box.bounds.height) { menu_at=box.key; break; }
    const auto change=[&](ui::ControlKey key) {
        effects(ui_.focus(key)); request_menu({key,0,0,0});
    };
    if (top && input.kind==ui::EventKind::pointer_move && menu_at && menu_at!=source) { change(menu_at); return true; }
    if (input.kind==ui::EventKind::pointer_down && menu_at) {
        if (menu_at==source) close_menu(); else change(menu_at);
        return true;
    }
    if (input.kind==ui::EventKind::key_down && (input.key==ui::Key::f10 || native_key==VK_MENU)) { close_menu(); return true; }
    if (top && menu_path_.empty() && input.kind==ui::EventKind::key_down && (input.key==ui::Key::left || input.key==ui::Key::right)) {
        change(editor::cycle_menu(source,input.key==ui::Key::left)); return true;
    }
    if (!menu_window()) {
        if (input.kind==ui::EventKind::key_down && input.key==ui::Key::escape) close_menu();
        return true; // Opening transition is modal before the first accepted popup frame.
    }
    const bool pointer=input.kind==ui::EventKind::pointer_down || input.kind==ui::EventKind::pointer_up ||
        input.kind==ui::EventKind::pointer_move || input.kind==ui::EventKind::wheel;
    std::size_t level=menu_path_.size();
    POINT screen{static_cast<LONG>(input.x),static_cast<LONG>(input.y)}; ClientToScreen(window(),&screen);
    if (pointer) for (std::size_t i=menu_path_.size()+1;i>0;--i) {
        const auto index=i-1; auto* host=index?submenus_[index-1].get():popup_.get();
        RECT bounds{}; GetWindowRect(host->window(),&bounds);
        if (PtInRect(&bounds,screen)) { level=index; break; }
    }
    if (input.kind==ui::EventKind::key_down && !menu_path_.empty() &&
        (input.key==ui::Key::left || input.key==ui::Key::escape)) {
        submenus_[menu_path_.size()-1]->hide(); menu_path_.pop_back(); return true;
    }
    auto* host=level?submenus_[level-1].get():popup_.get();
    auto items=std::span<const ui::MenuItem>(workspace_.menu_items);
    for (std::size_t depth=0;depth<level;++depth) {
        const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==menu_path_[depth];});
        if (at==items.end()) { close_menu(); return true; }
        items=at->children;
    }
    auto event=input; ScreenToClient(host->window(),&screen);
    event.x=static_cast<float>(screen.x); event.y=static_cast<float>(screen.y);
    const auto reply=host->input(event);
    for (const auto& effect:reply.effects) if (const auto* command=std::get_if<ui::Action>(&effect)) {
        if (command->kind==ui::ActionKind::activate) {
            const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==command->key;});
            if (at==items.end() || !at->enabled) continue;
            if (!at->children.empty()) {
                menu_path_.resize(level); menu_path_.push_back(at->key); update_menu(); return true;
            }
            invoke_menu(*command,items);
            return true;
        } else if (command->kind==ui::ActionKind::dismiss) { close_menu(); return true; }
    }
    return true;
}
bool Application::invoke_menu(const ui::Action& command,std::span<const ui::MenuItem> items) {
    const auto at=std::ranges::find(items,command.key,&ui::MenuItem::key);
    if (at==items.end() || !at->enabled || !at->children.empty()) return false;
    const auto edit=at->edit_command;
    if (edit) {
        const auto incarnation=menu_incarnation_;
        close_menu();effects(ui_.execute(*edit,incarnation));
    } else {
        const auto result=action(command);
        if (!result) status(result,L"");
        close_menu();
    }
    return true;
}
bool Application::pie_input(const ui::Event& input,WPARAM native_key) {
    if ((input.kind==ui::EventKind::key_down && (input.key==ui::Key::escape || input.key==ui::Key::tab)) ||
        (input.kind==ui::EventKind::pointer_down && input.button==ui::PointerButton::secondary)) {
        close_menu();return true;
    }
    const bool pointer=input.kind==ui::EventKind::pointer_move || input.kind==ui::EventKind::pointer_down || input.kind==ui::EventKind::pointer_up;
    if (pointer) pointer_position_={input.x,input.y};
    const bool release=input.kind==ui::EventKind::key_up && native_key=='S';
    if (release && !pie_release_select_) {
        const auto dx=pointer_position_.x-workspace_.menu->x,dy=pointer_position_.y-workspace_.menu->y;
        pie_release_select_=dx*dx+dy*dy>324*window_.dpi_scale()*window_.dpi_scale() ||
            std::chrono::steady_clock::now()-menu_requested_>=std::chrono::milliseconds(200);
    }
    const auto* frame=ui_.presented().get();
    const auto* pie=frame?frame->find(editor::control::viewport_shading_pie):nullptr;
    if (!pie || frame->popup!=pie->key) {
        if (release) pie_release_pending_=true;
        return true; // Opening owns input before its first accepted geometry.
    }
    const auto choose=[&] {
        // A click on a visible slot wins over gesture direction when screen-edge
        // fitting moved the menu away from its original invocation point.
        if (pointer) for (const auto& item:workspace_.menu_items) {
            const auto* box=frame->find(item.key);
            if (item.enabled && box && pointer_position_.x>=box->bounds.x && pointer_position_.x<box->bounds.x+box->bounds.width &&
                pointer_position_.y>=box->bounds.y && pointer_position_.y<box->bounds.y+box->bounds.height) return item.key;
        }
        // Use the invocation point as the gesture origin even if the visible
        // menu was fitted away from a screen edge. Slots come from accepted DSL geometry.
        const auto dx=pointer_position_.x-workspace_.menu->x,dy=pointer_position_.y-workspace_.menu->y;
        const auto distance=std::hypot(dx,dy);
        ui::ControlKey key{};float best=-2.f;
        if (distance>18*frame->drawing->dpi_scale) for (const auto& item:workspace_.menu_items) {
            const auto* box=frame->find(item.key);if (!item.enabled || !box) continue;
            const auto x=box->bounds.x+box->bounds.width*.5f-(pie->bounds.x+pie->bounds.width*.5f);
            const auto y=box->bounds.y+box->bounds.height*.5f-(pie->bounds.y+pie->bounds.height*.5f);
            const auto length=std::hypot(x,y);if (length==0) continue;
            const auto dot=(dx*x+dy*y)/(distance*length);
            if (dot>best) {best=dot;key=item.key;}
        }
        return key;
    };
    const auto activate=[&](ui::ControlKey key) {
        if (key) invoke_menu({ui::ActionKind::activate,key,true},workspace_.menu_items);
        else close_menu();
    };
    if (pointer) {
        pie_hot_=choose();effects(ui_.focus(pie_hot_));
        if (input.kind==ui::EventKind::pointer_down && input.button==ui::PointerButton::primary) pie_pressed_=true;
        if (input.kind==ui::EventKind::pointer_up && input.button==ui::PointerButton::primary && pie_pressed_) activate(pie_hot_);
        return true;
    }
    if (release) {if (pie_release_select_) activate(pie_hot_?pie_hot_:choose());return true;}
    // Existing popup keyboard navigation and Enter activation share the same command dispatch.
    auto reply=ui_.input(input);
    pie_hot_=ui_.focused_control();
    for (const auto& effect:reply.effects) if (const auto* command=std::get_if<ui::Action>(&effect)) {
        if (command->kind==ui::ActionKind::dismiss) {close_menu();break;}
        if (command->kind==ui::ActionKind::activate) {invoke_menu(*command,workspace_.menu_items);break;}
    }
    return true;
}
void Application::native(NativeEvent message) {
    if(control_) control_->input(message.message,message.wparam);
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) { invalidate_surface(); return; }
    if (message.message==WM_MOVE) { close_menu(false); return; }
    if (message.message==WM_CANCELMODE || (message.message==WM_ACTIVATEAPP && !message.wparam)) {
        cancel_interaction(ui_.cancel_interaction()); return;
    }
    if (message.message == WM_SETFOCUS) return;
    if (message.ime_owned) return;
    if (message.composition) {
        ui::Event composition;
        composition.kind = ui::EventKind::composition; composition.data = std::move(*message.composition);
        auto reply = ui_.input(composition);
        if (message.error) reply.error = ui::Error{ui::ErrorCode::platform_failed, 0, message.error};
        effects(reply);
        return;
    }
    if (message.error) { workspace_.status = L"Invalid native text input."; return; }
    const auto native_key=message.wparam;
    auto mapped=input_event(std::move(message),window());if (!mapped) return;
    input(*mapped,native_key);
}
void Application::input(const ui::Event& event,WPARAM native_key) {
    const bool pointer=event.kind==ui::EventKind::pointer_move || event.kind==ui::EventKind::pointer_down ||
        event.kind==ui::EventKind::pointer_up || event.kind==ui::EventKind::wheel;
    if (event.kind==ui::EventKind::focus_lost || event.kind==ui::EventKind::capture_lost) {
        auto reply=ui_.input(event);
        if (ui_.presented()) workspace_.dock.focus_control(ui_.focused_control(),*ui_.presented());
        cancel_interaction(reply); return;
    }
    if (menu_input(event,native_key)) return;
    if(pointer) pointer_position_={event.x,event.y};
    if(viewport_tool_.active() && viewport_input(event,static_cast<std::uint32_t>(native_key))) return;
    ui::ControlKey requested_menu{};
    if (event.kind==ui::EventKind::key_down) {
        if (event.key==ui::Key::f10 && !event.shift) requested_menu=editor::control::file_menu;
        else if (event.alt) for (const auto& group:editor::command_groups)
            if (native_key==static_cast<WPARAM>(group.access_key)) { requested_menu=group.key; break; }
    }
    if (requested_menu) {
        const auto menu=requested_menu;
        menu_return_focus_=ui_.focused_control();
        if (workspace_.menu_bar_collapsed) { workspace_.menu_bar_collapsed=false; pending_menu_=menu; }
        else { effects(ui_.focus(menu)); request_menu({menu,0,0,0}); }
        return;
    }
    if (event.kind==ui::EventKind::pointer_down && ui_.presented())
        for (const auto& box:ui_.presented()->controls) if (editor::control::is_menu(box.key) &&
            event.x>=box.bounds.x && event.x<box.bounds.x+box.bounds.width && event.y>=box.bounds.y && event.y<box.bounds.y+box.bounds.height)
            menu_return_focus_=ui_.focused_control();
    const bool release = event.kind == ui::EventKind::pointer_up;
    if (pointer && !release && !pointer_ready()) return;
    if (event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::primary) {
        const auto menu=ui_.pointer_target(event.x,event.y);
        if (editor::control::is_menu(menu)) {
            effects(ui_.focus(menu));request_menu({menu,0,0,0});return;
        }
    }
    if (event.kind==ui::EventKind::key_down && !workspace_.menu &&
        (native_key==VK_F6 || native_key==VK_F8)) {
        const auto command=native_key==VK_F6?editor::Command::build_project:event.shift?editor::Command::stop_project:editor::Command::run_project;
        if(auto result=execute(command);!result) status(result,L"");return;
    }
    if (event.kind==ui::EventKind::key_down && !workspace_.menu &&
        ((event.control && (native_key=='S' || native_key=='O')) || native_key==VK_F5)) {
        const auto command=native_key=='O'?editor::Command::open_project:native_key==VK_F5?editor::Command::refresh_assets:
            event.shift?editor::Command::save_scene_as:editor::Command::save_document;
        status(execute(command),L"Project command completed");return;
    }
    if (event.kind==ui::EventKind::key_down && event.control && !workspace_.menu &&
        (native_key==VK_TAB || native_key=='B' || native_key=='W')) {
        effects(ui_.cancel_interaction());
        const auto command=native_key==VK_TAB?(event.shift?editor::Command::previous_tab:editor::Command::next_tab):
            native_key=='B'?editor::Command::maximize_pane:editor::Command::close_tab;
        if (auto result=execute(command);!result) status(result,L"");
        return;
    }
    if(viewport_input(event,static_cast<std::uint32_t>(native_key))) return;
    auto reply = ui_.input(event);
    if (ui_.presented()) workspace_.dock.focus_control(ui_.focused_control(),*ui_.presented());
    effects(reply);
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        std::optional<editor::Command> command;
        if (event.control && (event.key==ui::Key::z || event.key==ui::Key::y))
            command=(event.key==ui::Key::y || event.shift)?editor::Command::redo:editor::Command::undo;
        else if (!workspace_.sample_history()) {
            if (event.key==ui::Key::delete_key) command=editor::Command::erase_selected;
            else if (native_key=='F' && !ui_.wants_keyboard()) command=editor::Command::focus;
            else if (event.key==ui::Key::escape) status(document_->cancel_edit(),L"Edit cancelled");
        }
        if (command) if (auto result=execute(*command);!result) status(result,L"");
    }
}
void Application::enable_control(const std::filesystem::path& directory) {
    if(!control_) control_=std::make_unique<EditorControl>(*this,directory);
}
bool Application::step(renderer::Capture* capture) {
    frame_timing_ = {};
    frame_statistics_ = {};
    if (closed_) return false;
    window_.poll([&](NativeEvent&& event) { native(std::move(event)); });
    if (window_.closing()) {
        if (confirm_discard()) {close();return false;}
        window_.cancel_close();
    }
    poll_project();
    if(control_) control_->poll();
    const auto title=(project_?project_->name():L"No project")+L" — "+(scene_path_.empty()?L"Untitled":scene_path_.filename().wstring())+
        (document_->dirty()?L" *":L"")+L" — FrostFox Atelier";
    if (title!=window_title_) { SetWindowTextW(window(),title.c_str());window_title_=title; }
    auto target = graphics(renderer_.target());
    const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) throw std::runtime_error("UI rejected the window surface.");
    effects(*updated);
    auto gc = document_->collect_step();
    if (!gc) throw std::runtime_error(gc.error().message);
    // Keep the last committed scopes while minimized; an empty surface cannot
    // validate a new layout or make its controls available for interaction.
    if (!target.drawable) { MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE); return true; }
    const ui_layout::Size view_size{static_cast<float>(extent.width)/surface_.dpi_scale,static_cast<float>(extent.height)/surface_.dpi_scale};
    workspace_.viewport_mode=viewport_tool_.displayed_tool(workspace_.viewport_settings);
    workspace_.viewport_readout=viewport_tool_.readout();
    workspace_.camera_view=camera_.view();
    workspace_.view.grid_spacing=workspace_.viewport_settings.grid_spacing;
    workspace_.view.grid_plane=camera_.view()==editor::CameraView::front?renderer::GridPlane::xy:
        camera_.view()==editor::CameraView::right?renderer::GridPlane::yz:renderer::GridPlane::xz;
    const auto describe=[&] {return workspace_.describe(*document_,view_size,ui_.presented().get(),project_?&*project_:nullptr);};
    auto controls = describe();
    auto described = ui_.set_controls(controls);
    if (!described && workspace_.reject_ui_controls("UI candidate rejected at control "+std::to_string(described.error().key))) {
        controls=describe();
        described=ui_.set_controls(controls);
    }
    if (!described) throw std::runtime_error("UI rejected editor controls (key " + std::to_string(described.error().key) + ").");
    effects(*described);
    if (workspace_.pie_menu()) effects(ui_.focus(pie_hot_));
    using Clock = std::chrono::steady_clock;
    const bool sample = sampling_ && !capture;
    const auto prepare_start = sample ? Clock::now() : Clock::time_point{};
    auto prepared = ui_.prepare();
    if (!prepared && workspace_.reject_ui_controls("Editor layout candidate rejected at control "+std::to_string(prepared.error().key))) {
        controls=describe();
        described=ui_.set_controls(controls);
        if (!described) throw std::runtime_error("UI rejected restored editor controls (key " + std::to_string(described.error().key) + ").");
        effects(*described);
        prepared=ui_.prepare();
    }
    const auto prepare_us = sample ? std::chrono::duration<double, std::micro>(Clock::now() - prepare_start).count() : 0;
    if (!prepared) {
        if (!ui_.presented()) throw std::runtime_error("Initial editor frame preparation failed (UI code " + std::to_string(static_cast<int>(prepared.error().code)) + ").");
        workspace_.status = L"Frame preparation failed; previous frame retained";
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    workspace_.commit_ui_controls();
    // Native popups render on their retained surface before scene submission /
    // Present can block. Their anchor still comes from accepted owner geometry.
    if (!workspace_.pie_menu()) update_menu();
    const auto& frame = **prepared;
    const auto* viewport = frame.find(editor::control::viewport);
    const auto scene_bounds=viewport?viewport->bounds:ui::Rect{};
    const bool had_viewport_gesture=viewport_tool_.active();
    if(had_viewport_gesture && accepted_view_ && accepted_view_->bounds!=scene_bounds)
        status(viewport_tool_.cancel(*document_),L"Viewport changed; edit cancelled");
    viewport_tool_.forget(*document_);
    if(had_viewport_gesture && !viewport_tool_.active()) window_.capture(false);
    if(viewport) take(scene_.refresh(*document_));
    const auto camera = camera_.matrices(std::max(scene_bounds.width,1.f) / std::max(scene_bounds.height,1.f));
    const auto viewport_view=editor::ViewportView::make(camera_,scene_bounds,window_.dpi_scale());
    overlay_.clear();
    if(viewport) viewport_tool_.overlay(*document_,viewport_view,workspace_.viewport_settings,overlay_);
    const bool projection_cut=accepted_view_ && accepted_view_->orthographic!=viewport_view.orthographic;
    renderer::Capture control_capture;
    const bool capturing_control=control_ && control_->wants_capture();
    auto* frame_capture=capture?capture:capturing_control?&control_capture:nullptr;
    const auto effective_view=control_?control_->view():workspace_.view;
    renderer::Frame render_frame{*frame.drawing, {camera.view.elements, camera.projection.elements}, scene_bounds,
        viewport?std::span<const renderer::MeshInstance>{scene_.meshes}:std::span<const renderer::MeshInstance>{},effective_view,overlay_,
        viewport?std::span<const renderer::Light>{scene_.lights}:std::span<const renderer::Light>{},scene_.identity,scene_.revision,camera_cut_ || projection_cut,
        scene_.environment};
    const auto render_start = sample ? Clock::now() : Clock::time_point{};
    auto rendered = renderer_.render(render_frame, frame_capture);
    const auto render_us = sample ? std::chrono::duration<double, std::micro>(Clock::now() - render_start).count() : 0;
    if (!rendered) {
        if(control_) control_->render_failed(rendered.error().cause.message);
        if (rendered.error().terminal) throw std::runtime_error("Renderer stopped: " + rendered.error().cause.message);
        workspace_.status = widen(rendered.error().cause.message); return true;
    }
    frame_statistics_ = rendered->stats;
    if (rendered->status == renderer::FrameStatus::accepted) {
        if (rendered->accepted_id != frame.drawing->id || rendered->accepted_extent != frame.drawing->extent)
            throw std::runtime_error("Renderer accepted a mismatched frame.");
        if (!ui_.accept(*prepared)) throw std::runtime_error("UI rejected an accepted renderer frame.");
        accepted_view_=viewport?std::optional(viewport_view):std::nullopt;
        if(viewport) camera_cut_=false;
        workspace_.render_notice.clear();
        if(viewport && effective_view.shading==renderer::Shading::scene_lit && effective_view.lit.shadows &&
            effective_view.wireframe!=renderer::Wireframe::only) {
            const auto requested=std::ranges::count_if(scene_.lights,[](const auto& light){return light.casts_shadow;});
            if(std::uint64_t(requested)>effective_view.lit.shadow_light_budget)
                workspace_.render_notice=L"Shadows "+std::to_wstring(rendered->stats.scene_lit_shadow_lights)+L"/"+std::to_wstring(requested);
        }
        ++accepted_frames_;
        if(control_) {
            if(capture && capturing_control) {control_capture=*capture;control_->rendered(render_frame,*rendered,&control_capture);}
            else control_->rendered(render_frame,*rendered,frame_capture);
        }
        if (pending_source_focus_) {
            pending_source_focus_=false;
            if (workspace_.source_editor.active()) effects(ui_.focus(editor::source_control::buffer));
        }
        if (pending_parameter_search_focus_) {
            pending_parameter_search_focus_=false;
            if (workspace_.parameter_search_visible) effects(ui_.focus(editor::control::parameter_search));
        }
        sync_text_input();
        if (pending_menu_) {
            const auto menu=pending_menu_; pending_menu_=0;
            effects(ui_.focus(menu)); request_menu({menu,0,0,0});
        } else update_menu();
        if (sample) frame_timing_ = {rendered->accepted_id, prepare_us, render_us};
    } else {
        if(control_) control_->rendered(render_frame,*rendered,nullptr);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return true;
}
void Application::close() {
    if (closed_) return;
    closed_ = true;
    control_.reset();
    play_.close();
    if(!play_snapshot_.empty()) {std::error_code ignored;std::filesystem::remove(play_snapshot_,ignored);play_snapshot_.clear();}
    close_menu(false);
    for (auto& submenu:submenus_) submenu->close();
    if (popup_) popup_->close();
    effects(ui_.cancel_interaction());
    const auto cancelled = document_->cancel_edit();
    if (!cancelled) throw std::runtime_error(cancelled.error().message);
    window_.capture(false);
    project_code_=nullptr;
    take(completion(document_->close()));
    scene_.reset();
    const auto stopped = renderer_.close();
    if (!stopped) throw std::runtime_error(stopped.error().cause.message);
}
}
