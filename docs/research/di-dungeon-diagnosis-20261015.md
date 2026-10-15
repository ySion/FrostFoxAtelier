# 地牢直接光成本诊断与工业方案核查 — 2026-10-15

状态：本文前半部分保留首次只读诊断和工业考据；后续经用户授权，已实施精确重复工作消除与 DI/DDGI 共用空间提议，见末节。首次诊断未修改生产代码；实施批次的构建、验收与限制分别记录，不能混用。没有提交或保存原用户 Scene。Astra 分别审查估计器、GPU 工作组织、资源生命周期与产品合同；RHI 负责人核对计时语义并提供运行态旁证。

## 判断

当前 DI 的整体成本确实需要优先处理。现场约 113 万视口像素下，Direct lighting 平均 12.62 ms、P95 18.12 ms，已经严重挤占 60 fps 的整帧预算。但证据不支持把它直接归因为 ReSTIR 公式错误、96 盏灯必然超载，或硬件阴影本身花了全部 16 ms。

现有实现已具备分阶段执行、稳定源身份、广义 MIS、独立最终可见性、材质解调和空间滤波。待改进的是重复工作、重建成本、全局选灯分布对多房间场景的适应性，以及尚未解释的共同运行态波动。保留现有主线，先做有测量约束的精确优化，再演进共享空间提议；不因附件的通用数字立即替换整条管线。

用户仍报告地牢突发亮斑，本轮尚未重新定位其来源。此前固定接收点的长窗改善只覆盖该测点和配置，不能推广成全地牢零闪烁，见[DDGI 记录](ddgi-stability-and-lighting-roadmap-20261015.md)。

## 现场与测量口径

- 源码基线 `99111cab3054cf717e68418dbec36b5d2a15cafb`，正式 Release，RX 6800 XT；启动参数只有 `--project`，未启用 GPU validation。运行文件 SHA256 为 `657b5a15a417857e1a853b6c575ea89e84c230deac00a8a68391ba2d295f6dc5`。
- 用户实例 PID 54084，场景为此前导出的地牢副本，当前存在未保存修改。记录和实验期间相机、Control token 均保持不变。
- 客户区/资源尺寸 `2380×1446`；实际视口约 `1411.16×802.28`。这不是原生 1440p 视口结果。
- 96 个光源，`direct_candidates=8`，`direct_reference=false`，SMAA。DDGI 为 1152 个探针、512 updates/frame、32 rays/probe、反馈 .85、最终间接光 gain 5。此前交付窗口是 128 rays/probe，两者不能直接作为等条件画质比较。
- 首轮采集 120 个不同 GPU 完成序号，跨度约 20.8 秒，没有截图等待。GPU engine 的单次观察为编辑器约 57%、DWM 约 8%，未见其他重负载 3D 进程；这不是全程独占或 GPU 时钟/occupancy 证明。

| 阶段 | 平均 ms | P95 ms |
|---|---:|---:|
| Direct lighting 合计 | 12.623 | 18.123 |
| 初始候选与时间复用 | 3.761 | 4.917 |
| 空间复用 | 2.416 | 3.203 |
| 可见性与最终着色 | 2.266 | 3.357 |
| 直接光 temporal resolve 与空间滤波 | 4.178 | 6.690 |
| DDGI 合计 | 3.238 | 3.854 |
| 八个计时主阶段之和 | 21.368 | 28.401 |

DDGI 平均成本中，更新/传输为 .661 ms，接收面 gather 为 2.577 ms。子项包含在父项中，不能再次相加；各项 P95 不能相加。总计不含 UI、辅助线、探针覆盖、CPU 与 Present，因此不能用 `1000 / 总计` 宣称编辑器 FPS。

根据同一完成样本的计数，总 visibility 查询约 1029.5 万次，其中 DDGI receiver gather 约 847 万次、probe-hit visibility 约 4.3 万次。扣除这两部分后，DI 约 178.3 万次。总 shadow counter 包含 DDGI，不应把它全部算给直接光。查询数量也不等同于 BVH 节点访问次数。

## 有界临时对照

通过正式 Control 的 `render.compare` 顺序执行四种配置，每种 256 个接受帧。仅保留 case ordinal 64～224 的不同完成序号，远离配置切换和末帧截图；每组保留 45～49 条观测。每种仅在终点截图一次，截图时刻不作为性能样本。全部对照结束后 token、相机、作者设置、Undo=5 和 dirty 状态均与开始一致，无活动任务。

