#include "control/control.hpp"
#include "foundation/array.hpp"
#include "foundation/sha256.hpp"
#include "foundation/span.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace atelier::control {
namespace {
struct Handle {
    HANDLE value{};

    explicit Handle(HANDLE handle = nullptr) : value(handle) {}

    ~Handle() { reset(); }

    void reset() noexcept {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = nullptr;
    }

    Handle(const Handle&) = delete;

    Handle& operator=(const Handle&) = delete;
};

std::expected<void, Failure> check(bool ok, std::string_view operation) {
    if (!ok) {
        const auto error = GetLastError();
        return std::unexpected(
            Failure("transport_error", std::string(operation) + " (Windows " + std::to_string(error) + ")"));
    }
    return {};
}

std::unexpected<Failure> disconnected() {
    return std::unexpected(Failure("disconnected", "Control peer disconnected"));
}

std::string hex(Span<const unsigned char> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto value : bytes) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

std::expected<std::uint64_t, Failure> started(HANDLE process) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    ATELIER_TRY(check(GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE, "Read process identity"));
    return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

bool live(json::Value instance) {
    const auto pid = instance.u32({"pid"});
    const auto identity = instance.u64({"started"});
    if (!pid || !identity)
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, *pid));
    return process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT &&
           started(process.value) == *identity;
}

struct Security {
    PSECURITY_DESCRIPTOR descriptor{};

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

    std::expected<void, Failure> initialize() {
        HANDLE raw{};
        ATELIER_TRY(check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "Read user token"));
        Handle token(raw);
        DWORD bytes{};
        if (!GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return check(false, "Read user identity");
        Array<std::byte> storage(bytes);
        ATELIER_TRY(check(GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes) != FALSE,
                          "Read user identity"));
        LPWSTR sid{};
        ATELIER_TRY(
            check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid) != FALSE,
                  "Format user identity"));
        const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        LocalFree(sid);
        ATELIER_TRY(check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                               &descriptor, nullptr) != FALSE,
                          "Create local control access policy"));
        attributes.lpSecurityDescriptor = descriptor;
        return {};
    }

    ~Security() {
        if (descriptor)
            LocalFree(descriptor);
    }
};

std::expected<void, Failure> wait_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout) {
    HANDLE events[]{stop, operation.hEvent};
    const auto waited = WaitForMultipleObjects(stop ? 2u : 1u, stop ? events : events + 1, FALSE, timeout);
    const auto ready = stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    if (waited != ready) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE); // Retire OVERLAPPED before its stack storage.
        return std::unexpected(
            Failure(waited == WAIT_TIMEOUT ? "timeout" : "disconnected", "Control I/O cancelled or timed out"));
    }
    return {};
}

std::expected<void, Failure> transfer(HANDLE pipe, void* storage, std::size_t bytes, bool write, HANDLE stop,
                                      DWORD timeout) {
    auto* cursor = static_cast<std::byte*>(storage);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (bytes) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ATELIER_TRY(check(event.value != nullptr, "Create pipe completion event"));
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        DWORD transferred{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes, maximum_message));
        const auto immediate = write ? WriteFile(pipe, cursor, count, &transferred, &operation)
                                     : ReadFile(pipe, cursor, count, &transferred, &operation);
        if (!immediate) {
            if (GetLastError() != ERROR_IO_PENDING)
                return disconnected();
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            ATELIER_TRY(wait_io(pipe, operation, stop, static_cast<DWORD>(std::max<std::int64_t>(0, remaining))));
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
                return disconnected();
        }
        if (!transferred)
            return disconnected();
        cursor += transferred;
        bytes -= transferred;
    }
    return {};
}

std::expected<json::Document, Failure> receive(HANDLE pipe, HANDLE stop, DWORD timeout) {
    std::uint32_t bytes{};
    ATELIER_TRY(transfer(pipe, &bytes, sizeof(bytes), false, stop, timeout));
    if (!bytes || bytes > maximum_message)
        return std::unexpected(Failure("invalid_frame", "Control message exceeds its size limit"));
    std::string text(bytes, '\0');
    ATELIER_TRY(transfer(pipe, text.data(), bytes, false, stop, timeout));
    return parse(text);
}

