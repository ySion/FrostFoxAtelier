#pragma once
#include "foundation/array.hpp"
#include "foundation/flags.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "editor/camera.hpp"
#include "json/json.hpp"
#include "editor/operator_parameters.hpp"
#include "editor/properties.hpp"
#include "editor/session.hpp"
#include "foundation/string.hpp"
#include "renderer/view_settings.hpp"
#include <functional>
#include <memory>
#include <string>
#include <utility>

// Every editor action is an operator: menus, buttons, shortcuts and Control call
// the same entry. See docs/modules/editor/operators.md.
namespace atelier::editor {
class Project;

class ViewportTool;

enum class OperatorFlag : std::uint32_t {
    none = 0,
    undo = 1, // At most one scene history entry per call.
    history = 2, // Moves the scene history cursor only (edit.undo / edit.redo).
    replaces_document = 4, // Asks the host to replace the document after the call returns.
};

}

template <>
inline constexpr bool atelier::enable_enum_flags<atelier::editor::OperatorFlag> = true;

namespace atelier::editor {

Span<const StringView> region_names() noexcept; // Indexed by RegionKind.

struct SelectionContext {
    ObjectId active; // Active Entity.

    ObjectId object; // Inspected Entity or one of the active Entity's components.

    Array<ObjectId> entities; // Selected Entities; active is the last when present.

    bool operator==(const SelectionContext&) const = default;
};

struct HistoryState {
    bool undo{};

    bool redo{};
};

struct Context {
    Session& document;

    Project* project{};

    SelectionContext selection;

    RegionKind region{};

    String asset; // Material or procedural graph asset key while Parameters/SOP context inspects it.

    String active_graph; // Non-empty when Scene View is isolated inside a SOP .procgraph context.

    String network_graph; // Graph currently shown in Network pane (Isolated SOP, Pinned, or World Entity follow).

    String selected_graph_node; // Selected node name in active_graph / network_graph; empty selects Graph root.

    Array<String> selected_graph_nodes; // Multi-node selection in active_graph Network View.

    Array<String> locked_graphs; // Open .procgraph keys currently in Locked Preview.

    HistoryState source_history; // The Code page's active document; both false without one.

    HistoryState workbench_history; // The Controls page samples.

    bool scene_available{true}; // False while a project has no loadable scene.

    bool interactive{}; // Human UI/keymap source, not the kind of this invocation; Control cannot override it.

    ToolSettings tools;

    math::Vector3 cursor{}; // The viewport's 3D cursor: where new Entities appear.

    renderer::ViewSettings view{};

    std::filesystem::path source{};

    Result<std::filesystem::path> editor_ui_source{};

    bool project_running{};

    bool project_stopping{};

    bool play_linked{};

    bool build_ready{};

    bool show_fps{};

    bool viewport_options{};

    CameraView camera_view{CameraView::perspective};

    [[nodiscard]] String focused_graph() const;

    [[nodiscard]] bool is_graph_locked(StringView key) const noexcept;
};

// Control replaces fields for one call; the document selection does not change.
struct ContextOverride {
    Optional<SelectionContext> selection;

    Optional<RegionKind> region;

    Optional<String> asset{};

    bool operator==(const ContextOverride&) const = default;
};

// Native file picker purposes, not editor operations. Explicit paths never open a picker.
enum class PathPurpose {
    folder,
    scene_open,
    scene_save,
    mesh,
    source,
    source_save,
    texture,
    material_save,
    material_source_save,
    procedural_graph_save
};

// Owns every value needed after the invoking Session and its parameter storage retire.
struct DocumentRequest {
    enum class Kind { new_project, open_project, open_scene, build } kind;

    std::filesystem::path path;

    bool interactive{};

    bool rebuild{};
};

// Host capabilities an operator may use; the application implements them.
class OperatorHost {
public:
    virtual ~OperatorHost() = default;

    void begin_operator() noexcept {
        cancelled_ = false;
        replacement_.reset();
    }

    void cancel_operator() noexcept { cancelled_ = true; }

    bool operator_cancelled() const noexcept { return cancelled_; }

    void replace_document(DocumentRequest request) { replacement_ = std::move(request); }

    Optional<DocumentRequest> take_document_request() { return replacement_.take(); }

    virtual Result<Optional<std::filesystem::path>> choose_path(PathPurpose, const std::filesystem::path& initial,
                                                                std::wstring_view filename = {}) = 0;

