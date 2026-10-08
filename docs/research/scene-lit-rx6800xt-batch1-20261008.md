# Scene Lit 第一批建议：以 RX 6800 XT 为基准

日期：2026-10-08。状态：基于源码和一手资料的实施建议，尚未实施、运行外部样例或测得本项目 Scene Lit 性能。代码基线为 `402fab075bb2aa6605b51fdb767387ebd0e08869`，工作分支 `codex/viewport-rendering`。本说明承接[算法与项目现状调研](scene-lit-first-plan-20261008.md)；本轮新增决策条件是用户接受较积极的方案，并以自己的显卡为开发基准。

## 结论与第一批边界

建议第一批的最终产品是 **光栅主可见性 + 三类真实灯 + 有预算的 RT 阴影 + DDGI + GTAO + SSSR + HDR/时域稳定性** 的混合 Scene Lit。批内按下方四个可验收步骤推进，每一步仍使用同一正式 Renderer 入口。DDGI 和 RT 阴影进入本批必交范围；SSSR 放在同批最后接入。ReSTIR DI 单独列为第二批，先用本批测量决定如何替换直接光采样。

以当前显卡为基准，第一批无需同时建设软件 BVH / SDF 光追回退或全平台质量档；继续保留现有 MatCap/Smooth/Flat，Edit/Play 仍与显示方式独立。内部着色函数、光照数据与 GPU 缓存由我们按引擎契约组织；成熟算法、shader 或数学代码按固定来源和许可借鉴，不引入外部样例的整套宿主框架。

## 实际机器与性能假设

本机只读查询确认物理适配器为 **AMD Radeon RX 6800 XT**，CPU 为 AMD Ryzen 9 3950X。查询还返回多个虚拟显示适配器，未把它们作为 GPU。Win32_VideoController 的 AdapterRAM 字段不能作为这张卡的大显存容量依据。

