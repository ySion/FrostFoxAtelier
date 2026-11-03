# Renderer 验收

本页说明 Renderer 的端到端验收入口各自证明什么、怎么运行、哪些还没覆盖，以及历史性能基线。行为和参数见[显示合同](contract.md)、[Scene Lit](scene-lit.md) 与[材质系统](materials.md)；某次运行的结果看门禁日志，历史过程看 Git。

## 运行

- `renderer_acceptance` 聚合三个真实窗口/GPU 入口：`renderer_acceptance_default`、`renderer_acceptance_scene_lit`、`renderer_acceptance_material_programs`。每个入口独立进程，限时 1800 s，任一失败即整体失败。日志写在构建目录的 `renderer-acceptance-<入口>.log`；有参数入口的图像与数据写在 `renderer-evidence/<入口>/`，默认入口的截图写在可执行文件目录。也可以直接运行 `atelier_renderer_acceptance --scene-lit|--material-programs --output <绝对目录>`，默认入口不带参数。
- 验证层和窗口锁以 [README 的构建与端到端验收规则](../../../README.md#构建与端到端验收)为准。三个验收入口自身请求 GPU-based validation；环境变量 `ATELIER_GPU_VALIDATION=debug` 会把进程内的 GBV 请求降为 debug layer。`--scene-lit-benchmark` 关闭验证，计时采样期间不读回图像。
- 检查不依赖 `assert`，Dev 和 Release 中同样生效。保留标准见[工程指南](../../engineering.md#验收保留标准)：同一行为一个代表、期望来自独立作者值或解析真值；不以另一帧或另一个执行后端的输出作为期望，不展开参数/规模矩阵。
- 硬件：RHI 要求同时支持 Mesh Shader、DXR 1.1 inline RayQuery 和 SV_Barycentrics 的 GPU。既有阈值和历史读数来自 RX 6800 XT，其他 GPU 没有跑过。窗口必须可见，运行期间桌面不能被其他全屏程序占用。

Renderer 消费者（默认入口、Scene Lit fixture、原生材质批次）共用可执行文件目录下的 `renderer-shader-cache/`，即通常的 `out/<配置>/renderer-shader-cache/`；不随当前工作目录或 `--output` 改变，也不使用 Editor 的用户缓存。缓存只复用已有 RHI 编译阶段字节码，不保留 GPU 对象或 PSO，缓存失效仍按原路径重编译。材质程序的直接 RHI 消费仍保留自己的证据目录缓存，`acceptance/rhi/preparation_cases.cpp` 的缓存验收不变。

默认入口先清空相邻的 `renderer-preparation-shader-cache/`，在同一窗口用独立 Renderer 保留首次 `not_ready`、准备期间工作室绘制和就绪 Lit 的序列，关闭后才打开使用持久缓存的 Renderer。热跑仍包含这段冷准备，不能把总耗时等同缓存命中成本。

## 默认入口

- **K6**：冷准备序列保持作者 Capture 哨兵；641×385→703×427 后按新尺寸接受一帧；首轮停止检查即真时返回 `cancelled`、零 accepted id、作者 Capture 不变，并在 HWND 销毁前正常 close。取消代表不证明中途驱动调用可抢占或硬性关闭上界。
- **K5**：一次非对角 view/projection，读回深度对作者公式 `1/(9.9×3.5)−.1/9.9`，检查 CPU→shader 相机 ABI。
- **K4**：一张圆角/边框/RGBA/clip/A-B-A 混合图、一张 perspective far=4 的选中裁剪图、一张 hidden-line 图。真值分别为作者纹素与固定混合值、作者跨远平面几何、作者 quad 边界及 depth=.8。
- **K6**：UI RGBA revision 替换与清空时的 weak owner 退休；材质图片替换、opacity 消费及空几何后的最终 owner 退休。它们对应不同 GPU 缓存，不重复 CPU 资源租约验收。

## `--scene-lit`

这是其余 Scene Lit 图像代表的唯一入口，每个特性保存一张图：

| 图像 | 独立真值 |
|---|---|
| `area-dielectric.png` | 现存双精度矩形面积 BRDF 积分，不调用 LTC；作者正交相机和平面确定样本域，不由实测 depth 选样本。沿用 HDR 半精度、±1 显示码、镜面 25% 及真值峰值 0.5% 的容差。 |
| `shadow-point-settled.png` | 作者 receiver z=4、blocker AABB 和 point light 的 segment-box 几何；缩小遮挡体排除轮廓边带，保留绝对暗界和作者亮区，不再用无阴影参考帧。 |
| `sky-marker-yaw-35.png` | 作者标记 texel 方向与 35° yaw 的独立投影质心，沿用 1 px 容差。 |
| `debug-02-coverage-dark-outside.png` | 作者探针体积和接收点；有效零照度支撑为青，体积外为蓝，不与另一个深度输出互比。 |

**K6** 与这些图共用夹具：移除同一 SkyMap 后作者背景为黑且 weak owner 过期；空场景退休 raster/ray Mesh，weak owner 过期且 depth=0，再在同一 Renderer 重入作者平面、释放材质并关闭。中间帧不另存代表图。旧 temporal、quality、stability、clustered、shadows、cost、sky 独立验收入口和矩阵已移除。

## `--material-programs`

- **K5**：真实 Source→HLSL→GPU 的一个 ProgramB 采样记录，作者 repeat UV=(-.5,.375)、位移与 texel 权重 .25/.75、字面参数推得期望；full 和 coverage-only 各对同一独立真值，不互相比。另保留一个 ordered-below-mid 数学输入元组，clamp/lerp/fract 的结果为作者字面值；不保留 CPU evaluator 对照、第二程序或参数矩阵。
- **K4**：原生 Renderer 批次保留 `native-smooth`（作者红/绿 Mask 与 .8/.4 深度）、`native-default-smooth`（公开不透明品红）、`native-final-uv-mips`（等面积红蓝棋盘的可见均值及 .8 深度）、`native-ddgi-masked`（无灯/环境、视锥外负 coverage 发光面经 GI 不透明近似照亮接收面，三通道 >10），每项一图。
- **K6**：同一 batch 序列覆盖 live batch 阻止 close、已准备但未采用图片跨旧帧保活、采用、最后使用不 capture/finish、释放后继续提交/复用，最终正常 close。

图像与光照夹具通过 `standard_fixture.inl` 作者值构造真实 StandardPBR MS/MI，再提交同一公开 MaterialBatch API；生产中不存在固定材质兼容入口。正式 Editor/presentation 绑定、保存/历史和 accepted-frame 点击由其行为所有者负责。合法 ProgramSurface 自然触发 DXC/PSO 失败仍无确定性夹具，不以取消或前端拒绝代替。

## `--scene-lit-benchmark`

这是独立测量入口，不进入 `renderer_acceptance`、`acceptance_window`、`acceptance` 或生成的窗口验收清单，因此门禁 `-Full` 不执行它。保留独立目标 `renderer_acceptance_scene_lit_benchmark` 以及直接运行 `atelier_renderer_acceptance --scene-lit-benchmark` 的方式；单独运行仍须遵守 [README 的窗口锁规则](../../../README.md#构建与端到端验收)。

不开验证，原生 2560×1440 room；计时采样期间不读回图像，结束后保存一张证据图。检查完成样本的合同：异步样本属于已提交的帧，各级 timestamp 有限、非负，子区间属于完成的父区间，DDGI 分类查询是全部可见性查询的子集，固定场景在同一完成样本里执行除阴影 atlas 更新外的每个计时区间。同一入口再测有无天空贴图两种情况。它输出耗时，不以耗时作为通过条件。设了 4K 贴图后 compose 中位数从 0.498 ms 变为 0.576 ms，GPU 合计从 4.04 ms 变为 4.12 ms，DDGI 主追踪不变；贴图上传帧的分项见 [Scene Lit](scene-lit.md)。

全项目保留的五个测量入口以[工程指南](../../engineering.md#验收保留标准)为准；本模块只拥有上述 benchmark，不改其他四个入口。

## 编辑器侧的渲染验收

Renderer 与 Editor 合计每个渲染特性只保留一张代表图；只有必须走原生窗口/输入路径的行为才由 Editor 保留。编辑器集成、输入、作者 Scene 保存/加载与撤销的当前清单见[编辑器](../editor/README.md)和 [Control](../editor/control.md)，不在这里复制另一套清单。Renderer 不保留 Dungeon serialize 自比较来代表 K1。

## 未定因的窗口失败

材质接入期的窗口间歇失败仍未关闭。以下是 Windows x64、RX 6800 XT 上已记录的事实；原始日志位于本机 `out/acceptance-gate/<运行号>/`，只证明当时版本、配置与场景，不作为当前门禁结果。

**已确认：**

- Project 组合目标在 `20261028-233948-063-67820` 因菜单子命令失败仍记 FAIL，后续 `20261029-044822-174-17612` 的 Release Project 原生窗口完整 PASS。Renderer Dev default 在 `20261028-231018-738-28932` 失败，后续 `20261028-233948-063-67820` 全入口 GBV PASS；原失败与后续通过是不同运行，后者不关闭前者的归因问题。
- Editor 受控反例 `20261029-044541-153-32708` 在 F10 与 Left 之间短暂失焦再恢复，真实 WM_KILLFOCUS/capture changed 关闭菜单；后续键盘入口焦点检查仍通过，却出现同一导航失败。
- Dev Renderer 诊断 `20261029-043215-721-19504` 对同一已准备 Lit 输入观察到：正常焦点、移交焦点、完全覆盖均 accepted；最小化为 skipped/draws=0，恢复后 accepted。该诊断子集省略完整 PASS 标记，运行器记录 FAIL 是预期，不能算完整 Renderer 验收通过。

**未确认：**受控失焦仅证明这种机制足以致错，**不证明历史 Editor 失败由它引起**。旧 Renderer 失败缺少当帧 status/capture/draws/HRESULT，至今未定因；窗口状态观测及后续 PASS 不能补足旧失败的缺失证据。

**处理规则：**当时调查未增加 sleep、盲重试、放宽断言或改变产品失焦语义；后续处理也不以这些方式换取通过。保留原始失败与后续通过的区别，依据失败当帧证据定位；证据不足时保持“未定因”，不以重跑通过宣称问题关闭。

## 当前性能

以下是不同阶段的历史测量，不是最新 main（含后续相机／Runtime 集成）的性能承诺。

### P1 之前的历史基线

`atelier_editor --profile-render-performance`，Release，原生 2560×1440 最大化视口，不开验证，VSync 关；两轮交替，取两轮 p50 的均值，单位 ms。场景是 Examples 的新 Sponza（573.6 万三角形，示例相机，DDGI 16×12×14、每帧更新 672，带天空贴图），提交 de6627b2，渲染代码与 63de8635 相同。

| 情形 | GPU 合计 | gbuffer | direct | ddgi（update / gather） | ao | reflections | hdr_resolve | display |
|---|---|---|---|---|---|---|---|---|
| Scene Lit 静止 | 15.20 | 5.33 | 1.41 | 2.87（1.09 / 1.78） | 1.88 | 2.76 | 0.52 | 0.42 |

- 静止时 wall 15.44 ms，约 65 帧/秒。目标是 RX 6800 XT 上 1440p 120 帧/秒，即 8.33 ms。
- 相机环绕时 GPU 合计 23.73 ms，其中 shadow_cache 6.14 ms（级联每帧重画整个网格）、ddgi_gather 4.27 ms。
- Smooth / MatCap 每个几何批次一次 draw，与按 part 绘制的 63de8635 交替测量：wall 4.70 / 4.23 ms（按 part 时 5.01 / 4.47），没有 GPU 计时；draw 由 444 降到 2，CPU `render.encode.scene.draws` 由约 1.56 ms 降到 0.026 ms。CPU 节省大于 wall 的下降，说明这两种模式在 Sponza 上主要受 GPU 限制。Scene Lit 与 Room、Dungeon 两场景没有可分辨的变化。
- 反射比旧 Sponza 多出的约 1.86 ms 是推断，还没有归因；性能报告的每个 Scene Lit 样本带 SSSR 的 `reflection_work_items`，可用于这项归因。

### P1 历史收益与已接受的性能取舍（2026-10-25）

宿主是 Windows 上的正式编辑器 `atelier_editor --profile-render-performance`，不是独立渲染微基准；RX 6800 XT、Release、接收视口 2560×1440、GBV/VSync 关闭。Dungeon 最终四份数据在渲染工作树 `.amp/in/p1/perf/dungeon-finalclean-{p0,p1fixed}-{1,2}/render-performance.json`；顺序为新、旧、旧、新，每边两轮，每个 Lit case 预热 192 accepted 帧、240 个完成 GPU 样本，CPU/wall 为 243 个 accepted-step 记录。下表为两轮分位数的均值，单位 ms，不混合重算分位数。

| Dungeon | GPU p50（旧→新） | CPU Lit p50（旧→新） | wall p50（旧→新） | wall p95（旧→新） |
|---|---|---|---|---|
| 静止 | 7.946→7.956 | 2.370→2.610（+0.240） | 8.289→8.285 | 8.771→9.254 |
| 环绕 | 8.447→8.429 | 4.036→4.627（+0.591） | 8.751→8.708 | 13.186→14.687（+1.501） |

相机、接收尺寸与工作量一致：静止 111 次场景 draw；环绕阴影记录逐项一致，中位 271 面、662 draw、静态／动态各 4 次更新。环绕 wall p95 的旧两轮为 12.690/13.683，新两轮为 14.573/14.802，范围不交叠；静止 wall p95 轮间范围交叠。GPU 中位恢复不等于 CPU 或尾延迟无回退；+1.501 ms 约为 120 FPS 帧预算的 18%，不能宣称稳定 120 FPS，也不据这些分位数保证稳定 60 FPS。

版本证据边界：旧报告自报 `2069cc63a4d286b33f702d46db9e2201ec24ddf5 + local changes`（schema v5），新报告自报 `2f0c2273a6d8ba194bdd47e8f82321151c863486 + local changes`（v7）。新二进制与后续 36548664／b3fc05f5 的对应来自当时交付记录；**没有采样时完整 EXE/DLL/资源制品清单的散列封存**，不能仅凭自报 revision 证明与某个干净提交逐字节一致，或把旧数据称为当前 main 的结果。Scene v8/v9 副本和旧报告字段差异保留在原始数据中。

原始 P0→P1（2069cc63→9ed0761e）Sponza 两轮数据保存在性能工作树 `.amp/in/sponza/runs/*-{2069cc63,9ed0761e}-*/`：静止／环绕 GPU p50 为 15.857→14.337、24.818→18.727 ms；Smooth／MatCap wall p50 为 4.624→2.937、4.064→2.340 ms。这是另一个历史配对，不与上述修复样本拼算。在渲染工作树另一个 1600×900 Control 流程中，首次上传 CPU 为旧两次均值 688.150→新一次 770.506 ms（+82.356）；完成相同 Lit 流程后进程 Dedicated Usage 为 9,877,245,952→7,342,018,560 B，减少约 2.36 GiB。后者是 Windows 进程显存计数，不是逻辑资源字节；新旧 `scene_lit_resource_bytes` 口径不同，不据其差值计算显存节省。

总负责人已明确接受历史 Dungeon CPU／wall 尾延迟取舍，保留 P1 收益，不扩绑定架构或 P2，不把新一轮匹配基线性能测量设为收敛前置。Room 旧配对的环绕 GPU 3.953→4.003 ms 仍是未归因的小幅差异记录，**不再列为待启动的复测门槛**，也不改称“无回退”。[installed/bound IA 修复及同 command list 验收](../rhi/acceptance.md#只读-buffer-状态合并2026-10-25) 已完成真实红绿验证、两轴独立复审及主线集成；复审核对代码和既有原始日志，不是重复执行。受影响目标 Dev/Release 构建及 Release RHI GBV、`--scene-lit-shadows`、`--scene-lit-quality`（RT/几何生命周期路径）均通过；本批没有性能测量，不据正确性通过更新上面的历史性能值。原始日志在 `.amp/in/artifacts/p1-installed-ia-20261025/`。

## 未覆盖

- Cavity、法线图、网格/轴、AO、SSSR、T2x、HDR/Bloom 的 K4 输出自比较已删除，不留人工图或新增 golden；保留的代表不声称完整覆盖这些特性。
- 只在 RX 6800 XT 上有历史运行证据；其他厂商、其他驱动和低端硬件没有验证。LTC 掠射、多投影、多光源、AA 和预算矩阵不由单个面光代表覆盖。
- 贴图流送、大规模材质压力、透明混合、蒙皮几何没有实现，也没有验收。
- DDGI 开启时，Scene Lit 画面在两个进程之间不逐位复现。历史 Sponza 1600×900 同相机/设置的第 2、16、64 帧最多差 3761 像素、最大差 2 级；再开 SSSR 与 GTAO 最多差 12976 像素、最大差 4 级。深度逐位相同；关闭 DDGI、SSSR、GTAO、AA 与 Bloom 时画面也逐位相同，原因尚未定位。不能用跨进程输出互比替代独立真值。
- 性能只测过 room、Dungeon 和 Sponza 几个场景；benchmark 和 profile 的耗时不是通过条件。本页没有把历史性能或旧验收结果当作删减后的正式门禁结果。
