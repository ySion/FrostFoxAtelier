# Scene Lit 下一步进化：多灯、自发光与 DDGI 成本

实施状态：以下保留实施前的调研与顺序建议。随后已完成 DDGI 成本拆分、去除额外 5×5 重建，以及发光三角 alias/streaming RIS 初版和能量分工，见[当前合同](../modules/renderer/scene-lit.md)与[实机记录](../modules/renderer/acceptance.md)。解析灯大规模采样、空间分布与 Adaptive DDGI 调度尚未实现。

2026-10-10。一手公开资料与当前源码核对，供下一轮设计讨论；本轮没有修改生产代码、启动编辑器或运行新的 GPU 验收。后台研究助手负责公开资料，主任务核对当前实现并形成实施顺序。Exa 两次搜索各 5 个结果，`sources_reviewed: 10`；实际依据收敛到文末 5 份一手资料。

当前默认 8×4×8 探针，64 probes/frame，每次更新 128 条照明射线和 32 条固定几何射线；已采用同帧表面—探针真实 LOS 与连续距离矩权重、受表面 LOS 约束的 5×5 滤波。上一轮 1440p fixedbench（另用 11×7×10 探针、96 更新/帧、64 照明射线）的 DDGI p50 从 .450 增至 1.567 ms，增加 28.125 MiB，约 3360 万额外 visibility queries。**这组证据证明修复有成本，不证明具体哪一阶段占多少。** 用户允许全场远超 256、局部也可能达到 256，自发光是重要光源。

主任务核对到的当前约束：

- [SceneLit::validate](../../src/renderer/scene_lit.cpp) 限制整帧 `frame.lights.size() <= 256`；当前不是按房间分别允许 256。
- [直接光分块](../../src/renderer/shaders/lit_direct.hlsl) 每 tile 保存 64 个灯索引；溢出时像素遍历整帧灯表。因此不能只移除整帧 256 检查就称为支持大灯数。
- [DDGI 命中着色](../../src/renderer/shaders/lit_ddgi.hlsl) 在超过 4 灯时均匀抽两盏不同灯并补偿概率；形式上有界，但对空间分散、强弱差异大的灯群可能方差很高。自发光主要经 probe 射线命中进入漫反射场，没有 mesh-light 直接采样。
- [SceneLit::draw](../../src/renderer/scene_lit.cpp) 的辐射变化响应仍针对整个探针体积；每次探针更新仍执行固定几何射线。照明和几何已分开采样方向与历史，但尚未分开调度工作。
- [SceneSnapshot](../../src/application/scene_snapshot.hpp) 已有权威对象到稳定键的映射。下一步应沿这些键和旧/新数据推导变化范围，优先复用现有链路，不预设新业务注册表或全局事件总线。

## 按优先级建议

### 1. 先分清并降低屏幕重建成本，保留墙角正确性

**设计推断**：默认探针主射线预算是 `64×(128+32)=10,240/frame`，与新增数千万 visibility queries 的量级差很大；屏幕重建/滤波比探针主射线更可能是主要新增成本。这里还未计探针命中着色中的灯光阴影射线，必须用阶段时间与分类计数确认，不能仅用总计数下结论。

下一轮先在现有 RHI 内分开测 probe update、probe resolve、surface filter 的时间和查询数，并在最终连续权重组合下对照 5×5 重建的实际收益，判断它需要覆盖哪些情况。这一步的候选优化不能预先承诺没有画质损失。随后才考虑把“每像素每探针都追 LOS”转成**由遮挡不确定性和几何有效性触发的严格查询**，或试较低分辨率辐照度重建配合全分辨率表面/墙角约束。LOS 复用必须由几何、相机对应关系和探针位置变化失效；灯光变化本身不等于几何 LOS 失效。FP32 深度与距离矩保持，不用降低深度精度换性能。

这两项是实验，不是已经成立的优化。风险是距离矩不能可靠表达薄墙/拐角时错误跳过 LOS，或低分辨率重建重新跨面漏光、显露格子。验收必须保留现有墙角、薄遮挡、移动遮挡和低多边形面边界，用测量选方案；不能为提速直接撤掉已证明必要的遮挡判断。RTXGI 的 classification 只减少无贡献**探针**的更新，不会替我们消除逐像素 LOS 成本。[S1]

### 2. 建立统一可采样光源集合，自发光与大灯数一起处理

