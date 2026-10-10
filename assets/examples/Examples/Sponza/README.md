# Sponza 默认场景

直接启动 `atelier_editor`，或选择 **Project → Examples → Sponza**。编辑器加载内庭相机、Scene Lit + DDGI，以及方向光、暖色可移动点光和冷色点光三盏正式 LightComponent。位置与强度可通过 Parameters 编辑。模板部署、用户副本和保存行为见 [官方 Examples](../../README.md)。

启动时按渲染几何的世界范围一次性适配 DDGI：边界外扩至少 .5 米并取整，目标探针间距 2 米，每轴最多 16 个，总数最多 4096，每帧更新约四分之一。当前完整建筑对应 `[-20,-2,-13]`～`[19,15,12]` 米、`16×10×14` 个探针。相机随后移动不会重新拟合体积；视口策略不写入 Scene 文件。

建筑实例使用线性基础色 `(.55,.55,.55)`、粗糙度 `.85`。Scene Lit 使用原始 OBJ/MTL 颜色及透明度贴图，植物和铁链的 Mask 同时参与光栅、阴影、DDGI 与编辑深度；Smooth/Flat 共用材质覆盖，MatCap 用于观察几何。实例基础色仍参与默认材质调制。MTL 的凹凸图不会自动转换为标准材质的切线法线图；需要显式材质绑定，见[材质系统](../../../../docs/modules/renderer/materials.md)。

模型来自 [Morgan McGuire Computer Graphics Archive](https://casual-effects.com/data/) 的 [Crytek Sponza 完整归档](https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip)，作者 Frank Meinl / Crytek，基于 Marko Dabrovic 原版，经 Morgan McGuire 修正。原始声明保留在 [copyright.txt](Sponza/copyright.txt)，逐文件来源与 SHA-256 在 [source.json](Sponza/source.json)。资产权利以原始声明为准。

原始 OBJ、MTL 与全部 54 张 PNG 未修改；OBJ/PNG 使用 LFS。Scene 按 .01 比例转换原模型单位，保留 262,267 个三角形、25 个命名材质（24 个有漫反射贴图）及 103 个连续材质组，覆盖完整建筑、旗帜、植物和装饰。

`editor_sponza_acceptance` 继续验证完整资源、Smooth/MatCap 和 DDGI 开关、移动灯光、关灯消退；`editor_examples_acceptance` 验证编辑器默认启动和官方菜单入口。当前 DDGI 实测与限制见 [Renderer 验收记录](../../../../docs/modules/renderer/acceptance.md)。
