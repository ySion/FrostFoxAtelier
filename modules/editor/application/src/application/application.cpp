#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/presentation.hpp"
#include "application/diagnostics.hpp"
#include "editor/error.hpp"
#include "editor/keymap.hpp"
#include "procedural/graph.hpp"
#include "foundation/result.hpp"
#include "logging/native.hpp"
#include "memory/fatal.hpp"
#include "assets/file.hpp"
#include "json/json.hpp"
#include "atelier/build_info.hpp"
#include "platform/utf.hpp"
#include <windowsx.h>
#include <shellapi.h>
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
Result<void> Application::completion(Result<void> result) {
    return result;
}

Result<void> Application::completion(Result<world::Commit> result) {
    if (!result)
        return std::unexpected(result.error());
    if (result->notification_error) {
        notice("editor.committed_with_notification_failure",
               L"Operation committed; lifecycle notification failed. The committed change remains applied.",
               logging::Severity::warning);
        return std::unexpected(std::move(*result->notification_error));
    }
    return {};
}

namespace {
template <class T>
T take(Result<T> result) {
    if (!result)
        throw editor::detail::Failure(std::move(result.error()));
    return std::move(*result);
}

void take(Result<void> result) {
    if (!result)
        throw editor::detail::Failure(std::move(result.error()));
}

template <class T>
T graphics(renderer::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().cause.message);
    return std::move(*result);
}

Result<Scalar> scalar_value(const Scalar& prototype, double value) {
    return std::visit(
        [&](const auto& original) -> Result<Scalar> {
            using T = std::decay_t<decltype(original)>;
            const auto invalid = [] {
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "The value is outside this property's type or range."});
            };
            if (!std::isfinite(value))
                return invalid();
            if constexpr (std::same_as<T, EnumValue>) {
                return invalid(); // Enumerations use named choices, never numeric editing.
            } else if constexpr (std::same_as<T, bool>) {
                if (value != 0 && value != 1)
                    return invalid();
                return Scalar{value != 0};
            } else {
                if (value < static_cast<double>(std::numeric_limits<T>::lowest()) ||
                    value > static_cast<double>(std::numeric_limits<T>::max()))
                    return invalid();
                if constexpr (std::is_integral_v<T>)
                    if (std::trunc(value) != value)
                        return invalid();
                return Scalar{static_cast<T>(value)};
            }
        },
        prototype);
}

bool live_table_context(const ui::UiFrame& frame, const ui::ContextMenuRequest& request, bool require_selection) {
    if (!request.table)
        return true;
    const auto* table = frame.find_table(request.source);
    if (!table || !table->data || (require_selection && table->selection.row != request.table->row))
        return false;
    const auto& rows = table->data->rows;
    const auto row = std::ranges::find(rows, request.table->row, &ui::TableRow::key);
    return row != rows.end() && row->enabled &&
           std::ranges::find(table->data->columns, request.table->column, &ui::TableColumn::key) !=
               table->data->columns.end();
}

ui::PixelExtent client_extent(HWND window) {
    if (IsIconic(window))
        return {};
    RECT bounds{};
    GetClientRect(window, &bounds);
    return {static_cast<std::uint32_t>(bounds.right), static_cast<std::uint32_t>(bounds.bottom)};
}
}

Application::Application() = default;

Result<void> Application::initialize(std::unique_ptr<editor::Session> document, bool validation, bool activate,
                                     std::optional<ui::PixelExtent> initial_client, bool vsync) {
    document_ = std::move(document);
    gpu_validation_ = validation;
    vsync_ = vsync;
    ATELIER_TRY(window_.initialize(nullptr, initial_client));
    ATELIER_TRY(ui_.initialize(editor_fonts()).transform_error(ui_error));
    require_frame_timing();
    auto style = ui_.style();
    style.font.family = L"Sarasa UI SC";
    style.font.size = 12.5f;
    style.caption_scale = 1;
    style.title_scale = 7.f / 6;
    if (auto styled = ui_.set_style(style); !styled) {
        auto error = ui_error(styled.error());
        error.message = "Cannot publish editor typography: " + error.message;
        return std::unexpected(std::move(error));
    }
    ATELIER_TRY_BIND(styles, editor_styles());
    style_sheet_ = std::move(*styles);
    if (auto styled = ui_.set_style_sheet(style_sheet_); !styled) {
        auto error = ui_error(styled.error());
        error.message = "Cannot publish editor style sheet: " + error.message;
        return std::unexpected(std::move(error));
    }
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    options.vsync = vsync_;
    options.frame_latency_waitable = true;
    options.shader_cache = shader_cache_directory();
    ATELIER_TRY_BIND(renderer, renderer::Renderer::open(window_.handle(), options).transform_error(renderer_error));
    renderer_ = std::move(*renderer);
    auto popup = std::make_unique<PopupHost>();
    ATELIER_TRY(popup->initialize(window(), editor_fonts(), gpu_validation_));
    popup_ = std::move(popup);
    auto submenu = std::make_unique<PopupHost>();
    ATELIER_TRY(submenu->initialize(window(), editor_fonts(), gpu_validation_));
    submenus_.push_back(std::move(submenu));
    closed_ = false;
    window_.show(activate);
    return {};
}

Application::~Application() {
    if (std::uncaught_exceptions())
        preserve_failure("Unhandled editor failure outside the frame loop");
    // External cleanup errors remain diagnostics. World/Renderer destructors
    // enforce fatal only when their own necessary retirement cannot complete.
    if (auto result = close(); !result)
        report_error(result.error(), "application.cleanup_failed");
}

void Application::notice(std::string_view code, std::wstring_view message, logging::Severity severity) noexcept {
    auto text = platform::utf16_to_utf8(message);
    if (!text) {
        std::fputs("Editor notice capture failed.\n", stderr);
        return;
    }
    auto root = platform::utf16_to_utf8(document_root_.generic_wstring());
    if (!root) {
        std::fputs("Editor notice capture failed.\n", stderr);
        return;
    }
    logging::Entry entry{
        .severity = severity, .category = "application", .event_code = std::string(code), .message = std::move(*text)};
    entry.fields = {{"document", document_->presentation().identity()}, {"project_root", std::move(*root)}};
    static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
}

void Application::diagnostics() noexcept {
    for (const auto& item : document_->take_diagnostics()) {
        auto entry = world_diagnostic(*document_, item, document_root_);
        if (!entry) {
            std::fputs("Editor diagnostic capture failed.\n", stderr);
            return;
        }
        static_cast<void>(workspace_.output.submit_owned(std::move(*entry)));
    }
    const auto total = document_->world().dropped_diagnostics();
    if (total != dropped_diagnostics_) {
        logging::Entry entry{.severity = logging::Severity::warning,
                             .category = "world",
                             .event_code = "world.diagnostics_dropped",
                             .message = "World diagnostics exceeded their retention capacity."};
        entry.fields = {{"document", document_->presentation().identity()},
                        {"dropped_total", total},
                        {"dropped_delta", total - dropped_diagnostics_}};
        static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
        dropped_diagnostics_ = total;
    }
}

void Application::preserve_failure(std::string_view reason) noexcept {
    if (recovery_.attempted() || !document_ || closed_)
        return;
    try {
        static_cast<void>(logging::submit({.severity = logging::Severity::critical,
                                           .category = "application",
                                           .event_code = "application.failure_preserved",
                                           .message = std::string(reason)}));
    } catch (...) {
        std::fputs("Cannot capture the editor's original failure.\n", stderr);
    }
    try {
        window_.capture(false);
        window_.cursor(ui::Cursor::arrow);
        static_cast<void>(
            recovery_.preserve(*document_, project_ ? project_->root() : std::filesystem::path{}, scene_path_, reason));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Scene recovery failed: %s\n", error.what());
    } catch (...) {
        std::fputs("Scene recovery failed with an unknown error\n", stderr);
    }
}

void Application::report_error(const Error& error, std::string_view event, logging::Severity severity) noexcept {
    auto entry = error_diagnostic(error, event, document_root_);
    if (!entry) {
        std::fputs("Cannot capture editor error; business result is unchanged.\n", stderr);
        return;
    }
    entry->severity = severity;
    static_cast<void>(workspace_.output.submit_owned(std::move(*entry)));
}

void Application::status(Result<void> result, const wchar_t* success) {
    if (!result)
        report_error(result.error());
    workspace_.status = result ? success : diagnostic_text(result.error().message);
}

void Application::status(Result<world::Commit> result, const wchar_t* success) {
    status(completion(std::move(result)), success);
}

void Application::sync_world_view() {
    workspace_.sync_world(*document_);
    // Undo/reopen may restore a finer follow-focus lattice. The scene remains
    // authoritative; re-anchor this view if its old target cannot represent it.
    if (rendering::scene_lighting_focus_error(workspace_.view.lit, lighting_focus())) {
        auto state = camera_.state();
        state.target = {};
        camera_.set_state(state);
        camera_cut_ = true;
    }
}

math::Vector3 Application::lighting_focus() const {
    return camera_.state().target;
}

Result<void> Application::set_camera(const editor::Camera& camera, bool cut) {
    if (const auto* error = renderer::probe_focus_error(workspace_.view.lit, camera.state().target))
        return std::unexpected(Error{ErrorCode::invalid_definition, error});
    if (camera.state() != camera_.state()) {
        camera_ = camera;
        camera_cut_ |= cut;
    }
    return {};
}

const ui::ControlBox* Application::command_focus() const noexcept {
    const auto& frame = ui_.presented();
    return frame ? frame->find(workspace_.menu ? menu_return_focus_ : ui_.focused_control()) : nullptr;
}

editor::Context Application::operator_context(bool interactive) {
    auto context =
        workspace_.operator_context(*document_, project_ ? &*project_ : nullptr, interactive, command_focus());
    context.cursor = viewport_tool_.cursor;
    return context;
}

Result<editor::OperatorReport> Application::call_operator(const editor::OperatorCall& request, bool interactive,
                                                          const editor::ResolveReference& resolve) {
    const auto shown = workspace_.status;
    auto report = editor::call(workspace_.operators, *this, operator_context(interactive), request, resolve);
    // call owns Runtime-backed values. Only now may a host operation retire their Session.
    auto replacement = take_document_request();
    if (report && report->status == editor::OperatorStatus::finished && replacement) {
        if (!has_any(workspace_.operators.find(request.id)->flags, editor::OperatorFlag::replaces_document))
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Only a replacing operator may request a document replacement."});
        ATELIER_TRY(apply_document_request(*replacement));
        if (operator_cancelled())
            report->status = editor::OperatorStatus::cancelled;
    }
    // An operator whose host work reported its own status keeps it.
    if (report && report->status == editor::OperatorStatus::finished && workspace_.status == shown)
        workspace_.status = workspace_.operators.find(request.id)->title;
    return report;
}

Result<void> Application::run_operator(const editor::OperatorCall& call, bool report_unavailable, bool interactive) {
    ATELIER_TRY_BIND(report, call_operator(call, interactive));
    if (report->status == editor::OperatorStatus::interaction_required ||
        (report->status == editor::OperatorStatus::failed && report_unavailable))
        return std::unexpected(Error{ErrorCode::unsupported_operation, report->message});
    return {};
}

// Command transitions retire the old owner before releasing native capture;
// cleanup must run before the command can establish a new gesture.
Result<void> Application::end_interaction() {
    if (camera_drag_ || workspace_.network.active_gesture())
        cancel_interaction(ui_.cancel_interaction());
    ATELIER_TRY(viewport_tool_.cancel(*document_));
    if (!workspace_.menu)
        window_.capture(false);
    return {};
}

Result<void> Application::source_history(bool redo) {
    ATELIER_TRY(workspace_.source_editor.active()->document->edit().history(redo).transform_error(ui_error));
    pending_source_focus_ = true;
    return {};
}

Result<void> Application::close_tab(editor::RegionKind region, bool interactive) {
    const auto tab = editor::region_tab(region);
    if (!tab)
        return {};
    if (const auto* file = workspace_.source_editor.active(); region == editor::RegionKind::code && file) {
        if (!interactive && file->dirty())
            return std::unexpected(
                Error{ErrorCode::unsupported_operation,
                      "The Code page's active document has unsaved changes; save or reload it first."});
        return close_source(interactive);
    }
    workspace_.dock.close(tab);
    return {};
}

void Application::maximize_tab(editor::RegionKind region) {
    if (const auto tab = editor::region_tab(region))
        workspace_.dock.maximize(tab);
}

void Application::show_tab(editor::RegionKind region) {
    workspace_.dock.select(editor::region_tab(region));
}

void Application::cycle_tab(editor::RegionKind region, int direction) {
    show_tab(region);
    workspace_.dock.cycle(direction);
}

Result<void> Application::workbench_history(bool redo) {
    if (redo)
        workspace_.workbench.redo();
    else
        workspace_.workbench.undo();
    return {};
}

void Application::scene_changed() {
    if (!workspace_.active_graph.empty())
        exit_graph_context();
    workspace_.inspecting_asset = false;
    workspace_.inspecting_network = false;
    sync_world_view();
}

Result<void> Application::set_view_settings(const renderer::ViewSettings& settings) {
    ATELIER_TRY(workspace_.set_view_settings(*document_, settings, lighting_focus()));
    workspace_.viewport_settings.grid_spacing = settings.grid_spacing;
    return {};
}

Result<renderer::ViewSettings> Application::fitted_probe_view(bool follow_focus) const {
    ATELIER_TRY(document_->sync_procedural_generators(project_ ? const_cast<editor::Project*>(&*project_) : nullptr));
    ATELIER_TRY_BIND(items, document_->presentation().render_items());
    if (items->empty())
        return std::unexpected(Error{ErrorCode::invalid_definition, "DDGI volume fitting requires rendered geometry."});
    auto world_bounds = math::Bounds::empty();
    for (const auto& item : *items)
        world_bounds = math::encapsulate(world_bounds, math::transform_bounds(item.geometry->bounds(), item.world));
    if (math::is_empty(world_bounds) || !math::is_finite(world_bounds))
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "DDGI volume fitting requires finite scene bounds."});
    auto candidate = workspace_.view;
    auto& lighting = candidate.lit;
    lighting.probe_volumes = {};
    auto& coarse = lighting.probe_volumes[follow_focus ? 1 : 0];
    coarse.enabled = true;
    for (unsigned axis = 0; axis < 3; ++axis) {
        coarse.minimum[axis] = std::floor(world_bounds.minimum[axis] - .5f);
        coarse.maximum[axis] = std::ceil(world_bounds.maximum[axis] + .5f);
        const double span = double(coarse.maximum[axis]) - double(coarse.minimum[axis]);
        coarse.counts[axis] =
            static_cast<std::uint32_t>(std::clamp(std::ceil(span * .5) + 1., 2., follow_focus ? 12. : 16.));
    }
    if (follow_focus) {
        auto& focused = lighting.probe_volumes[0];
        focused.enabled = true;
        focused.placement = renderer::ProbePlacement::follow_focus;
        focused.minimum = {-6, -2, -6};
        focused.maximum = {6, 4, 6};
        focused.counts = {13, 7, 13};
        focused.blend_cells = 1;
    }
    ATELIER_TRY(editor::validate_view_options(candidate, lighting_focus()));
    return candidate;
}

