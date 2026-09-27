#include "probe_api.h"

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr int32_t invalid_input = 1;
constexpr int32_t reentrant_call = 2;
constexpr int32_t layout_failed = 3;
constexpr int32_t adapter_exception = 4;

thread_local unsigned measure_depth = 0;
thread_local char boundary_error[512] = {};

struct Engine;

struct OwnedNode {
    LpNode input{};
    std::string text;
};

struct NodeContext {
    Engine* engine = nullptr;
    uint32_t index = 0;
    YGNodeRef node = nullptr;
    YGNodeRef projection_row = nullptr;
    // Zero is an ordinary child; one/two are the shared label/value tracks.
    unsigned track = 0;
};

struct GridRow {
    uint32_t parent = 0;
    YGNodeRef node = nullptr;
};

struct YogaTree {
    std::vector<NodeContext> nodes;
    std::vector<GridRow> rows;
    std::vector<YGNodeRef> owned;

    ~YogaTree() {
        // All nodes are exclusively owned and unreachable here. Finalize avoids
        // recursive teardown and never follows a previously released owner.
        for (auto node : owned) {
            YGNodeFinalize(node);
        }
    }

    YGNodeRef add_node(YGConfigConstRef config) {
        // owned is reserved for the entire logical + synthetic tree first.
        auto node = YGNodeNewWithConfig(config);
        if (!node) {
            throw std::bad_alloc();
        }
        owned.push_back(node);
        return node;
    }
};

struct ConfigDeleter {
    void operator()(YGConfig* config) const noexcept {
        YGConfigFree(config);
    }
};

struct Engine {
    std::unique_ptr<YGConfig, ConfigDeleter> config;
    std::vector<OwnedNode> inputs;
    std::unique_ptr<YogaTree> tree;
    std::vector<LpBox> published;
    std::vector<LpBox> pending;
    LpMeasureFn measure = nullptr;
    void* user_data = nullptr;
    LpStats stats{};
    uint64_t input_revision = 0;
    uint64_t output_revision = 0;
    bool busy = false;
    bool measure_failed = false;
    char error[512] = {};
    char nested_error[512] = {};
};

struct BusyScope {
    Engine& engine;
    explicit BusyScope(Engine& value) : engine(value) { engine.busy = true; }
    ~BusyScope() { engine.busy = false; }
};

struct MeasureScope {
    MeasureScope() { ++measure_depth; }
    ~MeasureScope() { --measure_depth; }
};

void set_error(Engine* engine, const char* message) noexcept {
    auto* output = engine ? engine->error : boundary_error;
    std::snprintf(output, sizeof(boundary_error), "%s", message);
}

void set_node_error(Engine& engine, uint32_t index, const char* message) noexcept {
    std::snprintf(engine.error, sizeof(engine.error), "node[%u]: %s", index, message);
}

void clear_error(Engine& engine) noexcept {
    engine.error[0] = '\0';
    engine.nested_error[0] = '\0';
}

bool reject_reentry(Engine& engine) noexcept {
    if (!engine.busy && measure_depth == 0) {
        return false;
    }
    // A rejected nested call must not overwrite an outer measurement failure.
    std::snprintf(engine.nested_error, sizeof(engine.nested_error),
        "callback reentry into a layout adapter mutation is not allowed");
    return true;
}

bool finite_nonnegative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

std::string_view node_text(const LpNode& node) noexcept {
    return node.text_length == 0 ? std::string_view{}
                                : std::string_view(node.text, node.text_length);
}

