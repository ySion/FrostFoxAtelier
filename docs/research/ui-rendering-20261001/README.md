# UI 绘制与样式：专家调研及项目建议

日期：2026-10-01。项目代码基线：`8d774cf82b30d320a246246853e3359a4f6c83e9`。四位研究成员均使用 `gpt-6.1-sol`，分别调查 Unity UI Toolkit、原生绘制器、样式机制和本地 Style 执行链；主研究补充本地 renderer 分析与方案整合。

本文保留上述基线的研究结论与建议。随后已按顺序实现接受帧文字快照、SS 样式资源、圆角/图片和相邻合批；当前行为、边界及验收入口以 [SS 模块文档](../../modules/ui/style-sheets.md) 为准。下文“当前实现”指研究时的基线，不作为新代码的能力清单。

本阶段的 review、旧结构清理及双配置验收见[阶段收口](closeout.md)。

## 建议方向

继续扩展现有 UI：一个控件身份与状态体系、一套样式求值、一套布局和文字服务、一个拥有式绘制帧、现有 RHI。样式应能表达主题、class/变体、状态、局部覆盖与少量继承，绘制器应逐步支持圆角盒、图片/图标、自定义几何与连续批处理。无需先建立浏览器，样式也不应退化成每个控件硬编码颜色。

最值得借鉴的共同机制是：按实际属性差异更新必要数据；复用排版与绘制几何；合批服从覆盖顺序；控件和资源变化通过明确版本进入同一完整帧。这是针对本项目的设计判断，不是某个框架官方规定的实施路线。

## 完整研究材料

| 材料 | 内容 |
| --- | --- |
| [实施前的绘制器](current-renderer.md) | 本地 CPU 协议、DirectWrite、逐项 GPU 绘制、裁剪与缓存；源码位置 |
| [实施前的样式](current-styles.md) | Style、角色、状态、度量失效、接受帧与文字交互风险 |
| [Unity UI Toolkit](unity-ui-toolkit.md) | 分类更新、布局结果比较、UIR 合批、atlas、clip、mesh、文本与 panel 生命周期 |
| [Qt / RmlUi / ImGui](native-renderers.md) | 绘制表示、CPU/GPU 缓存、顺序、字体、资源和线程合同 |
| [USS / Godot / Slate 样式](style-systems.md) | 样式来源、继承、状态、token、变化分类和有限声明式前端 |

Unity 使用明确的 6000.0 文档与固定的 6000.7.0b2 预览源码，不能把两者混成同一稳定合同。其他框架的版本与固定源码位置见各材料。研究只依赖官方文档、官方发布与项目源码；没有把搜索摘要或二手博客作为结论依据。

## 当前实现的实际水平

当前已经有保留的控件身份、输入/编辑状态，以及可复用的布局与文字缓存。每次 prepare 建立新的绘制快照；renderer 按快照逐项提交，不是已有完整的保留绘制几何系统。

```text
Workspace 的 Control 描述与 UI 状态
  → 唯一 resolve_style
  → NodeInput + TextStore → ui_layout
  → UiFrame：控件盒、文字输入位置、有序 DrawQuad、R8 Coverage
  → renderer：矩形/文字 pipeline + scissor → RHI/Present
  → accepted 后发布同次帧的输入命中信息
```

样式基础包括主题字体、字号/字重层级、颜色、内容 padding、边框/caret/滚动条尺寸，以及 hover、pressed、focus、selected、disabled、invalid 的统一解析。还没有 class、规则资源、局部主题与属性级覆盖。详细字段和优先级见[当前样式](current-styles.md)。

绘制基础包括实色矩形、文字 R8 覆盖图、UV、矩形裁剪、正确的线性颜色/预乘混合和资源退休。每个可见绘制项一次 draw；一个边框由四个矩形组成。文字由同一 DirectWrite layout 负责测量、命中、caret 和栅格；整段位图版本被 CPU/GPU 缓存。暂未支持一般图片、圆角/路径裁剪、矢量网格、共享字形图集或跨绘制项批处理。源码证据见[当前绘制器](current-renderer.md)。

这些机制已形成可扩展的基础。今后内部可以保存派生的绘制几何或 GPU batch，但控件语义、布局与输入仍应沿现有入口；内部缓存不是新的业务控件世界。

## 各框架具体借鉴什么

| 来源 | 最值得借鉴 | 在本项目中的用途 |
| --- | --- | --- |
| Unity UI Toolkit | 最终样式差异分类；布局结果比较；visual content 缓存；视觉参数更新 | 避免换色就重排文字，避免布局经过某节点就重建几何 |
| Qt Quick | 连续/正确合批、GPU 几何保留、transform batch root | 静态面板和滚动内容复用绘制数据 |
| RmlUi | Compile/Render/Release geometry；裁剪/层/效果的明确能力合同 | 定义自定义几何和资源寿命，区分已支持图元与高级效果 |
| Dear ImGui | 简单有序顶点/索引/命令流与连续命令合并 | 参考低成本批处理实现，不据此替换现有文字和控件模型 |
| Godot Theme / Slate | typed theme、变体、状态 brush、内容边距与失效分类 | 给原生编辑器提供样式表达，而不引入全部 CSS |

