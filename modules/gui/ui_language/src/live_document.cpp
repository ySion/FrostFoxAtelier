#include "ui_language/live_document.hpp"
#include <algorithm>
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

Optional<std::string> read_source(const std::filesystem::path& path, std::uintmax_t size) {
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!file || !file.read(bytes.data(), static_cast<std::streamsize>(size)) ||
        file.peek() != std::ifstream::traits_type::eof())
        return {};
    return bytes;
}

}

LiveDocument::LiveDocument(Runtime& runtime, std::filesystem::path path, ui::ControlKey& keys,
                           Optional<std::string> recovery)
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
    candidate_source_ = recovery_source_.take();
    candidate_failed_ = false;
}

const LiveDocument::ReloadTiming* LiveDocument::profile_reloads(Profile profile) {
    profile_components_ = profile == Profile::compiler;
    if (profile == Profile::off)
        timing_.reset();
    else if (!timing_)
        timing_ = std::make_unique<ReloadTiming>();
    return timing_.get();
}

void LiveDocument::poll(Clock::time_point now) {
    if (candidate_ || pending_submission_)
        return;
    const bool ready = compiling_ && Module::ready(*compiling_->build);
    if (!ready && now < next_poll_)
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
    if (compiling_) {
        if (changed_)
            Module::cancel(*compiling_->build);
        if (!ready)
            return;
        auto completed = std::move(compiling_);
        // Observe the file even before the next periodic poll when a worker finishes.
        // A changed file invalidates both successful code and diagnostics from that save.
        if (!changed_ && completed->catalog_size == runtime_.type_count()) {
            if (timing_)
                timing_->install_begin = Clock::now();
            // An atomic replacement can preserve both size and modification time.
            // Only a completed hot build pays for the final byte comparison.
            if (read_source(path_, current.size) == completed->source) {
                install(*completed);
                return;
            }
            changed_at_ = now;
        } else if (completed->catalog_size != runtime_.type_count())
            attempted_source_.reset();
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
    auto source = read_source(path_, current.size);
    if (!source) {
        attempted_ = true;
        attempted_source_.reset();
        changed_ = true;
        changed_at_ = now;
        diagnostic_ = "Cannot read UI authoring file: " + utf8(path_);
        recover_initial();
        return;
    }
    if (source == attempted_source_)
        return;
    attempted_ = true;
    failed_region_.clear();
    submission_failed_ = false;
    if (timing_)
        timing_->compile_begin = Clock::now();
    auto job = std::make_unique<InFlight>();
    job->source = std::move(*source);
    auto build = Module::start(runtime_, utf8(path_.filename()), job->source, active_.get(),
                               profile_components_ ? &job->components : nullptr, active_ != nullptr,
                               timing_ ? &job->worker : nullptr);
    if (!build) {
        attempted_source_ = std::move(job->source);
        diagnostic_ = build.error().message;
        recover_initial();
        return;
    }
    job->build = std::move(*build);
    job->catalog_size = runtime_.type_count();
    if (timing_)
        timing_->staged = Clock::now();
    if (Module::dispatched(*job->build))
        compiling_ = std::move(job);
    else
        install(*job);
}

void LiveDocument::install(InFlight& job) {
    attempted_source_ = std::move(job.source);
    auto* sample = timing_ && timing_->compile_begin != Clock::time_point{} ? timing_.get() : nullptr;
    if (sample && sample->install_begin == Clock::time_point{})
        sample->install_begin = Clock::now();
    auto module = Module::finish(*job.build);
    if (sample) {
        sample->compile_end = Clock::now();
        sample->worker_begin = job.worker.begin;
        sample->worker_end = job.worker.end;
        sample->module = std::move(job.components);
    }
    if (!module) {
        diagnostic_ = module.error().message;
        recover_initial();
        return;
    }
    candidate_ = std::move(*module);
    if (sample)
        sample->compilation = candidate_->compilation();
    candidate_source_ = attempted_source_;
    candidate_failed_ = false;
    for (auto [name, region] : regions_) {
        if (!region.accepted)
            continue;
        // An unmounted region can be rebound and committed without an intervening append.
        region.proposed_controls = 0;
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
        if (sample)
            ++sample->rebound_regions;
    }
    if (sample)
        sample->instances_ready = Clock::now();
    diagnostic_.clear();
}

Result<bool> LiveDocument::append(std::string_view name, ui::ControlKey parent, Span<const KeyBinding> keys,
                                  Span<const InputValue> inputs, ControlOutput output) {
    const auto& module = candidate_ ? candidate_ : active_;
    const auto report = [&](const Error& error) -> Result<bool> {
        diagnostic_ = error.message;
        candidate_failed_ = static_cast<bool>(candidate_);
        submission_failed_ = false;
        if (!candidate_)
            failed_region_ = name;
        return std::unexpected(error);
    };
    if (!module)
        return failure(diagnostic_.empty() ? "No compiled UI module." : diagnostic_);
    if (candidate_)
        output.require_full_submission();
    auto* found = regions_.find(name);
    bool blocked{};
    bool retried{};
    if (!found) {
        if (!module->find(name))
            return report(Error{ErrorCode::invalid_definition, "UI module is missing component: " + std::string(name)});
        found = regions_.try_emplace(std::string(name), Region{parent, Array<KeyBinding>{keys}, {}, {}, false}).value;
    }
    auto& region = *found;
    if (region.parent != parent || region.keys.view() != keys) {
        if (region.instance) {
            region.instance->reject_controls(static_cast<bool>(candidate_));
            const auto controls = region.instance->controls();
            output.append(controls, 0);
        }
        return report(Error{ErrorCode::invalid_definition, "A live region's parent and key aliases are immutable."});
    }
    blocked = !candidate_ && region.rejected_inputs &&
              std::ranges::equal(*region.rejected_inputs, inputs, detail::same_binding_value);
    retried = region.rejected_inputs.has_value() && !blocked;
    {
        auto* sample = candidate_ && timing_ && timing_->compile_begin != Clock::time_point{} ? timing_.get() : nullptr;
        const auto start = sample ? Clock::now() : Clock::time_point{};
        if (!region.instance) {
            if (blocked)
                return false; // A refused first mount contributes an empty accepted view.
            auto instance = Instance::create(module->find(name), parent, next_key_, keys, inputs);
            if (!instance) {
                if (!candidate_)
                    region.rejected_inputs = Array<InputValue>{inputs};
                return report(instance.error());
            }
            region.instance = std::move(*instance);
        } else {
            auto updated = region.instance->update(blocked ? region.instance->inputs() : inputs, module->find(name));
            if (!updated) {
                if (!candidate_)
                    region.rejected_inputs = Array<InputValue>{inputs};
                region.instance->reject_controls(static_cast<bool>(candidate_));
                const auto controls = region.instance->controls();
                output.append(controls, 0);
                return report(updated.error());
            }
        }
        if (sample)
            sample->append_updates += Clock::now() - start;
        if (!blocked)
            region.rejected_inputs.reset();
    }
    pending_submission_ |= !region.accepted || region.instance->has_pending();
    if (!region.accepted || candidate_ || region.instance->changes_controls()) {
        region.proposed_controls = ui::ControlList::new_region_revision();
    } else {
        if (!region.accepted_controls)
            region.accepted_controls = ui::ControlList::new_region_revision();
        region.proposed_controls = region.accepted_controls;
    }
    output.append(region.instance->controls(), region.proposed_controls);
    if (!blocked && failed_region_ == name) {
        failed_region_.clear();
        diagnostic_.clear();
    }
    const auto any_rejected = [&] {
        for (const auto entry : regions_)
            if (entry.value.rejected_inputs)
                return true;
        return false;
    };
    if (submission_failed_ && (retried || region.instance->changes_controls()) && !any_rejected()) {
        submission_failed_ = false;
        diagnostic_.clear();
    }
    return !blocked;
}

Result<bool> LiveDocument::event(const ui::Action& action) {
    for (auto [name, region] : regions_)
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
    for (auto [name, region] : regions_) {
        if (!region.instance)
            continue;
        region.proposed_controls = 0;
        if (!region.accepted) {
            if (!source) {
                const auto values = region.instance->inputs();
                region.rejected_inputs = Array<InputValue>{values};
            }
            region.instance.reset();
            continue;
        }
        if (source || region.instance->has_pending()) {
            if (!source) {
                const auto values = region.instance->inputs();
                region.rejected_inputs = Array<InputValue>{values};
            }
            region.instance->reject_controls(source);
            if (!source &&
                std::ranges::equal(*region.rejected_inputs, region.instance->inputs(), detail::same_binding_value))
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
        for (auto [name, region] : regions_)
            if (region.instance) {
                if (candidate_ || !region.accepted || region.instance->changes_controls())
                    region.accepted_controls = region.proposed_controls;
                region.proposed_controls = 0;
                region.instance->commit_controls();
                region.accepted = true;
            }
    if (candidate_) {
        active_ = std::move(candidate_);
        active_source_ = candidate_source_.take();
        recovery_source_.reset();
        ++revision_;
        if (timing_ && timing_->compile_begin != Clock::time_point{}) {
            timing_->revision = revision_;
            timing_->committed = Clock::now();
        }
    }
    pending_submission_ = false;
}

Optional<Statistics> LiveDocument::statistics(std::string_view name) const {
    const auto* region = regions_.find(name);
    return !region || !region->instance ? Optional<Statistics>{} : Optional{region->instance->statistics()};
}
}
