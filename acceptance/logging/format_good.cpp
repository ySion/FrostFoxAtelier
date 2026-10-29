#include "logging/native.hpp"

void logging_format_good() {
    namespace log = atelier::logging;
    const std::string text{"中文"};
    const auto result = ATELIER_LOG(log::Severity::info, "acceptance.format", "format.good", "{{value}}={:d}; {}", 37, text);
    static_cast<void>(result);
}
