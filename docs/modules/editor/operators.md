# 编辑器操作符（设计稿）

状态：O1 设计稿，已由总负责人定稿。第 1 步的无参数部分已实现（[operators.hpp](../../../modules/editor/application/include/editor/operators.hpp)、[operators.cpp](../../../modules/editor/application/src/editor/operators.cpp)、[builtin_operators.cpp](../../../modules/editor/application/src/editor/builtin_operators.cpp)，Control `op.list` / `op.call`）：`view.frame_all`、`view.frame_selected`、`view.reset`、`layout.reset`、`edit.undo`、`edit.redo`、`scene.select`。第 3 步已迁移不带参数的 `layout.close_tab`、`layout.maximize`、`scene.transform_add`、`scene.transform_remove`、`scene.discard_saved_data`。第 2 步已实现参数记录与 3 个带参数的操作符：`scene.add`（String）、`tool.settings`（seed）、`view.axis`（枚举），Create、工具、轴心、吸附步长、坐标轴、选择模式和轴向视图命令都改为调用它们；`editor.ViewSettings` 和 `view.set` 推迟到 4b-3 步骤 A 之后，区域状态记录移到第 3 步。注册表由 Workspace 持有，Context 也由 Workspace 构建；第 4 步之前菜单和快捷区仍按命令登记，`command_enabled` 对已迁移的命令直接取对应操作符的 poll，不再保留第二份可用性判断。其余按第 9 节继续迁移；全部完成后本文改写为当前行为文档。

编辑器的每个操作都是一个注册的操作符。菜单、快捷区、按钮、快捷键和 Control 经同一个入口调用它：`OperatorCall {id, args, context}` → poll → 解码参数 → 来源准入 → execute。UI 只描述“调用哪个操作符、带什么参数”，不写逻辑。只读状态、schema 和能力查询沿现有查询入口，不进入会结束手势的 execute 路径。

## 1. 边界

操作符：改动文档、选择、相机或视图、工具设置、工程、Play、布局或区域可导航状态（第 4 节）的操作。

不是操作符：控件内部的瞬时状态，即悬停、滚动、展开折叠、文本草稿、IME、字段的复制/粘贴，以及拖动过程中的预览帧。

分期：
- O1：注册表、Context、参数、keymap（非 modal 键）、菜单/快捷区/按钮、Control `op.list` / `op.call`，删除 `editor::Command`、`Workspace::command_enabled`、`Application::execute` 的 switch 和旧 Control op。
- O2：invoke/modal，包括 G/R/S、框选拖动、Shift+D 之后的放置、飞行导航、参数行拖动与提交、场景树和资源的拖放、“调整上次操作”面板。
- O3：自有语言调用和定义操作符。

## 2. 注册表 API

```cpp
namespace atelier::editor {
enum class OperatorFlag : std::uint32_t {
    none = 0,
    undo = 1,               // 一次调用最多产生一个场景历史条目（Blender：一个操作符 = 一步撤销）
    history = 2,            // edit.undo / edit.redo：只移动历史游标
    replaces_document = 4,  // 替换文档：execute 只向 Host 请求，替换在 call 返回后执行
};
enum class OperatorKind { query, direct, interactive };
struct OperatorParameters {
    ValueInput value;                     // 记录类型参数：Runtime 现有的 {TypeId, const void*}
    std::span<const PropertyPatch> patch; // @context.object：对象类型约束下的稀疏路径补丁（第 4 节）
};
struct OperatorInfo {
    std::string id;                 // 稳定 ID：小写 "<域>.<动作>"，如 "scene.delete"
    std::wstring title;
    std::string description;
    std::string parameters;         // Runtime 记录类型名；空表示空记录；"@context.object" / "@context.region" 表示动态类型
    std::string result;             // 可选的无引用结果记录类型名，例如新资产的键；引用字段直接拒绝
    OperatorFlag flags{};
    std::function<Result<void>(const Context&)> poll;            // 不可用时 error.message 就是原因
    std::function<Result<void>(OperatorHost&, const Context&, const OperatorParameters&, ValueOutput result)> execute;
    std::function<Result<void>(const Context&, ValueOutput parameters)> seed;  // 可选：用当前状态初始化补丁式参数
    std::function<std::optional<bool>(const Context&, ValueInput parameters)> state; // 可选：菜单的勾选/单选状态
    OperatorKind kind = OperatorKind::direct;
    std::function<bool(ValueInput)> requires_interaction; // 可选：由已解码参数决定是否需要人类交互
    std::string interaction; // 条件说明，供发现与拒绝结果使用
};
class OperatorRegistry {
public:
    Result<void> add(OperatorInfo);                // 重复 ID、ID 语法错误、标志组合非法时拒绝
    const OperatorInfo* find(std::string_view id) const;
    std::span<const OperatorInfo> all() const;
    // 原生便捷入口：参数记录 P 由 atelier_reflect 生成，包装成上面的类型擦除形式。
    template<class P> static OperatorInfo native(std::string id, std::wstring title, std::string description, OperatorFlag,
        std::function<Result<void>(const Context&)> poll,
        std::function<Result<void>(OperatorHost&, const Context&, const P&)> execute);
};
struct OperatorCall { std::string id; Json args = Json::object(); ContextOverride context; };
// poll → seed 或默认构造 → 按参数类型严格解码 args → 来源准入 → execute → 检查历史不变量。
// execute 返回后不再访问 Context。
Result<OperatorReport> call(OperatorRegistry&, OperatorHost&, const Context&, const OperatorCall&);
}
```

