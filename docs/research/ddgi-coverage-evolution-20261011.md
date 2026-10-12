# DDGI 覆盖、更新预算与调试可视化研究 — 2026-10-11

状态：以下研究记录对应实施前的源码核查与一次只读现场观察，研究阶段未修改生产代码、未运行 GPU 验收。后续调试基础设施的已实现合同见 [DDGI 诊断](../modules/renderer/scene-lit.md#ddgi-空间与接收面诊断)，验收结果单独记录。截图黑区尚未完成逐像素或开关对照归因。依据 NVIDIA 官方 DDGI SDK、原作者论文和有限的 AMD／Epic 对照；不以厂商演示帧时推算 RX 6800 XT 性能。

**结论：DDGI 的有限空间覆盖是缓存组织的正常属性，但突然变暗、始终未就绪、探针活跃却无实际贡献是不同问题。下一步最有价值的是让同一套 DDGI 查询同时解释 volume、probe 与 receiver 的状态，再按真实视野需求加入有预算的滚动与少量分层覆盖。** 对俯视 lowpoly 场景，可以接受漫反射光照低频与延迟收敛，不能把这种取舍扩展为历史身份错误、边界断层或静止闪烁。

## 0. 本地实现与当前现场核查

源码基线为 `72843f7a24d51542620f5bebedb83433cfca914b`。通过既有 Control 读取用户正在运行的同版本 Release 实例；场景为 `Examples/Dungeons/Scenes/DungeonOverview.scene`，627 个 Actor，文档有未保存修改。本轮没有保存、重开、关闭或修改该场景。原始只读记录在本机 `out/ddgi-evolution-inspection/editor.inspect.json` 与 `capabilities.json`，不随源码提交。后续 Control 读取在原生未保存场景模态对话框期间超时，应用负责人确认此时 Application 主循环暂停，因此未发起计划中的新捕获；不以旧截图加参数推断每个像素的确切成因。

| 当前值 | 实际观察 |
| --- | --- |
| Volume bounds | X `[-20,19]`、Y `[-2,15]`、Z `[-13,12]`，即 39 × 17 × 25 米 |
| Probe grid | 16 × 10 × 14 = 2240；间距约 2.6 × 1.889 × 1.923 米 |
| 更新预算 | 560 probes/frame、128 illumination rays/probe，另有 32 条固定几何射线；全格轮转需 4 帧，收敛仍可能需要多轮 |
| 相机关注点 | `(19.1705,-0.3826,8.8379)`；X 已超过该 volume 最大值，但 camera target 不是截图墙面的逐像素命中位置 |
| 光照 | 一盏投射阴影的方向光，有限角半径 0.05；环境 radiance 为 `(0,0,0)`；DDGI、Local GI、SSSR 开启，GTAO 关闭，AA 为 SMAA |
| Debug | `probe_debug=false`；Control 能力表未提供 `debug.*`，已有 display patch、render capture/compare 入口 |

基线源码已确认的限制（调试相关条目描述实施前状态）：

- [SceneLitSettings](../../src/renderer/view_settings.hpp) 仅描述一个 `probe_min/max/counts` 体积。[SceneLit::validate](../../src/renderer/scene_lit.cpp) 限制每轴 2～32、合计不超过 4096；改变 bounds/counts 会触发探针场重置，因此不能把每帧改 bounds 误当成保留历史的滚动。
- [ddgi_irradiance](../../src/renderer/shaders/lit_ddgi_common.hlsl) 对偏移后的接收位置先做 grid 边界检查，越界直接返回零。体积内仍会因探针未就绪、inside、无有效方向照度或真实 LOS 被挡而没有贡献；需要逐项诊断。当前没有粗层回退、级联或边界过渡合同。
- 基线 shader 的 `probe_debug` 是接收表面的最近探针伪彩：绿色可用、红色 inside、橙色未就绪/重定位、蓝色在体积外。它没有绘制空间球、volume 线框或真实八邻贡献，不能称为完整 probe debug。
- [Application::configure_view](../../src/application/application.cpp) 只在显式 `fit_ddgi_volume` 时根据几何边界拟合；目标约 2 米格距、每轴最多 16，并非持续覆盖管理。[open_example](../../src/application/project.cpp) 对 Dungeons/Physic 默认使用 Smooth 且关闭 DDGI，不拟合该体积。现场启用 Lit/DDGI 后保留的显示参数，不能据此声称该场景已经过大世界 GI 自动布置。
- [Compose](../../src/renderer/shaders/lit_direct.hlsl) 在 DDGI/Local GI 路径使用间接辐照度结果；当前环境为零，不会产生天光填充。覆盖之外的阴影面因此具备变黑的条件。具体黑墙还需要 coverage/支持诊断或临时对照确认，不能排除几何/探针支持等其它原因。

一次有效完成 GPU 样本（sample frame `135465`，sequence `76934`；观察 frame `135468`）给出：直接光/去噪 5.269 ms，DDGI 总阶段 1.089 ms，其中 update/transfer 0.393 ms、receiver gather 0.696 ms；GTAO/LocalGI 阶段 3.213 ms，HDR/SSSR 阶段 2.286 ms。receiver gather 有 3,233,833 次 visibility 查询，probe-hit shading 有 25,781 次，主 probe rays 为 89,088。它是单个现场样本，不能充当 p50/p95、FPS 或算法比较；但足以提醒后续统计应区分接收查询、probe 更新与直接光重建，不能把开销都归给探针数。

本次黑区应先完成覆盖诊断；ReSTIR DI 的主要目标是随后大量局部灯/发光面的直接光采样与阴影扩展，不负责补全缺失的 DDGI volume。详见 [DI 与阴影研究](restir-di-shadow-evolution-20261011.md)。

## 1. 研究对象和版本边界

- 本文 DDGI 指 `NVIDIAGameWorks/RTXGI-DDGI` 的 irradiance probes + distance moments。官方 main 在本次核对时为 **`f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6`**，提交时间 2024-02-05；下文源码链接锁定此版本，不将网页抓取时间当成算法发布日期。[核对提交](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/commit/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6)。
- 新 `NVIDIA-RTX/RTXGI` 仓库主角是 **NRC 与 SHaRC**，属于路径追踪中的 radiance cache；其 README 把 DDGI v1.x 明确指向独立仓库。NRC 需要 Tensor Cores，SHaRC 不具有同一厂商限制。它们不是给 DDGI 增加两个 volume 的版本升级，也不能据 NVIDIA 演示推定 RX 6800 XT 的适用性能。[RTXGI 当前 README](https://github.com/NVIDIA-RTX/RTXGI/blob/main/Readme.md)。
- 原始 DDGI 论文定义的是带可见性插值的漫反射照度场；光滑反射另有 glossy 路径。由此不能把 DDGI、SSSR 与直接光当成三个可任意互换的“GI 开关”。[Majercik 等，2019](https://research.nvidia.com/index.php/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields)。

## 2. 三个必须分开的空间概念

| 概念 | 官方实现中的意义 | 对本项目的设计判断 |
| --- | --- | --- |
| Volume 覆盖 | 一个世界空间区域及其规则探针网格；其数量、间距和位置控制可查询范围。 | 必须显示原点、轴向尺寸、间距、边界过渡带；只显示一个总包围盒不足以解释实际光照。 |
| Probe state | 对探针是否值得继续执行照明更新、是否可参与查询的分类。 | 活跃不是“已收敛”，休眠不是“墙体”，未初始化也不应伪装成正常黑色照明。 |
| Receiver support | 针对某个世界位置和法线，附近探针经过状态、几何方向、插值及可见性规则后留下的贡献。 | 应从实际 gather 输出贡献来源与失败原因；不能从“附近有绿色球”推断表面已有可靠 GI。 |

官方 `DDGIGetVolumeIrradiance` 查询 8 个邻近格点，跳过 inactive probes，并组合三线性、法线方向和距离矩权重。它还把 Chebyshev visibility 保底到 0.05，然后压低小权重、归一化；所有探针都被排除时返回 0。**因此非零 accumulated weight 也不等于无遮挡或高质量覆盖**，这是稳定性折衷，不是真实 receiver-to-probe 线段可见性的证明。[Irradiance.hlsl：查询与权重](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl#L65)。

建议诊断至少区分：volume 外、边界衰减、新探针未就绪、邻探针 inactive、邻探针过期、方向不匹配、visibility 被压低／实际线段被挡、有效但低照度。保留项目已有的更严格 receiver visibility 时，这条规则也必须显示出来，不能改成 NVIDIA 的保底权重来掩盖暗区。

## 3. Multiple volumes、滚动与边界不是同一个功能

**官方事实：**

1. SDK 把 tracing 与最终 gather 留给宿主，负责 probe blending、relocation、classification 等工作。多 volume 的优先级与选取同样是宿主职责：可依据密度、距离、屏幕覆盖或作者优先级，在重叠边缘混合。[Integration Guide](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/Integration.md#volume-blending)。
2. Infinite Scrolling Volume 是固定容量的活动窗口。移动时复用环形索引，只使新进入的边缘探针层失效并重新收敛；内部探针留在原世界位置并保留历史。滚动量按格点量化；SDK 的 scrolling volume 忽略旋转。它只覆盖当前活动空间，不能同时照亮无限世界。[DDGIVolume：Infinite Scrolling Movement](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#infinite-scrolling-movement)。
3. 当前 SDK `DDGIGetVolumeBlendWeight` 在网格外包络内取 1，外侧按各轴的一个 probe spacing 衰减到 0；网格 extent 是 `spacing * (counts - 1) / 2`。这是**volume 几何权重**，没有替宿主判断缓存是否有效。[Irradiance.hlsl：VolumeBlendWeight](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl#L39)。
4. 2021 production 论文采用的多 volume 策略并不与上述 helper 相同：从密到疏，在最后一个内部 cell 衰减，累计贡献达到 1 后停止。跟随窗口额外把过渡区缩进一格并以相机为中心，避免离散滚动突然把尚未稳定区域交给近场。[JCGT 正式论文 §7.3–7.4](https://jcgt.org/published/0010/02/01/paper.pdf)。

**针对本项目的建议：**

- 对俯视场景，以地面／可玩高度带的可见 footprint 定义近场需求，附加遮挡与接收面的余量；不要只把高空相机坐标当近场中心。水平与垂直 probe counts 可以不同，但仍是真正的 3D 支持，楼层、墙面和高物体不能被压成一张 2D 图。
- 首先验证一个固定密度、可滚动的 volume 是否覆盖整个目标视野；确有跨尺度需求时，限制为“近场细网格 + 广域粗网格”两层。有限关卡可用固定粗层；无界移动才需要粗层也滚动。两层共用一种 volume 描述、探针身份、调度和查询入口。
- 不同时缩放所有 probe positions 来追随相机；也不为覆盖更大关卡把固定 probe count 的间距无限放大。前者破坏历史稳定，后者牺牲局部遮挡和空间分辨率。
- 新进入区域先完成初始化与有效样本，再逐步获得近场权重；可靠粗层可以接替。若两层都没有有效支持，返回显式的低可信／未覆盖结果，按既有天空或环境回退合同处理。不要用不区分室内外的常量抬亮来遮住失败。
- 统一 gather 建议返回照度之外的 `source volume/layer`、有效支持与 fallback 原因，供最终 shading、probe-hit 递归照明及 debug 使用。具体数据布局由现有渲染结构决定，不需要再建一套外部 GI 服务。
- 混合权重应受几何覆盖、缓存有效性、收敛与过渡共同约束；两份完整 diffuse GI 不能直接相加。对可见表面和 probe ray hit 使用同一选层语义，避免边界处屏幕照明与递归能量传播分裂。

## 4. Relocation 与 classification 能解决什么

RTXGI classification 先用固定射线背面比例判断“可能在几何内部”，再判断是否有前向命中落在其相关邻域，决定 active／inactive；它不是判断某个屏幕表面是否被良好照明。[ProbeClassificationCS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeClassificationCS.hlsl#L135)。

Relocation 尝试从内部或过近表面移开，并限制归一化位移满足 `dot(offset / spacing, offset / spacing) < 0.45²`；这是椭球限制，不是每轴可独立位移 45% 的立方体。位置调整仍从属于原网格索引。[ProbeRelocationCS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeRelocationCS.hlsl#L193)。

SDK 默认以 32 条固定方向射线服务几何决策；inactive probe 仍需这些观测才可能再次激活。固定射线与随机旋转的照明射线分工，不参与照明积累。注意 SDK 示例的 `probeNumRays` 是包含固定射线槽位的总数，项目如果采用“固定 + 随机”两个独立计数，预算不能直接套同一公式。[ProbeTraceRGS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl#L53)，[Fixed Probe Rays](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#fixed-probe-rays-for-relocation-and-classification)。

本项目已在[前次研究](mesh-sdf-ddgi-tradeoffs-20261010.md)区分闭合实体和开放薄片；本次没有理由推翻该合同。扩大 volume 不等于修复内部判定，relocation 不等于自动把疏网格变成几何贴合网格。debug 应同时显示原始格点、实际位移及失败原因；否则偏移、无有效支撑和分类错误会混成一种“缺球”现象。

## 5. 更新预算、低噪声与事件失效

官方 SDK 支持宿主以低于帧率的频率更新；异步后台更新是可行组织方式，但 SDK 不直接实现该调度。其 variability 功能衡量 volume 平均变动，可在稳定后暂停更新，灯光或物体变化后由应用恢复。[DDGIVolume：Updating](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#updating-a-ddgivolume)，[Probe Variability](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#probe-variability)。

SDK 的历史权重默认 0.97，接近 1 更稳定但反应慢；降低后反应更快，也更易闪烁。改变更新频率时，响应时间随之变化。[DDGIVolumeDesc](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h#L97)。仅作**数学说明**：固定 EMA 权重 0.97 约需 99 次更新才将旧信号压到 5%；每帧更新与每四帧更新的墙钟收敛时间相差四倍。这不包含编码、变化检测、阈值和多次反弹，也不是实测。

官方 UE4 插件另提供 ray budget 与 volume update priority，以加权轮转分配更新；较多 volume 会拉长完整刷新周期。这是一种宿主调度范例，不是每个 DDGI 实现都必须“一帧只更新一个 volume”。[UE4 插件：预算与优先级](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/ue4-plugin/4.27/RTXGI/README.md#L128)。

**建议保留统一调度器，仅增加有明确价值的优先级：**

1. 先初始化新进入、重用槽位、被 invalidation 标记的 probe，再处理可见 receiver 的陈旧支持，最后轮转其余活跃 probe。
2. 设最长更新时间或最低轮转份额，避免静止远层长期饿死；inactive probe 保留廉价重新分类机会。
3. 区分“世界身份已失效”和“照明估计可延迟改进”。滚动槽位重用、格网尺度变更、显著 relocation 不能继承旧地点历史；灯光变化可按影响范围重新更新。遮挡体变化同时影响 distance moments 与 irradiance，不能只标记颜色。
4. 记录距上次几何检查、照明更新和有效初始化的年龄，以及失效版本。年龄是调度证据，不应无条件把稳定、仍有效的旧数据淡到黑。
5. 静态冻结只有在失效事件覆盖完整时才有价值。前期允许有上限的周期刷新；不要为冻结收益提前建设全套光传输依赖图。
6. 时间预算测量包括 tracing、hit shading／shadow、blend、classification／relocation、receiver gather 和几何更新。只计探针主射线会漏掉命中后的大量工作；只看平均帧时也看不见传送／缩放／大量灯切换的峰值。
7. 不先引入每个 probe 不同射线数、异步 compute 和复杂重要性反馈三套变化；固定每次照明样本数加优先队列／轮转即可完成第一个可测量增量。

变动很少时减少无意义随机更新有助于静止稳定；变化到来时应局部降低历史权重或重新积累。不能靠全局极高 hysteresis 把细小高亮发光体的欠采样伪装成正确收敛，也不能每次相机小移动全场清空。

## 6. 完整混合管线中的责任分工

官方 tracing 流程在 front-face ray hit 计算直接光，再查询附近 probes 的递归 irradiance；ray miss 使用环境。SDK 不接管场景加速结构、材质、灯光或 GPU 管线所有权。[Integration Guide：Tracing](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/Integration.md#tracing-probe-rays-for-a-ddgivolume)。

NVIDIA UE4 插件展示了发光面参与 DDGI 的效果，说明 DDGI 可以承载发光输运；这不规定同时使用独立 mesh NEE 的渲染器还应向 probes 注入直接命中的原始 emission。[官方发光体示例](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/ue4-plugin/4.27/RTXGI/README.md#emissive-surfaces-are-light-sources-with-rtxgi)。

本项目应保留当前能量归属：屏幕 mesh NEE 负责 receiver→emitter 的直接漫反射；probe-hit 的 analytic／mesh NEE 负责命中表面经散射后进入 DDGI 的光。以下为保留这一分工的目标组织方式，不代表所有环节都已统一实现：

```text
同一份场景几何／材质／灯光快照
  → 既有 RHI 的光追场景与命中数据
  → 统一材质、发光与直接光求值
      ├─ 相机可见表面的直接光／阴影（含 receiver→emitter 的 mesh NEE）
      └─ probe ray hit：命中表面的 analytic／mesh NEE 散射光 + 上一轮 DDGI 反射光
           （不注入 probe 直接命中发光材质的原始 emission）
           → 探针照度与距离矩更新
           → 同一 DDGI gather：最终 diffuse GI、递归查询、调试诊断
  → LocalGI／GTAO 的局部细节合同
  → SSSR／后续反射补全的 specular 合同
  → AA、曝光与显示
```

具体取舍：

- **发光体保留直接与后续散射的分工。** 当前组合不把 probe 直接命中的原始 emission 写入缓存，以免 DDGI gather 再次计入已由屏幕 mesh NEE 估计的 receiver→emitter 一跳能量。发光体对其他表面的照明先由 probe-hit mesh NEE 求值，经该表面散射后进入 DDGI；不能为了“支持发光体”无条件把 raw emissive-hit 加回。应核对两条路径共享的发光材质、采样权重与 HDR 能量，并分别观察直接光方差和间接光收敛。官方示例 ray shader 的 `saturate(radiance)` 属于示例存储路径，也不能直接抄成项目的 HDR 上限。[ProbeTraceRGS.hlsl：结果写入](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl#L194)。
- **大量灯的成本不能由 probe 数量单独限定。** 官方示例直接光遍历 spot／point lights，并执行 shadow visibility；这是展示完整求值的参考，不是 many-light 可扩展性保证。[Lighting.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl#L69)。未来 ReSTIR DI 可复用统一灯光记录、发射与 BSDF 求值、visibility API；probe hit 与屏幕 pixel 不具有相同采样域，不能直接借用屏幕 reservoir 当正确的 probe 估计器。具体 estimator 与重采样由 DI 专题研究确定。
- **DDGI 是漫反射低频基底。** LocalGI／SSIL 如继续存在，必须定义为缺失近场细节或明确的估计修正；不能把另一份完整 diffuse transport 无条件相加。GTAO 应有明确的间接光遮蔽用途，避免同时重复遮蔽直接光、DDGI 可见性和局部 GI。
- **SSSR 仍是 specular 路径。** 屏幕外反射需要独立回退；probe irradiance 只能在明确的粗糙度／二次反弹近似下帮助反射，不能替代镜面命中几何。此次不为 DDGI 覆盖而顺带重写反射。
- 保持 FP32 深度及既有历史精度合同。FP32 不能补回探针间距、方向分辨率或漏掉几何造成的信息损失；低频照度与距离可见性应独立预算。所有 GPU 查询、资源和调试绘制均从现有 RHI 进入，不复制 SDK 的 D3D12／Vulkan 宿主入口。

## 7. Debug 应交付的最小完整能力

NVIDIA sample 已有世界空间 probe 球的 irradiance／distance 可视化，并用红绿轮廓表达 inactive／active；UE4 插件还显示距离平方、设置 probe radius、隐藏或显示整组 probes。**球的半径只是绘制参数，不是 probe 的球形物理影响半径。** [ProbesRGS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/visualizations/ProbesRGS.hlsl#L43)，[UE4 Debug 设置](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/ue4-plugin/4.27/RTXGI/README.md#L128)。

建议以下视图复用生产数据与实际权重，作为一套 Scene Lit debug 控件，而不是独立 demo：

| 视图 | 最小信息 | 要回答的问题 |
| --- | --- | --- |
| Volume／Cell | 层或 volume ID、实际格网 bounds、混合带、选中 cell、滚动锚点；按层开关 | 地图哪里在覆盖内？这里由哪层负责？ |
| Irradiance 球 | 真实 probe 位置、方向照度；亮度显示范围可调 | 缓存是否接收到光？是否夹断或失去方向信息？ |
| Visibility 球 | 平均距离／距离平方／派生方差，配合数值读数 | 亮探针是否“看见”该方向？距离矩是否失效？ |
| State 与位置 | active／inactive／未就绪／本帧更新；原格点到 relocation 的线 | 是分类、位置还是采样初始化问题？ |
| Age／预算 | 上次几何与照明更新年龄、历史有效性、本帧更新数、预算占用 | 这是合理延迟，还是永远轮不到更新？ |
| Receiver coverage | 实际 source、边界权重、有效邻探针数、低可信／未覆盖原因 | 球存在时，表面为何仍没有有效贡献？ |
| 选中 receiver | 连到参与查询的 8 邻格点；贡献和拒绝原因；若叠层则按层显示 | 哪一个支撑环节造成当前结果？ |

球体颜色、visibility 诊断和 receiver 诊断要能独立切换；默认不要隐藏 inactive，否则最需要解释的失败位置会消失。普通遮挡绘制用于看真实空间关系，按需 x-ray 用于看墙内探针；二者不得混为最终贡献。未开启 debug 时不付出全屏诊断、逐探针 CPU readback 或持久复制整个 probe atlas 的成本。

在本项目中，调试球和线框应是 Renderer 对真实 GPU probe 数据的实例化叠层，不创建 World Actor、不进入 BLAS/TLAS、发光表、照明反馈、普通对象拾取或场景 Undo。普通着色/时间历史完成后再绘制；只读场景深度，保持公开 FP32 深度不变。Settings 是 view debug 策略；未来可编辑固定 volume 的作者数据才属于 World。Control 沿既有能力目录暴露同一设置与小型、带 frame/volume/probe 身份的拥有式诊断快照；仅在主动点选或捕获时做有界 readback，不建立第二套控制通道。

层间查询必须返回有效支持信息，不能把“有效观测到零照度”当成未覆盖而换成更亮的粗层。边界/未初始化的回退、可见性拒绝与真实暗部应保留独立原因；不存在有效缓存时也不擅自增加 ambient light。

## 8. 有限的 AMD 与 Epic 对照

**AMD Brixelizer GI** 用 screen probes，经 SDF tracing 查询 radiance cache，再生成世界空间 irradiance cache；radiance cache 来自上一帧 lighting output。它提供了多级表示、历史失效与分阶段 debug 的参考，但不是 RTXGI 的规则 3D probe volume，也不是对现有硬件 RT DDGI 的小型替换。[GPUOpen 算法说明](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/)。其 sample 分别显示 diffuse、specular、radiance cache、irradiance cache、cascade ID 等，支持“看实际中间表示”而非只看最终图像这一诊断原则。[GPUOpen sample](https://gpuopen.com/manuals/fidelityfx_sdk/samples/brixelizer-gi/)。

**Epic Lumen** 同样围绕相机维护有限场景表示，快速相机移动会令 GI 更新落后。官方视图将 Surface Cache 缺失覆盖标成粉色，并分别显示场景、法线和反射所见几何；这支持把“receiver 覆盖缺口”作为独立调试事实。Lumen 的 Surface Cache、screen tracing、Far Field 和 DDGI volume 不是同一算法，不能移植其距离／帧时参数作为我们的预算。[Epic Lumen Technical Details：Large Worlds／Visualization](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine)。

**AMD GI-1.0/1.1（Capsaicin）** 是更贴近默认硬件光追的另一条对照：GI-1.1 从 GGX 采样反射方向，在粗糙度不同的情况下选择屏幕探针插值或硬件光追 + 世界空间 hash radiance cache，并配有反射重建。可以借鉴跨屏幕/世界缓存的职责和按粗糙度分配工作；它不是简单扩大 DDGI volume，也不代表应直接迁入一套新引擎框架。[GPUOpen GI-1.1](https://gpuopen.com/learn/gi-1-1-glossy-reflection-rendering/)，[Capsaicin GI-1.0](https://gpuopen.com/learn/amd-capsaicin-framework-release-gi/)。

这两项对照支持的共同方向是：**可见工作集、分层表示、预算更新、可观察的缺失覆盖**。它们不构成本轮新增 SDF、Surface Cache 或完整 Lumen 的理由。

## 9. 下一批的范围与验收终点

以下为建议，需要结合本地代码检查形成最终实现方案。

**第一批：覆盖可解释。** 把 volume／probe debug 与 receiver coverage 接到当前生产查询；补齐年龄、失效和贡献来源。用当前场景确认问题来自覆盖外、分类失效、可见性拒绝还是低照度。保持现有场景照明行为，避免先改算法再失去对照。

**第二批：有界的覆盖演进。** 在同一描述与调度入口内实现／完善格点滚动；有真实多尺度需求才增加一个粗层。规定层间过渡、槽位历史身份、新区预热与 fallback；保留固定总内存／更新预算。近场细层不应使远层永远过期，也不因普通相机移动使整个 volume 重置。

**之后按证据决定：** 调度是否需要 variability 冻结、灯光变化优先；probe-hit many-light shading 是否成为热点。ReSTIR DI、阴影、反射分别推进，但共享材质／灯光／RHI 所有权。

本轮不优先做：任意数量可编辑 volume 系统、通用 probe BVH、逐 probe 可变射线调度、异步多队列、全场景 SDF、SHaRC／NRC 替换、ReSTIR GI／完整路径追踪。它们都可能有后续价值，目前没有证据能比修正覆盖与调度合同更直接地解决这次问题。

端到端验收只需围绕本次行为：

- 镜头连续移动、缩放、跨一格／多格和传送：保留区域历史稳定，新区不会读取旧地点缓存，边界没有突然切黑／过亮。
- 薄墙、开放薄片、闭合物体、动态遮挡和不同高度接收面：state 与 receiver support 能分别解释，既有几何语义不回归。
- 灯开关、移动灯、大小不同的 emissive：静止收敛后噪声与动态响应可观察，冻结会重新激活，迟滞符合约定。
- 同一场景比较各阶段 GPU 耗时、峰值、驻留内存、更新年龄分布与 coverage 缺口比例。只有测得数据后才给出 RX 6800 XT 预算；源项目的 ray 数与毫秒数不作为我们的承诺。

文档验收范围：一手来源和明确链接已核对；没有声称候选设计已在本地实现或达到帧时目标。
