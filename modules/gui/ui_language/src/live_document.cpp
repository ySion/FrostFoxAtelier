#include "ui_language/live_document.hpp"
#include <algorithm>
#include <bit>
#include <fstream>

namespace atelier::ui_language {
namespace {
auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

bool same_number(double a, double b) {
    return a == b || std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool same_field(const ui::NumberField& a, const ui::NumberField& b) {
    return a.key == b.key && a.label == b.label && a.axis == b.axis && a.single_precision == b.single_precision &&
           same_number(a.value, b.value) && same_number(a.default_value, b.default_value) &&
           same_number(a.minimum, b.minimum) && same_number(a.maximum, b.maximum) &&
           same_number(a.drag_step, b.drag_step) && same_number(a.fine_step, b.fine_step) &&
           a.slider.has_value() == b.slider.has_value() &&
           (!a.slider || (a.slider->key == b.slider->key && same_number(a.slider->minimum, b.slider->minimum) &&
                          same_number(a.slider->maximum, b.slider->maximum)));
}

bool same_rejected_inputs(std::span<const InputValue> a, std::span<const InputValue> b) {
    return std::ranges::equal(a, b, [](const InputValue& left, const InputValue& right) {
        if (left.index() != right.index())
            return false;
        if (const auto* number = std::get_if<double>(&left))
            return same_number(*number, std::get<double>(right));
        if (const auto* edit = std::get_if<EditBinding>(&left)) {
            const auto& other = std::get<EditBinding>(right);
            return edit->group == other.group && edit->enabled == other.enabled && edit->modified == other.modified &&
                   std::ranges::equal(edit->fields, other.fields, same_field);
        }
        if (const auto* list = std::get_if<ItemList>(&left)) {
            const auto& other = std::get<ItemList>(right);
            if (list->values == other.values)
                return true;
            const std::span<const Item> items = list->values ? std::span(*list->values) : std::span<const Item>{};
            const std::span<const Item> others = other.values ? std::span(*other.values) : std::span<const Item>{};
            return std::ranges::equal(items, others, [](const Item& x, const Item& y) {
                return x.key == y.key && x.keys == y.keys && x.parent == y.parent &&
                       same_rejected_inputs(x.inputs, y.inputs);
            });
        }
        return left == right;
    });
}
}

LiveDocument::LiveDocument(Runtime& runtime, std::filesystem::path path, ui::ControlKey& keys,
                           std::optional<std::string> recovery)
    : runtime_(runtime), path_(std::move(path)), next_key_(keys), recovery_source_(std::move(recovery)) {}

void LiveDocument::recover_initial() {
    if (active_ || candidate_ || !recovery_source_)
        return;
    auto fallback = Module::compile(runtime_, utf8(path_.filename()), *recovery_source_);
    if (!fallback) {
        diagnostic_ += " Recovery UI: " + fallback.error().message;
        return;
    }
    candidate_ = std::move(*fallback);
    candidate_source_ = std::move(recovery_source_);
    recovery_source_.reset();
    candidate_failed_ = false;
}

const LiveDocument::ReloadTiming* LiveDocument::profile_reloads(bool enable) {
    if (!enable)
        timing_.reset();
    else if (!timing_)
        timing_ = std::make_unique<ReloadTiming>();
    return timing_.get();
}

void LiveDocument::poll(Clock::time_point now) {
    if (candidate_ || pending_submission_ || now < next_poll_)
        return;
    next_poll_ = now + std::chrono::milliseconds(100);
    std::error_code error;
    Stamp current;
    current.time = std::filesystem::last_write_time(path_, error);
    if (!error)
        current.size = std::filesystem::file_size(path_, error);
    current.error = error.value();
    if (!stamp_ || current != *stamp_) {
        stamp_ = current;
        changed_at_ = now;
        changed_ = true;
    }
    if (!changed_ || (attempted_ && now - changed_at_ < std::chrono::milliseconds(200)))
        return;
    changed_ = false;
    if (timing_) {
        *timing_ = {};
        timing_->detected = changed_at_;
        timing_->read_begin = Clock::now();
    }
    if (error) {
        attempted_ = true;
        attempted_source_.reset();
        diagnostic_ = utf8(path_) + ": Cannot inspect UI authoring file (" + std::to_string(error.value()) + ").";
        recover_initial();
        return;
    }
    if (current.size > 1024 * 1024) {
        attempted_ = true;
        attempted_source_.reset();
        diagnostic_ = "UI authoring file exceeds 1 MiB.";
        recover_initial();
        return;
    }
    std::ifstream stream(path_, std::ios::binary);
    std::string source(static_cast<std::size_t>(current.size), '\0');
    if (!stream || !stream.read(source.data(), static_cast<std::streamsize>(source.size()))) {
        attempted_ = true;
        attempted_source_.reset();
        changed_ = true;
        changed_at_ = now;
        diagnostic_ = "Cannot read UI authoring file: " + utf8(path_);
        recover_initial();
        return;
    }
    if (attempted_source_ && source == *attempted_source_)
        return;
    attempted_ = true;
    attempted_source_ = source;
    failed_region_.clear();
    submission_failed_ = false;
    if (timing_)
        timing_->compile_begin = Clock::now();
    auto module = Module::compile(runtime_, utf8(path_.filename()), source, active_.get());
    if (timing_)
        timing_->compile_end = Clock::now();
    if (!module) {
        diagnostic_ = module.error().message;
        recover_initial();
        return;
    }
    candidate_ = std::move(*module);
    if (timing_)
        timing_->compilation = candidate_->compilation();
    candidate_source_ = std::move(source);
    candidate_failed_ = false;
    for (auto& [name, region] : regions_) {
        if (!region.accepted)
            continue;
        auto type = candidate_->find(name);
        if (!type) {
            reject_controls("UI module is missing the mounted component: " + name);
            return;
        }
        auto prepared = region.instance->update(region.instance->inputs(), type);
        if (!prepared) {
            reject_controls(prepared.error().message);
            return;
        }
        pending_submission_ |= region.instance->has_pending();
        if (timing_)
            ++timing_->rebound_regions;
    }
    if (timing_)
        timing_->instances_ready = Clock::now();
    diagnostic_.clear();
}

Result<void> LiveDocument::append(std::string_view name, ui::ControlKey parent, std::span<const KeyBinding> keys,
                                  std::span<const InputValue> inputs, std::vector<ui::Control>& output) {
    const auto& module = candidate_ ? candidate_ : active_;
    const auto report = [&](const Error& error) -> Result<void> {
        diagnostic_ = error.message;
        candidate_failed_ = static_cast<bool>(candidate_);
        submission_failed_ = false;
        if (!candidate_)
            failed_region_ = name;
        return std::unexpected(error);
    };
    if (!module)
        return failure(diagnostic_.empty() ? "No compiled UI module." : diagnostic_);
    auto at = regions_.find(name);
    bool blocked{};
    bool retried{};
    if (at == regions_.end()) {
        if (!module->find(name))
            return report(Error{ErrorCode::invalid_definition, "UI module is missing component: " + std::string(name)});
        at = regions_.emplace(std::string(name), Region{parent, {keys.begin(), keys.end()}, {}, {}, false}).first;
    }
    {
        auto& region = at->second;
        if (region.parent != parent || !std::ranges::equal(region.keys, keys)) {
            if (region.instance) {
                region.instance->reject_controls(static_cast<bool>(candidate_));
                const auto controls = region.instance->controls();
                output.insert(output.end(), controls.begin(), controls.end());
            }
            return report(
                Error{ErrorCode::invalid_definition, "A live region's parent and key aliases are immutable."});
        }
        blocked = !candidate_ && region.rejected_inputs && same_rejected_inputs(*region.rejected_inputs, inputs);
        retried = region.rejected_inputs.has_value() && !blocked;
        auto* sample = candidate_ ? timing_.get() : nullptr;
        const auto start = sample ? Clock::now() : Clock::time_point{};
        if (!region.instance) {
            if (blocked)
                return {}; // A refused first mount contributes an empty accepted view.
            auto instance = Instance::create(module->find(name), parent, next_key_, keys, inputs);
            if (!instance) {
                if (!candidate_)
                    region.rejected_inputs = std::vector<InputValue>(inputs.begin(), inputs.end());
                return report(instance.error());
            }
            region.instance = std::move(*instance);
        } else {
            auto updated = region.instance->update(blocked ? region.instance->inputs() : inputs, module->find(name));
            if (!updated) {
                if (!candidate_)
                    region.rejected_inputs = std::vector<InputValue>(inputs.begin(), inputs.end());
                region.instance->reject_controls(static_cast<bool>(candidate_));
                const auto controls = region.instance->controls();
                output.insert(output.end(), controls.begin(), controls.end());
                return report(updated.error());
            }
        }
        if (sample)
            sample->append_updates += Clock::now() - start;
        if (!blocked)
            region.rejected_inputs.reset();
    }
    pending_submission_ |= !at->second.accepted || at->second.instance->has_pending();
    const auto controls = at->second.instance->controls();
    output.insert(output.end(), controls.begin(), controls.end());
    if (!blocked && failed_region_ == name) {
        failed_region_.clear();
        diagnostic_.clear();
    }
    if (submission_failed_ && (retried || at->second.instance->changes_controls()) &&
        std::ranges::none_of(regions_, [](const auto& entry) { return entry.second.rejected_inputs.has_value(); })) {
        submission_failed_ = false;
        diagnostic_.clear();
    }
    return {};
}

Result<bool> LiveDocument::event(const ui::Action& action) {
    for (auto& [name, region] : regions_)
        if (region.accepted) {
            auto handled = region.instance->event(action);
            if (!handled || *handled)
                return handled;
        }
    return false;
}

bool LiveDocument::reject_controls(std::string message) {
    if (!candidate_ && !pending_submission_)
        return false;
    diagnostic_ = std::move(message);
    failed_region_.clear();
    submission_failed_ = !candidate_;
    const bool source = static_cast<bool>(candidate_);
    for (auto& [name, region] : regions_) {
        if (!region.instance)
            continue;
        if (!region.accepted) {
            if (!source) {
                const auto values = region.instance->inputs();
                region.rejected_inputs = std::vector<InputValue>(values.begin(), values.end());
            }
            region.instance.reset();
            continue;
        }
        if (source || region.instance->has_pending()) {
            if (!source) {
                const auto values = region.instance->inputs();
                region.rejected_inputs = std::vector<InputValue>(values.begin(), values.end());
            }
            region.instance->reject_controls(source);
            if (!source && same_rejected_inputs(*region.rejected_inputs, region.instance->inputs()))
                region.rejected_inputs.reset();
        }
    }
    candidate_.reset();
    candidate_source_.reset();
    candidate_failed_ = false;
    pending_submission_ = false;
    recover_initial();
    return true;
}

void LiveDocument::commit_controls() {
    if (candidate_failed_)
        return;
    if (candidate_ || pending_submission_)
        for (auto& [name, region] : regions_)
            if (region.instance) {
                region.instance->commit_controls();
                region.accepted = true;
            }
    if (candidate_) {
        active_ = std::move(candidate_);
        active_source_ = std::move(candidate_source_);
        recovery_source_.reset();
        ++revision_;
        if (timing_) {
            timing_->revision = revision_;
            timing_->committed = Clock::now();
        }
    }
    pending_submission_ = false;
}

std::optional<Statistics> LiveDocument::statistics(std::string_view name) const {
    const auto at = regions_.find(name);
    return at == regions_.end() || !at->second.instance ? std::nullopt
                                                        : std::optional{at->second.instance->statistics()};
}
}