bool validate_inputs(Engine& engine, const LpNode* nodes, uint32_t count) {
    if (!nodes || count == 0) {
        set_error(&engine, "lp_apply requires a nonempty complete node batch");
        return false;
    }
    std::unordered_set<uint64_t> ids;
    ids.reserve(count);
    std::vector<uint32_t> child_counts(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        const auto fail = [&](const char* message) {
            set_node_error(engine, i, message);
            return false;
        };
        if (node.id == 0 || !ids.insert(node.id).second) {
            return fail("IDs must be nonzero and unique");
        }
        if ((i == 0 && node.parent != UINT32_MAX) || (i != 0 && node.parent >= i)) {
            return fail("root must be index zero; every other parent must precede its child");
        }
        if (node.kind > LP_BOX || node.flow > LP_GRID_TWO ||
            node.width.mode > LP_GROW || node.height.mode > LP_GROW) {
            return fail("unknown node kind, flow, or axis mode");
        }
        if (node.absolute > 1 || node.has_min_width > 1 || node.has_max_width > 1 ||
            node.reserved != 0) {
            return fail("boolean flags must be zero/one and reserved must be zero");
        }
        if (!finite_nonnegative(node.width.value) || !finite_nonnegative(node.height.value) ||
            !finite_nonnegative(node.padding) || !finite_nonnegative(node.gap) ||
            !finite_nonnegative(node.min_width) || !finite_nonnegative(node.max_width) ||
            !finite_nonnegative(node.first_column) || !finite_nonnegative(node.font_size) ||
            !std::isfinite(node.left) || !std::isfinite(node.top)) {
            return fail("all scalar inputs must be finite; dimensions, spacing and font size must be nonnegative");
        }
        if (node.has_min_width && node.has_max_width && node.min_width > node.max_width) {
            return fail("min_width exceeds max_width");
        }
        if ((i == 0 && node.absolute) || (!node.absolute && (node.left != 0 || node.top != 0))) {
            return fail("left/top require an absolute non-root node");
        }
        if (node.flow != LP_GRID_TWO && node.first_column != 0) {
            return fail("first_column is only defined for LP_GRID_TWO");
        }
        if (node.kind == LP_TEXT) {
            if (node.font_size <= 0 || (node.text_length != 0 && !node.text)) {
                return fail("text requires a positive font size and a valid byte range");
            }
            if (node.flow != LP_COLUMN || node.gap != 0) {
                return fail("text leaves do not support child flow or gap properties");
            }
        } else if (node.text_length != 0) {
            return fail("only LP_TEXT supports text payloads");
        }
        if (i == 0) {
            continue;
        }
        const auto& parent = nodes[node.parent];
        if (parent.kind == LP_TEXT) {
            return fail("a text node cannot have children");
        }
        if (node.absolute && (!std::isfinite(parent.padding + node.left) ||
                              !std::isfinite(parent.padding + node.top))) {
            return fail("absolute inset plus parent padding overflows");
        }
        const auto child_ordinal = child_counts[node.parent]++;
        if (parent.flow == LP_GRID_TWO) {
            if (node.absolute) {
                return fail("absolute direct grid cells cannot participate in paired tracks");
            }
            if (node.width.mode == LP_FIXED &&
                ((child_ordinal % 2) != 0 || node.width.value != parent.first_column)) {
                return fail("grid owns cell widths; fixed width is only valid for a matching label track");
            }
            if ((child_ordinal % 2) == 0 &&
                ((node.has_min_width && node.min_width > parent.first_column) ||
                 (node.has_max_width && node.max_width < parent.first_column))) {
                return fail("label width bounds conflict with the shared first_column track");
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].flow == LP_GRID_TWO && (child_counts[i] % 2) != 0) {
            set_node_error(engine, i, "LP_GRID_TWO requires complete label/value pairs");
            return false;
        }
    }
    return true;
}

bool same_topology(const Engine& engine, const LpNode* nodes, uint32_t count) noexcept {
    if (engine.inputs.size() != count) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& previous = engine.inputs[i].input;
        const auto& next = nodes[i];
        if (previous.id != next.id || previous.parent != next.parent || previous.kind != next.kind ||
            (previous.flow == LP_GRID_TWO) != (next.flow == LP_GRID_TWO)) {
            return false;
        }
    }
    return true;
}

bool same_style(const LpNode& a, const LpNode& b) noexcept {
    return a.flow == b.flow && a.absolute == b.absolute &&
        a.width.mode == b.width.mode && a.width.value == b.width.value &&
        a.height.mode == b.height.mode && a.height.value == b.height.value &&
        a.padding == b.padding && a.gap == b.gap && a.left == b.left && a.top == b.top &&
        a.has_min_width == b.has_min_width && a.has_max_width == b.has_max_width &&
        a.min_width == b.min_width && a.max_width == b.max_width &&
        a.first_column == b.first_column;
}

