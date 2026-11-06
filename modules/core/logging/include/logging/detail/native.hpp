#pragma once

#include "logging/logging.hpp"
#include <quill/Logger.h>
#include <algorithm>
#include <new>
#include <stdexcept>
#include <tuple>

namespace atelier::logging::detail {
struct FrontendOptions : quill::FrontendOptions {
    static constexpr auto queue_type = quill::QueueType::BoundedDropping;

    static constexpr std::size_t initial_queue_capacity = producer_queue_bytes;
};

using Logger = quill::LoggerImpl<FrontendOptions>;

struct Ticket {
    SubmitResult result;

    Logger* logger{};

    std::string envelope;
};

// Only the resident host defines metadata and admission/health ownership.
const quill::MacroMetadata* metadata() noexcept;

void prepare(Ticket& ticket, const Entry& entry, bool message_is_template);

SubmitResult complete(SubmitResult result, SubmitStatus status) noexcept;

SubmitResult internal_failure(SubmitResult result, bool allocation) noexcept;

bool valid_utf8(std::string_view text) noexcept;

quill::LogLevel quill_level(Severity severity) noexcept;

template <class T>
auto argument(const T& value) {
    if constexpr (std::is_array_v<T>) {
        const auto end = std::find(std::begin(value), std::end(value), '\0');
        if (end == std::end(value))
            throw std::invalid_argument{"Unterminated log string array"};
        return std::string_view{value, static_cast<std::size_t>(end - value)};
    } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return std::string_view{value};
    } else
        return value;
}

template <class T>
bool valid_argument(const T& value) noexcept {
    if constexpr (std::is_same_v<T, std::string_view>)
        return valid_utf8(value);
    else
        return true;
}

// Instantiation is permitted only in resident code. No user codecs or objects.
template <class... Args>
SubmitResult enqueue(const Entry& entry, bool message_is_template, const char* format, const Args&... args) noexcept {
    Ticket ticket;
    try {
        if (!(valid_argument(args) && ...))
            return complete({}, SubmitStatus::invalid_record);
        prepare(ticket, entry, message_is_template);
        if (!ticket.logger)
            return ticket.result;
        const auto level = quill_level(entry.severity);
        const char* file = entry.source.file.c_str();
        const char* function = entry.source.function.c_str();
        const char* tags = ticket.envelope.c_str();
        quill::detail::SizeCacheVector cache;
        // Pinned v13.1.0 LoggerImpl header: timestamp, metadata, logger, decoder.
        const auto bytes =
            4 * sizeof(std::uint64_t) + quill::detail::compute_encoded_size_and_cache_string_lengths(
                                            cache, format, file, function, tags, entry.source.line, level, args...);
        if (bytes > maximum_record_bytes || entry.message.size() > maximum_message_bytes)
            return complete(ticket.result, SubmitStatus::record_too_large);
        const bool accepted = ticket.logger->template log_statement_runtime_metadata<false>(
            metadata(), format, file, function, tags, entry.source.line, level, args...);
        return complete(ticket.result, accepted ? SubmitStatus::accepted : SubmitStatus::queue_full);
    } catch (const std::bad_alloc&) {
        return internal_failure(ticket.result, true);
    } catch (...) {
        return internal_failure(ticket.result, false);
    }
}
}