Result<void> Application::configure_view(math::Vector3 eye, math::Vector3 target, renderer::Shading shading) {
    workspace_.sync_world(*document_);
    auto candidate = workspace_.view;
    auto camera = camera_;
    if (shading > renderer::Shading::scene_lit || !camera.look_at(eye, target))
        return std::unexpected(Error{ErrorCode::invalid_definition, "Invalid initial camera or shading mode."});
    candidate.shading = shading;
    ATELIER_TRY(editor::validate_view_options(candidate, target));
    ATELIER_TRY(workspace_.set_view_settings(*document_, candidate, target));
    camera_ = camera;
    camera_cut_ = true;
    invalidate_surface();
    return {};
}

Result<void> Application::action(const ui::Action& source) {
    // UI instance keys and host command/target keys occupy different roles.
    // Resolve once at the existing dispatch boundary; business checks stay below.
    std::optional<ui::Action> routed;
    if (source.target)
        routed.emplace(source.kind, source.target, source.value);
    const auto& event = routed ? *routed : source;
    if (event.key == editor::output_control::locate && event.kind == ui::ActionKind::activate) {
        auto located = workspace_.locate_output();
        if (located)
            pending_source_focus_ = true;
        return located;
    }
    if (event.key == editor::output_control::open_directory && event.kind == ui::ActionKind::activate) {
        const auto directory = workspace_.output.selected_directory();
        if (directory.empty())
            return std::unexpected(Error{ErrorCode::invalid_storage, "This record has no persisted run directory."});
        const auto opened = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(window(), L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (opened <= 32)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Cannot open the original run directory (Shell " +
                                                                         std::to_string(opened) + ")."});
        return {};
    }
    if (workspace_.output.apply(event))
        return {};
    if (workspace_.spreadsheet.apply(event))
        return {};
    using namespace editor::control;
    const auto invalid = [] {
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "UI action does not match its live binding or value type."});
    };
    const auto done = [&](auto result, const wchar_t* message) -> Result<void> {
        auto finished = completion(std::move(result));
        if (finished)
            workspace_.status = message;
        return finished;
    };
    if (event.kind == ui::ActionKind::context_menu) {
        const auto* request = std::get_if<ui::ContextMenuRequest>(&event.value);
        if (!request)
            return invalid();
        // A row menu acts on the selection: right-clicking outside it selects that row first.
        if (const auto* row = workspace_.binding(request->source);
            row && row->kind == editor::Workspace::Binding::Kind::object && row->path.empty() &&
            row->object != ObjectId{} && !document_->is_selected(row->object))
            ATELIER_TRY(workspace_.select_tree(*document_, *this, request->source));
        request_menu(*request);
        return {};
    }
    if (event.kind == ui::ActionKind::dismiss && event.key == editor::control::color_editor)
        return finish_color_edit(true);
    if (event.key == editor::control::inline_rename && workspace_.row_rename) {
        if (event.kind == ui::ActionKind::edit_preview) {
            if (const auto* text = std::get_if<std::wstring>(&event.value))
                workspace_.row_rename->text = *text;
            return {};
        }
        if (event.kind != ui::ActionKind::edit_commit && event.kind != ui::ActionKind::edit_cancel)
            return {};
        const auto rename = std::exchange(workspace_.row_rename, std::nullopt);
        if (!rename->asset.empty()) {
            const auto* text = std::get_if<std::wstring>(&event.value);
            if (event.kind == ui::ActionKind::edit_cancel || !text || *text == rename->asset.filename().wstring() ||
                !project_)
                return {};
            ATELIER_TRY_BIND(renamed, project_->rename(rename->asset, *text));
            workspace_.assets.reselect(*renamed);
            workspace_.status = L"Renamed to " + renamed->filename().wstring();
            return {};
        }
        effects(ui_.focus(workspace_.object_control(rename->object)));
        const auto* text = std::get_if<std::wstring>(&event.value);
        if (event.kind == ui::ActionKind::edit_cancel || !text || *text == document_->name(rename->object))
            return {};
        auto name = platform::utf16_to_utf8(*text);
        if (!name)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
        json::Builder args;
        args.set_validated(args.root(), "name", *name);
        return done(
            run_operator({"scene.rename",
                          args.finish(),
                          {.selection = editor::SelectionContext{rename->object, rename->object, {rename->object}},
                           .region = {}}},
                         true, false),
            L"Entity renamed / Undo restores its name");
    }
    if (event.kind == ui::ActionKind::dismiss) {
        close_menu();
        return {};
    }
    if (workspace_.color_edit)
        if (auto handled = color_edit_action(event); !handled || *handled)
            return handled ? Result<void>{} : Result<void>{std::unexpected(handled.error())};
    if (event.kind == ui::ActionKind::activate && (event.key == menu_bar_handle || event.key == shelf_handle)) {
        close_menu();
        if (event.key == shelf_handle) {
            const auto* collapsed = std::get_if<bool>(&event.value);
            if (!collapsed)
                return invalid();
            workspace_.shelf_collapsed = *collapsed;
        } else
            workspace_.menu_bar_collapsed = !workspace_.menu_bar_collapsed;
        return {};
    }
    if (event.kind == ui::ActionKind::activate && is_menu(event.key)) {
        request_menu({event.key, 0, 0, 0});
        return {};
    }
    if (event.kind == ui::ActionKind::activate && workspace_.source_editor.select(event.key)) {
        close_menu(false);
        workspace_.dock.select(editor::source_control::tab);
        pending_source_focus_ = true;
        return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::source_control::problem) {
        ATELIER_TRY_BIND(located, workspace_.source_editor.locate_problem());
        if (*located)
            pending_source_focus_ = true;
        return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::new_scratch) {
        ATELIER_TRY(document_->create_scratch_graph());
        return open_graph_context("scratch/procedural");
    }
    const std::string net_graph = workspace_.effective_network_graph(*document_, project_ ? &*project_ : nullptr);
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::pin_toggle) {
        if (!workspace_.network_pinned) {
            if (!net_graph.empty()) {
                workspace_.network_pinned = true;
                workspace_.pinned_graph = net_graph;
                workspace_.status = L"Pinned Network graph";
            }
        } else {
            workspace_.network_pinned = false;
            workspace_.pinned_graph.clear();
            workspace_.status = L"Network follows selected Generator Entity";
        }
        return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::isolate_toggle) {
        if (!workspace_.active_graph.empty()) {
            const std::string prev = workspace_.active_graph;
            exit_graph_context();
            if (workspace_.selected_generator_graph(*document_) != prev && document_->has_graph(prev)) {
                workspace_.network_pinned = true;
                workspace_.pinned_graph = prev;
            }
            workspace_.status = L"Returned 3D Viewport to World scene";
            return {};
        }
        if (!net_graph.empty())
            return open_graph_context(net_graph);
        return {};
    }
    if (event.kind == ui::ActionKind::activate)
        if (const auto choice = workspace_.network_path_choice(event.key)) {
            using Kind = editor::Workspace::NetworkPathChoice::Kind;
            if (choice->kind == Kind::unpin_world) {
                if (!workspace_.active_graph.empty())
                    exit_graph_context();
                workspace_.network_pinned = false;
                workspace_.pinned_graph.clear();
                workspace_.inspecting_network = false;
                workspace_.status = L"Network follows selected Generator Entity";
                return {};
            }
            if (choice->kind == Kind::follow_entity) {
                if (!workspace_.active_graph.empty())
                    exit_graph_context();
                workspace_.network_pinned = false;
                workspace_.pinned_graph.clear();
                workspace_.inspecting_network = false;
                return done(workspace_.select_object(*document_, *this, choice->entity), L"Generator Entity selected");
            }
            if (!workspace_.active_graph.empty())
                return open_graph_context(choice->graph_key);
            if (!document_->has_graph(choice->graph_key)) {
                if (!project_)
                    return invalid();
                ATELIER_TRY(document_->open_graph(*project_, choice->graph_key));
            }
            workspace_.network_pinned = true;
            workspace_.pinned_graph = choice->graph_key;
            workspace_.set_single_graph_selection({});
            workspace_.inspecting_network = false;
            workspace_.bump_view_revision();
            workspace_.status = L"Pinned Network graph";
            return {};
        }
    const auto network_canvas_dip = [&]() {
        ui_layout::Size canvas_dip{640.f, 360.f};
        if (ui_.presented())
            if (const auto* box = ui_.presented()->find(editor::network_control::canvas);
                box && box->bounds.width > 0.f && box->bounds.height > 0.f)
                canvas_dip = {box->bounds.width / window_.dpi_scale(), box->bounds.height / window_.dpi_scale()};
        return canvas_dip;
    };
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::add_node) {
        if (net_graph.empty() || !document_->has_graph(net_graph) || document_->world().running())
            return {};
        const auto canvas_dip = network_canvas_dip();
        std::string from_node;
        std::string from_port;
        if (!workspace_.selected_graph_node.empty())
            if (const auto inspected = document_->graph_topology(net_graph)) {
                const auto at = std::ranges::find((*inspected)->nodes, workspace_.selected_graph_node,
                                                  &editor::GraphNodeView::name);
                if (at != (*inspected)->nodes.end() && !at->ports.outputs.empty()) {
                    from_node = at->name;
                    from_port = at->ports.outputs.front();
                }
            }
        if (!source.target && workspace_.menu && workspace_.menu->source == editor::network_control::canvas) {
            const auto dip =
                workspace_.network.to_dip(workspace_.network_context_pos[0], workspace_.network_context_pos[1]);
            workspace_.network.open_tab_search(dip[0], dip[1], canvas_dip, std::move(from_node), std::move(from_port));
        } else {
            workspace_.network.open_tab_search(std::max(12.f, canvas_dip.width * .5f - 120.f),
                                               std::min(72.f, std::max(12.f, canvas_dip.height * .22f)), canvas_dip,
                                               std::move(from_node), std::move(from_port));
        }
        workspace_.dock.select(editor::network_control::tab);
        pending_network_search_focus_ = true;
        workspace_.status = L"Tab search · Type to filter · Up/Down select · Enter create · Esc cancel";
        return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key >= editor::network_control::tab_pick_first &&
        event.key < editor::network_control::tab_pick_first + editor::NetworkCanvas::max_tab_results) {
        if (net_graph.empty() || !document_->has_graph(net_graph) || !workspace_.network.tab_search().open)
            return {};
        ATELIER_TRY_BIND(catalog, document_->procedural_node_types());
        ATELIER_TRY_BIND(created, workspace_.network.commit_tab_pick(
                                      *document_, net_graph, *catalog,
                                      static_cast<std::size_t>(event.key - editor::network_control::tab_pick_first),
                                      workspace_.selected_graph_node, workspace_.selected_graph_nodes, false));
        effects(ui_.focus(0));
        workspace_.inspecting_asset = false;
        workspace_.material_inspection.clear();
        workspace_.inspecting_network = true;
        workspace_.bump_view_revision();
        workspace_.status = L"Added SOP node: " + platform::utf8_to_utf16_validated(*created);
        return {};
    }
    {
        const auto prev_sel = workspace_.selected_graph_node;
        const auto prev_multi = workspace_.selected_graph_nodes;
        std::wstring net_status;
        ATELIER_TRY_BIND(handled, workspace_.network.apply_action(*document_, net_graph, workspace_.selected_graph_node,
                                                                  workspace_.selected_graph_nodes, network_canvas_dip(),
                                                                  event, net_status));
        if (*handled) {
            if (workspace_.selected_graph_node != prev_sel || workspace_.selected_graph_nodes != prev_multi) {
                workspace_.inspecting_asset = false;
                workspace_.material_inspection.clear();
                workspace_.inspecting_network =
                    !workspace_.selected_graph_node.empty() || !workspace_.selected_graph_nodes.empty();
                workspace_.bump_view_revision();
            }
            if (!net_status.empty())
                workspace_.status = std::move(net_status);
            return {};
        }
    }
    if (event.kind == ui::ActionKind::activate)
        if (const auto type = workspace_.component_choice(event.key)) {
            if (document_->selection() != workspace_.menu_selection())
                return invalid();
            const auto info = document_->world().runtime().describe(*type);
            if (!info)
                return std::unexpected(info.error());
            json::Builder args;
            args.set_validated(args.root(), "type", (*info)->name);
            return done(run_operator({"scene.add_component", args.finish(), {}}, true), L"Component added");
        }
    if (event.kind == ui::ActionKind::activate)
        if (const auto* binding = workspace_.binding(event.key);
            binding && binding->kind == editor::Workspace::Binding::Kind::operator_call) {
            if (binding->call->request.context.selection && document_->selection() != workspace_.menu_selection())
                return invalid();
            // A document replacement may retire the binding during this call.
            const auto call = binding->call->request;
            return run_operator(call, true);
        }
    if (ui_.presented() && workspace_.dock.apply(event, *ui_.presented()))
        return {};
    if (event.kind == ui::ActionKind::activate)
        if (const auto choice = workspace_.material_choice(event.key)) {
            using Kind = editor::Workspace::MaterialChoice::Kind;
            if (choice->kind == Kind::clear)
                return document_->clear_material(choice->object, choice->slot);
            if (choice->kind == Kind::discard)
                return document_->discard_material_override(choice->object, choice->path);
            if (choice->kind == Kind::create_scene) {
                ATELIER_TRY_BIND(created, document_->create_scene_material(choice->object, choice->slot));
                return {};
            }
            if (!project_)
                return invalid();
            if (choice->kind == Kind::assign)
                return document_->assign_material(*project_, choice->object, choice->slot, choice->key);
            if (choice->kind == Kind::texture)
                return document_->set_material_texture(*project_, choice->object, choice->path, choice->key);
            ATELIER_TRY_BIND(root, document_->open_material(*project_, choice->key));
            workspace_.material_inspection = choice->key;
            workspace_.inspecting_asset = true;
            return {};
        }
    if (workspace_.workbench.apply(event))
        return {};
    if (workspace_.assets.apply(event)) {
        if (event.key == editor::browser_control::results && event.kind == ui::ActionKind::select) {
            ATELIER_TRY(document_->cancel_edit());
            workspace_.inspecting_asset = true;
            workspace_.inspecting_network = false;
            workspace_.material_inspection.clear();
            const auto& selected = workspace_.assets.selected();
            if (selected && selected->kind == assets::AssetKind::material && project_) {
                ATELIER_TRY_BIND(root, document_->open_material(*project_, selected->key));
                workspace_.material_inspection = selected->key;
            }
        }
        return {};
    }
    const auto view_action = workspace_.apply_view(*document_, *this, event);
    if (!view_action)
        return std::unexpected(view_action.error());
    if (*view_action)
        return {};
    if (!source.target) {
        const auto local_event = workspace_.apply_ui(source);
        if (!local_event)
            return std::unexpected(local_event.error());
        if (*local_event)
            return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::browser_control::rename)
        return begin_asset_rename();
    if (event.kind == ui::ActionKind::activate &&
        (event.key == editor::browser_control::open || event.key == editor::browser_control::add)) {
        const auto selected = workspace_.assets.selected();
        if (!selected)
            return {};
        if (const auto* cell = std::get_if<ui::TableSelection>(&event.value);
            cell && workspace_.assets.row_key(selected->key) != cell->row)
            return {};
        if (event.key == editor::browser_control::add &&
            (selected->kind == assets::AssetKind::mesh || selected->kind == assets::AssetKind::procedural_graph)) {
            const bool is_mesh = selected->kind == assets::AssetKind::mesh;
            json::Builder args;
            args.set_validated(args.root(), "kind", is_mesh ? "mesh" : "procedural_generator");
            const auto key = args.set(args.root(), is_mesh ? "mesh" : "graph", selected->key);
            if (!key)
                return std::unexpected(Error{ErrorCode::invalid_storage, key.error().describe()});
            return run_operator({"scene.add", args.finish()});
        }
        if (selected->kind == assets::AssetKind::folder)
            workspace_.assets.navigate(selected->path);
        else if (selected->kind == assets::AssetKind::material && project_) {
            ATELIER_TRY_BIND(root, document_->open_material(*project_, selected->key));
            workspace_.material_inspection = selected->key;
            workspace_.inspecting_asset = true;
            workspace_.dock.select(editor::control::parameters_tab);
        } else if (selected->kind == assets::AssetKind::scene)
            return open_scene(selected->path);
        else if (selected->kind == assets::AssetKind::procedural_graph && project_)
            return open_graph_context(selected->key);
        else if (selected->kind == assets::AssetKind::source && project_) {
            ATELIER_TRY_BIND(path, project_->resolve(selected->path));
            return open_source(*path);
        }
        return {};
    }
    if (event.key == parameter_search_toggle && event.kind == ui::ActionKind::activate) {
        const auto* checked = std::get_if<bool>(&event.value);
        if (!checked)
            return invalid();
        if (workspace_.parameter_search_visible == *checked)
            return {};
        workspace_.parameter_search_visible = *checked;
        pending_parameter_search_focus_ = *checked;
        return {};
    }
    if (event.kind == ui::ActionKind::activate) {
        if (const auto direction = navigation_direction(event.key))
            return done(workspace_.navigate(*document_, *this, direction), L"Selection history");
        if (event.key == world_context) {
            if (!workspace_.active_graph.empty()) {
                exit_graph_context();
                workspace_.status = L"World context";
                return {};
            }
            return done(workspace_.select_object(*document_, *this, {}), L"World context");
        }
    }
    if (event.kind == ui::ActionKind::select && event.key == tree_world) {
        if (!workspace_.active_graph.empty())
            exit_graph_context();
        return done(workspace_.select_tree(*document_, *this, event.key), L"World context");
    }
    if (event.kind == ui::ActionKind::expand) {
        // A filter shows every matching row expanded; row expansion resumes when it clears.
        const auto* expanded = std::get_if<bool>(&event.value);
        if (!expanded)
            return invalid();
        using Tree = editor::Workspace::EntityTree;
        if (event.key == tree_world)
            return workspace_.tree_query.empty()
                       ? workspace_.set_tree_expanded(*document_, Tree::scene_tree, std::array{ObjectId{}}, *expanded)
                       : Result<void>{};
        const auto* row = workspace_.binding(event.key);
        if (!row || !row->path.empty() ||
            (row->kind != editor::Workspace::Binding::Kind::object &&
             row->kind != editor::Workspace::Binding::Kind::structure))
            return invalid();
        const auto tree = row->kind == editor::Workspace::Binding::Kind::object ? Tree::scene_tree : Tree::structure;
        if (!(tree == Tree::scene_tree ? workspace_.tree_query : workspace_.structure_query).empty())
            return {};
        return workspace_.set_tree_expanded(*document_, tree, std::span{&row->object, 1}, *expanded);
    }
    // The rail snap toggle is a second view of the shared snapping command.
    if (event.kind == ui::ActionKind::activate && event.key == viewport_rail_snap) {
        json::Builder args;
        args.set_validated(args.root(), "setting", "grid_snap");
        return run_operator({"tool.toggle", args.finish()});
    }
    if (event.kind == ui::ActionKind::activate && (event.key == shelf_create || event.key == shelf_view)) {
        workspace_.shelf_page = event.key;
        return {};
    }
    if (event.key == parameter_search || event.key == tree_filter || event.key == structure_filter) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value)
            return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit ||
            event.kind == ui::ActionKind::edit_cancel) {
            (event.key == tree_filter        ? workspace_.tree_query
             : event.key == structure_filter ? workspace_.structure_query
                                             : workspace_.query) = *value;
            workspace_.status = event.kind == ui::ActionKind::edit_cancel ? L"Filter cancelled"
                                : event.key == tree_filter                ? L"Tree filtered"
                                                                          : L"Parameters filtered";
        }
        return {};
    }
    // A retired control's cleanup must not cancel a different live edit.
    if (event.kind == ui::ActionKind::edit_cancel)
        return document_->owns_edit(event.key) ? done(document_->cancel_edit(), L"Edit cancelled") : Result<void>{};
    if (const auto* binding = workspace_.binding(event.key);
        binding && binding->kind == editor::Workspace::Binding::Kind::color_pick &&
        event.kind == ui::ActionKind::activate)
        return open_color_edit(binding->object, binding->path, event.key);
    if (const auto* binding = workspace_.binding(event.key);
        binding && binding->kind == editor::Workspace::Binding::Kind::object && binding->path.empty() &&
        (event.kind == ui::ActionKind::drag_begin || event.kind == ui::ActionKind::drag_preview ||
         event.kind == ui::ActionKind::drag_commit || event.kind == ui::ActionKind::drag_cancel)) {
        // Scene Tree drag: drop on a row parents under it, on World or empty space unparents.
        // Keep-world reparenting leaves the Entity where it is in the viewport; one history entry.
        const auto* point = std::get_if<ui::DragPoint>(&event.value);
        const auto drop = point && ui_.presented()
                              ? workspace_.tree_drop(*document_, *ui_.presented(), binding->object, *point)
                              : std::nullopt;
        workspace_.tree_drop_row = event.kind == ui::ActionKind::drag_preview && drop ? drop->row : 0;
        if (event.kind == ui::ActionKind::drag_preview)
            workspace_.status = drop ? L"Drop to parent · World or empty space unparents" : L"Cannot parent here";
        if (event.kind != ui::ActionKind::drag_commit || !drop)
            return {};
        const auto current = world::parent_entity(document_->world().runtime(), binding->object);
        if (!current)
            return std::unexpected(current.error());
        if (*current == drop->parent)
            return {};
        // The moved Entity ends selected: the tree reveals a newly selected row under its new parent.
        ATELIER_TRY(completion(document_->reparent(binding->object, drop->parent)));
        return done(workspace_.select_object(*document_, *this, binding->object),
                    drop->parent == ObjectId{} ? L"Entity unparented" : L"Entity parented");
    }
    if (const auto* binding = workspace_.binding(event.key)) {
        if (binding->kind == editor::Workspace::Binding::Kind::material_override) {
            if (event.kind != ui::ActionKind::activate)
                return {};
            const auto* enabled = std::get_if<bool>(&event.value);
            if (!enabled)
                return invalid();
            return document_->set_material_override(binding->object, binding->material_path, *enabled,
                                                    project_ ? &*project_ : nullptr);
        }
        if (binding->kind == editor::Workspace::Binding::Kind::material_field) {
            if (event.kind == ui::ActionKind::edit_begin)
                return document_->begin_material_edit(binding->object, event.key);
            if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit)
                return {};
            const auto* number = std::get_if<double>(&event.value);
            if (!number || !std::isfinite(*number) || std::abs(*number) > std::numeric_limits<float>::max())
                return invalid();
            ATELIER_TRY_BIND(view, document_->inspect_material(binding->object));
            const auto field =
                std::ranges::find(view->declarations, binding->material_path, &editor::MaterialParameterView::path);
            if (field == view->declarations.end() || binding->index >= field->components)
                return invalid();
            material::Override entry{field->path, field->value, true};
            std::visit(
                [&](auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::same_as<T, float>)
                        value = static_cast<float>(*number);
                    else if constexpr (!std::same_as<T, material::TextureBinding>)
                        value[binding->index] = static_cast<float>(*number);
                },
                entry.value);
            auto result = document_->preview_material_entry(event.key, std::move(entry));
            if (result && event.kind == ui::ActionKind::edit_commit)
                result = document_->commit_material_edit(event.key);
            return done(result, L"Material parameter updated");
        }
        if (const auto* modifiers = std::get_if<ui::SelectionModifiers>(&event.value);
            event.kind == ui::ActionKind::select && modifiers && modifiers->double_click && !modifiers->shift &&
            !modifiers->control && binding->kind == editor::Workspace::Binding::Kind::object && binding->path.empty() &&
            document_->is_selected(binding->object)) {
            // Double-clicking a selected row renames it in place (Blender Outliner / Houdini Tree View).
            workspace_.dock.select(editor::control::entities_tab);
            return run_operator(
                {"scene.rename",
                 {},
                 {.selection = editor::SelectionContext{binding->object, binding->object, {binding->object}},
                  .region = editor::RegionKind::scene_tree}});
        }
        if (event.kind == ui::ActionKind::select &&
            (binding->kind == editor::Workspace::Binding::Kind::object ||
             binding->kind == editor::Workspace::Binding::Kind::structure) &&
            binding->path.empty()) {
            if (!workspace_.active_graph.empty())
                exit_graph_context();
            return done(workspace_.select_tree(*document_, *this, event.key,
                                               std::get_if<ui::SelectionModifiers>(&event.value)
                                                   ? std::get<ui::SelectionModifiers>(event.value)
                                                   : ui::SelectionModifiers{}),
                        L"Tree selection updated");
        }
        if (binding->kind == editor::Workspace::Binding::Kind::navigation) {
            if (event.kind == ui::ActionKind::activate) {
                if (!workspace_.active_graph.empty())
                    exit_graph_context();
                workspace_.inspecting_asset = false;
                workspace_.dock.select(editor::control::parameters_tab);
                return done(workspace_.select_object(*document_, *this, binding->object), L"Object selected");
            }
            return {};
        }
        if (binding->kind == editor::Workspace::Binding::Kind::name) {
            if (event.kind == ui::ActionKind::edit_commit) {
                auto name = platform::utf16_to_utf8(std::get<std::wstring>(event.value));
                if (!name)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
                if (document_->is_graph_object(binding->object)) {
                    const auto current_name = document_->graph_node_name(binding->object);
                    if (!current_name || *current_name == *name)
                        return {};
                    json::Builder args;
                    static_cast<void>(args.set(args.root(), "node", *current_name));
                    static_cast<void>(args.set(args.root(), "name", *name));
                    return done(run_operator({"graph.rename_node", args.finish()}, true, false),
                                L"Node renamed / Undo restores its name");
                }
                json::Builder args;
                args.set_validated(args.root(), "name", *name);
                return done(
                    run_operator(
                        {"scene.rename",
                         args.finish(),
                         {.selection = editor::SelectionContext{binding->object, binding->object, {binding->object}},
                          .region = {}}},
                        true, false),
                    L"Entity renamed / Undo restores its name");
            }
            return {};
        }
        if (binding->kind == editor::Workspace::Binding::Kind::text_field) {
            if (event.kind != ui::ActionKind::edit_commit)
                return {};
            const auto* wide = std::get_if<std::wstring>(&event.value);
            if (!wide)
                return invalid();
            const auto utf8 = platform::utf16_to_utf8(*wide);
            if (!utf8)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
            return done(document_->set_string_property(binding->object, binding->path, *utf8),
                        L"String parameter updated / Undo restores its value");
        }
        if (binding->kind == editor::Workspace::Binding::Kind::position_default) {
            if (event.kind != ui::ActionKind::reset)
                return invalid();
            return done(document_->reset_position(binding->object),
                        L"Position restored to default / Undo restores the vector");
        }
        if (binding->kind == editor::Workspace::Binding::Kind::rotation_default ||
            binding->kind == editor::Workspace::Binding::Kind::scale_default) {
            if (event.kind != ui::ActionKind::reset)
                return invalid();
            ATELIER_TRY_BIND(value, world::read_transform(document_->world().runtime(), binding->object));
            if (binding->kind == editor::Workspace::Binding::Kind::rotation_default)
                value->rotation = {};
            else
                value->scale = {1, 1, 1};
            const std::array objects{binding->object};
            const std::array values{*value};
            auto result = document_->begin_transform(objects, event.key);
            if (!result)
                return result;
            result = document_->preview_transform(event.key, values);
            if (result)
                result = document_->commit_edit(event.key);
            if (!result) {
                const auto cancelled = document_->cancel_edit();
                if (!cancelled)
                    return cancelled;
            }
            return done(result, L"Transform tuple restored");
        }
        if (binding->kind == editor::Workspace::Binding::Kind::rotation) {
            if (event.kind == ui::ActionKind::edit_begin) {
                const std::array objects{binding->object};
                return document_->begin_transform(objects, event.key);
            }
            if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit)
                return {};
            const auto* number = std::get_if<double>(&event.value);
            if (!number || !std::isfinite(*number) || std::abs(*number) > 360000)
                return invalid();
            ATELIER_TRY_BIND(value, document_->original_transform(event.key, binding->object));
            auto angles = editor::euler_degrees(value->rotation);
            for (std::size_t axis = 0; axis < 3; ++axis)
                if (std::ranges::equal(binding->path, document_->types().rotation_fields[axis]))
                    angles[axis] = float(*number);
            value->rotation = editor::from_euler_degrees(angles);
            const std::array values{*value};
            auto result = document_->preview_transform(event.key, values);
            if (result && event.kind == ui::ActionKind::edit_commit)
                result = document_->commit_edit(event.key);
            return done(result, L"Rotation updated");
        }
        if (binding->path.empty()) {
            return invalid();
        }
        if (event.kind == ui::ActionKind::activate)
            if (const auto* checked = std::get_if<bool>(&event.value)) {
                auto& runtime = document_->world().runtime();
                auto type = runtime.type_of(binding->object);
                json::Builder args;
                auto value = args.root();
                for (const auto field : binding->path) {
                    if (!type)
                        return std::unexpected(type.error());
                    const auto info = runtime.describe(*type);
                    if (!info)
                        return std::unexpected(info.error());
                    const auto at = std::ranges::find((*info)->fields, field, &FieldInfo::id);
                    const auto next = args.object(value, at->name);
                    if (!next)
                        return std::unexpected(Error{ErrorCode::invalid_definition, next.error().describe()});
                    value = *next;
                    type = at->value.type;
                }
                args.set(value, *checked);
                const auto entity = document_->selection().entity;
                const auto selection_ctx = document_->is_graph_object(binding->object)
                                               ? editor::SelectionContext{ObjectId{}, binding->object, {}}
                                               : editor::SelectionContext{entity, binding->object, {entity}};
                return done(run_operator({"scene.set_properties",
                                          args.finish(),
                                          {.selection = selection_ctx, .region = editor::RegionKind::parameters}}),
                            L"Property changed");
            }
        const auto* value = std::get_if<double>(&event.value);
        if (!value)
            return invalid();
        ATELIER_TRY_BIND(prototype, document_->read_property(binding->object, binding->path));
        ATELIER_TRY_BIND(typed, scalar_value(*prototype, *value));
        switch (event.kind) {
        case ui::ActionKind::edit_begin:
            return done(document_->begin_edit(binding->object, binding->path, event.key),
                        L"Editing field / Esc cancels");
        case ui::ActionKind::edit_preview:
            return done(document_->preview_edit(event.key, *typed), L"Field preview / Esc cancels");
        case ui::ActionKind::edit_commit: {
            ATELIER_TRY(document_->preview_edit(event.key, *typed));
            return done(document_->commit_edit(event.key), L"Field committed");
        }
        default:
            return {};
        }
    }
    return {};
}

