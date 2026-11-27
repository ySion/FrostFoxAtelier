#pragma once
#include "editor/operators.hpp"
#include "foundation/span.hpp"
#include "ui/session.hpp"

namespace atelier::editor {
enum class KeyPhase { before_controls, after_controls };

struct KeymapEntry {
    RegionKind region;

    KeyPhase phase;

    std::uint32_t key;

    unsigned modifiers{};

    unsigned ignored{}; // Control=1, Shift=2, Alt=4; ignored preserves existing modifier behavior.

    bool repeat{};

    bool text{true};

    OperatorCall call;

    std::wstring_view shortcut;
};

Span<const KeymapEntry> keymap();

const KeymapEntry* key_binding(RegionKind, KeyPhase, const ui::Event&, std::uint32_t, bool text);

std::wstring_view shortcut(const OperatorCall&, RegionKind);
}