- 核心注册项是类型擦除的：参数以 Runtime 现有的 `ValueInput {TypeId, const void*}` 或补丁交给 execute，不另建同义类型，也不要求 C++ lambda 能看到 P。C++ 的 `native<P>` 只是在上面包一层；O3 由语言方法生成同样的 `OperatorInfo`，参数类型是 Source 声明的记录。
- execute 只读 Context（`const Context&`）。需要回传的东西一律经 result 记录（例如 `asset.new_material` 返回新资产键）或 Host（例如新选择由 Session 产生，调用后从文档读取）。
- 参数值的生命周期：`call` 分配按 `TypeInfo::size/alignment` 对齐的存储，`initialize_value` 默认构造。类型 `contains_references` 时，紧接着 `root_value`，让 GC 追踪其中的引用；结束时先撤根，再 `destroy_value`。弱引用不保活目标，execute 读弱引用一律用 `try_acquire`，并处理目标在同一次调用里已被删除的情况。
- 参数类型发布在文档的 Runtime，`OperatorInfo` 只存类型名，每次调用在当前文档里解析 TypeId。只有 document 角色的 Session 发布 `editor.*` 参数类型：编辑器宿主构造文档 Session 时把这组类型定义交给 Session，与 World 类型一起注册。Play 角色（P0 的 `enter_play`）、独立运行器和以后的专用服务器不传这组定义，不会带上 `editor.*`。
- Runtime 没有类型可见性控制，Source 绑定会把所有已发布的、不含引用的类型纳入目录。编辑器参数类型进入文档 Runtime 后，靠 Source 绑定目录里的一条最小规则隔开：`editor.` 前缀保留给编辑器，工程 Source 既不能引用也不能声明这类类型（引用时按类型未登记报错，声明时报 "reserved for editor types"）。O3 再换成按 Program 划分的可见域。编辑器不自行添加机制。
- `OperatorHost` 是 execute 访问宿主能力的接口（相机、视口工具、Dock、Play 进程、文件选择器、状态栏、文档替换请求），由 Application 实现。Context 只装数据，Host 只装能力；O3 的语言操作符只拿 Context 和 Runtime。
- `state` 取代 `Workspace::command_item` 中按命令硬写的单选/勾选判断：单选项在“用这组参数调用不会改变任何东西”时为选中，勾选项返回当前布尔值。
- 菜单的 `OperatorBinding` 拥有不可变调用；第一次查询按原解码规则准备参数，无 seed、无引用的参数记录在该绑定内复用，状态结果仍每次从当前 Context 计算。seeded 参数（例如 `tool.settings`）继续每次以当前状态填充未指定字段；含引用的记录也不长期保留。绑定仅用于同一注册表和文档，不保存注册表容器元素地址，随菜单退役释放；文档切换和宿主关闭先清理绑定，再关闭旧文档。执行仍经 `call` 重新校验、poll、解码和历史检查，不使用查询缓存。
- 执行结果 `OperatorReport {status, message, result}`：status 为 finished、cancelled、failed 或 interaction_required；poll 失败时 status=failed、message 为原因。interaction_required 表示外部来源不能启动该人类交互，拒绝发生在 `end_interaction` 和 execute 之前，不取消已有手势或修改文档。execute 的原始错误仍经 Result 传播。

### 历史不变量

`call` 在 execute 前后比较场景 Session 的撤销计数 u、重做计数 r 和历史游标，按标志检查：

| 标志 | 要求 |
| --- | --- |
| `undo` | 最多新增一个历史条目：u 不变，或 u 加 1 且 r 归零 |
| `history` | 条目总数 u+r 不变，游标最多移动 1 |
| `replaces_document` | 不检查（文档被替换，历史随之清零） |
| 无以上标志 | u 和 r 都不变 |

检查依据是 Session 的 `recorded()`（累计记录的历史条目数，历史达到上限时淘汰旧条目也不会减少）加上撤销和重做计数。这是开发和发布构建中都生效的运行时检查：违反时 `call` 返回 failed，附带一条写明操作符 ID、标志和前后计数的诊断（开发构建可以额外断言）。验收在 dev 和 release 下都把它当作失败。迁移中发现的、会产生多个条目的现有动作，都要合成一个条目。目前已知 `unparent_selected` 按每个 Entity 各调一次 `reparent`，多选时会产生多个条目，`scene.parent_set` 需要 Session 提供批量改父级的单条目事务（属于 Session，若要改 `World::reparent` 的签名，先告知 World 负责人）；合不了的报总负责人。

这里的单条目事务是历史分组，不是整组发布的全有或全无。`Session::reparent` 按输入顺序逐对象调用现有 World 路径，在首个业务拒绝或通知失败处停止，只把实际已提交的移动记为一个条目；首项拒绝、空输入和全部 no-op 不新增历史。后项拒绝返回原始 Error，先前提交仍保留并可一次 Undo/Redo；通知失败保留已提交事实、完整 Error 与 World 既有停止语义，不继续执行后项或用清理错误覆盖原始 cause。Undo 按移动的逆序恢复父级、兄弟位置和局部姿态，Redo 按正序回放，Live 转发采用同样顺序和已记录的姿态。显式 index 是第一个目标位置，后续对象依次占用后续位置；默认 append 仍遵循 World 对同父级 no-op 的定义。

### 替换文档

`scene.open`、`project.new`、`project.open`、`project.rebuild_types` 会替换 Session。Context 持有 `Session&`，而 `publish_document` 会销毁旧 Session，所以这类操作符在 execute 里只调用 `host.replace_document(request)` 登记请求。`call` 在 execute 之后不再碰 Context；Application 在 `call` 返回后、仍在同一次宿主调用里执行替换，并把替换的成败并入返回给调用方的报告。Control 的 `op.call` 因此能拿到“打开失败”的原因。示例沿普通 `project.open` / `scene.open`，没有专用操作符。

## 3. Context

```cpp
struct Selection { ObjectId active;  /* active Entity */ ObjectId object; /* 被检视的对象：Entity、组件或材质 */ std::vector<ObjectId> entities; };
enum class RegionKind : std::uint32_t { none, viewport, scene_tree, structure, parameters, assets, code, output, world, controls };
struct Region { RegionKind kind{}; ui::ControlKey instance{}; };   // instance 是 Dock 页的键，现在每种只有一个
struct ViewportContext {
    CameraState camera; ViewportView view;          // 已接受帧的投影与范围
    math::Vector3 cursor; math::Quaternion cursor_rotation;
    std::optional<math::Vector2> pointer;           // 视口物理像素
    std::optional<Hit> hit;                         // 由 pointer 经真实射线求得 {entity, position, normal}
};
struct Context {
    Session& document; Project* project;
    Selection selection;
    Region region;
    std::optional<ViewportContext> viewport;        // 仅当 region 为 viewport
    std::string asset;                              // 资源浏览器的活动资产或被检视的材质资产键
    std::filesystem::path source;                   // 代码页的活动文档
    bool scene_available{true};                     // 工程场景是否可用；Play 运行期间的修改照常进入文档并转发给 Play
    bool interactive{};                             // UI/keymap 调用为 true；Control 与语言为 false
};
```

