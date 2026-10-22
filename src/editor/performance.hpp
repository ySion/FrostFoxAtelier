#pragma once
#include "foundation/frame_timing.hpp"
#include "renderer/renderer.hpp"
#include "ui/controls.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atelier::editor {
// One CPU+GPU frame pipeline. CPU samples are the host's frame timing records of
// sampled steps; GPU samples are completed Scene Lit timings. Both windows share
// one domain and reset together. The table, Control and the Play report read the
// same published snapshot; no consumer evaluates timings or owns another history.
class FramePerformance {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr std::size_t window_size=120,pass_count=23,total_pass=22;
    // CPU table rows; a phase row is cpu_frame_row plus its catalog index.
    static constexpr ui::TableKey cpu_frame_row=1000,cpu_unattributed_row=cpu_frame_row+timing::capacity;
    enum class State { unavailable,waiting,ready };
    struct Pass {
        std::string_view key,title;
        int parent{-1};
        unsigned depth{};
        bool in_total{};
    };
    struct Timing {
        double current{},mean{},p95{},max{};
        std::optional<double> share;
        std::size_t active_samples{};
        bool current_active{};
    };
    struct Cpu {
        std::size_t samples{};
        std::uint64_t sequence{},frame_id{}; // Recorder sequence and accepted UI frame of the latest sample.
        // By catalog index; empty until the phase is entered in this domain.
        std::array<std::optional<Timing>,timing::capacity> phases{};
        std::array<std::uint32_t,timing::capacity> current_entries{};
        std::optional<Timing> unattributed; // The frame minus its direct children.
        std::optional<Timing> interval; // Between the begins of consecutive sampled steps.
        std::optional<double> recent_interval; // Mean of the consecutive intervals in the last half second.
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
        bool ddgi{},direct_reference{},direct_clustered{},direct_shadow_cache{};
        std::optional<std::uint64_t> resource_bytes;
        std::array<std::optional<Timing>,pass_count> timings{};
        Cpu cpu;
    };
    FramePerformance();
    void observe(const renderer::Frame&,const renderer::FrameResult&,Clock::time_point now=Clock::now());
    // Every host step. A sampled step was accepted without capture and names its UI frame.
    void observe(const timing::FrameRecord&,std::optional<std::uint64_t> sampled_frame,Clock::time_point now=Clock::now());
    void publish(Clock::time_point now=Clock::now());
    void reset();
    bool apply(const ui::Action&);
    static std::span<const Pass> passes();
    const Snapshot& snapshot() const noexcept {return published_;}
    // Over every observed step since construction, sampled or not.
    std::uint64_t structure_errors() const noexcept {return structure_errors_;}
    const timing::Marker* first_structure_error() const noexcept {return first_structure_error_;}
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
    struct CpuSample {
        std::array<double,timing::capacity> ms{};
        std::uint64_t entered{}; // Bit per catalog index.
        double unattributed{};
    };
    std::optional<Domain> domain_;
    std::uint64_t first_frame_{},sequence_{},frame_id_{};
    std::array<std::array<double,pass_count>,window_size> samples_{};
    std::array<std::uint32_t,window_size> masks_{};
    std::size_t next_{},count_{};
    std::vector<CpuSample> cpu_; // window_size entries, allocated once.
    std::array<std::uint32_t,timing::capacity> cpu_entries_{};
    std::size_t cpu_next_{},cpu_count_{};
    std::uint64_t cpu_seen_{},cpu_sequence_{},cpu_frame_{};
    std::array<double,window_size> intervals_{};
    std::array<Clock::time_point,window_size> interval_ends_{};
    std::size_t interval_next_{},interval_count_{};
    std::optional<std::pair<std::uint64_t,Clock::time_point>> previous_sample_; // Sequence and begin in this domain.
    std::uint64_t structure_errors_{};
    const timing::Marker* first_structure_error_{};
    std::vector<const timing::Marker*> cpu_rows_; // Catalog in depth-first order, without the frame.
    std::optional<std::uint64_t> resource_bytes_;
    Clock::time_point next_publish_{};
    Snapshot published_;
    std::array<float,6> widths_{180,72,72,72,72,64};
    ui::TableSelection selection_{};
    std::shared_ptr<const ui::TableData> table_;
    std::wstring status_,resources_;
    bool dirty_{};
    void clear_cpu();
    void rebuild_table();
};
}
