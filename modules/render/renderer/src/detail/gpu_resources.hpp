#pragma once

#include "assets/atx_texture.hpp"
#include "foundation/result.hpp"
#include "math/math.hpp"
#include "rdg/graph.hpp"
#include "renderer/renderer.hpp"
#include "rhi/backend.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace atelier::renderer::detail {

inline Error make_error(rhi::ErrorCode code, Utf8Text message, bool terminal = false, std::int32_t native = 0) {
    return Error{rhi::Error{code, native, std::move(message)}, terminal};
}

template <std::same_as<StringView> SV>
inline Error make_error(rhi::ErrorCode code, SV message, bool terminal = false, std::int32_t native = 0) {
    return Error{rhi::Error{code, native, message}, terminal};
}

template <std::same_as<const char*> P>
inline Error make_error(rhi::ErrorCode code, P message, bool terminal = false, std::int32_t native = 0) {
    return Error{rhi::Error{code, native, message}, terminal};
}

inline Result<void> check_nri(nri::Result res, rhi::ErrorCode code, const char* message, bool terminal = false) {
    if (res == nri::Result::SUCCESS)
        return {};
    rhi::ErrorCode mapped = code;
    if (res == nri::Result::DEVICE_LOST)
        mapped = rhi::ErrorCode::device_lost;
    else if (res == nri::Result::OUT_OF_MEMORY)
        mapped = rhi::ErrorCode::out_of_memory;
    else if (res == nri::Result::UNSUPPORTED)
        mapped = rhi::ErrorCode::unsupported;
    return std::unexpected(make_error(mapped, message, terminal, static_cast<std::int32_t>(res)));
}

inline float fp16_to_fp32(std::uint16_t h) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h & 0x8000u)) << 16u;
    const std::uint32_t exp = (h >> 10u) & 0x1Fu;
    const std::uint32_t mant = h & 0x03FFu;
    std::uint32_t bits = 0u;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            const float sub = std::ldexp(static_cast<float>(mant), -24);
            return (h & 0x8000u) ? -sub : sub;
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13u);
    } else {
        bits = sign | ((exp + 112u) << 23u) | (mant << 13u);
    }
    return std::bit_cast<float>(bits);
}

inline std::uint16_t fp32_to_fp16(float v) noexcept {
    if (!std::isfinite(v))
        return 0u;
    const float clamped = std::clamp(v, -65504.0f, 65504.0f);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(clamped);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const int exp = static_cast<int>((bits >> 23u) & 0xFFu) - 127 + 15;
    std::uint32_t mant = (bits >> 13u) & 0x03FFu;
    if (exp <= 0) {
        if (exp < -10)
            return static_cast<std::uint16_t>(sign);
        mant = (mant | 0x0400u) >> static_cast<unsigned>(1 - exp);
        return static_cast<std::uint16_t>(sign | mant);
    }
    if (exp >= 31)
        return static_cast<std::uint16_t>(sign | 0x7BFFu);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10u) | mant);
}

inline math::Vector3 extract_camera_eye(const math::Matrix4& view) noexcept {
    // Row-vector LH view matrix: world->view is p_v = p_w * R + t => eye = -t * R^T.
    const float tx = view[12];
    const float ty = view[13];
    const float tz = view[14];
    return {
        -(tx * view[0] + ty * view[1] + tz * view[2]),
        -(tx * view[4] + ty * view[5] + tz * view[6]),
        -(tx * view[8] + ty * view[9] + tz * view[10]),
    };
}

inline std::uint32_t calc_mip_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t mips = 1u;
    for (std::uint32_t dim = std::max(width, height); dim > 1u; dim >>= 1u)
        ++mips;
    return mips;
}

inline std::uint64_t align_up_bytes(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment <= 1u)
        return value;
    const std::uint64_t rem = value % alignment;
    return rem == 0u ? value : (value + (alignment - rem));
}

inline nri::Format to_nri_format(assets::AtxFormat fmt) noexcept {
    switch (fmt) {
    case assets::AtxFormat::rgba8_unorm:
        return nri::Format::RGBA8_UNORM;
    case assets::AtxFormat::rgba8_srgb:
        return nri::Format::RGBA8_SRGB;
    case assets::AtxFormat::rgba16_sfloat:
        return nri::Format::RGBA16_SFLOAT;
    case assets::AtxFormat::bc1_rgba_unorm:
        return nri::Format::BC1_RGBA_UNORM;
    case assets::AtxFormat::bc1_rgba_srgb:
        return nri::Format::BC1_RGBA_SRGB;
    case assets::AtxFormat::bc4_r_unorm:
        return nri::Format::BC4_R_UNORM;
    case assets::AtxFormat::bc5_rg_unorm:
        return nri::Format::BC5_RG_UNORM;
    case assets::AtxFormat::bc6h_uf16:
        return nri::Format::BC6H_RGB_UFLOAT;
    case assets::AtxFormat::bc7_rgba_unorm:
        return nri::Format::BC7_RGBA_UNORM;
    case assets::AtxFormat::bc7_rgba_srgb:
        return nri::Format::BC7_RGBA_SRGB;
    }
    return nri::Format::RGBA8_UNORM;
}

inline std::uint64_t estimate_texture_bytes(nri::Format fmt, std::uint32_t w, std::uint32_t h, std::uint32_t d,
                                            std::uint32_t mips) noexcept {
    const nri::FormatProps* props = nri::nriGetFormatProps(fmt);
    if (!props)
        return std::uint64_t(w) * h * d * 4u;
    const std::uint32_t bw = std::max<std::uint32_t>(1u, props->blockWidth);
    const std::uint32_t bh = std::max<std::uint32_t>(1u, props->blockHeight);
    std::uint64_t total = 0u;
    for (std::uint32_t m = 0u; m < mips; ++m) {
        const std::uint32_t mw = std::max(1u, w >> m);
        const std::uint32_t mh = std::max(1u, h >> m);
        const std::uint32_t md = std::max(1u, d >> m);
        const std::uint64_t bx = (mw + bw - 1u) / bw;
        const std::uint64_t by = (mh + bh - 1u) / bh;
        total += bx * by * md * props->stride;
    }
    return total;
}

struct OwnedBuffer {
    nri::Buffer* buffer = nullptr;

    nri::Descriptor* srv = nullptr;

    nri::Descriptor* uav = nullptr;

    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint32_t uav_bindless = rhi::bindless_invalid_index;

    std::uint64_t capacity = 0u;

