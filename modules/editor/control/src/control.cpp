#include "control/control.hpp"
#include "foundation/sha256.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
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
#include <vector>

namespace atelier::control {
namespace {
struct Handle {
    HANDLE value{};

    explicit Handle(HANDLE handle = nullptr) : value(handle) {}

    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }

    Handle(const Handle&) = delete;

    Handle& operator=(const Handle&) = delete;
};

void check(bool ok, std::string_view operation) {
    if (!ok)
        throw Failure("transport_error", std::string(operation) + " (Windows " + std::to_string(GetLastError()) + ")");
}

std::string hex(std::span<const unsigned char> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto value : bytes) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

std::uint64_t started(HANDLE process) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    check(GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE, "Read process identity");
    return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

bool live(json::Value instance) {
    const auto pid = instance.u32({"pid"});
    const auto identity = instance.u64({"started"});
    if (!pid || !identity)
        return false;
    try {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, *pid));
        return process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT &&
               started(process.value) == *identity;
    } catch (...) {
        return false;
    }
}

struct Security {
    PSECURITY_DESCRIPTOR descriptor{};

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

    Security() {
        HANDLE raw{};
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "Read user token");
        Handle token(raw);
        DWORD bytes{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        check(GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes) != FALSE,
              "Read user identity");
        LPWSTR sid{};
        check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid) != FALSE,
              "Format user identity");
        const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        LocalFree(sid);
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                   nullptr) != FALSE,
              "Create local control access policy");
        attributes.lpSecurityDescriptor = descriptor;
    }

    ~Security() {
        if (descriptor)
            LocalFree(descriptor);
    }
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
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check(event.value != nullptr, "Create pipe completion event");
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        DWORD transferred{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes, maximum_message));
        const auto immediate = write ? WriteFile(pipe, cursor, count, &transferred, &operation)
                                     : ReadFile(pipe, cursor, count, &transferred, &operation);
        if (!immediate) {
            if (GetLastError() != ERROR_IO_PENDING)
                throw Failure("disconnected", "Control peer disconnected");
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            wait_io(pipe, operation, stop, static_cast<DWORD>(std::max<std::int64_t>(0, remaining)));
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
                throw Failure("disconnected", "Control peer disconnected");
        }
        if (!transferred)
            throw Failure("disconnected", "Control peer disconnected");
        cursor += transferred;
        bytes -= transferred;
    }
}

json::Result<json::Document> receive(HANDLE pipe, HANDLE stop, DWORD timeout) {
    std::uint32_t bytes{};
    transfer(pipe, &bytes, sizeof(bytes), false, stop, timeout);
    if (!bytes || bytes > maximum_message)
        throw Failure("invalid_frame", "Control message exceeds its size limit");
    std::string text(bytes, '\0');
    transfer(pipe, text.data(), bytes, false, stop, timeout);
    return parse(text);
}

void send(HANDLE pipe, json::Value value, HANDLE stop, DWORD timeout) {
    auto text = json::write(value);
    if (text.size() > maximum_message)
        throw Failure("result_too_large", "Control response exceeds its size limit");
    auto bytes = static_cast<std::uint32_t>(text.size());
    transfer(pipe, &bytes, sizeof(bytes), true, stop, timeout);
    transfer(pipe, text.data(), text.size(), true, stop, timeout);
}
}

std::expected<std::filesystem::path, Failure> default_directory() {
    std::wstring value(32768, L'\0');
    const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size())
        return std::unexpected(Failure("storage_error", "LOCALAPPDATA is unavailable"));
    value.resize(count);
    return std::filesystem::path(value) / L"FrostFoxAtelier/Control";
}

std::string unique_id() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value)))
        throw Failure("storage_error", "Cannot allocate control identity");
    return hex({reinterpret_cast<const unsigned char*>(&value), sizeof(value)});
}

json::Result<json::Document> failure(std::string_view code, std::string_view message) {
    json::Builder builder;
    builder.set_validated(builder.root(), "status", "error");
    const auto detail = builder.object_validated(builder.root(), "error");
    ATELIER_TRY(builder.set(detail, "code", code));
    ATELIER_TRY(builder.set(detail, "message", message));
    return builder.finish();
}

json::Result<json::Document> parse(std::string_view text) {
    if (text.size() > maximum_message)
        return std::unexpected(json::Error{json::Fault::range, "Message exceeds 4 MiB", {}, {}});
    return json::Document::parse(text, {32, json::DepthPolicy::containers, true});
}

