#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/presentation.hpp"
#include "application/diagnostics.hpp"
#include "editor/keymap.hpp"
#include "procedural/graph.hpp"
#include "foundation/result.hpp"
#include "foundation/scope_exit.hpp"
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
#include <filesystem>
#include <format>
#include <limits>

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
std::unexpected<Error> renderer_stopped(const renderer::Error& error) {
    return std::unexpected(
        Error{ErrorCode::invalid_storage, String::concat("Renderer stopped: ", error.cause.message)});
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

Application::Application(SceneRecovery* recovery) : recovery_(recovery ? recovery : &owned_recovery_) {}

Result<void> Application::initialize(std::unique_ptr<editor::Session> document, bool validation, bool activate,
                                     Optional<ui::PixelExtent> initial_client, bool vsync) {
    document_ = std::move(document);
    ATELIER_TRY(workspace_.initialize());
    gpu_validation_ = validation;
    vsync_ = vsync;
    ATELIER_TRY(window_.initialize(nullptr, initial_client));
    ATELIER_TRY_BIND(fonts, editor_fonts());
    ATELIER_TRY(ui_.initialize(*fonts).transform_error(ui_error));
    require_frame_timing();
    auto style = ui_.style();
    style.font.family = L"Sarasa UI SC";
    style.font.size = 12.5f;
    style.caption_scale = 1;
    style.title_scale = 7.f / 6;
    if (auto styled = ui_.set_style(style); !styled) {
        auto error = ui_error(styled.error());
        error.message = String::concat("Cannot publish editor typography: ", error.message);
        return std::unexpected(std::move(error));
    }
    Array<std::byte> initial_style_source;
    ATELIER_TRY_BIND(styles, editor_styles(&initial_style_source));
    style_sheet_ = std::move(*styles);
    if (auto styled = ui_.set_style_sheet(style_sheet_); !styled) {
        auto error = ui_error(styled.error());
        error.message = String::concat("Cannot publish editor style sheet: ", error.message);
        return std::unexpected(std::move(error));
    }
    if (auto style_path = editor_style_path()) {
        style_watch_.path = std::move(*style_path);
        std::error_code ec;
        style_watch_.time = std::filesystem::last_write_time(style_watch_.path, ec);
        if (!ec)
            style_watch_.size = std::filesystem::file_size(style_watch_.path, ec);
        style_watch_.error = ec.value();
        style_watch_.has_stamp = true;
    }
    style_watch_.accepted_source = std::move(initial_style_source);
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    options.vsync = vsync_;
    options.frame_latency_waitable = true;
    options.shader_cache = shader_cache_directory();
    ATELIER_TRY_BIND(renderer, renderer::Renderer::open(window_.handle(), options).transform_error(renderer_error));
    renderer_ = std::move(*renderer);
    auto popup = std::make_unique<PopupHost>();
    ATELIER_TRY(popup->initialize(renderer_.device(), window(), *fonts, gpu_validation_));
    popup_ = std::move(popup);
    auto submenu = std::make_unique<PopupHost>();
    ATELIER_TRY(submenu->initialize(renderer_.device(), window(), *fonts, gpu_validation_));
    submenus_.add(std::move(submenu));
    closed_ = false;
    window_.show(activate);
    return {};
}

Application::~Application() {
    // External cleanup errors remain diagnostics. World/Renderer destructors
    // enforce fatal only when their own necessary retirement cannot complete.
    if (auto result = close(); !result)
        report_error(result.error(), "application.cleanup_failed");
}

void Application::notice(StringView code, std::wstring_view message, logging::Severity severity) noexcept {
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
    logging::Entry entry{.severity = severity,
                         .category = "application"_s,
                         .event_code = String::from_utf8_validated(code),
                         .message = std::move(*text)};
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
                             .category = "world"_s,
                             .event_code = "world.diagnostics_dropped"_s,
                             .message = "World diagnostics exceeded their retention capacity."_s};
        entry.fields = {{"document", document_->presentation().identity()},
                        {"dropped_total", total},
                        {"dropped_delta", total - dropped_diagnostics_}};
        static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
        dropped_diagnostics_ = total;
    }
}

