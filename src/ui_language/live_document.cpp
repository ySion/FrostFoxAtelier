#include "ui_language/live_document.hpp"
#include <algorithm>
#include <fstream>

namespace atelier::ui_language {
namespace {
auto failure(std::string message) {return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)});}
std::string utf8(const std::filesystem::path& path) {const auto value=path.u8string();return {value.begin(),value.end()};}
}
LiveDocument::LiveDocument(Runtime& runtime,std::filesystem::path path,ui::ControlKey& keys)
    :runtime_(runtime),path_(std::move(path)),next_key_(keys) {}
void LiveDocument::poll(Clock::time_point now) {
    if (candidate_ || pending_submission_ || now<next_poll_) return;
    next_poll_=now+std::chrono::milliseconds(100);
    std::error_code error;
    Stamp current;current.time=std::filesystem::last_write_time(path_,error);
    if (!error) current.size=std::filesystem::file_size(path_,error);
    current.error=error.value();
    if (!stamp_ || current!=*stamp_) {stamp_=current;changed_at_=now;changed_=true;}
    if (!changed_ || (attempted_ && now-changed_at_<std::chrono::milliseconds(200))) return;
    changed_=false;
    if (error) {attempted_=true;attempted_source_.reset();diagnostic_=utf8(path_)+": Cannot inspect UI authoring file ("+std::to_string(error.value())+").";return;}
    if (current.size>1024*1024) {attempted_=true;attempted_source_.reset();diagnostic_="UI authoring file exceeds 1 MiB.";return;}
    std::ifstream stream(path_,std::ios::binary);
    std::string source(static_cast<std::size_t>(current.size),'\0');
    if (!stream || !stream.read(source.data(),static_cast<std::streamsize>(source.size()))) {
        attempted_=true;attempted_source_.reset();changed_=true;changed_at_=now;
        diagnostic_="Cannot read UI authoring file: "+utf8(path_);return;
    }
    if (attempted_source_ && source==*attempted_source_) return;
    attempted_=true;attempted_source_=source;failed_region_.clear();submission_failed_=false;
    const auto start=Clock::now();
    auto module=Module::compile(runtime_,utf8(path_.filename()),source,active_.module.get());
    compile_ms_=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    if (!module) {diagnostic_=module.error().message;return;}
    Version next;next.module=std::move(*module);
    for (const auto& [name,region]:active_.regions) {
        if (!region.accepted) continue; // Failed first mounts have no accepted state to migrate.
        auto type=next.module->find(name);
        if (!type) {diagnostic_="UI module is missing the mounted component: "+name;return;}
        auto instance=Instance::create(type,region.parent,next_key_,region.keys,region.instance->inputs(),region.instance.get());
        if (!instance) {diagnostic_=instance.error().message;return;}
        next.regions.emplace(name,Region{region.parent,region.keys,std::move(*instance),{},true});
    }
    candidate_=std::move(next);candidate_failed_=false;diagnostic_.clear();
}
Result<void> LiveDocument::append(std::string_view name,ui::ControlKey parent,std::span<const KeyBinding> keys,
    std::span<const InputValue> inputs,std::vector<ui::Control>& output) {
    auto& version=candidate_?*candidate_:active_;
    const auto report=[&](const Error& error)->Result<void> {
        diagnostic_=error.message;candidate_failed_=candidate_.has_value();submission_failed_=false;
        if (!candidate_) failed_region_=name;
        return std::unexpected(error);
    };
    if (!version.module) return failure(diagnostic_.empty()?"No compiled UI module.":diagnostic_);
    auto at=version.regions.find(name);
    bool blocked{},retried{};
    if (at==version.regions.end()) {
        if (!version.module->find(name)) return report(Error{ErrorCode::invalid_definition,"UI module is missing component: "+std::string(name)});
        at=version.regions.emplace(std::string(name),Region{parent,{keys.begin(),keys.end()},{},{},false}).first;
    }
    {
        auto& region=at->second;
        if (region.parent!=parent || !std::ranges::equal(region.keys,keys)) {
            if (region.instance) {
                region.instance->reject_controls();
                const auto controls=region.instance->controls();output.insert(output.end(),controls.begin(),controls.end());
            }
            return report(Error{ErrorCode::invalid_definition,"A live region's parent and key aliases are immutable."});
        }
        blocked=region.rejected_inputs && std::ranges::equal(*region.rejected_inputs,inputs);
        retried=region.rejected_inputs.has_value() && !blocked;
        if (!region.instance) {
            if (blocked) return {}; // A refused first mount contributes an empty accepted view.
            auto instance=Instance::create(version.module->find(name),parent,next_key_,keys,inputs);
            if (!instance) {
                region.rejected_inputs=std::vector<InputValue>(inputs.begin(),inputs.end());
                return report(instance.error());
            }
            region.instance=std::move(*instance);
        } else {
            auto published=blocked?Result<void>{}:region.instance->publish(inputs);
            auto updated=published?region.instance->update():published;
            if (!updated) {
                region.rejected_inputs=std::vector<InputValue>(inputs.begin(),inputs.end());
                region.instance->reject_controls();
                const auto controls=region.instance->controls();output.insert(output.end(),controls.begin(),controls.end());
                return report(updated.error());
            }
        }
        if (!blocked) region.rejected_inputs.reset();
    }
    pending_submission_|=!at->second.accepted || at->second.instance->has_pending();
    const auto controls=at->second.instance->controls();output.insert(output.end(),controls.begin(),controls.end());
    if (!blocked && failed_region_==name) {failed_region_.clear();diagnostic_.clear();}
    if (submission_failed_ && (retried || at->second.instance->changes_controls()) &&
        std::ranges::none_of(version.regions,[](const auto& entry){return entry.second.rejected_inputs.has_value();})) {
        submission_failed_=false;diagnostic_.clear();
    }
    return {};
}
Result<bool> LiveDocument::event(const ui::Action& action) {
    for (auto& [name,region]:active_.regions) if (region.accepted) {
        auto handled=region.instance->event(action);if (!handled || *handled) return handled;
    }
    return false;
}
bool LiveDocument::reject_controls(std::string message) {
    if (!candidate_ && !pending_submission_) return false;
    diagnostic_=std::move(message);failed_region_.clear();submission_failed_=!candidate_;
    if (candidate_) candidate_.reset();
    else for (auto& [name,region]:active_.regions) {
        if (!region.instance) continue;
        if (!region.accepted) {
            const auto values=region.instance->inputs();region.rejected_inputs=std::vector<InputValue>(values.begin(),values.end());
            region.instance.reset();continue;
        }
        if (region.instance->has_pending()) {
            const auto values=region.instance->inputs();region.rejected_inputs=std::vector<InputValue>(values.begin(),values.end());
            region.instance->reject_controls();
            // A rejected local event rolls back state; only external data needs
            // suppression. Otherwise identical accepted inputs would stay blocked.
            if (std::ranges::equal(*region.rejected_inputs,region.instance->inputs())) region.rejected_inputs.reset();
        }
    }
    candidate_failed_=false;pending_submission_=false;return true;
}
void LiveDocument::commit_controls() {
    if (candidate_failed_) return;
    if (candidate_) {active_=std::move(*candidate_);candidate_.reset();++revision_;}
    if (pending_submission_) for (auto& [name,region]:active_.regions) if (region.instance) {
        region.instance->commit_controls();region.accepted=true;
    }
    pending_submission_=false;
}
std::optional<Statistics> LiveDocument::statistics(std::string_view name) const {
    const auto& version=candidate_?*candidate_:active_;
    const auto at=version.regions.find(name);
    return at==version.regions.end() || !at->second.instance?std::nullopt:std::optional{at->second.instance->statistics()};
}
}
