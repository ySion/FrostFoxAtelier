#include "logging/native.hpp"

void logging_format_bad_type() {
    namespace log = atelier::logging;
    const auto result =
        ATELIER_LOG(log::Severity::info, "acceptance.format", "format.bad_type", "{:d}", "not-an-integer");
    static_cast<void>(result);
}