// Colour editor: one Session group transaction from open to Done/dismiss; Esc and
// outside clicks keep the edit (Houdini applies live), Revert restores the original.
Result<void> Application::open_color_edit(ObjectId object, std::span<const FieldId> prefix, ui::ControlKey swatch) {
    if (workspace_.color_edit)
        ATELIER_TRY(finish_color_edit(true));
    close_menu(false);
    editor::Workspace::ColorEdit edit;
    edit.object = object;
    const auto* binding = workspace_.binding(swatch);
    if (!binding || binding->kind != editor::Workspace::Binding::Kind::color_pick ||
        !std::isfinite(binding->color_maximum) || binding->color_maximum <= 0)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Colour editing requires the owning property's numeric range."});
    edit.maximum = binding->color_maximum;
    if (!binding->material_path.empty()) {
        ATELIER_TRY_BIND(view, document_->inspect_material(object));
        const auto field =
            std::ranges::find(view->declarations, binding->material_path, &editor::MaterialParameterView::path);
        if (field == view->declarations.end() || field->components < 3 || field->components > 4)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "A material colour needs a vector3 or vector4 declaration."});
        edit.material_entry = material::Override{field->path, field->value, true};
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, math::Vector3> || std::same_as<T, math::Vector4>)
                    for (std::size_t lane = 0; lane < 3; ++lane)
                        edit.original[lane] = edit.value[lane] = value[lane];
            },
            field->value);
        ATELIER_TRY(document_->begin_material_edit(object, editor::control::color_group));
    } else {
        for (std::uint32_t lane = 0; lane < 3; ++lane) {
            auto path = world::FieldPath(prefix.begin(), prefix.end());
            ATELIER_TRY_BIND(info, document_->world().runtime().describe(take_type(object, prefix)));
            if (lane >= (*info)->fields.size())
                return std::unexpected(Error{ErrorCode::invalid_definition, "A colour needs three channels."});
            path.push_back((*info)->fields[lane].id);
            ATELIER_TRY_BIND(value, document_->read_property(object, path));
            edit.original[lane] = edit.value[lane] = std::visit(
                [](const auto& v) -> double {
                    if constexpr (std::same_as<std::decay_t<decltype(v)>, EnumValue>)
                        return v.bits;
                    else
                        return double(v);
                },
                *value);
            edit.paths.push_back(std::move(path));
        }
        ATELIER_TRY(document_->begin_group_edit(object, edit.paths, editor::control::color_group));
    }
    // The swatch button routes to this pick key as its action target; anchor the editor under it.
    const ui::ControlBox* box = nullptr;
    if (ui_.presented())
        for (const auto& control : ui_.presented()->controls)
            if (control.kind == ui::ControlKind::button && control.action_target == swatch) {
                box = &control;
                break;
            }
    const auto dpi = window_.dpi_scale();
    edit.x = box ? (box->bounds.x + box->bounds.width) / dpi - 420 : 0;
    edit.y = box ? (box->bounds.y + box->bounds.height) / dpi + 2 : 0;
    edit.hex = editor::color_hex(edit.value);
    workspace_.color_edit = std::move(edit);
    workspace_.status = L"Colour editor · Done or click outside to keep · Revert restores";
    return {};
}

