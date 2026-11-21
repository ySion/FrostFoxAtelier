#pragma once

#include "editor/graph_view.hpp"
#include "editor/session.hpp"
#include "procedural/geometry.hpp"
#include "ui_language/live_document.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace atelier::editor {

namespace spreadsheet_control {
inline constexpr ui::ControlKey tab = 610;

inline constexpr ui::ControlKey content = 698;

inline constexpr ui::ControlKey table = 1100;

inline constexpr ui::ControlKey filter = 1101;

inline constexpr ui::ControlKey follow_toggle = 1102;

inline constexpr ui::ControlKey layer_point = 1103;

inline constexpr ui::ControlKey layer_vertex = 1104;

inline constexpr ui::ControlKey layer_primitive = 1105;

inline constexpr ui::ControlKey layer_detail = 1106;

inline constexpr ui::ControlKey layer_instance = 1107;

inline constexpr ui::ControlKey page_prev = 1109;

inline constexpr ui::ControlKey page_next = 1110;

inline constexpr ui::ControlKey column_first = 1120;
} // namespace spreadsheet_control

namespace network_control {
inline constexpr ui::ControlKey tab = 609;

inline constexpr ui::ControlKey content = 697;

inline constexpr ui::ControlKey add_node = 1150;

inline constexpr ui::ControlKey frame_all = 1151;

inline constexpr ui::ControlKey zoom_reset = 1153;

inline constexpr ui::ControlKey context_node_menu = 1155;

inline constexpr ui::ControlKey new_scratch = 1156;

inline constexpr ui::ControlKey context_canvas_menu = 1157;

inline constexpr ui::ControlKey canvas = 1168;

inline constexpr ui::ControlKey canvas_nodes = 1169;

inline constexpr ui::ControlKey canvas_backdrop = 1170;

inline constexpr ui::ControlKey canvas_overlay = 1171;

inline constexpr ui::ControlKey tab_search_box = 1172;

inline constexpr ui::ControlKey tab_search_input = 1173;

inline constexpr ui::ControlKey hover_port_tag = 1174;

inline constexpr ui::ControlKey node_key_base = 1ULL << 36;

inline constexpr ui::ControlKey node_key_limit = 1ULL << 38;

inline constexpr ui::ControlKey batch_key_base = node_key_limit - (1ULL << 24);

inline constexpr ui::ControlKey popup_root = 1ULL << 39;

inline constexpr ui::ControlKey add_sticky_note = popup_root + 1;

inline constexpr ui::ControlKey add_network_box = popup_root + 2;

inline constexpr ui::ControlKey open_shape_picker = popup_root + 3;

inline constexpr ui::ControlKey open_color_picker = popup_root + 4;

inline constexpr ui::ControlKey open_node_info = popup_root + 5;

inline constexpr ui::ControlKey toolbar_row = popup_root + 6;

inline constexpr ui::ControlKey path_combo = popup_root + 7;

inline constexpr ui::ControlKey nav_back = popup_root + 8;

inline constexpr ui::ControlKey nav_forward = popup_root + 9;

inline constexpr ui::ControlKey pin_toggle = popup_root + 10;

inline constexpr ui::ControlKey isolate_toggle = popup_root + 11;

inline constexpr ui::ControlKey action_bar = popup_root + 12;

inline constexpr ui::ControlKey popup_switch_style = popup_root + 13;

inline constexpr ui::ControlKey menu_add = popup_root + 14;

inline constexpr ui::ControlKey menu_edit = popup_root + 15;

inline constexpr ui::ControlKey tab_item_first = popup_root + 16; // +16..+31

inline constexpr ui::ControlKey tab_pick_first = popup_root + 32; // +32..+47

inline constexpr ui::ControlKey tab_cat_row = popup_root + 48;

inline constexpr ui::ControlKey tab_cat_first = popup_root + 50; // +50..+55

inline constexpr ui::ControlKey menu_view = popup_root + 56;

inline constexpr ui::ControlKey menu_layout = popup_root + 57;

inline constexpr ui::ControlKey frame_selected = popup_root + 58;

inline constexpr ui::ControlKey select_all_nodes = popup_root + 59;

inline constexpr ui::ControlKey deselect_all_nodes = popup_root + 60;

inline constexpr ui::ControlKey invert_select_nodes = popup_root + 61;

constexpr bool is_header_menu(ui::ControlKey key) noexcept {
    return key == menu_add || key == menu_edit || key == menu_view || key == menu_layout;
}

inline constexpr ui::ControlKey popup_box = popup_root + 64;

inline constexpr ui::ControlKey popup_close = popup_root + 65;

inline constexpr ui::ControlKey popup_header = popup_root + 66;

inline constexpr ui::ControlKey shape_pick_first = popup_root + 70; // 0=auto, 1..8=shapes

inline constexpr ui::ControlKey color_reset_btn = popup_root + 82;

inline constexpr ui::ControlKey color_pick_first = popup_root + 84; // 24 swatches (+84..+107)

inline constexpr ui::ControlKey info_comment_input = popup_root + 112;

inline constexpr ui::ControlKey note_title_input = popup_root + 113;

inline constexpr ui::ControlKey note_body_input = popup_root + 114;

inline constexpr ui::ControlKey box_title_input = popup_root + 115;

inline constexpr ui::ControlKey popup_delete_item = popup_root + 116;

inline constexpr ui::ControlKey popup_aux_first = popup_root + 128; // +128..+255

inline constexpr ui::ControlKey path_choice_first = popup_root + 256; // +256..+511
} // namespace network_control

