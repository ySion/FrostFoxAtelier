# Scene Lit 画质、稳定性与清晰度演进评估

日期：2026-10-08。状态：研究与设计评估完成，**本轮未修改渲染生产代码、未替换已打开的编辑器、未执行新的 GPU 验收**。本文的建议不是已经落地的能力。

依据：当前 `codex/viewport-rendering` 未提交实现、此前实际验收记录、NVIDIA/AMD/Intel 官方文档与源码、算法作者论文。Exa 共检索 105 条结果，包含重复和不适用项；该数不是 105 个独立引用来源。三位 6.1-sol 子代理分别提供 DDGI/深度、AA、局部信号代码审计，主代理核对关键执行链并作最终判断。研究材料保存在 `out/scene-lit-quality-research-20261008/`；引用以下原始来源即可复查，不依赖这些忽略目录中的材料。

## 1. 结论与用户的画质取舍

当前架构适合继续演进：统一 Renderer 入口、真实 World 数据、稳定对象键、GBuffer、RHI 内的 RayQuery、独立 DDGI 场及反射历史都值得保留。第一版已经跑通功能，但**强动态光照、低样本阴影和随机反射的信号重建仍不够成熟**。算法名称齐全不等于画质已经达到目标。

用户明确优先：

- 几何轮廓、lowpoly 面的形状、材质细节清楚。
- 光照允许稍微平滑、低频，允许轻微延迟收敛；不能接受明显斑块、反射沸腾和大量噪点。
- 深度使用当前硬件最高实用精度，不能为了带宽主动降位数。
- 可考虑 SMAA 主导的混合方案，屏幕空间 GI 是待评估的细节补充。
- 全部 GPU 工作保持现有 RHI 边界；若需底层能力，由 RHI 负责人处理。

建议优先顺序：**修历史有效性 → 保证深度契约 → 完整光照信号降噪与 DDGI 更新 → 反射/AO 稳定 → 原生分辨率 SMAA 对照 → 屏幕空间局部间接光**。其中深度和信号重建可按互不冲突的职责并行设计，但验收需能归因。此时继续增加 ReSTIR DI 或更多效果，不是解决已报告画质问题的最短路径。

## 2. 目前实际拥有的技术栈

| 环节 | 当前实现 | 重要边界 |
|---|---|---|
| 视口 | MatCap、Smooth、Flat、线框、背景/网格、Scene Lit | 着色方式与 Edit/Play 生命周期独立 |
| 表面 | 不透明 deferred GBuffer、GGX 金属度/粗糙度、HDR 自发光 | 没有透明材质/完整路径追踪 |
| 直接光 | 点光、矩形面光、方向光；局部灯分块筛选 | 面积采样仍有随机亮度方差；没有 LTC/RTXDI |
| 阴影 | RHI inline RayQuery，默认 4 盏阴影灯、每灯每像素 1 样本 | 目前过滤的是标量可见性；不是 NRD/SIGMA |
| 间接漫反射 | 自研 DDGI，方向化 irradiance、距离矩、八探针插值、分类/重定位、递归漫反射反馈 | 单有限体积，非官方 RTXGI 集成；默认 256 探针，每帧 32 个，每次 64 rays |
| 接触遮蔽 | 自研 GTAO 积分，3 slices × 两侧 × 4 steps | 当前无独立 AO 空间/时域降噪，时间噪声主要交给最终 TAA |
| 反射 | 自研 GGX VNDF 随机 SSR、FP32 min/max Hi-Z、work list、间接 dispatch、独立时空过滤 | 每个合格像素 1 ray，miss 回退均匀环境；不是 AMD SSSR SDK 的完整实现 |
| 抗锯齿 | 原生分辨率 TAA，8 相位 Halton jitter、运动重投影、深度/法线校验、YCoCg 裁剪 | 当前没有 SMAA、MSAA、FSR/DLSS；不是超分辨率 |
| 显示 | Bloom、EV 曝光、SDR filmic tone mapping | UI 在场景处理之后合成 |

