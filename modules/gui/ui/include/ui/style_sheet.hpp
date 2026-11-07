#pragma once
#include "foundation/flags.hpp"
#include "ui/style.hpp"
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace atelier::ui {

enum class StyleState : std::uint16_t {
    none = 0,
    enabled = 1,
    disabled = 2,
    hover = 4,
    pressed = 8,
    focus = 16,
    selected = 32,
    invalid = 64,
    modified = 128
};

}

template <>
inline constexpr bool atelier::enable_enum_flags<atelier::ui::StyleState> = true;

namespace atelier::ui {

// Unspecified properties fall back to lower rules. Clear an override by removing
// its optional value; a transparent color is a specified value, not "unset".
struct StyleProperties {
    std::optional<std::wstring> font_family;

    std::optional<std::wstring> font_locale;

    std::optional<float> font_size;

    std::optional<std::uint16_t> font_weight;

    std::optional<FontStyle> font_style;

    std::optional<TextAlign> text_align;

    std::optional<TextVerticalAlign> text_vertical_align;

    std::optional<BorderStyle> border_style;

    std::optional<BorderEdges> border_edges;

    std::optional<Color> border_inner;

    std::optional<Color> border_shadow;

    std::optional<Color> text_shadow; // One-pixel drop shadow for labels drawn over scenes.

    std::optional<ScrollbarGutter> scrollbar_gutter;

    std::optional<ComboIndicator> combo_indicator;

    std::optional<IconPlacement> icon_placement;

    std::optional<TextAlign> icon_align;

    std::optional<float> indicator_size;

    std::optional<Color> indicator_background;

    std::optional<Color> indicator_border;

    std::optional<Color> indicator_foreground;

    std::optional<Color> foreground;

    std::optional<Color> background;

    std::optional<Color> border;

    std::optional<Color> selection;

    std::optional<Color> caret;

    std::optional<Color> scrollbar;

    std::optional<Color> accent;

    std::optional<float> accent_width;

    std::optional<ui_layout::Insets> padding;

    std::optional<float> border_width;

    std::optional<float> caret_width;

    std::optional<CornerRadii> radius;

    std::optional<float> scrollbar_width;

    std::optional<float> scrollbar_inset;

    std::optional<float> scrollbar_min_thumb;

    std::optional<float> scrollbar_button_size;

    std::optional<float> tree_row_height;

    std::optional<float> tree_indent;

    std::optional<Color> tree_alternate;

    std::optional<Color> tree_line;

    std::optional<std::shared_ptr<const ColorImage>> image;

    std::optional<std::shared_ptr<const ColorImage>> scrollbar_image;

    bool operator==(const StyleProperties&) const = default;
};

struct StyleSelector {
    std::optional<ControlKind> kind;

    std::optional<Role> role;

    std::vector<std::string> classes;

    StyleState states{};
};

struct StyleRule {
    StyleSelector selector;

    StyleProperties properties;
};

struct StyleDiagnostic {
    std::size_t line{};

    std::size_t column{};

    std::string message;
};

struct NamedImage {
    std::string name;

    std::shared_ptr<const ColorImage> image;
};

// Complete value used by measurement, painting and the accepted frame.
struct ResolvedStyle {
    Font font;

    TextAlign text_align{TextAlign::left};

    TextVerticalAlign text_vertical_align{TextVerticalAlign::top};

    BorderStyle border_style{BorderStyle::solid};

    BorderEdges border_edges{BorderEdges::all};

    std::optional<Color> border_inner;

    std::optional<Color> border_shadow;

    std::optional<Color> text_shadow;

    ScrollbarGutter scrollbar_gutter{ScrollbarGutter::stable};

    ComboIndicator combo_indicator{ComboIndicator::down};

    IconPlacement icon_placement{IconPlacement::leading};

    TextAlign icon_align{TextAlign::center}; // Uncaptioned icons; text keeps its own alignment.

    float indicator_size{14};

    Color indicator_background{0, 0, 0, 0};

    Color indicator_border{0, 0, 0, 0};

    std::optional<Color> indicator_foreground;

    ui_layout::Insets padding;

    Color foreground;

    Color border;

    Color selection;

    Color caret;

    Color scrollbar;

    Color accent{.25f, .5f, .7f, 1};

    float accent_width{};

    std::optional<Color> background;

    float border_width{};

    float caret_width{1};

    CornerRadii radius;

    float scrollbar_width{3};

    float scrollbar_inset{1};

    float scrollbar_min_thumb{12};

    float scrollbar_button_size{}; // Zero follows width; thin (<8 DIP) bars omit step buttons.

    float tree_row_height{20};

    float tree_indent{18};

    Color tree_alternate{.20f, .20f, .20f, 1};

    Color tree_line{.38f, .38f, .38f, 1};

    std::shared_ptr<const ColorImage> image;

    std::shared_ptr<const ColorImage> scrollbar_image;

    bool operator==(const ResolvedStyle&) const = default;
};

// C++ and SS are producers of the same immutable rules. No file IO, business
// objects, GPU handles, global class registry or language runtime is required.
class StyleSheet {
public:
    using Result = std::expected<std::shared_ptr<const StyleSheet>, StyleDiagnostic>;

    static Result create(std::vector<StyleRule>);

    static Result parse(std::string_view utf8, std::span<const NamedImage> images = {});

    static bool valid_properties(const StyleProperties&) noexcept;

    static bool valid_class(std::string_view) noexcept;

    std::span<const StyleRule> rules() const noexcept { return rules_; }

private:
    explicit StyleSheet(std::vector<StyleRule> rules) : rules_(std::move(rules)) {}

    std::vector<StyleRule> rules_;
};

} // namespace atelier::ui
