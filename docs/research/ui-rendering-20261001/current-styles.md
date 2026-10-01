# SS 实施前的 UI 样式、状态与发布链

历史基线：`8d774cf`。下文保留研究时的代码事实与行号；后续实现已替换相关结构，当前合同见 [SS 文档](../../modules/ui/style-sheets.md)。

日期：2026-10-01。范围：`src/ui` 的 Style、Session、文字服务，以及 Workspace 对它们的消费；不评估 renderer。本文基于源码静态追踪，未运行构建、测试或 GUI。源码链接从本文所在目录回到仓库，行号在文字中标注。

## 结论

当前已有完整的 typed theme：同一 `Session::set_style` 配置、同一 `resolve_style` 解析角色和状态、同一 TextStore/布局/绘制链。它不是无样式的 UI，也还不是 Unity UI Toolkit 式的 class/stylesheet/cascade 系统。最自然的演进是扩展当前样式解析输入和结果，继续将结构几何送入现有 LayoutDocument；无需引入第二棵 UI 树或第二个 Runtime。

绘制快照与旧 Coverage 的寿命已有保障，但文字交互尚未完全绑定 accepted 样式：`edit_geometry()` 使用当前 Style 和当前 TextStore，而盒命中使用 accepted frame。因此换字体或 padding 后、新帧尚未接受时可能出现旧画面与新文字点命中的混合。应先明确并补齐这个发布契约，再扩展局部样式。

## 已有配置与统一状态解析

公共数据见 [style.hpp](../../../src/ui/style.hpp)，第9–37行。

| 配置 | 当前字段与默认值 |
|---|---|
| Role | normal、panel、title、caption |
| Font | family=`Microsoft YaHei UI`、locale=`en-US`、size=14 DIP、weight=400 |
| 角色字体 | title_scale=9/7、caption_scale=6/7、title_weight=600 |
| Palette | text、muted、panel、button、field、splitter、list、selected、hover、pressed、border、focus、invalid、selection；均为线性 RGBA |
| 内容边距 | control_padding={6,4,6,4} DIP |
| 线宽 | border_width=1、caret_width=1 DIP |
| 滚动条 | width=3、inset=1、min_thumb=12 DIP |

`Style` 为每个 Session 持有的完整值。`Control` 只提供 Role、enabled、selected 和显式 `NodeInput` 几何，没有独立 font/background/style 字段，见 [session.hpp](../../../src/ui/session.hpp) 第18–29行。当前不存在 class 集合、stylesheet 引用、选择器、样式属性优先级、声明级覆盖、样式变量和可继承字体属性。

私有 [style.inl](../../../src/ui/style.inl) 第2–11、34–64行定义 `Appearance`、`VisualState` 和唯一 `resolve_style`：

- 基础前景、caret 与 selection 来自 palette；title 改字号和字重，caption 改字号与 muted 前景。
- panel role 为非 viewport 设置 panel 背景；button/number/text_edit/splitter/list 的 kind 背景随后覆盖它。
- button/item/number/text_edit 有主题内容 padding；button/number/text_edit/splitter 有基础 border_width。border 是盒内绘制，不改变布局尺寸。
- selected 只作用 button/item；hover 作用 button/item/splitter 背景，但 selected 抑制该 hover。number/text_edit 的 hover 改 border。
- pressed 覆盖 button/item/splitter 背景。focus 改 border，item 的 focus 还开启边框。invalid 最后覆盖 border，因此 invalid 优先于 focus。
- disabled 只将前景和 caret 置 muted，并停止应用 hover/pressed/focus/invalid；基础背景和 selected 背景仍保留。

这些状态有统一解析入口，但其属性作用范围按控件语义而不同。不能把“统一”解释为所有 kind 必须绘制同一边框。例如 label 不具有基础 border_width，focus 单独改 border 色也不会让它自动产生边框。invalid 目前由活动数字编辑是否合法推导，未公开为一般控件语义；见 [session.cpp](../../../src/ui/session.cpp) 第369–371行及 [session_edit.inl](../../../src/ui/session_edit.inl) 第15–18行。

父级 enabled 在 prepare 中沿已生成父盒相与；输入又检查当前所有祖先 enabled/collapsed。它是交互状态传播，不是样式 cascade。首基线文字放置上下文也会沿子树继承，是对布局结果的消费政策，不能当作字体继承；见 [session.cpp](../../../src/ui/session.cpp) 第149–158、348–355行。

