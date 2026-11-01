#include "rhi/rhi.hpp"
#include "support.hpp"
#include "shaders.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;

using Clock = std::chrono::steady_clock;

template <class T>
T take_value(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void complete(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

Result<Pipeline> wait(Context& context, PipelineJob& job) {
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    for (;;) {
        if (auto taken = context.take(job))
            return std::move(*taken);
        require(Clock::now() < deadline, "pipeline job did not finish within 60 s");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

PipelineDesc compute(const char* source) {
    PipelineDesc desc;
    desc.source.cs = source;
    desc.name = "preparation case";
    return desc;
}

// The kernel adds one to the seed: proves an adopted pipeline binds and runs.
std::uint32_t dispatch(Context& context, const Pipeline& pipeline, std::uint32_t seed) {
    const auto gfx = context.borrow();
    auto counter = gfxCreateBuffer<std::uint32_t>(gfx, 1, &seed);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, 1, nullptr, kGfxCpuAccess_Read);
    require(counter && readback, "dispatch buffers unavailable");
    check(gfxProgramSetParameter(gfx, pipeline.program(), "counter", counter));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
    check(gfxCommandDispatch(gfx, 1, 1, 1));
    check(gfxCommandCopyBuffer(gfx, readback, counter));
    complete(context.finish());
    const auto* value = gfxBufferGetData<std::uint32_t>(gfx, readback);
    require(value != nullptr, "dispatch readback unavailable");
    const auto result = *value;
    check(gfxDestroyBuffer(gfx, counter));
    check(gfxDestroyBuffer(gfx, readback));
    return result;
}

std::vector<std::filesystem::path> entries(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".bin")
            files.push_back(entry.path());
    return files;
}

std::string bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void write(const std::filesystem::path& file, const std::string& content) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << content;
    require(out.good(), "fixture write failed");
}

std::size_t live_objects(ID3D12InfoQueue* queue) {
    std::size_t count = 0;
    std::size_t errors = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check_hr(queue->GetMessage(i, nullptr, &size));
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        check_hr(queue->GetMessage(i, message, &size));
        switch (message->ID) {
        case D3D12_MESSAGE_ID_LIVE_COMMANDALLOCATOR:
        case D3D12_MESSAGE_ID_LIVE_PIPELINESTATE:
        case D3D12_MESSAGE_ID_LIVE_COMMANDLIST12:
        case D3D12_MESSAGE_ID_LIVE_ROOTSIGNATURE:
            ++count;
            break;
        // Expected reports for the fixture's device and non-preparation objects.
        case D3D12_MESSAGE_ID_LIVE_OBJECT_SUMMARY:
        case D3D12_MESSAGE_ID_LIVE_DEVICE:
        case D3D12_MESSAGE_ID_LIVE_COMMANDQUEUE:
        case D3D12_MESSAGE_ID_LIVE_RESOURCE:
        case D3D12_MESSAGE_ID_LIVE_DESCRIPTORHEAP:
        case D3D12_MESSAGE_ID_LIVE_HEAP:
        case D3D12_MESSAGE_ID_LIVE_MONITOREDFENCE:
        case D3D12_MESSAGE_ID_LIVE_COMMANDSIGNATURE:
            break;
        default:
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                ++errors;
                std::fprintf(stderr, "D3D12 [%u]: %s\n", static_cast<unsigned>(message->ID), message->pDescription);
            }
            break;
        }
    }
    require(errors == 0, "D3D12 debug layer reported warnings/errors during live-object inspection");
    queue->ClearStoredMessages();
    return count;
}

void cache() {
    const std::filesystem::path directory = "rhi-shader-cache";
    std::filesystem::remove_all(directory);
    write("rhi-shader-include.hlsli", "uint increment() { return 1; }\n");
    ContextOptions options;
    options.validation = Validation::gpu;
    options.shader_cache = directory;
    auto context = take_value(Context::open(options));
    auto debug = debug_queue(context.borrow());
    const auto run = [&](bool asynchronous) {
        auto pipeline = asynchronous ? [&] {
            auto job = take_value(context.submit(compute(increment_source)));
            return take_value(wait(context, job));
        }()
                                     : take_value(context.prepare(compute(increment_source)));
        require(dispatch(context, pipeline, 7) == 8, "cached kernel produced a wrong value");
    };
    run(false);
    auto files = entries(directory);
    require(files.size() == 1, "a compiled stage was not stored");
    const auto entry = files.front();
    const auto original = bytes(entry);
    const auto written = std::filesystem::last_write_time(entry);
    run(false);
    run(true);
    require(std::filesystem::last_write_time(entry) == written && bytes(entry) == original,
            "a cache hit rewrote its entry");

    write(entry, original.substr(0, original.size() / 2));
    run(false);
    require(bytes(entry) == original, "a truncated entry was not recompiled and replaced");
    auto flipped = original;
    flipped[flipped.size() - 5] = static_cast<char>(flipped[flipped.size() - 5] ^ 0x5a);
    write(entry, flipped);
    run(true);
    require(bytes(entry) == original, "a corrupted entry was not rejected and replaced");

    const char included[] =
        "#include \"rhi-shader-include.hlsli\"\nRWStructuredBuffer<uint> counter;\n[numthreads(1,1,1)] void main() { counter[0] += increment(); }\n";
    auto pipeline = take_value(context.prepare(compute(included)));
    require(dispatch(context, pipeline, 9) == 10, "kernel with an include produced a wrong value");
    require(entries(directory).size() == 1, "a stage that loaded an include file was cached");
    pipeline = {};
    complete(context.close());
    check_debug(debug.Get());
    std::puts(
        "preparation-cache: miss-store=PASS hit-unchanged=PASS truncated/corrupt-replaced=PASS includes-uncached=PASS");
}

void close_while_preparing() {
    ContextOptions options;
    options.validation = Validation::gpu;
    auto context = take_value(Context::open(options));
    ComPtr<ID3D12Device> device = gfxGetDevice(context.borrow());
    auto debug = debug_queue(context.borrow());
    std::vector<std::string> sources;
    for (unsigned i = 0; i < 48; ++i)
        sources.push_back(std::string(increment_source) + "// variant " + std::to_string(i) + "\n");
    std::vector<PipelineJob> jobs;
    for (const auto& source : sources)
        jobs.push_back(take_value(context.submit(compute(source.c_str()))));
    auto first = take_value(wait(context, jobs.front()));
    check_debug(debug.Get());
    ComPtr<ID3D12DebugDevice> device_debug;
    check_hr(device.As(&device_debug));
    check_hr(device_debug->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));
    require(live_objects(debug.Get()) > 0, "live-object detection found nothing while pipelines exist");
    first = {};
    for (std::size_t i = 1; i < jobs.size(); i += 2)
        jobs[i] = {};
    complete(context.close());
    jobs.clear();
    check_hr(device_debug->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));
    const auto leaked = live_objects(debug.Get());
    require(leaked == 0, "close left preparation objects alive");
    std::printf("preparation-close: jobs=%zu leaked=0 PASS\n", sources.size());
}
} // namespace

void preparation_cases() {
    cache();
    close_while_preparing();
}
} // namespace rhi_acceptance
