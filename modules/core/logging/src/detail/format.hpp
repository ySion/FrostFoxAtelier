#pragma once

#include "logging/logging.hpp"
#include "json/json.hpp"

namespace atelier::logging::detail {
struct FormatError {
    ReaderFault fault;

    std::string message;

    FormatError(ReaderFault kind, std::string text) : fault(kind), message(std::move(text)) {}

    FormatError(json::Error error);
};

template <class T>
using FormatResult = std::expected<T, FormatError>;

bool valid_utf8(std::string_view text) noexcept;

std::string path_text(const std::filesystem::path& path);

std::filesystem::path text_path(std::string_view text);

std::uint64_t unix_ns() noexcept;

FormatResult<json::Builder> record_envelope(const Record& identity, const Entry& entry);

FormatResult<Record> read_record(std::string_view bytes);

FormatResult<json::Builder> run_json(const RunInfo& info);

FormatResult<RunInfo> read_run(std::string_view bytes);

FormatResult<json::Builder> health_json(const RunInfo& info, const PublishedHealth& health);

FormatResult<PublishedHealth> read_health(std::string_view bytes, const RunInfo& info);
}
