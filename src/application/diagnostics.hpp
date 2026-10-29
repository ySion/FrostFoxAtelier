#pragma once
#include "editor/session.hpp"
#include "logging/logging.hpp"
#include "source/diagnostics.hpp"

namespace atelier::application {
// These bridges own the domain payload, not the underlying diagnostic policy.
// Draining is performed by the host exactly once at its existing boundary.
logging::SourceLocation diagnostic_source(const SourceLocation&);
logging::Entry world_diagnostic(const editor::Session&,const world::Diagnostic&,
    const std::filesystem::path& root,std::string_view origin="world");
void report_world(const editor::Session&,std::span<const world::Diagnostic>,
    const std::filesystem::path& root,std::string_view origin="world") noexcept;
void drain_world(editor::Session&,const std::filesystem::path& root,std::uint64_t& dropped) noexcept;
void report_load(const editor::Session&,const std::filesystem::path& root) noexcept;
// Explicit capture only: callers pass an already requested Diagnostics snapshot.
// This function never enables instrumentation or changes a Program's options.
void report_source_capture(const source::DiagnosticReport&,const std::filesystem::path& root) noexcept;
}
