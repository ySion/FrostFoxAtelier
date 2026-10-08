# Scene Lit 首次方案：直接光、DDGI、GTAO 与 SSSR

研究日期：2026-10-08。状态：方案调研，尚未实现或测量 Scene Lit。产品目标为 low-poly、上帝视角 roguelike，要求点光源、面光源、方向光，以及鲜明且稳定的光照表现。以下将外部资料确认的事实与项目设计建议分开；最低显卡、输出分辨率、帧率目标与战斗时灯光负载尚待确定。

后续用户已选定以本机显卡为开发基准。RX 6800 XT 的硬件核对、Exa 工程源码深查与最新第一批建议见 [RX 6800 XT 批次方案](scene-lit-rx6800xt-batch1-20261008.md)；本文保留原始算法边界和硬件决定前的分阶段分析。

## 建议结论

**SSSR + DDGI + GTAO 可以组成有价值的间接光补充，但首次 Scene Lit 应以完整的直接光与阴影为中心。** 建议交付顺序为：统一材质与灯光、HDR 和阴影 → GTAO → DDGI → SSSR；ReSTIR DI 保留为直接光采样的后续升级点。此顺序是根据当前游戏方向作出的工程判断，不是算法性能排名。

low-poly 并不意味着光照廉价：大块面片和地面容易暴露阴影闪烁、法线错误、降噪拖影及漏光。对这类游戏，优先保证角色受光、彩色灯光衰减、遮挡关系和爆发效果的即时响应；如果材质以粗糙石材、木材为主，DDGI 的颜色反弹通常比镜面反射更值得先做。如果水面、金属和湿地是主要美术语言，则提高 SSSR 的优先级。这里是美术目标推导，仍需代表性关卡验证。

Scene Lit 与 MatCap 均是渲染选择；Edit/Play 独立决定模拟与交互。背景、网格也保持独立。不能以是否运行模拟决定使用哪种光照，也不能把未来 Scene Lit 写成只属于 Play 的另一条场景入口。

## 四项技术负责什么

| 技术 | 提供的信号 | 关键输入与状态 | 不替代的能力 |
| --- | --- | --- | --- |
| SSSR | 屏幕空间镜面间接反射 | 深度层级、法线、粗糙度、HDR 颜色；配套降噪使用运动矢量与历史；环境反射回退 | 直接光、阴影、完整的屏幕外反射 |
| DDGI | 世界空间漫反射间接光 | 场景射线查询、命中点材质与直接光、探针辐照度及距离缓存、更新预算 | 主表面直接光、锐利镜面反射、几何精确的所有遮挡细节 |
| GTAO | 局部间接遮蔽 | 深度；可选显式法线；深度预过滤和空间降噪；时间累积因实现而异 | 点光源或面光源的投影阴影、颜色反弹 |
| ReSTIR DI | 从大量候选灯光中高效采样直接光 | 材质/PDF、灯光记录、当前/历史表面、reservoir、可见性及降噪 | 灯光语义、BRDF、场景可见性实现、完整渲染器 |