- 实现中 Context 另带 `source_history` 和 `workbench_history`：代码页活动文档与 Controls 示例各自能否撤销、重做。`edit.undo` / `edit.redo` 的 poll 因此只读 Context，Control 覆盖 region 时也按覆盖后的区域判断。
- 操作符只读 Context，不读 Session 内部的选择。因此 Session 中按“当前选择”工作的方法（`erase_entities`、`duplicate_selected`、`erase_selected`、clear/snap 等）改为接收显式目标；仍然只有一份实现。
- UI 调用：由获得焦点的区域填充。视口在指针位于其上且没有文本控件占用键盘时是活动区域（保持现有行为），否则是获得焦点的 Dock 页。第 5 步 keymap 落地之前，UI 和 Control 的基础 Context 都取获得焦点的 Dock 页，与现有撤销路由一致。
- Control 覆盖（`op.call` 的 `context`），只在本次调用内有效，不改文档选择：
  ```json
  {"selection":{"entities":["h1","h2"],"active":"h1","object":"h3"},
   "region":"viewport","viewport":{"cursor":[0,1,0],"pointer":[640,360]},"asset":"Content/M.material"}
  ```
  校验：句柄必须是本文档的存活成员，active 属于 entities，object 属于 active，region 必须存在，pointer 在视口范围内，数值有限。hit 不能直接给，只能由 pointer 求出。
- 选择本身就是一个操作符：`scene.select` 把 Context 的选择设为文档选择。Control 的 `scene.select {entities, active, object}` 改为 `op.call {"id":"scene.select","context":{"selection":{...}}}`。这样不需要“引用数组”参数，Runtime 记录本来也不能包含引用序列。

## 4. 参数类型

参数是原生登记的 Runtime 记录：头文件里用 `ASTRUCT/AFIELD/AENUM` 声明，由 `atelier_reflect` 生成，范围用 `Min/Max/HardMin/HardMax`。默认值就是类型的默认构造。由它派生 `op.list` 的 schema、keymap 和菜单参数的校验，以后也派生“调整上次操作”面板。`modules/editor/core/include/editor/operator_parameters.hpp` 使用编辑器反射生成组。`editor.MaterialEntriesJson` 仅承载 assets 已有权威材质 entries JSON：编辑器 codec 保留原节点，创建时复用 `decode_entries`，schema 固定为 `array / material.entries`，格式见 [Control 合同](control.md)；不开放通用 JSON 参数或记录数组。

已实现的记录（`modules/editor/core/include/editor/operator_parameters.hpp`，由 `operator_parameters.cpp` 的 `register_editor_types` 登记）：

| 记录 | 操作符 | 字段 |
| --- | --- | --- |
| `editor.SceneAdd` | `scene.add` | kind（`editor.AddKind`：entity、mesh、procedural_generator、point、rectangle、directional、camera）、mesh（String，空为第一个内置网格）、graph（String，`.procgraph` 资产键） |
| `editor.AssignProceduralGraph` | `procedural.assign_graph` | component（弱 `world.ProceduralGeneratorComponent` 引用，默认 null 使用当前选择）、asset（String，绑定到生成器的 `.procgraph` 资产键，空字符串清空） |
| `editor.ProceduralBake` | `procedural.bake` | entity（弱 `world.Entity` 引用，默认 null）、component（弱 `world.ProceduralGeneratorComponent` 引用，默认 null）；将目标生成器的 `CopyToPoints` 实例烘焙为可单步撤销的 `StaticMeshEntity` 子树。只接受该生成器本次成功发布的结果；求值失败时返回原始错误，视口可继续显示上次输出。含剪切的实例矩阵需先在图中 `Realize` |
| `editor.SceneParent` | `scene.parent_set` | parent（弱 Entity 引用，默认 null）、keep_world（默认 true）、index（默认 -1，表示 append） |
| `editor.SceneSpaceReference` | `scene.space_reference_set` | origin（弱 Entity 引用，null 继承）、world_space（默认 false；true 时 origin 必须 null，显式使用 World 空间）、keep_world（默认 true） |
| `editor.SceneSelectAll` | `scene.select_all` | action（select、deselect、invert、toggle；toggle 保留视口 A 的全选/清空行为） |
| `editor.SceneDelete` | `scene.delete` | target（object、entities）；组件或子树目标来自 Context，未被删除的文档选择保留 |
| `editor.SceneDuplicate` | `scene.duplicate` | place（默认 false）；视口上下文可进入现有放置，取消放置保留原位副本 |
| `editor.SceneAddComponent` | `scene.add_component` | type（完整组件类型名），目标为 Context 的 active Entity；所需 Transform 与组件共用一个历史条目 |
| `editor.SceneRename` | `scene.rename` | name（String）；非交互调用必须非空，交互调用可开始现有内联编辑，文本提交不再弹提示 |
| `editor.SceneTransform` | `scene.transform_set` | position、rotation（四元数）、scale；完整局部变换，默认单位变换，必要时创建空间能力 |
| `editor.SceneTransformClear` | `scene.transform_clear` | channel（location、rotation、scale），只改显式选择的空间根 |
| `editor.SceneSnap` | `scene.snap_selection` | to（grid、cursor），光标目标和网格步长来自 Context |
| `editor.LayoutShow` | `layout.show` | tab（`editor.RegionKind`；none 不是可显示的页签） |
| `editor.LayoutCycle` | `layout.cycle_tab` | direction（-1 前一个、1 后一个、0 不移动），作用于 Context 区域所在窗格 |
| `editor.ToolSettings` | `tool.settings`（seed） | tool、pivot、grid_snap、cursor_surface、grid_spacing、select_mode、orientation、snap_move、snap_rotate、snap_scale、rotation_step、scale_step |
| `editor.ToolToggle` | `tool.toggle` | setting（grid_snap、cursor_surface、snap_move、snap_rotate、snap_scale） |
| `editor.ViewAxis` | `view.axis` | view（`editor.CameraView`） |
| `editor.ViewSettings`（描述型） | `view.set`（seed） | 由 `ViewOptionInfo` 的完整表生成；未指定项保留当前值，World 作者值从当前文档取种子 |
| `editor.NewProceduralGraph` | `asset.new_procedural_graph` | path（空路径在交互模式下弹出选择器）、source（可选初始图 JSON）；结果记录 `editor.AssetResult` 返回 `asset` 键 |
| `editor.GraphOpen` / `editor.GraphSave` | `graph.open`、`graph.save` | asset（`.procgraph` 资产键或临时图键；留空时取当前活动图 / Network 图） |
| `editor.GraphLayout` | `graph.layout` | asset（留空取当前图；按最长路径拓扑层级与重心排序自上而下排布全部节点，记一步图撤销） |
| `editor.GraphAddNode` | `graph.add_node` | asset、type（`procedural.Node` 派生类型名）、name（可选节点名）、position（二维坐标）；结果记录 `editor.GraphNodeResult` |
| `editor.GraphDeleteNode` | `graph.delete_node` | asset、name（省略时批量删除画布选中节点，并记一步撤销） |
| `editor.GraphDuplicateNodes` | `graph.duplicate_nodes` | asset、node（省略时批量复制选中节点，保留属性、外观及上游/内部连线）、offset（默认 `[36, 36]`）；结果记录 `editor.GraphNodeResult` |
| `editor.GraphSetDisplay` / `editor.GraphSetTemplate` | `graph.set_display`、`graph.set_template` | asset、node（省略或空串时清除对应标记） |
| `editor.GraphSetBypass` | `graph.set_bypass` | asset、node（留空取选中节点）、bypass |
| `editor.GraphSetFrozen` | `graph.set_frozen` | asset、node（留空取选中节点）、frozen |
| `editor.GraphSetAppearance` | `graph.set_appearance` | asset、node（省略时批量作用于选中节点集）、shape、color（`#RRGGBB` 或 `default`）、comment |
| `editor.GraphSelect` | `graph.select` | asset、node（留空时选中图根 `Graph.seed`） |
| `editor.GraphRenameNode` | `graph.rename_node` | asset、node（为空取选中节点）、name |
| `editor.GraphMoveNode` | `graph.move_node` | asset、node、position |
| `editor.GraphConnect` / `editor.GraphDisconnect` | `graph.connect`、`graph.disconnect` | asset、from_node、from_port、to_node、to_port |

