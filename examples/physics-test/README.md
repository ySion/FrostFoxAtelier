# Physics Test 工程

使用 `atelier_editor --project <仓库>/examples/physics-test` 打开；也可在编辑器中选择 File → Open Project Folder 并指向本目录。启动场景为 `Content/Scenes/Main.scene`，Scene Tree 包含静态三角网格地面、动态箱/球、普通 Actor 的胶囊 Body、Sensor、运动学平台和力/冲量目标。选择 Actor 后在 Parameters → Components 查看 Body 配置。

当前编辑器只负责场景与 Body 配置的查看、修改和保存，尚无物理播放、暂停、碰撞调试绘制或运行时力/冲量控件。打开工程时物体保持作者位置；拖动编辑器视角不会推进模拟。运行 `cmake --build --preset dev --target editor_physics_project_acceptance` 可加载本工程，启动真实 World/Jolt 并验证重力、力、冲量、运动学目标和 Sensor 接触；发布配置将 `dev` 换成 `release`。独立场景 `physics_acceptance` 覆盖查询、过滤、Cook、生命周期及角色、约束、布娃娃、车辆等 P4 系统；P4 配置目前不能存入编辑器场景格式。

工程场景仅引用内置网格，无需导入 OBJ 或生成 Cook 缓存。`Cache/` 可随时重建，不是场景源数据。

`Content/Synty/AnimationBaseLocomotion/Animations/` 保存从 Unity 工程 `Assets/Synty/AnimationBaseLocomotion/Animations/` 复制的 692 个原始 FBX，保留 Polygon/Sidekick 及动作分类目录。Unity 的 `.meta` 和 `.controller` 未导入，也不会删除源工程文件。FBX 由 Git LFS 跟踪，克隆后需安装 Git LFS 并执行 `git lfs pull` 取得原文件。当前 Asset Browser 将 FBX 列为普通文件，尚不能解析或播放骨骼动画，场景也不引用这些动画。
