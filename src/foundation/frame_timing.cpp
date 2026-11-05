#include "foundation/frame_timing.hpp"

#include <stdexcept>
#include <string>

namespace atelier::timing {
namespace {
constinit std::array<const Marker*, capacity> installed{};

constinit std::size_t installed_size{};

constinit bool frozen{};

const FrameRecord empty_record{};
}

constinit thread_local Recorder* detail::bound{};

void install(std::initializer_list<std::span<Marker* const>> modules) {
    if (frozen)
        throw std::logic_error("The frame timing catalog is already installed.");
    std::array<Marker*, capacity> order{};
    std::size_t size{};
    // Roll back before building the message, which can throw.
    const auto reject = [&](std::string_view reason, std::string_view key = {}) {
        for (std::size_t i = 0; i < size; ++i)
            order[i]->index_ = order[i]->parent_index_ = unregistered;
        throw std::invalid_argument(std::string(reason).append(key));
    };
    const auto add = [&](Marker* marker) {
        if (!marker)
            reject("A frame timing marker is null.");
        if (size == capacity)
            reject("The frame timing catalog exceeds its capacity.");
        if (marker->index_ != unregistered)
            reject("Frame timing marker is registered twice: ", marker->key_);
        for (std::size_t i = 0; i < size; ++i)
            if (order[i]->key_ == marker->key_)
                reject("Frame timing key is not unique: ", marker->key_);
        if (marker != &frame && (!marker->parent_ || marker->parent_->index_ == unregistered))
            reject("Frame timing parent must be registered before ", marker->key_);
        marker->index_ = static_cast<std::uint8_t>(size);
        marker->parent_index_ = marker->parent_ ? marker->parent_->index_ : unregistered;
        order[size++] = marker;
    };
    add(&frame);
    for (const auto module : modules)
        for (auto* marker : module)
            add(marker);
    for (std::size_t i = 0; i < size; ++i)
        installed[i] = order[i];
    installed_size = size;
    frozen = true;
}

std::span<const Marker* const> catalog() noexcept {
    return {installed.data(), installed_size};
}

std::size_t depth(const Marker& marker) noexcept {
    std::size_t result{};
    for (auto* parent = marker.parent(); parent; parent = parent->parent())
        ++result;
    return result;
}

Recorder::Frame::Frame(Recorder& recorder) noexcept : recorder_(&recorder) {
    // Suspend unbinds the thread, so the Recorder's own state also detects nesting.
    if (auto* outer = detail::bound ? detail::bound : recorder.open_ ? &recorder : nullptr) {
        outer->fail(frame);
        recorder_ = nullptr;
        return;
    }
    auto& record = recorder.record_;
    record = {};
    record.sequence = ++recorder.frames_;
    recorder.open_ = true;
    recorder.current_ = frame.index();
    if (!frame.registered())
        recorder.fail(frame);
    detail::bound = &recorder;
    record.begin = Clock::now();
}

const FrameRecord& Recorder::Frame::finish() noexcept {
    if (!recorder_)
        return empty_record;
    auto& recorder = *recorder_;
    auto& record = recorder.record_;
    if (frame.registered()) {
        record.duration[frame.index()] = Clock::now() - record.begin;
        record.entries[frame.index()] = 1;
        if (recorder.current_ != frame.index())
            recorder.fail(frame);
    }
    recorder.current_ = unregistered;
    recorder.open_ = false;
    detail::bound = nullptr;
    recorder_ = nullptr;
    return record;
}

void record(const Marker& marker, std::chrono::nanoseconds total_duration, std::uint32_t entries) noexcept {
    auto* recorder = detail::bound;
    if (!recorder)
        return;
    if (!recorder->accepts(marker, entries))
        return;
    recorder->record_.duration[marker.index_] += total_duration;
    recorder->record_.entries[marker.index_] += entries;
}

}
