# 工业游戏 UI：Slate/UMG、UI Toolkit 与 Control/Container

> 本页保留固定版本的一手来源与取舍依据，不构成本项目待实施计划。生产合同与验收统一见[布局模块](../../modules/ui-layout/README.md)。

## 1. 身份、版本与证据边界

**结论〔推断〕：应借鉴三者的布局协议、失效分层与集合视图生命周期，而不是把任一完整框架当作可替换的独立 solver。** 游戏 HUD 与复杂编辑器可以共用布局基础，但共享列宽、文本重排、滚动、虚拟化及 docking 不能全部压缩成一次 flex 求解。

| 对象 | 身份分类 | 本文核验基线及许可 |
|---|---|---|
| Unreal Slate / UMG | Slate 是控件、布局、输入与绘制框架；UMG 是其上的 UObject/可视化组织层；panel 实现布局协议 | **UE 5.6 官方版本化文档**。没有取得授权仓库的具体 patch/tag/commit，不伪装成固定源码审计。源码访问需 Epic/GitHub 授权，受 Unreal Engine EULA 约束。[E1–E8] |
| Unity UI Toolkit | retained UI 框架；内部布局节点及处理器只是其中一层 | **UnityCsReference tag `6000.0.0f1`**；手册取 `6000.0`。源码采用 Unity Reference-Only License，不是可直接移植进自有 C++ 引擎的宽松许可实现。[U1–U8] |
| Godot Control / Container | 场景树控件框架；最小尺寸、锚点及自定义 Container 协议；具体容器分别执行算法 | **Godot tag `4.4-stable`**，MIT；具体机制以固定 tag 源码为准。[G1–G7,G10] |

〔事实〕Unity、Godot 的上述 tag 原文件已取得，不依据 `master` 推断历史实现。版本化网页仍可能维护更新，不能等同首发快照；Godot 容器教程自身提示可能未更新至 4.4，因此关键机制由固定源码佐证。[G8] 本文不声称选择了最新版本，不把访问时间、页面构建时间当作发布日期；没有执行构建、测试或性能测量。

## 2. 三条机制路径

### Slate：desired-size → arrange，不是统一约束求解器

〔事实〕`SlatePrepass` 先下降至叶，再向上缓存 desired size；`ComputeDesiredSize(LayoutScaleMultiplier)` 可读取已缓存的子节点尺寸。`ArrangeChildren(AllottedGeometry, …)` 把父级实际分配的几何交给各 panel，生成子节点几何。自定义 panel 可以分别实现尺寸汇总和摆放规则；该接口并没有把 available width 作为通用 measure 参数。[E2–E4]

〔事实〕`FSlateTextBlockLayout` 持有独立文本布局缓存；`CachedSize` 明确用于根据前一次 `OnPaint` 尺寸估计自动换行宽度，并提供 `DirtyContent`、`DirtyLayout`。[E5] **〔推断〕不能把“一次自底向上 desired-size + 一次自顶向下 arrange”写成所有宽度依赖文本必然同帧收敛的合同。** 首次显示、祖先骤缩与字体变化必须单独验证。

〔事实〕失效区分 hierarchy、layout、paint；Invalidation Box 缓存子树，Global Invalidation 将缓存根提升到 `SWindow`，窗口内局部 Invalidation Box 随之停用。Volatile 不缓存自身及子项的 paint 数据，但仍可跳过不需要的布局；Retainer Panel 缓存到纹理并控制更新频率，属于绘制策略，不是尺寸算法。[E6]

〔事实〕`SGridPanel` 有共享行列数据、按内容取尺寸及行列 fill 系数；`UListView` 区分逻辑 item 与屏幕 entry，只生成显示所需的 entry。[E7–E8] 前者是专用 panel 算法，后者是集合视图实例化/回收协议，都不能全部算给 desired-size 核心。

〔上游自报〕Epic 建议 HUD 用事件更新替代每帧原始属性轮询，并警告 Scale Box 与 Size Box 组合可能形成尺寸往复。[E9] 这些是负载设计警示，不是本机性能结论。

### UI Toolkit：布局核之外还有版本、事件与虚拟化

〔事实〕手册将布局行为描述为 Yoga/Flexbox 子集原则；这只说明语义来源，不证明绑定某个上游 Yoga 版本或性能领先。[U1] 固定源码中 `LayoutNode` 以 handle 访问 style、computed 等数据；`MarkDirty()` 在已 dirty 时返回，否则清除 computed flex basis 并递归标脏 parent。[U2]

〔事实〕`UIRLayoutUpdater` 仅在布局根 dirty 时进入计算，随后按 `HasNewLayout` 选择性下降、比较新旧几何，再发 `GeometryChangedEvent`。事件可以再次改尺寸，因此存在反复求解循环；`kMaxValidateLayoutCount = 10` 的计数阈值触发错误日志并退出，不能把这个后置递增判断描述成严格“最多十次”。[U3] **〔推断〕solver 完成不等于框架整帧完成，几何事件是反馈回路入口。**