TypeId Application::take_type(ObjectId object, std::span<const FieldId> path) const {
    auto& runtime = document_->world().runtime();
    auto type = runtime.type_of(object).value_or(TypeId{});
    for (const auto id : path) {
        const auto info = runtime.describe(type);
        if (!info)
            return {};
        const auto at = std::ranges::find((*info)->fields, id, &FieldInfo::id);
        if (at == (*info)->fields.end())
            return {};
        type = at->value.type;
    }
    return type;
}

Result<void> Application::apply_color(std::array<double, 3> rgb) {
    auto& edit = *workspace_.color_edit;
    if (edit.material_entry) {
        for (const auto channel : rgb)
            if (!std::isfinite(channel) || std::abs(channel) > edit.maximum)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "Material colour channels must be finite float values."});
        auto entry = *edit.material_entry;
        std::visit(
            [&](auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, math::Vector3> || std::same_as<T, math::Vector4>)
                    for (std::size_t lane = 0; lane < 3; ++lane)
                        value[lane] = static_cast<float>(rgb[lane]);
            },
            entry.value);
        ATELIER_TRY(document_->preview_material_entry(editor::control::color_group, std::move(entry)));
        for (std::size_t lane = 0; lane < 3; ++lane)
            edit.value[lane] = static_cast<float>(rgb[lane]);
        edit.hex = editor::color_hex(edit.value);
        return {};
    }
    std::vector<Scalar> values;
    for (std::size_t lane = 0; lane < 3; ++lane) {
        const auto value = rgb[lane];
        if (!std::isfinite(value) || value < 0 || value > edit.maximum)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Colour channels must stay within the property's numeric range."});
        ATELIER_TRY_BIND(prototype, document_->read_property(edit.object, edit.paths[lane]));
        ATELIER_TRY_BIND(typed, scalar_value(*prototype, value));
        values.push_back(*typed);
    }
    ATELIER_TRY(document_->preview_group_edit(editor::control::color_group, values));
    for (std::size_t lane = 0; lane < 3; ++lane)
        edit.value[lane] = std::visit(
            [](const auto& value) -> double {
                if constexpr (std::same_as<std::decay_t<decltype(value)>, EnumValue>)
                    return value.bits;
                else
                    return double(value);
            },
            values[lane]);
    edit.hex = editor::color_hex(edit.value);
    return {};
}

Result<bool> Application::color_edit_action(const ui::Action& event) {
    using namespace editor::control;
    auto& edit = *workspace_.color_edit;
    if (event.key == color_done && event.kind == ui::ActionKind::activate) {
        ATELIER_TRY(finish_color_edit(true));
        return true;
    }
    if (event.key == color_revert && event.kind == ui::ActionKind::activate) {
        ATELIER_TRY(apply_color(edit.original));
        return true;
    }
    if (event.key == color_hex) {
        const auto* text = std::get_if<std::wstring>(&event.value);
        if (!text || event.kind != ui::ActionKind::edit_commit)
            return true;
        const auto parsed = editor::parse_color_hex(*text);
        if (!parsed) {
            workspace_.status = L"Hex colour needs six digits, e.g. FF8800";
            return true;
        }
        ATELIER_TRY(apply_color(*parsed));
        return true;
    }
    const bool rgb = event.key >= color_rgb && event.key < color_rgb + 3;
    const bool hsv = event.key >= color_hsv && event.key < color_hsv + 3;
    if (!rgb && !hsv)
        return false;
    if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit &&
        event.kind != ui::ActionKind::edit_cancel)
        return true;
    const auto* value = std::get_if<double>(&event.value);
    if (!value || !std::isfinite(*value))
        return true;
    auto next = edit.value;
    if (rgb)
        next[event.key - color_rgb] = *value;
    else {
        auto h = editor::rgb_to_hsv(edit.value);
        h[event.key - color_hsv] = *value;
        next = editor::hsv_to_rgb(h);
    }
    ATELIER_TRY(apply_color(next));
    return true;
}

Result<void> Application::finish_color_edit(bool keep) {
    if (!workspace_.color_edit)
        return {};
    // Retire the form before ending its transaction: queued field cleanup and
    // popup dismissal must not end it again, even if rollback reports an error.
    workspace_.color_edit.reset();
    if (!document_->owns_edit(editor::control::color_group))
        return {};
    auto finished = keep ? document_->commit_edit(editor::control::color_group) : document_->cancel_edit();
    if (!finished && keep) {
        const auto cancelled = document_->cancel_edit();
        if (!cancelled)
            report_error(cancelled.error(), "editor.rollback_failed");
    }
    if (finished) {
        workspace_.status = keep ? L"Colour applied" : L"Colour edit cancelled";
    }
    return finished;
}

Result<void> Application::begin_entity_rename(ObjectId entity, editor::RegionKind region) {
    // Reveal the explicit tree row; a filtered-out row uses the Parameters header instead.
    const auto shown =
        region == editor::RegionKind::scene_tree ? workspace_.reveal_in_tree(*document_, entity) : Result<bool>{false};
    if (!shown)
        return std::unexpected(shown.error());
    if (*shown) {
        workspace_.row_rename = editor::Workspace::RowRename{entity, std::wstring(document_->name(entity))};
        pending_rename_focus_ = true;
    } else {
        if (document_->selection().entity != entity)
            ATELIER_TRY(document_->select(entity));
        workspace_.inspecting_asset = false;
        workspace_.dock.select(editor::control::parameters_tab);
        pending_name_focus_ = true;
    }
    return {};
}

Result<void> Application::begin_asset_rename() {
    if (!project_ || !workspace_.assets.can_rename())
        return {};
    const auto& item = *workspace_.assets.selected();
    const auto row = workspace_.assets.row_key(item.key);
    if (!row)
        return {};
    workspace_.row_rename = editor::Workspace::RowRename{
        {}, item.path.filename().wstring(), item.path, (row << 2) | editor::browser_control::name_column};
    pending_rename_focus_ = true;
    return {};
}

Result<void> Application::rename_asset(std::string_view key, std::string_view name) {
    const auto* entry = project_ ? project_->entry(key) : nullptr;
    if (!entry)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Select a project asset to rename."});
    if (name.empty()) {
        const auto row = workspace_.assets.row_key(key);
        if (row) {
            workspace_.row_rename = editor::Workspace::RowRename{
                {}, entry->path.filename().wstring(), entry->path, (row << 2) | editor::browser_control::name_column};
            pending_rename_focus_ = true;
        }
        return {};
    }
    ATELIER_TRY_BIND(renamed, project_->rename(entry->path, platform::utf8_to_utf16_validated(name)));
    workspace_.assets.reselect(*renamed);
    workspace_.status = L"Renamed to " + renamed->filename().wstring();
    return {};
}

Result<void> Application::open_graph_context(std::string_view key) {
    close_menu(false);
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Finish the current field edit before switching context."});
    ATELIER_TRY(finish_color_edit(true));
    ATELIER_TRY(viewport_tool_.cancel(*document_));
    if (!document_->has_graph(key)) {
        if (!project_)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Open a project before opening a procedural graph asset."});
        ATELIER_TRY(document_->open_graph(*project_, key));
    }
    workspace_.inspecting_asset = false;
    workspace_.inspecting_network = false;
    workspace_.material_inspection.clear();
    workspace_.active_graph = std::string(key);
    if (const auto* doc = document_->graph_document(key)) {
        std::optional<std::string> disp_name;
        if (doc->graph.registered())
            if (const auto listed = procedural::contents(document_->world().runtime(), doc->graph.id()))
                if (listed->display() != ObjectId{})
                    disp_name = document_->graph_node_name(listed->display());
        if (disp_name)
            workspace_.set_single_graph_selection(*disp_name);
        else if (!doc->nodes.empty())
            workspace_.set_single_graph_selection(doc->nodes.back().name);
        else
            workspace_.set_single_graph_selection({});
    } else
        workspace_.set_single_graph_selection({});
    workspace_.dock.select(editor::network_control::tab);
    workspace_.dock.select(editor::control::parameters_tab);
    workspace_.bump_view_revision();
    camera_cut_ = true;
    workspace_.status = L"SOP context: " + workspace_.graph_path_title(project_ ? &*project_ : nullptr);
    return {};
}

void Application::exit_graph_context() {
    close_menu(false);
    workspace_.network.cancel_gesture();
    workspace_.network.set_cut_key(false);
    workspace_.network.close_tab_search();
    workspace_.active_graph.clear();
    workspace_.inspecting_network = false;
    workspace_.set_single_graph_selection({});
    workspace_.bump_view_revision();
    camera_cut_ = true;
}

void Application::select_graph_node(std::string_view key, std::string_view node_name) {
    const auto net_key = workspace_.current_network_graph(*document_);
    if (workspace_.active_graph != key && net_key != key)
        return;
    workspace_.inspecting_asset = false;
    workspace_.material_inspection.clear();
    if (workspace_.active_graph.empty() && node_name.empty() && workspace_.selected_graph_node.empty() &&
        workspace_.inspecting_network) {
        workspace_.inspecting_network = false;
    } else {
        workspace_.inspecting_network = workspace_.active_graph.empty();
    }
    workspace_.set_single_graph_selection(node_name);
    workspace_.bump_view_revision();
}

void Application::select_graph_nodes(std::string_view key, std::span<const std::string> node_names) {
    const auto net_key = workspace_.current_network_graph(*document_);
    if (workspace_.active_graph != key && net_key != key)
        return;
    workspace_.inspecting_asset = false;
    workspace_.material_inspection.clear();
    workspace_.selected_graph_nodes.assign(node_names.begin(), node_names.end());
    workspace_.selected_graph_node = node_names.empty() ? std::string{} : node_names.back();
    workspace_.inspecting_network = workspace_.active_graph.empty() && !workspace_.selected_graph_nodes.empty();
    workspace_.bump_view_revision();
}