std::expected<void, Failure> send(HANDLE pipe, json::Value value, HANDLE stop, DWORD timeout) {
    auto text = json::write(value);
    if (text.size() > maximum_message)
        return std::unexpected(Failure("result_too_large", "Control response exceeds its size limit"));
    auto bytes = static_cast<std::uint32_t>(text.size());
    ATELIER_TRY(transfer(pipe, &bytes, sizeof(bytes), true, stop, timeout));
    return transfer(pipe, text.data(), text.size(), true, stop, timeout);
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

std::expected<std::string, Failure> unique_id() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value)))
        return std::unexpected(Failure("storage_error", "Cannot allocate control identity"));
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

    Handle pipe;
    Handle stop;
    Handle connect_event;

    std::mutex mutex;

    // One pipe instance, with at most one request waiting for the owner thread.
    std::shared_ptr<Pending> pending;

    std::thread worker;

    std::expected<void, Failure> open(const std::filesystem::path& directory, json::Document build) {
        std::error_code error;
        root = std::filesystem::absolute(directory, error).lexically_normal();
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        ATELIER_TRY_BIND(instance, unique_id());
        const auto pipe_text = "\\\\.\\pipe\\FrostFoxAtelier.Control." + *instance;
        const auto pipe_name = platform::utf8_to_utf16_validated(pipe_text);
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        ATELIER_TRY(check(count && count < executable.size(), "Read executable path"));
        executable.resize(count);
        const auto executable_text = platform::utf16_to_utf8(executable);
        if (!executable_text)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        const auto digest = foundation::sha256_file(executable);
        if (!digest)
            return std::unexpected(Failure("storage_error", digest.error()));
        json::Builder record;
        (void)record.set(record.root(), "protocol", protocol_version);
        record.set_validated(record.root(), "instance_id", *instance);
        (void)record.set(record.root(), "pid", static_cast<std::uint32_t>(GetCurrentProcessId()));
        ATELIER_TRY_BIND(creation, started(GetCurrentProcess()));
        (void)record.set(record.root(), "started", *creation);
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
        Security security;
        ATELIER_TRY(security.initialize());
        stop.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ATELIER_TRY(check(stop.value != nullptr, "Create control lifecycle events"));
        connect_event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ATELIER_TRY(check(connect_event.value != nullptr, "Create connect event"));
        pipe.value = CreateNamedPipeW(pipe_name.c_str(),
                                      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                      65536, 65536, 0, &security.attributes);
        ATELIER_TRY(check(pipe.value != INVALID_HANDLE_VALUE, "Create control pipe"));
        worker = std::thread([this] { serve(); });
        descriptor_path = *native / (*instance + ".json");
        std::ofstream descriptor(descriptor_path, std::ios::binary | std::ios::trunc);
        const auto bytes = json::write(identity.root(), {.pretty = true});
        descriptor.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        descriptor.close();
        if (!descriptor)
            return std::unexpected(Failure("storage_error", "Cannot publish editor instance"));
        return {};
    }

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        if (worker.joinable()) {
            SetEvent(stop.value);
            worker.join();
        }
        pipe.reset();
        if (!descriptor_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(descriptor_path, ignored);
        }
    }

    std::expected<void, Failure> accept() {
        OVERLAPPED operation{};
        operation.hEvent = connect_event.value;
        if (ConnectNamedPipe(pipe.value, &operation))
            return {};
        const auto cause = GetLastError();
        if (cause == ERROR_PIPE_CONNECTED)
            return {};
        if (cause != ERROR_IO_PENDING)
            return std::unexpected(Failure("transport_error", "Cannot accept control client"));
        ATELIER_TRY(wait_io(pipe.value, operation, stop.value, INFINITE));
        DWORD ignored{};
        return check(GetOverlappedResult(pipe.value, &operation, &ignored, FALSE) != FALSE, "Accept control client");
    }

    std::expected<void, Failure> await_response(const Pending& request) {
        HANDLE events[]{stop.value, request.ready.value};
        for (;;) {
            const auto result = WaitForMultipleObjects(2, events, FALSE, 25);
            if (result == WAIT_OBJECT_0 + 1)
                return {};
            if (result != WAIT_TIMEOUT)
                return std::unexpected(Failure("disconnected", "Control server is stopping"));
            if (std::chrono::steady_clock::now() >= request.deadline)
                return std::unexpected(Failure("timeout", "Editor request deadline exceeded"));
            if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, nullptr, nullptr))
                return std::unexpected(Failure("disconnected", "Control client left before execution"));
        }
    }

    void identify(json::Builder& response, json::Value request) const {
        (void)response.set(response.root(), "protocol", protocol_version);
        response.set_validated(response.root(), "instance_id", *identity.root().at("instance_id"));
        if (request.kind() == json::Kind::object)
            if (const auto id = *request.find("request_id"))
                response.set_validated(response.root(), "request_id", *id);
    }

    std::expected<void, Failure> serve_peer(const std::shared_ptr<Peer>& peer) {
        ATELIER_TRY(accept());
        while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
            auto received = receive(pipe.value, stop.value, 120000);
            if (!received) {
                // A complete frame with invalid JSON leaves the next frame boundary intact.
                const auto& error = received.error();
                if (error.code != "invalid_request")
                    return std::unexpected(std::move(received.error()));
                ATELIER_TRY_BIND(rejected, failure(error.code, error.message));
                ATELIER_TRY(send(pipe.value, rejected->root(), stop.value, 5000));
                continue;
            }
            auto request = std::make_shared<Pending>();
            ATELIER_TRY(check(request->ready.value != nullptr, "Create request completion event"));
            request->peer = peer;
            request->request = std::move(*received);
            request->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maximum_timeout_ms);
            {
                std::lock_guard lock(mutex);
                pending = request;
            }
            if (auto ready = await_response(*request); !ready) {
                const auto& error = ready.error();
                if (error.code == "timeout") {
                    // The complete frame has an identity. Report the deadline if the client is still listening.
                    json::Builder reply(*failure(error.code, error.message));
                    identify(reply, request->request.root());
                    (void)send(pipe.value, reply.root(), stop.value, 5000);
                }
                return ready;
            }
            ATELIER_TRY(send(pipe.value, request->response.root(), stop.value, 5000));
        }
        return {};
    }

    void serve() noexcept {
        while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0) {
            auto peer = std::make_shared<Peer>();
            (void)serve_peer(peer);
            peer->connected.store(false);
            {
                std::lock_guard lock(mutex);
                pending.reset();
            }
            DisconnectNamedPipe(pipe.value);
        }
    }
};

