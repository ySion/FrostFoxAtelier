#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
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
struct Peer final { std::atomic_bool connected{true}; };

std::filesystem::path default_directory();
std::string unique_id();
std::string sha256(std::span<const std::byte>);
std::string sha256_file(const std::filesystem::path&);
std::string utf8(std::wstring_view);
std::wstring wide(std::string_view);
void fields(const Json&, std::initializer_list<std::string_view> allowed);
Json failure(std::string_view code, std::string_view message);
Json parse(std::string_view);

// Local transport owns no engine objects. poll() is called only by the host's
// owner thread. The I/O worker never invokes the handler or retains its borrows.
class Server final {
public:
    Server(const std::filesystem::path& directory, Json build);
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
    std::unique_ptr<Impl> impl_;
};

// One connected client owns its requests and any temporary experiment. Losing
// that connection cancels its experiment at the next host checkpoint.
class Client final {
public:
    Client(const Json& instance, std::chrono::milliseconds timeout = std::chrono::seconds(120));
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Json call(Json request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Json discover(const std::filesystem::path& directory);
}
