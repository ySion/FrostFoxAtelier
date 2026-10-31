#pragma once
#include "renderer/renderer.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace atelier::renderer::detail {
[[noreturn]] inline void frame_slot_failure(const char* name, const char* what) {
    throw Error{
        {rhi::ErrorCode::execution_failed, kGfxResult_InvalidOperation, std::string(name) + " frame slot " + what},
        true};
}
// Persistent CPU-written buffers, one per frame in flight, read by the GPU in
// place. gfx begins recording frame N only after frame N - frames_in_flight
// completed, so the slot picked by the recording sequence is no longer read.
// Slots grow to a power of two and never shrink until retired.
template <class T>
class FrameSlots {
public:
    // Returns this frame's slot holding values. A slot that already holds the
    // latest values is not rewritten while changed stays false.
    GfxBuffer write(GfxContext gfx, std::uint32_t frames_in_flight, std::span<const T> values, bool changed,
                    const char* name) {
        const auto sequence = gfxGetRecordingFrameSequence(gfx);
        if (!sequence)
            frame_slot_failure(name, "written outside a recording frame");
        // Work already recorded this frame reads the slot at execution; a second write would redirect it.
        if (sequence == sequence_)
            frame_slot_failure(name, "written twice in one recording frame");
        sequence_ = sequence;
        if (slots_.empty())
            slots_.resize(frames_in_flight);
        revision_ += changed;
        auto& slot = slots_[sequence % frames_in_flight];
        const auto count = static_cast<std::uint32_t>(std::max<std::size_t>(1, values.size()));
        if (slot.buffer.getCount() < count) {
            if (slot.buffer && gfxDestroyBuffer(gfx, slot.buffer) != kGfxResult_NoError)
                frame_slot_failure(name, "retirement failed");
            slot.buffer = gfxCreateBuffer<T>(gfx, std::bit_ceil(count), nullptr, kGfxCpuAccess_Write);
            if (!slot.buffer)
                throw Error{{rhi::ErrorCode::creation_failed, kGfxResult_OutOfMemory,
                             std::string(name) + " frame slot allocation failed"},
                            true};
            slot.revision = 0;
        }
        if (slot.revision != revision_) {
            if (!values.empty())
                std::memcpy(gfxBufferGetData(gfx, slot.buffer), values.data(), values.size_bytes());
            slot.revision = revision_;
        }
        return slot.buffer;
    }
    GfxResult retire(GfxContext gfx) {
        GfxResult result = kGfxResult_NoError;
        for (auto& slot : slots_)
            if (slot.buffer)
                if (const auto r = gfxDestroyBuffer(gfx, slot.buffer); r && !result)
                    result = r;
        slots_.clear();
        sequence_ = 0;
        return result;
    }
    std::uint64_t bytes() const {
        std::uint64_t result = 0;
        for (const auto& slot : slots_)
            result += slot.buffer.getSize();
        return result;
    }

private:
    struct Slot {
        GfxBuffer buffer{};
        std::uint64_t revision{};
    };
    std::vector<Slot> slots_;
    std::uint64_t revision_{1}, sequence_{};
};
// A persistent default-heap table for data that passes read many times per
// frame; upload-heap reads cost Dungeon's direct passes about 2%. Changed
// values are staged in a frame slot and copied in where write records, so
// work recorded earlier this frame still reads the previous values.
template <class T>
class DeviceTable {
public:
    // Copies values[first, last) in; growing the table copies every value.
    GfxBuffer write(GfxContext gfx, std::uint32_t frames_in_flight, std::span<const T> values, std::size_t first,
                    std::size_t last, const char* name) {
        const auto count = static_cast<std::uint32_t>(std::max<std::size_t>(1, values.size()));
        if (buffer_.getCount() < count) {
            if (buffer_ && gfxDestroyBuffer(gfx, buffer_) != kGfxResult_NoError)
                frame_slot_failure(name, "table retirement failed");
            buffer_ = gfxCreateBuffer<T>(gfx, std::bit_ceil(count));
            if (!buffer_)
                throw Error{{rhi::ErrorCode::creation_failed, kGfxResult_OutOfMemory,
                             std::string(name) + " table allocation failed"},
                            true};
            first = 0;
            last = values.size();
        }
        if (first < last) {
            const auto changed = values.subspan(first, last - first);
            const auto staging = staging_.write(gfx, frames_in_flight, changed, true, name);
            if (gfxCommandCopyBuffer(gfx, buffer_, first * sizeof(T), staging, 0, changed.size_bytes()) !=
                kGfxResult_NoError)
                frame_slot_failure(name, "table copy failed");
        }
        return buffer_;
    }
    GfxBuffer write(GfxContext gfx, std::uint32_t frames_in_flight, std::span<const T> values, bool changed,
                    const char* name) {
        return write(gfx, frames_in_flight, values, 0, changed ? values.size() : 0, name);
    }
    GfxResult retire(GfxContext gfx) {
        GfxResult result = staging_.retire(gfx);
        if (buffer_)
            if (const auto r = gfxDestroyBuffer(gfx, std::exchange(buffer_, {})); r && !result)
                result = r;
        return result;
    }
    std::uint64_t bytes() const { return buffer_.getSize() + staging_.bytes(); }

private:
    GfxBuffer buffer_{};
    FrameSlots<T> staging_;
};
}