void Application::preserve_failure(StringView reason) noexcept {
    if (recovery_->attempted() || !document_ || closed_)
        return;
    const auto message = diagnostic_utf8(reason).utf8_prefix(logging::maximum_message_bytes);
    const auto submitted = logging::submit({.severity = logging::Severity::critical,
                                            .category = "application"_s,
                                            .event_code = "application.failure_preserved"_s,
                                            .message = String::from_utf8_validated(message)});
    if (submitted.status != logging::SubmitStatus::accepted) {
        std::fputs("Editor failure preserved: ", stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
    }
    window_.capture(false);
    window_.cursor(ui::Cursor::arrow);
    recovery_->preserve(*document_, project_ ? project_->root() : std::filesystem::path{}, scene_path_, reason);
}

void Application::report_error(const Error& error, StringView event, logging::Severity severity) noexcept {
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
    const auto focus = lighting_focus();
    if (!math::is_finite(focus) || math::max_component(math::cwise_abs(focus)) > 100000.f) {
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
    ATELIER_TRY(editor::validate_view_options(workspace_.view, camera.state().target));
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
    auto context = workspace_.operator_context(*document_, project_.get(), interactive, command_focus());
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
    Optional<ui::Action> routed;
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
            return std::unexpected(Error{
                ErrorCode::invalid_storage,
                String::concat("Cannot open the original run directory (Shell ", opened, ").")});
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
        if (workspace_.color_edit) {
            const auto k = request->source;
            const bool in_color_editor = k == color_editor || k == color_hex || k == color_intensity ||
                                         (k >= color_rgb && k < color_rgb + 3) || (k >= color_hsv && k < color_hsv + 3);
            if (in_color_editor)
                return {};
            ATELIER_TRY(finish_color_edit(true));
        }
        // A row menu acts on the selection: right-clicking outside it selects that row first.
        if (const auto* row = workspace_.binding(request->source);
            row && row->kind == editor::Workspace::Binding::Kind::object && row->path.empty() &&
            row->object != ObjectId{} &&
            (document_->world().contains(row->object) ? !document_->is_selected(row->object)
                                                      : document_->selection().object != row->object))
            ATELIER_TRY(workspace_.select_tree(*document_, *this, request->source));
        request_menu(*request);
        return {};
    }
    if (event.kind == ui::ActionKind::dismiss && event.key == editor::control::color_editor)
        return finish_color_edit(true);
    if (event.key == editor::control::inline_rename && workspace_.row_rename) {
        if (event.kind == ui::ActionKind::edit_preview) {
            if (const auto* text = std::get_if<std::wstring>(&event.value)) {
                workspace_.row_rename->text = *text;
                workspace_.row_rename->invalid = false;
            }
            return {};
        }
        if (event.kind != ui::ActionKind::edit_commit && event.kind != ui::ActionKind::edit_cancel)
            return {};
        if (event.kind == ui::ActionKind::edit_cancel) {
            const auto rename = workspace_.row_rename.take();
            effects(ui_.focus(!rename->asset.empty() ? editor::browser_control::results
                                                     : workspace_.object_control(rename->object)));
            workspace_.status = L"Rename cancelled";
            return {};
        }
        const auto* text = std::get_if<std::wstring>(&event.value);
        if (text)
            workspace_.row_rename->text = *text;
        const auto current = *workspace_.row_rename;
        if (!current.asset.empty()) {
            if (!text || *text == current.asset.filename().wstring() || !project_) {
                workspace_.row_rename.reset();
                effects(ui_.focus(editor::browser_control::results));
                return {};
            }
            auto renamed = project_->rename(current.asset, *text);
            if (!renamed) {
                workspace_.row_rename->invalid = true;
                pending_rename_focus_ = true;
                return std::unexpected(renamed.error());
            }
            workspace_.row_rename.reset();
            asset_relocated(current.asset, *renamed);
            workspace_.assets.reselect(*renamed);
            effects(ui_.focus(editor::browser_control::results));
            workspace_.status = L"Renamed to " + renamed->filename().wstring();
            return {};
        }
        if (!text || *text == document_->name(current.object)) {
            workspace_.row_rename.reset();
            effects(ui_.focus(workspace_.object_control(current.object)));
            return {};
        }
        auto name = platform::utf16_to_utf8(*text);
        if (!name) {
            workspace_.row_rename->invalid = true;
            pending_rename_focus_ = true;
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
        }
        Result<void> res;
        if (name->empty()) {
            res = document_->rename(current.object, *text);
        } else {
            json::Builder args;
            args.set_validated(args.root(), "name", *name);
            res =
                run_operator({"scene.rename",
                              args.finish(),
                              {.selection = editor::SelectionContext{current.object, current.object, {current.object}},
                               .region = {}}},
                             true, false);
        }
        if (!res) {
            workspace_.row_rename->invalid = true;
            pending_rename_focus_ = true;
            return std::unexpected(res.error());
        }
        workspace_.row_rename.reset();
        effects(ui_.focus(workspace_.object_control(current.object)));
        return done(std::move(res), L"Entity renamed / Undo restores its name");
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
    if (event.kind == ui::ActionKind::activate &&
        event.key >= editor::source_control::first_document + editor::source_control::close_document_offset &&
        event.key < editor::source_control::first_document + editor::source_control::close_document_offset * 2) {
        const auto doc_key = event.key - editor::source_control::close_document_offset;
        return close_source_tab(doc_key, true);
    }
    if (event.kind == ui::ActionKind::activate && workspace_.source_editor.select(event.key)) {
        close_menu(false);
        workspace_.dock.select(editor::source_control::tab);
        pending_source_focus_ = true;
        return {};
    }
    {
        bool focus_buffer = false;
        ui::ControlKey focus_bar = 0;
        ATELIER_TRY_BIND(code_handled, workspace_.source_editor.apply_action(event, focus_buffer, focus_bar));
        if (*code_handled) {
            close_menu(false);
            if (focus_buffer) {
                workspace_.dock.select(editor::source_control::tab);
                pending_source_focus_ = true;
            }
            if (focus_bar)
                pending_code_bar_focus_ = focus_bar;
            return {};
        }
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::new_scratch) {
        ATELIER_TRY(document_->create_scratch_graph());
        return open_graph_context("scratch/procedural");
    }
    const String net_graph = workspace_.effective_network_graph(*document_, project_.get());
    const bool net_locked = workspace_.is_network_graph_locked(*document_, net_graph);
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::lock_toggle) {
        if (!net_graph.empty() && !document_->is_scratch_graph(net_graph) && !document_->world().running()) {
            const bool next_locked = !net_locked;
            workspace_.network_lock_overrides.insert_or_assign(net_graph, next_locked);
            if (next_locked) {
                workspace_.network.cancel_gesture();
                workspace_.network.close_tab_search();
                workspace_.network.close_popup();
            }
            workspace_.bump_view_revision();
            workspace_.status =
                next_locked ? L"Locked Preview: shared .procgraph protected from accidental edits"
                            : L"Unlocked shared .procgraph: live edits propagate to all AssetNodes and Generators";
        }
        return {};
    }
    if (event.kind == ui::ActionKind::activate && event.key == editor::network_control::pin_toggle) {
        if (!workspace_.network_pinned) {
            if (!net_graph.empty()) {
                workspace_.network_pinned = true;
                workspace_.pinned_graph = (!workspace_.graph_breadcrumbs.empty() && workspace_.active_graph.empty())
                                              ? workspace_.graph_breadcrumbs.front().graph_key
                                              : net_graph;
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
            const String cur_graph = workspace_.active_graph;
            const String root_graph =
                workspace_.graph_breadcrumbs.empty() ? cur_graph : workspace_.graph_breadcrumbs.front().graph_key;
            auto saved_crumbs = std::move(workspace_.graph_breadcrumbs);
            const String saved_sel = workspace_.selected_graph_node;
            exit_graph_context();
            if (workspace_.selected_generator_graph(*document_) != root_graph && document_->has_graph(root_graph)) {
                workspace_.network_pinned = true;
                workspace_.pinned_graph = root_graph;
            }
            if (!saved_crumbs.empty() &&
                (workspace_.network_pinned || workspace_.selected_generator_graph(*document_) == root_graph)) {
                workspace_.graph_breadcrumbs = std::move(saved_crumbs);
                workspace_.dived_network_graph = cur_graph;
            }
            if (!saved_sel.empty() && document_->graph_node(cur_graph, saved_sel))
                workspace_.set_single_graph_selection(saved_sel);
            workspace_.status = L"Returned 3D Viewport to World scene";
            return {};
        }
        if (!net_graph.empty()) {
            auto saved_crumbs = std::move(workspace_.graph_breadcrumbs);
            const String saved_sel = workspace_.selected_graph_node;
            ATELIER_TRY(open_graph_context(net_graph));
            workspace_.graph_breadcrumbs = std::move(saved_crumbs);
            if (!saved_sel.empty() && document_->graph_node(net_graph, saved_sel))
                workspace_.set_single_graph_selection(saved_sel);
            return {};
        }
        return {};
    }
    if (event.kind == ui::ActionKind::activate)
        if (const auto choice = workspace_.network_path_choice(event.key)) {
            using Kind = editor::Workspace::NetworkPathChoice::Kind;
            workspace_.graph_breadcrumbs.clear();
            workspace_.dived_network_graph.clear();
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
        if (net_graph.empty() || !document_->has_graph(net_graph) || document_->world().running() || net_locked)
            return {};
        const auto canvas_dip = network_canvas_dip();
        String from_node;
        String from_port;
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
        if (net_graph.empty() || !document_->has_graph(net_graph) || !workspace_.network.tab_search().open ||
            net_locked)
            return {};
        ATELIER_TRY_BIND(catalog, document_->procedural_node_types(project_ ? &*project_ : nullptr, net_graph));
        ATELIER_TRY_BIND(created, workspace_.network.commit_tab_pick(
                                      *document_, net_graph, *catalog,
                                      static_cast<std::size_t>(event.key - editor::network_control::tab_pick_first),
                                      workspace_.selected_graph_node, workspace_.selected_graph_nodes, false,
                                      project_ ? &*project_ : nullptr));
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
        ATELIER_TRY_BIND(handled,
                         workspace_.network.apply_action(*document_, net_graph, workspace_.selected_graph_node,
                                                         workspace_.selected_graph_nodes, network_canvas_dip(), event,
                                                         net_status, document_->world().running() || net_locked));
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
    if (ui_.presented()) {
        ATELIER_TRY_BIND(handled, workspace_.dock.apply(event, *ui_.presented()).transform_error(ui_error));
        if (*handled)
            return {};
    }
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
        if (event.key == editor::network_control::nav_back && !workspace_.graph_breadcrumbs.empty())
            return pop_graph_breadcrumb();
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
        if (event.key == tree_world) {
            if (!workspace_.tree_query.empty())
                return {};
            const std::array world_id{ObjectId{}};
            return workspace_.set_tree_expanded(*document_, world_id, *expanded);
        }
        const auto* row = workspace_.binding(event.key);
        if (!row || !row->path.empty() || row->kind != editor::Workspace::Binding::Kind::object)
            return invalid();
        if (!workspace_.tree_query.empty())
            return {};
        return workspace_.set_tree_expanded(*document_, Span{&row->object, 1}, *expanded);
    }
    if (event.kind == ui::ActionKind::activate && event.key == outliner_components_toggle) {
        workspace_.outliner_show_components = !workspace_.outliner_show_components;
        if (!workspace_.outliner_show_components) {
            const auto sel = document_->selection();
            if (sel.entity != ObjectId{} && sel.object != sel.entity) {
                ATELIER_TRY(end_interaction());
                ATELIER_TRY(completion(workspace_.select_object(*document_, *this, sel.entity)));
            }
        }
        workspace_.status = workspace_.outliner_show_components ? L"Showing inline components in Scene Outliner"
                                                                : L"Hiding inline components in Scene Outliner";
        return {};
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
    if (event.kind == ui::ActionKind::activate &&
        (event.key == tree_filter_clear || event.key == parameter_search_clear)) {
        if (event.key == tree_filter_clear) {
            workspace_.tree_query.clear();
            workspace_.status = L"Outliner filter cleared";
        } else {
            workspace_.query.clear();
            workspace_.status = L"Parameter filter cleared";
        }
        return {};
    }
    if (event.key == parameter_search || event.key == tree_filter) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value)
            return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit ||
            event.kind == ui::ActionKind::edit_cancel) {
            (event.key == tree_filter ? workspace_.tree_query : workspace_.query) = *value;
            workspace_.status = event.kind == ui::ActionKind::edit_cancel ? L"Filter cancelled"
                                : event.key == tree_filter                ? L"Outliner filtered"
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
                              : Optional<editor::Workspace::TreeDrop>{};
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
        if (binding->kind == editor::Workspace::Binding::Kind::graph_param_override_toggle) {
            if (event.kind != ui::ActionKind::activate)
                return {};
            const auto* enabled = std::get_if<bool>(&event.value);
            if (!enabled)
                return invalid();
            auto* proj = project_ ? &*project_ : nullptr;
            const bool is_gen_comp = binding->component != ObjectId{};
            const editor::PromotedParameterTarget target{binding->slot, binding->node_name, binding->component};
            if (!*enabled) {
                return done(document_->clear_promoted_parameter_override(target, binding->material_path, proj),
                            is_gen_comp ? L"Reverted generator override to Digital Asset default"
                                        : L"Reverted parameter override to Digital Asset default");
            }
            ATELIER_TRY_BIND(
                params, is_gen_comp ? document_->generator_promoted_parameters(binding->component, proj)
                                    : document_->graph_asset_node_parameters(binding->slot, binding->node_name, proj));
            const auto pit =
                std::ranges::find(*params, binding->material_path, &editor::GraphPromotedParameterView::name);
            if (pit == params->end() || !pit->valid || pit->properties.empty())
                return invalid();
            const wchar_t* msg = is_gen_comp ? L"Enabled explicit generator parameter override"
                                             : L"Enabled explicit Digital Asset override";
            if (pit->properties.front().is_string) {
                return done(document_->set_promoted_parameter_string(target, binding->material_path,
                                                                     pit->properties.front().string_value, proj),
                            msg);
            }
            Array<Scalar> scalars;
            scalars.reserve(pit->properties.size());
            for (const auto& prop : pit->properties)
                scalars.add(prop.value);
            return done(document_->set_promoted_parameter_scalars(target, binding->material_path, scalars, proj), msg);
        }
        if (binding->kind == editor::Workspace::Binding::Kind::graph_param_text) {
            if (event.kind != ui::ActionKind::edit_commit)
                return {};
            const auto* wide = std::get_if<std::wstring>(&event.value);
            if (!wide)
                return invalid();
            const auto utf8 = platform::utf16_to_utf8(*wide);
            if (!utf8)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
            const wchar_t* msg = binding->component != ObjectId{} ? L"Generator parameter override updated"
                                 : binding->node_name.empty()     ? L"Graph parameter updated"
                                                                  : L"Digital Asset override updated";
            const editor::PromotedParameterTarget target{binding->slot, binding->node_name, binding->component};
            return done(document_->set_promoted_parameter_string(target, binding->material_path, *utf8,
                                                                 project_ ? &*project_ : nullptr),
                        msg);
        }
        if (binding->kind == editor::Workspace::Binding::Kind::graph_param_field ||
            binding->kind == editor::Workspace::Binding::Kind::graph_param_slider) {
            auto* proj = project_ ? &*project_ : nullptr;
            const bool is_gen_comp = binding->component != ObjectId{};
            if (event.kind == ui::ActionKind::activate) {
                const auto* checked = std::get_if<bool>(&event.value);
                if (!checked)
                    return {};
                const Scalar val{*checked};
                const wchar_t* msg = is_gen_comp                  ? L"Generator parameter override updated"
                                     : binding->node_name.empty() ? L"Graph parameter updated"
                                                                  : L"Digital Asset override updated";
                const editor::PromotedParameterTarget target{binding->slot, binding->node_name, binding->component};
                return done(
                    document_->set_promoted_parameter_scalars(target, binding->material_path, Span{&val, 1}, proj),
                    msg);
            }
            const auto* number = std::get_if<double>(&event.value);
            if (!number || !std::isfinite(*number))
                return invalid();
            const auto params = is_gen_comp ? document_->generator_promoted_parameters(binding->component, proj)
                                : binding->node_name.empty()
                                    ? document_->graph_promoted_parameters(binding->slot)
                                    : document_->graph_asset_node_parameters(binding->slot, binding->node_name, proj);
            if (!params)
                return std::unexpected(params.error());
            const auto pit =
                std::ranges::find(*params, binding->material_path, &editor::GraphPromotedParameterView::name);
            if (pit == params->end() || !pit->valid || binding->index >= pit->properties.size())
                return invalid();
            ATELIER_TRY_BIND(typed, scalar_value(pit->properties[binding->index].value, *number));
            const Optional<std::size_t> lane =
                pit->is_vector ? Optional<std::size_t>{binding->index} : Optional<std::size_t>{};
            switch (event.kind) {
            case ui::ActionKind::edit_begin:
                return done(document_->begin_graph_parameter_edit(binding->slot, binding->node_name,
                                                                  binding->material_path, lane, event.key, proj,
                                                                  binding->component),
                            L"Editing Digital Asset parameter / Esc cancels");
            case ui::ActionKind::edit_preview:
                return done(document_->preview_graph_parameter_edit(event.key, Span{&*typed, 1}),
                            L"Parameter preview / Esc cancels");
            case ui::ActionKind::edit_commit: {
                if (auto previewed = document_->preview_graph_parameter_edit(event.key, Span{&*typed, 1});
                    !previewed) {
                    static_cast<void>(document_->cancel_edit());
                    return previewed;
                }
                return done(document_->commit_edit(event.key), L"Digital Asset parameter committed");
            }
            default:
                return {};
            }
        }
        if (binding->kind == editor::Workspace::Binding::Kind::graph_param_context) {
            if (event.kind != ui::ActionKind::reset)
                return {};
            auto* proj = project_ ? &*project_ : nullptr;
            const editor::PromotedParameterTarget target{binding->slot, binding->node_name, binding->component};
            if (binding->component != ObjectId{} ||
                (binding->component == ObjectId{} && !binding->node_name.empty() && binding->path.empty())) {
                return done(document_->clear_promoted_parameter_override(target, binding->material_path, proj),
                            binding->component != ObjectId{} ? L"Reverted generator override to Digital Asset default"
                                                             : L"Reverted override to Digital Asset default");
            }
            if (binding->component == ObjectId{} && binding->node_name.empty() && binding->path.empty()) {
                ATELIER_TRY_BIND(params, document_->graph_promoted_parameters(binding->slot));
                const auto pit =
                    std::ranges::find(*params, binding->material_path, &editor::GraphPromotedParameterView::name);
                if (pit == params->end() || !pit->valid || pit->properties.empty())
                    return {};
                if (pit->properties.front().is_string) {
                    return done(
                        document_->set_promoted_parameter_string(target, binding->material_path, pit->default_string),
                        L"Promoted parameter restored to default");
                }
                if (!pit->default_scalars.empty()) {
                    return done(
                        document_->set_promoted_parameter_scalars(target, binding->material_path, pit->default_scalars),
                        L"Promoted parameter restored to default");
                }
                return {};
            }
            if (!binding->path.empty() && binding->object != ObjectId{}) {
                auto& runtime = document_->world().runtime();
                auto cur_type = runtime.type_of(binding->object);
                if (!cur_type)
                    return std::unexpected(cur_type.error());
                const FieldInfo* leaf_field = nullptr;
                for (const auto fid : binding->path) {
                    const auto info = runtime.describe(*cur_type);
                    if (!info)
                        return std::unexpected(info.error());
                    const auto fit = std::ranges::find((*info)->fields, fid, &FieldInfo::id);
                    if (fit == (*info)->fields.end())
                        return {};
                    leaf_field = &*fit;
                    cur_type = fit->value.type;
                }
                if (!leaf_field || leaf_field->default_leaves.empty())
                    return {};
                const auto leaf_desc = runtime.describe(*cur_type);
                if (!leaf_desc)
                    return std::unexpected(leaf_desc.error());
                Array<editor::PropertyPatch> patches;
                if ((*leaf_desc)->math.vector() &&
                    leaf_field->default_leaves.size() == (*leaf_desc)->math.components() &&
                    (*leaf_desc)->fields.size() >= leaf_field->default_leaves.size()) {
                    for (std::size_t li = 0; li < leaf_field->default_leaves.size(); ++li) {
                        world::FieldPath lane_path = binding->path;
                        lane_path.add((*leaf_desc)->fields[li].id);
                        patches.add({std::move(lane_path), leaf_field->default_leaves[li]});
                    }
                } else if (leaf_field->default_leaves.size() == 1) {
                    patches.add({binding->path, leaf_field->default_leaves.front()});
                } else {
                    return {};
                }
                return done(document_->set_properties(binding->object, patches), L"Parameter restored to default");
            }
            return {};
        }
        if (binding->kind == editor::Workspace::Binding::Kind::material_override) {
            if (event.kind != ui::ActionKind::activate)
                return {};
            const auto* enabled = std::get_if<bool>(&event.value);
            if (!enabled)
                return invalid();
            return document_->set_material_override(binding->object, binding->material_path, *enabled, project_.get());
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
        if (event.kind == ui::ActionKind::activate && binding->kind == editor::Workspace::Binding::Kind::object &&
            binding->path.empty() && binding->object != ObjectId{}) {
            auto& rt = document_->world().runtime();
            ObjectId owner_entity = binding->object;
            bool current_enabled = true;
            if (document_->world().contains(binding->object)) {
                ATELIER_TRY_BIND(st, world::inspect_entity(rt, binding->object));
                current_enabled = st->enabled;
            } else {
                ATELIER_TRY_BIND(st, world::inspect_component(rt, binding->object));
                owner_entity = st->owner;
                current_enabled = st->enabled;
            }
            json::Builder args;
            static_cast<void>(args.set(args.root(), "enabled", !current_enabled));
            auto toggled =
                run_operator({"scene.set_enabled",
                              args.finish(),
                              {.selection = editor::SelectionContext{owner_entity, binding->object, {owner_entity}},
                               .region = editor::RegionKind::scene_tree}},
                             true, false);
            return done(std::move(toggled),
                        !current_enabled ? L"Enabled in World / Undo disables" : L"Disabled in World / Undo enables");
        }
        if (const auto* modifiers = std::get_if<ui::SelectionModifiers>(&event.value);
            event.kind == ui::ActionKind::select && modifiers && modifiers->double_click && !modifiers->shift &&
            !modifiers->control && binding->kind == editor::Workspace::Binding::Kind::object && binding->path.empty() &&
            document_->world().contains(binding->object) && document_->is_selected(binding->object)) {
            // Double-clicking a selected Entity row renames it in place (Blender Outliner / Houdini Tree View).
            workspace_.dock.select(editor::control::entities_tab);
            return run_operator(
                {"scene.rename",
                 {},
                 {.selection = editor::SelectionContext{binding->object, binding->object, {binding->object}},
                  .region = editor::RegionKind::scene_tree}});
        }
        if (event.kind == ui::ActionKind::select && binding->kind == editor::Workspace::Binding::Kind::object &&
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
Result<void> Application::open_color_edit(ObjectId object, Span<const FieldId> prefix, ui::ControlKey swatch) {
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
            world::FieldPath path{prefix};
            ATELIER_TRY_BIND(info, document_->world().runtime().describe(take_type(object, prefix)));
            if (lane >= (*info)->fields.size())
                return std::unexpected(Error{ErrorCode::invalid_definition, "A colour needs three channels."});
            path.add((*info)->fields[lane].id);
            ATELIER_TRY_BIND(value, document_->read_property(object, path));
            edit.original[lane] = edit.value[lane] = std::visit(
                [](const auto& v) -> double {
                    if constexpr (std::same_as<std::decay_t<decltype(v)>, EnumValue>)
                        return v.bits;
                    else
                        return double(v);
                },
                *value);
            edit.paths.add(std::move(path));
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
    edit.x = box ? (box->bounds.x + box->bounds.width) / dpi - 416 : 0;
    edit.y = box ? (box->bounds.y + box->bounds.height) / dpi + 2 : 0;
    const auto init_hsv = editor::rgb_to_hsv(edit.value);
    edit.hue = init_hsv[0];
    edit.saturation = init_hsv[1];
    edit.hdr_scale = edit.initial_hdr_scale =
        std::max(1.0, std::max(edit.value[0], std::max(edit.value[1], edit.value[2])));
    edit.hex = editor::color_hex(edit.value);
    workspace_.color_edit = std::move(edit);
    workspace_.status = L"Colour editor · Done or click outside to keep · Revert restores";
    return {};
}

TypeId Application::take_type(ObjectId object, Span<const FieldId> path) const {
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
    Array<Scalar> values;
    for (std::size_t lane = 0; lane < 3; ++lane) {
        const auto value = rgb[lane];
        if (!std::isfinite(value) || value < 0 || value > edit.maximum)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Colour channels must stay within the property's numeric range."});
        ATELIER_TRY_BIND(prototype, document_->read_property(edit.object, edit.paths[lane]));
        ATELIER_TRY_BIND(typed, scalar_value(*prototype, value));
        values.add(*typed);
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
        const auto orig_hsv = editor::rgb_to_hsv(edit.original);
        edit.hue = orig_hsv[0];
        edit.saturation = orig_hsv[1];
        edit.hdr_scale = edit.initial_hdr_scale =
            std::max(1.0, std::max(edit.original[0], std::max(edit.original[1], edit.original[2])));
        ATELIER_TRY(apply_color(edit.original));
        return true;
    }
    if (event.key == color_sv_plane) {
        if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit &&
            event.kind != ui::ActionKind::edit_cancel)
            return true;
        const auto* sv = std::get_if<ui::DragPoint>(&event.value);
        if (!sv || !std::isfinite(sv->x) || !std::isfinite(sv->y))
            return true;
        const double s = std::clamp(static_cast<double>(sv->x), 0.0, 1.0);
        const double v = std::clamp(static_cast<double>(sv->y), 0.0, 1.0) * std::max(1.0, edit.hdr_scale);
        edit.saturation = s;
        ATELIER_TRY(apply_color(editor::hsv_to_rgb({edit.hue, s, std::min(v, edit.maximum)})));
        return true;
    }
    if (event.key == color_hue_bar) {
        if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit &&
            event.kind != ui::ActionKind::edit_cancel)
            return true;
        const auto* h_val = std::get_if<double>(&event.value);
        if (!h_val || !std::isfinite(*h_val))
            return true;
        const auto cur_hsv = editor::rgb_to_hsv(edit.value);
        edit.hue = std::clamp(*h_val, 0.0, 360.0);
        ATELIER_TRY(apply_color(editor::hsv_to_rgb({edit.hue, edit.saturation, cur_hsv[2]})));
        return true;
    }
    if (event.key == color_intensity || event.key == color_intensity_slider) {
        if (event.kind == ui::ActionKind::edit_begin) {
            edit.initial_hdr_scale = edit.hdr_scale;
            return true;
        }
        if (event.kind != ui::ActionKind::edit_preview && event.kind != ui::ActionKind::edit_commit &&
            event.kind != ui::ActionKind::edit_cancel)
            return true;
        const auto* v_val = std::get_if<double>(&event.value);
        if (!v_val || !std::isfinite(*v_val))
            return true;
        const double next_v = std::clamp(*v_val, 0.0, edit.maximum);
        if (event.kind == ui::ActionKind::edit_cancel) {
            edit.hdr_scale = edit.initial_hdr_scale;
        } else if (event.kind == ui::ActionKind::edit_commit) {
            edit.hdr_scale = edit.initial_hdr_scale = std::max(1.0, next_v);
        } else if (event.key == color_intensity && next_v > edit.hdr_scale) {
            edit.hdr_scale = std::max(1.0, next_v);
        }
        ATELIER_TRY(apply_color(editor::hsv_to_rgb({edit.hue, edit.saturation, next_v})));
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
        const auto next_hsv = editor::rgb_to_hsv(*parsed);
        if (next_hsv[1] > 1e-5)
            edit.hue = next_hsv[0];
        if (next_hsv[2] > 1e-6)
            edit.saturation = next_hsv[1];
        edit.hdr_scale = edit.initial_hdr_scale =
            std::max(1.0, std::max((*parsed)[0], std::max((*parsed)[1], (*parsed)[2])));
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
    if (rgb) {
        next[event.key - color_rgb] = *value;
        const auto next_hsv = editor::rgb_to_hsv(next);
        if (next_hsv[1] > 1e-5)
            edit.hue = next_hsv[0];
        if (next_hsv[2] > 1e-6)
            edit.saturation = next_hsv[1];
        if (event.kind == ui::ActionKind::edit_commit)
            edit.hdr_scale = edit.initial_hdr_scale = std::max(1.0, std::max(next[0], std::max(next[1], next[2])));
        else
            edit.hdr_scale = std::max(1.0, std::max(next[0], std::max(next[1], next[2])));
    } else {
        auto h = editor::rgb_to_hsv(edit.value);
        h[0] = edit.hue;
        h[1] = edit.saturation;
        h[event.key - color_hsv] = *value;
        if (event.key == color_hsv)
            edit.hue = std::clamp(*value, 0.0, 360.0);
        if (event.key == color_hsv + 1)
            edit.saturation = std::clamp(*value, 0.0, 1.0);
        if (event.key == color_hsv + 2) {
            h[2] = std::clamp(*value, 0.0, edit.maximum);
            if (event.kind == ui::ActionKind::edit_commit)
                edit.hdr_scale = edit.initial_hdr_scale = std::max(1.0, h[2]);
            else if (h[2] > edit.hdr_scale)
                edit.hdr_scale = std::max(1.0, h[2]);
        }
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

Result<void> Application::rename_asset(StringView key, StringView name) {
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
    asset_relocated(entry->path, *renamed);
    workspace_.assets.reselect(*renamed);
    workspace_.status = L"Renamed to " + renamed->filename().wstring();
    return {};
}

Result<void> Application::open_graph_context(StringView key) {
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
    workspace_.graph_breadcrumbs.clear();
    workspace_.dived_network_graph.clear();
    if (workspace_.active_graph.empty())
        saved_world_camera_ = camera_;
    workspace_.active_graph = String::from_utf8_validated(key);
    const auto default_sel = workspace_.default_selected_graph_node(*document_, key);
    workspace_.set_single_graph_selection(default_sel);
    workspace_.dock.select(editor::network_control::tab);
    workspace_.dock.select(editor::control::parameters_tab);
    workspace_.bump_view_revision();
    camera_cut_ = true;
    workspace_.status = L"SOP context: " + workspace_.graph_path_title(project_.get());
    return {};
}

void Application::exit_graph_context() {
    close_menu(false);
    workspace_.network.cancel_gesture();
    workspace_.network.set_cut_key(false);
    workspace_.network.close_tab_search();
    workspace_.graph_breadcrumbs.clear();
    workspace_.dived_network_graph.clear();
    workspace_.active_graph.clear();
    if (saved_world_camera_) {
        camera_ = *saved_world_camera_;
        workspace_.camera_view = camera_.view();
        saved_world_camera_.reset();
    }
    workspace_.inspecting_network = false;
    workspace_.set_single_graph_selection({});
    workspace_.bump_view_revision();
    camera_cut_ = true;
}

Result<void> Application::dive_into_asset_node(StringView parent_graph, StringView node_name) {
    if (parent_graph.empty() || node_name.empty())
        return {};
    const auto* gnode = document_->graph_node(parent_graph, node_name);
    auto& runtime = document_->world().runtime();
    ATELIER_TRY_BIND(asset_node_type, runtime.find_type("procedural.AssetNode"));
    if (!gnode || !gnode->node.registered() || runtime.type_of(gnode->node.id()) != *asset_node_type ||
        gnode->asset_key.empty())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Selected node has no Digital Asset assigned."});
    const String target_key = gnode->asset_key;
    if (target_key == parent_graph || std::ranges::any_of(workspace_.graph_breadcrumbs, [&](const auto& crumb) {
            return crumb.graph_key == target_key;
        })) {
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Circular Digital Asset reference."});
    }
    close_menu(false);
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Finish the current field edit before diving into a Digital Asset."});
    if (!document_->has_graph(target_key)) {
        if (!project_)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Open a project before opening a Digital Asset (.procgraph)."});
        ATELIER_TRY(document_->open_graph(*project_, target_key));
    }
    workspace_.network.cancel_gesture();
    workspace_.network.close_tab_search();
    workspace_.network.close_popup();
    workspace_.network_lock_overrides.remove(target_key);
    workspace_.graph_breadcrumbs.add({String::from_utf8_validated(parent_graph), String::from_utf8_validated(node_name)});
    if (!workspace_.active_graph.empty()) {
        workspace_.active_graph = target_key;
        camera_cut_ = true;
    } else {
        workspace_.dived_network_graph = target_key;
    }
    const auto default_sel = workspace_.default_selected_graph_node(*document_, target_key);
    workspace_.set_single_graph_selection(default_sel);
    workspace_.inspecting_asset = false;
    workspace_.material_inspection.clear();
    workspace_.inspecting_network = true;
    workspace_.dock.select(editor::network_control::tab);
    workspace_.dock.select(editor::control::parameters_tab);
    workspace_.bump_view_revision();
    workspace_.status = workspace_.is_network_graph_locked(*document_, target_key)
                            ? L"Dived into Digital Asset (Locked Preview · Click Locked in Network toolbar to edit)"
                            : L"Dived into Digital Asset (Unlocked)";
    return {};
}

Result<void> Application::pop_graph_breadcrumb() {
    if (workspace_.graph_breadcrumbs.empty())
        return {};
    close_menu(false);
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Finish the current field edit before leaving the Digital Asset."});
    workspace_.network.cancel_gesture();
    workspace_.network.close_tab_search();
    workspace_.network.close_popup();
    editor::Workspace::GraphBreadcrumb crumb;
    bool found_valid = false;
    while (!workspace_.graph_breadcrumbs.empty()) {
        crumb = std::move(workspace_.graph_breadcrumbs.back());
        workspace_.graph_breadcrumbs.pop();
        if (!document_->has_graph(crumb.graph_key) && project_ && project_->entry(crumb.graph_key))
            static_cast<void>(document_->open_graph(*project_, crumb.graph_key));
        if (document_->has_graph(crumb.graph_key)) {
            found_valid = true;
            break;
        }
    }
    if (!found_valid) {
        workspace_.dived_network_graph.clear();
        if (!workspace_.active_graph.empty())
            exit_graph_context();
        else {
            workspace_.set_single_graph_selection({});
            workspace_.inspecting_network = false;
            workspace_.bump_view_revision();
        }
        return {};
    }
    if (!workspace_.active_graph.empty()) {
        workspace_.active_graph = crumb.graph_key;
        camera_cut_ = true;
    } else {
        workspace_.dived_network_graph = workspace_.graph_breadcrumbs.empty() ? String{} : crumb.graph_key;
    }
    const String next_sel = document_->graph_node(crumb.graph_key, crumb.asset_node_name)
                                ? crumb.asset_node_name
                                : workspace_.default_selected_graph_node(*document_, crumb.graph_key);
    workspace_.set_single_graph_selection(next_sel);
    workspace_.inspecting_asset = false;
    workspace_.material_inspection.clear();
    workspace_.inspecting_network = true;
    workspace_.bump_view_revision();
    workspace_.status = L"Returned to parent network";
    return {};
}

void Application::select_graph_node(StringView key, StringView node_name) {
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

void Application::select_graph_nodes(StringView key, Span<const String> node_names) {
    const auto net_key = workspace_.current_network_graph(*document_);
    if (workspace_.active_graph != key && net_key != key)
        return;
    workspace_.inspecting_asset = false;
    workspace_.material_inspection.clear();
    workspace_.selected_graph_nodes.assign(node_names);
    workspace_.selected_graph_node = node_names.empty() ? String{} : node_names.back();
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
    workspace_.bump_view_revision();
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
            call.id = "view.frame_all"_s;
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

void Application::cancel_interaction(const ui::Reply& reply, StringView input_event, std::source_location caller) {
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
    prepared_menu_frame_.reset();
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
    menu_projection_.reset();
    menu_dirty_ = true;
    menu_return_focus_ = return_focus;
    menu_incarnation_ = source->incarnation;
    menu_requested_ = std::chrono::steady_clock::now();
    menu_context_target_ = source->context_target;
    menu_context_group_ = source->context_group;
    menu_table_layout_ = source->table_layout;
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.requested",
                                       logging::native::Context{fields}, "Menu request adopted"));
}

Application::MenuProjection Application::current_menu_projection() const noexcept {
    MenuProjection state{};
    if (workspace_.menu) {
        state.source = workspace_.menu->source;
        state.group = workspace_.menu->group;
        state.table = workspace_.menu->table;
    }
    state.document = document_.get();
    state.document_revision = document_ ? document_->revision() : 0;
    state.project_revision = project_ ? project_->revision() : 0;
    state.view_revision = workspace_.view_revision();
    state.ui_revision = workspace_.ui_revision();
    state.build = workspace_.project_build.get();
    state.selection = document_ ? document_->selection() : editor::Selection{};
    state.project_open = workspace_.project_open;
    state.project_running = workspace_.project_running;
    state.project_stopping = workspace_.project_stopping;
    return state;
}

void Application::close_menu(bool restore_focus, std::source_location caller, StringView input_event) {
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
        logging::Field{"caller_function", String::from_utf8_validated(caller.function_name())},
        logging::Field{"input_event", String::from_utf8_validated(input_event)}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.closed", logging::native::Context{fields},
                                       "Menu closing"));
    workspace_.menu.reset();
    workspace_.menu_items.clear();
    workspace_.network_context_node.clear();
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
    menu_projection_.reset();
    prepared_menu_frame_.reset();
    menu_dirty_ = false;
}

Result<void> Application::update_menu() {
    if (!workspace_.menu || workspace_.menu_items.empty()) {
        close_menu();
        return {};
    }
    const auto request = *workspace_.menu;
    const auto* source = ui_.presented()->find(request.source);
    const auto& selection_frame = prepared_menu_frame_ ? *prepared_menu_frame_ : *ui_.presented();
    if (!source || source->incarnation != menu_incarnation_ || !source->enabled ||
        source->context_target != menu_context_target_ || source->context_group != menu_context_group_ ||
        source->table_layout != menu_table_layout_ || !live_table_context(*ui_.presented(), request, false) ||
        !live_table_context(selection_frame, request, true) || source->bounds.y >= source->clip.bottom ||
        source->bounds.y + source->bounds.height <= source->clip.top) {
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
                                logging::Field{"diagnostic", popup_->diagnostic()}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::warning, "menu", "menu.rejected",
                                           logging::native::Context{fields}, "Popup update rejected"));
        workspace_.status = L"UI: " + diagnostic_text(popup_->diagnostic());
        close_menu(false);
        return {};
    }
    window_.capture(true);
    ATELIER_TRY(popup_->render());
    auto items = Span<const ui::MenuItem>(workspace_.menu_items);
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
            ATELIER_TRY_BIND(fonts, editor_fonts());
            ATELIER_TRY(child->initialize(renderer_.device(), window(), *fonts, gpu_validation_));
            submenus_.add(std::move(child));
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
        menu_path_.pop();
        return true;
    }
    auto* host = level ? submenus_[level - 1].get() : popup_.get();
    auto items = Span<const ui::MenuItem>(workspace_.menu_items);
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
                    menu_path_.add(at->key);
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

bool Application::invoke_menu(const ui::Action& command, Span<const ui::MenuItem> items) {
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
        } else if (keep_open && workspace_.menu)
            menu_dirty_ = true;
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
        workspace_.source_editor.update_pointer(nullptr, 0.f, 0.f, false);
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
    if (workspace_.dock.visible(editor::source_control::tab)) {
        if (event.kind == ui::EventKind::pointer_move) {
            workspace_.source_editor.update_pointer(ui_.presented().get(), pointer_position_.x, pointer_position_.y,
                                                    event.control);
        } else if (event.kind == ui::EventKind::pointer_leave) {
            workspace_.source_editor.dismiss_hover();
        } else if (event.kind == ui::EventKind::text || event.kind == ui::EventKind::composition) {
            workspace_.source_editor.dismiss_hover();
        } else if (event.kind == ui::EventKind::key_down || event.kind == ui::EventKind::key_up) {
            workspace_.source_editor.set_ctrl_hover(ui_.presented().get(), event.control);
        }
    }
    if (event.kind == ui::EventKind::key_down) {
        const auto focused_key = ui_.focused_control();
        const bool in_code_buffer = focused_key == editor::source_control::buffer;
        const bool in_code_bar = focused_key == editor::source_control::find_input ||
                                 focused_key == editor::source_control::replace_input ||
                                 focused_key == editor::source_control::goto_input;
        if (in_code_buffer && workspace_.source_editor.completion_open()) {
            if (!event.control && !event.alt && event.key == ui::Key::down) {
                workspace_.source_editor.step_completion(1);
                return {};
            }
            if (!event.control && !event.alt && event.key == ui::Key::up) {
                workspace_.source_editor.step_completion(-1);
                return {};
            }
            if (!event.control && !event.alt && !event.shift &&
                (event.key == ui::Key::enter || event.key == ui::Key::tab)) {
                if (workspace_.source_editor.accept_selected_completion())
                    return {};
            }
            if (!event.control && !event.alt && !event.shift && event.key == ui::Key::escape) {
                workspace_.source_editor.dismiss_completion();
                return {};
            }
        }
        if (in_code_buffer || in_code_bar) {
            if (in_code_buffer && event.control && !event.alt && !event.shift && event.key == ui::Key::space) {
                workspace_.source_editor.trigger_completion(true);
                return {};
            }
            if (in_code_buffer && !event.control && !event.alt && !event.shift && event.key == ui::Key::f12) {
                ATELIER_TRY_BIND(jumped, workspace_.source_editor.go_to_definition());
                if (*jumped)
                    pending_source_focus_ = true;
                return {};
            }
            if (event.control && !event.alt && !event.shift && event.key == ui::Key::f) {
                pending_code_bar_focus_ = workspace_.source_editor.open_search(editor::CodeSearchMode::find);
                return {};
            }
            if (event.control && !event.alt && !event.shift && event.key == ui::Key::h) {
                pending_code_bar_focus_ = workspace_.source_editor.open_search(editor::CodeSearchMode::replace);
                return {};
            }
            if (event.control && !event.alt && !event.shift && event.key == ui::Key::g) {
                pending_code_bar_focus_ = workspace_.source_editor.open_search(editor::CodeSearchMode::goto_line);
                return {};
            }
            if (!event.control && !event.alt && event.key == ui::Key::f3) {
                workspace_.source_editor.find_step(!event.shift);
                return {};
            }
            if (in_code_buffer && !event.control && !event.alt && !event.shift && event.key == ui::Key::escape &&
                workspace_.source_editor.search_open()) {
                workspace_.source_editor.close_search();
                return {};
            }
        }
    }
    const auto previous_focus = ui_.focused_control();
    auto reply = ui_.input(event);
    if (ui_.presented() && ui_.focused_control() != previous_focus && ui_.focused_control() != 0)
        workspace_.dock.focus_control(ui_.focused_control(), *ui_.presented());
    if (!effects(reply))
        return {};
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        if (workspace_.color_edit && event.control && !event.shift && !event.alt && event.key == ui::Key::z) {
            status(finish_color_edit(false), L"Colour edit cancelled");
            return {};
        }
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
            return std::unexpected(Error{ErrorCode::invalid_storage, created.error().message});
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

void Application::poll_style() {
    if (style_watch_.path.empty())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= style_watch_.next_poll) {
        style_watch_.next_poll = now + std::chrono::milliseconds(100);
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(style_watch_.path, ec);
        std::uintmax_t size{};
        if (!ec)
            size = std::filesystem::file_size(style_watch_.path, ec);
        const int err = ec.value();
        if (!style_watch_.has_stamp || time != style_watch_.time || size != style_watch_.size ||
            err != style_watch_.error) {
            style_watch_.time = time;
            style_watch_.size = size;
            style_watch_.error = err;
            style_watch_.has_stamp = true;
            style_watch_.changed = true;
            style_watch_.changed_at = now;
        }
    }
    if (!style_watch_.changed || now - style_watch_.changed_at < std::chrono::milliseconds(200))
        return;
    style_watch_.changed = false;
    auto source = assets::read_file(style_watch_.path, 1024 * 1024);
    if (!source) {
        style_watch_.changed = true;
        style_watch_.changed_at = now;
        const auto message = diagnostic_text(source.error().message);
        workspace_.status = L"Style reload failed: " + message;
        if (!style_watch_.read_failed) {
            style_watch_.read_failed = true;
            notice("editor.style_reload_failed", workspace_.status, logging::Severity::warning);
        }
        return;
    }
    const bool had_failure = style_watch_.failed_source.has_value() || style_watch_.read_failed;
    style_watch_.read_failed = false;
    if (*source == style_watch_.accepted_source) {
        style_watch_.failed_source.reset();
        if (had_failure)
            workspace_.status = L"Reloaded editor.ss";
        return;
    }
    if (style_watch_.failed_source && *source == *style_watch_.failed_source)
        return;
    auto parsed = ui::StyleSheet::parse(as_chars(*source));
    if (!parsed) {
        style_watch_.failed_source = std::move(*source);
        const auto message = String::concat("editor.ss:", parsed.error().line, ":", parsed.error().column, ": ",
                                            parsed.error().message);
        workspace_.status = diagnostic_text(message);
        notice("editor.style_reload_failed", workspace_.status, logging::Severity::warning);
        return;
    }
    if (auto applied = ui_.set_style_sheet(*parsed); !applied) {
        style_watch_.failed_source = std::move(*source);
        workspace_.status = L"Style reload rejected: " + diagnostic_text(ui_error(applied.error()).message);
        notice("editor.style_reload_failed", workspace_.status, logging::Severity::warning);
        return;
    }
    style_sheet_ = std::move(*parsed);
    style_watch_.accepted_source = std::move(*source);
    style_watch_.failed_source.reset();
    menu_dirty_ = true;
    ui_.dismiss_tooltip();
    workspace_.status = L"Reloaded editor.ss";
}

Result<bool> Application::run_step(renderer::Capture* capture, StepTimes* times) {
    using Clock = std::chrono::steady_clock;
    frame_statistics_ = {};
    if (closed_)
        return false;
    // No dispatch or business callbacks run inside this wait. Input only wakes
    // the service pass; RHI retains any presentation permission it acquired.
    ATELIER_TRY_BIND(before_input, renderer_.target().transform_error(renderer_error));
    if (before_input->drawable) {
        timing::Scope admission{phases::admission};
        ATELIER_TRY(renderer_.wait_frame(20).transform_error(renderer_error));
    }
    // Everything this step changed reaches Play as one batch, whichever way the step ends.
    ScopeExit forward_live_guard{[this]() noexcept { forward_live(); }};
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
        poll_style();
        poll_project();
        diagnostics();
        if (workspace_.poll_output())
            menu_dirty_ = true;
    }
    if (control_) {
        timing::Scope control{phases::control};
        if (control_->poll())
            menu_dirty_ = true;
    }
    const auto title = (project_ ? project_->name() : L"No project") + L" — " +
                       (scene_path_.empty() ? L"Untitled" : scene_path_.filename().wstring()) +
                       (document_->dirty() ? L" *" : L"") + L" — FrostFox Atelier";
    if (title != window_title_) {
        SetWindowTextW(window(), title.c_str());
        window_title_ = title;
    }
    ATELIER_TRY_BIND(target, renderer_.target().transform_error(renderer_error));
    const auto extent = target->drawable ? target->extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) {
        auto error = ui_error(updated.error());
        error.message = String::concat("UI rejected the window surface. ", error.message);
        return std::unexpected(std::move(error));
    }
    effects(*updated);
    if (updated->error)
        return std::unexpected(ui_error(*updated->error));
    {
        timing::Scope collecting{phases::gc};
        ATELIER_TRY(document_->collect_step());
    }
    // Keep the last committed scopes while minimized; an empty surface cannot
    // validate a new layout or make its controls available for interaction.
    if (!target->drawable) {
        renderer_.release_transients(renderer::Dormancy::deep);
        camera_cut_ = true;
        play_idle_snapshot_.reset();
        lighting_clock_.reset();
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    if (saved_world_camera_ && (workspace_.active_graph.empty() || !document_->has_graph(workspace_.active_graph))) {
        camera_ = *saved_world_camera_;
        workspace_.camera_view = camera_.view();
        camera_cut_ = true;
        saved_world_camera_.reset();
    }
    if (play_.running() && GetForegroundWindow() != window()) {
        POINT cursor_pt{};
        const bool pointer_over = GetCursorPos(&cursor_pt) && WindowFromPoint(cursor_pt) == window();
        const bool busy_or_dirty =
            capture ||
            (control_ && (control_->active_job() || control_->wants_capture() || control_->command_trace())) ||
            menu_window() || pending_menu_ || workspace_.menu || workspace_.pie_menu() || camera_fly_ || camera_drag_ ||
            viewport_tool_.active() || camera_cut_ || menu_dirty_ || fallback_presented_ || source_build_pending_ ||
            pointer_over || !ui_.presented();
        const PlayIdleSnapshot cur_idle{current_menu_projection(), camera_.state(), surface_revision_};
        if (!busy_or_dirty && play_idle_snapshot_ && play_idle_snapshot_->matches(cur_idle)) {
            renderer_.release_transients(renderer::Dormancy::transients);
            lighting_clock_.reset();
            MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return true;
        }
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
        ATELIER_TRY_BIND(ready, renderer_.wait_frame(0).transform_error(renderer_error));
        host_ready = *ready;
    }
    if (!host_ready) {
        if (pending_menu_ || !workspace_.menu || workspace_.pie_menu())
            return true;
        if (menu_window() && popup_->source() == workspace_.menu->source && !menu_dirty_ &&
            menu_projection_ == current_menu_projection()) {
            // Native menus have their own surface and use the last accepted owner geometry
            // while authoritative document, view and menu state remain unchanged.
            timing::Scope menus{phases::popup};
            ATELIER_TRY(update_menu());
            return true;
        }
        // A newly requested or state-invalidated native menu needs its command model
        // projected and validated even when the owner cannot present.
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
    const auto describe = [&]() -> Result<void> {
        // Selection/document commands can cancel the business edit before the next UI frame.
        if (workspace_.color_edit && !document_->owns_edit(editor::control::color_group))
            workspace_.color_edit.reset();
        const auto project_controls = [&] {
            return workspace_.describe(workspace_controls_, *document_, view_size, ui_.presented().get(),
                                       project_.get(), &ui_, command_focus());
        };
        ATELIER_TRY(project_controls());
        if (workspace_.color_edit && (workspace_.color_edit->composition_failed ||
                                      std::ranges::none_of(workspace_controls_.controls(), [](const auto& control) {
                                          return control.key == editor::control::color_editor;
                                      }))) {
            const auto diagnostic = String::from_utf8_validated(workspace_.ui_error());
            ATELIER_TRY(finish_color_edit(false));
            ATELIER_TRY(project_controls());
            workspace_.status = L"Colour editor could not open" +
                                (diagnostic.empty() ? std::wstring{} : L": " + diagnostic_text(diagnostic));
        }
        return {};
    };
    Optional<timing::Scope> stage{std::in_place, phases::ui};
    ATELIER_TRY(describe());
    auto described = ui_.set_controls(workspace_controls_);
    if (!described && described.error().code != ui::ErrorCode::text_failed &&
        workspace_.reject_ui_controls(String::concat("UI candidate rejected at control ", described.error().key, ": ",
                                                     ui_error(described.error()).message))) {
        ATELIER_TRY(describe());
        described = ui_.set_controls(workspace_controls_);
    }
    if (!described) {
        auto error = ui_error(described.error());
        error.message = String::concat("UI rejected editor controls (key ", described.error().key, "). ", error.message);
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
        workspace_.reject_ui_controls(String::concat("Editor layout candidate rejected at control ",
                                                     prepared.error().key, ": ",
                                                     ui_error(prepared.error()).message))) {
        ATELIER_TRY(describe());
        described = ui_.set_controls(workspace_controls_);
        if (!described) {
            auto error = ui_error(described.error());
            error.message = String::concat("UI rejected restored editor controls (key ", described.error().key, "). ",
                                           error.message);
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
        error.message = ui_.presented()
                            ? String::concat("Frame preparation failed; previous frame retained: ", error.message)
                            : String::concat("Initial editor frame preparation failed (UI code ",
                                             static_cast<int>(prepared.error().code), "). ", error.message);
        return std::unexpected(std::move(error));
    }
    const auto previous_ui_revision = workspace_.ui_revision();
    ATELIER_TRY(workspace_.commit_ui_controls());
    if (workspace_.ui_revision() != previous_ui_revision) {
        ui_.dismiss_tooltip();
        if ((*prepared)->tooltip) {
            prepared = ui_.prepare();
            if (!prepared) {
                stage.reset();
                auto error = ui_error(prepared.error());
                error.message = String::concat("Frame preparation failed; previous frame retained: ", error.message);
                return std::unexpected(std::move(error));
            }
        }
    }
    prepared_menu_frame_ = *prepared;
    if (workspace_.menu) {
        menu_projection_ = current_menu_projection();
        menu_dirty_ = false;
    } else {
        menu_projection_.reset();
        menu_dirty_ = false;
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
    if (!in_sop && saved_world_camera_) {
        camera_ = *saved_world_camera_;
        workspace_.camera_view = camera_.view();
        camera_cut_ = true;
        saved_world_camera_.reset();
    }
    const procedural::Geometry* sop_template_geom = nullptr;
    renderer::Result<renderer::Preparation> material_preparation{renderer::Preparation{0, 0, true}};
    {
        timing::Scope snapshot{phases::scene};
        if (!viewport && play_.running())
            renderer_.release_transients(renderer::Dormancy::viewport_hidden);
        if (viewport) {
            if (in_sop) {
                const auto eval = document_->evaluate_graph(workspace_.active_graph, project_.get());
                if (!eval) {
                    ATELIER_TRY(procedural_preview_.refresh(*document_, project_.get(), nullptr, 0));
                    workspace_.render_notice = L"SOP cook error: " + diagnostic_text(eval.error().message);
                } else {
                    ATELIER_TRY(procedural_preview_.refresh(*document_, project_.get(), eval->preview_output,
                                                            eval->generation));
                    if (eval->template_output && eval->template_node != eval->display_node)
                        sop_template_geom = eval->template_output;
                    if (eval->display_error)
                        workspace_.render_notice = L"SOP cook error: " +
                                                   diagnostic_text(editor::format_error(*eval->display_error)) +
                                                   (eval->preview_output ? L" (showing last valid output)" : L"");
                }
                material_preparation = procedural_preview_.prepare(renderer_);
            } else {
                ATELIER_TRY(document_->sync_procedural_generators(project_.get()));
                const bool editing_network =
                    workspace_.inspecting_network ||
                    editor::region_of(workspace_.dock.focused_tab()) == editor::RegionKind::network;
                const bool network_visible = workspace_.dock.visible(editor::network_control::tab);
                ATELIER_TRY(scene_.refresh(*document_, !editing_network, !network_visible));
                material_preparation = scene_.prepare(renderer_);
            }
            if (!material_preparation) {
                if (material_preparation.error().terminal)
                    return renderer_stopped(material_preparation.error());
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
    const auto effective_view = control_ ? control_->view() : workspace_.view;
    const auto camera = camera_.matrices(std::max(scene_bounds.width, 1.f) / std::max(scene_bounds.height, 1.f));
    auto viewport_view = editor::ViewportView::make(camera_, scene_bounds, window_.dpi_scale());
    viewport_view.show_locators = effective_view.show_locators;
    if (sop_draw)
        viewport_view.scene = sop_draw->picking;
    else if (scene)
        viewport_view.scene = scene->picking;
    if (viewport_view.scene) {
        viewport_view.gpu_pick_pixel = [renderer = &renderer_](std::uint32_t px,
                                                               std::uint32_t py) -> Optional<editor::GpuPickSample> {
            auto picked = renderer->pick_pixel(px, py);
            if (!picked || !*picked)
                return {};
            return editor::GpuPickSample{(*picked)->instance_index, (*picked)->global_triangle_id, (*picked)->depth};
        };
        viewport_view.gpu_pick_box = [renderer = &renderer_](std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                                                             std::uint32_t y1) -> Array<std::uint32_t> {
            auto picked = renderer->pick_box(x0, y0, x1, y1);
            return picked ? std::move(*picked) : Array<std::uint32_t>{};
        };
    }
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
                                          workspace_.sop_show_bounds, true, 0.f, math::Matrix4::identity(),
                                          &workspace_.sop_group},
                                         overlay_);
            }
        } else {
            viewport_tool_.overlay(*document_, viewport_view, workspace_.viewport_settings, overlay_);
            if (const ObjectId gen_comp = workspace_.active_procedural_generator(*document_).value_or(ObjectId{});
                gen_comp != ObjectId{}) {
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
                        if (const auto eval = document_->evaluate_graph(src->source, project_.get());
                            eval && eval->template_output && eval->template_node != eval->display_node)
                            append_template_wireframe(*eval->template_output, viewport_view, overlay_, world_mtx);
                    }
                    append_procedural_guides(*geom, viewport_view,
                                             {workspace_.effective_show_points(geom), workspace_.sop_show_normals,
                                              workspace_.sop_show_face_normals, workspace_.sop_show_orient,
                                              workspace_.sop_show_bounds, true, chunk_size, world_mtx,
                                              &workspace_.sop_group},
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
    const auto frame_meshes = sop_draw ? Span<const renderer::MeshInstance>{sop_draw->meshes}
                              : scene  ? Span<const renderer::MeshInstance>{scene->meshes}
                                       : Span<const renderer::MeshInstance>{};
    const auto frame_lights =
        scene ? Span<const renderer::Light>{scene->lights} : Span<const renderer::Light>{};
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
    render_frame.trace_scope = control_ ? control_->trace_scope() : StringView{};
    Optional<renderer::Result<renderer::FrameResult>> traced_result;
    if (render_frame.command_trace)
        traced_result.emplace();
    const auto lighting_start = Clock::now();
    const bool lighting_active = scene_bounds.width > 0 && scene_bounds.height > 0 && !frame_meshes.empty() &&
                                 effective_view.shading == renderer::Shading::scene_lit &&
                                 effective_view.wireframe != renderer::Wireframe::only &&
                                 (effective_view.lit.indirect_lighting || effective_view.lit.reflections ||
                                  effective_view.lit.antialiasing == renderer::Antialiasing::taa ||
                                  effective_view.lit.fog_enabled);
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
            return renderer_stopped(drawn.error());
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
            return std::unexpected(Error{ErrorCode::invalid_storage, "Renderer accepted a mismatched frame."});
        if (times)
            times->accept_begin = Clock::now();
        if (!ui_.accept(*prepared))
            return std::unexpected(Error{ErrorCode::invalid_storage, "UI rejected an accepted renderer frame."});
        prepared_menu_frame_.reset();
        if (times)
            times->accepted = Clock::now();
        if (scene)
            scene_.accept(rendered.accepted_id, drawn->fallback, drawn->view.shading);
        render_retry_at_ = {};
        render_retry_delay_ = {};
        lighting_clock_.accept(lighting_start, lighting_active && drawn->view.shading == renderer::Shading::scene_lit);
        fallback_presented_ = drawn->fallback;
        accepted_view_ = viewport ? Optional{viewport_view} : Optional<editor::ViewportView>{};
        workspace_.lighting.observe(rendered.stats);
        if (viewport)
            camera_cut_ = false;
        ++accepted_frames_;
        if (!drawn->fallback && !material_pending)
            play_idle_snapshot_ = PlayIdleSnapshot{current_menu_projection(), camera_.state(), surface_revision_};
        else
            play_idle_snapshot_.reset();
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
        if (pending_code_bar_focus_ && (!workspace_.source_editor.search_open() ||
                                        (ui_.presented() && ui_.presented()->find(pending_code_bar_focus_)))) {
            const auto target_key = std::exchange(pending_code_bar_focus_, 0);
            if (workspace_.source_editor.search_open() && target_key)
                effects(ui_.focus(target_key));
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
            if (workspace_.row_rename) {
                std::size_t stem_w = workspace_.row_rename->text.size();
                if (!workspace_.row_rename->invalid && !workspace_.row_rename->asset.empty()) {
                    const auto* sel = workspace_.assets.selected() ? &*workspace_.assets.selected() : nullptr;
                    const bool is_folder = sel && sel->kind == assets::AssetKind::folder;
                    if (!is_folder) {
                        const auto dot = workspace_.row_rename->text.find_last_of(L'.');
                        if (dot != std::wstring::npos && dot > 0)
                            stem_w = dot;
                    }
                }
                const auto stem_u8 =
                    platform::utf16_to_utf8(std::wstring_view(workspace_.row_rename->text).substr(0, stem_w));
                const auto stem_len = static_cast<std::uint32_t>(stem_u8 ? stem_u8->size() : stem_w);
                effects(ui_.focus(editor::control::inline_rename, stem_len));
            }
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
        const DWORD wait_ms =
            (rendered.status == renderer::FrameStatus::skipped && !(control_ && control_->active_job())) ? 50u : 10u;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return true;
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
    if (document_) {
        if (auto root = platform::utf16_to_utf8(document_root_.generic_wstring())) {
            const StringView quit_source =
                window_.closing() ? Window::quit_source_name(window_.quit_source()) : StringView{"direct_close"};
            logging::Entry entry{.severity = logging::Severity::info,
                                 .category = "application"_s,
                                 .event_code = "application.closing"_s,
                                 .message = "Editor closing."_s};
            entry.fields = {{"document", document_->presentation().identity()},
                            {"project_root", std::move(*root)},
                            {"quit_source", String::from_utf8_validated(quit_source)},
                            {"play_running", play_.running()}};
            if (control_) {
                if (auto job = control_->active_job()) {
                    entry.fields.add({"active_control_job_id", std::move(job->id)});
                    entry.fields.add({"active_control_job_kind", String::from_utf8_validated(job->kind)});
                    entry.fields.add({"active_control_job_phase", String::from_utf8_validated(job->phase)});
                }
            }
            static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
        }
    }
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
