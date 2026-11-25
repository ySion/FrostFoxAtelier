#include "debugger.hpp"
#include "source/detail/bridge.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace atelier::source::detail {
namespace {
template <class T>
T read_value(const void* data) noexcept {
    T result;
    std::memcpy(&result, data, sizeof(result));
    return result;
}

// Only validated scalar storage, direct field offsets and trusted sequence leaf
// projections are read. No formatting, getters or value copy operations run.
class SnapshotBuilder {
    Invocation& invocation_;

    const DebugMetadata& metadata_;

    const DebugOptions& options_;

    DebugSnapshot result_;

    std::size_t values_{};

    std::size_t elements_{};

    std::size_t text_bytes_{};

    std::string text(std::string_view source, bool* truncated = nullptr) {
        auto count = std::min(source.size(), options_.max_text_bytes - text_bytes_);
        if (count < source.size()) {
            // Do not split a UTF-8 sequence in either source names or values.
            while (count && (static_cast<unsigned char>(source[count]) & 0xc0) == 0x80)
                --count;
            result_.truncated = true;
            if (truncated)
                *truncated = true;
        }
        text_bytes_ += count;
        return std::string{source.substr(0, count)};
    }

    SourceLocation source_location(const SourceLocation& source) {
        return {text(source.path), source.version, source.offset, source.line, source.column};
    }

    DiagnosticLocation location(std::uint32_t index) {
        if (index >= metadata_.locations->size())
            return {};
        const auto& original = (*metadata_.locations)[index];
        return {text(original.function), source_location(original.source)};
    }

    const BoundValue* aggregate_binding(BusinessType type) const noexcept {
        if (!aggregate(type) || type.type_index >= invocation_.binding.values.size())
            return nullptr;
        const auto& value = invocation_.binding.values[type.type_index];
        return value.kind == type.kind ? &value : nullptr;
    }

    std::size_t storage_size(BusinessType type) const noexcept {
        switch (type.kind) {
        case ValueKind::boolean:
            return 1;
        case ValueKind::f32:
        case ValueKind::u32:
        case ValueKind::i32:
        case ValueKind::enumeration:
            return 4;
        case ValueKind::f64:
            return 8;
        default:
            if (const auto* value = aggregate_binding(type))
                return value->size;
        }
        return 0;
    }

    std::size_t storage_alignment(BusinessType type) const noexcept {
        if (const auto* value = aggregate_binding(type))
            return value->alignment;
        return storage_size(type);
    }

    void identify(DebugValue& value, BusinessType type) {
        value.type = value_type_id(invocation_.binding, type);
        if (value.type == TypeId{})
            return;
        if (const auto info = invocation_.runtime.describe(value.type); info)
            value.type_name = text((*info)->name, &value.truncated);
    }

    bool append_child(DebugValue& parent, std::string_view name, BusinessType type, const void* data,
                      std::size_t depth) {
        if (elements_ == options_.max_elements || values_ == options_.max_values)
            return false;
        ++elements_;
        ++values_;
        auto& child = parent.children.emplace_back();
        child.name = text(name, &child.truncated);
        identify(child, type);
        preview(child, type, data, depth);
        return true;
    }

    void incomplete(DebugValue& value) noexcept {
        if (value.children.size() < value.total_children) {
            value.truncated = true;
            result_.truncated = true;
        }
    }

    void unavailable(DebugValue& value, std::string_view reason) {
        value.state = DebugValueState::unavailable;
        value.text = text(reason, &value.truncated);
    }

