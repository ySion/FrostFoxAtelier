#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace atelier::logging::detail {
class Handle final {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};
[[noreturn]] inline void io_error(const char* operation, DWORD code = GetLastError()) {
    throw std::system_error{static_cast<int>(code), std::system_category(), operation};
}
inline Handle open_read(const std::filesystem::path& path) {
    return Handle{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
}
inline void write_all(HANDLE handle, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = static_cast<DWORD>((std::min)(bytes.size(), std::size_t{1024 * 1024}));
        DWORD written{};
        if (!WriteFile(handle, bytes.data(), count, &written, nullptr)) io_error("WriteFile");
        if (written == 0) io_error("WriteFile made no progress", ERROR_WRITE_FAULT);
        bytes.remove_prefix(written);
    }
}
inline void atomic_write(const std::filesystem::path& path, std::string_view bytes) {
    auto temporary = path;
    temporary += L".tmp";
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) io_error("Create summary temporary file");
    write_all(file.get(), bytes);
    // Readers may still hold the previous immutable snapshot. POSIX rename
    // replaces its name without leaving that name delete-pending until close.
    const auto destination = std::filesystem::absolute(path).native();
    const auto length = destination.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = static_cast<DWORD>(length);
    std::memcpy(rename->FileName, destination.data(), length);
    if (!SetFileInformationByHandle(file.get(), FileRenameInfoEx, rename, static_cast<DWORD>(storage.size())))
        io_error("Publish summary");
    if (!CloseHandle(file.release())) io_error("Close summary temporary file");
}
inline std::string read_small(const std::filesystem::path& path, std::size_t maximum = 64 * 1024) {
    auto file = open_read(path);
    if (!file) io_error("Open metadata");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) io_error("Metadata size");
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum)
        throw std::runtime_error{"Metadata exceeds limit"};
    std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(file.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr)) io_error("Read metadata");
    if (read != result.size()) throw std::runtime_error{"Metadata changed during read"};
    return result;
}
struct FileIdentity {
    DWORD volume{}, high{}, low{};
    bool operator==(const FileIdentity&) const = default;
};
inline FileIdentity file_identity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) io_error("File identity");
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
}
}
