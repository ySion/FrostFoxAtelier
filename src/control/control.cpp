#include "control/control.hpp"
#include "foundation/sha256.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_set>

namespace atelier::control {
namespace {
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void check(bool ok, std::string_view operation) {
    if (!ok) throw Failure("transport_error", std::string(operation) + " (Windows " + std::to_string(GetLastError()) + ")");
}
std::string hex(std::span<const unsigned char> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (auto value : bytes) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}
std::uint64_t started(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    check(GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE, "Read process identity");
    return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
bool live(const Json& instance) {
    try {
        const auto pid = instance.at("pid").get<DWORD>();
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        return process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT &&
            started(process.value) == instance.at("started").get<std::uint64_t>();
    } catch (...) { return false; }
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    Security() {
        HANDLE raw{}; check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "Read user token");
        Handle token(raw); DWORD bytes{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        check(GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes) != FALSE, "Read user identity");
        LPWSTR sid{};
        check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid) != FALSE, "Format user identity");
        const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")"; LocalFree(sid);
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "Create local control access policy");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
void wait_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout) {
    HANDLE events[]{stop, operation.hEvent};
    const auto waited = WaitForMultipleObjects(stop ? 2u : 1u, stop ? events : events + 1, FALSE, timeout);
    const auto ready = stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    if (waited != ready) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE); // Retire OVERLAPPED before its stack storage.
        throw Failure(waited == WAIT_TIMEOUT ? "timeout" : "disconnected", "Control I/O cancelled or timed out");
    }
}
void transfer(HANDLE pipe, void* storage, std::size_t bytes, bool write, HANDLE stop, DWORD timeout) {
    auto* cursor = static_cast<std::byte*>(storage);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (bytes) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); check(event.value != nullptr, "Create pipe completion event");
        OVERLAPPED operation{}; operation.hEvent = event.value;
        DWORD transferred{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes, maximum_message));
        const auto immediate = write ? WriteFile(pipe, cursor, count, &transferred, &operation) : ReadFile(pipe, cursor, count, &transferred, &operation);
        if (!immediate) {
            if (GetLastError() != ERROR_IO_PENDING) throw Failure("disconnected", "Control peer disconnected");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            wait_io(pipe, operation, stop, static_cast<DWORD>(std::max<std::int64_t>(0, remaining)));
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) throw Failure("disconnected", "Control peer disconnected");
        }
        if (!transferred) throw Failure("disconnected", "Control peer disconnected");
        cursor += transferred; bytes -= transferred;
    }
}
Json receive(HANDLE pipe, HANDLE stop, DWORD timeout) {
    std::uint32_t bytes{}; transfer(pipe, &bytes, sizeof(bytes), false, stop, timeout);
    if (!bytes || bytes > maximum_message) throw Failure("invalid_frame", "Control message exceeds its size limit");
    std::string text(bytes, '\0'); transfer(pipe, text.data(), bytes, false, stop, timeout);
    return parse(text);
}
void send(HANDLE pipe, const Json& value, HANDLE stop, DWORD timeout) {
    auto text = value.dump();
    if (text.size() > maximum_message) throw Failure("result_too_large", "Control response exceeds its size limit");
    auto bytes = static_cast<std::uint32_t>(text.size());
    transfer(pipe, &bytes, sizeof(bytes), true, stop, timeout);
    transfer(pipe, text.data(), text.size(), true, stop, timeout);
}
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw Failure("invalid_request", "Invalid UTF-16 text");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr); return result;
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw Failure("invalid_request", "Invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count); return result;
}
std::filesystem::path default_directory() {
    std::wstring value(32768, L'\0'); const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) throw Failure("storage_error", "LOCALAPPDATA is unavailable");
    value.resize(count); return std::filesystem::path(value) / L"FrostFoxAtelier/Control";
}
std::string unique_id() {
    GUID value{}; if (FAILED(CoCreateGuid(&value))) throw Failure("storage_error", "Cannot allocate control identity");
    return hex({reinterpret_cast<const unsigned char*>(&value), sizeof(value)});
}
std::string sha256(std::span<const std::byte> bytes) try {return foundation::sha256(bytes);}
catch(const std::exception& error) {throw Failure("storage_error",error.what());}
std::string sha256_file(const std::filesystem::path& path) try {return foundation::sha256_file(path);}
catch(const std::exception& error) {throw Failure("storage_error",error.what());}
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw Failure("invalid_request", "Expected an object");
    for (const auto& item : value.items()) if (std::ranges::find(allowed, item.key()) == allowed.end())
        throw Failure("invalid_request", "Unknown field: " + item.key());
}
Json failure(std::string_view code, std::string_view message) { return {{"status", "error"}, {"error", {{"code", code}, {"message", message}}}}; }
Json parse(std::string_view text) {
    if (text.size()>maximum_message) throw Failure("invalid_request", "Message exceeds 4 MiB");
    std::vector<std::unordered_set<std::string>> objects;
    try {
        return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& parsed) {
            using enum Json::parse_event_t;
            if ((event==object_start || event==array_start) && depth>=32) throw Failure("invalid_request","JSON nesting exceeds 32");
            if(event==object_start) objects.emplace_back();
            else if(event==object_end) objects.pop_back();
            else if(event==key && !objects.back().insert(parsed.get<std::string>()).second) throw Failure("invalid_request","Duplicate JSON field");
            return true;
        });
    } catch (const Json::exception& error) {throw Failure("invalid_request",error.what());}
}

