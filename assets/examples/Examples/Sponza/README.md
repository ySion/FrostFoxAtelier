# Sponza 默认场景

直接启动 `atelier_editor`，或选择 **Project → Examples → Sponza**。编辑器以内庭相机打开 Scene Lit + DDGI。场景由建筑、布帘、一盏方向光和两盏暖色点光组成，Environment 带天空贴图和覆盖完整模型的探针体积。位置与强度可在 Parameters 中编辑。模板部署、用户副本和保存行为见[官方 Examples](../../README.md)。

## 模型与材质

模型是 Intel New Sponza（2022）的 glTF 版本：`NewSponza/` 为主体建筑，`SponzaExtend/` 为布帘。两者的 `.gltf`、`.bin` 和许可文件按原样复制；图片由 [downscale-sponza](../../../../tools/downscale-sponza/README.md) 从 4K 缩成 2K，法线图的绿色通道由源包的 DirectX 约定翻成 glTF 约定。每个文件的源和输出 SHA-256 记在各自目录的 `source.json`。PNG 和 `.bin` 使用 LFS。

两个网格按引擎的 glTF 导入规则加载：右手系转左手系，以米为单位，比例 1。主体 3,738,878 个三角形、27 个材质槽，布帘 1,997,366 个三角形、4 个材质槽。每个槽都有一份由 Import Mesh 生成的 `.material`，放在各目录的 `Materials/` 下，网格的 `.asset.json` 用 `materials` 把槽名映射到它们，作为[网格默认材质](../../../../docs/modules/editor/projects.md#材质与贴图资源)。场景不保存材质绑定，所有槽显示默认材质；编辑某个 `.material` 会改变所有用它的网格。主体的 `dirt_decal` 是 BLEND 材质，导入时跳过，它的贴图保留但没有被引用。

## 光照

方向光 Sun 为 3.5 lux，仰角约 56°。天空使用 [`Sky/kloppenheim_05_4k_nosun.hdr`](Sky/README.md)，这是去掉太阳的派生图，避免太阳经 DDGI 和方向光重复计入；`sky_rotation` 为 65°，让贴图里原来太阳的方位对准 Sun，`sky_intensity` 为 0.5，天空在水平面上的补光约 0.7 lux，约为 Sun 水平照度（按其颜色的亮度计约 2.6 lux）的 27%。背景里没有太阳圆盘。

探针体积覆盖完整模型：`[-21,-2,-10]`～`[17,20,15]` 米，`16×12×14` 个探针，每帧更新 672 个。它由场景保存，打开时不再重新拟合；改了模型后可用 View 菜单的 Fit Scene Lighting Coverage 重新拟合。

## 来源与许可

New Sponza 由 Frank Meinl 委托制作，Anton Kaplanyan 赞助，来源为 [Intel Sample Library](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)。许可以随包的 `credits_license.txt` 为准：开头注明个人和教育用途、有限的营销与印刷商业用途，正文为 CC BY 4.0。本仓库对它的修改是上面所述的贴图缩小、法线绿色通道翻转和生成的材质文件。

## 验收

`editor_sponza_acceptance` 验证完整模型和默认材质、Smooth/MatCap、Scene Lit 下 DDGI 开关、移动灯光和关灯消退；`editor_examples_acceptance` 验证编辑器默认启动和官方菜单入口；`--textured-obj` 检查两份网格的三角形数、槽数、默认材质和贴图，以及保存重开。当前 DDGI 实测与限制见 [Renderer 验收记录](../../../../docs/modules/renderer/acceptance.md)。