| 临时配置 | 初始/时间 | 空间 | 可见性/着色 | resolve/filter | DI 合计 |
|---|---:|---:|---:|---:|---:|
| 8 候选，原设置 | 4.439 | 2.917 | 2.709 | 4.687 | 14.754 |
| 2 候选 | 3.170 | 2.879 | 2.900 | 5.581 | 14.531 |
| 8 候选，关闭 Shadows | 4.674 | 2.928 | .483 | 5.541 | 13.627 |
| 8 候选，恢复原设置 | 4.500 | 2.915 | 2.792 | 4.534 | 14.742 |

单位均为平均 ms。减少候选只明显降低 initial；在这次窗口中，后续重建更贵，整体没有出现相应收益。关闭 Shadows 后 DI 查询计数为零，可见性阶段明显下降，但 DI 大部分工作仍在。本轮对照不支持仅降低候选数或关闭阴影就足以解决整体成本。

这些是归因实验，不是等质量优化基准：候选数改变也会改变噪声、confidence cap、历史与自适应滤波的工作；关闭阴影改变输入信号。多个阶段仍存在同步快慢波动，不能把每个差值全部归因于一处源码。

首次辅助脚本误用 CLI 的自动等待行为，20 秒后超时并由断连取消，未用于上表。随后沿文档化命名管道协议保持单连接，完成异步对照与只读采样；没有新增产品控制入口。

## 计时波动尚未完成归因

同一场景、相机和设置下，initial 常出现约 1.32/4.88 ms、spatial 约 .70/3.20 ms、GBuffer 约 1.28/3.78 ms 两组耗时。首轮 DI 与 GBuffer 的相关系数约 .927，且不是按序号奇偶交替；射线数量基本稳定。

RHI 负责人确认：计时区间包含资源状态安装、区间内 barrier、dispatch 及其依赖停顿，不是独立 shader busy time；起点前已完成的 GBuffer 工作不会整段转嫁给 DI。原始整数 ticks 先相减再换算，所有子项和计数按完成序号配对。DI 父项与四子项之和差约 .28～1.60 微秒，未发现明显混帧、重复平滑值或时间精度错误。

