#pragma once
#include "renderer/renderer.hpp"
#include "ui/controls.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace atelier::editor {
// Only completed GPU samples enter this window. UI and Control share the same
// published snapshot; neither consumer evaluates timings or owns another history.
class RenderPerformance {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr std::size_t window_size=120,pass_count=21,total_pass=20;
    enum class State { unavailable,waiting,ready };
    struct Pass {
        std::string_view key,title;
        int parent{-1};
        unsigned depth{};
        bool in_total{};
    };
    struct Timing {
        double current{},mean{},p95{};
        std::optional<double> share;
        std::size_t active_samples{};
        bool current_active{};
    };
    struct Snapshot {
        State state{State::unavailable};
        ui::PixelExtent extent{};
        ui::Rect viewport{};
        std::size_t samples{};
        std::uint64_t sequence{},frame_id{};
        std::uint64_t scene_identity{},first_frame{};
        renderer::Antialiasing antialiasing{};
        std::uint32_t direct_candidates{};
        bool ddgi{},direct_reference{};
        std::optional<std::uint64_t> resource_bytes;
        std::array<std::optional<Timing>,pass_count> timings{};
    };
    RenderPerformance();
    void observe(const renderer::Frame&,const renderer::FrameResult&,Clock::time_point now=Clock::now());
    void reset();
    bool apply(const ui::Action&);
    static std::span<const Pass> passes();
    const Snapshot& snapshot() const noexcept {return published_;}
    const std::shared_ptr<const ui::TableData>& table() const noexcept {return table_;}
    ui::TableSelection selection() const noexcept {return selection_;}
    const std::wstring& status() const noexcept {return status_;}
    const std::wstring& resources() const noexcept {return resources_;}
private:
    struct Domain {
        std::uint64_t scene{};
        ui::PixelExtent extent;
        ui::Rect viewport;
        renderer::ViewSettings view;
        bool active{};
        bool operator==(const Domain&) const=default;
    };
    std::optional<Domain> domain_;
    std::uint64_t first_frame_{},sequence_{},frame_id_{};
    std::array<std::array<double,pass_count>,window_size> samples_{};
    std::array<std::uint32_t,window_size> masks_{};
    std::size_t next_{},count_{};
    std::optional<std::uint64_t> resource_bytes_;
    Clock::time_point next_publish_{};
    Snapshot published_;
    std::array<float,5> widths_{180,72,72,72,64};
    ui::TableSelection selection_{};
    std::shared_ptr<const ui::TableData> table_;
    std::wstring status_,resources_;
    bool dirty_{};
    void publish(Clock::time_point);
    void rebuild_table();
};
}
