# 原生UI消费与交互

`atelier_ui`提供正式文字服务和基础控件，使用现有`atelier_ui_layout`；不依赖Runtime、editor或RHI。`atelier_ui_protocol`只导出纯CPU绘制协议，renderer可以单独依赖它，不链接控件或DirectWrite。两个目标均由`cmake/ui.cmake`提供，应用在根构建include该文件后调用`atelier_add_ui()`。

## 唯一入口

[session.hpp](../../../src/ui/session.hpp)的`ui::Session`管理同一窗口UI的布局节点、内容、焦点、按压、编辑缓冲和滚动。应用按稳定key提供父节点在前的平坦`Control`描述；控件布局直接复用`ui_layout::NodeInput`。零key表示隐式overlay根。普通容器可使用现有row/column/Flex/Grid；label/button/item/number/text_edit是文字叶，list是带裁剪和滚动的column，splitter/viewport是盒。viewport不自动填色。

应用拥有业务值、撤销和重做，控件状态不持有业务对象。数字编辑只保存这次操作的起始值、文本缓冲与预览值；每次新场景投影不会覆盖尚未提交的文字。参数控件key应包含业务对象key与轴，切换绑定时更换控件key。控件删除再出现会获得新incarnation，旧帧命中不能误派给重用key。

调用顺序：

1. `set_surface({backbuffer_extent,dpi_scale,revision})`，应用处理返回的取消/释放捕获动作。
2. `set_controls(controls)`，应用处理因目标移除、隐藏或禁用而产生的取消动作。
3. `prepare()`得到拥有式`shared_ptr<const UiFrame>`；失败仍可保有上一完整帧。
4. renderer同步消费`*frame->drawing`，应用持有整个帧至返回。
5. 仅renderer最终accepted后调用`accept(frame)`；skipped不调用。失败进入渲染终态时停止交互，不能声称画面回滚。

`UiFrame::find(key)`返回同次帧的物理像素bounds/clip，供场景viewport和真实窗口验收使用。候选prepare不会改变命中几何。输入始终使用已接受帧，依绘制逆序命中并检查当前控件身份与祖先启用状态。

## Style与失效

[style.hpp](../../../src/ui/style.hpp)的`Style`是唯一呈现配置，由`Session::set_style`完整校验、复制后应用；非法值返回`invalid_style`且不修改任何配置。`style()`读取当前配置；更换配置后仍通过`prepare → renderer accepted → accept`发布。候选失败保留已接受帧。`UiFrame`记录本帧的`style_revision`和`measure_provider_revision`，旧帧颜色、几何与文字像素不会跟随当前Style改变。

控件只选择`Role::normal/panel/title/caption`，不再单独设置字体或背景。一个私有状态解析入口处理normal、hover、pressed、focus、selected、disabled、invalid。字体配置包括family、locale、weight和DIP字号；title/caption通过比例表达层级，title另有字重。未知系统字体采用DirectWrite的字体匹配行为。

label文字从内容顶部绘制，普通button/item/number/text_edit在内容框内垂直居中。控件显式或经Flex父项参与首基线对齐时，其UI文字上下文沿子树继承，文字统一从内容顶部绘制，包括容器内贡献首基线的后代；避免绘制时额外居中破坏已求得的基线。这只决定文字内部放置，不改变NodeInput对齐和脱流规则。退出该上下文后恢复普通控件居中。

`Style.control_padding`只为button/item/number/text_edit增加内容边距，与显式`NodeInput.padding`相加；label/container/viewport没有隐式内容边距。消费者不能重复添加相同控件内边距。测量、栅格化、选区和caret使用同一个有效内容框。border/caret/scrollbar尺寸也以DIP配置，绘制时统一转换物理像素。

- family、locale、weight、字号或角色字体比例改变：推进文字provider revision，清理对应格式/段落缓存，再通过正常布局入口测量。
- 内容或角色改变：由内容revision识别实际解析后的文字与字体；旧帧保持自己的Coverage。
- padding改变：作为同一NodeInput几何属性失效，更新文字约束；不伪造字体provider变化。
- 颜色、边框和caret外观改变：复用原有R8覆盖率，不触发字体重新测量或栅格化。

