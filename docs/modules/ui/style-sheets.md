# SS（Style Sheets）

SS 是基础 UI 模块的样式资源。文本文件与 C++ 都生成同一个不可变 `StyleSheet`，经过 `Session` 的同一解析入口，得到供测量、绘制和接受帧持有的 `ResolvedStyle`。它不依赖业务语言、Runtime 或 GPU。

## 作者入口

编辑器的实际样式位于 [assets/editor.ss](../../../assets/editor.ss)。构建将它复制到可执行文件旁；应用按可执行文件位置读取，不依赖工作目录。修改源文件后构建并重启生效。缺失文件或非法内容会中止启动并报告错误；SS 解析错误包含行、列。当前没有文件监视器。

```css
@tokens {
    field: #171e29;
    accent: #569bd0;
    compact: 4dip 6dip;
}
button, number, text_edit { border-radius: 4dip; }
button.compact { padding: token(compact); }
text_edit { background: token(field); }
text_edit:focus { border-color: token(accent); }
*:role(caption) { font-size: 12dip; }
```

`Control.classes` 指定类名，`Control.style` 是逐属性的原生覆盖，`Control.style_scope` 为自身及后代附加局部样式表。`Session::set_style` 仍设置基础主题，`Session::set_style_sheet` 发布根样式表。传入空样式表恢复基础主题；删除某个 optional 覆盖恢复下层规则。透明色、零宽度、`background-image: none` 都是明确指定值。

## 优先级与继承

1. 基础 `Style`、控件种类、角色及交互状态建立默认值。
2. 继承父项明确指定的字体属性及文字颜色。
3. 从根到当前作用域依次应用样式表；内层表覆盖外层表。
4. 同一表中，类名与状态每项权重 100，种类与角色每项权重 1；权重相同则后写的规则胜出。类名排列不影响结果，重复类名去重。
5. `Control.style` 最后覆盖。

只有明确指定的字体 family、locale、size、weight、style 和文字颜色向下继承。背景、图片、内边距、圆角和边框不继承。Token 只属于所在表，解析时展开，不随控件继承。外层表仍会匹配局部作用域中的后代；scope 增加局部优先级，不隔离树。

## 支持的语法

- 单控件选择器：`button`、`*`、`.compact`、`button.compact.danger`，可用逗号共享声明。
- 种类：`container / label / button / list / item / number / splitter / viewport / text_edit / checkbox / foldout / popup / menu_item / combo / tab / slider / tree / tree_item`。
- 角色：`:role(normal|panel|title|caption)`。
- 状态：`:enabled / :disabled / :hover / :pressed / :focus / :selected / :invalid / :modified`，可组合；enabled 与 disabled 不能共存。`modified` 来自宿主的默认值比较，与焦点、选中和非法草稿分别表达。
- `@tokens { name: value; }` 与全值 `token(name)`；支持前向引用，拒绝循环及未知 token。
- UTF-8 文本、`/* 注释 */`；每条声明必须以分号结尾。类名为不以数字开头的 ASCII 字母、数字、`_`、`-`，最长 64 字节。

悬停、按下、焦点和非法状态只改变绘制属性。modified、selected、enabled、disabled 等持久状态可以改变 font-weight 和 font-style，例如非默认数值加粗；其他字体属性、text-align、padding、indicator-size 和滚动条尺寸仍禁止用于状态选择器。结构布局继续用 `NodeInput`；本片没有后代选择器、浏览器盒模型、百分比、表达式、动画或通用 CSS。

