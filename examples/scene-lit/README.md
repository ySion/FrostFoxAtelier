# Scene Lit Room

用于首批 Scene Lit 的可编辑房间。通过正式编辑器打开本目录，场景只引用内置网格，无需导入资源。

```powershell
./out/release/atelier_editor.exe --project ./examples/scene-lit
```

在 Scene View 显示菜单选择 **Scene Lit**。房间包含一盏方向光、两盏彩色点光和一盏朝下的矩形面光，以及粗糙石材、铜/钢块、反射地面和独立自发光装饰。灯与材质可以直接通过 Actor Structure / Parameters 编辑、保存和重新打开。自发光装饰是独立发光体，不是矩形灯的重复可见替身。

效果开关、曝光与阴影预算沿现有显示菜单调整。Edit/Play 与显示模式相互独立。材质和灯光改动属于场景数据，显示开关不进入场景撤销历史。

完整功能与验收状态见 [Renderer 合同](../../docs/modules/renderer/contract.md)；首批边界见 [实施建议](../../docs/research/scene-lit-rx6800xt-batch1-20261008.md)。本目录不是性能成绩，实际测量使用专用端到端场景及其日志。
