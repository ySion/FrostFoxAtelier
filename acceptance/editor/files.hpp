#pragma once
#include "platform/error.hpp"
#include "runtime/runtime.hpp"
#include <filesystem>
#include <type_traits>

namespace atelier::acceptance {
inline Error file_error(const std::error_code& error) {
    return {ErrorCode::invalid_storage, platform::error_message(error)};
}

// Call the error_code overload and keep its success value (including false for an absent entry).
template <class F>
auto file_operation(F operation) -> Result<std::invoke_result_t<F, std::error_code&>> {
    using T = std::invoke_result_t<F, std::error_code&>;
    std::error_code error;
    if constexpr (std::is_void_v<T>) {
        operation(error);
        if (error)
            return std::unexpected(file_error(error));
        return {};
    } else {
        auto value = operation(error);
        if (error)
            return std::unexpected(file_error(error));
        return value;
    }
}

inline Result<bool> file_exists(const std::filesystem::path& path) {
    return file_operation([&](auto& error) { return std::filesystem::exists(path, error); });
}

inline Result<bool> remove_file(const std::filesystem::path& path) {
    return file_operation([&](auto& error) { return std::filesystem::remove(path, error); });
}

inline Result<bool> create_directory(const std::filesystem::path& path) {
    return file_operation([&](auto& error) { return std::filesystem::create_directory(path, error); });
}

inline Result<bool> create_directories(const std::filesystem::path& path) {
    return file_operation([&](auto& error) { return std::filesystem::create_directories(path, error); });
}
}
