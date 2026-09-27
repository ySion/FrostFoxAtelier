# RHI 实现回交

## 当前交付

本 worktree 采用 gfx 为 RHI 主体；现行架构见 [README](README.md) 与 [contract](contract.md)。旧 Device/Recording/Ticket 草案不再是下一步实现任务。正式代码、消费者、依赖锁、CMake 集成和可重放补丁均已落在 worktree；没有自动提交、合并或推送。

后续[2026-09-27 审计](audit-2026-09-27.md)确认的两项 P1、七项 P2 已修正。dev/release 的新增 GPU 回归、既有 RHI/Runtime/Business/JIT 验收与隔离生命周期观察均通过，结果集中于 [acceptance](acceptance.md)。当前可继续模块集成。

### 文件与 target

- `src/rhi/rhi.hpp`、`rhi.cpp`：硬件/验证门禁、execution owner、move-only Pipeline、prepare/frame/finish/close。
- `acceptance/rhi/main.cpp`、`resource_cases.cpp`、`shaders.hpp`、`support.hpp`：真实六帧三类管线、间接大常量、连续 finish、range 上传/回读，以及 26 项准备拒绝场景。
- `acceptance/rhi/upstream_regression.cpp`：null-list、Mesh PS 状态、fence gate 超时及严格一次执行。
- `third_party/gfx/atelier.patch`：固定上游修改，配置时按 pristine 源码重放。
- `cmake/rhi.cmake`、`run-rhi-acceptance.cmake.in`、依赖锁、根 CMake：`atelier_rhi`、`atelier_rhi_acceptance`、`atelier_gfx_regression`、`rhi_acceptance`，根 `acceptance` 纳入 RHI。
- 根 README 与本目录文档已切换到实际实现。

## 重现与证据

在 worktree 根目录执行 `. ./tools/enter-dev.ps1`，配置所需 preset 后运行 `cmake --build --preset dev --target rhi_acceptance` 与 release 同名 target。根 `acceptance` 同时覆盖既有 Runtime/Business/JIT。

环境、逐场景结果、日志路径和明确未覆盖项集中记录于 [acceptance](acceptance.md)，不在交接里复制第二份版本表或矩阵。完整工具链与第三方 hash 分别由 `cmake/toolchain-lock.json`、`dependencies-lock.json` 维护。构建输出 `licenses/rhi` 保留各依赖 license/notice 和本项目补丁。

## Main 集成注意

1. 本 worktree 的根 CMake/锁文件变更是 RHI 集成产物；合入主工作区时由 Main 串行解决与其他模块的构建修改，不覆盖对方未提交工作。
2. 首次配置会拉取固定依赖。保留同目录 DXC、Agility、PIX、mimalloc 运行库，不能只拷贝 exe。
3. 借用 gfx 的范围是普通资源、参数与录制。frame/finish/close 与管线替换统一由 owner 驱动；没有 Publisher、通用资源镜像或第二套 barrier 账。
4. 超时是执行域失败终态，只能清理/close，不支持恢复普通录制。close 不能排空就保留 owner；最终析构仍无法安全清理则 fail-fast。
5. null-list 负向回归刻意留一个原始 Program/Kernel 给 context 收尾，上游会打印两个句柄未显式释放的诊断。这不同于主消费者生命周期，也不能当作所有原生对象计数归零的证明。
6. 本次没有执行真实 device removal、TDR 修改、driver 返回值替换或挂死 shader。Present/resize、并发录制、多 queue、业务绑定与性能结论不属于本次 PASS。

后续按真实业务需求选择增量；不要把 gfx 已包含的可选能力列表直接当作已验证功能，也不要恢复已经替代的占位方案。
