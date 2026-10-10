# Scene Lit 后续演进：多灯、反射历史与 DDGI 调度

更新于 2026-10-10。本文只列尚未完成的方向和设计依据；已实现能力、默认参数与资源合同以 [Scene Lit](../modules/renderer/scene-lit.md) 为准，测量结果以[验收记录](../modules/renderer/acceptance.md)为准。公开论文和 SDK 的功能不代表本引擎已经接入或达到其性能。

## 原计划中已经完成的部分

| 原计划 | 已完成的结果 |
|---|---|
| 拆分 DDGI 成本 | 已有 update/transfer、receiver gather 时间与分类查询计数，沿同一完成帧采样；Control 已消费这两项统计 |
| 降低屏幕重建成本 | 已移除额外 5×5 辐照度重建和对应中间纹理，保留接收点到探针的真实 LOS 与连续方向可见性权重 |
| 发光面主动采样 | 已有发光三角派生记录、面积／亮度 alias 分布、streaming RIS 和实际可见性；与 DDGI 完成一跳发光的能量分工 |
| 稳定性与无效工作 | 已补齐移动／原位材质编辑的光照历史响应，跳过确定性光源重复查询、无贡献 DDGI 接收查询及关闭效果后的无效通道 |

以上不再列为下一批任务。原先约 3360 万额外查询、DDGI p50 1.567 ms 的数据来自已被替换的表面重建候选，只能用于历史归因；不能称为当前 DDGI 的成本。

## 仍然存在的边界

- [SceneLit::validate](../../src/renderer/scene_lit.cpp) 的解析灯限制仍是整帧 256 盏，不是每房间 256。发光三角形不占这 256 个槽。
- [直接光分块](../../src/renderer/shaders/lit_direct.hlsl) 每 tile 保存 64 个灯索引，溢出后遍历整帧灯表。仅放大或删除 256 检查，不能解决高重叠时的成本。
- 发光三角已经主动采样，但尚无服务大量解析灯与发光面的空间重要性分布，也没有时空 reservoir。全局功率分布在远近、强弱差异很大的场景中可能仍有较高方差。
- [DDGI](../../src/renderer/shaders/lit_ddgi.hlsl) 仍是单个有限规则体积；几何射线与照明射线分开估计，但尚未分开调度，辐射变化响应仍覆盖整个体积。滚动、局部加密、稳定区域降频未实现。
- SSSR 的单个贡献来源见证不是整个粗糙反射波瓣的身份。多个不同颜色来源同时参与、只有次要来源变化时，尚缺专门的时间响应验收。
- 动态实例上传、发光采样分布重建和加速结构维护，仍需在大场景中按实际更新量采样，不能据小房间截图推断扩展性。

## 建议的后续顺序

### 1. 多来源反射的时间响应

先在现有 `--scene-lit-temporal` 中增加“主要来源静止、次要来源移动／熄灭／换色”的真实场景，分开观察接收面几何、来源身份与辐射变化。保留纯色不污染、无关物体运动不破坏静态反射、T2x 长时间来源不漂移的约束。只有确认剩余误差及归因后，再决定是否扩展来源表示；不直接增加全局历史拒绝或加长拖尾。

### 2. 大灯数与空间候选选择

沿现有 World 稳定身份、SceneSnapshot 和派生发光记录扩展采样语义，处理全场分散灯与局部高重叠两种负载。屏幕 tile 不能代替离屏 probe 命中点的世界空间候选查询；列表溢出必须有正确继续路径，不能静默漏灯。场景光源数量与每个接收点的采样预算分开。

优先比较功率／空间重要性采样与现有 streaming RIS，保证每个可能贡献的来源有非零概率、正确选择与面积 PDF。改变分布时保留稳定身份、增删映射和动态权重的失效规则。沿既有路径保持直接漫反射、DDGI 后续反弹、可见 emission 与镜面反射的分工，不重复叠加同一跳发光。[S2、S4]

### 3. 按真实更新量降低维护成本