void Application::select_box() {
    viewport_tool_.arm_box_select(pointer_position_, false);
    window_.capture(true);
}

void Application::toggle_view(editor::ViewToggleOption option) {
    auto& value = option == editor::ViewToggleOption::fps ? workspace_.show_fps : workspace_.viewport_options_collapsed;
    value = !value;
}

Result<void> Application::fit_lighting(bool follow_focus) {
    auto fitted = fitted_probe_view(follow_focus);
    return fitted ? workspace_.set_view_settings(*document_, *fitted, lighting_focus())
                  : Result<void>{std::unexpected(fitted.error())};
}

void Application::about() {
    close_menu(false);
    const auto details = L"FrostFox Atelier\n\nBuild: " + platform::utf8_to_utf16_validated(build_info::revision) +
                         L"\nConfiguration: " + platform::utf8_to_utf16_validated(build_info::configuration) +
                         L"\n\nProject:\n" + (project_ ? project_->root().wstring() : L"No project open") +
                         L"\n\nScene:\n" + (scene_path_.empty() ? L"Untitled" : scene_path_.generic_wstring());
    MessageBoxW(window(), details.c_str(), L"About FrostFox Atelier", MB_OK | MB_ICONINFORMATION);
}

bool Application::key_input(const ui::Event& event, std::uint32_t key, editor::RegionKind region,
                            editor::KeyPhase phase) {
    const auto* entry = editor::key_binding(region, phase, event, key, ui_.wants_keyboard());
    if (!entry || (workspace_.sample_history() && phase == editor::KeyPhase::after_controls &&
                   !entry->call.id.starts_with("edit.")))
        return false;
    if (event.repeat && !entry->repeat)
        return true;
    auto call = entry->call;
    if (call.id == "view.frame_selected") {
        if (region == editor::RegionKind::network)
            return true;
        if (workspace_.active_graph.empty() && document_->selected_entities().empty())
            call.id = "view.frame_all";
    }
    if (region != editor::RegionKind::none)
        call.context.region = region;
    if (call.id.starts_with("asset.") && workspace_.assets.selected())
        call.context.asset = workspace_.assets.selected()->key;
    if (call.id.starts_with("layout."))
        cancel_interaction(ui_.cancel_interaction());
    status(run_operator(call, true), L"");
    return true;
}

bool Application::effects(const ui::Reply& reply) {
    std::wstring first_error;
    bool business_failed = false;
    const auto remember = [&](std::wstring message) {
        if (first_error.empty())
            first_error = std::move(message);
    };
    const auto apply = [&](auto&& self, const ui::Reply& current, bool cleanup) -> void {
        if (current.error) {
            const auto& error = *current.error;
            report_error(ui_error(error), "editor.input_failed");
            if (error.code == ui::ErrorCode::platform_failed)
                remember(L"Input operation failed (Windows " + std::to_wstring(error.native_code) + L").");
            else if (error.code == ui::ErrorCode::capacity_exceeded)
                remember(L"Input is too long.");
            else if (error.code == ui::ErrorCode::text_failed)
                remember(diagnostic_text(ui_error(error).message));
            else
                remember(L"Input must be valid single-line text.");
            if (!cleanup && error.code == ui::ErrorCode::text_failed) {
                business_failed = true;
                cleanup = true; // Keep cancellation effects already retired by Session; skip new business actions.
            }
        }
        for (const auto& effect : current.effects) {
            if (business_failed && !cleanup)
                break;
            if (const auto* command = std::get_if<ui::Action>(&effect)) {
                if (cleanup && command->kind != ui::ActionKind::edit_cancel &&
                    command->kind != ui::ActionKind::dismiss && command->kind != ui::ActionKind::drag_cancel)
                    continue;
                const auto result = action(*command);
                if (!result) {
                    report_error(result.error(), cleanup ? "editor.rollback_failed" : "editor.operation_failed");
                    if (cleanup)
                        first_error += L"; rollback failed: " + diagnostic_text(result.error().message);
                    else {
                        remember(diagnostic_text(result.error().message));
                        business_failed = true;
                    }
                }
            } else {
                const auto& request = std::get<ui::PlatformRequest>(effect);
                ui::Event response;
                if (const auto* clipboard = std::get_if<ui::ClipboardRequest>(&request)) {
                    if (cleanup)
                        continue;
                    response.kind = ui::EventKind::clipboard_result;
                    response.data =
                        ui_.clipboard_current(*clipboard)
                            ? window_.clipboard(*clipboard)
                            : ui::ClipboardResult{clipboard->kind, clipboard->operation_id, clipboard->token,
                                                  false,           ERROR_INVALID_STATE,     {}};
                } else {
                    const auto& ime = std::get<ui::ImeRequest>(request);
                    if (cleanup && ime.kind != ui::ImeKind::cancel)
                        continue;
                    auto result = window_.ime(ime);
                    if (!result.success) {
                        const auto message =
                            L"IME operation failed (Windows " + std::to_wstring(result.native_error) + L").";
                        if ((cleanup || ime.kind == ui::ImeKind::cancel) && !first_error.empty())
                            first_error += L"; " + message;
                        else
                            remember(message);
                    }
                    response.kind = ui::EventKind::ime_result;
                    response.data = std::move(result);
                }
                self(self, ui_.input(response), cleanup);
            }
        }
        if (current.capture == ui::Capture::release)
            window_.capture(false);
        else if (current.capture == ui::Capture::acquire && !business_failed)
            window_.capture(true);
        if (current.cursor)
            window_.cursor(*current.cursor);
    };
    apply(apply, reply, false);
    if (business_failed) {
        const auto color_cancelled = finish_color_edit(false);
        if (!color_cancelled) {
            report_error(color_cancelled.error(), "editor.rollback_failed");
            first_error += L"; rollback failed: " + diagnostic_text(color_cancelled.error().message);
        }
        apply(apply, ui_.cancel_interaction(), true);
        const auto cancelled = document_->cancel_edit();
        if (!cancelled) {
            report_error(cancelled.error(), "editor.rollback_failed");
            first_error += L"; rollback failed: " + diagnostic_text(cancelled.error().message);
        }
        window_.capture(false);
    }
    const auto native_error = window_.text_input(ui_.text_input());
    if (native_error)
        remember(L"Text input unavailable (Windows " + std::to_wstring(native_error) + L").");
    const bool success = first_error.empty();
    if (!success)
        workspace_.status = std::move(first_error);
    return success;
}

void Application::sync_text_input() {
    const auto error = window_.text_input(ui_.text_input());
    if (error)
        workspace_.status = L"Text input unavailable (Windows " + std::to_wstring(error) + L").";
}

void Application::cancel_interaction(const ui::Reply& reply, std::string_view input_event,
                                     std::source_location caller) {
    // Session distinguishes native focus/capture loss from a forced UI cancellation.
    // Application-owned gestures must end for either kind before capture is released.
    finish_camera_fly(false);
    camera_fly_release_.reset();
    camera_drag_.reset();
    workspace_.network.cancel_gesture();
    workspace_.network.set_cut_key(false);
    workspace_.lighting.cancel();
    const auto color_cancelled = finish_color_edit(false);
    close_menu(false, caller, input_event);
    effects(reply);
    if (!color_cancelled) {
        report_error(color_cancelled.error(), "editor.rollback_failed");
        workspace_.status += L"; rollback failed: " + diagnostic_text(color_cancelled.error().message);
    }
    const auto cancelled = document_->cancel_edit();
    if (!cancelled) {
        report_error(cancelled.error(), "editor.rollback_failed");
        workspace_.status += L"; rollback failed: " + diagnostic_text(cancelled.error().message);
    }
    const auto tool_cancelled = viewport_tool_.cancel(*document_);
    if (!tool_cancelled) {
        report_error(tool_cancelled.error(), "editor.rollback_failed");
        workspace_.status = L"Viewport rollback failed: " + diagnostic_text(tool_cancelled.error().message);
    }
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

void Application::request_menu(const ui::ContextMenuRequest& request) {
    const auto frame = ui_.presented();
    const auto* source = frame ? frame->find(request.source) : nullptr;
    const std::array fields{logging::Field{"source_control", request.source}, logging::Field{"group", request.group},
                            logging::Field{"frame", frame ? frame->drawing->id : 0},
                            logging::Field{"incarnation", source ? source->incarnation : 0}};
    if (!source || !source->enabled || !live_table_context(*frame, request, false)) {
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.rejected",
                                           logging::native::Context{fields}, "Menu source is not live"));
        return;
    }
    const auto next = request;
    const auto return_focus = editor::control::is_menu(next.source) ? menu_return_focus_ : ui_.focused_control();
    // The native menu owns subsequent mouse-up events. Retire the previous
    // gesture and accepted menu before publishing the replacement request.
    // Retain its accepted source and return focus across that cancellation.
    cancel_interaction(ui_.cancel_interaction());
    if (document_->editing() || viewport_tool_.active()) {
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.rejected",
                                           logging::native::Context{fields},
                                           "Menu request conflicts with an active edit"));
        return;
    }
    workspace_.menu = next;
    menu_return_focus_ = return_focus;
    menu_incarnation_ = source->incarnation;
    menu_requested_ = std::chrono::steady_clock::now();
    menu_context_target_ = source->context_target;
    menu_context_group_ = source->context_group;
    menu_table_layout_ = source->table_layout;
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.requested",
                                       logging::native::Context{fields}, "Menu request adopted"));
}

void Application::close_menu(bool restore_focus, std::source_location caller, std::string_view input_event) {
    pending_menu_ = 0;
    pie_release_pending_ = pie_release_select_ = pie_pressed_ = false;
    pie_hot_ = 0;
    const bool was_open = workspace_.menu.has_value() || menu_window();
    if (!was_open)
        return;
    const std::array fields{
        logging::Field{"source_control", workspace_.menu ? workspace_.menu->source : menu_host()->source()},
        logging::Field{"incarnation", menu_incarnation_},
        logging::Field{"restore_focus", restore_focus},
        logging::Field{"caller_line", std::uint64_t{caller.line()}},
        logging::Field{"caller_function", std::string(caller.function_name())},
        logging::Field{"input_event", std::string(input_event)}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.closed", logging::native::Context{fields},
                                       "Menu closing"));
    workspace_.menu.reset();
    workspace_.menu_items.clear();
    for (auto& submenu : submenus_)
        if (auto hidden = submenu->hide(); !hidden)
            report_error(hidden.error(), "menu.cleanup_failed");
    menu_path_.clear();
    if (popup_)
        if (auto hidden = popup_->hide(); !hidden)
            report_error(hidden.error(), "menu.cleanup_failed");
    if (was_open) {
        window_.capture(false);
        if (restore_focus)
            effects(ui_.focus(menu_return_focus_));
    }
    menu_return_focus_ = 0;
    menu_incarnation_ = 0;
    menu_context_target_ = menu_context_group_ = 0;
    menu_table_layout_ = {};
}

Result<void> Application::update_menu() {
    if (!workspace_.menu || workspace_.menu_items.empty()) {
        close_menu();
        return {};
    }
    const auto request = *workspace_.menu;
    const auto* source = ui_.presented()->find(request.source);
    if (!source || source->incarnation != menu_incarnation_ || !source->enabled ||
        source->context_target != menu_context_target_ || source->context_group != menu_context_group_ ||
        source->table_layout != menu_table_layout_ || !live_table_context(*ui_.presented(), request, true) ||
        source->bounds.y >= source->clip.bottom || source->bounds.y + source->bounds.height <= source->clip.top) {
        close_menu();
        return {};
    }
    if (workspace_.pie_menu()) {
        if (popup_)
            ATELIER_TRY(popup_->hide());
        window_.capture(true);
        if (ui_.presented()->popup == editor::control::viewport_pie) {
            effects(ui_.focus(pie_hot_));
            if (pie_release_pending_) {
                pie_release_pending_ = false;
                if (pie_release_select_) {
                    ui::Event release;
                    release.kind = ui::EventKind::key_up;
                    pie_input(release, request.group == editor::control::viewport_axis_pie ? VK_OEM_3 : 'Z');
                }
            }
        }
        return {};
    }
    const bool below = editor::control::is_menu(source->key) || source->kind == ui::ControlKind::combo;
    POINT points[2];
    if (below) {
        points[0] = {static_cast<LONG>(source->bounds.x), static_cast<LONG>(source->bounds.y)};
        points[1] = {static_cast<LONG>(source->bounds.x + source->bounds.width),
                     static_cast<LONG>(source->bounds.y + source->bounds.height)};
    } else
        points[0] = points[1] = {static_cast<LONG>(request.x), static_cast<LONG>(request.y)};
    MapWindowPoints(window(), nullptr, points, 2);
    ATELIER_TRY(popup_->update(source->key, {points[0].x, points[0].y, points[1].x, points[1].y}, below,
                               workspace_.menu_items, ui_.style(), style_sheet_, workspace_.ui_component("PopupMenu"),
                               request.group == editor::control::viewport_delete_menu));
    if (!popup_->visible()) {
        const std::array fields{logging::Field{"source_control", source->key},
                                logging::Field{"incarnation", menu_incarnation_},
                                logging::Field{"diagnostic", std::string(popup_->diagnostic())}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::warning, "menu", "menu.rejected",
                                           logging::native::Context{fields}, "Popup update rejected"));
        workspace_.status = L"UI: " + diagnostic_text(popup_->diagnostic());
        close_menu(false);
        return {};
    }
    window_.capture(true);
    ATELIER_TRY(popup_->render());
    auto items = std::span<const ui::MenuItem>(workspace_.menu_items);
    auto* parent = popup_.get();
    for (std::size_t depth = 0; depth < menu_path_.size(); ++depth) {
        const auto at =
            std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == menu_path_[depth]; });
        const auto* box = parent->presented() ? parent->presented()->find(menu_path_[depth]) : nullptr;
        if (at == items.end() || at->children.empty() || !at->enabled || !box || box->bounds.x >= box->clip.right ||
            box->bounds.x + box->bounds.width <= box->clip.left || box->bounds.y >= box->clip.bottom ||
            box->bounds.y + box->bounds.height <= box->clip.top) {
            menu_path_.resize(depth);
            break;
        }
        if (depth == submenus_.size()) {
            auto child = std::make_unique<PopupHost>();
            ATELIER_TRY(child->initialize(window(), editor_fonts(), gpu_validation_));
            submenus_.push_back(std::move(child));
        }
        POINT corners[2]{{static_cast<LONG>(box->bounds.x), static_cast<LONG>(box->bounds.y)},
                         {static_cast<LONG>(box->bounds.x + box->bounds.width),
                          static_cast<LONG>(box->bounds.y + box->bounds.height)}};
        MapWindowPoints(parent->window(), nullptr, corners, 2);
        auto& child = *submenus_[depth];
        ATELIER_TRY(child.update(at->key, {corners[0].x, corners[0].y, corners[1].x, corners[1].y}, false, at->children,
                                 ui_.style(), style_sheet_, workspace_.ui_component("PopupMenu")));
        if (!child.visible()) {
            workspace_.status = L"UI: " + diagnostic_text(child.diagnostic());
            menu_path_.resize(depth);
            break;
        }
        ATELIER_TRY(child.render());
        parent = &child;
        items = at->children;
    }
    for (std::size_t depth = menu_path_.size(); depth < submenus_.size(); ++depth)
        ATELIER_TRY(submenus_[depth]->hide());
    return {};
}

