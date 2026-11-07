#include "memory/fatal.hpp"
#include "control/control.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
void output(atelier::json::Value value) {
    const auto bytes = atelier::json::write(value, {.pretty = true, .trailing_lf = true});
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

int reject(std::string_view code, std::string_view message) {
    const auto report = atelier::control::failure(code, message);
    if (report)
        output(report->root());
    else {
        const auto invalid = atelier::control::failure("invalid_request", "Diagnostic contains invalid UTF-8");
        output(invalid->root());
    }
    return 1;
}
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    using namespace atelier::control;
    namespace json = atelier::json;
    try {
        auto storage = default_directory();
        if (!storage)
            return reject(storage.error().code, storage.error().what());
        auto& directory = *storage;
        std::string instance;
        std::filesystem::path request_path;
        bool list{};
        unsigned timeout_ms = 120000;
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
                throw Failure("invalid_request", "Missing command-line option value");
            if (option == L"--directory")
                directory = argv[i];
            else if (option == L"--instance") {
                auto value = atelier::platform::utf16_to_utf8(argv[i]);
                if (!value)
                    return reject("invalid_request", "Invalid UTF-16 text");
                instance = std::move(*value);
            } else if (option == L"--request")
                request_path = argv[i];
            else if (option == L"--timeout-ms") {
                std::size_t end{};
                const std::wstring input(argv[i]);
                const auto value = std::stoul(input, &end);
                if (end != input.size() || !value || value > maximum_timeout_ms)
                    throw Failure("invalid_request", "Timeout must be between 1 and 600000 ms");
                timeout_ms = static_cast<unsigned>(value);
            } else
                throw Failure("invalid_request", "Unknown command-line option");
        }
        auto instances = discover(directory);
        if (!instances)
            return reject(instances.error().code, instances.error().what());
        if (list) {
            json::Builder listed;
            (void)listed.set(listed.root(), "protocol", protocol_version);
            listed.set_validated(listed.root(), "status", "ok");
            listed.set_validated(listed.root(), "instances", instances->root());
            output(listed.root());
            return 0;
        }
        if (instance.empty() || request_path.empty())
            throw Failure("invalid_request", "Specify --instance and --request; use --list to discover editors");
        std::optional<json::Document> found;
        for (const auto item : *instances->root().array()) {
            const auto id = item.string({"instance_id"});
            if (!id)
                return reject("invalid_request", id.error().describe());
            if (*id == instance) {
                found.emplace(item);
                break;
            }
        }
        if (!found)
            throw Failure("stale_instance", "The selected editor instance is unavailable");
        std::ifstream file;
        std::istream* input = &std::cin;
        if (request_path != L"-") {
            const auto native = atelier::platform::windows_file_path(request_path);
            if (!native)
                return reject("invalid_request", atelier::platform::error_message(native.error()));
            file.open(*native, std::ios::binary);
            if (!file)
                return reject("storage_error", "Cannot open request file");
            input = &file;
        }
        std::string bytes;
        char buffer[4096];
        while (input->read(buffer, sizeof(buffer)) || input->gcount()) {
            bytes.append(buffer, static_cast<std::size_t>(input->gcount()));
            if (bytes.size() > maximum_message)
                throw Failure("invalid_request", "Request exceeds its size limit");
        }
        auto client = Client::create(*found, std::chrono::milliseconds(timeout_ms));
        if (!client)
            return reject(client.error().code, client.error().what());
        auto request = parse(bytes);
        if (!request)
            return reject("invalid_request", request.error().describe());
        auto response = client->call(std::move(*request));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (!response)
                return reject(response.error().code, response.error().what());
            const auto status = response->root().string({"status"});
            if (!status)
                return reject("invalid_request", status.error().describe());
            if (*status != "pending") {
                output(response->root());
                return *status == "ok" ? 0 : 1;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw Failure("timeout", "Control job timed out; disconnect cancels its temporary override");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto job_id = response->root().string({"job_id"});
            if (!job_id)
                return reject("invalid_request", job_id.error().describe());
            json::Builder poll;
            poll.set_validated(poll.root(), "op", "job.status");
            const auto args = poll.object_validated(poll.root(), "args");
            poll.set_validated(args, "job_id", *job_id);
            response = client->call(poll.finish());
        }
    } catch (const Failure& error) {
        return reject(error.code, error.what());
    } catch (const std::exception& error) {
        return reject("invalid_request", error.what());
    }
}
