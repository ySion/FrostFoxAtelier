# 编辑器 renderer 当前契约

入口：[renderer.hpp](../../../src/renderer/renderer.hpp)，目标 `atelier_renderer`，由 [cmake/renderer.cmake](../../../cmake/renderer.cmake) 的 `atelier_add_renderer()` 创建。调用前由应用根构建创建 `atelier_rhi` 和 `atelier_ui_protocol`；可执行目标调用 `atelier_deploy_rhi_runtime(target)` 部署同一套 DXC、Agility 与 PIX DLL。shader 在配置时嵌入库，不依赖启动目录或另一 worktree。

## 一条执行链

应用持有 HWND、Runtime 对象与编辑状态 → CPU 场景数组和 UI 完整帧 → `Renderer::render` → 同一 RHI Context/gfx 录制与 Present → 应用只在 accepted 时替换命中帧。

renderer 不保留业务对象身份、GC 根或 JIT 回调。UI 布局、文本排版、输入处理和撤销归各自拥有者；renderer 消费 [公共 CPU 协议](../../../src/ui/frame.hpp)。同一 owner 线程完成打开、查询、绘制和关闭，窗口必须存活到 renderer 关闭之后。

## 场景和 UI

- `MeshInstance` 提供完整 local-to-world 矩阵、线性基础色、选中标记与共享不可变 Geometry。`geometry::Mesh` 是经构造验证的连续三角形顶点资源，`geometry::cube_mesh()` 只是一个内置资源。相邻且引用同一资源的实例合为一次 instanced draw，不同资源分批。GPU 缓存保有对应 CPU 资源，未使用项沿 gfx 机制退休；没有另一套业务对象身份。当前为基础光照与漫反射贴图、Alpha Test，不含阴影、DDGI、拾取或着色器图。
- 场景管线和深度纹理在首次绘制非空场景且 viewport 非零时创建；后续按原生命周期复用，深度尺寸在实际场景绘制时跟随目标更新。纯 UI 窗口仍使用同一 Renderer/Context，无场景管线创建、深度分配或清除。UI 管线在 `open` 时准备，应用可提前创建并保留隐藏的弹出窗口以移出首次点击的初始化成本。
- 相机为左手系，+Y 向上，D3D 深度 `[0,1]`。矩阵为 row-major，行向量按 `p * world * view * projection` 执行，匹配 LookAtLH/PerspectiveFovLH。renderer 不维护相机控制状态。
- World/应用负责矩阵求值，renderer 只消费最终数学值；不依赖 World/Runtime，不重做 TRS 分解。World 已提供空间树，renderer 只接收其最终仿射矩阵。仿射矩阵平移位于索引 12/13/14，索引 3/7/11 为 0、15 为 1。非有限、非仿射或奇异矩阵使整帧在编码前返回 invalid_argument、terminal=false；保留旧 capture 和执行域，不用单位矩阵兜底，也不静默略过对象。
- 法线按 world 的 3×3 逆转置计算。CPU 用 double 计算余因子，整个矩阵共用一个缩放因子，保留任意法线所需的行间比例；pixel shader 先按最大分量缩放再规范化。镜像输入保留朝外法线，沿用无背面剔除。CPU 同时检查实际上传的 FP32 矩阵，极小数清零后失去秩的输入在编码前拒绝。统一极小/极大缩放继续支持；这不承诺任意病态非均匀矩阵的数值精度。World 局部 scale 仍限定正、非零、有限。
- 场景 viewport 与 UI 坐标均为窗口物理像素，左上原点；DPI 已由 UI 应用。先画场景，再按 UI 数组顺序绘制；应用/UI 应给场景 viewport 留出可见区域。
- UI 支持半开 scissor、实色矩形、R8 覆盖图与 UV。输入为线性 straight RGBA，pixel shader 乘 coverage 与 alpha 后输出预乘颜色，混合使用 ONE/INV_SRC_ALPHA。
- renderer 显式请求 SDR RGBA8 backbuffer，并校验实际 RTV 格式为 `R8G8B8A8_UNORM_SRGB`；sRGB RTV 执行写入编码与线性混合。RHI 未指定该选项的旧调用仍采用 gfx 的显示器自动选择，不能推导所有 Context 都保证 RGBA8。
- Coverage 的非零 identity 与 revision 一起标识不可变内容，renderer 缓存同时保有 `shared_ptr<const Coverage>`。CPU 行可有 padding；上传时收紧行。每个实际使用的版本只上传一次，无用缓存释放 CPU owner，GPU 资源沿 gfx 原有机制退休。当前没有字体图集、显存预算或资源管理器。

