# 编辑器内源码工作流

代码面板已接入工程 Source 编译、真实组件、场景与独立运行宿主。代码面板的布局、文件选择、保存和诊断行由 `CodePanel` 声明；它使用正式 `CodeEditor` 控件，同其他面板共用 Session、SS、DirectWrite、布局、输入法和 RHI。

## 使用

- **Project → New Component Source / New Project UI** 在当前工程创建源码；F6 构建，F8 运行，Shift+F8 停止。新增或改变类型后使用 Rebuild Types。
- **Project → Edit Project UI / Preview Project UI** 编辑或独立预览工程配置的 UI。
- **File → Edit Editor UI Source** 打开编辑器自身的 UI DSL；开发工作树通常为 `assets/editor.ui.source`。
- **File → Open Source…** 打开已有 UTF-8 文本；Asset Browser 中 Source 文件支持双击、Enter 和 Open。
- **Window → Code** 显示可停靠代码面板。顶部文件菜单切换已打开的文档，名称后的 `*` 表示未保存。
- **Ctrl+S** 保存当前工作区域的文档：Code 区保存当前源码，其他区域保存场景。File → Save Scene 始终保存场景；Save All 保存已修改源码和当前场景。
- **Ctrl+W** 在 Code 区关闭当前源码文档；没有文档时关闭面板。标签栏关闭只隐藏 Code 面板，文件内容和历史仍保留。
- 关闭修改过的文档、切换工程或退出会询问保存；打开另一个场景保留源码文档。

支持鼠标选区、字符/词移动、上下行、Home/End、Ctrl+Home/End、PageUp/PageDown、全选、剪贴板、独立撤销/重做、Enter 延续缩进、Tab/Shift+Tab 缩进以及横纵滚动。行号与文本只绘制可见行。按 Esc 取消输入法组合或当前手势，已确认的代码输入留在文档中。

## 保存与编译

```text
CodeEditor 输入 → SourceEditor 持有的 CodeDocument
Ctrl+S → Application 命令 → 检查磁盘版本 → 原子写入源文件
ProjectCode / LiveDocument 检测保存 → Source/JIT 同步编译 → 候选事务
成功：提交兼容代码或 UI；失败：保留最后成功版本，在 Code 显示诊断
点击诊断 → UTF-8 源码列转 UTF-16 编辑位置 → 定位并聚焦代码
```

未保存输入只改变源码文档。编译诊断属于已经尝试的磁盘版本；继续编辑后，旧诊断标为 Previous saved version，定位入口失效。修正并保存后重新使用同一编译/发布链。没有第二个 UI 编译器或源码解析器。

Workspace 独立拥有编辑器 UI Runtime；场景切换清理旧业务绑定，保留最后提交的 UI 源码检查点以及所有源码文档。若启动时作者文件已损坏，会通过同一编译链使用随程序部署的 UI 源码恢复界面，同时保留原错误供修复；恢复不会覆盖损坏的源文件。部署副本也损坏时仍会报告启动失败。