bool same_parent_style(const LpNode& a, const LpNode& b) noexcept {
    return a.flow == b.flow && a.padding == b.padding && a.first_column == b.first_column;
}

bool horizontal_flow(uint32_t flow) noexcept {
    return flow == LP_ROW || flow == LP_WRAP;
}

void apply_node_style(NodeContext& context, const LpNode& input, const LpNode* parent) {
    const auto node = context.node;
    YGNodeStyleSetDirection(node, YGDirectionLTR);
    YGNodeStyleSetBoxSizing(node, YGBoxSizingBorderBox);
    YGNodeStyleSetFlexDirection(node, horizontal_flow(input.flow) ? YGFlexDirectionRow : YGFlexDirectionColumn);
    YGNodeStyleSetFlexWrap(node, input.flow == LP_WRAP ? YGWrapWrap : YGWrapNoWrap);
    YGNodeStyleSetAlignContent(node, YGAlignFlexStart);
    YGNodeStyleSetAlignItems(node, input.flow == LP_WRAP ? YGAlignFlexStart : YGAlignStretch);
    YGNodeStyleSetJustifyContent(node, YGJustifyFlexStart);
    YGNodeStyleSetPadding(node, YGEdgeAll, input.padding);
    YGNodeStyleSetGap(node, YGGutterAll, input.gap);
    YGNodeStyleSetPositionType(node, input.absolute ? YGPositionTypeAbsolute : YGPositionTypeRelative);
    YGNodeStyleSetAlignSelf(node, YGAlignAuto);
    YGNodeStyleSetFlexGrow(node, 0.0f);
    YGNodeStyleSetFlexShrink(node, 0.0f);
    YGNodeStyleSetFlexBasisAuto(node);

    if (parent) {
        if (input.width.mode == LP_FIXED) {
            YGNodeStyleSetWidth(node, input.width.value);
        } else {
            YGNodeStyleSetWidthAuto(node);
        }
        if (input.height.mode == LP_FIXED) {
            YGNodeStyleSetHeight(node, input.height.value);
        } else {
            YGNodeStyleSetHeightAuto(node);
        }
        YGNodeStyleSetMinWidth(node, input.has_min_width ? input.min_width : YGUndefined);
        YGNodeStyleSetMaxWidth(node, input.has_max_width ? input.max_width : YGUndefined);
    }
    // Root width/height and width bounds belong exclusively to lp_layout.
    const auto parent_horizontal = context.track != 0 || (parent && horizontal_flow(parent->flow));
    const auto main_mode = parent_horizontal ? input.width.mode : input.height.mode;
    const auto cross_mode = parent_horizontal ? input.height.mode : input.width.mode;
    if (parent && !input.absolute && main_mode == LP_GROW) {
        YGNodeStyleSetFlexBasis(node, 0.0f);
        YGNodeStyleSetFlexGrow(node, 1.0f);
        YGNodeStyleSetFlexShrink(node, 1.0f);
    }
    if (parent && !input.absolute && cross_mode == LP_GROW) {
        YGNodeStyleSetAlignSelf(node, YGAlignStretch);
    }

    if (context.track == 1) {
        YGNodeStyleSetWidth(node, parent->first_column);
        YGNodeStyleSetFlexBasisAuto(node);
        YGNodeStyleSetFlexGrow(node, 0.0f);
        YGNodeStyleSetFlexShrink(node, 0.0f);
    } else if (context.track == 2) {
        YGNodeStyleSetWidthAuto(node);
        YGNodeStyleSetFlexBasis(node, 0.0f);
        YGNodeStyleSetFlexGrow(node, 1.0f);
        YGNodeStyleSetFlexShrink(node, 1.0f);
    }

    // Explicit Yoga insets are measured from the containing border box. The
    // probe offsets start inside the parent's padding, which is projected here
    // as a style input, never added to the final solver-generated geometry.
    const auto padding = parent ? parent->padding : 0.0f;
    YGNodeStyleSetPosition(node, YGEdgeLeft, input.absolute ? padding + input.left : YGUndefined);
    YGNodeStyleSetPosition(node, YGEdgeTop, input.absolute ? padding + input.top : YGUndefined);
    YGNodeStyleSetPosition(node, YGEdgeRight,
        input.absolute && input.width.mode == LP_GROW ? padding : YGUndefined);
    YGNodeStyleSetPosition(node, YGEdgeBottom,
        input.absolute && input.height.mode == LP_GROW ? padding : YGUndefined);
}

