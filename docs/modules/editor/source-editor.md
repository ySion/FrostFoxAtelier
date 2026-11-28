# 编辑器内源码工作流

代码面板已接入工程 Source 编译、真实组件、场景与独立运行宿主。面板布局、标签条、面包屑大纲、内联查找替换、悬浮补全/签名卡片、问题抽屉与状态栏均由 [editor.ui.source](../../../assets/editor.ui.source) 中的 `CodePanel` 声明；底层使用原生 `CodeEditor` 控件与进程内 `CodeIntelligence` 语言服务，与其他面板共用同一套 Session、SS、DirectWrite、布局、输入法和 RHI。

## 界面结构与快捷操作

- **顶栏（文档标签条 + 构建运行组）**：左侧平铺已打开文档标签页（`CodeFileTab`，未保存文档以 `*` 标识）及已打开文件下拉菜单、新建组件（`+`）、打开（`Open`）、保存（`Save`）图标工具；右侧常驻 `Build [F6]`、`Run [F8]` / `Stop [Shift+F8]` 工程操作。外部磁盘写入冲突时展开 `.conflict-banner` 横幅供一键重载或确认覆盖。
- **第二栏（面包屑与符号大纲条）**：包含历史后退/前进（`Back` / `Forward`）、工作区相对路径面包屑、实时跟随光标所在类/方法/UI 组件/样式规则的 **Document Symbol Outline** 下拉框，以及右侧的查找替换（`Ctrl+F`）与问题抽屉（`Problems`）状态指示开关。
- **可折叠内联查找 / 替换 / 行跳转栏**：
  - `Ctrl+F`：展开查找栏（支持 `Aa` 大小写匹配、`ab` 全字匹配、`F3` / `Shift+F3` 上下跳转与编辑器内实时高亮及滚动条全局刻度预览）。
  - `Ctrl+H`：展开单步替换（`Replace`）与整文单步可撤销替换（`Replace All`）。
  - `Ctrl+G`（或点击底部状态栏行列读数）：展开 `Line[:Column]` 快速定位栏。
  - `Esc`：关闭弹出的补全列表或收起查找/行跳转栏并将焦点归还代码缓冲区。
- **编辑器渲染与现代编辑手势**：
  - 支持 `.source`、`.ui.source`、`.ss`、`.json` / `.scene` / `.material` / `.procgraph` 的多色词法+语义着色（基于单行 `Coverage` 水平 `clip` 切片，不增加额外 DirectWrite 布局与 GPU 纹理），并在 `.ss` 中内联渲染 `#rrggbb` 色块预览。
  - 独立行号槽（Gutter）集成未保存修改行色条（新增绿条、修改琥珀条，`Ctrl+S` 后归零）与行级错误/警告圆点；正文区绘制当前行高亮、4 空格缩进参考线（加亮当前光标所在 `{...}` 作用域）、匹配括号框、同标识符多处引用高亮、错误波浪下划线、行尾斜体 Error Lens 摘要以及右侧滚动条全局预览刻度（Overview Ruler）。
  - 支持 `{}` / `()` / `[]` / `""` 自动配对、选区包裹与右闭合符跳过、`{|}` 内按 `Enter` 自动展开缩进块、Smart `Home`（行首非空白列与第 0 列往返）、`Ctrl+/` 单行或块注释切换、`Alt+Up/Down` 整行上下移动、`Ctrl+D` 选词与下一处跳转、`Ctrl+Shift+K` 删除当前行、双击选词、三击或点击行号槽整行拖拽选择。
- **可折叠问题抽屉（Problems Drawer）与结构化状态栏**：底部抽屉以 `Table` 汇总全部实时编译与语法诊断，点击任意行直接跨文件定位；状态栏左侧展示问题计数徽章与构建/检查状态，右侧展示 `Ln X, Col Y (sel)`、缩进规格、换行符、编码与当前语言模式。

## 进程内实时语言服务（CodeIntelligence）

```text
CodeEditor 编辑输入 → CodeDocument 修订号递增
  → CodeIntelligence (~160 ms 防抖，无需写盘，亦不改动活动 World)
      ├─ .source    : 叠加全部未保存缓冲快照 → language::collect_module + 隔离 Runtime/Program::prepare(fast)
      ├─ .ui.source : ui_language::lower_module + 隔离 UI Runtime 增量组件编译复核
      ├─ .ss        : @tokens / 选择器扫描 + ui::StyleSheet::parse 状态与属性校验
      └─ .json 等   : json::Document::parse 语法与容器深度校验
  → 发布波浪线 + 行尾 Error Lens + Problems 抽屉 + 符号大纲 + 语义类型表
```

