# 原生 UI 语言首个编辑器组件验收

首次 M1 验收日期：2026-10-05。基线：`855433a840584b414073758d6587f5ce6ae66703`，加当时未提交的 UI 语言与 Session 属性更新。两个正式编辑器当时均报告 `+ local changes`；以下保留原始证据，后续审查和正式组合验收见末节。

## 实际交付

- `position.ui.source` 定义真实 Parameters 中的层级、文字、折叠状态、属性绑定和事件。C++ 保留选中对象采样与原编辑事务路由，原 Position 手写构造已移除。
- 声明前端使用既有 Source Lexer，生成同模块函数，经原编译器检查并 JIT。组件类型共享 Program/Lease，实例保留局部状态、实际读取依赖和控件数据。
- `atelier_ui_language` 不依赖 editor、World 或 RHI。对象上下文使用文档原有 Runtime；帧不持有组件回调或 JIT 指针。
- 卸载顺序改为清理输入和旧控件、卸载组件及租约、关闭旧文档 Runtime。补齐无属性绑定组件的首次挂载，以及绑定上下文时的异常所有权处理。
- 合入布局负责人交付的 `Session::update_controls`、描述 revision、失败恢复和统计；`set_controls` 复用同一校验及清理实现。

当前语法、调用链和限制统一维护于[声明式 UI 文档](../modules/editor/declarative-ui.md)。

## 执行与证据

已在开发环境初始化后执行以下目标；目标本身会运行后台端到端消费者：

```powershell
cmake --build --preset dev --parallel 8 --target atelier_editor editor_document_acceptance ui_acceptance
cmake --build --preset release --parallel 8 --target atelier_editor editor_document_acceptance ui_acceptance
./out/dev/atelier_editor.exe --acceptance-project out/ui-language-20261005/dev-window-evidence E:/ffa_testproj
./out/release/atelier_editor.exe --acceptance-project out/ui-language-20261005/release-window-evidence E:/ffa_testproj
```

| 验收 | Dev | Release |
| --- | --- | --- |
| 正式编辑器编译 | 通过 | 通过 |
| UI Session、布局、DirectWrite 消费者 | 20/20 | 20/20 |
| 真实文档与声明式 Parameters 消费者 | 通过 | 通过 |
| 正式项目窗口、输入及 Renderer/RHI | 通过，退出码 0 | 通过，退出码 0 |

后台声明式消费者覆盖实际 World 值、编辑预览合并、撤销重做、选择换绑、旧目标修改、删除、Foldout 输入事件、实例状态、卸载幂等、无绑定节点、源码诊断和辅助函数中的动态依赖。稳定输入不重新求值；未读取分支变化不重新求值；多个实例共享编译组件且保有不同控件身份。

Session 的 33 控件场景测得：单项颜色变化更新 1 个控件，布局同步、布局节点访问、测量、栅格化增量均为 0；文字变化布局同步增量为 1。这个测量不代表整个编辑器只遍历变化节点。

真实窗口共用原正式验收驱动，保留前台焦点检查。覆盖视口显示/TRS、当前工程身份、Actor 创建、活动字段保存、资产视图/搜索/过滤、OBJ 导入、Scene Tree 选择、位置编辑/撤销/重做、保存重开、失败打开、保存后目录刷新失败、工程搬移及 Cache 可丢弃。两轮均使用项目副本；原 `E:/ffa_testproj` 的 Config/Content/Source 文件集合和 SHA-256 与验收前记录一致。

本地证据位于 `out/ui-language-20261005/`：

- `dev-build.log`、`release-build.log`：编译和全部后台 PASS 输出。
- `dev-window-evidence/run-47924-343085750/acceptance.txt`。
- `release-window-evidence/run-30624-343117671/acceptance.txt`。
- 各窗口目录中的 `project-*.png` 等真实截帧，已查看参数区布局与保存重开状态。
- `manifest.json`：本阶段源码、部署资产和正式二进制 SHA-256。

## 保留的边界

本阶段不是整套编辑器迁移完成。Workspace 仍全量 describe/set_controls；组件内部按变化节点发布，但尚未将整套编辑器改为直接提交稀疏批次。条件销毁、稳定 key 列表、组件组合、动态文字输入、其余面板和游戏 HUD、热重载仍待后续增量。