void apply_row_style(YGNodeRef node, const LpNode& grid) {
    YGNodeStyleSetFlexDirection(node, YGFlexDirectionRow);
    YGNodeStyleSetFlexWrap(node, YGWrapNoWrap);
    YGNodeStyleSetAlignItems(node, YGAlignFlexStart);
    YGNodeStyleSetAlignContent(node, YGAlignFlexStart);
    YGNodeStyleSetJustifyContent(node, YGJustifyFlexStart);
    YGNodeStyleSetFlexGrow(node, 0.0f);
    YGNodeStyleSetFlexShrink(node, 0.0f);
    YGNodeStyleSetGap(node, YGGutterColumn, grid.gap);
}

void fail_measure(Engine& engine, uint32_t index, const char* message) noexcept {
    if (!engine.measure_failed) {
        set_node_error(engine, index, message);
    }
    engine.measure_failed = true;
}

bool convert_constraint(float value, YGMeasureMode mode, float& converted, uint32_t& converted_mode) noexcept {
    switch (mode) {
    case YGMeasureModeUndefined:
        converted = 0.0f;
        converted_mode = LP_UNBOUNDED;
        return true;
    case YGMeasureModeExactly:
        converted_mode = LP_EXACT;
        break;
    case YGMeasureModeAtMost:
        converted_mode = LP_AT_MOST;
        break;
    default:
        return false;
    }
    converted = value;
    return finite_nonnegative(value);
}

YGSize measure_node(YGNodeConstRef node, float width, YGMeasureMode width_mode,
    float height, YGMeasureMode height_mode) {
    const auto& context = *static_cast<const NodeContext*>(YGNodeGetContext(node));
    auto& engine = *context.engine;
    // Yoga's callback has no error channel. These scratch values only let Yoga
    // unwind normally; the entire tree is discarded and cannot be published.
    if (engine.measure_failed) {
        return {0.0f, 0.0f};
    }
    const auto& owned = engine.inputs[context.index];
    LpMeasureRequest request{};
    request.text = owned.text.data();
    request.text_length = owned.input.text_length;
    request.font_size = owned.input.font_size;
    if (!convert_constraint(width, width_mode, request.width, request.width_mode) ||
        !convert_constraint(height, height_mode, request.height, request.height_mode)) {
        fail_measure(engine, context.index, "Yoga supplied a nonfinite/negative measurement constraint");
        return {0.0f, 0.0f};
    }
    LpMeasured measured{YGUndefined, YGUndefined, YGUndefined, 0};
    int32_t status = 0;
    {
        MeasureScope scope;
        ++engine.stats.measure_calls;
        status = engine.measure(engine.user_data, &request, &measured);
    }
    if (status != 0) {
        if (!engine.measure_failed) {
            std::snprintf(engine.error, sizeof(engine.error),
                "node[%u] id=%llu: measure provider failed with status %d", context.index,
                static_cast<unsigned long long>(owned.input.id), static_cast<int>(status));
        }
        engine.measure_failed = true;
        return {0.0f, 0.0f};
    }
    if (!finite_nonnegative(measured.width) || !finite_nonnegative(measured.height) ||
        !finite_nonnegative(measured.baseline)) {
        fail_measure(engine, context.index, "measure provider returned a nonfinite/negative width, height or baseline");
        return {0.0f, 0.0f};
    }
    return {measured.width, measured.height};
}