- 只有编辑器自己的文档 Session（`SessionRole::editor`）在引擎类型之后、工程 Source 构建之前登记这些类型；Play、独立运行和其他 Session 不登记。工程 Source 既看不到也不能声明 `editor.*` 类型（运行时的前缀规则），`project_integration` 在编辑器文档里断言这一点。
- `tool.settings` 的参数是 `editor.ToolSettings`。各编辑器枚举的底层类型为 `uint32_t`，生成器支持 int32/uint32。工具、轴心、选择模式和坐标轴不进持久格式；`CameraView` 以序号出现在 Control 的编辑器相机 JSON 和 repro 包中。Play 使用场景主相机，不接收编辑器相机参数。
- `editor.AddKind` 没有 sky：天光是 Environment 组件的属性，由 World Settings（`set_environment`）编辑，不是新建的 Entity。
- 含引用参数记录构造后立即进入 Runtime 根集，解码和执行结束后先撤根再析构。弱引用不会保活目标；引用 token 由调用方解析，Control 沿用本文档句柄，null 由公共调用入口解析。
- 补丁式操作符（`tool.settings`、`view.set`、`region.set`）用 `seed` 从当前状态初始化参数，args 只写出现的叶。因此 `snap_grid_05` 就是 `tool.settings {"grid_spacing":0.5}`。
- `tool.toggle` 不幂等，只给菜单和快捷键这类交互绑定使用；`op.list` 的说明里写明这一点，Control 和脚本应当用 `tool.settings` 写绝对值。
- JSON → 参数值：调用运行时负责人的 R5-6a 共用叶解码（建在 P0 的 `read_properties` 之上：写入目标为“对象 | 调用方记录存储”，含 String 叶），不另写解码器。操作符一律用它的严格模式：未知字段、类型不符、越过硬边界都报错（越界按与 `constrain` 结果比较判断），不夹值；场景加载仍走宽容模式。
- 描述型记录不能带默认值，`editor.ViewSettings` 默认构造时全为零。seed 式操作符在 `op.list` 中不输出默认值，改为标注 `"seeded": true`（缺省表示保持当前值）。
- 引用参数：确有第二个目标时，才用 `Weak="world.Entity"` 的 ReferenceSlot，例如 `scene.parent_set` 的 parent。Control 用句柄字符串表示。主要目标一律来自 Context。

### `view.set`：由表格生成的描述型记录

`view.set` 的参数类型 `editor.ViewSettings` 是描述型记录，启动时由现有 `ViewOptionInfo` 表生成。表仍然是唯一的定义：表中的种类、范围和枚举值成为字段和 AENUM，类型名固定为 `editor.ViewSettings`，字段顺序就是表的顺序，确定且稳定。验收断言 `op.list` 中 `view.set` 的 schema 与表逐项一致。World 作者值是 Environment 组件的属性：`Session::set_environment` 比对候选中的 World 选项，把变化的项写成一次组写入，记一条普通的属性编辑历史；没有 Environment 时同一历史项先创建它。LightingChange 已删除。

### `scene.set_properties`：对象类型约束下的稀疏补丁

参数类型是 `@context.object`，即被检视对象自身的 Runtime 类型（组件、材质实例或材质资产对象）。`op.list` 中它的 schema 由该类型的字段目录生成（`{"dynamic":"context.object"}`，取实际 schema 时用 `editor.inspect` 给出该对象的类型）。