Unity 的 `generateVisualContent` 会缓存内容，并在需要重绘时重新生成；这可作为自定义控件绘制接口的参考。[Unity 官方接口](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.VisualElement-generateVisualContent.html)

Qt 默认 renderer 把合批与 GPU 几何保留列为核心策略，同时要求透明内容保持正确覆盖关系。本项目先做连续兼容项合批，后续才考虑能证明正确的跨项重排。[Qt 官方 renderer](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html)

RmlUi 的 RenderInterface 是整个 RmlUi 向宿主输出绘制的接口；仅接这个接口不能自动得到适配本项目外部布局的完整绘制库。高级 mask/layer/filter 也各有独立能力和资源合同。[RmlUi 官方接口](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html)

## 样式核心：统一指定值与求值结果

建议扩展现有 Style/Appearance 的职责，先确定 typed 规则和唯一最终样式结果，再让独立样式文件、自有语言或 C++ 生产同一规则资源。这样可以拥有类似 USS 的作者体验，文件格式也能演进；不用维护文本样式和原生样式两套执行路径。

每个属性应明确类型、默认值、是否继承、变化影响。指定结构要保留“未指定”，让删除覆盖恢复下层规则；它不同于透明色、0 或空字体。class、变体和主题 token 在资源发布时校验、编译为紧凑标识，帧循环不解析文本或逐项匹配临时字符串。

建议的来源顺序为：引擎默认 → 会话主题 → 显式局部主题 → kind/role/class/state 规则 → 控件显式覆盖。每层只覆盖实际指定属性，同级按有限 specificity 与稳定规则顺序解决冲突。多 class 的添加顺序不成为隐藏的优先级。这个顺序是项目建议，不宣称与 USS 完全兼容；Unity 的真实优先级对照见[官方文档](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-uss-selector-precedence.html)。