**公开证据**：RTXDI 的集成协议把 primitive lights、emissive mesh triangles 和 environment 放进应用准备的 light buffer；mesh light 按三角形展开，准备位置与平均辐射量，存在变形、动态/程序材质和纹理积分的实际接入问题。它并没有把“场景可有多少灯”与“每像素选多少候选”绑定成 256 这一永久上限。[S2]

**设计建议**：沿项目权威场景/材质与 RHI 入口扩展一个具有稳定身份、动态增删/变更信息和实际发光数据的光源集合。解析灯与自发光三角形共享可采样入口，采样预算独立配置。tile/list 溢出应有正确继续路径，不能静默漏灯；简单扩大固定数组只能过渡。初始权重可利用面积、辐射功率与保守影响范围，但每个可能贡献的光源必须有正确非零采样概率，动态纹理/变形要更新权重与旧样本对应。

**能量边界必须同时完成，不能先接亮度再补账**：相机直接看见的表面 emission 应保留；对接收面做 mesh-light 直接采样与 BRDF/probe 射线直接命中同一发光面的贡献，是可能重叠的直接照明估计。现有 DDGI 若已经把 probe 射线命中的 emission 包含在最终辐照度中，新增 mesh-light DI 不能再把相同一跳贡献完整相加。应明确 primary emission、直接光、间接反弹和镜面可见发光面的归属；要么完整划分路径项，要么在可获得正确 PDF 的估计器间合并，不能简单加两份全权重结果。也不能无差别删除所有 emission 命中，造成间接照明或镜中灯具消失。

这是我方组合 DDGI/DI 的设计责任，NVIDIA 不会替不同引擎自动完成。官方 sample 对 BRDF 射线的 emission 包含条件有显式分支，并保留 delta/mirror 例外，是其自身管线的具体分工证据，不是可照抄的通用开关。[S4 L206–209、L242–243]

### 3. 先提高光源样本质量，再评估 ReSTIR DI；探针可共享初始采样

**公开证据**：2020 原始 ReSTIR 研究覆盖数千至数百万 emissive triangles；其核心是 RIS / reservoir 中的光源候选、PDF/权重与时空重采样，最后对存活样本求照明，可减少“逐灯追所有阴影射线”的需求。无偏版本需要正确重采样权重/偏差修正；省掉部分修正的有偏版本确有能量损失，尤其在几何/阴影边界，不能把它当免费降噪。[S3]

**设计建议**：先做统一光源集合上的功率/空间重要性采样或 streaming RIS，并保留可比较的简单正确估计器；然后把 ReSTIR DI 作为直接光候选实验，复用现有 FP32 深度对应、真实 GGX、材质解调与信号滤波。目标是提高送进降噪器的样本质量，避免仅靠继续加历史把大灯数与小亮自发光的方差压住。被选样本仍须完整计算实际 `f×L×V`、选择/面积或立体角 PDF、动态灯身份，以及变化后的可见性；旧可见性或错误 PDF 复用可能漏光、偏暗或拖影。

SDK 明确有用于 secondary surface / RTXGI probes 的 buffer-less RIS，以及 ReGIR 世界空间预采样；ReGIR 可给 ReSTIR DI 或探针命中着色提供较好的初始候选，但它本身的 light selection 不求阴影可见性。大而分散场景下，仅 unordered light pools 的 RIS 仍可能很吵。[S2 §World-space sampling] 因此“屏幕 DI 与离屏探针共享光源/PDF体系、分别保留适用的复用上下文”值得试；不能把相机屏幕 reservoir 直接当离屏探针的权威光照。

RTXDI 的集成协议要求应用拥有材质、场景、RT、GBuffer 和 graphics API；其样例可用 D3D12/Vulkan。[S2、S5] 所以库或算法应接入我方现有 RHI，不引入第二个设备/执行入口。6800 XT 上的适配、资源成本与收益都必须实测；NVIDIA 论文的硬件时间、宣传收益和新 RTX 功能不构成我方承诺。ReSTIR DI 也不等于 ReSTIR GI/PT，不要求先替换整个 DDGI 或渲染器。

### 4. 场景规模扩展时，再让 DDGI 更新具备有效范围与生命周期