class GeometrySpreadsheet final {
public:
    static constexpr std::size_t page_size = 512;

    void reset() noexcept;

    bool apply(const ui::Action& action);

    bool follow_selected() const noexcept { return follow_selected_; }

    void set_follow_selected(bool follow) noexcept;

    procedural::Layer layer() const noexcept { return layer_; }

    void set_layer(procedural::Layer next) noexcept;

    std::size_t page_offset() const noexcept { return page_offset_; }

    void set_page_offset(std::size_t offset) noexcept;

    void set_filter(std::wstring query);

    const std::shared_ptr<const ui::TableData>& build_table(const procedural::Geometry* geometry,
                                                            std::uint64_t generation = 0);

    std::size_t total_rows() const noexcept { return total_rows_; }

    void append(ui_language::ControlOutput controls, ui::ControlKey parent, ui_language::LiveDocument& live,
                const procedural::Geometry* geometry, std::uint64_t generation, std::wstring_view target_label,
                std::wstring_view error_text = {});

private:
    bool follow_selected_{true};

    procedural::Layer layer_{procedural::Layer::point};

    std::wstring filter_;

    std::size_t page_offset_{};

    std::size_t total_rows_{};

    ui::TableKey selected_row_{};

    std::unordered_map<std::wstring, float> column_widths_;

    const procedural::Geometry* cached_geometry_{};

    std::uint64_t cached_generation_{};

    procedural::Layer cached_layer_{procedural::Layer::point};

    std::size_t cached_offset_{};

    std::wstring cached_filter_;

    bool dirty_{true};

    std::shared_ptr<const ui::TableData> table_;
};

class NetworkCanvas final {
public:
    static constexpr float node_width = 88.f;

    static constexpr float node_height = 28.f;

    static constexpr float circle_width = 56.f;

    static constexpr float left_flag_width = 13.f;

    static constexpr float template_flag_width = 12.f;

    static constexpr float flag_width = 13.f;

    static constexpr float pin_offset = 5.5f;

    static constexpr float air_inner_radius = 54.f;

    static constexpr float air_outer_radius = 72.f;

    static constexpr std::size_t max_tab_results = 14;

    static constexpr std::array<float, 12> zoom_steps{0.20f, 0.27f, 0.35f, 0.45f, 0.55f, 0.67f,
                                                      0.80f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f};

    static constexpr std::size_t default_zoom_index = 7;

    static constexpr std::array<std::uint32_t, 24> color_palette{
        0xdc2626, 0xea580c, 0xd97706, 0xeab308, 0xca8a04, 0xf59e0b, // Warm reds & golds
        0x65a30d, 0x16a34a, 0x059669, 0x0d9488, 0x0891b2, 0x0284c7, // Greens & cyans
        0x2563eb, 0x4f46e5, 0x7c3aed, 0x9333ea, 0xc026d3, 0xdb2777, // Blues & magentas
        0xbe123c, 0x78553a, 0x475569, 0x64748b, 0x334155, 0x94a3b8 // Rose, earth & slates
    };

    enum class TabCategory : std::uint8_t {
        all = 0,
        primitive,
        attribute,
        scatter_copy,
        utility,
        source_sop,
    };

    enum class PopupKind : std::uint8_t {
        none = 0,
        shape_picker,
        color_picker,
        node_info,
        edit_note,
        edit_box,
    };

    struct PortRef {
        std::string node;

        std::string port;

        std::size_t index{};

        bool is_output{};

        bool multiple{};

        bool operator==(const PortRef&) const = default;
    };

    struct TabSearch {
        bool open{};

        float dip_x{24.f};

        float dip_y{24.f};

        std::array<float, 2> world_pos{};

        std::wstring query;

        TabCategory category{TabCategory::all};

        std::size_t highlight{};

        std::string auto_from_node;

        std::string auto_from_port;

        std::string auto_to_node;

        std::string auto_to_port;
    };

    struct Popup {
        PopupKind kind{PopupKind::none};

        float dip_x{24.f};

        float dip_y{24.f};

        std::string target_name;

        std::vector<std::string> target_nodes;