〔事实〕`TextElement.DoMeasure` 将宽高及各自 measure mode 传给文本测量提供者。文本 setter 在宽或高为 auto/none 时发 Layout+Repaint，否则只发 Repaint。[U4] 这是动态 HUD 值得借鉴的精确失效：固定外框数值变化与会改变父尺寸的文本变化不应一律 dirty 全树。

〔事实〕`ListView` 支持固定/动态行高、实例复用与 bind/unbind；动态高度控制器另存行高字典、累计高度缓存、等待测量集合、锚定项及滚动偏移，并安排后续 fill。[U5–U6] 实例数量小不代表辅助缓存恒定为可见项数量。手册明确横向 flex 排列会破坏该 ListView 的虚拟化：**排版方向可表达，不代表集合控件能虚拟化这一方向。**[U5]

### Godot：minimum-size cache + 延迟容器重排

〔事实〕4.4 `Control` 缓存 `get_minimum_size()` 与 `custom_minimum_size` 的逐轴最大值；不能套用其他分支的 desired/max-size 协议。`update_minimum_size()` 向祖先清理有效缓存，遇已失效节点、top-level 或相应窗口边界停止；随后延迟更新，只有最终 minimum size 改变才发送 `minimum_size_changed`。[G1]

〔事实〕`Container` 连接孩子的尺寸标志、minimum-size、可见性信号；结构增删也调用尺寸更新及 `queue_sort()`。后者以 `pending_sort` 合并重复请求，通过 deferred call 发排序通知。具体容器在通知中调用 `fit_child_in_rect`；该 helper 设置矩形并归零旋转、恢复单位缩放，孩子的摆放权属于父容器。[G2]

〔事实〕`GridContainer` 汇总每列最大最小宽、每行最大最小高，再分配剩余空间，是共享轨道，不是每行独立 HBox。[G3] `Label` 将内容、字体和行布局 dirty 分开，断行使用当前分配宽度；resize 标记 lines dirty，文本修改更新 minimum size。shaping 属于 TextServer 路径，不是容器算法。[G4]

〔事实〕`ScrollContainer` 区分内容尺寸与 viewport，滚动值改变仍调用 `queue_sort()`，重排时用负滚动偏移摆放内容，并开启裁剪。[G5] **缓存不等于滚动完全不走 arrange，也不等于必然重测全部后代。**

〔事实〕`ItemList` 为全部项保存内容及矩形缓存；`force_update_list_size()` 在 `shape_changed` 时遍历全部项计算尺寸，主题/语言变化会遍历并重塑文本。[G6] `TreeItem` 是带 cell 与父子链接的 Object，不是每行一个 Control。[G7] 因而“没有逐行场景节点”“屏外不画”和“仅实例化可见行”必须分开；本次没有证据把 Godot Tree/ItemList 宣称为 Unity/UMG 式回收视图。

## 3. 所有权、成本与安全边界

| 维度 | 已核实内容及限制 |
|---|---|
| 所有权 | Slate panel slots/共享指针控件、UMG UObject 层在框架内管理；Unity manager 管布局数据，measure/owner 关联 VisualElement；Godot 在场景树控件内持有尺寸和主题状态。[E7–E8][U2,U7][G1–G2] **〔推断〕**都不是无需转换即可接入任意自有树存储的 drop-in solver。 |
| 分配 | Unity 分离数据组件，managed 回调存储分块并复用空位；初始节点容量 65,536 **不是上限**。Godot Grid 使用临时 RBMap/RBSet，Label 重排重建行资源，不能承诺零分配。[U7][G3–G4] Slate 此次没有固定源码分配审计。 |
| 深度/线程 | Unity dirty 上传及布局下传存在递归，回收队列的锁不证明整树线程安全。[U2–U3,U7] Godot 尺寸更新有主线程 guard，文档限制 active scene tree 跨线程访问。[G1,G9] Slate/Unity 任意 worker 布局保证与三者安全深度上限均**待验证**。 |
| 错误/重入 | Unity 有循环求解错误出口；Godot 拒绝非有限 custom minimum，`fit_child_in_rect` 检查空孩子与父关系。[U3][G1–G2] 这些不是统一的测量失败、回滚、旧结果保留或回调可重入合同。 |
| 复杂度 | **〔推断〕**叶失效标记沿祖先路径传播，同批已 dirty 可截断；不约束后续 solver、文本、共享轨道和事件成本。祖先宽度改变与结构编辑仍可能扩散，不能推出整帧 O(变更节点)。 |
| C++23 接入 | **〔推断〕**Slate/Godot 控件抽取涉及对象、主题、字体、事件与渲染依赖；Unity 还涉及 C#/引擎绑定及许可边界。未验证独立 Windows x64 ABI、链接闭包或工具链组合，本阶段只研究协议。 |