本轮未执行完整普通 UI 窗口矩阵或真实系统 IME 验收；不能据此宣称覆盖搜狗或解决此前间歇性 Unicode 剪贴板问题。渲染负责人的独立 Guides 改动未纳入本基线。

## 发布前审查与修正

上述报告保留首次 M1 验收时的版本和未提交事实。随后用户授权各功能负责人审查、清理、提交推送并统一集成。

- 运行时审查：修复 `for` 步进中的 state 赋值，按源语法括号深度关闭生成调用，保留嵌套函数调用；明确 UI 保留标识符。
- World 审查：Parameters 在读取对象前按本 World 成员筛选选择。外部 detach 后即使对象仍保活也退出参数投影，restore 后使用新字段 key。
- 需求审查：片段 Lexer/Tokens 诊断统一映射回原文件，验收精确检查错误行列。
- 规范审查：补全 `controls()` 借用的失效条件；稳定输入发布直接返回，避免重复复制快照。
- Foundation 审查：组件与 Session 复用 `validate_control_properties`，在发布前拒绝 NUL 文本、非法 class 和数值描述。参数行的已知输入异常转为 Result，旧快照保持可用，修正输入后恢复；分配异常继续传播。
- 布局审查：准备失败后的旧帧不能解除输入暂停，恢复必须接受失败之后的新候选。正式交付为 `08b2fa2cbeee1033948603f97a9c9564abd5e93d`。
- 文档清理：删除过期派发和分支指令，纠正逐 quad 提交、单级菜单、UI 语法未实施等旧描述；当前模块职责与历史证据分开维护。

新增边界检查仍在同一 `editor_document_acceptance` 内，使用真实 World、Source/JIT 和 UI 输入；相关负责人已定向复核修复。

## 正式组合验收

应用集成提交 `acae7b90784c7a1ad6a76e077a9ee5a3b6592530` 汇合以下生产者提交，保留各自历史：

| 改动 | 生产者提交 |
| --- | --- |
| 持久 Session、稀疏更新与失败恢复 | `08b2fa2cbeee1033948603f97a9c9564abd5e93d` |
| 原生 UI 组件、编辑器迁移、审查修复与文档清理 | `c49ba44d48f7d271b422e5fa199ea3ee74ce4bed` |
| RHI 所有权与当前能力文档 | `f2c5f8180001268c3819f8d2a511284dd16c7005` |
| Renderer Guides 与正式编辑器接线 | `d2dafe49298b2e67617e270df788c050badc246d` |

Runtime、World、Foundation 负责人完成各自接点审查，没有需要其模块修改的缺陷，不制造空提交。README 与 Workspace 邻接声明的合并冲突已保留两侧意图；旧 Position 构造和无效文档陈述未恢复。

在组合工作树执行两个配置的 `atelier_editor editor_document_acceptance ui_acceptance renderer_acceptance` 目标，均通过，UI 为 20/20；Renderer 包含真实 GPU 的空场景、渐变/网格/轴线、深度、相机及资源退休检查。随后以干净的 `acae7b9` 重编正式编辑器，依次运行两个配置的 `--acceptance-project`，退出码均为 0，报告均为 PASS。

正式窗口链同时覆盖 Guides 菜单和像素变化、显示/TRS、Position 参数编辑/撤销重做、项目导入、保存重开、失败打开及工程搬移。原 `E:/ffa_testproj` 的 Config/Content/Source 文件集合和 SHA-256 保持不变。未新增真实系统 IME 或完整普通控件窗口矩阵的验收结论。

组合证据在 `out/unified-review-20261005/`：

- `dev-build.log`、`release-build.log`：组合编译与后台消费者。
- `dev-window-evidence/run-68616-345766906/acceptance.txt`。
- `release-window-evidence/run-6184-345813640/acceptance.txt`。
- 窗口目录保留 Guides 与项目 PNG；`manifest.json` 固定验收提交、源码、部署资产和二进制哈希。

之后的收口提交只更新文档；以上窗口报告保持其真实验收提交身份，不重标为后续文档或主线合并 SHA。
