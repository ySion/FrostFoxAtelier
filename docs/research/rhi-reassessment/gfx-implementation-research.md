# gfx 实施研究：决定性证据

固定基线：`gboisse/gfx@e039b255efb327994d30b187caee400ca60110e0`。本轮已用项目锁定 clang-cl/C++23 配置并编译上游最小 gfx 静态库；这不是 GPU 行为验收。

## Shader 与绑定

绑定研究确认：Program/Kernel、DXC 反射、名字参数和 descriptor/常量池已构成有深度的 Interface，无须再造同形体系。参数修改后的编码会更新 descriptor，旧槽与常量区依照帧进度保护；这不授权任意覆盖映射中的上传内存。

准备阶段不能只检查 handle：文件 shader 编译或 PSO 创建失败可能留下有效 kernel，但 root signature/PSO 为空；编码会跳过并返回 NoError。引擎入口需要真实 Ready/Failed 门禁。上游 reload 先销毁旧内容再重建，不能把它当事务热替换。

确定的修正候选：`GetShaderVisibleResourceState` 仅对 Graphics+PS 返回 ALL_SHADER_RESOURCE，Mesh+PS 会返回 NON_PIXEL_SHADER_RESOURCE。应使用 Compute 写入、Mesh 的 PS 读取的实际负载验证，且 Mesh 先于 Vertex 绘制，避免后者转换掩盖问题。

源码：[gfx.cpp](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.cpp)、[gfx.h](https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.h)。

## 执行与寿命

主代理定向核对：owned context 创建 DIRECT queue、frame allocator/fence；`frame` 等待将复用的槽位后推进常量与垃圾回收。`gfxDestroyContext` 在非 interop 且 device 有效时先 `finish`，不是直接无条件释放。

interop initialize 借用并 AddRef device，但只为时间戳频率临时创建 queue 后立即释放，不替宿主管理持久 queue/list。导入资源销毁还可能在当前 command list 录制恢复 initial state 的 barrier。采用 interop 会把真实责任带回宿主，不能假装仅多几个 getter 就获得独立执行模型。

`frame`/`sync` 的等待是无限等待；Close/Signal/Reset 等结果存在未检查路径。任何修正必须保证提交后尚未完成的资源不会被提前退休，且失败收尾不能把已经提交的 command list 再提交一次。

## 工业消费者：AMD Capsaicin

固定 `GPUOpen-LibrariesAndSDKs/Capsaicin@914b91596cd119eda85fbc1d3c7ee6ac391b1452`：

- `CapsaicinMain::initialiseGfx` 创建 context，再借给核心 renderer。
- `Main::renderFrame` 先调用 renderer、再 UI，最后由应用唯一地调用 `gfxFrame`。
- 退出顺序为 ImGui、renderer Terminate、gfx context、window；renderer 只清理其资源，不夺取 context 所有权。
- `Internal::terminate` 在 context 有效时先 `gfxFinish` 再销毁消费者资源。

这支持把执行域和底层资源管理继续交给 gfx、业务层只组织工作；不证明不同版本无需适配，也不证明任意并发或外部队列安全。

源码：[main_shared.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/scene_viewer/main_shared.cpp)、[capsaicin_internal.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/capsaicin/capsaicin_internal.cpp)。

## 版本与构建

OMM 专题 `ea6fb48e35ac5b41cff8670fc9cc16ae8c735a26` 的实际 patch 只改 gfx.h/cpp，添加 OMM/几何/BLAS 路径，不改变本轮 shader/绑定/三管线基线；当前需求不要求迁入。

最小构建已取得真实 DXC、Agility、D3D12MA、PIX 依赖。首次配置遇到 Git 子模块脚本 stdin 问题，使用已经取得且固定版本的 D3D12MA 源码完成基线配置；正式集成将使用带 hash 的归档，避免依赖不必要的子模块调用。上游基线构建完成，尚未据此宣称 profile 或 GPU 输出通过。

## 进入对抗评审的提案

优先保留 gfx 为资源/绑定/状态/退休的单一权威。自有 Module 只承担真实新增不变量：硬件与调试门禁、shader/PSO Ready、事务式发布、明确的帧所有者与失败收尾。拒绝逐函数转发与第二套 descriptor/状态图。是否修正等待、如何处理提交后失败，由对抗评审审查后冻结；不将透明设备恢复或多队列临时变成硬门槛。
