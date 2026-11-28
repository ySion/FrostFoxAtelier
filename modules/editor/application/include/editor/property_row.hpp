#pragma once
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "platform/utf.hpp"
#include "ui_language/component.hpp"
#include <algorithm>
#include <array>

namespace atelier::editor {

// One host-side contract for the PropertyRow declared in editor.ui.source.
// Entity fields, resource fields and World options differ in how they commit,
// while publishing the same presentation and interaction bindings.
struct PropertyRow {
    enum class Kind { number, boolean, enumeration, text };

    ui::ControlKey key{};

    std::wstring title;

    ui_language::EditBinding edit;

    Kind kind{Kind::number};

    bool checked{};

    bool enabled{true};

    ui_language::ActionBinding change;

    std::wstring choice;

    ui::ControlKey target{};

    Optional<std::array<double, 3>> swatch;

    bool override_available{};

    bool overridden{};

    ui_language::ActionBinding inherit;

    ui_language::TooltipBinding help;

    ui_language::ActionBinding pick;

    ui_language::TextEditBinding text_edit;

    ui::ControlKey context_menu{};

    Optional<bool> row_enabled{};

    ui_language::Item item() && {
        const auto color = swatch.value_or(std::array<double, 3>{});
        // Optional actions still cross the DSL's nonzero-target contract.
        // An absent action is inert and retains this row's stable identity.
        const auto inherited = override_available ? inherit : ui_language::ActionBinding{key, false};
        const auto text_binding =
            kind == Kind::text ? std::move(text_edit) : ui_language::TextEditBinding{key, {}, false};
        const bool effective_row_enabled = row_enabled.value_or(enabled);
        return {key,
                {std::move(title),
                 std::move(edit),
                 kind == Kind::boolean,
                 checked,
                 enabled,
                 change,
                 kind == Kind::enumeration,
                 std::move(choice),
                 target,
                 swatch.has_value(),
                 color[0],
                 color[1],
                 color[2],
                 override_available,
                 overridden,
                 inherited,
                 std::move(help),
                 pick.target ? pick : ui_language::ActionBinding{key, false},
                 kind == Kind::text,
                 text_binding,
                 context_menu,
                 effective_row_enabled}};
    }
};

// Bounded to descriptions used by the owner's latest successful composition.
// Store text only: descriptions must never retain scene objects or Runtime borrows.
class PropertyHelpCache {
public:
    void begin() {
        for (auto [key, entry] : entries_) {
            static_cast<void>(key);
            entry.used = false;
        }
    }

    void end() {
        // A retained panel can use its already decorated descriptor without calling get() again.
        entries_.remove_if([](ui::ControlKey, const Entry& entry) {
            return !entry.used && entry.help.use_count() == 1;
        });
    }

    void clear() { entries_.clear(); }

    ui_language::TooltipBinding get(ui::ControlKey key, std::wstring_view title, std::wstring_view identity,
                                    StringView description) {
        auto& entry = *entries_.try_emplace(key).value;
        entry.used = true;
        if (!entry.help || entry.help->title != title || entry.help->identity != identity ||
            entry.description != description) {
            entry.description = String::from_utf8_validated(description);
            entry.help = std::make_shared<const ui::Tooltip>(ui::Tooltip{
                std::wstring(title), std::wstring(identity), platform::utf8_to_utf16_validated(description)});
        }
        return entry.help;
    }

private:
    struct Entry {
        ui_language::TooltipBinding help;

        String description;

        bool used{};
    };

    Map<ui::ControlKey, Entry> entries_;
};

} // namespace atelier::editor
