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
ProjectCode 检测工程保存 → Source/JIT 同步编译 → 工程候选事务
LiveDocument 检测 UI 保存 → 捕获绑定 → 后台编译变更入口 → Runtime 线程复核并准备新帧
成功：提交兼容代码或 UI；失败：保留最后成功版本，在 Code 显示诊断
点击诊断 → UTF-8 源码列转 UTF-16 编辑位置 → 定位并聚焦代码
```

未保存输入只改变源码文档。编译诊断属于已经完成尝试的磁盘版本；继续编辑或新版仍在后台编译时，旧诊断标为 Previous saved version，定位入口失效。修正并保存后重新使用同一编译/发布链。UI 首次加载及只有注释的更新见[声明式 UI 热更](declarative-ui.md#后台编译与发布)。

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

CodeDocument 创建、公开 `edit().replace` 和剪贴板/IME 的实际替换共用 [文档字符政策](../ui/README.md#多行源码编辑)：有效 Unicode、LF/tab，拒绝其余 C0、DEL 和直接 CR；粘贴先保留原有换行转换。非法直接编辑不会改变正文、版本或撤销/重做记录，不能再保存出违反创建字符合同的文件。长度预算只在创建或替换时检查，日志定位前缀与编译快照比较继续只检查字符。

加载先正规化、再由 CodeDocument 创建检查一次。只有 `invalid_text` 映射为原 binary/control 提示；ICU 分段失败保留系统错误诊断，不冒充文件格式错误。失败保留旧文档、BOM、磁盘版本与 saved_text。ICU 编辑/历史失败不发布部分正文或半次历史；缩进前选区与 history 的分组结束仍按既有行为执行，不扩大为所有交互完全事务化。

## 轻量后端取舍

评估了 [Scintilla](https://www.scintilla.org/ScintillaDoc.html)、[Lexilla](https://scintilla.org/LexillaDoc.html) 和 [Tree-sitter](https://github.com/tree-sitter/tree-sitter)。Scintilla 的 Windows 接口是独立原生控件；Lexilla 提供不依赖显示系统的词法器；Tree-sitter 提供增量语法树，均不直接提供本项目的 Source 类型语义。

源码编辑复用 ICU/DirectWrite/IMM32 编辑设施，持久文档、增量历史和多行呈现接入原生控件，不嵌入另一套窗口/渲染系统。后续词法着色可以按实际语言支持成本复用成熟后端；语义诊断、补全和调试继续由现有编译器/Runtime 提供权威信息。

## 端到端验收

Code 面板的编辑、保存、诊断定位、剪贴板与窗口交互目前没有自动验收。

`project_integration_acceptance` 无窗口地编译真实工程 Source，检查一次跨文件组件 tick，并检查独立 runner 中死循环的 Source 在停止时被强制结束；`project_scene_acceptance` 覆盖一次 Scene 字段值往返。运行命令和本轮日志位置以[验收门禁](../../../README.md#构建与端到端验收)为准，不将历史版本的 PASS 表作为当前构建结果。