    void preview(DebugValue& value, BusinessType type, const void* data, std::size_t depth) {
        if (!data || value.type == TypeId{}) {
            unavailable(value, "Storage is unavailable.");
            return;
        }
        value.state = DebugValueState::available;
        switch (type.kind) {
        case ValueKind::boolean: {
            const auto bits = read_value<std::uint8_t>(data);
            if (bits > 1)
                unavailable(value, "Invalid bool storage.");
            else
                value.scalar = bits != 0;
            return;
        }
        case ValueKind::f32:
            value.scalar = read_value<float>(data);
            return;
        case ValueKind::f64:
            value.scalar = read_value<double>(data);
            return;
        case ValueKind::u32:
            value.scalar = read_value<std::uint32_t>(data);
            return;
        case ValueKind::i32:
            value.scalar = read_value<std::int32_t>(data);
            return;
        case ValueKind::enumeration:
            value.scalar = EnumValue{value.type, read_value<std::uint32_t>(data)};
            return;
        default:
            break;
        }
        const auto* binding = aggregate_binding(type);
        if (!binding) {
            unavailable(value, "Type has no supported storage projection.");
            return;
        }
        if (type.kind == ValueKind::string || type.kind == ValueKind::array) {
            if (!binding->sequence_view) {
                unavailable(value, "Sequence has no read-only projection.");
                return;
            }
            const void* elements = nullptr;
            std::size_t count = 0;
            binding->sequence_view(data, &elements, &count);
            value.total_children = count;
            if (count && !elements) {
                unavailable(value, "Sequence storage is unavailable.");
                return;
            }
            if (type.kind == ValueKind::string) {
                if (count)
                    value.text = text({static_cast<const char*>(elements), count}, &value.truncated);
                return;
            }
            const auto size = storage_size(binding->element);
            const auto alignment = storage_alignment(binding->element);
            if (!size || !alignment || count > std::numeric_limits<std::size_t>::max() / size ||
                (count && reinterpret_cast<std::uintptr_t>(elements) % alignment != 0)) {
                unavailable(value, "Sequence element storage is invalid.");
                return;
            }
            if (depth < options_.max_depth) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (!append_child(value, "[" + std::to_string(i) + "]", binding->element,
                                      static_cast<const std::byte*>(elements) + i * size, depth + 1))
                        break;
                }
            }
            incomplete(value);
            return;
        }
        if (binding->math) {
            const auto scalar = binding->math.precision == math::Precision::f64 ? ValueKind::f64 : ValueKind::f32;
            value.total_children = binding->math.components();
            if (depth < options_.max_depth) {
                constexpr std::string_view component_names[]{"x", "y", "z", "w"};
                for (std::size_t i = 0; i < value.total_children; ++i) {
                    const auto name = binding->math.shape == math::Shape::matrix4 ? "[" + std::to_string(i) + "]"
                                                                                  : std::string{component_names[i]};
                    if (!append_child(value, name, scalar,
                                      static_cast<const std::byte*>(data) + i * binding->math.scalar_size(), depth + 1))
                        break;
                }
            }
            incomplete(value);
            return;
        }
        if (binding->fields.empty() && !binding->plain_value) {
            unavailable(value, "Native value is opaque.");
            return;
        }
        value.total_children = binding->fields.size();
        if (depth < options_.max_depth) {
            for (const auto& field : binding->fields) {
                const auto size = storage_size(field.type);
                const bool valid = size && field.offset <= binding->size && size <= binding->size - field.offset;
                if (!append_child(value, field.name, field.type,
                                  valid ? static_cast<const std::byte*>(data) + field.offset : nullptr, depth + 1))
                    break;
            }
        }
        incomplete(value);
    }

    void reference(DebugValue& value, DebugStorage storage, std::uint32_t slot) {
        value.weak = storage == DebugStorage::weak_reference;
        ObjectReference reference;
        if (slot != null_reference_slot) {
            if (value.weak) {
                const auto index = slot & ~observation_slot_bit;
                if ((slot & observation_slot_bit) == 0 || index >= invocation_.observations.size()) {
                    unavailable(value, "Reference slot is unavailable.");
                    return;
                }
                reference = invocation_.observations[index];
            } else {
                const auto held = invocation_.frame.reference(slot);
                if (!held) {
                    unavailable(value, "Reference slot is unavailable.");
                    return;
                }
                reference = *held;
            }
        }
        value.state = DebugValueState::available;
        value.object = reference.id();
        value.type = reference.description().type;
        if (value.type == TypeId{})
            value.type = invocation_.binding.type;
        if (const auto info = invocation_.runtime.describe(value.type); info)
            value.type_name = text((*info)->name, &value.truncated);
        value.object_live = value.object != ObjectId{} && invocation_.runtime.type_of(value.object).has_value();
    }

    void local(DebugFrame& frame, const DebugVariableMetadata& variable, const BusinessDebugLocal& storage) {
        ++values_;
        auto& value = frame.locals.emplace_back();
        value.name = text(variable.name, &value.truncated);
        value.passing = variable.passing;
        if (variable.storage == DebugStorage::value)
            identify(value, variable.type);
        else {
            value.type = invocation_.binding.type;
            value.type_name = text(invocation_.binding.name, &value.truncated);
            value.weak = variable.storage == DebugStorage::weak_reference;
        }
        if (storage.availability == DebugAvailability::out_of_scope) {
            value.state = DebugValueState::out_of_scope;
            return;
        }
        if (storage.availability == DebugAvailability::uninitialized) {
            value.state = DebugValueState::uninitialized;
            return;
        }
        if (storage.availability != DebugAvailability::available) {
            unavailable(value, "Variable availability is invalid.");
            return;
        }
        if (variable.storage == DebugStorage::value)
            preview(value, variable.type, storage.data, 0);
        else
            reference(value, variable.storage, storage.slot);
    }

