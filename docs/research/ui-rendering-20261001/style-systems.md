# 游戏 UI 样式机制：USS、Theme 与 typed Style

本文保留外部样式机制与采用成本的比较；本项目当前实现以 [SS 文档](../../modules/ui/style-sheets.md)为准。

检索日期：2026-10-01。本文为研究与建议，不是已实施合同。Unity 手册固定为 6.0（6000.0）；UnityCsReference 固定于提交 `88ce7b60434ba7a8ca0218590a4cb509971788ad`，其 README 标注 6000.7.0b2；这是预发布源码观察，与 6000.0 手册不是同一版本，不能视为 6.0 实现保证。Godot 固定为 4.5，避开 latest 中新增能力。Epic 官方链接当日显示 Unreal Engine 5.8，仅表示实际见到的动态文档视图，不据此判断最新或稳定版本，也不代表项目应采用该版本。未运行这些引擎或项目验收。

## Unity UI Toolkit：USS 实际提供什么

### 选择器与状态

USS 有类型、名称、class、通配选择器，可组合多条件、后代、直接子节点与逗号列表。它是类似 CSS 的受限集合，文档没有承诺所有 CSS 选择器。[官方选择器](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-Selectors.html)

官方状态为 `:hover`、`:active`、`:inactive`、`:focus`、`:disabled`、`:enabled`、`:checked` 和作用域 `:root`，可组合 `:checked:hover`；不能自定义 pseudo-class。文档明确 `:selected` 不受支持，建议使用 `:checked`。项目的 selected/invalid 语义应保留自己的状态定义，不能机械映射成 Unity 状态；自定义业务外观可以用 class。`:root` 是挂载该 stylesheet 的最高元素，不等同于浏览器 HTML 根。[官方 pseudo-classes](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-Selectors-Pseudo-Classes.html)

启发：有限状态位和有限 class 已能覆盖编辑器常见控件。状态由输入/业务语义产生，样式规则只消费它，不因为颜色看起来 disabled 就禁用交互。

### 来源、优先级与继承

同 stylesheet 优先 specificity，再按规则顺序；跨 stylesheet 依次考虑用户/default 来源、specificity、挂载层级和规则位置。名称优先 class，class 优先类型，类型优先通配。直接应用的属性优先继承值；UXML inline 覆盖 USS；C# inline 覆盖 UXML；没有 `!important`。[官方优先级](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-uss-selector-precedence.html)

继承是逐属性的：父属性只在该属性可继承且子元素未指定值时使用。属性表单独标记 inherited 与 animatability，例如背景色不继承；不能把父节点整个 Style 深复制给孩子。[官方属性表](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-Properties-Reference.html)

