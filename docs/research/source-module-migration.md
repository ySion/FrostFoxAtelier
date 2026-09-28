# 源码执行模块正式化

日期：2026-09-28。目标：消除完整源码执行链只能由验收程序复用的技术债，并把机器快照从通用帧和生成槽布局中移出。

## 实施方案

- 正式宿主库 `atelier_source`，公共入口 `source/program.hpp` 中的 `atelier::source::Program`。沿用既有 request → prepare → try_publish 以及 entry / bind / pin → invoke / start 的拥有链，公共参数与结果使用 Runtime Scalar。
- Binding、Invocation、目录映射、原生桥接和验证接缝置于内部实现。公共入口不提供机器快照、统计、故障注入或私有 DLL 标量类型。
- LLVM 源码后端迁入 `src/jit`，以独立 DLL `atelier_source_backend` 保持 SDK 的静态 CRT 隔离。平台探针与源码后端共用现有 Windows 展开实现和 LLVM 修复对象。
- 验收继续使用正式程序对象与实际编译器；raw ABI 防御和故障注入通过内部接缝验证同一实现，不复制一套宿主或编译器。
- 机器快照由验收消费者的 ObjectRoot 持有；生成帧仅预留 receiver 槽。保留创建、轮询刷新、成功移交与失败释放的可观察顺序。

## 迁移不变量

1. Runtime 类型、对象身份和 GC 仍只有原有体系；公共程序接口不新增通用方法注册或业务调度。
2. 同步与协程的准入、首因错误、结果提交、显式清理和取消规则不变。
3. 活动调用先固定版本；回调销毁外层拥有者后，执行不访问失效外壳。
4. 协程先销毁旧版本生成帧，再释放调用存储和版本；旧代码与展开表一起保活、一起退休。
5. 发布的失败检查和分配先于候选代码转移；忙时保留候选，过期请求不能覆盖当前版本。
6. 当前保持单线程；快照准备不被解释成后台编译与关闭可并发。

## 验收终点

保留并迁移现有同步、协程、原生/描述型对象、快照、GC、真实坏链接、原始 ABI、回调销毁拥有者及 Windows 代码退休场景。增加仅包含正式公共头并链接正式库的最小消费者，防止对验收私有定义的隐式依赖。运行 dev/release 的受影响端到端目标；构建组织改变后核对原总验收入口。

任务范围不包括 await、任务树、并发调度、对象池、布局迁移、MLIR 或性能基准工程。

## 实施结果

上述模块与构建迁移已完成。旧 `acceptance/jit/business_backend.cpp/.hpp` 已移除；`business.cpp` 不再定义 Binding、Invocation、Contract 或程序拥有者，只消费正式实现及验收辅助。原有 50 个 `verify_*` 场景全部保留，机器快照和统计集中于 `source_fixture.inl`。

三位实施专家分别负责宿主、后端/构建及消费者迁移，另一位独立专家复核生命周期和调用契约。交叉复核发现并修复了一项验收适配差异：finish 已移交快照后，若后续生成清理报告失败，必须在执行清理阶段释放外部快照，并维持原先“清理失败、失败输出、宿主首因”的诊断优先级。现有真实源码矩阵已加入该尾部失败场景。

公共最小消费者只包含 `source/program.hpp`，验证同步标量结果、void、绑定、兼容替换与固定租约、执行种类不兼容拒绝、冷启动、参数复制，以及程序关闭后挂起帧继续完成。另覆盖公共 Scalar 转换的负零/NaN payload 与超过八参时的扩展缓冲。

### 验收证据

| 配置与范围 | 结果 | 边界 |
| --- | --- | --- |
| dev 总 `acceptance` | PASS，退出 0 | 迁移主体完成后的完整 Runtime、源码、JIT、RHI 和 UI 验收 |
| dev 最终 `jit_acceptance` | PASS，退出 0 | 在上述总验收后，针对新增的尾部清理失败和公共浮点转换场景重新运行受影响目标 |
| release 总 `acceptance` | PASS，退出 0 | 包含最终新增场景及所有默认配置消费者 |
| release `ATELIER_BUILD_JIT_ACCEPTANCE=OFF` 的 `source_acceptance` | PASS，退出 0 | 使用正式库完成公共入口调用；此轮早于新增浮点验收，但相同生产实现未再变化 |
| 正式构建的 DLL 导出与 EXE 导入 | PASS | 后端不导出两个故障注入函数；公共消费者不导入 `atelier_jit_probe` 或 `atelier_jit_native_client` |

日志、文件哈希及配置边界见 [verification.json](../../out/review/source-module-migration/verification.json)。release 的构建选项已恢复 ON，默认验收入口可用；没有建立额外 preset、提交、合并或推送。

### 成本与剩余范围

- 公共 Scalar 参数最多八项使用栈缓冲，更多参数才分配；浮点按原始位转换。
- Candidate 的内部存储与 Request 的独立 shared_ptr 控制块增加冷端分配。它们隐藏代码拥有者和不可变请求内部结构；本轮未测量性能，不宣称迁移带来提速。
- Runtime 关停仍由宿主先取消/释放帧，再释放 Runtime 和所需后端；当前没有新增任务树或全局协程登记。
- 资源规模测量、源码调试、一般方法分派与 await 等剩余范围仍见[结构与债务清单](../runtime-structure.md#技术债与处理时机)。

后续[阶段收尾](runtime-stage-closeout.md)增加了 `Program::close()`、两阶段宿主关停消费者与正式公共接口的资源测量。上表保留迁移当轮的验收范围；后续证据单独记录，不以新结果覆盖旧日志。
