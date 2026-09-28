# 内容绑定与完整呈现消费

本页说明正式消费者中的资源持有与提交机制。它是UI接入示例；生产窗口、输入和GPU绘制尚未接入。当前39项验收见[acceptance](acceptance.md)，同场景成本见[audit](audit.md)。

## 唯一消费链与资源责任

代码位于现有正式目标的 [main.cpp](../../../acceptance/ui_layout/main.cpp)、[scaling.inl](../../../acceptance/ui_layout/scaling.inl) 和 [editor.inl](../../../acceptance/ui_layout/editor.inl)。这是可执行接入示例，不是已交付的编辑器控件框架。

```text
业务key、展开状态、属性修改、候选拖动宽度
  → 上层commit
  → LayoutDocument计算外壳与固定分配的Inspector视口
  → VirtualList选择逻辑位置 → 业务key绑定可见节点
  → LayoutDocument计算可见内容并反馈实际行高
  → TextFrame按最终节点宽度准备文字版本
  → 一次替换外壳帧、Inspector帧和滚动范围
```

外壳和Inspector使用两个LayoutDocument实例，求解Interface完全相同。外壳明确分配Inspector的宽高，内部自然内容不反向决定外壳尺寸，因此这个边界没有循环尺寸依赖。列表内容变化可保持外壳发布不变；改变面板宽度时两者在同一次上层提交中协调。若未来要求子面板自然尺寸反向影响外壳，必须重新明确尺寸依赖，不能照搬本例边界。

### 已发布文字帧

- 核心 LayoutSnapshot 仍只拥有几何。消费者 TextFrame 另持有各节点对应的段落版本，捕获时按最终内容宽度调用同一个 TextProvider::resolve；测量回调也使用此方法。
- 即使两个节点使用同一内容key，也按各自宽度保有正确段落。固定宽高节点可能完全跳过测量，捕获阶段仍准备新文字；几何复用不等于文字资源复用。
- provider缓存和帧共享段落拥有者。内容/provider版本变化创建新版本；已被帧引用的段落需要改变宽度或换行时，先另建段落。仅给可变COM对象AddRef不足以保证旧画面。
- 范围修正可能经历多次内部compute。只有最终范围、几何和文字都准备成功，消费者才替换完整输出。失败允许输入和工作缓存继续保留待提交状态，已发布帧保持可消费。
- 删除离屏缓存不会提前释放仍被帧使用的段落。最后一个拥有者释放后即可回收；长期保留多个历史帧会保留相应资源。这里验证CPU资源寿命，GPU提交及fence寿命仍由将来的渲染层负责。

### 列表与业务状态

VirtualInspector保留一份位置到业务key的序列。可见节点按key保留；在视口上方插入/删除不会让属性值和选择状态错绑到复用槽。折叠示例暂时移除展开序列中的连续子项，业务编辑仍按原key保存，恢复时重新实现可见节点。该示例没有引入通用树模型或全量过滤框架。

realize统一比较宽度与provider版本，任一变化均重估逻辑高度并保持锚点。调用者更新字体政策时推进provider版本即可，不再在每个调用点手工清高度。结构编辑与统一重估仍为O(N)，正常可见范围查询/行高更新为O(log N)。例子要求传入有效且唯一的业务key，业务目录的校验不属于布局库。

### ghost与提交

begin/update只修改候选宽度，cancel丢弃候选，release提交最终宽度。正式场景执行64次预览再取消，确认零测量、零节点访问、零布局发布；释放只触发一次上层commit，内部允许为行高修正计算多次。这里验收的是命令序列，没有伪称验证了鼠标事件或屏幕绘制。


## 发布边界与图片接入

核心几何发布与完整呈现帧提交是两个事件。布局成功后，如果图片或绘制准备失败，composition继续持有上一完整帧，不回退LayoutDocument的publication，也不把旧几何配上新内容。固定尺寸且未触发测量的叶也必须捕获资源。

后续图片测量从捕获的固定内容版本读取固有尺寸，逻辑尺寸、宽高比和DPI政策由UI消费方负责；资源侧提供内容捕获与设备表示。LayoutDocument不持有Runtime根、Texture或GPU资源。生产composition模块位置和共同构建接线由跨线集成确定。

## 工业依据

[Qt QSplitter](https://doc.qt.io/qt-6/qsplitter.html#details)支持拖动结束再调整子控件；ghost是调用方交互政策。[WPF控件性能](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-controls)区分虚拟化与容器复用；业务状态须绑定业务key。[DirectWrite SetMaxWidth](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextlayout-setmaxwidth)会改变排版对象，因此单纯AddRef不能固定旧版排版结果。
