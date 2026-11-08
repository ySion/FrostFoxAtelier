#pragma once

#include "logging/logging.hpp"
#include "logging/detail/native.hpp"
#include <quill/bundled/fmt/base.h>
#include <array>
#include <concepts>
#include <limits>
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

template <class... Args>
class FormatString {
    // The pinned Windows floating types leave room for fmt's precision + exponent.
    static_assert(std::numeric_limits<long double>::digits <= 53 &&
                  std::numeric_limits<long double>::max_exponent <= 1024);

    static_assert(maximum_message_bytes < std::numeric_limits<int>::max() - 1024);

    struct Policy {
        std::array<fmtquill::detail::type, sizeof...(Args)> types{fmtquill::detail::mapped_type_constant<
            std::remove_cvref_t<decltype(*detail::argument(std::declval<const std::remove_cvref_t<Args>&>()))>,
            char>::value...};

        fmtquill::detail::compile_parse_context<char> context;

        constexpr explicit Policy(fmtquill::string_view text)
            : context(text, static_cast<int>(sizeof...(Args)), types.data()) {}

        constexpr void on_text(const char*, const char*) {}

        constexpr int on_arg_id() { return context.next_arg_id(); }

        constexpr int on_arg_id(int id) {
            context.check_arg_id(id);
            return id;
        }

        int on_arg_id(fmtquill::string_view) {
            fmtquill::report_error("Named arguments are not supported in native log formats");
            return -1;
        }

        constexpr void on_replacement_field(int, const char*) {}

        constexpr const char* on_format_specs(int id, const char* begin, const char* end) {
            context.advance_to(begin);
            fmtquill::detail::dynamic_format_specs<char> specs;
            const auto type = types[static_cast<std::size_t>(id)];
            const auto next = fmtquill::detail::parse_format_specs(begin, end, specs, context, type);
            if (specs.dynamic_width() != fmtquill::arg_id_kind::none)
                fmtquill::report_error("Dynamic width is not supported in native log formats");
            if (specs.dynamic_precision() != fmtquill::arg_id_kind::none)
                fmtquill::report_error("Dynamic precision is not supported in native log formats");
            if (fmtquill::detail::in(type, fmtquill::detail::float_set) &&
                specs.precision > static_cast<int>(maximum_message_bytes))
                fmtquill::report_error("Floating precision exceeds maximum_message_bytes in native log format");
            return next;
        }

        void on_error(const char* message) { fmtquill::report_error(message); }
    };

    fmtquill::format_string<Args...> format_;

public:
    template <std::size_t N>
    consteval FormatString(const char (&spelling)[N]) : format_(spelling) {
        const fmtquill::string_view text{spelling, N - 1};
        // Reuse fmt's grammar after its full type check; only policy is local.
        fmtquill::detail::parse_format_string(text, Policy{text});
    }

    [[nodiscard]] fmtquill::string_view get() const noexcept { return format_.get(); }
};

// Positional, compile-time-checked format; no named/runtime formats or user codecs.
// Copy strings, fields and repeat into the queue before return; format the message
// on the backend. Direct calls evaluate arguments first; use ATELIER_LOG[_WITH]
// for compile-time/runtime filtering before argument evaluation.
template <Severity Level, Argument... Args>
    requires(Level != Severity::off && sizeof...(Args) <= 16)
[[nodiscard]] SubmitResult write(std::string_view category, std::string_view event_code, const Context& context,
                                 std::source_location source, std::type_identity_t<FormatString<Args...>> format,
                                 Args&&... args) noexcept {
    if constexpr (Level < minimum_severity)
        return {SubmitStatus::filtered};
    if (!current_run())
        return {SubmitStatus::no_active_run};
    if (!enabled(Level, category))
        return {SubmitStatus::filtered};
    const auto spelling = format.get();
    Entry entry;
    entry.severity = Level;
    entry.category = category;
    entry.event_code = event_code;
    entry.message.assign(spelling.data(), spelling.size());
    if (entry.message.find('\0') != std::string::npos)
        return detail::complete({}, SubmitStatus::invalid_record);
    auto location = native_source(source);
    if (!location)
        return detail::complete({}, SubmitStatus::invalid_record);
    entry.source = std::move(*location);
    entry.fields.assign(context.fields.begin(), context.fields.end());
    entry.repeat = context.repeat;
    const auto values = std::tuple{detail::argument(args)...};
    return std::apply(
        [&](const auto&... value) {
            if (!(value.has_value() && ...))
                return detail::complete({}, SubmitStatus::invalid_record);
            return detail::enqueue(entry, true, entry.message.c_str(), *value...);
        },
        values);
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
                constexpr ::atelier::logging::native::FormatString<Args...> atelier_log_format{format};                \
                return ::atelier::logging::native::write<(level)>(atelier_log_category, (code), (context),             \
                                                                  atelier_log_source, atelier_log_format,              \
                                                                  std::forward<Args>(atelier_log_args)...);            \
            }(__VA_ARGS__);                                                                                            \
        }                                                                                                              \
    }(std::source_location::current()))

#define ATELIER_LOG(level, category, code, format, ...)                                                                \
    ATELIER_LOG_WITH(level, category, code, ::atelier::logging::native::Context{}, format __VA_OPT__(, ) __VA_ARGS__)
