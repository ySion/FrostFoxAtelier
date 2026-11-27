#include "foundation/frame_timing.hpp"
#include "memory/fatal.hpp"

#include <algorithm>

namespace atelier::timing {
namespace {
constinit std::array<const Marker*, capacity> installed{};

constinit std::size_t installed_size{};

constinit bool frozen{};

const FrameRecord empty_record{};
}

constinit thread_local Recorder* detail::bound{};

void install(std::initializer_list<Span<Marker* const>> modules) noexcept {
    if (frozen)
        fatal("The frame timing catalog is already installed.");
    // Bounded stack diagnostic: fixed declarations can identify a damaged key
    // without allocating or entering the logger on the fatal path.
    const auto reject = [](std::string_view reason, std::string_view key) {
        char message[256];
        const auto prefix = std::min(reason.size(), sizeof(message));
        std::copy_n(reason.data(), prefix, message);
        const auto suffix = std::min(key.size(), sizeof(message) - prefix);
        std::copy_n(key.data(), suffix, message + prefix);
        fatal({message, prefix + suffix});
    };
    const auto add = [&](Marker* marker) {
        if (!marker)
            fatal("A frame timing marker is null.");
        if (installed_size == capacity)
            reject("The frame timing catalog exceeds its capacity: ", marker->key_);
        if (marker->index_ != unregistered)
            reject("Frame timing marker is registered twice: ", marker->key_);
        for (std::size_t i = 0; i < installed_size; ++i)
            if (installed[i]->key_ == marker->key_)
                reject("Frame timing key is not unique: ", marker->key_);
        if (marker != &frame && (!marker->parent_ || marker->parent_->index_ == unregistered))
            reject("Frame timing parent must be registered before ", marker->key_);
        marker->index_ = static_cast<std::uint8_t>(installed_size);
        marker->parent_index_ = marker->parent_ ? marker->parent_->index_ : unregistered;
        installed[installed_size++] = marker;
    };
    add(&frame);
    for (const auto module : modules)
        for (auto* marker : module)
            add(marker);
    frozen = true;
}

Span<const Marker* const> catalog() noexcept {
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
