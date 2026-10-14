# 渲染管线成本审计与工业研究 — 2026-10-13

范围：工业资料核验、`b0ede07d` 加 Pass 统计工作区的代码审计，以及本轮已接入的修复和优化。当前合同见[Scene Lit](../modules/renderer/scene-lit.md)，实际验收状态见[验收记录](../modules/renderer/acceptance.md)。此前已实现的 ReSTIR 合同见[实现记录](restir-di-implementation-20261012.md)，DDGI 覆盖、状态、反馈与调度合同见[覆盖研究](ddgi-coverage-next-batch-20261012.md)，这里不重建另一套方案。

## 本轮代码审查与取舍

五位 Astra 分别审查直接光与历史、DDGI、整帧管线、遥测生命周期、工业资料；另由 Astra 补充真实 Renderer 端到端用例。根节点审定改动，修复后再交叉复审。审查没有发现需要推翻当前广义 MIS 的证据，也未发现 Pass Table 的父子合计、120 样本窗口或异步序号混帧问题。

已接入的修复与精确工作删减：

- 公共 64 位表面身份门禁贯通 DI、SSSR 接收面与来源见证、GTAO、Local GI、最终 TAA。motion 有效只说明对象自身有前帧对应，不能证明重投影 tap 属于同一对象。SMAA T2x 的互补覆盖样本仍遵循独立 AA 规则。
- 遮罩 alpha/阈值/相关贴图变化进入 DDGI 距离与屏幕可见性响应；shadow bias 变化进入探针照度响应。没有增加 BLAS 重建。
- DI、DDGI gather、GTAO、Local GI 只派发实际视口像素，保持全局地址和 RNG；HDR、公共表面历史及后处理仍完成必要的全窗口定义。
- 直接光漫反射/镜面空间滤波共用每个邻点的几何读取和世界重建，保留两者独立权重。DDGI receiver 共用一次八面体采样 stencil，未删 LOS。
- DDGI target 合并为单份 RGBA32F；SSSR logical level 0 直接读 D32F，删除 full-resolution RG32F 复制。4096 probes 时前者少 4 MiB，1440p 后者少 28.125 MiB，均为逻辑存储而非实测外存流量。
- Scene Lit 无选中且无线框叠加时不提交必然被丢弃的编辑线；选中混合 batch 仅绘制连续选中区间，编辑实例 GPU 数据按实际消费者准备。移除同帧重复的完整资源统计扫描。

新增 fractional viewport 验收暴露了既有硬件边界：Direct3D §15.7 会对 viewport 四边独立向下取整形成隐式裁剪，扩大显式 scissor 不能补回右侧像素。RHI 负责人核对规范和 gfx 后确认传参无误。Renderer 现在统一导出整数外包硬件 viewport、像素中心 scissor 和最终 SV_Position 的 XY 补偿；逻辑矩阵、motion、Z/W 不变，表面、线框、公共深度、背景、网格和探针诊断复用此转换。首级 Hi-Z 采用独立编译入口，避免 1×1 层级的未用 SRV 与输出 UAV 别名。[Direct3D 功能规范 §15.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.7)

数值复审发现：仅检查有限输入不足以保证中间计算有限，原路径会接受单位颜色、强度 `1e38` 的点光，随后贡献或 RIS 累计溢出而显示为黑。现已在 World 与 Renderer 共享定义解析光输入域：每通道 `color × intensity ≤ 2^32`，矩形光的该乘积再乘面积也不得超过 `2^32`，进入编码前拒绝超界值。矩形先计算已验证的辐射量乘面积，再乘衰减，使用 `precise` 保留次序。该界结合现有衰减、BRDF、均匀 proposal 下限和至多 32 个初始候选，为解析贡献与初始累计保留 FP32 余量；不会把普通日光限制到 65504。

极大有限 target 的 MIS 比例共同缩放加权项，普通分支保留原乘法及累加次序。小 target 先乘 confidence 再除 scale，大 target 先除后乘，避免溢出或过早 FTZ；confidence 的 `1..640` 与至多三个输入是该界的前提。Astra 复审修正了“统一先除”会丢掉可表示微小权重的问题，并确认最终运算次序。World 自发光范围同时与既有 Surface / FP16 GBuffer 的 65504 上限对齐，拒绝操作保持原值。

