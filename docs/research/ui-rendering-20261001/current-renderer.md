# SS 实施前的 UI 绘制链路

下文保留研究时的代码事实与行号；后续已接入圆角、图片和相邻合批，当前合同见 [SS 文档](../../modules/ui/style-sheets.md)。

日期：2026-10-01。代码基线：`8d774cf82b30d320a246246853e3359a4f6c83e9`。本文为源码分析，没有启动窗口、GPU 验收或新的性能采样。

## 已有实现

应用在同一帧中投影控件，调用 `ui::Session::prepare`，把绘制快照交给正式 `renderer::Renderer::render`，最终结果 accepted 后再调用 `Session::accept` 更新输入命中帧。入口见 [application.cpp](../../../src/application/application.cpp) 308–346 行、[session.cpp](../../../src/ui/session.cpp) 302–471 行。这是已有控件身份、布局与编辑状态配合每帧绘制快照的结构；不是直接在输入事件里执行 GPU 绘制。

UI 使用现有 `ui_layout`，renderer 只依赖 `atelier_ui_protocol`，不链接控件、DirectWrite、World 或 Runtime。协议与构建职责见 [frame.hpp](../../../src/ui/frame.hpp)、[ui.cmake](../../../cmake/ui.cmake)、[renderer 当前合同](../../modules/renderer/contract.md)。这一分工可以继续承载样式扩展，不需要替换布局或对象系统。

`ui::Frame` 的 `draws` 是按顺序绘制的 `DrawQuad` 数组。每项包含物理像素 bounds、半开矩形 clip、线性 straight RGBA、UV，以及可选的不可变 R8 `Coverage`。没有 Coverage 时画实色矩形；有 Coverage 时用覆盖率着色。后面的项覆盖前面的项，透明内容不能随意按纹理全局排序。协议见 [frame.hpp](../../../src/ui/frame.hpp) 19–61 行。

Session 在 prepare 时输出背景、边框、选区、文字、组合下划线、caret 与滚动条。边框现在由四个实色矩形构成，文字另占一个覆盖图四边形；祖先矩形逐层裁剪子项。完全不相交的文字控件保留布局/命中盒，但不生成本帧文字像素和绘制项。见 [session.cpp](../../../src/ui/session.cpp) 337–459 行。控件描述与布局仍遍历实际实例；减少逻辑长列表实例数应使用现有 `VirtualList`，不能把绘制裁剪称为完整虚拟化。

renderer 先绘制场景，再消费 UI 数组。UI vertex shader 通过 `SV_VertexID` 产生六个顶点，每项设置 bounds、UV、颜色、scissor 与 pipeline，然后调用一次 `gfxCommandDraw(gfx, 6)`。因此当前 UI 没有跨项几何批处理，draw 数基本随可见绘制项增加。见 [renderer.cpp](../../../src/renderer/renderer.cpp) 304–371 行、[ui_vs.hlsl](../../../src/renderer/shaders/ui_vs.hlsl)。

UI 不写场景深度。shader 把 alpha 与 coverage 相乘，输出预乘颜色，混合使用 ONE / INV_SRC_ALPHA；SDR backbuffer 为 sRGB RTV，使颜色混合发生在线性空间。见 [renderer.cpp](../../../src/renderer/renderer.cpp) 235–258 行、[solid_ps.hlsl](../../../src/renderer/shaders/solid_ps.hlsl)、[coverage_ps.hlsl](../../../src/renderer/shaders/coverage_ps.hlsl)。扩展图片、图集或新材质时要保留这套约定。

## 文字与缓存

DirectWrite 的同一文字布局负责尺寸、基线、字形排版、文字点命中、caret 和选择矩形。栅格实现通过 `IDWriteGlyphRunAnalysis` 生成灰度像素，再把字形 run 合成为整段 R8 `Coverage`。见 [text.cpp](../../../src/ui/text.cpp) 64–85、135–185、220–255 行。这已把复杂排版留给成熟平台实现；字形图集不应重新承担文字排版或自行按码点估算字符宽度。

每个控件内容项保留无宽度约束和有宽度约束的段落缓存；文字、字体或换行策略变化使段落失效，宽度变化更新 DirectWrite layout 并撤下对应 raster。相同内容、字体、宽度、DPI 和栅格 viewport 可复用像素。编辑字段栅格只生成当前可见窗口，保留完整 layout 供字素导航与双向选择使用。见 [text.cpp](../../../src/ui/text.cpp) 135–164、188–255 行。

当前不是共享字形图集。相同文字出现在不同控件时也没有跨控件段落像素去重；字体格式缓存是共享的。整段位图在固定文字与现有单行字段中实现简单，但高频文本变化会产生新覆盖图，重复文字也不能共享同一字形页。这里描述机制成本，没有测得该成本已成为当前产品瓶颈。

renderer 按 Coverage 的 identity/revision 缓存独立 R8 texture，同时持有 CPU owner，首次实际使用该版本时上传一次。每帧标记使用项，帧末立即退休本帧未使用的 GPU 缓存；GPU 退休沿 gfx 的已有在飞资源机制完成。见 [renderer.cpp](../../../src/renderer/renderer.cpp) 128–142、279–301、344–384 行。文字离开可见范围后，CPU 缓存仍可能保有位图；再次出现时仍可能需要重新创建与上传 GPU texture。这是当前策略，不是已有显存预算或 LRU。

`UiFrame` 和 `Coverage` 发布后不可变，旧帧继续拥有旧文字像素；renderer 同步消费帧，在飞 GPU 资源由自身执行域管理。该所有权分工应保留。样式与文字交互版本是否完全一致，另见 [样式源码分析](current-styles.md)。

## 明确的能力缺口

当前协议无法表达一般三角网格、圆角/路径裁剪、彩色图片、彩色字形、矢量图标、描边路径、模糊或离屏合成，也没有保留控件级绘制几何缓存、通用图集与 GPU 缓存预算。不是所有能力都应同时加入：编辑器常用的圆角盒、图片/图标、连续批处理更直接；复杂滤镜、任意混合模式或完整浏览器绘制机制需要真实需求支持。

`Session::prepare` 每次新建拥有式绘制快照并遍历控件；renderer 逐项重录 UI 命令。已有布局与文字缓存减少部分重复工作，不等于已实现按控件绘制脏标记更新。是否值得保留绘制几何，应根据控件数量、状态/滚动/文本更新频率与实际 CPU 采样决定，不能仅按框架名判断。

## 可用的验收与采样基础

当前已有 UI prepare/测量/排版/栅格/缓存统计，以及 renderer 的 UI draw、覆盖图上传次数/字节和缓存持有量；正式应用还有显式 CPU prepare/render 采样。接口见 [session.cpp](../../../src/ui/session.cpp) 221–226 行、[renderer.hpp](../../../src/renderer/renderer.hpp) 55–70 行、[编辑器模块](../../modules/editor/README.md)。这些计数可以比较后续批处理或图集变化，无须另建常驻 benchmark 工程。

历史 renderer GPU 验收见 [renderer acceptance](../../modules/renderer/acceptance.md)，当前集成的窗口状态以[编辑器模块](../../modules/editor/README.md)为准。旧负载采样包含 vsync、Present 和 debug/GBV，不能直接用来声称当前 GPU 成本或预测改进百分比。
