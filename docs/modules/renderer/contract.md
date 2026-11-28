# 编辑器与运行时 Renderer 当前契约

本页维护当前执行、几何与材质分桶、显示和资源合同；光照算法与默认值统一见 [Scene Lit](scene-lit.md)，可编程材质与压缩纹理见 [材质系统](materials.md)，已验证的配置和证据见 [验收记录](acceptance.md)。

入口：[renderer.hpp](../../../modules/render/renderer/include/renderer/renderer.hpp)，目标 `atelier_renderer`，由 [cmake/renderer.cmake](../../../cmake/renderer.cmake) 的 `atelier_add_renderer()` 创建，构建于 `atelier_rhi`、`atelier_rdg` 与 `atelier_ui_protocol` 之上；可执行目标调用 `atelier_deploy_rhi_runtime(target)` 部署同一套 DXC、Agility SDK 与 PIX DLL。着色器在配置阶段嵌入库，不依赖启动工作目录。

## 一条执行链与单设备多表面宿主

应用持有 `HWND`、Runtime 对象与编辑状态 → 生成纯值 CPU 场景快照与 UI 帧 → `Renderer::render` → 在线性 Arena 上构建 `rdg::Graph` 并经共享 `rhi::Device` 与窗口专属 `rhi::WindowSurface` 提交呈现 → 应用仅在 `FrameStatus::accepted` 时替换命中快照。

