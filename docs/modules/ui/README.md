# 原生UI消费与交互

`atelier_ui`提供正式文字服务和基础控件，使用现有`atelier_ui_layout`；不依赖Runtime、editor或RHI。`atelier_ui_protocol`只导出纯CPU绘制协议，renderer可以单独依赖它，不链接控件或DirectWrite。两个目标均由`cmake/ui.cmake`提供，应用在根构建include该文件后调用`atelier_add_ui()`。

## 唯一入口

[session.hpp](../../../src/ui/session.hpp)的`ui::Session`管理同一窗口UI的布局节点、内容、焦点、按压、编辑缓冲和滚动。应用按稳定key提供父节点在前的平坦`Control`描述；控件布局直接复用`ui_layout::NodeInput`。零key表示隐式overlay根。普通容器可使用现有row/column/Flex/Grid；label/button/item/number是文字叶，list是带裁剪和滚动的column，splitter/viewport是盒。viewport不自动填色。

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

label文字从内容顶部绘制，普通button/item/number在内容框内垂直居中。控件显式或经Flex父项参与首基线对齐时，其UI文字上下文沿子树继承，文字统一从内容顶部绘制，包括容器内贡献首基线的后代；避免绘制时额外居中破坏已求得的基线。这只决定文字内部放置，不改变NodeInput对齐和脱流规则。退出该上下文后恢复普通控件居中。

`Style.control_padding`只为button/item/number增加内容边距，与显式`NodeInput.padding`相加；label/container/viewport没有隐式内容边距。消费者不能重复添加相同控件内边距。测量、栅格化、选区和caret使用同一个有效内容框。border/caret/scrollbar尺寸也以DIP配置，绘制时统一转换物理像素。

- family、locale、weight、字号或角色字体比例改变：推进文字provider revision，清理对应格式/段落缓存，再通过正常布局入口测量。
- 内容或角色改变：由内容revision识别实际解析后的文字与字体；旧帧保持自己的Coverage。
- padding改变：作为同一NodeInput几何属性失效，更新文字约束；不伪造字体provider变化。
- 颜色、边框和caret外观改变：复用原有R8覆盖率，不触发字体重新测量或栅格化。

字体名称和locale要求非空、最多128个UTF-16码元且无内嵌NUL；解析后的所有角色字号为4–256 DIP，字重1–999；四边控件内边距0–128 DIP。有效Style仍可能与显式固定盒尺寸冲突，此时prepare返回布局失败，调用方可恢复Style或调整几何后重试。

## 应用与OS交接

`Event`区分物理像素指针、wheel、key命令和文本字符。应用转换Win32消息，按`Reply`处理capture acquire/release、cursor和redraw，再执行actions；不要把每个keydown当文本字符。UI区域消费事件，viewport保留给应用相机操作。

Surface的revision在客户区坐标映射或DPI变化时递增。尺寸/DPI变化取消按压和编辑、释放捕获，并暂停命中到匹配的新帧accepted；零extent表示暂停。旧帧的坐标不会按新窗口尺寸重解释。失焦/捕获丢失事件即使在暂停期间也会清理状态。

应用应先将事件交给UI，只在`consumed=false`时处理场景快捷键。数字焦点消费Delete/Backspace，不能因此删场景对象。Tab按已呈现顺序移动焦点，按钮支持Enter/Space；Enter提交数字，Esc取消，Ctrl+A全选，Ctrl+Z取消本次文字编辑。鼠标点击数字选中其文本；移动超过阈值开始连续拖调，Shift切换细调步长。仅处理数值所需ASCII字符，未实现通用IME/富文本或剪贴板服务。

## 编辑事务

数字和splitter统一输出`edit_begin → edit_preview* → edit_commit`或`edit_cancel`。应用在begin固定业务目标与原值，preview实时修改同一权威对象，commit只写一条历史，cancel恢复原值。拖动使用相邻指针的逻辑像素差，切换Shift不会将整段位移重新计价。splitter也允许负value，step/fine_step须为正；应用决定如何将该值映射到面板宽度。

文字可暂时为空或不构成有效数值；只有完整、有限且在范围内的值才输出preview。Enter/常规焦点切换提交有效编辑，非法值取消；Esc、失焦、按压/拖动中的捕获丢失、目标撤除和Surface变化取消。正常释放后的capture_lost通知不取消新进入的键盘编辑。list通过wheel滚动，子项输出select；滚动偏移和绘制/命中clip属于同一UIFrame。当前小场景列表实例化所提供的行，未自动接入大型逻辑集合；布局库现有VirtualList可由具体集合消费者使用。

