#include "logging/native.hpp"

void logging_format_bad_arity() {
    namespace log = atelier::logging;
    const auto result = ATELIER_LOG(log::Severity::info, "acceptance.format", "format.bad_arity", "{} {}", 37);
    static_cast<void>(result);
}
