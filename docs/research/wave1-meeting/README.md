# Wave 1 历史审查与实验索引

历史快照：本目录保留第一次并行审查时的源码依据、反方意见与本机实验结果。输入以 [baseline.json](baseline.json) 的文件哈希为准；正文中的“当前”“未实现”、旧文件名和行号均指当轮，不是现行合同或待办。

当时的 A-F1/C-D1 随后已修复，证据见 `out/review/wave1-fixes/verification.json`；之后的共同构建验收见[首次三线集成](../wave1-integration.md)。ABI 结果诊断与 async 绑定问题的后续关闭见[运行时阶段记录](../runtime-stage-closeout.md#已关闭的历史缺陷)。

现行能力从[项目 README](../../../README.md)、[运行时结构](../../runtime-structure.md)、[布局模块](../../modules/ui-layout/README.md)和[RHI 模块](../../modules/rhi/README.md)进入。本目录不再承担下一任务交接或实施调度。

## 历史证据

- [当轮裁决](decisions.md)：复现问题、反证和取舍。
- [实验结果](evidence.md)：构建与运行范围、原始数据、未覆盖项。旧探针仅与各自快照配套重放。
- [输入基线](baseline.json)及`out/review/wave1-meeting/validation.json`。

| 主题 | 探索 | 反方 |
| --- | --- | --- |
| 类型与生命周期 | [types](types/explore.md) | [challenge](types/challenge.md) |
| 根与 GC | [gc](gc/explore.md) | [challenge](gc/challenge.md) |
| 源码与诊断 | [source](source/explore.md) | [challenge](source/challenge.md) |
| JIT 与资源 | [execution](execution/explore.md) | [challenge](execution/challenge.md) |
| 发布与退休 | [reload](reload/explore.md) | [challenge](reload/challenge.md) |

当轮支线依据：[依赖组合](parallel-modules/map.md)、[RHI](parallel-modules/rhi.md)、[布局](parallel-modules/ui-layout.md)。这些报告保留采用前的比较和质询，模块的当前生产合同由上方模块入口维护。
