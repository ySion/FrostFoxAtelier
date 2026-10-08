# 2026-10-09 编辑器、资源与渲染阶段合并

## 合并范围

本次候选汇合三个已交付分支，保留各自提交历史：

- `origin/main` 的 `6766689`：Sponza 工程、OBJ/MTL 材质分组、UV、贴图与 Alpha Test。
- `claude/houdini-viewport-ui` 的 `af9d491`：紧凑视口工具区、模式显示、导航、游标、变换手柄与参数控件改良。
- `codex/viewport-rendering` 的 `fbd2cad`：Scene Lit、Reversed-Z、抗锯齿选项、DDGI 与相关质量修正。

后两个分支各自包含同一批前期改动。处理重复添加的文件时，使用其共同内容快照 `f967165` 进行三方比较，保留后续差异，避免以整文件覆盖丢失另一侧功能。

## 合并后的行为与边界

- 视口继续使用左键定位游标、右键选择、W 菜单与 G/R/S 变换；网格吸附、选区及操作历史沿现有文档命令链执行。
- Sponza 的相机初始化与 Houdini 风格视图切换共存。显式设置相机后重置渲染历史；含材质的导入模型使用白色实例乘色，同时保留游标创建位置。
- 基础显示路径保留贴图与 Alpha Test。Scene Lit 使用不透明实例材质，其具体能力以[渲染合同](../modules/renderer/contract.md)与 [Scene Lit](../modules/renderer/scene-lit.md) 为准。
- 新增 UV 后的 GPU 数据读取遵循实际顶点布局，材质深度验收使用统一的 Reversed-Z 约定。
- 正式工程入口、Sponza 验收、视口编辑验收、Scene Lit 验收及无输入快照入口同时保留。

## 本候选验收

开发与发布两种配置已完成编辑器、文档及 UI 验收目标编译。以下结果属于本次合并候选：

- 完整 `atelier_editor_document_acceptance`：两种配置通过，包含对象生命周期、工程持久化、DSL 输入与源码 patch、撤销和视口编辑。
- `atelier_ui_acceptance`：两种配置均通过 23/23。
- `--textured-obj`：两种配置通过，实际 Sponza 工程读取到 262267 个三角形、103 个部分和 24 个带贴图材质。
- 无参数 `atelier_renderer_acceptance`：两种配置通过，覆盖材质分组、UV、Alpha Test、反向深度、MatCap、线框与资源生命周期。首次验收发现测试几何与相机投影重复反转深度；恢复几何位置后通过，原颜色和深度阈值保持不变。
- `atelier_renderer_acceptance --scene-lit-quality`：两种配置通过，覆盖 GBuffer／射线读取、四种抗锯齿、光照稳定性、DDGI、Local GI 和深度。

日志与捕获位于集成工作树的 `out/stage-sync-20261009/`。本轮不把既有分支的历史前台验收结果记为本候选的新结果。