| 属性 | 值 |
| --- | --- |
| `color / background / border-color / selection-color / caret-color / scrollbar-color / accent-color` | `#RRGGBB`、`#RRGGBBAA`、`transparent` 或 `none`；作者 RGB 是 sRGB，解析为线性颜色 |
| `font-family / font-locale` | 单引号或双引号 UTF-8 字符串，不支持转义 |
| `font-size / font-weight` | 4–256 DIP / 整数 1–999 |
| `font-style` | `normal / italic`；进入同一 DirectWrite 字体身份、测量和字形缓存；缺少斜体字面时由 DirectWrite 合成 |
| `text-align` | `left / center / right`；不继承，在同一 DirectWrite 段落内处理测量、绘制和命中 |
| `text-vertical-align` | `top / center / bottom`；不继承，不能用于状态选择器；文字、选区、caret 和输入法锚点共用偏移；显式首基线布局优先 |
| `border-style` | `solid / inset / outset`；后两者为方角的明暗边缘，沿现有四边实色实例绘制 |
| `border-edges` | `all`（默认）、`none`，或不重复的 `top right bottom left` 子集；可表达共边页签及选中页签的开口底边 |
| `border-inner-color` | 方角框上侧、左侧的内沿颜色；厚度同 border-width，透明值关闭，不影响文字测量 |
| `border-shadow-color` | inset/outset 边框的独立暗边颜色；省略时沿用 border-color 推导的暗边，solid 忽略此属性 |
| `indicator-size` | 4–32 DIP，控制勾选/折叠/下拉及图标。纯图标按钮在 padding 后的内容区默认居中，空间不足时缩小 |
| `icon-align` | `left / center / right`；纯内容图标的水平对齐，默认 center，不继承，仅影响绘制。带文字图标仍遵循 icon-placement，分隔条点阵等专用标记保持自己的几何 |
| `indicator-background / indicator-border-color` | 勾选框、滑条拇指的背景与边缘颜色；最大化图标也使用 indicator-background 绘制对角底色 |
| `indicator-color` | 勾选/折叠标记及下拉箭头的颜色；缺省跟随文字前景，tree_item 的类型图标也使用此属性，其他内容图标使用 color |
| `combo-indicator` | `down`（默认）或 `arrows`；上下双箭头仍开启同一原生菜单，右侧预留宽度不变 |
| `accent-width` | 0–16 DIP，控件底部提示线厚度；0 隐藏。滑条填充色也使用 accent-color |
| `padding` | 1、2 或 4 个长度；四值依次上、右、下、左，0–128 DIP |
| `border-radius` | 1、2 或 4 个长度；四值依次左上、右上、右下、左下，0–32768 DIP |
| `border-width / caret-width` | 0–128 / 0.25–16 DIP |
| `scrollbar-width / scrollbar-inset / scrollbar-min-thumb` | 0.25–32 / 0–32 / 1–128 DIP |
| `scrollbar-gutter` | `stable`（默认）预留并裁剪滚动条槽；`overlay` 不预留，溢出时滚动条覆盖内容右侧。不允许通过交互伪状态切换 |
| `scrollbar-button-size` | 0–64 DIP；0 跟随条宽，窄于 8 DIP 的自动细条省略步进按钮 |
| `tree-row-height / tree-indent` | 12–128 / 8–64 DIP；在 tree 上统一规定行高与层级间距，不允许通过状态选择器修改 |
| `tree-alternate-color / tree-line-color` | tree 空白区交替行底色与层级连接线颜色 |
| `background-image / scrollbar-image` | `image(name)`、`linear-gradient(起色, 终色)` 或 `none`；可在两色前指定 `to right,` 或 `to bottom,`；name 由宿主传入 `NamedImage` 绑定 |

list/tree/popup 的滚动条使用 `scrollbar-color` 和可选 `scrollbar-image` 绘制滑块及可用的步进按钮，`indicator-background` 绘制轨道，`indicator-border-color` 绘制细边和点阵握柄的亮侧；暗侧使用同透明度的黑色。到达端点后相应按钮变暗。宽度和 inset 同时参与布局留白及接受帧命中；颜色仍只影响绘制。编辑器 SS 采用 16 DIP 宽度、21.333 DIP 步进按钮和 24 DIP 最短滑块。工作区及内容页签的 hover 保持原底色，选中页签以底面区分。

长度可带 `dip`，裸数也按 DIP。圆角双值分别作用于左上/右下、右上/左下。输入最多 1 MiB、1024 个块、4096 条展开规则，每个选择器最多 16 个类。原生 `StyleSheet::create` 使用相同属性与选择器校验；其诊断无文本位置，行列为零。

两色渐变在解析时生成一张纵向 1×64 或横向 64×1、256 字节的预乘 RGBA 纹理；缺省方向向下。同一表中的相同声明共享资源，稳定帧不生成像素。透明端点在预乘线性空间插值后编码为 sRGB，继续走现有 ColorImage 上传、裁剪和缓存。仅支持横纵两个方向，没有任意角度或多色标语法。

## 图片、圆角与批处理

`ColorImage` 是不可变 CPU 像素快照，包含非零 identity、revision、尺寸、行跨度及 RGBA8 字节。同一 identity/revision 不能表示不同内容；发布前放弃可变别名。图片上限每轴 16384，允许带 padding 的行。SS 不读取路径，也不解码图片，资源生产者通过 `NamedImage` 绑定已有快照。背景图片当前拉伸填满控件；底层 `DrawQuad.uv` 可选择图像区域。

