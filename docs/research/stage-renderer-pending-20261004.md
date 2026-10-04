# 2026-10-04 Renderer 集成待验候选

状态：Dev／Release 后台组合验证通过，GPU／真实窗口验收未运行。该分支单独保留，不属于[本轮可发布主线候选](stage-sync-20261004.md)。

## 固定来源

- 分支：`codex/stage-renderer-pending-20261004`。
- 基础候选：`a59320054fbf705ba62b6e8bd159a78628cdf81d`，已包含新 Runtime 与方案归档。
- Renderer 交付：`58e4bf51abd5afb2aca928d096c1a2169ebe525c`。
- 正常合并后的实际验证代码：`ed5a49b79efdf94d27ba1b11e4c10958d1a5ee58`，无冲突；其后仅收口本文。

改动包含 MatCap／Flat／Smooth、纯线／叠线／隐藏线、Scene View 显示设置，以及内置网格的可选三角形边 mask。World 已确认 MeshRef／Scene 来源键兼容，无格式迁移；RHI 接口与后端没有改动。实现范围及限制见 [Renderer 合同](../modules/renderer/contract.md)，参考资料见 [Houdini 研究](houdini-viewport-rendering-20261004.md)。

## 本树组合验证

工作树为 `C:/Users/Arzuo/.codex/worktrees/math-project-sync/FrostFoxAtelier`。同一工作树、同一配置内通过普通 presets 构建全部依赖，没有借用其他工作树产物。

| 入口 | Dev | Release |
| --- | --- | --- |
| `atelier_editor` 与 `atelier_renderer_acceptance` 编译 | PASS | PASS |
| `world_acceptance` | PASS（8／8） | PASS（8／8） |
| `editor_document_acceptance`，含 Project／Workspace 和显示设置保持 | PASS | PASS |
| `renderer_acceptance` 真实 GPU／窗口 | 未运行 | 未运行 |
| `editor_viewport_acceptance` 正式编辑器窗口 | 未运行 | 未运行 |

后台入口为 `cmake --build --preset <配置> --parallel 8 --target atelier_editor atelier_renderer_acceptance editor_document_acceptance world_acceptance`，两种配置均以 0 退出。日志保存在 `out/stage-sync-20261004/renderer-{dev,release}-{configure,build}.log`。

## 下一步验收

获准使用桌面后，在该固定候选树依次运行两种配置，避免窗口争抢焦点：

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target renderer_acceptance
cmake --build --preset dev --target editor_viewport_acceptance
cmake --build --preset release --target renderer_acceptance
cmake --build --preset release --target editor_viewport_acceptance
```

画面、GPU 数据与日志按 [Renderer 验收](../modules/renderer/acceptance.md)和正式 editor 的 viewport 入口留存。由 Renderer 负责人主导专项，应用与 RHI 按真实失败接入；主持在证据齐备后决定发布。

普通 Project 窗口确认链、间歇性 Unicode 剪贴板问题及其他既有未验边界保持原状态，不由本专项代替。用户正在使用的窗口和 `E:/ffa_testproj` 均未改动。
