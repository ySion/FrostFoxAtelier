#pragma once

#include "foundation/function_ref.hpp"
#include "json/json.hpp"
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace atelier::control {
inline constexpr unsigned protocol_version = 1;

inline constexpr std::size_t maximum_message = 4 * 1024 * 1024;

inline constexpr unsigned maximum_timeout_ms = 600000;

struct Failure final {
    std::string code;
    std::string message;

    Failure(std::string category, std::string diagnostic) : code(std::move(category)), message(std::move(diagnostic)) {}

    Failure(const json::Error& error) : Failure("invalid_request", error.describe()) {}
};

struct Peer final {
    std::atomic_bool connected{true};
};

std::expected<std::filesystem::path, Failure> default_directory();

std::expected<std::string, Failure> unique_id();

json::Result<json::Document> failure(std::string_view code, std::string_view message);

json::Result<json::Document> parse(std::string_view);

// Local transport owns no engine objects. poll() is called only by the host's
// owner thread. The I/O worker never invokes the handler or retains its borrows.
class Server final {
public:
    static std::expected<Server, Failure> create(const std::filesystem::path& directory, json::Document build);

    Server(Server&&) noexcept;

    ~Server();

    Server(const Server&) = delete;

    Server& operator=(const Server&) = delete;

    using Handler = FunctionRef<std::expected<json::Document, Failure>(json::Value, const std::shared_ptr<Peer>&)>;

    void poll(Handler);

    void stop() noexcept;

    const json::Document& identity() const noexcept;

    const std::filesystem::path& directory() const noexcept;

private:
    struct Impl;

    explicit Server(std::unique_ptr<Impl>) noexcept;

    std::unique_ptr<Impl> impl_;
};

// One connected client owns its requests and any temporary experiment. Losing
// that connection cancels its experiment at the next host checkpoint.
class Client final {
public:
    static std::expected<Client, Failure> create(const json::Document& instance,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(120));

    Client(Client&&) noexcept;

    ~Client();

    Client(const Client&) = delete;

    Client& operator=(const Client&) = delete;

    std::expected<json::Document, Failure> call(json::Document request);

private:
    struct Impl;

    explicit Client(std::unique_ptr<Impl>) noexcept;

    std::unique_ptr<Impl> impl_;
};

std::expected<json::Document, Failure> discover(const std::filesystem::path& directory);
}