共同波动支持调查 GPU 核心/显存频率、功耗状态及队列调度，尚不能单独证明降频或驱动问题。AMD 文档也明确普通时钟会动态变化，RGP 捕获会切换到 peak clock，因此不能把捕获结果无条件当成当前普通运行态。[AMD 时钟语义](https://gpuopen.com/manuals/rdp_manual/system/)

本机已由负责人确认 AMD Software、PresentMon、WPR/GPUView 可用。本轮没有启动系统时钟记录或 ETW 捕获、修改时钟或绕过 RHI。下一轮性能比较应先补这项运行态证据，否则不能可靠宣称数倍 shader 提速。

## 源码审查

调用链见 [lit_light_sampling.hlsl](../../src/renderer/shaders/lit_light_sampling.hlsl)、[lit_restir.hlsl](../../src/renderer/shaders/lit_restir.hlsl)、[lit_direct.hlsl](../../src/renderer/shaders/lit_direct.hlsl) 与 [scene_lit.cpp](../../src/renderer/scene_lit.cpp)。

### 已有正确性与工程设施

- 初始 RIS 使用全局功率 alias 加 5% 均匀分布，保存实际 PMF；当前/前帧源按稳定身份映射。
- 时间复用最多两个输入，空间复用最多三个输入，以 generalized balance MIS 计算跨接收面的目标与责任。零实现不等于该分布不存在。
- 当前 reservoir 为 24 字节，没有保存整盏灯或三维命中点。三份不可变读写域分离的数组按客户区分配；本尺寸仅 reservoir 逻辑容量约 236.3 MiB，非实际外存流量。
- confidence 上限是候选数乘 20，默认 160；该值不是独立样本数，也不能与附件的“20 帧”直接比较。
- 常规 initial/reuse 不追可见性射线；最终每个接收面最多一条 local shadow，加每盏方向灯独立查询。没有每个候选都追一条 shadow 的问题。
- DI 已拆为 initial/temporal、spatial、visibility/shading、resolve、三个 filter dispatch；使用视口局部 dispatch、first-hit shadow、wave 聚合计数和 diffuse/specular 解调。
- 空间滤波已有成对 guide 读取及 inactive 像素直通。不能把这些列为“尚未实现”的新优化。

### 确实值得改善的部分

**1. 求值重复。** 默认 8 候选、全部复用输入合格时，局部光求值上界是 `8 + 2×2 + 3×3 + 1 = 22` 次/像素。它不是 22 条阴影射线。当前相同 `(source, UV)` 在不同 receiver 上反复准备光源端点，mesh emitter 还可能重复读取 emission/coverage。

可以在同一现有入口内准备一次光源样本，再对不同 receiver 求距离、朝向和 BRDF；spatial 从最多九次完整准备变为三次准备加九次必要 target。current/previous snapshot 必须分开。完全相同样本可共享 target 向量，但各输入的 M/W/MIS 责任与 stream 顺序仍须保留。

这是等价优化候选，不是已获收益：prepared 数据会增加寄存器压力，编译器也可能已有部分消除。2026-10-14 两个不同的 MIS 等价重写曾实测退化并撤回，见[此前测量](../modules/renderer/acceptance.md#2026-10-14-空闲-gpu-性能对照)。

**2. 重建重复。** `lit_direct.hlsl` 的 temporal resolve 对 diffuse/specular 分别进行局部统计和历史资格检查，存在可共享的深度、身份、法线与世界位置读取；保留各信号独立的方差、颜色与权重。filter step 4 对许多像素虽已直通，仍有整视口调度和写回，后续 tile 分类必须衡量分类/路由成本，不能假设 compact 总是更快。

**3. 提议缺少空间信息。** 全局功率分布不了解距离、房间、楼层或遮挡。远处/隔墙强光可以消耗候选，少量近处有效光成为高权重尾部。这是代码中明确的能力缺口；本轮尚未测量该地牢具体浪费比例。96 个 128 字节源记录仅约 12 KiB，不能套用数百万灯场景的缓存灾难解释。

空间提议应供屏幕 DI 与 DDGI hit 共用，保持同一光源身份、真实提议概率、全局保底和域外回退。它应与 DDGI 探针密度解耦；使用世界位置选灯，不等于拿探针照度当 PDF，也不要求先建设 SDF。

## 对附件的工业考据

| 主张 | 核查结果 |
|---|---|
| ReSTIR 的不相干访问需要优化 | 有作者一手支持；相干预采样在特定巨大灯池中收益显著，但不是本机的倍数承诺。[Wyman 性能报告](https://cwyman.org/presentations/GTC2021_RTXDI_Performance.pdf) |
| 官方 reservoir 应为 8/16 B、UV 各 6 bit | 所核 SDK 固定提交的 packed DI reservoir 为 24 B，四个 uint、两个 float，UV 各 16 bit。自定义更小布局需另评精度和重算成本。[官方结构](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/RtxdiParameters.h#L136) |
| RTXDI 使用 Morton，禁止 row-major | 官方是 16×16 block-linear，块内 row-major；不能把分块等同 Morton。[官方寻址](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/Utils/ReservoirAddressing.hlsli#L39) |
| M 强制不能超过 20～30 | 官方 temporal 限制与当前样本 M、maxHistoryLength 共同相关，不是固定绝对值。[官方实现](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/TemporalResampling.hlsli#L42) |
| MegaLights 的可见列表、STBN 与低分辨率值得参考 | 是；20% 隐藏光比例属于可调策略，低分辨率有收敛/阴影质量代价，不能直接视作无损替代。[作者讲义](https://advances.realtimerendering.com/s2025/content/MegaLights_Stochastic_Direct_Lighting_2025.pdf) |
| MegaLights 能彻底根治噪声和拖影 | 官方仍明确描述高照明复杂度下的模糊、噪声和 ghosting 限制。[Epic 官方说明](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine) |
| 前置 visibility、复用旧遮挡和 boiling clamp 都是免费优化 | 不同措施有不同支持集和偏差合同；现有 visibility-excluded MIS 不能只清 W 就宣布正确。[RTXDI Noise and Bias](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/NoiseAndBias.md)、[Boiling Filter](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/BoilingFilter.hlsli) |
| 六项必做可把 6 ms 压到 1.2 ms | 没有找到支持该通用结论的一手依据。平台、内部像素、BVH、灯型、画质和计时范围必须对齐。 |

ReGIR 的可取之处是用空间区域生成更好的候选分布，包括供二次命中消费；它不是天然知道遮挡的房间灯表，也有构建和更新成本。应参考机制，沿本项目的 LightFrame 扩展，而非增加第二套灯系统。[RTXDI ReGIR 接入](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md#world-space-sampling-and-regir)

## 建议的下一批范围与验收终点

1. **补足运行态证据。** 对齐现有 pass 采样与短时 GPU 时钟/队列记录；必要时把 resolve 与 filter 细分。继续沿现有 RHI 计时接口，由负责人提供缺失能力。
2. **先消除精确重复工作。** 优先评估 temporal resolve 的共享 guide，其次 light-sample preparation/完全相同候选缓存。一次一个候选，测编译资源和端到端阶段耗时；保留完整质量基线，失败就撤回。
3. **演进共享空间选灯。** 在同一 LightFrame 下提供有界的世界区域提议，混合全局保底，明确 PMF、源移动/增删、区域滚动/溢出、无候选和历史寿命。先测“构建+消费+降噪”合计，目标是同时降低无效候选和高权重尾部。屏幕可见性反馈作为后续提议改进，最终阴影仍重新查询。
4. **保留全分辨率质量入口。** checkerboard/低分辨率只作为独立质量实验，覆盖薄几何、遮挡显露、物体/灯光运动和小高光后再决定；不降低 FP32 深度，不通过强行 TAA、过度模糊或删高能样本掩盖问题。

判定标准包括：相同有效像素与运行态下的 DI/整帧时间、线性照度均值与高分位、静止时间波动、光源开关/运动响应、纹理清晰度、薄墙/离屏遮挡、源身份与资源寿命。减少时间方差不能单独证明正确，更不能把删能量产生的稳定当成成功。

DDGI 当前是每个 hit 独立 fresh RIS，没有屏幕 ReSTIR reservoir 的时间/空间复制；两者共享光源 evaluator/proposal。故改良空间选灯有共同价值，但不能预先声称它已解释或治好本次 DDGI 亮斑。32 rays/probe 相比 128 改变积分方差，是后续归因变量，不作为让用户调回旧值就结案的理由。

## 证据位置

本地原始数据在 `out/di-analysis-20261015/`：`initial-state.json`、`live-baseline.json`、`ab-before.json`、`ab-perf.json`、`ab-analysis.json`、`ab-result.json`、`ab-after.json`。Astra 备查分别为 `astra-estimator.md`、`astra-gpu.md`、`astra-industry.md`，不构成第二套现行实现合同。Control 截图产物目录由 `ab-result.json` 指向。

工业研究使用 Exa 四次查询、每次请求五项，`sources_reviewed=20` 表示请求结果数量而非全文数量；结论回到官方源码和作者讲义。对所核 SDK 用固定 runtime 提交 `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`，不假定所有发行版布局相同。

## 本批实施与实测（2026-10-15）

生产增量沿既有 LightFrame / Scene Lit / ViewOption 入口完成：

- 直接光时间重建共享 diffuse/specular 的局部几何与历史资格检查，保留各自 moments、支撑与颜色权重；便宜的法线/粗糙度拒绝在邻域世界位置重建之前执行。
- 同一候选的端点、发光贴图与 Mask 在 MIS 的多个 receiver 之间复用；current/previous 光源快照仍分别准备，BRDF、距离和法向项仍按 receiver 计算。
- 有界近源网格供屏幕 DI 与 DDGI hit 共用，源身份、全局兜底及完整混合 q 保持一致。实际整数候选分配与资源合同以 [Scene Lit](../modules/renderer/scene-lit.md) 为准，不复制第二套长期参数说明。
- 新诊断开关不写 World；源／格／逻辑字节／上传状态可经 Control 读取。空场景验收抓到新增统计未接 Renderer 手动清零链的问题，已修复；没有放宽该断言。

两项初始精确优化分别测量，原生 2560×1440 固定场景、96 帧预热、240 个唯一完成样本、validation 关闭，计时期间不 Capture。原版两次 DI 中位数 4.999/4.964 ms；只配对时间重建为 4.776/4.750 ms，其 resolve/filter 约从 2.40 降到 2.18 ms；加入候选准备复用为 4.261 ms。再加入空间提议及前置便宜拒绝，首次 DI 中位数为 4.328 ms，八主阶段合计中位数 8.976 ms。空间提议并非零成本；相对 prepared 候选本场景多约 .07 ms 的 DI 工作。

地牢使用完整的已保存工程副本，恢复原窗口的相机及可读取作者/视口参数，保留 96 源、1152 探针、512 updates、32 rays、gain 5、SMAA。不是原未保存几何的完整导出。测试 client 为 1920×1260，实际 viewport 约 1126×666，不能把本表称为地牢 1440p 成绩。每项收集 240 个唯一完成样本；原窗口暂用 MatCap，用户的 Scene/Undo 未保存或替换。

| 地牢候选 | DI 均值 ms | DDGI 均值 ms | 八主阶段合计均值 ms |
|---|---:|---:|---:|
| 原版 | 12.214 | 4.128 | 21.544 |
| 配对重建 + prepared | 11.087 | 5.005 | 21.713 |
| 新版、全局提议诊断 | 10.598 | 4.464 | 20.628 |
| 新版、空间提议 | 11.046 | 4.843 | 21.483 |

因此本轮证实 DI 下降，**尚未证实地牢整帧提速或达到 60 fps**。跨 Pass 的运行态波动仍在，表格不等于因果分解；新二进制内的空间/全局 A/B 支付相同 CPU 网格维护成本。旁证 PresentMon 窗口有 921 个呈现样本，GPUBusy 均值约 25.29 ms，GPUWait 均值约 .28 ms；它没有记录 GPU 频率或电功率，不能据此宣称时钟根因或节电收益。CPUBusy 列也不能直接当作 CPU 线程实际执行时长。

静态质量对照使用同一新二进制，两个模式各预热 128 accepted frames 后捕获 12 张、间隔 8 accepted frames，保持相机和作者参数不变。以下为固定 ROI 的 sRGB 亮度时间标准差均值（0～255 单位），包含整个启用的渲染管线：

| 区域 | 全局提议 | 空间提议 |
|---|---:|---:|
| 后墙 | .598 | .485 |
| 右墙 | 1.081 | .546 |
| 地面 | .274 | .266 |
| 取样视口 | .660 | .481 |

地面平均亮度为 146.155 / 146.124，后墙为 10.862 / 11.223，右墙为 13.776 / 12.819。短窗、非线性显示值不能证明线性能量无偏；固定配额保证全局候选数量，也不能保证选中可见主光。后墙相邻捕获最大差由 36.39 降至 16.81，但整个视口仍有约 109 的局部差值，**不能宣布突发高亮已根治**。后续应针对真实高权重尾部与可见性分布继续归因，不通过删能量或加重最终 AA 掩盖。

收尾还隔离出一个原有 DI 降噪限制：封闭遮挡突然插入后，接收面经过 38 帧仍有均匀 RGB 3/255 的低亮度历史残留。保留的原版二进制也失败；关闭 DDGI 不改变残留，直接光参考则为零。这不属于本批引入的探针泄漏，也没有在本批修改其历史响应策略。原来的 DDGI scratch 断言现隔离直接光，并检查暖探针没有被重置；详细失败与通过证据只在验收记录维护。

本批验证与最终结果以 [Renderer 验收记录](../modules/renderer/acceptance.md) 为准。原始数据位于 `out/di-evolution-20261015/`：`performance-summary.json`、`dungeon-*-perf.json`、`dungeon-quality-analysis.json`、`dungeon-new-*-quality.json`、`rhi-runtime/` 与源码/二进制 `manifest.json`。捕获 JSON 指向正式 Control 产物。三个 Astra 最终只读复审覆盖概率分配、资源/边界/幂等、World/view 与统计合同，未发现阻断项；GPU 验收独立于专家判断。

## 保存地牢后的大视口复查（2026-10-15）

本节是上述增量落地后的新现场诊断，不是新一轮性能修复。用户报告 Direct lighting 17–20 ms；确认正在运行同一标准 Release 二进制，SHA256 `fdf6d2f7696444f4829e4b440d9aa6b220710042d549b0450e00db7fcd6cd8c7`，并非旧版编辑器。项目为 `%LOCALAPPDATA%/FrostFoxAtelier/OfficialExamples`，场景 `Examples/Dungeons/Scenes/Dungeon.scene`。本轮未修改生产代码、未构建、未保存用户场景。

### 现场与可重复信号

- 实际 client **3840×2054**，viewport **2316.36×1246.12**，约 289 万像素；比上一轮地牢对照的 1126×666 大约 3.85 倍，但不是按整个 4K 客户区着色。两个场景／视角不同，不能用像素比直接预测 DI 速度。
- 磁盘 Scene 有 3594 entities、191 个 LightComponent；实体和组件均启用的为 **95 点光 + 1 方向光**，与运行帧的 96 sources 一致。点光源半径均 .05 m；方向光角半径 .05 rad，照度 1.5 lux。点光与方向光的强度单位不同，不能直接按属性数字比较能量。没有实测这些参数就是 DDGI 闪烁根因。
- K=8，空间提议开启，SMAA；DDGI 为 512 probes、256 updates、128 rays、feedback .85、gain 2。保存文件中的这些作者值与当前 Control 一致。Control 仍报告 dirty=true，故本轮以当前现场为准并保留其状态，不假定全部未保存数据已经落盘。
- `sample_live.py repeat 23676` 在真实实例读取 240 个唯一 GPU 完成样本，不 Capture、不修改设置，按 DI P95 是否超过 16.667 ms 输出 RED。本轮两次 baseline 均值 **15.874 / 15.806 ms**、P95 **23.366 / 22.512 ms**；同视角临时基线重复出现 **15.751–17.967 ms**。这复现了用户症状，不能把上一轮的小视口结果视作性能问题已经解决。

初次 240 样本的 DI 四项如下；各项 P95 不相加作为总 P95：

| 阶段 | 均值 ms | P50 ms | P95 ms |
|---|---:|---:|---:|
| Initial / temporal reservoir | 5.720 | 2.937 | 10.468 |
| Spatial reservoir | 2.137 | 1.528 | 4.058 |
| Visibility / shading | 2.042 | 1.800 | 2.782 |
| Radiance resolve / filter | 5.976 | 5.957 | 6.113 |

稳定的约 6 ms 重建成本和前段的慢档是两个不同问题。当前 resolve/filter 合计一次 temporal resolve 和步长 1/2/4 的三次空间 dispatch，每步最多 5×5 taps；配对 guide 已共享，但有效邻居仍进行几何资格、世界位置／平面检查及各信号的亮度权重。不能把 6 ms 全归于某个滤波步，因为尚无四段独立测量。

### 分档与临时反证

Astra 对已有初次样本以 initial≥6 ms 分组：快档 139 帧为 initial **2.755** / GBuffer **1.732** / spatial **1.426** / resolve **5.951 ms**；慢档 101 帧为 **9.800 / 5.952 / 3.115 / 6.010 ms**。initial 与 GBuffer 相关系数 **.963**，与 spatial 为 **.908**；与直接光 visibility 查询数相关仅 **.004**，两档查询均值都约 479.2344 万。这里按 `shadow_rays - sum(ddgi_visibility_rays)` 从完成帧的三个独立计数分离 DI；原 `shadow_rays` 合计还包括 DDGI，不能直接改称 DI 查询数。快慢档也不按帧奇偶交替。以上否定了“只是这帧 ReSTIR 多追很多射线”的简单解释，尚未识别 GPU 时钟、资源依赖或调度的具体责任。

正式 `render.compare` 只覆盖 Renderer 临时参数，每个 case 暖机至少 96 accepted frames，再读取 240 个唯一完成样本；完成读取后主动取消自有任务，未触发 Capture。顺序为 global → baseline → DDGI off → baseline → shadows off → baseline。结果不是降低质量后的产品配置：

| 临时配置 | DI 均值 ms | 关键观察 |
|---|---:|---|
| 关闭空间提议，K8 | 16.975 | 快档 initial 2.441、慢档 8.756；共同慢档仍在，网格构造不是唯一原因。与开启时约 2.7–2.8 / 9.6–9.8 相比，局部提议有可测成本；整帧均值还受慢档比例影响。 |
| 关闭 DDGI | 18.889 | DDGI 本身归零，initial 慢档约 9.629，resolve 5.990；DI 慢档没有消失。慢档比例升高，不能解释成 DDGI 关闭让 DI 算法必然变慢。 |
| 关闭 Shadows | 14.621 | 可见性／着色均值降至 .428；resolve 仍 6.091，initial 慢档约 9.604。阴影不是 DI 高耗时的全部来源。此开关不关闭探针命中点的物理遮挡。 |

前后 state token、camera、基础 view 完全一致，Undo 均25、dirty均true。所有临时设置均由现有任务取消路径恢复。用户编辑器继续运行，没有用修改作者值、最终 AA 或分辨率来制造改善。

### RHI 旁证与尚缺证据

RHI 负责人只读核查：子阶段时间戳包含其间的 GPU 资源转换、依赖和调度时间；没有发现 CPU Present/fence 等待直接累加到 DI。原 240 样本中，DI 父项与四个子项和的差只有 .28–2.76 μs，未发现毫秒级漏项／重复归属。GPU timestamp frequency 是计时频率，不是核心工作频率。

独立 PresentMon 窗口采集1015帧：GPUTime mean31.94 / P95 41.47 ms，GPUBusy mean31.65 / P95 41.10 ms，GPUWait mean.29 / P95 .64 ms。它不与原240样本逐帧对应，亦不能用 GPUBusy 排除 shader 内部 stall、缓存或细粒度调度。两次 Windows 引擎计数同时看到编辑器约68–70%、DWM high-priority 3D约12%、System copy约5–6%；这些是不同 engine 的活动，不相加当显卡总利用率，不据此宣布 DWM 是根因。

本机有 ADLX 驱动库，但尚无实际频率/功率样本。WPR GPU trace 因系统性能分析策略返回 `0xc5585011`，没有提权，且未留下会话。因此“确定降频／驱动问题”仍无证据。Scene Lit 当前逻辑资源约3.90 GB（约3.63 GiB），采用完整客户区资源尺寸；这是值得后续评估的占用，不是已证实的双峰原因。

### 下一步优先级

1. **拆清稳定的重建成本。** 沿既有 RHI 时间戳把 temporal resolve 与三个 filter 步分开，避免盲改。然后独立测 guide/邻域复用、提前廉价拒绝和静态区域跳过；保留深度、薄墙、材质边界及动态显露门禁。先前已经做过的配对重建不能再次包装成新优化。
2. **解释共同慢档。** 对齐当前已保存的 UTC/完成 sequence/配置身份和实际频率／GPU trace；如需要原始 ticks 与时钟校准，由 RHI 负责人扩展，不能绕过 RHI。减少着色工作与改变运行态须分开衡量。
3. **再优化候选与复用。** 当前近源网格只依据源锚点和功率，不知道房间遮挡，也不是覆盖每盏灯影响范围的剔除表。先测同档净成本和方差，再决定 receiver 相关重要性、相干复用及廉价邻居拒绝；不靠伪造 PDF 或删去远光支持提速。
4. **DDGI 另沿真实贡献归因。** 用户新材料的一手核查见 [DDGI firefly 建议核查](ddgi-firefly-claims-20261015.md)。已有 inspected-ray 能记录局部／太阳／反馈、q、W 和 hit，先定位哪类贡献主导尖峰；感知压峰可以作为明确的呈现质量取舍评估，不能宣称保能量或根治，也不能用它解释屏幕 DI 的成本。

本节仅完成诊断、反证和设计排序，没有实施上述候选、没有新增通过或提速声明。原始文件位于 `out/di-followup-20261015/`：`initial/repeat-*.json`、`ab-*-samples.json`、`ab-summary.json`、`ab-before/after.json`、`saved-scene-lights.json`、`temporary-ab.log` 与 `rhi-runtime/`。所存 `utc_ns` 是 CPU 观察完成样本的时间，不是 GPU 校准执行时刻。

## 重建与 reservoir 热点实施（2026-10-15 后续）

沿主目录 `94d92f0e` 与上述未提交增量继续实施。新增四个独立完成计时：temporal radiance resolve、filter step 1/2/4；World 性能表与 Control 共用 21 行统计，八主阶段合计不重复加入子项。具体接口及生命周期只在模块文档维护。

独立副本使用应用负责人提供的 `--client-size 3840 2054 --no-activate`，实际 viewport 为 **2265.36×1246.12**。相机与可读取作者/视口参数恢复自用户窗口，保留 96 sources、K8、512 probes、256 updates、128 rays、gain 2、SMAA。副本默认停靠布局比原现场窄约 51 px；本表的各候选彼此等尺寸，不能将其冒充原 2316 px 视口或原生 1440p。每次暖机至少192 accepted frames，采集360个唯一完成样本，正常 Release、validation 关闭，计时期间不 Capture。用户原窗口临时切换 MatCap 以隔离负载，场景和 Undo 保留。

| 候选 | DI 均值 ms | initial P50 ms | 重建均值 ms | filter 1 / 2 均值 ms |
|---|---:|---:|---:|---:|
| 仪表化基线 | 16.247 | 7.720 | 5.845 | 2.615 / 1.228 |
| 仅固定三种 filter 步长入口 | 14.372 | 2.527 | 5.819 | 2.609 / 1.231 |
| 仅 step 1 协作缓存 | 13.265 | 2.764 | 4.823 | 1.624 / 1.223 |
| step 1/2 均缓存，已撤回 | 13.017 | 2.769 | 5.198 | 1.619 / 1.571 |
| step 1 + 同帧 target 缓存 | 12.749 | 2.816 | 4.827 | 1.614 / 1.227 |
| 再固定 temporal 两输入上界，保留 | 11.514 | 1.693 | 4.882 | 1.629 / 1.220 |
| 保留版本独立复测 | 11.774 | 1.872 | 4.893 | 1.627 / 1.233 |

这些窗口仍混合共同快慢档，不能把整段均值差全部归因于某一改动。仅固定 filter 入口几乎没有净滤波收益；**step 1 的约 .99 ms / 38% 改善在两档均出现**。step 2 更稀疏的有效 footprint 不能抵消全 halo 的读取和同步，实测退化，故移除该候选。实际 DXC 6.5 产物只在 step 1 保留6912 B LDS和一次组 barrier；temporal resolve、step 2/4 都没有 LDS或 barrier。新缓存仍是 FP32，保留法线、双切平面、粗糙度、逐材质通道资格和各信号累积次序，不承诺编译后逐位一致。

Reservoir 用原预留 DWORD 缓存所属 receiver 的 target，不增加24 B stride；满三个空间输入时目标求值最多9→6。时间复用仅当前 fresh 输入可使用缓存，前帧目标仍重算；整个 initial 的 K8 fresh 求值并没有消失。随后把已保证最多两个的时间复用输入数显式传为2，使编译器不再保留一般三输入循环；独立测量确认 initial 中位数下降。以未修改的GBuffer区间独立分组，step1-only版与最终复测在GBuffer<3 ms组的initial均值为2.830/1.911 ms，在GBuffer≥4 ms组为8.238/5.354 ms，两个组都观察到收益；这仍是按完成区间的观测分组，不是GPU时钟校准。两位 Astra 核查了空 realization 的分母责任、源映射、极大值缩放和生命周期，GPU验收以验收页实际日志为准。

最终两次 DI P95 为 **16.976 / 17.314 ms**，八主阶段合计均值 **26.489 / 27.256 ms**、P95 **35.800 / 36.301 ms**。该合计不包括所有编辑器、UI、CPU与Present工作，**不是帧率测量，也不支持稳定60 fps声明**。保留版本仍存在共同慢档；本批没有通过降低分辨率、K、光源数、AA或深度精度获取收益。

### 运行态归因的结论与边界

RHI提供只读ADLX采集器：250 ms读取adapter级核心频率、显存频率、GPU功率和使用率，不改变电源设置。`specialized`/`cache1`两轮与GPU完成样本各有约13.7秒交集。按250 ms窗口及0～500 ms接收延迟假设，频率与initial耗时关联会变号，尚无稳定证据将慢档归因于降频。部分低功率窗口耗时较长，也可能是等待或利用率变化，不等于证明功率限制。最早的`baseline`曾因相机变化中断后复用采集名，旧ADLX文件与成功的GPU样本不重叠，明确排除其跨记录相关分析；测量脚本已拒绝重复证据名并检查采集退出状态。

RHI源码审计确认GBuffer计时包含各复用目标的转换、clear、draw，initial包含shader state安装、barrier与dispatch。未发现CPU Present/fence、上一帧整段执行或读回延迟被直接错算为当前时长；但现有区间无法拆分barrier、cache/decompression、调度与shader实际成本。若继续归因，应由RHI扩展现有完成样本的原始ticks与所属queue时钟校准，或提供默认关闭的内部dispatch边界标记；Renderer不取得原生device/queue，也不新增手动flush入口。本批没有实施该低层扩展。

完整本机证据在 `out/di-reconstruction-20261015/`：候选 `*-samples.json` / `*-summary.json`、`dxil/summary.json`、两次最终 `temporal-bound` / `final-repeat` 记录、`rhi/`分析、`final-control-*.json`、构建及E2E日志。最终验收状态见 [Renderer验收记录](../modules/renderer/acceptance.md)。
