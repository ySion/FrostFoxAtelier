# Runtime 与编辑器冻结版本集成（2026-10-02）

## 输入与合并范围

- 编辑器基线：`2e36b47f35e8c891dc986c5da502e00c4b115fdc`，即 PR #6 合入后的 main；其 tree 与编辑器交付 `84136b3` 一致。
- Runtime 交付：`e009a5198ec62d05f212e88eaf5be67ec050777a`。保留其原始历史，包括同模块多函数与内部调用、源码 enum/struct 的类型和代码联合发布、String/注册 Array/无 GC 引用记录的字段组合及作用域清理。
- 唯一冲突为 README 的编辑器与历史输入法说明。逐段保留已完成的菜单、单宿主停靠、字体及 Runtime 能力导航；尚未实现的编辑器能力仍是浮动视图与布局文件保存等。没有整文件取任一侧。
- 未改动任一侧生产代码；Runtime、Source、JIT、Language 及其验收代码与冻结 `e009a51` 相同，应用、UI、World、RHI、renderer、foundation 源码与编辑器基线相同。
- World `6503bee` 的 Scene/Mesh 增量未纳入本轮，继续保持独立。

实际受测合并树为 `5b67451950180c7708532a5df04d08e8ac735641`。验收后仅新增本记录和 README 链接，生产代码和验收代码没有再变更。

## 同候选重建与验收

所有目标都在应用工作树 `C:/Users/Arzuo/.codex/worktrees/2a7e/FrostFoxAtelier` 构建和运行，使用自己的 `out/dev`、`out/release`；没有借用其他工作树的库或可执行文件。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target atelier_editor world_acceptance editor_document_acceptance ui_acceptance
cmake --build --preset release --target atelier_editor world_acceptance editor_document_acceptance ui_acceptance
```

两配置均按依赖重建 Runtime、Source/JIT 后端、World、反射生成代码及编辑器消费者，并重新链接 `atelier_editor`。共享 UI 源码未改变，沿现有 UI 消费者执行验收。

| 本次集成验收 | dev | release |
| --- | --- | --- |
| `atelier_editor` 编译/链接 | 通过 | 通过 |
| `world_acceptance` | 4/4 通过 | 4/4 通过 |
| `editor_document_acceptance` | 通过 | 通过 |
| `ui_acceptance` | 15/15 通过 | 15/15 通过 |

World 消费者覆盖真实 JIT 更新同一派生组件、依赖与 TRS、任务取消、对象身份撤销、GC 生命周期和失败边界。编辑器文档消费者覆盖名称/字段历史、成员关系、退役与关闭。这些场景覆盖本次类型注册/发布和源码执行链与现有消费者的结合点。

新日志为 `out/runtime-editor-integration-20261002/dev.log` 和 `release.log`，产物与日志校验值见同目录 `manifest.json`。Runtime 冻结版本的 Source、Business/JIT、Reflection/CLI 完整验收复用交付证据；字段组合的原验收入口与证据说明见 [源码类型声明](../source-type-declarations.md#值组合增量)。

## 窗口验收边界

本候选没有启动普通窗口或搜狗验收。UI 输入、窗口、绘制与 World 对象布局代码均未变化；变更涉及的原生注册、文档历史、World 生命周期和真实 JIT 消费者已在同一候选下完成双配置验收，未发现需要补充前台操作的行为变化或失败。

编辑器 `84136b3` 的 2870 帧仅属于 [上一版本窗口证据](editor-ui-20261002-closeout.md)，不作为本次新二进制的窗口结果。原日志及截图保持不变；重建前将旧 `atelier_editor.exe` 和 `editor.ss` 保存在 `out/editor-ui-closeout-20261002/binaries/{dev,release}/`，旧可执行文件 SHA-256 与冻结时相同。

本轮不扩展新功能。单宿主停靠、未实现浮动编辑器视图/布局持久化等边界沿用原交付。