        std::wstring title_edit;

        std::wstring body_edit;

        bool palette_floating{};

        bool open() const noexcept { return kind != PopupKind::none; }
    };

    struct InteractionOutcome {
        bool consumed{};

        bool capture_acquire{};

        bool capture_release{};

        bool focus_tab_search{};

        bool open_node_context{};

        bool open_canvas_context{};

        std::string context_node;

        std::array<float, 2> context_world_pos{};

        std::optional<ui::Cursor> cursor;

        std::wstring status;
    };

    void reset();

    bool active_gesture() const noexcept { return gesture_ != Gesture::none; }

    void cancel_gesture() noexcept;

    void set_cut_key(bool held) noexcept { cut_key_held_ = held; }

    float zoom() const noexcept { return zoom_steps[zoom_index_]; }

    std::array<float, 2> pan() const noexcept { return {pan_x_, pan_y_}; }

    void sync_graph(std::string_view graph_key, std::span<const GraphNodeView> nodes, ui_layout::Size canvas_dip,
                    std::span<const GraphNetworkBox> boxes = {}, std::span<const GraphStickyNote> notes = {});

    void reset_zoom(ui_layout::Size canvas_dip);

    void step_zoom(int delta_steps, float anchor_dip_x, float anchor_dip_y);

    void frame_nodes(std::span<const GraphNodeView> nodes, ui_layout::Size canvas_dip,
                     std::span<const std::string> only_selected = {}, std::span<const GraphNetworkBox> boxes = {},
                     std::span<const GraphStickyNote> notes = {});

    std::array<float, 2> to_world(float dip_x, float dip_y) const noexcept;

    std::array<float, 2> to_dip(float world_x, float world_y) const noexcept;

    std::array<float, 2> displayed_position(const GraphNodeView& node) const;

    std::array<float, 2> displayed_note_position(const GraphStickyNote& note) const;

    std::array<float, 2> displayed_note_size(const GraphStickyNote& note) const;

    std::array<float, 2> displayed_box_position(const GraphNetworkBox& box) const;

    std::array<float, 2> displayed_box_size(const GraphNetworkBox& box) const;

    std::array<float, 2> port_dip(const GraphNodeView& node, bool is_output, std::size_t port_index) const;

    static std::uint8_t resolve_shape_id(const GraphNodeView& node, bool is_source_sop) noexcept;

    static TabCategory categorize_node_type(const GraphNodeCatalogEntry& entry) noexcept;

    const TabSearch& tab_search() const noexcept { return tab_search_; }

    void open_tab_search(float dip_x, float dip_y, ui_layout::Size canvas_dip, std::string from_node = {},
                         std::string from_port = {}, std::string to_node = {}, std::string to_port = {});

    void close_tab_search() noexcept;

    void set_tab_query(std::wstring query);

    void set_tab_category(TabCategory category) noexcept;

    void move_tab_highlight(int delta, std::size_t count) noexcept;

    std::vector<GraphNodeCatalogEntry> filter_catalog(std::span<const GraphNodeCatalogEntry> catalog) const;

    Result<std::string> commit_tab_pick(Session& session, std::string_view graph_key,
                                        std::span<const GraphNodeCatalogEntry> catalog, std::size_t pick_index,
                                        std::string& primary_selected, std::vector<std::string>& selected_nodes,
                                        bool set_display = false);

    const Popup& popup() const noexcept { return popup_; }

    void open_node_popup(PopupKind kind, const GraphInspectView& view, std::string_view primary_selected,
                         std::span<const std::string> selected_nodes, ui_layout::Size canvas_dip,
                         std::optional<std::array<float, 2>> anchor_dip = {});

    void close_popup() noexcept { popup_ = {}; }

    Result<bool> apply_action(Session& session, std::string_view graph_key, std::string& primary_selected,
                              std::vector<std::string>& selected_nodes, ui_layout::Size canvas_dip,
                              const ui::Action& action, std::wstring& out_status);

    Result<InteractionOutcome> pointer_event(Session& session, std::string_view graph_key,
                                             std::string& primary_selected, std::vector<std::string>& selected_nodes,
                                             ui::Rect canvas_px, float dpi_scale, const ui::Event& event,
                                             Project* project = nullptr);

    Result<InteractionOutcome> key_event(Session& session, std::string_view graph_key, std::string& primary_selected,
                                         std::vector<std::string>& selected_nodes, ui::Rect canvas_px, float dpi_scale,
                                         const ui::Event& event, std::uint32_t native_key);

    void append_canvas(ui_language::ControlOutput output, ui::ControlKey canvas_parent, ui_layout::Size canvas_dip,
                       const std::shared_ptr<const GraphInspectView>& topology,
                       std::span<const GraphNodeCatalogEntry> catalog, std::string_view primary_selected,
                       std::span<const std::string> selected_nodes);

