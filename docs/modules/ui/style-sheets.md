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

只有明确指定的字体 family、locale、size、weight 和文字颜色向下继承。背景、图片、内边距、圆角和边框不继承。Token 只属于所在表，解析时展开，不随控件继承。外层表仍会匹配局部作用域中的后代；scope 增加局部优先级，不隔离树。

## 支持的语法

- 单控件选择器：`button`、`*`、`.compact`、`button.compact.danger`，可用逗号共享声明。
- 种类：`container / label / button / list / item / number / splitter / viewport / text_edit / checkbox / foldout / popup / menu_item / combo / tab`。
- 角色：`:role(normal|panel|title|caption)`。
- 状态：`:enabled / :disabled / :hover / :pressed / :focus / :selected / :invalid / :modified`，可组合；enabled 与 disabled 不能共存。`modified` 来自宿主的默认值比较，与焦点、选中和非法草稿分别表达。
- `@tokens { name: value; }` 与全值 `token(name)`；支持前向引用，拒绝循环及未知 token。
- UTF-8 文本、`/* 注释 */`；每条声明必须以分号结尾。类名为不以数字开头的 ASCII 字母、数字、`_`、`-`，最长 64 字节。

悬停、按下、焦点和非法状态只改变绘制属性。modified、selected、enabled、disabled 等持久状态可以改变 font-weight，例如非默认数值加粗；其他字体属性、text-align、padding、indicator-size 仍禁止用于状态选择器。结构布局继续用 `NodeInput`；本片没有后代选择器、浏览器盒模型、百分比、表达式、动画或通用 CSS。

| 属性 | 值 |
| --- | --- |
| `color / background / border-color / selection-color / caret-color / scrollbar-color` | `#RRGGBB`、`#RRGGBBAA`、`transparent` 或 `none`；作者 RGB 是 sRGB，解析为线性颜色 |
| `font-family / font-locale` | 单引号或双引号 UTF-8 字符串，不支持转义 |
| `font-size / font-weight` | 4–256 DIP / 整数 1–999 |
| `text-align` | `left / center / right`；不继承，在同一 DirectWrite 段落内处理测量、绘制和命中 |
| `border-style` | `solid / inset / outset`；后两者为方角的明暗边缘，沿现有四边实色实例绘制 |
| `indicator-size` | 4–32 DIP，控制勾选/折叠/下拉符号，命中与文字留白沿同一最终样式 |
| `indicator-background / indicator-border-color` | 勾选框内部背景及凹入边缘颜色 |
| `padding` | 1、2 或 4 个长度；四值依次上、右、下、左，0–128 DIP |
| `border-radius` | 1、2 或 4 个长度；四值依次左上、右上、右下、左下，0–32768 DIP |
| `border-width / caret-width` | 0–128 / 0.25–16 DIP |
| `scrollbar-width / scrollbar-inset / scrollbar-min-thumb` | 0.25–32 / 0–32 / 1–128 DIP |
| `background-image` | `image(name)`、`linear-gradient(上色, 下色)` 或 `none`；name 由宿主传入 `NamedImage` 绑定 |

长度可带 `dip`，裸数也按 DIP。圆角双值分别作用于左上/右下、右上/左下。输入最多 1 MiB、1024 个块、4096 条展开规则，每个选择器最多 16 个类。原生 `StyleSheet::create` 使用相同属性与选择器校验；其诊断无文本位置，行列为零。

两色纵向渐变在解析时生成一张 1×64、256 字节的预乘 RGBA 纹理；同一表中的相同声明共享资源，稳定帧不生成像素。透明端点在预乘线性空间插值后编码为 sRGB，继续走现有 ColorImage 上传、裁剪和缓存。渐变方向固定为向下，不引入任意角度或多色标语法。

## 图片、圆角与批处理

`ColorImage` 是不可变 CPU 像素快照，包含非零 identity、revision、尺寸、行跨度及 RGBA8 字节。同一 identity/revision 不能表示不同内容；发布前放弃可变别名。图片上限每轴 16384，允许带 padding 的行。SS 不读取路径，也不解码图片，资源生产者通过 `NamedImage` 绑定已有快照。背景图片当前拉伸填满控件；底层 `DrawQuad.uv` 可选择图像区域。

图片 RGB 字节存储 **sRGB 编码后的线性预乘颜色**，alpha 字节为线性值。导入普通 straight sRGB RGBA 时，先解码 RGB 到线性，乘 alpha，再将 RGB 编码回 sRGB。例如半透明纯红约为 `{188,0,0,128}`。不能直接将普通 PNG 的 straight RGBA 字节当作本协议。R8 文字覆盖图继续使用原协议，两种资源分开标识、统计，共用上传及退休实现；尚未接入引擎 Texture 资源对象或文件解码器。

solid 盒的背景、图片及向内边框在一次绘制中合成并应用共同圆角遮罩，避免边缘重复叠色。四角独立，过大半径按相邻边和统一缩放，边框限制在较短边一半以内。圆角只作用于该盒自身，后代裁剪与输入命中仍是矩形；不宣称支持 rounded overflow 或路径裁剪。

inset/outset 使用方角背景及四个相邻边缘实例。浅边使用 border-color，深边使用其线性 RGB 的 15%，alpha 不变；两种方向交换明暗边。此模式明确使用方角，不与 border-radius 组合。焦点可切回 solid 统一细边。

renderer 只合并相邻且有效 scissor、管线、图片身份/版本相同的项，不重排。位置、UV、颜色、圆角及边框在实例数据中，可以在同一批次变化。一帧 UI 实例共用一次上传；没有新增图集或长期几何缓存。`FrameStats` 同时记录 `ui_quads`、实际 `ui_draw_calls`、`ui_instance_upload_bytes`，以及 R8 / RGBA 的上传与缓存数量/字节。缓存统计不含 gfx 在飞退休队列或物理显存。

## 发布与文字一致性

样式资源解析与校验在发布前完成。稳定控件复用最终样式，样式来源、继承、类名或状态改变才重新匹配；实际结果相同时保留原对象。测量和绘制消费同一个结果，局部字体变化不会增加全局字体 provider 版本，绘制属性变化复用段落和 R8 像素。

`UiFrame` 保有每个可编辑字段的不可变段落、文字内容、最终原点、DPI、横滚和编辑 token。指针命中使用已接受帧的段落；未接受的新字体、padding 或横滚不会移动旧画面的命中。内容已变化但尚未绘制时，指针请求重绘，不把旧 offset 应用于新草稿。连续键盘输入则用接受帧的字体/对齐/宽度策略重排当前草稿。平台输入法位置继续要求 token、组合显示版本和 Surface 匹配。

## 验收

- `ui_acceptance`：15 组生产消费者场景，包含样式解析、作用域/继承/覆盖、错误保旧、资源接线，以及接受帧文字快照。
- `renderer_acceptance`：真实 D3D12 debug/GBV 与颜色读回，包含非对称圆角、透明填充边框、RGBA 带 padding 行、图片版本与释放、合批/裁剪分界、A/B/A 透明顺序及错误帧恢复。
- `atelier_editor --acceptance <目录>`：同一产品入口验证 SS 文件接线、交互状态、文字编辑、主题切换、256 Actor 场景和 GPU 捕获。真实系统 IME 有独立验收，本增量不以普通窗口验收替代它。

2026-10-01 的双配置实测、review、滚动边距修复、旧文字接口清理及证据路径统一记录在[阶段收口](../../research/ui-rendering-20261001/closeout.md)。
