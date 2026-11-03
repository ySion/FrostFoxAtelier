# 原生 UI 绘制：Qt Quick、RmlUi 与 Dear ImGui

检索日期：2026-10-01。范围是绘制、样式落到几何的路径和缓存边界；不替换 FrostFoxAtelier 的类型、输入或独立布局体系。未运行性能基准，下文性能结论描述机制与成立条件，不给出未经测量的速度数字。

## 版本和证据边界

| 系统 | 本次版本基线 | 源码快照 |
| --- | --- | --- |
| RmlUi | GitHub 最新稳定发布 6.3，2026-08-22；核对 master 的 6.x 绘制接口 | [`3045e6e`](https://github.com/mikke89/RmlUi/tree/3045e6e3510425ef2870f7647b3f59d3ae9970f5) |
| Dear ImGui | 最新稳定发布 v1.92.9b，2026-07-31；核对 master 动态字体与纹理更新接口 | [`3f00c0d`](https://github.com/ocornut/imgui/tree/3f00c0d0f414d45af2a0979b925ebf2e8f09936b) |
| Qt Quick | Qt 6 在线文档，本次打开页面标为 6.12.0；源码 dev，仅作实现参考 | [`2fba5e4`](https://github.com/qt/qtdeclarative/tree/2fba5e4ba8b4c54c244500dcd880d98817be4f2c) |

Qt `/qt-6/` 链接是滚动文档；检索缓存曾显示 6.11.2，实时页面显示 6.12.0，因此这里不声称 6.12 是已发布稳定版。集成时必须选定实际版本；固定源码链接使实现判断可复查。RmlUi 与 ImGui 发布信息分别见[官方发布](https://github.com/mikke89/RmlUi/releases/tag/6.3)、[官方发布](https://github.com/ocornut/imgui/releases/tag/v1.92.9b)。

## 三种模型不能混称 retained UI

| 项目 | 上层输入模型 | 绘制表示 | 跨帧保留 | 对已有布局引擎的实际关系 |
| --- | --- | --- | --- | --- |
| Qt Quick | QQuickItem/QML 保留树 | 独立 QSGNode 树，geometry/material/transform/clip 节点 | 节点、CPU 几何、GPU batch；dirty 驱动更新 | 可学习同步与 batch 算法；直接搬渲染器会引入 Qt 场景图和窗口/资源契约 |
| RmlUi | RML 元素树、RCSS 样式与自有布局 | 编译几何 handle 加有序绘制及状态回调 | 元素、mesh、编译资源、字体纹理 | RenderInterface 是“RmlUi 输出到现有 RHI”的接口，不是接受任意外部布局的独立绘制库 |
| Dear ImGui | 每帧调用控件 API 的 immediate 模型 | ImDrawList 顶点/索引/命令流 | 上下文状态、容器容量、字体缓存；官方 DX11 每帧上传几何 | 可使用 ImDrawList 作为局部绘制工具，但它不提供已有控件树的 retained 更新机制 |

Qt 文档明确场景图与 QQuickItem 独立，RmlUi 明确库由宿主 update/render 调用驱动，ImGui 明确产出 vertex/index/command buffers；保留一份显示列表或 GPU buffer 只能说明绘制缓存，不能说明控件身份、布局、命中测试与事件都被保留。[Qt 场景图](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)、[RmlUi 架构说明](https://github.com/mikke89/RmlUi)、[ImGui README](https://github.com/ocornut/imgui)

## Qt Quick：保留绘制树与可证明的重排

QSGGeometryNode 持有几何与材质，QSGTransformNode 表达变换，QSGNode 的 DirtyMatrix、DirtyGeometry、DirtyMaterial、DirtyOpacity 等区分变化。修改颜色不应被当成重建全部布局；修改矩阵也不应天然触发重新生成圆角和文字顶点。这里的价值是分离失效域，而非复制整个 QML 属性系统。[QSGNode dirty 契约](https://doc.qt.io/qt-6/qsgnode.html)、[QSGGeometryNode](https://doc.qt.io/qt-6/qsggeometrynode.html)

默认 renderer 的两个核心机制是合批与 GPU 几何保留。复杂且运动的 transform 子树可以成为 batch root：子节点几何在 CPU 上变换到根局部坐标，上传后保留；后续滚动只改根矩阵。增删节点或改变相关几何仍需要重建受影响 batch。“滚动几乎免费”仅指省掉重复几何处理与上传，仍有遍历、draw、采样和混合成本。[默认 renderer：Batch Roots](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html#batch-roots)

透明图元依赖先后关系。Qt 使用重叠关系约束透明 batch 合并，不能把中间重叠内容跨过去；不透明部分能利用深度等条件获得更多排序自由。材质兼容、clip 状态和变换约束都会打断 batch。应用侧若没有同等级的正确性判定，应先只合并连续、完全兼容的命令。[默认 renderer：Batching](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html#batching)

Rectangle 的内部实现位于 QSGBasicInternalRectangleNode：四角半径、边框和抗锯齿共同生成几何，属性 setter 标记变化，更新阶段重算网格；这是专用矩形几何路径，不意味着每个圆角矩形都要使用通用路径栅格器，也不能把它描述成统一 SDF quad。[固定源码：矩形实现](https://github.com/qt/qtdeclarative/blob/2fba5e4ba8b4c54c244500dcd880d98817be4f2c/src/quick/scenegraph/qsgbasicinternalrectanglenode.cpp)

轴对齐矩形 clip 可走 scissor，其他形状用 stencil；clip 增多会限制合批。默认 renderer 不替业务做完整 CPU 视口剔除，列表虚拟化和文字行剔除仍有必要。小图片进入 texture atlas 能减少换纹理，但 mipmapped 图片不进入全局 atlas；图集不是所有图片的无条件快路径。[默认 renderer：Clipping 与 Texture Atlas](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html)

QSGTextNode 接受已排好的 QTextLayout/QTextDocument，区分 shaping/layout 与绘制。QtRendering 使用 glyph distance field，NativeRendering 使用平台方式，CurveRendering 在 GPU 直接栅格化曲线；距离场在极大字号可能有伪影，曲线方式有不同的速度/显存权衡。字体缓存位于渲染上下文相关实现，不能把一个 glyph atlas 等同文本布局缓存。[QSGTextNode](https://doc.qt.io/qt-6/qsgtextnode.html)、[距离场 RHI cache 源码](https://github.com/qt/qtdeclarative/blob/2fba5e4ba8b4c54c244500dcd880d98817be4f2c/src/quick/scenegraph/qsgdistancefieldglyphcache.cpp)

Qt threaded render loop 在同步阶段阻塞 GUI 线程，将变化通过 updatePaintNode 同步到绘制树，随后 GUI 与 renderer 并行；QSG 节点与原生 GPU 操作属于渲染线程。它不是允许两线程任意写同一树的 lock-free 方案。OwnedByParent 简化节点销毁，GPU 相关资源必须处理 sceneGraphInvalidated。[线程与所有权](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html#threaded-render-loop-threaded)、[文本节点失效处理](https://doc.qt.io/qt-6/qsgtextnode.html)

QRhi 是图形 API 抽象，不负责控件布局或 Unicode shaping；虽从 Qt 6.6 起有文档，仍链接 Qt6::GuiPrivate，官方明确无跨 minor 版本的源/二进制兼容保证。已有 RHI 时，为学习 Qt batching 而再包一层 QRhi 没有必然收益。[QRhi 接口及兼容说明](https://doc.qt.io/qt-6/qrhi.html)

## RmlUi 6.x：几何编译与样式效果能力是独立契约

本次核对的新接口为 `CompileGeometry(Span<const Vertex>, Span<const int>)`、`RenderGeometry(handle, translation, texture)`、`ReleaseGeometry(handle)`。6.0 已移除旧 immediate 几何提交路径；texture 不属于 compiled geometry，可以变化。几何数据一直有效且不可变，直到 ReleaseGeometry；后端可以保留 span 或上传 GPU。这个生命周期保证只覆盖 RmlUi 存活期间，不覆盖宿主自行排队后、资源释放后的使用。[6.0 接口迁移记录](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/changelog.md#major-overhaul-of-the-render-interface)、[当前 RenderInterface.h](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Include/RmlUi/Core/RenderInterface.h)

内部 RenderManager 统一包装几何、texture 和 shader 资源，Geometry 是 move-only 唯一资源的一部分；保留 CPU Mesh 与编译句柄的职责有明确入口。字体或装饰器生成 mesh 后也进入同一绘制管理，而非分别拥有孤立 GPU 路径。可借鉴的是这个生命周期中心，不必引入 RmlUi 元素类型。[Geometry](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Source/Core/Geometry.cpp)、[RenderManager](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Source/Core/RenderManager.cpp)

背景与 border 在 GeometryBackgroundBorder 中三角化；圆角先限制半径，避免相邻角超出边长，再根据弧构造背景与内外边界，处理边框颜色。复杂度取决于弧采样点，而非恒定四顶点。更换为解析圆角 shader 是另一个实现选择，必须保留四角、四边、颜色连接与 AA 的可观察结果。[背景与边框源码](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Source/Core/GeometryBackgroundBorder.cpp)

基本后端只提供 indexed triangles、textures、scissor，并不能承诺所有 RCSS 外观。旋转/圆角 overflow 要 clip mask；阴影、filter、backdrop-filter 和 mask-image 需要层与相应效果接口。Set/SetInverse/Intersect 定义遮罩操作；scissor 保持窗口坐标，不随 SetTransform 改变。不能把“已接 RenderInterface”写成“圆角裁剪和阴影均已支持”。[接口功能表与裁剪约定](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html#feature-table)

层接口提供 PushLayer、CompositeLayers、PopLayer，配合保存 layer 为纹理/遮罩、编译 filter/shader 的句柄；宿主可以自行实现 stencil 或 mask texture，也承担目标复用、合成时状态恢复和格式约定。阴影缓存可以保存为纹理，但 backdrop-filter 输入依赖后方内容，不能只用控件自身 dirty 判断缓存有效。[RenderInterface 的高级操作](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Include/RmlUi/Core/RenderInterface.h)

RmlUi 要求按接口到达顺序渲染；缓存几何不允许重排透明对象。合批只能在保持效果、clip、层边界和绘制顺序时实施。其颜色与生成纹理是 sRGB、预乘 alpha；采样色乘顶点色，后端必须明确纹理坐标原点与 blending 约定，不能套用 ImGui 默认直通 alpha 的混合状态。[绘制次序与颜色约定](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html#rendering-conventions)

默认 font engine 用 FreeType；FontFaceHandleDefault 生成文字 mesh，并按 font layer 的 texture 分段，追踪 glyph/layer dirty 与 handle version。新增 glyph 可使纹理层与对应文字几何需要更新，atlas UV 变化必须传递给几何缓存。完整 shaping 不等同 FreeType 栅格化：官方 HarfBuzz 示例单独提供 shaping、方向和字体 fallback；不能把示例能力自动算到默认字体引擎。[默认字体实现](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Source/Core/FontEngineDefault/FontFaceHandleDefault.cpp)、[HarfBuzz shaping 示例](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/Samples/basic/harfbuzz/src/FontFaceHandleHarfBuzz.cpp)

官方描述库只随宿主调用运行，不能据此推断 Context/Element 可并发变更。若把回调转换为跨线程命令，必须复制当前 transform/scissor 等状态，保留几何与纹理资源直到消费完成；ReleaseGeometry 后 span 已失效，GPU 延迟销毁则还需等待 fence。它的编译句柄契约适合建立线程边界，但没有替宿主完成这个边界。[宿主驱动模型](https://github.com/mikke89/RmlUi)、[ReleaseGeometry 生命周期](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html#geometry)

## Dear ImGui：轻量命令流，不是 retained 控件渲染器

ImDrawVert 表示 position、UV、color；ImDrawCmd 表示 texture、clip rect、index/vertex offset、元素数量和可选 callback。后端按 CmdLists 和命令顺序提交 indexed triangles；16-bit index 可以借 RendererHasVtxOffset 支持更大的顶点范围。callback 是明确的合批屏障，不能视为普通三角形。[绘制数据定义](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui.h)

AddRectFilled 无圆角时直接输出 quad；圆角通过 PathRect、圆弧采样与 PathFillConvex 生成网格；边框通过 PathStroke。抗锯齿使用外围覆盖几何等路径，部分线条使用 atlas 中的线纹理。该工具集适合小型即时绘制与调试，复杂样式、独立四边 border、背景效果仍需要上层表达与实现。[矩形及 AA 源码](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui_draw.cpp)

合并只发生在兼容的连续命令：相同状态头、连续索引、无 callback 等条件。官方 DX11 后端增长动态 vertex/index buffers 后每帧 Map/WRITE_DISCARD 并拷贝整帧几何，按命令设置 scissor、绑定纹理并 DrawIndexed。缓存 buffer 容量避免频繁分配，与缓存上一帧几何避免上传是两回事。[命令合并](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui_draw.cpp)、[DX11 后端](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/backends/imgui_impl_dx11.cpp)

ClipRect 由后端转为 framebuffer 坐标 scissor；圆角背景不会自动把子内容裁成圆角，ImDrawCmd 没有通用嵌套 stencil/mask clip 描述。旋转矩形 mask、group opacity 或 backdrop-filter 都不能仅靠修改 ClipRect 获得。后端 alpha blending 参数也属于实际颜色契约的一部分。[DX11 scissor 与 blend 实现](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/backends/imgui_impl_dx11.cpp)

1.92+ 使用动态字号与按需 glyph 加载，后端必须实现 `ImGuiBackendFlags_RendererHasTextures`，处理纹理 WantCreate、WantUpdates、WantDestroy 等状态。旧版“启动时生成一次字体 atlas 并永久保持”说明已经不充分。默认栅格化可用 stb_truetype，另有 FreeType loader；这仍不自动提供完整语言 shaping。当前 RenderText 逐码点查 glyph、advance 并生成 quad，没有采用 HarfBuzz 的 glyph cluster 输出协议。[字体说明](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/docs/FONTS.md)、[后端迁移契约](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/docs/BACKENDS.md)、[RenderText 源码](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui_draw.cpp)

同一 ImGui context 不能任意并发调用。动态字体尤其会让文字测量加载新 glyph，不能把 CalcTextSize 当成纯只读线程安全 API。CloneOutput 可复制 draw list 输出，但快照仍要处理 texture 更新、用户 callback 外部数据和 GPU 生命周期；复制顶点不是完整资源快照。[线程说明](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui.cpp)、[CloneOutput](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/imgui_draw.cpp)

## 机制比较与采用边界

以下是研究判断，不是本项目待实施清单；本地机制由 [UI](../../modules/ui/README.md)和 [Renderer 合同](../../modules/renderer/contract.md)维护。Qt 值得参考 transform batch root、细分 dirty 和绘制树同步；RmlUi 值得参考编译几何句柄、效果能力表、资源生命周期中心；ImGui 值得参考简单有序命令与连续合批。三者的算法可比较，不意味着需要接入三套控件系统。

这些系统共同区分布局/样式结果、绘制表示、有序帧和后端提交。缓存片段须记录依赖版本，跨线程快照须拥有绘制数据与资源引用，不能回读可变业务对象。增量同步、阻塞同步、双缓冲和版本化发布是不同的所有权选择；引用外部库的机制不等于宿主已经满足其契约。

| 比较维度 | 必须明确的内容 | 原因 |
| --- | --- | --- |
| 图元与样式 | 填充、四边 border、四角 radius、image、已 shaping glyph run；统一 AA 和 DPI 规则 | 避免矩形、文字、装饰器各建一套提交入口 |
| dirty 与缓存 | layout/shape/geometry/paint/transform/clip/resource 分开失效；缓存键包含依赖版本 | 位置变化不该失效 glyph raster；atlas UV 变化却必须失效相应几何 |
| 排序 | 明确 painter order；连续合批起步；重排必须证明无重叠依赖 | 透明图元和抗锯齿边缘无法任意排序 |
| clip 与 effect | 矩形 scissor；圆角/旋转 mask；层边界、合成和可选 filter 能力 | 背景圆角不等于子内容圆角裁剪；group opacity 不等于每子项改 alpha |
| 资源 | stable handle + generation，引用持有、atlas page/version、上传完成与 fence 后销毁 | CPU Release 与 GPU 完成有时间差 |
| 文本 | shaping run 缓存与 glyph raster 缓存独立；字号/DPI/字体/fallback/version 进键 | glyph atlas 本身不能处理双向、组合、断行和 cluster 命中 |

CPU 缓存、GPU 几何缓存与离屏像素缓存应是三个明确层级：第一层减少 shaping/三角化，第二层减少上传，第三层减少重复绘制但增加显存、合成与失效依赖。对大量静态 UI 和滚动列表，前两层通常值得优先实现；对持续动画的背景效果，离屏缓存可能不断失效。实际收益需测量 CPU shaping 时间、几何重建数、上传字节、batch、clip/effect pass 和显存峰值，不能仅比较 draw-call 数。

### 可直接集成与维护成本

- **RmlUi 整库**适合愿意使用其 RML/RCSS/布局/元素生命周期的区域；它有自定义后端入口，但不能只接 RenderInterface 就获得外部布局的全功能绘制库。当前任务已有布局和统一类型时，宜参考契约或挑选局部算法，避免再维护一棵业务元素树。MIT 允许复用，保留版权与许可；字体、插件和第三方依赖仍分别核对。[许可证](https://github.com/mikke89/RmlUi/blob/3045e6e3510425ef2870f7647b3f59d3ae9970f5/LICENSE.txt)
- **Dear ImGui**适合调试界面或即时绘制片段，MIT；官方后端可参考或适配现有 RHI。若把 ImDrawList 当长期 UI 底座，需要自己增加 retained invalidation、复杂文本、mask/effect 与资源快照，最终维护成本不再只是一个简单后端。直接使用内部函数还会受动态字体接口演进影响。[许可证](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/LICENSE.txt)、[后端文档](https://github.com/ocornut/imgui/blob/3f00c0d0f414d45af2a0979b925ebf2e8f09936b/docs/BACKENDS.md)
- **Qt Quick renderer/QRhi**与 Qt 场景图、线程、窗口和私有实现耦合。整体集成需选择适用模块的商业或开源许可证并满足对应条件；从内部源码抽取代码同样受其许可证约束。对已有 RHI，优先理解算法，不默认复制私有 renderer，也不将 LGPL 条件简化成“动态链接即可”。[Qt 许可说明](https://doc.qt.io/qt-6/licensing.html)、[QRhi 私有兼容边界](https://doc.qt.io/qt-6/qrhi.html)

本研究不覆盖 Slint/Vello，也未测量通用 GPU path renderer 与现有 RHI 的桥接成本。
