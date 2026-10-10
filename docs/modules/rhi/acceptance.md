# RHI 验收与证据

本页记录 2026-09-27 的 gfx 正式 Interface 与九项审计修正验收，源码随后进入交付提交 `a1eb44b7f3e6f1363fb60e5e6357d32f3908fec7`。dev/release 已通过真实 GPU 执行；下文“本轮”均指该次运行，文档清理不构成新的运行结果。此前 SDK 探针只属于历史可行性证据。

2026-09-28 更新：编辑器 renderer 新增单窗口、显式 RGBA8、backbuffer 与独立深度同时绑定、带行距读回和 Present 状态。本页原 RHI 矩阵已在 dev/release 重新通过；新增窗口验收、641/703 宽度和未覆盖范围见 [renderer 验收](../renderer/acceptance.md)。以下保留 2026-09-27 的证据边界，不将其旧的窗口“未覆盖”列表当作当前全部能力。

## 2026-10-10：升级固定 gfx

在引擎 `17996cee07bcb234860848e31d6717fac3dec335` 上，将 gfx 从 `e039b255` 更新至当日核实的上游 master `a8d9bd64ea7d95cfc015a047845bf709ac9f6582`（10 月 7 日提交）。[上游差异](https://github.com/gboisse/gfx/compare/e039b255efb327994d30b187caee400ca60110e0...a8d9bd64ea7d95cfc015a047845bf709ac9f6582)包含 9 个提交，涉及 GPU validation 开关、DRED 日志与日志严重度修正；DXC、Agility、D3D12MA 和 PIX 的锁定版本未变。

迁移处理两处补丁冲突：保留 shader 编译失败的显式错误返回；上游扩展创建标志后，将项目 `ForceRGBA8SwapChain` 移至空闲 bit 9，并把 `Validation::gpu` 接到上游明确标志。所有既有 owner、有限等待/终态、根签名预算、AS 屏障和原始 timestamp/录制序号补丁保留，接口未另建入口。

首次在原构建目录升级复现了嵌套 SDK 文件被父 gfx 解压替换、FetchContent 旧记录却仍有效的问题。现改为按 gfx 修订号隔离源码根，并由锁定路径更新 D3D12MA 的 Agility cache 设置；运行库部署查询真实 gfx target 源码目录。现有 dev/release 构建目录均成功完成升级配置和编译，四份已打补丁的 gfx 源文件在两种配置中 hash 一致。

| RX 6800 XT，D3D12 debug/GBV | Dev | Release |
| --- | --- | --- |
| `rhi_acceptance`：完整 GPU 主场景及三个后端回归 | PASS | PASS |
| `atelier_renderer_acceptance`：材质、UI、基础场景、窗口/深度和既有模式 | PASS | PASS |
| `atelier_renderer_acceptance --scene-lit`：三类灯/阴影、HDR、DDGI、GTAO、SSSR、时域重置及资源寿命 | PASS | PASS |

以上进程均退出 0；GPU 验收没有 D3D12 debug/GBV warning/error。未重新执行编辑器输入自动化，也未注入真实设备移除验证 DRED。证据位于 `out/gfx-upgrade-a8d9bd64/`：`configure-{dev,release}.log`、`build-rhi-{dev,release}.log`、`renderer-{dev,release}.log`、`scene-lit-{dev,release}.log` 及对应 Scene Lit 图像目录。RHI 日志中的故意无效 HLSL、timeout 和 null-list 清理输出沿既有验收预期。

### 同日独立诊断：DDGIFilter 展开循环访问冲突

Renderer 提供的失败 shader 在独立无窗口进程中复现，调用链只有 `Context::open → prepare(DDGIFilter)`，未录制 draw/dispatch，也未创建 TLAS。DXC CLI 使用相同 SM 6.5、entry 和编译选项成功生成 DXIL；`Validation::disabled` 下仍在 prepare 阶段以 `0xC0000005` 退出。CDB 栈确认异常地址位于 `amdxc64.dll`，下层调用链经过 `D3D12Core::CreateComputePipelineState → gfx → Context::prepare`。驱动没有返回可转换成 `Result` 的失败 HRESULT；不能将原生访问冲突当作普通 shader 编译失败吞掉并继续使用执行域。

只把该 5×5 循环的两个 `[unroll]` 改成 `[loop]`，同一个探针即可完成 prepare/close 并退出 0。该现象在本次新 gfx 上仍可复现，不将 gfx 升级记为驱动故障修复。此处仅完成单管线复现，尚未把 HLSL 裁剪到最小指令集；Renderer 已采用正常循环，本轮未修改其工作树。

输入快照、独立 probe、DXC 结果、CDB 栈、loop 对照与文件指纹保存在 `out/ddgi-unroll-probe-20261010/`，关键文件为 `dxc.log`、`cdb-disabled.log`、`prepare-loop-disabled.log` 和 `manifest.json`。这些是本地诊断产物，不成为第二套生产编译入口。

## 2026-10-05：跨阶段根参数预算

固定候选 `495bf513c4f3cb7e7a2a0ab9506559e34ee3f3c7` 的 Dev `renderer_acceptance` 实际复现：纯 UI 先通过，随后 `editor surface` 在 `CreateRootSignature` 因超过 64 DWORD 失败。原 gfx 只检查单个 `$Globals` 是否超过 256 字节，未计算跨阶段常量和描述符表的总成本。修正进入同一 `atelier.patch`，复用原有反射变量填充、CBV 绑定、常量池与退休机制；Renderer/shader 无需参数旁路。

正式离屏消费者增加两个 GPU 回归，均在旧后端会超过预算：VS/PS 各 32 DWORD 加一个 SRV 表；单个 CS 恰为 64 DWORD 加一个 UAV 表。前者还让同名字段在 VS/PS 位于不同偏移。每项连续更新四次并核对实际像素或整数读回，覆盖混合根常量/CBV 绑定与完成后常量池复用。跨阶段场景已在修复前观察到同一 D3D12 超限错误。

修正后 dev/release `rhi_acceptance` 均通过，含新增回归及既有间接常量、Mesh PS、range 复制、null-list、timeout 回归；真实 RX 6800 XT，主消费者启用 D3D12 debug/GBV。Dev `renderer_acceptance` 也完整通过，含 UI-only、矩阵/mesh、UI 图片与 viewport MatCap/线框/选择/退休。Release Renderer 与编辑器视口组合验收由渲染负责人在最终集成树执行，此处不记为已通过。

本机证据保留在 `out/dev/root-budget-renderer-before.log`、`out/dev/root-budget-rhi-before.log`、`out/dev/root-budget-renderer-after.log` 及 `out/{dev,release}/root-budget-rhi-after.log`；常规 `rhi-acceptance.log` 和三个 `rhi-regression-*.log` 同时保留详细结果。以下历史证据范围不变。

## 2026-10-08：Scene Lit 所需 RHI 能力

以 `402fab075bb2aa6605b51fdb767387ebd0e08869` 为基线，本次增量在 RX 6800 XT、驱动 `32.0.21045.5002` 上通过 dev/release `rhi_acceptance`，两者退出 0。新增场景全部为无窗口、真实 GPU、D3D12 debug layer + GBV；没有输入自动化或前台窗口验收。

- 公开 `ContextInfo::inline_ray_query`，由固定 gfx 的 DXR 1.1 门槛及设备 SM 6.5 查询决定；本机为 true，实际 SM 6.5 compute RayQuery 编译与执行成功。
- [RayQuery 场景](../../../acceptance/rhi/ray_query_cases.cpp)：同一命令列表内 BLAS/TLAS build → trace → BLAS 顶点更新及 TLAS 变换更新 → trace，只在末尾 `finish`。两份读回各含五条射线，检查命中/未命中、instance mask、InstanceID、primitive、重心坐标和最近距离；二值 first-hit 遮挡单独查询。TLAS、instance、BLAS、Geometry、原始 mesh buffers、输出 buffer 和 Pipeline 都在提交前释放，结果仍匹配。Geometry 接管独立 range 句柄，原始 mesh 句柄保留至各自 owner 清理。
- [离屏资源场景](../../../acceptance/rhi/scene_lit_cases.cpp)：64×32 双 FP16 MRT，检查每个像素的 half 位型及后续 compute 采样；颜色含大于 1 的值与负法线分量，D32 深度 SRV 值为 0.25。mip 2 顺序 UAV 写/SRV 读的 128 个值、GPU 生成间接派发的 64 个输出均匹配。三个帧槽回收后得到有限正 timestamp duration。
- 原始 timestamp：8 个 query 连续复用 6 个提交帧，共发布 48 个原始样本。首次回收前 sequence 为 0，录制新计时及重复 getter 均不改变已缓存样本；回收后各 pass 的 sequence 一致且逐帧递增。检查原始值有限非负、每帧存在正耗时，并核对旧 getter 的首次平滑结果仍约为原始值的 5%。排空后的空闲帧不伪造新样本。计时验收中只使用正常 `frame` 回收，无逐帧 `finish` 或原生等待。
- 录制标识与 GPU 读回：在此前已有帧推进的同一个 Context 内，从 `gfxGetRecordingFrameSequence` 取得标识，验证额外一次诊断 `finish` 不推进它。后续六个计时帧分别将该标识传入真实 compute，编码结果读回；每帧回收时核对 sample sequence 与录制标识完全一致，并核对对应读回的 64 个 GPU 值。由此验证 timestamp 和普通 GPU 结果的关联，不依赖 Renderer 估算 `frame` 次数。
- gfx 补丁补齐 BLAS/TLAS build/update 前后的 AS UAV 屏障与失败执行域录制门禁。AS 屏障缺口由源码及 DXR 同步契约确认；修复前后的小场景均可通过，未声称复现了硬件可见错误。D32 SRV 实测原路径可用，没有增加格式转换补丁。
- 既有 root-budget、间接常量、Mesh/Vertex、range copy、owner/close、null-list、Mesh PS 和 timeout 场景也全部通过。D3D12 InfoQueue 的 warning/error 均为 0。日志中的故意无效 HLSL、timeout 及 null-list 对象清理诊断沿既有回归预期，不代表新增场景失败。

命令：环境脚本后，依次 `cmake --preset dev` / `cmake --build --preset dev --target rhi_acceptance`，以及对应 release 命令。日志保存在 `out/scene-lit-{dev,release}.log`；每个配置也有正式 `rhi-acceptance.log` 与三份 `rhi-regression-*.log`。最终补丁分别在两种配置中从 pristine gfx 重放；Dev 重放后源码 hash 与已验收源码一致。

范围：验证上述 RHI 调用链，不代表 Scene Lit 产品画质、1440p/60 性能、不同 GPU、透明/程序化几何、AS compaction 或 DispatchRays 已验收。整纹理状态跟踪禁止同一 dispatch 同时 SRV/UAV 访问同一 texture；性能采样使用新增原始 sample 接口，旧 duration getter 保留平滑行为，详见[当前契约](contract.md#scene-litinline-rayquery-与离屏资源)。

## 命令与环境

在 worktree 根目录执行 `. ./tools/enter-dev.ps1`，分别 `cmake --preset dev`、`cmake --preset release`，再运行：

```powershell
cmake --build --preset dev --target acceptance
cmake --build --preset release --target acceptance
```

以上是完整重现命令。2026-09-27 修正后，release 的完整 `acceptance` 退出 0；dev 先运行 `rhi_acceptance`，随后构建并分别运行 `atelier_runtime_acceptance`、`atelier_jit_acceptance`，各自退出 0，避免重复已经通过的 GPU 矩阵。两种配置的 RHI、Runtime、Business execution、JIT 均 PASS。执行器给主消费者 180 秒、每个后端回归 60 秒的进程上限；这不是 GPU 单次 wait budget。

| 环境 | 实测 |
| --- | --- |
| OS | Windows 11 Pro，10.0.26200，x64 |
| Adapter | AMD Radeon RX 6800 XT，vendor 0x1002 |
| Driver | 32.0.21045.5002 |
| gfx 帧槽 | 3 |
| 主消费者 / Mesh PS 回归 | D3D12 debug layer + GPU-based validation |
| null-list / timeout 回归 | D3D12 debug layer |
| 工具链 | 从 `cmake/toolchain-lock.json` 加载；构建日志保留实际版本 |
| 来源身份 | gfx 与运行库归档由 `cmake/dependencies-lock.json` 固定；补丁与当前源文件 SHA-256 保存在 `out/rhi-verification.json` |

## 两种配置的结果

以下结果在 dev 与 release 各自通过，检查不依赖 C++ assert：

| 场景 | 观察结果 |
| --- | --- |
| Compute → Mesh → Vertex | 6 帧真实执行；Compute 生成数据、Mesh 间接参数和 Vertex draw 参数；Mesh PS 先读 Compute 输出，随后 Vertex 绘制 |
| 完整颜色/深度回读 | 24,576 个颜色像素、12,288 个深度像素、90 个 GPU 参数字，mismatches=0 |
| 常量与绑定 | 同帧两次不同 seed/不同 buffer；超过 root-constant 阈值的 globals；跨帧变化均匹配 |
| 在飞生命周期 | 每帧已编码临时资源在提交前释放 CPU 句柄；跨帧回读仍正确；在飞旧管线被成功候选替换 |
| 准备失败 | 26 个错误候选拒绝；原 13 项加 11/12/128/8192 字节 model、非法后缀、空指针和显式空文件路径；新增项返回 invalid_argument，旧 Program/Kernel 身份不变 |
| 可选文件阶段 | GS/AS 不存在时 Vertex/Mesh 文件管线成功；存在但编译错误时拒绝 |
| owner 与 close | 异线程执行拒绝，borrow 不可用；Pipeline 存活时 close busy；重复 close 成功 |
| 共享执行域 | Context 外壳退出后 Pipeline 保持原生 PSO 有效；最后释放 Pipeline 后安全清理 |
| 创建输入 | 零宽度、零等待、INFINITE 等待、无效验证枚举，共 4 项在创建前拒绝 |
| null-list | 合法 interop device、不设置 command list、CPU 创建 pipeline，context 清理成功 |
| Mesh PS 状态 | 独立回归检查 2,048 像素；GPU validation 无资源状态错误 |
| 真超时 / 严格一次 | 原生 queue 等待独立 CPU gate fence；finish 与未放行 close 超时；新录制/frame/finish/execute/reset 拒绝；放行后 close 只 drain；计数器回读严格为 1 |
| D3D12 调试消息 | 主消费者及后端回归的 InfoQueue 检查无 warning/error |
| 间接大常量 | 冷常量池下单次/多次间接 compute，连续 64 次 finish，每次 GPU 值匹配；另验证 Vertex、Indexed Vertex、Mesh 间接绘制各 2,048 像素 |
| range 上传与回读 | 64×32 RGBA8；源 range 偏移 256，目标 range 偏移 512；8,192 图像字节匹配，512 字节前缀/尾部哨兵保持不变 |

主消费者只经正式 `Context` 与 gfx 借用许可录制、推进和关闭。专用后端回归的 native queue gate、外部 committed readback 和 interop device 是明确隔离的诊断场景，不是普通业务的逃生 Interface。

## 审计修正的生命周期观察

[审计发现](audit-2026-09-27.md)的九项修正已进入正式宿主/CMake 或可重放 gfx 补丁。以下观察另在 dev/release 各运行一次，均退出 0：

| 观察 | 两种配置的结果 |
| --- | --- |
| 连续 finish 的存储复用 | 各三轮、每轮 64 次真实大常量 GPU dispatch/回读；完成后游标均为 0，池容量保持 65,536 字节 |
| buffer 参数分配/释放 | 每轮反复切换 2 个/1 个绑定；实际 gfxFree 配平旧数组，Program/Context 退出后跟踪的参数数组为 0 |
| DXGI 拥有引用 | 三轮正常 close，释放观察者最后一份引用后均返回 0；初始化失败路径也为 0 |
| 部分初始化清理 | 隔离源码令第二个 CreateCommandAllocator 使用真实无效 list type，由 D3D12 原生拒绝；Context 返回 creation_failed，清理成功 |

观察副本由当次正式生成的 gfx.cpp 派生，只增加计数、只读游标/池大小观察及上述原生错误输入；编译/链接使用对应配置的真实命令。它们位于 `out/review/rhi-fixes/`，没有向生产 API 加入测试钩子，也没有伪造 HRESULT 或驱动返回值。证据覆盖列出的拥有引用与参数数组，不等于全部原生对象或完整 OOM 故障普查。

此外，独立 CMake object 消费者只 `target_link_libraries(... atelier_rhi)`，没有调用 `atelier_target` 或指定标准，实际以 C++23 编译公开头文件成功；临时配置入口已移除。Mesh 的成功硬件路径已运行，不支持 Mesh 的硬件和多卡选择仍为源码校验范围。

修正汇总、当前源文件 hash、补丁重放身份与日志：rhi-verification.json（`out/rhi-verification.json`）。观察脚本：run-lifetime-probe.ps1（`out/review/rhi-fixes/run-lifetime-probe.ps1`）；dev 日志（`out/review/rhi-fixes/rhi_lifetime_dev.log`）、release 日志（`out/review/rhi-fixes/rhi_lifetime_release.log`）、公开 target 消费者编译命令（`out/review/rhi-fixes/public-consumer-command.json`）。

## 日志与诊断解释

以下为验收时本机保存的证据路径，位于忽略的 `out/`，不随仓库克隆交付；重新执行正式入口会生成相应运行日志。清理没有删除这些证据，也不将临时观察脚本当作生产目标。

每个配置的 `out/<preset>/` 当时保留：

- `aggregate-acceptance.log`：release 当前聚合结果；dev 同名文件保留此前执行，当前 dev 的补充结果使用下述日志。
- `rhi-acceptance.log`：正式消费者，包含逐场景结果与 `exit=0`。
- `rhi-regression-null-list.log`、`rhi-regression-mesh-pixel-state.log`、`rhi-regression-timeout.log`：独立子进程结果与 `exit=0`。
- `licenses/rhi/`：gfx、DXC、Agility、D3D12MA、PIX、AMD 扩展 notice/source 与项目补丁。

汇总及源码 hash：rhi-verification.json（`out/rhi-verification.json`）。这些 `out` 文件是本机证据，不是已提交仓库内容。

本轮 dev 的 `out/review/rhi-fixes/dev-rhi.log`、`dev-core-build.log`、`dev-runtime.log`、`dev-jit.log` 记录分开执行的完整组件结果。

坏 shader、等待超时和失败后录制拒绝会按预期打印错误；不能用全日志出现单词 error 判定失败。null-list 场景刻意让一个原始 Program/Kernel 存活到 context 清理，上游打印两个句柄未显式释放的诊断；它不是正式消费者退出路径，也不构成完整原生 live-object census。主消费者没有这两项诊断。固定 D3D12MA 源码在当前 Clang 下有一项 `TYPE_COUNT` 未在 switch 显式列出的编译 warning；项目自身目标仍启用 warnings-as-errors，没有为此关闭项目检查。

## 未覆盖，不计作 PASS

真实 device removal、Signal/事件/Reset 的驱动故障注入、native OOM、fence/back-buffer 创建失败、多卡及不支持 Mesh 的硬件、设备恢复、所有原生对象计数归零、其他 GPU/驱动或 WARP、并发录制、多 queue、Present/resize/HDR/多窗口、ray tracing、RDG/renderer、业务语言绑定与性能基线均未获得本轮动态证明。没有修改 TDR、挂死 shader、伪造 fence 完成值或替换 driver 返回值。

旧 Main SDK 探针曾验证两轮 37×19 clear/copy/readback、row pitch=256 与 debug=0 errors（当时未开 GBV），属于历史路径证据；本次不重跑或改写该结果，也不宣称当前 gfx 消费者验证了任意非对齐 texture pitch。历史依据见 [research.md](research.md)。
