# Physics Test 工程

使用 `atelier_editor --project <仓库>/examples/physics-test` 打开；也可在编辑器中选择 File → Open Project Folder 并指向本目录。启动场景为 `Content/Scenes/Main.scene`，Scene Tree 包含静态三角网格地面、动态箱/球、普通 Actor 的胶囊 Body、Sensor、运动学平台和力/冲量目标。选择 Actor 后在 Actor Structure 查看和增删 Body 组件；完整 Body 配置目前通过 Scene 文件或 C++ API 调整。

打开工程时物体保持作者位置；拖动编辑器视角不会推进模拟。Project → Run 在独立会话启动 World/Jolt，停止后不回写编辑场景。当前没有物理暂停、碰撞调试绘制或运行时力/冲量控件。运行 `cmake --build --preset dev --target editor_physics_project_acceptance` 可加载本工程并验证重力、力、冲量、运动学目标和 Sensor 接触；发布配置将 `dev` 换成 `release`。独立场景 `physics_acceptance` 覆盖查询、过滤、Cook、生命周期及角色、约束、布娃娃、车辆等 P4 系统；P4 配置目前不能存入编辑器场景格式。

`Main.scene` 仅引用内置网格，无需导入 OBJ 或生成 Cook 缓存。`Cache/` 可随时重建，不是场景源数据。

`Content/Synty/AnimationBaseLocomotion/Animations/` 保存从 Unity 工程 `Assets/Synty/AnimationBaseLocomotion/Animations/` 复制的 692 个原始 FBX，保留 Polygon/Sidekick 及动作分类目录。Unity 的 `.meta` 和 `.controller` 未导入，也不会删除源工程文件。FBX 由 Git LFS 跟踪，克隆后需安装 Git LFS 并执行 `git lfs pull` 取得原文件。当前 Asset Browser 将 FBX 列为普通文件，尚不能解析或播放骨骼动画，场景也不引用这些动画。

## Sponza 场景

在仓库根目录执行 `./examples/physics-test/Open-Sponza.ps1`（默认 release，dev 使用 `-Configuration dev`）。脚本打开当前工程的 `Content/Scenes/Sponza.scene`，设置内庭相机，并默认进入 **Scene Lit + DDGI**。场景保存一盏方向光、可移动的暖色点光和冷色点光，均使用正式 LightComponent，可在 Parameters 编辑位置和强度。物理工程的启动场景仍为 Main.scene。

启动参数 `--scene-lit-camera eye-x eye-y eye-z target-x target-y target-z` 沿既有 `--project … --scene …` 入口使用，并在打开时按渲染几何的世界范围一次性适配 DDGI：边界外扩至少 .5 米并取整，目标探针间距 2 米，每轴最多 16 个，总数最多 4096，每帧更新约四分之一。相机随后移动不会重新拟合体积；这份视口策略不写入 Scene 文件。原有 `--material-camera` 仍打开 Smooth。

建筑实例使用线性基础色 `(.55,.55,.55)`、粗糙度 `.85`，避免以纯白反射率 1 检查整个建筑的多次反弹。该基础色在 Smooth 中也会乘到原贴图；OBJ、MTL 和 PNG 源文件保持原样。

模型来自 [Morgan McGuire Computer Graphics Archive](https://casual-effects.com/data/) 的 [Crytek Sponza 完整归档](https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip)，作者 Frank Meinl / Crytek，基于 Marko Dabrovic 原版，经 Morgan McGuire 修正。原始声明保留在 [copyright.txt](Content/Sponza/copyright.txt)，逐文件来源与 SHA-256 在 [source.json](Content/Sponza/source.json)。这是一份公开发布的测试资产，权利说明以原始声明为准，不将“DDGI 测试”视作新的授权许可。

原始 OBJ、MTL 与全部 54 张 PNG 未修改；OBJ/PNG 使用 LFS。场景按 0.01 比例转换原模型单位，含 262,267 个三角形、25 个命名材质（其中 24 个有漫反射贴图）。导入后的 103 个连续材质组覆盖整栋建筑、旗帜、植物和装饰；源坐标及空间关系完整保留。**Scene Lit 当前使用实例基础色和粗糙度，支持真实灯光、阴影与 DDGI，但尚不采样这些材质贴图，植物和铁链也按不透明覆盖。** 将 Shading 切到 Smooth/Flat 可查看漫反射贴图及镂空，MatCap 用于观察几何；这些模式不能作为带贴图 DDGI 的验收结果。凹凸贴图未接入。本场景没有附加物理 Body，原物理验收场景继续由 Main.scene 提供。

完整验收：`cmake --build --preset dev --target editor_sponza_acceptance renderer_acceptance`，发布配置替换为 release。图片输出到 `out/<配置>/sponza-evidence/`。Sponza 窗口验收同时覆盖 Smooth/MatCap、正式 Scene Lit、隔离其他效果的 DDGI 开关对照、32 步真实灯位置编辑、停止后的间接光和关灯收敛；`ddgi.txt` 与 `ddgi-motion.csv` 保存图像指标和历史状态。捕获使用 D3D12 GPU validation，不以运行耗时宣称帧率。当前候选结果见 [Renderer 验收记录](../../docs/modules/renderer/acceptance.md)。
