# Editor CPU 重复工作清理（2026-10-16）

状态：两项 App/UI 改动已通过 Release 构建、无 GPU 文档端到端验收，以及空闲时段的最大化原生 1440p 前后对照。正常停靠布局仍待补测；未提交或推送。

## 依据与范围

`out/pipeline-baseline-20261016/uncapped-full-01/` 的分段报告帮助定位控件生成、场景快照与未归类的 Application 开销。但测试期间另一个游戏占用了大量 GPU 和显存，该轮只能用于定位，不能作为正常 FPS 或 GPU 性能基线。最大化视口还隐藏了其他面板，不能代替完整停靠布局的验收。

这次只移除代码中确定存在的重复工作，没有改光照算法、RHI 提交路径或 World 作者值。

## 两项候选

- **共享同帧几何成员**：`SceneSnapshot::refresh` 在生成 MeshInstance 时一并记录所属 Entity。`ViewportTool::overlay` 使用这份成员列表判断哪些实体需要 locator，移除原先第二次完整 `World::render_items()` 调用。列表每帧重建、文档 reset 时清空，不持有 Runtime 根，也不引入跨帧缓存。拾取与交互命令保持原入口。
- **按需查询材质成员**：Workspace 仍每帧读取当前 Entity/Component 目录，校验现有绑定、选择和导航。只有这些 UI 引用未被目录解析时，才沿原路径扫描材质槽。材质实例、隐藏面板、删除与撤销仍使用当前成员关系；不依赖 Session revision 猜测 Runtime 或材质是否变化。

第一份 SceneSnapshot 仍执行正式 World 提取，未添加未经验证的跳帧或作者状态缓存。

## 已执行的验收

- Release `atelier_editor`、`atelier_editor_document_acceptance` 构建成功：`out/pipeline-baseline-20261016/editor-cpu-reuse-build.log`。
- `--viewport-edit` 通过：覆盖选择、手柄、历史、实体组合，以及新增的几何成员对应、删除清空和 undo 恢复检查。日志为 `editor-cpu-reuse-viewport.log`。
- `--materials` 通过：覆盖材质资源和颜色事务的拒绝回滚、取消、资产/实例历史及保存。日志为 `editor-cpu-reuse-materials.log`。

后两份日志位于同一目录。上述文档验收没有启动原生编辑器窗口或 GPU；后续 GPU 对照由渲染负责人串行运行，没有保存用户场景。

## 最大化布局的空闲对照

证据为 `out/pipeline-baseline-20261016/idle-app-{before,after}-01/render-performance.json`。两次均 PASS、VSync 关闭，实际 viewport 为 2560×1440，源 Scene SHA-256 均为 `68657ad2…c36b38`。以下为接受步数除以真实墙钟时间，非 GPU 耗时的倒数。

| 模式 | 修改前接受步/秒 | 修改后接受步/秒 |
| --- | ---: | ---: |
| Smooth | 59.05 | 77.36 |
| MatCap | 59.39 | 72.80 |
| 默认 Scene Lit | 32.43 | 32.24 |

Smooth 的 controls 均值从 3.586 ms 降至 2.064 ms，other_application 从 3.426 ms 降至 0.527 ms；MatCap 分别为 3.589 → 2.222 ms、3.536 → 0.562 ms。默认 Scene Lit 本轮吞吐未改善。该结果只覆盖同机最大化布局，不能替代完整编辑器的结果。

## 留存版本与正常布局补测

完整部署副本保留在 `out/pipeline-baseline-20261016/application-before-d09018/`：

- `atelier_editor.exe`：修改前测量版，SHA-256 前缀 `D09018E63FE8`。
- `atelier_editor.application-reuse.exe`：两项候选，SHA-256 前缀 `EBDC6C95ACAD`。

二者共用留存的 DLL、字体和样式；完整哈希及 UI 源/样式一致性记录在 `baseline.json`、`candidate.json`。两份程序均支持无 VSync、原生 1440p、Smooth/MatCap 与分段 CPU 采样。

后续验收源码在原四组之后，通过 Dock 原有恢复入口追加 `docked_smooth`、`docked_matcap`，保留树和分隔比例，重新校准 viewport，记录各自 layout、完整 RT 与活动面板。此补测比较同一最终版本的正常布局与最大化布局，不构成旧版正常布局的前后对照；上面留存的两个二进制不含这项追加。

## 正常布局校准收尾

