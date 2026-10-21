#pragma once
#include "renderer/renderer.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace atelier::renderer::detail {
// Persistent CPU-written buffers, one per frame in flight, read by the GPU in
// place. gfx begins recording frame N only after frame N - frames_in_flight
// completed, so the slot picked by the recording sequence is no longer read.
// Slots grow to a power of two and never shrink until retired.
template<class T> class FrameSlots {
public:
    // Returns this frame's slot holding values. A slot that already holds the
    // latest values is not rewritten while changed stays false.
    GfxBuffer write(GfxContext gfx,std::uint32_t frames_in_flight,std::span<const T> values,bool changed,const char* name) {
        const auto sequence=gfxGetRecordingFrameSequence(gfx);
        if(!sequence)fail(name,"written outside a recording frame");
        // Work already recorded this frame reads the slot at execution; a second write would redirect it.
        if(sequence==sequence_)fail(name,"written twice in one recording frame");
        sequence_=sequence;
        if(slots_.empty())slots_.resize(frames_in_flight);
        revision_+=changed;
        auto& slot=slots_[sequence%frames_in_flight];
        const auto count=static_cast<std::uint32_t>(std::max<std::size_t>(1,values.size()));
        if(slot.buffer.getCount()<count) {
            if(slot.buffer&&gfxDestroyBuffer(gfx,slot.buffer)!=kGfxResult_NoError)fail(name,"retirement failed");
            slot.buffer=gfxCreateBuffer<T>(gfx,std::bit_ceil(count),nullptr,kGfxCpuAccess_Write);
            if(!slot.buffer)throw Error{{rhi::ErrorCode::creation_failed,kGfxResult_OutOfMemory,std::string(name)+" frame slot allocation failed"},true};
            slot.revision=0;
        }
        if(slot.revision!=revision_) {
            if(!values.empty())std::memcpy(gfxBufferGetData(gfx,slot.buffer),values.data(),values.size_bytes());
            slot.revision=revision_;
        }
        return slot.buffer;
    }
    GfxResult retire(GfxContext gfx) {
        GfxResult result=kGfxResult_NoError;
        for(auto& slot:slots_)if(slot.buffer)if(const auto r=gfxDestroyBuffer(gfx,slot.buffer);r&&!result)result=r;
        slots_.clear();sequence_=0;
        return result;
    }
    std::uint64_t bytes() const {
        std::uint64_t result=0;for(const auto& slot:slots_)result+=slot.buffer.getSize();return result;
    }
private:
    [[noreturn]] static void fail(const char* name,const char* what) {
        throw Error{{rhi::ErrorCode::execution_failed,kGfxResult_InvalidOperation,std::string(name)+" frame slot "+what},true};
    }
    struct Slot { GfxBuffer buffer{}; std::uint64_t revision{}; };
    std::vector<Slot> slots_;
    std::uint64_t revision_{1},sequence_{};
};
}
