# 2026-10-04 阶段同步

状态：可发布候选已完成本轮后台验收。本记录区分已完成的代码、方案归档与尚待窗口验证的渲染工作；主线发布和七条负责人工作树的同步由主持统一执行。

## 集成范围

可发布候选分支为 `codex/stage-sync-20261004`，从固定主线 `a57cb0ca5cd15aef61e79cef42513105b1c9bd3e` 开始，正常合并各交付提交，保留来源历史。

| 来源 | 固定提交 | 本轮状态 |
| --- | --- | --- |
| 主线基础 | `a57cb0ca5cd15aef61e79cef42513105b1c9bd3e` | 已包含上一轮数学／Project 集成、Foundation Map／Set 及容器生命周期修正 |
| Runtime | `cb76eadc8836970d46ba31cf325740476e70a0b5` | 正常合入；保留 bool／控制流 `8734288` 和 plain record 按值调用 `80ea055` 的历史 |
| STL 研究 | `a6355df99cd250709d393415128f50ad3ba8862f` | 单文件归档，来源内容哈希已核对 |
| UI 方案 | `9f57bf9` | 单文件归档后正常合入；没有新增 UI 生产实现 |

集成代码基线为 `a07df9d91f49b2ccf867f29b0b1e2645fec9dc5f`。它相对 Runtime 的固定验收树仅增加两份文档；后续收口只修改文档。Runtime 的类型身份、借用、值生命周期和调用结果发布合同沿现有机制，当前能力以 [LANGUAGE](../../LANGUAGE.md)、[Runtime 模型](../runtime-model.md)和[原生值借用](../native-value-borrows.md)为准。

## 验收证据

所有补验在独立集成树 `C:/Users/Arzuo/.codex/worktrees/math-project-sync/FrostFoxAtelier` 进行，没有操作用户正在使用的窗口，也没有修改 `E:/ffa_testproj`。

| 检查 | 代码版本与位置 | Dev | Release |
| --- | --- | --- | --- |
| Foundation、Runtime、Source、JIT、reflection 消费者及 CLI | Runtime 负责人树 `cb76ead`；与候选生产代码相同，复用已完成证据 | PASS | PASS |
| 正式 `atelier_editor` 编译 | 集成树 `a07df9d` | PASS | PASS |
| `world_acceptance` | 集成树 `a07df9d` | PASS（8／8） | PASS（8／8） |
| `editor_document_acceptance`，含 Project 文件、Workspace 三种视图及 1／1.5／2 DPI | 集成树 `a07df9d` | PASS | PASS |

本轮集成树执行入口：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --parallel 8 --target atelier_editor editor_document_acceptance world_acceptance
cmake --preset release
cmake --build --preset release --parallel 8 --target atelier_editor editor_document_acceptance world_acceptance
```

两种配置的上述命令均以 0 退出。集成日志保存在 `out/stage-sync-20261004/{dev,release}-{configure,build}.log`。复用的 Runtime 日志保存在负责人树的 `out/runtime-stage-sync-{dev,release}.log`。代码相同的已通过验收没有再次整轮执行。

## 文档与旧描述清理

- [声明式 UI 方案](../modules/editor/declarative-ui.md)按已交付的 bool、控制流、plain record 按值调用和容器能力更新，明确类 UXML 标记、现有 SS、自有语言与同一 Runtime 的关系。第一片仍建议从真实 Position 参数区开始，尚未派发实施。
- [基础容器研究](compact-stl-direction.md)将已交付能力与后续方向分开，容器合同继续以现有 Foundation 文档为准。归档文件的规范化 Git blob 为 `09e32d0383099c30f243034f8355b7520c6bd86c`；源文件与归档的 SHA256 均为 `6BB22A1BC55E29EF3C5C71158DC4F9BFD0E3D9819BA7A212FBAA058816554147`。
- `LANGUAGE.md` 的内部同步调用与原生成员方法两处旧枚举已补上合格 plain record，引用原有参数及临时值规则，没有改变语言合同。
- 原应用工作树的 UI 方案已形成正常提交；没有隐藏、重置或删除其他负责人的未提交工作。

## Renderer 独立待验范围

Renderer 固定交付为 `58e4bf51abd5afb2aca928d096c1a2169ebe525c`，实际后台验证代码为 `f70d4e5e5f1059764a6ea35c6f2c1f3205a97d36`，两者只差验收记录。该交付包含 MatCap、Flat／Smooth、线框显示、Scene View 显示选项、三角形边 mask 和 Houdini 研究归档。

负责人树的 Dev／Release 正式 editor 与 renderer 验收程序编译、World 8／8、Project／Workspace 文档链已通过；日志位于其 `out/viewport-integration/stage-sync-{dev,release}.log`。这些证据不等同于与新 Runtime 合并后的 GPU／窗口结果。

该交付将正常合入独立分支 `codex/stage-renderer-pending-20261004`，不进入本轮可发布候选。获准使用桌面后，须在固定集成代码上依次运行 Dev／Release 的 `renderer_acceptance` 与 `editor_viewport_acceptance`。执行命令与画面证据以该分支的专项记录为准。

## 保留的边界

- 本轮未运行 GPU／窗口验收；既有数学值进入实际 GPU 帧的证明没有因后台通过而自动补齐。
- 普通 Project 窗口确认链及间歇性 Unicode 剪贴板问题保留原状态，不以专项或无窗口验收代替。
- 自有语言仍为每个 Program 一个选定公开入口；多具名导出、模块间调用和普通值记录自定义方法未随 UI 方案归档而实现。Map／Set 仍为原生 C++ 能力。
- 没有更换 JSON 库，没有迁移 Scene 格式，没有改动用户测试工程。