代码：[设置](../../src/renderer/view_settings.hpp)、[统一执行链](../../src/renderer/scene_lit.cpp)、[DDGI](../../src/renderer/shaders/lit_ddgi.hlsl)、[后处理](../../src/renderer/shaders/lit_post.hlsl)。当前合同见 [Scene Lit](../modules/renderer/scene-lit.md)。

## 3. 最高优先级：一次灯光变化被当成全画面历史不可信

### 3.1 已确认的执行链

`scene_lit.cpp:346` 比较完整 GPU 灯数据，因此位置/方向连续改变会令 `light_changed=true`。`:379` 将它写成全局 `lit_temporal.z=1`。后续：

| 消费者 | 当前变化帧行为 | 直接后果 |
|---|---|---|
| 阴影 filter | `h = .9 × (1 - lit_temporal.z)` | 历史权重变为 0 |
| Reflection resolve | `lit_post_response` 含 `1 - lit_temporal.z` | 反射历史权重变为 0 |
| 最终 TAA | 同一个 response | TAA 历史权重变为 0，但投影仍在 jitter |
| DDGI | pending 时 `.5`，shader 再乘 `1 - .85 × lit_temporal.z` | 旧历史只剩 **.075**，新样本占 92.5% |

证据：`lit_shadow.hlsl:30–36`、`lit_post.hlsl:443–449,522,589`、`lit_ddgi.hlsl:328–338`。这是代码直接事实，不是由视觉现象猜测。

因此，**移动一盏灯可以使全画面每帧失去大部分随机信号积累**，包括离这盏灯很远的反射和阴影。它能够解释一类移动时噪声显著增加的机制；本轮没有录制用户正在操作的逐帧画面，所以不宣称它独占所有现象的根因。

另一个不同分支：`SceneSnapshot::refresh` 有意不把 TRS 作为 scene revision，但灯的颜色、强度等内容变化会增加 revision（`scene_snapshot.hpp:42–66`）。Renderer 对任意 revision 调用 `reset()`（`scene_lit.cpp:354`），包括清 probe cursor。若连续逐帧改变强度/颜色，在该分支会反复清空 DDGI 并重新更新第一批探针。**不能把“移动灯”与“连续编辑灯参数”混为同一失效路径。**

### 3.2 应进化的机制

沿现有稳定键及快照比较，将变化解释为场景替换、拓扑/材质改变、光源辐射改变、几何运动和相机运动；在 SceneLit 私有状态中让对应信号决定如何重用历史，不另建业务对象身份或一套 Renderer。

- 真正的场景域替换、尺寸/不兼容表示改变仍彻底失效。
- 光源移动/变色首先影响 irradiance 与实际受影响的直接光；不自动毁掉静态几何的 distance history。
- 对已知动态区域降低历史信任、增加有效新样本；保留其余区域积累。最初可保守标记一个体积，再逐步按光照影响范围收紧，不能遗漏间接传播。
- 允许用户接受的短暂收敛，避免用“整屏历史归零”追求表面上的即时响应。
- 几何遮挡解除、物体移走仍需及时拒绝错误历史；不能简单把全局 `.9` 改成 `.99`。

## 4. DDGI 斑块与 NVIDIA 经典方案的比对

### 4.1 我们的具体风险

