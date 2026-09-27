//! Experimental Taffy 0.14 adapter for ../probe_api.h, not a production UI interface.
//! Handles are single-thread-owned; synchronous callback reentry is rejected before
//! borrowing solver state. The C++ provider must catch its own exceptions and return
//! a nonzero status. Rust unwinding is caught at every fallible exported boundary.

use std::alloc::{GlobalAlloc, Layout as AllocationLayout};
use std::cell::{RefCell, UnsafeCell};
use std::collections::HashSet;
use std::ffi::{c_char, c_void};
use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::Arc;

use taffy::prelude::*;
use taffy::tree::{Baselines, LayoutInput, LayoutOutput};

// Import the same shared allocator that owns native C++ allocations. Selecting
// its debug DLL here avoids a separate Rust System allocator in timed runs.
#[cfg_attr(debug_assertions, link(name = "mimalloc-debug", kind = "dylib"))]
#[cfg_attr(not(debug_assertions), link(name = "mimalloc", kind = "dylib"))]
extern "C" {
    fn mi_malloc_aligned(size: usize, alignment: usize) -> *mut c_void;
    fn mi_zalloc_aligned(size: usize, alignment: usize) -> *mut c_void;
    fn mi_realloc_aligned(pointer: *mut c_void, new_size: usize, alignment: usize) -> *mut c_void;
    fn mi_free(pointer: *mut c_void);
}

struct Mimalloc;

// Mimalloc's process-wide entry points are thread-safe, honor the requested
// power-of-two alignment, and preserve the old allocation on realloc failure.
// These methods perform no Rust allocation or unwinding of their own.
unsafe impl GlobalAlloc for Mimalloc {
    unsafe fn alloc(&self, layout: AllocationLayout) -> *mut u8 {
        unsafe { mi_malloc_aligned(layout.size(), layout.align()).cast::<u8>() }
    }

    unsafe fn alloc_zeroed(&self, layout: AllocationLayout) -> *mut u8 {
        unsafe { mi_zalloc_aligned(layout.size(), layout.align()).cast::<u8>() }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, _layout: AllocationLayout) {
        unsafe { mi_free(pointer.cast::<c_void>()); }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: AllocationLayout, new_size: usize) -> *mut u8 {
        unsafe { mi_realloc_aligned(pointer.cast::<c_void>(), new_size, layout.align()).cast::<u8>() }
    }
}

#[global_allocator]
static ALLOCATOR: Mimalloc = Mimalloc;

