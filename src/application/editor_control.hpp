#pragma once
#include "renderer/renderer.hpp"
#include <filesystem>

namespace atelier::application {
class Application;
// Editor-thread host for the public control protocol. Transport and file workers
// never borrow Application, Runtime, UI or GPU state.
class EditorControl final {
public:
    EditorControl(Application&, const std::filesystem::path&);
    ~EditorControl();
    void poll();
    void input(UINT message, WPARAM);
    void document_replaced();
    renderer::ViewSettings view() const;
    bool wants_capture();
    void rendered(const renderer::Frame&, const renderer::FrameResult&, renderer::Capture*);
    void render_failed(std::string_view);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