普通 `.source` 文件以工程为编译单元，诊断保留原文件、行列和磁盘版本。即使当前打开的是另一份源码，也能从错误入口跳到目标文件；定位前读取目标磁盘并与编译快照核对，干净但过期的已打开缓冲从该快照刷新；有草稿或磁盘已变成新版本则拒绝定位。连续点击同一诊断复用 CodeDocument，不重建其历史。Project Output 保留最近 256 行构建和运行输出。SS 文件可以编辑保存，仍在启动时加载。Code 面板中的生成 Source/LLVM IR 查看、语义补全、符号跳转、查找替换和调试交互仍待接入。Runtime/JIT 的[诊断](../../source-diagnostics.md)与[源码调试核心](../../source-debugger.md)已经可用；面板、快捷键和外部调试指令的共同接入方向见 [Editor Control](control.md#源码调试接入待实现)。

创建组件、添加属性、保存场景、运行及项目 UI 的完整用法见[工程源码与运行](projects.md#工程源码与运行)。项目 UI 的表达式和事件只在独立运行进程求值；Code 所在的编辑器只做编译检查。

## 文档、边界与生命周期

- 每个规范文件路径只有一份打开的文档；最多同时打开 16 份，每份 UTF-8 文件上限 1 MiB，与现有 UI 作者文件限制一致。
- SourceEditor 属于工作区，不持有场景 Runtime。UI 热更新、隐藏、停靠和场景切换不会重建其缓冲或历史。
- TextEdit 共用 Unicode 字符边界、选区和增量撤销记录；源码文档长期持有它，单行参数字段保留原有草稿事务。源码撤销不进入场景历史；源码历史最多 512 组、8 MiB 文字，单行字段保持原有 64 组、64 KiB 上限。
- 行索引与字符边界在编辑时更新，仅重分段受影响行；每帧不复制整份源码，也不为每行创建控件。当前使用有界连续文本存储，尚未宣称超大文件性能。
- CodeGeometry 记录接受帧的文本版本、可见行和布局快照；过期文本不能用于指针定位，版本不匹配的候选帧不能被接受。
- ClipboardRequest 沿既有 token、操作身份与结果校验，源码放宽至文档容量，单行字段保持原限制。跨文件切换取消旧组合和请求，旧结果不能写入新文档。
- 磁盘检查每 500 ms 比较时间和大小，变化后读取。干净文档可重新加载，存在本地修改时保留草稿并提示冲突。保存前还会读取并比较原文件，冲突需要用户明确覆盖或重新加载。
- 文件保持原有 UTF-8 BOM 和主要 LF/CRLF 约定，内存统一 LF；粘贴时规范化换行。文件删除、读取失败、无效 UTF-8 与超限不会清除当前草稿。
- 保存使用既有同目录临时文件、刷新与替换机制。磁盘比较与替换尚不是跨进程事务锁，不承诺多个外部写者同时提交时的严格串行化。

## 轻量后端取舍

评估了 [Scintilla](https://www.scintilla.org/ScintillaDoc.html)、[Lexilla](https://scintilla.org/LexillaDoc.html) 和 [Tree-sitter](https://github.com/tree-sitter/tree-sitter)。Scintilla 的 Windows 接口是独立原生控件；Lexilla 提供不依赖显示系统的词法器；Tree-sitter 提供增量语法树，均不直接提供本项目的 Source 类型语义。

本阶段复用已有 ICU/DirectWrite/IMM32 编辑设施，把持久文档、增量历史和多行呈现接入原生控件。没有嵌入另一套窗口/渲染系统，也没有新增第三方依赖。后续词法着色可以按实际语言支持成本复用成熟后端；语义诊断、补全和调试继续由现有编译器/Runtime 提供权威信息。

## 端到端验收

后台验收包含于 `editor_document_acceptance`；聚焦运行可在对应构建目录执行 `atelier_editor_document_acceptance --source-editor`。它使用真实作者文件、Source/JIT、Workspace、CodeEditor 和 Session，覆盖编辑/保存/错误保留/定位/恢复、虚拟行、剪贴板协议、文件冲突、换行保持、重复打开和隐藏恢复。

真实窗口入口为 `atelier_editor --acceptance-source <输出目录>`，要求前台焦点。覆盖菜单打开、原生选区输入、Ctrl+S 发布、错误点击、恢复、大段系统剪贴板、撤销与关闭；同一窗口还创建工程、添加 Source 组件、编辑数值与 bool、跨文件定位、保存重开并启动和停止游戏/UI 预览。每轮导出 PNG。后台协议验收不替代真实系统 IME 或窗口验收。

2026-10-06 工程接线基线结果（审查前）；本轮修订后的结果以[工作流审查](../../research/editor-project-review-20261006.md)为准：

| 验收 | Dev | Release |
|---|---|---|
| 工程 Source → 类型/属性 → 场景 → 热更 → 独立运行，含游戏/UI 死循环终止、空工程和首次新增类型 | PASS | PASS |
| Scene v1/v2、真实派生类型、标量历史、生命周期与 Jolt 所有权 | PASS | PASS |
| 共享 UI | 23/23 | 23/23 |
| 完整编辑器文档、DSL 更新与源码编辑 | PASS | PASS |
| 原生 Code 与工程工作流窗口 | PASS，4288 个接受帧 | PASS，468 个接受帧 |

后台日志位于 `out/project-{dev,release}-{integration,scenes,ui,documents}.log`。窗口报告与截图位于 `out/project-window-dev/` 和 `out/project-window-release/`；发布版窗口验收之后补充的空快照边界已通过两种配置的工程验收，开发版窗口使用该最终行为。真实系统 IME 未在本轮重跑。

`E:/ffa_testproj` 已加入 `Source/Math.source`、`Source/Move.source`、`Source/UI/Main.ui.source` 和 `Content/Scenes/SourceWorkflow.scene`，并配置项目 UI 入口；该目录已通过正式运行入口的 20 帧验收。打开 SourceWorkflow.scene 可以直接修改 speed/moving 并运行，原启动场景配置仍指向 Main.scene。
