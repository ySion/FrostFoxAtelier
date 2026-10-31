#include "control/control.hpp"
#include "platform/windows_path.hpp"
#include <fstream>
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    using namespace atelier::control;
    try {
        auto directory = default_directory();
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
            else if (option == L"--instance")
                instance = utf8(argv[i]);
            else if (option == L"--request")
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
        if (list) {
            std::cout << Json{{"protocol", protocol_version}, {"status", "ok"}, {"instances", instances}}.dump(2)
                      << '\n';
            return 0;
        }
        if (instance.empty() || request_path.empty())
            throw Failure("invalid_request", "Specify --instance and --request; use --list to discover editors");
        const auto found =
            std::ranges::find_if(instances, [&](const auto& item) { return item.at("instance_id") == instance; });
        if (found == instances.end())
            throw Failure("stale_instance", "The selected editor instance is unavailable");
        std::ifstream file;
        std::istream* input = &std::cin;
        if (request_path != L"-") {
            file.open(atelier::platform::windows_file_path(request_path), std::ios::binary);
            if (!file)
                throw Failure("storage_error", "Cannot open request file");
            input = &file;
        }
        std::string bytes;
        char buffer[4096];
        while (input->read(buffer, sizeof(buffer)) || input->gcount()) {
            bytes.append(buffer, static_cast<std::size_t>(input->gcount()));
            if (bytes.size() > maximum_message)
                throw Failure("invalid_request", "Request exceeds its size limit");
        }
        Client client(*found, std::chrono::milliseconds(timeout_ms));
        auto request = parse(bytes);
        const auto original_id = request.value("request_id", unique_id());
        request["request_id"] = original_id;
        auto response = client.call(std::move(request));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (response.value("status", "") == "pending") {
            if (std::chrono::steady_clock::now() >= deadline)
                throw Failure("timeout", "Control job timed out; disconnect cancels its temporary override");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto job_id = response.at("job_id").get<std::string>();
            response = client.call({{"op", "job.status"}, {"args", {{"job_id", job_id}}}});
        }
        std::cout << response.dump(2) << '\n';
        return response.value("status", "error") == "ok" ? 0 : 1;
    } catch (const Failure& error) {
        std::cout << failure(error.code, error.what()).dump(2) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cout << failure("invalid_request", error.what()).dump(2) << '\n';
        return 1;
    }
}
