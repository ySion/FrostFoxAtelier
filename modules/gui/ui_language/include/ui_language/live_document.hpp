#pragma once
#include "ui_language/component.hpp"
#include "foundation/map.hpp"
#include <chrono>
#include <filesystem>

namespace atelier::ui_language {
// Borrowed descriptor slice and producer revision from a LiveDocument region.
struct ControlRegion {
    ui::ControlList* target{};

    Span<const ui::Control> controls{};

    std::uint64_t revision{};
};

// A snapshot consumer copies descriptors; a retained host can reuse the same authored region.
class ControlOutput {
public:
    ControlOutput(Array<ui::Control>& values) : flat_(&values) {}

    ControlOutput(ui::ControlList& list) : list_(&list) {}

    ControlOutput(ControlRegion& region) : region_(&region) {}

    void require_full_submission() const {
        if (list_)
            list_->require_full_submission();
        else if (region_ && region_->target)
            region_->target->require_full_submission();
    }

    void append(Span<const ui::Control> controls, std::uint64_t revision) const {
        if (list_)
            list_->append(controls, revision);
        else if (region_) {
            region_->controls = controls;
            region_->revision = revision;
        } else
            flat_->append(controls);
    }

private:
    Array<ui::Control>* flat_{};

    ui::ControlList* list_{};

    ControlRegion* region_{};
};

// One authoring file, one Runtime and one host key authority. Hot entry compilation
// uses captured bindings; the Runtime thread publishes. The host owns Session submission:
// append -> set_controls -> route cancellation effects -> prepare layout ->
// commit_controls. Reject a failed submission/layout before staging the old
// scopes again; a surface that cannot be prepared cannot publish a candidate.
class LiveDocument {
public:
    using Clock = std::chrono::steady_clock;

    struct ReloadTiming {
        // Detected uses the watcher's poll time. All later boundaries use the monotonic clock.
        Clock::time_point detected;

        Clock::time_point read_begin;

        Clock::time_point compile_begin;

        Clock::time_point compile_end;

        Clock::time_point instances_ready;

        Clock::time_point committed;

        Clock::time_point staged;

        Clock::time_point worker_begin;

        Clock::time_point worker_end;

        Clock::time_point install_begin;

        // Region creation/update against fresh host inputs, after rebinding the mounted definitions.
        Clock::duration append_updates{};

        Module::Compilation compilation{};

        Module::CompileTiming module;

        std::size_t rebound_regions{};

        std::uint64_t revision{};
    };

    enum class Profile { off, frame, compiler };

    // Last attempted file reload. Borrow valid until sampling is disabled or the document closes.
    const ReloadTiming* profile_reloads(Profile);

    LiveDocument(Runtime&, std::filesystem::path, ui::ControlKey&, Optional<std::string> recovery_source = {});

    void poll(Clock::time_point now = Clock::now());

    bool compiling() const noexcept { return compiling_ != nullptr; }

    // Parent and key aliases are immutable region configuration. On an input
    // error, output contains the accepted snapshot and that input is suppressed
    // until it changes; the accepted scopes can still handle their own events.
    // A successful false means the region used its accepted fallback for suppressed inputs.
    Result<bool> append(std::string_view component, ui::ControlKey parent, Span<const KeyBinding>,
                        Span<const InputValue>, ControlOutput);

    Result<bool> event(const ui::Action&);

    // Reject source replacement or ordinary prepared input updates. Old scope
    // membership, values and state survive; returns false if nothing was staged.
    bool reject_controls(std::string message);

    bool candidate_failed() const noexcept { return candidate_failed_; }

    void commit_controls();

    Optional<Statistics> statistics(std::string_view component) const;

    std::string_view diagnostic() const noexcept { return diagnostic_; }

    const Optional<std::string>& committed_source() const noexcept { return active_source_; }

    Optional<std::string_view> attempted_source() const noexcept {
        return attempted_source_ ? Optional<std::string_view>{*attempted_source_} : Optional<std::string_view>{};
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    std::uint64_t revision() const noexcept { return revision_; }

    // Auxiliary native surfaces share only the last committed authoring module.
    // Their Instances and accepted frames retain their own publication lifetimes.
    std::shared_ptr<Component> component(std::string_view name) const {
        return active_ ? active_->find(name) : nullptr;
    }

private:
    struct Region {
        ui::ControlKey parent{};

        Array<KeyBinding> keys;

        std::unique_ptr<Instance> instance;

        Optional<Array<InputValue>> rejected_inputs;

        bool accepted{};

        std::uint64_t accepted_controls{};

        std::uint64_t proposed_controls{};
    };

    struct Stamp {
        std::filesystem::file_time_type time{};

        std::uintmax_t size{};

        int error{};

        bool operator==(const Stamp&) const = default;
    };

    Runtime& runtime_;

    std::filesystem::path path_;

    ui::ControlKey& next_key_;

    Map<std::string, Region> regions_;

    std::shared_ptr<Module> active_;

    std::shared_ptr<Module> candidate_;

    Optional<Stamp> stamp_;

    Clock::time_point next_poll_{};

    Clock::time_point changed_at_{};

    std::unique_ptr<ReloadTiming> timing_;

    bool profile_components_{};

    Optional<std::string> attempted_source_;

    Optional<std::string> active_source_;

    Optional<std::string> candidate_source_;

    Optional<std::string> recovery_source_;

    void recover_initial();

    struct InFlight {
        std::string source;

        std::size_t catalog_size{};

        Module::CompileTiming components;

        Module::WorkerTiming worker;

        std::shared_ptr<Module::Build> build; // Joins before the worker's samples are destroyed.
    };

    void install(InFlight&);

    std::string diagnostic_;

    std::string failed_region_;

    bool changed_{};

    bool attempted_{};

    bool candidate_failed_{};

    bool pending_submission_{};

    bool submission_failed_{};

    std::uint64_t revision_{};

    std::unique_ptr<InFlight> compiling_;
};
}