std::expected<Server, Failure> Server::create(const std::filesystem::path& directory, json::Document build) {
    auto impl = std::make_unique<Impl>();
    ATELIER_TRY(impl->open(directory, std::move(build)));
    return Server(std::move(impl));
}

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Server::Server(Server&&) noexcept = default;

Server::~Server() = default;

void Server::stop() noexcept {
    impl_->shutdown();
}

const json::Document& Server::identity() const noexcept {
    return impl_->identity;
}

const std::filesystem::path& Server::directory() const noexcept {
    return impl_->root;
}

void Server::poll(Handler handler) {
    std::shared_ptr<Impl::Pending> request;
    {
        std::lock_guard lock(impl_->mutex);
        request = std::move(impl_->pending);
    }
    if (!request)
        return;
    auto result = [&]() -> json::Result<json::Document> {
        if (!request->peer->connected.load() || std::chrono::steady_clock::now() >= request->deadline)
            return failure("cancelled", "Request expired before execution");
        auto handled = handler(request->request.root(), request->peer);
        if (!handled)
            return failure(handled.error().code, handled.error().message);
        return std::move(*handled);
    }();
    // A handler may report an invalid raw key in its diagnostic path. That path
    // cannot itself be emitted as JSON text; keep the protocol response valid.
    if (!result)
        result = failure("internal_error", "Response diagnostic contains invalid UTF-8");
    json::Builder response(*result);
    impl_->identify(response, request->request.root());
    if (json::write(response.root()).size() > maximum_message) {
        const auto token = *response.root().find("state_token");
        const auto rejected = failure("result_too_large", "Result exceeds the 4 MiB protocol limit");
        json::Builder limited(*rejected);
        impl_->identify(limited, request->request.root());
        if (!*limited.root().find("request_id"))
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
            return std::unexpected(Failure("stale_instance", "Editor instance is no longer running"));
        ATELIER_TRY_BIND(pipe_text, instance.root().string({"pipe"}));
        ATELIER_TRY_BIND(id_text, instance.root().string({"instance_id"}));
        const auto name = platform::utf8_to_utf16_validated(*pipe_text);
        const auto instance_id = platform::utf8_to_utf16_validated(*id_text);
        const auto expected = L"\\\\.\\pipe\\FrostFoxAtelier.Control." + instance_id;
        if (name != expected)
            return std::unexpected(Failure("invalid_instance", "Invalid editor endpoint descriptor"));
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<DWORD>(timeout, 5000));
        do {
            pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) {
                ULONG pid{};
                ATELIER_TRY(
                    check(GetNamedPipeServerProcessId(pipe.value, &pid) != FALSE, "Read server process identity"));
                if (pid != *instance.root().u32({"pid"}))
                    return std::unexpected(Failure("stale_instance", "Pipe belongs to another process"));
                return {};
            }
            const auto cause = GetLastError();
            if (cause != ERROR_PIPE_BUSY && cause != ERROR_FILE_NOT_FOUND)
                break;
            WaitNamedPipeW(name.c_str(), 50);
        } while (std::chrono::steady_clock::now() < deadline);
        return std::unexpected(Failure("busy", "Editor control endpoint is busy or unavailable"));
    }

    std::expected<json::Document, Failure> exchange(json::Value outbound) {
        if (!pipe.value)
            return disconnected();
        ATELIER_TRY(send(pipe.value, outbound, nullptr, timeout));
        ATELIER_TRY_BIND(response, receive(pipe.value, nullptr, timeout));
        const auto reply = response->root();
        const auto instance_id = reply.string({"instance_id"});
        const auto request_id = reply.at("request_id");
        const auto version = reply.u32({"protocol"});
        if (!instance_id || *instance_id != *instance.root().string({"instance_id"}) || !request_id ||
            *request_id != *outbound.at("request_id") || version != protocol_version) {
            // A parser rejection contains exactly status and error: the server could not read a request identity.
            if (reply.size() == 2 && reply.string({"status"}) == "error" &&
                reply.string({"error", "code"}) == "invalid_request") {
                if (const auto message = reply.string({"error", "message"}))
                    return std::unexpected(Failure("invalid_request", std::string(*message)));
            }
            return std::unexpected(
                Failure("protocol_mismatch", "Response belongs to a different request or editor instance"));
        }
        return std::move(*response);
    }
};

std::expected<Client, Failure> Client::create(const json::Document& instance, std::chrono::milliseconds timeout) {
    auto impl = std::make_unique<Impl>();
    ATELIER_TRY(impl->open(instance, timeout));
    return Client(std::move(impl));
}

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Client::Client(Client&&) noexcept = default;

Client::~Client() = default;

std::expected<json::Document, Failure> Client::call(json::Document request) {
    if (request.root().kind() != json::Kind::object)
        return std::unexpected(Failure("invalid_request", "Expected a request object"));
    json::Builder outbound(request);
    if (!*outbound.root().find("request_id")) {
        ATELIER_TRY_BIND(id, unique_id());
        outbound.set_validated(outbound.root(), "request_id", *id);
    }
    (void)outbound.set(outbound.root(), "protocol", protocol_version);
    outbound.set_validated(outbound.root(), "instance_id", *impl_->instance.root().at("instance_id"));
    auto response = impl_->exchange(outbound.root());
    // An oversized outbound frame was rejected before any bytes were written.
    if (!response && response.error().code != "result_too_large")
        impl_->pipe.reset();
    return response;
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