`element.style` 是 inline 输入，`resolvedStyle` 是综合 classes、继承和 inline 后的当前帧结果。相对尺寸还需布局完成；不能把写入的百分比值当成最终像素尺寸。挂在元素上的样式表作用于该元素与后代，不作用于父节点或兄弟。[官方 C# 样式接口](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-apply-styles-with-csharp.html)

启发：指定值、computed 值和 layout 后使用值必须可区分，但初期不需要三个公共对象世界。typed 指定规则保留 unset，内部求值得到完整的 Appearance；具体坐标仍由已有布局/帧承担。调试时能说明某属性来自哪条规则比支持大量语法更有价值。

### 变量与自定义控件

USS `--name` 和 `var(--name, fallback)` 支持主题共享值；变量改变会更新使用它的属性。6.0 文档明确不支持变量数学运算，也不支持在 `rgb()` 等函数内嵌套 `var()`。[官方变量](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-CustomProperties.html)

这足以证明可复用 token 不要求完整 CSS 表达式。项目初期可直接用 typed color、length、font token；引用在规则编译时校验类型。若保留可继承 token，规定缺失、循环和 fallback 行为，避免渲染热路径解析任意字符串。

### 更新与缓存：源码观察

UnityCsReference `VisualTreeStyleUpdater` 在上述固定提交中可观察到：版本未变就跳过 Update；stylesheet 变化收集节点；遍历传播 dirty，并记录 pseudo-state 依赖；匹配规则排序后求 computed style，再应用 inline。缓存键包含规则/stylesheet 内容、继承值、变量环境和 DPI；重复规则组合可复用结果。源码还显式处理缓存资源引用失效。这些是实现启发，不是所有 Unity 版本的稳定 API。[官方源码](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/VisualTreeStyleUpdater.cs)

不能据此宣称“改一个 class 永远 O(1)”：后代选择器、继承、变量与树重挂载都可能使子树失效。最小选择器集合直接决定项目需要维护多少依赖。初期以 class/kind/state 的本节点匹配配合显式主题 scope，可以避免任意祖先查询。

### Transition

USS 有 property、duration、timing-function、delay；样式值改变可触发，第一帧没有前态。单位不匹配会阻止部分过渡；可继承属性及中断反向过渡另有语义。官方建议把 transition 放在基本规则上，否则离开 hover 可能没有退场过渡。UsageHints 可减少几何重生成，但晚加 hint 仍有重建代价。[官方 transition](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-Transitions.html)

项目收益顺序应先是颜色/透明度反馈，再是变换，最后才是布局属性动画。动画仍生成统一 computed/used 属性；不能直接向 renderer 写第二套控件状态。目标值与当前采样值分开，重定向从当前可见值开始，并明确 reduced motion 和对象销毁取消。若变换进入样式，命中/clip/IME anchor 必须消费同帧变换。

## Godot：Theme 与 StyleBox

Theme 按控件类型和命名项组织颜色、常量、字体、字号、图标、StyleBox。它提供配置，控件负责解释和应用；focus StyleBox 可作为普通状态之上的覆盖。查找从本控件 local override 开始，随后沿 Control 祖先的主题、项目主题、默认主题回退，同时按 type variation、控件类和基类寻找相应项。主题作用域传播与 CSS 每个属性继承不是同一种机制。[Godot 4.5 skinning](https://docs.godotengine.org/en/4.5/tutorials/ui/gui_skinning.html)

type variation 如 `GrayButton` 基于 `Button`，替换部分主题项、保留其余项，可形成变体链；控件通过 theme_type_variation 选择它，无需复制整个主题。[Godot 4.5 variation](https://docs.godotengine.org/en/4.5/tutorials/ui/gui_theme_type_variations.html)

StyleBox 是 Resource，负责盒形绘制、内容边距/最小尺寸等信息，并提供点测试接口。项目可借鉴它把面板/边框/内容 inset 作为同一外观合同，但几何和 hit-test 仍归现有 UI 链计算，不能各自拼一份尺寸。[Godot 4.5 StyleBox](https://docs.godotengine.org/en/4.5/classes/class_stylebox.html)

资源默认共享，官方在单控件修改 StyleBox 的例子中先 duplicate，避免修改全部使用者；Control 有 begin/end_bulk_theme_override 合并主题变化通知。[Godot 4.5 Control](https://docs.godotengine.org/en/4.5/classes/class_control.html)

启发：对于原生编辑器，typed theme + variant + state brush 已有高表达力。局部 scope 提供主题查找；字体/文本色的逐属性继承另行定义，两者不要混成“祖先所有属性自动传下来”。批量变更在提交后只发布一个版本，共享资源采用不可变发布能减少隐式全局写入。

## Unreal Slate：控件样式结构与变化分类

`FSlateStyleSet` 提供按名称查询/设置的颜色、margin、font、brush、WidgetStyle 等条目，并有 ParentStyleName；这是样式注册/资源查找接口，不能从它推断 CSS selector cascade。[官方 FSlateStyleSet](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/FSlateStyleSet)

`FButtonStyle` 是具体 typed widget style，包含 normal/hovered/pressed/disabled brush 与前景色，还有 normal/pressed padding；它能枚举样式引用的 brush 资源。状态样式不限于颜色，但改 padding 确实可能改变度量，必须正确分类。[官方 FButtonStyle](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/FButtonStyle)

Slate invalidation 区分 paint、layout、child 等变化；例如颜色可只失效 paint，子结构变化需要更大重算。官方明确缓存与 Retainer 的成本不同，Retainer 另有 render target 内存/重绘开销，先尝试 invalidation。[官方 invalidation](https://dev.epicgames.com/documentation/en-us/unreal-engine/invalidation-in-slate-and-umg-for-unreal-engine)

启发：样式数据可以是原生结构，关键是控件状态和哪些缓存受影响。暂不复制全窗口纹理来解决普通主题切换，也不提前复制 Slate 的所有缓存层。

## 两种表达方式的成本与收益

| 方面 | 扩展现有 typed Style | CSS-like 声明式规则 |
| --- | --- | --- |
| 表达 | kind/role/variant/state + typed token/局部 scope | class/type/state + 有限属性表/变量 |
| 好处 | 类型校验直接；能马上复用现有 resolve、字体与帧合同 | 风格与业务声明分开；多 class 组合；编辑/重载不必修改 C++ |
| 成本 | 若变体只靠增加 enum，会把产品组合塞进底层 | parser、诊断、规则编译、优先级、匹配依赖、工具支持 |
| 主要风险 | 每控件写特例、整个 Style 复制、渲染端额外覆盖 | 无限选择器、优先级战争、隐藏祖先依赖、每帧文本解析 |
| 共同核心 | 部分指定值 → 唯一 typed computed style → 同一变化分类与发布 | 使用相同核心，不另加 stylesheet 专用 renderer/layout |

因此，typed 与声明式可以是同一规则数据的不同生产者，不必成为两套执行模式。上述比较不代表项目承诺支持 USS、Godot 或 Slate 的全部能力；实际属性、状态限制、发布和布局覆盖关系见 [SS 合同](../../modules/ui/style-sheets.md)。
