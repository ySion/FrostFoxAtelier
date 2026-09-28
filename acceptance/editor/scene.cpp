#include "editor/session.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
template<class T> T take(atelier::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(atelier::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void collect(atelier::scene::Scene& scene) { for (unsigned step = 0; step < 64; ++step) take(scene.collect_step()); }
}
int main() {
    try {
        using namespace atelier;
        scene::Scene first, second;
        auto original = take(first.create({1, 2, 3}));
        const auto id = original.id();
        require(!second.insert(original, 0) && second.members().empty() && original.root.registered(),
            "Cross-scene insertion changed ownership or members.");
        take(first.insert(original, 0));
        auto alias = original;
        alias.key += 100;
        require(!first.insert(alias, 1) && first.members().size() == 1 && first.find(original.key)->id() == id,
            "Aliased insertion changed scene members.");
        take(first.remove(original.key));
        alias.root.reset(); original.root.reset();
        collect(first); collect(second);
        require(first.live_objects() == 0 && second.live_objects() == 0,
            "Rejected insertion retained an unexpected root.");

        editor::Session session;
        const auto key = take(session.add({1, 2, 3}));
        const auto identity = session.scene().find(key)->id();
        const auto base_history = session.undo_count();
        take(session.begin_position());
        for (unsigned index = 0; index < 64; ++index) take(session.preview_position(0, static_cast<float>(index)));
        take(session.commit_position());
        require(session.undo_count() == base_history + 1, "Drag produced more than one history entry.");
        take(session.undo());
        require(take(session.scene().position(key)) == scene::Vec3{1, 2, 3}, "Undo did not restore drag origin.");
        take(session.redo());
        take(session.begin_position());
        take(session.preview_position(1, 7));
        take(session.cancel_position());
        require(take(session.scene().position(key)) == scene::Vec3{63, 2, 3} && session.undo_count() == base_history + 1,
            "Cancelled edit changed value or history.");
        take(session.erase_selected());
        require(!session.scene().find(key), "Deleted member is still visible.");
        for (unsigned step = 0; step < 64; ++step) take(session.collect_step());
        take(session.undo());
        require(session.scene().find(key)->id() == identity && take(session.scene().position(key)) == scene::Vec3{63, 2, 3},
            "Delete/GC/undo did not restore the same object and values.");
        std::puts("Editor scene acceptance: PASS (foreign roots, aliases, retirement, drag history, delete/GC/identity)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor scene acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
