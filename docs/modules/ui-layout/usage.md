# 布局调用与消费

唯一入口是 `ui_layout/layout.hpp` 中的 `LayoutDocument`。常用描述构造直接返回 `NodeInput`，所有创建、更新和计算仍经过同一校验及发布路径。

## 创建一次，按变化更新

下面的函数在既有Document中创建一个两列面板，并把两个需要后续更新的身份交给调用者。Document构造时注入同步测量函数与非拥有context；实际DirectWrite实现可参考正式消费者的 `TextProvider`，不需要把文字库链接进布局核心。

```cpp
using namespace atelier::ui_layout;
struct PanelNodes { LayoutNodeId panel, label, value; };

Result<PanelNodes> create_panel(LayoutDocument& document,
                               std::uint64_t label_key, std::uint64_t value_key) {
    auto panel = document.create(document.root(),
        NodeInput::table({Axis::content(40, 140), Axis::expand(1, 40)}, 12, 6));
    if (!panel) return std::unexpected(panel.error());
    auto row = document.create(*panel, NodeInput::row());
    if (!row) { (void)document.remove(*panel); return std::unexpected(row.error()); }
    auto label = document.create(*row, NodeInput::measured(label_key));
    if (!label) { (void)document.remove(*panel); return std::unexpected(label.error()); }
    auto value = document.create(*row, NodeInput::measured(value_key));
    if (!value) { (void)document.remove(*panel); return std::unexpected(value.error()); }
    return PanelNodes{*panel, *label, *value};
}
```

多次成功编辑可以合并到下一次compute，但不提供跨调用的自动事务。示例回收显式失败后的半成品子树；内存异常仍正常传播。

物品格使用 `NodeInput::grid(80, 8, 6)`：最小槽宽80、项间距8、行间距6，随viewport自动改变列数。`NodeInput::wrap(8, 6)`用于不同自然宽度的标签流；描述上的`line_align`为0/0.5/1时分别靠起点、居中、靠终点。更多规则见[合同](contract.md)。

## 一次提交最终树

已持有完整描述的消费者用 `set_tree` 提交结构变化；普通属性变化继续用 `update`。下面保留面板与标签，创建一个新值节点；未列出的其他旧节点会全部退役，因此应提交文档的完整目标树。

```cpp
const TreeNodeInput tree[]{
    {nodes.panel, std::nullopt, NodeInput::column()},
    {nodes.label, 0, NodeInput::measured(label_key)},
    {{}, 0, NodeInput::measured(value_key)},
};
auto next = document.set_tree(tree);
if (!next) return std::unexpected(next.error());
nodes.value = (*next)[2];
```

父项下标必须指向较早条目；同父条目的输入顺序决定兄弟顺序。保留节点的 ID 不变，新节点的 ID 从结果读取。批次失败不部分修改布局输入，旧几何仍可消费；尺寸依赖和文字测量错误仍可能在后续 `compute` 出现，见[完整树合同](contract.md#完整树批量提交)。

## 内容变化与版本

1. provider更新实际文本或图片内容，并推进该内容的revision。
2. 用`document.input(node)`取描述，更新`content_revision`，再调用`update`。字体、排版政策等影响全部内容的变化推进provider revision。
3. 用`compute(viewport, provider_revision)`取得同次发布的几何。内容、版本与context在整个同步调用中必须稳定。
4. 折叠通过描述的`collapsed`更新；身份和输入保留，恢复后无需重建子树。

provider自身的排版对象不会随布局失败回滚。实际绘制需要由调用方保持几何与文字/资源版本一致；不能把几何快照当作旧绘制包的拥有者。当前DirectWrite示例在已发布帧引用的段落需要改宽/换行时另建版本；捕获帧与测量共用resolve入口，固定尺寸未触发测量的文字也会按最终宽度准备。完整的TextFrame与提交示例见[编辑器消费](composition.md#已发布文字帧)。

## 查询、遍历与结果寿命

```cpp
auto result = document.compute({800, 600}, provider_revision);
if (!result) {
    // describe(result.error().code/field)提供静态说明；provider_code保留内容错误。
    return std::unexpected(result.error());
}
if (const LayoutBox* value = result->find(nodes.value)) {
    // value->x/y相对其parent外框。按需通过find(parent)累加逻辑坐标。
}
auto held = document.snapshot(); // 显式复制几何与索引，供跨发布消费。
```

`find()`是常数时间完整身份查询；同样可以顺序遍历`boxes`。返回nullptr表示该发布中不存在该身份，包括折叠节点。编辑或失败计算后，旧view仍能查询其原节点，即使输入侧已经移除并复用了slot。

下一次成功发布或Document析构结束view及其Box指针的借用；快照独立拥有结果。`snapshot.view()`借用快照，快照销毁、赋值、移动或手动修改其boxes后不要继续使用先前借用。保留快照的原始boxes顺序才能保持完整索引查询。

## 性能与内存观察

通过两次`stats()`的差值观察measure_calls、node_visits和内部缓冲分配。`topology_commits` 统计产生变化的完整树提交，`topology_links_written` 统计改变的最终孩子序列写入的节点数；它们不计普通 `move`，也不表示整个调用没有其他扫描。内部缓冲计数排除provider、快照、公开结果 ID 数组及列描述复制，不能把其零分配推广到整个调用链。频繁读取使用view；只有需要跨发布保存时才复制快照。

## 原生Flex与二维Grid

原生 Flex、二维 Grid、首基线与两轴 Alignment 使用同一 LayoutDocument 入口，完整描述形式见[Flex/Grid 示例](flex-grid.md)。

## 大型列表

用VirtualList保存逻辑高度和滚动范围，range返回的行才实例化为LayoutDocument节点。计算成功后反馈实际行高；如果修正估计改变了范围，补齐新进入的行。调用方按业务key保留可见节点，统一处理宽度/provider版本的高度失效，回收离屏缓存；已提交帧仍可持有旧段落直到消费结束。

同一窗口内滚动时，使用range.local_origin作为composition偏移，无需把每个像素的滚动写进节点。宽度/字体政策改变时统一重估高度，并保留顶端锚点；局部内容变化只更新对应行。未知高度的extent仍是估计。

固定尺寸、折叠或其他没有实际度量依赖的内容变化，可复用几何发布；需要自然大小的节点仍会计算。`geometry_reuses`、node_visits和measure_calls的差值能区分这些情况。完整成本和复杂度见[契约与测量](virtual-list.md)。