struct Server::Impl {
    struct Pending {
        Json request, response;
        std::shared_ptr<Peer> peer;
        Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::chrono::steady_clock::time_point deadline;
    };
    std::filesystem::path root, descriptor_path;
    Json identity;
    std::wstring pipe_name;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex;
    std::deque<std::shared_ptr<Pending>> queue;
    std::thread worker;
    std::string startup_error;
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    explicit Impl(std::filesystem::path directory, Json build) : root(std::filesystem::absolute(directory).lexically_normal()) {
        check(stop.value && ready.value, "Create control lifecycle events");
        const auto instance = unique_id();
        pipe_name = L"\\\\.\\pipe\\FrostFoxAtelier.Control." + wide(instance);
        std::wstring executable(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        check(count && count < executable.size(), "Read executable path"); executable.resize(count);
        identity = {{"protocol", protocol_version}, {"instance_id", instance}, {"pid", GetCurrentProcessId()},
            {"started", started(GetCurrentProcess())}, {"executable", utf8(executable)}, {"executable_sha256", sha256_file(executable)},
            {"pipe", utf8(pipe_name)}, {"build", std::move(build)}};
        std::filesystem::create_directories(platform::windows_file_path(root / "instances"));
        descriptor_path = platform::windows_file_path(root / "instances" / (instance + ".json"));
        worker = std::thread([this] { serve(); });
        try {
            WaitForSingleObject(ready.value, INFINITE);
            if (!startup_error.empty()) throw Failure("transport_error", startup_error);
            std::ofstream descriptor(descriptor_path, std::ios::binary | std::ios::trunc); descriptor << identity.dump(2); descriptor.close();
            if (!descriptor) throw Failure("storage_error", "Cannot publish editor instance");
        } catch (...) {
            SetEvent(stop.value); worker.join();
            std::error_code ignored; std::filesystem::remove(descriptor_path, ignored);
            throw;
        }
    }
    ~Impl() {
        SetEvent(stop.value); if (worker.joinable()) worker.join();
        std::error_code ignored; std::filesystem::remove(descriptor_path, ignored);
    }
    void serve() noexcept {
        bool announced{};
        try {
            Security security;
            while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
                Handle pipe(CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security.attributes));
                check(pipe.value != INVALID_HANDLE_VALUE, "Create control pipe");
                if (!announced) { announced = true; SetEvent(ready.value); }
                auto peer = std::make_shared<Peer>();
                try {
                    Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr)); check(connected.value != nullptr, "Create connect event");
                    OVERLAPPED operation{}; operation.hEvent = connected.value;
                    if (!ConnectNamedPipe(pipe.value, &operation)) {
                        const auto error = GetLastError();
                        if (error == ERROR_IO_PENDING) { wait_io(pipe.value, operation, stop.value, INFINITE); DWORD ignored{}; check(GetOverlappedResult(pipe.value, &operation, &ignored, FALSE) != FALSE, "Accept control client"); }
                        else if (error != ERROR_PIPE_CONNECTED) throw Failure("transport_error", "Cannot accept control client");
                    }
                    while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
                        auto pending = std::make_shared<Pending>(); pending->peer = peer;
                        check(pending->ready.value != nullptr, "Create request completion event");
                        try { pending->request = receive(pipe.value, stop.value, 120000); }
                        catch (const Failure& error) {
                            if(error.code!="invalid_request") throw;
                            send(pipe.value,failure(error.code,error.what()),stop.value,5000); continue;
                        }
                        pending->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maximum_timeout_ms);
                        {
                            std::lock_guard lock(mutex);
                            std::erase_if(queue,[](const auto& value){return !value->peer->connected.load();});
                            if(queue.size()>=8) throw Failure("busy","Control request queue is full");
                            queue.push_back(pending);
                        }
                        HANDLE events[]{stop.value, pending->ready.value};
                        for(;;) {
                            const auto result=WaitForMultipleObjects(2,events,FALSE,25);
                            if(result==WAIT_OBJECT_0+1) break;
                            if(result!=WAIT_TIMEOUT) throw Failure("disconnected","Control server is stopping");
                            if(std::chrono::steady_clock::now()>=pending->deadline) throw Failure("timeout","Editor request deadline exceeded");
                            DWORD available{};
                            if(!PeekNamedPipe(pipe.value,nullptr,0,nullptr,&available,nullptr)) throw Failure("disconnected","Control client left before execution");
                        }
                        send(pipe.value, pending->response, stop.value, 5000);
                    }
                } catch (const std::exception&) { /* The client receives disconnect/timeout; its experiment observes the lease ending. */ }
                peer->connected.store(false);
                DisconnectNamedPipe(pipe.value);
            }
        } catch (const std::exception& error) { if (!announced) startup_error = error.what(); }
        SetEvent(ready.value);
    }
};
Server::Server(const std::filesystem::path& directory, Json build) : impl_(std::make_unique<Impl>(directory, std::move(build))) {}
Server::~Server() = default;
void Server::stop() noexcept {SetEvent(impl_->stop.value);if(impl_->worker.joinable()) impl_->worker.join();std::error_code ignored;std::filesystem::remove(impl_->descriptor_path,ignored);}
const Json& Server::identity() const noexcept { return impl_->identity; }
const std::filesystem::path& Server::directory() const noexcept { return impl_->root; }
void Server::poll(const Handler& handler) {
    std::shared_ptr<Impl::Pending> request;
    { std::lock_guard lock(impl_->mutex); if (impl_->queue.empty()) return; request = std::move(impl_->queue.front()); impl_->queue.pop_front(); }
    if (!request->peer->connected.load() || std::chrono::steady_clock::now() >= request->deadline)
        request->response = failure("cancelled", "Request expired before execution");
    else try { request->response = handler(request->request, request->peer); }
        catch (const Failure& error) { request->response = failure(error.code, error.what()); }
        catch (const std::exception& error) { request->response = failure("internal_error", error.what()); }
    request->response["protocol"] = protocol_version;
    request->response["instance_id"] = impl_->identity.at("instance_id");
    if (request->request.is_object() && request->request.contains("request_id")) request->response["request_id"] = request->request["request_id"];
    if(request->response.dump().size()>maximum_message) {
        const auto token=request->response.value("state_token",Json{});
        request->response=failure("result_too_large","Result exceeds the 4 MiB protocol limit");
        request->response["protocol"]=protocol_version;request->response["instance_id"]=impl_->identity.at("instance_id");
        request->response["request_id"]=request->request.value("request_id",Json{});request->response["state_token"]=token;
    }
    SetEvent(request->ready.value);
}