    rdg::BufferState state{nri::AccessBits::NONE, nri::StageBits::NONE};

    void retire(rhi::Device& device) noexcept {
        if (!buffer && !srv && !uav && srv_bindless == rhi::bindless_invalid_index &&
            uav_bindless == rhi::bindless_invalid_index)
            return;
        const rhi::FencePoint fence = rhi::BackendAccess::timeline(device).current_point();
        auto& rec = rhi::BackendAccess::reclaimer(device);
        if (srv || srv_bindless != rhi::bindless_invalid_index) {
            rec.retire({.fence = fence, .descriptor = srv, .bindless_index = srv_bindless});
            srv = nullptr;
            srv_bindless = rhi::bindless_invalid_index;
        }
        if (uav || uav_bindless != rhi::bindless_invalid_index) {
            rec.retire({.fence = fence, .descriptor = uav, .bindless_index = uav_bindless});
            uav = nullptr;
            uav_bindless = rhi::bindless_invalid_index;
        }
        if (buffer) {
            rec.retire({.fence = fence, .buffer = buffer});
            buffer = nullptr;
        }
        capacity = 0u;
        state = {nri::AccessBits::NONE, nri::StageBits::NONE};
    }
};

struct OwnedTexture {
    nri::Texture* texture = nullptr;

    nri::Descriptor* srv = nullptr;

    nri::Descriptor* uav = nullptr;

    nri::Descriptor* rtv = nullptr;

    nri::Descriptor* dsv = nullptr;

    nri::Descriptor* dsv_readonly = nullptr;

    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint32_t uav_bindless = rhi::bindless_invalid_index;

    Array<nri::Descriptor*> mip_srvs;

    Array<std::uint32_t> mip_srv_bindless;

    Array<nri::Descriptor*> mip_uavs;

    Array<std::uint32_t> mip_uav_bindless;

    std::uint32_t width = 0u;

    std::uint32_t height = 0u;

    std::uint32_t depth = 1u;

    std::uint32_t mip_count = 1u;

    nri::Format format = nri::Format::UNKNOWN;

    std::uint64_t byte_size = 0u;

    rdg::TextureState state{nri::AccessBits::NONE, nri::Layout::GENERAL, nri::StageBits::NONE};

    void retire(rhi::Device& device) noexcept {
        if (!texture && !srv && !uav && !rtv && !dsv && !dsv_readonly && mip_srvs.empty() && mip_uavs.empty())
            return;
        const rhi::FencePoint fence = rhi::BackendAccess::timeline(device).current_point();
        auto& rec = rhi::BackendAccess::reclaimer(device);
        if (srv || srv_bindless != rhi::bindless_invalid_index) {
            rec.retire({.fence = fence, .descriptor = srv, .bindless_index = srv_bindless});
            srv = nullptr;
            srv_bindless = rhi::bindless_invalid_index;
        }
        if (uav || uav_bindless != rhi::bindless_invalid_index) {
            rec.retire({.fence = fence, .descriptor = uav, .bindless_index = uav_bindless});
            uav = nullptr;
            uav_bindless = rhi::bindless_invalid_index;
        }
        for (std::size_t i = 0; i < mip_srvs.size(); ++i) {
            rec.retire(
                {.fence = fence,
                 .descriptor = mip_srvs[i],
                 .bindless_index = (i < mip_srv_bindless.size()) ? mip_srv_bindless[i] : rhi::bindless_invalid_index});
        }
        mip_srvs.clear();
        mip_srv_bindless.clear();
        for (std::size_t i = 0; i < mip_uavs.size(); ++i) {
            rec.retire(
                {.fence = fence,
                 .descriptor = mip_uavs[i],
                 .bindless_index = (i < mip_uav_bindless.size()) ? mip_uav_bindless[i] : rhi::bindless_invalid_index});
        }
        mip_uavs.clear();
        mip_uav_bindless.clear();
        if (rtv) {
            rec.retire({.fence = fence, .descriptor = rtv});
            rtv = nullptr;
        }
        if (dsv) {
            rec.retire({.fence = fence, .descriptor = dsv});
            dsv = nullptr;
        }
        if (dsv_readonly) {
            rec.retire({.fence = fence, .descriptor = dsv_readonly});
            dsv_readonly = nullptr;
        }
        if (texture) {
            rec.retire({.fence = fence, .texture = texture});
            texture = nullptr;
        }
        width = 0u;
        height = 0u;
        depth = 1u;
        mip_count = 1u;
        byte_size = 0u;
        state = {nri::AccessBits::NONE, nri::Layout::GENERAL, nri::StageBits::NONE};
    }
};

struct OwnedAccelerationStructure {
    nri::AccelerationStructure* as = nullptr;

    nri::Descriptor* srv = nullptr;

    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint64_t handle = 0u;

    std::uint64_t byte_size = 0u;

    std::uint32_t capacity = 0u;

    void retire(rhi::Device& device) noexcept {
        if (!as && !srv && srv_bindless == rhi::bindless_invalid_index)
            return;
        const rhi::FencePoint fence = rhi::BackendAccess::timeline(device).current_point();
        auto& rec = rhi::BackendAccess::reclaimer(device);
        if (srv || srv_bindless != rhi::bindless_invalid_index) {
            rec.retire({.fence = fence, .descriptor = srv, .bindless_index = srv_bindless});
            srv = nullptr;
            srv_bindless = rhi::bindless_invalid_index;
        }
        if (as) {
            rec.retire({.fence = fence, .as = as});
            as = nullptr;
        }
        handle = 0u;
        byte_size = 0u;
        capacity = 0u;
    }
};

template <class T>
class HostSlotRing {
public:
    static constexpr std::size_t slot_count = 4u;

