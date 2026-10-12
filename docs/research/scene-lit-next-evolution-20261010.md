# Scene Lit 后续演进：DDGI 覆盖、多光源与光追重建

更新于 2026-10-11。本文只列尚未完成的方向和设计依据；已实现能力、默认参数与资源合同以 [Scene Lit](../modules/renderer/scene-lit.md) 为准，测量结果以[验收记录](../modules/renderer/acceptance.md)为准。公开论文和 SDK 的功能不代表本引擎已经接入或达到其性能。

本轮根据 Dungeons 大场景重新安排优先级：先让覆盖与支持可观察，再扩展 DDGI 工作范围；ReSTIR DI 作为多局部灯/发光面直接光的明确候选推进，阴影、光照历史和光源身份与之一起设计。详细考据分为 [DDGI 覆盖、调度与 debug](ddgi-coverage-evolution-20261011.md) 和 [ReSTIR DI／阴影](restir-di-shadow-evolution-20261011.md)。本页保持统一的后续顺序，不表示这些方案已经实现。

## 原计划中已经完成的部分

| 原计划 | 已完成的结果 |
|---|---|
| 拆分 DDGI 成本 | 已有 update/transfer、receiver gather 时间与分类查询计数，沿同一完成帧采样；Control 已消费这两项统计 |
| 降低屏幕重建成本 | 已移除额外 5×5 辐照度重建和对应中间纹理，保留接收点到探针的真实 LOS 与连续方向可见性权重 |
| 发光面主动采样 | 已有发光三角派生记录、面积／亮度 alias 分布、streaming RIS 和实际可见性；与 DDGI 完成一跳发光的能量分工 |
| DDGI 调试基础 | 已实现实际探针球、状态/年龄/方向距离、Volume 边界、重定位和真实八邻 Coverage；Dev/Release GPU 与正式 Editor Control 验收通过，逐像素明细仍待后续 |
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

### 1. DDGI 覆盖与生产数据可视化

