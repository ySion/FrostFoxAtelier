# ReSTIR DI 与光追阴影：面向 FrostFoxAtelier 的增量研究

日期：2026-10-11。面向 RX 6800 XT、DX12 inline RayQuery、所有 GPU 能力经项目 RHI 的约束。本文仅研究与建议，不代表已集成、已验收或已测得本机性能。项目现状由主任务核查；本文不把外部 sample 的能力当作本项目已有能力。

## 结论

**值得纳入 ReSTIR DI，首先服务大量局部灯和发光网格的直接光。** 对低 poly 俯视 roguelike，重点收益是让真正有贡献的灯获得阴影查询预算，以及让发光几何参与直接照明。DDGI 继续承担漫反射间接光。第一批应完成光源身份、有效采样分布、运动与历史契约、当前帧最终可见性、独立光照信号；随后才按 RX 6800 XT 的实测选择降分辨率、ReGIR 或可见性缓存。

不能以“能接受收敛延迟”替代运动正确性：静态光照噪声逐渐收敛可以接受，移动角色身后的旧阴影、开灯后残留黑斑、相机移开遮挡物后露出旧画面是需要单独解决的错误。

## 一手资料确认的边界

| 名称 | 负责的工作 | 对本项目的含义 |
|---|---|---|
| ReSTIR DI | 对直接光样本做重要性重采样与时空复用 | 选择值得查询的灯/光源位置；不自动提供间接光、材质、加速结构或去噪 |
| ReSTIR GI / PT | 分别复用间接光样本或更一般的光路径 | 需要额外的路径追踪与路径复用契约，不是“开启 DI 后顺便得到”的能力 |
| ReGIR | 构建世界空间的光源候选分布 | 可以供屏幕 DI 和 probe-hit 着色共享，适合大而分散的光源场景 |
| RTXDI | 提供上述算法的运行库、桥接接口与示例 | 运行库可择需集成，示例渲染器和 Donut/NVRHI 不是必须迁入的架构 |

