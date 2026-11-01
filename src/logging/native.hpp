#pragma once

#include "logging/logging.hpp"
#include "logging/detail.hpp"
#include <quill/bundled/fmt/base.h>
#include <concepts>
#include <span>
#include <type_traits>

namespace atelier::logging::native {

// This convenience header has a real Quill compile dependency. Only permanently
// resident code linked into the host binary may instantiate it: the header-only
// backend registry/TLS must not be duplicated in a DLL. DLL/JIT producers call
// the host's submit() entry instead of linking another atelier_logging instance.
#ifdef NDEBUG
inline constexpr Severity minimum_severity = Severity::info;
#else
inline constexpr Severity minimum_severity = Severity::trace;
#endif

template <class T>
concept Argument =
    (std::is_arithmetic_v<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, wchar_t> &&
     !std::same_as<std::remove_cvref_t<T>, char8_t> && !std::same_as<std::remove_cvref_t<T>, char16_t> &&
     !std::same_as<std::remove_cvref_t<T>, char32_t>) ||
    std::same_as<std::remove_cvref_t<T>, std::string> || std::same_as<std::remove_cvref_t<T>, std::string_view> ||
    (std::is_array_v<std::remove_reference_t<T>> &&
     std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>);

struct Context {
    std::span<const Field> fields{};

    std::optional<Repeat> repeat{};
};

// Positional, compile-time-checked format; no named/runtime formats or user codecs.
// Copy strings, fields and repeat into the queue before return; format the message
// on the backend. Direct calls evaluate arguments first; use ATELIER_LOG[_WITH]
// for compile-time/runtime filtering before argument evaluation.
template <Severity Level, Argument... Args>
    requires(Level != Severity::off && sizeof...(Args) <= 16)
[[nodiscard]] SubmitResult write(std::string_view category, std::string_view event_code, const Context& context,
                                 std::source_location source, fmtquill::format_string<Args...> format,
                                 Args&&... args) noexcept {
    if constexpr (Level < minimum_severity)
        return {SubmitStatus::filtered};
    if (!current_run())
        return {SubmitStatus::no_active_run};
    if (!enabled(Level, category))
        return {SubmitStatus::filtered};
    try {
        const auto spelling = format.get();
        Entry entry;
        entry.severity = Level;
        entry.category = category;
        entry.event_code = event_code;
        entry.message.assign(spelling.data(), spelling.size());
        if (entry.message.find('\0') != std::string::npos)
            return detail::complete({}, SubmitStatus::invalid_record);
        entry.source = native_source(source);
        entry.fields.assign(context.fields.begin(), context.fields.end());
        entry.repeat = context.repeat;
        const auto values = std::tuple{detail::argument(args)...};
        return std::apply(
            [&](const auto&... value) { return detail::enqueue(entry, true, entry.message.c_str(), value...); },
            values);
    } catch (const std::bad_alloc&) {
        return detail::internal_failure({}, true);
    } catch (const std::invalid_argument&) {
        return detail::complete({}, SubmitStatus::invalid_record);
    } catch (...) {
        return detail::internal_failure({}, false);
    }
}

}

// level must be a Severity constant. Category is evaluated once for the runtime
// filter; code/context/format arguments are evaluated only if that filter passes.
// Capture source at the call site, not inside the wrapper lambda.
#define ATELIER_LOG_WITH(level, category, code, context, format, ...)                                                  \
    ([&](std::source_location atelier_log_source) -> ::atelier::logging::SubmitResult {                                \
        if constexpr ((level) < ::atelier::logging::native::minimum_severity) {                                        \
            return {::atelier::logging::SubmitStatus::filtered};                                                       \
        } else {                                                                                                       \
            if (::atelier::logging::current_run() == nullptr) {                                                        \
                return {::atelier::logging::SubmitStatus::no_active_run};                                              \
            }                                                                                                          \
            auto&& atelier_log_category_value = (category);                                                            \
            const std::string_view atelier_log_category = atelier_log_category_value;                                  \
            if (!::atelier::logging::enabled((level), atelier_log_category)) {                                         \
                return {::atelier::logging::SubmitStatus::filtered};                                                   \
            }                                                                                                          \
            return [&]<class... Args>(Args&&... atelier_log_args) {                                                    \
                constexpr fmtquill::format_string<Args...> atelier_log_format{format};                                 \
                return ::atelier::logging::native::write<(level)>(atelier_log_category, (code), (context),             \
                                                                  atelier_log_source, atelier_log_format,              \
                                                                  std::forward<Args>(atelier_log_args)...);            \
            }(__VA_ARGS__);                                                                                            \
        }                                                                                                              \
    }(std::source_location::current()))

#define ATELIER_LOG(level, category, code, format, ...)                                                                \
    ATELIER_LOG_WITH(level, category, code, ::atelier::logging::native::Context{}, format __VA_OPT__(, ) __VA_ARGS__)