    Result<std::pair<nri::Buffer*, std::uint32_t>> write(rhi::Device& device, std::size_t slot_index,
                                                         Span<const T> items, bool byte_address = false,
                                                         nri::BufferUsageBits extra_usage = nri::BufferUsageBits::NONE,
                                                         bool create_srv = true) {
        if (items.empty())
            return std::pair<nri::Buffer*, std::uint32_t>{nullptr, rhi::bindless_invalid_index};
        OwnedBuffer& slot = slots_[slot_index % slot_count];
        const std::uint64_t stride = byte_address ? 4u : static_cast<std::uint64_t>(sizeof(T));
        const std::uint64_t needed_bytes =
            std::max<std::uint64_t>(stride, static_cast<std::uint64_t>(items.size()) * sizeof(T));
        const auto& core = rhi::BackendAccess::core(device);
        nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);
        if (!slot.buffer || slot.capacity < needed_bytes) {
            slot.retire(device);
            const std::uint64_t alloc_bytes =
                align_up_bytes(std::max<std::uint64_t>(4096u, std::bit_ceil(needed_bytes)), stride);
            bool alloc_ok = false;
            struct SlotGuard {
                OwnedBuffer* s;

                rhi::Device* d;

                bool* ok;

                ~SlotGuard() {
                    if (!*ok)
                        s->retire(*d);
                }
            } guard{&slot, &device, &alloc_ok};

            nri::BufferDesc desc{};
            desc.size = alloc_bytes;
            desc.structureStride = (byte_address || !create_srv) ? 0u : static_cast<std::uint32_t>(sizeof(T));
            desc.byteAddress = byte_address;
            desc.usage =
                (create_srv ? nri::BufferUsageBits::SHADER_RESOURCE : nri::BufferUsageBits::NONE) | extra_usage;
            ATELIER_TRY(check_nri(
                core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_UPLOAD, 0.0f, desc, slot.buffer),
                rhi::ErrorCode::out_of_memory, "HostSlotRing buffer allocation failed", true));
            if ((desc.usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0) {
                nri::BufferViewDesc v_desc{};
                v_desc.buffer = slot.buffer;
                v_desc.type = byte_address ? nri::BufferView::BYTE_ADDRESS_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
                v_desc.offset = 0u;
                v_desc.size = nri::WHOLE_SIZE;
                v_desc.structureStride = desc.structureStride;
                ATELIER_TRY(check_nri(core.CreateBufferView(v_desc, slot.srv), rhi::ErrorCode::creation_failed,
                                      "HostSlotRing SRV creation failed", true));
                auto bindless = rhi::BackendAccess::bindless(device).allocate_resource(slot.srv);
                if (!bindless)
                    return std::unexpected(Error{bindless.error(), true});
                slot.srv_bindless = *bindless;
            }
            slot.capacity = alloc_bytes;
            alloc_ok = true;
        }
        void* mapped = core.MapBuffer(*slot.buffer, 0u, items.size_bytes());
        if (!mapped)
            return std::unexpected(make_error(rhi::ErrorCode::out_of_memory, "HostSlotRing MapBuffer failed", true));
        std::memcpy(mapped, items.data(), items.size_bytes());
        core.UnmapBuffer(*slot.buffer);
        return std::pair<nri::Buffer*, std::uint32_t>{slot.buffer, slot.srv_bindless};
    }

    void retire_all(rhi::Device& device) noexcept {
        for (OwnedBuffer& slot : slots_)
            slot.retire(device);
    }

private:
    std::array<OwnedBuffer, slot_count> slots_{};
};

template <class T>
class ReadbackSlotRing {
public:
    static constexpr std::size_t slot_count = 4u;

    struct Slot {
        OwnedBuffer buffer;

        rhi::FencePoint fence{};

        std::uint64_t frame_id = 0u;

        std::uint64_t epoch = 0u;

        std::uint64_t tag = 0u;

        bool pending = false;
    };

    Result<Slot*> acquire_write(rhi::Device& device, std::size_t slot_index) {
        Slot& s = slots_[slot_index % slot_count];
        if (!s.buffer.buffer) {
            const auto& core = rhi::BackendAccess::core(device);
            nri::BufferDesc desc{};
            desc.size = (sizeof(T) + 255u) & ~std::size_t{255u};
            ATELIER_TRY(
                check_nri(core.CreateCommittedBuffer(rhi::BackendAccess::nri_device(device),
                                                     nri::MemoryLocation::HOST_READBACK, 0.0f, desc, s.buffer.buffer),
                          rhi::ErrorCode::out_of_memory, "ReadbackSlotRing allocation failed", true));
            s.buffer.capacity = desc.size;
        }
        return &s;
    }

    template <class Fn>
    Result<void> poll_completed(rhi::Device& device, Fn&& on_completed) {
        auto completed = rhi::BackendAccess::timeline(device).completed_point();
        if (!completed)
            return std::unexpected(Error{completed.error(), true});
        std::array<std::size_t, slot_count> ready_indices{};
        std::size_t ready_count = 0u;
        for (std::size_t i = 0u; i < slot_count; ++i) {
            const Slot& s = slots_[i];
            if (!s.pending || !s.buffer.buffer)
                continue;
            bool done = true;
            for (std::size_t q = 0; q < 3u; ++q)
                done &= (completed->values[q] >= s.fence.values[q]);
            if (done)
                ready_indices[ready_count++] = i;
        }
        std::sort(ready_indices.begin(), ready_indices.begin() + ready_count,
                  [&](std::size_t a, std::size_t b) { return slots_[a].epoch < slots_[b].epoch; });
        const auto& core = rhi::BackendAccess::core(device);
        for (std::size_t r = 0u; r < ready_count; ++r) {
            Slot& s = slots_[ready_indices[r]];
            const void* ptr = core.MapBuffer(*s.buffer.buffer, 0u, sizeof(T));
            if (ptr) {
                T copy{};
                std::memcpy(&copy, ptr, sizeof(T));
                core.UnmapBuffer(*s.buffer.buffer);
                on_completed(copy, s.frame_id, s.epoch, s.tag);
            }
            s.pending = false;
        }
        return {};
    }

    void retire_all(rhi::Device& device) noexcept {
        for (Slot& s : slots_) {
            s.buffer.retire(device);
            s.pending = false;
        }
    }

private:
    std::array<Slot, slot_count> slots_{};
};

// Batched HOST_UPLOAD -> DEVICE uploader tracked strictly on `QueueTimeline` (never calls NRI `helper.UploadData`,
// never swallows timeouts/DEVICE_LOST, and performs zero redundant intermediate CPU vector copies!).
class UploadBatch {
public:
    explicit UploadBatch(rhi::Device& device, bool terminal_errors = false) noexcept
        : device_(device), terminal_(terminal_errors) {
        const auto& core = rhi::BackendAccess::core(device);
        const nri::DeviceDesc& dd = core.GetDeviceDesc(rhi::BackendAccess::nri_device(device));
        row_align_ = std::max<std::uint32_t>(256u, dd.memoryAlignment.uploadBufferTextureRow);
        slice_align_ = std::max<std::uint32_t>(512u, dd.memoryAlignment.uploadBufferTextureSlice);
    }

