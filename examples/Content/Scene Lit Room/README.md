# Scene Lit Room

用于观察光照、材质与轮廓清晰度的可编辑房间。打开 Examples 工程中的 `Content/Scene Lit Room/Scenes/Main.scene`，场景只引用内置网格，无需导入资源。工程入口与保存约定见 [Examples](../../README.md)。

```powershell
./out/release/atelier_editor.exe --project ./examples --scene "Content/Scene Lit Room/Scenes/Main.scene"
```

选择 Main Camera 后用 **View from Camera** 观察房间，在显示菜单选择 **Scene Lit**；Play 直接使用场景主相机。房间包含一盏方向光、两盏彩色点光、一盏朝下的矩形面光和显式的均匀天光，以及粗糙石材、铜/钢块、反射地面和独立自发光装饰。灯与材质可以直接通过 Entity Structure / Parameters 编辑、保存和重新打开。天光与四层全局光照、镜面反射、后处理配置保存在场景的 Environment 上，在 World Settings 中编辑。天光关闭时没有环境补光。Scene Lit 默认使用黑背景（仅在开启物理大气或绑定天空贴图时显示天空），切回 MatCap 恢复其渐变偏好。自发光装饰是独立发光体，不是矩形灯的重复可见替身。

**Window → World Settings** 提供光照、四层 GI、反射、雾效、曝光、色调映射（PBR Neutral / AgX）与抗锯齿设置。**Antialiasing** 可选 Off / SMAA / Atelier TAAU，默认 SMAA。可用房间中的硬边、反射地面和动态灯光观察各项差异。Edit/Play 与显示模式相互独立；材质、灯光与 World 效果配置属于场景数据，修改支持撤销和保存；着色预览与诊断开关不进入场景撤销历史。

完整功能、近场近似的边界和默认值见 [Scene Lit 合同](../../../docs/modules/renderer/scene-lit.md)，保留代表与覆盖边界见 [Renderer 验收](../../../docs/modules/renderer/acceptance.md)。固定场景的性能结果不代表编辑器工程的操作耗时。现行入口 `atelier_renderer_acceptance --scene-lit --output <绝对目录>` 运行独立真值代表，不再运行旧质量矩阵。