std::unique_ptr<YogaTree> build_tree(Engine& engine, const std::vector<OwnedNode>& inputs) {
    auto tree = std::make_unique<YogaTree>();
    const auto count = inputs.size();
    size_t grid_children = 0;
    for (size_t i = 1; i < count; ++i) {
        if (inputs[inputs[i].input.parent].input.flow == LP_GRID_TWO) {
            ++grid_children;
        }
    }
    tree->nodes.resize(count);
    tree->rows.reserve(grid_children / 2);
    tree->owned.reserve(count + grid_children / 2);
    for (size_t i = 0; i < count; ++i) {
        auto& context = tree->nodes[i];
        context.engine = &engine;
        context.index = static_cast<uint32_t>(i);
        context.node = tree->add_node(engine.config.get());
        YGNodeSetContext(context.node, &context);
        if (inputs[i].input.kind == LP_TEXT) {
            YGNodeSetMeasureFunc(context.node, measure_node);
        }
    }
    std::vector<uint32_t> child_counts(count, 0);
    std::vector<YGNodeRef> last_row(count, nullptr);
    for (size_t i = 1; i < count; ++i) {
        auto& context = tree->nodes[i];
        const auto parent_index = inputs[i].input.parent;
        auto parent = tree->nodes[parent_index].node;
        const auto ordinal = child_counts[parent_index]++;
        if (inputs[parent_index].input.flow == LP_GRID_TWO) {
            if ((ordinal % 2) == 0) {
                auto row = tree->add_node(engine.config.get());
                tree->rows.push_back({parent_index, row});
                apply_row_style(row, inputs[parent_index].input);
                YGNodeInsertChild(parent, row, YGNodeGetChildCount(parent));
                last_row[parent_index] = row;
            }
            parent = last_row[parent_index];
            context.projection_row = parent;
            context.track = (ordinal % 2) + 1;
        }
        YGNodeInsertChild(parent, context.node, YGNodeGetChildCount(parent));
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& input = inputs[i].input;
        apply_node_style(tree->nodes[i], input, i == 0 ? nullptr : &inputs[input.parent].input);
    }
    return tree;
}

std::vector<OwnedNode> own_inputs(const LpNode* nodes, uint32_t count) {
    std::vector<OwnedNode> result(count);
    for (uint32_t i = 0; i < count; ++i) {
        result[i].input = nodes[i];
        // The copied input never retains a caller pointer, including SSO moves.
        result[i].input.text = nullptr;
        if (nodes[i].kind == LP_TEXT) {
            result[i].text = node_text(nodes[i]);
        }
    }
    return result;
}

void update_tree(Engine& engine, const LpNode* nodes, uint32_t count) {
    struct TextUpdate {
        uint32_t index;
        std::string text;
    };
    std::vector<TextUpdate> text_updates;
    // Allocate changed payloads before modifying the solver or owned snapshot.
    // Unchanged text and its cached measurements require no payload allocation.
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].kind == LP_TEXT && engine.inputs[i].text != node_text(nodes[i])) {
            text_updates.push_back({i, std::string(node_text(nodes[i]))});
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& previous = engine.inputs[i].input;
        const auto& next = nodes[i];
        const auto parent_changed = i != 0 &&
            !same_parent_style(engine.inputs[next.parent].input, nodes[next.parent]);
        if (!same_style(previous, next) || parent_changed) {
            apply_node_style(engine.tree->nodes[i], next, i == 0 ? nullptr : &nodes[next.parent]);
        }
        if (next.kind == LP_TEXT &&
            (previous.content_revision != next.content_revision ||
             previous.measure_revision != next.measure_revision || previous.font_size != next.font_size ||
             engine.inputs[i].text != node_text(next))) {
            YGNodeMarkDirty(engine.tree->nodes[i].node);
        }
    }
    for (const auto& row : engine.tree->rows) {
        if (engine.inputs[row.parent].input.gap != nodes[row.parent].gap) {
            apply_row_style(row.node, nodes[row.parent]);
        }
    }
    // No fallible operations remain: contexts resolve this stable Engine plus
    // input index, not temporary LpNode/string/vector addresses.
    for (auto& update : text_updates) {
        engine.inputs[update.index].text.swap(update.text);
    }
    for (uint32_t i = 0; i < count; ++i) {
        engine.inputs[i].input = nodes[i];
        engine.inputs[i].input.text = nullptr;
    }
}

bool nearly_equal(float a, float b) noexcept {
    return std::abs(a - b) <= 0.0001f + 0.00001f * std::max(std::abs(a), std::abs(b));
}

