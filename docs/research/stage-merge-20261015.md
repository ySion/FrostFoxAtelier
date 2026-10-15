# 2026-10-15 阶段合并与结构审查

状态：本批合并及下列审查修复已完成双配置验收。范围是 `3932b310` 之后的已完成增量，不代表全仓库不存在技术债。

## 合并范围

- Claude UI：`92630102`、`1bbf6cef`、`94d92f0e`、`2fc5b263`，包含紧凑视口导航、吸附与 Global/Local 方向、Scene Tree 拖动和颜色编辑器。
- 反射与 UI：`a8fcdfa3` 的可选 DESC、拥有式描述元数据、500 ms Tooltip、共享 PropertyRow 和 World 选项描述。
- 渲染：`99111cab` 的 DDGI 历史与更新，以及 `86c197d3` 的空间光源提议、直接光重建和分阶段计时。
- 启动：`c1b73937` 的初始 client 物理尺寸。默认 DIP 和后续 resize 行为保持原合同。
- 合并点：`04b1c0cd`。后续审查修复沿现有事务和输入机制完成，不增加第二套属性或材质保存入口。

PropertyRow 同时传递说明和颜色操作，删除被 Claude 单行参数标题替代的 ObjectHeader/ViewportTools 定义。World 使用 ViewOptionInfo 的权威说明，实际作者值仍走原 Scene 历史，诊断选项仍为视口偏好。

## Standards

依据根 AGENTS.md 和 docs/engineering.md，由独立结构审查覆盖所有权、事务、状态清理与重复入口。

| 发现 | 处理 |
| --- | --- |
| 颜色分组预览未进入 Scene 脏状态，关闭确认可能跳过保存提示 | dirty 包含活动分组；关闭确认和保存先完成颜色事务 |
| 材质色片进入只接受 Entity/Component 的分组入口 | 分组编辑复用既有 material_edit_，共用材质撤销、取消和保存 |
| Popup 的菜单键盘过滤吞掉表单文字、方向键和 Tab | 菜单导航限定到菜单；表单走普通字段输入，Tab 限于弹窗内部 |
| TreeItem 开始拖动却在取消时发送 edit_cancel | Esc、失焦、失去捕获统一发送 drag_cancel |
| Tooltip 缓存误混入未使用的颜色操作字段 | 删除该字段，缓存仅保留描述和使用状态 |

4 项行为契约缺口和 1 项残留已关闭；本次没有保留待处理的阻塞项。

## Spec

依据用户要求：保留 Claude UI、统一反射描述和 World 参数展示、维护既有行为，并清理本批结构问题。

| 发现 | 处理 |
| --- | --- |
| 材质颜色选择未接通正式材质编辑链 | 复用原材质事务；真实文件往返验证资产和实例保存 |
| 合法 HDR 颜色因 RGB/V 上限固定为 1 而挂载失败 | 沿用原属性范围；只在预览成功后更新显示；挂载失败取消业务事务 |
| 取消树拖动后落点高亮未清除 | 补齐同一拖动取消协议，保留原开始/预览/提交链 |

3 项需求或行为缺口已关闭（其中两项与结构审查重复）；未发现需要保留的无依据范围扩张。

## 验收

以下均在主目录 `E:/FrostFoxAtelier` 执行，Dev/Release 均通过：

| 链路 | 结果与证据 |
| --- | --- |
| 编译及反射生成/发布/CLI | `out/integration-20261015-{dev,release}-build.log`，描述、类型寿命、UTF-8 与 30 项 CLI 拒绝场景通过 |
| UI 正式 Session/Layout/DirectWrite | `out/integration-20261015-{dev,release}-ui.log`，25/25，包含 Popup 表单与 TreeItem 取消 |
| 参数描述、DSL/JIT、热更和检查对象 | `out/integration-20261015-{dev,release}-document.log` |
| 材质与颜色事务 | `out/integration-20261015-{dev,release}-materials.log`，拒绝原子性、取消、单次 undo/redo、资产/实例保存重开 |
| 原生窗口与 GPU | `out/integration-20261015/{dev,release}-tooltips/`，Position/World 描述、HDR=2 色片、RGB/Hex 输入、Tab、Done、undo 与失焦取消 |
| Control 元数据和 Scene 隔离 | `out/integration-20261015-{dev,release}-descriptions.log` |
| 初始 client 尺寸与焦点隔离 | `out/integration-20261015-{dev,release}-client-size.log`，默认、1601×1003、3840×2054 和非法参数 |

原生颜色截图为 `parameters-hdr-color-editor.png`。这些窗口验收不注入全局键鼠。Renderer 自身的 ReSTIR/GPU 验收和性能边界保留在对应模块记录；本轮没有重新声称完整地牢达到某个 FPS，也没有运行所有模块的总验收。

## 原工程启动边界

从当前打包宿主继承的进程环境会把部分 AppData 文件映到包缓存：工程目录、Config 和场景可能落在不同的最终物理根，项目路径边界检查会拒绝。经现有 Explorer 桌面进程启动的只读预检中，Config 和 Dungeon 都留在原工程根内；Dungeon SHA-256 与刚保存的原文件一致。

因此恢复原窗口使用正常桌面启动路径，保留原工程、Scene、相机和视口设置；没有改写配置、复制工程或放宽链接逃逸检查。探针和恢复证据位于 `out/editor-restart-20261015/`。以后从受虚拟化的工具环境启动同一 AppData 工程仍可能触发边界拒绝，这不等于该启动环境已被引擎支持。
