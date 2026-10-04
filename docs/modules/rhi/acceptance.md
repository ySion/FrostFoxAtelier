# RHI 验收与证据

本页记录 2026-09-27 的 gfx 正式 Interface 与九项审计修正验收，源码随后进入交付提交 `a1eb44b7f3e6f1363fb60e5e6357d32f3908fec7`。dev/release 已通过真实 GPU 执行；下文“本轮”均指该次运行，文档清理不构成新的运行结果。此前 SDK 探针只属于历史可行性证据。

2026-09-28 更新：编辑器 renderer 新增单窗口、显式 RGBA8、backbuffer 与独立深度同时绑定、带行距读回和 Present 状态。本页原 RHI 矩阵已在 dev/release 重新通过；新增窗口验收、641/703 宽度和未覆盖范围见 [renderer 验收](../renderer/acceptance.md)。以下保留 2026-09-27 的证据边界，不将其旧的窗口“未覆盖”列表当作当前全部能力。

## 2026-10-05：跨阶段根参数预算

固定候选 `495bf513c4f3cb7e7a2a0ab9506559e34ee3f3c7` 的 Dev `renderer_acceptance` 实际复现：纯 UI 先通过，随后 `editor surface` 在 `CreateRootSignature` 因超过 64 DWORD 失败。原 gfx 只检查单个 `$Globals` 是否超过 256 字节，未计算跨阶段常量和描述符表的总成本。修正进入同一 `atelier.patch`，复用原有反射变量填充、CBV 绑定、常量池与退休机制；Renderer/shader 无需参数旁路。

正式离屏消费者增加两个 GPU 回归，均在旧后端会超过预算：VS/PS 各 32 DWORD 加一个 SRV 表；单个 CS 恰为 64 DWORD 加一个 UAV 表。前者还让同名字段在 VS/PS 位于不同偏移。每项连续更新四次并核对实际像素或整数读回，覆盖混合根常量/CBV 绑定与完成后常量池复用。跨阶段场景已在修复前观察到同一 D3D12 超限错误。

修正后 dev/release `rhi_acceptance` 均通过，含新增回归及既有间接常量、Mesh PS、range 复制、null-list、timeout 回归；真实 RX 6800 XT，主消费者启用 D3D12 debug/GBV。Dev `renderer_acceptance` 也完整通过，含 UI-only、矩阵/mesh、UI 图片与 viewport MatCap/线框/选择/退休。Release Renderer 与编辑器视口组合验收由渲染负责人在最终集成树执行，此处不记为已通过。

本机证据保留在 `out/dev/root-budget-renderer-before.log`、`out/dev/root-budget-rhi-before.log`、`out/dev/root-budget-renderer-after.log` 及 `out/{dev,release}/root-budget-rhi-after.log`；常规 `rhi-acceptance.log` 和三个 `rhi-regression-*.log` 同时保留详细结果。以下历史证据范围不变。

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
