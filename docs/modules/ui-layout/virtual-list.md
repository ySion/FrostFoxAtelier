# 逻辑集合与可见范围

VirtualList只拥有逻辑行高与滚动索引，业务数据、布局实例和文字资源由各自拥有者管理。当前验收见[acceptance](acceptance.md)，文字消费寿命见[composition](composition.md)。

## VirtualList契约

`VirtualList(max_items, gap)`管理紧凑行高数组和Fenwick前缀和索引。索引中的行号是位置，不是业务对象或新的稳定身份。调用者以自己的业务key对应这些位置，并在同一个LayoutDocument内实例化range返回的行。

- 容量必须在[1, UINT32_MAX−1]，gap有限非负；行高必须正且有限，不超过float最大值。构造非法参数抛invalid_argument，其余可恢复错误使用现有Result/Error；内存异常正常传播。
- `reset(count, estimated_height)`替换集合并将scroll归零。`set_height(row,height)`更新单行高度；它不自行判定高度是精确测量还是调用方估计。
- `set_viewport(scroll,height,overscan)`接收逻辑像素，height/overscan有限非负且不超过float最大值。scroll有限非负，超过末端时夹到`max(0,extent-height)`。零高viewport返回空范围。
- `range()`返回半开区间[begin,end)、首实例相对viewport的local_origin、scroll、extent和revision。范围包括overscan，gap算入行的区间；可少量包含与间隙相邻的行。extent是现有行高和间距之和，末尾没有gap；未测行使用估计时，总extent也只是估计。
- 全局prefix、extent和scroll使用double；先做prefix(begin)−scroll，再将局部坐标交给几何/绘制。不能把巨大scroll先转成float。特别巨大的单行或overscan仍会产生很大的局部坐标，不承诺无限精度。
- 修改viewport上方的高度时保留当前顶端行及行内偏移。`insert/erase`相应调整锚点的位置；删除锚点行选择后继或末尾有效行。行变矮、集合末端或viewport约束无法保留原偏移时会夹紧。即使在顶部插入也默认保留原锚点；需要显示新首行时调用者显式set_viewport(0,...)。
- `invalidate_heights(estimate)`用于宽度/字体政策改变：用统一估计替换高度，保留锚点。业务内容局部变化可只对该行set_height；屏幕外无需立即排版。
- 节点/字体不由VirtualList保活。旧几何与异步绘制包的寿命仍由调用侧保证；VirtualList不是滚动输入或渲染系统。

### 成本和失败

range/offset/单行更新为O(log N)，普通viewport变更无分配。reset、insert、erase和统一高度重估为O(N)；insert/reset在所有可能失败的分配完成后交换状态，erase在既有容量中执行并保留容量，reset交换新存储时释放旧缓冲。错误不改变revision或数据。两个double数组为O(N)存储；10万行当前约1.6MB，节点和文字缓存按可见窗口另计。

该接口不自动回收业务数据、不自动维护任意排序的业务身份，也不自动对所有未见文字求最长标签。排序可由业务重新建立对应高度输入；共享列应选择固定/预计算宽度，或由集合维护完整宽度索引。不能直接用可见行最大值冒充全局最长列。

## 正式可见消费者

`acceptance/ui_layout/scaling.inl`中的VirtualInspector实际管理10万逻辑行：

1. 用range获取窗口，将位置映射到业务key；按key保留仍在窗口内的布局节点，新增进入的行，删除离开的行。
2. 离开窗口时同步回收示例的内容/DirectWrite段落缓存，避免只裁剪几何却保留所有文字对象。
3. 通过LayoutDocument计算真实行高，再反馈set_height。估计修正改变窗口时继续实现新范围，直到可见范围稳定；同宽度下每个新行只需要收敛到其真实高度。
4. 共享标签列明确固定120px，值列使用现有Expand。滚动不会因为标签集合改变而改变列宽。
5. local_origin由composition消费。同一组行内滚动只改变这个偏移，LayoutDocument复用发布；没有每像素改节点offset或重新测量。
6. 估计修正可能经历多次compute；示例只在范围稳定、最终几何及对应文字版本均准备好后，与range一起保存。仅原点变化时复用已有完整帧。中途失败保留上次完整可见输出，文字对象按版本保有，具体机制见[已发布文字帧](composition.md#已发布文字帧)。快照和段落存储不计入Document内部缓冲统计；GPU异步寿命仍需渲染层处理。
7. 在realize统一比较宽度和provider版本，变化时重估逻辑高度；调用方无须重复执行字体失效接线。业务组的插入/折叠操作由自己的key序列驱动，VirtualList的行号仍只表示位置。

该示例用Microsoft YaHei UI作为中文界面的显式字体政策；缺失时保留Segoe UI并在性能输出中报告。它展示真实文字和布局消费，没有窗口、鼠标事件或GPU绘制验收。

ghost拖动由调用方保留候选宽度，在松开时执行一次上层提交；内部可以为高度修正进行多次compute。当前命令序列已有取消/提交验收，见[编辑器消费](composition.md#ghost与提交)。核心不规定必须实时重排或必须延迟，也不对浏览器的逐帧处理能力作结论。


## 验收

正式消费者覆盖十万逻辑行、滚动锚点、巨大extent下的局部小数、增删、统一高度失效及失败保旧。使用正式布局验收目标；当前结果与性能分别见[验收](acceptance.md)和[审计](audit.md)。
