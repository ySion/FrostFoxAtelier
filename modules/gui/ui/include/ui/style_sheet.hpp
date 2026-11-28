#pragma once
#include "foundation/array.hpp"
#include "foundation/flags.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "ui/style.hpp"
#include <expected>

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
    Optional<std::wstring> font_family;

    Optional<std::wstring> font_locale;

    Optional<float> font_size;

    Optional<std::uint16_t> font_weight;

    Optional<FontStyle> font_style;

    Optional<TextAlign> text_align;

    Optional<TextVerticalAlign> text_vertical_align;

    Optional<BorderStyle> border_style;

    Optional<BorderEdges> border_edges;

    Optional<Color> border_inner;

    Optional<Color> border_shadow;

    Optional<Color> text_shadow; // One-pixel drop shadow for labels drawn over scenes.

    Optional<ScrollbarGutter> scrollbar_gutter;

    Optional<ComboIndicator> combo_indicator;

    Optional<IconPlacement> icon_placement;

    Optional<TextAlign> icon_align;

    Optional<float> indicator_size;

    Optional<Color> indicator_background;

    Optional<Color> indicator_border;

    Optional<Color> indicator_foreground;

    Optional<Color> foreground;

    Optional<Color> background;

    Optional<Color> border;

    Optional<Color> selection;

    Optional<Color> caret;

    Optional<Color> scrollbar;

    Optional<Color> accent;

    Optional<float> accent_width;

    Optional<ui_layout::Insets> padding;

    Optional<float> border_width;

    Optional<float> caret_width;

    Optional<CornerRadii> radius;

    Optional<float> scrollbar_width;

    Optional<float> scrollbar_inset;

    Optional<float> scrollbar_min_thumb;

    Optional<float> scrollbar_button_size;

    Optional<float> tree_row_height;

    Optional<float> tree_indent;

    Optional<Color> tree_alternate;

    Optional<Color> tree_line;

    Optional<std::shared_ptr<const ColorImage>> image;

    Optional<std::shared_ptr<const ColorImage>> scrollbar_image;

    bool operator==(const StyleProperties&) const = default;
};

struct StyleSelector {
    Optional<ControlKind> kind;

    Optional<Role> role;

    Array<String> classes;

    StyleState states{};
};

struct StyleRule {
    StyleSelector selector;

    StyleProperties properties;
};

struct StyleDiagnostic {
    StyleDiagnostic() = default;

    StyleDiagnostic(std::size_t error_line, std::size_t error_column, Utf8Text text)
        : line(error_line), column(error_column), message(std::move(text).into_string()) {}

    std::size_t line{};

    std::size_t column{};

    String message;
};

struct NamedImage {
    NamedImage() = default;

    NamedImage(Utf8Text resource_name, std::shared_ptr<const ColorImage> resource = {})
        : name(std::move(resource_name).into_string()), image(std::move(resource)) {}

    String name;

    std::shared_ptr<const ColorImage> image;
};

// Complete value used by measurement, painting and the accepted frame.
struct ResolvedStyle {
    Font font;

    TextAlign text_align{TextAlign::left};

    TextVerticalAlign text_vertical_align{TextVerticalAlign::top};

    BorderStyle border_style{BorderStyle::solid};

    BorderEdges border_edges{BorderEdges::all};

    Optional<Color> border_inner;

    Optional<Color> border_shadow;

    Optional<Color> text_shadow;

    ScrollbarGutter scrollbar_gutter{ScrollbarGutter::stable};

    ComboIndicator combo_indicator{ComboIndicator::down};

    IconPlacement icon_placement{IconPlacement::leading};

    TextAlign icon_align{TextAlign::center}; // Uncaptioned icons; text keeps its own alignment.

    float indicator_size{14};

    Color indicator_background{0, 0, 0, 0};

    Color indicator_border{0, 0, 0, 0};

    Optional<Color> indicator_foreground;

    ui_layout::Insets padding;

    Color foreground;

    Color border;

    Color selection;

    Color caret;

    Color scrollbar;

    Color accent{.25f, .5f, .7f, 1};

    float accent_width{};

    Optional<Color> background;

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

    static Result create(Array<StyleRule>);

    static Result parse(StringView utf8, Span<const NamedImage> images = {});

    static bool valid_properties(const StyleProperties&) noexcept;

    static bool valid_class(StringView) noexcept;

    Span<const StyleRule> rules() const noexcept { return rules_; }

private:
    explicit StyleSheet(Array<StyleRule> rules) : rules_(std::move(rules)) {}

    Array<StyleRule> rules_;
};

} // namespace atelier::ui