上述区分来自 [RTXDI Integration](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md#four-algorithms-in-one-sdk) 与 [SDK 包组成](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/README.md#package-contents)。原论文同时给出无偏和有偏估计器，有偏版本用能量损失交换较低噪声；“ReSTIR”这个名字本身不保证无偏、无斑块或固定成本。[原论文与作者摘要](https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal.html)

**AMD 与 RHI 可行性：** NVIDIA 官方明确称 RTXDI 可运行于具备硬件光追能力的任意 GPU；SDK 示例支持 D3D12/Vulkan。RTXDI 核心不调用图形 API，应用实现材质、G-buffer、资源和光追回调。因此现有 RHI 的 compute + RayQuery 是合理接入边界，不能因 SDK 名称含 RTX 就推断只能运行在 NVIDIA。[官方兼容性说明](https://developer.nvidia.com/blog/lighting-scenes-with-millions-of-lights-using-rtx-direct-illumination/)，[D3D12/Vulkan 示例支持](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/README.md#vulkan-support)，[运行库职责](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md#integrating-rtxdi)

NRD 官方也允许用引擎自身能力实现其 API；它返回资源、管线和 compute dispatch 描述，由应用创建资源和执行，不自行调用图形 API。官方源码还明确记录了 AMD/Linux 测试贡献。**可在兼容 AMD 的 D3D12/Vulkan 引擎中集成，与“RX 6800 XT 上已通过性能和画质验收”是不同结论。** 本项目应把 NRD 描述翻译到已有 RHI；不采用需要向外暴露原生句柄的 NRI 包装接法。[NRD API](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/README.md#api)，[官方 AMD 测试记录](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/Include/NRD.h#L25)

## 当前代码对应的演进位置

基线 `72843f7` 的本地核查：

- [SceneLit::validate](../../src/renderer/scene_lit.cpp) 对整帧解析灯数执行 `<=256` 校验，超限拒绝候选帧；不是静默截掉第 257 盏。发光三角不占这些解析灯槽。
- [直接光 shader](../../src/renderer/shaders/lit_direct.hlsl) 用 16×16 tile、每 tile 64 个索引；溢出会遍历整帧灯表，保留贡献但增加工作量。当前解析灯按实际候选逐盏求值，而不是时空 reservoir。
- 阴影预算按全局灯表分配，`shadow_light_budget` 默认 4、上限 16；未进入预算的灯仍提供无阴影直接光。这是当前显式质量策略。ReSTIR 的目标是让有贡献的光源获得每个接收点的查询预算，不能沿用“其余灯禁用阴影”的标志后声称所有采样灯都完成可见性。
- 发光几何已有面积/亮度 alias 与 streaming RIS、真实可见性，尚无 temporal/spatial reservoir。应扩展已有数据与 sampler，而不是建立第二套发光物体目录。
- 当前直接光已输出去材质的 diffuse/specular 两个信号及独立时间/方差滤波；现有 motion、几何法线、材质与光照变化提示可继续复用。reservoir 状态和去噪历史仍应各自持有、分别失效。

当前被观察的 Dungeons 场景只有一盏方向光、没有 Sky Light 环境，主要诉求是 DDGI 覆盖；不能用这一帧证明 ReSTIR DI 对 many-light 的收益。多区域分散灯和局部高度重叠的灯必须分别比较；新增 reservoir 也有固定带宽、内存和 pass 成本。

**必须保留的能量归属：** 屏幕 mesh NEE 目前只负责发光面直接漫反射，镜面发光路径由 SSSR 承担；probe 直接命中的 emission 不再重复注入，probe-hit 的 analytic/mesh NEE 负责后续散射。初版 DI 重采样应沿该分工输出，解析灯的 diffuse/specular 继续求值。如果随后让发光网格也进入 DI specular，必须同时定义它与 BRDF/SSSR 命中发光面的互斥或 MIS 权重，不能把两份直接镜面结果简单相加。此项是集成门槛，不是“接上库后自然解决”的细节。参见 [当前 Scene Lit 合同](../modules/renderer/scene-lit.md)。

## 建议采用的统一执行链

这是本项目的设计建议，而非官方规定的唯一布局：

```text
场景灯/发光几何 → 统一光源数据与身份、当前/上一帧索引、有效候选分布
                                      ↓
当前 G-buffer + 历史几何 + motion → 初始 RIS → temporal → spatial
                                                       ↓
                                 当前场景最终阴影查询 → DI diffuse/specular
                                                       ↓
                          光照重建/去噪 → 与 DDGI、反射、自发光合成 → AA

DDGI probe ray hit → 同一光源数据/采样函数 → 世界空间 RIS（后续可用 ReGIR）
                 → probe-hit 当前位置的阴影查询 → 更新 DDGI
```

默认光追不要求主可见性也改成完整路径追踪：当前 raster G-buffer 可以继续提供接收面，RayQuery 查询被选光源的阴影，DDGI 查询间接光。所有资源、加速结构、屏障、dispatch 与统计都由 RHI 管理；只在渲染算法层组织这些已有能力，不另建外部 GPU 生命周期。

屏幕 reservoir 记录的是特定接收面的候选、权重和复用历史。probe-hit 的位置、法线、可见性和采样域均可能不同，不能把屏幕 reservoir 当作世界空间光照缓存直接套用。官方明确把 RTXGI probe-hit 着色列为 buffer-less/world-space sampling 用例，给出的方案是 RIS/ReGIR。[RTXDI 世界空间采样](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md#world-space-sampling-and-regir)

SDF 或闭合体分类可以服务探针内外判定、占据和漏光控制，但“点处于何种空间”不能回答“接收点到这个光样本之间是否被挡住”。本轮不能用它们替代 DI 阴影。若以后用近似结构加速候选可见性，必须声明误差，且保守判断遇到不确定情况应保留可见候选；最终阴影仍遵守实际几何和材质语义。[可见性回调契约](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/RtxdiApplicationBridge.md#rab_getconservativevisibility)

## 光源、采样和历史：第一批不可省略的契约

### 光源种类与数量

- **零半径点光、零角半径方向光是 delta 光源。** 它们可以参与离散选灯，但不能用有限面积光的 PDF 或依赖普通 BRDF 射线“撞中它们”。保留显式的选灯概率与解析评估。有限角直径的太阳、矩形和发光三角形则有各自的连续采样域。官方明确区分点/方向光的硬阴影与面积光的接触硬化软阴影；其初始采样也分别处理 local、infinite、environment 与 BRDF 候选。[灯型与阴影](https://developer.nvidia.com/blog/lighting-scenes-with-millions-of-lights-using-rtx-direct-illumination/)，[采样策略接口](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/ShaderAPI.md#rtxdi_samplelightsforsurface)
- **矩形、mesh emissive 是优先受益对象。** 推荐保留同一光源登记与评估入口，在内部按灯型分派采样/PDF。发光网格采样必须携带实例、三角形/面片身份和面内坐标；拓扑变化后不能继续把旧坐标解释成另一片面。可见的自发光颜色与它投射到其他表面的 DI 是两个信号，合成时各计一次。
- **世界内超过 256 灯不是 reservoir 数组大小问题。** 单像素保留有限样本，不意味着场景只保存有限数量灯。用能覆盖当前相关场景的光源表和候选结构，避免固定上限直接截断贡献灯。预算限制应限制候选评估与查询次数。
- **离屏发光体仍可能照亮屏内表面。** 不可只从相机可见 mesh 列表登记光源，也不可把 frustum culling 当作光照影响范围。场景流送可以限定驻留范围，但范围必须覆盖潜在贡献；超出范围造成的能量截断应是明确的距离/质量策略。

后两项是由采样支持域推导的项目要求：候选分布若永远不给某贡献光源非零概率，时空复用不会凭空恢复它。官方 target-PDF 说明也强调不得把本应非零的贡献置零。[采样支持域与 target PDF](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/NoiseAndBias.md#target-pdf)

### stable ID、更新与 PDF

| 变化 | 建议的契约 |
|---|---|
| 重排、压缩或流送光源表 | 场景身份与 GPU 紧凑索引分开；维护 current ↔ previous 映射，不能用数组下标充当永久身份 |
| 删除、替换、复用槽位 | 旧身份失效；generation/revision 防止旧 reservoir 命中新灯 |
| 同一灯移动、变色、改变功率 | 保留可解释的身份与面内参数，用当前数据重新评估；需要旧分布时保留旧数据。不能只沿用旧 radiance/PDF |
| 新增灯、零功率灯重新点亮 | 本帧候选分布必须及时获得非零概率；新候选与必要的局部历史降权帮助及时发现它 |
| 发光 mesh 拓扑/采样参数化变化 | 使不再可解释的样本失效，不能仅更新变换矩阵 |

RTXDI 的 `RAB_TranslateLightIndex` 明确要求双向映射，不存在的光源返回负值；`RAB_LoadLightInfo` 区分当前/上一帧。这证明“稳定身份 + 帧内索引映射”是接入责任，不是库会替应用自动猜测的状态。[Light bridge](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/RtxdiApplicationBridge.md#rab_translatelightindex)

全局功率分布可用现有 alias table，或采用 SDK 的分层 PDF texture；关键是能够给出**实际抽样所用**的 source PDF，并和灯数据同代更新。alias table 是一种选择，不是 ReSTIR 的硬依赖。大世界中只按全局功率可能反复选中远处强灯：先使用现有空间分布/保守相关灯集合；真实场景仍难收敛时再加 ReGIR。官方说明 ReGIR 可同时改善大而分散场景的初始候选并减少 boiling，但有每帧构建和存储成本，也不在选灯时自动完成阴影。[PDF texture](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md#pdf-textures)，[ReGIR 约束](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md#world-space-sampling-and-regir)

### 几何与材质历史

temporal reuse 要以有效 motion 重投影，并验证深度、法线和接收面关系；spatial reuse 要拒绝明显不同的面与材质。disocclusion、相机切换、尺寸变化、不连续的材质/几何修改不能继承旧历史。保留本项目当前/上一帧 FP32 Reversed-Z 深度；若接入 NRD，另提供其要求的线性 viewZ，不能把 device-Z 直接当线性距离。不因 reservoir 压缩顺带降低深度精度。大世界原点移动必须一致修正位置、矩阵、motion 与历史。

光源稳定身份不代表阴影有效：即使接收面和灯都没动，另一物体也可能移入光路。因此需分别管理 **reservoir 有效性、可见性有效性、去噪历史置信度**，共享变化证据但不混用一个“历史有效”布尔值。平滑运动不宜每帧全屏清空；全局 reset 留给相机切换等事件，普通变化通过重投影、样本重评估与局部置信度处理。[重投影与复用接口](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/ShaderAPI.md#rtxdi_ditemporalresampling)，[深度/法线/材质参数](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/ReSTIRDIParameters.h)，[NRD 矩阵和原点约定](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/Include/NRDSettings.h#L91)

## 阴影质量与光线预算

**复用光样本，与复用它的旧可见性，是两项不同优化。** 第一批应在当前 TLAS 上重新查询最终被选样本的可见性；统一自相交 offset、ray 范围、双面、alpha cutout 和透明遮挡语义。这样可以保留 ReSTIR 的选灯收益，同时让当前遮挡变化进入结果。它仍不能独自解决候选贫乏或去噪滞后。

官方 `reuseFinalVisibility` 使用缓存年龄和移动距离等限制；阈值放大会增加偏差。最终被挡住的样本直接丢弃也会产生暗化偏差。`enableVisibilityShortcut` 是另一项跳过 temporal 校正光线的优化，依赖上一帧已经丢弃不可见样本的假设，不能把两项开关当作等价操作。[visibility API](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/ShaderAPI.md#rtxdi_getdireservoirvisibility)，[shortcut 的源码前提](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/ReSTIRDIParameters.h#L109)

**建议初版关闭最终 visibility reuse 和 visibility shortcut。** 先用正确当前可见性建立画质基线；后续若测得它们确实占瓶颈，再对稳定区域设置短寿命缓存，并在光源、遮挡物、材质或场景版本变化时强制刷新。仅靠屏幕距离小、缓存年轻，不足以证明光路没有变化。

归一化的偏差校正也要明确：basic 校正不等于完整无偏；ray-traced 校正增加额外查询，并且严格时序正确需要旧接收面、旧灯及相应旧场景可见性。只用当前 BVH 代替旧 BVH 是可选择的成本折衷，不能声称等价无偏。当前源码还提供 spatial Pairwise 模式，说明文档的旧“三档”描述并不覆盖全部当前 API；版本选择应锁定代码与文档一起核对。[偏差来源](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/NoiseAndBias.md#spatiotemporal-reuse-bias)，[当前模式枚举](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/ReSTIRDIParameters.h#L27)

“一个最终 reservoir”不等于“全管线每像素一条光线”。还应计入初始可见性、BRDF 候选、temporal/spatial 校正、置信度重着色，以及独立的 DDGI、反射、加速结构更新成本。原论文补充材料直接展示了不同 MIS 方案引入额外可见性查询的成本。[原论文补充材料](https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal-supplemental.pdf)

少灯区域未必从完整 ReSTIR 获益：reservoir 带宽、历史 G-buffer、重采样 pass 和去噪都有基础成本。建议保留同一直接光入口内的小集合精确评估策略，或把少量主方向光独立确定性评估；与随机采样灯集合互斥，避免重复计能量。阈值以本机测量决定，不设未经测量的“灯数超过 X 必快”。

## 去噪、AA 与低频更新

官方说明仅 temporal reuse 会形成过度稳定的斑块与移动亮簇；temporal 后加入 spatial reuse 有助于打散相关性。新显露区域还需要更多新信息，不能只拉长历史或加大模糊半径。因此推荐分开的 initial/temporal/spatial/final shading 边界，先调好信号再合并 pass；不把最少 pass 数当作最佳总成本。[去噪友好性](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/NoiseAndBias.md#denoiser-friendliness)

NRD 的 RELAX 面向 RTXDI 信号，REBLUR 也是可选 radiance denoiser；SIGMA 主要处理单灯阴影，不能直接把多灯 ReSTIR 的加权 radiance 当成一个普通 shadow mask。接入任一去噪器都必须先完成契约：分开 diffuse/specular、材料解调与回乘、线性 viewZ、世界法线、正确 motion，并保留自发光细节。尤其不要把 ReSTIR 选中灯的距离无条件塞给 specular hit distance；该距离可能来自 diffuse 主导的选择，破坏镜面运动重投影。[NRD 信号与 hit-distance 要求](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/README.md#noisy-inputs)

建议先让现有光照去噪链消费完整 DI 信号，确定瓶颈后再决定是否接 NRD；若直接选 NRD，优先把 RELAX 作为可比较候选，不预先宣称它比现有实现更快或更清晰。SDK 示例依赖不应变成引擎无条件依赖。

变化置信度是防拖影的独立工作：在对应的同一光样本上比较前后帧照明，可发现灯光运动、亮度变化和遮挡变化，驱动局部历史降权。官方 RTXDI 给出 reservoir 重着色方法，避免“灯总数变了，同一随机数恰好选到不同灯”造成伪变化。只有当前 BVH 时应选择前一帧样本向当前场景重着色的方向。[RTXDI Confidence](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Confidence.md#temporal-gradients-in-rtxdi)

**光照时序重建不以 TAA/T2x 开关为开关。** 用户选择 SMAA，仍然可以继续 ReSTIR 与照明去噪；AA 改变 jitter 时要同时遵守 motion/matrix 约定，并按需重置受影响历史。降分辨率照明用几何引导恢复，最终 AA 处理画面采样，两者各自验收。[NRD 与 upscaler 的交互](https://github.com/NVIDIA-RTX/NRD/blob/e06852bd545973cb224eb28820b891205cfc1624/README.md#interaction-with-upscaling-dlssfsrxesstaau)

允许低频更新的建议次序是：先降低稳定间接光/probe 的更新频率，再试稳定区域或较低分辨率的直接光采样。接收面、motion 和遮挡变化仍每帧可用；disocclusion、移动角色、灯开关附近立即补充新候选与查询。不能冻结整张直接光照图，再用最终 AA 掩盖时差。低分辨率、checkerboard 或更新频率改变，每次只引入一种变量；保留全分辨率当前可见性模式用于对照。

## 有限增量与验收终点

| 阶段 | 实际产出 | 暂不附带 |
|---|---|---|
| 第一批 A：统一输入 | 解除整帧固定灯数限制；统一解析灯和发光几何的身份、功率、采样/PDF、当前/上一帧映射；RHI 管理 GPU 数据 | 不并行保留另一套 light registry，不迁入示例渲染器 |
| 第一批 B：DI 闭环 | initial RIS → temporal → spatial → 当前帧阴影 → 独立 diffuse/specular；保留可对照的少灯/高采样模式 | 不同时上 ReSTIR GI/PT、最终可见性缓存和复杂空间层级 |
| 第一批 C：运动与诊断 | 正确 disocclusion、material/geometry/light 变化处理，局部历史置信度；可查看原始 DI、重建 DI、选中光源、M/age、历史拒绝原因、实际 ray/pass 成本 | 不靠无限历史或全局滤镜掩盖结构错误 |
| 后续按证据选择 | ReGIR 或已有空间灯结构、checkerboard/较低照明分辨率、NRD 替换评估、稳定区域 visibility reuse、pass 融合 | 任何一项都不承诺自动提升 FPS 或消除全部噪声 |

验收只需有针对性的端到端场景：强面光接触阴影、多个相邻发光 mesh、近距离离屏强光、多房间远超 256 灯、快速移动角色、相机平移造成 disocclusion、灯突然开关/删增/重排、遮挡物移动、SMAA 与 T2x 切换。画质检查保留低 poly 棱线，观察阴影响应、斑块、firefly、亮暗漂移和拖影；性能记录每个相关 pass、实际光线数、GPU 常驻历史内存和加速结构更新。

只有这些结果才能决定 RX 6800 XT 的分辨率、样本量、历史长度和低频策略。本文没有给出 FPS 预测，也没有把 NVIDIA 示例设备上的时间或加速比外推到本机。
