# Editor Control

Editor Control 是编辑器指令化操作与调试的标准入口。UI、快捷键、CLI、自动化、端到端验收和 LLM 都是调用方，共享指令语义、状态与结果、错误和生命周期合同。指令使用稳定业务名称，不暴露 UI control key、指针或 Runtime 私有布局。

UI 和快捷键直接调用负责业务不变量的执行层；外部调用经当前的本机管道与 JSON 适配到 Application、Session、Camera 和显示设置的相同业务入口。统一入口不要求 UI 序列化 JSON，也不把传输连接当成业务状态的拥有者。实例发现、外部请求身份与重试缓存由 Control 适配层处理。

本文的能力目录记录已实现操作。源码调试核心已经可用，编辑器调试面板与 `debug.*` 指令仍处于接入方案阶段，见[源码调试接入](#源码调试接入待实现)。

## 启动和调用

正常编辑器默认启用本机接口，--no-control 可关闭。发现目录默认为 %LOCALAPPDATA%/FrostFoxAtelier/Control/instances；--control-directory folder 可隔离目录。--no-activate 打开窗口但不抢键盘焦点。窗口验收模式及独立项目运行器不自动开放接口。

    . ./tools/enter-dev.ps1
    cmake --build --preset dev --target atelier_editor atelier_control
    ./out/dev/atelier_editor.exe --project E:/ffa_testproj
    ./out/dev/atelier_control.exe --list

从发现结果中明确选择 instance_id，不要自动连接第一个实例。先读取能力目录和当前状态：

    '{"op":"capabilities"}' | ./out/dev/atelier_control.exe --instance INSTANCE_ID --request -
    '{"op":"editor.inspect"}' | ./out/dev/atelier_control.exe --instance INSTANCE_ID --request -

将最近观察到的 state_token 填入 UTF-8 JSON 请求：

    {
      "request_id": "create-first-entity",
      "op": "scene.create",
      "state_token": "INSTANCE_ID:VERSION",
      "args": {"kind": "mesh", "mesh": "builtin/cube"}
    }

    ./out/dev/atelier_control.exe --instance INSTANCE_ID --request request.json

CLI 补齐协议及实例身份；没有 request_id 时生成新 ID。异步操作保持连接并轮询到完成，--timeout-ms 为 1–600000 毫秒，默认 120000。成功退出码为 0，错误为 1。超时退出会断开连接，宿主在下个检查点取消任务。

## 能力和业务边界

实时参数 schema、枚举和范围由 capabilities 返回，职责如下：

| 操作 | 权威入口与行为 |
| --- | --- |
| editor.inspect | 默认 `detail: full` 返回工程、内存场景、Entity/Component 句柄、反射属性、选择、资源、组件类型、相机、基础显示设置和最近接受帧；`detail: summary` 返回工程/文档状态、忙碌项、历史计数、相机、显示和最近接受帧，不遍历场景或资源；两者的 `performance` 共用 World 性能表的有界统计快照 |
| view.layout | `layout: fit_scene` / `scene_focus`，沿 World 页同一命令按当前可渲染几何拟合 DDGI 覆盖；保留传播和显示偏好，完整布局提交为一条场景历史 |
| project.open、scene.open、scene.save | Application 工程/场景命令；替换拒绝未保存场景或源码草稿，避免无人处理的保存对话框 |
| scene.create | Entity、StaticMeshEntity、Point/Rectangle/Directional/Sky Light，共用现有命令和 World 工厂 |
| scene.select、scene.rename、scene.delete | Session 的选择、名称和删除；rename 只修改 Entity 名称，Component 使用类型名 |
| scene.set_property | 按发现到的属性名及精确标量类型写入；枚举使用名称 |
| scene.transform | Entity 的可选内联完整 TRS；无空间时显式添加空间，一次撤销；位置/缩放三分量，旋转 quaternion xyzw |
| scene.add_component、scene.remove_component | 现有组件组合和生命周期；类型来自 component_types |
| scene.remove_transform | 移除指定 Entity 的内联空间，一次撤销 |
| scene.reparent | 调整 Entity 组合 parent；支持 keep_world 和组成 index，一次撤销 |
| scene.set_space_reference | 设置 Entity 空间参考：继承组合祖先、世界或另一个 Entity；支持 keep_world，一次撤销 |
| scene.undo、scene.redo | 文档原有撤销/重做链 |
| material.create、texture.import | Project 创建 `.material` 或导入图片，沿资源 GUID 索引与文件发布规则；重复请求由既有 request_id 合同处理 |
| material.inspect | 按资产 GUID 或实例对象句柄读取同一 Runtime 材质，返回解析后的属性、父资产、覆盖位与保存状态 |
| material.set_property、material.set_texture | 通过 Session 编辑材质对象；参数使用发现到的字段名/标量/枚举，贴图使用 input 名与 Texture GUID |
| material.assign | 按组件句柄与槽名赋予 Material GUID；空 asset 恢复网格默认 |
| material.override | 按真实参数字段名启用/取消实例覆盖；实例句柄由 editor.inspect 的组件 materials 返回，后续用 material.set_property 编辑 |
| material.save、material.undo、material.redo | 共享资产独立保存和历史；实例覆盖使用 scene.undo/redo。保存后目录刷新失败返回 saved 与 catalog_warning |
| camera.set、camera.look_at | 公共 Camera 状态和观察目标 |
| view.set | 原子应用显示 patch，与菜单共用 ViewOption 规则 |
| repro.export、repro.open | 内存场景的独立复现工程 |
| render.prepare | 通过真实接受帧准备当前实例的 Scene Lit 管线，结束后恢复基础显示；单独报告准备耗时 |
| render.capture、render.compare | 当前显示捕获或临时设置对照 |
| job.status、job.cancel | 查询和幂等取消任务 |

场景身份与参数统一使用 Entity 名称：`editor.inspect` 的目录和 `scene.select` 的有序选择数组均为 `entities`；没有 actor/entitys 别名。`scene.create` 的空实体 `kind` 为 `entity`。`scene.add_component` 对当前选中 Entity 添加 `type` 指定的真实组件。

`scene.transform / scene.remove_transform / scene.reparent / scene.set_space_reference` 的 `object` 必须是 Entity 句柄，组件不接受 TRS。reparent 的 parent 为 Entity 句柄，null 或省略表示顶层；keep_world 默认 true，index 可省略。set_space_reference 的 origin 为 null 时继承组合祖先，空字符串表示世界，Entity 句柄表示显式空间参考；keep_world 默认 true。

完整 inspect 的 Entity 包含 parent（顶层为空字符串）、children 句柄数组、可选 transform 和 space_reference（null / 空字符串 / 句柄）。组件目录保留反射参数与材质，组件记录不含 TRS。创建子实体复用 scene.create 和 scene.reparent；没有第二套 create_child 工厂或对象身份。

材质参数、纹理、赋予和覆盖复用与 UI 相同的事务，不直接写 JSON 或 GPU 数据。`editor.inspect` 返回 `dirty_materials`；替换文档会拒绝未保存材质。`repro.export` 在导出副本中保存实际材质草稿并核对已载入贴图内容，原工程不变。材质范围和一层参数继承合同见[资源工作流](projects.md#材质与贴图资源)。

坐标为米、左手系、+Y 向上，矩阵按行存储、向量左乘。CameraState 的 view 0–6 依次为 perspective/top/front/right/bottom/back/left，原有 0–3 编码不变；yaw/pitch 为弧度，distance 为 0.5–2000 米，恢复时验证相机基底。视图命令与 UI 使用同一六向相机入口，`camera.set` 仍恢复完整状态。

启用 GTAO 或 Local GI/SSIL 会关闭另一项；同一个 patch 同时请求两者为 true 会失败。持久网格平面由相机派生，view.set 拒绝改变它，临时 compare 可以覆盖。Scene Lit 背景为黑色，环境光来自场景的唯一 Sky Light；background 仍保存工作室模式偏好。

直接光作者项 `direct_candidates` 通过相同的 `view.set` / `render.compare` 接受 1～32，默认 8；它控制统一局部光源池的初始候选数，方向光独立评估。视口诊断 `direct_reference` 固定平均 64 次独立估计，每次保留当前候选预算，关闭 reservoir 复用和直接光颜色降噪。当前接口不接受旧阴影槽预算与单灯样本参数；它们只在 Scene v4 文件读取边界严格校验后丢弃。

DDGI 诊断通过相同的入口使用 `probe_visualization`（off/irradiance/state/update_age/distance）、`probe_volume`、`probe_relocation`、`probe_xray`、`probe_radius`（最小间距的 0.01～0.45 倍）。既有 `probe_debug` 改为真实八邻 Receiver Coverage，默认为 false。具体显示、年龄、资源和精度合同见 [Scene Lit](../renderer/scene-lit.md#ddgi-空间与接收面诊断)；关闭 DDGI 不会自动开启光照，灰色表示当前无数据。

多体积字段统一命名为 `probe_volume_0_*` 至 `probe_volume_3_*`，后缀为 `enabled`、`placement`、`min_x/y/z`、`max_x/y/z`、`count_x/y/z` 和 `blend_cells`。`placement` 取 `fixed` / `follow_focus`；后者以相机 target 为中心解释边界偏移。共享传播设置为 `probe_update_budget`、`probe_rays`、`probe_bounce`、`indirect_gain` 和 `probe_feedback_visibility`。`probe_debug_volume` 为 −1（全部）或 0–3，`probe_coverage` 为 status / volume / contributors / dominance。范围由 `capabilities.display_options` 返回，跨字段约束复用 Renderer 校验，整个 patch 原子发布。

旧九个 `probe_min_*` / `probe_max_*` / `probe_count_*` 名称仅映射槽位 0，映射见 `capabilities.display_aliases`；没有第二份存储。同一 patch 同时提供别名和对应正式名称会拒绝。观察与复现导出只写正式名称，重开仍接受旧名称；复现设置从默认值恢复，不继承当前会话中的额外体积或跟随模式。反馈/可见性改变会重建光场历史，最终间接光增益只影响合成；各体积容量和滚动网格的约束见 [Scene Lit](../renderer/scene-lit.md)。

显示发布、相机更新和复现恢复在提交前用候选相机 target 校验跟随网格的世界坐标精度。超出受支持位置范围、或网格在该位置因浮点精度而塌缩时，整个操作失败，保留原相机、设置和 token；临时渲染对照也使用同一校验。

工作室 Cavity 同样通过 `view.set`、`render.compare` 和复现工程使用统一 ViewOption：`cavity` 为 bool，`cavity_ridge` / `cavity_valley` 为 0–2，`cavity_radius` 为 0.5–4 物理像素。默认开启且三个数值均为 1。MatCap、Smooth、Flat 使用该效果；Scene Lit 与纯线框保留设置但不执行效果。非法值使整个 patch 失败，不部分应用。

当前不包括布局编辑、Source 文本编辑、Play 命令、鼠标回放或任意 C++/JIT 调用。后续能力按业务名称接入现有权威模块。

`material.inspect` 接受 `asset` 或 `object` 二选一；后者使用组件材质槽返回的对象句柄，可读取实例已解析的属性、父资产和显式覆盖位。实例修改由 `scene.save` 保存，共享资产由 `material.save` 保存。

## 协议、身份和重试

命名管道使用 FrostFoxAtelier.Control.<instance_id> 名称、当前用户 ACL，拒绝远端客户端。字节帧为 4 字节小端无符号长度加 UTF-8 JSON。消息最大 4 MiB、JSON 最大 32 层，拒绝重复字段；非法长度断开连接。

请求包含 protocol: 1、instance_id、request_id、op、args 和按需提供的 state_token。正常响应关联前述身份和 token，返回 status: ok / pending / error、result 或 error。无法解析的消息没有可用的请求身份。

- 实例身份包含随机 ID、PID、进程创建时间、构建和 exe SHA-256。客户端校验管道服务进程；发现时过滤已退出或 PID 复用的实例。
- 有副作用或需要固定上下文的操作要求最近的 token。过期返回 stale_state；重新观察后再决定是否继续。
- token 覆盖文档代次、单调编辑版本、选择、相机、显示偏好、窗口映射、工程/已发布代码状态和用户输入。Undo 游标及渲染 revision 不承担此职责。
- 对象句柄仅在当前实例和文档代次内有效，不持有对象；离开成员关系返回 stale_object。重开文档后重新发现。
- 缓存最近至多 256 个改变状态的请求，总计最多 8 MiB。同 ID、同内容重放缓存响应；异步任务完成后，缓存更新为最终结果。同 ID 改内容返回 request_id_conflict。只读轮询不挤占缓存。这是有界重试缓存，不是持久事务日志；旧响应的 token 不是当前状态的新证明。
- 未提交的字段草稿、IME/剪贴板请求、菜单、按压/拖动或相机手势返回 busy，不会先取消用户操作。单纯文本焦点及提交/取消后保留的输入绑定不构成 busy。业务通知可能在提交后失败，此时应重新观察返回状态。
- editor.inspect 的 busy_reasons 列出实际阻塞项：document_edit、viewport_tool、camera_orbit、camera_pan、view_edit、menu、ui_pointer、ui_key、text_draft、ime、clipboard、ui_popup；`view_edit` 表示 World 数值草稿。text_focus 单独表示文本键盘焦点，不参与忙碌判定。交互导致的 busy 错误同时附带 busy_reasons。Code 文档的稳定未保存草稿仍由既有保存/替换规则保护，repro.export 可将其单独保存为 drafts.json。
- 首版一个活动连接、一个异步任务，保留最近 16 个任务。job_id 是原请求 ID；轮询响应的 request_id 属于本次查询。请求内容已淘汰但任务仍保留时，同 ID 返回 request_id_expired，应以 job.status 查询；不会再创建同 ID 的任务。任务记录淘汰前保存其最终缓存结果。两种记录均已淘汰后没有重试保证。

主要错误为 invalid_request、unknown_operation、busy、unsaved_changes、stale_state、stale_object、request_id_conflict、request_id_expired、not_ready、cancelled、state_changed、timeout、render_failure、dependency_mismatch、invalid_destination、storage_error、result_too_large。上下文取消会附上改变的状态类别。

相机平移和旋转属于同一个活动手势的互斥模式。模式、按键和上次指针位置只在手势存活期间保有；松手、失焦、捕获丢失及转入菜单会结束该手势。`camera_pan` / `camera_orbit` 只报告当前操作，不依据上次使用过的模式。结束后 Control 恢复可调用，活动期间仍按原合同拒绝改变状态的请求。

## 渲染对照

冷启动先调用 render.prepare，等待 phase=ready，再重新观察状态并发起正式对照。准备需要可绘制视口和网格实例，默认临时使用 Scene Lit 填充模式，不改变基础显示。准备默认期限 600000 毫秒，CLI 也应指定 --timeout-ms 600000；成功的 preparation_ms 是请求到接受帧的墙钟时间，包含 UI、管线准备和绘制，不是独立的 shader 编译计时。

    {"op":"render.prepare","state_token":"INSTANCE_ID:VERSION","args":{}}

当前 Renderer 首次 Scene Lit 绘制同步准备相关管线，同实例可复用；目前没有公开的异步准备入口。同步调用期间主线程无法及时处理状态查询和取消，取消在下一个宿主边界执行。准备阶段与正式采样分开，避免把冷启动混进等待帧；渐进准备和跨进程 shader 缓存由 Renderer/RHI 后续演进。准备进程的 CPU 时长或工作集不能作为 GPU 帧时。

准备耗时应同时记录构建配置、显卡/驱动、是否启用 GPU validation 及管线是否已准备。Control 端到端验收开启了 --gpu-validation，其耗时不能直接代表普通编辑器启动体验，也不能与未开启验证的样本直接比较。当前协议未返回显卡及验证模式，调用方需随测量记录这些条件；单次工作流观察不等同于性能基准。

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
- .depth.f32：紧密排列的小端 float32，左上角原点、Reversed-Z near=1、far/background=0。现有 Renderer 提供不带 TAA 抖动的编辑器深度，使用 metadata 的 viewport 定位场景区。
- 每次采样 JSON：接受帧号、全局计数、case 内序号、viewport、相机矩阵、有效设置、Sky radiance、等待/跳过帧和 GPU 统计。`frame_stats` 提供当前帧的 `history_resets`、`light_sources`、`shadow_sources`、`di_candidates`；后两者是启用 Shadows 时具备投影资格的源数和初始候选预算，不是实际射线数。`case_reported_history_resets` 累计本 case 所有已报告帧的重置次数。旧阴影槽统计不再属于当前输出。
- manifest.json：文件大小及 SHA-256，最后发布的完整产物标记。

GPU 统计带独立采样帧号和有效标志，可能落后于截图。`gpu_sample.ddgi_pass_ms` 的两项分别为探针更新/传输与接收点 gather 耗时；`ddgi_visibility_rays` 的两项分别为探针命中着色与接收点 gather 的可见性查询数。它们共用 `valid`、`sequence`、`frame_id`，分别是 `pass_ms[3]` 和 `shadow_rays` 的细分，不能再次相加。相机矩阵与深度使用不抖动的编辑器投影；TAA/SMAA T2x 内部栅格投影有抖动，本接口不提供该内部矩阵。捕获会等待 GPU，不是常规帧性能测量。Renderer 按自然变化维护历史；接口不承诺完整清空 DDGI 探针、逐位确定性或恢复原 GPU 历史。

接受帧与捕获元数据的 `lighting_focus` 是实际传入 Renderer 的观察目标。`frame_stats.probe_volumes` 按固定槽位返回启用/滚动状态、世界边界、间距、数量、resident、updated 和 reset_probes；`probe_volumes_source: cpu_layout_and_schedule` 明确其来源。resident 是分配的槽位数，updated 是本帧计划更新数，均不能当成 GPU 完成数或有效探针数。

### DDGI 单接收点归因

`view.set` / `render.compare` 共用视口选项 `probe_history: live | hold_irradiance | hold_distance`、`probe_inspect: bool`、`probe_inspect_x/y: 0..65535`。坐标是窗口物理像素；不改变 World 配置和 Undo。归因结束恢复 `live` 并关闭 inspection。

`last_frame.frame_stats.probe_inspection` 为可空的异步完成样本，提供自己的 `sequence/frame_id/scene_identity/pixel/history/rays/updates/volumes`。`receiver` 返回实际 gather 状态、覆盖、权重和及贡献支配度；每个 probe 返回支持阶段、年龄、呈现/运输 RGB 与方向观测数、空间/朝向/距离可见性/最终权重和位置。`transport_observations` 对应权威运输场，`irradiance_observations` 对应显示场并镜像运输成熟度；不能把后者当成额外独立样本。空场、停用 DDGI 或关闭 inspection 返回 null，背景/越界产生 `position_valid.w=0` 且无邻居的样本。完成帧可能滞后或缺样，调用方按 sequence 去重并使用样本自己的布局，不能拼上当前截图冒充同步数据。

`response_epoch_min_max_visits` 是请求代次、整个方向 tile 的最少/最多有效响应观测（0～12）及有效访问游标。`traced_probe` 按前一完成样本中最大的有效邻居权重选择，当前帧未更新它时 `ray_samples` 为空；这些记录只含实际可着色命中，不包含天空和未着色命中。每条提供 source/kind/ray ordinal、局部 diffuse/specular 和方向光 diffuse/specular 亮度、命中位置/roughness、RIS weight/源 PMF/反馈亮度/主射线距离。需要整批均值时除以 `rays`，不能除以有效记录数。诊断专用 Update 入口和读回按需创建，正常着色编译掉记录代码；仍共用完成序列、32 槽无等待发布与 Renderer 资源退休。

### 性能观测

`editor.inspect` 的 `performance` 包含状态、最多 120 个完成样本、发布时间间隔、样本 sequence / frame_id、场景与域首帧、RT/视口尺寸、直接光/AA/DDGI 配置，以及 17 行层级统计（八个主阶段、八个子阶段、一个合计）。`current_ms`、`mean_ms`、`p95_ms`、`share_percent` 在没有对应观测时为 null；未启用项的当前 `state` 为 `skipped`。`active_samples` 表示窗口内计时 scope 启用的样本数，不是 GPU 命令数。表格和接口消费同一约 4 Hz 的发布快照，不因查询增加计时样本或读取 GPU。

`logical_resource_bytes` 是当前 CPU 所知的 Scene Lit 逻辑资源分配，和异步计时样本不是同一时刻的物理显存统计。`total_semantics` 明确只合计八个主区间；子项通过 `parent` 关联，`included_in_total` 防止重复累加。生命周期与统计范围以[性能表](README.md#gpu-pass-耗时)为准。

原始 `last_frame.gpu_sample` 继续保留八项 `pass_ms` 与两项 `ddgi_pass_ms`，新增 `direct_pass_ms` 四项、`ddgi_update_pass_ms` 两项及 `pass_mask`。mask 位 0～7 为主阶段、8～9 为 DDGI update/gather、10～13 为 direct 子阶段、14～15 为 DDGI update 子阶段；这些数组和 mask 共用完成 sequence。`resource_bytes_source: current_cpu_logical` 说明旧 `resource_bytes` 字段的来源，不能把它当作采样帧物理驻留量。

## 复现工程

repro.export 读取 Session::serialize() 的内存场景，保留原路径、dirty 和 Undo；scene.save 才是普通保存。复制 Config、Content、Source，排除 Cache；没有工程时创建独立工程容器。副本 startup scene 指向当前内存场景。

保存相机、显示偏好、Entity 选择顺序和检查的 Component、构建身份及资源指纹。publication_version 标识完整已发布构建，source_version 标识游戏源码集，ui_source_version 标识 UI 输入。副本 Source 只包含该成功构建的游戏和 UI 源码；磁盘上新增或修改但未发布的源码移入 unpublished/，内存 Code 草稿另存 drafts.json。没有可用场景或成功发布构建的工程返回 not_ready。这是场景与代码复现，不保存整个桌面、编辑器布局或 GPU 历史。

复制期间复核文件集合和内容。在副本中重新加载资源，与内存 Mesh 的顶点、边、分段、材质和图像指纹比较；不一致返回 dependency_mismatch。不跟随链接，拒绝无法持久化的程序化资源。最多 10000 输入文件 / 1 GiB，单文件 256 MiB；manifest 使用现有 16 MiB、32 层及重复字段拒绝规则。文件操作共用支持 Windows 长路径的原子写入设施。输出不能位于源工程的 Config、Content 或 Source 内，包含大小写别名和链接别名；创建产物前返回 invalid_destination，避免复制自身。控制目录可以放在 Cache 内。

Windows 长路径转换覆盖产物复制、归档、文件时间读取、目录身份比较、实例发现与清理以及 CLI 请求文件。路径拼接和 canonical 解析后，在文件操作边界使用现有 windows_file_path 统一转换；协议中的绝对路径、工程相对路径和资源身份保持原格式。深目录副本可以重开、保存并再次导出。

repro.open 校验完整文件集合、大小及哈希，拒绝缺失、修改、额外文件或链接；运行时生成的 Project/Cache 不参与复现依赖。随后通过同一工程入口的严格加载策略编译源码并读取场景，全部成功才替换当前文档并恢复视图与选择。编译或反序列化失败保留当前工程和对象；常规 UI 的修复工程模式保持原有行为。有未保存场景或源码时拒绝替换。失败/取消可能留下诊断目录，未返回成功的任务不应当作成功交付。

含 v1–v5 场景的旧包通过同一场景迁移器解析选择与检查目标，再按文档 ID 查找新对象。空间组件展开成子 Entity 后，旧序号仍对应原来的对象；不会用展开后的 World 成员下标猜测身份。旧纯空间组件的检查目标映射到相应 Entity，能力组件则保留其真实 owner。正式 Control 请求继续使用 Entity 命名。

新产物的 manifest 标记 `view_schema: 2`；无标记或显式版本 1 视为旧视图快照，未知版本拒绝。`repro.open` 对旧 `shadow_light_budget`（整数 0..16）和 `shadow_samples`（整数 1..64）先严格验证，再迁移到默认 `direct_candidates: 8`；旧值 64 不开启 `direct_reference`。新旧候选字段混用、版本 2 中的退休字段及非法旧值均拒绝。嵌入场景版本 4 的 World 设置在一致性预校验中应用同一迁移，正式加载仍由场景版本迁移负责；验证失败保留当前文档。此兼容仅用于复现包读取，`view.set` 不接受退休字段。

## 生命周期和扩展

外部执行链为：CLI / 自动化 / 验收 / LLM → 本机传输 → EditorControl 主线程调度 → Application / Session / Camera / ViewOption → 既有 Runtime、UI 和 Renderer。UI 与快捷键直接调用相同业务层。

传输线程只解析、排队和收发消息，引擎操作在主线程执行。当前每个连接串行处理请求，上一条请求得到响应后才读取下一条；异步任务先返回 `pending`，随后查询结果。

- src/control/：传输、身份、JSON 边界、发现和 CLI，不依赖 Runtime/UI/GPU。
- src/application/editor_control.cpp：能力目录、主线程调度、token、句柄、重试和实验状态。Application 在轮询及接受帧边界调用。
- src/editor/view_options.*：菜单与指令的显示参数合同。Camera/Session 保留各自业务不变量。
- src/application/control_artifacts.*：拥有快照的文件及图像工作，只持有数据和不可变 MeshRef，不跨线程借用 Runtime。

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

    cmake --build --preset dev --target editor_control_acceptance
    cmake --build --preset release --target editor_control_acceptance

长路径专项使用独立真实编辑器验证短控制目录与超 MAX_PATH 的控制目录、长文件名场景、源码归档、连续导出/重开/保存、CLI 请求文件及实例发现文件清理：

    cmake --build --preset dev --target editor_control_path_acceptance
    cmake --build --preset release --target editor_control_path_acceptance

独立工程和真实编辑器进程通过命名管道验收：发现与 CLI、创建/选择/组件/属性/TRS、Undo/Redo、相机、显示约束、重复请求、请求/任务淘汰、过期 token、未保存场景导出/重开、游戏和 UI 发布快照、额外文件拒绝、重开失败保留原文档、嵌套输出拒绝、多接受帧捕获、Scene Lit、失败与无效修改保留任务、取消、上下文变化、超时、断线和关闭。需要可绘制的独立窗口，不要求键盘焦点或人工输入；报告和图像保存在构建目录 control-acceptance/。

DDGI 接线专项沿同一驱动运行：

    ./out/dev/atelier_editor_control_acceptance.exe ./out/dev/atelier_editor.exe ./out/ddgi-control-dev --ddgi-debug
    ./out/release/atelier_editor_control_acceptance.exe ./out/release/atelier_editor.exe ./out/ddgi-control-release --ddgi-debug

覆盖四体积共享 schema、summary 观察、原子拒绝和幂等、覆盖预设、相机目标滚动的真实捕获、世界坐标精度、完整复现及真正旧格式恢复。World 页的输入预览/提交/取消、关闭和槽位切换在既有 `--acceptance-surfaces` 窗口链中验收；`atelier_editor_document_acceptance --scene-lit` 验证正式 DSL 页面与文档交互边界。

### World 配置与字段显示元数据

`display_options` 每项包含 `scope: scene | view`。`view.set` 保持单一批量入口：完整验证后，scene 部分经 Session 记一条历史，view 部分保存为当前视口偏好；重复候选不改 revision。`direct_candidates` 是 scene 项，随 Scene v6 保存；`direct_reference` 与探针显示是 view 项，对应 World Settings 的 Diagnostics。`render.compare` 的覆盖仅作用于临时帧，不写 World 或 Undo。`scene.undo/redo` 恢复 World 后，下次观察与渲染重新组合有效视图。`scene.set_property` 仍使用字段名，inspection 同时返回 `label` 和 `category`，显示文本不参与字段身份。