参数不存成整个对象值的拷贝：组件可能含引用、数组或不可复制的原生成员，整值拷贝既贵又可能做不到。args 按对象类型解码成稀疏路径补丁 `PropertyPatch {path, Scalar}` 列表，规则与 P0 的 `set` 相同：未知字段和类型不符报错，越过硬边界报错。所有叶解码后交给现有 `World::write(object, paths, values)` 组写入，在组内写完后统一校验，避免 `body` 等记录的中间叶状态违反跨字段规则；拒绝时由同一机制恢复。Session 只记一个 `FieldGroupChange`，Undo/Redo 也沿该组写入，不另建整对象拷贝或逐叶历史。

Transform 是例外：Entity（包括 Source 派生 Entity）实际继承的 position/rotation/scale 路径只由 `scene.transform_set`、`scene.transform_clear`、`scene.transform_remove` 等专用 Transform 操作符负责。`scene.set_properties` 对纯 Transform 或含 Transform 的混合补丁整批拒绝，发生在任何作者写入、取消现有编辑或新增历史之前；诊断指向专用 Transform 操作符。判断按实际类型和字段身份，不误拒普通业务字段的同名属性。派生 Entity 的非 Transform Scalar 补丁仍可组写入。本接口不支持依赖 Transform 与业务字段同时改变才能满足 validator 的组合修改，分成两次调用不是等价替代。

### 区域状态记录：`region.set`

每种区域的可导航状态是一个 Runtime 记录，由同一个补丁式操作符 `region.set` 修改。参数类型是 `@context.region`，即 Context 区域对应的状态记录；seed 取当前状态。`region.set` 不进撤销历史（无标志，撤销和重做计数都不变）。不为每个按钮各开一个操作符。

| 区域 | 状态记录 | 字段 |
| --- | --- | --- |
| assets | `editor.AssetBrowserState` | location（project/engine）、path、filter、view |
| world | `editor.WorldPanelState` | page、slot |
| code | `editor.CodeState` | file |
| controls | `editor.ControlsState` | mode |

资源浏览器的路径菜单、过滤和视图菜单、World 页的槽位菜单、代码页的文件菜单和 Controls 页的模式菜单，都是带不同参数的 `region.set`。Control 可以用 `{"id":"region.set","context":{"region":"assets"},"args":{"path":"Content/Meshes"}}` 浏览资源。新增区域状态时只需增加一个记录类型。

## 5. Keymap

```cpp
enum class KeyPhase : std::uint8_t { before_controls, after_controls };
struct KeyChord { std::uint32_t key; bool control{}, shift{}, alt{}; bool repeat{}; };
struct KeymapEntry { RegionKind region; KeyChord chord; KeyPhase phase; std::string_view op; std::string_view args; /* JSON */ };
```

- 内置表放在 `modules/editor/application/src/editor/keymap.cpp`，保存真实 `OperatorCall`。启动时只检查操作符 ID 已注册，未知 ID 明确报错；参数在调用时严格解码，不做无宿主执行或参数试运行。
- 以后增加用户覆盖文件时，用户文件出错的条目只给诊断，并回退到内置表的对应条目，不拒绝启动。现在不实现。
- 查找顺序与现有行为一致：
  1. 区域为视口、没有文本控件占用键盘时，先查视口表（目前视口键都在 UI 输入之前处理）。
  2. 全局表中的 `before_controls` 项：Ctrl+S/O、F5/F6/F8、Shift+F8、Ctrl+Tab、Ctrl+B/W。它们即使在文本框中也生效。
  3. 交给 UI 控件。
  4. 控件没有消费时，先查焦点区域的 `after_controls` 表，再查全局表：Ctrl+Z/Y、Delete、F2、Ctrl/Shift+D、F。
- 同一个键在不同区域解析到不同操作符，例如 Alt+P：场景树里是 `scene.parent_set {"parent":null}`，其他区域保留菜单助记键；X：视口里打开删除菜单。
- 菜单上显示的快捷键文字由 keymap 生成：找 `op` 与 `args` 都相同的条目，优先取菜单所在区域，再取全局。只有 modal 的 G/R/S 在 O2 之前不显示快捷键文字，提示写进说明。
- 要改为查表的硬写分支：`application.cpp` 的 `input()`（约 1120–1195 行）、`viewport.cpp` 的 `viewport_input()`（数字键盘、Home、小数点、Shift+Tab、Q、W、X、Z、Shift+A、`、Shift+D、Ctrl+Z/Y），以及 `viewport_tool.cpp` 的 `key()` 里非 modal 的部分（A、Alt+A、Ctrl+I、Alt+G/R/S）。G/R/S、B 的拖动和 Shift+` 飞行留给 O2。

### 查询、直接执行与人类交互

- `query` 表示只读状态、schema、能力查询，沿 `OperatorBinding::state`、schema 和 Control 的既有查询入口；注册表不接受 query execute 项。查询不结束交互，也不修改作者值或历史。
- `direct` 由完整参数直接执行；合法默认值仍有效。UI、keymap 与 Control 共用执行体。`Context.interactive` 表示人类调用来源，不代表本次调用类别，Control 的 context override 不能改它。
- `interactive` 需要人类完成手势、内联输入或对话框。`view.select_box`、`ui.menu`、`app.about`、`app.quit` 固定属于此类。begin/preview/commit/cancel 仍由原来的局部拥有者管理，不提供外部分步交互协议。
- 双态项保留同一个 ID：`scene.duplicate` 的 `place=true` 需要交互，false（默认）直接复制；scene/asset rename 的空 name 需要内联编辑；路径选择项的空 path 需要选择器；`scene.save` 仅在 `as_new=true && path.empty()` 时需要选择器。元数据条件读取已解码记录，不重复解释 JSON，不按命令名维护准入名单。
- context/poll 与严格参数解码的原顺序不变。strict decode 能发现的错误优先于来源准入；execute 内的业务校验不前移。例如空 path 且 entries 内容非法的 `asset.new_material`，先因需交互返回 interaction_required，不先校验 entries。已有保存冲突和未保存内容保护继续有效。
- 外部来源遇到交互调用返回 interaction_required 和操作 ID/条件说明，不静默改参数，不进入捕获、放置、选择器或退出确认。原生对话框的 UI 验收仍使用 `ExpectedNativeDialogs` 声明预期范围。

## 6. 菜单、快捷区与按钮