public:
    SnapshotBuilder(Invocation& invocation, const DebugSessionState& state, DebugStopReason reason, std::uint64_t stop)
        : invocation_(invocation), metadata_(*state.metadata), options_(state.options) {
        result_.program_id = metadata_.program_id;
        result_.code_version = metadata_.code_version;
        result_.stop_id = stop;
        result_.reason = reason;
    }

    DebugSnapshot build() {
        for (auto* frame = invocation_.debug_top; frame; frame = frame->previous) {
            if (result_.frames.size() == options_.max_frames) {
                result_.truncated = true;
                break;
            }
            auto& output = result_.frames.emplace_back();
            output.activation_id = frame->activation;
            output.location = location(frame->location);
            const auto& variables = metadata_.tables->functions[frame->function].variables;
            for (std::size_t i = 0; i < variables.size(); ++i) {
                if (values_ == options_.max_values || !frame->locals) {
                    result_.truncated = true;
                    break;
                }
                local(output, variables[i], frame->locals[i]);
            }
        }
        if (invocation_.failed) {
            std::optional<ErrorElement> element;
            if (invocation_.error_storage.element != no_failure_element)
                element = ErrorElement{text(invocation_.error_storage.layer), invocation_.error_storage.element};
            result_.error =
                Error{invocation_.error_code(), text(invocation_.error_message()), std::nullopt, std::move(element)};
            if (invocation_.debug_top && invocation_.debug_top->location < metadata_.locations->size())
                result_.error->source = source_location((*metadata_.locations)[invocation_.debug_top->location].source);
        }
        return std::move(result_);
    }
};

bool contains_activation(const BusinessDebugFrame* frame, std::uint64_t activation) noexcept {
    for (; frame; frame = frame->previous)
        if (frame->activation == activation)
            return true;
    return false;
}

bool stepping(const DebugSessionState& state, const BusinessDebugFrame& frame, DebugPoint point) noexcept {
    const bool bindable = point == DebugPoint::entry || point == DebugPoint::statement;
    switch (state.action) {
    case DebugAction::step_into:
        return bindable;
    case DebugAction::step_over:
        return (bindable && frame.activation == state.step_activation) ||
               !contains_activation(&frame, state.step_activation);
    case DebugAction::step_out:
        return !contains_activation(&frame, state.step_activation);
    default:
        return false;
    }
}
}

std::uint32_t debug_enter(void* context, BusinessDebugFrame* frame) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    const auto& state = invocation.debugger;
    if (!state || !state->active)
        return 0;
    if (!frame || frame->function >= state->metadata->tables->functions.size()) {
        invocation.fail(ErrorCode::invalid_definition, "Generated debug frame has invalid metadata.");
        return 1;
    }
    frame->previous = invocation.debug_top;
    frame->activation = ++invocation.next_debug_activation;
    invocation.debug_top = frame;
    return 0;
}