    Result<OwnedBuffer> stage_buffer(const void* data, std::uint64_t byte_size, std::uint32_t stride, bool byte_address,
                                     nri::BufferUsageBits extra_usage, const char* debug_name) {
        if (!byte_address && stride == 0u)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "Structured GPU buffer requires non-zero stride", terminal_));
        if (byte_size > 0u && !data)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "Non-empty buffer upload has null data pointer", terminal_));

        const std::uint64_t align_unit = byte_address ? 4u : static_cast<std::uint64_t>(stride);
        const std::uint64_t min_cap = std::max<std::uint64_t>(16u, align_unit);
        OwnedBuffer out{};
        out.capacity = align_up_bytes(std::max<std::uint64_t>(min_cap, byte_size), align_unit);

        bool ok = false;
        struct Guard {
            OwnedBuffer* b;

            rhi::Device* d;

            bool* ok;

            ~Guard() {
                if (!*ok)
                    b->retire(*d);
            }
        } guard{&out, &device_, &ok};

        const auto& core = rhi::BackendAccess::core(device_);
        nri::Device& nri_dev = rhi::BackendAccess::nri_device(device_);

        nri::BufferDesc desc{};
        desc.size = out.capacity;
        desc.structureStride = byte_address ? 0u : stride;
        desc.byteAddress = byte_address;
        desc.usage = nri::BufferUsageBits::SHADER_RESOURCE | extra_usage;
        ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, desc, out.buffer),
                              rhi::ErrorCode::out_of_memory, "Immutable GPU buffer creation failed", terminal_));
        if (debug_name)
            core.SetDebugName(out.buffer, debug_name);

        nri::BufferViewDesc v_desc{};
        v_desc.buffer = out.buffer;
        v_desc.type = byte_address ? nri::BufferView::BYTE_ADDRESS_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
        v_desc.offset = 0u;
        v_desc.size = nri::WHOLE_SIZE;
        v_desc.structureStride = desc.structureStride;
        ATELIER_TRY(check_nri(core.CreateBufferView(v_desc, out.srv), rhi::ErrorCode::creation_failed,
                              "Immutable buffer SRV creation failed", terminal_));
        auto bindless = rhi::BackendAccess::bindless(device_).allocate_resource(out.srv);
        if (!bindless)
            return std::unexpected(Error{bindless.error(), terminal_});
        out.srv_bindless = *bindless;

        nri::AccessBits after_access = nri::AccessBits::SHADER_RESOURCE;
        nri::StageBits after_stages = nri::StageBits::ALL_SHADERS;
        if ((extra_usage & nri::BufferUsageBits::INDEX) != 0) {
            after_access = after_access | nri::AccessBits::INDEX_BUFFER;
            after_stages = nri::StageBits::ALL;
        }
        if ((extra_usage & nri::BufferUsageBits::VERTEX) != 0) {
            after_access = after_access | nri::AccessBits::VERTEX_BUFFER;
            after_stages = nri::StageBits::ALL;
        }
        if ((extra_usage & nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT) != 0) {
            after_stages = nri::StageBits::ALL;
        }
        out.state = {after_access, after_stages};

        staging_bytes_ = align_up_bytes(staging_bytes_, 256u);
        buf_jobs_.add(BufferJob{
            .dst = out.buffer,
            .src_data = static_cast<const std::uint8_t*>(data),
            .src_bytes = byte_size,
            .dst_bytes = out.capacity,
            .staging_offset = staging_bytes_,
            .after_access = after_access,
            .after_stages = after_stages,
        });
        staging_bytes_ += out.capacity;

        ok = true;
        return out;
    }

    Result<OwnedTexture> stage_texture_2d(nri::Format fmt, std::uint32_t width, std::uint32_t height,
                                          const void* src_pixels, std::uint32_t src_row_pitch, const char* debug_name) {
        if (width == 0u || height == 0u || width > 16384u || height > 16384u || !src_pixels)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "Invalid 2D texture dimensions or null pixels", terminal_));
        const nri::FormatProps* props = nri::nriGetFormatProps(fmt);
        if (!props || props->stride == 0u)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "Unsupported NRI format for 2D texture upload", terminal_));
        const std::uint32_t bw = std::max<std::uint32_t>(1u, props->blockWidth);
        const std::uint32_t bh = std::max<std::uint32_t>(1u, props->blockHeight);
        const std::uint32_t blocks_x = (width + bw - 1u) / bw;
        const std::uint32_t blocks_y = (height + bh - 1u) / bh;
        const std::uint32_t row_bytes = blocks_x * props->stride;
        if (src_row_pitch < row_bytes)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "2D texture source row_pitch smaller than row bytes", terminal_));

        MipSource mip{
            .data = static_cast<const std::uint8_t*>(src_pixels),
            .src_row_pitch = src_row_pitch,
            .row_bytes = row_bytes,
            .num_rows = blocks_y,
        };
        return stage_texture_mips(fmt, width, height, Span{&mip, 1u}, debug_name);
    }

    Result<OwnedTexture> stage_atx_texture(assets::AtxTextureRef atx_ref, const char* debug_name) {
        if (!atx_ref) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Null AtxTexture for GPU upload", terminal_));
        }
        const assets::AtxTexture& atx = *atx_ref;
        if (atx.width == 0u || atx.height == 0u || atx.width > 16384u || atx.height > 16384u || atx.mips.empty() ||
            atx.payload.empty()) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Invalid or empty AtxTexture for GPU upload", terminal_));
        }
        const nri::Format fmt = to_nri_format(atx.format);
        const nri::FormatProps* props = nri::nriGetFormatProps(fmt);
        if (!props || props->stride == 0u)
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Unsupported AtxTexture format", terminal_));
        const std::uint32_t bw = std::max<std::uint32_t>(1u, props->blockWidth);
        const std::uint32_t bh = std::max<std::uint32_t>(1u, props->blockHeight);
        if (atx.mips.size() > calc_mip_count(atx.width, atx.height) ||
            (bw > 1u && ((atx.width % bw) != 0u || (atx.height % bh) != 0u))) {
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              "AtxTexture mip chain or BC block alignment invalid", terminal_));
        }

        Array<MipSource> mips;
        mips.reserve(atx.mips.size());
        for (std::uint32_t m = 0u; m < static_cast<std::uint32_t>(atx.mips.size()); ++m) {
            const assets::AtxMipSlice& s = atx.mips[m];
            const std::uint32_t mw = std::max(1u, atx.width >> m);
            const std::uint32_t mh = std::max(1u, atx.height >> m);
            const std::uint32_t bx = (mw + bw - 1u) / bw;
            const std::uint32_t by = (mh + bh - 1u) / bh;
            const std::uint32_t row_bytes = bx * props->stride;
            if (by == 0u || s.row_pitch < row_bytes || s.byte_offset > atx.payload.size())
                return std::unexpected(
                    make_error(rhi::ErrorCode::invalid_argument, "AtxTexture mip row_pitch invalid", terminal_));
            const std::uint64_t avail = atx.payload.size() - s.byte_offset;
            const std::uint64_t min_span = std::uint64_t(by - 1u) * s.row_pitch + std::uint64_t(row_bytes);
            const std::uint64_t padded_span = std::uint64_t(by) * s.row_pitch;
            if (min_span > avail)
                return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                                  "AtxTexture mip slice exceeds payload bounds", terminal_));
            mips.add(MipSource{
                .data = atx.payload.data() + s.byte_offset,
                .src_row_pitch = s.row_pitch,
                .row_bytes = row_bytes,
                .num_rows = by,
                .pre_padded = (padded_span <= avail),
            });
        }
        return stage_texture_mips(fmt, atx.width, atx.height, mips, debug_name, std::move(atx_ref));
    }

    Result<void> execute() {
        if (buf_jobs_.empty() && tex_jobs_.empty())
            return {};

        const auto& core = rhi::BackendAccess::core(device_);
        nri::Device& nri_dev = rhi::BackendAccess::nri_device(device_);
        auto& timeline = rhi::BackendAccess::timeline(device_);

        nri::Buffer* staging = nullptr;
        nri::CommandAllocator* cmd_alloc = nullptr;
        nri::CommandBuffer* cmd = nullptr;
        bool cmd_open = false;
        struct ExecGuard {
            const nri::CoreInterface* core;

            nri::Buffer** staging;

            nri::CommandAllocator** alloc;

            nri::CommandBuffer** cmd;

            bool* open;

            ~ExecGuard() {
                if (*cmd) {
                    if (*open)
                        static_cast<void>(core->EndCommandBuffer(**cmd));
                    core->DestroyCommandBuffer(*cmd);
                }
                if (*alloc)
                    core->DestroyCommandAllocator(*alloc);
                if (*staging)
                    core->DestroyBuffer(*staging);
            }
        } guard{&core, &staging, &cmd_alloc, &cmd, &cmd_open};

        nri::BufferDesc s_desc{};
        s_desc.size = std::max<std::uint64_t>(256u, staging_bytes_);
        ATELIER_TRY(
            check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_UPLOAD, 0.0f, s_desc, staging),
                      rhi::ErrorCode::out_of_memory, "UploadBatch staging buffer allocation failed", terminal_));

        auto* mapped = static_cast<std::uint8_t*>(core.MapBuffer(*staging, 0u, s_desc.size));
        if (!mapped)
            return std::unexpected(
                make_error(rhi::ErrorCode::out_of_memory, "UploadBatch MapBuffer failed", terminal_));

        for (const BufferJob& bj : buf_jobs_) {
            std::uint8_t* dst = mapped + bj.staging_offset;
            if (bj.src_data && bj.src_bytes > 0u)
                std::memcpy(dst, bj.src_data, static_cast<std::size_t>(bj.src_bytes));
            if (bj.dst_bytes > bj.src_bytes)
                std::memset(dst + bj.src_bytes, 0, static_cast<std::size_t>(bj.dst_bytes - bj.src_bytes));
        }
        for (const TextureJob& tj : tex_jobs_) {
            for (const StagedMip& m : tj.mips) {
                std::uint8_t* dst_base = mapped + m.layout.offset;
                if (m.pre_padded && m.src_row_pitch == m.layout.rowPitch) {
                    std::memcpy(dst_base, m.src_data, std::size_t(m.layout.rowPitch) * m.num_rows);
                } else {
                    for (std::uint32_t r = 0u; r < m.num_rows; ++r) {
                        std::uint8_t* dst_row = dst_base + std::size_t(r) * m.layout.rowPitch;
                        std::memcpy(dst_row, m.src_data + std::size_t(r) * m.src_row_pitch, m.row_bytes);
                        if (m.layout.rowPitch > m.row_bytes)
                            std::memset(dst_row + m.row_bytes, 0, m.layout.rowPitch - m.row_bytes);
                    }
                }
            }
        }
        core.UnmapBuffer(*staging);

        // Route pure buffer uploads through the hardware Copy queue; texture uploads use Graphics for Layout::SHADER_RESOURCE transitions.
        const rhi::QueueKind submit_q =
            tex_jobs_.empty() ? timeline.resolve_kind(rhi::QueueKind::copy) : rhi::QueueKind::graphics;
        nri::Queue* hw_q = timeline.queue(submit_q);
        ATELIER_TRY(check_nri(core.CreateCommandAllocator(*hw_q, cmd_alloc), rhi::ErrorCode::creation_failed,
                              "UploadBatch CreateCommandAllocator failed", terminal_));
        ATELIER_TRY(check_nri(core.CreateCommandBuffer(*cmd_alloc, cmd), rhi::ErrorCode::creation_failed,
                              "UploadBatch CreateCommandBuffer failed", terminal_));
        ATELIER_TRY(check_nri(core.BeginCommandBuffer(*cmd, nullptr), rhi::ErrorCode::execution_failed,
                              "UploadBatch BeginCommandBuffer failed", terminal_));
        cmd_open = true;

        Array<nri::TextureBarrierDesc> pre_tex;
        pre_tex.reserve(tex_jobs_.size());
        for (const TextureJob& tj : tex_jobs_) {
            nri::TextureBarrierDesc tb{};
            tb.texture = tj.dst;
            tb.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
            tb.after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
            tb.mipNum = nri::REMAINING;
            tb.layerNum = 1u;
            tb.planes = nri::PlaneBits::COLOR;
            pre_tex.add(tb);
        }
        if (!pre_tex.empty()) {
            nri::BarrierDesc bd{};
            bd.textures = pre_tex.data();
            bd.textureNum = static_cast<std::uint32_t>(pre_tex.size());
            core.CmdBarrier(*cmd, bd);
        }

        for (const BufferJob& bj : buf_jobs_) {
            core.CmdCopyBuffer(*cmd, *bj.dst, 0u, *staging, bj.staging_offset, bj.dst_bytes);
        }
        for (const TextureJob& tj : tex_jobs_) {
            for (std::uint32_t m = 0u; m < static_cast<std::uint32_t>(tj.mips.size()); ++m) {
                const StagedMip& sm = tj.mips[m];
                nri::TextureRegionDesc reg{0u,
                                           0u,
                                           0u,
                                           nri::WHOLE_SIZE,
                                           nri::WHOLE_SIZE,
                                           nri::WHOLE_SIZE,
                                           static_cast<nri::Dim_t>(m),
                                           0u,
                                           nri::PlaneBits::COLOR};
                core.CmdUploadBufferToTexture(*cmd, *tj.dst, reg, *staging, sm.layout);
            }
        }

        Array<nri::BufferBarrierDesc> post_buf;
        if (submit_q == rhi::QueueKind::graphics) {
            post_buf.reserve(buf_jobs_.size());
            for (const BufferJob& bj : buf_jobs_) {
                nri::BufferBarrierDesc bb{};
                bb.buffer = bj.dst;
                bb.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
                bb.after = {bj.after_access, bj.after_stages};
                post_buf.add(bb);
            }
        }
        Array<nri::TextureBarrierDesc> post_tex;
        post_tex.reserve(tex_jobs_.size());
        for (const TextureJob& tj : tex_jobs_) {
            nri::TextureBarrierDesc tb{};
            tb.texture = tj.dst;
            tb.before = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
            tb.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS};
            tb.mipNum = nri::REMAINING;
            tb.layerNum = 1u;
            tb.planes = nri::PlaneBits::COLOR;
            post_tex.add(tb);
        }
        if (!post_buf.empty() || !post_tex.empty()) {
            nri::BarrierDesc bd{};
            bd.buffers = post_buf.empty() ? nullptr : post_buf.data();
            bd.bufferNum = static_cast<std::uint32_t>(post_buf.size());
            bd.textures = post_tex.empty() ? nullptr : post_tex.data();
            bd.textureNum = static_cast<std::uint32_t>(post_tex.size());
            core.CmdBarrier(*cmd, bd);
        }

        ATELIER_TRY(check_nri(core.EndCommandBuffer(*cmd), rhi::ErrorCode::execution_failed,
                              "UploadBatch EndCommandBuffer failed", terminal_));
        cmd_open = false;

        auto sub = timeline.submit(submit_q, Span{&cmd, 1u});
        if (!sub)
            return std::unexpected(Error{sub.error(), terminal_});

        // Retire staging & command allocator asynchronously on GPU fence completion (zero CPU timeline.wait stall!).
        const rhi::FencePoint fp = timeline.current_point();
        rhi::BackendAccess::reclaimer(device_).retire({
            .fence = fp,
            .buffer = std::exchange(staging, nullptr),
            .command_buffer = std::exchange(cmd, nullptr),
            .command_allocator = std::exchange(cmd_alloc, nullptr),
        });

        // Enqueue GPU-side cross-queue fence waits onto Graphics and Async Compute so subsequent passes see uploaded data without stalling CPU!
        const std::pair<rhi::QueueKind, std::uint64_t> upload_signal{submit_q, *sub};
        if (submit_q != rhi::QueueKind::graphics) {
            auto gfx_wait = timeline.submit(rhi::QueueKind::graphics, {}, Span{&upload_signal, 1u});
            if (!gfx_wait)
                return std::unexpected(Error{gfx_wait.error(), terminal_});
        }
        const rhi::QueueKind comp_q = timeline.resolve_kind(rhi::QueueKind::compute);
        if (comp_q != rhi::QueueKind::graphics && comp_q != submit_q) {
            auto comp_wait = timeline.submit(comp_q, {}, Span{&upload_signal, 1u});
            if (!comp_wait)
                return std::unexpected(Error{comp_wait.error(), terminal_});
        }

        buf_jobs_.clear();
        tex_jobs_.clear();
        staging_bytes_ = 0u;
        return {};
    }