## set_style 到最终消费

1. [Session::set_style](../../../src/ui/session.cpp) 第208–220行完整校验，相等值直接返回；先复制候选，分配完成后移动应用，推进 style_revision。非法样式不会改变当前值。校验包括名称、所有角色解析字号、字重、padding、线宽、滚动条和颜色，见 [style.inl](../../../src/ui/style.inl) 第13–29行。
2. 字体政策变化通过 `same_font_policy` 检测，推进 TextStore provider_revision；[text.cpp](../../../src/ui/text.cpp) 第199–203行清理格式和所有段落缓存。颜色或线宽变化不推进 provider。
3. prepare 的 synchronize 逐控件调用 input_for：解析 Role/kind，将主题 padding 与显式 NodeInput.padding 相加，文字叶变为 measured，TextStore::set 返回内容 revision；见 [session.cpp](../../../src/ui/session.cpp) 第147–170、174–199行。角色/内容/实际字体变化由 TextStore 第188–195行推进内容版本、清掉该条目的段落。
4. 现有 LayoutDocument 接收相同节点的 update/create，然后 `compute(viewport / dpi_scale, provider_revision)`；测量回调校验 content/provider revision，并调用同一 DirectWrite paragraph，见 Session 第309行、[text.cpp](../../../src/ui/text.cpp) 第210–218行。padding 是几何输入失效，不伪装成字体服务变化。
5. prepare 将几何转为物理像素，处理父变换、裁剪、滚动和有效 enabled，再以同一 resolver 的 VisualState 生成背景/边框。文字 metrics、raster、selection、caret 共用同一内容宽度和有效 padding；caret 宽度、scrollbar 尺寸此时按 DPI 转换；见 Session 第346–459行。
6. UiFrame 携带 drawing、ControlBox、style_revision、measure_provider_revision、caret、input_anchor。只有应用在 renderer 最终接受后调用 accept，才替换 published；候选 prepare 不发布命中盒。见 [session.hpp](../../../src/ui/session.hpp) 第67–75行及 Session 第466–472行。

文字测量、caret、range 和点命中均调用 TextStore::resolve，依赖同一 DWrite layout，而不是另一个字符宽度模型；见 [text.cpp](../../../src/ui/text.cpp) 第135–163、257–270行。OS 鼠标 cursor 不属于 Style，按命中控件 kind、编辑模式及 splitter 轴生成 text/drag/resize，见 [session_input.inl](../../../src/ui/session_input.inl) 第69–73行。

## accepted frame 与资源保护：已有保障和实际边界

已有保障：

- [frame.hpp](../../../src/ui/frame.hpp) 第28–53行要求 Coverage identity/revision 不得对应不同像素，发布后资源和几何不可变。DrawQuad 拥有 `shared_ptr<const Coverage>`，旧帧持有旧像素，当前 TextStore 清缓存不会改变旧帧。
- [text.cpp](../../../src/ui/text.cpp) 第23–35行每个 Coverage 获得唯一 identity，revision=1，并通过共享 accounting 记录实际存活期；raster 缓存依赖 paragraph、DPI 和 viewport（第220–254行）。颜色独立于 R8 coverage，因此颜色更换可以复用覆盖率。
- active/hit 只使用 surface extent/DPI/revision 相符的 published frame，并逆序检查盒、clip、当前 incarnation/kind 和祖先有效性，见 [session.cpp](../../../src/ui/session.cpp) 第122–145行。Surface 变化取消编辑/手势并使旧帧命中暂停，见第228–235行。
- accepted caret/IME anchor 来自 published，另校验 EditToken 和 composition id/revision，见第479–493行。

未完成的契约：

- style_revision/provider_revision 在 UiFrame 中是版本记录，`accept()` 不将它们与当前配置比较。它检查 Session、Surface、帧 ID 顺序，不拒绝由较旧 Style 产生的候选。因此不能声称它们已经是“当前样式版本发布屏障”。
- `set_style` 不暂停命中，不取消当前编辑。文字编辑的 [edit_geometry](../../../src/ui/session_edit.inl) 第103–114行取 accepted 盒，却使用当前 Role/Style/padding，并重新 `text.set` 当前字体；第116–120行以它点命中。旧帧绘制资源安全不等于文字交互已经快照一致。
- prepare 也会更新当前 TextStore；若候选最终失败或 skipped，旧画面可以继续保留，但当前 paragraph 可能已变。这个风险不限于单纯 set_style。
- `text_input()` 校验 token/组合版本，却不校验样式版本；旧 anchor 的坐标仍准确描述旧画面，但后续文字选择可能采用新解析结果。应明确这是待处理的一致性边界，而不是资源悬空问题。

