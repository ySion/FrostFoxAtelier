#pragma once
#include "application/project_runner.hpp"
#include "runtime/runtime.hpp"
#include <Windows.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atelier::editor {
struct LiveChange;
}
namespace atelier::application {
// A duplex message link over two anonymous pipes. Each message is a uint32 LE
// byte count followed by UTF-8 JSON. One thread reads and one writes; they only
// move text, so the owner thread never waits for the peer.
class LiveLink final {
public:
    static constexpr std::size_t maximum_message = 4 * 1024 * 1024;
    LiveLink(HANDLE read, HANDLE write); // Owns both handles.
    ~LiveLink();
    LiveLink(const LiveLink&) = delete;
    LiveLink& operator=(const LiveLink&) = delete;
    void send(std::string);
    std::vector<std::string> receive();
    // The peer closed its end, a transfer failed or a message broke the size limit.
    bool closed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Editor end of one Play run's change link: numbers batches, spills large ones to
// files, matches acknowledgements and decides when Play no longer follows the
// document. Divergence is final for the run; forwarding stops until Play restarts.
class PlayLink final {
public:
    static constexpr std::size_t maximum_batch = 16 * 1024 * 1024;
    static constexpr std::size_t maximum_pending = 256, maximum_pending_bytes = 64 * 1024 * 1024;
    // Shared by texture pixels and encoded mesh snapshots, independent of JSON limits.
    static constexpr std::size_t maximum_pending_resource_bytes = 256 * 1024 * 1024;
    ~PlayLink() { static_cast<void>(close()); }
    // spill is the run's private directory for large batches and resource content.
    void open(std::unique_ptr<LiveLink>, std::filesystem::path spill);
    bool active() const noexcept { return link_ != nullptr; }
    // Sends one step's ops in order, split at op boundaries into consecutive batches.
    void forward(const std::vector<editor::LiveChange>& ops);
    void fail(std::string reason);
    // Reads acknowledgements. running and stopping describe the Play process; a link
    // that closes while Play keeps running is divergence.
    void poll(bool running, bool stopping);
    // Ends the run's link and removes its spill directory.
    std::error_code close() noexcept;
    std::size_t unacknowledged() const noexcept { return pending_.size(); }
    const std::string& divergence() const noexcept { return divergence_; }
    // Internal and unstable, for acceptance: the Play graph of one Entity by document identity.
    std::uint64_t inspect(std::uint64_t identity);
    // The graph text, or an error message prefixed with "error: ".
    std::optional<std::string> inspected(std::uint64_t request);

private:
    // Owns every spill file until ACK/close, or until an unsent batch is abandoned.
    struct Pending {
        std::uint64_t seq{};
        std::size_t bytes{}, resource_bytes{};
        std::vector<std::filesystem::path> files;
        Pending() = default;
        Pending(Pending&&) noexcept = default;
        ~Pending();
    };
    std::unique_ptr<LiveLink> link_;
    std::filesystem::path spill_;
    std::uint64_t next_seq_{1}, next_request_{1}, next_texture_{1}, next_mesh_{1};
    std::deque<Pending> pending_;
    std::size_t pending_bytes_{}, pending_resource_bytes_{};
    std::string divergence_;
    std::optional<std::chrono::steady_clock::time_point> closed_since_;
    std::unordered_map<std::uint64_t, std::string> inspected_;
    void send(std::string_view ops, Pending&&);
};

// Play end: applies arrived batches at the runner's host boundary and acknowledges
// each one. After a sequence gap or a failed batch it applies nothing more.
class PlayEnd final : public PlayChannel {
public:
    explicit PlayEnd(std::unique_ptr<LiveLink> link) : link_(std::move(link)) {}
    void service(editor::Session&, editor::Project&, const Report&) override;

private:
    std::unique_ptr<LiveLink> link_;
    std::uint64_t expected_{1};
    bool diverged_{};
};
}
