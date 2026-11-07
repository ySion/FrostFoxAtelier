# 编辑器 renderer 当前契约

本页维护当前执行、显示和资源合同；光照算法、默认值与限制统一见 [Scene Lit](scene-lit.md)，已验证的配置和证据见[验收记录](acceptance.md)，尚未实施的方向见[后续方向](scene-lit.md#后续方向)。

入口：[renderer.hpp](../../../modules/render/renderer/include/renderer/renderer.hpp)，目标 `atelier_renderer`，由 [cmake/renderer.cmake](../../../cmake/renderer.cmake) 的 `atelier_add_renderer()` 创建。调用前由应用根构建创建 `atelier_rhi` 和 `atelier_ui_protocol`；可执行目标调用 `atelier_deploy_rhi_runtime(target)` 部署同一套 DXC、Agility 与 PIX DLL。shader 在配置时嵌入库，不依赖启动目录或另一 worktree。

## 一条执行链

应用持有 HWND、Runtime 对象与编辑状态 → CPU 场景数组和 UI 完整帧 → `Renderer::render` → 同一 RHI Context/gfx 录制与 Present → 应用只在 accepted 时替换命中帧。

renderer 不保留业务对象身份、GC 根或 JIT 回调。UI 布局、文本排版、输入处理和撤销归各自拥有者；renderer 消费 [公共 CPU 协议](../../../modules/gui/ui_protocol/include/ui/frame.hpp)。同一 owner 线程完成打开、查询、绘制和关闭，窗口必须存活到 renderer 关闭之后。

`Options::frame_latency_waitable` 选择 [RHI 呈现准入](../rhi/contract.md#呈现准入)，默认关闭。宿主通过 `wait_frame` 在安全外层有限等待；Renderer 不取得原生句柄、不分发消息。`render` 在目标可绘制且尺寸匹配后、任何本帧编码前零超时确认许可，未就绪返回 `not_ready`，不更新 Capture 或接受身份。许可只由实际 RHI `frame` 消费，材质/Scene Lit 准备尚未就绪及 LitFallback 重试仍沿同一许可；准入不是接受帧或渲染尝试计数。

### 合作停机

`Frame::stop_requested` 可借用宿主的只读停止谓词，仅在本次 `render` 内于 owner 线程调用；谓词须为常数级廉价查询，不抛异常、不重入 Renderer。Renderer 不持有宿主事件、不等待，也不创建取消工作线程。未请求停止时，原有准备、编码和呈现顺序不变。

渲染入口、几何实例/材质表准备、几何焊接的每 1024 个顶点/三角形、共享几何缓冲上传、工作室逐网格批次及提交前检查停止。观察到停止后返回独立的 `FrameStatus::cancelled`，而不是 `not_ready`：不接受或 Present 半帧、不替换 Capture，之后只允许 `close`/析构。已录制命令和部分创建的 GPU 资源仍由原缓存及 gfx 延迟退休机制拥有，正常关闭会提交、排空并释放，不是丢弃命令或回滚；显式 Capture 也可能在最后检查前调用 finish。关闭前仍须先释放 MaterialBatch。

取消只记一条正式 info `renderer.frame_cancelled`，包含 frame_id、phase 和本次 render 的 elapsed_ms。它不表示 GPU 零执行或已完成。UI 纹理批次、Scene Lit 内部编码和 BLAS 循环尚无停止检查点；单个驱动调用、同步编译、分配或 fence 等待内部也不能被这个谓词抢占。异步管线准备的关闭仍遵守 RHI 原有 join/资源寿命约束，不新增等待路径或截止时间，不能承诺所有首帧阻塞均可取消。现有 Renderer 默认验收中的 K6 代表以首次查询即为真的谓词检查初始取消、未发布帧/捕获和正常关闭，不证明中途部分上传的实际关闭耗时。

## 程序材质批次

`prepare_materials(span<ProgramSurfaceRef>)` 创建 move-only `MaterialBatch`，保有完整 Surface 表，不采用候选。17 个材质相关管线按精确 Program 对象集合共享，不按每个 MI 单独生成；参数、图片、policy 和表顺序不进入代码集合键。参数按各 Program 的真实布局打包，生成器的 `ms_load_parameters` 读取 byte base；普通参数不进入 shader 源码。

`preparation(batch)` 收取任务且不等待。全部成功后准备候选自己的表与图片；即使全部管线被复用，也必须调用此入口。ready 表示同一 Context 中有序可用，不表示 GPU 已完成或已 Present。可恢复准备失败属于该 batch；资源创建/上传之后失败仍令 Renderer terminal，只能关闭/析构，并不等于已确认设备丢失。`preparation(view)` 独立准备 view 管线，不证明某个候选完成。

`Frame::materials` 借用同 Renderer 的 ready batch，全部 `MeshInstance::material_slots` 必须按几何槽顺序完整映射至该表；Renderer 不解析网格默认材质。UI/空场景可以没有 batch，有网格则必须提供。pending 返回 not_ready，不编码、不 Present、不替换 Capture；宿主可继续提交上一完整采用组。工作室、GBuffer、阴影、编辑深度/边线、inline RT/DDGI 及材质相关诊断共用该批次。Masked 判断 `coverage >= 0`，Opaque 忽略 coverage；共享 BLAS 的 opaque 标志取所有实例/槽策略的交集。任一 Mask 内容变化保守失效覆盖/距离/阴影缓存；任意程序保守视为可能发光，实际辐亮度来自执行结果。MatCap、Flat 与 Cavity 仍保留各自的作者几何法线规则。

batch 必须在所属 Renderer 关闭前于 owner 线程释放；存活批次令 close 在任何破坏性清理前返回 busy。最后编码使用后的释放沿原 gfx 延迟退休，不逐对象 finish；Surface 本身不保留 Runtime 根，CPU 内容可独立保活。帧槽映射使用原 DeviceTable/FrameSlots，不重写仍在途的上传存储。

具名纹理使用真实局部 slot，沿现有图片缓存上传 linear/sRGB 资源与 mip；未采用 batch 同样保有图片 owner。工作室、GBuffer、编辑深度使用最终坐标的程序 footprint；阴影/射线候选保持 LOD0，接受后的 DDGI 命中使用射线足迹。采样边界及 MS/MI、宿主完整组采用和 accepted-frame 拾取统一见[材质系统](materials.md)。不存在无 batch 的固定材质产品路径；实现范围不代替逐目标端到端验收结果。

## 场景和 UI

- `MeshInstance` 提供完整 local-to-world 矩阵、选中标记、共享不可变 Geometry、稳定组件键和完整材质槽映射；表面参数来自 `Frame::materials`。`geometry::Mesh` 是经构造验证的连续三角形顶点资源，`geometry::cube_mesh()` 只是一个内置资源。引用同一资源的可见实例合为一个批次，每批在每个光栅 pass 中一次 instanced draw，三角形按所属 part 取材质槽，不同资源分批；批次的划分与顺序见[运行状态与着色模式的独立性](#运行状态与着色模式的独立性)。GPU 缓存保有对应 CPU 资源，未使用项沿 gfx 机制退休；没有另一套业务对象身份。全部表面路径消费同一 ProgramSurface 和 Opaque/Mask 覆盖；具体范围见[材质系统](materials.md)和 [Scene Lit](scene-lit.md)。
- 主视图采用不可变 Mesh 构造时计算的局部包围盒，按当前 world/view/projection 做保守齐次视锥剔除。六个裁剪面包含浮点误差余量，XY 另留一个像素覆盖时间抖动与小数视口；不作透视除法，跨眼点/近平面、镜像和非均匀变换仍按包围盒支持范围处理。可见实例写入按资源分组的绘制索引表，着色器经该表读取原 Frame 索引上的实例、材质偏移与身份，Frame 本身不重排；GBuffer、工作室、深度和选中边线共用这张表，选中边线只读其中可见且选中的一段。完整 Frame、材质表、TLAS、发光域、DDGI 与阴影投影者仍保留屏幕外物体，缓存也不随相机进出逐帧退休。不可表示的浮点裁剪范围保守保留；非法 world 变换仍在剔除前拒绝整帧。当前尚无物体遮挡剔除、LOD 或网格簇剔除。
- 场景管线和深度纹理在首次绘制非空场景且 viewport 非零时创建；后续按原生命周期复用，深度尺寸在实际场景绘制时跟随目标更新。纯 UI 窗口仍使用同一 Renderer/Context，无场景管线创建、深度分配或清除。UI 管线在 `open` 时准备，应用可提前创建并保留隐藏的弹出窗口以移出首次点击的初始化成本。
- Scene Lit 管线按使用条件分组，在 RHI Context 的工作线程上准备；Renderer 的 owner 线程从不等待编译、PSO 创建或 GBV 预热。非材质基础组 25 条，任何 Lit 视图都需要；接收面统计和探针几何叠层按视图需求各准备一条，其余材质相关管线归 MaterialBatch。`preparation(view)` 启动该视图缺少的组，收集已完成的管线，并按该视图所需的组报告 done/total；不画 Lit 的视图总是 ready。done/total 只描述当前视图：设置改变后需要新的组时，total 随之变大，已经 ready 的进度会回到未就绪，直到新组完成。进度显示和作业状态不能假设 total 固定不变。`render` 对画 Lit 的帧做同样的检查。准备失败（例如编译错误）不是终止错误，之后的 Lit 视图都返回同一错误，工作室着色不受影响。`Options::shader_cache` 是交给 RHI 的 DXIL 缓存目录，空路径表示不用缓存，见 [RHI 合同](../rhi/contract.md#异步准备与-dxil-磁盘缓存)。
- 相机为左手系，+Y 向上，使用 Reversed-Z：D3D 深度近端为 1，背景清除值为 0。常规透视使用无限远深度映射 `depth=near/view_z`，正交仍使用有限投影。矩阵为 row-major，行向量按 `p * world * view * projection` 执行。`Camera::far_depth` 是主视图几何裁剪下限，须有限且在 `[0,1]`；宿主由透视 near/far 派生该值，正交为 0，已有有限投影或无限制直接调用者默认 0。CPU 视锥、表面/GBuffer、编辑深度、边线、参考网格与探针诊断采用同一远平面；跨面三角形用 `SV_ClipDistance` 裁剪，不靠整网格丢弃或像素 discard。透视 far 平面上的有效深度为 far_depth，仍是表面；背景只认 0，UI/天空不受几何远平面裁剪，深度不重新归一化。无限透视的深度 0 反投影为无穷远方向，不作为有限位置除以 w；有限正交的深度 0 仍反投影为有限 far 位置（w 非零）。far_depth 改变是投影变化，调用方须设置 camera_cut，即使 VP 未变。renderer 不维护相机控制状态；无限远投影不表示无限场景范围。
- `Frame::lighting_focus` 由宿主提供：编辑器使用观察 target，Play 使用主相机的世界位置（平移部分，不加前向偏移）。DDGI 跟随体积与有界近源选灯网格消费该点，分别维护自己的布局；原地转动 Play 相机不移动 focus。宿主发布 camera/view 前共用 `probe_focus_error` 校验实际世界精度；Renderer 编码前再次校验。Play 中相机或 focus 不可用时不提交场景/Lit 帧，黑色背景上的工程 UI 照常呈现，照明时钟重置；恢复首帧、主相机实体切换或 FOV/near/far 改变时发送 camera cut。近源网格无法保持 8 米格边精度时退回全局 proposal，不为这个可选采样加速结构额外收紧合法 Frame；原有 DDGI 体积、focus 与 World 作者值校验继续适用。体积组织、历史与预算合同见 [Scene Lit](scene-lit.md#ddgi-探针场)。
- World/应用负责矩阵求值，renderer 只消费最终数学值；不依赖 World/Runtime，不重做 TRS 分解。World 已提供空间树，renderer 只接收其最终仿射矩阵。仿射矩阵平移位于索引 12/13/14，索引 3/7/11 为 0、15 为 1。非有限、非仿射或奇异矩阵使整帧在编码前返回 invalid_argument、terminal=false；保留旧 capture 和执行域，不用单位矩阵兜底，也不静默略过对象。Renderer 按 `Frame::meshes` 位置保存上一次通过检查的实例内容与准备结果，逐字段按位比较（Geometry 与材质槽表以所有权比较，其内容发布后不可变），只重新检查和准备变化的位置；非法内容持续存在的每一帧都被拒绝。非零 `stable_key` 的使用计数跨帧维护、在所有着色模式下更新，Lit 帧中出现重复即拒绝。
- 法线按 world 的 3×3 逆转置计算。CPU 用 double 计算余因子，整个矩阵共用一个缩放因子，保留任意法线所需的行间比例；pixel shader 先按最大分量缩放再规范化。镜像输入保留作者朝向，默认双面；显式单面材质在共享表面路径中裁弃背面。正反面由逆转置几何法线判定，不依赖三角形绕序；将来启用光栅背面剔除或 RayQuery/TLAS 剔除标志时，镜像实例必须按行列式符号翻转正面约定，或按符号分批。CPU 同时检查实际上传的 FP32 矩阵，极小数清零后失去秩的输入在编码前拒绝。统一极小/极大缩放继续支持；这不承诺任意病态非均匀矩阵的数值精度。World 局部 scale 可为负（镜像）或零；World 不提交奇异 world 矩阵的网格，零缩放空间在 World 侧按退化跳过并诊断。
- 场景 viewport 与 UI 坐标均为窗口物理像素，左上原点；DPI 已由 UI 应用。场景覆盖按像素中心处于逻辑视口半开区间判断。`RasterViewport` 统一导出整数外包硬件视口、像素中心 scissor 与最终 clip 补偿，避免 D3D 对小数 viewport 的隐式向下取整漏掉末列/末行；场景表面、深度、线框、辅助显示和 Lit 诊断复用同一转换。逻辑相机、运动、深度 Z/W 和屏幕重建矩阵不变，整数视口直接保留原 clip。先画场景，再按 UI 数组顺序绘制；应用/UI 应给场景 viewport 留出可见区域。
- UI 支持半开像素裁剪、实色矩形、R8 覆盖图、RGBA 图片与 UV。输入为线性 straight RGBA，pixel shader 乘 coverage 与 alpha 后输出预乘颜色，混合使用 ONE/INV_SRC_ALPHA。裁剪逐实例进行：clip 收紧到目标范围后打包进实例，pixel shader 按像素中心落在半开区间内判断，与同一整数 scissor 逐像素一致；空 clip、零尺寸、透明以及与 clip 不相交的项在 CPU 侧剔除，不上传实例或贴图。全部 UI 实例共用一次上传、一条管线和一次 draw，视口叠加层另占一次，两者都只依赖 D3D 在同一次 draw 内按图元提交顺序混合来保持数组顺序，不重排、不按 clip 或贴图切批。本帧用到的 Coverage 与 Image 按首次出现的顺序去重成一张贴图表，作为无界纹理数组绑定，实例携带表内下标；表的内容和顺序与上一帧相同时 gfx 不重写描述符，任何变化都会让 gfx 重写整张表的描述符。
- renderer 显式请求 SDR RGBA8 backbuffer，并校验实际 RTV 格式为 `R8G8B8A8_UNORM_SRGB`；sRGB RTV 执行写入编码与线性混合。未指定该选项的 RHI 调用采用 gfx 的显示器自动选择，不能推导所有 Context 都保证 RGBA8。
- Coverage 的非零 identity 与 revision 一起标识不可变内容，renderer 缓存同时保有 `shared_ptr<const Coverage>`。CPU 行可有 padding；上传时收紧行。每个实际使用的版本只上传一次，无用缓存释放 CPU owner，GPU 资源沿 gfx 原有机制退休。当前没有字体图集、显存预算或资源管理器。
- 同一录制帧的新 UI 贴图共用一个 `FrameSlots<uint8_t>` 上传槽：先聚合唯一 miss 的紧密像素，再只写槽一次，逐图借用 `gfxCreateBufferRange` 录制 copy，全部完成后才录制 UI draw；无 miss 不写槽。每个源区间按 `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`（512 字节）对齐，而非仅满足 buffer range 的256字节要求，避免原本行宽符合 footprint RowPitch 的图片因源偏移而退化到 gfx scratch 逐行复制。gfx 原有行宽检查、OPTIONS13 查询与 scratch 路径不变。上传槽按录制序号而非 accepted id 选择，沿原3槽/fence复用；容量不足按2幂增长，关闭时退休，不逐图创建 native 上传 buffer，也不额外等待。CPU 聚合向量及各槽保留峰值容量；聚合字节数超过 FrameSlots 的32位计数可表示的2幂容量（2 GiB）时，在增长向量前返回容量错误，不截断偏移或尺寸，不冒充宿主堆耗尽。此时已开始编码，Renderer 进入仅可关闭的终态。纹理身份、首用顺序、逐图像素与描述符安装合同不变。
- 占用预算先按512字节起点对齐累加末项结束位置，再对单槽向上取2幂，不能直接按像素字节数取整。例如60,727字节的23项上传若排布至66,048字节，则单槽预计131,072字节，三个槽均使用该规模后合计393,216字节。这是布局预算，不是实测峰值；真实槽容量、CPU向量/任务列表容量和gfx scratch占用分别记录。

## 视口显示策略

应用的 Workspace 将 World 光照/效果作者值与视口显示/诊断偏好求值为同一 `Frame::view`。World 配置随 Scene 保存并参与 Session 历史；诊断偏好与实际探针缓存不存入 Scene，详情见 [Scene Lit](scene-lit.md)。Scene View 路径栏末尾的显示图标菜单包含 Shading、Edges、Topology 三组互斥选项，Guides 图标菜单提供独立勾选；两者使用既有 CPU UI、独立菜单窗口和 accepted 输入链。着色模式切换不修改 World 材质、几何资源、对象身份或文档撤销历史。无参数启动为空文档，普通新建场景使用 MatCap；打开工程不按示例名称改变显示模式，详见[Examples 工程](../../../examples/README.md)。直接调用 Renderer 的默认设置仍为 Smooth。

显示语义参考 [Houdini 视口模式](https://www.sidefx.com/docs/houdini/basics/view.html)与 [Display Options](https://www.sidefx.com/docs/houdini/ref/windows/displayopts_3d.html)：着色、边线与照明是不同维度，Smooth 不等于 PBR。视图 MatCap 覆盖与“未赋材质时使用 MatCap 默认材质”也不同；本项目采用前者。MatCap 纹理中的固定明暗不响应场景灯光（[官方说明](https://www.sidefx.com/docs/houdini/basics/viewporteffects.html)）。这些资料支持显示语义，不证明相同内部算法、默认值或 GPU 成本；Ghost 淡显不是当前合同。

World Settings 的 `direct_candidates` 属于作者值（1～32，默认 8），随 Scene v10 保存；它只控制 DDGI 探针命中点每次挑选的局部光候选数，屏幕直接光不随机选灯。`spatial_light_sampling` 是视口诊断偏好，默认 true，经统一 ViewOption/Control 入口切换探针命中点 fresh RIS 的近源/全局混合 proposal，不写 World、Scene 或撤销历史。屏幕直接光对 cluster 列表中的解析灯逐盏确定求值，方向光独立评估，没有历史；发光网格只经 DDGI 照明。新增资源仍沿现有 RHI 生命周期，D32_FLOAT 和前帧 FP32 深度合同不变。具体算法、能量归属和验收范围以 [Scene Lit](scene-lit.md)及[验收记录](acceptance.md)为准。

`LightFrame` 消费已验证的低层 Frame，在 CPU 从 `frame.lights` 派生统一的解析光源快照及当前近源 proposal；它不持有 World 或创建 GPU 资源。Scene Lit 仍是上传、绑定、统计与退休的唯一拥有者。近源网格只改变抽样概率，候选批次按确定性近源/全局配额生成，并以实际整数配额组成的完整概率求权重；单候选或无有效局部域时全部使用全局 proposal，远源与溢出源保留全局支持。网格滚动不作为辐射变化清历史，也不要求第二份旧网格。当前编码帧的保留源数、非空 cell 数、逻辑字节及上传标记，与异步 GPU 完成统计分开，具体口径见 [Scene Lit 诊断](scene-lit.md#诊断与性能边界)。

直接光只有一条路径：保守灯列表、阴影 atlas 与融合 compose，没有视口开关。可选 `direct_cluster_inspect` 是默认关闭的视口诊断，不创建第二套 World 光源或作者配置入口。灯列表和 `ShadowCache` 只消费既有 Frame、LightFrame、Geometry/Object 缓存；阴影通过同一 RHI 的 D32 深度、矩形 clear、viewport/scissor 与延迟退休执行。缓存缺页不得默认为无阴影；atlas 放不下的投影光源整盏不贡献直接光；完整场景投影者与屏幕接收需求分开。投射阴影的光没有 stable_key 时每帧重绘，不受每帧 24 个 face 的预算；需要预算的生产方必须给光源非零键。DDGI 仅复用几何与端点都未改变的精确屏幕 LOS，细节与当前限制见 [Scene Lit](scene-lit.md)。

探针检查的 `probe_inspect_source` 为视口诊断选项，`-1` 默认自动选择，非负值选择全局物理 probe 槽位。typed ViewOption 与低层 Frame 共同校验最大探针池范围；合法索引超过本帧驻留池时返回明确的无选择结果，不因此拒绝 Frame 或回退另一个探针。检查不改变调度、运输或 World 作者配置，异步样本保留请求与完成帧身份；字段语义见 [Scene Lit 探针检查](scene-lit.md#ddgi-探针场)。

- **Shading**：Smooth 使用顶点法线，Flat 使用依据作者法线定向的几何面法线；两者消费解析后的标准材质基础色和法线图，遵循材质双面设置。MatCap 按视空间法线采样一个 256×256 sRGB 陶土球图，覆盖基础色；球图在第一次实际场景绘制时生成并上传一次，不消费场景灯光。相机 view 沿既有刚体 LookAt 约定。MatCap 自身不产生物体间阴影、接触遮蔽或真实环境反射。
- **Edges**：Off 只保留选中边高亮；Overlay 将边线直接合成到表面着色中，沿原几何批次一次绘制；Lines: Visible 先绘制整个场景的深度，再绘制通过深度测试的边；Lines: All 使用相同深度结果，显示包括被遮挡部分在内的边。两种纯线框均保留完整场景深度供诊断读取，未覆盖的颜色保留背景。
- **Topology**：Polygons 尊重生产者提交的边标记；Triangles 显示全部三角形边。每个三角形的三位 mask 中，bit i 表示顶点 i 对边。没有 mask 的既有资源按全部三角形边处理；不通过位置焊接或共面关系猜测原始拓扑。内置 cube、plane、sphere 提供边标记，过滤三角化对角线。OBJ 导入器对简单多边形三角化并保留边界；已三角化源文件无法恢复丢失的原始边。
- 线宽以物理像素计，默认 1.25，接受范围 (0,8]；叠线不透明度默认 0.7，范围 [0,1]。原始三角形重心坐标与 clip w 使用同一透视插值，两者相除得到屏幕仿射边函数；以函数绝对值除以屏幕梯度长度得到到原始边的像素距离，跨相机平面或近裁剪面时仍成立。梯度先按最大分量归一化，避免极端投影尺度在平方时溢出或下溢；常量边函数不生成有限边界。选中边至少 2 像素，显示橙色；表面不整体染黄。仅线模式的可见线为不透明浅灰（抗锯齿边缘除外）。

全部资源仍在当前 Context 的借用 gfx 合同内创建、绑定和录制。场景采用 D32_FLOAT 深度和 GREATER 比较；深度预通道使用 gfx 的 ZERO/ONE 混合保持颜色，可见线通道使用 GREATER_EQUAL，全部线使用 ALWAYS，两者关闭深度写入。没有新建原生设备、队列、提交或等待入口。pipeline 和 MatCap 随 Renderer 保有、关闭时沿原机制退休；纯 UI 窗口继续不创建场景资源。

表面、编辑深度与边线共用 `raster_position.hlsl`：CPU 以相同顺序合成 view-projection，再由 GPU 计算 world → view-projection。位置计算及 `SV_Position` 使用 HLSL `precise`，保持独立编译 pass 的运算顺序；这是 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-variable-syntax)建议的多 pass 深度一致性约束。避免一条路径在 GPU 连乘三矩阵、另一条路径预先合成矩阵所产生的深度舍入差异。TAA / T2x 的颜色仍使用抖动投影，编辑深度与边线使用同一无抖动投影；没有用深度偏移或关闭遮挡来掩盖误差。

选中高亮目前表达原始网格边，与用户显式 wireframe 共用同一个测距机制。独立 ID/mask 加深度的屏幕轮廓适合表达对象外轮廓：例如 [Blender 的轮廓解析](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_outline_detect_frag.glsl)读取邻近 ID 与场景／轮廓深度，[Unreal Custom Depth/Stencil](https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-materials?application_version=4.27)也支持类似对象轮廓。它不等同于显示原始多边形边，并需要额外屏幕资源与解析工作；对于作为单个对象导入的复杂场景，相同 ID 内部不会自动给出原始网格拓扑。当前不把两者互换，保留显式线框；没有复制这些引擎的实现。

### 背景与参考辅助显示

`ViewSettings.background` 保存 MatCap / Smooth / Flat 工作室模式的背景偏好，正式编辑器默认蓝灰渐变；Scene Lit 没有天空贴图时背景为黑，设了天空贴图时 Lit 合成在背景显示天空，空场景与纯线框显示仍为黑背景。Guides 中的 Gradient Background 在 Scene Lit 下禁用，切回工作室模式恢复原偏好。`reference_grid`、`origin_axes` 仍独立于着色模式；正式编辑器默认开启 XZ 平面 Y=0 参考网格及 X/Z 原点轴线，直接 Renderer 调用默认关闭。辅助显示不创建 Entity，也不改变几何、材质、场景序列化、选择或撤销历史。

背景只绘制 scene viewport；网格与轴线通过程序化全屏三角形生成，没有网格顶点缓冲或贴图。网格基准为 0.2 米细格、每五格主线，根据投影像素密度在十倍尺度之间交叉渐变，并在接近地平线和远处淡出。颜色输入为线性值，由既有 sRGB RTV 编码；线宽以物理像素计算。

有物体时参考网格读取同一场景深度、关闭深度写入。实体模式为背景→实体→网格→UI；纯线模式为背景→完整物体深度→网格→Visible/All 边线→UI。共面物体优先，All 只改变物体边线显示，不令网格穿透表面。空 World 仍绘制背景与网格，但不为辅助显示创建深度、MatCap 或实例缓冲，capture 深度保持全 0；零尺寸 viewport 和纯 UI 窗口不绘制这些辅助内容。

网格在齐次坐标中直接解参考平面交点，避免相近世界坐标相减损失方向精度。辅助深度比较按当前深度斜率覆盖光栅化子像素舍入与 FP32 运算误差，使共面实体优先；该偏移不写入场景 D32，也不降低其保存精度。

CPU 在编码前复用数学模块计算一次 inverse view-projection。有限但不可逆或无法表达为有限 FP32 逆矩阵的相机，仅跳过参考网格；背景与既有场景路径继续，保持旧相机输入合同。相机落在平面、交点位于近裁面后方或接近水平时，网格稳定裁弃/衰减。没有另一个相机控制状态。

### 运行状态与着色模式的独立性

Edit/Play 决定场景是否执行及如何更新；视口显示决定同一场景快照如何呈现。两者不互相选择：Play 可以使用 MatCap 等工作室显示，编辑状态也可以使用 Scene Lit。Scene Lit 消费场景真实光源与材质；不以是否 Play 作为渲染分支，也不把 MatCap 定义为只能用于编辑的模式。

Scene Lit 的环境照明由 World 的天光设置提供（`SceneLightingSettings` 的 `sky_enabled`、`sky_color`、`sky_intensity`，以及方向性贴图 `sky_map` 与 `sky_rotation`，写在 Environment 组件上）。宿主用 `rendering::sky_radiance` 求颜色乘强度，填入 `Frame.environment`；贴图和旋转角随 `Frame::view.lit` 传入。默认关闭，值为零。关闭天光、删除 Environment、换贴图或更换场景会更新这些值，并沿既有信号历史处理收敛。天光参与 DDGI 和环境反射；只有设了贴图时背景才显示天空，否则 Scene Lit 背景为黑，参考网格继续消费同一场景深度且不写深度。抗锯齿菜单与独立信号历史的现状见 [Scene Lit](scene-lit.md)。天空贴图的约定与限制见 [Scene Lit](scene-lit.md)。

每份共享 Mesh 首次使用时上传一份索引几何：逐位相同的角点焊接成顶点缓冲，每个三角形三个 uint32 索引，保持 Mesh 的三角形和角点顺序，另有每三角形 16 字节记录（定向几何法线、材质槽和多边形边标记）。工作室与 Lit 的光栅 pass 都以索引绘制这一份，像素着色器按 draw 的起始三角形加 `SV_PrimitiveID` 读记录，边线距离由 `SV_Barycentrics` 求得；Scene Lit 的 BLAS 按这份索引构建，射线命中按 primitive 下标读同一份索引、顶点和记录；切换显示方式复用同一份几何缓存。批次按资源在完整 `Frame::meshes` 中首次出现的顺序排列，批内保持 Frame 顺序；这个顺序只由成员表决定，不随可见性或选中状态变化。不同资源之间的绘制顺序不属于契约，但对同一份成员表稳定：完全共面且重叠的不同资源由谁覆盖、Lines: All 半透明边线在重叠处的混合，都取决于它，调用方和验收不能依赖原成员顺序。每个有可见实例的批次在每个光栅 pass 中为一次 draw：实体模式一个 pass，纯线模式为深度与边线两个 pass，Cavity 开启时叠线另加一个边线 pass；同一 draw 内按实例的 Frame 顺序、每个实例按 part 顺序画完全部三角形；两者都计入唯一的 `FrameResult::stats.scene_draw_calls`，不代表 GPU 计时。MatCap 属于场景资源，不混入 UI image/coverage 统计。

## 工作室 Cavity

`ViewSettings::cavity` 是显示设置，独立于 Material、World 与 Edit/Play。默认开启，ridge/valley 默认 1、范围 0～2，radius 默认 1、范围 0.5～4 个物理像素；DPI 不再缩放一次。MatCap、Smooth、Flat 消费它，Scene Lit 和纯线框跳过。关闭或两项强度为零时不创建目标、不编码效果 pass。

语义参考 [Blender Screen Cavity](https://docs.blender.org/manual/en/5.2/editors/3dview/display/shading.html)：强调几何凹凸，半径按屏幕像素定义，属于工作室对比度增强。实现使用视空间作者法线（Flat 用定向面法线）的有符号散度，D32 深度重建用于拒绝相隔过远的表面；不依赖材质颜色、法线贴图、三角边掩码、灯光、随机数或时间历史。平面内部与共面三角剖分为零曲率，凸边提亮、凹处压暗；屏幕之外没有补全，也不代替 AO 或真实光照。

表面直接绘入 R8G8B8A8_SRGB 颜色与 RGBA16_FLOAT 法线/覆盖 MRT，随后一个全屏 pass 合成到窗口。gfx 的隐式窗口目标不与额外 MRT 混用；这里没有原始图形 API 或额外 RHI 入口。背景、参考网格、选择/线框叠层和 UI 保持原显示职责：背景先绘制，Cavity 只合成已覆盖表面，网格与编辑叠层随后绘制。选择线与 Overlay 按需使用既有可见线管线。

有效期间保有每窗口像素 12 字节的两个目标，复用既有 D32 深度；整数半径查询四个邻居，分数半径插值两个邻域。尺寸随实际接受的 render target 更新，空场景、关闭、零强度、切换 Scene Lit/纯线框时沿 RHI 退休。旧 GPU 使用期仍由 RHI 管理。零法线或无法重建的位置保留基础颜色；参数非法在编码前拒绝。这里给出工作量和逻辑存储，不宣称已测得 GPU 耗时。

## 帧接受与失败

`Frame::ui_frame` 是帧 id、extent、DPI 和 surface revision 的唯一来源；所有输入借用到同步 `render` 返回。renderer 的 `target()` 报告实际 backbuffer 尺寸，窗口 client 尺寸可能在 resize 时领先一帧。

`Frame::render_delta_seconds` 为独立于模拟的活跃渲染间隔，有限 `[0,.1]` 秒，默认固定步进 `1/60`。零表示重建时间基线，不冻结几何失效。它只控制 DDGI 呈现低通，不改变射线预算或运输观测权重；动态宿主须按匹配 accepted 的相邻渲染开始时间产生，不能使用 Play delta 或帧内 CPU 耗时。编辑器与 Project Run 共用 `application::RenderClock`；连续慢帧限幅推进，已知停用/跳过后重建。

- 隐藏、最小化、零 client 尺寸或输入 extent 与实际 target 不符：编码前返回 skipped，capture 不变。空 scissor 不会落入 gfx 的“零尺寸代表整个目标”惯例。
- 画 Lit 的帧（Lit 着色、不是纯线框、有网格或探针叠层、viewport 非零）所需的管线组未就绪：通过 CPU 校验后、编码前返回 not_ready。不录制、不 Present，capture 和 Lit 历史不变。它不是错误，宿主可以重试，也可以改用工作室着色绘制同一帧。
- 完整 `Context::frame` 成功且 Present 未返回 `DXGI_STATUS_OCCLUDED`：accepted。`accepted_extent` 是编码前捕获的实际尺寸；gfx 在旧目标 Present 后 resize，`next_extent` 只适用于下一帧。accepted 不承诺已经完成扫描显示。
- CPU 输入校验及管线准备失败通过 `Result` 返回，通常为 `Error::terminal=false`，可重新准备帧；已有候选错误保留原终态标记。开始 GPU 创建、上传或录制后失败：terminal，后续只可关闭，不宣称回滚或继续使用上一 GPU 帧。GPU 资源拒绝保留设备错误通道；宿主 CPU 分配耗尽按基础层合同 fatal，不再捕获并伪装成 GPU OutOfMemory。
- 模式退出引起的 Scene Lit 历史失效也在 CPU 准备成功后执行；被拒绝的候选帧不改变有效 Lit 历史。接受空场景沿原 Context 退休旧场景的光栅与射线资源，可复用的 Scene Lit 目标仍由 Renderer 持有并报告逻辑字节。
- Present 之后的 resize、等待、Reset 等失败沿 RHI 终态返回，不会提前发布 accepted。遮挡是 skipped，GPU 可能已经提交；它不是 GPU 回滚。
- 应用只根据 accepted 替换完整命中数据，并负责在窗口大小/DPI 与命中帧不匹配时暂停命中、取消 press/capture；renderer 不建立第二份输入状态。

私有准备和录制链显式短路返回首个错误，不借助 C++ 异常传播。LightFrame 派生失败清空未完成快照；ShadowCache 的计划仍由 cache 拥有，未完成计划不 commit，下一次 prepare 按原规则失效。正常失败与取消均保留 RAII 清理及原 gfx 延迟退休，不建立第二套资源状态。

`close` 退休 GPU 资源、释放 Pipeline、通过原 Context 排空和关闭。重复关闭成功，Context 关闭错误保留清理入口；检测到帧槽在无录制帧或同帧二次写入、必要资源退休失败、跨线程析构等不能安全继续的生命周期损坏直接 fatal，最终析构无法关闭亦如此。

## 诊断捕获

### 每帧计数

`FrameResult::stats` 是该次 `render` 返回的 `FrameStats` 值快照。下表中的既有计数为 `uint64_t`，不增加 GPU 查询、等待或动态分配。Scene Lit 另有完成帧 GPU 计时与射线计数，具体时序、单位和内存统计边界见 [Scene Lit 诊断](scene-lit.md#诊断与性能边界)。

CPU 墙钟不进入该快照，而是记录为[帧阶段计时](../../foundation.md#帧阶段计时)的 `render.*` 阶段（标记见 [renderer/timing.hpp](../../../modules/render/renderer/include/renderer/timing.hpp)），只在宿主绑定 Recorder 时生效：`render` 覆盖整次调用；`render.prepare` 覆盖 Frame 校验、相机、实例、可见性与按资源分组的绘制索引表准备、UI 准备；`render.encode` 覆盖显示模式失效处理及场景/UI 绘制命令录制，其中 `render.encode.scene` 覆盖有场景时的全部录制（子阶段：`.materials` 为材质表与贴图的创建和上传；`.upload` 为逐批的几何缓存使用标记（静止帧也不为 0）、未命中时焊接并上传索引几何、绘制索引表和实例缓冲写入本帧槽（每个在途帧一份持久的 CPU 可写缓冲，GPU 直接读取；按录制帧序号选槽，容量不足时才按 2 的幂重建），实例缓冲在 Lit 模式只在需要编辑深度或编辑线时才写；`.draws` 为各 raster pass 逐批绘制的录制，只有工作室模式（MatCap、Smooth、Flat、线框）才能用它除以 FrameStats 的 `scene_draw_calls` 估算每次绘制的平均开销，因为 Scene Lit 的表面绘制计入 `scene_draw_calls`，耗时却记在 `.lit`；`.lit` 为 Scene Lit 的 draw 和 draw_debug，含其内部的灯、实例上传与 AS 构建），`render.encode.ui` 覆盖 UI 贴图表的建立（含新版本贴图上传）、实例写入本帧槽（子阶段 `.upload`，与绘制索引表同一种帧槽）和叠加层、UI 各至多一次 draw 的录制；同一阶段在一帧内可进入多次，时间累加、次数另计。无场景时的网格与 Scene Lit 调试绘制只计入 `render.encode`。`render.frame` 覆盖 `Context::frame`，其中由 RHI 测得的分段记为子阶段 `render.submit`（提交前准备与提交）、`render.present`、`render.wait`（下一帧槽等待）和 `render.recycle`（resize、回收与 reclaim），字段定义以 [RHI 合同](../rhi/contract.md) 为准。提前 skipped 的调用只有 `render`；not_ready 另有 `render.prepare`。墙钟包含阻塞与线程调度，不等于 CPU 活跃执行时间，也不与 GPU 时长相加。

| 字段 | 统计边界 |
| --- | --- |
| `ui_draw_calls` | 实际成功编码的 UI draw 次数：有可见 UI 实例时为 1，否则为 0；不含视口叠加层（`overlay_draw_calls`） |
| `scene_draw_calls` | 实际成功编码的场景 draw 次数；同网格资源的可见实例共享批次，不论在 Frame 中是否相邻。每批在每个光栅 pass 中一次（pass 数见[运行状态与着色模式的独立性](#运行状态与着色模式的独立性)），空场景为 0 |
| `scene_instances` / `scene_culled_instances` | 本帧输入实例数 / 主视锥判定完全不可见的实例数；不是 TLAS、光源或阴影投影者的裁剪数，也不包括物体之间的遮挡 |
| `material_upload_count` | 本帧新场景图片上传次数，包括首次使用的白图；复用版本不上传，不含 UI 图片 |
| `cached_material_images` | 当前场景 Image/颜色空间缓存项数，不含 UI 图片或 gfx 等待退休的资源 |
| `viewport_draw_calls` | 实际成功编码的视口辅助 draw 次数；渐变一次，网格与轴线共用一次；不计入物体或 UI draw，不代表 GPU 时间 |

编码前 skipped 与 not_ready 的活动计数为 0，场景图片缓存计数反映仍被持有的资源。原生遮挡发生在编码之后，skipped 仍报告已编码活动。unexpected 失败不返回完整统计；原有接受状态与终态规则不变。缓存计数不包含已交给 gfx 等待 GPU 退休的资源，也不包含几何、深度缓冲与上传暂存。每帧活动计数重置，场景图片缓存项数随现有插入/退休动作更新，未改变淘汰策略。

应用可在正式交互中测量 prepare/render 的 CPU 墙钟时间；render 时间包含 Present 或必要等待，不能称为 GPU 时长。使用同一 `FrameResult` 关联活动计数，显式 Capture 另行记录，避免把它的同步成本混入普通交互样本。

### 显式图像读回

`render(frame, &capture)` 显式请求 GPU 同步，返回本帧目标的紧密 sRGB RGBA8 和 float32 深度，行自上向下；Scene Lit 帧另返回 GBuffer 的逐像素表面运动（前帧减当前窗口 UV，两个 float32），其他着色模式为空。仅在 accepted 时替换输出。UI 不写场景深度；深度遵循上述 Reversed-Z 合同，背景为 0。普通 render 不调用 finish；诊断捕获不应放入交互帧热路径。

TAA / SMAA T2x 的采样深度只用于内部光照重建与历史。需要编辑辅助显示或 Capture 时，AA 完成后复用现有深度通道重建公开 Camera 对应的无抖动 D32 深度，再绘制网格与选中边；这次绘制计入 `scene_draw_calls`。没有这些消费者时省去重绘。

本帧没有场景绘制时，capture 的深度数组直接填充为 0，不创建或读回 GPU 深度资源，也不返回之前场景残留的深度；颜色仍从实际 backbuffer 读回。场景首次创建失败沿既有 GPU 阶段终态规则处理。

捕获命令经现有 gfx 录制，使用 256 字节对齐的 GPU 行距，完成后拆成紧密 CPU 行。读回源是 resize 前真正绘制的目标。没有原生队列逃生口，也不改变原有紧密 texture copy 入口。

## 场景材质与贴图

标准表面的权威合同见[材质系统](materials.md)。Mesh 保存 UV、MikkTSpace 切线、命名材质槽及覆盖全部三角形的连续 MeshPart；World 捕获按槽解析后的不可变 Surface。材质、贴图沿现有 Runtime 对象身份与 Project 资源身份，渲染帧只拥有 CPU 内容，不持有业务对象或磁盘路径。

颜色与发光图使用 sRGB，粗糙度、金属度、法线及透明度图使用线性采样。共享材质表供工作室、Scene Lit GBuffer、射线命中与编辑深度使用；Mask 在这些路径使用同一覆盖公式。MatCap 继续以几何法线选择 MatCap 颜色，同时遵循表面覆盖；显示模式不改写资产。

GPU 图像缓存以不可变 Image owner 与颜色空间为键并持有 CPU 所有者；未使用版本随帧退休，关闭时沿原 Context 合同释放。稳定帧复用材质表与纹理，材质参数修改不重建网格；Opaque/Mask 变化重新核对 BLAS 透明性。`material_upload_count` 为本帧新增图片上传数（包括首次白图），`cached_material_images` 为当前场景图片缓存项数；不含 gfx 等待退休的资源。不增加原生设备/队列或帧内磁盘读取。

源顶点为 48 字节，焊接后的顶点缓冲沿用这一布局；光栅与射线共用的每三角形记录为 16 字节，携带几何法线、材质槽和编辑边信息。Scene Lit 另为每份几何保存每三角形 4 字节的连通分量闭合信息，含多个材质槽的闭合分量在同一缓冲末尾列出各槽。法线图不改变几何闭合、深度或射线起点偏移。没有绑定材质、也没有网格默认材质的槽，组件颜色与标量参数通过同一材质表解析。