struct Server::Impl {
    struct Pending {
        json::Document request;

        json::Document response;

        std::shared_ptr<Peer> peer;

        Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

        std::chrono::steady_clock::time_point deadline;
    };

    std::filesystem::path root;

    std::filesystem::path descriptor_path;

    json::Document identity;

    std::wstring pipe_name;

    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    std::mutex mutex;

    std::deque<std::shared_ptr<Pending>> queue;

    std::thread worker;

    std::string startup_error;

    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    std::expected<void, Failure> open(const std::filesystem::path& directory, json::Document build) {
        std::error_code error;
        root = std::filesystem::absolute(directory, error).lexically_normal();
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        check(stop.value && ready.value, "Create control lifecycle events");
        const auto instance = unique_id();
        const auto pipe_text = "\\\\.\\pipe\\FrostFoxAtelier.Control." + instance;
        pipe_name = platform::utf8_to_utf16_validated(pipe_text);
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        check(count && count < executable.size(), "Read executable path");
        executable.resize(count);
        const auto executable_text = platform::utf16_to_utf8(executable);
        if (!executable_text)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        const auto digest = foundation::sha256_file(executable);
        if (!digest)
            return std::unexpected(Failure("storage_error", digest.error()));
        json::Builder record;
        (void)record.set(record.root(), "protocol", protocol_version);
        record.set_validated(record.root(), "instance_id", instance);
        (void)record.set(record.root(), "pid", static_cast<std::uint32_t>(GetCurrentProcessId()));
        (void)record.set(record.root(), "started", started(GetCurrentProcess()));
        record.set_validated(record.root(), "executable", *executable_text);
        record.set_validated(record.root(), "executable_sha256", *digest);
        record.set_validated(record.root(), "pipe", pipe_text);
        record.set_validated(record.root(), "build", build.root());
        identity = record.finish();
        auto native = platform::windows_file_path(root / "instances");
        if (!native)
            return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
        std::filesystem::create_directories(*native, error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        descriptor_path = *native / (instance + ".json");
        worker = std::thread([this] { serve(); });
        WaitForSingleObject(ready.value, INFINITE);
        if (!startup_error.empty())
            throw Failure("transport_error", startup_error);
        std::ofstream descriptor(descriptor_path, std::ios::binary | std::ios::trunc);
        const auto bytes = json::write(identity.root(), {.pretty = true});
        descriptor.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        descriptor.close();
        if (!descriptor)
            return std::unexpected(Failure("storage_error", "Cannot publish editor instance"));
        return {};
    }

    ~Impl() {
        SetEvent(stop.value);
        if (worker.joinable())
            worker.join();
        std::error_code ignored;
        std::filesystem::remove(descriptor_path, ignored);
    }

    void serve() noexcept {
        bool announced{};
        try {
            Security security;
            while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
                Handle pipe(CreateNamedPipeW(
                    pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
                    &security.attributes));
                check(pipe.value != INVALID_HANDLE_VALUE, "Create control pipe");
                if (!announced) {
                    announced = true;
                    SetEvent(ready.value);
                }
                auto peer = std::make_shared<Peer>();
                try {
                    Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    check(connected.value != nullptr, "Create connect event");
                    OVERLAPPED operation{};
                    operation.hEvent = connected.value;
                    if (!ConnectNamedPipe(pipe.value, &operation)) {
                        const auto error = GetLastError();
                        if (error == ERROR_IO_PENDING) {
                            wait_io(pipe.value, operation, stop.value, INFINITE);
                            DWORD ignored{};
                            check(GetOverlappedResult(pipe.value, &operation, &ignored, FALSE) != FALSE,
                                  "Accept control client");
                        } else if (error != ERROR_PIPE_CONNECTED)
                            throw Failure("transport_error", "Cannot accept control client");
                    }
                    while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
                        auto pending = std::make_shared<Pending>();
                        pending->peer = peer;
                        check(pending->ready.value != nullptr, "Create request completion event");
                        auto received = receive(pipe.value, stop.value, 120000);
                        if (!received) {
                            const auto rejected = failure("invalid_request", received.error().describe());
                            send(pipe.value, rejected->root(), stop.value, 5000);
                            continue;
                        }
                        pending->request = std::move(*received);
                        pending->deadline =
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(maximum_timeout_ms);
                        {
                            std::lock_guard lock(mutex);
                            std::erase_if(queue, [](const auto& value) { return !value->peer->connected.load(); });
                            if (queue.size() >= 8)
                                throw Failure("busy", "Control request queue is full");
                            queue.push_back(pending);
                        }
                        HANDLE events[]{stop.value, pending->ready.value};
                        for (;;) {
                            const auto result = WaitForMultipleObjects(2, events, FALSE, 25);
                            if (result == WAIT_OBJECT_0 + 1)
                                break;
                            if (result != WAIT_TIMEOUT)
                                throw Failure("disconnected", "Control server is stopping");
                            if (std::chrono::steady_clock::now() >= pending->deadline)
                                throw Failure("timeout", "Editor request deadline exceeded");
                            DWORD available{};
                            if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr))
                                throw Failure("disconnected", "Control client left before execution");
                        }
                        send(pipe.value, pending->response.root(), stop.value, 5000);
                    }
                } catch (
                    const std::
                        exception&) { /* The client receives disconnect/timeout; its experiment observes the lease ending. */
                }
                peer->connected.store(false);
                DisconnectNamedPipe(pipe.value);
            }
        } catch (const std::exception& error) {
            if (!announced)
                startup_error = error.what();
        }
        SetEvent(ready.value);
    }
};