Result<bool> Application::menu_input(const ui::Event& input, WPARAM native_key) {
    if (!workspace_.menu)
        return false;
    if (workspace_.pie_menu())
        return pie_input(input, native_key);
    const auto source = workspace_.menu->source;
    const bool top = editor::control::is_menu(source);
    const auto is_viewport_strip = [](ui::ControlKey k) noexcept {
        return k == editor::control::viewport_view_menu || k == editor::control::viewport_select_menu ||
               k == editor::control::viewport_header_add || k == editor::control::viewport_object_menu;
    };
    const bool strip_hover = top || is_viewport_strip(source) || editor::network_control::is_header_menu(source);
    const auto same_strip = [&](ui::ControlKey k) noexcept {
        return (top && editor::control::is_menu(k)) || (is_viewport_strip(source) && is_viewport_strip(k)) ||
               (editor::network_control::is_header_menu(source) && editor::network_control::is_header_menu(k));
    };
    auto menu_at = ui::ControlKey{};
    if (strip_hover && ui_.presented())
        for (const auto& box : ui_.presented()->controls)
            if (box.enabled && same_strip(box.key) && input.x >= box.bounds.x &&
                input.x < box.bounds.x + box.bounds.width && input.y >= box.bounds.y &&
                input.y < box.bounds.y + box.bounds.height) {
                menu_at = box.key;
                break;
            }
    const auto change = [&](ui::ControlKey key) {
        effects(ui_.focus(key));
        request_menu({key, 0, 0, 0});
    };
    if (strip_hover && input.kind == ui::EventKind::pointer_move && menu_at && menu_at != source) {
        change(menu_at);
        return true;
    }
    if (input.kind == ui::EventKind::pointer_down && menu_at) {
        if (menu_at == source)
            close_menu();
        else
            change(menu_at);
        return true;
    }
    if (input.kind == ui::EventKind::key_down && (input.key == ui::Key::f10 || native_key == VK_MENU)) {
        close_menu();
        return true;
    }
    const bool navigate_top = top && menu_path_.empty() && input.kind == ui::EventKind::key_down &&
                              (input.key == ui::Key::left || input.key == ui::Key::right);
    if (navigate_top && (input.key == ui::Key::left || !menu_window())) {
        change(editor::cycle_menu(source, input.key == ui::Key::left));
        return true;
    }
    if (!menu_window()) {
        if (input.kind == ui::EventKind::key_down && input.key == ui::Key::escape)
            close_menu();
        return true; // Opening transition is modal before the first accepted popup frame.
    }
    const bool pointer = input.kind == ui::EventKind::pointer_down || input.kind == ui::EventKind::pointer_up ||
                         input.kind == ui::EventKind::pointer_move || input.kind == ui::EventKind::wheel;
    std::size_t level = menu_path_.size();
    POINT screen{static_cast<LONG>(input.x), static_cast<LONG>(input.y)};
    ClientToScreen(window(), &screen);
    if (pointer)
        for (std::size_t i = menu_path_.size() + 1; i > 0; --i) {
            const auto index = i - 1;
            auto* host = index ? submenus_[index - 1].get() : popup_.get();
            RECT bounds{};
            GetWindowRect(host->window(), &bounds);
            if (PtInRect(&bounds, screen)) {
                level = index;
                break;
            }
        }
    if (input.kind == ui::EventKind::key_down && !menu_path_.empty() &&
        (input.key == ui::Key::left || input.key == ui::Key::escape)) {
        ATELIER_TRY(submenus_[menu_path_.size() - 1]->hide());
        menu_path_.pop_back();
        return true;
    }
    auto* host = level ? submenus_[level - 1].get() : popup_.get();
    auto items = std::span<const ui::MenuItem>(workspace_.menu_items);
    for (std::size_t depth = 0; depth < level; ++depth) {
        const auto at =
            std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == menu_path_[depth]; });
        if (at == items.end()) {
            close_menu();
            return true;
        }
        items = at->children;
    }
    auto event = input;
    ScreenToClient(host->window(), &screen);
    event.x = static_cast<float>(screen.x);
    event.y = static_cast<float>(screen.y);
    const auto reply = host->input(event);
    if (reply.error) {
        close_menu();
        return std::unexpected(ui_error(*reply.error));
    }
    for (const auto& effect : reply.effects)
        if (const auto* command = std::get_if<ui::Action>(&effect)) {
            if (command->kind == ui::ActionKind::activate) {
                const auto at = std::find_if(items.begin(), items.end(),
                                             [&](const auto& item) { return item.key == command->key; });
                if (at == items.end() || !at->enabled)
                    continue;
                if (!at->children.empty()) {
                    menu_path_.resize(level);
                    menu_path_.push_back(at->key);
                    ATELIER_TRY(update_menu());
                    return true;
                }
                invoke_menu(*command, items);
                return true;
            } else if (command->kind == ui::ActionKind::dismiss) {
                close_menu();
                return true;
            }
        }
    // Right first uses the accepted popup's branch activation. Only a leaf
    // continues across the menubar through the ordinary request transition.
    if (navigate_top)
        change(editor::cycle_menu(source, false));
    return true;
}

bool Application::invoke_menu(const ui::Action& command, std::span<const ui::MenuItem> items) {
    const auto at = std::ranges::find(items, command.key, &ui::MenuItem::key);
    if (at == items.end() || !at->enabled || !at->children.empty())
        return false;
    const auto edit = at->edit_command;
    const bool keep_open = command.keep_menu_open && at->behavior == ui::MenuBehavior::checkbox;
    if (edit) {
        const auto incarnation = menu_incarnation_;
        close_menu();
        effects(ui_.execute(*edit, incarnation));
    } else {
        auto activation = command;
        if (activation.kind == ui::ActionKind::activate &&
            (activation.target ? activation.target : activation.key) == editor::control::menu_group_reset) {
            if (!workspace_.menu)
                return false;
            activation = {
                ui::ActionKind::reset, workspace_.menu->group ? workspace_.menu->group : workspace_.menu->source, {}};
        }
        // Retire the menu before a command can open a dialog or acquire capture
        // for a new tool. No menu-item reference survives this ownership change.
        if (!keep_open)
            close_menu();
        const auto result = action(activation);
        if (!result) {
            status(result, L"");
            if (keep_open)
                close_menu();
        }
    }
    return true;
}

bool Application::pie_input(const ui::Event& input, WPARAM native_key) {
    if ((input.kind == ui::EventKind::key_down && (input.key == ui::Key::escape || input.key == ui::Key::tab)) ||
        (input.kind == ui::EventKind::pointer_down && input.button == ui::PointerButton::secondary)) {
        close_menu();
        return true;
    }
    const bool pointer = input.kind == ui::EventKind::pointer_move || input.kind == ui::EventKind::pointer_down ||
                         input.kind == ui::EventKind::pointer_up;
    if (pointer)
        pointer_position_ = {input.x, input.y};
    const bool release = input.kind == ui::EventKind::key_up &&
                         native_key == (workspace_.menu->group == editor::control::viewport_axis_pie ? VK_OEM_3 : 'Z');
    if (release && !pie_release_select_) {
        const auto dx = pointer_position_.x - workspace_.menu->x;
        const auto dy = pointer_position_.y - workspace_.menu->y;
        pie_release_select_ = dx * dx + dy * dy > 324 * window_.dpi_scale() * window_.dpi_scale() ||
                              std::chrono::steady_clock::now() - menu_requested_ >= std::chrono::milliseconds(200);
    }
    const auto* frame = ui_.presented().get();
    const auto* pie = frame ? frame->find(editor::control::viewport_pie) : nullptr;
    if (!pie || frame->popup != pie->key) {
        if (release)
            pie_release_pending_ = true;
        return true; // Opening owns input before its first accepted geometry.
    }
    const auto choose = [&] {
        // A click on a visible slot wins over gesture direction when screen-edge
        // fitting moved the menu away from its original invocation point.
        if (pointer)
            for (const auto& item : workspace_.menu_items) {
                const auto* box = frame->find(item.key);
                if (item.enabled && box && pointer_position_.x >= box->bounds.x &&
                    pointer_position_.x < box->bounds.x + box->bounds.width && pointer_position_.y >= box->bounds.y &&
                    pointer_position_.y < box->bounds.y + box->bounds.height)
                    return item.key;
            }
        // Use the invocation point as the gesture origin even if the visible
        // menu was fitted away from a screen edge. Slots come from accepted DSL geometry.
        const auto dx = pointer_position_.x - workspace_.menu->x;
        const auto dy = pointer_position_.y - workspace_.menu->y;
        const auto distance = std::hypot(dx, dy);
        ui::ControlKey key{};
        float best = -2.f;
        if (distance > 18 * frame->drawing->dpi_scale)
            for (const auto& item : workspace_.menu_items) {
                const auto* box = frame->find(item.key);
                if (!item.enabled || !box)
                    continue;
                const auto x = box->bounds.x + box->bounds.width * .5f - (pie->bounds.x + pie->bounds.width * .5f);
                const auto y = box->bounds.y + box->bounds.height * .5f - (pie->bounds.y + pie->bounds.height * .5f);
                const auto length = std::hypot(x, y);
                if (length == 0)
                    continue;
                const auto dot = (dx * x + dy * y) / (distance * length);
                if (dot > best) {
                    best = dot;
                    key = item.key;
                }
            }
        return key;
    };
    const auto activate = [&](ui::ControlKey key) {
        if (key)
            invoke_menu({ui::ActionKind::activate, key, true}, workspace_.menu_items);
        else
            close_menu();
    };
    if (pointer) {
        pie_hot_ = choose();
        effects(ui_.focus(pie_hot_));
        if (input.kind == ui::EventKind::pointer_down && input.button == ui::PointerButton::primary)
            pie_pressed_ = true;
        if (input.kind == ui::EventKind::pointer_up && input.button == ui::PointerButton::primary && pie_pressed_)
            activate(pie_hot_);
        return true;
    }
    if (release) {
        if (pie_release_select_)
            activate(pie_hot_ ? pie_hot_ : choose());
        return true;
    }
    // Existing popup keyboard navigation and Enter activation share the same command dispatch.
    auto reply = ui_.input(input);
    pie_hot_ = ui_.focused_control();
    for (const auto& effect : reply.effects)
        if (const auto* command = std::get_if<ui::Action>(&effect)) {
            if (command->kind == ui::ActionKind::dismiss) {
                close_menu();
                break;
            }
            if (command->kind == ui::ActionKind::activate) {
                invoke_menu(*command, workspace_.menu_items);
                break;
            }
        }
    return true;
}

Result<void> Application::native(NativeEvent message) {
    if (control_)
        control_->input(message.message, message.wparam);
    if (message.relative_motion) {
        ui_.dismiss_tooltip();
        look_camera_fly(message.relative_motion->x, message.relative_motion->y);
        return {};
    }
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) {
        invalidate_surface();
        return {};
    }
    if (message.message == WM_MOVE) {
        ui_.dismiss_tooltip();
        finish_camera_fly(false);
        close_menu(false);
        return {};
    }
    if (message.message == WM_CANCELMODE || (message.message == WM_ACTIVATEAPP && !message.wparam)) {
        cancel_interaction(ui_.cancel_interaction());
        return {};
    }
    if (message.message == WM_SETFOCUS)
        return {};
    if (message.ime_owned)
        return {};
    if (message.composition) {
        ui::Event composition;
        composition.kind = ui::EventKind::composition;
        composition.data = std::move(*message.composition);
        auto reply = ui_.input(composition);
        if (message.error)
            reply.error = ui::Error{ui::ErrorCode::platform_failed, 0, message.error};
        effects(reply);
        return {};
    }
    if (message.error) {
        workspace_.status = L"Invalid native text input.";
        return {};
    }
    const auto native_key = message.wparam;
    auto mapped = input_event(std::move(message), window());
    if (!mapped)
        return {};
    return input(*mapped, native_key);
}

