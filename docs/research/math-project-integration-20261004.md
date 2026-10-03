# 数学基础、World、工程与布局集成

日期：2026-10-04。隔离分支 `codex/math-project-sync` 的后台集成验收已通过；普通窗口与实际 GPU 提交复验仍待执行。本记录不把各负责人的历史窗口验收视为组合版本的结果。

## 提交来源

从应用冻结提交开始，依次正常合并四位负责人的交付，保留原始提交的祖先关系。

| 来源 | 冻结提交 |
| --- | --- |
| 应用／工程 | `1fa1a6eaf53af9b4234bc13592b90f516fbd03ef` |
| World | `e9370d36b8eae1d58201b0c4559e3353ef066d47` |
| RHI／renderer | `161b4cb8fc0d7150afc38e06c0911b168585014e` |
| Runtime／数学 | `f1c3cde859b0606897c7a8cc20b99e174ac3d4ca` |
| UI 布局 | `5d5336b430b20af88724c89cf949061bb653e280` |

本轮实际编译和执行的代码提交为 `a39bf1ba23858361c793bf6a9e0d9ee47542e9af`；其后的交付记录只修改文档。四个合入提交的 `git merge-base --is-ancestor` 均返回 0。

## 冲突与合同

- 保留当前 Actor／SceneComponent／StaticMeshActor 模型、有序生命周期、显式空间根和父子关系；旧分支携带的 Transform、Cube 和示例业务接线没有恢复。
- 保留空场景启动、共享编辑器命令、Asset Browser、文件夹工程，以及 `mesh_source`、`set_mesh_binding`、`read_static_mesh` 和 `core.String` 资源来源合同。
- World 和编辑器改用 Runtime 统一的 `math::Vector3`、`math::Quaternion`、`math::Matrix4`，类型记录改为 `Types::vector3`，没有旧名称别名。World 继续负责有限值、正缩放、层级矩阵溢出和不可表示剪切的拒绝。
- renderer 公共边界仍接收矩阵数值数组；应用显式提交 `Matrix4::elements`，网格、材质和选择状态保持原路径。按需场景／深度资源和弹出窗口资源复用保留。
- Scene v1 保存 xyz／xyzw 数组及 Actor／Component 名称，不保存数学类型名、Runtime ID 或原生字节布局。float 精度、分量顺序、局部 TRS 和行向量语义不变，因此无需升版。固定 v1 样例经过真实文件读写、对象和资源恢复、父子矩阵计算及再次保存重开，确认语义保持一致。详见[工程格式](../modules/editor/projects.md)。
- UI 合入数值边界与首基线能力拒绝修复、Grid 跨度贡献查询和正增量批次提交；其独立证据归属 UI 冻结提交，见[布局审计](../modules/ui-layout/audit.md)。

## 本轮验收

使用正式工具链和独立的 `out/dev`、`out/release`，没有复用其他目录的构建产物。

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --parallel 8 --target atelier_editor editor_document_acceptance world_acceptance math_acceptance source_acceptance atelier_runtime_acceptance
./out/dev/atelier_runtime_acceptance.exe
# release 使用相同目标与对应的 preset、可执行文件路径。
```

| 项目 | dev | release |
| --- | --- | --- |
| 正式编辑器、application、renderer 编译 | PASS | PASS |
| World 生命周期／空间树／StaticMeshActor | PASS，8/8 | PASS，8/8 |
| 原生数学与源码消费者（含数学、生命周期和更新） | PASS | PASS |
| Runtime 消费者 | PASS | PASS |
| Project／Workspace／编辑器文档链 | PASS | PASS |
| 固定 Scene v1 文件兼容性 | PASS | PASS |

上述构建和运行均退出 0。原始日志位于集成工作树的 `out/math-project-integration/`：`dev-build-20261004-005505.log`、`release-build-20261004-005603.log`、`runtime-dev-20261004-005626.log`、`runtime-release-20261004-005658.log`。工程文件证据位于各配置的 `project-document-evidence/`。

## 验收边界

本轮没有启动普通编辑器窗口、IME 或 renderer 窗口验收。现有 `renderer_acceptance` 会显式显示并恢复真实窗口，尚无保证不激活前台的执行路径；实际 GPU 矩阵／网格提交的组合版本复验待执行，编译和 CPU 矩阵结果不能代替它。

工程窗口的保存确认后重开、取消新建、保存成功但目录刷新失败提示仍待普通窗口验收。此前 Unicode 剪贴板偶发失败没有确定根因，继续保留为未解决观察；历史复跑通过不代表问题已修复。真实用户工程 `E:\ffa_testproj` 未用于本轮验收。

本轮未替换 JSON 库；[候选与 UE5 调研](../modules/editor/json-libraries.md)只记录来源和证据边界，没有新的性能收益结论。
