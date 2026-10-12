# 编辑器 renderer 当前契约

本页维护当前执行、显示和资源合同；光照算法、默认值与限制统一见 [Scene Lit](scene-lit.md)，已验证的配置和证据见[验收记录](acceptance.md)，尚未实施的方向见[后续演进](../../research/scene-lit-next-evolution-20261010.md)。研究文档中的历史参数不覆盖这些当前合同。

入口：[renderer.hpp](../../../src/renderer/renderer.hpp)，目标 `atelier_renderer`，由 [cmake/renderer.cmake](../../../cmake/renderer.cmake) 的 `atelier_add_renderer()` 创建。调用前由应用根构建创建 `atelier_rhi` 和 `atelier_ui_protocol`；可执行目标调用 `atelier_deploy_rhi_runtime(target)` 部署同一套 DXC、Agility 与 PIX DLL。shader 在配置时嵌入库，不依赖启动目录或另一 worktree。

## 一条执行链

应用持有 HWND、Runtime 对象与编辑状态 → CPU 场景数组和 UI 完整帧 → `Renderer::render` → 同一 RHI Context/gfx 录制与 Present → 应用只在 accepted 时替换命中帧。

renderer 不保留业务对象身份、GC 根或 JIT 回调。UI 布局、文本排版、输入处理和撤销归各自拥有者；renderer 消费 [公共 CPU 协议](../../../src/ui/frame.hpp)。同一 owner 线程完成打开、查询、绘制和关闭，窗口必须存活到 renderer 关闭之后。

## 场景和 UI