字体名称和locale要求非空、最多128个UTF-16码元且无内嵌NUL；解析后的所有角色字号为4–256 DIP，字重1–999；四边控件内边距0–128 DIP。有效Style仍可能与显式固定盒尺寸冲突，此时prepare返回布局失败，调用方可恢复Style或调整几何后重试。

## 应用与OS交接

`Event`区分物理像素指针、wheel、key命令、拥有的UTF-16文字、剪贴板结果、IME命令结果和组合快照。`Event.data`只承载本次事件的数据；不把keydown当成字符，也不把孤立的WM_CHAR代理项直接传给UI。`Reply.effects`是唯一有序`variant<Action,PlatformRequest>`列表；应用执行effects并处理capture/cursor/redraw。`Reply.error`报告可恢复的输入/平台错误。

Surface的revision在客户区坐标映射或DPI变化时递增。尺寸/DPI变化取消手势、草稿和组合、释放捕获，并暂停命中直到匹配的新帧accepted；零extent暂停。控件移除、禁用、collapsed或改变kind取消旧编辑；旧帧不能把旧控件类型解释成新类型。失焦/捕获丢失在暂停期间也可清理状态。正常ReleaseCapture产生的通知不取消刚进入的文字编辑。

## 共享单行编辑

`text_edit`以`Control.text`作为权威投影，number保留value/min/max及拖调策略；两者共用[TextEdit](../../../src/ui/text_edit.hpp)的唯一草稿、original、caret/anchor、选择和有界历史。number只增加完整有限数值解析、范围和格式化。输入文字本身不使用ASCII白名单：非法数字可以保留为invalid草稿，不写业务preview。

focus、文字模式和编辑事务分开。number普通拖动仍在超过原指针阈值后调值，Shift细调；单击进入文字模式后，拖动用于选择文字。仅聚焦时Enter或普通文字输入可进入文字模式。text_edit聚焦即可输入。移动caret、选择文字和进入模式均不建立事务。

- 首次实际文字变化产生`edit_begin(original)`，随后有效草稿产生`edit_preview`；普通文字每次修改均可预览，IME预编辑不发预览。
- 无事务时Ctrl+Z/Y（含Ctrl+Shift+Z）返回未消费，由应用历史处理。存在本地事务时只操作本地历史，即使undo回原文或当前方向没有历史也不穿透。
- 合法Enter提交并结束本地事务；非法Enter保留草稿、焦点和文字模式。Esc取消整次字段编辑。结束后保留focus、清历史和待处理平台请求并换generation；number退出文字模式。按住Enter/Esc的repeat及其WM_CHAR不能重复提交或切模式。
- 普通换焦点合法提交、非法取消；OS失焦、Surface失效、控件失效取消。数值拖调提交后回仅聚焦，下一次Ctrl+Z交业务。仅聚焦时新的权威投影更新被动缓冲，不能被旧文字覆盖。
- 字段聚焦时Delete/Backspace由共享文字编辑消费，不能删除场景对象。Tab按可见呈现控件顺序导航，Ctrl+A全选，Left/Right按视觉方向移动或折叠选择，Shift扩选，Home/End到视觉行端。

Action.value为`variant<monostate,double,wstring>`：activate/select无值，number/splitter编辑为double，text_edit为wstring。begin/preview/commit/cancel继续沿应用的同一事务消费路径；cancel携带原值。业务派发失败停止同组剩余普通effects，仍须调用统一取消和IME/capture清理，保留首错。剪贴板失败则保留草稿、选择和历史供重试。

本地历史保存文字和选择快照；连续同类插入、连续Backspace或连续Delete可合组，导航/选择/pointer/快捷键/平台操作断组，无计时阈值。cut、paste和每次IME确认各占一组。undo+redo总计最多64组、历史UTF-16文字最多64KiB；淘汰最老完整组，original独立保留保证Esc可恢复。text_edit为4096码元、number为64码元；超长插入、孤立代理项、控制字符或换行整笔拒绝。

