# UI 布局核心

`atelier_ui_layout` 是正式原生布局核心。支持稳定节点身份、共享列、重复格、Flex 伸缩/换行/分布/首基线、二维 Grid 轨道/跨格/自动放置，以及逻辑列表的可见范围和滚动锚点。当前布局消费者只保留 2 个代表，覆盖与独立真值见[验收记录](acceptance.md)。

尺寸语义、CSS 兼容边界和不支持的循环依赖见[Flex/Grid 合同](flex-grid.md)。正式控件、文字与输入由[UI 模块](../ui/README.md)提供；声明式作者层和真实窗口消费见[编辑器](../editor/README.md)。布局验收不替代这些上层模块的验收。

## 阶段状态

原生布局基建阶段已按选定范围完成，正式编辑器已消费同一布局库。后续优先处理真实消费者的成本；完整 CSS 兼容不属于本阶段完成条件。

| 范围 | 当前状态与边界 |
|---|---|
| Flex/Grid 核心 | 已实现上述原生能力，正式验收只保留有界分配与快照寿命代表；未开放的尺寸循环及基线组合见合同 |
| 文字与绘制 | 生产 Session 使用 DirectWrite 完成文字排版与命中，UiFrame 持有对应资源；布局库负责控件几何 |
| 大型集合 | VirtualList 提供行高、范围与锚点；生产 Table 按可见行列绘制。普通 list/tree 仍保留完整控件实例，裁剪不等于实例虚拟化 |
| 更新成本 | 属性更新按脏控件同步；结构变化通过 `set_tree` 一次提交最终父子关系与兄弟顺序，保留节点身份；契约与成本见[UI 更新](../ui/updates.md#失效与实际成本) |

## 当前入口

| 目的 | 权威 |
|---|---|
| 使用公开 C++ 接口 | [`modules/gui/ui_layout/include/ui_layout/layout.hpp`](../../../modules/gui/ui_layout/include/ui_layout/layout.hpp) |
| 了解尺寸、身份、编辑、错误与结果寿命 | [生产合同](contract.md) |
| 查看正式目标、复现命令、覆盖和未测项 | [验收记录](acceptance.md) |
| 了解当前设计取舍 | [决策](decisions.md) |
| 接入共享列、物品格、版本和错误处理 | [调用示例](usage.md) |
| 使用原生Flex、二维Grid、基线和统一对齐 | [Flex/Grid合同与示例](flex-grid.md) |
| 管理逻辑行高、可见范围与滚动锚点 | [VirtualList契约](virtual-list.md) |
| 接入内容绑定与完整呈现帧 | [完整内容消费](composition.md) |

生产命名空间是 `atelier::ui_layout`，静态目标是 `atelier_ui_layout`。公开包含路径为 `ui_layout/layout.hpp`；核心不链接 Runtime、LLVM/JIT、RHI 或文字库。根CMake复用既有工具链和分配器规则，不另建求解器后端工厂。

## 消费链

1. 创建带明确容量/深度限制的 `LayoutDocument`，传入同步叶测量函数与非拥有context。
2. 通过稳定局部 `LayoutNodeId` 创建、更新、移动、移除节点；完整树变更使用 `set_tree` 批量提交，编辑和旧发布相互独立。
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

当前支持Fixed/Content/正权重Expand及min/max、padding/gap、槽内填满/对齐、覆盖与父内容矩形锚定。`table` 为各行统一列宽，`wrap` 按自然宽度分行，`NodeInput::grid(float, ...)` 在同一换行算法中使用自适应等宽槽；`NodeInput::grid(GridSpec)` 创建二维 Grid。折叠保留节点身份但退出几何和测量。真实文字按最终宽度取得自然高度，不用viewport高度压缩自然需求。

编辑可批量合并到下一次计算，稳定叶度量可命中缓存。内容变化若未参与上次几何度量，计算可复用几何并发布新版本；尺寸、结构、provider或viewport变化仍求解参与布局的实例树。折叠子树不承担逐次工作数组初始化，节点描述仍保留。调用方可用 VirtualList 只实例化可见窗口，普通滚动只改变 composition 偏移；它不自动虚拟化任意 Session 控件树。原生 UI Table 的可见绘制走其自身统一表格入口。不承诺通用局部调度、全链路零分配或硬帧预算。

## 构建与验收

在仓库根目录准备正式环境后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target ui_layout_acceptance
cmake --preset release
cmake --build --preset release --target ui_layout_acceptance
```

该目标运行 `atelier_ui_layout_acceptance` 的解析几何代表；总体 `acceptance` 也包含它。布局目标只验证CPU布局消费；窗口/渲染/输入目前只有编辑器日志冒烟，非Windows构建仍未经验证。

## 研究与历史

- [三候选真实验证](../../research/ui-layout-reassessment/validation-results.md)：历史比较路径、性能分项、反例及覆盖限制。
- [工业游戏UI](../../research/ui-layout-reassessment/industry-game-ui.md) / [工业编辑器UI](../../research/ui-layout-reassessment/industry-editor-ui.md)：通过Exa调查的机制依据。
- [研究总入口](../../research/ui-layout-reassessment/README.md)：来源和历史资料，不是当前生产依赖。
- [算法取舍与来源](decisions.md#算法依据)：保留规范和工业实现的依据。
- 已退役的候选与原生首步探针可从Git历史恢复；源码、独立runner和已失效方案不再留在当前树中。固定来源、许可与必要实测摘要保存在研究目录。

正式代码位于 `modules/gui/ui_layout/`，消费者位于 `acceptance/ui_layout/`。当前树只保留这一套正式布局实现和验收入口。
