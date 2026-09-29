#include "editor/session.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error) throw std::runtime_error(commit.notification_error->message);
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void collect(editor::Session& document) { for (unsigned step = 0; step < 96; ++step) take(document.collect_step()); }
ObjectId component(editor::Session& document, ObjectId actor, TypeId type) {
    for (const auto& root : take(world::components(document.world().runtime(), actor)))
        if (take(document.world().runtime().type_of(root.id())) == type) return root.id();
    throw std::runtime_error("Expected component is absent.");
}
float value(editor::Session& document, ObjectId target, std::size_t axis = 0) {
    return std::get<float>(take(document.world().read(target, document.parameters()[axis].path)));
}
}
int main() {
    try {
        editor::Session document;
        changed(document.add_actor());
        const auto actor = document.selected();
        auto& runtime = document.world().runtime();
        require(take(document.world().members()).size() == 1 && take(world::components(runtime, actor)).empty() &&
            take(document.world().render_items()).empty(), "An empty Actor acquired implicit components or rendering.");
        const auto state = take(world::inspect_actor(runtime, actor));
        require(state.state == world::ActorState::attached_inactive && state.can_edit && state.can_present,
            "Stopped editor World did not expose an editable/presentable Actor.");
        changed(document.add_component(document.types().transform));
        changed(document.add_component(document.types().cube));
        const auto transform = component(document, actor, document.types().transform);
        const auto cube = component(document, actor, document.types().cube);
        require(take(document.world().render_items()).size() == 1, "Cube component did not create a value projection.");
        changed(document.remove_component(cube));
        require(take(document.world().members()).size() == 1 && take(document.world().render_items()).empty(),
            "Removing Cube removed its Actor or retained rendering.");
        collect(document);
        changed(document.undo());
        require(component(document, actor, document.types().cube) == cube &&
            take(world::inspect_component(runtime, cube)).owner == actor, "Component undo replaced identity or owner.");
        changed(document.redo()); changed(document.undo());
        const auto history = document.undo_count();
        const auto path = document.parameters()[0].path;
        take(document.begin_edit(transform, path, 11));
        for (unsigned step = 0; step < 64; ++step) take(document.preview_edit(11, static_cast<float>(step)));
        require(!document.preview_edit(12, 99.f) && value(document, transform) == 63.f,
            "A different control session modified the active target.");
        take(document.commit_edit(11));
        require(document.undo_count() == history + 1, "Continuous field editing produced more than one history entry.");
        changed(document.undo()); require(value(document, transform) == 0, "Field undo did not restore its before value.");
        changed(document.redo());
        take(document.begin_edit(transform, document.parameters()[1].path, 13));
        take(document.preview_edit(13, 7)); take(document.cancel_edit());
        require(value(document, transform, 1) == 0 && document.undo_count() == history + 1,
            "Field cancellation changed history or the committed value.");
        changed(document.add_actor());
        const auto other = document.selected();
        take(document.begin_edit(transform, path, 14));
        take(document.preview_edit(14, 34)); take(document.commit_edit(14));
        changed(document.undo());
        require(document.selected() == actor && value(document, transform) == 63,
            "Property undo failed to select its recorded owner or wrote another component.");
        changed(document.redo());
        const auto original_transform = take(world::read_transform(runtime, transform));
        take(document.begin_edit(transform, document.types().rotation_fields[2], 17));
        take(document.preview_edit(17, .7f)); take(document.commit_edit(17));
        take(document.begin_edit(transform, document.types().scale_fields[0], 18));
        take(document.preview_edit(18, 2.f)); take(document.commit_edit(18));
        require(take(world::read_transform(runtime, transform)) != original_transform,
            "Transform field transactions did not update rotation and scale.");
        changed(document.undo()); changed(document.undo());
        require(take(world::read_transform(runtime, transform)) == original_transform,
            "TRS undo did not restore the complete value after quaternion normalization.");
        take(document.select(actor));
        changed(document.erase_selected());
        require(take(document.world().members()).size() == 1 && take(document.world().render_items()).empty() &&
            !document.begin_edit(transform, path, 15), "A detached Actor remained editable or presentable in the World.");
        collect(document);
        changed(document.undo());
        require(document.selected() == actor && take(document.world().members()).front().id() == actor &&
            component(document, actor, document.types().transform) == transform && component(document, actor, document.types().cube) == cube &&
            value(document, transform) == 34, "Actor delete/GC/undo did not restore identities, composition, order and values.");
        take(document.begin_edit(transform, path, 16)); take(document.preview_edit(16, 88));
        changed(document.erase_selected()); changed(document.undo());
        require(!document.editing() && value(document, transform) == 34,
            "Structural removal failed to cancel the active field before changing membership.");
        auto retained = take(runtime.hold(actor));
        changed(document.erase_selected());
        for (unsigned step = 0; step < 66; ++step) { changed(document.add_actor()); changed(document.erase_selected()); }
        collect(document);
        require(take(world::inspect_actor(runtime, actor)).state == world::ActorState::detached &&
            take(world::components(runtime, actor)).size() == 2 && take(world::read_transform(runtime, transform)).position.x == 34,
            "History retirement destroyed an Actor still held by an ordinary root or lost its composition.");
        retained.reset(); collect(document);
        require(take(world::inspect_actor(runtime, actor)).state == world::ActorState::invalid,
            "Retired document history retained an unreferenced detached Actor.");
        auto survivor = take(runtime.hold(other));
        changed(document.close());
        require(survivor.registered() && take(world::inspect_actor(runtime, other)).state == world::ActorState::detached &&
            document.undo_count() == 0 && document.redo_count() == 0,
            "Document close did not release participation/history or destroyed a retained Actor.");
        std::puts("Editor document acceptance: PASS (Actor/component identity, field history, membership, retirement, close)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