Result<void> Application::input(const ui::Event& event, WPARAM native_key) {
    if (event.kind != ui::EventKind::pointer_move)
        ui_.dismiss_tooltip();
    if (event.kind == ui::EventKind::key_up && (native_key == 'Y' || event.key == ui::Key::y))
        workspace_.network.set_cut_key(false);
    if (event.kind == ui::EventKind::pointer_leave) {
        effects(ui_.input(event));
        return {};
    }
    const bool pointer = event.kind == ui::EventKind::pointer_move || event.kind == ui::EventKind::pointer_down ||
                         event.kind == ui::EventKind::pointer_up || event.kind == ui::EventKind::wheel;
    if (event.kind == ui::EventKind::focus_lost || event.kind == ui::EventKind::capture_lost) {
        auto reply = ui_.input(event);
        if (ui_.presented())
            workspace_.dock.focus_control(ui_.focused_control(), *ui_.presented());
        cancel_interaction(reply, event.kind == ui::EventKind::focus_lost ? "focus_lost" : "capture_lost");
        return {};
    }
    // Menu toggles must ignore repeats on both sides of menu_input(), including
    // repeats arriving after Escape has closed the original menu.
    if (event.kind == ui::EventKind::key_down && event.repeat && (event.key == ui::Key::f10 || native_key == VK_MENU))
        return {};
    if (event.kind == ui::EventKind::pointer_down)
        camera_fly_release_.reset();
    if (camera_fly_release_ && event.kind == ui::EventKind::pointer_up && event.button == *camera_fly_release_) {
        camera_fly_release_.reset();
        return {};
    }
    if (camera_fly_ && event.kind == ui::EventKind::key_down &&
        (event.key == ui::Key::f10 || event.alt || (event.control && (native_key == VK_TAB || native_key == 'W'))))
        finish_camera_fly(false);
    ATELIER_TRY_BIND(menu_handled, menu_input(event, native_key));
    if (*menu_handled) {
        ui_.dismiss_tooltip();
        return {};
    }
    if (pointer)
        pointer_position_ = {event.x, event.y};
    if ((camera_fly_ || viewport_tool_.active()) && viewport_input(event, static_cast<std::uint32_t>(native_key))) {
        ui_.dismiss_tooltip();
        return {};
    }
    if (workspace_.network.active_gesture() && network_input(event, static_cast<std::uint32_t>(native_key))) {
        ui_.dismiss_tooltip();
        return {};
    }
    if (event.kind == ui::EventKind::pointer_move && !camera_fly_ && !camera_drag_ && !viewport_tool_.active() &&
        !workspace_.network.active_gesture() && !workspace_.dock.dragging() && !ui_.interaction_state().pointer &&
        !ui_.has_popup() && !workspace_.menu && !workspace_.color_edit && !workspace_.row_rename &&
        !workspace_.network.tab_search().open && !workspace_.network.popup().open() &&
        (!ui_.wants_keyboard() || ui_.focused_control() == editor::source_control::buffer) && pointer_ready() &&
        ui_.presented()) {
        if (workspace_.dock.focus_point(event.x, event.y, *ui_.presented()))
            effects(ui_.focus(0));
    }
    // Scene Tree owns Clear Parent; text fields and other panes retain the
    // Project mnemonic. Resolve this context before opening a top-level menu.
    const auto region = editor::region_of(workspace_.dock.focused_tab());
    if (region == editor::RegionKind::scene_tree &&
        key_input(event, static_cast<std::uint32_t>(native_key), region, editor::KeyPhase::before_controls))
        return {};
    ui::ControlKey requested_menu{};
    if (event.kind == ui::EventKind::key_down) {
        if (event.key == ui::Key::f10 && !event.shift)
            requested_menu = editor::control::file_menu;
        else if (event.alt && !event.control)
            for (const auto& group : editor::menu_groups)
                if (native_key == static_cast<WPARAM>(group.access_key)) {
                    requested_menu = group.key;
                    break;
                }
    }
    if (requested_menu) {
        if (event.repeat)
            return {};
        const auto menu = requested_menu;
        menu_return_focus_ = ui_.focused_control();
        if (workspace_.menu_bar_collapsed) {
            workspace_.menu_bar_collapsed = false;
            pending_menu_ = menu;
        } else {
            effects(ui_.focus(menu));
            request_menu({menu, 0, 0, 0});
        }
        return {};
    }
    if (event.kind == ui::EventKind::pointer_down && ui_.presented())
        for (const auto& box : ui_.presented()->controls)
            if (editor::control::is_menu(box.key) && event.x >= box.bounds.x &&
                event.x < box.bounds.x + box.bounds.width && event.y >= box.bounds.y &&
                event.y < box.bounds.y + box.bounds.height)
                menu_return_focus_ = ui_.focused_control();
    const bool release = event.kind == ui::EventKind::pointer_up;
    if (pointer && !release && !pointer_ready()) {
        ui_.dismiss_tooltip();
        return {};
    }
    if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::primary) {
        const auto menu = ui_.pointer_target(event.x, event.y);
        if (editor::control::is_menu(menu)) {
            effects(ui_.focus(menu));
            request_menu({menu, 0, 0, 0});
            return {};
        }
    }
    if (!workspace_.menu && key_input(event, static_cast<std::uint32_t>(native_key), editor::RegionKind::none,
                                      editor::KeyPhase::before_controls))
        return {};
    if (viewport_input(event, static_cast<std::uint32_t>(native_key))) {
        ui_.dismiss_tooltip();
        return {};
    }
    if (network_input(event, static_cast<std::uint32_t>(native_key))) {
        if (event.kind != ui::EventKind::pointer_move || workspace_.network.active_gesture())
            ui_.dismiss_tooltip();
        return {};
    }
    const auto previous_focus = ui_.focused_control();
    auto reply = ui_.input(event);
    if (ui_.presented() && ui_.focused_control() != previous_focus && ui_.focused_control() != 0)
        workspace_.dock.focus_control(ui_.focused_control(), *ui_.presented());
    if (!effects(reply))
        return {};
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        if (key_input(event, static_cast<std::uint32_t>(native_key), region, editor::KeyPhase::after_controls) ||
            key_input(event, static_cast<std::uint32_t>(native_key), editor::RegionKind::none,
                      editor::KeyPhase::after_controls))
            return {};
        if (!workspace_.sample_history() && !event.repeat && event.key == ui::Key::escape)
            status(document_->cancel_edit(), L"Edit cancelled");
    }
    return {};
}

Result<void> Application::enable_control(const std::filesystem::path& directory) {
    if (!control_) {
        auto created = EditorControl::create(*this, directory);
        if (!created)
            return std::unexpected(Error{ErrorCode::invalid_storage, created.error().what()});
        control_ = std::make_unique<EditorControl>(std::move(*created));
    }
    return {};
}

Result<bool> Application::step(renderer::Capture* capture, StepTimes* times) {
    // The frame closes after run_step, so the retirement of its frame-local containers is included.
    timing::Recorder::Frame frame{recorder_};
    sampled_frame_.reset();
    if (times)
        *times = {};
    auto running = run_step(capture, times);
    if (!running) {
        close_menu(false);
        report_error(running.error(), "editor.frame_failed");
        preserve_failure(running.error().message);
    }
    auto& performance = workspace_.lighting.performance();
    performance.observe(frame.finish(), sampled_frame_);
    // Only a sampled step shows a reading, so readback and stalls never appear as the live FPS.
    const auto& recent = performance.snapshot().cpu.recent_interval;
    if (!sampled_frame_ || !workspace_.show_fps || !workspace_.dock.visible(editor::control::scene_tab) || !recent ||
        *recent <= 0)
        workspace_.viewport_fps.clear();
    else if (workspace_.viewport_fps.empty() || *recent != fps_interval_) {
        fps_interval_ = *recent;
        workspace_.viewport_fps = std::format(L"{:.1f} FPS  ·  {:.1f} ms", 1000 / *recent, *recent);
    }
    return running;
}