**Containment 原则〔推断〕：**绘制缓存根、尺寸依赖边界、逻辑数据虚拟化边界是三回事。固定外框 HUD 岛可限制尺寸传播；依赖子内容自然尺寸的祖先不能因为套了缓存框就停止接收失效。[E6][U4][G1]

## 4. S1–S9 场景映射

下表是据上述证据导出的验证分解〔推断〕，不是性能排名。

| 场景 | 应验证的工业机制 |
|---|---|
| S1 HUD | Canvas 锚定、Safe Zone、Unity absolute overlay、Godot anchors；DPI、安全区和物理像素转换是上层策略，不由 flex 自动提供。[E9–E10][U1][G1] |
| S2 格子/标签 | Grid、Flow、flex-wrap 是不同组合；固定物品格不等于内容自适应共享列。[E7][G3,G8][U1] |
| S3 Inspector | 共享列线需要共享轨道或上层同步；折叠和多行错误提示影响测量；独立 row-flex 不自然保证跨行对齐。 |
| S4 Docking/popup | splitter 改约束，popup 脱离普通流；停靠状态、拖放命中和窗口归属属于框架。[U1][G8] |
| S5 大集合 | 分别统计逻辑项、实例化节点、可见项和行高缓存；比较回收型视图与轻量 item/cell 模型。[E8][U5–U6][G6–G7] |
| S6 文本 | 有限宽度下高度、字体/语言版本、混合字号基线进入测量协议；Unity 存在 baseline 回调不等于全部控件基线正确。[E5][U2,U4][G4] |
| S7 嵌套滚动 | viewport、extent、offset、clip 分开；滚动条显隐会改变可用宽度并反馈到文本高度。[U3][G5] |
| S8 冷热更新 | 静态、叶颜色/数值变化、祖先 resize、结构增删分别计 solver、measure、通知与缓存工作。 |
| S9 边界 | 非有限值、零宽、深树、测量中改树、延迟回调期间销毁、测量失败及旧结果策略。 |

## 5. 六组可执行反例/probe

以下均**待验证，未执行**。候选适配层与工业框架参考程序使用相同内容；实验规模不作为产品上限。

1. **共享列（S2/S3/S6）**：40 行 Inspector，各行独立 row-flex，标签长短交替；切换长本地化文本并展开错误提示。要求共同列线、不覆盖编辑器。列线错位即击穿“flex-only 足够”；修复须明确新增共享轨道/同步协议及传播范围。
2. **宽高反馈（S3/S6/S7）**：自动换行文本置于自动滚动条面板，宽度在 360/240 间切换，内容恰跨滚动条出现阈值，同时替换字体。逐帧记录约束、测量次数、行数、extent、矩形；稳定输入持续振荡、裁切或旧尺寸不收敛即失败。Slate 观察首次 auto-wrap，Unity 记录循环出口。[E5][U3][G4–G5]
3. **静态与叶更新（S1/S8）**：四个固定尺寸 HUD 岛和一个自然尺寸岛，分别运行静态、只改颜色、固定框数值、自动尺寸长文本、增删孩子。计访问节点、dirty 根、measure、arrange、分配与输出复制。静态完整遍历或颜色更新全树测量，首先反驳适配/失效合同，不自动归罪于 solver。
4. **虚拟化（S5/S6）**：10,000 逻辑项、约 30 项可见，先固定行高再用本地化动态行高；滚到中部后改宽、头部插项、立即跳尾。检查绑定身份、滚动锚点、末项可达和实例数量。Unity 再改 row 验证官方限制；Godot 分开计 item/TreeItem 与 Control。[U5–U6][G6–G7]
5. **交互与覆盖（S1/S4/S7）**：拖动 splitter，popup 跨滚动区边界，再改变缩放与安全区。检查定位参考系、裁剪、命中一致性；矩形正确却被错误裁剪，说明框架协议缺失，不能宣告布局任务成功。
6. **失败与生命周期（S9）**：递增深度构造单链，注入非有限测量值、测量中删除孩子、deferred 排序前销毁父节点及交替返回两个尺寸。要求无无限循环和悬空访问，并说明错误通知、是否保留最后成功结果；记录栈/容量边界，不把错误日志当作恢复成功。

性能 probe 必须包含真实 shaping、冷/热字体、同步、结果读取/复制与框架通知，同时记录逻辑项、布局节点、可见项和变更数量。**无本机测量，不给三者或候选 solver 排性能名次。**

## 6. 最强支持、反对与可借鉴协议

