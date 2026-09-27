#pragma once
// Experimental comparison ABI, not the production layout Interface.
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum LpAxisMode { LP_AUTO = 0, LP_FIXED = 1, LP_GROW = 2 };
enum LpFlow { LP_COLUMN = 0, LP_ROW = 1, LP_WRAP = 2, LP_GRID_TWO = 3 };
enum LpMeasureMode { LP_UNBOUNDED = 0, LP_AT_MOST = 1, LP_EXACT = 2, LP_MIN_CONTENT = 3 };
enum LpNodeKind { LP_CONTAINER = 0, LP_TEXT = 1, LP_BOX = 2 };

typedef struct LpAxis { uint32_t mode; float value; } LpAxis;
typedef struct LpNode {
    uint64_t id;
    uint32_t parent; // input index; UINT32_MAX for the sole root
    uint32_t kind;
    uint32_t flow;
    uint32_t absolute;
    LpAxis width;
    LpAxis height;
    float padding;
    float gap;
    float left;
    float top;
    uint32_t has_min_width;
    uint32_t has_max_width;
    float min_width;
    float max_width;
    float first_column; // explicit shared-track result, LP_GRID_TWO only
    float font_size;
    const char* text; // UTF-8; borrowed until the next successful lp_apply/destroy
    uint32_t text_length;
    uint32_t reserved;
    uint64_t content_revision;
    uint64_t measure_revision;
} LpNode;

typedef struct LpMeasureRequest {
    const char* text;
    uint32_t text_length;
    float font_size;
    float width;
    float height;
    uint32_t width_mode;
    uint32_t height_mode;
} LpMeasureRequest;
typedef struct LpMeasured { float width; float height; float baseline; uint32_t lines; } LpMeasured;
// Return 0 on success. No exception may cross this callback.
typedef int32_t (*LpMeasureFn)(void*, const LpMeasureRequest*, LpMeasured*);

typedef struct LpBox {
    uint64_t id;
    uint64_t parent_id; // zero for root
    float x; // relative to parent outer box; padding already included
    float y;
    float width;
    float height;
} LpBox;
typedef struct LpStats {
    uint64_t apply_calls;
    uint64_t layouts;
    uint64_t rebuilds;
    uint64_t measure_calls;
    uint64_t input_nodes;
    uint64_t output_boxes;
} LpStats;

const char* lp_name(void);
void* lp_create(LpMeasureFn measure, void* user_data);
void lp_destroy(void* engine);
// Synchronous batch. Reject malformed tree/values and all callback reentry.
int32_t lp_apply(void* engine, const LpNode* nodes, uint32_t count, uint64_t input_revision);
// On failure, last successful owned boxes and output_revision remain unchanged.
// Before retry, invalidate/rebuild any solver caches touched by a failed measure.
int32_t lp_layout(void* engine, float root_width, float root_height);
uint32_t lp_copy_boxes(void* engine, LpBox* output, uint32_t capacity);
uint64_t lp_output_revision(void* engine);
const char* lp_last_error(void* engine);
LpStats lp_stats(void* engine);
size_t lp_sizeof_node(void);
size_t lp_sizeof_box(void);

#ifdef __cplusplus
}
#endif