## 视口显示策略

应用的 Workspace 持有视口设置，经同一 `Frame::view` 提交。Scene View 路径栏末尾的显示图标菜单包含 Shading、Edges、Topology 三组互斥选项，Guides 图标菜单提供独立勾选；两者使用既有 CPU UI、独立菜单窗口和 accepted 输入链。模式切换不修改 World 材质、几何资源、对象身份或文档撤销历史。正式编辑器默认 MatCap；直接调用 Renderer 的默认设置仍为 Smooth。

- **Shading**：Smooth 使用已有顶点法线、实例基础色与 Mesh 漫反射材质贴图；Flat 使用几何面法线，并依据输入法线的总体方向确定正反，保留当前双面绘制。MatCap 按视空间法线采样一个 256×256 sRGB 陶土球图，覆盖基础色；球图在第一次实际场景绘制时生成并上传一次，不消费场景灯光。相机 view 沿既有刚体 LookAt 约定。MatCap 自身不产生物体间阴影、接触遮蔽或真实环境反射。
- **Edges**：Off 只保留选中边高亮；Overlay 将边线直接合成到表面着色中，沿原几何批次一次绘制；Lines: Visible 先绘制整个场景的深度，再绘制通过深度测试的边；Lines: All 使用相同深度结果，显示包括被遮挡部分在内的边。两种纯线框均保留完整场景深度供诊断读取，未覆盖的颜色保留背景。
- **Topology**：Polygons 尊重生产者提交的边标记；Triangles 显示全部三角形边。每个三角形的三位 mask 中，bit i 表示顶点 i 对边。没有 mask 的既有资源按全部三角形边处理；不通过位置焊接或共面关系猜测原始拓扑。内置 cube、plane、sphere 提供边标记，过滤三角化对角线。OBJ 导入器对简单多边形三角化并保留边界；已三角化源文件无法恢复丢失的原始边。
- 线宽以物理像素计，默认 1.25，接受范围 (0,8]；叠线不透明度默认 0.7，范围 [0,1]。线框用无透视插值重心坐标的屏幕导数估算像素距离，并平滑覆盖边界。选中边至少 2 像素，显示橙色；表面不再整体染黄。仅线模式的可见线为不透明浅灰（抗锯齿边缘除外）。

全部资源仍在当前 Context 的借用 gfx 合同内创建、绑定和录制。深度预通道使用 gfx 的 ZERO/ONE 混合保持颜色，线通道使用 LESS_EQUAL 或 ALWAYS 比较且关闭深度写入。没有新建原生设备、队列、提交或等待入口。pipeline 和 MatCap 随 Renderer 保有、关闭时沿原机制退休；纯 UI 窗口继续不创建场景资源。

### 背景与参考辅助显示

`ViewSettings` 的 `background`、`reference_grid`、`origin_axes` 与着色/线框模式独立。正式编辑器默认开启蓝灰渐变、XZ 平面 Y=0 参考网格及 X/Z 原点轴线；直接 Renderer 调用默认关闭。Scene View 的 Guides 菜单沿同一显示设置入口切换，既不创建 Actor，也不改变几何、材质、场景序列化、选择或撤销历史。

背景只绘制 scene viewport；网格与轴线通过程序化全屏三角形生成，没有网格顶点缓冲或贴图。网格基准为 0.2 米细格、每五格主线，根据投影像素密度在十倍尺度之间交叉渐变，并在接近地平线和远处淡出。颜色输入为线性值，由既有 sRGB RTV 编码；线宽以物理像素计算。