索引为UTF-16码元，编辑端点来自系统ICU的extended grapheme边界。DirectWrite同一layout负责点命中、带视觉affinity的caret和`HitTestTextRange`多矩形选择，不能用两个x表示双向选区。Windows SDK的`icu.h/icu.lib`仅为UI私有依赖，不新建文字渲染器或系统控件镜像。当前实机报告ICU 72.1、Unicode 15.1；覆盖非BMP代理对、组合重音、variation selector、明确ZWJ序列和双向混排，不据此宣称所有Unicode版本与字体均已验收。一手依据：[Windows ICU](https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-)、[ICU边界分析](https://unicode-org.github.io/icu/userguide/boundaryanalysis/)。

## 剪贴板、组合与平台位置

平台由Window/Application持有，UI不持HWND/HIMC。剪贴板请求含kind、唯一operation_id及`EditToken{key,incarnation,generation,revision}`；只结算匹配的当前请求一次。文字或普通caret/selection变化推进revision；结束/重绑上下文换generation。cut仅在匹配成功结果后删除请求时选区；paste成功沿统一插入。失败、重复、过期结果不改草稿，同revision不同operation也不能误配。

IME使用独立composition_id和固定起始token/替换范围；`CompositionEvent`的start/update/end/cancel以及可选result/preedit区分缺失、空和取消。Preedit拥有text/cursor/逐码元属性，只作暂态文字和选区/下划线显示；确认result进入普通编辑与历史。同事件可先确认一个片段，再开始新preedit；后段绑定更新后的token。UI请求complete/cancel的成功回报只代表平台调用成功，不能凭此合成文字。IME期间Enter/Esc先确认/取消组合，不同时提交/取消字段，终止键重复及keyup保持归属；普通焦点或选择变化取消未确认组合。结束/取消使旧绑定失效，迟到事件不能复活原组合。

`UiFrame.caret`保存该帧的token、客户区物理Rect和clip。`Session::text_input()`给出当前输入token/是否组合，只有当前身份、焦点、token、Surface及组合编号/显示版本匹配时才附带已接受帧的caret；候选prepare不改变平台位置。应用据此更新系统候选位置，窗口消息逐条复制、顺序投递和执行effects，不重入UI，也不把整个poll盖上同一旧revision。

`UiFrame.input_anchor`在同一次prepare中记录`TextInputAnchor{token,composition_id,composition_revision,bounds,clip}`。无组合时起点为选区的逻辑最小端点，空选区保留当前插入affinity；组合中为`c.first`，不取显示光标的`c.first + preedit.cursor`。同一DirectWrite布局提供字符左上与行高，RTL使用字符矩形左上，不将其右侧leading caret当左上。bounds和有效text_clip随绘制采用相同父变换、横滚和DPI，均为客户区物理像素。

`Session::text_input().accepted_anchor`只提供当前匹配的已接受快照，起点不可见、目标或Surface失效、组合更换、临时preedit/cursor已变化但尚未接受新帧时为空。临时组合显示版本独立于基础EditToken revision；旧UiFrame仍可被持有，但不能作为当前位置。平台在原生消息改变组合或坐标映射时先撤下缓存，再依UI顺序处理与accepted发布更新；WndProc只读取拥有的这个小值，不回调UI，也不创建第二文字模型。

本轮只支持[IMR_QUERYCHARPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/imr-querycharposition)实测需要的`dwCharPos=0`；Window把bounds左上和clip转换成屏幕坐标，填[IMECHARPOSITION](https://learn.microsoft.com/en-us/windows/win32/api/imm/ns-imm-imecharposition)的pt、cLineHeight和rcDocument。其他offset或无有效快照明确返回0，不夹到offset0，也不以显示caret冒充起点；没有预建逐字符坐标数组。此合同修复本身不证明真实输入法的组合链已经通过。

UI验收中的组合事件与剪贴板结果是生产协议注入，用于失败和过期边界；真实CF_UNICODETEXT、本机搜狗输入法的系统确认/取消/候选位置由应用窗口验收负责，不能由本模块的合成事件替代。当前真实IME验收尚未通过；验收结论只覆盖实际记录的输入法与环境。

## CPU绘制与文字寿命

[frame.hpp](../../../src/ui/frame.hpp)定义唯一renderer协议：有序DrawQuad、物理像素bounds、半开clip、linear straight RGBA及可选R8 Coverage。没有Coverage时为实色矩形；文字引用不可变CPU覆盖率像素。shader输出和blend必须配对，避免重复预乘。Coverage identity/revision不可重新解释成另一份像素；renderer自行管理GPU缓存、上传和在飞退休。

文字服务位于生产`text.cpp`。同一个DirectWrite布局对象提供尺寸、首基线、caret位置和栅格字形；使用Factory2灰度分析合成为整段文字R8覆盖率，不建立通用图集。格式缓存按family/locale/weight/解析后字号索引。相同内容/字体/宽度/DPI复用像素；文字/约束改变时生成新版本，已发布帧持有旧像素，Session析构不使旧帧失效。固定尺寸、未参与布局测量的可见文字也在prepare时栅格化。

所有未collapsed且成功布局的控件保留`ControlBox`、身份、父子变换及滚动位置。只有最终物理bounds与祖先clip相交时才栅格化文字、输出绘制；子树仍按正常顺序处理，不以屏外父项替代语义遍历。滚回屏内会正常生成或复用文字。此裁剪减少栅格化和绘制成本，未将描述、布局或输入遍历改为可见行数量；大型逻辑集合仍应由消费者使用现有VirtualList控制实例数。

`Session::statistics()`提供常数时间观测：`prepares/measure_calls/text_layouts/rasterizations/raster_bytes/coverage_cache_hits/culled_text_controls`为Session创建以来累计值，窗口采样使用差值。prepare和rasterization计尝试次数，text_layouts计成功创建的原生段落，raster_bytes计完成的R8像素字节。`provider_revision`是版本标识，不是数量。

`live_coverages/live_coverage_bytes`是读取时仍被文字缓存、当前/旧帧或renderer持有的唯一CPU覆盖率数量和R8像素字节，同一像素的多个引用不重复计数，不包含DirectWrite对象、临时栅格数据、容器容量或GPU内存。已访问的屏外行可能继续被CPU缓存持有；内容移除或变化清理缓存后，旧帧仍可延长像素寿命。独立共享记账对象随最后Coverage释放，不回指Session，也不为计数保活像素。Session仍要求单拥有者访问；外部持有者释放采用原子记账。

可编辑字段保持完整文字排版，灰度纹理只生成当前内容窗口内的像素。横滚偏移和可见裁剪进入该字段的raster缓存键；4096个宽字符在100 DIP字段中也不会分配整条屏外位图。选择、字素导航和caret仍针对完整原文。边界由[CreateAlphaTexture](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwriteglyphrunanalysis-createalphatexture)的指定纹理矩形实现，不在生成整张大图后裁剪。

一手接口依据：[自定义文字绘制](https://learn.microsoft.com/en-us/windows/win32/directwrite/how-to-implement-a-custom-text-renderer)、[Factory2字形分析](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritefactory2-createglyphrunanalysis)。核心layout保持无平台依赖；Windows/DirectWrite仅位于UI文字实现。

## 限额与验收

单Session最多10,000个控件、布局深度128；Surface每轴最多32,768物理像素，DPI倍率0.25–8；每段文字最多4,096个UTF-16码元，单张CPU覆盖率最多16MiB。单行text_edit缓冲最多4096个UTF-16码元，number最多64个；字素边界和完整数值解析分别负责编辑与业务有效性。可恢复错误通过Result返回，内存分配失败正常传播。Session单拥有者线程；发布后不修改UiFrame/Coverage，也不保留可变别名。

正式`ui_acceptance`目标运行[真实消费者](../../../acceptance/ui/main.cpp)，13组场景覆盖实际中文/英文灰度文字、Style失效/候选失败保旧、DPI/暂停、旧帧寿命、数字拖调、控件身份、列表可见性，以及共享编辑的焦点/事务政策、合组与数量/字节预算、Unicode删除与双向选择、4096码元横滚、accepted caret、剪贴板身份/失败/过期和组合结果/取消/清理顺序，以及正反向选区、RTL组合起点、横滚不可见起点拒绝、显示版本/候选/旧帧和DPI失效。Dev/Release均有效；组合协议注入不算真实系统输入法通过。

S1的256行/96 DIP窗口首屏仍栅格化4行、裁剪252行，滚尾再返回累计栅格化8行。S2的4096宽字符/100 DIP字段保持完整编辑，生成的覆盖率宽度不超过100像素。正式窗口平台与搜索业务由[编辑器端到端验收](../editor/README.md)负责，S2整体是否完成以该集成记录及本机搜狗真实系统输入验收证据为准。

当前模块验收只使用本worktree源码和项目固定工具链。根构建接线由应用负责人统一提交；未通过跨工作树绝对路径include或链接替代集成。
