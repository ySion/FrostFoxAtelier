# SceneLit 应用集成 — 2026-10-08

工作目录：`E:/FrostFoxAtelier`，基础提交 `402fab075bb2aa6605b51fdb767387ebd0e08869`。本波次叠加已有视口与 UI 未提交改动，没有提交、推送、主线同步或前台输入操作。

## 交付边界

- Renderer 的新头来自 `C:/Users/Arzuo/.codex/worktrees/viewport-rendering/FrostFoxAtelier`，World 的新头来自 `C:/Users/Arzuo/.codex/worktrees/actor-world/FrostFoxAtelier`。本波次不修改这两个模块的实现。
- 应用负责正式菜单命令、三种灯的单事务创建、通用 Actor Structure/Parameters、场景序列化和 World→Renderer 快照。
- 既有视口叠层、正交网格和共享吸附步长已单独导出 `out/scenelit-application-20261008/viewport-renderer.patch`；该补丁以基础提交为比较点，不包含本波次 SceneLit 应用代码。

## 数据与生命周期

LightComponent.settings 是唯一灯参数源。强度单位为 point cd、rectangle cd/m²、directional lux；灯的位置和正交方向来自 World 提取，不在应用重新计算层级或维护灯索引。材质保留基础色并传递 roughness、metallic、HDR emissive。

SceneSnapshot 拥有提交期间需要的 CPU 数组和不可变网格引用；Renderer 借用只持续到 render 返回。组件身份映射不保活业务对象，按完整 ObjectId 比较并单调分配非零渲染键，未呈现项移出映射。文档替换改变 scene_identity；连续位姿不触发全场景历史重置，成员、资源、材质和灯参数改变递增 revision。

预算是同时追踪阴影的灯数，菜单允许 4/8/16；超额灯仍属于真实场景，状态栏从 accepted FrameStats 显示实际阴影数和请求数。效果勾选、曝光和预算均走原有显示设置入口。

## 验收入口与状态

`atelier_editor_document_acceptance --scene-lit` 使用真实 World、Project、Runtime、Session、DSL 和快照链，覆盖三类灯创建/撤销、反射字段与单位、组件移除恢复、材质及灯持久化、非法值拒绝、旧 v2 默认值、稳定映射、修订规则和菜单幂等。完整文档验收也调用它；v1→v2 的现有验收已更新新增材质字段的预期值。

使用实际 Dev 编译参数与双方正式合同头，session.cpp、scene.cpp、workspace.cpp、application.cpp、project.cpp 和文档验收入口六个翻译单元均通过 Clang `/Zs` 编译检查，日志位于 `out/scenelit-application-20261008/*.syntax.log`。临时头放在该交付目录的 include 下，没有替换 E 盘仓库的 Renderer/World 文件。

当前还未完成与新 World/Renderer 二进制的集成运行验收，E 盘源码需要与这两者的新增实现一起构建。GPU、真实编辑器窗口和超预算照明效果由渲染负责人统一构建、排期验收，不能用旧视口截图或后台语法编译代替。