有物体时参考网格读取同一场景深度、关闭深度写入。实体模式为背景→实体→网格→UI；纯线模式为背景→完整物体深度→网格→Visible/All 边线→UI。共面物体优先，All 只改变物体边线显示，不令网格穿透表面。空 World 仍绘制背景与网格，但不为辅助显示创建深度、MatCap 或实例缓冲，capture 深度保持全 1；零尺寸 viewport 和纯 UI 窗口不绘制这些辅助内容。

CPU 在编码前复用数学模块计算一次 inverse view-projection。有限但不可逆或无法表达为有限 FP32 逆矩阵的相机，仅跳过参考网格；背景与既有场景路径继续，保持旧相机输入合同。相机落在平面、交点位于近裁面后方或接近水平时，网格稳定裁弃/衰减。没有另一个相机控制状态。

### 运行状态与着色模式的独立性

Edit/Play 决定场景是否执行及如何更新；视口显示决定同一场景快照如何呈现。两者不互相选择：Play 可以使用 MatCap 等工作室显示，编辑状态也可以使用未来的场景光照模式（Scene Lit）。场景光照将消费场景真实光源与材质；不以是否 Play 作为渲染分支，也不把 MatCap 定义为只能用于编辑的模式。

背景与参考辅助显示处于独立的视口策略中，可用于上述任意着色模式；场景天空/环境属于场景光照的画面内容，视口渐变不能在其后覆盖场景环境。本轮只实现背景与参考网格，没有加入未实现的 Scene Lit 枚举、Play 状态或空渲染管线。

每份共享 Mesh 首次使用时创建一个 GPU 三角形缓冲，包含顶点、边标记和面法线；切换显示方式复用同一份几何缓存。每个场景批次的每个材质分组在实体/叠线模式为一次 draw，在纯线模式为深度与边线两次 draw；两者都计入唯一的 `FrameResult::stats.scene_draw_calls`，不代表 GPU 计时。MatCap 属于场景资源，不混入 UI image/coverage 统计。

## 帧接受与失败

`Frame::ui_frame` 是帧 id、extent、DPI 和 surface revision 的唯一来源；所有输入借用到同步 `render` 返回。renderer 的 `target()` 报告实际 backbuffer 尺寸，窗口 client 尺寸可能在 resize 时领先一帧。

- 隐藏、最小化、零 client 尺寸或输入 extent 与实际 target 不符：编码前返回 skipped，capture 不变。空 scissor 不会落入 gfx 的“零尺寸代表整个目标”惯例。
- 完整 `Context::frame` 成功且 Present 未返回 `DXGI_STATUS_OCCLUDED`：accepted。`accepted_extent` 是编码前捕获的实际尺寸；gfx 在旧目标 Present 后 resize，`next_extent` 只适用于下一帧。accepted 不承诺已经完成扫描显示。
- CPU 校验和编码前 CPU 分配失败：`Error::terminal=false`，可重新准备帧。开始 GPU 创建、上传或录制后失败：terminal，后续只可关闭，不宣称回滚或继续使用上一 GPU 帧。
- Present 之后的 resize、等待、Reset 等失败沿 RHI 终态返回，不会提前发布 accepted。遮挡是 skipped，GPU 可能已经提交；它不是 GPU 回滚。
- 应用只根据 accepted 替换完整命中数据，并负责在窗口大小/DPI 与命中帧不匹配时暂停命中、取消 press/capture；renderer 不建立第二份输入状态。

`close` 退休 GPU 资源、释放 Pipeline、通过原 Context 排空和关闭。重复关闭成功，关闭失败保留清理入口；无法安全清理的最终析构 fail-fast。

## 诊断捕获

### 每帧计数

`FrameResult::stats` 是该次 `render` 返回的 `FrameStats` 值快照，各项均为 `uint64_t`。它不持有额外资源，也不增加 GPU 查询、等待或动态分配。

