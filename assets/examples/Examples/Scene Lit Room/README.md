# Scene Lit Room

用于观察光照、材质与轮廓清晰度的可编辑房间。通过 Project → Examples → Scene Lit Room 打开，场景只引用内置网格，无需导入资源。示例的用户副本与保存约定见 [官方 Examples](../../README.md)。

```powershell
./out/release/atelier_editor.exe
```

在 Project → Examples 中选择 **Scene Lit Room**，入口自动设置相机与 Scene Lit。房间包含一盏方向光、两盏彩色点光、一盏朝下的矩形面光和显式的均匀天光，以及粗糙石材、铜/钢块、反射地面和独立自发光装饰。灯与材质可以直接通过 Entity Structure / Parameters 编辑、保存和重新打开。天光与 DDGI 体积保存在场景的 Environment 上，天光在 World Settings 的 Sky 组编辑。天光关闭时没有环境补光。Scene Lit 使用黑背景，切回 MatCap 恢复其渐变偏好。自发光装饰是独立发光体，不是矩形灯的重复可见替身。

**Window → World Settings** 提供效果开关、曝光与直接光候选数。**Antialiasing** 可选 Off / TAA / SMAA / SMAA T2x，默认 SMAA。可用房间中的硬边、反射地面和动态灯光观察各项差异。Edit/Play 与显示模式相互独立；材质、灯光与 World 效果配置属于场景数据，修改支持撤销和保存；着色预览与诊断开关不进入场景撤销历史。

完整功能、近场近似的边界和默认值见 [Scene Lit 合同](../../../../docs/modules/renderer/scene-lit.md)，实际结果见 [验收记录](../../../../docs/modules/renderer/acceptance.md)。首批工程加载、参数编辑、保存和重新打开的历史验收，不自动代表本轮新版质量已通过；固定场景的性能结果也不代表编辑器工程的操作耗时。质量专项入口为 `atelier_renderer_acceptance --scene-lit-quality --output <绝对目录>`。