**明确保留的数值限制：**上述论证限定于解析光 evaluator 和初始累计，不涵盖发光三角形的 `area/d²` 几何奇异情况、数学结果本身超过 FP32 的 reservoir W，或任意当前/历史 target 倍率。没有加入 W 夹取或静默降亮度。支持任意有限光源和几何需贯通缩放表示或另行约定有界 integrand；本轮范围检查及正常场景验收不等于这一全域保证。

未在本轮引入 active-tile compact、LDS 滤波、ReGIR、光栅视锥列表或 AS 物理裁剪。前两者需真实稀疏场景证明能摊销调度和寄存器成本；ReGIR 需要大灯数场景及提议支持合同；光栅剔除不能误用为 TLAS 剔除。保留三个 reservoir 的跨像素不可变读域，没有用原地 spatial 写入省显存。

成本评估使用完成 GPU 时间、实际射线数、draw/dispatch 数与逻辑资源字节；本轮没有电功率计量，不能把毫秒改善直接表述为瓦数或焦耳下降。

2026-10-14 的空闲 GPU 交叉实测显示本轮合计 p50 **9.29928 → 9.33224 ms**，整体持平：直接光滤波约 −4.03%、DDGI −2.42%、SSSR/HDR −4.28%，同时直接光采样和 GTAO 的成本上升。不能把所有差值都归因于某一个改动；身份门禁和数值保护属于同一候选。两种针对 MIS 热路径的源码等价改写均经测量否决并撤回，证据与边界见[性能对照](../modules/renderer/acceptance.md#2026-10-14-空闲-gpu-性能对照)。后续应优先实测共享 guide 数据及有效 tile 分类，避免从“少了源码循环”直接推断 GPU 更快。

## 工业研究的结论

优先处理**同一结果被多余像素、重复加载和不必要中间写回支付的成本**。主线程提供的当前固定场景量级为 DI 约 4.8 ms，其中 initial 1.29、spatial 0.80、visibility 0.25、resolve/filter 2.44 ms；DDGI 约 1.05 ms，其中 gather 0.90、update 0.145 ms。这些数只用来排实施顺序，不作为跨场景或帧尾保证。最先削候选数、减少 fresh visibility、降低探针更新频率，都没有对准目前最大的分项，而且会改画质或响应。

NVIDIA 的 RTXDI 性能报告明确把随机灯数据访问、跨像素 reservoir 复用及中间读写列为内存相干性问题；其工业优化包含预采样、布局及 kernel 合并。AMD 的 SSSR/denoiser 与 NVIDIA NRD 则提供了可核验的有效工作分类、tile 跳过、indirect dispatch、groupshared 复用路径。共同指向本项目的优化顺序：先统一有效计算区域，再处理逐像素反复读取和写回，最后才考虑改变采样分布或可见性近似。[RTXDI 性能报告](https://cwyman.org/presentations/GTC2021_RTXDI_Performance.pdf)、[AMD SSSR](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)、[AMD Denoiser](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/)、[NRD RELAX PrePass](https://github.com/NVIDIA-RTX/NRD/blob/9fc5c8e7/Shaders/Include/RELAX_PrePass.hlsli)。

### 1. 视口、空像素和可见表面的工作剔除

AMD SSSR 先分类需要求交的像素与需要降噪的 8×8 tile，再生成 indirect 参数，后续 expensive pass 消费这些列表；NRD RELAX 依据 tile 是否为天空及有效 rect 尺寸提前退出。这证明“有效工作域”能贯穿效果链，而不必所有 pass 都铺满窗口。[SSSR 的 Tile classification / Indirect arguments generation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)、[NRD PrePass](https://github.com/NVIDIA-RTX/NRD/blob/9fc5c8e7/Shaders/Include/RELAX_PrePass.hlsli)。

对本项目，先按成本从低到高分三层实施：

- **视口局部 dispatch。** 从 viewport 原点和 extent 派生 dispatch，shader 在局部 thread 坐标上界检查后映射到原有全局像素坐标。比“全窗口 dispatch 后逐线程判断 viewport”少发出组，仍保留现有纹理地址、随机数 seed 和历史坐标。G-buffer 历史、motion、SSSR 层级深度、AA 与滤波边界都必须明确使用哪个 rect；边界邻域不能读取编辑器 chrome 或旧帧垃圾。只缩 dispatch 不会自动减少全窗口资源驻留。
- **有效 surface tile。** 在已有读取深度/身份的阶段顺便生成 tile 是否含合法接收面的位；DI、DDGI gather、几何引导滤波共用相同 surface 有效性定义。第一步可仅做整组一致 early-out，避免增加一次列表构建、原子计数和 indirect 参数生成。如果活跃 tile 比例高，列表本身可能不值得。
- **稀疏列表与 indirect。** 只有真实空域比例和后续 pass 数足以摊销时，才添加 compact tile/ray list。保持 tile 内二维局部性，避免把整个图像散列成相干性很差的逐像素队列；RHI 缺能力时在现有入口补 indirect 契约。

以上为对公开实现的项目推导。它们保留每个有效像素的候选数、邻居、fresh visibility 与滤波 taps，较容易做到相同随机序列和相同结果。被跳过的像素必须有确定的背景/无历史状态；仅仅不写会留下旧 reservoir 或旧颜色。包含 groupshared barrier 的 kernel 不能让部分 lane 在 barrier 前单独退出，越界 lane 应参加同步并提供无效值。滤波 tile 还要加载有效 halo，不能把“邻 tile 没有输出任务”误解为“不需要它的输入”。

**可见表面剔除只影响接收者工作，不能据此删除光源和 RT 场景。** 屏外灯可照亮屏内表面；屏外物体可投影；屏外探针仍参与反馈。Remedy 的 Northlight 资料同时展示主视图 meshlet/遮挡剔除和 RT 对大范围场景实例的需求，不能只引用前半段。[Northlight GPU-driven rendering](https://www.remedygames.com/article/how-northlight-makes-alan-wake-2-shine)、[PS5 Pro 工程说明](https://www.remedygames.com/article/alan-wake-2-on-playstation-5-pro---behind-the-scenes)。

### 2. ReSTIR：优先压重复工作，保持估计器和 fresh visibility

当前三份 `24 B/pixel` reservoir 在 2560×1440 下逻辑容量约 253.1 MiB；一次全屏 24 字节写入是约 84.4 MiB。这只是按布局计算的逻辑数据量，不是显存实际分配，也不是实测外存流量。它足以说明 reservoir 地址域、布局和生命周期值得检查，却不能推出压成两份就一定正确。

RTXDI 集成文档提供独立和 fused 流水线，并说明某些简单时空组合可用两个 reservoir 数组。当前项目的空间阶段读取同帧 immutable scratch，最终结果跨帧保留；这个依赖与 fused spatiotemporal 从前帧取邻居不等价。因此可优先融合**没有跨像素依赖的同像素尾部工作**，减少重复 material/BRDF、被选源解码与颜色中间写回；不可把 spatial 原地读写作为“省一份 buffer”。是否消除某个 buffer，必须由资源最后读取点和历史交换证明。[RTXDI Integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)。

预采样把随机光源读取移出每像素内循环，以块内共享的随机候选池改善缓存行为；这是 GTC 报告里直接针对 initial sampling 的方法。若本机 initial 继续显著，且真实关卡灯数远大于当前小场景，可以复用同一源身份、功率 PMF 和求值 API 加入候选池。重采样概率与权重必须完整传递；不要把每 tile 的灯列表当成硬截断。相干候选会改变样本相关性，应检查方差和动态响应，不能只验证平均亮度。[RTXDI 性能报告](https://cwyman.org/presentations/GTC2021_RTXDI_Performance.pdf)。

ReGIR 可以进一步改善大范围灯的提议分布，但官方说明格子存的是粗粒度平均 irradiance 提议，不是精确可见性；结构还要每帧重建。小场景、少量灯的本轮基线未证明需要它。若以后接入，应作为同一候选分布的内部生产者，同时服务屏幕 initial 与 DDGI hit，并保留覆盖所有合法贡献的 fallback/support；不恢复已退役的硬上限屏幕灯域。[RTXDI ReGIR](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md#regir)。

**不把旧 visibility 当作当前遮挡。** GTC 报告明确承认更激进 visibility reuse 会使阴影变亮；SDK 也区分采样/复用中的可见性与最终 shading。当前 visibility 约 0.25 ms，不值得优先牺牲严格遮挡。receiver 与 light 身份没变不代表遮挡体没变；仅以两端身份缓存 LOS 不能覆盖门、动态遮挡体、alpha-mask 变化。当前 generalized balance MIS 中零贡献样本的分布责任也不能因为本次遮挡而被删去。先保留既有估计器，只移除重复解码/求值和确定无效工作。[RTXDI 性能报告](https://cwyman.org/presentations/GTC2021_RTXDI_Performance.pdf)、[现有估计器合同](restir-di-implementation-20261012.md)。

### 3. DDGI：当前优先 gather，不重写已经完成的反馈模型

RTXGI 的公开 gather 遍历八邻探针，先判断 inactive，再计算权重和取样；其 visibility 是基于距离矩的 Chebyshev 近似，并显式保留非零权重下限。这与本项目严格 receiver/probe LOS 的合同不同。直接替换成 SDK gather、复用邻像素遮挡或只查最近 probe，均不能称为无损优化。[RTXGI Irradiance.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl)。

本轮可安全寻找的是：无 surface 不 gather；覆盖外/inactive/pending/严格零空间权重在 LOS 前拒绝；细层已经有效且剩余混合权重严格为零时停止后续体积查询；同一 receiver 的法线、位置、体积常量只解码/变换一次。严格零判断必须对应实际插值规则：RTXGI 对某些插值项保留下限，所以不能把“距离较远”“权重很小”擅自算成零。本项目是否已有这些判断由代码审计确认，只补真实缺口。

同一 tile 对相邻探针 atlas 的读局部性可能较高，可测量缓存命中与 LDS staging；但 strict LOS 仍是一条 receiver-to-probe 几何查询，不能因共享 atlas 数据而共享遮挡结果。若 gather 主要时间来自 LOS，单纯压 atlas 带宽收益有限，应该记录执行查询数和拒绝原因；不应先把 probe 更新 .145 ms 作为主要优化对象。

官方 probe trace 对 inactive probe 仍保留 fixed rays，且 fixed rays 不进入辐照度卷积，以维持分类/重定位并避免固定方向偏置。分类休眠有唤醒成本，不能让离屏探针永久停更；本项目已有预算与反馈合同，继续沿该入口做优先级即可。[RTXGI ProbeTraceRGS](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl)、[既有 DDGI 传播与调度合同](ddgi-coverage-next-batch-20261012.md#传播控制和性能优先级)。

EA 的 2024 GIBS 工程报告有特别相关的取舍：团队保留 surfel/probe 系统，在调度、存储、ray tracing 基础设施中迭代；也明确分开无画质损优化与偏置优化。其短射线后 probe 续接、shadow-map 替代部分阴影射线、低分辨率 apply、体育场加载后冻结，依赖自身内容/采样合同。它们不能直接用作本项目“画质不降”的提速证明。尤其不能把达到 ray 长度上限当作环境 miss，也不能在可编辑动态世界中照搬静态场景冻结。[EA Shipping Dynamic GI](https://advances.realtimerendering.com/s2024/content/EA-GIBS2/Apers_Advances-s2024_Shipping-Dynamic-GI.pdf)。

### 4. 降噪：最大分项先找带宽与共享读取

AMD Shadow Denoiser 在一个 temporal pass 内合并可分离的局部统计计算，避免额外中间目标；其三个空间滤波 pass 使用 groupshared 缓存。这里可借鉴的是数据复用机制。它的 32-bit/8×4 shadow mask 只编码单光的二值遮挡，**不适合直接替代**本项目 many-light diffuse/specular HDR signal。[AMD Denoiser](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/)。

对当前 resolve + 三次 filter，代码审计应首先回答：每 tap 是否重复读取同一套 depth/normal/material ID；diffuse 与 specular 是否重复做相同的几何邻域工作；同一 pass 的输出是否只被紧邻同像素消费者使用。可以共享引导数据加载和有效性计算，但保留两种信号各自权重、history 与 clamp。短半径 pass 最适合 tile + halo 的共享内存，后续 à-trous 大步长 halo 会扩大 LDS 与重复加载，应单独测寄存器数、occupancy 和 dispatch 时间。

不能把三个跨组依赖的 à-trous 阶段简单合成一个 dispatch：group barrier 不提供全图同步，后轮需要的邻居输出可能尚未产生。合法融合须在每组重算足够 halo 或采用已证明的 tile 算法，收益可能被重复工作、LDS 和寄存器压力抵消。因此，近期优先同一阶段的共享读取和不涉及跨像素反馈的融合。

NRD RELAX 的公开资源表采用 RGBA16F illumination/history，同时保留 R32F view-Z，说明“所有资源统一半精度”并非工业必要条件。保留当前 FP32 深度/距离矩与精确身份；若审计发现某颜色中间项可打包，也只能单独验证 HDR 范围、弱光细节和时间累积误差，不能列为无需验收的无损项。[NRD RELAX 资源声明](https://github.com/NVIDIA-RTX/NRD/blob/9fc5c8e7/Source/Denoisers/Relax_Specular.hpp)。

### 5. AS 与实例：剔除边界由所有光线路径决定

NVIDIA 建议复用未变化 BLAS、紧凑组织几何 AABB、池化小分配、compact 静态 BLAS，以及比较 rebuild/update 的完整成本。对真不透明几何使用 opaque flags、对仅需遮挡的射线在首个合法 blocker 终止，也属于保持查询意义的优化；alpha-mask、单双面和最近命中查询不能混用这些捷径。[NVIDIA RTX Best Practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/)。

可见性边界必须按用途区分：

| 工作域 | 可剔除依据 | 必须保留的屏外影响 |
| --- | --- | --- |
| 主视图光栅与屏幕接收者 | 相机 frustum、合法保守 occlusion、viewport/surface mask | 剔除不传播到灯源域或共享 TLAS |
| DI shadow | 当前 receiver 到选中灯的完整线段；方向光对应遮挡域 | 屏外灯、线段上的屏外 blocker、mask 材质 |
| DDGI primary / feedback / gather | 活跃体积与查询线段的实际可达范围 | 体积周围离屏几何、反馈路径、跨房间/楼层影响 |
| 将来 RT reflection | 反射路径可达范围 | 主相机后方或视锥外的反射物体 |

这张表是工程推导。只有一个实例不可能与**任何仍启用的查询域**相交时，才可以从共享 TLAS 安全移除。简单扩大相机 frustum 是常见启发式，不是任意灯距、DDGI 体积和动态遮挡条件下的正确性证明。NVIDIA 2023 实践文章明确要求为 shadow/reflection 保留视锥外物体；Remedy 也强调 RT 与主相机 culling 的差异。[NVIDIA Practical Tips](https://developer.nvidia.com/blog/practical-tips-for-optimizing-ray-tracing/)、[Remedy PS5 Pro](https://www.remedygames.com/article/alan-wake-2-on-playstation-5-pro---behind-the-scenes)。

本轮若 AS 不是热项，优先不建立复杂的多用途实例裁剪系统。静态数据复用和真实 dirty 更新可以先做；用前帧 ray-hit 统计推迟动态对象更新会引入至少一帧延迟和突然出现物体的特殊处理，属于后续有误差预算的调度研究，不能描述为绝对无损。

## 实施优先级与验收门槛

| 档位 | 本项目候选 | 收益机制 | 代价及通过条件 |
| --- | --- | --- | --- |
| 近期先做 | 视口局部 dispatch；一致的 surface/tile 空工作拒绝 | 少发无贡献线程组，减少所有后续重 pass 的空域读取 | 保留全局像素 seed/历史、清理无效输出、正确 halo 与 barrier；确认当前代码尚未实现 |
| 近期先做 | 同 pass 共享引导数据/被选光源解码；同像素尾部融合 | 减少重复纹理/源读取及中间写回 | 不改 DI 估计器、fresh visibility、独立信号权重和历史；寄存器压力无反向收益 |
| 近期先做 | gather 中确定无支持与严格零剩余权重早拒绝 | 避免不可能贡献的 atlas/LOS 工作 | 不以亮度或近似小权重裁剪，不删除有效暗部和跨层合法支持 |
| 需先测 | compact tile list + indirect；filter LDS staging | 稀疏工作与重复采样的摊销 | 统计 active tile 比例，计入 classification/atomics/参数 pass，分别检查大半径 pass |
| 需先测 | reservoir 局部布局/生命周期重叠；灯源预采样 | 提升内存相干性、减少驻留与读写 | 先证明读写生命周期；保留采样 PMF/身份/支持；用真实 many-light 场景核验噪声 |
| 需先测 | BLAS reuse/compact/布局和 rebuild 策略 | 降构建、遍历或内存成本 | 总成本包含建立、复制/同步；不改变 alpha/单双面与离屏遮挡 |
| 后续架构 | ReGIR/世界空间候选提议 | 分布更贴近接收面，提高有效候选比例 | 世界空间构建成本、动态失效、全域支持和屏幕/DDGI 共用入口 |
| 后续架构 | dirty/优先级调度与有界缓存续接 | 摊销大世界的更新/远程光线路径 | 必须先定义动态响应与误差预算；不是本轮无损默认选项 |

验收用现有真实端到端路径：窗口 resize/视口移动、空场与天空、大量局部和发光三角源、薄墙与门动态、屏外灯/遮挡体、volume 边界与滚动、新显露表面。固定 seed 的结构性优化先比较线性光照和历史稳定性，再采关闭 validation/capture 的 GPU 分项分布；滤波误差不能仅由 tone-mapped 截图判断。总帧尾不稳时先报告稳定分项，不能把阶段时间相加冒充端到端帧时改善。2026-10-14 已完成 Dev GPU 合同、Release 光照组合与 Renderer 全套验收，均退出 0、验证日志为空；本轮已实施改动的性能证据见上方空闲 GPU 对照，不能推及表中尚未实施的候选。

## 研究账本

检索日期为 2026-10-13。本轮使用 Exa 分八个角度检索：RTXDI、RTXGI、AMD SSSR/denoiser、AS、NRD、Remedy、EA、DDGI 原始论文。八次有效 search 各请求 5 项；一次缺少必需 objective 的失败调用也请求 5 项，按技能口径计 `sources_reviewed = 45`，其中有效请求额度为 40。该数字是 `sum(numResults)`，不是全文阅读数，也不是独立论文数。

正文/源码/演示关键段落实际提取涉及 **14 个唯一 URL**（重复 fetch 去重）：RTXDI Integration、RTXGI Integration/Irradiance/ProbeTraceRGS、AMD SSSR/Denoiser、NRD PrePass/Relax_Specular、GTC RTXDI 性能报告、NVIDIA 两篇 RTX 实践文章、EA GIBS 2024、Remedy 两篇工程文章。其中 RTXGI Integration 用于责任边界交叉核验；以上其余 13 个为本文直接引用。GitHub main 页面是本日读取状态，固定提交的 NRD 链接按所列提交核验。旧项目研究只用来避免重复既定合同，不计入外部唯一 URL。

本文引用厂商实现来证明机制和限制，实施顺序、像素/字节估算、TLAS 查询域联合与质量门槛是结合当前管线的工程判断；没有把 SDK 演示、EA 主机数字或 Remedy 产品描述冒充 RX 6800 XT 的本机测量。

视口失败定位另由根节点追加 1 次 Exa 查询（5 项），提取并按章节核对上述 Microsoft 功能规范。因此本轮请求计数合计 50（有效请求额度 45），实际使用的唯一外部 URL 增至 15。受竞争污染的旧样本、有效空闲 GPU 对照、已完成构建与最终 GPU 收尾均记录在[验收页](../modules/renderer/acceptance.md#多-astra-审查与工作剔除2026-10-13)。
