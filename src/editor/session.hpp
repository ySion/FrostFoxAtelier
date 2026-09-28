#pragma once

#include "scene/scene.hpp"

namespace atelier::editor {

struct Parameter {
    const char* name;
    const char* unit;
    float default_value;
    float minimum;
    float maximum;
    float drag_step;
};
inline constexpr std::array position_parameters{
    Parameter{"X", "m", 0, -scene::Scene::position_limit, scene::Scene::position_limit, .01f},
    Parameter{"Y", "m", 0, -scene::Scene::position_limit, scene::Scene::position_limit, .01f},
    Parameter{"Z", "m", 0, -scene::Scene::position_limit, scene::Scene::position_limit, .01f}};

// All document mutations enter here. Continuous previews update the real object;
// the active edit stores only the before-value and the target's holding root.
class Session final {
public:
    Session();
    const scene::Scene& scene() const noexcept { return scene_; }
    scene::ObjectKey selected() const noexcept { return selected_; }
    Result<void> select(scene::ObjectKey);
    Result<scene::ObjectKey> add(scene::Vec3 = {});
    Result<void> erase_selected();
    Result<void> begin_position();
    Result<void> preview_position(std::size_t axis, float value);
    Result<void> commit_position();
    Result<void> cancel_position();
    Result<void> undo();
    Result<void> redo();
    Result<void> collect_step() { return scene_.collect_step(); }
    bool editing() const noexcept { return active_.has_value(); }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }
private:
    enum class Kind { add, remove, position };
    struct Change {
        Kind kind;
        scene::Member member;
        std::size_t index{};
        scene::Vec3 before{}, after{};
    };
    static constexpr std::size_t history_limit = 128;
    scene::Scene scene_;
    std::vector<Change> history_;
    std::size_t cursor_{};
    scene::ObjectKey selected_{};
    std::optional<Change> active_;
    void remember(Change);
    Result<void> replay(const Change&, bool forward);
};
}