- `MeshInstance` 提供完整 local-to-world 矩阵、线性基础色、选中标记与共享不可变 Geometry；Scene Lit 还消费粗糙度、金属度、HDR 自发光及稳定组件键。`geometry::Mesh` 是经构造验证的连续三角形顶点资源，`geometry::cube_mesh()` 只是一个内置资源。相邻且引用同一资源的实例合为一次 instanced draw，不同资源分批。GPU 缓存保有对应 CPU 资源，未使用项沿 gfx 机制退休；没有另一套业务对象身份。全部表面路径消费同一标准材质和 Opaque/Mask 覆盖；具体范围见[材质系统](materials.md)和 [Scene Lit](scene-lit.md)。
- 场景管线和深度纹理在首次绘制非空场景且 viewport 非零时创建；后续按原生命周期复用，深度尺寸在实际场景绘制时跟随目标更新。纯 UI 窗口仍使用同一 Renderer/Context，无场景管线创建、深度分配或清除。UI 管线在 `open` 时准备，应用可提前创建并保留隐藏的弹出窗口以移出首次点击的初始化成本。
- 相机为左手系，+Y 向上，使用有限 Reversed-Z：D3D 深度近端为 1、远端为 0，背景清除值为 0。矩阵为 row-major，行向量按 `p * world * view * projection` 执行；透视和正交都遵循同一深度约定。renderer 不维护相机控制状态。
- `Frame::lighting_focus` 由宿主提供 DDGI 跟随关注点，默认世界原点，与相机眼点及 Edit/Play 独立；编辑器使用观察 target，游戏可提供其接收区域关注点。宿主发布 camera/view 前共用 `probe_focus_error` 校验实际世界精度；Renderer 编码前再次校验。体积组织、历史与预算合同见 [Scene Lit](scene-lit.md#ddgi-探针场)。
- World/应用负责矩阵求值，renderer 只消费最终数学值；不依赖 World/Runtime，不重做 TRS 分解。World 已提供空间树，renderer 只接收其最终仿射矩阵。仿射矩阵平移位于索引 12/13/14，索引 3/7/11 为 0、15 为 1。非有限、非仿射或奇异矩阵使整帧在编码前返回 invalid_argument、terminal=false；保留旧 capture 和执行域，不用单位矩阵兜底，也不静默略过对象。
- 法线按 world 的 3×3 逆转置计算。CPU 用 double 计算余因子，整个矩阵共用一个缩放因子，保留任意法线所需的行间比例；pixel shader 先按最大分量缩放再规范化。镜像输入保留作者朝向，默认双面；显式单面材质在共享表面路径中裁弃背面。CPU 同时检查实际上传的 FP32 矩阵，极小数清零后失去秩的输入在编码前拒绝。统一极小/极大缩放继续支持；这不承诺任意病态非均匀矩阵的数值精度。World 局部 scale 仍限定正、非零、有限。
- 场景 viewport 与 UI 坐标均为窗口物理像素，左上原点；DPI 已由 UI 应用。先画场景，再按 UI 数组顺序绘制；应用/UI 应给场景 viewport 留出可见区域。
- UI 支持半开 scissor、实色矩形、R8 覆盖图与 UV。输入为线性 straight RGBA，pixel shader 乘 coverage 与 alpha 后输出预乘颜色，混合使用 ONE/INV_SRC_ALPHA。
- renderer 显式请求 SDR RGBA8 backbuffer，并校验实际 RTV 格式为 `R8G8B8A8_UNORM_SRGB`；sRGB RTV 执行写入编码与线性混合。RHI 未指定该选项的旧调用仍采用 gfx 的显示器自动选择，不能推导所有 Context 都保证 RGBA8。
- Coverage 的非零 identity 与 revision 一起标识不可变内容，renderer 缓存同时保有 `shared_ptr<const Coverage>`。CPU 行可有 padding；上传时收紧行。每个实际使用的版本只上传一次，无用缓存释放 CPU owner，GPU 资源沿 gfx 原有机制退休。当前没有字体图集、显存预算或资源管理器。

## 视口显示策略

应用的 Workspace 将 World 光照/效果作者值与视口显示/诊断偏好求值为同一 `Frame::view`。World 配置随 Scene 保存并参与 Session 历史；诊断偏好与实际探针缓存不存入 Scene，详情见 [Scene Lit](scene-lit.md)。Scene View 路径栏末尾的显示图标菜单包含 Shading、Edges、Topology 三组互斥选项，Guides 图标菜单提供独立勾选；两者使用既有 CPU UI、独立菜单窗口和 accepted 输入链。着色模式切换不修改 World 材质、几何资源、对象身份或文档撤销历史。普通新建场景使用 MatCap；无参数启动的官方 Sponza 与 Examples 入口使用 Scene Lit，详见[官方 Examples](../../../assets/examples/README.md)。直接调用 Renderer 的默认设置仍为 Smooth。

- **Shading**：Smooth 使用顶点法线，Flat 使用依据作者法线定向的几何面法线；两者消费解析后的标准材质基础色和法线图，遵循材质双面设置。MatCap 按视空间法线采样一个 256×256 sRGB 陶土球图，覆盖基础色；球图在第一次实际场景绘制时生成并上传一次，不消费场景灯光。相机 view 沿既有刚体 LookAt 约定。MatCap 自身不产生物体间阴影、接触遮蔽或真实环境反射。
- **Edges**：Off 只保留选中边高亮；Overlay 将边线直接合成到表面着色中，沿原几何批次一次绘制；Lines: Visible 先绘制整个场景的深度，再绘制通过深度测试的边；Lines: All 使用相同深度结果，显示包括被遮挡部分在内的边。两种纯线框均保留完整场景深度供诊断读取，未覆盖的颜色保留背景。
- **Topology**：Polygons 尊重生产者提交的边标记；Triangles 显示全部三角形边。每个三角形的三位 mask 中，bit i 表示顶点 i 对边。没有 mask 的既有资源按全部三角形边处理；不通过位置焊接或共面关系猜测原始拓扑。内置 cube、plane、sphere 提供边标记，过滤三角化对角线。OBJ 导入器对简单多边形三角化并保留边界；已三角化源文件无法恢复丢失的原始边。
- 线宽以物理像素计，默认 1.25，接受范围 (0,8]；叠线不透明度默认 0.7，范围 [0,1]。原始三角形重心坐标与 clip w 使用同一透视插值，两者相除得到屏幕仿射边函数；以函数绝对值除以屏幕梯度长度得到到原始边的像素距离，跨相机平面或近裁剪面时仍成立。梯度先按最大分量归一化，避免极端投影尺度在平方时溢出或下溢；常量边函数不生成有限边界。选中边至少 2 像素，显示橙色；表面不整体染黄。仅线模式的可见线为不透明浅灰（抗锯齿边缘除外）。

全部资源仍在当前 Context 的借用 gfx 合同内创建、绑定和录制。场景采用 D32_FLOAT 深度和 GREATER 比较；深度预通道使用 gfx 的 ZERO/ONE 混合保持颜色，可见线通道使用 GREATER_EQUAL，全部线使用 ALWAYS，两者关闭深度写入。没有新建原生设备、队列、提交或等待入口。pipeline 和 MatCap 随 Renderer 保有、关闭时沿原机制退休；纯 UI 窗口继续不创建场景资源。

表面、编辑深度与边线共用 `raster_position.hlsl`：CPU 以相同顺序合成 view-projection，再由 GPU 计算 world → view-projection。位置计算及 `SV_Position` 使用 HLSL `precise`，保持独立编译 pass 的运算顺序；这是 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-variable-syntax)建议的多 pass 深度一致性约束。避免一条路径在 GPU 连乘三矩阵、另一条路径预先合成矩阵所产生的深度舍入差异。TAA / T2x 的颜色仍使用抖动投影，编辑深度与边线使用同一无抖动投影；没有用深度偏移或关闭遮挡来掩盖误差。

