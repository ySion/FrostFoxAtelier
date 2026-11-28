#pragma once

#include "logging/logging.hpp"
#include "json/json.hpp"

namespace atelier::logging::detail {
struct FormatError {
    ReaderFault fault;

    String message;

    Optional<std::uint32_t> native_code;

    FormatError(ReaderFault kind, String text, Optional<std::uint32_t> native = {})
        : fault(kind), message(std::move(text)), native_code(native) {}

    FormatError(json::Error error);
};

template <class T>
using FormatResult = std::expected<T, FormatError>;

bool valid_utf8(StringView text) noexcept;

FormatResult<String> path_text(const std::filesystem::path& path);

std::uint64_t unix_ns() noexcept;

FormatResult<json::Builder> record_envelope(const Record& identity, const Entry& entry);

FormatResult<Record> read_record(StringView bytes);

FormatResult<json::Builder> run_json(const RunInfo& info);

FormatResult<RunInfo> read_run(StringView bytes);

FormatResult<json::Builder> health_json(const RunInfo& info, const PublishedHealth& health);

FormatResult<PublishedHealth> read_health(StringView bytes, const RunInfo& info);
}