struct Client::Impl {
    Json instance;
    Handle pipe;
    DWORD timeout{};
    Impl(Json descriptor, std::chrono::milliseconds wait) : instance(std::move(descriptor)), timeout(static_cast<DWORD>(std::clamp<std::int64_t>(wait.count(), 1, maximum_timeout_ms))) {
        if (!live(instance)) throw Failure("stale_instance", "Editor instance is no longer running");
        const auto name = wide(instance.at("pipe").get<std::string>());
        const auto expected = L"\\\\.\\pipe\\FrostFoxAtelier.Control." + wide(instance.at("instance_id").get<std::string>());
        if (name != expected) throw Failure("invalid_instance", "Invalid editor endpoint descriptor");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<DWORD>(timeout, 5000));
        do {
            pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) {
                ULONG pid{};check(GetNamedPipeServerProcessId(pipe.value,&pid)!=FALSE,"Read server process identity");
                if(pid!=instance.at("pid").get<DWORD>()) throw Failure("stale_instance","Pipe belongs to another process");
                return;
            }
            if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
            WaitNamedPipeW(name.c_str(), 50);
        } while (std::chrono::steady_clock::now() < deadline);
        throw Failure("busy", "Editor control endpoint is busy or unavailable");
    }
};
Client::Client(const Json& instance, std::chrono::milliseconds timeout) : impl_(std::make_unique<Impl>(instance, timeout)) {}
Client::~Client() = default;
Json Client::call(Json request) {
    if (!request.is_object()) throw Failure("invalid_request", "Expected a request object");
    if (!request.contains("request_id")) request["request_id"] = unique_id();
    request["protocol"] = protocol_version; request["instance_id"] = impl_->instance.at("instance_id");
    send(impl_->pipe.value, request, nullptr, impl_->timeout);
    auto response = receive(impl_->pipe.value, nullptr, impl_->timeout);
    if (response.value("instance_id", "") != impl_->instance.at("instance_id").get<std::string>() ||
        response.value("request_id", Json{}) != request["request_id"] || response.value("protocol", 0u) != protocol_version)
        throw Failure("protocol_mismatch", "Response belongs to a different request or editor instance");
    return response;
}
Json discover(const std::filesystem::path& directory) {
    auto result = Json::array(); std::error_code error;
    const auto entries = std::filesystem::directory_iterator(platform::windows_file_path(directory / "instances"), error);
    if (error) return result;
    for (const auto& item : entries) {
        if (item.path().extension() != ".json" || !item.is_regular_file() || item.file_size() > 65536) continue;
        try { std::ifstream stream(item.path(), std::ios::binary); auto value = Json::parse(stream); if (live(value)) result.push_back(std::move(value)); }
        catch (const std::exception&) { /* Stale or interrupted discovery records do not identify a live endpoint. */ }
    }
    return result;
}
}
