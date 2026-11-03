# RHI 验收与证据

最近一次完整 RHI 验收于 2026-10-29 在 Dev、Release 两种配置通过，基于 `7ade1ab159a9698583680a3c24af784993a8cd8e` 加权威工作树的未提交候选。日志位于 `out/acceptance-gate/20261029-044219-650-39904/`，两配置主进程与三个后端回归均退出 0，gate 验证级别为 GBV；各回归的实际验证范围见下表。

准备中关闭的存活对象检查现在明确区分预期 LIVE_* 报告和其他 warning/error；后者在清队列前拒绝。真实 InfoQueue 普通 warning 注入在修补前被漏报（`20261029-043215-721-19504`），同一注入修补后使主进程退出 1（`20261029-043800-657-39200`）；临时注入移除后才执行上述正常两配置验收。四类泄漏计数和阳性对照未放宽，未据此声称此前已有隐藏 GPU 警告。

下文基础矩阵保留 2026-10-16 的原始计数，覆盖当时基线 `5e5d73c8ce1fbf4c07a100dc4cd61e42ed939f54` 加矩形清除与固定 depth bias；2026-10-19 异步准备批次当时仅在 Release 验证，现已随完整入口通过两配置。2026-10-27 的记录见 [RAW SRV 接入](#raw-srv-接入2026-10-27)。本文记录实际运行证据，不替代 Renderer/Application 的产品验收。

## 当前保留代表与验证范围

2026-11-02 的裁剪仅修改验收及运行清单，产品合同和实现不变；冻结件基于本地 `main` 的 `4a488a288add118471d23ba45cc3a03549594a4d`。主树整合同时纠正快速门禁改为 debug 后遗留的 GBV-only 断言，未修改 gate 或产品。`out/acceptance-gate/20261102-072619-099-43904/` 记录 Dev/Release `all` 通过、Release `rhi_acceptance` 在默认 debug 下通过（7.34 秒），主进程与两个后端回归退出 0；双配置实际编译数据库的 `atelier_style` 各 8 个 TU、零发现。本次没有运行 Dev RHI 或 GBV，GBV 留待下一次 `-Full`；不能将此结果当作 GBV 通过或与旧完整矩阵比较提速。

- Compute → Mesh → Vertex 保留 6 帧的提前释放、管线替换和 90 个参数字；Mesh/Vertex 各读回一张代表图，共 4,096 个颜色像素与 2,048 个深度像素。不缩小帧槽回绕序列。
- RAW/Structured 字节布局与 range 偏移各保留一个原有代表；跨阶段根参数用固定输入核对 RGBA `64/110/37/255`，indexed indirect 用 `91/0/0/255`。冷大 CBV 搬表只派发一次，独立期望为 73，保留 [a1eb44b7](https://github.com/ySion/FrostFoxAtelier/commit/a1eb44b7f3e6f1363fb60e5e6357d32f3908fec7) 中已修 UAF 的受控反例条件，移除 64 次 finish 与 single/multi 矩阵。
- FP16 MRT、D32 SRV、mip 2 与 GPU 生成 indirect 各保留一个结果；64 个 indirect word 的独立期望为 `i*7+19`。移除 timestamp/profile 矩阵。RayQuery 的 build → trace → update → trace 与提交前退休保留，仅删除 AS DataSize 观察。
- atlas 固定 mip 2，保留目标 slice 与未改动邻 slice，共 1,536 个字；按输入矩形和固定颜色/深度核对。depth bias 仅留 Vertex 正 clamp 单图，以解析式 `.25+.125*(x+.5)/64+.001` 核对，不再用另一张 GPU 图作为期望。
- DXIL 缓存 K1 不变；异步成功派发由缓存场景覆盖，准备中采用/丢弃/关闭仍保留 48 作业、live-object 阳性对照及零泄漏检查。普通 warning/error 仍不能被 LIVE 报告吞掉。
- 后端回归只保留 `null-list` 与 `timeout`；后者仍建立真实阻塞，执行 timeout → 失败销毁保留 → 放行 → 排空，以外部读回计数严格为 1 核对。重复 Mesh PS 图归入主场景；拒绝与错误文本矩阵、额外模式和性能阈值删除。
- `shared_buffer_reads_and_writes` 按裁定删除，其特定 IA/range 再绑定序列不再由此验收保留；现有 Full GBV 只覆盖实际执行的消费者，不声称等价替代该序列。历史红绿证据保留在下文。

窗口函数与窗口相关正常观察尚未删改，待 Editor 日志零错误冒烟通过后再实施已批准的窗口裁剪；窗口故障造景与 timeout 清理序列继续保留。旧完整矩阵的通过与耗时不能当作本候选通过或提速的证据。

## 命令与环境

在仓库根目录的 PowerShell 中执行：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target rhi_acceptance
cmake --preset release
cmake --build --preset release --target rhi_acceptance
```

快速门禁设置 `ATELIER_GPU_VALIDATION=debug`，正式消费者须报告 `debug-layer=1 gpu-validation=0`；`-Full` 对此目标设置 `gbv`，未设置环境变量的直接运行也保留 GPU 验证请求，须报告两者均为 1。验收期望仅由输入模式推出，实际标志另行核对并打印；debug 通过不代表 GBV 通过。

`rhi_acceptance` 运行正式消费者和 `null-list`、`timeout` 两个独立后端回归。主进程上限 180 秒，各回归上限 60 秒；这与 Context 的单次 GPU 等待预算不同。检查在发布构建中同样有效。以下环境表对应裁剪前的历史运行。

| 项目 | 已验证条件 |
| --- | --- |
| 平台与硬件 | Windows x64，AMD Radeon RX 6800 XT，vendor 0x1002 |
| 驱动与帧槽 | 32.0.21045.5002，3 个帧槽 |
| 正式消费者与 Mesh PS 回归 | D3D12 debug layer + GPU-based validation |
| null-list / timeout 回归 | D3D12 debug layer |
| 依赖与工具链 | 版本、归档与 hash 以 [依赖锁](../../../cmake/dependencies-lock.json)、[工具链锁](../../../cmake/toolchain-lock.json)为准 |

## 历史完整验收矩阵

以下场景在裁剪前的最近一次 Dev、Release 验收均通过，各进程退出 0，D3D12 InfoQueue 无非预期 warning/error；存活对象阳性对照的预期 LIVE_* 报告单独计数。它们不代表当前候选仍逐项执行这些矩阵。

| 场景 | 实际检查 |
| --- | --- |
| Compute → Mesh → Vertex | 6 帧；24,576 个颜色像素、12,288 个深度像素、90 个 GPU 参数字全部匹配 |
| 参数与生命周期 | 同帧换值/换 buffer、跨帧常量、编码后提前释放、在途管线替换 |
| 管线准备 | 26 个错误候选被拒绝，旧管线保持；可选 GS/AS 缺失可用，存在但错误时拒绝 |
| 执行域 | 异线程拒绝；Pipeline 保持执行域；有 Pipeline 时 close busy；重复 close 成功；4 类非法创建输入在设备创建前拒绝 |
| 根参数预算 | VS/PS 各 32 DWORD 加 SRV；CS 64 DWORD 加 UAV；混合根常量/CBV、跨阶段不同偏移及更新结果正确 |
| 间接命令 | 单次/多次 compute、连续 64 次 finish；Vertex、Indexed Vertex、Mesh 各 2,048 像素正确 |
| Buffer range 复制 | 上传偏移 256、读回偏移 512；8,192 图像字节匹配，前后哨兵不变 |
| 绘制前窗口同步 | 6 次隐藏窗口 resize；每次首次 Present 即使用新尺寸；285,531 个 RGBA 像素全部匹配，详见下一节 |
| inline RayQuery | SM 6.5 compute；同次提交中 BLAS/TLAS build → trace → update → trace；命中、mask、instance、primitive、重心、最近距离和 first-hit 遮挡正确；提交前释放后的退休正确 |
| Scene Lit 资源 | 双 FP16 MRT，包含 HDR 值；D32 深度 SRV；mip 2 顺序 UAV/SRV；GPU 生成间接派发 |
| 原始 GPU 时间戳 | 8 个 query × 6 帧，共 48 个样本；完成序号、重复读取、旧平滑接口和 GPU 结果读回关联正确；finish 不推进录制序号 |
| 阴影 atlas 局部清除 | D32 与 RGBA32F、3 个 mip × 2 个 slice；32,256 个读回字匹配。半开矩形及边缘像素、tile 外保留、viewport/scissor 独立、空列表、非法批次整批拒绝、原整图清除入口、深度比较采样和编码后退休均通过 |
| 固定光栅 depth bias | Vertex/Mesh 各 7 种状态 × 2,048 像素；默认/显式零、正负 constant、slope-scaled、正负 clamp、非有限输入拒绝并保留旧值、prepare 后 DrawState 修改不影响已有 PSO 均通过 |
| null-list / Mesh PS | 无 command list 的 interop 清理；Mesh PS 回归检查 2,048 个像素 |
| 有限超时与严格一次执行 | 真实 queue 等待 CPU gate；失败后拒绝录制/推进/reset；放行后只排空，GPU 计数严格为 1 |
| 异步准备（2026-10-19，仅 Release） | 工作线程准备的 compute 管线算出 41→42；编译失败以值返回（shader_failed）；非法描述在提交时拒绝；同一作业只能取一次 |
| DXIL 缓存（2026-10-19，仅 Release） | 未命中写入 1 个条目；同步与异步命中都不改写条目；截断和翻转一字节的条目被重编并恢复为相同字节；读取过 include 的阶段不入缓存 |
| 准备中关闭（2026-10-19，仅 Release） | 48 个作业未完成时：有 Pipeline 则 close 返回 busy；阳性对照能检出存活对象；丢弃一半作业后 close 0.013～0.035 s 完成；之后 take 返回 closed；命令分配器、命令列表、PSO、根签名存活数为 0 |

生产调用经 `Context` 与许可的 gfx 借用范围执行。验收中的原生 fence gate、interop 和外部 readback 只用于验证对应原生边界，不是业务接口。

本批独立工作树的构建与验收日志位于 `out/rhi-shadow-support-20261016/`；标准矩阵逐进程日志仍写入 `out/<配置>/rhi-acceptance.log` 和 `rhi-regression-*.log`。Dev 与 Release 的矩阵退出码均为 0，新增场景输出 `Shadow atlas RHI acceptance: PASS`。非法输入、错误 shader 与 timeout 拒绝场景会产生预期的 gfx 错误输出；通过条件是实际结果和 D3D12 InfoQueue 检查，不是 stderr 为空。

## 异步准备与 DXIL 缓存（2026-10-19）

状态：rendering 分支，基线 integration/ec-4 `80de8156`，本地提交，未推送。合同见[异步准备与 DXIL 磁盘缓存](contract.md#异步准备与-dxil-磁盘缓存)。

- 运行：Release `rhi_acceptance`（主进程与三个回归），退出码均为 0，主进程输出 `RHI acceptance: PASS`，D3D12 InfoQueue 的 warning/error 为 0。新增场景在 [preparation_cases.cpp](../../../acceptance/rhi/preparation_cases.cpp)，全部在 GPU-based validation 下运行。
- 日志中的一批 `Live ID3D12RootSignature/PipelineState` 警告来自阳性对照：故意在 Pipeline 仍存在时调用 `ReportLiveDeviceObjects`，证明泄漏检查能看到对象；关闭后的同一检查计数为 0。
- Dev 未在本批运行：按分层门禁规则，改到 RHI 的集成轮次由门禁加跑 Dev 窗口层。
- 准备期间 GBV 预热的成本与收益在 Renderer 层测量，见 [Renderer 验收](../renderer/acceptance.md#运行)。

## 只读 buffer 状态合并（2026-10-25）

初版（36548664）受影响的 RHI、Renderer 与编辑器目标在 Dev / Release 构建通过；Release `rhi_acceptance` 主进程与三个后端回归均退出 0。主进程使用 GBV，最终 InfoQueue warning/error 均为 0；Dev 本批只构建，不重复运行窗口矩阵。

[资源消费者](../../../acceptance/rhi/resource_cases.cpp)的初版真实 GPU 循环让同一底层顶点/索引 buffer 经 range 别名交替作为 IA、像素 SRV、compute SRV；两次连续 UAV 读改写之间保留顺序，下一轮由 COPY_DEST 覆写顶点数据，然后重新绘制。四轮逐像素核对 8,192 个 RGBA 像素，并核对四个 compute 读回值，覆盖合并的读状态离开到写状态、UAV → UAV 和写 → 读。既有 BLAS/TLAS build/update → RayQuery 与间接派发消费者同时通过。

初版日志保存在 `.amp/in/p1/rhi-final.log`；当时 `shared-buffer-reads` 输出 PASS。这些是本工作树的本机证据，不表示其他工作树已经验证。

独立审查发现初版写前解绑错误地匹配逻辑 `bound_*`，而非实际 `installed_*`：draw A → bind B（不 draw）→ range 写 A → bind A → draw 可漏掉 IA 重新安装。初版用例的写入均在该 command list 首次 draw 前，每轮 `finish()` 重置安装标记，不能证明这一分支正确。

修复仅把两处资源匹配改为 `installed_*`，原有 transition/UAV/AS 同步不变。同一 command list 回归先 draw A，再经 range 执行 COPY_DEST/UAV 写入，最后重新 draw A；四轮中两轮插入只 bind 不 draw 的 B，两轮保留 A，继续核对原有像素和 compute 真值以及 InfoQueue。

本轮真实红绿已通过：相同新增用例在旧 bound 匹配下连续两次退出 1，均报实际状态 0xC0 缺 INDEX_BUFFER（0x2）与 VERTEX_AND_CONSTANT_BUFFER（0x1），并因像素错误失败；改为 installed 匹配后四轮 8,192 像素与四个 compute word 正确，`shared-buffer-reads` 和 Release RHI 主进程、三个后端回归均 PASS。主进程启用 GBV，最终 InfoQueue warning/error 为 0；日志中故意超时、非法输入和 live-object 阳性对照保留，不是忽略了意外错误。

Dev/Release 的 `atelier_rhi_acceptance`、`atelier_gfx_regression`、`atelier_renderer_acceptance`、`atelier_editor` 均以 Jobs=2 构建通过，Dev 不另跑窗口；两配置重放后的 gfx.cpp 散列相同。证据在渲染工作树 `.amp/in/artifacts/p1-installed-ia-20261025/`：`red{,-repeat}-rhi.log`、`green-rhi.log`、`green-rhi-run.log`、两配置构建日志及制品散列；`red-to-green-gfx.diff` 证明生成实现仅有两处匹配差异。该修复已完成两轴独立复审并随 `29164fd3` 进入主线；复审是代码与既有日志核对，不是再次运行，GPU 结论仍限于该候选和本机硬件。

## RAW SRV 接入（2026-10-27）

可编程材质 B0 的参数加载使用 `ByteAddressBuffer`。固定 gfx 已识别该反射种类，但原 SRV 分支仍按逻辑 stride 创建 Structured 视图；本批补丁复用 `raw_access_`，为 RAW SRV 设置 `R32_TYPELESS`、零结构 stride、RAW 标志和以四字节为单位的范围。RAW 不再使用逻辑 stride 的准入/警告；最小有效范围是一个四字节 word，Structured 分支保持原样。

[资源消费者](../../../acceptance/rhi/resource_cases.cpp)在同一次 compute/readback 中对照 RAW/Structured：84 字节非对称数据、逻辑 stride=12，公开 range 为 offset=48、size=24，RAW 尾视图为 offset=80、size=4。RAW 视图起点遵守 D3D12 的 16 字节对齐；检查跨记录读取、最后 word、视图字节数及 Structured 元素数/stride。尾视图区分错误使用 size<逻辑stride 拒绝完整 word 的实现。八组 uint4 的实际值和独立期望均写日志，另检查真实 GBV InfoQueue。

同一验收源码在旧补丁下的 Release 红轮 `20261027-022408-457-78584` 退出 1：RAW dimensions 实际 `7,2,2,12`、期望 `84,24,2,12`；尾读实际全零、期望 `1601,4,0,0`；Structured 对照正确。仅换入修复补丁后，最终门禁 `20261027-022824-128-78336` 的 Dev/Release `all`、两配置 `rhi_acceptance` 与 `renderer_acceptance_material_programs` 全部通过。八组 RAW/Structured 读回全部匹配；RHI 主进程和材质入口均实际启用 GBV，原 null-list/timeout 后端回归仍为 debug layer，Mesh PS 回归为 GBV。RHI 最终 warning/error 均为 0；既有故意超时、非法输入和 live-object 阳性对照诊断不被删去。

本机为 Windows x64、AMD Radeon RX 6800 XT、驱动 32.0.21045.16010。材质入口每配置检查两个真实程序、三个资源环境、三个 UV 组成的 18 条记录，经过原值、仅换程序索引、仅改参数、失败候选后保持旧内容四阶段，输出 72 行 CPU/GPU/独立期望；只证明 CPU/compute LOD0，不代表光栅、阴影、RT/DDGI 或完整材质系统已迁移，也不是性能采样。

原始证据在渲染工作树 `.amp/in/artifacts/b0-gpu-20261027/`：`final-red-gate/`、`final-red-rhi.log`、`final-green-gate/`、`final-green-{dev,release}-rhi.log`、`final-green-{dev,release}-material/outputs.csv`、源码与制品散列，以及实际生成 gfx.cpp 的 `final-red-to-green-gfx.diff`。验收源码红绿 SHA256 相同：`99a33f94aa70c62d07a737b9f058b0648cc57ce150c05d697714d0c00fd26650`。旧完整补丁另存 `.amp/in/artifacts/b0-raw-srv-before.patch`；非长期兼容实现。

此前两类失效尝试也保留：一次复制补丁保留旧时间戳，Ninja 未重配置、实际仍是旧 gfx，不能算绿轮；一次初版夹具使用非法 offset=12/44，触发 RAW 起点对齐错误及设备移除，不能归因于待验证修复。之后修正夹具、显式按 preset 配置并核对应用补丁 stamp，再重新执行上述同源码红绿；没有降低 GBV 或放宽断言。

## 绘制前窗口同步

[资源与窗口消费者](../../../acceptance/rhi/resource_cases.cpp)创建并管理自己的不激活窗口，依次切换 641×73、257×181、703×97、320×96、641×73、257×181。

- 隐藏时调用 `sync_window_target()`，返回新尺寸和 `drawable=false`；显示后只调用一次 `frame`，`rendered` 与 `next` 均为新尺寸。
- 逐像素核对全屏三角形，确认首次呈现没有旧 viewport 留边；auto-resize 纹理同步变化，前次 GPU 使用沿既有退休机制结束。
- 同步不改变 recording frame sequence 或 last Present 结果。真实 fence 阻塞队列时，同尺寸、隐藏同尺寸、零客户区和最小化调用仍可返回，证明没有额外排空。
- 无窗口、异线程、关闭后的重复调用符合合同。尺寸变化遇到阻塞队列时触发有限 timeout；重复调用保留终态，放行 GPU 后可安全清理并重复 close。

调用时机与生命周期见[窗口合同](contract.md#窗口与读回)。此处验证 RHI 在首个 Present 前同步尺寸的能力；菜单打开耗时及 execute 回调保护以应用接线验收为准。

## 日志与诊断解释

最近一次配置与完整输出保存在 `out/rhi-window-target-sync/`：`configure-dev.log`、`configure-release.log`、`dev.log`、`release.log`。最终补丁从 pristine 源码重放后的四个 gfx 文件与两种已验收配置的源码 hash 一致。

正式运行器每次更新 `out/<preset>/rhi-acceptance.log` 和 `rhi-regression-{null-list,timeout}.log`；旧 `rhi-regression-mesh-pixel-state.log` 不再更新，不能用于证明当前候选通过。许可证与补丁部署于 `out/<preset>/licenses/rhi/`。`out/` 受 Git 忽略，本机证据不随克隆交付；这些路径不能作为其他工作树已通过的证明。

故意阻塞的 GPU 等待与失败销毁仍会打印预期错误，结果应结合场景检查、退出码与 InfoQueue 判断；坏 shader 与失败后录制矩阵仅在旧日志中出现。null-list 回归刻意将一个 Program/Kernel 留给 Context 清理，会产生上游句柄未显式释放诊断；正式消费者没有该诊断。

## 重要变更的验证来源

| 日期 / 增量 | 当时验证与证据 |
| --- | --- |
| 2026-10-10 gfx 升级 | `17996ce` 上由 `e039b255` 升至 `a8d9bd64`；迁移创建标志与既有补丁，并修复嵌套 SDK 下载目录失效。Dev/Release 的 RHI、基础 Renderer、Scene Lit 均通过。配置、构建、运行日志与图像在 `out/gfx-upgrade-a8d9bd64/`；运行库版本未变 |
| 2026-10-08 Scene Lit 接入 | `402fab0` 基线上新增 RayQuery、AS 屏障、FP16/D32/mip、间接派发和原始 timestamp；Dev/Release 真实 GPU 验收通过。完整输出为 `out/scene-lit-{dev,release}.log` |
| 2026-10-05 根参数预算 | `495bf51` 上真实 Renderer 复现超过 64 DWORD；修正后 Dev/Release RHI 和 Dev Renderer 通过。前后日志为 `out/dev/root-budget-{renderer,rhi}-{before,after}.log` 及 `out/<preset>/root-budget-rhi-after.log`。后续双配置 Renderer 证据见上方 gfx 升级记录 |

### DDGIFilter 展开循环的驱动故障

2026-10-10 的独立单管线探针中，DXC 成功编译失败 shader；关闭 validation 后，`Context::prepare` 仍在 `D3D12Core::CreateComputePipelineState → amdxc64.dll` 内触发 `0xC0000005`，尚未录制 draw/dispatch 或创建 TLAS。仅将 5×5 双层循环的 `[unroll]` 改为 `[loop]`，同一探针即可 prepare/close 并退出 0。

该故障在升级后的 gfx 上仍能复现；驱动访问冲突没有返回可转换成 `Result` 的 HRESULT。证据在 `out/ddgi-unroll-probe-20261010/`，包括 `dxc.log`、`cdb-disabled.log`、`prepare-loop-disabled.log` 和 `manifest.json`。这是一份 shader 快照的单管线诊断，未裁剪到最小指令集，也不代表当前 Renderer 仍使用该 shader。

## 审计修正的生命周期观察

2026-09-27 的九项缺陷已修正：初始化数组、内部 Buffer 引用、DXGI 拥有引用、常量池复用、shader 输入边界、Mesh 选卡、range 偏移、参数数组释放和公开 C++23 要求。正式矩阵持续覆盖其中的可观察行为；以下是当时额外执行、在 Dev/Release 各通过的观察，不标作本次重测：

| 观察 | 当时结果 |
| --- | --- |
| 常量池 | 各三轮，每轮 64 次 dispatch/finish；完成后游标为 0，容量保持 65,536 字节 |
| 参数数组 | 反复切换 2 个/1 个 buffer 参数，Program/Context 退出后跟踪数组为 0 |
| DXGI 引用 | 三轮正常退出及初始化失败退出，释放观察者最后一份引用后均为 0 |
| 部分初始化 | 第二个 CreateCommandAllocator 使用真实无效 list type；创建失败并完成清理 |
| 公开编译要求 | 独立 CMake 消费者仅链接 `atelier_rhi`，继承 C++23 后成功编译公开头文件 |

当时的观察副本和日志位于 `out/review/rhi-fixes/`，汇总为 `out/rhi-verification.json`。它们没有增加生产测试钩子，也不等于全部原生对象或 OOM 故障普查。修正的维护理由见[模块决策](decisions.md#补丁与依赖维护)。更早的原生 SDK clear/copy 探针仅验证链接与硬件可行性，已由正式 gfx 验收接替。

## 未覆盖范围

- 真实 device removal、Signal/事件/Reset 驱动故障、native OOM、完整原生对象泄漏普查；DRED 保留上游实现，但未进行真实设备移除验收。
- 多卡选择、不支持 Mesh 的硬件、其他 GPU/驱动、WARP、HDR 显示，以及同一执行域承载多个窗口。
- 并行录制、多 queue、生产原生资源导入、设备恢复、通用 RDG。
- DispatchRays、透明/程序化光追几何、AS compaction；现有验证仅覆盖不透明三角形的 compute inline RayQuery。
- Runtime Texture 与业务语言/UI 的资源贯通链，以及稳定的产品性能基准。启用 validation 的 GPU 耗时只用于功能验证。

窗口和 Renderer 的其他产品能力见[Renderer 验收](../renderer/acceptance.md)，业务资源剩余缺口见[资源连接提案](../resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)。
