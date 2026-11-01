#pragma once

#include "logging/logging.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace atelier::logging::detail {
using Json = nlohmann::json;

struct FormatError final : std::runtime_error {
    ReaderFault fault;

    FormatError(ReaderFault kind, const char* message) : std::runtime_error(message), fault(kind) {}
};

bool valid_utf8(std::string_view text) noexcept;

std::string path_text(const std::filesystem::path& path);

std::filesystem::path text_path(std::string_view text);

std::uint64_t unix_ns() noexcept;

Json parse_json(std::string_view text);

Json entry_json(const Entry& entry, bool omit_message = false);

Entry read_entry(const Json& json);

Json record_envelope(const Record& identity, const Entry& entry);

Record read_record(const Json& json);

Json run_json(const RunInfo& info);

RunInfo read_run(const Json& json);

Json health_json(const RunInfo& info, const PublishedHealth& health);

PublishedHealth read_health(const Json& json, const RunInfo& info);
}