AMD 官方资料确认：RDNA 2、72 个 Ray Accelerators、16 GB GDDR6。该型号支持硬件光追，但纸面规格不能推出本游戏的实际帧率。[AMD 产品规格](https://www.amd.com/en/products/graphics/desktops/radeon/6000-series/amd-radeon-rx-6800-xt.html)、[AMD DXR/架构说明](https://www.amd.com/en/newsroom/press-releases/2020-10-28-amd-unveils-next-generation-pc-gaming-with-amd-radeon-rx-6000-series-bringing-leadership-4k-resolution-performance-to-aaa-gaming)

建议使用 **2560×1440 原生场景分辨率、60 FPS** 作为初始工程目标，1080p 用于成本对照。该分辨率/帧率是本次建议，用户目前仅明确了 GPU；不是已达到的结果，也不是最终最低配置承诺。初始预算以代表场景稳定运行时 GPU p95 不高于约 12 ms 为目标，为波动和后续内容留余量；CPU 与总体帧时间独立记录，不把 CPU/GPU 简单相加。初期评估不使用插帧掩盖基础帧率。

固定测试场景尺寸和内容，记录逐 pass 时间、光追/探针工作量、显存峰值和 p95/p99 帧时间。真正超预算时先归因，再调分辨率、阴影预算、探针更新或反射范围。16 GB 物理显存也不是编辑器可独占的资源预算。

## 现有工程借鉴结果

| 工程 / 一手证据 | 已核对的实现 | 我们借鉴的部分与边界 |
| --- | --- | --- |
| **AMD Capsaicin**：[GI1 调度源码](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.cpp)、[屏幕探针](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/screen_probes.hlsl) | 同样采用 gboisse/gfx；GI-1.x 使用屏幕探针与世界空间 hash radiance cache，包含按需射线、重投影、缓存清理与间接调度 | 重点参考同类底层上的 compute/RT 组织、工作压缩和缓存预算。其 GI 并非 DDGI，不移植完整 50 余种 kernel 的系统 |
| **Wicked Engine**：[Renderer host](https://github.com/turanszkij/WickedEngine/blob/1eef4d642943ae2c212aa757afd900a7399c807e/WickedEngine/wiRenderer.cpp)、[阴影时域 shader](https://github.com/turanszkij/WickedEngine/blob/1eef4d642943ae2c212aa757afd900a7399c807e/WickedEngine/shaders/rtshadow_denoise_temporalCS.hlsl) | `CreateRTShadowResources / Postprocess_RTShadow` 使用半宽半高的 16 个 packed 阴影槽；只有前 4 灯走完整空间降噪，其余主要做简单时域处理 | 借局部 tile、固定预算、批量可见性与分级降噪；不能把 16 槽宣传成 16 个同等质量的软阴影灯 |
| **Wicked DDGI**：[射线分配](https://github.com/turanszkij/WickedEngine/blob/1eef4d642943ae2c212aa757afd900a7399c807e/WickedEngine/shaders/ddgi_rayallocationCS.hlsl)、[探针更新](https://github.com/turanszkij/WickedEngine/blob/1eef4d642943ae2c212aa757afd900a7399c807e/WickedEngine/shaders/ddgi_updateCS.hlsl) | 根据 inconsistency 分配射线，视锥外降频，射线分桶/间接 dispatch；辐照度与距离信息分别更新 | 借变化驱动的预算。首帧全探针打满最大射线数、固定混合系数、依赖 voxel 修正等不直接沿用 |
| **AMD FidelityFX SSSR**：[样例调度源码](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/samples/sssr/sssrrendermodule.cpp) | 输入 HDR、depth、motion、normal、roughness、environment 与 BRDF LUT；控制 roughness threshold、samples per quad、variance guided tracing | 借 shader 算法和明确输入/输出，在现有 RHI 中调度；不把 Cauldron 或它的原生 command list 包装带进产品 |
| **RTXDI MinimalSample / NRD**：[最小调度](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Samples/MinimalSample/Source/RenderPass.cpp)、[NRD 契约](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md) | RTXDI 可用一个组合 Compute pass 演示 DI；NRD 返回资源/管线描述及 Compute dispatch，自身不负责图形 API 调用 | 借 shader bridge、资源/信号契约与局部可替换算法；不引入 Donut/NVRHI/NRI 作为第二宿主。最小演示也不等于已经解决本游戏的噪声和动态稳定性 |

Capsaicin 源码固定在 `914b91596cd119eda85fbc1d3c7ee6ac391b1452`，Wicked 固定在 `1eef4d642943ae2c212aa757afd900a7399c807e`。这些 commit 固定本次引用内容，不是建议全部升级为引擎依赖。

一个特别相关的细节：Capsaicin 在二次命中处使用世界空间 reservoir 做灯光选择，并让直接发光命中绕过辐射缓存，以免光源形状被缓存放大。它适合启发 DDGI 命中点照明的预算与分量处理，但不等同于主表面的 ReSTIR DI。[世界空间采样源码](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/world_space_restir.hlsl)

性能证据边界：GI-1.0 原论文的测试使用 RX 6900 XT / RTX 3080、1080p、特定场景与采样设置；不能外推为 RX 6800 XT 的完整游戏帧率，也不能直接代表当前 GI-1.2 的所有功能。[原论文](https://gpuopen.com/download/GPUOpen2022_GI1_0.pdf)


## 第一批的四个交付步骤

### 1. 正式场景数据、HDR 与最小 RT 能力

- World/现有作者系统增加点光、矩形面光、方向光，以及材质 base color、roughness、metallic、emissive。灯光是真实场景组件，保存/加载、属性编辑与独立运行共用同一数据链；不在 Renderer 里创建演示专用灯。
- 不透明场景采用紧凑 G-buffer，保存实际所需的法线、材质、深度与 motion；由深度重建位置。保留 lowpoly 硬边，不为降噪改平滑几何法线。
- 一次定义矩阵、深度、motion 方向/单位、jitter、曝光空间和视图尺寸。共享这些事实与重投影工具；各效果保有自己需要的历史，不共用一张含义不明的“历史颜色”。
- 场景实例/灯的键来自权威对象身份并带场景代际；删除、重排、切场景与 Edit/Play 世界切换不会让历史引用另一个对象。Renderer 继续只消费快照，不持有业务对象、GC 根或 JIT 回调。
- RHI 验证现有 gfx 的 DXR 1.1 / inline RayQuery、BLAS/TLAS 更新与退休、AS shader binding；同时确认离屏 FP16、MRT、depth SRV、UAV/mip 与逐 pass GPU timestamp。优先复用现有能力；新增工作由 RHI 负责人负责。
- 共享静态网格复用一个 BLAS；刚体移动更新实例/TLAS，避免每帧重建静态网格。几何变更才更新相关 BLAS。按真实房间和资源组织遮挡集合，避免整个地图一个极大且稀疏的包围盒；也不机械拆成每个材质一个 BLAS。[AMD RRA 实践](https://gpuopen.com/learn/improving-rt-perf-with-rra/)
- 首批沿当前单执行域/单队列工作。AMD 的异步队列建议是后续测量选项，不是本批绕过 RHI 或先建设多队列的理由。

验收终点：正式编辑器可创建/保存/重开三类灯与材质；真实几何在相同实例身份下同时可供光栅和射线查询；resize、删除、切场景和关闭遵守已有生命周期合同。先有可解释的 HDR 受光结果和 GPU 计时，再接时间算法。

### 2. 三类直接光与可控的真实阴影

- **方向光**：统一 BRDF 下的主光；使用 RT 可见性验证遮挡。有限光源角度的软阴影属于采样/滤波策略，默认优先稳住角色轮廓和地面接触。
- **点光**：明确光强、范围和近距离行为。半径为零的理想点光产生硬阴影；有限源半径可以形成软阴影，不能把灯的照明范围当作发光体尺寸。
- **矩形面光**：长宽、朝向、单/双面、发光强度语义明确。LTC 是无遮挡受光/高光的参考与解析求值候选；可见性在真实矩形上采样。若首版采用 LTC 结果乘面积可见率，应明确它是工程近似，不能宣称对任意粗糙度/部分遮挡严格无偏；用单灯多采样参考图核对高光与半影。尺寸变大应同时改变受光和阴影表现。[LTC 作者资料](https://eheitzresearch.wordpress.com/415-2/)
- 光源按受影响屏幕区域筛选，对阴影使用可配置预算；初始代表场景采用 **1 方向光、2 点光、1 矩形面光，四灯同时投影**。再以约 32 个可见局部灯检验筛选与照明成本，投影灯数量单独增加。4/32 是测量场景的起点，不是已经证实的显卡能力或固定引擎上限。
- 借鉴局部 tile 筛选、低分辨率可见性、稳定阴影槽位与统一批处理，不给每个灯无条件分配全分辨率降噪链。是否投影和质量预算必须有明确语义，不静默把关键灯改成不投影。
- 稀疏阴影结果进行深度/法线约束的空间重建与时域滤波；细小角色和接触区域比较全分辨率结果，半分辨率不是所有画面固定正确的默认。
- 对低灯数保有同一 BRDF 的确定性/高样本诊断参考，服务端到端图像比较，不形成另一个产品 Renderer。

**信号合同优先于照搬优化。** AMD 建议纯二值不透明阴影尽早终止于 first hit；但 NRD SIGMA 需要可信遮挡距离，官方明确不能用同样的 first-hit shortcut。SIGMA 也是逐灯阴影降噪器，不适合把所有不同灯的阴影混成一张图后直接调用。应先选信号与滤波方法，再选射线 flags。[AMD RDNA 指南](https://gpuopen.com/learn/rdna-performance-guide/)、[NRD 输入契约](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md)

若采用 NRD，使用它的计算调度/资源描述，通过当前 RHI 创建资源与执行；不使用索取原生 device/command list 的 NRI 接入捷径。其资源池、输入编码、动态分辨率和历史失效按实际 API 适配。是否复用某个 denoiser 由输入匹配和 RX 6800 XT 实测决定，第一批不同时接入全部 NRD 算法。

验收终点：受光、衰减、背面、面积高光与物体间阴影成立；快速移动、转镜头、灯开关、爆炸与删除不产生不可接受的拖影。光照计算与曝光能分别调试，关键闪光在当前帧可见。

### 3. 有限体积 DDGI 与 GTAO

- 第一批直接做 RT DDGI，不把 GI 留到未来。先覆盖一个战斗房间及周边遮挡，探针保存辐照度与距离信息，并实现必要的有效性分类、位置修正、时间混合和调试显示。
- 建立固定总工作预算，按变化/收敛状态分配更新。避免首帧对所有探针同时打满最高射线数；加载/切房间的初始化和过渡也属于完整功能。
- 探针命中处的直接光复用同一灯光/材质函数，并显式计算可见性。主视图 screen-space shadow mask 和将来的 ReSTIR 屏幕 reservoir 都不能直接作为屏幕外命中点的正确阴影。
- 首个有限灯集可采用正确加权的灯采样和受控候选数，统计探针射线之外的阴影射线；不让“每探针 N 条射线”掩盖命中处遍历全部灯的额外成本。
- 近景、高变化区域优先，但视锥外仍保有更新，因为它可能向当前房间反弹光。不盲目照抄外部引擎的视锥倍率。
- GTAO 采用短半径接触补充，调制适合的间接分量，避免与 DDGI 可见性重复压黑。直接光和自发光保持独立。
- 以开门、关门、薄墙、灯光突变、房间切换、场景编辑为验收核心。DDGI 的反馈不能承担短促技能闪光的唯一表达。

验收终点：暖/冷色反弹清楚，暗面有层次，薄墙与接触关系合理；开关灯的响应和探针收敛有记录，更新预算不会产生明显加载尖峰。视图显示继续独立于场景是否 Play。

### 4. SSSR、最终合成与整体画面

- SSSR 使用深度层级、粗糙度与实际有效像素分类；生成需要追踪的工作列表，避免整个画面执行相同反射工作。借鉴 AMD 的 roughness / variance 控制和间接 dispatch。[AMD SSSR 样例](https://gpuopen.com/manuals/fidelityfx_sdk/samples/stochastic-screen-space-reflections/)
- 先有预过滤环境反射与 BRDF LUT，SSSR 按可信命中补充；屏幕边缘和被遮住的区域平滑回退。第一批不做完整 RT 反射回退。
- 自发光、直接光、漫反射间接光与镜面反射保持清楚的贡献归属；同一发光面与解析面光不能重复计光。反射源不包含自身最终反射、UI 或参考网格。
- 完成原生时域抗锯齿、受控曝光、色调映射与克制的 Bloom；UI 与编辑辅助在显示空间合成。灯光变化加速历史响应，低多边形硬边和技能轮廓不能被累积洗掉。
- 使用实际金属/低粗糙度物体和反射地面验收。完整透明水面、折射与粒子系统未在本批材料范围内；SSSR 本身不等于这些系统。

验收终点：在同一真实房间里同时开启整套效果，动态画面清楚，录下 GPU 分项与显存，并通过 Dev/Release 的正式编辑器端到端。窗口失焦/抢占仍按既有真实窗口规则协调，不额外建立单元测试或 mock 工程。

## ReSTIR DI 为什么放第二批

第一批已经决定使用硬件光追和完整时间数据，ReSTIR DI 的关键基础会自然存在。第二批可把同一 `DirectLighting` 内的灯采样替换/扩展为 reservoir 重采样，继续使用相同灯、材质、加速结构与视图历史事实；不用另建“RTX 模式”或复制场景。

官方 MinimalSample 的 `RenderPass.cpp` 与 `Render.hlsl` 展示了单 Compute pass 的采样/重采样闭环，但 shader 仍有初始候选采样、历史复用、可见性查询与最终加权。一个 pass 不代表一个射线，也不消除运动矢量、稳定 light remapping、显露区域、噪声与降噪成本。[实际 host 源码](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Samples/MinimalSample/Source/RenderPass.cpp)、[实际 shader](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Samples/MinimalSample/Shaders/Render.hlsl)

官方 reservoir 源码也明确：复用旧可见性会换取更柔/更迟滞的阴影。这对频繁闪光和突然刷怪的 roguelike 尤其需要验证。[Reservoir.hlsli](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/main/Include/Rtxdi/DI/Reservoir.hlsli)

触发第二批的证据是：同等画质目标下，真实房间中许多重叠投影灯让直接光/阴影成本明显超预算；或者美术明确需要大量发光面共同照明。届时比较确定性路径和 ReSTIR 的**完整直接光成本（含筛选、追踪、时域与降噪）**及响应，而不是仅比较 TraceRay 或一个 dispatch。

第一批不做 ReSTIR GI/PT、任意发光网格、完整路径追踪、通用材质图、大世界多层辐射缓存、第二套 RHI、多队列改造或插帧。它们各自需要实际需求支撑。

## 范围与来源说明

本轮使用 Exa，按硬件/AMD 优化、直接光与降噪、屏幕反射、Capsaicin、Wicked 五个方向，共进行 10 次搜索，请求 50 条结果；结果用于发现来源，再按 URL 与工程去重、筛选第一方资料，深入到上表的实际 host/shader 函数。这里的 50 是检索结果口径，不代表 50 个独立工程或 50 篇全部通读的论文。核心依据是固定源码与官方合同，未采用排行榜/转载文章作为性能证明。

本地模块、RHI 和帧生命周期事实沿前一份报告的源码核对，不重复扫描整仓。


本轮只读硬件与工程源码、调研外部资料并形成本文。没有安装外部渲染工程、改动驱动、运行其基准、修改产品代码或提交推送。引用中的厂商样例性能不代表本机结果；例如 NRD README 的 1440p 时间对应 RTX 4080，不能移作 RX 6800 XT 的预算结论。
