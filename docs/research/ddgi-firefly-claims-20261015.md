# DDGI firefly 建议的一手核查

日期：2026-10-15。前半为文献与源码核查，本身未运行GPU或修改生产代码；末节记录随后独立完成的地牢端点归因。用户报告的 Direct lighting 17–20 ms 性能诊断另见[地牢 DI 研究](di-dungeon-diagnosis-20261015.md)，不把一般建议直接当作已证实修复。

## 结论

建议指出的“近场高能、重要性抽样大权重、太阳小受光区、历史响应误识别”都是值得区分的机制，但**“五步彻底根治”没有一手依据**。RTXGI 的感知编码与限幅确实可以压制闪点，代价包含非线性平均、响应变化与能量偏差；它们不能直接移植到本项目当前线性运输、独立呈现的合同中。

可采纳：按局部源/方向光/反馈分开记录贡献，核对真实完整 proposal 概率与近场距离；保留方向光独立采样；改善采样分布和分层覆盖；以已知场景编辑驱动响应。可评估但须明确质量取舍：只作用于独立呈现层的感知滤波或异常值抑制。不能照搬：`p≥.02` 的分母替换、无补偿 W clamp、GI 独有 .25–.5 m 半径、在运输历史恢复 gamma=5、要求 25% 射线变化才响应，以及对所有点光逐盏精确求和作为多灯场景的默认解。

## 固定版本与证据口径

本轮通过 `git ls-remote ... refs/heads/main` 确认 RTXGI-DDGI 的主分支为 **`f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6`**。下列代码链接均固定此提交；其 ChangeLog 顶部为 SDK 1.3.7，特别记录了 brightness threshold 的修复。论文与此 SDK 不是同一实现版本，不能把两者的阈值伪代码混为一谈。[固定提交 ChangeLog](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/ChangeLog.md)

依据包括 2019 DDGI 原始论文、2021 production 扩展论文、NVIDIA 固定提交源码与 Brian Karis 原文。下面明确区分外部事实、数学推导和本项目代码事实；对文献中的性能结果不作本机换算。

## 1. RTXGI 实际做了什么

### 1.1 Gamma 编码、历史混合与查询不是一项操作