- **单设备多表面与纯 UI 表面通道**：`Renderer::open(std::shared_ptr<rhi::Device>, NativeWindow, Options)` 允许主编辑器窗口与独立原生弹出菜单窗口（开启 `Options::ui_only = true`，跳过全部 3D Mesh/Lit 管线创建并通过弱引用 `SharedUiTextureHub` 共享字体/图标纹理池）共享同一个 `rhi::Device`、全局 Bindless 堆与后台 DXC 编译线程池，各窗口仅持有轻量 `WindowSurface`；兼容重载 `Renderer::open(NativeWindow, Options)` 为单窗口消费者自动创建设备。Edit 与独立 `PlayProcess`（经 `PlayLink` 隔离运行）使用同一套引擎渲染内核。
- `Renderer` 不保留业务对象身份、GC 根或 JIT 回调；同一 owner 线程完成打开、查询、绘制和关闭，窗口必须存活到 `Renderer::close()` 之后。
- `Options::frame_latency_waitable` 选择 [RHI 呈现准入](../rhi/contract.md#呈现准入)，默认关闭。宿主通过 `wait_frame` 在安全外层有限等待；`render` 在目标可绘制且尺寸匹配后零超时确认许可，未就绪返回 `FrameStatus::not_ready`，不更新 `Capture` 或接受身份。

### 合作停机

`Frame::stop_requested` 借用宿主的只读常数级停止谓词，仅在本次 `render` 内于 owner 线程查询。在准备入口、几何网格池构建、持久场景增量同步及 RDG 提交前检查停止；观察到停止后返回独立的 `FrameStatus::cancelled`（不提交/Present 半帧、不替换 `Capture`、记录一条 `renderer.frame_cancelled` info 日志），之后只允许释放 `MaterialBatch` 并调用 `close` / 析构。

## GPU-Driven 几何池、持久 `RenderScene` 与 VisBuffer 管线

- **五流 `GeometryPool`、按材质槽聚合 Meshlet LOD 与 `.amsh` 几何缓存**：每份不可变 `geometry::Mesh` 首次进入渲染器时按材质槽聚合三角形并锁定跨材质接缝，在分桶紧凑局部顶点表上并行运行 `meshoptimizer` 构建最多 4 级离散 LOD（每簇至多 64 顶点 / 124 三角形，32B `GpuMeshlet` 内紧致打包包围球、屏幕误差界与零量化误差保守背面法线锥 `SNORM5×2 + UNORM6`），同时在 `renderer-geometry-cache` 下以内容寻址 `.amsh` 二进制缓存持久化并做边界校验；就绪数据上传至五条紧凑 GPU 流（`Positions` 12B、`Attributes` 16B 八面体法线/切线/UV、`Meshlets`、`MeshletVertices`、`MeshletTriangles`）及供光追与材质解包共用的统一 `BLASIndexStream`、`FaceMeta`（闭合流形标记、绕序翻转、材质槽与原始多边形边掩码）与 `PartTriBaseBuffer`。
- **多上下文持久 `RenderScene`**：按 `Frame::scene_identity` 维护独立持久场景槽位（支持主视口与 `.procgraph` SOP 预览上下文切换保留各自 TLAS）。实例按非零 `stable_key` 追踪世界变换与 `prev_world` 运动矩阵，选中状态解耦至紧凑位图 `SelectedBitmaskBuffer`。
- **两阶段 HZB `VisBuffer` → 按原型分桶着色**：
  1. `VisCull CS` 在 GPU 上执行实例视锥剔除、Meshlet 离散 LOD 选择、仿射变换不变的单面材质背面法线锥剔除（透视与正交投影通用，自动豁免双面材质、工作室 `backface_tint` 与 `Lines: All` 透视线框）及两阶段 HZB 遮挡剔除（Phase 1 测 `PrevHZB` 并光栅可见簇 → 构建 `CurrHZB` → Phase 2 对被 `PrevHZB` 遮挡的候选簇重测 `CurrHZB` 并增量光栅不遮挡簇 → 重建完整 `LitHzbBuildPhase2`），生成紧凑可见簇队列与 `CmdDrawMeshTasksIndirect` 间接参数。
  2. 光栅阶段仅保留 **1 条全局 Opaque Mesh+PS PSO**（配极简 `[earlydepthstencil]` 直通 Pixel Shader 输出 `R32_UINT` `VisBuffer`：`cluster_idx << 8 | local_tri << 1 | front_face`）与每个活动 Masked `.source` 原型 **1 条 Mesh+PS PSO**（仅执行轻量 `hlsl_coverage` alpha clip），配合 Reversed-Z `D32_SFLOAT` 深度。
  3. 全屏 `VisUnpackAndClassify CS` 解析 `VisBuffer`，精确求解透视校正重心坐标及其对屏幕像素的解析偏导数 `(db_dx, db_dy)`（隐函数微分，跨近平面与极端透视保持精确）、表面与天空运动向量（`RG16_SFLOAT`）、预期前帧深度（`R32_SFLOAT`）及八面体几何法线（`R32_UINT`），并将可见像素按材质所属 `.source` 原型（Archetype）前缀和分桶。
  4. `MaterialShade Bin CS` 按原型 `CmdDispatchIndirect` 派发，执行 `hlsl_footprint`（含 Kaplanyan 2016 几何法线曲率高光抗锯齿与 BC5 法线贴图 Toksvig 方差补偿），在工作室模式下直接输出工作室颜色与可选 Cavity 视空间法线，在 Scene Lit 模式下打包写入 OpenPBR v1.1 `3×8B` Canonical Slab（`Slab0_SpecNormal` `RG32_UINT`、`Slab1_AlbedoClosure` `RG32_UINT` 及由 `TileEmissiveMask` 8×8 瓦片门控的 `Slab2_Emissive` `RGBA16_SFLOAT`）。修改单份 `.source` 仅重编该原型的着色桶管线，不再触发全管线重编风暴。

## 场景和 UI

- 相机采用左手系、`+Y` 向上、Reversed-Z（近端为 `1`，背景清除值为 `0`），矩阵为 row-major 行向量约定 `p * world * view * projection`。`Camera::far_depth` 在 `[0, 1]` 内通过 `SV_ClipDistance0` 精确裁剪跨远平面三角形，不整体丢弃网格或重归一化设备深度。非法（非有限、非仿射或奇异）`world` 矩阵在编码前拒绝整帧并返回 `ErrorCode::invalid_argument`（`terminal = false`）。
- 场景逻辑视口与 UI 坐标均为左上原点物理像素；`RasterViewport` 统一导出整数外包硬件视口、像素中心 scissor 与齐次裁剪补偿。
- Scene Lit 非材质核心计算管线在 `Renderer` 后台异步准备，`preparation(view)` 非阻塞推进并汇报 `done/total`；未就绪时 `render` 返回 `FrameStatus::not_ready`，工作室模式不受影响。
- UI 绘制支持独立圆角、向内边框、半开像素裁剪、程序化节点形状/扇环、R8 字形覆盖图与预乘线性 RGBA 图像；同一层按提交顺序单次实例化绘制，纹理通过全局 Bindless 描述符索引直接采样。

## 视口显示策略

- **Shading**：`Smooth`、`Flat`（定向几何面法线）、`MatCap`（256×256 sRGB 陶土球图，遵循材质双面与 Mask）与 `Scene Lit` 四种模式相互正交于 Edit/Play 运行态。
- **Edges & Topology**：支持 `Off`（仅保留至少 2 px 宽的橙色选中边）、`Overlay`、`Lines: Visible` 与 `Lines: All`（Mesh Shader 线框通道 `wireframe_mesh.hlsl` 读取同一可见簇与 `SV_Barycentrics / clip_w` 屏幕仿射边距离），以及尊重作者边掩码的 `Polygons` 与全边 `Triangles` 切换。

### 背景与参考辅助显示

工作室模式支持可选蓝灰渐变背景；Scene Lit 在 `sky_mode == physical_atmosphere` 或 `hdri_sky_map` 时显示天空背景，否则为纯黑。XZ 参考网格与原点轴线在齐次空间直接求交并带斜率深度偏置，共面实体优先且不污染场景 `D32_SFLOAT` 深度。

### 运行状态与着色模式的独立性

Edit 与 Play 只决定模拟是否推进；两者消费相同的 `Frame` 数据结构、材质批次与灯光快照。

## 工作室 Cavity

`ViewSettings::cavity` 默认开启（仅作用于 `MatCap / Smooth / Flat`，Scene Lit 与纯线框跳过）：读取工作室分桶输出的视空间作者法线与 `D32_SFLOAT` 深度，计算有符号屏幕曲率增强凹凸边界，随后叠加参考网格、选中/线框边与 UI。

## 帧接受与失败

- 隐藏、最小化、零客户区尺寸或 `frame.ui_frame.extent` 与实际交换链尺寸不符：编码前返回 `FrameStatus::skipped`，`Capture` 不变。
- 所需 `MaterialBatch`、呈现准入或 Scene Lit 管线尚未就绪：编码前返回 `FrameStatus::not_ready`。
- RDG 执行与 `present` 均成功且未返回 `DXGI_STATUS_OCCLUDED`：返回 `FrameStatus::accepted`，发布 `accepted_id`、`accepted_extent` 与可选 `Capture`。若 `IDXGISwapChain4::Present` 返回 `DXGI_STATUS_OCCLUDED`（如显示器休眠或运行于非活动输入桌面），按契约返回 `FrameStatus::skipped`。
- GPU 资源创建、上传或命令执行阶段发生的错误标记 `Error::terminal = true`，此后仅允许释放批次并 `close`。

## 诊断捕获

### 每帧计数

`FrameResult::stats` 返回当帧活动统计（`ui_draw_calls`、`overlay_draw_calls`、`scene_draw_calls`、`viewport_draw_calls`、`scene_instances`、`material_upload_count`、`cached_material_images`）以及 Scene Lit 最新已完成帧的时间轴并集耗时 `scene_lit_gpu_union_ms`、非阻塞 GPU 阶段耗时、掩码 `scene_lit_gpu_pass_mask`、VSM 页池缓存统计、分簇统计与硬件光追射线计数；CPU 墙钟分段写入 `timing::Recorder` 的 `render.*` 作用域。支持通过 `Frame::trace_scope` 采集完整 RDG 图或 `"scene_lit_post"` 子范围 `rdg::TraceSnapshot`，并通过 `Renderer::release_transients()` 在宿主休眠时释放瞬态池。

### 显式图像读回

`render(frame, &capture)` 在同一 RDG 图末尾追加 `CaptureReadback` 通道，同步等待完成后将 256 字节行对齐读回缓冲解包为紧密 sRGB `RGBA8`、Reversed-Z `float32` 深度，以及（Scene Lit 下）来自 `VisUnpack` 的两个 `float32` 表面运动向量。当开启时间抖动抗锯齿（`Atelier TAAU`）且存在参考网格、编辑边线或显式 `Capture` 时，自动运行零几何重绘的 `3×3` 邻域精确射线—三角形求交 `UnjitterDepth CS` 恢复公开无抖动相机深度。空场景不分配深度资源，`capture.depth` 直接填 `0`。
