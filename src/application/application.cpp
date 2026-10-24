#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/presentation.hpp"
#include "atelier/build_info.hpp"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_map>

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
Application::Application(bool validation,bool activate,std::filesystem::path examples_workspace,std::optional<ui::PixelExtent> initial_client,bool vsync)
    : window_(nullptr,initial_client),examples_workspace_(std::move(examples_workspace)),ui_(editor_fonts()),gpu_validation_(validation),vsync_(vsync) {
    require_frame_timing();
    auto style=ui_.style(); style.font.family=L"Sarasa UI SC"; style.font.size=12.5f; style.caption_scale=1; style.title_scale=7.f/6;
    if (!ui_.set_style(style)) throw std::runtime_error("Cannot publish editor typography");
    style_sheet_=editor_styles();
    if (!ui_.set_style_sheet(style_sheet_)) throw std::runtime_error("Cannot publish editor style sheet");
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    options.vsync = vsync_;
    options.shader_cache = shader_cache_directory();
    renderer_ = graphics(renderer::Renderer::open(window_.handle(), options));
    popup_=std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_);
    submenus_.push_back(std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_));
    window_.show(activate);
}
Application::~Application() {
    if(std::uncaught_exceptions()) preserve_failure("Unhandled editor failure outside the frame loop");
    try { close(); }
    catch (const std::exception& error) { std::fprintf(stderr, "Editor shutdown: %s\n", error.what()); }
}
void Application::preserve_failure(std::string_view reason) noexcept {
    if(recovery_.attempted() || !document_ || closed_) return;
    try {
        window_.capture(false);
        window_.cursor(ui::Cursor::arrow);
        const auto& saved=recovery_.preserve(*document_,project_?project_->root():std::filesystem::path{},scene_path_,reason);
        if(!saved) std::fprintf(stderr,"Scene recovery failed: %s\n",saved.error().message.c_str());
        else if(*saved) {
            std::fprintf(stderr,"Scene recovery saved: %s\n",editor::utf8(saved->value().scene.wstring()).c_str());
            if(!saved->value().warning.empty()) std::fprintf(stderr,"Scene recovery warning: %s\n",saved->value().warning.c_str());
        }
        std::fflush(stderr);
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Scene recovery failed: %s\n",error.what());
    } catch(...) {std::fputs("Scene recovery failed with an unknown error\n",stderr);}
}
void Application::status(Result<void> result, const wchar_t* success) {
    workspace_.status = result ? success : widen(result.error().message);
}
void Application::status(Result<world::Commit> result, const wchar_t* success) {
    status(completion(std::move(result)), success);
}

