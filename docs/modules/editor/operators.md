# 编辑器操作符（设计稿）

状态：O1 设计稿，已由总负责人定稿。第 1 步的无参数部分已实现（`src/editor/operators.*`、`builtin_operators.cpp`，Control `op.list` / `op.call`）：`view.frame_all`、`view.frame_selected`、`view.reset`、`layout.reset`、`edit.undo`、`edit.redo`、`scene.select`。第 3 步已迁移不带参数的 `layout.close_tab`、`layout.maximize`、`scene.transform_add`、`scene.transform_remove`、`scene.discard_saved_data`。第 2 步已实现参数记录与 3 个带参数的操作符：`scene.add`（String）、`tool.settings`（seed）、`view.axis`（枚举），Create、工具、轴心、吸附步长、坐标轴、选择模式和轴向视图命令都改为调用它们；`editor.ViewSettings` 和 `view.set` 推迟到 4b-3 步骤 A 之后，区域状态记录移到第 3 步。注册表由 Workspace 持有，Context 也由 Workspace 构建；第 4 步之前菜单和快捷区仍按命令登记，`command_enabled` 对已迁移的命令直接取对应操作符的 poll，不再保留第二份可用性判断。其余按第 9 节继续迁移；全部完成后本文改写为当前行为文档。

编辑器的每个操作都是一个注册的操作符。菜单、快捷区、按钮、快捷键、Control，以及以后的自有语言，都经同一个入口调用它：`OperatorCall {id, args, context}` → poll → 解码参数 → execute。UI 只描述“调用哪个操作符、带什么参数”，不写逻辑。

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
    prompts = 8,            // 过渡标志：交互调用缺少路径或名称时弹出选择器或内联编辑；O2 由 invoke 接管后删除
};
struct OperatorParameters {
    ValueInput value;                     // 记录类型参数：Runtime 现有的 {TypeId, const void*}
    std::span<const PropertyPatch> patch; // @context.object：对象类型约束下的稀疏路径补丁（第 4 节）
};
struct OperatorInfo {
    std::string id;                 // 稳定 ID：小写 "<域>.<动作>"，如 "scene.delete"
    std::wstring title;
    std::string description;
    std::string parameters;         // Runtime 记录类型名；空表示空记录；"@context.object" / "@context.region" 表示动态类型
    std::string result;             // 可选的结果记录类型名，例如新资产的键
    OperatorFlag flags{};
    std::function<Result<void>(const Context&)> poll;            // 不可用时 error.message 就是原因
    std::function<Result<void>(OperatorHost&, const Context&, const OperatorParameters&, ValueOutput result)> execute;
    std::function<Result<void>(const Context&, ValueOutput parameters)> seed;  // 可选：用当前状态初始化补丁式参数
    std::function<std::optional<bool>(const Context&, ValueInput parameters)> state; // 可选：菜单的勾选/单选状态
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
// poll → seed 或默认构造 → 按参数类型严格解码 args → execute → 检查历史不变量。
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
- 执行结果 `OperatorReport {status, message, result}`：status 为 finished、cancelled 或 failed；poll 失败时 status=failed、message 为原因、execute 不运行。

### 历史不变量

`call` 在 execute 前后比较场景 Session 的撤销计数 u、重做计数 r 和历史游标，按标志检查：

| 标志 | 要求 |
| --- | --- |
| `undo` | 最多新增一个历史条目：u 不变，或 u 加 1 且 r 归零 |
| `history` | 条目总数 u+r 不变，游标最多移动 1 |
| `replaces_document` | 不检查（文档被替换，历史随之清零） |
| 无以上标志 | u 和 r 都不变 |

检查依据是 Session 的 `recorded()`（累计记录的历史条目数，历史达到上限时淘汰旧条目也不会减少）加上撤销和重做计数。这是开发和发布构建中都生效的运行时检查：违反时 `call` 返回 failed，附带一条写明操作符 ID、标志和前后计数的诊断（开发构建可以额外断言）。验收在 dev 和 release 下都把它当作失败。迁移中发现的、会产生多个条目的现有动作，都要合成一个条目。目前已知 `unparent_selected` 按每个 Entity 各调一次 `reparent`，多选时会产生多个条目，`scene.parent_set` 需要 Session 提供批量改父级的单条目事务（属于 Session，若要改 `World::reparent` 的签名，先告知 World 负责人）；合不了的报总负责人。

### 替换文档

`scene.open`、`project.new`、`project.open`、`project.open_example`、`project.rebuild_types` 会替换 Session。Context 持有 `Session&`，而 `publish_document` 会销毁旧 Session，所以这类操作符在 execute 里只调用 `host.replace_document(request)` 登记请求。`call` 在 execute 之后不再碰 Context；Application 在 `call` 返回后、仍在同一次宿主调用里执行替换，并把替换的成败并入返回给调用方的报告。Control 的 `op.call` 因此能拿到“打开失败”的原因。

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

参数是原生登记的 Runtime 记录：头文件里用 `ASTRUCT/AFIELD/AENUM` 声明，由 `atelier_reflect` 生成，范围用 `Min/Max/HardMin/HardMax`。默认值就是类型的默认构造。由它派生 `op.list` 的 schema、keymap 和菜单参数的校验，以后也派生“调整上次操作”面板。编辑器为 `src/editor/operator_parameters.hpp` 新增一个反射生成组。

已实现的记录（`src/editor/operator_parameters.hpp`，由 `operator_parameters.cpp` 的 `register_editor_types` 登记）：

| 记录 | 操作符 | 字段 |
| --- | --- | --- |
| `editor.SceneAdd` | `scene.add` | kind（`editor.AddKind`：entity、mesh、point、rectangle、directional、camera）、mesh（String，空为第一个内置网格） |
| `editor.ToolSettings` | `tool.settings`（seed） | tool、pivot、grid_snap、cursor_surface、grid_spacing、select_mode、orientation、snap_move、snap_rotate、snap_scale、rotation_step、scale_step |
| `editor.ViewAxis` | `view.axis` | view（`editor.CameraView`） |

- 只有编辑器自己的文档 Session（`SessionRole::editor`）在引擎类型之后、工程 Source 构建之前登记这些类型；Play、独立运行和其他 Session 不登记。工程 Source 既看不到也不能声明 `editor.*` 类型（运行时的前缀规则），`project_integration` 在编辑器文档里断言这一点。
- `tool.settings` 的参数是 `editor.ToolSettings`。各编辑器枚举的底层类型为 `uint32_t`，生成器支持 int32/uint32。工具、轴心、选择模式和坐标轴不进持久格式；`CameraView` 以序号出现在 Control 的编辑器相机 JSON 和 repro 包中。Play 使用场景主相机，不接收编辑器相机参数。
- `editor.AddKind` 没有 sky：天光是 Environment 组件的属性，由 World Settings（`set_environment`）编辑，不是新建的 Entity。
- 参数记录暂不能含引用（调用时拒绝），第一个引用参数落地时再处理根集。
- 补丁式操作符（`tool.settings`、`view.set`、`region.set`）用 `seed` 从当前状态初始化参数，args 只写出现的叶。因此 `snap_grid_05` 就是 `tool.settings {"grid_spacing":0.5}`。
- `tool.toggle` 不幂等，只给菜单和快捷键这类交互绑定使用；`op.list` 的说明里写明这一点，Control 和脚本应当用 `tool.settings` 写绝对值。
- JSON → 参数值：调用运行时负责人的 R5-6a 共用叶解码（建在 P0 的 `read_properties` 之上：写入目标为“对象 | 调用方记录存储”，含 String 叶），不另写解码器。操作符一律用它的严格模式：未知字段、类型不符、越过硬边界都报错（越界按与 `constrain` 结果比较判断），不夹值；场景加载仍走宽容模式。
- 描述型记录不能带默认值，`editor.ViewSettings` 默认构造时全为零。seed 式操作符在 `op.list` 中不输出默认值，改为标注 `"seeded": true`（缺省表示保持当前值）。
- 引用参数：确有第二个目标时，才用 `Weak="world.Entity"` 的 ReferenceSlot，例如 `scene.parent_set` 的 parent。Control 用句柄字符串表示。主要目标一律来自 Context。

### `view.set`：由表格生成的描述型记录

`view.set` 的参数类型 `editor.ViewSettings` 是描述型记录，启动时由现有 `ViewOptionInfo` 表生成。表仍然是唯一的定义：表中的种类、范围和枚举值成为字段和 AENUM，类型名固定为 `editor.ViewSettings`，字段顺序就是表的顺序，确定且稳定。验收断言 `op.list` 中 `view.set` 的 schema 与表逐项一致。World 作者值是 Environment 组件的属性：`Session::set_environment` 比对候选中的 World 选项，把变化的项写成一次组写入，记一条普通的属性编辑历史；没有 Environment 时同一历史项先创建它。LightingChange 已删除。

### `scene.set_properties`：对象类型约束下的稀疏补丁

参数类型是 `@context.object`，即被检视对象自身的 Runtime 类型（组件、材质实例或材质资产对象）。`op.list` 中它的 schema 由该类型的字段目录生成（`{"dynamic":"context.object"}`，取实际 schema 时用 `editor.inspect` 给出该对象的类型）。

参数不存成整个对象值的拷贝：组件可能含引用、数组或不可复制的原生成员，整值拷贝既贵又可能做不到。args 按对象类型解码成稀疏路径补丁 `PropertyPatch {path, Scalar}` 列表，规则与 P0 的 `set` 相同：未知字段和类型不符报错，越过硬边界报错。执行时逐叶经 `World::write` 写入，整个调用合成一个 `FieldGroupChange` 历史条目。同一个带类型校验器的记录字段（例如 `body`）下的多个叶，作为该记录的一次整值写入（读出该记录，套用补丁，写回一次，校验一次），避免逐叶写入时中间态违反跨字段规则；具体做法依赖第 11 节的记录存储按路径写入，实现时与现有 `BodyChange` 的重放方式核对。

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

- 内置表放在 `src/editor/keymap.cpp`。`args` 写成 JSON 文本，启动时按参数类型解码一次并缓存。内置表中出现未知操作符、非法参数或同一区域和阶段内重复的组合键，是程序缺陷，编辑器拒绝启动。
- 以后增加用户覆盖文件时，用户文件出错的条目只给诊断，并回退到内置表的对应条目，不拒绝启动。现在不实现。
- 查找顺序与现有行为一致：
  1. 区域为视口、没有文本控件占用键盘时，先查视口表（目前视口键都在 UI 输入之前处理）。
  2. 全局表中的 `before_controls` 项：Ctrl+S/O、F5/F6/F8、Shift+F8、Ctrl+Tab、Ctrl+B/W。它们即使在文本框中也生效。
  3. 交给 UI 控件。
  4. 控件没有消费时，先查焦点区域的 `after_controls` 表，再查全局表：Ctrl+Z/Y、Delete、F2、Ctrl/Shift+D、F。
- 同一个键在不同区域解析到不同操作符，例如 Alt+P：场景树里是 `scene.parent_set {"parent":null}`，其他区域保留菜单助记键；X：视口里打开删除菜单。
- 菜单上显示的快捷键文字由 keymap 生成：找 `op` 与 `args` 都相同的条目，优先取菜单所在区域，再取全局。只有 modal 的 G/R/S 在 O2 之前不显示快捷键文字，提示写进说明。
- 要改为查表的硬写分支：`application.cpp` 的 `input()`（约 1120–1195 行）、`viewport.cpp` 的 `viewport_input()`（数字键盘、Home、小数点、Shift+Tab、Q、W、X、Z、Shift+A、`、Shift+D、Ctrl+Z/Y），以及 `viewport_tool.cpp` 的 `key()` 里非 modal 的部分（A、Alt+A、Ctrl+I、Alt+G/R/S）。G/R/S、B 的拖动和 Shift+` 飞行留给 O2。

### `prompts` 过渡规则

- 只在 `interactive=true` 时弹出文件选择器或开始内联编辑。
- 非交互调用缺少路径或名称时直接失败，原因写明“非交互调用必须提供 path”（或 name）。
- 弹出的原生对话框要在 d00e751f 的验收守卫里声明预期范围（`ExpectedNativeDialogs`）；验收经 UI 调用带 `prompts` 的操作符时都要这样做。
- O2 的 invoke 接管后删除这个标志。

## 6. 菜单、快捷区与按钮

- `ui::MenuItem` 和快捷区项不再把 `Command` 值当作键，而是在 Workspace 的绑定表里登记一项 `OperatorCall`（新增 `Binding::Kind::operator_call`，做法与现有的 property_choices_ 相同），控件键按次分配。菜单项也可以带 Context 覆盖，相当于 Blender 的 `context_pointer_set`，例如路径导航项是 `scene.select`，覆盖为该成员。
- 可用性取自 poll，勾选/单选取自 state，标题取自操作符或调用处的文字。
- 覆盖范围：主菜单、视口头部菜单与饼菜单、场景树行菜单、添加组件菜单、着色/参考线/Cavity 菜单、World 页菜单、材质槽菜单、资源浏览器的操作/过滤/视图/路径菜单、代码页文件菜单、Dock 页菜单、Controls 页模式菜单，以及快捷区按钮和面板中改动文档或工程的按钮（移除组件、清除材质槽、Fit 等）。
- 不经操作符的只有字段右键菜单里的“Copy Value / Paste Value / Revert to Default”：它们作用于控件草稿，再走字段提交，随参数行一起在 O2 迁移。

## 7. Control

- 新增 `op.list`：返回每个操作符的 `{id, title, description, flags, parameters: JSON schema, result: JSON schema}`，以及 Context 覆盖的 schema。参数 schema 完全由 Runtime 的 TypeInfo/FieldInfo 生成：`{"type":"object","additionalProperties":false,"title":记录名,"properties":{…}}`；每个字段给出 `type`（boolean/integer/number/string）或 `enum`（成员名）和 `description`，硬边界写成 `minimum`/`maximum`，界面范围写成 `ui_min`/`ui_max`。非 seed 式操作符带 `default`；seed 式操作符不带默认值，改标 `"seeded": true`（缺省表示保持当前值）。无参数的操作符是空的 properties，没有 title。`@context.object` 写成 `{"dynamic":"context.object"}`。
- 新增 `op.call {id, args, context}`：需要状态令牌，与现有的改动类 op 相同；返回 `OperatorReport`，并附上调用后的选择。poll 失败时返回 `{"status":"failed","code":"poll_failed","message":原因}`。Control 调用时 `interactive=false`，带 `prompts` 标志的操作符缺少路径或名称时直接失败，不弹窗。
- 删除所有已迁移的专用 op，不保留兼容名字。Control 只保留 `capabilities`、`editor.inspect`（并入 `material.inspect`，新增 `detail:"asset"`）、`render.*`、`job.*`、`repro.*`。验收客户端 `acceptance/editor/control.cpp` 和 `control.md` 一起改。

## 8. 对照表

### 旧命令 → 操作符

| 旧命令 | 操作符与参数 |
| --- | --- |
| new_project / open_project | `project.new {path}` / `project.open {path}`（prompts） |
| example_sponza / scene_lit / physic / dungeons | `project.open_example {example: sponza\|scene_lit\|physic\|dungeons}` |
| open_scene | `scene.open {path}`（prompts） |
| save_scene / save_scene_as | `scene.save {path, as_new}`（as_new 时 prompts） |
| save_document / save_all | `file.save`（按区域保存场景或源码）/ `file.save_all` |
| import_mesh / import_texture | `asset.import {kind: mesh\|texture, path}`（prompts） |
| create_material | `asset.new_material {path}`（prompts） |
| refresh_assets | `asset.refresh` |
| open_source / edit_ui_source | `source.open {path}`（prompts）/ `source.open_editor_ui` |
| close_source / reload_source | `source.close` / `source.reload` |
| new_source / new_project_ui / edit_project_ui | `source.new_component {path}` / `source.new_project_ui {path}` / `source.open_project_ui` |
| build_project / rebuild_project | `project.build {rebuild: false\|true}` |
| run_project / stop_project / show_project_ui | `play.start` / `play.stop` / `play.preview_ui` |
| exit / about | `app.quit` / `app.about` |
| undo / redo | `edit.undo` / `edit.redo`（按区域选择历史：源码、材质资产、Controls 示例或场景） |
| create_entity / create_static_mesh / create_point/rectangle/directional_light | `scene.add {kind, mesh}`（create_sky_light 由 4b-3 步骤 A 删除，见第 4 节） |
| duplicate_selected | `scene.duplicate {place}`（视口的 Shift+D 为 place=true，接到现有放置；O2 改为 modal） |
| erase_selected / erase_entities | `scene.delete {target: object\|entities}` |
| rename_selected | `scene.rename {name}`（prompts：交互调用不带 name 时开始内联重命名） |
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
| 字段的枚举下拉 | `scene.set_properties`（Context object 为目标） |
| 材质槽菜单：指派、默认、编辑共享材质 | `material.assign {slot, asset}`（asset 为空恢复网格默认）/ `asset.open` |
| 贴图槽菜单 | `material.set_texture {input, texture}` |
| 资源操作菜单：Open / Add to Scene / Rename | `asset.open` / `scene.add {kind: mesh, mesh}` / `asset.rename {name}`（prompts） |
| 资源过滤、视图、路径 | `assets.display {filter, view}`（seed）/ `assets.browse {location, path}` |
| World 页体积槽位、代码页文件切换、Controls 模式 | `world.select_slot {slot}` / `source.focus {path}` / `workbench.set {mode}` |
| Dock 页菜单 | `layout.*` |

### 旧 Control op → 操作符

| 旧 op | 新调用 |
| --- | --- |
| project.open / scene.open / scene.save | `project.open` / `scene.open` / `scene.save` |
| scene.create {kind, mesh} | `scene.add {kind, mesh}` |
| scene.select {entities, active, object} | `scene.select`，选择放在 context |
| scene.rename {object, name} | `scene.rename {name}`，context.selection.active |
| scene.set_property {object, property, value} | `scene.set_properties {<字段路径>: 值}`，context.selection.object |
| scene.transform / remove_transform | `scene.transform_set {position, rotation, scale}` / `scene.transform_remove` |
| scene.set_space_reference {object, origin, keep_world} | `scene.space_reference_set {origin, keep_world}`（origin 为弱引用或空） |
| scene.reparent {object, parent, keep_world, index} | `scene.parent_set {parent, keep_world, index}` |
| scene.add_component / remove_component | `scene.add_component {type}` / `scene.delete {target: object}` |
| scene.delete / undo / redo | `scene.delete {target: entities}` / `edit.undo` / `edit.redo` |
| camera.set / camera.look_at | `view.camera_set {state: editor.CameraState}` / `view.look_at {eye, target}` |
| view.set {patch} / view.layout {layout} | `view.set {...}` / `view.lighting_fit {mode}` |
| material.create / texture.import / mesh.import | `asset.new_material {path}` / `asset.import {kind: texture\|mesh, path}`，结果记录带资产键 |
| material.set_property | `scene.set_properties`，context object 为材质对象 |
| material.set_texture / assign / override | `material.set_texture {input, texture}` / `material.assign {slot, asset}` / `material.override {slot, property, enabled}` |
| material.save / undo / redo {asset} | `asset.save` / `edit.undo` / `edit.redo`，context.asset |
| material.inspect | 并入 `editor.inspect {detail: "asset"}` |

合并后约 65 个操作符，取代现在的 110 余个命令和约 30 个专用 Control op。

## 9. 迁移步骤

子线程目前不可用，各步都由我自己完成。每一步结束时，dev/release 无窗口全量和受影响的窗口验收都要通过。

1. **核心**：注册表、Context、`call`、参数存储、历史不变量、文档替换请求；Control 的 `op.list` / `op.call`；启动校验，以及 Control 验收中的 `op.list`、`op.call`、Context 覆盖和 poll 失败用例。先用无参数的操作符打通整条链（`view.frame_all`、`view.reset`、`edit.undo`/`edit.redo`、`layout.reset`、`scene.select`）；R5-6a 落地后接上参数解码，再迁移 3 个带参数的代表：`scene.add`（String）、`tool.settings`（seed）、`view.axis`（枚举）。
2. **参数类型**（已完成）：`operator_parameters.hpp` 生成组，只经编辑器文档 Session 发布；`ViewportTool::Settings` 改为 `editor.ToolSettings`；`scene.add`、`tool.settings`、`view.axis`。由表格生成 `editor.ViewSettings` 和 `view.set` 推迟到 4b-3 步骤 A 之后；区域状态记录随第 3 步的对应区域一起做。
3. **按组迁移**：把 `Application::execute` 的每个 case 搬进操作符定义，把 `command_enabled` 的对应 case 改为 poll，参数和 ID 以第 8 节的表为准。顺序：window/layout → view/tool/cursor → scene → project/play/source → asset/material。Session 中依赖选择的方法同时改为显式目标；会产生多个历史条目的动作（已知 `unparent_selected`）合成一个条目。
4. **菜单与按钮**：菜单构建改为 `OperatorCall`，删除 `command_item` 的硬写状态逻辑。
5. **Keymap**：建表，删除 `application.cpp`、`viewport.cpp`、`viewport_tool.cpp` 中的非 modal 分支。
6. **Control 清理**：删除旧 op，`acceptance/editor/control.cpp` 改为 `op.call`；同步 `control.md`。
7. **删除** `editor::Command`、`command_definitions`、`command_enabled` 和 `execute` 的 switch；更新编辑器 README 与 `control.md`。

窗口验收中引用 `Command::` 的地方（约 120 处）随第 7 步改写，但点击路径和断言的行为不变：在验收侧新增一个查找函数，按“操作符 ID + 参数”在当前菜单或快捷区中找到对应的项（例如 `operator_item(app, "view.axis", {{"view","top"}})`），各处只把原来的 `static_cast<ControlKey>(Command::…)` 换成它，不各自重写查找逻辑。

## 10. 验收

- 启动校验：每个菜单项、快捷区按钮、面板按钮和 keymap 条目都解析到已注册的操作符，且参数能按其类型严格解码，否则编辑器拒绝启动（只针对内置表）。Control 验收经 `op.list` 和一份菜单与 keymap 的转储，断言这一点。
- `op.list` 中每个操作符的参数 schema 与其 Runtime TypeInfo 逐字段一致：名称、种类、枚举成员、范围。非 seed 式操作符的 schema 还要带默认值并与类型默认构造一致；seed 式操作符不带默认值，只断言 `"seeded": true`。`view.set` 的 schema 与 `ViewOptionInfo` 表逐项一致，类型名和字段顺序固定。
- Control 用 `op.call` 执行代表性操作符，包括：
  - 带 Context 覆盖的 `scene.delete`：按显式选择删除，调用后文档选择不变（被删的除外）；
  - 视口覆盖 cursor 后的 `scene.add`；
  - poll 失败（空选择时的 `view.frame_selected`）返回原因；
  - 无标志的操作符（`view.axis`、`region.set`）执行后撤销和重做计数都不变；`scene.set_properties` 写多个叶只产生一个历史条目；`edit.undo` 只移动游标；
  - 经 `op.call` 打开示例（`project.open_example`）：调用返回时文档已经替换，报告给出新场景；验收在 dev 构建（带调试堆检查）下跑，确认替换后没有访问已经销毁的 Session；
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
2. `prompts` 过渡按第 5 节的三条规则执行，O2 删除。
3. `scene.set_properties` 的参数类型是对象自身的类型，但按稀疏路径补丁解码和写入，一次调用一个历史条目（第 4 节）。
4. `view.set` 的参数类型由 `ViewOptionInfo` 表生成，表是唯一定义，类型名和字段顺序稳定，验收断言一致（第 4 节）。
5. 运行时契约（第 11 节 1–3）、R5-6 拆成 a/b、`editor.` 前缀规则的时机，都已由总负责人批准。
