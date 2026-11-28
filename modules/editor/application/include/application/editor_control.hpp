#pragma once
#include "renderer/renderer.hpp"
#include "control/control.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include <windows.h>
#include <filesystem>

namespace atelier::application {
class Application;

// Editor-thread host for the public control protocol. Transport and file workers
// never borrow Application, Runtime, UI or GPU state.
class EditorControl final {
public:
    struct ActiveJobInfo {
        String id;

        StringView kind;

        StringView phase;
    };

    static std::expected<EditorControl, control::Failure> create(Application&, const std::filesystem::path&);

    EditorControl(EditorControl&&) noexcept;

    ~EditorControl();

    [[nodiscard]] Optional<ActiveJobInfo> active_job();

    bool poll();

    void input(UINT message, WPARAM);

    void document_replaced();

    renderer::ViewSettings view() const;

    bool wants_capture();

    // Borrow after wants_capture()'s checkpoint, until this render returns.
    rdg::TraceSnapshot* command_trace() const;

    StringView trace_scope() const;

    // Completes the requested-view attempt before fallback/not_ready rewriting.
    // A terminal host outcome (including fallback failure) preserves its file
    // output before shutdown cancels jobs; it does not replace the first result.
    void traced(const renderer::Frame&, const renderer::Result<renderer::FrameResult>&, bool host_stopping);

    void rendered(const renderer::Frame&, const renderer::FrameResult&, renderer::Capture*);

    void render_failed(StringView);

private:
    struct Impl;

    explicit EditorControl(std::unique_ptr<Impl>) noexcept;

    std::unique_ptr<Impl> impl_;
};
}