    // A successfully published resource becomes the browser's active resource.
    virtual void asset_created(StringView key, bool inspect_material) = 0;

    // Ends gestures and captures before an operator runs.
    virtual Result<void> end_interaction() = 0;

    virtual const Camera& camera() const noexcept = 0;

    virtual Result<void> set_camera(const Camera&) = 0;

    virtual ViewportTool& viewport_tool() noexcept = 0;

    // Starts the existing placement gesture for the copies just selected by Session.
    virtual Result<void> place_duplicates() = 0;

    // Starts the existing tree-row or Parameters name editor for this explicit target.
    virtual Result<void> begin_entity_rename(ObjectId, RegionKind) = 0;

    virtual Result<void> reset_layout() = 0;

    // A region without a Dock page is a no-op; the Code page closes its active document first,
    // asking about unsaved changes only when interactive.
    virtual Result<void> close_tab(RegionKind, bool interactive) = 0;

    virtual void maximize_tab(RegionKind) = 0;

    virtual void show_tab(RegionKind) = 0;

    virtual void cycle_tab(RegionKind, int direction) = 0;

    virtual Result<void> source_history(bool redo) = 0;

    virtual Result<void> workbench_history(bool redo) = 0;

    // The scene document changed outside an edit gesture; views follow it.
    virtual void scene_changed() = 0;

    virtual void set_tool_settings(const ToolSettings&) = 0;

    virtual Result<void> set_view_settings(const renderer::ViewSettings&) = 0;

    virtual void quit() = 0;

    virtual void about() = 0;

    virtual void select_box() = 0;

    virtual void show_menu(MenuKind) = 0;

    virtual void toggle_view(ViewToggleOption) = 0;

    virtual Result<void> fit_lighting(bool follow_focus) = 0;

    virtual Result<void> rename_asset(StringView key, StringView name) = 0;

    virtual Result<void> save_scene(const std::filesystem::path&) = 0;

    virtual Result<void> save_documents(const Context&, bool all) = 0;

    virtual Result<void> open_source(const std::filesystem::path&) = 0;

    virtual Result<void> create_source(const std::filesystem::path&, bool project_ui) = 0;

    virtual Result<void> close_source(bool interactive) = 0;

    virtual Result<void> reload_source(bool interactive) = 0;

    virtual Result<void> start_play(bool preview_ui, bool interactive) = 0;

    virtual void stop_play(bool restart) = 0;

    virtual Result<String> import_asset(ImportKind, const std::filesystem::path&) = 0;

    virtual Result<void> reimport_asset(StringView key, const std::filesystem::path&) = 0;

    virtual Result<void> open_graph_context(StringView key) = 0;

    virtual void exit_graph_context() = 0;

    virtual Result<void> dive_into_asset_node(StringView, StringView) {
        return std::unexpected(Error{ErrorCode::unsupported_operation, "Dive-in requires an editor workspace."});
    }

    virtual void select_graph_node(StringView key, StringView node_name) = 0;

    virtual void select_graph_nodes(StringView key, Span<const String> node_names) {
        select_graph_node(key, node_names.empty() ? StringView{} : node_names.back().view());
    }

private:
    bool cancelled_{};

    Optional<DocumentRequest> replacement_;
};

struct OperatorParameters {
    ValueInput value;

    Span<const PropertyPatch> patch; // @context.object; owned by call through execute.
};

// Queries use state/schema/inspection, not the gesture-retiring execute path.
enum class OperatorKind { query, direct, interactive };

struct OperatorInfo {
    String id; // "<domain>.<action>", lowercase.

    std::wstring title;

    String description;

    OperatorFlag flags{};

    std::function<Result<void>(const Context&)> poll; // The error message is the reason.

    // The decoded parameter record or sparse patch; both empty for parameterless operators.
    std::function<Result<void>(OperatorHost&, const Context&, const OperatorParameters&, ValueOutput)> execute;

    // Runtime name of the parameter record, published in the document; empty for none.
    String parameters{};

    // Patch-style operators start from the current state, so arguments name only the leaves they change.
    std::function<Result<void>(const Context&, ValueOutput parameters)> seed{};

    String result{}; // Optional result record name; encoded before its Runtime can be replaced.

