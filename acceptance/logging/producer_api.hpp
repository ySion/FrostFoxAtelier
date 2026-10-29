#pragma once
#include "logging/logging.hpp"

namespace logging_acceptance {
// Only values cross this same-toolchain/CRT boundary. No native.hpp, formatter,
// decoder, callback into the producer, or backend ownership crosses it.
using HostSubmit = atelier::logging::SubmitResult (*)(atelier::logging::Entry) noexcept;
using Produce = int (*)(HostSubmit) noexcept;
}
