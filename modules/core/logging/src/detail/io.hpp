#pragma once

#include "foundation/array.hpp"
#include "foundation/inline_array.hpp"
#include "logging/logging.hpp"
#include "platform/error.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

namespace atelier::logging::detail {
class Handle final {
public:
    Handle() noexcept = default;

    explicit Handle(HANDLE value) noexcept : value_(value) {}

    ~Handle() { reset(); }

    Handle(const Handle&) = delete;

    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(other.release()) {}

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    HANDLE get() const noexcept { return value_; }

    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }

    HANDLE release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this)
            CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

inline Error io_failure(const char* operation, DWORD code = GetLastError()) {
    return {ErrorCode::io_failed,
            String::concat(String::validated(operation), ": "_s,
                           platform::error_message({static_cast<int>(code), std::system_category()})),
            code};
}

inline Handle open_read(const std::filesystem::path& path) {
    return Handle{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
}

inline Result<void> write_all(HANDLE handle, StringView bytes) {
    while (!bytes.empty()) {
        const auto count = static_cast<DWORD>((std::min)(bytes.size(), std::size_t{1024 * 1024}));
        DWORD written{};
        if (!WriteFile(handle, bytes.data(), count, &written, nullptr))
            return std::unexpected(io_failure("WriteFile"));
        if (written == 0)
            return std::unexpected(io_failure("WriteFile made no progress", ERROR_WRITE_FAULT));
        bytes.remove_prefix(written);
    }
    return {};
}

inline Result<void> atomic_write(const std::filesystem::path& path, StringView bytes) {
    auto temporary = path;
    temporary += L".tmp";
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file)
        return std::unexpected(io_failure("Create summary temporary file"));
    ATELIER_TRY(write_all(file.get(), bytes));
    // Readers may still hold the previous immutable snapshot. POSIX rename
    // replaces its name without leaving that name delete-pending until close.
    std::error_code error;
    const auto destination = std::filesystem::absolute(path, error).native();
    if (error)
        return std::unexpected(io_failure("Resolve summary destination", static_cast<DWORD>(error.value())));
    const auto length = destination.size() * sizeof(wchar_t);
    const auto bytes_needed = offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t);
    InlineArray<std::uint64_t, 80> storage((bytes_needed + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = static_cast<DWORD>(length);
    std::memcpy(rename->FileName, destination.data(), length);
    if (!SetFileInformationByHandle(file.get(), FileRenameInfoEx, rename, static_cast<DWORD>(bytes_needed)))
        return std::unexpected(io_failure("Publish summary"));
    if (!CloseHandle(file.release()))
        return std::unexpected(io_failure("Close summary temporary file"));
    return {};
}

inline Result<Array<char>> read_small(const std::filesystem::path& path, std::size_t maximum = 64 * 1024) {
    auto file = open_read(path);
    if (!file)
        return std::unexpected(io_failure("Open metadata"));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size))
        return std::unexpected(io_failure("Metadata size"));
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum)
        return std::unexpected(Error{ErrorCode::invalid_manifest, "Metadata exceeds limit"_s});
    Array<char> result;
    result.resize_uninitialized(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    if (!ReadFile(file.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr))
        return std::unexpected(io_failure("Read metadata"));
    if (read != result.size())
        return std::unexpected(Error{ErrorCode::invalid_manifest, "Metadata changed during read"_s});
    return result;
}

struct FileIdentity {
    DWORD volume{};

    DWORD high{};

    DWORD low{};

    bool operator==(const FileIdentity&) const = default;
};

inline Result<FileIdentity> file_identity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        return std::unexpected(io_failure("File identity"));
    return FileIdentity{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
}
}