void Application::sync_world_view() {
    workspace_.sync_world(*document_);
    // Undo/reopen may restore a finer follow-focus lattice. The scene remains
    // authoritative; re-anchor this view if its old target cannot represent it.
    if(rendering::scene_lighting_focus_error(workspace_.view.lit,lighting_focus())) {
        auto state=camera_.state();state.target={};camera_.set_state(state);camera_cut_=true;
    }
}
std::array<float,3> Application::lighting_focus() const {
    const auto target=camera_.state().target;return {target.x,target.y,target.z};
}
Result<void> Application::set_camera(const editor::Camera& camera,bool cut) {
    const auto target=camera.state().target;
    if(const auto* error=renderer::probe_focus_error(workspace_.view.lit,{target.x,target.y,target.z}))
        return std::unexpected(Error{ErrorCode::invalid_definition,error});
    if(camera.state()!=camera_.state()) {camera_=camera;camera_cut_|=cut;}
    return {};
}
editor::Context Application::operator_context(bool interactive) {
    auto context=workspace_.operator_context(*document_,project_?&*project_:nullptr,interactive);
    context.cursor=viewport_tool_.cursor;
    return context;
}
Result<editor::OperatorReport> Application::call_operator(const editor::OperatorCall& request,bool interactive) {
    const auto shown=workspace_.status;
    auto report=editor::call(workspace_.operators,*this,operator_context(interactive),request);
    // An operator whose host work reported its own status keeps it.
    if(report && report->status==editor::OperatorStatus::finished && workspace_.status==shown) workspace_.status=workspace_.operators.find(request.id)->title;
    return report;
}
Result<void> Application::run_operator(std::string_view id,bool report_unavailable) {
    auto report=call_operator({std::string(id),editor::json_format::Json::object(),{}},true);if(!report) return std::unexpected(report.error());
    if(report->status==editor::OperatorStatus::unavailable && report_unavailable)
        return std::unexpected(Error{ErrorCode::unsupported_operation,report->message});
    return {};
}
// Command transitions retire the old owner before releasing native capture;
// cleanup must run before the command can establish a new gesture.
Result<void> Application::end_interaction() {
    if(camera_drag_) cancel_interaction(ui_.cancel_interaction());
    if(auto cancelled=viewport_tool_.cancel(*document_);!cancelled) return cancelled;
    if(!workspace_.menu) window_.capture(false);
    return {};
}
Result<void> Application::source_history(bool redo) {
    workspace_.source_editor.active()->document->edit().history(redo);pending_source_focus_=true;return {};
}
Result<void> Application::close_tab(editor::RegionKind region,bool interactive) {
    const auto tab=editor::region_tab(region);if(!tab) return {};
    if(const auto* file=workspace_.source_editor.active();region==editor::RegionKind::code && file) {
        if(!interactive && file->dirty())
            return std::unexpected(Error{ErrorCode::unsupported_operation,"The Code page's active document has unsaved changes; save or reload it first."});
        return source_command(editor::Command::close_source,{});
    }
    workspace_.dock.close(tab);return {};
}
void Application::maximize_tab(editor::RegionKind region) {
    if(const auto tab=editor::region_tab(region)) workspace_.dock.maximize(tab);
}
Result<void> Application::workbench_history(bool redo) {
    if(redo) workspace_.workbench.redo();else workspace_.workbench.undo();return {};
}
void Application::scene_changed() {workspace_.inspecting_asset=false;sync_world_view();}
Result<renderer::ViewSettings> Application::fitted_probe_view(bool follow_focus) const {
    auto items=document_->presentation().render_items();if(!items) return std::unexpected(items.error());
    if(items->empty()) return std::unexpected(Error{ErrorCode::invalid_definition,"DDGI volume fitting requires rendered geometry."});
    struct Bounds {math::Vector3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},high=-low;};
    Bounds world_bounds;
    std::unordered_map<const geometry::Mesh*,Bounds> local_bounds;
    const auto include=[](Bounds& bounds,math::Vector3 point) {
        for(unsigned axis=0;axis<3;++axis) {bounds.low[axis]=std::min(bounds.low[axis],point[axis]);bounds.high[axis]=std::max(bounds.high[axis],point[axis]);}
    };
    for(const auto& item:*items) {
        const auto [found,inserted]=local_bounds.try_emplace(item.geometry.get());
        if(inserted) for(const auto& vertex:item.geometry->vertices()) include(found->second,{vertex.position[0],vertex.position[1],vertex.position[2]});
        // Eight corners cover each transformed instance; shared mesh vertices
        // are scanned once for this explicit layout command.
        for(unsigned corner=0;corner<8;++corner) {
            math::Vector3 point;for(unsigned axis=0;axis<3;++axis) point[axis]=(corner&(1u<<axis))?found->second.high[axis]:found->second.low[axis];
            include(world_bounds,math::transform_point(point,item.world));
        }
    }
    auto candidate=workspace_.view;auto& lighting=candidate.lit;lighting.probe_volumes={};
    auto& coarse=lighting.probe_volumes[follow_focus?1:0];coarse.enabled=true;
    for(unsigned axis=0;axis<3;++axis) {
        coarse.minimum[axis]=std::floor(world_bounds.low[axis]-.5f);coarse.maximum[axis]=std::ceil(world_bounds.high[axis]+.5f);
        coarse.counts[axis]=static_cast<std::uint32_t>(std::clamp(std::ceil((coarse.maximum[axis]-coarse.minimum[axis])/2.f)+1.f,2.f,follow_focus?12.f:16.f));
    }
    if(follow_focus) {
        auto& focused=lighting.probe_volumes[0];focused.enabled=true;focused.placement=renderer::ProbePlacement::follow_focus;
        focused.minimum={-6,-2,-6};focused.maximum={6,4,6};focused.counts={13,7,13};focused.blend_cells=1;
    }
    if(auto valid=editor::validate_view_options(candidate,lighting_focus());!valid) return std::unexpected(valid.error());
    return candidate;
}
Result<void> Application::configure_view(math::Vector3 eye,math::Vector3 target,renderer::Shading shading,bool fit_ddgi_volume) {
    workspace_.sync_world(*document_);
    auto candidate=workspace_.view;
    // Fitting fills probe volumes for a document without an Environment. It belongs to
    // loading, not authoring: no history, the document stays clean and the next save
    // writes the Environment. Saved settings are the author's and stay.
    const bool fit=fit_ddgi_volume && document_->environment()==ObjectId{};
    if(fit) {
        auto fitted=fitted_probe_view(false);if(!fitted) return std::unexpected(fitted.error());candidate=*fitted;
        const auto& counts=candidate.lit.probe_volumes[0].counts;
        candidate.lit.probe_update_budget=(counts[0]*counts[1]*counts[2]+3)/4;
    }
    auto camera=camera_;
    if(shading>renderer::Shading::scene_lit || !camera.look_at(eye,target))
        return std::unexpected(Error{ErrorCode::invalid_definition,"Invalid initial camera or shading mode."});
    candidate.shading=shading;
    // Reject the whole view before the fitted volume reaches the World, which keeps no history of it.
    if(auto valid=editor::validate_view_options(candidate,{target.x,target.y,target.z});!valid) return valid;
    if(fit) if(auto initialized=document_->initialize_environment(candidate.lit);!initialized) return initialized;
    if(auto set=workspace_.set_view_settings(*document_,candidate,{target.x,target.y,target.z});!set) return set;
    camera_=camera;camera_cut_=true;invalidate_surface();return {};
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
        // A row menu acts on the selection: right-clicking outside it selects that row first.
        if(const auto* row=workspace_.binding(request->source);row && row->kind==editor::Workspace::Binding::Kind::object &&
           row->path.empty() && row->object!=ObjectId{} && !document_->is_selected(row->object))
            if(auto selected=workspace_.select_tree(*document_,request->source);!selected) return selected;
        request_menu(*request); return {};
    }
    if (event.kind==ui::ActionKind::dismiss && event.key==editor::control::color_editor) return finish_color_edit(true);
    if (event.key==editor::control::inline_rename && workspace_.row_rename) {
        if (event.kind==ui::ActionKind::edit_preview) {
            if (const auto* text=std::get_if<std::wstring>(&event.value)) workspace_.row_rename->text=*text;
            return {};
        }
        if (event.kind!=ui::ActionKind::edit_commit && event.kind!=ui::ActionKind::edit_cancel) return {};
        const auto rename=std::exchange(workspace_.row_rename,std::nullopt);
        if (!rename->asset.empty()) {
            const auto* text=std::get_if<std::wstring>(&event.value);
            if (event.kind==ui::ActionKind::edit_cancel || !text || *text==rename->asset.filename().wstring() || !project_) return {};
            auto renamed=project_->rename(rename->asset,*text);if(!renamed) return std::unexpected(renamed.error());
            workspace_.assets.reselect(*renamed);workspace_.status=L"Renamed to "+renamed->filename().wstring();return {};
        }
        effects(ui_.focus(workspace_.object_control(rename->object)));
        const auto* text=std::get_if<std::wstring>(&event.value);
        if (event.kind==ui::ActionKind::edit_cancel || !text || *text==document_->name(rename->object)) return {};
        return done(document_->rename(rename->object,*text),L"Entity renamed / Undo restores its name");
    }
    if (event.kind==ui::ActionKind::dismiss) { close_menu(); return {}; }
    if (workspace_.color_edit) if (auto handled=color_edit_action(event);!handled || *handled) return handled?Result<void>{}:Result<void>{std::unexpected(handled.error())};
    if (event.kind==ui::ActionKind::activate && event.key==command_key(editor::Command::show_fps)) {
        const auto* checked=std::get_if<bool>(&event.value);
        if (!checked) return invalid();
        workspace_.show_fps=*checked;
        return {};
    }
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
    if(event.kind==ui::ActionKind::activate) if(const auto choice=workspace_.material_choice(event.key)) {
        if(!project_) return invalid();
        using Kind=editor::Workspace::MaterialChoice::Kind;
        if(choice->kind==Kind::assign) return document_->assign_material(*project_,choice->object,choice->slot,choice->key);
        if(choice->kind==Kind::clear) return document_->clear_material(choice->object,choice->slot);
        if(choice->kind==Kind::texture) return document_->set_material_texture(*project_,choice->object,static_cast<material::Input>(choice->index),choice->key);
        auto root=document_->open_material(*project_,choice->key);if(!root) return std::unexpected(root.error());
        workspace_.material_inspection=choice->key;workspace_.inspecting_asset=true;return {};
    }
    if (workspace_.workbench.apply(event)) return {};
    if (workspace_.assets.apply(event)) {
        if(event.key==editor::browser_control::results && event.kind==ui::ActionKind::select) {
            if(auto cancelled=document_->cancel_edit();!cancelled) return cancelled;
            workspace_.inspecting_asset=true;workspace_.material_inspection.clear();
            const auto& selected=workspace_.assets.selected();
            if(selected && selected->kind==editor::AssetKind::material && project_) {
                auto root=document_->open_material(*project_,selected->key);if(!root) return std::unexpected(root.error());
                workspace_.material_inspection=selected->key;
            }
        }
        return {};
    }
    const auto view_action=workspace_.apply_view(*document_,event,lighting_focus());
    if(!view_action) return std::unexpected(view_action.error());
    if(*view_action) return {};
    if (!source.target) {
        const auto local_event=workspace_.apply_ui(source);
        if (!local_event) return std::unexpected(local_event.error());
        if (*local_event) return {};
    }
    if (event.kind==ui::ActionKind::activate && event.key==editor::browser_control::rename) return begin_asset_rename();
    if (event.kind==ui::ActionKind::activate && (event.key==editor::browser_control::open || event.key==editor::browser_control::add)) {
        const auto selected=workspace_.assets.selected();
        if (!selected) return {};
        if (const auto* cell=std::get_if<ui::TableSelection>(&event.value);
            cell && workspace_.assets.row_key(selected->key)!=cell->row) return {};
        if (event.key==editor::browser_control::add && selected->kind==editor::AssetKind::mesh)
            return execute(editor::Command::create_static_mesh,selected->key);
        if (selected->kind==editor::AssetKind::folder) workspace_.assets.navigate(selected->path);
        else if(selected->kind==editor::AssetKind::material && project_) {
            auto root=document_->open_material(*project_,selected->key);if(!root) return std::unexpected(root.error());
            workspace_.material_inspection=selected->key;workspace_.inspecting_asset=true;workspace_.dock.select(editor::control::parameters_tab);
        }
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
    if (event.kind==ui::ActionKind::expand) {
        // A filter shows every matching row expanded; row expansion resumes when it clears.
        const auto* expanded=std::get_if<bool>(&event.value);
        if (!expanded) return invalid();
        using Tree=editor::Workspace::EntityTree;
        if (event.key==tree_world) return workspace_.tree_query.empty()?
            workspace_.set_tree_expanded(*document_,Tree::scene_tree,std::array{ObjectId{}},*expanded):Result<void>{};
        const auto* row=workspace_.binding(event.key);
        if (!row || !row->path.empty() || (row->kind!=editor::Workspace::Binding::Kind::object && row->kind!=editor::Workspace::Binding::Kind::structure))
            return invalid();
        const auto tree=row->kind==editor::Workspace::Binding::Kind::object?Tree::scene_tree:Tree::structure;
        if (!(tree==Tree::scene_tree?workspace_.tree_query:workspace_.structure_query).empty()) return {};
        return workspace_.set_tree_expanded(*document_,tree,std::span{&row->object,1},*expanded);
    }
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
    if (const auto* binding=workspace_.binding(event.key);binding && binding->kind==editor::Workspace::Binding::Kind::color_pick &&
        event.kind==ui::ActionKind::activate) return open_color_edit(binding->object,binding->path,event.key);
    if (const auto* binding = workspace_.binding(event.key);binding && binding->kind==editor::Workspace::Binding::Kind::object &&
        binding->path.empty() && (event.kind==ui::ActionKind::drag_begin || event.kind==ui::ActionKind::drag_preview ||
        event.kind==ui::ActionKind::drag_commit || event.kind==ui::ActionKind::drag_cancel)) {
        // Scene Tree drag: drop on a row parents under it, on World or empty space unparents.
        // Keep-world reparenting leaves the Entity where it is in the viewport; one history entry.
        const auto* point=std::get_if<ui::DragPoint>(&event.value);
        const auto drop=point && ui_.presented()?workspace_.tree_drop(*document_,*ui_.presented(),binding->object,*point):std::nullopt;
        workspace_.tree_drop_row=event.kind==ui::ActionKind::drag_preview && drop?drop->row:0;
        if(event.kind==ui::ActionKind::drag_preview) workspace_.status=drop?L"Drop to parent · World or empty space unparents":L"Cannot parent here";
        if(event.kind!=ui::ActionKind::drag_commit || !drop) return {};
        const auto current=world::parent_entity(document_->world().runtime(),binding->object);
        if(!current) return std::unexpected(current.error());
        if(*current==drop->parent) return {};
        // The moved Entity ends selected: the tree reveals a newly selected row under its new parent.
        if(auto moved=completion(document_->reparent(binding->object,drop->parent));!moved) return moved;
        return done(document_->select_entities(std::array{binding->object},binding->object),drop->parent==ObjectId{}?L"Entity unparented":L"Entity parented");
    }
    if (const auto* binding = workspace_.binding(event.key)) {
        if(binding->kind==editor::Workspace::Binding::Kind::material_override) {
            if(event.kind!=ui::ActionKind::activate) return {};
            const auto* enabled=std::get_if<bool>(&event.value);if(!enabled) return invalid();
            return document_->set_material_override(binding->component,binding->slot,binding->path,*enabled);
        }
        if (const auto* modifiers=std::get_if<ui::SelectionModifiers>(&event.value);event.kind==ui::ActionKind::select && modifiers &&
            modifiers->double_click && !modifiers->shift && !modifiers->control && binding->kind==editor::Workspace::Binding::Kind::object &&
            binding->path.empty() && document_->is_selected(binding->object)) {
            // Double-clicking a selected row renames it in place (Blender Outliner / Houdini Tree View).
            workspace_.dock.select(editor::control::entities_tab);
            workspace_.row_rename=editor::Workspace::RowRename{binding->object,std::wstring(document_->name(binding->object))};
            pending_rename_focus_=true;return {};
        }
        if (event.kind==ui::ActionKind::select && (binding->kind==editor::Workspace::Binding::Kind::object || binding->kind==editor::Workspace::Binding::Kind::structure) && binding->path.empty())
            return done(workspace_.select_tree(*document_,event.key,std::get_if<ui::SelectionModifiers>(&event.value)?std::get<ui::SelectionModifiers>(event.value):ui::SelectionModifiers{}),L"Tree selection updated");
        if (binding->kind==editor::Workspace::Binding::Kind::navigation) {
            if (event.kind==ui::ActionKind::activate) {workspace_.inspecting_asset=false;workspace_.dock.select(editor::control::parameters_tab);return done(document_->select(binding->object),L"Object selected");}
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::name) {
            if (event.kind==ui::ActionKind::edit_commit)
                return done(document_->rename(binding->object,std::get<std::wstring>(event.value)),L"Entity renamed / Undo restores its name");
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
    if (event.kind != ui::ActionKind::activate) return {}; // Includes retired field/Entity bindings.
    if (const auto* command=editor::command_definition(event.key)) return execute(command->id);
    return {};
}
// Colour editor: one Session group transaction from open to Done/dismiss; Esc and
// outside clicks keep the edit (Houdini applies live), Revert restores the original.
Result<void> Application::open_color_edit(ObjectId object,std::span<const FieldId> prefix,ui::ControlKey swatch) {
    if(workspace_.color_edit) if(auto closed=finish_color_edit(true);!closed) return closed;
    close_menu(false);
    editor::Workspace::ColorEdit edit;edit.object=object;
    const auto* binding=workspace_.binding(swatch);
    if(!binding || binding->kind!=editor::Workspace::Binding::Kind::color_pick ||
        !std::isfinite(binding->color_maximum) || binding->color_maximum<=0)
        return std::unexpected(Error{ErrorCode::invalid_definition,"Colour editing requires the owning property's numeric range."});
    edit.maximum=binding->color_maximum;
    for(std::uint32_t lane=0;lane<3;++lane) {
        auto path=world::FieldPath(prefix.begin(),prefix.end());
        auto info=document_->world().runtime().describe(take_type(object,prefix));if(!info) return std::unexpected(info.error());
        if(lane>=(*info)->fields.size()) return std::unexpected(Error{ErrorCode::invalid_definition,"A colour needs three channels."});
        path.push_back((*info)->fields[lane].id);
        auto value=document_->read_property(object,path);if(!value) return std::unexpected(value.error());
        edit.original[lane]=edit.value[lane]=std::visit([](const auto& v)->double {
            if constexpr(std::same_as<std::decay_t<decltype(v)>,EnumValue>) return v.bits; else return double(v);},*value);
        edit.paths.push_back(std::move(path));
    }
    if(auto begun=document_->begin_group_edit(object,edit.paths,editor::control::color_group);!begun) return begun;
    // The swatch button routes to this pick key as its action target; anchor the editor under it.
    const ui::ControlBox* box=nullptr;
    if(ui_.presented()) for(const auto& control:ui_.presented()->controls)
        if(control.kind==ui::ControlKind::button && control.action_target==swatch) {box=&control;break;}
    const auto dpi=window_.dpi_scale();
    edit.x=box?(box->bounds.x+box->bounds.width)/dpi-420:0;edit.y=box?(box->bounds.y+box->bounds.height)/dpi+2:0;
    edit.hex=editor::color_hex(edit.value);
    workspace_.color_edit=std::move(edit);workspace_.status=L"Colour editor · Done or click outside to keep · Revert restores";
    return {};
}
TypeId Application::take_type(ObjectId object,std::span<const FieldId> path) const {
    auto& runtime=document_->world().runtime();
    auto type=runtime.type_of(object).value_or(TypeId{});
    for(const auto id:path) {
        const auto info=runtime.describe(type);if(!info) return {};
        const auto at=std::ranges::find((*info)->fields,id,&FieldInfo::id);if(at==(*info)->fields.end()) return {};
        type=at->value.type;
    }
    return type;
}
Result<void> Application::apply_color(std::array<double,3> rgb) {
    auto& edit=*workspace_.color_edit;
    std::vector<Scalar> values;
    for(std::size_t lane=0;lane<3;++lane) {
        const auto value=rgb[lane];
        if(!std::isfinite(value) || value<0 || value>edit.maximum)
            return std::unexpected(Error{ErrorCode::argument_mismatch,"Colour channels must stay within the property's numeric range."});
        auto prototype=document_->read_property(edit.object,edit.paths[lane]);if(!prototype) return std::unexpected(prototype.error());
        auto typed=scalar_value(*prototype,value);if(!typed) return std::unexpected(typed.error());
        values.push_back(*typed);
    }
    if(auto applied=document_->preview_group_edit(editor::control::color_group,values);!applied) return applied;
    for(std::size_t lane=0;lane<3;++lane)
        edit.value[lane]=std::visit([](const auto& value)->double {
            if constexpr(std::same_as<std::decay_t<decltype(value)>,EnumValue>) return value.bits; else return double(value);},values[lane]);
    edit.hex=editor::color_hex(edit.value);
    return {};
}
Result<bool> Application::color_edit_action(const ui::Action& event) {
    using namespace editor::control;
    auto& edit=*workspace_.color_edit;
    if(event.key==color_done && event.kind==ui::ActionKind::activate) {if(auto done=finish_color_edit(true);!done) return std::unexpected(done.error());return true;}
    if(event.key==color_revert && event.kind==ui::ActionKind::activate) {
        if(auto applied=apply_color(edit.original);!applied) return std::unexpected(applied.error());return true;
    }
    if(event.key==color_hex) {
        const auto* text=std::get_if<std::wstring>(&event.value);
        if(!text || event.kind!=ui::ActionKind::edit_commit) return true;
        const auto parsed=editor::parse_color_hex(*text);
        if(!parsed) {workspace_.status=L"Hex colour needs six digits, e.g. FF8800";return true;}
        if(auto applied=apply_color(*parsed);!applied) return std::unexpected(applied.error());return true;
    }
    const bool rgb=event.key>=color_rgb && event.key<color_rgb+3,hsv=event.key>=color_hsv && event.key<color_hsv+3;
    if(!rgb && !hsv) return false;
    if(event.kind!=ui::ActionKind::edit_preview && event.kind!=ui::ActionKind::edit_commit && event.kind!=ui::ActionKind::edit_cancel) return true;
    const auto* value=std::get_if<double>(&event.value);if(!value || !std::isfinite(*value)) return true;
    auto next=edit.value;
    if(rgb) next[event.key-color_rgb]=*value;
    else {auto h=editor::rgb_to_hsv(edit.value);h[event.key-color_hsv]=*value;next=editor::hsv_to_rgb(h);}
    if(auto applied=apply_color(next);!applied) return std::unexpected(applied.error());
    return true;
}
Result<void> Application::finish_color_edit(bool keep) {
    if(!workspace_.color_edit) return {};
    // Retire the form before ending its transaction: queued field cleanup and
    // popup dismissal must not end it again, even if rollback reports an error.
    workspace_.color_edit.reset();
    if(!document_->owns_edit(editor::control::color_group)) return {};
    auto finished=keep?document_->commit_edit(editor::control::color_group):document_->cancel_edit();
    if(!finished && keep) {
        const auto cancelled=document_->cancel_edit();
        if(!cancelled) return std::unexpected(Error{finished.error().code,
            finished.error().message+"; rollback failed: "+cancelled.error().message});
    }
    if(finished) {
        workspace_.status=keep?L"Colour applied":L"Colour edit cancelled";
    }
    return finished;
}
Result<void> Application::begin_asset_rename() {
    if(!project_ || !workspace_.assets.can_rename()) return {};
    const auto& item=*workspace_.assets.selected();
    const auto row=workspace_.assets.row_key(item.key);
    if(!row) return {};
    workspace_.row_rename=editor::Workspace::RowRename{{},item.path.filename().wstring(),item.path,(row<<2)|editor::browser_control::name_column};
    pending_rename_focus_=true;return {};
}
Result<void> Application::execute(editor::Command command,std::string_view asset_key) {
    using enum editor::Command;
    if(auto call=editor::operator_for(command)) {
        if(command==create_static_mesh && !asset_key.empty()) call->args["mesh"]=std::string(asset_key);
        auto report=call_operator(*call,true);if(!report) return std::unexpected(report.error());
        // A command that fixes arguments reports its own title rather than the operator's.
        if(report->status==editor::OperatorStatus::finished && !call->args.empty())
            workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
        return {};
    }
    if (!workspace_.command_enabled(command,*document_)) return {};
    if(auto ended=end_interaction();!ended) return ended;
    Result<void> result;
    switch (command) {
    case frame_all:case focus:case reset_camera:case reset_layout:case close_tab:case maximize_pane:case undo:case redo:
    case add_transform:case remove_transform:case discard_saved_data:
    case create_entity:case create_static_mesh:case create_point_light:case create_rectangle_light:case create_directional_light:
    case tool_select:case tool_transform:case tool_move:case tool_rotate:case tool_scale:case pivot_median:case pivot_cursor:
    case snap_grid_01:case snap_grid_02:case snap_grid_05:case snap_grid_1:
    case snap_rotation_1:case snap_rotation_5:case snap_rotation_15:case snap_rotation_45:case orientation_global:case orientation_local:
    case select_mode_set:case select_mode_extend:case select_mode_subtract:case select_mode_invert:case select_mode_intersect:
    case view_perspective:case view_top:case view_front:case view_right:case view_bottom:case view_back:case view_left:
        break; // Operators, routed above.
    case example_sponza:case example_scene_lit:case example_physic:case example_dungeons:return open_example(command);
    case cursor_origin:viewport_tool_.cursor={};viewport_tool_.cursor_rotation={};break;
    case snap_toggle:workspace_.viewport_settings.grid_snap=!workspace_.viewport_settings.grid_snap;break;
    case selection_grid:case selection_cursor: result=viewport_tool_.snap_selection(*document_,workspace_.viewport_settings,command==selection_cursor);break;
    case cursor_selected:result=viewport_tool_.cursor_to_selection(*document_);break;
    case cursor_grid:viewport_tool_.cursor_to_grid(workspace_.viewport_settings);break;
    case cursor_surface:workspace_.viewport_settings.cursor_surface=!workspace_.viewport_settings.cursor_surface;break;
    case snap_affect_move:workspace_.viewport_settings.snap_move=!workspace_.viewport_settings.snap_move;break;
    case snap_affect_rotate:workspace_.viewport_settings.snap_rotate=!workspace_.viewport_settings.snap_rotate;break;
    case snap_affect_scale:workspace_.viewport_settings.snap_scale=!workspace_.viewport_settings.snap_scale;break;
    case select_all:case select_none:case select_invert:
        result=viewport_tool_.select_all(*document_,command==select_all?editor::SelectAll::select:command==select_none?editor::SelectAll::deselect:editor::SelectAll::invert);break;
    case select_box:viewport_tool_.arm_box_select(pointer_position_,false);window_.capture(true);break;
    case clear_location:case clear_rotation:case clear_scale:
        result=viewport_tool_.clear(*document_,command==clear_location?editor::TransformChannel::location:
            command==clear_rotation?editor::TransformChannel::rotation:editor::TransformChannel::scale);break;
    case rename_selected: {
        if(workspace_.dock.focused_tab()==editor::control::assets_tab) return begin_asset_rename();
        // Houdini/Blender F2: rename in place on the Scene Tree row, revealing it first;
        // a filtered-out row edits the name in the Parameters header instead.
        const auto entity=document_->selection().entity;
        const auto shown=workspace_.dock.focused_tab()==editor::control::entities_tab && entity!=ObjectId{}?
            workspace_.reveal_in_tree(*document_,entity):Result<bool>{false};
        if(!shown) return std::unexpected(shown.error());
        if(*shown) {
            workspace_.row_rename=editor::Workspace::RowRename{entity,std::wstring(document_->name(entity))};
            pending_rename_focus_=true;
        } else {workspace_.inspecting_asset=false;workspace_.dock.select(editor::control::parameters_tab);pending_name_focus_=true;}
        break;
    }
    case unparent_selected: {
        const std::vector selected(document_->selected_entities().begin(),document_->selected_entities().end());
        for(const auto entity:selected) {
            const auto parent=world::parent_entity(document_->world().runtime(),entity);
            if(!parent) {result=std::unexpected(parent.error());break;}
            if(*parent==ObjectId{}) continue;
            if(auto changed=completion(document_->reparent(entity,ObjectId{}));!changed) {result=changed;break;}
        }
        break;
    }
    case viewport_options:workspace_.viewport_options_collapsed=!workspace_.viewport_options_collapsed;break;
    case show_fps:return action({ui::ActionKind::activate,editor::control::command_key(command),!workspace_.show_fps});
    case open_source: case edit_ui_source: case save_document: case close_source: case reload_source: case save_all:
    case new_source: case new_project_ui: case edit_project_ui:
        return source_command(command,asset_key);
    case build_project:case rebuild_project:case run_project:case stop_project:case restart_project:case show_project_ui:case show_output:
        return execution_command(command);
    case show_code: workspace_.dock.select(editor::source_control::tab);break;
    case new_project: case open_project: case save_scene: case save_scene_as: case open_scene: case import_mesh:case import_texture:case create_material: case refresh_assets:
        workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
        return project_command(command,asset_key);
    case exit: PostMessageW(window(),WM_CLOSE,0,0); break;
    case erase_selected: workspace_.inspecting_asset=false;result=completion(document_->erase_selected()); break;
    case erase_entities: workspace_.inspecting_asset=false;result=completion(document_->erase_entities()); break;
    case duplicate_selected: workspace_.inspecting_asset=false;result=completion(document_->duplicate_selected()); break;
    case show_scene: workspace_.dock.select(editor::control::scene_tab); break;
    case show_parameters: workspace_.dock.select(editor::control::parameters_tab); break;
    case show_controls: workspace_.dock.select(editor::control::workbench); break;
    case show_entities: workspace_.dock.select(editor::control::entities_tab); break;
    case show_structure: workspace_.dock.select(editor::control::structure_tab); break;
    case show_world_settings: workspace_.dock.select(editor::world_control::tab); break;
    case fit_probe_scene:case fit_probe_scene_focus: {
        auto fitted=fitted_probe_view(command==fit_probe_scene_focus);if(!fitted) return std::unexpected(fitted.error());
        return workspace_.set_view_settings(*document_,*fitted,lighting_focus());
    }
    case show_assets: workspace_.dock.select(editor::control::assets_tab); break;
    case previous_tab: case next_tab: workspace_.dock.cycle(command==previous_tab?-1:1); break;
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
        const auto color_cancelled=finish_color_edit(false);
        if(!color_cancelled) first_error+=L"; rollback failed: "+widen(color_cancelled.error().message);
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
    finish_camera_fly(false);camera_fly_release_.reset();
    camera_drag_.reset();
    workspace_.lighting.cancel();
    const auto color_cancelled=finish_color_edit(false);
    close_menu(false);
    effects(reply);
    if(!color_cancelled) workspace_.status+=L"; rollback failed: "+widen(color_cancelled.error().message);
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
    const auto frame=ui_.presented();
    const auto* source=frame?frame->find(request.source):nullptr;
    if (!source || !source->enabled || !live_table_context(*frame,request,false)) return;
    const auto next=request;
    const auto return_focus=editor::control::is_menu(next.source)?menu_return_focus_:ui_.focused_control();
    // The native menu owns subsequent mouse-up events. Retire the previous
    // gesture and accepted menu before publishing the replacement request.
    // Retain its accepted source and return focus across that cancellation.
    cancel_interaction(ui_.cancel_interaction());
    if(document_->editing() || viewport_tool_.active()) return;
    workspace_.menu=next; menu_return_focus_=return_focus;menu_incarnation_=source->incarnation;
    menu_requested_=std::chrono::steady_clock::now();
    menu_context_target_=source->context_target; menu_context_group_=source->context_group;
    menu_table_layout_=source->table_layout;
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
        if (ui_.presented()->popup==editor::control::viewport_pie) {
            effects(ui_.focus(pie_hot_));
            if (pie_release_pending_) {
                pie_release_pending_=false;
                if (pie_release_select_) {
                    ui::Event release;release.kind=ui::EventKind::key_up;
                    pie_input(release,request.group==editor::control::viewport_axis_pie?VK_OEM_3:'Z');
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
        if (at==items.end() || at->children.empty() || !at->enabled || !box ||
            box->bounds.x>=box->clip.right || box->bounds.x+box->bounds.width<=box->clip.left ||
            box->bounds.y>=box->clip.bottom || box->bounds.y+box->bounds.height<=box->clip.top) {
            menu_path_.resize(depth); break;
        }
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
    const bool navigate_top=top && menu_path_.empty() && input.kind==ui::EventKind::key_down &&
        (input.key==ui::Key::left || input.key==ui::Key::right);
    if (navigate_top && (input.key==ui::Key::left || !menu_window())) {
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
    // Right first uses the accepted popup's branch activation. Only a leaf
    // continues across the menubar through the ordinary request transition.
    if (navigate_top) change(editor::cycle_menu(source,false));
    return true;
}
bool Application::invoke_menu(const ui::Action& command,std::span<const ui::MenuItem> items) {
    const auto at=std::ranges::find(items,command.key,&ui::MenuItem::key);
    if (at==items.end() || !at->enabled || !at->children.empty()) return false;
    const auto edit=at->edit_command;
    const bool keep_open=command.keep_menu_open && at->behavior==ui::MenuBehavior::checkbox;
    if (edit) {
        const auto incarnation=menu_incarnation_;
        close_menu();effects(ui_.execute(*edit,incarnation));
    } else {
        auto activation=command;
        if (activation.kind==ui::ActionKind::activate &&
            (activation.target?activation.target:activation.key)==editor::control::menu_group_reset) {
            if (!workspace_.menu) return false;
            activation={ui::ActionKind::reset,workspace_.menu->group?workspace_.menu->group:workspace_.menu->source,{}};
        }
        // Retire the menu before a command can open a dialog or acquire capture
        // for a new tool. No menu-item reference survives this ownership change.
        if (!keep_open) close_menu();
        const auto result=action(activation);
        if (!result) {status(result,L"");if (keep_open) close_menu();}
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
    const bool release=input.kind==ui::EventKind::key_up &&
        native_key==(workspace_.menu->group==editor::control::viewport_axis_pie?VK_OEM_3:'Z');
    if (release && !pie_release_select_) {
        const auto dx=pointer_position_.x-workspace_.menu->x,dy=pointer_position_.y-workspace_.menu->y;
        pie_release_select_=dx*dx+dy*dy>324*window_.dpi_scale()*window_.dpi_scale() ||
            std::chrono::steady_clock::now()-menu_requested_>=std::chrono::milliseconds(200);
    }
    const auto* frame=ui_.presented().get();
    const auto* pie=frame?frame->find(editor::control::viewport_pie):nullptr;
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
    if(message.relative_motion) {ui_.dismiss_tooltip();look_camera_fly(message.relative_motion->x,message.relative_motion->y);return;}
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) { invalidate_surface(); return; }
    if (message.message==WM_MOVE) { ui_.dismiss_tooltip();finish_camera_fly(false);close_menu(false); return; }
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
    if (event.kind!=ui::EventKind::pointer_move) ui_.dismiss_tooltip();
    if (event.kind==ui::EventKind::pointer_leave) {effects(ui_.input(event));return;}
    const bool pointer=event.kind==ui::EventKind::pointer_move || event.kind==ui::EventKind::pointer_down ||
        event.kind==ui::EventKind::pointer_up || event.kind==ui::EventKind::wheel;
    if (event.kind==ui::EventKind::focus_lost || event.kind==ui::EventKind::capture_lost) {
        auto reply=ui_.input(event);
        if (ui_.presented()) workspace_.dock.focus_control(ui_.focused_control(),*ui_.presented());
        cancel_interaction(reply); return;
    }
    // Menu toggles must ignore repeats on both sides of menu_input(), including
    // repeats arriving after Escape has closed the original menu.
    if(event.kind==ui::EventKind::key_down && event.repeat &&
        (event.key==ui::Key::f10 || native_key==VK_MENU)) return;
    if(event.kind==ui::EventKind::pointer_down) camera_fly_release_.reset();
    if(camera_fly_release_ && event.kind==ui::EventKind::pointer_up && event.button==*camera_fly_release_) {
        camera_fly_release_.reset();return;
    }
    if(camera_fly_ && event.kind==ui::EventKind::key_down &&
        (event.key==ui::Key::f10 || event.alt || (event.control && (native_key==VK_TAB || native_key=='W'))))
        finish_camera_fly(false);
    if (menu_input(event,native_key)) {ui_.dismiss_tooltip();return;}
    if(pointer) pointer_position_={event.x,event.y};
    if((camera_fly_ || viewport_tool_.active()) && viewport_input(event,static_cast<std::uint32_t>(native_key))) {ui_.dismiss_tooltip();return;}
    // Scene Tree owns Clear Parent; text fields and other panes retain the
    // Project mnemonic. Resolve this context before opening a top-level menu.
    if(event.kind==ui::EventKind::key_down && native_key=='P' && event.alt && !event.control &&
        !ui_.wants_keyboard() && workspace_.dock.focused_tab()==editor::control::entities_tab) {
        if(event.repeat) return;
        if(auto result=execute(editor::Command::unparent_selected);!result) status(result,L"");
        return;
    }
    ui::ControlKey requested_menu{};
    if (event.kind==ui::EventKind::key_down) {
        if (event.key==ui::Key::f10 && !event.shift) requested_menu=editor::control::file_menu;
        else if (event.alt && !event.control) for (const auto& group:editor::command_groups)
            if (native_key==static_cast<WPARAM>(group.access_key)) { requested_menu=group.key; break; }
    }
    if (requested_menu) {
        if(event.repeat) return;
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
    if (pointer && !release && !pointer_ready()) {ui_.dismiss_tooltip();return;}
    if (event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::primary) {
        const auto menu=ui_.pointer_target(event.x,event.y);
        if (editor::control::is_menu(menu)) {
            effects(ui_.focus(menu));request_menu({menu,0,0,0});return;
        }
    }
    if (event.kind==ui::EventKind::key_down && !event.alt && !workspace_.menu &&
        (native_key==VK_F6 || native_key==VK_F8)) {
        if(event.repeat) return;
        const auto command=native_key==VK_F6?editor::Command::build_project:event.shift?editor::Command::stop_project:editor::Command::run_project;
        if(auto result=execute(command);!result) status(result,L"");return;
    }
    if (event.kind==ui::EventKind::key_down && !event.alt && !workspace_.menu &&
        ((event.control && (native_key=='S' || native_key=='O')) || native_key==VK_F5)) {
        if(event.repeat) return;
        const auto command=native_key=='O'?editor::Command::open_project:native_key==VK_F5?editor::Command::refresh_assets:
            event.shift?editor::Command::save_scene_as:editor::Command::save_document;
        status(execute(command),L"Project command completed");return;
    }
    if (event.kind==ui::EventKind::key_down && event.control && !event.alt && !workspace_.menu &&
        (native_key==VK_TAB || native_key=='B' || native_key=='W')) {
        if(event.repeat && native_key!=VK_TAB) return;
        cancel_interaction(ui_.cancel_interaction());
        const auto command=native_key==VK_TAB?(event.shift?editor::Command::previous_tab:editor::Command::next_tab):
            native_key=='B'?editor::Command::maximize_pane:editor::Command::close_tab;
        if (auto result=execute(command);!result) status(result,L"");
        return;
    }
    if(viewport_input(event,static_cast<std::uint32_t>(native_key))) {ui_.dismiss_tooltip();return;}
    auto reply = ui_.input(event);
    if (ui_.presented()) workspace_.dock.focus_control(ui_.focused_control(),*ui_.presented());
    effects(reply);
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        std::optional<editor::Command> command;
        if (event.control && (event.key==ui::Key::z || event.key==ui::Key::y))
            command=(event.key==ui::Key::y || event.shift)?editor::Command::redo:editor::Command::undo;
        else if (!workspace_.sample_history() && !event.repeat) {
            if (event.key==ui::Key::delete_key) command=editor::Command::erase_selected;
            else if (native_key==VK_F2 && !event.control && !event.alt) command=editor::Command::rename_selected;
            else if (native_key=='D' && (event.control || event.shift) && !event.alt && !ui_.wants_keyboard()) command=editor::Command::duplicate_selected;
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
    // The frame closes after run_step, so the retirement of its frame-local containers is included.
    timing::Recorder::Frame frame{recorder_};
    sampled_frame_.reset();
    const bool running=run_step(capture);
    auto& performance=workspace_.lighting.performance();
    performance.observe(frame.finish(),sampled_frame_);
    // Only a sampled step shows a reading, so readback and stalls never appear as the live FPS.
    const auto& recent=performance.snapshot().cpu.recent_interval;
    if(!sampled_frame_ || !workspace_.show_fps || !workspace_.dock.visible(editor::control::scene_tab) || !recent || *recent<=0)
        workspace_.viewport_fps.clear();
    else if(workspace_.viewport_fps.empty() || *recent!=fps_interval_) {
        fps_interval_=*recent;
        workspace_.viewport_fps=std::format(L"{:.1f} FPS  ·  {:.1f} ms",1000/ *recent,*recent);
    }
    return running;
}
bool Application::run_step(renderer::Capture* capture) try {
    using Clock = std::chrono::steady_clock;
    frame_statistics_ = {};
    if (closed_) return false;
    // Everything this step changed reaches Play as one batch, whichever way the step ends.
    struct ForwardLive {Application& app;~ForwardLive(){app.forward_live();}} forward_live{*this};
    {
        timing::Scope input{phases::input};
        window_.poll([&](NativeEvent&& event) { native(std::move(event)); });
        if (window_.closing()) {
            if (confirm_discard()) {close();return false;}
            window_.cancel_close();
        }
    }
    {timing::Scope project{phases::project};poll_project();}
    if(control_) {timing::Scope control{phases::control};control_->poll();}
    const auto title=(project_?project_->name():L"No project")+L" — "+(scene_path_.empty()?L"Untitled":scene_path_.filename().wstring())+
        (document_->dirty()?L" *":L"")+L" — FrostFox Atelier";
    if (title!=window_title_) { SetWindowTextW(window(),title.c_str());window_title_=title; }
    auto target = graphics(renderer_.target());
    const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) throw std::runtime_error("UI rejected the window surface.");
    effects(*updated);
    {
        timing::Scope collecting{phases::gc};
        auto gc = document_->collect_step();
        if (!gc) throw std::runtime_error(gc.error().message);
    }
    // Keep the last committed scopes while minimized; an empty surface cannot
    // validate a new layout or make its controls available for interaction.
    if (!target.drawable) { lighting_clock_.reset();MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE); return true; }
    // Only recoverable render failures enter this gate. Service work above
    // remains live, while input wakeups cannot turn a persistent error into
    // repeated UI/scene preparation. Normal successful frames have no cap.
    if(const auto now=Clock::now();now<render_retry_at_) {
        const auto wait=std::min(std::chrono::ceil<std::chrono::milliseconds>(render_retry_at_-now),std::chrono::milliseconds(20));
        MsgWaitForMultipleObjectsEx(0,nullptr,static_cast<DWORD>(wait.count()),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        return true;
    }
    update_camera_fly();
    const ui_layout::Size view_size{static_cast<float>(extent.width)/surface_.dpi_scale,static_cast<float>(extent.height)/surface_.dpi_scale};
    workspace_.camera_flying=camera_fly_.has_value();
    workspace_.viewport_mode=viewport_tool_.displayed_tool(workspace_.viewport_settings);
    workspace_.viewport_readout=viewport_tool_.readout();
    workspace_.camera_view=camera_.view();
    workspace_.view.grid_spacing=workspace_.viewport_settings.grid_spacing;
    const auto plane_normal=camera_.construction_normal();
    workspace_.view.grid_plane=plane_normal.z!=0?renderer::GridPlane::xy:
        plane_normal.x!=0?renderer::GridPlane::yz:renderer::GridPlane::xz;
    // After its pipelines are ready this is a bit check; before, the frame draws a fallback.
    if(const auto preparation=renderer_.preparation(control_?control_->view():workspace_.view);!preparation)
        workspace_.render_notice=L"Shader preparation failed: "+widen(preparation.error().cause.message);
    else workspace_.render_notice=preparation->ready?std::wstring{}:
        L"Compiling shaders "+std::to_wstring(preparation->done)+L"/"+std::to_wstring(preparation->total);
    const auto describe=[&] {
        // Selection/document commands can cancel the business edit before the next UI frame.
        if(workspace_.color_edit && !document_->owns_edit(editor::control::color_group)) workspace_.color_edit.reset();
        auto controls=workspace_.describe(*document_,view_size,ui_.presented().get(),project_?&*project_:nullptr,&ui_);
        if(workspace_.color_edit && (workspace_.color_edit->composition_failed ||
            std::ranges::none_of(controls,[](const auto& control){return control.key==editor::control::color_editor;}))) {
            const auto diagnostic=std::string(workspace_.ui_error());
            if(auto cancelled=finish_color_edit(false);!cancelled) throw std::runtime_error(cancelled.error().message);
            controls=workspace_.describe(*document_,view_size,ui_.presented().get(),project_?&*project_:nullptr,&ui_);
            workspace_.status=L"Colour editor could not open"+(diagnostic.empty()?std::wstring{}:L": "+widen(diagnostic));
        }
        return controls;
    };
    std::optional<timing::Scope> stage{std::in_place,phases::ui};
    auto controls = describe();
    auto described = ui_.set_controls(controls);
    if (!described && workspace_.reject_ui_controls("UI candidate rejected at control "+std::to_string(described.error().key))) {
        controls=describe();
        described=ui_.set_controls(controls);
    }
    if (!described) throw std::runtime_error("UI rejected editor controls (key " + std::to_string(described.error().key) + ").");
    effects(*described);
    if (workspace_.pie_menu()) effects(ui_.focus(pie_hot_));
    stage.emplace(phases::layout);
    auto prepared = ui_.prepare();
    if (!prepared && workspace_.reject_ui_controls("Editor layout candidate rejected at control "+std::to_string(prepared.error().key))) {
        controls=describe();
        described=ui_.set_controls(controls);
        if (!described) throw std::runtime_error("UI rejected restored editor controls (key " + std::to_string(described.error().key) + ").");
        effects(*described);
        prepared=ui_.prepare();
    }
    if (!prepared) {
        stage.reset();
        lighting_clock_.reset();
        if (!ui_.presented()) throw std::runtime_error("Initial editor frame preparation failed (UI code " + std::to_string(static_cast<int>(prepared.error().code)) + ").");
        workspace_.status = L"Frame preparation failed; previous frame retained";
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    const auto previous_ui_revision=workspace_.ui_revision();
    workspace_.commit_ui_controls();
    if (workspace_.ui_revision()!=previous_ui_revision) {
        ui_.dismiss_tooltip();
        if ((*prepared)->tooltip) {
            prepared=ui_.prepare();
            if (!prepared) {
                stage.reset();
                workspace_.status=L"Frame preparation failed; previous frame retained";
                MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                return true;
            }
        }
    }
    // Native popups render on their retained surface before scene submission /
    // Present can block. Their anchor still comes from accepted owner geometry.
    // A pending keyboard reveal waits for this frame's acceptance; closing here would drop it.
    stage.reset();
    if (!workspace_.pie_menu() && !pending_menu_) {timing::Scope menus{phases::popup};update_menu();}
    const auto& frame = **prepared;
    const auto* viewport = frame.find(editor::control::viewport);
    const auto scene_bounds=viewport?viewport->bounds:ui::Rect{};
    if(camera_fly_ && (!viewport || (accepted_view_ && accepted_view_->bounds!=scene_bounds))) finish_camera_fly(false);
    const bool had_viewport_gesture=viewport_tool_.active();
    if(had_viewport_gesture && accepted_view_ && accepted_view_->bounds!=scene_bounds)
        status(viewport_tool_.cancel(*document_),L"Viewport changed; edit cancelled");
    viewport_tool_.forget(*document_);
    if(had_viewport_gesture && !viewport_tool_.active()) window_.capture(false);
    {
        timing::Scope snapshot{phases::scene};
        if(viewport) take(scene_.refresh(*document_));
        // Any edit of the Environment (Parameters, deletion, Play) reaches the evaluated view before rendering;
        // only such a change re-anchors the lighting focus.
        if(workspace_.sync_world(*document_)) sync_world_view();
    }
    for(const auto& item:document_->take_diagnostics()) workspace_.log_repeated(editor::describe_diagnostic(*document_,item));
    const auto camera = camera_.matrices(std::max(scene_bounds.width,1.f) / std::max(scene_bounds.height,1.f));
    const auto viewport_view=editor::ViewportView::make(camera_,scene_bounds,window_.dpi_scale());
    overlay_.clear();
    if(viewport) viewport_tool_.overlay(*document_,viewport_view,workspace_.viewport_settings,scene_.mesh_entities,overlay_);
    const bool projection_cut=accepted_view_ && accepted_view_->orthographic!=viewport_view.orthographic;
    renderer::Capture control_capture;
    const bool capturing_control=control_ && control_->wants_capture();
    auto* frame_capture=capture?capture:capturing_control?&control_capture:nullptr;
    const auto effective_view=control_?control_->view():workspace_.view;
    const auto lighting_focus=camera_.state().target;
    renderer::Frame render_frame{*frame.drawing, {camera.view.elements, camera.projection.elements}, scene_bounds,
        viewport?std::span<const renderer::MeshInstance>{scene_.meshes}:std::span<const renderer::MeshInstance>{},effective_view,overlay_,
        viewport?std::span<const renderer::Light>{scene_.lights}:std::span<const renderer::Light>{},scene_.identity,camera_cut_ || projection_cut,
        rendering::sky_radiance(effective_view.lit),{lighting_focus.x,lighting_focus.y,lighting_focus.z}};
    const auto lighting_start=Clock::now();
    const bool lighting_active=viewport && scene_bounds.width>0 && scene_bounds.height>0 && !scene_.meshes.empty() &&
        effective_view.shading==renderer::Shading::scene_lit && effective_view.wireframe!=renderer::Wireframe::only && effective_view.lit.ddgi;
    render_frame.render_delta_seconds=lighting_clock_.delta(lighting_start,lighting_active);
    auto drawn = lit_fallback_.render(renderer_, render_frame, frame_capture);
    if (!drawn) {
        lighting_clock_.reset();
        if(control_) control_->render_failed(drawn.error().cause.message);
        if (drawn.error().terminal) throw std::runtime_error("Renderer stopped: " + drawn.error().cause.message);
        render_retry_delay_=std::min(render_retry_delay_.count()?render_retry_delay_*2:std::chrono::milliseconds(20),std::chrono::milliseconds(250));
        render_retry_at_=Clock::now()+render_retry_delay_;
        workspace_.status = widen(drawn.error().cause.message); return true;
    }
    const auto& rendered = drawn->result;
    auto drawn_frame = render_frame;drawn_frame.view = drawn->view;
    frame_statistics_ = rendered.stats;
    workspace_.lighting.performance().observe(drawn_frame,rendered);
    // Control sees a fallback as the requested frame not being ready, so it is neither a Lit sample nor its last frame.
    if(control_ && drawn->fallback) {
        if(!drawn->failure.empty()) control_->render_failed(drawn->failure);
        renderer::FrameResult not_ready;not_ready.status=renderer::FrameStatus::not_ready;control_->rendered(render_frame,not_ready,nullptr);
    }
    if (rendered.status == renderer::FrameStatus::accepted) {
        if (rendered.accepted_id != frame.drawing->id || rendered.accepted_extent != frame.drawing->extent)
            throw std::runtime_error("Renderer accepted a mismatched frame.");
        if (!ui_.accept(*prepared)) throw std::runtime_error("UI rejected an accepted renderer frame.");
        render_retry_at_={};render_retry_delay_={};
        lighting_clock_.accept(lighting_start,lighting_active && drawn->view.shading==renderer::Shading::scene_lit);
        fallback_presented_=drawn->fallback;
        accepted_view_=viewport?std::optional(viewport_view):std::nullopt;
        workspace_.lighting.observe(rendered.stats);
        if(viewport) camera_cut_=false;
        ++accepted_frames_;
        if(!frame_capture) sampled_frame_=rendered.accepted_id;
        if(control_ && !drawn->fallback) {
            if(capture && capturing_control) {control_capture=*capture;control_->rendered(render_frame,rendered,&control_capture);}
            else control_->rendered(render_frame,rendered,frame_capture);
        }
        if (pending_source_focus_) {
            pending_source_focus_=false;
            if (workspace_.source_editor.active()) effects(ui_.focus(editor::source_control::buffer));
        }
        // A revealed row can take a frame to be described and presented.
        if (pending_rename_focus_ && (!workspace_.row_rename || (ui_.presented() && ui_.presented()->find(editor::control::inline_rename)))) {
            pending_rename_focus_=false;
            if (workspace_.row_rename) effects(ui_.focus(editor::control::inline_rename));
        }
        if (pending_name_focus_) {
            pending_name_focus_=false;
            if (const auto key=workspace_.name_control(document_->selection().entity);key && ui_.presented() && ui_.presented()->find(key))
                effects(ui_.focus(key));
        }
        if (pending_parameter_search_focus_) {
            pending_parameter_search_focus_=false;
            if (workspace_.parameter_search_visible) effects(ui_.focus(editor::control::parameter_search));
        }
        sync_text_input();
        timing::Scope menus{phases::popup};
        if (pending_menu_) {
            const auto menu=pending_menu_; pending_menu_=0;
            effects(ui_.focus(menu)); request_menu({menu,0,0,0});
        } else update_menu();
    } else {
        lighting_clock_.reset();
        if(control_ && !drawn->fallback) control_->rendered(render_frame,rendered,nullptr);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return true;
} catch(const std::exception& error) {
    preserve_failure(error.what());throw;
} catch(...) {
    preserve_failure("Unknown editor frame failure");throw;
}
void Application::close() {
    if (closed_) return;
    finish_camera_fly(false);
    closed_ = true;
    control_.reset();
    play_.close();static_cast<void>(live_.close());
    if(!play_snapshot_.empty()) {std::error_code ignored;std::filesystem::remove(play_snapshot_,ignored);play_snapshot_.clear();}
    close_menu(false);
    for (auto& submenu:submenus_) submenu->close();
    if (popup_) popup_->close();
    cancel_interaction(ui_.cancel_interaction());
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
