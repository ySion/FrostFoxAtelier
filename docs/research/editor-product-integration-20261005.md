# 2026-10-05 编辑器完整操作链

状态：固定代码 `aa9b310c619beec1cc3c21a1b000f047ce2f3233` 的 Dev（Debug）和 Release 完整窗口验收均通过。本轮主线发布与各工作树同步由主持统一执行。

## 范围与来源

在同一正式 `atelier_editor` 中完成打开工程、创建／导入网格、Tree 选择、位置修改与撤销、MatCap／线框切换、保存重开。应用负责完整操作链和组帧，Renderer 负责画面，World／Runtime 保持对象与文档语义，RHI 负责 GPU 执行和资源生命周期。

- 主线来源：`323a1e9988367f4b6f42847d6cdc3372fe58ff5d`。
- 应用构建信息与菜单归并：`93a394ea9a122442d3873a8b576e2873e27d67ba`。
- 独立工程窗口验收模式：`0a3f634ebe4757d833f70c6210926fd5208c1909`。
- Renderer／RHI 来源：`fb77b6a04063cb3e1df7f6d6e1872a9804890cc2`，包含原 `495bf51` 候选及跨 shader stage 根签名预算修复。
- 正常合并：`5cd730484b9a31abba222d80d904af236a54a0bc`，保留双方历史、正式视口和工程验收模式。后续工程链增量复用同一程序。
- 后续正常合入 Renderer 固定 `4722d20673ba659073606735bbcabd9eee73607c`，merge 为 `b663e222e9ff03a9b6d368b1179b8c80f7e4b615`；保留 RGB／真实相机放大采样及前台激活诊断。Project 与 Viewport 的初始激活等待归入同一个验收 helper。

## 用户可以辨认的运行版本

CMake 在构建时生成提交与配置，工作树有未提交内容时明确标为 `local changes`；没有 Git 信息时标为 `unknown`。未变化的生成文件不重复写入。启动日志、`--version` 与 Help → About 共用这些二进制内数据，运行时不查询当前目录的 Git 状态。

About 显示实时工程根目录与场景路径；工程成功打开后记录实际根目录。菜单分组、标题、Alt 键及左右导航均来自现有命令定义表，新增 Help 沿用同一菜单／Action 执行链。

## 同一程序的工程验收

`editor_project_acceptance` 调用正式程序的 `--acceptance-project` 模式，在同一次窗口激活后执行工程操作链。当前覆盖范围见[编辑器验收入口](../modules/editor/README.md#验收入口)。直接提供源工程的形式为：

```powershell
./out/dev/atelier_editor.exe --acceptance-project ./out/dev/project-evidence E:/ffa_testproj
./out/release/atelier_editor.exe --acceptance-project ./out/release/project-evidence E:/ffa_testproj
```

每次产生独立 `run-*` 目录。源工程只读复制 Config／Content／Source；不复制链接或 Cache，拒绝在源工程内生成验收输出。所有导入、文件破坏场景、保存及目录搬移均发生在副本。初始原工程两份文件的 SHA256 与备份保存在 `out/product-integration-20261005/project-source-manifest.json` 和 `project-source/`。

检查覆盖 About 的构建／配置／当前根目录，菜单键盘循环，真实资源目录与 Actor 创建，Tree 选择，位置修改／undo／redo，MatCap／叠线实际 RGB 像素变化，显示设置不改文档，保存重开及资源身份恢复。原有失败打开、保存成功但资源刷新失败、工程搬移和 Cache 可丢弃检查继续保留。文件选择由正式命令的路径参数驱动；这些检查不等同于人工操作系统文件选择对话框。

## 验收状态与证据

- 合并 `5cd7304` 的 Dev／Release 正式 editor 编译、World 8／8、Project／Workspace／editor_document 均通过，日志为 `out/product-integration-20261005/{dev,release}-merged-build.log`。
- `aa9b310` 的 Dev／Release 正式 editor 均重新编译，实际二进制的 `--version` 与窗口报告均对应这一干净提交。两次窗口串行执行并正常退出，报告均为 PASS；共享视口／TRS、About、完整工程操作及失败路径全部通过。
- 本次窗口证据根目录为 `out/product-integration-20261005/`：Dev 为 `dev-window-evidence-resume2/run-63520-332969390/`，Release 为 `release-window-evidence-resume2/run-73296-332990718/`；每个目录含 `acceptance.txt`、实际 GPU PNG 及独立工程副本。对应启动日志为 `{dev,release}-product-window-resume2.log`，构建日志为 `{dev,release}-overlay-fixture-build.log`。
- 已查看 MatCap、Flat、纯线框、导入网格叠线及保存重开截图。两配置在同一视口内部区域 `(0,290)-(1152,907)` 比较导入网格 Off／Overlay，RGB 任一通道差值大于 3 的像素均为 1126；自动验收另检查深度、选择、序列化文档和撤销历史保持一致。
- 原 `E:/ffa_testproj` 的 Config／Content／Source 文件集合与 SHA256 均与验收前快照一致。`candidate-manifest.json` 记录实际验收提交、可执行文件 SHA256、配置、报告和日志；旧 `f0052b2` 的清单与首错证据另行保留。
- `f0052b2` 的 Dev 普通窗口已完成共享视口／TRS、About、工程打开、导入及参数撤销重做；随后导入网格的叠线像素检查失败。截图与 Renderer 合同确认：选中对象在 Off 和 Overlay 均保留同样的橙色轮廓。验收改为通过 Scene Tree 选择 World 后比较同一未选中网格，保持原像素阈值，并独立检查深度、选择和文档不变；保留两帧截图与变化像素数。生产渲染行为未修改。首错日志为 `out/product-integration-20261005/dev-product-window-resume1.log`。
- Renderer 在固定 `5c1f21a29fd281cca6e9b87545d46e7c318dd48d` 的 Dev／Release `renderer_acceptance` 均通过，覆盖 MatCap、线框、矩阵和资源寿命；后续到 `4722d2` 仅改变窗口验收初始等待和记录。本轮应用候选的 Renderer／Geometry／RHI／gfx 生产代码与该版本一致。
- 正式视口专项先因小球体的单 R 通道采样不足而失败，已改为通过现有相机放大球体并比较 RGB，保留原差异阈值。此前的初始前台拒绝单独保留；本次两配置均实际获得前台后完成验收。当前同一窗口最多等待激活 120 秒，运行后失焦仍停止输入。
- 本阶段不宣称普通 Project 确认对话框和间歇性 Unicode 剪贴板问题已解决；按实际覆盖结果更新，不以 Renderer 专项代替。