private:
    struct MipSource {
        const std::uint8_t* data = nullptr;

        std::uint32_t src_row_pitch = 0u;

        std::uint32_t row_bytes = 0u;

        std::uint32_t num_rows = 0u;

        bool pre_padded = false;
    };

    struct StagedMip {
        const std::uint8_t* src_data = nullptr;

        std::uint32_t src_row_pitch = 0u;

        std::uint32_t row_bytes = 0u;

        std::uint32_t num_rows = 0u;

        bool pre_padded = false;

        nri::TextureDataLayoutDesc layout{};
    };

    struct BufferJob {
        nri::Buffer* dst = nullptr;

        const std::uint8_t* src_data = nullptr;

        std::uint64_t src_bytes = 0u;

        std::uint64_t dst_bytes = 0u;

        std::uint64_t staging_offset = 0u;

        nri::AccessBits after_access = nri::AccessBits::SHADER_RESOURCE;

        nri::StageBits after_stages = nri::StageBits::ALL_SHADERS;
    };

    struct TextureJob {
        nri::Texture* dst = nullptr;

        assets::AtxTextureRef pinned_atx;

        Array<StagedMip> mips;
    };

    Result<OwnedTexture> stage_texture_mips(nri::Format fmt, std::uint32_t width, std::uint32_t height,
                                            Span<const MipSource> mips, const char* debug_name,
                                            assets::AtxTextureRef pinned_owner = {}) {
        OwnedTexture out{};
        out.width = width;
        out.height = height;
        out.mip_count = static_cast<std::uint32_t>(mips.size());
        out.format = fmt;
        out.byte_size = estimate_texture_bytes(fmt, width, height, 1u, out.mip_count);
        out.state = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS};

        bool ok = false;
        struct Guard {
            OwnedTexture* t;

            rhi::Device* d;

            bool* ok;

            ~Guard() {
                if (!*ok)
                    t->retire(*d);
            }
        } guard{&out, &device_, &ok};

        const auto& core = rhi::BackendAccess::core(device_);
        nri::Device& nri_dev = rhi::BackendAccess::nri_device(device_);

        nri::TextureDesc t_desc{};
        t_desc.type = nri::TextureType::TEXTURE_2D;
        t_desc.usage = nri::TextureUsageBits::SHADER_RESOURCE;
        t_desc.format = fmt;
        t_desc.width = static_cast<nri::Dim_t>(width);
        t_desc.height = static_cast<nri::Dim_t>(height);
        t_desc.depth = 1u;
        t_desc.mipNum = static_cast<nri::Dim_t>(out.mip_count);
        t_desc.layerNum = 1u;
        t_desc.sampleNum = 1u;
        ATELIER_TRY(
            check_nri(core.CreateCommittedTexture(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, t_desc, out.texture),
                      rhi::ErrorCode::out_of_memory, "GPU 2D texture creation failed", terminal_));
        if (debug_name)
            core.SetDebugName(out.texture, debug_name);

        nri::TextureViewDesc v_desc{};
        v_desc.texture = out.texture;
        v_desc.type = nri::TextureView::TEXTURE;
        v_desc.format = fmt;
        v_desc.mipNum = nri::REMAINING;
        v_desc.layerNum = 1u;
        ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, out.srv), rhi::ErrorCode::creation_failed,
                              "GPU 2D texture SRV creation failed", terminal_));
        auto bindless = rhi::BackendAccess::bindless(device_).allocate_resource(out.srv);
        if (!bindless)
            return std::unexpected(Error{bindless.error(), terminal_});
        out.srv_bindless = *bindless;

        TextureJob job{};
        job.dst = out.texture;
        job.pinned_atx = std::move(pinned_owner);
        job.mips.reserve(mips.size());
        for (const MipSource& m : mips) {
            staging_bytes_ = align_up_bytes(staging_bytes_, slice_align_);
            const auto dst_pitch = static_cast<std::uint32_t>(align_up_bytes(m.row_bytes, row_align_));
            const std::uint64_t dst_slice64 = std::uint64_t(dst_pitch) * m.num_rows;
            if (dst_slice64 > 0xFFFFFFFFuLL) {
                return std::unexpected(make_error(rhi::ErrorCode::out_of_memory,
                                                  "Texture mip slice exceeds 4 GiB staging limit", terminal_));
            }
            const auto dst_slice = static_cast<std::uint32_t>(dst_slice64);
            job.mips.add(StagedMip{
                .src_data = m.data,
                .src_row_pitch = m.src_row_pitch,
                .row_bytes = m.row_bytes,
                .num_rows = m.num_rows,
                .pre_padded = m.pre_padded,
                .layout = {staging_bytes_, dst_pitch, dst_slice},
            });
            staging_bytes_ += dst_slice;
        }
        tex_jobs_.add(std::move(job));

        ok = true;
        return out;
    }

    rhi::Device& device_;

    std::uint32_t row_align_ = 256u;

    std::uint32_t slice_align_ = 512u;

    std::uint64_t staging_bytes_ = 0u;

    bool terminal_ = false;

    Array<BufferJob> buf_jobs_;

    Array<TextureJob> tex_jobs_;
};

