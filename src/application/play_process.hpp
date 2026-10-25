#pragma once
#include "application/play_link.hpp"
#include "runtime/runtime.hpp"
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace atelier::application {
// Owns one project run and its descendants. Runtime identities and GPU objects
// remain in that process; only scene values, a stop event, text and change
// messages cross here. With live, the run gets a change link and *live its editor end.
class PlayProcess final {
public:
    PlayProcess()=default;
    ~PlayProcess();
    PlayProcess(const PlayProcess&)=delete;
    PlayProcess& operator=(const PlayProcess&)=delete;
    Result<void> start(const std::filesystem::path& project,const std::filesystem::path& scene,
        bool headless=false,std::uint64_t frames=0,const std::filesystem::path& report={},bool preview_ui=false,bool halt=false,
        std::unique_ptr<LiveLink>* live=nullptr);
    void stop() noexcept;
    void poll();
    void close() noexcept;
    bool running() const noexcept {return process_!=nullptr;}
    bool stopping() const noexcept {return stopping_;}
    bool forced() const noexcept {return forced_;}
    std::optional<DWORD> exit_code() const noexcept {return exit_code_;}
    std::string take_output();
private:
    HANDLE process_{},job_{},stop_{},output_{};
    bool stopping_{},forced_{};
    std::optional<DWORD> exit_code_;
    std::chrono::steady_clock::time_point stop_deadline_{};
    std::string pending_output_;
    void drain();
    void release() noexcept;
};
}