| 字段 | 统计边界 |
| --- | --- |
| `ui_draw_calls` | 实际成功编码的 UI draw 次数；空 clip、零尺寸和透明项不计入 |
| `scene_draw_calls` | 实际成功编码的场景 draw 次数；相邻同网格资源实例共享批次。实体/叠线每批的每个材质组一次，纯线框深度与边线各一次，空场景为 0 |
| `material_upload_count` | 本帧新场景图片上传次数，包括首次使用的白图；复用版本不上传，不含 UI 图片 |
| `cached_material_images` | 当前场景 Image/颜色空间缓存项数，不含 UI 图片或 gfx 等待退休的资源 |
| `viewport_draw_calls` | 实际成功编码的视口辅助 draw 次数；渐变一次，网格与轴线共用一次；不计入物体或 UI draw，不代表 GPU 时间 |
| `coverage_upload_count` | 本次新建内容版本后成功编码的 R8 上传次数；命中已有版本为 0 |
| `coverage_upload_bytes` | 上述上传的紧密 R8 有效字节数；不含 CPU 行 padding、D3D 复制对齐与 scratch，也不含网格实例上传 |
| `cached_coverage_count` | 返回时 renderer 缓存持有的 Coverage/GPU texture 项数 |
| `cached_coverage_texel_bytes` | 缓存项宽×高的逻辑 R8 字节总量，不是物理显存占用 |
| `cached_coverage_cpu_bytes` | 缓存持有的 Coverage 的 `pixels.size()` 总量，包含 CPU 行 padding；不是 vector capacity 或整个 UI 的 CPU 内存 |

编码前 skipped 的活动计数为 0，缓存计数反映仍被持有的资源。原生遮挡发生在编码之后，skipped 仍报告已编码活动。unexpected 失败不返回完整统计；原有接受状态与终态规则不变。缓存量不包含已交给 gfx 等待 GPU 退休的资源，也不包含几何、深度缓冲与上传暂存。每帧计数重置，缓存总量随现有插入/退休动作更新，未改变淘汰策略。

应用可在正式交互中测量 prepare/render 的 CPU 墙钟时间；render 时间包含 Present 或必要等待，不能称为 GPU 时长。使用同一 `FrameResult` 关联活动计数，显式 Capture 另行记录，避免把它的同步成本混入普通交互样本。

### 显式图像读回

`render(frame, &capture)` 显式请求 GPU 同步，返回本帧目标的紧密 sRGB RGBA8 和 float32 深度，行自上向下。仅在 accepted 时替换输出。UI 不写场景深度，背景深度为 1。普通 render 不调用 finish；诊断捕获不应放入交互帧热路径。

本帧没有场景绘制时，capture 的深度数组直接填充为 1，不创建或读回 GPU 深度资源，也不返回之前场景残留的深度；颜色仍从实际 backbuffer 读回。场景首次创建失败沿既有 GPU 阶段终态规则处理。

捕获命令经现有 gfx 录制，使用 256 字节对齐的 GPU 行距，完成后拆成紧密 CPU 行。读回源是 resize 前真正绘制的目标。没有原生队列逃生口，也不改变原有紧密 texture copy 入口。

## 场景材质与贴图

共享不可变 Mesh 同时拥有 UV、Material、连续的 MeshPart 和共享 RGBA8 Image。分组覆盖全部三角形且不重叠；无材质的既有网格使用白色默认材质，原实例基础色与 MatCap 行为保持。World 的 Material 基础色仍是实例 tint，场景文件继续只保存网格 UUID 和 tint，不保存 GPU 对象或图像像素。

每个材质组显式传入顶点起点，使用同一 Mesh 三角形缓冲；相邻同 Mesh 的实例继续合批。颜色纹理使用 sRGB SRV，透明度纹理使用线性 SRV；各向异性 wrap 采样保留负 UV，首次上传后通过 gfx 生成 mip。漫反射 alpha × MTL opacity × 遮罩红通道小于 0.5 的像素在实体、MatCap、线框和深度预通道均裁弃。

GPU 图像缓存以不可变 Image owner 与颜色空间为键并持有 CPU 所有者；未使用版本随帧退休，关闭时沿原 Context 合同释放。无材质贴图使用共享白图。场景图片与 UI 图片统计分开：`material_upload_count` 为本帧新增图片上传数（包括首次白图），`cached_material_images` 为当前场景图片缓存项数；不含 gfx 等待退休的资源。不增加原生设备/队列或帧内磁盘读取。

此路径使用既有基础方向光和环境项，没有复现参考 DDGI 图中的间接光、阴影和 PBR；未使用的高度/高光贴图仍保留在源资产中。