    std::function<Optional<bool>(const Context&, ValueInput)> state{};

    OperatorKind kind{OperatorKind::direct};

    // A direct operator can require human interaction for particular decoded arguments.
    std::function<bool(ValueInput)> requires_interaction{};

    String interaction{}; // Discovery and refusal guidance for that condition.

    OperatorInfo() = default;

    OperatorInfo(Utf8Text operator_id, std::wstring operator_title, Utf8Text operator_description,
                 OperatorFlag operator_flags, std::function<Result<void>(const Context&)> operator_poll,
                 std::function<Result<void>(OperatorHost&, const Context&, const OperatorParameters&, ValueOutput)>
                     operator_execute,
                 Utf8Text operator_parameters = {},
                 std::function<Result<void>(const Context&, ValueOutput parameters)> operator_seed = {},
                 Utf8Text operator_result = {}, std::function<Optional<bool>(const Context&, ValueInput)> operator_state = {},
                 OperatorKind operator_kind = OperatorKind::direct,
                 std::function<bool(ValueInput)> operator_requires_interaction = {}, Utf8Text operator_interaction = {})
        : id(std::move(operator_id).into_string()),
          title(std::move(operator_title)),
          description(std::move(operator_description).into_string()),
          flags(operator_flags),
          poll(std::move(operator_poll)),
          execute(std::move(operator_execute)),
          parameters(std::move(operator_parameters).into_string()),
          seed(std::move(operator_seed)),
          result(std::move(operator_result).into_string()),
          state(std::move(operator_state)),
          kind(operator_kind),
          requires_interaction(std::move(operator_requires_interaction)),
          interaction(std::move(operator_interaction).into_string()) {}
};

class OperatorRegistry {
public:
    [[nodiscard]] static Result<OperatorRegistry> builtins();

    const OperatorInfo* find(StringView id) const noexcept;

    Span<const OperatorInfo> all() const noexcept { return operators_; }

private:
    Result<void> validate() const;

    Array<OperatorInfo> operators_;
};

struct OperatorCall {
    String id;

    json::Document args{};

    ContextOverride context{};

    OperatorCall() = default;

    OperatorCall(Utf8Text operator_id, json::Document operator_args = {}, ContextOverride operator_context = {})
        : id(std::move(operator_id).into_string()),
          args(std::move(operator_args)),
          context(std::move(operator_context)) {}

    bool operator==(const OperatorCall&) const = default;
};

namespace detail {
class OperatorValue;
}

// One immutable UI call. Its first state query prepares fixed, reference-free
// parameters; seeded/reference-bearing records are decoded on every query.
// Queries use the same registry and document for this binding's lifetime. Retire
// the binding before closing that document; neither state results nor registry
// element addresses are retained. Execution still goes through call().
class OperatorBinding {
public:
    explicit OperatorBinding(OperatorCall);

    ~OperatorBinding();

    OperatorBinding(const OperatorBinding&) = delete;

    OperatorBinding& operator=(const OperatorBinding&) = delete;

    const OperatorCall request;

    Result<Optional<bool>> state(const OperatorRegistry&, const Context&);

private:
    std::unique_ptr<detail::OperatorValue> parameters_;
};

enum class OperatorStatus { finished, cancelled, failed, interaction_required };

struct OperatorReport {
    OperatorStatus status{};

    String message; // Poll or admission reason; execution failures retain their original Error.

    json::Document result{};
};

// Poll, then seed or default-construct the parameters and decode the arguments strictly
// into them. Reject human-only calls before ending interactions or executing; otherwise
// execute and check the history contract of the operator's flags.
// Unknown operators, invalid arguments or context, execution errors and history
// contract violations are errors in every build configuration.
// Reference tokens belong to the caller (Control uses its document-scoped handles).
// Null references need no resolver. The resolver is used only during argument decoding.
Result<OperatorReport> call(const OperatorRegistry&, OperatorHost&, const Context&, const OperatorCall&,
                            const ResolveReference& = {});

// JSON schema of an operator's arguments, from its parameter record in this Runtime:
// field names, kinds, enum members, hard bounds as minimum/maximum, the UI range, and
// defaults; a seeded operator has no defaults and reports "seeded": true instead.
Result<json::Document> parameter_schema(Runtime&, const OperatorInfo&);

Result<json::Document> result_schema(Runtime&, const OperatorInfo&);
}