图片 RGB 字节存储 **sRGB 编码后的线性预乘颜色**，alpha 字节为线性值。导入普通 straight sRGB RGBA 时，先解码 RGB 到线性，乘 alpha，再将 RGB 编码回 sRGB。例如半透明纯红约为 `{188,0,0,128}`。不能直接将普通 PNG 的 straight RGBA 字节当作本协议。R8 文字覆盖图继续使用原协议，两种资源分开标识、统计，共用上传及退休实现；尚未接入引擎 Texture 资源对象或文件解码器。

solid 盒的背景、图片及向内边框在一次绘制中合成并应用共同圆角遮罩，避免边缘重复叠色。四角独立，过大半径按相邻边和统一缩放，边框限制在较短边一半以内。圆角只作用于该盒自身，后代裁剪与输入命中仍是矩形；不宣称支持 rounded overflow 或路径裁剪。

inset/outset 使用方角背景及四个相邻边缘实例。浅边使用 border-color，深边使用其线性 RGB 的 15%，alpha 不变；两种方向交换明暗边。此模式明确使用方角，不与 border-radius 组合。焦点可切回 solid 统一细边。

方角边框的边界及非零线宽对齐物理像素，最细为一像素；编辑器使用 0.667 DIP 在当前 150% DPI 下表达一像素细边。布局、文字和接受帧命中仍保留原浮点几何。`border-edges` 的部分边使用同一 painter 的实色实例，也采用方角；`border-inner-color` 只在方角框启用。普通四边 solid/圆角继续走原单实例路径。未扩展 RHI 绘制协议，也没有按编辑器类名分支绘制。

renderer 只合并相邻且有效 scissor、管线、图片身份/版本相同的项，不重排。位置、UV、颜色、圆角及边框在实例数据中，可以在同一批次变化。一帧 UI 实例共用一次上传；没有新增图集或长期几何缓存。`FrameStats` 同时记录 `ui_quads`、实际 `ui_draw_calls`、`ui_instance_upload_bytes`，以及 R8 / RGBA 的上传与缓存数量/字节。缓存统计不含 gfx 在飞退休队列或物理显存。

## 发布与文字一致性

样式资源解析与校验在发布前完成。稳定控件复用最终样式，样式来源、继承、类名或状态改变才重新匹配；实际结果相同时保留原对象。测量和绘制消费同一个结果，局部字体变化不会增加全局字体 provider 版本，绘制属性变化复用段落和 R8 像素。

`UiFrame` 保有每个可编辑字段的不可变段落、文字内容、最终原点、DPI、横滚和编辑 token。指针命中使用已接受帧的段落；未接受的新字体、padding 或横滚不会移动旧画面的命中。内容已变化但尚未绘制时，指针请求重绘，不把旧 offset 应用于新草稿。连续键盘输入则用接受帧的字体/对齐/宽度策略重排当前草稿。平台输入法位置继续要求 token、组合显示版本和 Surface 匹配。

## 验收

- `ui_acceptance`：正式生产消费者包含样式解析、作用域/继承/覆盖、错误保旧、资源接线，以及接受帧文字快照；当前完整覆盖见[UI 验收入口](README.md#限额与验收)，不沿用 SS 首次交付时的场景数量。
- `renderer_acceptance`：真实 D3D12 debug/GBV 与颜色读回，包含非对称圆角、透明填充边框、RGBA 带 padding 行、图片版本与释放、合批/裁剪分界、A/B/A 透明顺序及错误帧恢复。
- `atelier_editor --acceptance <目录>`：同一产品入口验证 SS 文件接线、交互状态、文字编辑、主题切换、256 Entity 场景和 GPU 捕获。真实系统 IME 有独立验收，本增量不以普通窗口验收替代它。

2026-10-01 的双配置实测、review、滚动边距修复、旧文字接口清理及证据路径统一记录在[阶段收口](../../research/ui-rendering-20261001/closeout.md)。

图标文字组合支持 `icon-placement: leading | above`，默认 leading 保留已有横向排列。above 在测量时为顶部图标与 2 DIP 间距预留空间，绘制仍使用同一控件 bounds 和点击身份；`indicator-size` 控制图标尺寸，`text-align` / `text-vertical-align` 控制标题。该属性改变布局，不允许在 hover/pressed/focus 等状态规则中切换。
