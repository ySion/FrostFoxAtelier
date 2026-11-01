#include "native_client.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <stdexcept>

namespace {

using namespace atelier::jit_acceptance;

struct CleanupProbe {
    ProbeDiagnostic& diagnostic;

    ~CleanupProbe() noexcept { ++diagnostic.cleanups; }
};

static_assert(sizeof(ProbeRecord) == 32 && alignof(ProbeRecord) == 32);

static_assert(offsetof(ProbeRecord, total) == 0);

static_assert(offsetof(ProbeRecord, checksum) == 8);

static_assert(offsetof(ProbeRecord, amount) == 16);

static_assert(offsetof(ProbeRecord, count) == 20);

}

std::uint32_t atelier_jit_native_calculate(std::uint32_t fail, std::int32_t value, float fraction, double scale,
                                           std::uint64_t offset, const ProbeRecord* input, ProbeRecord* output,
                                           ProbeDiagnostic* diagnostic) noexcept {
    if (!input || !output || !diagnostic || reinterpret_cast<std::uintptr_t>(input) % alignof(ProbeRecord) != 0 ||
        reinterpret_cast<std::uintptr_t>(output) % alignof(ProbeRecord) != 0)
        return invalid_storage;
    diagnostic->cleanups = 0;
    diagnostic->message[0] = '\0';
    try {
        CleanupProbe cleanup{*diagnostic};
        if (fail)
            throw std::runtime_error("Requested native failure.");
        const ProbeRecord result{input->total + static_cast<double>(value) + static_cast<double>(fraction) * scale,
                                 input->checksum + offset, input->amount + fraction, input->count + 1};
        *output = result;
        return native_success;
    } catch (const std::exception& error) {
        std::snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", error.what());
        return native_failure;
    } catch (...) {
        std::snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", "Unknown native failure.");
        return native_failure;
    }
}

std::uint32_t atelier_jit_native_capture(StackSnapshot* snapshot) noexcept {
    if (!snapshot)
        return 0;
    CONTEXT context{};
    RtlCaptureContext(&context);
    ULONG_PTR stack_low{};
    ULONG_PTR stack_high{};
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    UNWIND_HISTORY_TABLE history{};
    snapshot->count = 0;
    snapshot->storage_address = reinterpret_cast<std::uintptr_t>(snapshot);
    // CaptureStackBackTrace stops at these dynamic frames. Use Windows' actual
    // function-table lookup and unwinder, not a frame-pointer or stack scan.
    while (snapshot->count < 64 && context.Rip != 0 && context.Rsp >= stack_low &&
           context.Rsp <= stack_high - sizeof(DWORD64)) {
        snapshot->frames[snapshot->count++] = reinterpret_cast<void*>(context.Rip);
        const auto previous_stack = context.Rsp;
        DWORD64 image_base{};
        if (auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, &history)) {
            void* handler_data{};
            DWORD64 establisher_frame{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data,
                             &establisher_frame, nullptr);
        } else {
            // Windows x64 leaf functions have only the return address to pop.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (context.Rsp <= previous_stack)
            break;
    }
    return snapshot->count;
}
