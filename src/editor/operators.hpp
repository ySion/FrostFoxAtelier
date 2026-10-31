#pragma once
#include "editor/camera.hpp"
#include "assets/json_format.hpp"
#include "editor/operator_parameters.hpp"
#include "editor/session.hpp"
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Every editor action is an operator: menus, buttons, shortcuts and Control call
// the same entry. See docs/modules/editor/operators.md.
namespace atelier::editor {
class Project;
enum class OperatorFlag : std::uint32_t {
    none = 0,
    undo = 1, // At most one scene history entry per call.
    history = 2, // Moves the scene history cursor only (edit.undo / edit.redo).
    replaces_document = 4, // Asks the host to replace the document after the call returns.
    prompts = 8, // Transitional: interactive calls may ask for a missing path or name.
};
constexpr OperatorFlag operator|(OperatorFlag a, OperatorFlag b) noexcept {
    return static_cast<OperatorFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has_flag(OperatorFlag set, OperatorFlag flag) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}
enum class RegionKind : std::uint32_t {
    none,
    viewport,
    scene_tree,
    structure,
    parameters,
    assets,
    code,
    output,
    world,
    controls
};
std::span<const std::string_view> region_names() noexcept; // Indexed by RegionKind.

struct SelectionContext {
    ObjectId active; // Active Entity.
    ObjectId object; // Inspected Entity or one of the active Entity's components.
    std::vector<ObjectId> entities; // Selected Entities; active is the last when present.
};
struct HistoryState {
    bool undo{}, redo{};
};
struct Context {
    Session& document;
    Project* project{};
    SelectionContext selection;
    RegionKind region{};
    std::string asset; // Material asset key while Parameters inspects that asset.
    HistoryState source_history; // The Code page's active document; both false without one.
    HistoryState workbench_history; // The Controls page samples.
    bool scene_available{true}; // False while a project has no loadable scene.
    bool interactive{};
    ToolSettings tools;
    math::Vector3 cursor{}; // The viewport's 3D cursor: where new Entities appear.
};
// Control replaces fields for one call; the document selection does not change.
struct ContextOverride {
    std::optional<SelectionContext> selection;
    std::optional<RegionKind> region;
};

// Host capabilities an operator may use; the application implements them.
class OperatorHost {
public:
    virtual ~OperatorHost() = default;
    // Ends gestures and captures before an operator runs.
    virtual Result<void> end_interaction() = 0;
    virtual const Camera& camera() const noexcept = 0;
    virtual Result<void> set_camera(const Camera&) = 0;
    virtual void reset_layout() = 0;
    // A region without a Dock page is a no-op; the Code page closes its active document first,
    // asking about unsaved changes only when interactive.
    virtual Result<void> close_tab(RegionKind, bool interactive) = 0;
    virtual void maximize_tab(RegionKind) = 0;
    virtual Result<void> source_history(bool redo) = 0;
    virtual Result<void> workbench_history(bool redo) = 0;
    // The scene document changed outside an edit gesture; views follow it.
    virtual void scene_changed() = 0;
    virtual void set_tool_settings(const ToolSettings&) = 0;
};

struct OperatorInfo {
    std::string id; // "<domain>.<action>", lowercase.
    std::wstring title;
    std::string description;
    OperatorFlag flags{};
    std::function<Result<void>(const Context&)> poll; // The error message is the reason.
    // The decoded parameter record, or an empty input when `parameters` is empty.
    std::function<Result<void>(OperatorHost&, const Context&, ValueInput parameters)> execute;
    // Runtime name of the parameter record, published in the document; empty for none.
    std::string parameters{};
    // Patch-style operators start from the current state, so arguments name only the leaves they change.
    std::function<Result<void>(const Context&, ValueOutput parameters)> seed{};
};
class OperatorRegistry {
public:
    Result<void> add(OperatorInfo); // Rejects duplicate or malformed IDs and illegal flag combinations.
    const OperatorInfo* find(std::string_view id) const noexcept;
    std::span<const OperatorInfo> all() const noexcept { return operators_; }

private:
    std::vector<OperatorInfo> operators_;
};
void register_builtin_operators(OperatorRegistry&);

struct OperatorCall {
    std::string id;
    assets::json_format::Json args = assets::json_format::Json::object();
    ContextOverride context;
};
enum class OperatorStatus { finished, unavailable };
struct OperatorReport {
    OperatorStatus status{};
    std::string message; // Poll reason when unavailable.
};
// Poll, then seed or default-construct the parameters and decode the arguments strictly
// into them, then execute, then check the history contract of the operator's flags.
// Unknown operators, invalid arguments or context, execution errors and history
// contract violations are errors in every build configuration.
Result<OperatorReport> call(const OperatorRegistry&, OperatorHost&, const Context&, const OperatorCall&);
// JSON schema of an operator's arguments, from its parameter record in this Runtime:
// field names, kinds, enum members, hard bounds as minimum/maximum, the UI range, and
// defaults; a seeded operator has no defaults and reports "seeded": true instead.
Result<assets::json_format::Json> parameter_schema(Runtime&, const OperatorInfo&);
}
