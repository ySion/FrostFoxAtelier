#pragma once
#include "editor/session.hpp"
#include "logging/logging.hpp"
#include "source/diagnostics.hpp"
#include "ui/error.hpp"

namespace atelier::renderer {
struct Error;
}

namespace atelier::application {
// Preserve the UI domain payload in the host's existing owning diagnostic.
Error ui_error(const ui::Error&);

// The caller handles terminal state before converting its diagnostic.
Error renderer_error(const renderer::Error&);

// Presentation of an existing failure only. Callers retain its original Error
// and display prefix; invalid diagnostic bytes must not cause a second failure.
std::wstring diagnostic_text(std::string_view message) noexcept;

std::string diagnostic_text(std::wstring_view message) noexcept;

// Borrows valid input unchanged, otherwise returns the static diagnostic placeholder.
std::string_view diagnostic_utf8(std::string_view message) noexcept;

// These bridges own the domain payload, not the underlying diagnostic policy.
// Draining is performed by the host exactly once at its existing boundary.
logging::SourceLocation diagnostic_source(const SourceLocation&);

Result<logging::Entry> error_diagnostic(const Error&, std::string_view event, const std::filesystem::path& root);

Result<logging::Entry> world_diagnostic(const editor::Session&, const world::Diagnostic&,
                                        const std::filesystem::path& root, std::string_view origin = "world");

void report_world(const editor::Session&, Span<const world::Diagnostic>, const std::filesystem::path& root,
                  std::string_view origin = "world") noexcept;

void drain_world(editor::Session&, const std::filesystem::path& root, std::uint64_t& dropped) noexcept;

void report_load(const editor::Session&, const std::filesystem::path& root) noexcept;

// Explicit capture only: callers pass an already requested Diagnostics snapshot.
// This function never enables instrumentation or changes a Program's options.
void report_source_capture(const source::DiagnosticReport&, const std::filesystem::path& root) noexcept;
}
