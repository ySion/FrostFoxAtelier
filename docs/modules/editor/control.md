# Editor Control

Editor Control 是编辑器指令化操作与调试的标准入口。UI、快捷键、CLI、自动化、端到端验收和 LLM 都是调用方，共享指令语义、状态与结果、错误和生命周期合同。指令使用稳定业务名称，不暴露 UI control key、指针或 Runtime 私有布局。

UI 和快捷键直接调用负责业务不变量的执行层；外部调用经当前的本机管道与 JSON 适配到 Application、Session、Camera 和显示设置的相同业务入口。统一入口不要求 UI 序列化 JSON，也不把传输连接当成业务状态的拥有者。实例发现、外部请求身份与重试缓存由 Control 适配层处理。

本文的能力目录记录已实现操作。源码调试核心已经可用，编辑器调试面板与 `debug.*` 指令仍处于接入方案阶段，见[源码调试接入](#源码调试接入待实现)。

## 启动和调用

正常编辑器默认启用本机接口，--no-control 可关闭。发现目录默认为 %LOCALAPPDATA%/FrostFoxAtelier/Control/instances；--control-directory folder 可隔离目录。--no-activate 打开窗口但不抢键盘焦点。快照模式及独立项目运行器不自动开放接口。

需要复现固定画布时，可以组合 `--client-size 3840 2054 --no-activate`。宽、高明确是初始 **client 物理像素**，非 DIP 或含边框的窗口尺寸；应用在创建 Renderer 和显示窗口前，按实际 HWND DPI 补上非客户区。省略后仍使用原 1280×840 DIP 默认值。参数只接受 1–32768 的十进制正整数，禁止重复；低于当前 DPI 下编辑器最小窗口尺寸、或系统无法精确兑现的尺寸会在创建图形设备前拒绝，不静默改成其他尺寸。后续手动 resize 与 DPI 变化沿现有窗口规则。

客户区相同不保证 viewport 相同：渲染对照还须核对同一 DPI、停靠布局、工具架状态，以及 `editor.inspect.last_frame.extent` 和 `viewport`。尺寸为启动参数，没有新增 Control 窗口写操作。

    . ./tools/enter-dev.ps1
    cmake --build --preset dev --target atelier_editor atelier_control
    ./out/dev/atelier_editor.exe --project E:/ffa_testproj
    ./out/dev/atelier_control.exe --list

从发现结果中明确选择 instance_id，不要自动连接第一个实例。先读取能力目录和当前状态：

    '{"op":"capabilities"}' | ./out/dev/atelier_control.exe --instance INSTANCE_ID --request -
    '{"op":"editor.inspect","args":{"detail":"summary"}}' | ./out/dev/atelier_control.exe --instance INSTANCE_ID --request -

将最近观察到的 state_token 填入 UTF-8 JSON 请求：

    {
      "request_id": "create-first-entity",
      "op": "op.call",
      "state_token": "INSTANCE_ID:VERSION",
      "args": {"id": "scene.add", "args": {"kind": "mesh", "mesh": "builtin/cube"}}
    }

    ./out/dev/atelier_control.exe --instance INSTANCE_ID --request request.json

CLI 补齐协议及实例身份；没有 request_id 时生成新 ID。异步操作保持连接并轮询到完成，--timeout-ms 为 1–600000 毫秒，默认 120000。成功退出码为 0，错误为 1。CLI 先选择实例，读取并限制请求输入大小，再连接和解析 JSON。服务端终态响应原样输出。首次收到 pending 后开始计算 CLI 轮询期限，到期后返回 `timeout` 并断开连接，宿主在下个检查点取消任务。

## 能力和业务边界

实时参数 schema、枚举和范围由 capabilities 返回。`display_options` 的每项同时包含 `label`、`category`、`description` 与 `scope`；它们直接来自 UI 使用的同一 `ViewOptionInfo` 定义，描述支持 UTF-8 中英文与换行。职责如下：

`capabilities.operations` 的 `kind` 区分 `query` 与 `direct`，不从 `requires_state_token` 推导：例如 job.status 是 query，job.cancel 虽不要求令牌仍是 direct。`op.call` 另声明 `accepted_operator_kinds:["direct"]`；操作符的可用类别及参数条件由 op.list 提供，不提供外部分步交互协议。

| 操作 | 权威入口与行为 |
| --- | --- |
| editor.inspect | 默认 `detail: full` 返回完整场景与资源目录，适用于小场景；`summary` 返回状态、历史、相机、显示和最近接受帧，不枚举对象或资产；`assets` 分页读取资源目录，`objects` 分页发现 Entity/Component 句柄，`object` 读取一个已发现对象的反射属性。full/summary 的 `performance` 共用 World 性能表的有界统计快照 |
| op.list | 列出编辑器操作符：ID、标题、说明、kinds、历史标志和参数 schema，以及 Context 覆盖的 schema。固定项 kinds 为 `["direct"]` 或 `["interactive"]`；参数决定类别的项为 `["direct","interactive"]`，另有 interaction 条件说明。参数 schema 由文档 Runtime 中的参数记录生成（字段、枚举成员、硬边界 minimum/maximum、界面范围 ui_min/ui_max；非补丁式带 default，补丁式标 `"seeded": true`）。菜单、快捷键和 Control 调用同一个注册表，见[操作符](operators.md) |
| op.call | `{id, args, context}` 调用一个操作符，需要状态令牌。`args` 按参数记录严格解码：未知字段、类型不符或越过硬边界都返回 `invalid_request`，不夹值；补丁式操作符（`tool.settings`、`view.set`、`scene.set_properties`）只改出现的字段。`context` 只在本次调用内替换 `selection {entities, active, object}`、`region` 或 `asset`，不改文档选择；句柄必须是存活成员，active 属于 entities，object 是 active 或它的组件。poll 不满足时返回 `poll_failed` 和原因；未知操作符为 `unknown_operator`。协议外层为 `status: ok`，其 `result` 包含业务 `status: finished / cancelled`、`message`、操作符结果 `result`（如新资产的 `asset` 键）及调用后的 `selection`、`active`、`inspected_object` |
| op.call view.lighting_fit | `mode: scene` / `scene_focus`，沿 World 页同一操作符按当前可渲染几何拟合 DDGI 覆盖；保留传播和显示偏好，完整布局提交为一条场景历史 |
| op.call project.new / project.open / scene.open / scene.save | Application 工程/场景操作；New/Open 拒绝未保存场景、材质或源码草稿，避免无人处理的保存对话框。`project.build {rebuild:true}` 保留内存作者值及场景 dirty 标记，保存 Source 草稿和材质，但显式退役历史，不受 New/Open 的未保存检查限制 |
| op.call scene.add | Entity、StaticMeshEntity、ProceduralGeneratorEntity（`kind: "procedural_generator"`，可选 `graph`）、Camera、Point/Rectangle/Directional Light，与 Create 菜单共享操作符；返回创建后的 active/object/选择。kind 不含 sky，天空通过 view.set 的 sky 键设置 |
| op.call procedural.assign_graph / procedural.bake | `procedural.assign_graph {component, asset}` 为目标或当前 `ProceduralGeneratorComponent` 绑定或清空 `.procgraph` 资产键并记一步场景历史；`procedural.bake {entity, component}` 将目标或当前生成器的 `CopyToPoints` 实例一次性烘焙为 `<Entity> Baked` 下的 `StaticMeshEntity` 子层级（单步可撤销） |
| op.call scene.rename / scene.delete / scene.select | Session 的名称、删除和选择；rename 只修改 Entity 名称，Component 使用类型名。目标来自 context.selection，delete 的 target 为 object 或 entities |
| op.call scene.set_properties | 按实际类型字段名组成嵌套 Scalar 补丁，枚举使用名称；多叶沿组写入和最终校验记一个历史条目。Entity 的实际 Transform 字段在写入前拒绝，使用专用 Transform 操作符 |
| op.call scene.transform_set / scene.transform_remove | Entity 的可选内联完整 TRS；设置时无空间则添加空间，一次撤销。position/scale 为 `{x,y,z}`，rotation 为 `{x,y,z,w}` |
| op.call scene.add_component / scene.delete | 添加组件的 type 来自 component_types；删除组件使用 target: object，Context object 为组件且 active 为其 Entity |
| op.call scene.parent_set | 按 Context 的 Entity 顺序调整组合 parent；支持 keep_world 和组成 index，实际提交前缀记一个历史条目，后项失败不回滚已发布部分 |
| op.call scene.space_reference_set | 设置活动 Entity 的空间参考：null origin 继承组合祖先，world_space: true 使用世界，指定 origin 使用另一 Entity；支持 keep_world，一次撤销 |
| op.call asset.new_material / asset.new_material_source / asset.new_procedural_graph | MI 参数为 path、entries（默认空数组），父材质来自 context.asset；MS 参数为 path、source（默认生成正文），创建 `.ms.material` 和 Source；`asset.new_procedural_graph` 参数为 path、source（默认空 v2 图 JSON），在工程 `Content` 下创建 `.procgraph` 与 `.asset.json` GUID 身份，交互调用打开该图的 SOP 视口上下文，非交互调用只发布作者文件并返回 `asset` 键。Control 的 Context 默认取当前编辑器焦点，Parameters/Assets 正在检查材质时，asset 指向该材质，创建 MI 会以它为父材质，edit.undo 会撤销其历史。创建 MI 时要使用内置默认父材质而非当前材质，显式传 `context: {"asset":""}`；要明确撤销场景历史，传 `context: {"asset":"","region":"viewport"}`，避免 Code/Controls/Network 焦点转到其他历史 |
| op.call graph.open / graph.exit / graph.save | `graph.open {asset}` 打开工程 `.procgraph`（或已存在的临时图键）并将 Scene View 切换至 SOP 上下文；`graph.exit` 返回场景 `World` 上下文；`graph.save {asset}` 保存指定或当前活动 `.procgraph` 资产并刷新工程目录 |
| op.call graph.add_node / graph.delete_node / graph.rename_node / graph.move_node / graph.layout | 编辑活动或显式 `asset` 图文档并记入该图的独立 128 步撤销栈（不改场景历史计数）：`add_node {type, name, position}` 添加内置 C++ 或 `.source` `procedural.Node` 派生实例；`delete_node {name}` 删除指定节点或当前画布多选节点（保活 `ObjectRoot` 以供撤销复用求值缓存）；`rename_node {node, name}` 重命名节点并同步连线、display 与 template；`move_node {node, position}` 更新二维画布坐标；`layout {asset}` 自上而下自动拓扑排布全部节点 |
| op.call graph.connect / graph.disconnect / graph.set_display / graph.set_template / graph.set_bypass / graph.set_frozen / graph.set_appearance / graph.select | `connect {from_node, from_port, to_node, to_port}` 与 `disconnect {from_node, from_port, to_node, to_port}` 维护端口连线；`set_display {node}` 与 `set_template {node}` 设置或清空持久化 Display / Template 标记；`set_bypass {node, bypass}` 切换节点直通跳过；`set_frozen {node, frozen}` 在当前会话中锁定或解冻节点已求值的几何输出快照（记一步撤销，撤销和重做复用原冻结快照）；`set_appearance {node, shape, color, comment}` 设置节点形状预设、24 色/自定义 `#RRGGBB` 颜色与备注；`select {node}` 设置 Workspace 当前选中节点（空字符串选中图根 `seed` 设置，不写撤销栈） |
| graph.inspect / graph.set_properties / graph.create_scratch | `graph.inspect {asset, node, timings}` 评估并返回图的节点列表（含 `display`/`template`/`bypass`/`frozen`/`shape`/`color`/`comment`）、端口、连线、`notes`、`boxes`、每节点求值/缓存计数与错误、Display 几何统计及可选任意 `node` 的独立几何统计与可用节点类型目录 `node_types`（每项带 name、description、native、inputs 和 outputs，空图也可查询）。Boolean 节点的 `status.boolean` 记录最近一次实际调用的 `input_faces`、`prepared_input_hits`（A、B 分别占第 0、1 位）、`candidate_pairs`、`ray_candidates`、`splits`、`peak_face_scratch_bytes` 和各阶段耗时（`prepare_ms`、`arrange_ms`、`sew_ms`、`publish_ms`）。即使关闭 `timings` 也采集；调用失败保留本次已完成的工作，Bypass、缓存命中或上游阻塞保留上次调用的数据，尚未调用则省略此字段；`graph.set_properties {asset, node, properties}` 以完整 JSON 属性补丁支持含 `String` 与 `Array<String>` 在内的任意节点/图根字段并记一步图撤销；`graph.create_scratch {key}` 创建无需磁盘文件的内存临时图（默认 `scratch/procedural`）并进入 SOP 上下文 |
| op.call asset.import | kind: texture / mesh，path 为图片或 OBJ/glTF；模型导入生成普通 MI 和网格默认关联，结果记录带 asset 键，重复请求由既有 request_id 合同处理 |
| op.call asset.reimport | context.asset 与源文件 path；同身份更新并发布精确内容，不进入参数 Undo，模型按槽名保留用户 MI |
| material.inspect | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。按资产 GUID 或对象句柄读取 MS/MI 的声明、有效值、本地稀疏项、父关系、诊断与保存状态；asset 冷路径可能加载并发布资源、增加文档 revision，后续观察可能更新 state token 或取消活动渲染任务，因此整体标为 direct，仍不要求 state token，不承诺纯只读查询 |
| material.set_entry、material.set_texture | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。前者接受 object 与完整 typed entry（path/type/enabled/value）；后者接受 object/path/texture，空纹理键显式清空，不恢复继承 |
| material.assign | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。按组件句柄与槽名赋予 Material GUID；空 asset 恢复网格默认 |
| material.create_scene | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。按 component/slot 创建场景 MI，继承该槽当前材质；未绑定槽继承 DefaultWorldGrid，返回实例 object 句柄 |
| material.override、material.discard | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。按 object/path 启用或关闭本地项（enabled），关闭保留本地值；discard 才删除本地项并恢复继承 |
| material.save | 保留：无重复 operator；材质编辑随 O2 的 preview/commit 一起迁移。共享资产独立保存；保存后目录刷新失败返回 saved 与 catalog_warning |
| op.call edit.undo / edit.redo | 共享材质资产历史使用 context.asset，region 取 parameters；场景实例覆盖沿场景历史，context.asset 为空 |
| op.call view.camera_set / view.look_at | 公共 Camera 状态和观察目标，参数使用 Runtime 值记录；inspect 的 camera 可原样作为 camera_set 的 state |
| op.call view.set | 直接以 args 为显示补丁，与菜单共用 ViewOption 规则，不再嵌套 patch |
| repro.export、repro.open | 内存场景的独立复现工程 |
| render.trace | 按需观察下一次请求视图的后处理命令录制，导出 JSON 与自包含 HTML/SVG；不读回像素、不等待 accepted |
| render.prepare | 通过真实接受帧准备当前实例的 Scene Lit 管线，结束后恢复基础显示；单独报告准备耗时 |
| render.capture、render.compare | 当前显示捕获或临时设置对照 |
| job.status、job.cancel | 查询和幂等取消任务 |

`op.call` 固定使用外部来源，不能通过 context override 伪装成人类。context/poll 与严格解码之后，统一按注册元数据和已解码参数准入；需要人类交互时返回 `status: error`、`error.code: interaction_required`，message 给出操作 ID 和适用的参数条件。拒绝发生在结束旧交互或执行前，不取消手势、不复制实体、不取得捕获。`view.select_box`、`ui.menu`、`app.about`、`app.quit` 仅支持人类交互；`scene.duplicate` 传 `place:false`（默认）直接复制，`place:true` 拒绝；空 name 的重命名和空 path 的选择器也拒绝，`scene.save` 仅在 `as_new:true` 且空 path 时需要选择器。显式路径的直接调用仍遵循原保存冲突、未保存内容和业务校验规则。

严格解码能发现的错误优先于 interaction_required；execute 内业务校验不前移。例如 `asset.new_material {entries:{}}` 且省略 path，entries 的 JSON 在严格解码阶段仅存储，先因需要选择器返回 interaction_required；显式 path 的直接调用才进入原 entries 业务校验。通过请求重放检查、状态令牌和 busy 校验后，依次校验操作符 ID、args 对象形状和 context 覆盖。选择中的过期句柄返回 `stale_object`；句柄有效但 active 不属于 entities 时返回 `invalid_request`。

场景身份与参数统一使用 Entity 名称：`editor.inspect` 的目录和 `scene.select` 的 context 选择数组均为 `entities`；没有 actor/entitys 别名。`scene.add` 的空实体 `kind` 为 `entity`。`scene.add_component` 对当前选中 Entity 添加 `type` 指定的真实组件。

Transform、parent_set 和 space_reference_set 的 Context 目标必须是 Entity，组件不接受 TRS。parent_set 的 parent 为 Entity 句柄，null 或省略表示顶层；keep_world 默认 true，index 可省略。space_reference_set 的 origin 为 null 时继承组合祖先，world_space: true 表示世界，Entity 句柄表示显式空间参考；keep_world 默认 true。

程序化图的属性补丁按请求中的字段顺序处理。字段被拒绝时，`graph.set_properties` 返回 `operation_failed` 和首个字段的诊断，保留整次修改之前的值，以及图的撤销和重做位置。非对象形状的 `properties` 返回 `invalid_request`。

完整 inspect 的 Entity 包含 parent（顶层为空字符串）、children 句柄数组、可选 transform 和 space_reference（null / 空字符串 / 句柄）。实体与组件句柄和分页目录一致。组件记录沿用单对象详情，包含反射参数与材质。完整查询读取场景层级、对象详情或组件类型目录失败时，返回 `operation_failed` 和底层首条诊断，响应中没有 `result`。组件记录不含 TRS。创建子实体复用 op.call scene.add 和 scene.parent_set；没有第二套 create_child 工厂或对象身份。

`editor.inspect` 的普通对象记录带 `type_description` 和反射 `properties`。每条属性的 `description` 来自 Runtime 的字段描述；Vector 分量沿用聚合字段的说明，未声明说明时返回空字符串。名称、类型、值和可选枚举 `choices` 保持原有语义；声明了范围的属性另附 `range`，只列出已声明的端点：`ui_min`/`ui_max` 为常用范围（面板滑条），`hard_min`/`hard_max` 为写入时夹值的边界，Vector 分量沿用聚合字段的范围。材质不以固定 Runtime 字段表达参数，改用下述 `declarations` 与 `entries`。说明不包含可执行入口，查询不改变作者值或历史。

### 资产目录分页

```json
{"op":"editor.inspect","args":{"detail":"assets","limit":64}}
```

`assets` 只接受 `detail`、`limit`、`cursor`，返回 `detail: assets`、`assets`、`next_cursor`、`document_generation` 和 `project_revision`；外层仍有 `state_token`。条目与 full.assets 的形状及顺序完全一致：先内建网格（key/name/kind，kind 为字符串 `mesh`），再按工程目录路径排序的条目（key/path/kind，kind 为 AssetKind 数值）。目录包括文件夹、Source 和 Config，不只包括可加载资源；没有工程时仍返回内建项。

默认每页 64 项，limit 为 1–256；条目字节预算为 1 MiB，复用 `capabilities.limits.inspect_page_items/inspect_page_bytes`。仅序列化本页，不生成全量 JSON 再截取；成功的非末页至少含一项并推进游标，单项超过预算报 `result_too_large`。`next_cursor: null` 表示结束，否则原样传回 cursor；可调整 limit。游标区分 assets 与 objects 域，绑定实例、文档代际、工程目录 revision 和下一位置，不持有跨请求目录快照。

工程或文档替换（含同工程切场景/重建）、实例或已发布目录 revision 改变时返回 `stale_cursor`，调用方应丢弃已收集页重新查询。普通场景、选择、相机编辑与无变化 Refresh 不使资产游标失效。查询只读最后成功发布的目录，不扫描磁盘、不加载资源、不触发 Refresh；失败 Refresh 不发布半份目录。非法/跨域游标、非法 limit 或对象过滤参数返回 `invalid_request`。

full 仍返回完整 assets，不静默截断；full/summary 不接受分页参数。4 MiB 响应上限、repro 输入文件与 Source 编译限额保持独立，不因工程目录取消项数上限而取消。大工程调用方可先 summary，再逐页读取 assets，并用资源 key 与最新 state_token 沿既有操作编辑。

### 大场景对象查询

大场景先用 `summary` 观察状态，再用 `objects` 发现句柄，避免完整场景展开后超过 4 MiB 协议上限：

```json
{"op":"editor.inspect","args":{"detail":"objects","entity_name":"Lights - URP/Directional Light","limit":64}}
```

`objects` 返回平铺的 Entity/Component 目录。每项只有 `handle`、`name`、`type`、`kind`、所属 `entity`；Entity 另有组合 `parent`。目录不展开反射属性、材质或 children 数组，也不先生成 full 再截取。`entity_name` 对所属 Entity 名称做区分大小写的精确过滤，包含该 Entity 及其组件；重名返回所有匹配项。可叠加 `type` 对实际注册类型名做精确过滤。

默认每页 64 项，`limit` 范围 1–256；条目序列化字节预算为 1 MiB（另有少量页元数据），由 `capabilities.limits` 返回。`next_cursor` 为 null 时结束，否则将返回的对象原样作为下一次请求的 `cursor`；可调整 limit，过滤条件自动继承，不能更换。游标绑定实例、文档代际和 Session revision，编辑、撤销或替换文档后返回 `stale_cursor`，调用方须重新查询。分页没有长期持有的对象快照。

取得组件 handle 后，沿同一入口读取其描述、精确属性类型和值：

```json
{"op":"editor.inspect","args":{"detail":"object","object":"<返回的组件 handle>"}}
```

结果的 `object` 与 full 中同一对象的反射记录一致，复用现有句柄注册与成员有效性检查；删除对象或替换文档后，旧句柄返回 `stale_object`。读取成员列表、反射属性或适用的组件数据失败时，返回 `operation_failed` 和底层首条诊断，整个详情请求失败。Scene 文件中的持久 ID 不能替代 Control handle。随后使用 `op.call scene.set_properties` 和 `op.call edit.undo` 等编辑入口。full/summary 不接受分页参数，单对象详情同样受 4 MiB 协议上限约束。

宽容加载为未注册类型创建的占位对象，其对象记录另带 `placeholder: true` 和 `saved_type`（存档中的原类型名），`type` 是其原生基类；普通对象不含这两项。full 与单对象详情另以 `saved_data` 列出无法应用的保存值（`path`、截断预览 `value`、诊断 `kind`）。这些值只读，丢弃仍走编辑器的 Discard Saved Data。网格组件详情带 `mesh: {source, missing, default_materials}`；默认关联包含 slot/object/source/missing。显式 `materials` 项包含 slot/object/asset/parent/scene/assignment_override/default_material，具体材质诊断由该 object 的 `material.inspect` 返回。替代显示不改写保存引用。

材质参数、纹理、赋予和覆盖复用与 UI 相同的事务，不直接写 JSON 或 GPU 数据。`editor.inspect` 返回 `dirty_materials`、`dirty_graphs`、`open_graphs`、`active_graph`、`selected_graph_node` 与 `sop_guides`；替换文档会拒绝未保存材质或脏 `.procgraph`。`repro.export` 在导出副本中保存场景引用到的材质（含网格默认材质与可达父链）的实际草稿，并核对已载入贴图内容，原工程不变；会话里载入过但场景没有引用的材质不进入副本。MS→MI→MI 继承、稀疏作者项与资源发布合同见[资源工作流](projects.md#材质与贴图资源)。

坐标为米、左手系、+Y 向上，矩阵按行存储、向量左乘。CameraState 的 view 使用枚举名 perspective/top/front/right/bottom/back/left，target 以及 look_at 的 eye/target 使用 `{x,y,z}`；yaw/pitch/roll 为弧度，distance 为 0.5–2000 米，vertical_fov 为 1–179 度，near_clip 为 0.001–100000 米，far_clip 不超过 1000000 米且大于 near_clip。`op.call view.camera_set` 验证参数记录及相机基底后整体替换状态；inspect 相机与参数使用同一编解码。未给字段取 CameraState 默认值（distance 没有合法默认，必须给出）；通常直接把 inspect 返回的 camera 原样传回。视图操作符与 UI 使用同一六向相机入口。`view.from_camera` 通过选中 Entity 一次性复制场景相机的位姿（含 roll）和投影，不改作者数据。`scene.main_camera_set` / `scene.main_camera_clear` 编辑场景主相机引用，详见[场景相机](projects.md#场景相机)。

Control 的相机和视图 float 值与 Scene 共用 `json_format::number(float)`，写作可往返的最短十进制；manifest 与 inspect 使用同一编码。常规透视采用无限远 reversed-Z 深度映射：设备深度为 `near_clip / view_z`，near 映射为 1，深度 0 反投影为无穷远方向。`far_clip` 仍是相机 +Z 方向的有限裁剪平面，快照派生 `far_depth = near_clip / far_clip`，不新增作者字段或持久状态。正交仍为有限 reversed-Z，near=1、far=0，派生 far_depth=0。背景始终为 0，不按 far_depth 判断，也不把裁剪后的深度区间重新归一化。far 改变仍触发投影 cut，即使透视矩阵本身没有改变。有限且有序的裁剪值仍按既有范围接受；FP32 矩阵和运算不承诺一至数 ULP 极窄区间的端点保真，不另加固定间距阈值。

持久网格平面由相机派生，view.set 拒绝改变它，临时 compare 可以覆盖。Scene Lit 背景为黑色，环境光来自 World 的天光设置；background 仍保存工作室模式偏好。天光是 scene 项 `sky_enabled`（bool）、`sky_intensity`（cd/m²）和 `sky_color_x/y/z`（线性 RGB 三个标量），与其他 World 选项一样经 `view.set` 写入 Environment 组件，第一次写入时创建 Environment，二者是同一条历史；`sky_enabled` 为 false 时环境辐射为零。

作者项 `direct_candidates` 通过相同的 `view.set` / `render.compare` 接受 1～32，默认 8；它控制 DDGI 探针命中点每次挑选的局部光候选数，方向光独立评估，屏幕直接光不受它影响。视口诊断 `spatial_light_sampling` 默认 true，控制探针命中点是否使用近源空间提议；false 只禁用该提议，仍保留 CPU 网格及 GPU 表以便同场景对照，不写 World 或 Undo。屏幕直接光只有一条路径（tile + depth-slice 的保守局部灯列表，逐盏确定求值，列表超过 64 项时回退完整枚举）。`direct_reference`、`direct_clustered` 与 `direct_shadow_cache` 已删除：`view.set` 和 `render.compare` 对这三个键返回 `invalid_request`，消息标明被拒绝的字段。`direct_cluster_inspect` 默认 false，按需统计 GBuffer 接收像素的局部解析灯支持数量；它不重置光照历史、不持久化，但其诊断工作计入列表构建，性能采样必须关闭。当前接口不接受旧阴影槽预算与单灯样本参数。

DDGI 诊断通过相同的入口使用 `probe_visualization`（off/irradiance/state/update_age/distance）、`probe_volume`、`probe_relocation`、`probe_xray`、`probe_radius`（最小间距的 0.01～0.45 倍）。既有 `probe_debug` 改为真实八邻 Receiver Coverage，默认为 false。具体显示、年龄、资源和精度合同见 [Scene Lit](../renderer/scene-lit.md#ddgi-空间与接收面诊断)；关闭 DDGI 不会自动开启光照，灰色表示当前无数据。

多体积字段统一命名为 `probe_volume_0_*` 至 `probe_volume_3_*`，后缀为 `enabled`、`placement`、`min_x/y/z`、`max_x/y/z`、`count_x/y/z` 和 `blend_cells`。`placement` 取 `fixed` / `follow_focus`；后者以相机 target 为中心解释边界偏移。共享传播设置为 `probe_update_budget`、`probe_rays`、`probe_bounce`、`indirect_gain` 和 `probe_feedback_visibility`。`probe_debug_volume` 为 −1（全部）或 0–3，`probe_coverage` 为 status / volume / contributors / dominance。范围由 `capabilities.display_options` 返回，跨字段约束复用 Renderer 校验，整个 patch 原子发布。

`op.call view.set`、`render.compare` 和复现重开只接受正式字段名称。观察与复现导出也只写正式名称；复现设置从默认值恢复，不继承当前会话中的额外体积或跟随模式。反馈/可见性改变会重建光场历史，最终间接光增益只影响合成；各体积容量和滚动网格的约束见 [Scene Lit](../renderer/scene-lit.md)。

显示发布、相机更新和复现恢复在提交前用候选相机 target 校验跟随网格的世界坐标精度。超出受支持位置范围、或网格在该位置因浮点精度而塌缩时，整个操作失败，保留原相机、设置和 token；临时渲染对照也使用同一校验。

工作室 Cavity 同样通过 `view.set`、`render.compare` 和复现工程使用统一 ViewOption：`cavity` 为 bool，`cavity_ridge` / `cavity_valley` 为 0–2，`cavity_radius` 为 0.5–4 物理像素。默认开启且三个数值均为 1。MatCap、Smooth、Flat 使用该效果；Scene Lit 与纯线框保留设置但不执行效果。非法值使整个 patch 失败，不部分应用。

当前不包括布局编辑、Source 文本编辑、Play 命令、鼠标回放或任意 C++/JIT 调用。后续能力按业务名称接入现有权威模块。

`material.inspect` 接受 `asset` 或 `object` 二选一。返回 role、asset、parent、scene、default_material、dirty，以及 `declarations`（path/type/value/initial/components）、`entries`（材质 v2 typed entries）和 `diagnostics`（path/message）；父链中存在已加载 MS 文本时，另带 Session 保有的精确 `source: {path, version, text}`。声明值是当前有效结果，本地项保留 enabled 和休眠/失配值；`properties` 不承载固定材质参数。单次覆盖修改在发布前校验输入并解析所需的纹理。读取失败时返回 `operation_failed` 与首条诊断，原有效值、本地项和编辑历史均保留，随后可以继续编辑或保存。场景实例由 `scene.save` 保存，共享资产由 `material.save` 保存。不可用输入或父链的默认替代伴随诊断，保存仍保留原资源键，不把替代结果写回作者项。

## 协议、身份和重试

命名管道使用 FrostFoxAtelier.Control.<instance_id> 名称、当前用户 ACL，拒绝远端客户端。字节帧为 4 字节小端无符号长度加 UTF-8 JSON。消息最大 4 MiB、JSON 最大 32 层，拒绝重复字段；非法长度断开连接。

请求包含 protocol: 1、instance_id、request_id、op、args 和按需提供的 state_token。正常响应关联前述身份和 token，返回 status: ok / pending / error、result 或 error。无法解析的消息没有可用的请求身份，服务端仅回复 `status` 和 `error`。客户端将这种解析拒绝作为 `invalid_request` 返回并保留解析位置；响应声明的身份不匹配时返回 `protocol_mismatch`。

一个管道实例一次服务一条连接，各请求顺序收发。连接等待期内端点持续占用时，`Client::create` 返回 `busy`。已连接客户端的 I/O 超时返回 `timeout`，对端断开返回 `disconnected`。读写中断后客户端关闭该连接，后续请求须重新连接；宿主丢弃已检测到断连且尚未派发的请求。

- 实例身份包含随机 ID、PID、进程创建时间、构建和 exe SHA-256。客户端先核对 PID 与创建时间，再验证管道名称和服务进程。已退出或 PID 已复用的实例返回 `stale_instance`，发现时同样过滤。
- 有副作用或需要固定上下文的操作要求最近的 token。过期返回 stale_state；错误消息列出操作名、客户端和当前版本，以及两者之间每次版本变化的 Observation 字段名（最多保留最近 16 次，超出时明确标记截断；token 的实例不同时直接说明）。重新观察后再决定是否继续。
- token 覆盖文档代次、单调编辑版本、选择、相机、显示偏好、窗口映射、工程/已发布代码状态和用户输入。Undo 游标及渲染 revision 不承担此职责。
- 对象句柄仅在当前实例和文档代次内有效，不持有对象；离开成员关系返回 stale_object。重开文档后重新发现。
- 缓存最近至多 256 个改变状态的请求，总计最多 8 MiB。同 ID、同内容重放缓存响应；异步任务完成后，缓存更新为最终结果。同 ID 改内容返回 request_id_conflict。只读轮询不挤占缓存。这是有界重试缓存，不是持久事务日志；旧响应的 token 不是当前状态的新证明。
- 未提交的字段草稿、IME/剪贴板请求、菜单、按压/拖动或相机手势返回 busy，不会先取消用户操作。单纯文本焦点及提交/取消后保留的输入绑定不构成 busy。业务通知可能在提交后失败，此时应重新观察返回状态。
- editor.inspect 的 busy_reasons 列出实际阻塞项：document_edit、viewport_tool、camera_orbit、camera_pan、view_edit、menu、ui_pointer、ui_key、text_draft、ime、clipboard、ui_popup；`view_edit` 表示 World 数值草稿。text_focus 单独表示文本键盘焦点，不参与忙碌判定。交互导致的 busy 错误同时附带 busy_reasons。Code 文档的稳定未保存草稿仍由既有保存/替换规则保护，repro.export 可将其单独保存为 drafts.json。
- 首版一个活动连接、一个异步任务，保留最近 16 个任务。job_id 是原请求 ID；轮询响应的 request_id 属于本次查询。请求内容已淘汰但任务仍保留时，同 ID 返回 request_id_expired，应以 job.status 查询；不会再创建同 ID 的任务。任务记录淘汰前保存其最终缓存结果。两种记录均已淘汰后没有重试保证。

主要错误为 invalid_request、unknown_operation、interaction_required、busy、unsaved_changes、stale_state、stale_object、request_id_conflict、request_id_expired、not_ready、cancelled、state_changed、timeout、render_failure、dependency_mismatch、invalid_destination、storage_error、result_too_large。上下文取消会附上改变的状态类别。

相机平移和旋转属于同一个活动手势的互斥模式。模式、按键和上次指针位置只在手势存活期间保有；松手、失焦、捕获丢失及转入菜单会结束该手势。`camera_pan` / `camera_orbit` 只报告当前操作，不依据上次使用过的模式。结束后 Control 恢复可调用，活动期间仍按原合同拒绝改变状态的请求。

## 后处理录制视图

`render.trace` 是需要 state_token 的异步诊断操作，使用现有 job.status/job.cancel、请求去重、输出目录和取消规则。唯一范围为 `scene_lit_post`（也是默认值），从 Scene Lit 的 TAA 起点到 SMAA 末尾，不包含之前的光照、末尾 display draw、UI 或 Present。跟踪请求依次校验 scope、当前着色模式和 timeout_ms。未知范围返回 `invalid_request`，当前视图未选 Scene Lit 返回 `not_ready`，超时值越界返回带字段路径的 `invalid_request`。通过校验后才发布 job。需要就绪管线时先调用 render.prepare。trace 不切换显示、不修改 AA/Bloom、不推进作者历史、不激活窗口，也不为等到 accepted 自动重试。

    {"op":"render.trace","state_token":"INSTANCE_ID:VERSION","args":{"scope":"scene_lit_post","timeout_ms":120000}}

它只收集下一次**请求视图的首次 render 尝试**。同一 Application step 随后采用旧 Lit 或 Smooth 回退时，不携带该快照借用，也不覆盖首轮 FrameResult/错误。未进入后处理范围返回 `observation.status: not_entered`；进入后正常关闭且全部观测到为 `complete`；中断、截断或未知绑定为 `incomplete`。这些与 `frame_status: accepted / skipped / not_ready / cancelled / error` 独立：导出成功的 job 可以包含失败的 render 尝试。一直没有 render 尝试时沿原期限失败，默认 120000 毫秒，可设 1–600000。

若本次宿主步骤因首轮或后续 fallback 的终止性错误立即退出，会先等待这份自有 CPU 快照的文件写入结束，避免关闭时取消任务而丢失首轮证据；仍记录首轮结果，不把 fallback 的错误嫁接给它。此等待不涉及 GPU，不补录或重试渲染；普通取消、断线和期限规则不变。

结果给出 `json`、`html`、`manifest` 路径。`trace.json` 的 `schema: atelier.render.trace`、`version: 1` 包含实例/文档、UI frame、gfx recording sequence、实际请求视图、首轮结果和有序 `commands`；每条命令包含快照内 kernel 标识、entry、groups 和实际绑定。直接 dispatch 的 groups 来自录制参数；间接命令为 null，不猜测 GPU 缓冲内容。绑定记录 root_parameter/array_index、名称、kind、valid/dummy/unknown、resource 和 view 范围。`resources` 的 index 只在这次快照内有效，格式数值为 DXGI_FORMAT；不导出原生地址、常量值或资源内容，不把物理 index 当作者 ID。manifest 的 complete 只代表产物写完，不代表观测完整或 GPU 成功。

`trace.html` 直接由同一事实生成内联 SVG、可展开绑定详情和资源表，不依赖外部脚本、不自动打开。实线箭头仅表示 CPU 命令录制顺序，虚线仅表示绑定；SRV/UAV 不等于完整读写集合，同一资源的多次绑定不推导生产者依赖。没有观察到命令时显示空态，不套用预制流程图。资源来源没有在本范围内证明，图不声称 GPU 完成或显示器显示。采集与有界存储见 [RHI 合同](../rhi/contract.md#按需命令录制观测)；录制视图目前没有自动验收。

## 渲染对照

冷启动先调用 render.prepare，等待 phase=ready，再重新观察状态并发起正式对照。准备需要可绘制视口和网格实例，默认临时使用 Scene Lit 填充模式，不改变基础显示。管线在工作线程上准备，编辑器在此期间画回退帧（见[编辑器](README.md)中的 Scene View 说明）；回退帧不计入任务的接受帧，也不成为 last_frame，任务在第一个真正画出该视图的接受帧完成。进行中的 job.status 附带 compiled/total，即该视图需要的管线中已完成的数量和总数；total 随视图启用的管线组变化，不保证不变。准备默认期限 600000 毫秒，CLI 也应指定 --timeout-ms 600000；成功结果为 phase=ready 和该接受帧。

    {"op":"render.prepare","state_token":"INSTANCE_ID:VERSION","args":{}}

Shadows 开启时，投影光源的可见性一律来自阴影 atlas；普通 view 快照与 Performance 的 `configuration` 不再记录直接光路径。

当前 Renderer 首次 Scene Lit 绘制同步准备相关管线，同实例可复用；目前没有公开的异步准备入口。同步调用期间主线程无法及时处理状态查询和取消，取消在下一个宿主边界执行。准备阶段与正式采样分开，避免把冷启动混进等待帧；渐进准备和跨进程 shader 缓存由 Renderer/RHI 后续演进。准备进程的 CPU 时长或工作集不能作为 GPU 帧时。

准备耗时应同时记录构建配置、显卡/驱动、是否启用 GPU validation 及管线是否已准备。开启 --gpu-validation 时的耗时不能直接代表普通编辑器启动体验，也不能与未开启验证的样本直接比较。当前协议未返回显卡及验证模式，调用方需随测量记录这些条件；单次工作流观察不等同于性能基准。

    {
      "request_id": "shadows-ab-01",
      "op": "render.compare",
      "state_token": "INSTANCE_ID:VERSION",
      "args": {
        "cases": [
          {"name": "on", "patch": {"shading": "scene_lit", "shadows": true, "exposure_ev": 0}},
          {"name": "off", "patch": {"shading": "scene_lit", "shadows": false, "exposure_ev": 0}}
        ],
        "sample_frames": [1, 16, 64],
        "timeout_ms": 120000
      }
    }

每个 case 从开始时的基础设置派生，顺序执行。最多 8 个 case、16 次采样，原始颜色加深度最多 256 MiB。sample_frames 为该 case 生效后的接受帧序号，范围 1–4096、严格递增；[1,16,64] 分别经过 0、15、63 个接受帧后采样。采样任务默认期限 120000 毫秒，可设置到 600000。跳过帧单独计数；最小化、遮挡或没有视口时不伪造完成。

实验只覆盖本帧 Renderer 设置，不写基础偏好、场景或 Undo。用户输入、相机/场景/窗口/工程/代码的实际变化取消采样；校验失败及无实际变化的指令保留任务。进入文件写入阶段的数据已经独立拥有，之后的场景变化不会改写这份历史快照；断线、显式取消和截止时间仍有效。取消保留首次原因，重复取消不覆盖已记录的结果；后台写入结束后才释放活动任务位置。

结果在控制目录 artifacts/<instance>/<random>/：

- PNG：完整客户区的 sRGB RGBA8，左上角原点。
- .depth.f32：紧密排列的小端 float32，左上角原点、Reversed-Z near=1、background=0；透视 far 平面深度为 near/far，正交 far 为 0。现有 Renderer 提供不带 TAA 抖动的编辑器深度，使用 metadata 的 viewport 定位场景区。metadata 与 `last_frame.camera` 同时提供 `view`、`projection` 和 `far_depth`，三者共同描述已接受帧的投影与有限裁剪边界；不能只凭透视矩阵重建作者 far。
- 每次采样 JSON：接受帧号、全局计数、case 内序号、viewport、相机矩阵、有效设置、Sky radiance、等待/跳过帧和 GPU 统计。`frame_stats` 提供当前帧的 `history_resets`、`light_sources`、`shadow_sources`、`di_candidates`；后两者是启用 Shadows 时具备投影资格的源数和初始候选预算，不是实际射线数。`spatial_light_sampling` 子对象返回当前编码帧的 `retained_sources`（局部表保留源数）、`nonempty_cells`、`resource_bytes`（三张表的逻辑容量，含空表 sentinel）和 `uploaded`；它们不是完成帧 GPU 计数，也不是可见光源或剔除结果。未编码 Scene Lit（例如无几何且不绘制探针诊断）时这四项为零，其他保留的 Scene Lit 缓存仍计入总 `resource_bytes`。`case_reported_history_resets` 累计本 case 所有已报告帧的重置次数。旧阴影槽统计不再属于当前输出。
- manifest.json：文件大小及 SHA-256，最后发布的完整产物标记。

GPU 统计带独立采样帧号和有效标志，可能落后于截图。`gpu_sample.ddgi_pass_ms` 的两项分别为探针更新/传输与接收点 gather 耗时；`ddgi_visibility_rays` 的两项分别为探针命中着色与接收点 gather 的可见性查询数。它们共用 `valid`、`sequence`、`frame_id`，分别是 `pass_ms[3]` 和 `shadow_rays` 的细分，不能再次相加。相机矩阵与深度使用不抖动的编辑器投影；TAA/SMAA T2x 内部栅格投影有抖动，本接口不提供该内部矩阵。捕获会等待 GPU，不是常规帧性能测量。Renderer 按自然变化维护历史；接口不承诺完整清空 DDGI 探针、逐位确定性或恢复原 GPU 历史。

接受帧与捕获元数据的 `lighting_focus` 是实际传入 Renderer 的观察目标。`frame_stats.probe_volumes` 按固定槽位返回启用/滚动状态、世界边界、间距、数量、resident、updated 和 reset_probes；`probe_volumes_source: cpu_layout_and_schedule` 明确其来源。resident 是分配的槽位数，updated 是本帧计划更新数，均不能当成 GPU 完成数或有效探针数。

`last_frame.frame_stats` 还提供当前接受帧的 `scene_draw_calls`、`scene_instances` 和 `scene_culled_instances`。后两项分别为主视图栅格化判定的总实例数和保守视锥剔除数；它们不是 GPU 完成样本，也不表示从 TLAS、发光或阴影场景中删除了对象。

### DDGI 单接收点归因

`view.set` / `render.compare` 共用视口选项 `probe_history: live | hold_irradiance | hold_distance`、`probe_inspect: bool`、`probe_inspect_x/y: 0..65535`。坐标是窗口物理像素；不改变 World 配置和 Undo。归因结束恢复 `live` 并关闭 inspection。

`last_frame.frame_stats.probe_inspection` 为可空的异步完成样本，提供自己的 `sequence/frame_id/scene_identity/pixel/history/rays/updates/volumes`。`receiver` 返回实际 gather 状态、覆盖、权重和及贡献支配度；每个 probe 返回支持阶段、年龄、呈现/运输 RGB 与方向观测数、空间/朝向/距离可见性/最终权重和位置。`transport_observations` 对应权威运输场，`irradiance_observations` 对应显示场并镜像运输成熟度；不能把后者当成额外独立样本。空场、停用 DDGI 或关闭 inspection 返回 null，背景/越界产生 `position_valid.w=0` 且无邻居的样本。完成帧可能滞后或缺样，调用方按 sequence 去重并使用样本自己的布局，不能拼上当前截图冒充同步数据。

`response_epoch_min_max_visits` 是请求代次、整个方向 tile 的最少/最多有效响应观测（0～12）及有效访问游标。`traced_probe` 按前一完成样本中最大的有效邻居权重选择，当前帧未更新它时 `ray_samples` 为空；这些记录只含实际可着色命中，不包含天空和未着色命中。每条提供 source/kind/ray ordinal、局部 diffuse/specular 和方向光 diffuse/specular 亮度、命中位置/roughness、RIS weight/接收点完整混合提议 PMF/反馈亮度/主射线距离。需要整批均值时除以 `rays`，不能除以有效记录数。诊断专用 Update 入口和读回按需创建，正常着色编译掉记录代码；仍共用完成序列、32 槽无等待发布与 Renderer 资源退休。

### 性能观测

`editor.inspect` 的 `performance` 包含状态、最多 120 个完成样本、发布时间间隔、样本 sequence / frame_id、场景与域首帧、RT/视口尺寸、候选数/AA/DDGI 配置，以及 16 行层级统计（八个主阶段、七个子阶段、一个合计）。索引 0～7 为主阶段，8～9 为 DDGI update/gather，10～11 为 DDGI update 的主射线与命中照明，12～14 为 Direct lighting 之下的 `direct_cluster`（Light list construction）、`shadow_cache`（Shadow atlas updates）与 `direct_shading`（Shading / HDR composition），合计为 15；消费者应按稳定 `id` 识别行。`current_ms`、`mean_ms`、`p95_ms`、`max_ms`、`share_percent` 在没有对应观测时为 null；未启用项的当前 `state` 为 `skipped`。`active_samples` 表示窗口内计时 scope 启用的样本数，不是 GPU 命令数。表格和接口消费同一约 4 Hz 的发布快照，不因查询增加计时样本或读取 GPU。

`performance.cpu` 是同一快照的 CPU 段：`state`（waiting / ready）、`window_capacity`、`samples`、最新样本的帧计时序号 `sequence` 与接受 UI 帧 `frame_id`、相邻采样步之间的 `frame_interval_ms`（mean / p95 / max）及其均值的倒数 `frames_per_second`、自启动以来所有步（不论是否采样）累计的 `structure_errors` 与 `first_structure_error`，以及 `phases`。`phases` 按目录顺序列出全部已安装阶段，包括本域从未进入的阶段（`state` 为 unavailable，统计为 null），最后是 `frame.unattributed`；每项带 `id`、`title`、`parent`、`depth`、`current_entries`（最新样本中的进入次数，unattributed 为 null）和与上文各行相同的统计字段。消费者按 `id` 识别阶段，不依赖数组下标；行与统计范围以[帧性能](README.md#帧性能)为准。

`logical_resource_bytes` 是当前 CPU 所知的 Scene Lit 逻辑资源分配，和异步计时样本不是同一时刻的物理显存统计。`total_semantics` 明确只合计八个主区间；子项通过 `parent` 关联，`included_in_total` 防止重复累加。生命周期与统计范围以[性能表](README.md#帧性能)为准。

原始 `last_frame.gpu_sample` 包含八项 `pass_ms`、两项 `ddgi_pass_ms`、两项 `ddgi_update_pass_ms`、`direct_cluster_ms`、`shadow_cache_ms`、`direct_shading_ms` 及 `pass_mask`；`direct_pass_ms` 与 `direct_reconstruction_pass_ms` 已删除。mask 位 0～7 为主阶段、8～9 为 DDGI update/gather、10～11 为 DDGI update 子阶段、12 为灯列表、13 为阴影 atlas 更新（本帧有面重绘时才置位）、14 为着色与 HDR 合成；这些数组、耗时和 mask 共用完成 sequence。`direct_cluster` 返回 `cells`、`overflows`、`max_sources` 三个 uint32 完成帧统计，分别为有接收面的非空 cluster 数、候选数超过 64 而回退完整枚举的 cluster 数、单个 cluster 的最大保守候选数；它们不是每像素实际灯数。三个计数与耗时共用外层 `valid`、`sequence`、`frame_id`，不能作为当前 CPU 编码帧的数据。 `direct_cluster.receiver_histogram` 为 66 项 uint32：bin 0–64 是精确计数，bin 65 是超过 64 的接收像素数；`receiver_max_sources` 是最大支持数。仅统计有效 GBuffer 接收面通过支持球裁剪后的解析局部灯，不含太阳，也不表示遮挡后的灯数；诊断关闭时 histogram 与 max 均为零。这些值同样属于该完成帧，不新增 Performance 行。`shadow_cache_ms` 仅包含 atlas 更新，GPU mask 不包含性能表内部的合计位 15。`shadow_cache` 返回 faces、reused_faces、static_updates、dynamic_updates、draws、resolution_downgrades、dropped_lights、near_plane_faces、missing_taps、plane_fallbacks、deferred_faces、deferral_frames、atlas_cells、pending_lights，均与外层完成帧 sequence/frame_id 对齐；不是 CPU 当前帧数值。`dropped_lights` 是 atlas 最低分辨率也放不下、本帧整盏不贡献直接光的投影光源数；`deferred_faces` 是本帧因重绘预算沿用上一次投影与内容的过期 face 数，`deferral_frames` 是被延后的光连续等待的最长帧数，`atlas_cells` 是全部请求 face 占用的 128² 格数，`pending_lights` 是驻留但仍有 face 没画、本帧不贡献直接光的投影光源数；`pressure_batches`、`rt_fallbacks` 与 `clamped_taps` 已删除。`frame_stats` 不再有 `di_candidates`。数值与质量归因统一见 [Renderer 验收记录](../renderer/acceptance.md)。`resource_bytes_source: current_cpu_logical` 说明旧 `resource_bytes` 字段的来源，不能把它当作采样帧物理驻留量。

## 复现工程

`repro.export` 在编辑器线程冻结场景及依赖。快照失败直接返回协议错误，成功后才发布可轮询的任务。后台写入副本、核对依赖并生成响应路径，最后原子写入带 `complete: true` 的 `manifest.json`。取消先被接受时，任务保留其原因，残留的文件可供排查。后台先进入最终发布时，`job.status` 报告这次发布的实际结果，包括清单写入失败。重复取消保留原结果；`repro.open` 要求完成清单并校验其中的文件。

repro.export 读取 Session::serialize() 的内存场景，保留原路径、dirty 和 Undo；scene.save 才是普通保存。整份复制 Config 和 Source；其余工程根只复制 `snapshot_resources` 冻结的类型化 `references` 闭包及身份 sidecar：场景网格、天空、材质、网格默认关系、MS/MI 父链，以及声明匹配且启用的纹理。禁用、已删除声明和改型失配的条目原样保留为作者数据，不因此复制或加载图片。场景生成器随包保存所引用的图；网格依赖取自实际求值和发布。若启用的生成器在 Display 分支使用会话冻结，导出前用作者图和各实例实际种子核对冻结输出与传递资源依赖。上游变化导致输出无法重现时，导出返回 `dependency_mismatch` 并指出节点；未参与 Display 的冻结节点不影响导出。重开时核对候选场景的依赖，启用的生成器求值失败会拒绝重开，未参与求值的节点可以保留缺失资源键。宿主不重新遍历磁盘材质，避免旧文件引用覆盖当前草稿；原始 glTF buffer、OBJ MTL 等伴随文件单独取自 `Reader::mesh_dependency_files`，不递归材质。文档仍在使用但目录已无同类资产时拒绝导出；已知缺失资源的键保留在场景，严格重开仍拒绝缺失的有效依赖。字体、样式和着色器来自安装目录，Cache 是派生数据，不进入复现包。没有工程时创建独立工程容器，副本 startup scene 指向当前内存场景。

工作线程只持有拥有型作者数据与不可变内容，不持有 Runtime 根。`material_sources` 固定 Session 已加载的精确 MS 文本，`textures` 固定有效输入的 RGBA 像素；导出不回读这些内容的新磁盘版本。图片以无损 PNG 编码写入原资产路径，保持文件名和身份，只排列 RGBA/BGRA 通道，不转换颜色空间；重新解码逐像素核对。一个纹理键对应不同已加载像素、或同一 Source 路径对应不同文本时明确拒绝，不能任取一个版本。

保存相机、显示偏好、Entity 选择顺序和检查的 Component、构建身份及资源指纹。publication_version 标识完整已发布构建，source_version 标识项目与引擎源码集，ui_source_version 标识 UI 输入。副本 Source 只包含该成功构建的游戏和 UI 源码；磁盘上新增或修改但未发布的源码移入 unpublished/，内存 Code 草稿另存 drafts.json。没有可用场景或成功发布构建的工程返回 not_ready。这是场景与代码复现，不保存整个桌面、编辑器布局或 GPU 历史。

引擎 `engine/...` Source 仍由安装提供（开发态优先源码树的 `assets/engine`，部署态为可执行文件旁的 `engine`），不复制进副本的项目 Source。manifest 的 `engine_sources` 逐路径记录成功发布时的引擎源码文本 SHA256（按源码读取规则去除 UTF-8 BOM），不是导出时重新读取磁盘的版本。没有工程时记录当前安装的引擎输入。`repro.open` 要求当前安装的引擎源码路径集合和文本指纹完全一致；增删或内容不同返回 `dependency_mismatch`，不替换当前文档。依赖检查至候选编译期间要求安装输入保持不变，不提供并发安装更新的原子快照。不支持用复现包覆盖引擎源码；缺少此依赖字段的旧包须重新导出。

manifest 必须含 `engine_assets: {version: 1, records: [...]}`，空集合也写出。每条记录为 `key`、`kind: "mesh" | "material"`、`semantic_version`、`digest_version`、`content_sha256`；按 key 排序输出，输入顺序无语义。仅接受本安装注册的精确内建网格和材质键，不承认任意 `builtin/` 前缀。mesh 摘要 v2 描述几何、槽及默认关系；material 摘要 v2 包含 MS 作者数据与 Source 文本，以当前确定性 JSON writer 编码。未绑定的网格槽声明隐式 DefaultWorldGrid 依赖。语义版本由生产者维护，编码见[资源读取](projects.md#引擎依赖摘要)；它们不认证任意 C++/shader 实现，也不是安装二进制签名。

严格根集是冻结 Scene 的资源位及全部 World 成员（含停用成员）的网格/默认关系，再按身份取相应材质的当前草稿；不包括整个会话材质缓存或全部复制文件。普通名字/属性字符串不是资产协议。导出逐份核对加载的引擎资源，再写唯一依赖记录；重开先预检实际 Scene 资源位及其默认材质/有效贴图中的引擎键，候选发表前使用与导出相同的实际集合收集器，与清单双向比较。因此删一条或清空 records 不能绕过真实使用项，多报未使用项也拒绝。

`material_resources: {version: 2, records: {...}}` 同样必需，核对可达材质的作者记录（包括休眠条目和 enabled 标记）、父身份、精确 Source 文本及有效图片尺寸/RGBA 摘要。manifest 的 `json_bytes_version: 2`、material_resources 版本与天空资源 `digest_version: 2` 共用 `assets::json_format::json_bytes_version`；网格仍使用独立的二进制摘要 v2。缺少必需依赖字段的旧包、未知 schema/编码版本返回 `invalid_request` 并要求重新导出；重复键、非正整数版本、非法摘要也是 `invalid_request`。未知/错种类引擎键、语义版本/内容不同、实际集合缺项/多项返回 `dependency_mismatch`。真正文件读取或密码提供者失败为 `storage_error`，不伪装成内容不一致。dirty、材质/Source 草稿与运行 Play 的替换前置拒绝保持不变；候选验证失败不关闭旧文档或改变历史、选择和相机。只支持 Scene v10、材质 v2，不提供旧格式转换。

复制期间复核普通文件集合和内容，精确 Source/图片快照不与后来改变的源文件混用。在副本中重新加载资源，与文档实际持有的 Mesh 顶点、边、分段、槽和默认材质键比较；天空贴图比较纹素、2^k 缩放、均值和 cube。同键的不同已加载版本都必须匹配，不能只检查第一个；即使依赖文件集合相同，槽→材质关系不同也返回 `dependency_mismatch`。导出不按磁盘新关系重绑当前文档，不改变原文件、dirty 或 Undo。材质当前作者草稿只写入副本；严格加载允许正常保留的休眠/声明失配条目并提供诊断，但不把有效依赖缺失当成功。

不跟随链接，拒绝无法持久化的程序化资源。最多 10000 输入文件 / 1 GiB，单文件 256 MiB；manifest 使用现有 16 MiB、32 层及重复字段拒绝规则。文件操作共用支持 Windows 长路径的原子写入设施。输出不能位于源工程的任何根（`Project::roots`）内，包含大小写别名和链接别名；创建产物前返回 invalid_destination，避免复制自身。控制目录可以放在 Cache 内。

Windows 长路径转换覆盖产物复制、归档、文件时间读取、目录身份比较、实例发现与清理以及 CLI 请求文件。路径拼接和 canonical 解析后，在文件操作边界使用现有 windows_file_path 统一转换；协议中的绝对路径、工程相对路径和资源身份保持原格式。深目录副本可以重开、保存并再次导出。

repro.open 校验完整文件集合、大小及哈希，拒绝缺失、修改、额外文件或链接；运行时生成的 Project/Cache 不参与复现依赖。清单中的场景必须与副本工程配置的启动场景一致。预检和严格加载共用已打开的工程、启动场景字节和已校验的引擎依赖记录。候选会话完成编译、反序列化、依赖与环境核对，并恢复选择及检查目标后，才通过工程打开共用的发布步骤替换当前文档和视图。编译或反序列化失败保留当前工程和对象；常规 UI 的修复工程模式保持原有行为。有未保存场景或源码时拒绝替换。失败/取消可能留下诊断目录，未返回成功的任务不应当作成功交付。

manifest 标记 `view_schema: 7`，相机或视图快照合同改变时加一。`repro.open` 只接受当前 `view_schema` 和当前格式的场景；缺少标记、其他版本、带已删除选项（例如 `ssil`、`shadow_bias`、`direct_reference`）的视图，以及旧格式场景都拒绝。视图里的 World 设置与加载出的候选文档的 Environment 不一致时返回 `dependency_mismatch`；天空贴图和旋转角只由场景记录，视图快照不含这两项，核对前取自场景的 Environment。选择按导出时的 World 成员序号、检查目标按该 Entity 上的组件序号，在候选文档替换当前文档之前解析；序号越界返回 `dependency_mismatch`。视图版本或已删除选项不合格返回 `invalid_request`，旧格式场景在严格加载时失败，返回 `operation_failed`。任何验证失败都保留当前文档。

## 生命周期和扩展

外部执行链为：CLI / 自动化 / 验收 / LLM → 本机传输 → EditorControl 主线程调度 → Application / Session / Camera / ViewOption → 既有 Runtime、UI 和 Renderer。UI 与快捷键直接调用相同业务层。

传输线程只解析、排队和收发消息，引擎操作在主线程执行。当前每个连接串行处理请求，上一条请求得到响应后才读取下一条；异步任务先返回 `pending`，随后查询结果。

- modules/editor/control/ 与 apps/control/：传输、身份、JSON 边界、发现和 CLI，不依赖 Runtime/UI/GPU。
- modules/editor/application/src/application/editor_control.cpp：能力目录、主线程调度、token、句柄、重试和实验状态。Application 在轮询及接受帧边界调用。
- [view_options.hpp](../../../modules/editor/core/include/editor/view_options.hpp) 与 [view_options.cpp](../../../modules/editor/core/src/view_options.cpp)：菜单与指令的显示参数合同。Camera/Session 保留各自业务不变量。
- [control_artifacts.hpp](../../../modules/editor/application/src/application/detail/control_artifacts.hpp) 与 [control_artifacts.cpp](../../../modules/editor/application/src/application/control_artifacts.cpp)：拥有快照的文件及图像工作，只持有数据和不可变 MeshRef，不跨线程借用 Runtime。

关闭时先停止接入，再取消并等待文件工作，之后释放 Renderer 和文档。空闲帧只保留固定大小的最近帧信息，查询/采样时才构造 JSON。

新增能力同时补目录中的参数/副作用说明、主线程调度和业务端到端验收。UI 与指令调用同一个负责不变量的模块。以后可在此协议上增加 MCP 适配层，无须复制引擎业务。

### 源码调试接入（待实现）

已对齐的方向是：调试面板、快捷键和 `debug.*` 共用一个宿主调试会话，沿现有 [Runtime/JIT 调试核心](../../source-debugger.md)执行断点、单步、快照查询和受控终止。会话拥有固定代码版本与暂停状态；各调用方使用相同结果和错误，不另建 UI 或工具专属调试状态。

接入时必须满足以下约束；它们尚未构成可调用的协议能力：

- 调试动作核对会话和停点身份。继续或单步一经接受，该停点就不能再次控制执行；历史快照仍可展示，但不能恢复后续暂停。外部重试继续复用既有请求身份与缓存。
- 启动请求先响应，在外层安全边界进入执行。不能在请求分发中同步等待下一停点，再从停点递归处理同一个请求链。
- 停点处理器内需要受限的宿主消息处理，使调试指令和基于快照的界面保持响应；世界 tick、普通编辑操作和被暂停 Runtime 的 JIT 重入仍受限制。
- 关闭时先结束停点等待，待执行栈正常清理返回后再销毁宿主。控制权、连接结束以及一次性 CLI 调用的语义须一同落定，不能把 CLI 发完指令后的正常断连误判成取消调试。
- 核心当前只在命中停点时调用处理器，运行中的任意时刻暂停或中止还需要额外接线；长时间原生调用也不具备抢占保证。

这里的外部控制沿用本机管道。DAP、跨机器连接以及通用协议框架不在本轮接入范围。实现与端到端验收完成后，再将支持的调试指令加入 `capabilities`。

## 端到端验收

日志零错误冒烟复制 Examples 的 `Config`、`Source` 与启动场景所在的 `Content/<场景包>`（不复制 `Cache` 或其他无关示例目录），打开启动场景并接受渲染帧后，经 Control 执行一次 `scene.add` → `scene.save` → `edit.undo` 并确认撤销后的新接受帧，再启动 Play。观察到正式 `runner.started` 后，在 Play 仍活动时向编辑器发送 WM_CLOSE；沿用 300 秒退出等待及 exit code 0，不先 Stop、不自动重试请求。通过验收脚本串行运行窗口：

    pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Targets editor_log_smoke_acceptance

验收直接启动 `atelier_control.exe`，核对文件与标准输入、参数诊断、过期实例和进程退出码。宿主处理真实管道请求并暂停出帧，让跟踪任务保持 pending。分别缩短服务端任务期限和 CLI 轮询期限，核对两种 `timeout` 的消息及任务身份，并在 CLI 断连后从新连接核对宿主记录的取消原因。

`op.call layout.reset` 经宿主返回布局容量错误时，CLI 输出 `operation_failed` 和原始诊断，退出码为 1。验收耗尽实际节点身份后检查失败响应的请求 ID、状态令牌和重放结果，并验证失败前的视口仍有焦点。

窗口冒烟通过真实 CLI 发起 `render.capture` 和 `repro.export`。验收先核对捕获参数的首错，再用普通文件占住实例的工件目录。两种任务都应在轮询中返回 `storage_error` 和文件系统原始诊断，CLI 退出码为 1。随后用新的 CLI 进程取消、查询同一任务，核对错误和任务身份，并用查询请求 ID 确认直接返回终态。恢复目录后重新捕获，核对成功退出、完成清单、PNG 文件头和深度文件长度。

复现包验收在工程副本的 Source 里放入超过单文件上限的输入，检查 `job.status` 和随后取消返回相同的 `resource_limit` 原因，再通过 `repro.open` 打开半成品，核对拒绝结果与当前场景身份。验收另用独占文件句柄制造真实读取失败，将返回的 `storage_error` 消息与底层哈希读取诊断逐字比较。移除故障输入后重新导出，核对完成清单中的场景和工程资产身份。暂时移走一个实际引用的身份文件时，快照直接返回带资产键的 `dependency_mismatch`，查询该请求的任务 ID 返回 `unknown_job`。

打开验收通过 Control 新建工程、添加内置网格并导出复现包。它先等长修改场景文件，核对内容摘要错误；再为含重复对象身份的场景更新清单，确认严格加载返回首个构造错误。验收还分别检查了选择越界、清单场景与工程配置不一致，以及漏报内置网格依赖。每次拒绝都与请求前的工程、场景、对象句柄、选择、历史、相机和视图比较。恢复原包后，验收核对新文档中的场景和选中实体。

分页验收通过同一 Control 连接取一项的首页，调整续页大小，将资产和对象逐项与 full 的内容及顺序对照。它同时核对名称筛选的继承、跨域和越界游标的诊断。新增 Entity 后旧对象游标过期，新增工程目录并刷新后旧资产游标过期，另一目录的有效游标仍可继续。含多个错误的游标会核对首错的协议码和完整消息。

该目标调用 Control harness 的 `--smoke-logs`。每次证据保存在构建目录 `log-smoke-acceptance/<唯一目录>/`：`editor.log` 是子进程 stdout/stderr，正式 JSONL 位于 `Logs/editor/` 及其中的 `runner-N/`；通过后移除临时 `Project/` 工程副本并写入 `passed` 标记。自动清理仅处理带标记的目录，保留最近 5 次通过的日志。中途失败的目录保留已生成的日志和现场。Reader 扫描完整轮转段，核对 Editor PID、父子 run 身份、全部附属 run 和磁盘 manifest；一次 Play 只能对应一个 Runner。父日志的 `play.process_ended` 必须确认该 Runner 的实际正常退出（非 forced、exit code 0），才排空子日志；Editor 则以进程句柄退出为依据。Runner 必须在同一文档中记录 started/stopped，且 `runner.stop_observed` 恰好一次；即时停止允许完成帧数为 0。两个进程都须记录 application.started/finished。

任何 error/critical、日志设施故障、Reader 损坏/丢失、不健康或非 final 的最终 health、缺失日志或退出证据都失败；读取期间的 final=false 不提前判失败。不会以“Reader 能打开目录”、没有 stderr 或进程退出码为唯一通过证据，也不把 warning 文本中的关键词当 fault。该冒烟不覆盖 glTF 导出后关闭等其他特定 K6 序列。

相机投影的 GPU 结果由 [Renderer 验收](../renderer/acceptance.md)的 hidden-line 代表覆盖。

### World 配置与字段显示元数据

`display_options` 每项包含 `scope: scene | view`。`op.call view.set` 保持单一批量入口：完整验证后，scene 部分经 Session 记一条历史，view 部分保存为当前视口偏好；重复候选不改 revision。`direct_candidates` 与天光键是 scene 项，写入 Environment 组件并随 Scene v10 保存；`spatial_light_sampling` 与探针显示是 view 项，对应 World Settings 的 Diagnostics。`render.compare` 的覆盖仅作用于临时帧，不写 World 或 Undo。`edit.undo/redo` 恢复 World 后，下次观察与渲染重新组合有效视图。`scene.set_properties` 使用嵌套字段名，inspection 同时返回 `label`、`category` 和 `description`，显示文本不参与字段身份。