bool collect_boxes(Engine& engine) {
    engine.pending.resize(engine.inputs.size());
    for (size_t i = 0; i < engine.inputs.size(); ++i) {
        const auto& input = engine.inputs[i].input;
        const auto& context = engine.tree->nodes[i];
        auto& box = engine.pending[i];
        box = {input.id, i == 0 ? 0 : engine.inputs[input.parent].input.id,
            YGNodeLayoutGetLeft(context.node), YGNodeLayoutGetTop(context.node),
            YGNodeLayoutGetWidth(context.node), YGNodeLayoutGetHeight(context.node)};
        if (context.projection_row) {
            // Only synthetic ancestry is collapsed; every coordinate and size
            // comes from Yoga, and parent_id remains the public input parent.
            box.x += YGNodeLayoutGetLeft(context.projection_row);
            box.y += YGNodeLayoutGetTop(context.projection_row);
            const auto& grid = engine.inputs[input.parent].input;
            const auto expected_width = context.track == 1 ? grid.first_column :
                std::max(0.0f, YGNodeLayoutGetWidth(context.projection_row) - grid.first_column - grid.gap);
            if (!nearly_equal(box.width, expected_width)) {
                set_node_error(engine, static_cast<uint32_t>(i),
                    "cell width bounds or padding conflict with its shared grid track");
                return false;
            }
        }
        if (!std::isfinite(box.x) || !std::isfinite(box.y) ||
            !finite_nonnegative(box.width) || !finite_nonnegative(box.height)) {
            set_node_error(engine, static_cast<uint32_t>(i), "Yoga produced a nonfinite/negative logical box");
            return false;
        }
    }
    return true;
}

void record_exception(Engine* engine, const char* operation, const char* detail) noexcept {
    auto* output = engine ? engine->error : boundary_error;
    std::snprintf(output, sizeof(boundary_error), "%s: %s", operation, detail);
}

} // namespace