采样单个物体移动、多个物体连续移动、单个发光面改色、静态大场景等负载，分开记录 CPU 准备／上传、AS 更新、分布重建和 GPU 接收成本。只有这些成本成为实际瓶颈时，再扩展局部上传、局部重建或按消费者延迟维护；优先保留现有缓存、所有权和退休机制。

屏幕 LOS 复用或低分辨率间接光仍属于待验证实验。距离矩无法可靠表示薄墙／拐角时，错误跳过真实查询会重新漏光；需要几何、相机与探针位置的有效性证明。当前完成的优化没有引入这些近似。

### 4. Adaptive DDGI 与覆盖扩展

先保留规则格，让几何维护与照明更新能够分别排队；根据保守的影响范围、稳定度和更新年龄调度，避免饥饿。间接传播会越过直接光的范围，冻结的探针也不会通过不再执行的 variance 检测发现新变化，必须保留可靠唤醒与低频维护。[S1]

随后由实际地图需求决定单体积滚动、局部密集区域或多体积。滚动槽位必须保留内部探针的世界身份与历史，只预热新进入部分；不能凭空添加环境光掩盖未收敛区域。响应终点按实际内容、预算和图像度量判断，不承诺固定帧数。

### 5. 在同一采样入口比较 ReSTIR DI

当大量重叠灯或小亮发光面使简单重要性采样无法满足质量／成本目标时，再比较 ReSTIR DI。重点是候选质量、PDF／权重、动态灯映射、可见性与降噪的总成本；不把 256 当作算法切换阈值。时空复用省掉必要修正会产生能量偏差，不能作为免费降噪。[S3]

RTXDI 的 buffer-less RIS、ReGIR 可作为探针命中着色的初始候选参考，但屏幕 reservoir 不能直接成为离屏探针的权威光照。它们必须通过现有 RHI 接入，在 RX 6800 XT 上比较完整路径；不引入第二套设备／提交体系。[S2、S5]

## 保留的验收约束

- 深度、距离矩及前帧对应保持现有 FP32 合同；直接光、反射、AO／Local GI 与最终 AA 各自负责信号历史。
- 保留墙角、亚像素隔墙、移动遮挡、发光面增删／编辑、相机变化和低多边形硬边的真实端到端场景。
- 按相同质量比较成本，GPU 数据使用去重的原始完成样本；同步 Capture 时长不充当 GPU 基准，不靠扩大模糊或放宽门限宣布优化。
- [Mesh SDF](mesh-sdf-ddgi-tradeoffs-20261010.md) 是独立的未来表示选择，尚未实施；它不能自动消除开放表面与实体内部的语义问题。

## 保留的一手资料

| 编号 | 来源与核对边界 |
|---|---|
| S1 | [RTXGI-DDGI v1.3.7 DDGIVolume 文档](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/docs/DDGIVolume.md#L663-L782)：更新频率、scrolling、classification、fixed rays、variability。说明 NVIDIA 能力，不代表我方已实现全部。 |
| S2 | [RTXDI Integration Guide](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md)：light buffer、mesh processing、PDF/presampling、world-space/ReGIR、application bridge。 |
| S3 | [Bitterli 等 2020 原始 ReSTIR 论文](https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf)：直接照明 reservoir 算法、emissive triangle 内容、有偏/无偏边界。论文 GPU 时间未用于推断 6800 XT。 |
| S4 | [RTXDI FullSample BrdfRayTracing.hlsl](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl#L206-L243)：BRDF-hit emission 的显式归属和 mirror/delta 例外。它不是我方 DDGI 与 DI 组合已实现正确的证据。 |
| S5 | [RTXDI README](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/README.md#L106-L110)：样例 D3D12/Vulkan 入口；不能据此宣称本引擎集成或 6800 XT 性能已经验证。 |

RTXDI 仓库源链接固定本次 GitHub API 返回的 main commit `a6efab966b7c3b272da0461578eb56ac61c7cbff`；DDGI 固定既有 v1.3.7 commit。
