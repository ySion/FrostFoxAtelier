#include "memory/fatal.hpp"
#include "control/control.hpp"
#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
using namespace atelier::control;

namespace json = atelier::json;

namespace platform = atelier::platform;

using namespace std::chrono_literals;

void output(json::Value value) {
    const auto bytes = json::write(value, {.pretty = true, .trailing_lf = true});
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::expected<unsigned, Failure> timeout_ms(const wchar_t* input) {
    while (std::iswspace(*input))
        ++input;
    wchar_t* end{};
    errno = 0;
    const auto value = std::wcstoul(input, &end, 10);
    if (end == input)
        return std::unexpected(Failure("invalid_request", "invalid stoul argument"));
    if (errno == ERANGE)
        return std::unexpected(Failure("invalid_request", "stoul argument out of range"));
    if (*input == L'-' || *end || !value || value > maximum_timeout_ms)
        return std::unexpected(Failure("invalid_request", "Timeout must be between 1 and 600000 ms"));
    return static_cast<unsigned>(value);
}

std::expected<atelier::Array<char>, Failure> read_request(const std::filesystem::path& path) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (path != L"-") {
        const auto native = platform::windows_file_path(path);
        if (!native)
            return std::unexpected(Failure("invalid_request", platform::error_message(native.error())));
        file.open(*native, std::ios::binary);
        if (!file)
            return std::unexpected(Failure("storage_error", "Cannot open request file"));
        input = &file;
    }
    atelier::Array<char> bytes;
    char buffer[4096];
    while (input->read(buffer, sizeof(buffer)) || input->gcount()) {
        const auto count = static_cast<std::size_t>(input->gcount());
        if (count > maximum_message - bytes.size())
            return std::unexpected(Failure("invalid_request", "Request exceeds its size limit"));
        bytes.append(atelier::Span<const char>{buffer, count});
    }
    if (input->bad() || !input->eof())
        return std::unexpected(Failure("storage_error", "Cannot read request input"));
    return bytes;
}

std::expected<int, Failure> call(json::Value instance, const std::filesystem::path& path,
                                 std::chrono::milliseconds timeout) {
    ATELIER_TRY_BIND(bytes, read_request(path));
    ATELIER_TRY_BIND(client, Client::create(json::Document(instance), timeout));
    ATELIER_TRY_BIND(request, parse(atelier::StringView{bytes->data(), bytes->size()}));
    auto response = client->call(std::move(*request));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!response)
            return std::unexpected(std::move(response.error()));
        ATELIER_TRY_BIND(status, response->root().string({"status"}));
        if (*status != "pending") {
            output(response->root());
            return *status == "ok" ? 0 : 1;
        }
        ATELIER_TRY_BIND(job_id, response->root().string({"job_id"}));
        std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + 20ms));
        if (std::chrono::steady_clock::now() >= deadline)
            return std::unexpected(
                Failure("timeout", "Control job timed out; disconnect cancels its temporary override"));
        json::Builder poll;
        poll.set_validated(poll.root(), "op", "job.status");
        poll.set_validated(poll.object_validated(poll.root(), "args"), "job_id", *job_id);
        response = client->call(poll.finish());
    }
}

std::expected<int, Failure> run(int argc, wchar_t** argv) {
    ATELIER_TRY_BIND(directory, default_directory());
    atelier::String instance;
    std::filesystem::path request_path;
    bool list{};
    std::chrono::milliseconds timeout = 120s;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view option(argv[i]);
        if (option == L"--help") {
            std::cout
                << "atelier_control --list [--directory folder]\n"
                   "atelier_control --instance ID --request request.json|- [--directory folder] [--timeout-ms N]\n"
                   "Requests use op/args/request_id and state_token for changes. Call capabilities or editor.inspect first.\n";
            return 0;
        }
        if (option == L"--list") {
            list = true;
            continue;
        }
        if (++i == argc)
            return std::unexpected(Failure("invalid_request", "Missing command-line option value"));
        if (option == L"--directory")
            *directory = argv[i];
        else if (option == L"--instance") {
            auto value = platform::utf16_to_utf8(argv[i]);
            if (!value)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
            instance = std::move(*value);
        } else if (option == L"--request")
            request_path = argv[i];
        else if (option == L"--timeout-ms") {
            ATELIER_TRY_BIND(value, timeout_ms(argv[i]));
            timeout = std::chrono::milliseconds(*value);
        } else
            return std::unexpected(Failure("invalid_request", "Unknown command-line option"));
    }
    ATELIER_TRY_BIND(instances, discover(*directory));
    if (list) {
        json::Builder listed;
        ATELIER_TRY(listed.set(listed.root(), "protocol", protocol_version));
        listed.set_validated(listed.root(), "status", "ok");
        listed.set_validated(listed.root(), "instances", instances->root());
        output(listed.root());
        return 0;
    }
    if (instance.empty() || request_path.empty())
        return std::unexpected(
            Failure("invalid_request", "Specify --instance and --request; use --list to discover editors"));
    for (const auto item : *instances->root().array()) {
        ATELIER_TRY_BIND(id, item.string({"instance_id"}));
        if (*id == instance)
            return call(item, request_path, timeout);
    }
    return std::unexpected(Failure("stale_instance", "The selected editor instance is unavailable"));
}
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    const auto result = run(argc, argv);
    if (result)
        return *result;
    const auto& error = result.error();
    const auto report = atelier::control::failure(error.code, error.message);
    output(report.root());
    return 1;
}
