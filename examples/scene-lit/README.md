# Scene Lit Room

用于观察光照、材质与轮廓清晰度的可编辑房间。通过正式编辑器打开本目录，场景只引用内置网格，无需导入资源。

```powershell
./out/release/atelier_editor.exe --project ./examples/scene-lit
```

在 Scene View 显示菜单选择 **Scene Lit**。房间包含一盏方向光、两盏彩色点光和一盏朝下的矩形面光，以及粗糙石材、铜/钢块、反射地面和独立自发光装饰。灯与材质可以直接通过 Actor Structure / Parameters 编辑、保存和重新打开。自发光装饰是独立发光体，不是矩形灯的重复可见替身。

显示菜单的 **Scene Lighting** 提供效果开关、曝光与阴影预算。**Antialiasing** 可选 Off / TAA / SMAA / SMAA T2x，默认 SMAA；**Local GI** 默认关闭，开启时自动关闭 GTAO，重新开启 GTAO 会关闭 Local GI。可用房间中的硬边、反射地面和动态灯光观察各项差异。Edit/Play 与显示模式相互独立；材质和灯光改动属于场景数据，显示开关不进入场景撤销历史。

完整功能、近场近似的边界和默认值见 [Scene Lit 合同](../../docs/modules/renderer/scene-lit.md)，实际结果见 [验收记录](../../docs/modules/renderer/acceptance.md)。首批工程加载、参数编辑、保存和重新打开的历史验收，不自动代表本轮新版质量已通过；固定场景的性能结果也不代表编辑器工程的操作耗时。质量专项入口为 `atelier_renderer_acceptance --scene-lit-quality --output <绝对目录>`。
