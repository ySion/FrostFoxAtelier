# UI 布局核心

状态：**自研生产首增量已落地。** 生产库、稳定节点身份、真实编辑、行列/父框锚定、测量缓存、失败发布和几何结果消费已具备正式实现。不是控件/渲染系统；共享列、换行/重复格、局部增量调度和虚拟视图仍属于后续增量。实际双配置运行状态与覆盖以[验收记录](acceptance.md)为准，不沿用研究探针PASS。

## 当前入口

| 目的 | 权威 |
|---|---|
| 使用公开C++ Interface | [`src/ui_layout/layout.hpp`](../../../src/ui_layout/layout.hpp) |
| 了解尺寸、身份、编辑、错误与结果寿命 | [生产合同](contract.md) |
| 查看正式目标、复现命令、覆盖和未测项 | [验收记录](acceptance.md) |
| 了解自研取舍与后续S01–S08 | [决策](decisions.md) |
| 接手当前实现与剩余工作 | [交接](session-handoff.md) |

生产命名空间是 `atelier::ui_layout`，静态目标是 `atelier_ui_layout`。公开包含路径为 `ui_layout/layout.hpp`；核心不链接 Runtime、LLVM/JIT、RHI 或文字库。根CMake复用既有工具链和分配器规则，不另建求解器后端工厂。

## 已实现的最小消费链

1. 创建带明确容量/深度限制的 `LayoutDocument`，传入同步叶测量函数与非拥有context。
2. 通过稳定局部 `LayoutNodeId` 创建、更新、移动、移除节点；编辑和旧发布相互独立。
3. 给根确定viewport和provider版本，计算完整未舍入几何；失败不提交半结果或本次新测量缓存。
4. 默认借用带版本的 `LayoutView`，跨成功发布/析构消费时显式复制 `LayoutSnapshot`。

核心有序树不是第二套业务对象世界。Document identity＋slot/generation只服务布局资源；内容键不保活业务对象或文字资源。原生内存异常正常传播，测量期间禁止重入编辑及销毁拥有者。

## 责任划分

| 拥有者 | 负责 | 不负责 |
|---|---|---|
| 布局库 | 节点/结构、尺寸规则、容器分配、测量缓存、完整逻辑几何与版本 | 场景业务对象、主题、事件、字体shaping、renderer |
| 叶测量 Adapter | 实际内容、字体/排版对象、内容与provider版本、可复用度量 | 修改正在求解的布局树、隐式保留旧绘制版本 |
| 集合视图 | 未来逻辑集合、可见实例、共享宽度索引、滚动锚点和extent | 把集合扫描成本隐藏成核心增量收益 |
| UI composition / 绑定拥有者 | 业务状态投影、几何/文字消费期、像素政策、绘制包与Runtime根 | 让布局节点代替业务对象或GPU资源寿命 |

当前支持Fixed/Content/正权重Expand及min/max、padding/gap、槽内填满/对齐、覆盖与父内容矩形锚定。真实文字采用按需独立宽度和确定宽度下的自然高度，不用viewport高度压缩自然需求。

**当前不是局部增量求解器。** 编辑可批量合并到下一次计算，稳定叶度量可命中缓存；发生变化后的几何仍全树计算。不承诺零分配、硬帧预算或已经快于第三方。

## 构建与验收

在仓库根目录准备正式环境后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target ui_layout_acceptance
cmake --preset release
cmake --build --preset release --target ui_layout_acceptance
```

该目标依次运行无文字依赖的 `atelier_ui_layout_minimal` 和真实DirectWrite的 `atelier_ui_layout_acceptance`；总体 `acceptance` 也包含它们。没有窗口/渲染/输入验收，也没有非Windows构建证明。

## 研究与历史

- [首步原生实验](../../research/ui-layout-reassessment/native-step1.md)：12项隔离实验、极端权重相消修复与本次合同收敛来源。
- [三候选真实验证](../../research/ui-layout-reassessment/validation-results.md)：历史比较路径、性能分项、反例及覆盖限制。
- [工业游戏UI](../../research/ui-layout-reassessment/industry-game-ui.md) / [工业编辑器UI](../../research/ui-layout-reassessment/industry-editor-ui.md)：通过Exa调查的机制依据。
- [研究总入口](../../research/ui-layout-reassessment/README.md) / [旧Yoga来源记录](research.md)：来源和历史资料，不是当前生产依赖。
- 旧Yoga合同、验收和交接在相应文档页尾保留为明确历史附录；当前实现以文档上半部分为准。

正式代码位于 `src/ui_layout/`、消费者位于 `acceptance/ui_layout/`。`acceptance/ui_layout_probe/` 与 `acceptance/ui_layout_step1_probe/` 继续保存比较/探索代码，不参与正式布局目标。变更仅在布局专属worktree完成，没有向其他worktree合并、提交或推送。