- `ui::MenuItem` 和快捷区项不再把 `Command` 值当作键，而是在 Workspace 的绑定表里登记一项 `OperatorCall`（新增 `Binding::Kind::operator_call`，做法与现有的 property_choices_ 相同），控件键按次分配。菜单项也可以带 Context 覆盖，相当于 Blender 的 `context_pointer_set`，例如路径导航项是 `scene.select`，覆盖为该成员。
- 可用性取自 poll，勾选/单选取自 state，标题取自操作符或调用处的文字。
- 覆盖范围：主菜单、视口头部菜单与饼菜单、场景树行菜单、添加组件菜单、着色/参考线/Cavity 菜单、World 页菜单、材质槽菜单、资源浏览器的操作/过滤/视图/路径菜单、代码页文件菜单、Dock 页菜单、Controls 页模式菜单，以及快捷区按钮和面板中改动文档或工程的按钮（移除组件、清除材质槽、Fit 等）。
- 不经操作符的只有字段右键菜单里的“Copy Value / Paste Value / Revert to Default”：它们作用于控件草稿，再走字段提交，随参数行一起在 O2 迁移。

## 7. Control

- `op.list` 返回每个操作符的 `{id, title, description, kinds, flags, parameters: JSON schema, result: JSON schema}`，以及 Context 覆盖的 schema。固定项 kinds 为 `["direct"]` 或 `["interactive"]`，双态项为 `["direct","interactive"]` 并带 `interaction` 条件说明。参数 schema 完全由 Runtime 的 TypeInfo/FieldInfo 生成：`{"type":"object","additionalProperties":false,"title":记录名,"properties":{…}}`；每个字段给出 `type`（boolean/integer/number/string）或 `enum`（成员名）和 `description`，硬边界写成 `minimum`/`maximum`，界面范围写成 `ui_min`/`ui_max`。非 seed 式操作符带 `default`；seed 式操作符不带默认值，改标 `"seeded": true`（缺省表示保持当前值）。无参数的操作符是空的 properties，没有 title。`@context.object` 写成 `{"dynamic":"context.object"}`。
- `op.call {id, args, context}` 需要状态令牌，返回业务结果和调用后的选择；Control 固定为外部来源，仅接受本次类别为 direct 的调用。poll 拒绝映射为 `error.code: "poll_failed"`，来源准入拒绝为 `"interaction_required"`。`capabilities.operations` 给出各入口的 kind，op.call 另有 `accepted_operator_kinds:["direct"]`；类别与 requires_state_token 分开，job.cancel 不要求令牌但仍为 direct。协议外层状态与错误形状以 [Control 合同](control.md)为准。
- 删除所有已迁移的专用 op，不保留兼容名字。Control 只保留 `capabilities`、`editor.inspect`（并入 `material.inspect`，新增 `detail:"asset"`）、`render.*`、`job.*`、`repro.*`。验收客户端 `acceptance/editor/control.cpp` 和 `control.md` 一起改。

## 8. 对照表

### 旧命令 → 操作符

| 旧命令 | 操作符与参数 |
| --- | --- |
| new_project / open_project | `project.new {path}` / `project.open {path}`（空 path 需要人类选择） |
| open_scene | `scene.open {path}`（空 path 需要人类选择） |
| save_scene / save_scene_as | `scene.save {path, as_new}`（as_new 且空 path 需要人类选择） |
| save_document / save_all | `file.save`（按区域保存场景或源码）/ `file.save_all` |
| import_mesh / import_texture | `asset.import {kind: mesh\|texture, path}`（空 path 需要人类选择） |
| create_material | `asset.new_material {path}`（空 path 需要人类选择） |
| refresh_assets | `asset.refresh` |
| open_source / edit_ui_source | `source.open {path}`（空 path 需要人类选择）/ `source.open_editor_ui` |
| close_source / reload_source | `source.close` / `source.reload` |
| new_source / new_project_ui / edit_project_ui | `source.new_component {path}` / `source.new_project_ui {path}` / `source.open_project_ui` |
| build_project / rebuild_project | `project.build {rebuild: false\|true}` |
| run_project / stop_project / show_project_ui | `play.start` / `play.stop` / `play.preview_ui` |
| exit / about | `app.quit` / `app.about`（仅人类交互） |
| undo / redo | `edit.undo` / `edit.redo`（按区域选择历史：源码、材质资产、Controls 示例或场景） |
| create_entity / create_static_mesh / create_point/rectangle/directional_light | `scene.add {kind, mesh}`（create_sky_light 由 4b-3 步骤 A 删除，见第 4 节） |
| duplicate_selected | `scene.duplicate {place}`（false 默认直接复制；视口 Shift+D 的 true 启动人类放置） |
| erase_selected / erase_entities | `scene.delete {target: object\|entities}` |
| rename_selected | `scene.rename {name}`（空 name 需要人类内联重命名） |
| unparent_selected | `scene.parent_set {parent: null, keep_world: true}` |
| add_transform / remove_transform | `scene.transform_add` / `scene.transform_remove` |
| clear_location / rotation / scale | `scene.transform_clear {channel}` |
| selection_grid / selection_cursor | `scene.snap_selection {to: grid\|cursor}` |
| discard_saved_data | `scene.discard_saved_data` |
| select_all / select_none / select_invert | `scene.select_all {action: select\|deselect\|invert}` |
| select_box | `view.select_box`（进入现有框选；拖动部分 O2） |
| tool_* / pivot_* / snap_grid_* / snap_rotation_* / orientation_* / select_mode_* | `tool.settings {...}`，例如 `{"tool":"move"}`、`{"grid_spacing":0.5}` |
| snap_toggle / snap_affect_* / cursor_surface | `tool.toggle {setting: grid_snap\|snap_move\|snap_rotate\|snap_scale\|cursor_surface}` |
| cursor_origin / cursor_selected / cursor_grid | `cursor.reset` / `cursor.to_selection` / `cursor.to_grid` |
| view_perspective / top / front / right / bottom / back / left | `view.axis {view}` |
| focus / frame_all / reset_camera | `view.frame_selected` / `view.frame_all` / `view.reset` |
| create_camera | `scene.add {kind: camera}`，不自动选为主相机 |
| set_main_camera / clear_main_camera | `scene.main_camera_set` / `scene.main_camera_clear`（undo；Set 使用 Context 的 active Entity） |
| view_from_camera | `view.from_camera`（一次性复制 Context active 相机的完整位姿和投影，无作者历史） |
| viewport_options / show_fps | `view.toggle {option: tool_options\|fps}` |
| fit_probe_scene / fit_probe_scene_focus | `view.lighting_fit {mode: scene\|scene_focus}`（undo） |
| show_scene / parameters / entities / structure / assets / code / output / controls / world_settings | `layout.show {tab}` |
| previous_tab / next_tab / close_tab / maximize_pane / reset_layout | `layout.cycle_tab {direction}` / `layout.close_tab` / `layout.maximize` / `layout.reset` |