## CPU绘制与文字寿命

[frame.hpp](../../../src/ui/frame.hpp)定义唯一renderer协议：有序DrawQuad、物理像素bounds、半开clip、linear straight RGBA及可选R8 Coverage。没有Coverage时为实色矩形；文字引用不可变CPU覆盖率像素。shader输出和blend必须配对，避免重复预乘。Coverage identity/revision不可重新解释成另一份像素；renderer自行管理GPU缓存、上传和在飞退休。

文字服务位于生产`text.cpp`。同一个DirectWrite布局对象提供尺寸、首基线、caret位置和栅格字形；使用Factory2灰度分析合成为整段文字R8覆盖率，不建立通用图集。格式缓存按family/locale/weight/解析后字号索引。相同内容/字体/宽度/DPI复用像素；文字/约束改变时生成新版本，已发布帧持有旧像素，Session析构不使旧帧失效。固定尺寸、未参与布局测量的可见文字也在prepare时栅格化。

所有未collapsed且成功布局的控件保留`ControlBox`、身份、父子变换及滚动位置。只有最终物理bounds与祖先clip相交时才栅格化文字、输出绘制；子树仍按正常顺序处理，不以屏外父项替代语义遍历。滚回屏内会正常生成或复用文字。此裁剪减少栅格化和绘制成本，未将描述、布局或输入遍历改为可见行数量；大型逻辑集合仍应由消费者使用现有VirtualList控制实例数。

`Session::statistics()`提供常数时间观测：`prepares/measure_calls/text_layouts/rasterizations/raster_bytes/coverage_cache_hits/culled_text_controls`为Session创建以来累计值，窗口采样使用差值。prepare和rasterization计尝试次数，text_layouts计成功创建的原生段落，raster_bytes计完成的R8像素字节。`provider_revision`是版本标识，不是数量。

`live_coverages/live_coverage_bytes`是读取时仍被文字缓存、当前/旧帧或renderer持有的唯一CPU覆盖率数量和R8像素字节，同一像素的多个引用不重复计数，不包含DirectWrite对象、临时栅格数据、容器容量或GPU内存。已访问的屏外行可能继续被CPU缓存持有；内容移除或变化清理缓存后，旧帧仍可延长像素寿命。独立共享记账对象随最后Coverage释放，不回指Session，也不为计数保活像素。Session仍要求单拥有者访问；外部持有者释放采用原子记账。

一手接口依据：[自定义文字绘制](https://learn.microsoft.com/en-us/windows/win32/directwrite/how-to-implement-a-custom-text-renderer)、[Factory2字形分析](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritefactory2-createglyphrunanalysis)。核心layout保持无平台依赖；Windows/DirectWrite仅位于UI文字实现。

## 限额与验收

单Session最多10,000个控件、布局深度128；Surface每轴最多32,768物理像素，DPI倍率0.25–8；每段文字最多4,096个UTF-16码元，单张CPU覆盖率最多16MiB。数字输入缓冲最多64个ASCII字符。可恢复错误通过Result返回，内存分配失败正常传播。Session单拥有者线程；发布后不修改UiFrame/Coverage，也不保留可变别名。

正式`ui_acceptance`目标运行[真实消费者](../../../acceptance/ui/main.cpp)，8组场景覆盖实际中文/英文灰度文字与缓存复用、旧帧寿命、候选/已接受几何、DPI/零尺寸暂停、按钮、数字文本及连续拖调/细调/取消、列表滚动、负值splitter、捕获丢失、控件key复用，以及Style事务、失效、资源计数和真实字形基线对齐。256行/96 DIP窗口首屏栅格化4行、裁剪252行，滚到末尾再返回累计栅格化8行；嵌套屏外控件保留几何，最后像素引用释放后live计数归零。检查在Dev和Release均有效。真实窗口和GPU消费由[编辑器端到端验收](../editor/README.md)负责，模块验收不代替窗口结果。

当前模块验收只使用本worktree源码和项目固定工具链。根构建接线由应用负责人统一提交；未通过跨工作树绝对路径include或链接替代集成。