1. **更新不同时。** 默认 256 探针/每帧 32 个，需要 8 帧一轮。32 个连续索引恰好是默认网格的一层 `8×4`，持续移动灯时会形成按层更新的新旧光场。60 fps 下遍历一轮约 133 ms，是参数算术，不是实测响应延迟。间接反弹和重定位可能需要更多轮。
2. **动态历史过短。** 上述 .075 与稀疏更新叠加，会把每次低样本估计直接显露为低频色块。静态 `.93` 也不能孤立看：理想 EMA 半衰期约 9.55 次探针更新；8 帧一次即约 76 帧。实际还有启动期、可见性和多次反弹，不等于最终图像精确延迟。
3. **几何判断也在随机变化。** 同一批每次旋转的 64 rays 同时承担照明、背面分类、最近表面判断和重定位。重定位会清该探针 irradiance/distance、暂时使它不可用。边界样本变化有机会造成分类/偏移反复变化；当前画面是否触发需观察 probe 状态，不能仅凭斑块下结论。
4. **命中照明本身方差较高。** 每条 probe ray 在全部灯中有放回均匀抽最多两盏，再按概率补偿；默认四盏灯时两次抽中同一盏的概率是 25%。强弱灯差距大、小亮发光体、尖锐 GGX 响应会增加方差。提高 probe 空间密度不能自动解决这些采样问题。
5. **两种历史被一起削弱。** irradiance 与 distance moments 使用同一个最终 history weight；纯灯光变化也使几何距离估计不稳定。

证据：`view_settings.hpp:29–33`、`scene_lit.cpp:365,402–416`、`lit_ddgi.hlsl:160–206,263–338`。

### 4.2 NVIDIA 采用什么

