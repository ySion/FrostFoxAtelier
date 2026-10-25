# DDGI 下一批覆盖演进判断 — 2026-10-12

状态：本批机制与参数入口已落地。当前代码已实现最多四个固定/跟随体积、共享 4096 探针池与更新预算、环形历史、统一反馈可见性、反弹/最终增益分离，以及多体积诊断。Astra 修复路径复审完成；Dev/Release 联合 GPU、正式编辑器与 Control 端到端均通过。地牢同预算对照证实局部楼层布局能改善固定地板区域的支持，上层墙面与远区仍有配置缺口；这不是全建筑自动覆盖或整体光照品质已达标的结论。具体范围和证据以[验收记录](../modules/renderer/acceptance.md#ddgi-覆盖与反馈演进2026-10-12)为准。首跳 RT 与 ReSTIR DI 不在本批实现范围。

本文保留实施前的研究证据与取舍；下文“当前/待实施”均描述研究时的基线，不作为最新功能清单。最新行为见[模块文档](../modules/renderer/scene-lit.md)。研究复用[前次资料](ddgi-coverage-evolution-20261011.md)，经三位 Astra 分别审议覆盖架构、光传输和工业成本，再交叉讨论反例。核心判断不变：**两层是优先候选，交付终点应是目标视野内有合法、稳定、预算可控的 GI 支持。** 体积几何包围不代表室内已获得有效支持，仍需真实关卡对照。

## 实施前基线与缺口

研究开始时已有探针球、体积、重定位、更新年龄和 receiver coverage，当时实现只有单个有限 volume；外部查询返回零，每轴探针数上限 32、总数上限 4096。调试设施让这个限制可见，并未让它自动覆盖关卡。

因此，必须分别确认“程序是否为新版”和“新版是否具有广域覆盖”。当时即使打开新版，单 volume 外没有 DDGI 仍符合能力边界；提高反弹系数不会给未覆盖位置创建探针支持。

当时 [`open_example`](../../src/application/project.cpp) 会复位 Lit 设置，Dungeons/Physic 示例默认关闭 DDGI，也不在该路径进行 volume fit；重新打开 DDGI 可先使用默认范围。`Application::configure_view` 的一次性几何 AABB fit 是另一条设置路径，不能据此声称示例随相机或关卡范围自动覆盖。实施中已将此绕过统一发布的重置交由应用负责人移除；用户自定义布局应保留，拟合通过明确操作完成。

## 今日证据与观测边界

- 本轮现场识别记录是主目录 `out/dialog-release/atelier_editor.exe`，PID 111316，实例 `f6218bb0904bce4ab97fb8126e1afb36`，SHA-256 `d521ae969e840ff23c503c9cc6ff7edbd3b21578a56ebaa89d556b55cb12a2a4`。当时已包含新 DDGI Debug，不能再将局部覆盖归结为只打开了旧程序。
- [`SceneLitSettings`](../../src/renderer/view_settings.hpp) 的默认范围是 `[-6,-1,-6]..[6,5,6]` 米，即 12 × 6 × 12 米、8 × 4 × 8 共 256 个探针；这是代码默认值，不是本次用户现场的已读取值。
- 三次 `repro.export` 均被 `camera_pan` 保护拒绝。用户明确结束相机操作后，token 从 `:1300` 推进至 `:1331`，仍报告相同忙状态；未绕过保护。应用负责人已定位为手势结束时残留 pan 标志，改为统一的 `optional<CameraDrag>` 寿命，并完成 Dev/Release 编辑器端到端验收。本任务已核对 `out/camera-control-{dev,release}.log` 的 PASS；标准主目录 `out/release/atelier_editor.exe` 已重建，包含 DDGI 整合与此修复。用户原窗口未被关闭或保存，运行中的旧代码不会自动更新。大场景 inspect 分段读取尚未纳入该修复。
- 原始 Control 证据在本机 `out/ddgi-coverage-20261012/`。仍没有成功的当前 Receiver Coverage 捕获，因此不能给出未覆盖像素比例，或将每个暗部都判为 volume 外；输入状态修复的验收不能代替 DDGI 画质验收。
- 为判断场景级别，另行只读统计随仓库交付的 [`Dungeon.scene`](../../examples/Content/Dungeons/Scenes/Dungeon.scene)：3617 个 Actor，其中 3425 个网格、192 个灯；网格 Actor 原点跨度约 142.7 × 40.08 × 246.42 米。这是随包场景的原点分布，不是几何 AABB，也不代表用户未保存版本。仅对这一跨度均匀铺设不超过 2 米间距，就约需 73 × 22 × 125 = 200750 个格点，说明“保持当前数量，只扩大一个体积”会显著牺牲密度。

据此，把真实关卡需要的广域与多楼层支持纳入下一批是有依据的；具体粗层范围、细层间距和预算仍须在可操作的实景上确定。范围应来自需要照明的关卡区域，避免被无关远处道具或导入布局离群点无限拉大。

## 本地审议：扩覆盖前必须区分的三类问题

1. **空间没有支持。** volume 外直接缺失；volume 内也可能因楼板/墙体遮挡八邻而没有合法贡献。若粗层覆盖上述 246.42 米原点跨度，每轴最多 32 个探针意味着该轴间距至少约 7.95 米，总预算约束可能使其更稀。这样的格网包住建筑，不保证能服务每一层房间。
2. **已有支持切换不平滑。** 当前 receiver 使用真实遮挡射线剔除探针，再对剩余权重归一化。若只剩一个贡献者，即使它的未归一化权重很小，最终仍为该探针完整照度；它被遮挡时可产生跳变。连续距离权重不能单独保证输出连续。但旧楔形是否由该路径主导，仍缺逐像素归因，不能据此宣布唯一根因。
3. **缓存内的光能是否可信。** [`lit_ddgi.hlsl`](../../src/renderer/shaders/lit_ddgi.hlsl) 的屏幕查询使用真实 LOS，probe-hit 反弹反馈传入 `trace_support=false`，主要依靠距离矩；[`lit_ddgi_common.hlsl`](../../src/renderer/shaders/lit_ddgi_common.hlsl) 还让两种调用使用不同的选格偏移。receiver 看得见某个探针，不代表该探针的缓存没有从墙后收到错误反馈。此处已证的是合同差异，实际污染程度仍待隔墙对照。

当前 relocation 后的 pending、旧照度/target/moments 清理和下一轮新位置观测已经存在，不重复列作未修复缺陷。旧硬边对照只隔离到 DDGI 路径与布局/历史有关，恢复同一 CPU 场景不保证 GPU 历史相同。

最小归因应分别观察 receiver 的有效成员、权重和、最大归一化权重，以及 probe 的目标照度、实际位置和状态。封闭薄墙明暗两室比较：A 当前反馈，B 仅增加反馈 LOS 判定，C 关闭反弹反馈；B 保留 A 的邻居和权重，仅向真实命中点的数值偏移端点追踪。另组再单独改变选格偏移，避免把 `false` 改 `true` 同时改变多个变量。使用相同冷启动序列、实际探针访问轮次，并保留热编辑后的现场证据。以上是待实施的端到端诊断，不是本轮已有测量。

## 建议：统一有界 volume 机制，以实景选择布局

**固定广域粗层 + 关注区域滚动细层**仍是最接近当前实现的首个候选。少量 **room/floor 局部 volumes** 是多层地牢的并列候选：将密度投向实际房间和楼层，减少不可达空域的浪费。二者应共用一种 volume 描述、历史身份、更新调度和 gather 入口；不建设任意数量的体积编辑器、自动 portal 图或另一套 GI 系统。

| 方案 | 能解决什么 | 主要边界 |
| --- | --- | --- |
| 单层 rolling | 玩家/关注区移动时，有限高质量区域跟随，保留内部历史 | 不增加同时受支持的空间；拉远看全建筑、远处灯间接贡献、新切片预热仍可能缺失 |
| 固定粗层 + rolling 细层 | 有界关卡的广域基底与近场分辨率分开预算；细层新区可逐步接替粗层 | 粗网格不能承诺狭窄房间的遮挡质量；多一层会增加内存、更新和部分 gather 成本 |
| 少量房间/楼层局部 volumes | 在同一画面多处室内保持所需密度，降低空体积浪费 | 需要有限候选选择、重叠接替和总预算；房间标签只组织覆盖，不能切断门洞真实传光 |

布局决策先取三条固定视图路径：近景单室、最大拉远的两个重要区域、楼板上下同时可见。用同区域密集单 volume 作支持参照；两种候选在相同总驻留/更新预算下比较最坏重要房间的无支持缺口、接替稳定性和成本。若两层已满足目标，选更简单的两层；若房间/楼层局部布局解决两层无法支持的固定缺口，就用它完成原用例，不把明知存在的缺口留到下一批。若一个合适密度的 volume 已能覆盖最大目标视野，则收缩为单层 rolling。

这不是要求先实现两套完整产品再比较。先做空间容量检查 `n_i = ceil(L_i / s_i) + 1` 和少量固定 volume 描述，进入同一查询与调度路径；无需先造通用 authoring 系统。只有范围近乎无界、尺度缺口连续扩大，才进一步评估更多 rolling clipmaps；若长期需要很多不规则局部密度，才有充分动机建设稀疏 adaptive bricks。

官方 ISV 只维护有限 active space，保持内部探针不动、复用边缘切片，并不同时覆盖无限世界。[SDK：Infinite Scrolling Movement](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#infinite-scrolling-movement)。生产论文讨论 tracking window、不同密度和滚动过渡，但 §7.4 的多 volume shader 遍历明确标为 prototype；不能将其全部等同已发行游戏的实现。[JCGT 2021，§7.3–7.4](https://jcgt.org/published/0010/02/01/paper.pdf)。

## 工业方案的借鉴边界

| 参考 | 可核实的成熟度 | 本项目应借鉴什么 |
| --- | --- | --- |
| [NVIDIA RTXGI-DDGI](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md) | 官方 SDK；规则网格、scrolling、relocation 有明确合同，也承认几何厚度与密度限制 | 稳定格点、切片复用、探针状态与局部密度；保留本项目严格薄墙要求，避免照搬可见性非零下限 |
| [4A：Metro Exodus PC Enhanced Edition](https://www.4a-games.com.mt/4a-dna/in-depth-technical-dive-into-metro-exodus-pc-enhanced-edition) | 已发行产品的开发团队技术说明 | 显式 RT 首跳，在命中处求直接光，DDGI 负责第二跳及以后；递归读取上一帧。它证明职责划分可用，没有证明本项目应立刻改写或更快 |
| [Epic：Lumen](https://www.advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf) | 官方实现者讲座，[Fortnite Chapter 4](https://www.fortnite.com/news/drop-into-the-next-generation-of-fortnite-battle-royale-powered-by-unreal-engine-5-1) 有实际产品部署 | 共享命中表面的材质/光照工作、有限更新预算、按实际使用组织采样；整体移植会引入新的场景表示和重建链，超出本次覆盖任务 |
| [AMD：Brixelizer GI](https://gpuopen.com/download/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf) | 官方 SDK/研究路线；本轮未建立具体发行游戏的整库部署证据 | 多尺度缓存与有界更新；其屏幕辐射亮度注入存在离屏变化限制，不能当作现有完整 RT 场景查询的直接替代 |
| [Unity：Adaptive Probe Volumes](https://docs.unity3d.com/Manual/urp/probevolumes-concept.html) | 已发布引擎功能，主要使用烘焙间接光 | 几何相关的局部密度与砖块组织；不能用它证明实时 sparse DDGI 已有可直接迁入的成熟实现 |
| [RTXGI：SHaRC / NRC](https://github.com/NVIDIA-RTX/RTXGI/blob/main/Readme.md) | 路径追踪辐射亮度缓存的 SDK 示例；README 将 NRC 标为 experimental | SHaRC 支持 DXR 设备，但不是 DDGI 的无缝升级；官方 NRC 要求 NVIDIA Turing+ / Tensor Cores，不适合本机 RX 6800 XT |

**首跳 RT 是后续限定实验。** 当合理密度与可信反馈仍无法满足首跳结构，才在固定 ROI 比较少量真实漫反射射线 + DDGI 高阶缓存，并以高采样参考校核。成本包含 closest-hit、材质、灯/阴影、时空重建；不能由“一条 RT 代替八条 LOS”推出提速。若限追踪距离，达到上限不是环境 miss，必须定义缓存续接。镜头/物体移动出现噪点、拖影，或总成本不合算，就不推广。

实验沿用 Scene Lit、RHI 和合成入口，明确与直接 mesh-emission NEE、现有 DDGI/Local GI 的能量归属；同一首跳贡献只能估计一次。该分工减少近场首跳对 probe 插值的依赖，但不会自动净化高阶缓存内已有的错误光能。

## 必须先固定的六个合同

以下是本项目的候选合同；适用于同一机制下的固定、rolling 与有限局部 volumes。两层公式是首个配置示例，不把层数写死为另一个入口。官方依据在对应段落，超出官方现成行为的部分明确作为工程推导。

1. **空间关注点与格网稳定性。** 粗层覆盖有界的目标关卡范围；细层由可见接收面/可玩高度带的 footprint 与视图关注点决定，额外留插值和预热余量。俯视相机的位置往往在建筑上方，不能直接把相机高度当作细层中心。楼层必须保留真实三维支持，不压成地面二维缓存。固定 spacing，跨整数 cell 才滚动；普通相机移动不重新缩放或清空整个格网。这是对官方 tracking anchor 的本项目适配。

2. **世界身份与存储槽位分离。** 逻辑世界格点及格网配置代次决定历史身份，环形地址只是存储位置。滚动保留交集格点；重用槽位必须同时失效 irradiance、distance moments、relocation、classification、初始化状态及年龄，涵盖所有 ping-pong 历史。跨多格清理进入切片的并集；任一轴移动超过缓存跨度时按整层重置处理。负坐标和三轴交叉滚动不得重用错误历史；相同 anchor 重复提交应无副作用。[官方清理路径](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L180)只证明 SDK 对滚动切片有特殊清理；本项目全部状态的统一失效仍需自行落实。

3. **新区不是已就绪的黑色探针。** 重用后先标为 pending，再完成有效几何检查、必要重定位和新位置上的照明观测，才允许参与查询；因重定位改变采样点的旧射线结果不能继续代表新位置。新切片优先更新，但受总预算约束，不能一次滚动就无限追加工作。缓存就绪度与过渡带共同控制接替速度。论文将滚动细层的过渡区额外缩进一格并使其随相机平滑移动，以免离散滚动直接接管像素；本项目还要加入就绪状态。[JCGT §7.4，页 24](https://jcgt.org/published/0010/02/01/paper.pdf)。

4. **有效零照度与无支持分开。** 查询有可用邻探针、有效几何/可见性支持，但测得照度为零，是合法暗部，应占有该层权重；不能因为颜色黑就回退到更亮的粗层。覆盖外、pending、inside solid、所有方向/可见性支持被拒绝，应保留不同原因。缺失细层可以尝试粗层，但粗层也必须独立通过有效性与 receiver 可见性检查，不能直接把颜色补上。SDK gather 在无累积权重时返回零，接口本身不表达这些原因，且采用非零可见性权重下限；不能不加分辨地照搬为我们的回退合同。[SDK Irradiance.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl#L67)。

5. **同一份照明估计，不叠加两份完整 GI。** 两层都估计相同物理量。候选权重为 `w_f = b_f * q_f`、`w_c = (1-w_f) * b_c * q_c`，其中 b 为几何过渡、q 为有效支持/预热程度，均不依赖亮度；输出 `w_f*E_f + w_c*E_c`，保留剩余未覆盖权重及原因。有效细层内部即使 E_f 为零也可占满权重。屏幕 shading、probe-hit 反弹和 debug 使用同一选层规则；所有层更新从同一不可变的上一轮快照查询，避免执行先后改变传播能量。保留现有直接光/mesh NEE/间接反弹的能量归属。论文密到疏累计权重到 1 后停止；SDK `DDGIGetVolumeBlendWeight` 只提供几何权重，不能直接等同本合同。[JCGT §7.4](https://jcgt.org/published/0010/02/01/paper.pdf)、[SDK 几何权重](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl#L39)。

6. **所有 volume 共用一个整体预算。** 首次对比沿用总驻留不超过现有 4096 个探针的约束；若实景不满足，再明示内存/时延取舍，不暗中按层复制上限。总更新与射线共享，优先处理重用/失效/可见陈旧支持，并保留粗层、离屏有效支持与 inactive 几何检查的最低轮转机会。年龄用于调度，不把仍有效的旧照明衰减到黑。首版采用固定每次采样量与有界调度；不同时引入逐探针可变射线、异步队列和自动冻结。官方允许宿主低频或异步更新，但 SDK 不提供这一调度器；固定几何射线与照明射线的成本也应分开计量。[SDK Updating / Fixed Probe Rays](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#updating-a-ddgivolume)。

## 传播控制和性能优先级

当前反馈的 `0.85` 是每次递归反弹的额外损耗，会改变稳态；它不是最终画面的间接亮度旋钮。建议将 transport 近似与最终艺术 `indirect_gain` 明确区分：后者只在最终间接漫反射合成应用一次，不回写 probe 或重复放大 Local GI；前者改变时要处理历史响应。先保留现有稳定性余量，结合材质反照率和对照场景校准，不能为“传播更远”直接调高或取消。增益也不能给无支持区域创造光照。

性能按实际职责分项计量：共享 TLAS、历史传递/清理、probe 几何、命中光照/阴影、receiver gather、调度与诊断。先做确定性节省：无效探针与零空间权重提前剔除；细层合法且权重已满时停止查询粗层；总更新预算共享。保持 FP32 深度与距离矩，不使用按亮度判无效、恒定 ambient 或降低遮挡质量换覆盖。

反馈严格 LOS 的新增理论上界为 `8·U·R`，默认 U=64、R=128 时最多 65,536 段查询/更新帧；实际可被 miss、backface 和无效邻居降低。此数不是耗时，不代表整个 DDGI 倍率，更没有包含现有 NEE 阴影；应先做受控对照再决定生产策略，不直接全量铺开。

**静态或离屏不等于可冻结。** 服务离屏几何的探针仍传播高阶反弹，这是生产论文 §6.2 的明确约束。采用世界灯/材质/几何变化驱动的保守 dirty 优先级和全场最低刷新；灯移动包含旧、新影响域，方向光/环境变化是全局事件，不能永久冻结直接影响半径外的后续反弹。[JCGT §6.2](https://jcgt.org/published/0010/02/01/paper.pdf)。真正不服务几何的空域探针可以考虑休眠与唤醒；它和静态表面低频轮转不是同一资格。

ReSTIR DI 用于 many-light 候选选择及阴影估计，不创建空间支持。本批同期计量 direct 与 probe-hit shading；只有真实场景证明它们持续占据主要预算，才把 ReSTIR DI 提前。当前不因总灯数就承诺性能收益，也不新增另一套灯/发光体身份和控制入口。

## 重要反例与实现边界

- **“只扩大 bounds”**：探针数不变会降低密度，多层地牢的楼板、门洞和走廊可能失去足够支撑；volume 内不等于 GI 正确。粗层也有同样风险，不能称为无条件可靠的兜底。[SDK 几何与探针密度说明](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md#geometry-configuration)。
- **“多一层就能修掉硬边”**：层级解决覆盖/密度分配，不自动修复已有单层的分类、可见性插值或历史失效。现有硬边仍需在固定同场景对照中单独定位；不能让更亮的粗层掩盖问题。
- **“只要新切片清零即可”**：零值未区分 pending 时，会出现黑切片，或者因粗层回退过早产生闪亮边缘。SDK 用黑值影响 hysteresis 的做法并不等于我们应该用黑色代替显式初始化状态。[SDK blending](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl#L448)。
- **“近场跟相机就好”**：高空俯视、正交缩放、楼层切换及远近多个可见区，都能使相机中心与实际 receiver 分布偏离。细层应覆盖场景关心的区域，粗层承担其余可见区域，不能暗中改变 Play 与渲染解耦的既有约定。
- **“两层仍是原先射线数，所以同成本”**：第二层还增加 atlas/状态存储、更新/清理，以及过渡区域额外 gather；probe-hit 的材质/灯光/阴影求值也可能比主射线更贵。必须在 RX 6800 XT 实景分别测量，不能引用厂商毫秒数。

## 建议的完成顺序与端到端终点

1. **取得基线与正确性归因。** 读取实景 bounds/spacing，固定最大目标视野及楼层；区分 outside、pending、inside、全部遮挡、少量主导支持和有效暗部。完成薄墙反馈与旧楔形 ROI 对照，不凭亮度截图单独归因。
2. **统一状态与空间组织。** 实现同一 volume 的历史身份、整数格点 rolling、共享快照与预算。用单层确认生命周期，再按上述同预算实验选择两层或有限局部布局；固定 volume 复用相同状态机制。
3. **接齐实际可用入口。** UI/Control 共用设置与约束；显示实际 bounds/spacing、驻留/更新预算、反弹与最终增益的区别；Debug 标明 volume、probe 状态、receiver 来源与支持缺失原因。按需采样诊断，不增加常驻逐探针 readback；RHI 能力缺口通知负责人。
4. **完成端到端与实机计量。** 连续多轴移动/负坐标/传送、拉远/楼层切换、有效黑暗室/新切片、动态门/遮挡体、离屏灯和发光材质变化、重复同配置/关闭重开。确认交集历史保留、旧槽位完整失效、跨层不额外增亮、合法黑色不错误回退，以及远层不饿死。
5. **决定后续升级。** 记录关闭诊断时的稳定 GPU 分项分布/显存，以及冷启动/传送响应；诊断捕获单独进行。覆盖与反馈达标后，若首跳结构仍不够，再做 Metro 式受限 RT 实验；若 many-light 成本主导，再提前 ReSTIR DI。两者都以本机结果决定，不按技术名称排序。

研究阶段未测定最优 spacing、volume 数、显存或帧时。上述清单是方案的验收目标，不自动成为已有证据；实施后的通过范围与测量结果由[验收记录](../modules/renderer/acceptance.md)维护。

## 研究账本

三份独立审议保存在本机 `out/ddgi-astra-20261012/{architecture,transport,industry-cost}.md`，分别含源码依据、反对意见与来源清单。Exa 检索请求计数合计 `sources_reviewed = 15 + 10 + 17 = 42`，这是各次 `numResults` 之和，不是 42 篇全文阅读；跨审议去重后使用 14 个一手 URL，其中 Fortnite 产品部署公告仅核验搜索摘要，其余为正文/源码重点段落。主任务另复核 4A 的 DDGI 原文与 NVIDIA 当前 RTXGI README，不增加唯一来源数。

工业来源的支持限于各段标明的能力、部署或硬件约束；本项目布局、严格可见性、能量/历史合同和实施顺序是结合当前代码提出的工程判断。文档更新只检查内容与链接，没有以重复 GPU 验收充当研究成果。