选中高亮目前表达原始网格边，与用户显式 wireframe 共用同一个测距机制。独立 ID/mask 加深度的屏幕轮廓适合表达对象外轮廓：例如 [Blender 的轮廓解析](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_outline_detect_frag.glsl)读取邻近 ID 与场景／轮廓深度，[Unreal Custom Depth/Stencil](https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-materials?application_version=4.27)也支持类似对象轮廓。它不等同于显示原始多边形边，并需要额外屏幕资源与解析工作；对于作为单个对象导入的复杂场景，相同 ID 内部不会自动给出原始网格拓扑。当前不把两者互换，保留显式线框；没有复制这些引擎的实现。

### 背景与参考辅助显示

`ViewSettings.background` 保存 MatCap / Smooth / Flat 工作室模式的背景偏好，正式编辑器默认蓝灰渐变；Scene Lit 始终使用黑背景，包括空场景与纯线框显示。Guides 中的 Gradient Background 在 Scene Lit 下禁用，切回工作室模式恢复原偏好。`reference_grid`、`origin_axes` 仍独立于着色模式；正式编辑器默认开启 XZ 平面 Y=0 参考网格及 X/Z 原点轴线，直接 Renderer 调用默认关闭。辅助显示不创建 Actor，也不改变几何、材质、场景序列化、选择或撤销历史。

背景只绘制 scene viewport；网格与轴线通过程序化全屏三角形生成，没有网格顶点缓冲或贴图。网格基准为 0.2 米细格、每五格主线，根据投影像素密度在十倍尺度之间交叉渐变，并在接近地平线和远处淡出。颜色输入为线性值，由既有 sRGB RTV 编码；线宽以物理像素计算。

有物体时参考网格读取同一场景深度、关闭深度写入。实体模式为背景→实体→网格→UI；纯线模式为背景→完整物体深度→网格→Visible/All 边线→UI。共面物体优先，All 只改变物体边线显示，不令网格穿透表面。空 World 仍绘制背景与网格，但不为辅助显示创建深度、MatCap 或实例缓冲，capture 深度保持全 0；零尺寸 viewport 和纯 UI 窗口不绘制这些辅助内容。

网格在齐次坐标中直接解参考平面交点，避免相近世界坐标相减损失方向精度。辅助深度比较按当前深度斜率覆盖光栅化子像素舍入与 FP32 运算误差，使共面实体优先；该偏移不写入场景 D32，也不降低其保存精度。

CPU 在编码前复用数学模块计算一次 inverse view-projection。有限但不可逆或无法表达为有限 FP32 逆矩阵的相机，仅跳过参考网格；背景与既有场景路径继续，保持旧相机输入合同。相机落在平面、交点位于近裁面后方或接近水平时，网格稳定裁弃/衰减。没有另一个相机控制状态。

### 运行状态与着色模式的独立性

Edit/Play 决定场景是否执行及如何更新；视口显示决定同一场景快照如何呈现。两者不互相选择：Play 可以使用 MatCap 等工作室显示，编辑状态也可以使用 Scene Lit。Scene Lit 消费场景真实光源与材质；不以是否 Play 作为渲染分支，也不把 MatCap 定义为只能用于编辑的模式。