首片选择器只需 kind、role、class、有限状态和显式作用域；字体与文本色可作为少量可继承属性，背景、边框和 padding 不自动继承。复杂后代查询、兄弟关系、`:has`、伪元素和完整 CSS 表达式会增加依赖传播成本，没有实际需求时不加入。共享颜色/长度/字体 token 足以支撑主题；参考 [Unity 变量](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-CustomProperties.html) 与 [Godot 主题变体](https://docs.godotengine.org/en/4.5/tutorials/ui/gui_theme_type_variations.html)。

结构布局继续以现有 NodeInput 为权威输入。初期样式负责控件内容 inset 和呈现；以后允许样式写 size/gap 等属性时，必须先定义它和显式 NodeInput 的覆盖关系，并在原输入生成点合并一次。不能由 style、layout 和 renderer 各自补一次 padding。

### 作者体验示意

可以提供接近 USS 的简洁写法，让工具栏、普通按钮和危险命令共享规则。下面仅用于讨论目标体验，属性名、单位与文件语法尚未定稿或实现：

```css
.button {
    padding: 4dip 8dip;
    background: token(surface-control);
    border-radius: 4dip;
}
.button.compact { padding: 2dip 6dip; }
.button.danger { background: token(danger); }
.button.danger:hover { background: token(danger-hover); }
.button:focus { border-color: token(focus); }
```

控件只声明自己具有 `button`、`compact` 或 `danger` 类；hover/focus 由现有输入状态产生。文件在加载或重载时编译为有类型的规则，主题引用也在这一阶段校验。C++ 与自有语言可以生产等价规则，共用同一求值入口。示例中的 padding 是控件内容边距，仍只向现有布局输入合并一次；圆角则依赖后续绘制原语。这样能同时改善样式作者体验和内部执行一致性。

## 状态与失效：先保证一份结果贯穿全帧

测量和绘制应使用同一最终样式、同一交互状态快照。当前状态主要改变颜色；首片建议保持 hover/pressed 等规则只改绘制属性。若以后允许 pressed 改字体或内容 padding，就必须把状态变化同时接入测量、布局和文字交互，避免在绘制阶段使用另一个尺寸。

| 实际改变 | 应做的工作 |
| --- | --- |
| 颜色/背景/状态 tint | 更新绘制参数，复用相同文字 Coverage |
| 字体、字号、字重、locale | 失效受影响的文字内容/排版，重新测量与布局，更新文字像素和交互结果 |
| padding、内容宽度 | 更新同一 NodeInput/内容框，必要时重排文字 |
| 位移/clip | 更新绘制与命中位置，不默认重栅格同一字形 |
| class/token/规则变化 | 先比较最终样式值，按属性影响失效 |
| 图片内容版本或图集位置 | 同步更新资源/UV 依赖和相关绘制几何 |

这些影响可以同时发生。局部字体差异应使用 TextStore 已有的内容版本；只有全局文字提供者策略变化才推进对应 provider 版本，不能为了一个控件字体变化清掉所有文字缓存。调用方提交样式变化，由唯一求值/更新链判断其影响。

本地分析还发现一项需要先闭合的契约：盒命中来自 accepted frame，`edit_geometry()` 却可能用当前字体、padding 和 paragraph 重新计算文字点命中。新样式尚未接受或候选 skipped 时，两者可能采用不同版本。源码分支与最小事件序列见[当前样式分析](current-styles.md)；这是源码推导出的交互风险，本次没有实机复现。

建议在现有 UiFrame 中保留同次文字排版、有效内容框与变换的不可变查询快照，文字服务仍唯一负责生成它。点命中与平台文字位置消费实际接受的快照；若快照内容和当前编辑 token 失配，明确请求新帧或暂停相关几何查询。逻辑文字编辑不因此建立另一套文本模型。颜色-only 变化不应一律暂停输入；仅拒绝旧 style_revision 也不能解决候选 prepare 已改当前 paragraph、最后却未显示的问题。

## 绘制演进：先扩大正确表达，再按负载优化

第一步可在现有 renderer 上合并连续兼容的 solid/quad：相同目标、pipeline、clip、纹理和混合状态可以共享批次。保留 painter order，明确遇到资源或状态变化就结束批次。不要按材质全局排序透明 UI，也不要为少几个 draw 牺牲 AA 或覆盖关系。

随后增加圆角盒与图片/图标原语。圆角背景和圆角子内容裁剪是两个功能；专用 rounded-rect 几何或 shader 均可实现前者，后者需要明确的 clip/mask 合同。标准控件和自定义 mesh 走同一绘制协议，颜色、DPI、抗锯齿与资源退休规则一致。复杂路径和离屏效果按真实使用需求再加入。

图片接线应沿已有[资源连接方案](../../modules/resources/design-proposal.md)的内容版本捕获与设备表示分工，该方案仍未实现。其历史语言能力描述不能替代最新 LANGUAGE；本研究只沿用资源身份、固定内容版本和完整呈现持有的方向，不新建 UI 专用业务资源身份。

几何缓存可以按控件外观/内容版本复用，位置变化尽可能只更新变换。GPU batch、纹理页与在飞退休由 renderer 管理，样式资源不能直接持有裸设备句柄。需要容量上限、资源版本与释放路径；不让 CPU/GPU 缓存永久增长，也不把“控件移除”误认为“GPU 已完成”。

字体图集属于后续有条件的变化。先观察现有段落 Coverage 的重栅格、换纹理、上传和内存是否构成瓶颈；再选择覆盖图页或字形页。两者是不同收益/成本方案，选定后完成原路径迁移，不同时建立多个长期文字 renderer。共享字形时以 font face、glyph ID、字号/DPI/栅格设置等识别，不以 Unicode 码点识别；继续让 DirectWrite layout 负责塑形、基线、选择、caret 和文字命中。

离屏子树缓存、group opacity、阴影和 backdrop-filter 需要额外目标、正确合成与失效范围。逐子项降低 alpha 不等于把有重叠内容的整组先合成再降低 alpha。没有实际需求时不先引入完整滤镜/合成系统。相关成本对照见[原生绘制研究](native-renderers.md)。

## 推荐落地顺序与验收终点

1. **样式发布与交互一致性**：先明确同次 accepted 的文字几何和样式合同，保留现有外观；主题换色复用文字，换字体更新尺寸和交互，候选失败或 skipped 保留正确旧帧。
2. **可复用样式表达**：统一规则资源，加入少量 class/变体、局部主题和 token；用真实工具栏 compact、危险命令 danger、普通参数字段验证复用。样式文件加载或热重载也生产同一资源，非法候选保留旧定义。
3. **常用图元与连续批处理**：圆角盒、边框、图片/图标和 compatible quad 合批，覆盖状态组合、裁剪、DPI、透明叠加及资源更新/退出。
4. **按真实瓶颈深化**：保留绘制几何、文字/小图 atlas、视觉参数更新；最后才是复杂路径、并行生成和离屏效果。

以上是实现建议，本次没有执行这些新增验收。以后沿现有正式 UI/编辑器/renderer 端到端入口交付开发与发布配置，使用已有 prepare、文字缓存、draw、上传和资源持有计数；不新增单元测试或 mock 框架，也不为普通改动附加常驻 benchmark 工程。

主要待决定的作者需求是样式文件热编辑是否属于首片。它影响交付顺序，不影响共有规则/求值核心。其他建议默认值为：显式结构布局优先，首片交互状态只改绘制，不承诺 USS 或完整 CSS 兼容。具体布局覆盖语义、文字快照和新绘制原语在实施前形成简短可审查合同。