`idle-rhi-root-cbv-01` 在正常布局校准时失败，没有产生 docked 样本；运行期间另有游戏占用 GPU，该轮 FPS/GPU 数据不能用于候选收益判断。root-CBV 候选已单独留存并从主目录撤回，App/UI 复用与 CPU 计时保留。

隐藏 Win32 窗口探针 `out/pipeline-baseline-20261016/calibration-window-cpu-probe.json` 没有创建图形设备：普通 `SetWindowPos` 对 4800×2800 / 6400×3600 均返回成功，但外框高度实际为 2186，客户区高度为 2130。加入 `SWP_NOSENDCHANGING` 后两种尺寸均真实到达，`WM_SIZE` 仍派发。此结果验证本机默认 tracking-size 限制；原失败报告未记录每步尺寸，无法回溯其全部收敛过程。

验收校准已使用该标志，并记录逐步请求、实际尺寸和像素误差；窗口截断、重复状态或超出允许范围立即失败。仅调整验收代码，保留产品窗口逻辑、Dock 树与比例，以及实际 viewport 精确等于 2560×1440 的要求。依据见 [WM_WINDOWPOSCHANGING](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanging) 和 [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)。

完整产品的校准和正常布局性能仍待空闲时实机验收；隐藏窗口探针及编译均不能替代 GPU 端到端通过。

## World 提取与 UI 成员目录

继续使用原地牢工程进行 CPU-only 对照，证据为同目录的 `scene-cpu-before.json` / `scene-cpu-after.json`。通过 Explorer 桌面启动正式文档验收程序，避免工具宿主的 AppData 虚拟路径；未启动 Editor/GPU，未保存工程。每项预热 20 次并独立采样 121 次。源 Scene 共 3572 个 Entity、3381 个 Mesh instance、481 份几何、2652 个相邻几何批次、1853242 个实例三角形。

本轮保留两处修改：

- `Workspace::describe` 从 World 已有类型目录取得全部组件身份，移除逐 Entity 读取 components、构造并释放临时 ObjectRoot 的过程；当前调用的 live 集合使用已有连续散列表。禁用组件仍在成员目录内，结构变化仍由 World 的同一发布链更新，没有新增注册表或跨帧作者缓存。
- `SpaceGraph` 在同次空间求值中复用路径与去重容器，根节点直接返回空父级，已有矩阵直接复用；几何提取按现有目录预留结果和映射容量。整个求值上下文仍在调用结束销毁，后续调用重新读取 Runtime TRS。材质 Surface 实测占比很小，本轮未新增材质缓存。

| 独立 CPU 调用 | 修改前中位数 ms | 修改后中位数 ms |
| --- | ---: | ---: |
| World render_items | 3.690 | 2.186 |
| World light_items | 0.197 | 0.173 |
| material_surfaces | 0.124 | 0.115 |
| SceneSnapshot | 3.984 | 3.062 |
| Workspace 最大化布局 | 1.749 | 0.232 |
| Workspace 正常停靠布局 | 11.578 | 7.671 |

SceneSnapshot 均值为 4.418 → 3.123 ms，最大化 Workspace 均值为 1.948 → 0.237 ms。各项是独立调用，不能把整项与子项相加。Workspace 包含 DSL 提交但没有接受 UI 帧：树展开状态不从帧更新，资源目录宽度使用默认限制，没有 UI prepare/裁剪；表中正常布局不是产品稳态帧耗时，后续仍需真实窗口补测。记录不据此宣称新的 FPS。

复用原 `idle-app-before-01/render-performance.json` 接受相机矩阵的静态 CPU 统计为 668 个可见实例、2713 个剔除实例、557 个连续可见几何段、373180 个可见实例三角形。实际采样图像按 Image 身份+sRGB 模式去重为 12，含默认白图的纹理表共 13 项。它们用于后续 Renderer/RHI 归因，不替代其图像正确性和 GPU 性能验收。

Release `atelier_editor_document_acceptance` / `atelier_world_acceptance` 构建通过，现有 World 端到端 15/15、viewport/Entity composition、materials/color transactions 均通过。新增视口验收覆盖直接 Runtime 修改在 Session revision 不变时可见、非法无限值拒绝以及随后恢复。证据为 `scene-cpu-reuse-build.log`、`scene-cpu-profile-final-build.log` 和 `scene-cpu-{world,viewport,materials}-e2e.log`；Dev/Release 最终集成由渲染负责人接续，本轮无提交或推送。