inline Result<OwnedBuffer> upload_immutable_buffer(rhi::Device& device, const void* data, std::uint64_t byte_size,
                                                   std::uint32_t stride, bool byte_address,
                                                   nri::BufferUsageBits extra_usage, const char* debug_name) {
    UploadBatch batch(device, false);
    ATELIER_TRY_BIND(buf, batch.stage_buffer(data, byte_size, stride, byte_address, extra_usage, debug_name));
    if (auto exec = batch.execute(); !exec) {
        buf->retire(device);
        return std::unexpected(exec.error());
    }
    return std::move(*buf);
}

inline Result<OwnedBuffer> create_rw_buffer(rhi::Device& device, std::uint64_t byte_size, std::uint32_t stride,
                                            bool byte_address, nri::BufferUsageBits extra_usage,
                                            const char* debug_name) {
    if (!byte_address && stride == 0u)
        return std::unexpected(
            make_error(rhi::ErrorCode::invalid_argument, "RW structured buffer requires non-zero stride", true));

    const std::uint64_t align_unit = byte_address ? 16u : static_cast<std::uint64_t>(stride);
    OwnedBuffer out{};
    out.capacity =
        align_up_bytes(std::max<std::uint64_t>(std::max<std::uint64_t>(16u, align_unit), byte_size), align_unit);

    bool ok = false;
    struct Guard {
        OwnedBuffer* b;

        rhi::Device* d;

        bool* ok;

        ~Guard() {
            if (!*ok)
                b->retire(*d);
        }
    } guard{&out, &device, &ok};

    const auto& core = rhi::BackendAccess::core(device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);

    nri::BufferDesc desc{};
    desc.size = out.capacity;
    desc.structureStride = byte_address ? 0u : stride;
    desc.byteAddress = byte_address;
    desc.usage = nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE | extra_usage;
    ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, desc, out.buffer),
                          rhi::ErrorCode::out_of_memory, "RW GPU buffer creation failed", true));
    if (debug_name)
        core.SetDebugName(out.buffer, debug_name);

    nri::BufferViewDesc srv_desc{};
    srv_desc.buffer = out.buffer;
    srv_desc.type = byte_address ? nri::BufferView::BYTE_ADDRESS_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
    srv_desc.size = nri::WHOLE_SIZE;
    srv_desc.structureStride = desc.structureStride;
    ATELIER_TRY(check_nri(core.CreateBufferView(srv_desc, out.srv), rhi::ErrorCode::creation_failed,
                          "RW buffer SRV failed", true));
    auto srv_b = rhi::BackendAccess::bindless(device).allocate_resource(out.srv);
    if (!srv_b)
        return std::unexpected(Error{srv_b.error(), true});
    out.srv_bindless = *srv_b;

    nri::BufferViewDesc uav_desc = srv_desc;
    uav_desc.type =
        byte_address ? nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER : nri::BufferView::STORAGE_STRUCTURED_BUFFER;
    ATELIER_TRY(check_nri(core.CreateBufferView(uav_desc, out.uav), rhi::ErrorCode::creation_failed,
                          "RW buffer UAV failed", true));
    auto uav_b = rhi::BackendAccess::bindless(device).allocate_resource(out.uav);
    if (!uav_b)
        return std::unexpected(Error{uav_b.error(), true});
    out.uav_bindless = *uav_b;

    ok = true;
    return out;
}