**公开证据**：RTXGI SDK 允许应用安排低频/异步更新，但没有内置一个替应用完成的通用调度器；classification 判断几何内/附近无有效几何的探针以跳过大部分更新；variability 只表示更新稳定程度，静态收敛到适当范围后可暂停，场景事件再唤醒。Scrolling volume 重用探针槽位，仅新进入的边缘平面无效化，内部探针保持世界位置和历史。[S1]

**设计建议**：先扩展现有变更与探针有效状态，让相关区域按光源/几何/探针移动的真实作用域唤醒，结合稳定度和更新年龄避免饥饿；再在大场景实际需要时引入滚动 volume、局部较密区域或多 volume。间接传播会越过光源直接影响域，dirty 范围须保守；冻结区域不会自行通过不再运行的 variance 检测发现未来变化。

风险是漏失变更造成陈旧光场、只按屏幕可见性更新造成离屏反弹缺失、scroll 槽位身份/历史错配，以及更新年龄不均引起亮块。优先保持当前已统一的固定几何射线、照明射线、独立距离/辐照度历史和查询入口。**不给固定若干帧收敛承诺**，收敛终点应由实际内容、预算和画面度量决定。

## 本次查阅的一手资料

| 编号 | 来源与核对边界 |
|---|---|
| S1 | [RTXGI-DDGI v1.3.7 DDGIVolume 文档](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/docs/DDGIVolume.md#L663-L782)：更新频率、scrolling、classification、fixed rays、variability。说明 NVIDIA 能力，不代表我方已实现全部。 |
| S2 | [RTXDI Integration Guide](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md)：light buffer、mesh processing、PDF/presampling、world-space/ReGIR、application bridge。 |
| S3 | [Bitterli 等 2020 原始 ReSTIR 论文](https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf)：直接照明 reservoir 算法、emissive triangle 内容、有偏/无偏边界。论文 GPU 时间未用于推断 6800 XT。 |
| S4 | [RTXDI FullSample BrdfRayTracing.hlsl](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl#L206-L243)：BRDF-hit emission 的显式归属和 mirror/delta 例外。它不是我方 DDGI 与 DI 组合已实现正确的证据。 |
| S5 | [RTXDI README](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/README.md#L106-L110)：样例 D3D12/Vulkan 入口；不能据此宣称本引擎集成或 6800 XT 性能已经验证。 |

RTXDI 仓库源链接固定本次 GitHub API 返回的 main commit `a6efab966b7c3b272da0461578eb56ac61c7cbff`；DDGI 固定既有 v1.3.7 commit。

## 主任务建议的交付顺序

1. **成本与质量基准收口。** 拆开阶段时间/查询，做上述必要性对照；保留这轮修复为画质基准，只有墙角、亚像素隔墙、相机运动和移动发光体均不退步才替换默认路径。不预先承诺可以回收多少毫秒。
2. **统一 LightSampler 初版。** 同一场景身份下支持解析灯与简单发光面的主动采样，完成直接光/间接光的能量分工；采样数量与场景光源数量分离。区域筛选处理全场大量分散光源，局部高密度保有完整概率支持；禁止靠静默截掉低排名灯过关。屏幕灯列表不能代替离屏 probe 命中点的世界空间查询。发光三角形是派生的采样记录，不等于作者必须建立相同数量的 LightComponent。
3. **Adaptive DDGI 调度。** 保留规则格，几何维护和照明更新分别排队；局部变化优先、稳定区域降频、更新年龄避免饥饿。变化范围要保守覆盖间接传播，保留低频维护；用实测响应曲线验证收敛，不能承诺固定 2–4 或 4–12 帧。仅改变调度不会消除屏幕端逐像素查询成本。
4. **场景覆盖扩展。** 先做围绕俯视相机关注区域的单体积滚动，保留内部历史、预热新进入的边缘区域，再由真实多层场景决定局部加密或第二体积。没有有效 Sky Light 的场景不能凭空加环境光来掩盖滚入未收敛区域。
5. **在同一采样入口比较 ReSTIR DI。** 实际大量重叠灯或小亮发光面使简单重要性采样达不到同等画质/预算时进入实现对比，不把 256 当成算法切换阈值。比较完整直接光、时空复用、可见性与降噪的总成本及动态响应，不只看最终阴影射线数。

每步沿现有 Scene Lit 与 RHI 入口形成独立可验收增量。需要新增底层能力时交由 RHI 负责人扩展合同；需要上游变化范围或 Control 指标时再协调相应负责人，不越过所有权边界。