表中算法边界依据 [AMD SSSR 手册](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)、[DDGI 原论文](https://research.nvidia.com/index.php/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields)、[GTAO 原始技术报告](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf) 与 [ReSTIR 原论文](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/)。

### SSSR：有回退的反射，而不是全场景追踪

AMD 实现以相机深度层级寻找交点；未命中或过于粗糙的像素使用预过滤环境立方体和 BRDF LUT。它不能从单层相机深度恢复屏幕外或被前景遮住的物体。算法本身可以不使用时间信息，但实际稳定输出依赖配套反射降噪器的运动矢量。标准接入文档要求输入当前帧直接光 HDR；应先定义反射源包含哪些光照分量，避免把最终反射或 UI 再送回自身。[SSSR 输入、回退与降噪](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)

设计建议：先提供环境反射，SSSR 在可信命中处补充细节。不能将 DDGI 的漫反射辐照度当作清晰镜面反射缓存。面光源若要在水面上呈现灯具形状，还需明确可见发光表面的几何/材质表达；单有一个用于直接照明的解析灯记录不足以保证该形状进入屏幕反射。

低俯角、正交与透视镜头均应进入实际验收；尤其观察相机移动时屏幕边缘反射消失、薄墙漏射和角色遮挡地面的跳变。固定上帝视角减少部分变化，但不消除这些限制。

### DDGI：适合动态关卡，但前提是能给探针正确的入射光

RTXGI 的探针更新要求应用维护射线查询所用场景，处理命中/未命中，在命中表面计算直接光，再查询已有探针获得多次反弹贡献。SDK 并不会自动提供完整场景材质与直接光系统。采样时会结合距离信息过滤被遮挡探针，查询使用几何法线。[RTXGI 集成契约](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Integration.md)

DDGI 是探针与可见性缓存的算法选择，并不在定义上等同于 DXR。官方 RTXGI-DDGI 实现面向 GPU 光线追踪 API；另有以 SDF 场景近似计算动态漫反射 GI 的研究。这说明追踪后端可以变化，但自建 SDF、体素或软件 BVH 不是免费的兼容层，也不建议为首次 Scene Lit 同时开发两种场景追踪体系。[官方实现要求](https://github.com/NVIDIAGameWorks/RTXGI-DDGI)、[SDF 动态漫反射 GI 论文](https://arxiv.org/abs/2007.14394)

官方实现提供探针 relocation、classification 与 scrolling volume。滚动时仅新进入边缘的探针平面需要失效重收敛，内部探针保留；更新频率也可以低于显示帧率。[DDGIVolume 参考](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/DDGIVolume.md) 生产扩展论文专门研究偏置、光照变化响应、无效探针工作剔除与多分辨率体积，不能据此承诺所有薄墙和窄空间天然无漏光。[生产扩展论文](https://arxiv.org/abs/2009.10796)

设计建议：首个场景先用覆盖战斗房间及外围遮挡物的有限探针体积，再依据地图组织决定是否滚动；不要一开始铺满无限世界。预算按活跃探针数、每探针射线数、更新频率与命中点着色成本计算。短促爆炸的可读性应由即时直接光保证，不能依赖多帧累积的 GI 才显现。移动门、薄墙、房间切换、灯光开关及加载后的收敛必须成为验收内容。

### GTAO：补近处细节，避免把所有光一并压黑

原始 GTAO 报告把 AO 定位为间接遮蔽近似，并讨论局部多次反弹导致的能量恢复；屏幕空间深度只是场景的高度场近似。[GTAO 技术报告](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf)

XeGTAO 是可复用的 HLSL 实现：深度预过滤、主体计算、空间降噪三个阶段；法线输入可选。它可在没有 TAA 的引擎中使用，TAA 存在时再利用时间抖动。仓库为 MIT，已于 2024-04-22 归档，不能将其作为仍由 Intel 持续维护的依赖。[XeGTAO 实现与维护状态](https://github.com/GameTechDev/XeGTAO)

设计建议：AO 只调制合适的间接光分量，保留直接光和自发光。DDGI 已编码部分可见性，再给大范围 AO 会重复变暗；组合时先用短半径、弱强度，比较接触处细节和整体能量。AO 不能代替真正的灯光阴影。可保留调试输出分别查看直接光、DDGI、AO 和最终合成，以便判断问题来自哪个信号。

### ReSTIR DI：值得保留的升级点，首版是否启用取决于真实灯光负载

ReSTIR 的主要收益来自把大量候选灯光的直接光积分转化为少量高价值样本，并在时间与邻近像素之间复用样本。论文示范大量动态发光三角形；这些结果不意味着本项目只要启用该算法就能获得固定倍数加速。[ReSTIR 原论文](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/)

RTXDI runtime 本身不调用图形 API，由应用提供资源、光源表、材质、G-buffer 和射线查询桥接。时间复用需要当前与上一帧灯光数据及索引映射，不能在删除/重排灯光后让旧 reservoir 指向新灯。最终采样仍要计算可见性和 BRDF，并进行降噪。[RTXDI Integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)

目标 PDF 与最终 BRDF 不匹配会增大噪声；历史不足、遮挡显露以及可见性复用可能产生噪声或偏差。最严格的时间偏差修正还涉及上一帧表面、灯和可见性数据，不能只保存一个 reservoir 便宣称无偏。[RTXDI Noise and Bias](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/NoiseAndBias.md)

设计建议：先用同一灯光/材质模型完成确定性的直接光路径。若战斗实测出现大量重叠动态灯或发光面，直接光/阴影成本成为主要瓶颈，再比较 ReSTIR DI。不要只用“场景总灯数”作阈值，还要看可见区域重叠、屏幕覆盖、遮挡、亮度差异与动态程度。暂不同时引入 ReSTIR GI/PT，避免与已选 DDGI 重叠扩大范围。

## 接入实现与版本选择

| 候选 | 本次核实的版本/状态 | 接入判断 |
| --- | --- | --- |
| AMD SSSR | 官方手册 1.5；SDK `v1.1.4` 的头文件明确为 SSSR `1.5.0`，该版本 SDK 许可为 MIT | 可固定到已核实版本，按模块引入所需 shader/host 代码；不要笼统跟随最新整个 SDK |
| XeGTAO | MIT；Intel 已停止维护并归档 | 代码量与接口较集中，适合在自身渲染路径调度，维护责任由项目承担 |
| RTXGI-DDGI | DDGI 专用仓库 README 为 `1.3`；NVIDIA RTX SDKs 许可 | 优先研究 shader 与算法数据契约；官方 D3D12/Vulkan host 包装不能成为越过项目 RHI 的入口 |
| RTXDI | SDK README 为 `3.1.0`；可集成 runtime 为单独 `RTXDI-Library`；该库同样为 NVIDIA RTX SDKs 许可 | 可只选 DI 相关实现，通过应用 shader bridge 连接；示例依赖 Donut/NVRHI 不代表项目需要引入第二套 RHI |

版本与许可证据：[SSSR 固定版本头文件](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/include/FidelityFX/host/ffx_sssr.h)、[该 SDK 版本许可](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/LICENSE.txt)、[XeGTAO](https://github.com/GameTechDev/XeGTAO)、[RTXGI README](https://github.com/NVIDIAGameWorks/RTXGI-DDGI)、[RTXGI 许可](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/License.txt)、[RTXDI README](https://github.com/NVIDIA-RTX/RTXDI)、[RTXDI runtime 许可](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/main/LICENSE.txt)。后两者不能标成 MIT；采用源码时应保存所固定版本的许可和第三方 notices。

AMD 当前仓库 main 的产品包名称与版本已变为 FSR SDK 2.3.0，说明“SDK 版本”和“SSSR 效果版本”不可混用；这不是建议为 SSSR 升级整套依赖。[当前仓库 README](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)

对本项目，所有 GPU 资源、调度、屏障和寿命均经过现有 RHI。SSSR 可以适配其 modular backend 或接入效果 shader；RTXGI 的 API host 示例只能作为参考。RHI 真正缺失的能力交给 RHI 负责人补齐，Renderer 不直接调用原始 D3D12/Vulkan。此处是用户已明确的项目边界。

## 合成时只计算一次同一份光

建议明确保留直接光的漫反射/镜面分量、漫反射间接光、镜面间接光及自发光的语义，而不是让各效果对最终颜色随意相加：

- DDGI 与 GTAO 的组合只补需要的间接遮蔽，避免与探针已有可见性重复压暗。
- 环境反射作为 SSSR 的回退，与可信屏幕命中按一致权重混合；不能完整环境反射与完整 SSSR 再各加一遍。
- 如果解析面光源已计算该灯对表面的镜面直接光，SSSR 又命中代表同一灯的发光面，要确定其归属，避免同一直接镜面贡献重复计算。SSSR 命中普通受光物体产生的反射则是另一类间接传输，不能一概删除。
- LTC 与 ReSTIR DI 可以分别服务不同灯光集合或不同路径；同一灯的同一直接光贡献不能两种方法无条件叠加。

这些是拟定的项目合成约束，落地时用明确分量和灯光归属解决；不因此增加一套通用采样框架。

## 分阶段交付建议

1. **Scene Lit 基础闭环。** 同一场景与材质入口支持点光源、矩形面光源、方向光、实际遮挡、内部 HDR 与显示映射；保持 low-poly 几何法线和可控粗糙度。矩形面光源的形状积分与遮挡分别解决，不能将无阴影的亮度效果当成完整面光源验收。具体方案见下方三类灯的范围。
2. **GTAO 与画面稳定性。** 接触细节和抗锯齿先形成可控基线；为时间效果建立明确运动矢量、历史失效与重投影契约。不要为了 GTAO 单项强制建设全部时间系统。
3. **DDGI。** 若最低硬件允许硬件 RT，优先走已选 RHI 的正式追踪能力；若要求无 RT 硬件，先确定可接受的间接光回退，再决定是否值得承担软件追踪建设。第一间代表性房间证明颜色反弹、遮挡、动态更新和成本后扩展。
4. **SSSR。** 在粗糙度、环境反射、HDR 反射源和时间历史已经正确时接入；用水/金属/湿地等实际内容证明收益，控制与整体战斗画面争夺注意力的问题。
5. **ReSTIR DI 选择性评估。** 以直接光和阴影的测量瓶颈及大量重叠光源需求作为触发条件，保留相同的灯光、材质、场景身份和 Render 入口。

这不是宣称阶段 1 已覆盖所有游戏最终效果。体积光、透明 VFX 受光、Bloom 等可能影响“强光表现”，但应由美术目标单独确定优先级；它们不是 SSSR/DDGI/GTAO 自带能力，也不自动进入本轮实现范围。

## 当前代码与接入边界（父级源码核对）

核对对象是 Renderer 专属工作树 `codex/viewport-rendering`，基线 `402fab075bb2aa6605b51fdb767387ebd0e08869`；这描述当前代码，不代表本文方案已经实施。

| 当前事实 | 对 Scene Lit 的影响 |
| --- | --- |
| [World Material](../../src/world/world.hpp) 只有 `base_color`；`RenderItem` 已带 Actor/Component 身份、最终世界矩阵和共享几何 | 灯光组件、材质粗糙度/金属度/自发光以及场景提取需要正式扩展 World 与既有作者入口 |
| [Geometry](../../src/geometry/mesh.hpp) 顶点目前只有 position/normal | 首版可采用常量材质值，保留 lowpoly 硬边；纹理、法线贴图与任意网格发光不应被算成已有能力 |
| [Renderer::Frame / MeshInstance](../../src/renderer/renderer.hpp) 只有相机、网格、基础色、选择和显示策略；无灯光、前帧变换、实例历史键 | 时间重投影、动态对象 motion vectors、ReSTIR light remapping 都需要真实数据合同，不能用数组下标或几何指针充当实例身份 |
| [scene_ps.hlsl](../../src/renderer/shaders/scene_ps.hlsl) 的 Smooth/Flat 使用固定方向的基础照明；[Renderer](../../src/renderer/renderer.cpp) 直接绘制 SDR sRGB backbuffer | 当前不是完整光照系统；需新增线性 HDR 场景中间目标和最终色调映射，而不是在当前像素结果上叠三个后处理 |
| [RHI 公开头](../../src/rhi/rhi.hpp) 与[当前合同](../modules/rhi/contract.md) 正式覆盖 Vertex/Mesh/Compute；ContextInfo 无正式 RT 能力报告 | 有 Compute 基础；RT 能力、加速结构使用和生命周期由 RHI 负责人确认/验收 |
| [固定 gfx 来源](../../cmake/dependencies-lock.json) 是 `e039b255efb327994d30b187caee400ca60110e0` | 本地固定源码已有 `gfxIsRaytracingSupported`、BLAS/TLAS 构建更新、AS shader binding、MRT/纹理 mip、间接 dispatch 和 timestamp；不能把“尚未验收”写成“整个底层没有这些功能” |

固定 gfx 的初始化还会检查 DXR Tier 1.1。能力函数/源码存在不等于项目产品路径已经验证；接入先复用当前机制，再由 RHI 补真实缺口。当前 `Context::open` 已要求 FL12_1 与 Mesh Shader，选择“无硬件光追路径”也不自动扩展到全部旧显卡。

### 建议的最小架构

继续一条数据与执行链：

```text
World / 同一作者系统的灯光和材质
  → 场景提取：不可变资源 + 最终变换 + 灯光值 + 实例/灯光稳定身份
  → Renderer::render：同一个 Frame，按 ViewSettings 选择着色
      ├─ MatCap / Smooth / Flat
      └─ Scene Lit：几何 → 直接光与阴影 → 间接光/反射 → HDR 合成
  → 色调映射后的场景 + 视口辅助线/网格 + UI
  → 同一 RHI Context 提交与 Present
```

对于已明确要组合 AO、SSR、DDGI，且后续考虑 ReSTIR 的不透明主场景，建议 **紧凑 Deferred 路径**：一次几何阶段产生 depth、normal、base color、roughness/metallic 及后续时间效果所需 motion；世界位置由深度重建。发光信号按实际材质存储需求安排，不先冻结一套最大 G-buffer。灯列表先做屏幕 tile / 深度分层的局部筛选，避免每像素遍历全世界灯。Clustered 可用于 Forward 或 Deferred，和 ReSTIR 也不是互斥选项；本次选择 Deferred 是项目组合需求的判断，不是对所有 lowpoly 游戏的普遍结论。[Clustered Shading 原论文](https://www.highperformancegraphics.org/previous/www_2012/media/Papers/HPG2012_Papers_Olsson.pdf)

首版不透明灯光闭环先完成。未来半透明粒子可使用同一灯/材质规则的 Forward 阶段，不复制另一套灯光语义。无需为这次功能先建设通用 RDG、材质节点图、第二设备或第二资源管理器；固定 pass 顺序与清楚的中间资源拥有者足以启动。

- **World / 应用**负责灯光组件、序列化、编辑/撤销、最终变换和场景身份。方向光、点光、矩形面光使用同一真实组件/属性链；UI DSL、Source、独立运行消费相同数据。渲染器不持有 GC 根、Runtime 对象或 JIT 回调。
- **Renderer**负责材质求值、灯光筛选、阴影/探针调度、时域历史和 GPU 渲染资源；已有几何资源缓存继续共享。稳定实例键来自已有权威对象身份的提取，不再发明第二套业务对象注册表。
- **RHI/gfx**负责能力边界、加速结构/纹理/缓冲的执行与退休；不让第三方示例接管原生设备、queue、barrier 或 Present。Inline RayQuery 可由 Compute shader 使用，简单不透明可见性不必首先引入独立 ray-generation/hit-group 管线；具体选择仍需目标机验证。[Microsoft DXR 1.1](https://devblogs.microsoft.com/directx/dxr-1-1/)
- **ViewSettings**只选显示方式与质量策略。Edit/Play 决定场景更新，Scene Lit/MatCap 决定呈现，两者正交；编辑状态同样允许持续更新 GI。暂停模拟时仍可推进渲染历史。
- **时域状态**按视图保存，至少能区分场景、更换相机、viewport 尺寸与模式切换。相机切换、瞬移、resize、历史不兼容、场景重建后失效；快速灯变化要缩短历史响应。DDGI 探针描述世界空间，不跟随每一帧屏幕像素重建；多视图跨 Context 共享 GPU 缓存不属于首版。
- **帧接受**沿现有合同：UI 命中数据仍只接受 accepted。GPU 已执行但 Present occluded 的 skipped 不能误当作“未执行”；恢复时使屏幕历史失效是首版可审查的保守策略。不要另建一套提交计数或改写 UI 的帧身份。
- **显示空间**：场景使用线性 FP16 HDR 中间结果，最终映射到现有 SDR sRGB 窗口。此处 HDR 是内部光照动态范围，并不要求立即支持 HDR 显示器。UI、编辑网格和选中线不进入光照/SSSR/DDGI/TAA 反馈，不随曝光变暗或参与反射。

当前 Guides 与 Scene Lit 的独立性继续遵循[已定 Renderer 合同](../modules/renderer/contract.md#运行状态与着色模式的独立性)。世界天空/环境是场景内容，不能被最后绘制的编辑渐变覆盖。

### 三类灯的首版明确范围

本文把用户的“直射光源”理解为 **Directional Light（方向光 / 太阳光）**。

| 类型 | 建议首版 | 阴影和边界 |
| --- | --- | --- |
| 方向光 | 方向、颜色、照度；优先主光稳定性 | 覆盖活动房间/相机可见区的稳定 shadow map；实际地图跨度需要时再使用 cascades，不预建大世界系统 |
| 点光 | 位置、颜色、光强、合理范围与近距离控制；用于火把、法术、爆炸 | 优先稳定 atlas/cubemap 阴影和明确的更新预算；不能承诺所有瞬时灯每帧六面满分辨率更新 |
| 面光 | **矩形面光**、长宽、朝向、单/双面发射与明确单位 | 无遮挡 BRDF 积分可用 LTC；遮挡另算。RT 路径用面积采样可见性与降噪；无 RT 时必须明确是阴影近似，不把矩形中央一张 shadow map 冒充准确面积软阴影 |

LTC 能给出随灯的尺寸和表面粗糙度变化的面积高光，是实际面光求值方案；它本身不包含遮挡求解。多边形之外的任意发光网格、纹理光、IES、聚光灯属于后续可选范围，不把它们塞进首个完整增量。[LTC 作者论文页](https://eheitzresearch.wordpress.com/415-2/)、[作者参考代码](https://github.com/selfshadow/ltc_code)

材质先采用 base color、roughness、metallic、emissive 这组小而完整的值；保持几何硬法线，不通过全局平滑换取“高级渲染”。采用简洁、能量一致的 diffuse + GGX 响应并不要求复杂贴图或写实美术。单位与曝光在作者入口定义一次：例如点光 candela / 可换算 lumen，方向光 lux，面光按面积与亮度一致换算，避免调灯尺寸时亮度语义漂移。[Filament 光照与材质设计](https://google.github.io/filament/Filament.md.html)

### 对这个游戏的画面判断

以下是针对题材的设计推断，不是已测效果：

1. 俯视角里角色体块、脚底接触、障碍阴影与技能范围比到处反光更直接影响可读性。把主光方向、明暗分组、彩色局部灯、软硬适中的阴影和受控 bloom 做好，再判断 SSR 的实际收益。
2. lowpoly 降低部分几何成本，但不会自动降低全屏 AO/反射/降噪、局部灯重叠、探针更新或透明特效的成本。大面积平坦墙地也容易暴露噪声、漏光和时域拖影。
3. 程序化房间、关门开门、快速闪光和新刷怪是 DDGI/时域光照的关键场景。静态截图漂亮不代表游戏运动中合格。
4. 镜头若使用正交或很窄 FOV，必须专项核对深度重建、AO 半径、SSSR 射线与降噪；不能默认第三方透视 shader 公式适用于正交。
5. 发光材质、真正照亮附近的灯、bloom 和体积光是不同职责。首版可先固定/受控曝光与 bloom；体积雾应由确切美术目标触发，不视为本轮必需算法。

### 成本与可验收终点

硬件最低目标尚未确定，本文不承诺具体毫秒数或普适灯数上限。1080p/60 FPS 如被选定，整帧预算约 16.67 ms，渲染预算还必须给游戏、物理与其他工作留出余量。

用可解释的量先约束规模：一个 1920×1080 的 RGBA16F 逻辑图面约 15.8 MiB；多张 history、G-buffer 与降噪暂存会叠加，这不是完整物理 VRAM 测量。DDGI 例如 24×6×24=3456 个探针，每探针 64 条射线意味着全更新 221184 条探针射线，**还没计命中处灯光遮挡与其他射线**；这是算术示例，不是推荐默认值。先按房间尺度/墙厚/高度选布局，再按更新预算实验。

以同一真实小型关卡验收：太阳/方向光、几个火把、一个矩形面光、金属/粗糙物体与可选水面；随后增加动态灯重叠、开门/关灯、角色穿过薄墙邻近区域、快速相机移动、刷怪/移除/重建场景。核对面光背面、光源大小、DDGI 漏光与响应、GTAO 过黑、屏幕外反射 fallback、无鬼影的运动、无过曝洗白，以及 Edit/Play × MatCap/Scene Lit 的独立性。开发/发布配置都通过对应端到端；记录逐 pass GPU 时间、显存、AS/探针更新成本和稳定画面，不能拿 draw-call 计数或同步 Capture 耗时充当 GPU 基准。ReSTIR 的开关决策依据相同画质下实测的直接光总成本与噪声，而非单独一个 shader 的耗时。

已于 2026-10-08 按用户既有授权通知 RHI 负责人：这是研究中的能力需求，当前不改代码；待方案和目标硬件确定后，先确认现有 gfx 能力，再补 RHI 的实际缺口。本轮仅添加调研文档，没有实施 Scene Lit、接入第三方 SDK、改动构建或运行验收。
