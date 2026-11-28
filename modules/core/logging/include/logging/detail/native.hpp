#pragma once

#include "logging/logging.hpp"
#include <quill/Logger.h>
#include <algorithm>
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

    Array<char> envelope;
};

// Only the resident host defines metadata and admission/health ownership.
const quill::MacroMetadata* metadata() noexcept;

void prepare(Ticket& ticket, const Entry& entry, bool message_is_template);

SubmitResult complete(SubmitResult result, SubmitStatus status) noexcept;

bool valid_utf8(StringView text) noexcept;

quill::LogLevel quill_level(Severity severity) noexcept;

template <class T>
auto argument(const T& value) noexcept {
    if constexpr (std::is_array_v<T>) {
        const auto end = std::find(std::begin(value), std::end(value), '\0');
        if (end == std::end(value))
            return Optional<std::string_view>{};
        return Optional{std::string_view{value, static_cast<std::size_t>(end - value)}};
    } else if constexpr (std::is_same_v<T, String>) {
        return Optional{value.view().std_view()};
    } else if constexpr (std::is_same_v<T, StringView>) {
        return Optional{value.std_view()};
    } else
        return Optional{value};
}

template <class T>
using MappedArgument = typename decltype(argument(std::declval<const std::remove_cvref_t<T>&>()))::Value;

template <class T>
bool valid_input_argument(const T& value) noexcept {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, String>) {
        return true;
    } else if constexpr (std::is_same_v<V, StringView>) {
        return valid_utf8(value);
    } else if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
        const auto end = std::find(std::begin(value), std::end(value), '\0');
        return end != std::end(value) && valid_utf8(StringView{value, static_cast<std::size_t>(end - value)});
    } else {
        return true;
    }
}

// Instantiation is permitted only in resident code. No user codecs or objects.
template <class... Args>
SubmitResult enqueue(const Entry& entry, bool message_is_template, const char* format, const Args&... args) noexcept {
    Ticket ticket;
    prepare(ticket, entry, message_is_template);
    if (!ticket.logger)
        return ticket.result;
    const auto level = quill_level(entry.severity);
    const char* file = entry.source.file.c_str();
    const char* function = entry.source.function.c_str();
    const char* tags = ticket.envelope.data();
    quill::detail::SizeCacheVector cache;
    // Pinned v13.1.0 LoggerImpl header: timestamp, metadata, logger, decoder.
    const auto bytes = 4 * sizeof(std::uint64_t) +
                       quill::detail::compute_encoded_size_and_cache_string_lengths(cache, format, file, function, tags,
                                                                                    entry.source.line, level, args...);
    if (bytes > maximum_record_bytes || entry.message.size() > maximum_message_bytes)
        return complete(ticket.result, SubmitStatus::record_too_large);
    const bool accepted = ticket.logger->template log_statement_runtime_metadata<false>(
        metadata(), format, file, function, tags, entry.source.line, level, args...);
    return complete(ticket.result, accepted ? SubmitStatus::accepted : SubmitStatus::queue_full);
}
}