std::expected<Server, Failure> Server::create(const std::filesystem::path& directory, json::Document build) {
    auto impl = std::make_unique<Impl>();
    auto opened = impl->open(directory, std::move(build));
    if (!opened)
        return std::unexpected(std::move(opened.error()));
    return Server(std::move(impl));
}

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Server::Server(Server&&) noexcept = default;

Server::~Server() = default;

void Server::stop() noexcept {
    SetEvent(impl_->stop.value);
    if (impl_->worker.joinable())
        impl_->worker.join();
    std::error_code ignored;
    std::filesystem::remove(impl_->descriptor_path, ignored);
}

const json::Document& Server::identity() const noexcept {
    return impl_->identity;
}

const std::filesystem::path& Server::directory() const noexcept {
    return impl_->root;
}

void Server::poll(const Handler& handler) {
    std::shared_ptr<Impl::Pending> request;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->queue.empty())
            return;
        request = std::move(impl_->queue.front());
        impl_->queue.pop_front();
    }
    json::Result<json::Document> result;
    if (!request->peer->connected.load() || std::chrono::steady_clock::now() >= request->deadline)
        result = failure("cancelled", "Request expired before execution");
    else
        try {
            result = handler(request->request.root(), request->peer);
        } catch (const Failure& error) {
            result = failure(error.code, error.what());
        } catch (const std::exception& error) {
            result = failure("internal_error", error.what());
        }
    if (!result)
        result = failure("invalid_request", result.error().describe());
    // A handler may report an invalid raw key in its diagnostic path. That path
    // cannot itself be emitted as JSON text; keep the protocol response valid.
    if (!result)
        result = failure("internal_error", "Response diagnostic contains invalid UTF-8");
    json::Builder response(*result);
    (void)response.set(response.root(), "protocol", protocol_version);
    response.set_validated(response.root(), "instance_id", *impl_->identity.root().at("instance_id"));
    std::optional<json::Value> request_id;
    if (request->request.root().kind() == json::Kind::object)
        request_id = *request->request.root().find("request_id");
    if (request_id)
        response.set_validated(response.root(), "request_id", *request_id);
    if (json::write(response.root()).size() > maximum_message) {
        const auto token = *response.root().find("state_token");
        const auto rejected = failure("result_too_large", "Result exceeds the 4 MiB protocol limit");
        json::Builder limited(*rejected);
        (void)limited.set(limited.root(), "protocol", protocol_version);
        limited.set_validated(limited.root(), "instance_id", *impl_->identity.root().at("instance_id"));
        if (request_id)
            limited.set_validated(limited.root(), "request_id", *request_id);
        else
            (void)limited.set_null(limited.root(), "request_id");
        if (token)
            limited.set_validated(limited.root(), "state_token", *token);
        else
            (void)limited.set_null(limited.root(), "state_token");
        response = std::move(limited);
    }
    request->response = response.finish();
    SetEvent(request->ready.value);
}

struct Client::Impl {
    json::Document instance;

    Handle pipe;

    DWORD timeout{};

