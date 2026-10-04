# 2026-10-05 编辑器完整操作链

状态：统一候选，等待最终窗口验收。后台通过不代表本页列出的窗口操作已经通过；本轮最终发布由主持统一执行。

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

`editor_project_acceptance` 调用正式程序的 `--acceptance-project` 模式。在同一次窗口激活后，先执行与视口专项相同的 `verify_viewport_display`（包含实际 RGB 模式变化及 World TRS → GPU），再接续工程操作链，减少重新启动和抢占前台。直接提供源工程的形式为：

```powershell
./out/dev/atelier_editor.exe --acceptance-project ./out/dev/project-evidence E:/ffa_testproj
./out/release/atelier_editor.exe --acceptance-project ./out/release/project-evidence E:/ffa_testproj
```

每次产生独立 `run-*` 目录。源工程只读复制 Config／Content／Source；不复制链接或 Cache，拒绝在源工程内生成验收输出。所有导入、文件破坏场景、保存及目录搬移均发生在副本。初始原工程两份文件的 SHA256 与备份保存在 `out/product-integration-20261005/project-source-manifest.json` 和 `project-source/`。

检查覆盖 About 的构建／配置／当前根目录，菜单键盘循环，真实资源目录与 Actor 创建，Tree 选择，位置修改／undo／redo，MatCap／叠线实际 RGB 像素变化，显示设置不改文档，保存重开及资源身份恢复。原有失败打开、保存成功但资源刷新失败、工程搬移和 Cache 可丢弃检查继续保留。文件选择由正式命令的路径参数驱动；这些检查不等同于人工操作系统文件选择对话框。

## 验收状态与证据

- 合并 `5cd7304` 的 Dev／Release 正式 editor 编译、World 8／8、Project／Workspace／editor_document 均通过，日志为 `out/product-integration-20261005/{dev,release}-merged-build.log`。
- 后续完整工程链已编入同一 editor，尚待串行窗口验收。程序在每次运行的报告中写入其实际构建标识和配置。
- `f0052b2` 的 Dev 普通窗口已完成共享视口／TRS、About、工程打开、导入及参数撤销重做；随后导入网格的叠线像素检查失败。截图与 Renderer 合同确认：选中对象在 Off 和 Overlay 均保留同样的橙色轮廓。验收改为通过 Scene Tree 选择 World 后比较同一未选中网格，保持原像素阈值，并独立检查深度、选择和文档不变；保留两帧截图与变化像素数。生产渲染行为未修改。首错日志为 `out/product-integration-20261005/dev-product-window-resume1.log`。
- Renderer 在固定 `5c1f21a29fd281cca6e9b87545d46e7c318dd48d` 的 Dev／Release `renderer_acceptance` 均通过，覆盖 MatCap、线框、矩阵和资源寿命；后续到 `4722d2` 仅改变窗口验收初始等待和记录。本轮应用候选的 Renderer／Geometry／RHI／gfx 生产代码与该版本一致。
- 正式视口专项先因小球体的单 R 通道采样不足而失败，已改为通过现有相机放大球体并比较 RGB，保留原差异阈值。后续启动遭遇初始前台拒绝；该结果没有证明运行期间发生了失焦。当前同一窗口最多等待激活 120 秒，分别记录 WAITING、RUNNING 或启动超时；运行后失焦仍停止输入。
- 本阶段不宣称普通 Project 确认对话框和间歇性 Unicode 剪贴板问题已解决；按实际覆盖结果更新，不以 Renderer 专项代替。