    std::uint64_t idle_revision() const noexcept { return idle_projection_ ? idle_projection_->revision : 0; }

    void append_tab_categories(std::vector<ui::Control>& controls, ui::ControlKey tab_box_key) const;

    static bool is_canvas_control(ui::ControlKey key) noexcept;

private:
    enum class Gesture {
        none,
        pan,
        press_node,
        move_nodes,
        move_note,
        resize_note,
        move_box,
        resize_box,
        press_canvas,
        marquee,
        wire_drag,
        wire_cut
    };

    enum class FlagZone : std::uint8_t { none = 0, bypass, template_flag, display };

    enum class AirButton : std::uint8_t { none = 0, bypass, info, freeze, display, template_flag };

    struct IdleCanvasState {
        ui::ControlKey parent;

        ui_layout::Size extent;

        std::array<float, 2> pan;

        float zoom;

        std::string primary;

        std::string note;

        std::string box;

        std::string hovered_node;

        std::string air_node;

        std::optional<PortRef> hovered_port;

        FlagZone flag;

        AirButton air;

        bool air_ring_hidden;

        bool operator==(const IdleCanvasState&) const = default;
    };

    struct IdleProjection {
        std::weak_ptr<const GraphInspectView> topology;

        IdleCanvasState state;

        std::vector<std::string> selection;

        std::vector<GraphNodeCatalogEntry> catalog;

        std::vector<ui::Control> controls;

        std::uint64_t revision{};
    };

    std::optional<IdleProjection> idle_projection_;

    struct RoutedSegment {
        std::array<float, 2> from;

        std::array<float, 2> to;
    };

    struct WireRoutes {
        struct Wire {
            std::size_t connection;

            std::size_t end;

            bool upstream_error;
        };

        std::weak_ptr<const GraphInspectView> topology;

        std::array<float, 2> origin{};

        float zoom{};

        ui::Rect region{}; // In canvas DIP at origin; includes a margin around the viewport.

        std::vector<Wire> wires;

        std::vector<RoutedSegment> segments;
    };

    WireRoutes wire_routes_;

    std::array<float, 2> prepare_wire_routes(const std::shared_ptr<const GraphInspectView>&, ui_layout::Size);

    void append_canvas_popup(std::vector<ui::Control>&, ui::ControlKey parent, float width, float height,
                             const GraphInspectView&, std::span<const GraphNodeCatalogEntry>) const;

    struct SavedView {
        float pan_x{160.f};

        float pan_y{56.f};

        std::size_t zoom_index{5};

        bool framed{};
    };

    ui::ControlKey node_base_key(std::string_view graph_key, std::string_view item_name);

    std::string active_graph_;

    float pan_x_{160.f};

    float pan_y_{56.f};

    std::size_t zoom_index_{default_zoom_index};

    bool framed_{};

    ui_layout::Size last_canvas_dip_{640.f, 360.f};

    std::unordered_map<std::string, SavedView> saved_views_;

    std::unordered_map<std::string, ui::ControlKey> node_keys_;

    ui::ControlKey next_node_key_{network_control::node_key_base};

    Gesture gesture_{Gesture::none};

    ui::PointerButton gesture_button_{ui::PointerButton::primary};

    bool gesture_shift_{};

    float start_dip_x_{};

    float start_dip_y_{};

    float current_dip_x_{};

    float current_dip_y_{};

    float pan_start_x_{};

    float pan_start_y_{};

    std::string pressed_node_;

    std::string selected_note_;

    std::string selected_box_;

    std::string active_annotation_;

    std::array<float, 2> annotation_origin_pos_{};

    std::array<float, 2> annotation_origin_size_{};

    std::vector<std::pair<std::string, std::array<float, 2>>> drag_origins_;

    std::vector<std::pair<std::string, std::array<float, 2>>> drag_note_origins_;

    std::vector<std::pair<std::string, std::array<float, 2>>> drag_box_origins_;

    std::array<float, 2> drag_world_delta_{};

    std::optional<GraphConnectionView> splice_wire_;

    PortRef wire_anchor_{};

    std::optional<GraphConnectionView> detached_wire_;

    std::optional<PortRef> snapped_port_;

    std::optional<PortRef> hovered_port_;

    std::string hovered_node_;

    std::string air_node_;

    FlagZone hovered_flag_{FlagZone::none};

    AirButton hovered_air_{AirButton::none};

    bool cut_key_held_{};

    bool keyboard_grab_{};

    bool amend_duplicate_{};

    float wheel_remainder_{};

    std::uint64_t last_click_tick_{};

    std::string last_click_target_;

    TabSearch tab_search_;

    Popup popup_;

    std::unordered_map<ui::ControlKey, std::shared_ptr<const ui::Tooltip>> visible_tooltips_;
};

} // namespace atelier::editor