最小真实事件序列：先显示并接受一个获得焦点的 text_edit 帧；保持 Surface 与控件身份不变；调用 set_style 增大 font.size 或 control_padding；在新帧尚未被 renderer 接受时发送字段内 pointer_down 或 pointer_move 拖选。盒命中进入旧 published frame，select_at 则进入上述当前样式分支。也可以先 prepare 新样式候选但最终 skipped，再发送该输入。源码确定这两条查询采用不同版本来源；“用户点击的字符与被选择字符可能不同”是由该分支推出的交互风险，本文未作实机复现，未断言每种文字和点位必然失败。

颜色-only set_style 同样不会暂停交互，但 font/provider/padding 不变时 edit_geometry 的几何语义也不因本次颜色变化而变化。建议的版本门禁应区分度量/文字交互版本与纯绘制外观版本，避免仅因 style_revision 不同而无条件禁用所有输入。

## Layout 与 UI 样式职责

[ui-layout 合同](../../modules/ui-layout/contract.md) 第7、31–49行以及 [责任表](../../modules/ui-layout/README.md) 第31–38行规定 LayoutDocument 负责有序树、结构输入、尺寸分配、测量缓存及逻辑几何；不负责主题、shaping、滚动、裁剪或 renderer。当前 Session 已正确作为消费适配层：将样式中影响测量的 padding/字体映射为 NodeInput 与内容/provider 版本，再消费结果完成像素、状态与输入。

Workspace 的 [describe](../../../src/editor/workspace.cpp) 第34–154行只投影业务：panel/title/caption Role、按钮 enabled、列表项 selected，以及明确的行列、宽高、gap 和容器 padding。它不复制调色板、不直接绘制文字、不建立局部 style resolver。这与 [ui-foundation-plan](../../ui-foundation-plan.md) 第66–72行已落地的 S1 合同一致；计划中的后续菜单/参数表达不得被当成当前已完成能力。

## 建议：保持一个入口扩展

第一优先是让文字交互消费同次 accepted 的排版语义。推荐在现有 UiFrame 的可编辑控件快照中保留不可变 paragraph 引用及有效内容框/滚动信息，TextStore 继续唯一生成 DWrite layout；点命中、视觉移动、caret 通过该 accepted 句柄查询。基础编辑事务可以保持当前值，但交互不得把当前样式拼到旧盒。仅保存样式 revision 或禁止接受旧候选不足以覆盖候选 prepare 已改变 TextStore、却最终 skipped 的情况。应由同一发布入口负责，不增加第二套文字模型。

同一快照也可记录必要的 computed style 与文字交互版本，由输入入口明确判定哪些查询仍与当前编辑值匹配；确需失配时才暂停相关文字查询、请求重绘。颜色状态变化继续允许旧 accepted 几何承接输入。具体采用快照保留还是局部失配门禁，需要在实现前明确契约和失败保旧行为。

接着才扩展样式表达。若实际页面确实需要，给 Control 增加紧凑的 style/class 标识，Session 持有经过校验的 typed rules，仍由唯一 resolver 产生完整 ResolvedStyle；可先支持 kind、role、class、state 与有限局部覆盖，不必实现浏览器全部选择器。定义清楚优先级及哪些字体属性继承，结果将布局属性送回现有 NodeInput，绘制属性送到现有 DrawQuad。class 本身不建立控件或布局树。

失效按最终解析差异划分：颜色/背景变化仅重建绘制值；字体/文字排版变化推进测量内容或 provider 版本；padding/尺寸规则变化更新同一节点几何；输入快照随同一 accepted frame 发布。当前 state 不改变字体和 padding，因此可以在不重新测量的情况下更新状态外观；今后若规则允许 state 改变度量，必须把状态纳入测量解析与失效链，不能只在 painting resolver 处理。

现阶段不建议直接加入 CSS 解析器、通用 selector engine、动画系统或全量继承。typed rules 的稳定 ID、共享不可变规则和缓存后的 ResolvedStyle 足以服务实需；避免每帧字符串匹配和每控件复制整份 Style。尚未测量扩展后的成本，不将这些建议写成已证实的性能收益。