std::uint32_t debug_point(void* context, BusinessDebugFrame* frame, std::uint32_t location, DebugPoint point,
                          const BusinessFailure* failure) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    // Hold the session through the handler: callers may destroy or replace the
    // public Debugger while this same native stack remains stopped.
    const auto state = invocation.debugger;
    if (!state || !state->active)
        return 0;
    const auto failure_reason = failure ? failure->reason : EntryFailure::none;
    if (point == DebugPoint::error && invocation.debug_aborted && !invocation.failed &&
        failure_reason == EntryFailure::none)
        return 1;
    if (point == DebugPoint::error) {
        if (!invocation.failed) {
            const auto& views = state->metadata->tables->failure_message_views;
            const auto message =
                failure_message(failure_reason, views.data(), static_cast<std::uint32_t>(views.size()));
            const auto element = failure ? failure->element : no_failure_element;
            const auto layer = failure && element != no_failure_element
                                   ? resolve_failure_layer_view(invocation.binding, failure->layer)
                                   : std::string_view{};
            invocation.fail_view(ErrorCode::invocation_failed,
                                 message ? *message : std::string_view("Source execution failed."), element, layer);
        }
        if (state->error_stopped)
            return 1;
        state->error_stopped = true;
    } else if (invocation.failed || invocation.debug_aborted)
        return 1;
    if (!frame || frame != invocation.debug_top || location >= state->metadata->locations->size()) {
        invocation.fail(ErrorCode::invalid_definition, "Generated debug point has invalid frame or source metadata.");
        return 1;
    }
    frame->location = location;
    if (invocation.trace) {
        invocation.trace->current_location = location;
        if (point == DebugPoint::error && invocation.trace->failure_location == no_diagnostic_location)
            invocation.trace->failure_location = location;
    }
    const bool initial_entry = point == DebugPoint::entry && !state->entry_stopped;
    if (point == DebugPoint::entry)
        state->entry_stopped = true;
    const bool bindable = point == DebugPoint::entry || point == DebugPoint::statement;
    std::optional<DebugStopReason> reason;
    if (point == DebugPoint::error) {
        if (state->options.break_on_error)
            reason = DebugStopReason::error;
    } else if (bindable && std::binary_search(state->bound_sites.begin(), state->bound_sites.end(), location)) {
        reason = DebugStopReason::breakpoint;
    } else if (initial_entry && state->options.break_on_entry) {
        reason = DebugStopReason::entry;
    } else if (stepping(*state, *frame, point)) {
        reason = DebugStopReason::step;
    }
    if (!reason || !state->handler || state->handler_failed)
        return point == DebugPoint::error ? 1 : 0;
    auto inspection = ExecutionInspection::enter(invocation.runtime);
    if (!inspection) {
        state->handler_failed = true;
        invocation.fail(inspection.error());
        return 1;
    }
    state->last_stop = SnapshotBuilder{invocation, *state, *reason, ++state->next_stop_id}.build();
    state->status = DebugStatus::paused;
    const auto command = state->handler(*state->last_stop);
    state->status = DebugStatus::running;
    if (!command) {
        state->handler_failed = true;
        invocation.fail(command.error());
        return 1;
    }
    if (command->stop_id != state->last_stop->stop_id) {
        state->handler_failed = true;
        invocation.fail(ErrorCode::argument_mismatch, "Debug command does not belong to the current stop.");
        return 1;
    }
    switch (command->action) {
    case DebugAction::continue_execution:
    case DebugAction::step_into:
    case DebugAction::step_over:
    case DebugAction::step_out:
        state->action = command->action;
        state->step_activation = frame->activation;
        break;
    case DebugAction::abort:
        if (!invocation.failed)
            invocation.debug_aborted = true;
        return 1;
    default:
        state->handler_failed = true;
        invocation.fail(ErrorCode::argument_mismatch, "Debug command has an invalid action.");
        return 1;
    }
    // Inspecting the first failure can never resume its failed instruction.
    return point == DebugPoint::error ? 1 : 0;
}

void debug_leave(void* context, BusinessDebugFrame* frame) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    if (frame && invocation.debug_top == frame) {
        invocation.debug_top = frame->previous;
        frame->previous = nullptr;
        frame->activation = 0;
    }
}
}
