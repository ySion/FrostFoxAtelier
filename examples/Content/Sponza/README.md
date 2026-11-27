# Sponza

运行 `atelier_editor --project ./examples`，或打开 Examples 工程中的 `Content/Sponza/Scenes/Sponza.scene`。选择 Main Camera 后用 **View from Camera** 观察内庭，在显示菜单选择 Scene Lit。场景由建筑、布帘、一盏方向光和两盏暖色点光组成，Environment 绑定天空贴图并启用三层全局光照、三级反射与 `AgX` 色调映射；Play 使用场景主相机。位置与强度可在 Parameters 中编辑。工程入口和保存行为见[Examples](../../README.md)。

## 模型与材质

模型是 Intel New Sponza（2022）的 glTF 版本：`NewSponza/` 为主体建筑，`SponzaExtend/` 为布帘。两者的 `.gltf`、`.bin` 和许可文件按原样复制；图片由 [downscale-sponza](../../../tools/downscale-sponza/README.md) 从 4K 缩成 2K，法线图的绿色通道由源包的 DirectX 约定翻成 glTF 约定。每个文件的源和输出 SHA-256 记在各自目录的 `source.json`。PNG 和 `.bin` 使用 LFS。

两个网格按引擎的 glTF 导入规则加载：右手系转左手系，以米为单位，比例 1。主体 3,738,878 个三角形、27 个材质槽，布帘 1,997,366 个三角形、4 个材质槽。每个槽都有一份由 Import Mesh 生成的 `.material`，放在各目录的 `Materials/` 下，网格的 `.asset.json` 用 `materials` 把槽名映射到它们，作为[网格默认材质](../../../docs/modules/editor/projects.md#材质与贴图资源)。场景不保存材质绑定，所有槽显示默认材质；编辑某个 `.material` 会改变所有用它的网格。主体的 `dirt_decal` 是 BLEND 材质，导入时跳过，它的贴图保留但没有被引用。

31 个材质文件均为 v2 MI，继承内置 StandardPBR 的对应单面/双面策略；原资源 UUID、贴图引用、标量/向量参数、UV 变换和有效通道选择保持不变，包括显式写出的默认值与空贴图绑定。Scene v10 不再保存已被网格默认材质遮蔽的组件颜色等旧字段。

## 光照

方向光 Sun 为 3.5 lux，仰角约 56°。天空使用 [`Sky/kloppenheim_05_4k_nosun.hdr`](Sky/README.md)，这是去掉太阳的派生图，避免太阳经天空全局光照和方向光重复计入；`sky_rotation` 为 65°，让贴图里原来太阳的方位对准 Sun，`sky_intensity` 为 0.5，天空在水平面上的补光约 0.7 lux，约为 Sun 水平照度（按其颜色的亮度计约 2.6 lux）的 27%。背景里没有太阳圆盘。

三层全局光照（`SSILVB` + `8×8 Screen Probes` + `SHaRC` 多跳表面辐射度缓存）无需手工摆放探针体积即可自动覆盖内庭、拱廊与高处天窗反弹，场景曝光（`exposure_ev = 1.2`）与 `AgX` 色调映射参数保存在 `EnvironmentComponent` 中。

## 来源与许可

New Sponza 由 Frank Meinl 委托制作，Anton Kaplanyan 赞助，来源为 [Intel Sample Library](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)。许可以随包的 `credits_license.txt` 为准：开头注明个人和教育用途、有限的营销与印刷商业用途，正文为 CC BY 4.0。本仓库对它的修改是上面所述的贴图缩小、法线绿色通道翻转和生成的材质文件。

## 验收

Sponza 是 Examples 的启动场景；`editor_log_smoke_acceptance` 在复制的工程中打开它并做一次添加/保存/撤销与日志零错误冒烟，不修改仓库场景，也不检查本场景的资产身份往返。渲染特性使用 [Renderer 的独立真值代表](../../../docs/modules/renderer/acceptance.md)，不检查本场景的光照开关对照、移动灯光或关灯消退矩阵。
