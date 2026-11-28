# 布局核心设计决定

当前采用自研原生布局核心，面向游戏UI和编辑器的具体容器需求。选型原因与历史实测见[研究证据](../../research/ui-layout-reassessment/README.md)；当前行为只由[合同](contract.md)、[Flex/Grid](flex-grid.md)及公开头文件定义。

## 一个拥有者与明确的依赖

LayoutDocument统一拥有节点身份、有序树、工作区、度量缓存和几何发布。容器算法在同一Impl中工作；专门的table共享列、wrap重复槽和二维Grid各承载真实语义，不建立后端工厂或第二棵业务树。布局局部身份不替代Runtime身份或内容资源持有。

文字依赖按独立宽度、实际分配宽度、自然高度解析；只请求当前算法需要的信息。未知高度反向影响基线或宽度的组合明确返回unsupported_dependency，不通过无界反复试排近似。真实文字排版交给提供者，核心不链接DirectWrite。

## 分配和对齐各有明确职责

Expand采用整池带权分配，Flex采用明确的basis及grow/shrink权重。硬最小值允许溢出，全部到达最大值时留下空白。分配槽与实际内容框分开；两轴Alignment是唯一对齐入口。当前规则服务原生UI，不承诺CSS特性覆盖。

无饱和候选走线性快路径；触边时保留排序断点和平衡权重和，使大权重退出后小权重仍可取得剩余量。Grid跨格同跨度批次读同一初值、收集每轨最大增量，避免结果取决于插入顺序。基线传播复用真实排列，避免另写近似位置公式。

## 缓存、范围与资源

静态输入直接复用结果；未参与几何度量的内容更新可以复用几何并发布新版本。其他几何变化仍求解参与布局的实例树。折叠退出工作区初始化；可见列表通过VirtualList管理逻辑高度和锚点，实例继续进入同一LayoutDocument。当前数据不足以支持另建通用局部调度器。

几何及新度量缓存只在全部成功后提交。发布索引核验完整身份，允许旧结果独立于当前节点池。借用到下一次成功发布为止，跨期显式快照；完整内容版本由消费层保有。核心保持同步单写者与显式遍历；控件、输入和接受帧由[生产 UI](../ui/README.md)负责。

## 算法依据

以下整理自 2026-10-04 的原始资料核对，起点为 `f42d071b937ad0c9c81e2315c9a8b0ff49ddc00d`。当时的优化建议已并入现行实现，不再单列为待办。上游 `main` 链接保留为来源入口，本次没有重新评估上游版本。

- **有界分配。** [W3C Flexbox 伸缩步骤](https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths)提供按权重分配、夹限和冻结的依据。本实现使用自己的整池相对权重合同：严格位于 min/max 内部的比例候选走 O(N) 快路径；触界使用排序断点和平衡权重和，避免大权重舍入吞掉小权重余量。该路径为 O(N log N)、临时空间 O(N)；不采用 CSS 权重和小于 1 时的部分填充规则。
- **自动放置。** [W3C Grid 放置算法](https://www.w3.org/TR/css-grid-1/#auto-placement-algo)与 [Taffy placement.rs](https://github.com/DioxusLabs/taffy/blob/main/src/compute/grid/placement.rs)支持 sparse 游标与碰撞跳跃的机制依据。本实现沿冲突格向前推进，跨轨项目更新全部覆盖轨道的游标；查询仍取决于跨度、位表边界和候选探测数，没有所有输入线性时间保证。
- **跨度批次。** [W3C 跨格轨道求解](https://www.w3.org/TR/css-grid-1/#algo-spanning-items)与[额外空间分配](https://www.w3.org/TR/css-grid-1/#distribute-extra-space)说明共同初值、每轨最大计划增量及统一提交。本实现只提交收到正增量的轨道，已满足需求的跨度提前退出；Fixed 不增长，max 始终是硬界。
- **失效与缓存。** [Yoga 增量布局](https://www.yogalayout.dev/docs/advanced/incremental-layout)及 [Cache.cpp](https://github.com/facebook/yoga/blob/main/yoga/algorithm/Cache.cpp)说明父约束改变时不能仅凭节点未修改跳过测量。本实现缓存核验内容/provider 版本、宽度与基线请求，失败不提交新缓存；通用子树跳过仍需覆盖共享列、基线和祖先自然需求的依赖。
- **工作区寿命。** [Grid 布局算法](https://www.w3.org/TR/css-grid-1/#layout-algorithm)支持先定列宽、再取得文字高度的依赖顺序。自然 Grid 查询可嵌套在父分配中，因此保留独立可复用工作区；省去无人消费的提前行求解，不能省去最终自然高度。无对应算法时不分配专用状态；复用容量仍保留内存高水位。

上述复杂度为本地实现分析，性能收益以同输入测量为准。CSS 自动 min-content、百分比/宽高循环、writing-mode、auto margins、dense 回填及未开放的基线组合维持[原生边界](flex-grid.md)，不从参考来源推导完整 CSS 兼容性。

## 取舍依据

[三候选实测](../../research/ui-layout-reassessment/validation-results.md)暴露了饱和分配、文字测量量和深链边界；矩形榜单不足以决定整套UI方案。尺寸与失败发布的语义由[生产合同](contract.md)定义。借鉴[游戏UI](../../research/ui-layout-reassessment/industry-game-ui.md)和[编辑器UI](../../research/ui-layout-reassessment/industry-editor-ui.md)的容器、失效和虚拟化责任划分，不引入其完整对象、输入或渲染世界。

正式 UI、声明式作者层与编辑器已经接入；当前消费成本和后续边界统一见[模块状态](README.md#阶段状态)。