inline Result<OwnedTexture> create_rw_texture(rhi::Device& device, nri::TextureType type, nri::Format fmt,
                                              std::uint32_t width, std::uint32_t height, std::uint32_t depth_or_layers,
                                              std::uint32_t mip_count, nri::TextureUsageBits usage, bool per_mip_views,
                                              const char* debug_name) {
    if (width == 0u || height == 0u || depth_or_layers == 0u || mip_count == 0u || width > 16384u || height > 16384u)
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid RW texture dimensions", true));

    OwnedTexture out{};
    out.width = width;
    out.height = height;
    out.depth = depth_or_layers;
    out.mip_count = mip_count;
    out.format = fmt;
    out.byte_size = estimate_texture_bytes(fmt, width, height,
                                           (type == nri::TextureType::TEXTURE_3D) ? depth_or_layers : 1u, mip_count);

    bool ok = false;
    struct Guard {
        OwnedTexture* t;

        rhi::Device* d;

        bool* ok;

        ~Guard() {
            if (!*ok)
                t->retire(*d);
        }
    } guard{&out, &device, &ok};

    const auto& core = rhi::BackendAccess::core(device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);

    nri::TextureDesc desc{};
    desc.type = type;
    desc.usage = usage;
    desc.format = fmt;
    desc.width = static_cast<nri::Dim_t>(width);
    desc.height = static_cast<nri::Dim_t>(height);
    desc.depth = (type == nri::TextureType::TEXTURE_3D) ? static_cast<nri::Dim_t>(depth_or_layers) : 1u;
    desc.mipNum = static_cast<nri::Dim_t>(mip_count);
    desc.layerNum = (type == nri::TextureType::TEXTURE_3D) ? 1u : static_cast<nri::Dim_t>(depth_or_layers);
    desc.sampleNum = 1u;
    if ((usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0)
        desc.optimizedClearValue.depthStencil = {0.0f, 0u};

    ATELIER_TRY(check_nri(core.CreateCommittedTexture(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, desc, out.texture),
                          rhi::ErrorCode::out_of_memory, "GPU texture creation failed", true));
    if (debug_name)
        core.SetDebugName(out.texture, debug_name);

    if ((usage & nri::TextureUsageBits::SHADER_RESOURCE) != 0) {
        nri::TextureViewDesc v_desc{};
        v_desc.texture = out.texture;
        v_desc.type = (type != nri::TextureType::TEXTURE_3D && desc.layerNum > 1u) ? nri::TextureView::TEXTURE_ARRAY
                                                                                   : nri::TextureView::TEXTURE;
        v_desc.format = fmt;
        v_desc.mipNum = nri::REMAINING;
        v_desc.layerNum = desc.layerNum;
        v_desc.sliceNum = nri::REMAINING;
        ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, out.srv), rhi::ErrorCode::creation_failed,
                              "Texture SRV creation failed", true));
        auto b = rhi::BackendAccess::bindless(device).allocate_resource(out.srv);
        if (!b)
            return std::unexpected(Error{b.error(), true});
        out.srv_bindless = *b;

        if (per_mip_views && mip_count > 1u) {
            out.mip_srvs.resize(mip_count, nullptr);
            out.mip_srv_bindless.resize(mip_count, rhi::bindless_invalid_index);
            v_desc.mipNum = 1u;
            for (std::uint32_t m = 0u; m < mip_count; ++m) {
                v_desc.mipOffset = static_cast<nri::Dim_t>(m);
                ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, out.mip_srvs[m]), rhi::ErrorCode::creation_failed,
                                      "Texture mip SRV creation failed", true));
                auto mb = rhi::BackendAccess::bindless(device).allocate_resource(out.mip_srvs[m]);
                if (!mb)
                    return std::unexpected(Error{mb.error(), true});
                out.mip_srv_bindless[m] = *mb;
            }
        }
    }

    if ((usage & nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0) {
        nri::TextureViewDesc u_desc{};
        u_desc.texture = out.texture;
        u_desc.type = (type != nri::TextureType::TEXTURE_3D && desc.layerNum > 1u)
                          ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                          : nri::TextureView::STORAGE_TEXTURE;
        u_desc.format = fmt;
        u_desc.mipOffset = 0u;
        u_desc.mipNum = 1u;
        u_desc.layerNum = desc.layerNum;
        u_desc.sliceNum = nri::REMAINING;
        ATELIER_TRY(check_nri(core.CreateTextureView(u_desc, out.uav), rhi::ErrorCode::creation_failed,
                              "Texture UAV creation failed", true));
        auto b = rhi::BackendAccess::bindless(device).allocate_resource(out.uav);
        if (!b)
            return std::unexpected(Error{b.error(), true});
        out.uav_bindless = *b;

        if (per_mip_views && mip_count > 1u) {
            out.mip_uavs.resize(mip_count, nullptr);
            out.mip_uav_bindless.resize(mip_count, rhi::bindless_invalid_index);
            for (std::uint32_t m = 0u; m < mip_count; ++m) {
                u_desc.mipOffset = static_cast<nri::Dim_t>(m);
                ATELIER_TRY(check_nri(core.CreateTextureView(u_desc, out.mip_uavs[m]), rhi::ErrorCode::creation_failed,
                                      "Texture mip UAV creation failed", true));
                auto mb = rhi::BackendAccess::bindless(device).allocate_resource(out.mip_uavs[m]);
                if (!mb)
                    return std::unexpected(Error{mb.error(), true});
                out.mip_uav_bindless[m] = *mb;
            }
        }
    }

    if ((usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0) {
        nri::TextureViewDesc d_desc{};
        d_desc.texture = out.texture;
        d_desc.type = nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
        d_desc.format = fmt;
        d_desc.mipNum = 1u;
        d_desc.layerNum = 1u;
        ATELIER_TRY(check_nri(core.CreateTextureView(d_desc, out.dsv), rhi::ErrorCode::creation_failed,
                              "Texture DSV creation failed", true));
        d_desc.layout = nri::Layout::DEPTH_STENCIL_READONLY;
        ATELIER_TRY(check_nri(core.CreateTextureView(d_desc, out.dsv_readonly), rhi::ErrorCode::creation_failed,
                              "Texture RO-DSV creation failed", true));
    }

    if ((usage & nri::TextureUsageBits::COLOR_ATTACHMENT) != 0) {
        nri::TextureViewDesc r_desc{};
        r_desc.texture = out.texture;
        r_desc.type = nri::TextureView::COLOR_ATTACHMENT;
        r_desc.format = fmt;
        r_desc.mipNum = 1u;
        r_desc.layerNum = 1u;
        ATELIER_TRY(check_nri(core.CreateTextureView(r_desc, out.rtv), rhi::ErrorCode::creation_failed,
                              "Texture RTV creation failed", true));
    }

    ok = true;
    return out;
}

} // namespace atelier::renderer::detail
