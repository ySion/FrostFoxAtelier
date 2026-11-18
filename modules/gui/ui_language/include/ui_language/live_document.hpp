#pragma once
#include "ui_language/component.hpp"
#include <chrono>
#include <filesystem>
#include <map>

namespace atelier::ui_language {
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

    LiveDocument(Runtime&, std::filesystem::path, ui::ControlKey&, std::optional<std::string> recovery_source = {});

    void poll(Clock::time_point now = Clock::now());

    bool compiling() const noexcept { return compiling_ != nullptr; }

    // Parent and key aliases are immutable region configuration. On an input
    // error, output contains the accepted snapshot and that input is suppressed
    // until it changes; the accepted scopes can still handle their own events.
    Result<void> append(std::string_view component, ui::ControlKey parent, std::span<const KeyBinding>,
                        std::span<const InputValue>, std::vector<ui::Control>&);

    Result<bool> event(const ui::Action&);

    // Reject source replacement or ordinary prepared input updates. Old scope
    // membership, values and state survive; returns false if nothing was staged.
    bool reject_controls(std::string message);

    bool candidate_failed() const noexcept { return candidate_failed_; }

    void commit_controls();

    std::optional<Statistics> statistics(std::string_view component) const;

    std::string_view diagnostic() const noexcept { return diagnostic_; }

    const std::optional<std::string>& committed_source() const noexcept { return active_source_; }

    std::optional<std::string_view> attempted_source() const noexcept {
        return attempted_source_ ? std::optional<std::string_view>{*attempted_source_} : std::nullopt;
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

        std::vector<KeyBinding> keys;

        std::unique_ptr<Instance> instance;

        std::optional<std::vector<InputValue>> rejected_inputs;

        bool accepted{};
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

    std::map<std::string, Region, std::less<>> regions_;

    std::shared_ptr<Module> active_;

    std::shared_ptr<Module> candidate_;

    std::optional<Stamp> stamp_;

    Clock::time_point next_poll_{};

    Clock::time_point changed_at_{};

    std::unique_ptr<ReloadTiming> timing_;

    bool profile_components_{};

    std::optional<std::string> attempted_source_;

    std::optional<std::string> active_source_;

    std::optional<std::string> candidate_source_;

    std::optional<std::string> recovery_source_;

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
