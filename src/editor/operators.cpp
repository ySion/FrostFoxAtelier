#include "editor/operators.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

namespace atelier::editor {
namespace {
Error invalid(std::string message) { return Error{ErrorCode::invalid_definition, std::move(message)}; }
// The caller sent an operator call this registry cannot accept.
Error rejected(std::string message) { return Error{ErrorCode::argument_mismatch, std::move(message)}; }
bool valid_id(std::string_view id) {
    const auto dot = id.find('.');
    if (dot == std::string_view::npos || id.find('.', dot + 1) != std::string_view::npos) return false;
    const auto part = [](std::string_view text) {
        return !text.empty() && std::islower(static_cast<unsigned char>(text.front())) &&
            std::ranges::all_of(text, [](char c) { return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'; });
    };
    return part(id.substr(0, dot)) && part(id.substr(dot + 1));
}
Result<void> admit(const Context& context, const ContextOverride& changes) {
    if (!changes.selection) return {};
    const auto& selection = *changes.selection;
    if (selection.entities.size() > Session::maximum_entities) return std::unexpected(rejected("The selection exceeds the Entity limit."));
    auto members = context.document.world().members();
    if (!members) return std::unexpected(members.error());
    std::unordered_set<ObjectId> entities, seen;
    for (const auto& member : *members) entities.insert(member.id());
    for (const auto entity : selection.entities) {
        if (!seen.insert(entity).second) return std::unexpected(rejected("The selection repeats an Entity."));
        if (!entities.contains(entity)) return std::unexpected(rejected("The selection names an object that is not an Entity of this document."));
    }
    if (selection.active != ObjectId{} && std::ranges::find(selection.entities, selection.active) == selection.entities.end())
        return std::unexpected(rejected("The active Entity must be selected."));
    if (selection.object != ObjectId{} && selection.object != selection.active) {
        auto component = world::inspect_component(context.document.world().runtime(), selection.object);
        if (!component || !component->valid || component->owner != selection.active)
            return std::unexpected(rejected("The inspected object must be the active Entity or one of its components."));
    }
    return {};
}
std::string history_violation(const OperatorInfo& info, std::uint64_t recorded, std::size_t undo, std::size_t redo,
    std::uint64_t recorded_after, std::size_t undo_after, std::size_t redo_after) {
    const auto entries = recorded_after - recorded;
    const bool unchanged = entries == 0 && undo_after == undo && redo_after == redo;
    bool valid = unchanged;
    std::string_view flag = "none";
    if (has_flag(info.flags, OperatorFlag::replaces_document)) return {};
    if (has_flag(info.flags, OperatorFlag::undo)) {
        flag = "undo";
        valid = unchanged || (entries == 1 && redo_after == 0);
    } else if (has_flag(info.flags, OperatorFlag::history)) {
        flag = "history";
        valid = entries == 0 && undo_after + redo_after == undo + redo && (undo_after > undo ? undo_after - undo : undo - undo_after) <= 1;
    }
    if (valid) return {};
    return "Operator " + info.id + " (" + std::string(flag) + ") broke its history contract: " + std::to_string(entries) +
        " entries recorded, undo " + std::to_string(undo) + "->" + std::to_string(undo_after) + ", redo " +
        std::to_string(redo) + "->" + std::to_string(redo_after) + ".";
}
}
std::span<const std::string_view> region_names() noexcept {
    static constexpr std::array<std::string_view, 10> names{"none", "viewport", "scene_tree", "structure", "parameters", "assets", "code", "output", "world", "controls"};
    return names;
}
Result<void> OperatorRegistry::add(OperatorInfo info) {
    if (!valid_id(info.id)) return std::unexpected(invalid("Operator ID must be lowercase <domain>.<action>: " + info.id));
    if (find(info.id)) return std::unexpected(invalid("Operator is already registered: " + info.id));
    if (!info.poll || !info.execute) return std::unexpected(invalid("Operator needs poll and execute: " + info.id));
    const auto exclusive = int(has_flag(info.flags, OperatorFlag::undo)) + int(has_flag(info.flags, OperatorFlag::history)) +
        int(has_flag(info.flags, OperatorFlag::replaces_document));
    if (exclusive > 1) return std::unexpected(invalid("Operator history flags are exclusive: " + info.id));
    operators_.push_back(std::move(info));
    return {};
}
const OperatorInfo* OperatorRegistry::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find(operators_, id, &OperatorInfo::id);
    return found == operators_.end() ? nullptr : &*found;
}
Result<OperatorReport> call(const OperatorRegistry& registry, OperatorHost& host, const Context& base, const OperatorCall& request) {
    const auto* info = registry.find(request.id);
    if (!info) return std::unexpected(rejected("Unknown operator: " + request.id));
    if (!request.args.is_object()) return std::unexpected(rejected("Operator arguments must be an object."));
    if (!request.args.empty()) return std::unexpected(rejected("Unknown argument for " + request.id + ": " + request.args.begin().key()));
    if (auto admitted = admit(base, request.context); !admitted) return std::unexpected(admitted.error());
    Context context = base;
    if (request.context.selection) context.selection = *request.context.selection;
    if (request.context.region) context.region = *request.context.region;
    if (auto available = info->poll(host, context); !available) return OperatorReport{OperatorStatus::unavailable, available.error().message};
    if (auto ended = host.end_interaction(); !ended) return std::unexpected(ended.error());
    // A replacing operator only files a host request, so the Session stays valid here.
    auto& document = context.document;
    const auto recorded = document.recorded(), undo = document.undo_count(), redo = document.redo_count();
    if (auto executed = info->execute(host, context); !executed) return std::unexpected(executed.error());
    if (auto violation = history_violation(*info, recorded, undo, redo, document.recorded(), document.undo_count(), document.redo_count());
        !violation.empty())
        return std::unexpected(invalid(std::move(violation)));
    return OperatorReport{OperatorStatus::finished, {}};
}
}
