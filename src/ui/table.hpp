#pragma once

#include "ui/style.hpp"
#include <vector>

namespace atelier::ui {

using TableKey = std::uint64_t; // Stable identity inside one table; zero means no selection.
enum class TableView { details, list, tiles };
// Presentation is independent of the immutable model; changing a view or its
// density keeps the same row storage and selection identity.
struct TableLayout {
    TableView view{TableView::details};
    float row_height{23}, header_height{25};
    float tile_width{90}, tile_height{66}, item_gap{3};
    bool operator==(const TableLayout&) const = default;
};
enum class TableSortDirection { ascending, descending };
struct TableSort {
    TableKey column{};
    TableSortDirection direction{TableSortDirection::ascending};
    bool operator==(const TableSort&) const = default;
};
struct TableSelection {
    TableKey row{}, column{};
    bool operator==(const TableSelection&) const = default;
};
struct TableColumnResize {
    TableKey column{};
    float width{}; // DIPs; emitted once on release. Capture previews are Session-owned.
    bool operator==(const TableColumnResize&) const = default;
};
struct TableColumn {
    TableKey key{};
    std::wstring title;
    float width{160}, min_width{40}; // DIPs.
    TextAlign alignment{TextAlign::left};
    bool sortable{true};
    bool operator==(const TableColumn&) const = default;
};
struct TableCell {
    TableKey key{};
    std::wstring text;
    ControlIcon icon{ControlIcon::none};
    bool operator==(const TableCell&) const = default;
};
struct TableRow {
    TableKey key{};
    std::vector<TableCell> cells; // Same order and count as columns.
    bool enabled{true};
    bool operator==(const TableRow&) const = default;
};
// Publish an immutable snapshot and reuse it while unchanged. The host owns row
// order, business values, selection and committed column widths. Session owns
// scrolling, focus and an in-flight column preview; it never sorts cell text.
struct TableData {
    std::vector<TableColumn> columns;
    std::vector<TableRow> rows;
    TableSort sort;
    bool operator==(const TableData&) const = default;
};

} // namespace atelier::ui