    std::expected<void, Failure> open(json::Document descriptor, std::chrono::milliseconds wait) {
        instance = std::move(descriptor);
        timeout = static_cast<DWORD>(std::clamp<std::int64_t>(wait.count(), 1, maximum_timeout_ms));
        if (!live(instance.root()))
            throw Failure("stale_instance", "Editor instance is no longer running");
        const auto pipe_text = instance.root().string({"pipe"});
        if (!pipe_text)
            return std::unexpected(Failure("invalid_request", pipe_text.error().describe()));
        const auto id_text = instance.root().string({"instance_id"});
        if (!id_text)
            return std::unexpected(Failure("invalid_request", id_text.error().describe()));
        const auto name = platform::utf8_to_utf16_validated(*pipe_text);
        const auto instance_id = platform::utf8_to_utf16_validated(*id_text);
        const auto expected = L"\\\\.\\pipe\\FrostFoxAtelier.Control." + instance_id;
        if (name != expected)
            throw Failure("invalid_instance", "Invalid editor endpoint descriptor");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<DWORD>(timeout, 5000));
        do {
            pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) {
                ULONG pid{};
                check(GetNamedPipeServerProcessId(pipe.value, &pid) != FALSE, "Read server process identity");
                if (pid != *instance.root().u32({"pid"}))
                    throw Failure("stale_instance", "Pipe belongs to another process");
                return {};
            }
            if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
                break;
            WaitNamedPipeW(name.c_str(), 50);
        } while (std::chrono::steady_clock::now() < deadline);
        throw Failure("busy", "Editor control endpoint is busy or unavailable");
    }
};

std::expected<Client, Failure> Client::create(const json::Document& instance, std::chrono::milliseconds timeout) {
    auto impl = std::make_unique<Impl>();
    auto opened = impl->open(instance, timeout);
    if (!opened)
        return std::unexpected(std::move(opened.error()));
    return Client(std::move(impl));
}

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Client::Client(Client&&) noexcept = default;

Client::~Client() = default;

std::expected<json::Document, Failure> Client::call(json::Document request) {
    if (request.root().kind() != json::Kind::object)
        return std::unexpected(Failure("invalid_request", "Expected a request object"));
    json::Builder outbound(request);
    if (!*outbound.root().find("request_id"))
        outbound.set_validated(outbound.root(), "request_id", unique_id());
    (void)outbound.set(outbound.root(), "protocol", protocol_version);
    outbound.set_validated(outbound.root(), "instance_id", *impl_->instance.root().at("instance_id"));
    send(impl_->pipe.value, outbound.root(), nullptr, impl_->timeout);
    auto response = receive(impl_->pipe.value, nullptr, impl_->timeout);
    if (!response)
        return std::unexpected(Failure("invalid_request", response.error().describe()));
    const auto instance_id = response->root().string({"instance_id"});
    const auto request_id = response->root().at("request_id");
    const auto version = response->root().u32({"protocol"});
    if (!instance_id || *instance_id != *impl_->instance.root().string({"instance_id"}) || !request_id ||
        *request_id != *outbound.root().at("request_id") || !version || *version != protocol_version)
        throw Failure("protocol_mismatch", "Response belongs to a different request or editor instance");
    return std::move(*response);
}

std::expected<json::Document, Failure> discover(const std::filesystem::path& directory) {
    json::Builder result(json::Kind::array);
    const auto native = platform::windows_file_path(directory / "instances");
    if (!native)
        return std::unexpected(Failure("invalid_request", platform::error_message(native.error())));
    std::error_code error;
    auto entry = std::filesystem::directory_iterator(*native, error);
    if (error)
        return result.finish();
    for (; entry != std::filesystem::directory_iterator{}; entry.increment(error)) {
        const auto& item = *entry;
        if (item.path().extension() != ".json")
            continue;
        const auto status = item.status(error);
        if (error && !std::filesystem::status_known(status))
            return std::unexpected(Failure("invalid_request", platform::error_message(error)));
        if (!std::filesystem::is_regular_file(status))
            continue;
        const auto size = item.file_size(error);
        if (error)
            return std::unexpected(Failure("invalid_request", platform::error_message(error)));
        if (size > 65536)
            continue;
        std::ifstream stream(item.path(), std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            stream.peek() != std::char_traits<char>::eof())
            continue;
        // Discovery has no depth limit. Malformed or interrupted records are skipped.
        const auto value = json::Document::parse(
            bytes, {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
        if (value && live(value->root()))
            result.append(result.root(), value->root());
    }
    if (error)
        return std::unexpected(Failure("invalid_request", platform::error_message(error)));
    return result.finish();
}
}