Result<bool> Application::run_step(renderer::Capture* capture, StepTimes* times) try {
    using Clock = std::chrono::steady_clock;
    frame_statistics_ = {};
    if (closed_)
        return false;
    // No dispatch or business callbacks run inside this wait. Input only wakes
    // the service pass; RHI retains any presentation permission it acquired.
    if (graphics(renderer_.target()).drawable) {
        timing::Scope admission{phases::admission};
        static_cast<void>(graphics(renderer_.wait_frame(20)));
    }
    // Everything this step changed reaches Play as one batch, whichever way the step ends.
    struct ForwardLive {
        Application& app;

        ~ForwardLive() { app.forward_live(); }
    } forward_live{*this};
    {
        timing::Scope input{phases::input};
        Result<void> polled;
        if (times)
            times->input = Clock::now();
        window_.poll([&](NativeEvent&& event) {
            if (polled)
                polled = native(std::move(event));
        });
        ATELIER_TRY(polled);
        if (window_.closing()) {
            const auto confirmed = confirm_discard();
            if (!confirmed)
                status(Result<void>{std::unexpected(confirmed.error())}, L"");
            else if (*confirmed) {
                ATELIER_TRY(close());
                return false;
            }
            window_.cancel_close();
        }
    }
    {
        timing::Scope project{phases::project};
        poll_project();
        diagnostics();
        static_cast<void>(workspace_.poll_output());
    }
    if (control_) {
        timing::Scope control{phases::control};
        control_->poll();
    }
    const auto title = (project_ ? project_->name() : L"No project") + L" — " +
                       (scene_path_.empty() ? L"Untitled" : scene_path_.filename().wstring()) +
                       (document_->dirty() ? L" *" : L"") + L" — FrostFox Atelier";
    if (title != window_title_) {
        SetWindowTextW(window(), title.c_str());
        window_title_ = title;
    }
    auto target = graphics(renderer_.target());
    const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) {
        auto error = ui_error(updated.error());
        error.message = "UI rejected the window surface. " + error.message;
        return std::unexpected(std::move(error));
    }
    effects(*updated);
    if (updated->error)
        return std::unexpected(ui_error(*updated->error));
    {
        timing::Scope collecting{phases::gc};
        auto gc = document_->collect_step();
        if (!gc)
            throw editor::detail::Failure(std::move(gc.error()));
    }
    // Keep the last committed scopes while minimized; an empty surface cannot
    // validate a new layout or make its controls available for interaction.
    if (!target.drawable) {
        lighting_clock_.reset();
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    // Only recoverable render failures enter this gate. Service work above
    // remains live, while input wakeups cannot turn a persistent error into
    // repeated UI/scene preparation. Normal successful frames have no cap.
    if (const auto now = Clock::now(); now < render_retry_at_) {
        const auto wait = std::min(std::chrono::ceil<std::chrono::milliseconds>(render_retry_at_ - now),
                                   std::chrono::milliseconds(20));
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wait.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    bool host_ready;
    {
        timing::Scope admission{phases::admission};
        host_ready = graphics(renderer_.wait_frame(0));
    }
    if (!host_ready) {
        if (pending_menu_ || !workspace_.menu || workspace_.pie_menu())
            return true;
        if (menu_window() && popup_->source() == workspace_.menu->source) {
            // Native menus have their own surface and use the last accepted owner geometry.
            timing::Scope menus{phases::popup};
            ATELIER_TRY(update_menu());
            return true;
        }
        // A newly requested native menu needs its command model projected and validated
        // even when the owner cannot present. Keep the normal projection path for its first frame.
    }
    update_camera_fly();
    const ui_layout::Size view_size{static_cast<float>(extent.width) / surface_.dpi_scale,
                                    static_cast<float>(extent.height) / surface_.dpi_scale};
    workspace_.camera_flying = camera_fly_.has_value();
    workspace_.viewport_mode = viewport_tool_.displayed_tool(workspace_.viewport_settings);
    workspace_.viewport_readout = viewport_tool_.readout();
    workspace_.camera_view = camera_.view();
    workspace_.view.grid_spacing = workspace_.viewport_settings.grid_spacing;
    const auto plane_normal = camera_.construction_normal();
    workspace_.view.grid_plane = plane_normal.z != 0   ? renderer::GridPlane::xy
                                 : plane_normal.x != 0 ? renderer::GridPlane::yz
                                                       : renderer::GridPlane::xz;
    // After its pipelines are ready this is a bit check; before, the frame draws a fallback.
    if (const auto preparation = renderer_.preparation(control_ ? control_->view() : workspace_.view); !preparation)
        workspace_.render_notice = L"Shader preparation failed: " + diagnostic_text(preparation.error().cause.message);
    else if (!preparation->ready)
        workspace_.render_notice =
            L"Compiling shaders " + std::to_wstring(preparation->done) + L"/" + std::to_wstring(preparation->total);
    // Retain the previous material progress/error until that request is adopted;
    // the next poll is after layout, which may cancel a live viewport edit.
    else if (!scene_.picking() || (scene_.draw() && scene_.draw()->picking == scene_.picking()))
        workspace_.render_notice.clear();
    const auto describe = [&]() -> Result<std::vector<ui::Control>> {
        // Selection/document commands can cancel the business edit before the next UI frame.
        if (workspace_.color_edit && !document_->owns_edit(editor::control::color_group))
            workspace_.color_edit.reset();
        ATELIER_TRY_BIND(controls, workspace_.describe(*document_, view_size, ui_.presented().get(),
                                                       project_ ? &*project_ : nullptr, &ui_, command_focus()));
        if (workspace_.color_edit &&
            (workspace_.color_edit->composition_failed || std::ranges::none_of(*controls, [](const auto& control) {
                 return control.key == editor::control::color_editor;
             }))) {
            const auto diagnostic = std::string(workspace_.ui_error());
            if (auto cancelled = finish_color_edit(false); !cancelled)
                throw editor::detail::Failure(std::move(cancelled.error()));
            controls = workspace_.describe(*document_, view_size, ui_.presented().get(),
                                           project_ ? &*project_ : nullptr, &ui_, command_focus());
            ATELIER_TRY(controls);
            workspace_.status = L"Colour editor could not open" +
                                (diagnostic.empty() ? std::wstring{} : L": " + diagnostic_text(diagnostic));
        }
        return controls;
    };
    std::optional<timing::Scope> stage{std::in_place, phases::ui};
    ATELIER_TRY_BIND(controls, describe());
    auto described = ui_.set_controls(*controls);
    if (!described && described.error().code != ui::ErrorCode::text_failed &&
        workspace_.reject_ui_controls("UI candidate rejected at control " + std::to_string(described.error().key) +
                                      ": " + ui_error(described.error()).message)) {
        controls = describe();
        ATELIER_TRY(controls);
        described = ui_.set_controls(*controls);
    }
    if (!described) {
        auto error = ui_error(described.error());
        error.message =
            "UI rejected editor controls (key " + std::to_string(described.error().key) + "). " + error.message;
        return std::unexpected(std::move(error));
    }
    effects(*described);
    if (described->error)
        return std::unexpected(ui_error(*described->error));
    if (workspace_.pie_menu())
        effects(ui_.focus(pie_hot_));
    stage.emplace(phases::layout);
    auto prepared = ui_.prepare();
    if (!prepared && prepared.error().code != ui::ErrorCode::text_failed &&
        workspace_.reject_ui_controls("Editor layout candidate rejected at control " +
                                      std::to_string(prepared.error().key) + ": " +
                                      ui_error(prepared.error()).message)) {
        controls = describe();
        ATELIER_TRY(controls);
        described = ui_.set_controls(*controls);
        if (!described) {
            auto error = ui_error(described.error());
            error.message = "UI rejected restored editor controls (key " + std::to_string(described.error().key) +
                            "). " + error.message;
            return std::unexpected(std::move(error));
        }
        effects(*described);
        if (described->error)
            return std::unexpected(ui_error(*described->error));
        prepared = ui_.prepare();
    }
    if (!prepared) {
        stage.reset();
        lighting_clock_.reset();
        auto error = ui_error(prepared.error());
        error.message = (ui_.presented() ? "Frame preparation failed; previous frame retained: "
                                         : "Initial editor frame preparation failed (UI code " +
                                               std::to_string(static_cast<int>(prepared.error().code)) + "). ") +
                        error.message;
        return std::unexpected(std::move(error));
    }
    const auto previous_ui_revision = workspace_.ui_revision();
    workspace_.commit_ui_controls();
    if (workspace_.ui_revision() != previous_ui_revision) {
        ui_.dismiss_tooltip();
        if ((*prepared)->tooltip) {
            prepared = ui_.prepare();
            if (!prepared) {
                stage.reset();
                auto error = ui_error(prepared.error());
                error.message = "Frame preparation failed; previous frame retained: " + error.message;
                return std::unexpected(std::move(error));
            }
        }
    }
    // Native popups render on their retained surface before scene submission /
    // Present can block. Their anchor still comes from accepted owner geometry.
    // A pending keyboard reveal waits for this frame's acceptance; closing here would drop it.
    stage.reset();
    if (!workspace_.pie_menu() && !pending_menu_) {
        timing::Scope menus{phases::popup};
        ATELIER_TRY(update_menu());
    }
    if (!host_ready)
        return true;
    const auto& frame = **prepared;
    const auto* viewport = frame.find(editor::control::viewport);
    const auto scene_bounds = viewport ? viewport->bounds : ui::Rect{};
    if (camera_fly_ && (!viewport || (accepted_view_ && accepted_view_->bounds != scene_bounds)))
        finish_camera_fly(false);
    const bool had_viewport_gesture = viewport_tool_.active();
    if (had_viewport_gesture && accepted_view_ && accepted_view_->bounds != scene_bounds)
        status(viewport_tool_.cancel(*document_), L"Viewport changed; edit cancelled");
    viewport_tool_.forget(*document_);
    if (had_viewport_gesture && !viewport_tool_.active())
        window_.capture(false);
    const bool in_sop = !workspace_.active_graph.empty() && document_->has_graph(workspace_.active_graph);
    const procedural::Geometry* sop_template_geom = nullptr;
    renderer::Result<renderer::Preparation> material_preparation{renderer::Preparation{0, 0, true}};
    {
        timing::Scope snapshot{phases::scene};
        if (viewport) {
            if (in_sop) {
                const auto eval = document_->evaluate_graph(workspace_.active_graph, project_ ? &*project_ : nullptr);
                if (!eval) {
                    take(procedural_preview_.refresh(*document_, project_ ? &*project_ : nullptr, nullptr, 0));
                    workspace_.render_notice = L"SOP cook error: " + diagnostic_text(eval.error().message);
                } else {
                    take(procedural_preview_.refresh(*document_, project_ ? &*project_ : nullptr, eval->preview_output,
                                                     eval->generation));
                    if (eval->template_output && eval->template_node != eval->display_node)
                        sop_template_geom = eval->template_output;
                    if (eval->display_error)
                        workspace_.render_notice = L"SOP cook error: " + diagnostic_text(eval->display_error->message) +
                                                   (eval->preview_output ? L" (showing last valid output)" : L"");
                }
                material_preparation = procedural_preview_.prepare(renderer_);
            } else {
                take(document_->sync_procedural_generators(project_ ? &*project_ : nullptr));
                const bool editing_network =
                    workspace_.inspecting_network ||
                    editor::region_of(workspace_.dock.focused_tab()) == editor::RegionKind::network;
                const bool network_visible = workspace_.dock.visible(editor::network_control::tab);
                take(scene_.refresh(*document_, !editing_network, !network_visible));
                material_preparation = scene_.prepare(renderer_);
            }
            if (!material_preparation) {
                if (material_preparation.error().terminal)
                    throw std::runtime_error("Renderer stopped: " + material_preparation.error().cause.message);
                workspace_.render_notice =
                    L"Material preparation failed: " + diagnostic_text(material_preparation.error().cause.message);
                if (control_)
                    control_->render_failed(material_preparation.error().cause.message);
            } else if (!material_preparation->ready)
                workspace_.render_notice = L"Compiling materials " + std::to_wstring(material_preparation->done) +
                                           L"/" + std::to_wstring(material_preparation->total);
        }
        // Any edit of the Environment (Parameters, deletion, Play) reaches the evaluated view before rendering;
        // only such a change re-anchors the lighting focus.
        if (workspace_.sync_world(*document_))
            sync_world_view();
    }
    diagnostics();
    const auto* scene = viewport && !in_sop ? scene_.draw() : nullptr;
    const auto* sop_draw = viewport && in_sop ? procedural_preview_.draw() : nullptr;
    if (viewport && (in_sop ? !sop_draw : !scene)) {
        // Do not submit a partial first group. Input still belongs to the last
        // accepted view, if any; compilation never publishes a picking frame.
        lighting_clock_.reset();
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    const bool material_pending = viewport && (!material_preparation || !material_preparation->ready);
    const auto camera = camera_.matrices(std::max(scene_bounds.width, 1.f) / std::max(scene_bounds.height, 1.f));
    auto viewport_view = editor::ViewportView::make(camera_, scene_bounds, window_.dpi_scale());
    if (sop_draw)
        viewport_view.scene = sop_draw->picking;
    else if (scene)
        viewport_view.scene = scene->picking;
    overlay_.clear();
    if (viewport) {
        if (in_sop) {
            if (sop_template_geom)
                append_template_wireframe(*sop_template_geom, viewport_view, overlay_);
            if (sop_draw && sop_draw->shown_geometry) {
                const auto* shown = sop_draw->shown_geometry.get();
                append_procedural_guides(*shown, viewport_view,
                                         {workspace_.effective_show_points(shown), workspace_.sop_show_normals,
                                          workspace_.sop_show_face_normals, workspace_.sop_show_orient,
                                          workspace_.sop_show_bounds, true},
                                         overlay_);
            }
        } else {
            viewport_tool_.overlay(*document_, viewport_view, workspace_.viewport_settings, overlay_);
            if (const ObjectId gen_comp = workspace_.active_procedural_generator(*document_); gen_comp != ObjectId{}) {
                auto& runtime = document_->world().runtime();
                if (const auto src = presentation::read_procedural_source(runtime, gen_comp);
                    src && src->output && src->output->geometry) {
                    const auto* geom = src->output->geometry.get();
                    float chunk_size = 32.f;
                    static_cast<void>(runtime.with_native<const presentation::ProceduralGeneratorComponent>(
                        gen_comp, [&](const presentation::ProceduralGeneratorComponent& comp) -> Result<void> {
                            chunk_size = comp.chunk_size;
                            return {};
                        }));
                    math::Matrix4 world_mtx = math::Matrix4::identity();
                    if (const auto comp_state = world::inspect_component(runtime, gen_comp);
                        comp_state && comp_state->valid) {
                        if (const auto mtx = world::world_matrix(runtime, comp_state->owner))
                            world_mtx = *mtx;
                    }
                    if (!src->source.empty() && document_->has_graph(src->source)) {
                        if (const auto eval = document_->evaluate_graph(src->source, project_ ? &*project_ : nullptr);
                            eval && eval->template_output && eval->template_node != eval->display_node)
                            append_template_wireframe(*eval->template_output, viewport_view, overlay_, world_mtx);
                    }
                    append_procedural_guides(*geom, viewport_view,
                                             {workspace_.effective_show_points(geom), workspace_.sop_show_normals,
                                              workspace_.sop_show_face_normals, workspace_.sop_show_orient,
                                              workspace_.sop_show_bounds, true, chunk_size, world_mtx},
                                             overlay_);
                }
            }
        }
        append_orientation_gnomon(viewport_view, overlay_);
    }
    const bool projection_cut = accepted_view_ && accepted_view_->orthographic != viewport_view.orthographic;
    renderer::Capture control_capture;
    const bool capturing_control = control_ && control_->wants_capture();
    auto* frame_capture = capture ? capture : capturing_control ? &control_capture : nullptr;
    const auto effective_view = control_ ? control_->view() : workspace_.view;
    const auto frame_meshes = sop_draw ? std::span<const renderer::MeshInstance>{sop_draw->meshes}
                              : scene  ? std::span<const renderer::MeshInstance>{scene->meshes}
                                       : std::span<const renderer::MeshInstance>{};
    const auto frame_lights =
        scene ? std::span<const renderer::Light>{scene->lights} : std::span<const renderer::Light>{};
    const auto frame_identity = sop_draw ? sop_draw->identity : scene ? scene->identity : scene_.identity;
    renderer::Frame render_frame{*frame.drawing,
                                 {camera.view, camera.projection, camera.far_depth},
                                 scene_bounds,
                                 frame_meshes,
                                 effective_view,
                                 overlay_,
                                 frame_lights,
                                 frame_identity,
                                 camera_cut_ || projection_cut,
                                 rendering::sky_radiance(effective_view.lit),
                                 lighting_focus()};
    render_frame.materials = sop_draw ? sop_draw->materials.get() : scene ? scene->materials.get() : nullptr;
    render_frame.command_trace = control_ ? control_->command_trace() : nullptr;
    std::optional<renderer::Result<renderer::FrameResult>> traced_result;
    if (render_frame.command_trace)
        traced_result.emplace();
    const auto lighting_start = Clock::now();
    const bool lighting_active = scene_bounds.width > 0 && scene_bounds.height > 0 && !frame_meshes.empty() &&
                                 effective_view.shading == renderer::Shading::scene_lit &&
                                 effective_view.wireframe != renderer::Wireframe::only && effective_view.lit.ddgi;
    render_frame.render_delta_seconds = lighting_clock_.delta(lighting_start, lighting_active);
    auto drawn =
        lit_fallback_.render(renderer_, render_frame, frame_capture, traced_result ? &*traced_result : nullptr);
    if (traced_result) {
        control_->traced(render_frame, *traced_result, !drawn && drawn.error().terminal);
        render_frame.command_trace = nullptr;
    }
    if (!drawn) {
        lighting_clock_.reset();
        if (control_)
            control_->render_failed(drawn.error().cause.message);
        if (drawn.error().terminal)
            throw std::runtime_error("Renderer stopped: " + drawn.error().cause.message);
        render_retry_delay_ =
            std::min(render_retry_delay_.count() ? render_retry_delay_ * 2 : std::chrono::milliseconds(20),
                     std::chrono::milliseconds(250));
        render_retry_at_ = Clock::now() + render_retry_delay_;
        workspace_.status = diagnostic_text(drawn.error().cause.message);
        return true;
    }
    const auto& rendered = drawn->result;
    auto drawn_frame = render_frame;
    drawn_frame.view = drawn->view;
    frame_statistics_ = rendered.stats;
    workspace_.lighting.performance().observe(drawn_frame, rendered);
    // Control sees a fallback as the requested frame not being ready, so it is neither a Lit sample nor its last frame.
    if (control_ && (drawn->fallback || material_pending)) {
        if (!drawn->failure.empty())
            control_->render_failed(drawn->failure);
        renderer::FrameResult not_ready;
        not_ready.status = renderer::FrameStatus::not_ready;
        control_->rendered(render_frame, not_ready, nullptr);
    }
    if (rendered.status == renderer::FrameStatus::accepted) {
        if (rendered.accepted_id != frame.drawing->id || rendered.accepted_extent != frame.drawing->extent)
            throw std::runtime_error("Renderer accepted a mismatched frame.");
        if (times)
            times->accept_begin = Clock::now();
        if (!ui_.accept(*prepared))
            throw std::runtime_error("UI rejected an accepted renderer frame.");
        if (times)
            times->accepted = Clock::now();
        if (scene)
            scene_.accept(rendered.accepted_id, drawn->fallback, drawn->view.shading);
        render_retry_at_ = {};
        render_retry_delay_ = {};
        lighting_clock_.accept(lighting_start, lighting_active && drawn->view.shading == renderer::Shading::scene_lit);
        fallback_presented_ = drawn->fallback;
        accepted_view_ = viewport ? std::optional(viewport_view) : std::nullopt;
        workspace_.lighting.observe(rendered.stats);
        if (viewport)
            camera_cut_ = false;
        ++accepted_frames_;
        if (!frame_capture)
            sampled_frame_ = rendered.accepted_id;
        if (control_ && !drawn->fallback && !material_pending) {
            if (capture && capturing_control) {
                control_capture = *capture;
                control_->rendered(render_frame, rendered, &control_capture);
            } else
                control_->rendered(render_frame, rendered, frame_capture);
        }
        if (pending_source_focus_) {
            pending_source_focus_ = false;
            if (workspace_.source_editor.active())
                effects(ui_.focus(editor::source_control::buffer));
        }
        if (pending_network_search_focus_ &&
            (!workspace_.network.tab_search().open ||
             (ui_.presented() && ui_.presented()->find(editor::network_control::tab_search_input)))) {
            pending_network_search_focus_ = false;
            if (workspace_.network.tab_search().open)
                effects(ui_.focus(editor::network_control::tab_search_input));
        }
        // A revealed row can take a frame to be described and presented.
        if (pending_rename_focus_ &&
            (!workspace_.row_rename || (ui_.presented() && ui_.presented()->find(editor::control::inline_rename)))) {
            pending_rename_focus_ = false;
            if (workspace_.row_rename)
                effects(ui_.focus(editor::control::inline_rename));
        }
        if (pending_name_focus_) {
            pending_name_focus_ = false;
            if (const auto key = workspace_.name_control(document_->selection().entity);
                key && ui_.presented() && ui_.presented()->find(key))
                effects(ui_.focus(key));
        }
        if (pending_parameter_search_focus_) {
            pending_parameter_search_focus_ = false;
            if (workspace_.parameter_search_visible)
                effects(ui_.focus(editor::control::parameter_search));
        }
        sync_text_input();
        timing::Scope menus{phases::popup};
        if (pending_menu_) {
            const auto menu = pending_menu_;
            pending_menu_ = 0;
            effects(ui_.focus(menu));
            request_menu({menu, 0, 0, 0});
        } else
            ATELIER_TRY(update_menu());
    } else {
        lighting_clock_.reset();
        if (control_ && !drawn->fallback && !material_pending)
            control_->rendered(render_frame, rendered, nullptr);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return true;
} catch (const std::exception& error) {
    report_error(editor::detail::caught_error(error, ErrorCode::invalid_storage), "editor.frame_failed");
    preserve_failure(error.what());
    throw;
} catch (...) {
    preserve_failure("Unknown editor frame failure");
    throw;
}

Result<void> Application::close() {
    if (closed_)
        return {};
    Result<void> result;
    const auto retire = [&](Result<void> next) {
        if (next)
            return;
        if (result)
            result = std::move(next);
        else
            report_error(next.error(), "application.cleanup_failed");
    };
    finish_camera_fly(false);
    closed_ = true;
    control_.reset();
    play_.close();
    consume_play_output();
    static_cast<void>(live_.close());
    if (!play_snapshot_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(play_snapshot_, ignored);
        play_snapshot_.clear();
    }
    close_menu(false);
    for (auto& submenu : submenus_)
        retire(submenu->close());
    if (popup_)
        retire(popup_->close());
    cancel_interaction(ui_.cancel_interaction());
    retire(document_->cancel_edit());
    window_.capture(false);
    workspace_.reset_document();
    project_code_ = nullptr;
    diagnostics();
    const auto closed = document_->close();
    diagnostics();
    retire(completion(closed));
    scene_.reset();
    procedural_preview_.reset();
    retire(renderer_.close().transform_error(renderer_error));
    return result;
}
}