const LP_AUTO: u32 = 0;
const LP_FIXED: u32 = 1;
const LP_GROW: u32 = 2;
const LP_COLUMN: u32 = 0;
const LP_ROW: u32 = 1;
const LP_WRAP: u32 = 2;
const LP_GRID_TWO: u32 = 3;
const LP_UNBOUNDED: u32 = 0;
const LP_AT_MOST: u32 = 1;
const LP_EXACT: u32 = 2;
const LP_MIN_CONTENT: u32 = 3;
const LP_CONTAINER: u32 = 0;
const LP_TEXT: u32 = 1;
const LP_BOX: u32 = 2;
const NO_PARENT: u32 = u32::MAX;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LpAxis {
    pub mode: u32,
    pub value: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LpNode {
    pub id: u64,
    pub parent: u32,
    pub kind: u32,
    pub flow: u32,
    pub absolute: u32,
    pub width: LpAxis,
    pub height: LpAxis,
    pub padding: f32,
    pub gap: f32,
    pub left: f32,
    pub top: f32,
    pub has_min_width: u32,
    pub has_max_width: u32,
    pub min_width: f32,
    pub max_width: f32,
    pub first_column: f32,
    pub font_size: f32,
    pub text: *const c_char,
    pub text_length: u32,
    pub reserved: u32,
    pub content_revision: u64,
    pub measure_revision: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LpMeasureRequest {
    pub text: *const c_char,
    pub text_length: u32,
    pub font_size: f32,
    pub width: f32,
    pub height: f32,
    pub width_mode: u32,
    pub height_mode: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LpMeasured {
    pub width: f32,
    pub height: f32,
    pub baseline: f32,
    pub lines: u32,
}

pub type LpMeasureFn = unsafe extern "C" fn(
    *mut c_void,
    *const LpMeasureRequest,
    *mut LpMeasured,
) -> i32;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LpBox {
    pub id: u64,
    pub parent_id: u64,
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LpStats {
    pub apply_calls: u64,
    pub layouts: u64,
    pub rebuilds: u64,
    pub measure_calls: u64,
    pub input_nodes: u64,
    pub output_boxes: u64,
}

// Match the fixed Windows x64 header, including padding around the text pointer.
#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(size_of::<LpAxis>() == 8);
    assert!(size_of::<LpNode>() == 112);
    assert!(std::mem::offset_of!(LpNode, text) == 80);
    assert!(std::mem::offset_of!(LpNode, content_revision) == 96);
    assert!(std::mem::offset_of!(LpNode, measure_revision) == 104);
    assert!(size_of::<LpMeasureRequest>() == 32);
    assert!(size_of::<LpMeasured>() == 16);
    assert!(size_of::<LpBox>() == 32);
    assert!(size_of::<LpStats>() == 48);
};

struct OwnedNode {
    // The borrowed input pointer is nulled after capture; bytes are owned below.
    node: LpNode,
    text: Arc<[u8]>,
}

struct TextContext {
    id: u64,
    text: Arc<[u8]>,
    font_size: f32,
}

impl OwnedNode {
    fn text_context(&self) -> TextContext {
        TextContext {
            id: self.node.id,
            text: Arc::clone(&self.text),
            font_size: self.node.font_size,
        }
    }
}

struct Solver {
    tree: TaffyTree<TextContext>,
    ids: Vec<NodeId>,
}

struct State {
    measure: LpMeasureFn,
    user_data: *mut c_void,
    nodes: Vec<OwnedNode>,
    solver: Solver,
    root_constraint: Option<Size<f32>>,
    rebuild_required: bool,
    input_revision: u64,
    output_revision: u64,
    published: Vec<LpBox>,
    staging: Vec<LpBox>,
    stats: LpStats,
}

struct Engine {
    state: RefCell<State>,
    // Independent of the solver borrow: rejected reentry can report its error
    // without creating a second reference to the state used by the outer layout.
    error: UnsafeCell<[u8; 768]>,
}

impl Engine {
    fn set_error(&self, message: &str) {
        // Only synchronous, single-threaded access is supported. No borrow of this
        // buffer is held across a callback; lp_last_error returns a borrowed C string.
        let buffer = unsafe { &mut *self.error.get() };
        let count = message.len().min(buffer.len() - 1);
        buffer[..count].copy_from_slice(&message.as_bytes()[..count]);
        buffer[count] = 0;
    }

    fn error_ptr(&self) -> *const c_char {
        self.error.get().cast::<u8>().cast::<c_char>()
    }
}

type ProbeResult<T> = Result<T, String>;

fn taffy_error(error: taffy::TaffyError) -> String {
    format!("Taffy: {error}")
}

fn dimension(axis: LpAxis) -> Dimension {
    if axis.mode == LP_FIXED { length(axis.value) } else { auto() }
}

fn make_style(
    node: &LpNode,
    parent: Option<&LpNode>,
    root_constraint: Option<Size<f32>>,
) -> Style {
    let mut style = Style {
        display: if node.flow == LP_GRID_TWO { Display::Grid } else { Display::Flex },
        box_sizing: BoxSizing::BorderBox,
        size: Size { width: dimension(node.width), height: dimension(node.height) },
        min_size: Size {
            width: length(if node.has_min_width != 0 { node.min_width } else { 0.0 }),
            height: length(0.0),
        },
        max_size: Size {
            width: if node.has_max_width != 0 { length(node.max_width) } else { auto() },
            height: auto(),
        },
        padding: Rect::length(node.padding),
        gap: Size::length(node.gap),
        flex_direction: if node.flow == LP_COLUMN { FlexDirection::Column } else { FlexDirection::Row },
        flex_wrap: if node.flow == LP_WRAP { FlexWrap::Wrap } else { FlexWrap::NoWrap },
        // Fixed spacers and AUTO-height content must overflow a viewport, not shrink.
        flex_shrink: 0.0,
        align_items: Some(if node.flow == LP_WRAP || node.flow == LP_GRID_TWO {
            AlignItems::START
        } else {
            AlignItems::STRETCH
        }),
        align_content: Some(AlignContent::START),
        justify_content: Some(JustifyContent::START),
        ..Style::default()
    };

    if node.flow == LP_GRID_TWO {
        // Native Grid owns track sizing, placement, row maxima and child reflow.
        // minmax(0, 1fr), rather than an automatic intrinsic minimum, enforces
        // the shared owner's explicit first column and remaining second column.
        style.grid_template_columns = vec![length(node.first_column), flex(1.0)];
        // The probe's rows are natural content extents, not a share of the
        // viewport's block size. With our zero item minima, CSS auto tracks
        // would start at zero and divide finite free space between rows.
        // A max-content minimum instead makes each native row own the maximum
        // measured cell height at its resolved column width, even on overflow.
        style.grid_auto_rows = vec![max_content()];
        style.grid_auto_flow = GridAutoFlow::Row;
        style.justify_items = Some(AlignItems::STRETCH);
    }

    if let Some(parent) = parent {
        if node.absolute != 0 {
            // CSS insets start at the parent's padding edge. The probe's offsets
            // start at its content edge. Translate INPUT insets, never output boxes.
            style.position = Position::Absolute;
            style.inset.left = length(parent.padding + node.left);
            style.inset.top = length(parent.padding + node.top);
            if node.width.mode == LP_GROW {
                style.inset.right = length(parent.padding);
            }
            if node.height.mode == LP_GROW {
                style.inset.bottom = length(parent.padding);
            }
        } else if parent.flow == LP_GRID_TWO {
            if node.width.mode == LP_GROW {
                style.justify_self = Some(AlignItems::STRETCH);
            }
            if node.height.mode == LP_GROW {
                style.align_self = Some(AlignItems::STRETCH);
            }
        } else {
            let row = parent.flow == LP_ROW || parent.flow == LP_WRAP;
            let main = if row { node.width } else { node.height };
            let cross = if row { node.height } else { node.width };
            if main.mode == LP_GROW {
                style.flex_grow = 1.0;
                style.flex_basis = length(0.0);
            }
            if cross.mode == LP_GROW {
                style.align_self = Some(AlignItems::STRETCH);
            }
        }
    } else if let Some(size) = root_constraint {
        // Layout constraints supersede the input root's preferred/min/max size.
        style.size = Size { width: length(size.width), height: length(size.height) };
        style.min_size = Size::length(0.0);
        style.max_size = Size::auto();
    }
    style
}

fn validate_axis(axis: LpAxis, index: usize, name: &str) -> ProbeResult<()> {
    if !matches!(axis.mode, LP_AUTO | LP_FIXED | LP_GROW) || !axis.value.is_finite() || axis.value < 0.0 {
        return Err(format!("node {index}: invalid {name} axis mode/value"));
    }
    Ok(())
}

unsafe fn capture_batch(
    input: *const LpNode,
    count: u32,
    previous: &[OwnedNode],
) -> ProbeResult<Vec<OwnedNode>> {
    if count == 0 || input.is_null() || (input as usize) % align_of::<LpNode>() != 0 {
        return Err("apply requires an aligned, nonempty full node batch".into());
    }
    let count = count as usize;
    if count > isize::MAX as usize / size_of::<LpNode>() {
        return Err("node batch exceeds the addressable slice size".into());
    }
    // As with the C++ adapters, non-null pointers must refer to readable storage.
    let input = unsafe { slice::from_raw_parts(input, count) };
    let mut ids = HashSet::with_capacity(count);
    let mut child_counts = vec![0usize; count];
    let mut result = Vec::with_capacity(count);
    for (index, source) in input.iter().enumerate() {
        let node = *source;
        if node.id == 0 || !ids.insert(node.id) {
            return Err(format!("node {index}: identity must be nonzero and unique"));
        }
        if (index == 0 && node.parent != NO_PARENT)
            || (index != 0 && node.parent as usize >= index)
        {
            return Err(format!("node {index}: root must be index 0; each other parent must precede its child"));
        }
        if !matches!(node.kind, LP_CONTAINER | LP_TEXT | LP_BOX) || node.flow > LP_GRID_TWO || node.reserved != 0 {
            return Err(format!("node {index}: unsupported kind, flow, or reserved field"));
        }
        if node.absolute > 1 || node.has_min_width > 1 || node.has_max_width > 1 {
            return Err(format!("node {index}: boolean fields must be 0 or 1"));
        }
        validate_axis(node.width, index, "width")?;
        validate_axis(node.height, index, "height")?;
        for (name, value) in [
            ("padding", node.padding), ("gap", node.gap),
            ("min_width", node.min_width), ("max_width", node.max_width),
            ("first_column", node.first_column), ("font_size", node.font_size),
        ] {
            if !value.is_finite() || value < 0.0 {
                return Err(format!("node {index}: {name} must be finite and nonnegative"));
            }
        }
        if !node.left.is_finite() || !node.top.is_finite() {
            return Err(format!("node {index}: offsets must be finite"));
        }
        if node.has_min_width != 0 && node.has_max_width != 0 && node.min_width > node.max_width {
            return Err(format!("node {index}: min_width exceeds max_width"));
        }
        if node.flow != LP_GRID_TWO && node.first_column != 0.0 {
            return Err(format!("node {index}: first_column is supported only on GRID_TWO"));
        }
        if node.absolute == 0 && (node.left != 0.0 || node.top != 0.0) {
            return Err(format!("node {index}: offsets require absolute positioning"));
        }
        if index == 0 && node.absolute != 0 {
            return Err("root absolute positioning is not supported".into());
        }
        if node.kind == LP_TEXT {
            if node.font_size <= 0.0 || node.flow != LP_COLUMN || node.gap != 0.0 {
                return Err(format!("node {index}: text requires positive font_size and no container-only flow/gap"));
            }
        } else if node.text_length != 0 {
            return Err(format!("node {index}: text bytes are supported only for TEXT nodes"));
        }
        if index != 0 {
            let parent = &input[node.parent as usize];
            if parent.kind == LP_TEXT {
                return Err(format!("node {index}: TEXT nodes cannot have children"));
            }
            if node.absolute != 0 {
                if !(parent.padding + node.left).is_finite() || !(parent.padding + node.top).is_finite() {
                    return Err(format!("node {index}: absolute inset overflows f32"));
                }
            } else {
                child_counts[node.parent as usize] += 1;
            }
        }
        if node.text.is_null() && node.text_length != 0 {
            return Err(format!("node {index}: nonempty text has a null pointer"));
        }
        let bytes = if node.text_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(node.text.cast::<u8>(), node.text_length as usize) }
        };
        // Preserve arbitrary byte slices, including invalid UTF-8: validation belongs
        // to the real text provider and must fail layout, not a successful apply.
        let text = match previous.get(index) {
            Some(old) if old.text.as_ref() == bytes => Arc::clone(&old.text),
            _ => Arc::from(bytes),
        };
        let mut stored = node;
        stored.text = ptr::null();
        result.push(OwnedNode { node: stored, text });
    }
    for (index, source) in input.iter().enumerate() {
        if source.flow == LP_GRID_TWO && child_counts[index] % 2 != 0 {
            return Err(format!("node {index}: GRID_TWO requires label/value pairs of in-flow children"));
        }
    }
    Ok(result)
}

fn build_solver(nodes: &[OwnedNode], root_constraint: Option<Size<f32>>) -> ProbeResult<Solver> {
    let mut tree = TaffyTree::with_capacity(nodes.len());
    tree.disable_rounding();
    let mut ids = Vec::with_capacity(nodes.len());
    for source in nodes {
        let parent = if source.node.parent == NO_PARENT {
            None
        } else {
            Some(&nodes[source.node.parent as usize].node)
        };
        let style = make_style(&source.node, parent, root_constraint);
        let id = if source.node.kind == LP_TEXT {
            tree.new_leaf_with_context(style, source.text_context())
        } else {
            tree.new_leaf(style)
        }.map_err(taffy_error)?;
        ids.push(id);
    }
    for (index, source) in nodes.iter().enumerate().skip(1) {
        tree.add_child(ids[source.node.parent as usize], ids[index]).map_err(taffy_error)?;
    }
    Ok(Solver { tree, ids })
}

impl State {
    fn new(measure: LpMeasureFn, user_data: *mut c_void) -> Self {
        let mut tree = TaffyTree::with_capacity(0);
        tree.disable_rounding();
        Self {
            measure,
            user_data,
            nodes: Vec::new(),
            solver: Solver { tree, ids: Vec::new() },
            root_constraint: None,
            rebuild_required: false,
            input_revision: 0,
            output_revision: 0,
            published: Vec::new(),
            staging: Vec::new(),
            stats: LpStats::default(),
        }
    }

    unsafe fn apply(&mut self, input: *const LpNode, count: u32, revision: u64) -> ProbeResult<()> {
        self.stats.apply_calls = self.stats.apply_calls.saturating_add(1);
        let next = unsafe { capture_batch(input, count, &self.nodes) }?;
        let structure_changed = self.nodes.len() != next.len()
            || self.nodes.iter().zip(&next).any(|(old, new)| {
                old.node.id != new.node.id || old.node.parent != new.node.parent || old.node.kind != new.node.kind
            });
        if structure_changed || self.rebuild_required {
            self.stats.rebuilds = self.stats.rebuilds.saturating_add(1);
            let solver = build_solver(&next, self.root_constraint)?;
            // Drop the entire old tree, including its context SecondaryMap. Taffy's
            // clear/remove do not themselves remove every stored context in this pin.
            self.solver = solver;
        } else {
            // Keep the committed source batch until the complete update succeeds.
            // An internal error/panic causes the next layout to reconstruct it.
            self.rebuild_required = true;
            for (index, source) in next.iter().enumerate() {
                let parent = if source.node.parent == NO_PARENT {
                    None
                } else {
                    Some(&next[source.node.parent as usize].node)
                };
                let id = self.solver.ids[index];
                let style = make_style(&source.node, parent, self.root_constraint);
                if self.solver.tree.style(id).map_err(taffy_error)? != &style {
                    self.solver.tree.set_style(id, style).map_err(taffy_error)?;
                }
                let old = &self.nodes[index];
                let text_changed = old.text.as_ref() != source.text.as_ref()
                    || old.node.font_size != source.node.font_size;
                if source.node.kind == LP_TEXT && text_changed {
                    self.solver.tree.set_node_context(id, Some(source.text_context())).map_err(taffy_error)?;
                } else if old.node.content_revision != source.node.content_revision
                    || old.node.measure_revision != source.node.measure_revision
                {
                    self.solver.tree.mark_dirty(id).map_err(taffy_error)?;
                }
            }
        }
        self.nodes = next;
        self.input_revision = revision;
        self.rebuild_required = false;
        self.stats.input_nodes = self.stats.input_nodes.saturating_add(count as u64);
        Ok(())
    }

    fn layout(&mut self, width: f32, height: f32) -> ProbeResult<()> {
        self.stats.layouts = self.stats.layouts.saturating_add(1);
        if !width.is_finite() || !height.is_finite() || width < 0.0 || height < 0.0 {
            return Err("root constraints must be finite and nonnegative".into());
        }
        if self.nodes.is_empty() {
            return Err("layout requires a successful full apply first".into());
        }
        if self.rebuild_required {
            self.stats.rebuilds = self.stats.rebuilds.saturating_add(1);
            self.solver = build_solver(&self.nodes, self.root_constraint)?;
        }
        self.rebuild_required = true;
        let root = self.solver.ids[0];
        let constraint = Size { width, height };
        if self.root_constraint != Some(constraint) {
            let style = make_style(&self.nodes[0].node, None, Some(constraint));
            if self.solver.tree.style(root).map_err(taffy_error)? != &style {
                self.solver.tree.set_style(root, style).map_err(taffy_error)?;
            }
            self.root_constraint = Some(constraint);
        }

        let mut failure = None;
        let measure = self.measure;
        let user_data = self.user_data;
        let measure_calls = &mut self.stats.measure_calls;
        self.solver.tree.compute_layout_with_measure(
            root,
            Size { width: AvailableSpace::Definite(width), height: AvailableSpace::Definite(height) },
            |inputs, _id, context, style| {
                measure_leaf(inputs, context, style, measure, user_data, measure_calls, &mut failure)
            },
        ).map_err(taffy_error)?;
        if let Some(error) = failure {
            // Failed measurement may have populated Taffy's cache with quarantine
            // sizes. Never publish them; replace the whole tree before any retry.
            return Err(error);
        }

        self.staging.clear();
        self.staging.reserve(self.nodes.len());
        for (index, source) in self.nodes.iter().enumerate() {
            let layout = self.solver.tree.layout(self.solver.ids[index]).map_err(taffy_error)?;
            if !layout.location.x.is_finite() || !layout.location.y.is_finite()
                || !layout.size.width.is_finite() || !layout.size.height.is_finite()
                || layout.size.width < 0.0 || layout.size.height < 0.0
            {
                return Err(format!("Taffy returned invalid geometry for node {}", source.node.id));
            }
            self.staging.push(LpBox {
                id: source.node.id,
                parent_id: if source.node.parent == NO_PARENT {
                    0
                } else {
                    self.nodes[source.node.parent as usize].node.id
                },
                x: layout.location.x,
                y: layout.location.y,
                width: layout.size.width,
                height: layout.size.height,
            });
        }
        std::mem::swap(&mut self.published, &mut self.staging);
        self.output_revision = self.input_revision;
        self.stats.output_boxes = self.stats.output_boxes.saturating_add(self.published.len() as u64);
        self.rebuild_required = false;
        Ok(())
    }
}

fn measure_constraint(space: AvailableSpace, exact: bool) -> ProbeResult<(f32, u32)> {
    match space {
        AvailableSpace::Definite(value) if value.is_finite() => {
            // Taffy can subtract padding from a smaller available content area.
            // A text provider accepts an empty area, not a negative dimension.
            Ok((value.max(0.0), if exact { LP_EXACT } else { LP_AT_MOST }))
        }
        AvailableSpace::Definite(_) => Err("Taffy requested nonfinite text constraints".into()),
        AvailableSpace::MinContent => Ok((0.0, LP_MIN_CONTENT)),
        AvailableSpace::MaxContent => Ok((0.0, LP_UNBOUNDED)),
    }
}

fn measure_leaf(
    inputs: LayoutInput,
    context: Option<&mut TextContext>,
    style: &Style,
    measure: LpMeasureFn,
    user_data: *mut c_void,
    calls: &mut u64,
    failure: &mut Option<String>,
) -> LayoutOutput {
    let exact_width = inputs.known_dimensions.width.is_some() || !style.size.width.is_auto();
    let exact_height = inputs.known_dimensions.height.is_some() || !style.size.height.is_auto();
    let mut baseline = None;
    let output = taffy::compute_leaf_layout(inputs, style, |_, _| 0.0, |known, available| {
        let Some(context) = context else { return Size::ZERO; };
        if failure.is_some() { return Size::ZERO; }
        let measured = (|| -> ProbeResult<Size<f32>> {
            let (width, width_mode) = measure_constraint(available.width, exact_width || known.width.is_some())?;
            // Available block space is not an AUTO text height limit. Preserve
            // intrinsic height at the resolved width; only a known/fixed block
            // size is exact. Otherwise the provider's AT_MOST clamp would turn
            // a viewport or grid-area budget into truncated content geometry.
            let (height, height_mode) = match available.height {
                AvailableSpace::Definite(value)
                    if value.is_finite() && !exact_height && known.height.is_none() => (0.0, LP_UNBOUNDED),
                space => measure_constraint(space, exact_height || known.height.is_some())?,
            };
            let request = LpMeasureRequest {
                text: context.text.as_ptr().cast::<c_char>(),
                text_length: context.text.len() as u32,
                font_size: context.font_size,
                width,
                height,
                width_mode,
                height_mode,
            };
            let mut result = LpMeasured::default();
            *calls = calls.saturating_add(1);
            // Foreign exceptions MUST be contained by the callback implementation.
            // Rust cannot turn a C++ exception crossing extern "C" into a Result.
            let status = unsafe { measure(user_data, &request, &mut result) };
            if status != 0 {
                return Err(format!("text provider failed for node {} (status {status}); old output retained", context.id));
            }
            if !result.width.is_finite() || !result.height.is_finite() || !result.baseline.is_finite()
                || result.width < 0.0 || result.height < 0.0 || result.baseline < 0.0
            {
                return Err(format!("text provider returned invalid dimensions/baseline for node {}", context.id));
            }
            // Probe padding is a definite scalar, so its top inset is known here.
            let padding_top = LengthPercentageAuto::from(style.padding.top)
                .resolve_to_option(0.0, |_, _| 0.0).unwrap_or(0.0);
            baseline = Some(result.baseline + padding_top);
            Ok(Size { width: result.width, height: result.height })
        })();
        match measured {
            Ok(size) => size,
            Err(error) => { *failure = Some(error); Size::ZERO }
        }
    });
    LayoutOutput { baselines: Baselines::from_first(baseline), ..output }
}

unsafe fn access<R: Copy>(
    raw: *mut c_void,
    fallback: R,
    clear_error: bool,
    operation: impl FnOnce(&mut State) -> ProbeResult<R>,
) -> R {
    if raw.is_null() { return fallback; }
    let engine = unsafe { &*raw.cast::<Engine>() };
    let guarded = catch_unwind(AssertUnwindSafe(|| {
        let Ok(mut state) = engine.state.try_borrow_mut() else {
            engine.set_error("callback reentry is forbidden while an engine operation is active");
            return fallback;
        };
        match catch_unwind(AssertUnwindSafe(|| operation(&mut state))) {
            Ok(Ok(value)) => {
                if clear_error { engine.set_error(""); }
                value
            }
            Ok(Err(error)) => { engine.set_error(&error); fallback }
            Err(_) => {
                state.rebuild_required = true;
                engine.set_error("Rust panic contained at probe ABI boundary; next layout rebuilds the committed tree");
                fallback
            }
        }
    }));
    match guarded {
        Ok(value) => value,
        Err(_) => {
            engine.set_error("Rust panic contained while accessing probe state");
            fallback
        }
    }
}

#[no_mangle]
pub extern "C" fn lp_name() -> *const c_char {
    b"taffy-0.14-native\0".as_ptr().cast::<c_char>()
}

#[no_mangle]
pub extern "C" fn lp_create(measure: Option<LpMeasureFn>, user_data: *mut c_void) -> *mut c_void {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(measure) = measure else { return ptr::null_mut(); };
        Box::into_raw(Box::new(Engine {
            state: RefCell::new(State::new(measure, user_data)),
            error: UnsafeCell::new([0; 768]),
        })).cast::<c_void>()
    })).unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn lp_destroy(raw: *mut c_void) {
    if raw.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let engine = unsafe { &*raw.cast::<Engine>() };
        match engine.state.try_borrow_mut() {
            Ok(guard) => drop(guard),
            Err(_) => {
                engine.set_error("destroy rejected during callback reentry; engine remains alive");
                return;
            }
        }
        unsafe { drop(Box::from_raw(raw.cast::<Engine>())); }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn lp_apply(raw: *mut c_void, nodes: *const LpNode, count: u32, revision: u64) -> i32 {
    unsafe { access(raw, -1, true, |state| { state.apply(nodes, count, revision)?; Ok(0) }) }
}

#[no_mangle]
pub unsafe extern "C" fn lp_layout(raw: *mut c_void, width: f32, height: f32) -> i32 {
    unsafe { access(raw, -1, true, |state| { state.layout(width, height)?; Ok(0) }) }
}

#[no_mangle]
pub unsafe extern "C" fn lp_copy_boxes(raw: *mut c_void, output: *mut LpBox, capacity: u32) -> u32 {
    unsafe { access(raw, 0, false, |state| {
        let count = state.published.len();
        if (capacity as usize) < count {
            return Err(format!("copy_boxes requires capacity {count}, got {capacity}; no partial copy"));
        }
        if count != 0 {
            if output.is_null() || (output as usize) % align_of::<LpBox>() != 0 {
                return Err("copy_boxes requires an aligned, writable output buffer".into());
            }
            ptr::copy_nonoverlapping(state.published.as_ptr(), output, count);
        }
        Ok(count as u32)
    }) }
}

#[no_mangle]
pub unsafe extern "C" fn lp_output_revision(raw: *mut c_void) -> u64 {
    unsafe { access(raw, 0, false, |state| Ok(state.output_revision)) }
}

#[no_mangle]
pub unsafe extern "C" fn lp_last_error(raw: *mut c_void) -> *const c_char {
    if raw.is_null() { return b"null probe engine\0".as_ptr().cast::<c_char>(); }
    catch_unwind(AssertUnwindSafe(|| unsafe { (&*raw.cast::<Engine>()).error_ptr() }))
        .unwrap_or(b"Rust panic retrieving probe error\0".as_ptr().cast::<c_char>())
}

#[no_mangle]
pub unsafe extern "C" fn lp_stats(raw: *mut c_void) -> LpStats {
    unsafe { access(raw, LpStats::default(), false, |state| Ok(state.stats)) }
}

#[no_mangle]
pub extern "C" fn lp_sizeof_node() -> usize { size_of::<LpNode>() }

#[no_mangle]
pub extern "C" fn lp_sizeof_box() -> usize { size_of::<LpBox>() }