extern "C" {

const char* lp_name(void) {
    return "Yoga 042f501 (native flex + shared-track row projection)";
}

void* lp_create(LpMeasureFn measure, void* user_data) {
    if (!measure) {
        set_error(nullptr, "lp_create requires a measure provider");
        return nullptr;
    }
    if (measure_depth != 0) {
        set_error(nullptr, "lp_create is not allowed during a measure callback");
        return nullptr;
    }
    try {
        auto engine = std::make_unique<Engine>();
        engine->measure = measure;
        engine->user_data = user_data;
        engine->config.reset(YGConfigNew());
        if (!engine->config) {
            throw std::bad_alloc();
        }
        YGConfigSetUseWebDefaults(engine->config.get(), false);
        YGConfigSetPointScaleFactor(engine->config.get(), 0.0f);
        YGConfigSetErrata(engine->config.get(), YGErrataNone);
        boundary_error[0] = '\0';
        return engine.release();
    } catch (const std::exception& error) {
        record_exception(nullptr, "lp_create", error.what());
    } catch (...) {
        record_exception(nullptr, "lp_create", "unknown exception");
    }
    return nullptr;
}

void lp_destroy(void* handle) {
    auto* engine = static_cast<Engine*>(handle);
    if (!engine || reject_reentry(*engine)) {
        return;
    }
    delete engine;
}

int32_t lp_apply(void* handle, const LpNode* nodes, uint32_t count, uint64_t input_revision) {
    auto* engine = static_cast<Engine*>(handle);
    if (!engine) {
        set_error(nullptr, "lp_apply requires a live engine");
        return invalid_input;
    }
    if (reject_reentry(*engine)) {
        return reentrant_call;
    }
    BusyScope scope(*engine);
    clear_error(*engine);
    ++engine->stats.apply_calls;
    try {
        if (!validate_inputs(*engine, nodes, count)) {
            return invalid_input;
        }
        if (!engine->tree || !same_topology(*engine, nodes, count)) {
            auto inputs = own_inputs(nodes, count);
            auto tree = build_tree(*engine, inputs);
            engine->tree.swap(tree);
            engine->inputs.swap(inputs);
            ++engine->stats.rebuilds;
        } else {
            update_tree(*engine, nodes, count);
        }
        engine->input_revision = input_revision;
        engine->stats.input_nodes += count;
        clear_error(*engine);
        return 0;
    } catch (const std::exception& error) {
        record_exception(engine, "lp_apply", error.what());
    } catch (...) {
        record_exception(engine, "lp_apply", "unknown exception");
    }
    // The accepted input snapshot is unchanged if style updates threw. Never
    // reuse a partially changed solver; lp_layout can rebuild that snapshot.
    engine->tree.reset();
    return adapter_exception;
}

int32_t lp_layout(void* handle, float root_width, float root_height) {
    auto* engine = static_cast<Engine*>(handle);
    if (!engine) {
        set_error(nullptr, "lp_layout requires a live engine");
        return invalid_input;
    }
    if (reject_reentry(*engine)) {
        return reentrant_call;
    }
    BusyScope scope(*engine);
    clear_error(*engine);
    if (!finite_nonnegative(root_width) || !finite_nonnegative(root_height)) {
        set_error(engine, "root constraints must be finite and nonnegative");
        return invalid_input;
    }
    if (engine->inputs.empty()) {
        set_error(engine, "lp_layout requires a successful lp_apply first");
        return invalid_input;
    }
    try {
        if (!engine->tree) {
            engine->tree = build_tree(*engine, engine->inputs);
            ++engine->stats.rebuilds;
        }
        const auto root = engine->tree->nodes[0].node;
        YGNodeStyleSetWidth(root, root_width);
        YGNodeStyleSetHeight(root, root_height);
        YGNodeStyleSetMinWidth(root, YGUndefined);
        YGNodeStyleSetMaxWidth(root, YGUndefined);
        engine->measure_failed = false;
        ++engine->stats.layouts;
        YGNodeCalculateLayout(root, root_width, root_height, YGDirectionLTR);
        if (engine->measure_failed || !collect_boxes(*engine)) {
            // Yoga may have cached scratch dimensions after a provider failure.
            // Destroy every such cache before any subsequent attempt.
            engine->tree.reset();
            return layout_failed;
        }
        if (!nearly_equal(engine->pending[0].width, root_width) ||
            !nearly_equal(engine->pending[0].height, root_height)) {
            set_error(engine, "root padding conflicts with the supplied root constraints");
            engine->tree.reset();
            return layout_failed;
        }
        engine->published.swap(engine->pending);
        engine->output_revision = engine->input_revision;
        engine->stats.output_boxes += engine->published.size();
        clear_error(*engine);
        return 0;
    } catch (const std::exception& error) {
        record_exception(engine, "lp_layout", error.what());
    } catch (...) {
        record_exception(engine, "lp_layout", "unknown exception (including a throwing measure provider)");
    }
    engine->tree.reset();
    return adapter_exception;
}

uint32_t lp_copy_boxes(void* handle, LpBox* output, uint32_t capacity) {
    auto* engine = static_cast<Engine*>(handle);
    if (!engine) {
        set_error(nullptr, "lp_copy_boxes requires a live engine");
        return 0;
    }
    const auto count = engine->published.size();
    if (capacity < count || (!output && count != 0)) {
        if (!engine->busy) {
            set_error(engine, "lp_copy_boxes requires a buffer large enough for all published boxes");
        }
        return 0;
    }
    if (count != 0) {
        std::memcpy(output, engine->published.data(), count * sizeof(LpBox));
    }
    return static_cast<uint32_t>(count);
}

uint64_t lp_output_revision(void* handle) {
    const auto* engine = static_cast<const Engine*>(handle);
    return engine ? engine->output_revision : 0;
}

const char* lp_last_error(void* handle) {
    const auto* engine = static_cast<const Engine*>(handle);
    if (!engine) {
        return boundary_error;
    }
    return (engine->busy || measure_depth != 0) && engine->nested_error[0] != '\0'
        ? engine->nested_error : engine->error;
}

LpStats lp_stats(void* handle) {
    const auto* engine = static_cast<const Engine*>(handle);
    return engine ? engine->stats : LpStats{};
}

size_t lp_sizeof_node(void) {
    return sizeof(LpNode);
}

size_t lp_sizeof_box(void) {
    return sizeof(LpBox);
}

} // extern "C"
