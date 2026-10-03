# UI 布局核心

状态：**原生布局基建已完成本阶段审计与优化，可以收尾。** 支持基准尺寸与伸缩、换行/分布/首基线，以及二维轨道、跨行跨列和自动放置；既有稳定身份、共享列、重复格、虚拟列表、缓存和事务发布保持。开发/发布覆盖41个场景，生产UI消费者两配置各15/15通过，见[验收记录](acceptance.md)与[交叉审计和性能](audit.md)。这是原生UI语义，CSS兼容、循环依赖与基线边界见[Flex/Grid合同](flex-grid.md)；正式控件与文字见[UI消费模块](../ui/README.md)，真实窗口与绘制接线见[编辑器](../editor/README.md)。

## 当前入口

| 目的 | 权威 |
|---|---|
| 使用公开C++ Interface | [`src/ui_layout/layout.hpp`](../../../src/ui_layout/layout.hpp) |
| 了解尺寸、身份、编辑、错误与结果寿命 | [生产合同](contract.md) |
| 查看正式目标、复现命令、覆盖和未测项 | [验收记录](acceptance.md) |
| 了解当前设计取舍 | [决策](decisions.md) |
| 接入共享列、物品格、版本和错误处理 | [调用示例](usage.md) |
| 使用原生Flex、二维Grid、基线和统一对齐 | [Flex/Grid合同与示例](flex-grid.md) |
| 查看本轮修复、交叉二审和同场景性能 | [审计与优化](audit.md) |
| 管理逻辑行高、可见范围与滚动锚点 | [VirtualList契约](virtual-list.md) |
| 接入内容绑定与完整呈现帧 | [完整内容消费](composition.md) |
| 回看全量Inspector/物品格的成本 | [历史性能证据](../../research/ui-layout-reassessment/native-performance-history.md) |

生产命名空间是 `atelier::ui_layout`，静态目标是 `atelier_ui_layout`。公开包含路径为 `ui_layout/layout.hpp`；核心不链接 Runtime、LLVM/JIT、RHI 或文字库。根CMake复用既有工具链和分配器规则，不另建求解器后端工厂。

## 已实现的最小消费链

1. 创建带明确容量/深度限制的 `LayoutDocument`，传入同步叶测量函数与非拥有context。
2. 通过稳定局部 `LayoutNodeId` 创建、更新、移动、移除节点；编辑和旧发布相互独立。
3. 给根确定viewport和provider版本，计算完整未舍入几何；失败不提交半结果或本次新测量缓存。
4. 默认借用带版本的 `LayoutView`，通过 `find(node)` 常数时间查询该次发布；跨成功发布/析构消费时显式复制 `LayoutSnapshot`，它也支持相同查询。

核心有序树不是第二套业务对象世界。Document identity＋slot/generation只服务布局资源；内容键不保活业务对象或文字资源。原生内存异常正常传播，测量期间禁止重入编辑及销毁拥有者。

## 责任划分

| 拥有者 | 负责 | 不负责 |
|---|---|---|
| 布局库 | 节点/结构、尺寸规则、容器分配、测量缓存、完整逻辑几何与版本 | 场景业务对象、主题、事件、字体shaping、renderer |
| 叶测量 Adapter | 实际内容、字体/排版对象、内容与provider版本、可复用度量 | 修改正在求解的布局树、隐式保留旧绘制版本 |
| 集合视图 | 逻辑集合、业务key投影、可见实例、宽度政策、滚动锚点和extent | 把集合扫描成本隐藏成核心增量收益 |
| UI composition / 绑定拥有者 | 业务状态投影、几何/文字消费期、像素政策、绘制包与Runtime根 | 让布局节点代替业务对象或GPU资源寿命 |

当前支持Fixed/Content/正权重Expand及min/max、padding/gap、槽内填满/对齐、覆盖与父内容矩形锚定。`table` 为各行统一列宽，`wrap` 按自然宽度分行，`grid()` 在同一换行算法中使用自适应等宽槽。折叠保留节点身份但退出几何和测量。真实文字按最终宽度取得自然高度，不用viewport高度压缩自然需求。

编辑可批量合并到下一次计算，稳定叶度量可命中缓存。内容变化若未参与上次几何度量，计算可复用几何并发布新版本；尺寸、结构、provider或viewport变化仍求解参与布局的实例树。折叠子树不再承担逐次工作数组初始化；节点描述仍保留。VirtualList让树只保留可见窗口，普通滚动可只改变composition偏移。不承诺通用局部调度、全链路零分配或硬帧预算。

## 构建与验收

在仓库根目录准备正式环境后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target ui_layout_acceptance
cmake --preset release
cmake --build --preset release --target ui_layout_acceptance
```

该目标依次运行无文字依赖的 `atelier_ui_layout_minimal` 和真实DirectWrite的 `atelier_ui_layout_acceptance`；总体 `acceptance` 也包含它们。布局目标只验证CPU布局消费；窗口/渲染/输入由编辑器端到端验收覆盖，非Windows构建仍未经验证。

## 研究与历史

- [首步原生实验](../../research/ui-layout-reassessment/native-step1.md)：12项隔离实验、极端权重相消修复与本次合同收敛来源。
- [三候选真实验证](../../research/ui-layout-reassessment/validation-results.md)：历史比较路径、性能分项、反例及覆盖限制。
- [工业游戏UI](../../research/ui-layout-reassessment/industry-game-ui.md) / [工业编辑器UI](../../research/ui-layout-reassessment/industry-editor-ui.md)：通过Exa调查的机制依据。
- [研究总入口](../../research/ui-layout-reassessment/README.md)：来源和历史资料，不是当前生产依赖。
- [本次Flex/Grid原始资料核对](research.md)：规范和工业实现的算法依据、性能取舍与保留边界。
- 已退役的候选与原生首步探针可从Git历史恢复；源码、独立runner和已失效方案不再留在当前树中。固定来源、许可与必要实测摘要保存在研究目录。

正式代码位于 `src/ui_layout/`，消费者位于 `acceptance/ui_layout/`。当前树只保留这一套正式布局实现和验收入口；共享README与跨模块验收由集成负责人协调。