以下固定为经典 **RTXGI-DDGI v1.3.7，commit `c233266e95931231961f35c842c78e7c4bdc50cf`**。新的 RTXGI v2 NRC/SHaRC 是其他 radiance cache 路线，不能把其宣传效果视作经典 DDGI 的默认能力。[版本](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/releases/tag/v1.3.7)、[官方产品区分](https://github.com/NVIDIA-RTX/RTXGI/blob/main/Readme.md)。

| 机制 | 当前引擎 | NVIDIA 经典 SDK 的核实结果 |
|---|---|---|
| 方向表示 | irradiance 与 distance 都是 8×8 有效 texel；软件 seam folding，无边框 | 常见样例 irradiance 6×6 内区 + 边框，distance 14×14 内区 + 边框；各自可配置 |
| 射线数 | 上限 64，64 lanes 同时用于 texel/ray 布局 | descriptor 默认 256 总 rays；不是必须固定 256 |
| 几何判断 | 全部随更新旋转 | classification/relocation 启用时默认保留 32 fixed rays；排除在 irradiance/distance blending 之外，224 条才是该配置的随机照明 rays |
| 采样旋转 | 每个探针独立旋转 Fibonacci 球面序列 | 同一次 volume Update 的随机旋转作用于非 fixed rays |
| 时间累积 | 线性 irradiance + 固定/全局变化权重 | 编码域累积，默认 gamma=5、旧历史权重 .97；还存在阈值与亮脉冲限制 |
| distance 历史 | 跟随灯变化一起大幅降低 | distance 不应用 irradiance 的 gamma/亮度阈值逻辑；几何事件需单独处理 |
| 静态收敛 | 一直继续更新 | 可用 variability 判断收敛，应用可在静态稳定后暂停 trace/blend，由场景事件唤醒 |

表示/配置证据：[DDGIVolume descriptor](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h#L97-L131)、[样例](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/samples/test-harness/config/cornell.ini#L97-L105)、[ray directions](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/rtxgi-sdk/shaders/ddgi/include/ProbeRayCommon.hlsl#L109-L133)、[fixed rays 排除](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L403-L409)。

SDK 的动态启发式需要按源码理解：encoded `old-new` 某通道超过阈值时 history 减 .75；正向亮度 delta 超阈值则只采用原 delta 的 .25。后者可以抑制亮脉冲，也会拖慢真实突然开灯。它不是无条件的“动态即刻稳定”。2021 论文里的另一套双阈值/事件时段示例与这个版本不完全相同。[blending 源码](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L508-L567)、[生产化论文 §4.3](https://jcgt.org/published/0010/02/01/paper-lowres.pdf)。

NVIDIA 也承认某些探针 texel 持续有低频波动；静态收敛后冻结、事件唤醒有实际价值，但变化事件遗漏会使 GI 过时。[variability 官方说明](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/docs/DDGIVolume.md#L774-L782)。经典 DDGI 不依赖最终 TAA 或 NRD 才能成立，官方要求可用 DXR 的 GPU，算法本身不强制 NVIDIA 专有硬件。[硬件与集成](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/c233266e95931231961f35c842c78e7c4bdc50cf/README.md#L43-L60)。

### 4.3 推荐的 DDGI 演进

先拆分几何状态射线与随机照明射线、irradiance/distance 历史，以及灯变化与几何变化。静态探针稳定积累；动态区域优先、分散交错更新并保持公平轮转，避免按完整空间层突变。history 根据有效样本/更新间隔设计，不让更改 budget 无意改变整个响应时间。

少量灯场景先考虑确定性覆盖全部有效灯或无放回/分层灯采样；灯数多时再使用带正确 PDF 的重要性选择。增加 ray 预算与增大 distance 方向表示要分别测量，不能把“64→256”或“更多探针”当唯一答案。64 线程组布局不应该成为算法永远只能 64 rays 的语义限制。

用户接受轻微延迟，因此可以选择更保守的照明累积和边界保持的低频光场，而不以全局骤降 history 换即时噪声。缓存误差仍要通过表示/可见性修正；不能用平滑掩盖穿墙或错误能量。

## 5. 软阴影、直接光与反射沸腾

### 5.1 阴影噪点不只来自阴影滤波半径

当前 trace/direct **确实使用同一随机 seed**，不能误报为两个原始样本不匹配。但是 trace 只保存 `V`，3×3 空间 + EMA 得到平均可见性后，direct 仍计算当帧随机的 `f×L` 并乘滤后 V。因此直接光的随机 BRDF、方向和衰减方差没有独立重建；过滤后的乘积也不再保持完整 `f×L×V` 样本相关性。这个信号继续进入 SSR 的 HDR source。

阴影还缺少半影大小/遮挡距离、统计矩、有效历史长度、方差指导的空间支持；历史几何校验比反射/TAA 已有的逐 tap、投影平面校验更弱，使用固定 device-depth `.002` 容差。证据：`lit_shadow.hlsl:7–39`、`lit_direct.hlsl:41–59`、`lit_post.hlsl:375–385`。

**推荐主路线**：沿现有直接光积分入口重建实际直接光照，必要时分 diffuse/specular 信号，用 material demodulation、深度/法线门控及统计置信度，保持硬几何边界与材质颜色。将 1 样本与多样本的真实积分契约统一，迁移完成后删除被替代的 scalar 历史生产路径，不长期并列两套渲染入口。参数应偏稳定光照，运动局部响应而非全画面清历史。

若选择保留独立 visibility denoiser，则仍须同时解决随机 `f×L` 方差。LTC 可以稳定无阴影矩形面积光的 GGX 近似积分，但 **LTC × 平均 V 仍是遮挡因子近似**，不能等同于完整遮挡积分；不建议本轮在两条主路线上同时投入。[LTC 作者资料](https://eheitzresearch.wordpress.com/415-2/)。

### 5.2 反射已有的能力与缺口

已有：GGX VNDF、层级深度遍历、confidence、环境 control-variate、5×5 几何/粗糙度约束池化、逐 tap 历史验证、当前命中面重投影，以及保存在 alpha 的持续方差。rough lobe 允许随机 hit/miss 跨帧累计；near-delta 才加强命中关联。这些都应保留，不能以“目前没有反射降噪”重新发明一次。

主要缺口：

- 合格像素已是 1 ray/pixel；每个 ray 读取单个 HDR texel，没有按反射 footprint 的 source prefilter。高亮小物和高频直接光容易带来大方差。
- 固定单层 5×5 并未按 ray distance、粗糙度、有效支持数调整；缺少 history age/sample count 与方差指导的额外采样/多尺度滤波。
- 反射积累后，最终 TAA 又用当前 3×3 的 min/max 和 1.25σ 裁剪整幅 HDR，却不消费反射的持续方差。存在把已经稳定的估计重新拉向当帧值的风险，需信号对照验证其贡献。
- **确定的统计不一致**：空间方差使用 YCoCg 的 Y，跨帧均值差使用 Rec.709 luminance，却混入同一个历史方差。饱和彩灯时量纲尺度不同；可在现有通道内修正。
- 单层屏幕数据看不到屏外或被挡住的表面。降噪不能生成缺失的几何/辐射，均匀环境也无法提供丰富的屏外反射。

证据：`lit_post.hlsl:237–256,355–385,452–535,563–591`。AMD 的成熟 SSSR 明确把 tracing 与 Reflection Denoiser 分开，使用 disocclusion、variance、空间重建和时间重建；不是给 tracing 接最终 TAA 就结束。[AMD SSSR](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)、[AMD Denoiser](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/)。

### 5.3 借鉴哪个降噪器

| 参考 | 适合学习的能力 | 接入约束 |
|---|---|---|
| AMD Reflection Denoiser | 反射方差、支持度、重投影、显露区与空间/时间过滤 | 适合现有 SSSR 形态；不是声称当前已集成 |
| AMD Shadow Denoiser | 1 ray/pixel 的局部统计、方差指导边界保持滤波 | 以单光源 visibility mask 为对象，不能自动过滤随机 BRDF 辐射 |
| NVIDIA SIGMA | 按光源的阴影、半影/遮挡距离指导 | 光源形状、距离和输入含义必须匹配；不是完整 direct radiance denoiser |
| NVIDIA REBLUR/RELAX | diffuse/specular 辐射重建、材质去调制、置信度及历史处理 | 需要匹配 radiance、hit-distance、normal、roughness、motion 契约；不能把任意 RGBA 贴图直接送入 |

NRD 是 API-agnostic compute 调度库，可以由引擎使用其 dispatch/resource 描述；不必使用 NRI 原生句柄整合层。任何采用都必须经本引擎 RHI，不能为了接库绕过该边界。[NRD 官方接口与信号定义](https://github.com/NVIDIA-RTX/NRD/blob/abb00a7fb3535d969435f1b68c0c37e480ca88b6/README.md)。首选在当前私有通道吸收匹配的成熟机制；若维护完整高质量重建的成本超出收益，应优先评估正式库适配，不宣称自研必然优于成熟方案。

采样也应从独立 hash 伪随机升级到有空间/时间分布设计的序列。STBN 有助于降低低频噪声和滤后色块，但运动会削弱其时间优势；它不能替代有效历史或增加真实样本信息。[NVIDIA 作者说明](https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-2/)。

## 6. SMAA 主导方案：可行，但先让光照独立稳定

当前 TAA 是 native-resolution 的长历史重建：8 相位 jitter、基础旧历史权重 .9、双线性历史重建与 3×3 YCoCg 裁剪。原生分辨率也会因反复重采样和历史裁剪损失细节；不是只有超分辨率才可能模糊。[TAA 综述 §6.1](http://www.leiy.cc/publications/TAA/TemporalAA.pdf#page=9)。

| 路线 | 清晰度与稳定性 | 建议 |
|---|---|---|
| SMAA1x + 各光照信号独立降噪 | 无最终颜色长历史，可清楚保留可见轮廓；不能恢复当帧漏掉的亚像素细线，也不能消灭随机光照方差 | 优先做清晰度基线和候选默认 |
| SMAAT2x + 同一信号降噪 | 两相位 jitter、两帧重建，改善亚像素覆盖；仍需正确 motion/disocclusion | 1x 的细线/运动轮廓不稳时优先比较 |
| 改进 native TAA | shading alias 与亚像素重建能力较强；需处理低通、裁剪与光照置信度 | 保留作公平对照和质量选项，不能立即判定一定输给 SMAA |
| SMAAS2x / SMAA4x | 增加空间 MSAA，4x 还结合时间采样 | deferred GBuffer 的多样本存储/着色/resolve 改造较大，本轮不优先 |

原版 SMAA 分为边缘检测、权重计算、邻域混合。**原版 T2x 的历史是上一帧独立 SMAA 空间结果，不是最终 temporal 输出的递归长历史**；两帧也不等于充分的 Monte Carlo 降噪。[作者论文](https://www.iryoku.com/smaa/downloads/SMAA-Enhanced-Subpixel-Morphological-Antialiasing.pdf#page=6)、[官方 Demo](https://github.com/iryoku/smaa/blob/71c806a838bdd7d517df19192a20f0c61b3ca29d/Demo/DX10/Code/Demo.cpp#L677-L702)、[resolve](https://github.com/iryoku/smaa/blob/71c806a838bdd7d517df19192a20f0c61b3ca29d/SMAA.hlsl#L1313-L1345)。Filmic SMAA 是另一种演进，不能把其能力当作原版 T2x 自带。

本项目要移走最终 TAA，必须补齐 GTAO 的稳定化：目前 AO 每帧随机旋转/步进，直接写 AO，缺少独立重建。XeGTAO 官方自己也明确在空间去噪后借最终 TAA 累积 temporal noise；不能只因改用某个 GTAO 实现就假设不再需要它。[XeGTAO](https://github.com/GameTechDev/XeGTAO/blob/a5b1686c7ea37788eeb3576b5be47f7c03db532c/README.md)。

建议在原生分辨率下，先固定信号降噪质量，再对照三个最终 AA 选择。SMAA 的色域采样和 LUT 契约按原实现遵守；UI 在场景 AA 后合成；AA1x 不抖投影时，光照自己的随机序列仍可继续。不要把 TAA→SMAA→FXAA 全部串起来。锐化只能最后微调，不能解决错误重投影或照明方差。

## 7. 深度：保持 FP32，补齐映射与辅助通道

| 数据 | 当前 | 建议 |
|---|---|---|
| 主深度 | D32_FLOAT，forward-Z | 保持 D32_FLOAT，统一 finite Reversed-Z |
| 历史深度 | R32_FLOAT device depth | 保持完整 FP32 |
| Hi-Z | RG32_FLOAT min/max device depth | 两分量都保持 FP32；同步解释反向后的近远关系 |
| 预期上一帧深度 | motion.z 的 FP16 深度残差 + 当前 R32 depth | 由 GBuffer 直接写独立 R32 previous-depth，通过统一 helper 消费，移除旧残差契约 |
| DDGI 距离矩 | RGBA16F 中的 x/y | 评估改 RG32F；仍是 8 bytes/texel，可避免为两个实际距离矩保留无效 z/w 通道 |

格式依据：`renderer.cpp:360,570`、`scene_lit.cpp:170–193`、`lit_gbuffer.hlsl:26`、`lit_common.hlsl:54`。残差方式比保存 FP16 绝对深度有利，尤其静态 residual=0；但一般运动并非无损。新用户约束下应消除这条辅助精度限制，不能只保证主 DSV 位数。

在标准 D3D12/DXGI 深度附件中，D32_FLOAT 是最高常规浮点深度格式；没有通用 D64_FLOAT DSV。D32_FLOAT_S8X24_UINT 的“64 位元素”是 32 位 depth + 8 位 stencil + 24 位空位，不会增加 depth 精度。最终硬件能力由 RHI 查询确认。[Microsoft 格式表](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)。

透视 `d=a+b/z` 将远深度挤在 1 附近；float Reversed-Z 把远处移向 0，利用浮点分布显著改善精度。应在权威投影构造处生成矩阵，不先算 forward depth 再 `1-d`。首轮保留 finite far，避免顺便改变相机裁剪合同；orthographic 没有同等 1/z 收益，但要统一 clear/compare/背景方向。[NVIDIA 深度精度分析](https://developer.nvidia.com/blog/visualizing-depth-precision/)、[Upchurch/Desbrun 原论文](https://www.cs.cornell.edu/~paulu/tightening.pdf)。

需要整体迁移：clear/background=0，GREATER/GREATER_EQUAL，普通渲染与 Scene Lit、网格/线框、Capture、反投影视线、Hi-Z、SSR/GTAO 和历史拒绝。不能只翻比较操作。深度匹配/厚度用明确 world/view 单位，或把这些容差转换为局部 device 容差；固定 device epsilon 不是固定几何距离。

这里的“精度”还包括深度值的位数、采样空间覆盖和几何数据完整性三个不同维度。Hi-Z 上层分辨率变小是保守区间表示，不能用普通平均 downsample 丢掉边缘；即使每个深度都是 FP32，单层深度仍看不到背面或屏外几何。FP32 也不是解决光照随机方差的降噪器。

## 8. 屏幕空间 GI 能否补细节

**可以作为近场补充，不能把它当 DDGI 稳定性修补。** 它适合可见小物附近的颜色反弹、接触间接遮蔽、自发光邻域贡献；GTAO 当前仅给遮蔽标量，不会生成这种颜色传递。DDGI 继续负责屏外及较稳定的世界空间低频照明。

值得比较的两种局部方法：

- 沿已有 Hi-Z 做短程屏幕空间 diffuse rays；复用深度遍历与几何有效性，但增加独立漫反射采样/重建成本。
- 将当前 horizon AO 路径评估为 visibility-bitmask 的局部 SSIL：同时表达遮蔽方向和屏幕命中颜色，更贴合已有 GTAO 结构。原论文针对薄表面无限厚假设改进，但仍用近似厚度，也存在漏光、单层遮挡缺失、噪声与视角依赖。[SSIL 作者论文](https://arxiv.org/abs/2301.11376)、[正文](https://arxiv.org/html/2301.11376v2)。

后者更值得先做有边界的对照，**尚未据此批准全面替换 GTAO**。两个要守住的融合问题：

1. 不能直接 `GI = DDGI + SSGI`；相同间接贡献可能重复计入。需要明确近场替代范围、可信度和缺失时回退，同时避免再乘一层重复的 GTAO 遮蔽。
2. 当前 DDGI 存的是 **irradiance**，并不是每个方向的入射 radiance。八面体图的方向是接收法线方向，对半球已经积分过；不能把某 texel 直接当“这一条 ray 的屏外 radiance”。将它除以 π 作为各向同性背景是额外近似，必须显式说明。正确的方向/近远场分配可能需要更强表示；仅混合两幅图不会自动获得无偏能量。

首轮原型应读取 scene-linear、去除最终反射/UI/曝光的可信照明源，限制为局部一次反弹；自引用的最终 GI 颜色容易重复反馈。融合保持 DDGI 基线，屏幕方法只在明确可见、尺度/几何可信的范围替代相应贡献；“残差修正”必须先定义被减去的基线，不能发明一个没有可计算意义的减法。

Lumen 证明屏幕信息与世界空间信息可以配合，但它还有 surface/radiance cache、重要性采样和专门 final gather；不能据此认为给现有 DDGI 加一张 SSGI 就等同 Lumen。[Epic 原作者技术说明](https://www.unrealengine.com/en-US/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen)。镜头边缘、物体出屏、被遮挡及相机转动是这一步的必测终点。先把目前的噪声源稳定，再引入这个新信号。

## 9. 可实施的演进批次

| 批次 | 可观察成果 | 边界 |
|---|---|---|
| A：稳定基础 | 移动一盏灯不再让整幅画面历史归零；irradiance/distance 分开；完整 FP32 depth 契约与 Reversed-Z | 复用快照/稳定键与现有 RHI，保留远裁剪和 MatCap/网格行为 |
| B：干净光照 | 实际直接光信号重建；DDGI 稳定几何射线、合理预算/历史；SSSR 支持度/统计一致/多尺度；AO 独立稳定 | 可牺牲少量光照高频，不能跨几何边界糊材质；不盲目移植多套 denoiser |
| C：清晰 AA | 原生 SMAA1x、SMAAT2x 与改进 TAA 同轨迹对照，形成单一可选 AA policy | 用模式取代 boolean，不新建 Renderer，不把多个最终 AA 连串堆叠 |
| D：局部间接细节 | 受控 SSIL/SSGI 与 DDGI 融合对照，验证颜色反弹和薄物体接触质量 | 不重复能量/遮蔽，明确屏外回退；不顺带扩展完整 Lumen/大世界 |

A/B 应先解决用户看到的现象；C 决定默认 AA；D 是经过验证再保留的增量。ReSTIR DI、大世界级联、RT 反射、方向 HDR 环境等另列后续范围，本次不会以继续堆功能代替质量进化。

## 10. 成本与验收终点

此前 RX 6800 XT 原生 2560×1440 固定性能场景、240 个完成样本，阶段 GPU 耗时之和 P95=4.24867 ms；其中 DDGI P95=.40032 ms、HDR/SSSR=1.42144 ms、TAA=.48304 ms。**这是之前的特定小场景，探针配置也不同于打开的默认示例，不能作为新算法或完整游戏的帧时保证。** [已有验收证据](../modules/renderer/acceptance.md)。

资源算术：1440p 新增一个 R32 通道约 14.06 MiB；把 DDGI distance 从 RGBA16F 换成 RG32F，在方向分辨率相同时每 texel 都为 8 bytes。提高 distance 方向分辨率、增加光照 moments/history 或 filter scratch 才进一步增加存储。释放被替代的 per-light shadow history 能抵消部分新信号开销。以上不含 pitch、驱动开销及待退休资源，不能称实测显存。

只扩展现有端到端入口，不建立 mock/单元测试或另一套通用基准系统。此前稳定测试主要比较预热后间隔 8 帧的图像，并有刚体移动引起照明改变的断言；这不足以保证连续移动强光的局部反射/半影稳定。新验收必须包括：

| 场景 | 观察和量化 |
|---|---|
| 静态强面光/彩灯/小亮物 | diffuse、GI、reflection、penumbra 各 ROI 的时间方差、亮脉冲、残留斑块；不能只看全图平均 |
| 灯连续移动/强度渐变/骤开骤关 | 历史权重与拒绝原因、探针更新时间分布、达到目标光照的响应时间；不接受反复整场重置 |
| 物体/遮挡物移动 | 解除遮挡恢复、间接可见性、拖影、probe 分类/重定位状态 |
| 镜头亚像素平移和转动 | 轮廓锐度、细线保留、specular 闪烁；同样的 scene、native resolution 与运动轨迹比较 AA |
| 反射 hit/miss、出屏和薄几何 | 局部闪烁、缺失回退、roughness 一致性，不把随机命中切换直接判为错误历史 |
| 深度 | 远处近共面物、near/far 边界、正交/透视、网格和线框遮挡、previous-depth 对应 |
| SSIL/SSGI（到 D 再做） | 局部彩色反弹、能量重复、重复 AO、出屏与相机变化回退 |

必要诊断是同一 accepted-frame 序列的 raw direct、raw visibility、resolved lighting、probe、SSR raw/resolved 与 final AA；这些为端到端失败归因服务，不另建长期监控框架。光照参考需要足够样本且检查收敛，现有 64 样本诊断路径不是任意场景的绝对真值。GPU 阶段计时、逻辑资源和图像质量一起评估；不能通过单纯扩大模糊半径或放宽稳定阈值宣布成功。

## 11. 交付状态

本轮产出是以上带证据的演进评估。没有把建议描述成新实现，也没有因为研究重跑已通过的功能验收。已向 RHI 负责人同步 FP32/Reversed-Z 与降噪接入边界；未要求其现在实施，未绕 RHI 调用原生 API。下一次实现应以 A/B 的明确结果为起点。