Scene Lit 的环境照明由场景唯一的 Sky Light（天光）提供。Application 从 World 提取天光颜色乘强度，填入 `Frame.environment`；默认是零，不再由 ViewSettings 隐式补光。禁用、删除或更换场景会更新该值，并沿既有信号历史处理收敛。该辐射参与 DDGI 和环境反射，但不自动绘制可见天空；当前 Scene Lit 背景为黑，参考网格继续消费同一场景深度且不写深度。抗锯齿菜单、独立信号历史与 Local GI / GTAO 互斥合同见 [Scene Lit](scene-lit.md)。带方向的环境贴图和可见天空资产尚未接入。

每份共享 Mesh 首次使用时创建一个 GPU 三角形缓冲，包含顶点、边标记和面法线；切换显示方式复用同一份几何缓存。每个场景批次的每个材质分组在实体/叠线模式为一次 draw，在纯线模式为深度与边线两次 draw；两者都计入唯一的 `FrameResult::stats.scene_draw_calls`，不代表 GPU 计时。MatCap 属于场景资源，不混入 UI image/coverage 统计。

## 工作室 Cavity

`ViewSettings::cavity` 是显示设置，独立于 Material、World 与 Edit/Play。默认开启，ridge/valley 默认 1、范围 0～2，radius 默认 1、范围 0.5～4 个物理像素；DPI 不再缩放一次。MatCap、Smooth、Flat 消费它，Scene Lit 和纯线框跳过。关闭或两项强度为零时不创建目标、不编码效果 pass。

