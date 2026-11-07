#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atelier::control {
using Json = nlohmann::json;

inline constexpr unsigned protocol_version = 1;

inline constexpr std::size_t maximum_message = 4 * 1024 * 1024;

inline constexpr unsigned maximum_timeout_ms = 600000;

struct Failure final : std::runtime_error {
    std::string code;

    Failure(std::string category, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(category)) {}
};

struct Peer final {
    std::atomic_bool connected{true};
};

std::expected<std::filesystem::path, Failure> default_directory();

std::string unique_id();

void fields(const Json&, std::initializer_list<std::string_view> allowed);

Json failure(std::string_view code, std::string_view message);

Json parse(std::string_view);

// Local transport owns no engine objects. poll() is called only by the host's
// owner thread. The I/O worker never invokes the handler or retains its borrows.
class Server final {
public:
    static std::expected<Server, Failure> create(const std::filesystem::path& directory, Json build);

    Server(Server&&) noexcept;

    ~Server();

    Server(const Server&) = delete;

    Server& operator=(const Server&) = delete;

    using Handler = std::function<Json(const Json&, const std::shared_ptr<Peer>&)>;

    void poll(const Handler&);

    void stop() noexcept;

    const Json& identity() const noexcept;

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
    static std::expected<Client, Failure> create(const Json& instance,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(120));

    Client(Client&&) noexcept;

    ~Client();

    Client(const Client&) = delete;

    Client& operator=(const Client&) = delete;

    Json call(Json request);

private:
    struct Impl;

    explicit Client(std::unique_ptr<Impl>) noexcept;

    std::unique_ptr<Impl> impl_;
};

std::expected<Json, Failure> discover(const std::filesystem::path& directory);
}
