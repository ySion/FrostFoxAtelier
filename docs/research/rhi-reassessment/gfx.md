# gfx 固定上游源码与实施依据

历史来源记录：2026-09-27 实施前核对的上游 `gboisse/gfx@e039b255efb327994d30b187caee400ca60110e0`，以下缺陷与行为指未加项目补丁的版本。当前修正与选择见[模块决策](../../modules/rhi/decisions.md)，正式行为和运行结果分别见[契约](../../modules/rhi/contract.md)、[验收](../../modules/rhi/acceptance.md)。本文保留来源，不再提供下一阶段计划。

## 上游接口与责任

| 项目 | 固定源码观察 | 本项目采用时的处理 |
| --- | --- | --- |
| 三类管线 | Graphics、Mesh、Compute kernel 及对应直接/间接命令已有真实原生路径；ProgramDesc 包含 AS/MS | 正式消费者验证三类管线，硬件能力由 Context 实际筛选 |
| 参数与绑定 | Program/Kernel、DXC 反射、参数、descriptor 和常量池组成已有机制 | 沿用其权威，不增加同形绑定和状态账；映射上传内存仍需遵守使用期 |
| 管线准备 | 某些编译/反射/PSO 失败可能留下有效 kernel，编码时跳过而未按本项目要求报告失败 | 补丁贯通阶段错误；宿主检查完整准备；候选失败保留旧 Pipeline |
| Mesh PS 状态 | 上游可见性判断只将 Graphics+PS 计入 pixel 读取 | 补丁覆盖 Mesh+PS；独立场景令 Mesh 在 Vertex 之前消费 Compute 结果 |
| owned 执行域 | 管理 DIRECT queue、帧槽 allocator/fence；复用槽位前等待并推进常量和资源退休 | 保留单 owner 模型并补有界等待、返回值检查与错误终态 |
| interop | device 被 AddRef，但不管理宿主持久 queue/list；导入资源退出可能录制初始状态恢复 | 普通消费者不使用该执行模式，诊断回归的使用不升级为业务契约 |
| 原始错误路径 | frame/sync 存在无限等待和未检查 Close/Signal/Reset 的路径 | 显式保存提交事实；清理只排空已提交工作，不重复 Execute |

对应固定来源：[gfx.h](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.h)、[gfx.cpp](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.cpp)、[构建文件](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/CMakeLists.txt)。以上是定向源码事实，不代表完整上游审计。

## 工业消费者：AMD Capsaicin

固定 `GPUOpen-LibrariesAndSDKs/Capsaicin@914b91596cd119eda85fbc1d3c7ee6ac391b1452` 的消费链提供了执行域归属参照：

- `CapsaicinMain::initialiseGfx` 创建 context，再借给核心 renderer。
- `Main::renderFrame` 依次组织 renderer、UI，再由应用调用 `gfxFrame`。
- 退出按 ImGui、renderer Terminate、gfx context、window 的顺序执行；renderer 不夺取 context 所有权。
- `Internal::terminate` 在有效 context 上先 `gfxFinish`，再释放消费者资源。

固定源码：[main_shared.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/scene_viewer/main_shared.cpp)、[capsaicin_internal.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/capsaicin/capsaicin_internal.cpp)。该实例支持清楚的宿主/消费者所有权，不证明任意并发、外部队列或其他版本无需适配。

## 版本、许可与构建证据

- gfx 自身定位快速原型，固定版本根许可证为 MIT；这既不是性能证明，也不是淘汰条件。[README](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/README.md)、[LICENSE](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/LICENSE.md)
- OMM 专题 `ea6fb48e35ac5b41cff8670fc9cc16ae8c735a26` 添加 OMM/几何/BLAS 路径，不构成当时三类管线首增量的前置条件；项目没有随之迁入。
- 最早的上游最小静态库构建只证明工具链可编译。当前正式构建已改用[依赖锁](../../../cmake/dependencies-lock.json)中的固定归档和 hash，避免旧的临时子模块配置路径。
- 网页清洗源码可能丢失 C++ 模板标记，只用于研究定位；构建始终使用可校验的源码归档。模块编译和 DXC、Agility、D3D12MA、PIX 部署由[模块构建](../../../cmake/rhi.cmake)承担。
