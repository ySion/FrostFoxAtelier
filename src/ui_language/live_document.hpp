#pragma once
#include "ui_language/component.hpp"
#include <chrono>
#include <filesystem>
#include <map>

namespace atelier::ui_language {
// One authoring file, one Runtime and one host key authority. Compilation and
// publication run on that Runtime's thread. The host owns Session submission:
// append -> set_controls -> route cancellation effects -> commit_controls.
class LiveDocument {
public:
    using Clock=std::chrono::steady_clock;
    LiveDocument(Runtime&,std::filesystem::path,ui::ControlKey&);
    void poll(Clock::time_point now=Clock::now());
    // Parent and key aliases are immutable region configuration. On an input
    // error, output contains the accepted snapshot and that input is suppressed
    // until it changes; the accepted scopes can still handle their own events.
    Result<void> append(std::string_view component,ui::ControlKey parent,std::span<const KeyBinding>,
        std::span<const InputValue>,std::vector<ui::Control>&);
    Result<bool> event(const ui::Action&);
    // Reject source replacement or ordinary prepared input updates. Old scope
    // membership, values and state survive; returns false if nothing was staged.
    bool reject_controls(std::string message);
    bool candidate_failed() const noexcept {return candidate_failed_;}
    void commit_controls();
    std::optional<Statistics> statistics(std::string_view component) const;
    std::string_view diagnostic() const noexcept {return diagnostic_;}
    const std::filesystem::path& path() const noexcept {return path_;}
    std::uint64_t revision() const noexcept {return revision_;}
    double compile_milliseconds() const noexcept {return compile_ms_;}
private:
    struct Region {
        ui::ControlKey parent{};
        std::vector<KeyBinding> keys;
        std::unique_ptr<Instance> instance;
        std::optional<std::vector<InputValue>> rejected_inputs;
        bool accepted{};
    };
    struct Version {
        std::shared_ptr<Module> module;
        std::map<std::string,Region,std::less<>> regions;
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
    Version active_;
    std::optional<Version> candidate_;
    std::optional<Stamp> stamp_;
    Clock::time_point next_poll_{},changed_at_{};
    std::optional<std::string> attempted_source_;
    std::string diagnostic_,failed_region_;
    bool changed_{},attempted_{},candidate_failed_{},pending_submission_{},submission_failed_{};
    std::uint64_t revision_{};
    double compile_ms_{};
};
}
