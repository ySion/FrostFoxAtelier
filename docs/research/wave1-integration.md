# Wave 1 三线集成验收

日期：2026-09-27。历史状态：运行时、RHI 与自研布局首增量在下述基点完成集成，正式 dev/release 配置及总 `acceptance` 均退出 0。本页保留当轮输入和发现，不代表后续提交的当前缺陷清单。

本记录证明三个模块在共同构建入口下的共存与各自正式行为。业务状态到布局再到 GPU 的跨模块消费链尚未实现，不计入本次结果。

## 提交来源与集成范围

| 方向 | 输入提交 | 内容 |
| --- | --- | --- |
| 运行时 | `491c01134624eb1bc5c27ab1a6432f73f534b800`，包含 `942f3ec` | 共享函数签名、原生/源码结果契约及当前 Runtime/JIT 能力 |
| RHI | `a1eb44b7f3e6f1363fb60e5e6357d32f3908fec7` | gfx 主体、Context/Pipeline、九项修正、正式离屏验收 |
| 布局 | `610bfd9db2e7629e808526098426dbc86d71840a`，包含 `cba27da` | 自研布局生产首增量、正式消费者及研究来源 |

候选分支依次产生 RHI 合并 `5d48234`、布局合并 `98a919d`，保留各线原始提交。冲突仅涉及根 `CMakeLists.txt` 和 README：

- 保留两种布局消费者、独立 `ui_layout_acceptance`、RHI 主矩阵与三项后端回归。
- 总 `acceptance` 先运行 RHI 依赖，再执行 Runtime、JIT 业务及布局消费者；任一失败返回非零。
- 保留各线公开目标、依赖隔离、运行库部署和现行文档入口，没有改写模块生产实现。

随后提交 `6d185fedd5d729b9785a9fa3b509a1e2e0842cbc` 修正补丁签出换行；**两种配置实际验收的代码版本均为该提交**。本记录及 README 的验收链接是在运行完成后补充的文档。

## 集成发现：Windows 签出的补丁无法重放

首次 dev 配置在正式 `atelier_patch_gfx` 路径失败。锁定归档的 gfx 源码与 RHI 原始源哈希一致；`core.autocrlf=true` 将签出的补丁转换成 CRLF，导致上下文匹配失败。

最小复现使用同一份真实原始 gfx 源码：

| 输入 | `git apply --check` 退出码 |
| --- | --- |
| 签出后含 1,019 个 CRLF 的补丁 | 1 |
| 只恢复 LF 的同一补丁 | 0 |
| 新增属性规则后，通过 `git checkout-index` 新签出的补丁 | 0 |

修正位于根 [.gitattributes](../../.gitattributes)：`*.patch text eol=lf`。没有更改全局 Git 设置、补丁内容或下载源码。修正后的补丁 SHA-256 与 RHI 原验收一致：`bd149361d08d91943e1797e692eaa48e2df9a6d3f8792bbb91de20e5d6aaf76d`。随后两配置的正式配置均成功，生成的四个 gfx 补丁源文件哈希也逐项一致。

## 本轮实际验收

在 `E:\FrostFoxAtelier` 使用根 README 的正式命令，按配置串行运行：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target acceptance
cmake --preset release
cmake --build --preset release --target acceptance
```

| 正式场景 | dev | release |
| --- | --- | --- |
| RHI 主消费者 | PASS | PASS |
| RHI null-list、Mesh PS 状态、真实超时回归 | PASS | PASS |
| Runtime | PASS | PASS |
| Business execution | PASS | PASS |
| JIT | PASS | PASS |
| 布局最小消费者 | PASS | PASS |
| 布局真实文字与生产合同 | 17 PASS / 0 FAIL | 17 PASS / 0 FAIL |
| 总入口退出码 | 0 | 0 |

一次独立只读审查核对了共享目标、验收失败传播、CRT/宏与输出隔离、当前验收的运行库部署，以及冲突解决是否保留原行为，未发现额外接入阻塞。模块内部历史审计未重复执行。

本机证据保留在 `out/review/wave1-integration/`：

- `out/review/wave1-integration/verification.json`：实际验收提交、51 个源码/构建文件哈希、日志哈希、双配置结果及生成 gfx 身份。
- `out/review/wave1-integration/dev-acceptance.log`、`out/review/wave1-integration/release-acceptance.log`，以及对应的 configure 日志。
- `out/review/wave1-integration/patch-reproduction.json`、初次配置失败日志和新签出补丁副本。
- 各配置 `out/<preset>/rhi-acceptance.log` 与三个 `rhi-regression-*.log` 保留独立 GPU 场景输出。

这些 `out` 文件不提交；缺失时按正式入口重新运行，不把文档中的历史 PASS 当成本机已有结果。

## 当轮问题与能力边界

- 当轮仍存在防御性 ABI 回调结果违约的空诊断 P2；之后已在 `2b2d348` 修复，并经正式 dev/release 端到端验收关闭，见[缺陷关闭证据](runtime-stage-closeout.md#已关闭的历史缺陷)。不得将此历史问题继续列为当前阻塞。
- RHI 结果仍限于已测硬件和现行离屏路径；Present、多 queue、设备恢复和完整 renderer 等不由本次合流补齐。
- 布局输入变化后仍全树计算；共享列、换行/重复格、局部调度、虚拟化、绘制与业务绑定仍属于后续增量。
- 本轮没有性能采样或性能收益结论。RHI 与布局的独立开发分支保持原提交；运行时所在的 `main` 已快进到集成基线。