### 现在不是命令的 UI 动作 → 操作符

| 现状 | 操作符 |
| --- | --- |
| 视口 W / X / Z / Shift+A / ` 打开的菜单和饼菜单 | `ui.menu {menu: tools\|delete\|shading\|add\|axis}` |
| 着色饼菜单、着色/边线/拓扑、参考线、Cavity | `view.set {...}`，例如 `{"shading":"matcap"}` |
| 路径导航、场景树点选 | `scene.select`（Context 覆盖） |
| 添加组件 | `scene.add_component {type}` |
| 字段的枚举下拉与布尔开关 | `scene.set_properties`（Context object 为目标；带预览的字段编辑留给 O2） |
| 材质槽菜单：指派、默认、编辑共享材质 | 指派/默认保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。`asset.open` 未迁移：单一 UI 路径，出现 keymap/Control/语言调用方时再迁 |
| 贴图槽菜单 | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移 |
| 资源操作菜单：Open / Add to Scene / Rename | `scene.add {kind: mesh, mesh}` / `asset.rename {name}`（空 name 需要人类内联重命名）；`asset.open` 未迁移：单一 UI 路径，出现 keymap/Control/语言调用方时再迁 |
| 资源过滤、视图、路径 | `assets.display` / `assets.browse` 未迁移：单一 UI 路径，出现 keymap/Control/语言调用方时再迁 |
| World 页体积槽位、代码页文件切换、Controls 模式 | `world.select_slot` / `source.focus` / `workbench.set` 未迁移：单一 UI 路径，出现 keymap/Control/语言调用方时再迁 |
| Dock 页菜单 | `layout.*` |

### 旧 Control op → 操作符

下表列出 Control 的操作符映射和保留的原生入口，现行调用形状以 [Control 合同](control.md)为准。材质采用 MS/MI 的 typed entry、稳定声明 path 和独立父链，不转为 Scalar 属性。

| 旧 op | 新调用 |
| --- | --- |
| project.open / scene.open / scene.save | `project.open` / `scene.open` / `scene.save` |
| scene.create {kind, mesh} | `scene.add {kind, mesh}` |
| scene.select {entities, active, object} | `scene.select`，选择放在 context |
| scene.rename {object, name} | `scene.rename {name}`，context.selection.active |
| scene.set_property {object, property, value} | `scene.set_properties {<字段路径>: 值}`，context.selection.object |
| scene.transform / remove_transform | `scene.transform_set {position, rotation, scale}` / `scene.transform_remove` |
| scene.set_space_reference {object, origin, keep_world} | `scene.space_reference_set {origin, world_space, keep_world}`；null origin 继承，显式 World 用 world_space=true，与指定 Entity 分开表达 |
| scene.reparent {object, parent, keep_world, index} | `scene.parent_set {parent, keep_world, index}` |
| scene.add_component / remove_component | `scene.add_component {type}` / `scene.delete {target: object}` |
| scene.delete / undo / redo | `scene.delete {target: entities}` / `edit.undo` / `edit.redo` |
| camera.set / camera.look_at | `view.camera_set {state: editor.CameraState}` / `view.look_at {eye, target}`；相机 view 用枚举名，向量用 `{x,y,z}`，inspect 与参数共用 Runtime 编解码 |
| view.set {patch} / view.layout {layout} | `view.set {...}` / `view.lighting_fit {mode}` |
| material.create / texture.import / mesh.import | `asset.new_material {path, entries}`（父材质为 context.asset）/ `asset.new_material_source {path, source}` / `asset.import {kind: texture\|mesh, path}`，结果记录带资产键 |
| material.set_entry / set_texture / assign / create_scene / override / discard | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移 |
| material.undo / redo {asset} | `edit.undo` / `edit.redo`，context.asset，region 为 parameters |
| material.inspect / save | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移 |

合并后约 65 个操作符，取代现在的 110 余个命令和约 30 个专用 Control op。

## 9. 迁移步骤

子线程目前不可用，各步都由我自己完成。每一步结束时，dev/release 无窗口全量和受影响的窗口验收都要通过。

1. **核心**：注册表、Context、`call`、参数存储、历史不变量、文档替换请求；Control 的 `op.list` / `op.call`；启动校验，以及 Control 验收中的 `op.list`、`op.call`、Context 覆盖和 poll 失败用例。先用无参数的操作符打通整条链（`view.frame_all`、`view.reset`、`edit.undo`/`edit.redo`、`layout.reset`、`scene.select`）；R5-6a 落地后接上参数解码，再迁移 3 个带参数的代表：`scene.add`（String）、`tool.settings`（seed）、`view.axis`（枚举）。
2. **参数类型**（已完成）：`operator_parameters.hpp` 生成组，只经编辑器文档 Session 发布；`ViewportTool::Settings` 改为 `editor.ToolSettings`；`scene.add`、`tool.settings`、`view.axis`。由表格生成 `editor.ViewSettings` 和 `view.set` 推迟到 4b-3 步骤 A 之后；区域状态记录随第 3 步的对应区域一起做。
3. **命令迁移**（已完成）：原 Command 动作由操作符定义及 poll 承担，Application 只提供文件选择、文档替换和交互等宿主能力，不按命令二次分派。Session 操作使用显式目标；多对象解除父级记录已提交前缀为一个历史条目，失败边界遵循第 2 节。
4. **菜单与按钮**（已完成）：菜单、快捷区与命令按钮绑定 `OperatorCall`，状态来自操作符，不再硬写启用、勾选与单选状态。
5. **Keymap**（已完成）：非 modal 操作由区域、阶段与修饰键匹配 keymap 后进入 `call`，快捷键文字由同一表生成；modal 手势仍由 ViewportTool 处理。
6. **Control 清理**（本轮完成）：工程、场景、相机、显示、网格/贴图导入重导入及材质创建/撤销/重做已删除旧专用 op 及 schema；验收使用 `op.call`，当前调用形状见 `control.md`。其余材质原生入口按第 8 节保留，随 O2 迁移。
7. **旧路由删除**（已完成）：`commands.hpp`、Command 枚举、命令定义表、适配查找、启用判断与 Application 的命令执行包装均已删除；Control 的创建物体与光照布局也只走 `op.call`。UI 的选择、枚举/布尔属性和显示设置与 Control 共享操作符执行路径；带预览的字段编辑留给 O2，第 8 节列出的六个单一 UI 路径暂不迁移。

窗口验收通过共享的 `operator_item(app, id, args)` 按操作符 ID 与参数查找当前菜单或快捷区中的真实绑定，保留原点击路径与行为断言，不依赖命令枚举数值。

## 10. 验收

- 启动校验：内置菜单、快捷区和 keymap 表逐项通过 `registry.find(id)` 检查操作符存在，未知 ID 拒绝启动；参数错误由实际调用的严格解码报告，不在启动时试解码或执行。
- `op.list` 中每个操作符的参数 schema 与其 Runtime TypeInfo 逐字段一致：名称、种类、枚举成员、范围。非 seed 式操作符的 schema 还要带默认值并与类型默认构造一致；seed 式操作符不带默认值，只断言 `"seeded": true`。`view.set` 的 schema 与 `ViewOptionInfo` 表逐项一致，类型名和字段顺序固定。
- Control 用 `op.call` 执行代表性操作符，包括：
  - 带 Context 覆盖的 `scene.delete`：按显式选择删除，调用后文档选择不变（被删的除外）；
  - 视口覆盖 cursor 后的 `scene.add`；
  - poll 失败（空选择时的 `view.frame_selected`）返回原因；
  - 无标志的操作符（`view.axis`、`region.set`）执行后撤销和重做计数都不变；`scene.set_properties` 写多个叶只产生一个历史条目；`edit.undo` 只移动游标；
  - 经 `op.call` 打开工程（`project.open`）：调用返回时文档已经替换，报告给出新场景；验收在 dev 构建（带调试堆检查）下跑，确认替换后没有访问已经销毁的 Session；
  - 非交互调用 `scene.open` 不带 path 时失败，原因写明需要 path。
- 同一个键在视口和场景树中解析到不同的操作符：经 keymap 查询，并在窗口验收中用真实按键验证。
- 原有窗口验收的点击路径和断言全部通过；菜单项改用第 9 节的查找函数定位。

## 11. 运行时负责人的支持（已批准）

总负责人已批准以下方案，由运行时负责人在 runtime-language 上实施；涉及 Runtime 契约的 diff 照例先发总负责人审。

1. **按路径读写调用方存储**：新增 `read/write(TypeId, span<const byte>/span<byte>, path, Scalar)` 和 `read/write(TypeId, storage, path, ValueOutput/ValueInput)` 四个重载，与 `read/write_reference(TypeId, storage, path, …)` 对称。对象重载和存储重载共用同一份按地址执行的实现：路径解析、精确类型检查、硬边界（越界夹值，NaN/±Inf 拒绝）只有一份。只在引用遍历或批量引用准备期间返回 busy。
2. **非 plain 值的整值读写**：`ValueInput`/`ValueOutput` 扩大到“可复制赋值、值操作保持借用、不含 GC 引用”的值类型，覆盖 String、Array 以及含它们的记录。plain 仍走 memmove 快路径。非 plain 先在临时值上构造、检查受约束叶并夹值，再与目标 noexcept swap，所以失败（包括分配失败）时字段保持原值；只有类型没有 noexcept swap 时才退回基本保证。对象目标在回调、原生借用和引用遍历期间返回 busy。编辑器写 String 叶的方式：构造 `atelier::String`，调用 `write(…, ValueInput{string_type, &s})`。
3. **反射生成器**：AFIELD 的 Strong/Weak/Sequence 目标接受任意限定名（例如 `Weak="world.Entity"`），由 Runtime 发布时按名称解析；目标缺失时，发布错误写明字段和所引用的名字。
4. **R5-6a 共用叶解码**：建在 P0 的 `read_properties` 之上，写入目标为“对象 | 调用方记录存储”，加入 String 叶和严格模式，场景读写支持 String 字段（格式只增不改，不升版本）。P0 进入 main 之后再改 scene.cpp。O1 第 1 步先做无参数部分，不需要它；参数解码只需 R5-6a 在带参数的代表迁移之前落地。如果需要更早，我提前告诉运行时负责人。R5-6b（数组）随后。
5. **`editor.` 前缀规则**：见第 3 节，由运行时负责人在 O1 第 2 步加入 Source 绑定目录。
6. **O3 缺口（记录在案，不阻塞 O1）**：含 GC 引用的记录目前不能传入源码，所以带 Weak 参数的操作符在 O3 之前不能由语言调用或定义，引用参数继续只在确有第二个目标时使用；源码函数的同步值区间限制了“语言定义的操作符以参数记录为入口参数”，到 O3 由运行时负责人提方案。Context 保持纯数据，O3 再映射为 Runtime 类型。

## 12. 已定事项

1. 区域可导航状态做成操作符，形状是每种区域一个状态记录，由补丁式的 `region.set` 修改，不进撤销历史（第 4 节）。
2. 调用按第 5 节区分查询、直接执行和人类交互；来源准入位于严格解码后、结束交互及执行前。
3. `scene.set_properties` 的参数类型是对象自身的类型，但按稀疏路径补丁解码和写入，一次调用一个历史条目（第 4 节）。
4. `view.set` 的参数类型由 `ViewOptionInfo` 表生成，表是唯一定义，类型名和字段顺序稳定，验收断言一致（第 4 节）。
5. 运行时契约（第 11 节 1–3）、R5-6 拆成 a/b、`editor.` 前缀规则的时机，都已由总负责人批准。
