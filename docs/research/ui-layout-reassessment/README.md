# 布局研究与历史证据

当前生产实现、合同与验收从[布局模块](../../modules/ui-layout/README.md)进入。本目录只保留必要的历史实测和一手来源，不作为待实施清单或另一套生产规范。

## 实测与来源

- [三候选比较](validation-results.md)：Yoga/Taffy/Clay真实DirectWrite、行为失败、成本和栈边界；包含失败结果，不代表当前生产验收。
- [原生首步实验](native-step1.md)：整池分配、宽度依赖测量和失败提交的反例，相关能力已进入正式消费者。
- [原生历史性能](native-performance-history.md)：24/27/29/35项版本的工作负载与数据；后续39/41项版本的比较见[审计记录](../../modules/ui-layout/audit.md)。
- [原生算法依据](../../modules/ui-layout/decisions.md#算法依据)：Flex/Grid 原始资料核对中仍有效的依据已并入设计决定，已完成建议不再保留独立待办文档。
- [固定实验来源](sources.json)：保留实际候选commit、下载URL、SHA-256和实验Rust版本；不是生产依赖锁。
- [工业游戏UI](industry-game-ui.md)、[工业编辑器UI](industry-editor-ui.md)：容器、失效、虚拟化与资源责任的一手依据。
- [Yoga](yoga.md)、[Taffy](taffy.md)、[Clay](clay.md)、[其他原生框架](native-alternatives.md)：固定版本机制与采用成本，明确标记历史。

## 退役范围与许可

acceptance/ui_layout_probe和ui_layout_step1_probe的适配器、第二套原生kernel、Cargo锁与独立构建/下载runner均已移除。需要重现旧实验时，从Git提交`c644ba5692fe2d67d1dda93953e227a50ba64f9f`恢复；不将它们重新接入生产目标。正式消费者是acceptance/ui_layout。

已实施的Flex/Grid及实用增量计划、旧Yoga合同/交接、比较探针计划和中间评审草稿已删除；有效规则已进入现行合同和设计决定。结果摘要及上游来源继续保留。

历史实验使用的Yoga和Taffy为MIT，Clay为zlib/libpng；准确pin见sources.json，各来源报告保留官方许可证链接。没有把第三方solver源码复制进生产布局核心。out中的第三方原始公告与日志未删除，但out不随源码提交；研究摘要不替代未来重新分发时的许可核对。