- **实时免落盘诊断**：`CodeDocument`、`TextEdit`、词法着色与 `CodeIntelligence` 全链路统一持有有效 UTF-8（`atelier::String` / `StringView`），输入停顿约 160 ms 后零转码直接在内存快照上执行与正式构建同源的前端及类型检查，UTF-8 字节列偏移直通对齐到具体词法 token；无需按 `Ctrl+S` 即可捕获语法、类型、字段范围、UI 属性互斥及样式表瞬态规则错误。`Ctrl+S` 落盘后继续走原有的 `ProjectCode` / `LiveDocument` 正式热更发布链。
- **上下文感知自动补全（`Ctrl+Space` 或输入触发）**：
  - `.source`：识别 `self.`（聚合当前业务类字段/方法与 `world.Component` / `world.Entity` 等原生基类反射成员）、`vector2/3/4` / `quaternion` / `Array<T>` / `String` 等内置值与序列成员、`struct` 字段、`Enum::` 枚举项、命名空间前缀、`[DESC = "..."]` / `[Label = "..."]` / `[Min = ..., Max = ...]` / `[InOut("...")]` 方括号属性注解，以及全部 `math.*` 内建函数与关键字。
  - `.ui.source`：识别当前 `ui` 组件的 `input.`、`state.`、`node.` 成员、`icon: "..."` 矢量图标名、控件属性名及内置/自定义组件名。
  - `.ss`：识别 `token(...)` 中的 `@tokens` 变量（附带颜色/数值预览）、`:hover` 等伪类选择器与样式属性名。
  - 补全浮层承载于 `CodePanel` 的 `Overlay` 层，键盘与输入法焦点始终留在 `CodeEditor` 内（`Up`/`Down` 切换，`Tab`/`Enter` 上屏，`Esc` 关闭）。
- **函数参数签名提示（Signature Help）**：在函数、原生方法或数学内建调用括号内自动悬浮显示完整签名并高亮当前实参槽位。
- **悬停检视与跨文件定义跳转（`F12` / `Ctrl+Click`）**：鼠标悬停展示编译诊断详情、反射类型/方法签名或 `@tokens` 取值；按住 `Ctrl` 悬停可跳转符号时显示链接下划线，`Ctrl+Click` 或按 `F12` 直接跨文件跳转至类、结构体、枚举、方法、函数、UI 组件或样式变量的声明位置，并压入 `Back` / `Forward` 导航栈。

## 文档、边界与生命周期

- 每个规范文件路径只有一份打开的文档；最多同时打开 16 份，每份 UTF-8 文件上限 1 MiB。
- `SourceEditor` 属于工作区，不持有场景 `Runtime`；`CodeIntelligence` 内部仅惰性维护独立的只读编译校验 `Runtime`，绝不向活动场景 `World` 注册预览类型或执行用户代码。UI 热更新、隐藏、停靠和场景切换不会重建源码缓冲或撤销历史。
- `TextEdit` 共用 Unicode 字符边界、选区和增量撤销记录；源码历史最多 512 组、8 MiB 文字，单行字段保持原有 64 组、64 KiB 上限。
- 磁盘检查每 500 ms 比较时间和大小，干净文档自动同步，存在未保存草稿时保留草稿并提示冲突。保存保持原有 UTF-8 BOM 和 LF/CRLF 约定，内存统一使用 LF。
- 生成 Source / LLVM IR 对照视图与断点单步调试器面板仍待后续接入；Runtime/JIT 的[诊断](../../source-diagnostics.md)与[源码调试核心](../../source-debugger.md)契约见对应文档。

## 端到端验收

- `ui_acceptance`（`acceptance/ui/main.cpp`）：覆盖 `CodeEditor` 单行 `Coverage` 水平 `clip` 多色语法切片、`.ss` 内联色块渲染、自动括号配对、`{|}` 智能回车缩进、`Ctrl+/` 注释切换以及 `mark_saved()` / `line_diff()` 修改行追踪。
- `editor_document_acceptance`（`acceptance/editor/document.cpp`）：覆盖多文件 `.source` 工程与 `.ui.source` / `.ss` 的免落盘实时内存诊断、`self.` 原生反射+业务字段上下文补全、参数签名提示、跨文件 `F12` 定义跳转与历史回退，以及单步可撤销的查找替换。
- `project_integration_acceptance` 与 `project_scene_acceptance`：覆盖真实工程跨文件编译运行、进程强停与场景字段序列化往返。