当前只读现场的 volume 为 `[-20,-2,-13]..[19,15,12]`、2240 probes；相机关注点 X 已达 19.17，环境为零。源码确认单体积外返回零间接光，但截图具体黑区还需逐接收点或对照确认，不能只凭 camera target 判定墙面坐标。该只读基线的 `probe_debug` 仅显示最近探针分类。后续首个调试增量已改为真实八邻 Coverage，并加入空间 probe/volume 显示，当前合同见 [DDGI 诊断](../modules/renderer/scene-lit.md#ddgi-空间与接收面诊断)。

首个增量交付 Volume 边界、实际 probe 位置与 relocation、辐照度/距离球、state/更新年龄以及 receiver coverage；有效暗部、未覆盖、未初始化和无可见支持已分别表示。调试叠层消费真实 GPU 数据，不创建场景 Actor，不进入 AS、光照、普通拾取或 Undo；不开启时不付出诊断 pass/readback 成本。UI、Control 和验收使用同一 ViewOption。完整 Cell 网格与点选接收面的邻探针编号、权重和逐项拒绝原因仍是后续诊断扩展，不能把颜色分类称为逐像素明细。下一步先用当前诊断确定真实黑区的覆盖/支持成因，再选择滚动工作集。

### 2. 保留规则格，建立有界的滚动与两层覆盖

先实现固定密度的格点滚动、稳定世界 cell 身份与环形槽位；仅重置新进入区域，跳跃超过完整范围才按新的工作集重建。俯视视角以地面/可玩高度带的视野为需求，加接收面及遮挡余量，仍保留真正三维的高度支持。不能逐帧缩放网格或通过改当前 bounds 触发全体历史重置。

需要跨尺度时增加一个粗层：有限地图可固定粗层，连续大世界才让粗层滚动。最多两层先把混合带、有效支持、预热与回退契约完成；粗层供广域稳定基底，细层供当前活动区。有效零照度不能当作缺失去换成更亮的缓存，没有合法环境照明时不增加隐式 ambient。优先选择足够的层，仅在过渡或真实缺失支持时追加查询，避免无条件翻倍每像素八条 LOS。

几何维护与辐射更新按不同变化原因排队，保留最低维护频率、年龄上限和新增/移动区域的优先更新。不能依赖被冻结探针自行发现变化，也不能把灯范围外的多次反弹永久排除。总内存、每帧主射线、probe-hit 阴影、receiver gather 与更新年龄共同构成预算。[S1]

### 3. 扩展已有光源采样入口，建立 ReSTIR DI 闭环

沿已有解析灯、发光三角、alias 与 streaming RIS 扩展统一光源数据和采样/PDF；明确稳定身份、current/previous 索引映射、功率/参数化版本和离屏贡献。先处理整帧 256 灯校验与 64/tile 溢出全遍历这两个独立扩展边界，不能只删除一个检查。场景容量与每接收点工作预算分开。[S2、S3]

闭环为 initial candidates → temporal → spatial → 当前场景最终可见性 → 去材质 diffuse/specular → 光照重建。初版不复用最终旧 visibility、不使用 visibility shortcut；少量主方向光/小灯集合保留同一入口内的精确评估，与随机灯集合互斥。时空重采样有其校正与历史成本，不声称严格固定一条射线或自动无偏。

保留现有发光能量分工：屏幕 mesh NEE 的直接漫反射、probe-hit NEE 的后续散射、可见 emission 和 SSSR 中的发光镜面各自只计一次。初版不无条件扩充 mesh DI specular；要扩充时先定义与 BRDF/SSSR 命中发光面的互斥或 MIS。[S4]

DDGI probe-hit 可消费同一光源表与世界空间 RIS，后续候选质量不足时比较 ReGIR；屏幕 reservoir 不直接充当离屏 probe 的光照。RTXDI 的算法层经现有 RHI 接入，不迁入第二套设备、提交或场景管理。[S2、S5]

### 4. 阴影与光照重建一起收口

阴影预算逐步转为每个接收点的采样/可见性预算；保留 `casts_shadow=false` 的明确作者语义。不要在 ReSTIR 选中某灯后仍沿用旧全局“前若干灯才有阴影”的限制。不同光源样本保持 `f×L×V` 的耦合，不能将平均 shadow mask 与另一份平均光照相乘。

现有 diffuse/specular 历史、方差与几何约束先作为基线。比较 NRD 时优先评估 RELAX 对 DI radiance 的适配；SIGMA 的单灯 shadow 信号与多灯 weighted radiance 分开。按 NRD 合同提供 linear viewZ、motion、normal/roughness 与合适 hit distance，保留现有 FP32 深度。reservoir、可见性和去噪历史独立有效，不能用延长最终 TAA 历史处理运动错误。[S6]

对接触阴影、薄几何、强面光、相机新显露区域和物体运动做真实对照。可接受静态低频照明与短时收敛，保留轮廓、及时遮挡响应与 SMAA 下的独立光照稳定性。半分辨率、checkerboard、可见性缓存和 pass 融合在基线后逐项比较。

### 5. 完善反射回退，并按实际工作量优化

当前 SSSR 的屏幕外/追踪失败只回退环境。后续可比较 SSSR 命中优先、缺失部分硬件 RT，并复用同一命中材质、光源采样与间接缓存；这比给镜面直接套 DDGI 更符合信号分工。AMD GI-1.1 的屏幕 probe／世界 hash cache 与按粗糙度分配反射工作可作为参考。[S7]

多来源粗糙反射仍需原计划中的时间响应对照：主要来源静止、次要来源移动/关灯/改色，保留纯色不污染、无关运动不破坏历史及 T2x 来源稳定性。不得以更长历史掩盖较小贡献源变化。

持续拆分 CPU 场景准备/上传、AS、光源候选维护、DI/阴影、DDGI update/gather、Local GI、反射/去噪和常驻资源。对当前实际使用的物理 viewport 评估照明目标分配与 dispatch 范围，明确公开深度/编辑坐标映射后再缩减窗口外的中间工作；不顺带降低深度精度。probe 更新更少不保证整帧更快，灯/发光面维护和各类 receiver 查询都要测量。

## 保留的验收约束

- 深度、距离矩及前帧对应保持现有 FP32 合同；直接光、反射、AO／Local GI 与最终 AA 各自负责信号历史。
- 保留墙角、亚像素隔墙、移动遮挡、发光面增删／编辑、相机变化和低多边形硬边的真实端到端场景。
- 按相同质量比较成本，GPU 数据使用去重的原始完成样本；同步 Capture 时长不充当 GPU 基准，不靠扩大模糊或放宽门限宣布优化。
- [Mesh SDF](mesh-sdf-ddgi-tradeoffs-20261010.md) 是独立的未来表示选择，尚未实施；它不能自动消除开放表面与实体内部的语义问题。

## 保留的一手资料

| 编号 | 来源与核对边界 |
|---|---|
| S1 | [RTXGI-DDGI DDGIVolume 文档](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md)、[Production DDGI 论文](https://jcgt.org/published/0010/02/01/paper.pdf)：固定射线、分类、滚动、分层与更新策略；SDK helper 与论文边界混合并非完全相同，细节见 DDGI 专题。 |
| S2 | [RTXDI Integration Guide](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md)：light buffer、mesh processing、PDF/presampling、world-space/ReGIR、application bridge。 |
| S3 | [Bitterli 等 2020 原始 ReSTIR 论文](https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf)：直接照明 reservoir 算法、emissive triangle 内容、有偏/无偏边界。论文 GPU 时间未用于推断 6800 XT。 |
| S4 | [RTXDI FullSample BrdfRayTracing.hlsl](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl#L206-L243)：BRDF-hit emission 的显式归属和 mirror/delta 例外。它不是我方 DDGI 与 DI 组合已实现正确的证据。 |
| S5 | [RTXDI README](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/README.md#L106-L110)：样例 D3D12/Vulkan 入口；不能据此宣称本引擎集成或 6800 XT 性能已经验证。 |
| S6 | [NRD 信号/API 合同](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/README.md)、[RTXDI NoiseAndBias](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/NoiseAndBias.md)：radiance/shadow、hit distance、时空相关性与可见性偏差。 |
| S7 | [AMD GI-1.1](https://gpuopen.com/learn/gi-1-1-glossy-reflection-rendering/)：屏幕 probe 与世界 hash radiance cache、硬件 RT 和粗糙度相关的反射求值。 |

RTXDI 源链接锁定 `a6efab966b7c3b272da0461578eb56ac61c7cbff`，DDGI 锁定 `f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6`，NRD 锁定 `e06852bd545973cb224eb28820b891205cfc1624`；版本、能力与适配边界的核对详见两份专题研究。