固定版本先把射线辐射做方向余弦加权与归一化，再对新照度的存储值取 `1/gamma` 次幂，**随后才**与旧编码值混合。默认 gamma=5、hysteresis=.97；旧编码 RGB 完全为零时将 hysteresis 降为零。[默认参数](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h#L103)、[混合源码](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L500)

查询时，采样到的编码值先取 `gamma/2` 次幂，以保留近似 sqrt 域的空间混合；八探针加权归一后再平方回线性，并乘回原先移出的 `2π`。特定低精度 U32 格式额外乘 `1.0989`，注释说明其目的为补偿该格式的精度损失。**这个常数不是对任意 gamma、clamp、RIS 或场景的能量补偿。** [Irradiance.hlsl:178](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl#L178)

2021 论文 §4.2 的动机是感知上的明到暗收敛；作者实验选择 gamma=5，并报告对小而亮源造成的低频闪烁有附带抑制作用。这支持“有效的感知工程手段”，不支持“免费且不改变均值”。2019 论文的原始更新描述是直接 alpha 混合，hysteresis 取 .85–.98；不要把后来的 gamma 编码倒写成 DDGI 必需定义。[2021 论文 §4.2](https://jcgt.org/published/0010/02/01/paper-lowres.pdf)、[2019 论文 §4.3](https://jcgt.org/published/0008/02/01/paper-lowres.pdf)

**数学推导：** 忽略附加阈值、量化及空间插值，gamma 混合等价于有限权重的幂平均：

`E_out = (Σ w_i E_i^(1/5))^5`，其中 `Σw_i=1`。

这不是几何平均（几何平均是幂指数趋于 0 的极限），也不是线性照度平均。由幂平均不等式，它不大于同一组非负观测的算术平均。例：0 与 100 等权混合得到 `3.125`，算术平均为 `50`，几何平均为 `0`。这只是数值例子，不是本项目误差测量。常量输入在理想条件下仍可收敛到常量；风险发生在带方差的有限观测与递归传播，而不是声称任何稳定值都必然被乘固定暗化系数。

若只是压缩存储、在混合前完整解码回线性，则不产生上述非线性平均；但这也失去 gamma 历史的主要闪点抑制效果。本项目运输缓存已是 FP32，恢复编码不能以“避免低位宽存储丢失”为直接理由。幂函数还有实际指令成本，是否可忽略须测量。

### 1.2 两种阈值的当前实现

| 固定 SDK 行为 | 精确含义 | 不能推导成什么 |
|---|---|---|
| `max(oldEncoded-newEncoded) > probeIrradianceThreshold`，默认 .25；令 h=max(0,h-.75) | 至少一个编码颜色通道下降超过阈值时加快历史响应 | 不是线性照度相对变化率，也不是“25% 射线改变”；没有数射线。 |
| `luminance(newEncoded-oldEncoded) > probeBrightnessThreshold`，默认 .10；delta 乘 .25 | 编码域正向亮度跃迁被减弱，之后仍乘 `(1-h)` | 不是对每条 ray 设上限；也不是把最终增量硬限制到 .10。非常大的增量乘 .25 后仍可能很大。 |
| 暗化时对 lerpDelta 施加至少 1/1024 的有效步长，但不越过 delta | 避免低位宽舍入使暗化停住；源码对 FP32 也使用该规则 | 不是保持线性 HDR 均值的证明。 |

以上发生在 `pow(new,1/gamma)` 之后。参数注释使用了 ratio 一词，但当前执行代码直接比较编码值差，未除旧值。颜色通道下降检查与亮度上升检查并非同一个对称误差指标；不能用“绝对亮度差触发统一自适应”概括它。[ProbeBlendingCS.hlsl:515–550](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L515)

2021 论文 §4.3 另给较早的启发式：照度变化幅度达到 .25 时降低 hysteresis .15、达到 .8 时设零；还讨论应用知道小灯变化、日夜突变、几何大变时主动调整。这些数值与当前 SDK 代码不一致，必须标明版本。论文也说未穷尽所有内容；§8.1 明确承认小亮源仍有 ghosting，全局强化响应会使其他区域不稳定。[2021 论文 §4.3、§8.1](https://jcgt.org/published/0010/02/01/paper-lowres.pdf)

### 1.3 SDK 负责缓存，不规定宿主必须用哪种直接光积分器

SDK test harness 的 probe hit 通过 `DirectDiffuseLighting` 求直接漫反射，再加缓存反馈。其 Lighting.hlsl 对方向光、spot、point 分别执行逻辑；近场 falloff 是 `1/max(distance,1)^2`，方向光可见性用 `TMax=1e27`。这是一套示例着色策略，不是 RTXGI 公共接口要求点光永远不能进入离散源 RIS 池。[Lighting.hlsl:30](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl#L30)、[方向光:167](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl#L167)

同一示例把反馈 albedo 限到 .9，且在写 frontface ray data 前 `saturate(radiance)`。但 SDK 的 FP32 `DDGIStoreProbeRayFrontfaceHit` 本身直接存传入 RGB，不强制 saturate；低精度路径另有打包和弱值阈值。因而必须区分**示例调色/数值范围**与 **SDK 存储合同**。本项目以物理单位和线性 HDR 工作，不能因为 NVIDIA 示例截图稳定，就原样裁掉所有大于 1 的辐射。[ProbeTraceRGS.hlsl:193](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl#L193)、[ProbeRayCommon.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/include/ProbeRayCommon.hlsl)

## 2. 对具体建议的判断

### “点光 1/d² 奇点，所以 GI 半径改成 .25–.5 m”

理想点光存在近场奇点；有限源或有界衰减可避免无限值。但是本项目当前已经使用 `max(distance²,max(.0025,radius²))` 作解析局部源衰减分母，并在距离平方不合法/为零时不求值，因此不能先把现场描述为“尚未防护的无穷奇点”。`.0025 m²` 相当于 5 cm 距离地板。该保护仍允许近场高能，尤其光源靠近受光面时；是否是闪点主因需观察具体 hit distance、源身份、贡献与 q。[lit_light_sampling.hlsl:71](../../src/renderer/shaders/lit_light_sampling.hlsl#L71)、[133](../../src/renderer/shaders/lit_light_sampling.hlsl#L133)

GI 专用 .25/.5 m 下限会改变同一灯的运输模型，并使直接可见光与间接注入不一致。以原 5 cm 地板、同强度且进入两种地板范围的样本为例，新分母可令贡献降到原来的 1/25 或 1/100；这是模型改动而非仅数值修复。如果实际产品需要柔化，应明确灯源尺寸/统一衰减合同并保持能量与源采样一致，或把 GI 降能作为显式近似选项，而不是静默只改 secondary hit。不能由原厂示例的 1 米地板推出本项目通用正确半径。

### “RIS 小 PDF 会放大，所以 p≥.02 / W clamp”

小 q 下的 `f/q` 可能很大，表示 proposal 与真实贡献不匹配；单看 W 大不能证明错误，必须同时看最终 `W×f×visibility`。正确补偿可能正是维持亮源均值所必需。

对离散源 PMF，要求 N 个源都满足 p≥.02，在 N>50 时不可能归一化。对连续 PDF 则还要考虑采样域和单位，不能拿无量纲 .02 直接作为跨域下限。若仅把分母改成 `max(q,.02)` 而不改变真实选源分布，估计会系统性压低低概率贡献；若截 W 或只截正向高能样本，同样引入偏差。保持所有源支持的合法做法是修改并归一化实际 proposal，同时用其完整概率求权重，而不是伪造 PDF。

本项目当前按真实整数配额 `mLocal=floor(3M/4)`、`mGlobal=M-mLocal` 生成候选，使用 `q=(mLocal/M)qNear+(mGlobal/M)qGlobal`；M=1 或无局部域全局回退。全局源 proposal 原有均匀支持项仍保留。此合同减少整批偶然全选近源的情况，但不保证重要源概率足够，更不保证低方差。若还有巨大的 f/q，应继续改 receiver 相关重要性，而非把其正确分母 clamp 掉。[lit_light_sampling.hlsl:256](../../src/renderer/shaders/lit_light_sampling.hlsl#L256)、[271](../../src/renderer/shaders/lit_light_sampling.hlsl#L271)

### “directional 与点光永不混池”

将少量高影响方向光独立评估是合理分工，本项目已经这样做，避免太阳参与大量局部源的离散选源竞争；它不构成“点光不得与矩形/emissive 共用完整域”的定理。继续把全部点光独立求和会把其工作量推向每 hit O(N)，必须与当前 17–20 ms 的性能目标一起评估，不能默认是低成本修复。

如果建议实际只指“太阳与局部池分离”，可接受且已经实现；如果指“所有点光也必须从随机源池拿出”，需要独立收益证明。RTXGI 示例的直接求和不能作为面向 >256 源的性能保证。

### “太阳窄缝稀有高能；方向光 visibility 很短/可省”

窄缝形成的小太阳受光斑，可能很少被 probe 主射线命中；即使每个 hit 都独立评估太阳，这个**主方向采样**问题仍在。软太阳还可能有角域可见性方差。两者与“太阳在离散源池被漏抽”是不同机制，不应混用同一个解决方案。

方向光从表面沿来光方向检查远处遮挡，不是极短 ray。RTXGI 示例用 1e27，当前项目用 1e5 再扣 bias；accept-first-hit 允许遇遮挡早停，但没有证明遍历一定短或廉价。错误缩短距离会放过远墙/屋顶而漏光，跳过查询则改变阴影。[NVIDIA 方向光实现](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl#L167)、[本项目方向光](../../src/renderer/shaders/lit_light_sampling.hlsl#L116)、[visibility](../../src/renderer/shaders/lit_common.hlsl#L149)

### “adaptive hysteresis 把随机尖峰当场景变化；25% rays change 才响应”

对**依赖样本亮度突变**的响应检测，这个风险成立；早期生产论文也承认响应/稳定性的取舍。但本项目当前响应依据作者/几何变化代次，每方向消耗有效观测，运输/呈现分离；不能在没有证据时继续把已经移除的“随机峰触发快响应”当当前故障。

固定要求 25% rays change 没有从所查论文或 SDK 获得支持，疑似混淆了照度阈值 .25 或固定射线 backface ratio .25。小灯或窄缝的真实变化可能只占很小角域；该门槛会让它迟迟无法响应。旋转采样下相同 ray ordinal 也不对应同一世界方向，逐项变化计数不能未经对应关系就代表场景变化。应保留可靠编辑信号，并以稳定统计作辅助，而不是用硬百分比否决真实局部变化。[本项目方向响应](../../src/renderer/shaders/lit_ddgi.hlsl#L337)、[生产论文 §4.3](https://jcgt.org/published/0010/02/01/paper-lowres.pdf)

### “按历史/batch 均值 ray clamp 或 Karis 就能无害去火花”

历史阈值裁切会把真实新亮源限制在旧暗状态附近，产生响应迟滞；同批均值阈值会随稀有样本本身变化，其行为依赖 batch 大小和零样本比例。将非负样本 X 截到 K，损失为 `E[(X-K)+]`；未补偿尾部就改变均值，更多帧也不自动补回。反馈链会把被削弱的注入继续传播，不能仅看屏幕是否稳定。

Karis 原文采用亮度相关压缩/逆变换，等价于特定亮度权重的归一化平均，目的确实包含抑制 undersampled HDR fireflies；原文还指出按通道 tone map 可能影响颜色。**可逆变换不等于“变换→平均→逆变换”保留算术均值。** 例如灰度 0 与100，`w=1/(1+L)` 的归一化加权结果约 .9804，算术平均为50。它是有价值的感知重建工具，不是无偏运输积分器。[Brian Karis，Tone mapping，2013-12-15](http://graphicrants.blogspot.com/2013/12/tone-mapping.html)

当前架构允许在独立 presentation 研究这些近似，避免它们回灌运输。这只能避免多跳能量合同一起改变，并不保证显示均值或动态响应不偏；必须保留可读的线性运输结果作为对照。[DDGIPresent](../../src/renderer/shaders/lit_ddgi.hlsl#L373)

## 3. 对当前项目可执行但尚未实施的取舍

1. **先保留已正确的合同。** 保留线性运输、独立 presentation、方向光独立、整数配额完整 q、材质/几何可见性和实际变化代次；不要为压制峰值回滚这些机制。
2. **以已有诊断区分贡献来源。** 对问题像素/探针记录局部/方向/反馈亮度、命中距离、源种类、完整 q、W 与实际贡献；同一次闪烁检查来自近场高能、源分布失配、主射线撞到小亮区，还是缓存/权重变化。文献不能替代这一步。
3. **优先降低产生方差的原因。** 改善 receiver 相关 proposal 或对极高影响源作正确分层/多技术采样时，保持完整概率与支持；若改变近场模型，统一其单位、源尺寸、直接/间接贡献合同。稀有太阳受光区需要针对主方向覆盖，不是仅提高太阳选源概率。
4. **感知压峰作为显式折中。** 若真实预算内仍需产品级降噪，可首先在独立 presentation 评估有界异常抑制，检查静态平均照度、局部色彩、小亮源开关、遮挡移动和低更新预算恢复；不将“亮峰不见”单独作为通过标准。
5. **性能另按实际阶段归因。** Direct lighting 父阶段包含 fresh/temporal、spatial、shade/visibility、resolve/filter；DDGI 命中局部 RIS 虽共享 shader，却是另一个 workload。gamma/clamp 既不减少既有 shadow ray，也不自动减少屏幕 MIS/resolve 成本。当前17–20 ms需要对应完成样本和子阶段，不能从 firefly 建议推导性能瓶颈，更不能承诺五步同时根治画质与帧时。

上述为建议，没有新增实现或通过声明。现有有限 grid 也不应宣传为覆盖全部照明影响的索引；它只改变 proposal，是否足够仍由现场样本决定。

## 研究记录

- Exa 四个检索角度：RTXGI blending/阈值源码、DDGI 原始与 production 论文、示例灯光/visibility、Karis 原文。`sources_reviewed=20`（4×numResults=5），这是检索请求结果数之和，不是20篇全文证实。
- 本文引用并定向审阅10个独立一手来源：固定提交的7个 SDK/示例文件（ChangeLog、DDGIVolume.h、ProbeBlendingCS、Irradiance、Lighting、ProbeTraceRGS、ProbeRayCommon）、2篇作者论文 PDF、Karis 原文。另读取 RayTracing include 仅用于追踪源码依赖，未将其计入本文主要论据数。
- API 网页读取失败后，以 `git ls-remote` 确定提交；原始源码由 Exa fetch 获取，关键固定行号再由 raw HTTP 读取核对。未下载或改动第三方工作树。
- 本地引用对应本轮审阅时源码；其他代理可能继续修改，后续落地应以最终实现更新行号/合同。没有使用第三方教程或论坛作为事实依据。

## 当前地牢的端点归因补充（2026-10-15）

本节是随后使用真实地牢得到的新证据，不将上文的一般理论当成本场景的既定原因。Control增加显式探针选择和实际局部光端点/距离后，对probe 372、300、310各取得768个唯一完成样本。新副本viewport约2265×1246；像素坐标按视口中心移位，记录自身实际接收点，不与先前宽约2316 px的ROI直接合并。

probe 300在sequence 9619的该接收方向运输照度上升 **53.25%**，接收面上升 **6.70%**。同批局部点光样本source32/ray109的hit→light距离为 **.192114 m**，完整q **.394476**、W **1.18636**；其局部diffuse/specular亮度分别 **.443119 / .369809**，太阳项0、反馈.009526。另一峰sequence 9793运输上升27.05%，相关source34/ray87距离.158348 m、q .751316、W .770681。两者均未触及现有.05 m衰减下限。这支持优先调查稀有近灯表面方向的高能贡献，**不支持把这两个峰直接解释为未保护的零距离奇点或低q/W爆炸**。

floor记录中最大W约302.7的局部贡献为0，最小q对应的局部贡献仅.000126；太阳正贡献每批42～56条，未加atlas余弦权重的太阳批和CV约4.1%，局部specular批和CV约215%。右墙有更大的单ray亮度但接收面较平稳，因此不能按全方向ray最大值裁剪，也不能把未经卷积的批和相关性当作精确方差分解。

固定RTXGI提交的 [ProbeTraceRGS](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl) 调用 [DirectDiffuseLighting](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl)，使用albedo/π；本实现对secondary hit还保留解析灯GGX，因此包含漫反射接收面经光泽表面到光源的间接路径。低样本球面追踪可能偶然采中其窄峰，但本次主ray的diffuse仍大于specular，不能声称删除GGX即可根治。屏幕SSSR也不能自动补回漫反射墙面接收的这类间接光。diffuse-only、扩大软化半径或钳制均涉及物理模型/质量取舍，本批没有更改这些语义。

下一步可先对照128 rays×256 updates与256 rays×128 updates，保持32768条主射线预算，测方向采样方差与灯变响应；更多每probe射线会降低访问频率，不承诺免费改善。若保持现有光泽运输，应针对外层方向采样建立有完整PDF与全域支持的引导，而不是继续盲增内层选灯候选。具体诊断合同见 [Scene Lit](../modules/renderer/scene-lit.md)，原始数据与分析在 `out/di-reconstruction-20261015/probe-*-endpoint-inspection.json`、`endpoint-attribution.json`、`ddgi-secondary-specular-review.md`。
