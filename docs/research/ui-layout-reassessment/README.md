# 布局研究与选型证据

当前生产实现、合同与验收从[布局模块](../../modules/ui-layout/README.md)进入。本目录只保留必要的历史实测和一手来源，不作为待实施清单或另一套生产规范。

## 实测与来源

- [三候选比较](validation-results.md)：Yoga/Taffy/Clay真实DirectWrite、行为失败、成本和栈边界；包含失败结果，不代表当前生产验收。
- [原生算法依据](../../modules/ui-layout/decisions.md#算法依据)：当前 Flex/Grid 设计决定及其来源。
- [固定实验来源](sources.json)：保留实际候选commit、下载URL、SHA-256和实验Rust版本；不是生产依赖锁。
- [工业游戏UI](industry-game-ui.md)、[工业编辑器UI](industry-editor-ui.md)：容器、失效、虚拟化与资源责任的一手依据。
- [Yoga](yoga.md)、[Taffy](taffy.md)、[Clay](clay.md)、[其他原生框架](native-alternatives.md)：固定版本机制与采用成本，明确标记历史。

## 复现与许可

三候选比较的源码、runner与Cargo锁可从Git提交`c644ba5692fe2d67d1dda93953e227a50ba64f9f`取得；它们不在生产构建中。正式消费者是`acceptance/ui_layout`，当前覆盖见[验收入口](../../modules/ui-layout/acceptance.md)。

历史实验使用的Yoga和Taffy为MIT，Clay为zlib/libpng；准确pin见sources.json，各来源报告保留官方许可证链接。没有把第三方solver源码复制进生产布局核心。out中的第三方原始公告与日志未删除，但out不随源码提交；研究摘要不替代未来重新分发时的许可核对。