语义参考 [Blender Screen Cavity](https://docs.blender.org/manual/en/5.2/editors/3dview/display/shading.html)：强调几何凹凸，半径按屏幕像素定义，属于工作室对比度增强。实现使用视空间作者法线（Flat 用定向面法线）的有符号散度，D32 深度重建用于拒绝相隔过远的表面；不依赖材质颜色、法线贴图、三角边掩码、灯光、随机数或时间历史。平面内部与共面三角剖分为零曲率，凸边提亮、凹处压暗；屏幕之外没有补全，也不代替 AO 或真实光照。

表面直接绘入 R8G8B8A8_SRGB 颜色与 RGBA16_FLOAT 法线/覆盖 MRT，随后一个全屏 pass 合成到窗口。gfx 的隐式窗口目标不与额外 MRT 混用；这里没有原始图形 API 或额外 RHI 入口。背景、参考网格、选择/线框叠层和 UI 保持原显示职责：背景先绘制，Cavity 只合成已覆盖表面，网格与编辑叠层随后绘制。选择线与 Overlay 按需使用既有可见线管线。

有效期间保有每窗口像素 12 字节的两个目标，复用既有 D32 深度；整数半径查询四个邻居，分数半径插值两个邻域。尺寸随实际接受的 render target 更新，空场景、关闭、零强度、切换 Scene Lit/纯线框时沿 RHI 退休。旧 GPU 使用期仍由 RHI 管理。零法线或无法重建的位置保留基础颜色；参数非法在编码前拒绝。这里给出工作量和逻辑存储，不宣称已测得 GPU 耗时。

## 帧接受与失败

`Frame::ui_frame` 是帧 id、extent、DPI 和 surface revision 的唯一来源；所有输入借用到同步 `render` 返回。renderer 的 `target()` 报告实际 backbuffer 尺寸，窗口 client 尺寸可能在 resize 时领先一帧。

- 隐藏、最小化、零 client 尺寸或输入 extent 与实际 target 不符：编码前返回 skipped，capture 不变。空 scissor 不会落入 gfx 的“零尺寸代表整个目标”惯例。
- 完整 `Context::frame` 成功且 Present 未返回 `DXGI_STATUS_OCCLUDED`：accepted。`accepted_extent` 是编码前捕获的实际尺寸；gfx 在旧目标 Present 后 resize，`next_extent` 只适用于下一帧。accepted 不承诺已经完成扫描显示。
- CPU 校验和编码前 CPU 分配失败：`Error::terminal=false`，可重新准备帧。开始 GPU 创建、上传或录制后失败：terminal，后续只可关闭，不宣称回滚或继续使用上一 GPU 帧。
- 模式退出引起的 Scene Lit 历史失效也在 CPU 准备成功后执行；被拒绝的候选帧不改变有效 Lit 历史。接受空场景沿原 Context 退休旧场景的光栅与射线资源，可复用的 Scene Lit 目标仍由 Renderer 持有并报告逻辑字节。
- Present 之后的 resize、等待、Reset 等失败沿 RHI 终态返回，不会提前发布 accepted。遮挡是 skipped，GPU 可能已经提交；它不是 GPU 回滚。
- 应用只根据 accepted 替换完整命中数据，并负责在窗口大小/DPI 与命中帧不匹配时暂停命中、取消 press/capture；renderer 不建立第二份输入状态。

`close` 退休 GPU 资源、释放 Pipeline、通过原 Context 排空和关闭。重复关闭成功，关闭失败保留清理入口；无法安全清理的最终析构 fail-fast。

## 诊断捕获

### 每帧计数

`FrameResult::stats` 是该次 `render` 返回的 `FrameStats` 值快照。下表中的既有计数为 `uint64_t`，不增加 GPU 查询、等待或动态分配。Scene Lit 另有完成帧 GPU 计时与射线计数，具体时序、单位和内存统计边界见 [Scene Lit 诊断](scene-lit.md#诊断与性能边界)。

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

`render(frame, &capture)` 显式请求 GPU 同步，返回本帧目标的紧密 sRGB RGBA8 和 float32 深度，行自上向下。仅在 accepted 时替换输出。UI 不写场景深度；深度遵循上述 Reversed-Z 合同，背景为 0。普通 render 不调用 finish；诊断捕获不应放入交互帧热路径。

TAA / SMAA T2x 的采样深度只用于内部光照重建与历史。需要编辑辅助显示或 Capture 时，AA 完成后复用现有深度通道重建公开 Camera 对应的无抖动 D32 深度，再绘制网格与选中边；这次绘制计入 `scene_draw_calls`。没有这些消费者时省去重绘。

本帧没有场景绘制时，capture 的深度数组直接填充为 0，不创建或读回 GPU 深度资源，也不返回之前场景残留的深度；颜色仍从实际 backbuffer 读回。场景首次创建失败沿既有 GPU 阶段终态规则处理。

捕获命令经现有 gfx 录制，使用 256 字节对齐的 GPU 行距，完成后拆成紧密 CPU 行。读回源是 resize 前真正绘制的目标。没有原生队列逃生口，也不改变原有紧密 texture copy 入口。

## 场景材质与贴图

标准表面的权威合同见[材质系统](materials.md)。Mesh 保存 UV、MikkTSpace 切线、命名材质槽及覆盖全部三角形的连续 MeshPart；World 捕获按槽解析后的不可变 Surface。材质、贴图沿现有 Runtime 对象身份与 Project 资源身份，渲染帧只拥有 CPU 内容，不持有业务对象或磁盘路径。

颜色与发光图使用 sRGB，粗糙度、金属度、法线及透明度图使用线性采样。共享材质表供工作室、Scene Lit GBuffer、射线命中、发光采样与编辑深度使用；Mask 在这些路径使用同一覆盖公式。MatCap 继续以几何法线选择 MatCap 颜色，同时遵循表面覆盖；显示模式不改写资产。

GPU 图像缓存以不可变 Image owner 与颜色空间为键并持有 CPU 所有者；未使用版本随帧退休，关闭时沿原 Context 合同释放。稳定帧复用材质表与纹理，材质参数修改不重建网格；Opaque/Mask 变化重新核对 BLAS 透明性。`material_upload_count` 为本帧新增图片上传数（包括首次白图），`cached_material_images` 为当前场景图片缓存项数；不含 gfx 等待退休的资源。不增加原生设备/队列或帧内磁盘读取。

源顶点为 48 字节，光栅与射线派生三角形均为 176 字节：前者携带编辑边信息，后者携带连通分量闭合信息及材质槽。法线图不改变几何闭合、深度或射线起点偏移。原先无资产绑定的实例颜色与标量参数通过同一材质表解析，既有场景保持可读。