- **Slate〔推断〕**：最强支持是可替换 panel 的 desired-size/arrange 与分级失效；最强反对是自动换行可能借用前次几何，不能直接作为强约束纯函数模型，具体缓存路径也缺固定源码审计。[E2–E6]
- **UI Toolkit〔推断〕**：最强支持是版本化 dirty、受约束 measure 与回收视图分层；最强反对是事件反馈、动态行高缓存形成额外成本，内部数据结构也非公开独立 C++ 接口。[U2–U7]
- **Godot〔推断〕**：最强支持是可定制容器、共享轨道和延迟去重源码明确；最强反对是 minimum-size 不等于通用受约束 measure，ItemList 全项尺寸更新不能冒充可见行虚拟化。[G1–G6]

**建议保留四组评估协议〔推断〕：**①带宽高约束、内容/字体版本、基线的测量；②容器可定制的尺寸汇总及 arrange，允许共享轨道；③分开的 measure/layout/paint/structure 失效及明确传播边界；④逻辑 item—实体节点 bind/unbind/recycle，以及 extent 与滚动锚点。它们不是本阶段冻结的生产合同。

**未知项〔待验证〕：**UE 精确 patch/tag/commit、逐属性失效和变行高列表细节；三者完整测量异常/重入恢复语义、安全深度；Godot Tree 热点遍历范围；真实字体 fallback、混合字号基线、深层滚动收敛；Windows C++23 独立接入成本。已核验源码不能替代这些端到端实验。

## 7. 直接主源

### Epic

- [E1] 源码访问及许可边界：https://www.unrealengine.com/en-US/ue-on-github
- [E2] `SlatePrepass`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/SWidget/SlatePrepass?application_version=5.6
- [E3] `ComputeDesiredSize`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/SWidget/ComputeDesiredSize?application_version=5.6
- [E4] `ArrangeChildren`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/SWidget/ArrangeChildren?application_version=5.6
- [E5] `FSlateTextBlockLayout`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Slate/FSlateTextBlockLayout?application_version=5.6
- [E6] Invalidation：https://dev.epicgames.com/documentation/en-us/unreal-engine/invalidation-in-slate-and-umg-for-unreal-engine?application_version=5.6
- [E7] `SGridPanel`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Slate/SGridPanel?application_version=5.6
- [E8] `UListView`：https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/UMG/UListView?application_version=5.6
- [E9] HUD、绑定与布局建议：https://dev.epicgames.com/documentation/en-us/unreal-engine/optimization-guidelines-for-umg-in-unreal-engine?application_version=5.6
- [E10] Safe Zone：https://dev.epicgames.com/documentation/en-us/unreal-engine/umg-safe-zones-in-unreal-engine?application_version=5.6

### Unity

- [U1] 布局及 absolute overlay：https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-LayoutEngine.html
- [U2] `LayoutNode`：https://raw.githubusercontent.com/Unity-Technologies/UnityCsReference/6000.0.0f1/Modules/UIElements/Core/Layout/LayoutNode.cs
- [U3] `UIRLayoutUpdater`：https://raw.githubusercontent.com/Unity-Technologies/UnityCsReference/6000.0.0f1/Modules/UIElements/Core/Renderer/UIRLayoutUpdater.cs
- [U4] `TextElement`：https://raw.githubusercontent.com/Unity-Technologies/UnityCsReference/6000.0.0f1/Modules/UIElements/Core/TextElement.cs
- [U5] ListView：https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-uxml-element-ListView.html
- [U6] 动态行高控制器：https://raw.githubusercontent.com/Unity-Technologies/UnityCsReference/6000.0.0f1/Modules/UIElements/Core/Collections/Virtualization/DynamicHeightVirtualizationController.cs
- [U7] `LayoutManager`：https://raw.githubusercontent.com/Unity-Technologies/UnityCsReference/6000.0.0f1/Modules/UIElements/Core/Layout/LayoutManager.cs
- [U8] Reference-Only License：https://unity.com/legal/licenses/unity-reference-only-license

### Godot

- [G1] `Control`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/control.cpp
- [G2] `Container`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/container.cpp
- [G3] `GridContainer`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/grid_container.cpp
- [G4] `Label`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/label.cpp
- [G5] `ScrollContainer`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/scroll_container.cpp
- [G6] `ItemList`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/item_list.cpp
- [G7] `TreeItem/Tree`：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/scene/gui/tree.h
- [G8] 容器教程，注意自身版本警告：https://docs.godotengine.org/en/4.4/tutorials/ui/gui_containers.html
- [G9] 线程约束，注意自身版本警告：https://docs.godotengine.org/en/4.4/tutorials/performance/thread_safe_apis.html
- [G10] 固定版本许可：https://raw.githubusercontent.com/godotengine/godot/4.4-stable/LICENSE.txt
