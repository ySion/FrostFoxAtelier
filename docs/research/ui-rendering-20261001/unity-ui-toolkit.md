# Unity UI Toolkit：面向游戏编辑器的绘制机制研究

检索日期：2026-10-01。本文关注保留式 UI 的更新与绘制；选择器、级联和主题的具体设计由样式研究覆盖。本次没有运行 Unity，也没有对 FrostFoxAtelier 做性能测量。

## 证据范围与版本

稳定行为主要依据 Unity 6.0（6000.0）官方 Manual 与 Scripting API。公开 C# 源码固定为 UnityCsReference 提交 `88ce7b60434ba7a8ca0218590a4cb509971788ad`，其 README 标明 **6000.7.0b2**。因此以下源码观察是预览版本的实现证据，不能冒充 6000.0 的精确实现，也不能把公开 C# 当成全部原生渲染器。源码会调用 Unity 的 GPU、作业、字体与原生绑定。[源码版本与使用范围](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/README.md)

该仓库是参考源码，不是可自由移植的库；README 明确限制修改和再分发。本文的用途是理解机制、独立设计，不能直接复制 UIR 到自有 C++ 引擎。[同上](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/README.md)

## 核心心智模型：一棵长期存在的元素树，按变化更新绘制数据

VisualElement 同时携带样式、布局信息、事件处理和子元素，控件也建立在这棵 visual tree 上。EditorWindow 与 UIDocument 各提供根元素；没有必要为自定义绘图再建立独立控件世界。[Visual tree](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-VisualTree.html)

Panel 是承载 visual tree 的上下文。[Panels](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-panels.html) 在固定源码中，`Panel.UpdateForRepaint` 明确依次推进 Styles、Layout、TransformClip、Accessibility、Repaint 等阶段；`ValidateLayout` 也使用同一 Styles → Layout → TransformClip 路径。`OnVersionChanged` 把元素变化交给各 updater。这说明事件、查询和绘制可以要求不同程度的更新，但使用的是同一组权威状态。[Panel.cs，UpdateForRepaint / ValidateLayout](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Panel.cs#L1680)，[VisualTreeUpdater.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/VisualTreeUpdater.cs)

### 从样式差异到绘制更新

预览源码 `ComputedStyle.CompareChanges` 并非把所有样式改变都标成 repaint，而是根据最终属性差异返回不同 VersionChangeType。以下是该版本实际可见分类；不是对全部属性的穷举。[ComputedStyle.gen.cs，CompareChanges](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Style/Generated/ComputedStyle.gen.cs#L4774)

| 属性变化 | 主要更新类别 | 对绘制的含义 |
| --- | --- | --- |
| width/height、padding、margin、flex 等 | Layout | 先重新布局，再由实际矩形变化决定后续更新 |
| border-width | BorderWidth + Layout + Repaint | 影响盒子和边框几何 |
| overflow | Layout + Overflow | 可能改变裁剪方式及传播 |
| 字体、font-size、white-space 等 | Layout + Repaint | 文本测量与字形几何需要一致 |
| translate/rotate/scale/origin | Transform | 不直接要求重新布局 |
| 背景与边框颜色 | Color | 可进入专用颜色更新路径 |
| opacity | Opacity | 可更新透明度参数而非重建全部几何 |
| border-radius | BorderRadius + Repaint | 几何与 mask 选择可能变化 |
| 背景图 | Overflow + Repaint | 绘制内容及潜在 SVG mask 改变 |

这里最有价值的细节是：**Layout dirty 不等于 mesh dirty**。`VisualTreeLayoutUpdater.UpdateSubTree` 比较新旧 layout/padding 矩形，尺寸变化才发 Size + Repaint；位置变化发 Transform。由此避免布局求解经过某个元素后就无条件重建该元素几何。[VisualTreeLayoutUpdater.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/VisualTreeLayoutUpdater.cs#L479)

`UIRRepaintUpdater.OnVersionChanged` 随后将变化交给 transform/size、clipping、opacity、color、visuals 等入口。其 Update 调用 `RenderTreeManager.ProcessChanges`；RenderTree 分批处理 clipping、opacity、color、transform/size，最后处理 visuals。顺序使几何生成能使用已更新的继承参数。[UIRRepaintUpdater.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRepaintUpdater.cs#L108)，[UIRRenderTree.cs，ProcessChanges](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRenderTree.cs#L281)

对本项目的设计推论：应让样式求值产出明确变化类别，让布局只在结果改变时继续失效；自定义控件和标准控件共用这条更新链。不要用几套互相补偿的 dirty 布尔值，也不要一开始照搬 Unity 预览版完整 RenderTreeManager。

## 绘制顺序与 GPU 批处理

6000.0 官方说明按 visual tree 深度优先绘制：父元素先画，子元素覆盖父元素，兄弟按列表顺序。BringToFront、SendToBack、PlaceBehind、PlaceInFront 改变这个顺序。[Draw order](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-draw-order.html)

固定的 6000.7 预览源码已经能看到 zIndex、stacking context 和 nested render tree；它与上述稳定文档的简单顺序模型不完全相同。本项目首阶段可以采用确定的树顺序与显式覆盖层；只有真实业务需要时再定义层叠上下文，不能把预览源码当成必须一次实现的功能清单。[UIRRepaintUpdater.cs，结构变化处理](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRepaintUpdater.cs#L119)

`UIRenderDevice.EvaluateChain` 顺着命令链累积 indexed draw ranges。代码中的 batch 中断原因包括材料变化、纹理槽耗尽、stencil reference 变化、GPU 数据页变化和非 draw 状态命令；最终通过 Utility.DrawRanges 或 CommandList 输出。它不是先按纹理全局排序再绘制：那样会破坏透明 UI 的覆盖关系。源码也表明“换一张纹理必然多一 draw call”并不准确，纹理槽还有空间时可以绑定多个纹理。[UIRenderDevice.cs，EvaluateChain](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRenderer/UIRenderDevice.cs#L679)，[UIRTextureSlotManager.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRTextureSlotManager.cs)

借鉴项是保留绘制顺序，在相邻且状态兼容的命令间合批，并能报告批次为什么断开。纹理槽、bindless 或 descriptor indexing 应根据本项目 Windows 图形后端选择，不能只因 Unity 使用槽表就照搬其数量和 shader 编码。

## 纹理与 dynamic atlas

6000.0 的 dynamic atlas 根据引用自动纳入或移除纹理；sprite atlas 提供手工管理方式。图集从最小尺寸开始，沿一个方向翻倍增长到配置上限；过滤器决定哪些纹理能进入。官方还明确讨论长期添加、删除引起的碎片，并提供 ResetDynamicAtlas 重置。图集是否成功收纳纹理应通过 Frame Debugger 检查，而非仅依据“启用了图集”。[Control textures of the dynamic atlas](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-control-textures-of-the-dynamic-atlas.html)，[DynamicAtlasSettings API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.DynamicAtlasSettings.html)

自定义 mesh 的纹理也可能被放入内部 atlas，不能假定输入纹理的原始 UV 与底层 atlas 完全等价；应使用 MeshGenerationContext 提供的契约。[MeshGenerationContext API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.MeshGenerationContext.html)

推论：编辑器图标和小图片很适合 atlas；大视口 render target、频繁改写的纹理应保留明确外部纹理路径。资源缓存需要引用和释放机制、容量预算、碎片或重建策略，不能成为永久增长的字典。动态图片图集与字体 atlas 应各有更新契约；它们都叫 atlas，并不表示可以共用全部分配策略。

## 裁剪：矩形、scissor 与 stencil 必须分清

6000.0 对外用父元素 `overflow: hidden` 定义裁剪，支持矩形、圆角以及背景 SVG 形状；复杂 mask 使用 stencil。嵌套 stencil 状态变化会打断批次，MaskContainer usage hint 用于降低特定层级结构的中断。[Masking](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-masking.html)

固定源码 `DetermineSelfClipMethod` 更具体：普通矩形默认走 ShaderDiscard；GroupTransform 或内部 ClipWithScissors hint 倾向 Scissor；需要复杂 mask 且未达到深度上限时走 Stencil。camera 绘制路径直接选 ShaderDiscard。达到 mask 上限会回退矩形方式。因此“UI Toolkit 所有矩形都用硬件 scissor”不成立，也不能假定 mask 无限嵌套后仍保留相同形状语义。[UIRRenderEvents.cs，DetermineSelfClipMethod](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRenderEvents.cs#L1189)

推论：本项目应先完成矩形交集、空区域剔除与一致的绘制/命中裁剪。shader clip 可以减少 GPU 状态切换，但增加逐片元检查；scissor 限制光栅区域但改变 GPU 状态；stencil 能表示复杂区域但需要附加命令与深度预算。选择应保留为绘制器内部策略，而不是控件各自实现三套裁剪。

## 自定义几何与缓存

`generateVisualContent` 在创建及需要 repaint 时调用，并非每帧刷新都调用；MarkDirtyRepaint 可显式请求更新。官方要求回调内把 VisualElement 视为只读，避免边生成边改状态造成遗漏或延迟更新。[generateVisualContent API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.VisualElement-generateVisualContent.html)

同一 MeshGenerationContext 提供 Painter2D 和手工 indexed mesh 方式。前者适合路径，后者允许控件提供自己的顶点与索引。[Generate 2D visual content](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-generate-2d-visual-content.html) 缓存应由框架负责保留可复用绘制数据；它不是把每个控件先渲染成一张纹理，也不意味着自定义内容自动获知业务数据变化。

Unity 明确提供并行 tessellation：在主线程分配并记录 mesh，作业填充数据，AddMeshGenerationJob 注册依赖；若分配规模本身需要计算，也有相应的延迟生成方式。DrawMesh 的输入必须在回调结束前完成，除非注册生成作业；渲染器会等 dirty 元素回调、生成依赖和延迟回调完成后再消费数据。[Parallel tessellation](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-parallel-tessellation.html)，[DrawMesh API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.MeshGenerationContext.DrawMesh.html)

推论：本项目可以从同步、只读生成回调开始，缓存标准图元和自定义 mesh，再为真实的大图表、曲线、节点连线增加并行生成。并行边界是稳定输入和独占输出缓冲区，不能据此推断整棵元素树能被任意线程修改。

## transform、opacity、color 的低成本更新

6000.0 的 UsageHints 包含 DynamicTransform、GroupTransform、DynamicColor 和 MaskContainer，描述预期使用方式以减少几何重建或 draw calls。提示应在加入 panel 前设置；transition 自动补充提示可能产生一帧重建代价。[UsageHints API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.UsageHints.html)，[Usage hints manual](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-use-usage-hints-to-reduce-draw-calls-and-geometry-regeneration.html)

固定源码可解释其方向：transform ID 指向 shader 参数；继承同一个变换的后代可复用祖先更新，group transform 的处理也能停止常规递归。opacity 合成父透明度，按需分配 opacity ID，并递归传播变化；DynamicColor 为文字、背景与边框等分配独立 color ID。颜色 ID、透明度 ID 和变换参数并不是免费无限资源，源码包含分配失败路径。因此不能宣称这些动画始终 O(1) 或永不触碰子树。[UIRRenderEvents.cs，transform / opacity / color](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRenderEvents.cs#L840)，[UIRVEShaderInfoAllocator.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRVEShaderInfoAllocator.cs)

推论：优先用 transform 做视觉移动，保持布局矩形语义清楚；把频繁变化的参数与静态顶点分离。是否采用用户显式 hint，应根据实际复杂度决定：必要时框架可以自动选择，避免把内部批处理知识变成每个控件作者必须承担的义务。

## 文本：字体栅格化、塑形、测量和绘制是不同责任

6000.0 TextCore 字体资产支持 Static、Dynamic、Dynamic OS atlas population，并提供 bitmap、color bitmap、SDF 渲染模式。静态 atlas 适合已知字符；动态 atlas 适合输入等未知文本；Dynamic OS 引用目标系统字体，减少打包来源文件，但依赖目标机字体存在。SDF 适合变换和轮廓效果，bitmap/hinted 模式有其像素对齐用途。[Font assets](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-font-asset.html)

6000.0 Advanced Text Generator 使用 HarfBuzz、ICU、FreeType，提供 Unicode shaping 与 RTL 支持，需显式启用并选择 advanced。该版文档列明只支持动态字体资产，一些富文本标签、字符/词/段落间距功能尚不支持；双向文本光标采用逻辑移动，视觉移动选项仍在计划中。不能把“支持 Unicode shaping”推导成编辑行为和标准生成器完全等价。[Advanced Text Generator](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-advanced-text-generator.html)，[unityTextGenerator API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/UIElements.IResolvedStyle-unityTextGenerator.html)

彩色 emoji 有单独字体资产和 fallback 设置；Editor 与 runtime 的默认资源配置不同。对于 Windows 游戏编辑器，必须明确 fallback 链和缺字行为，不能因为某台开发机能显示 emoji 就认定打包客户端也能显示。[Color emojis](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-color-emojis.html)

借鉴项：建立同一文本结果供 measure、绘制、选区和 caret 使用，缓存键包含字体、字号、塑形输入和可用宽度。字体 rasterizer 可以复用成熟组件，但 SDF 或 atlas 自身不会解决 ligature、组合字符、RTL、换行和 IME。以上缓存设计是本项目建议，并非 Unity 已公开保证的精确 cache key。

## panel 生命周期与线程边界

固定源码中 Panel.Dispose 从 atlas 解关联并 Dispose updater；UIRRepaintUpdater 在 panel 改变、atlas 变化或配置重建时 Dispose RenderTreeManager，并解除事件订阅。RenderTreeManager 进一步释放临时 mesh、job manager、vector image、shader 参数和 device 等资源。GPU 设备销毁还包含延迟释放处理，不能用“从 visual tree 移除节点”替代整个 panel 的资源退出。[Panel.cs，Dispose](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Panel.cs#L1589)，[UIRRepaintUpdater.cs，DestroyRenderChain](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRepaintUpdater.cs#L273)，[UIRRenderTreeManager.cs，Dispose](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRRenderTreeManager.cs#L319)，[UIRenderDevice.cs，Dispose](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Renderer/UIRenderer/UIRenderDevice.cs#L319)

这里可确认的线程能力是有显式依赖的 mesh 生成作业。公开证据不足以承诺 VisualElement 树、样式、panel 和绘制回调可任意跨线程调用；源码中样式匹配还注明同一 context 单线程。建议本项目把树与样式更新限定到明确的 UI 所有者线程，后台只产生不可变结果或独占几何，下一次更新统一接入。[VisualTreeStyleUpdater.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/VisualTreeStyleUpdater.cs#L207)，[Parallel tessellation](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-parallel-tessellation.html)

## 样式边界与建议实施顺序

6000.0 USS 是受 CSS 启发并为 Unity 定制的格式；布局采用 Yoga 的 Flexbox 子集原则，盒模型等价 border-box、默认纵向排列，圆角语法也存在明确差异。这是一套可用的游戏 UI 样式体系，并不需要先实现浏览器全规范。[USS](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS.html)，[Layout engine](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-LayoutEngine.html)，[USS common properties](https://docs.unity3d.com/6000.0/Documentation/Manual/UIE-USS-SupportedProperties.html)

6000.7 预览源码已出现 grid、filter/backdropFilter 与 zIndex，因此“Unity 永远只有 Flexbox”也是错误总结。这些新增能力说明需求会演进，不表示本项目当前必须承接同等实现规模。[ComputedStyle.gen.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/88ce7b60434ba7a8ca0218590a4cb509971788ad/Modules/UIElements/Core/Style/Generated/ComputedStyle.gen.cs#L4936)

以下是面向本项目的建议顺序，不是 Unity 官方路线图：

1. **先闭合状态与绘制契约**：权威元素树、最终样式、布局矩形、绘制顺序、矩形裁剪、文本和图像；标准与自定义控件使用同一输出入口。
2. **再做到变化只更新必要数据**：属性变化分类，布局结果比较，保留 mesh 与绘制命令，明确业务数据触发 repaint 的方式。
3. **再处理真实编辑器负载**：相邻状态合批，小图 atlas，字体 atlas，transform/opacity/color 参数更新；采样批次中断、mesh 重建量、GPU 上传字节和缓存内存。
4. **按场景补高级能力**：圆角 stencil、复杂路径、并行大 mesh、子树离屏合成；每项都定义资源退出、裁剪深度和失败行为。

当前不值得仿制的浏览器能力包括 HTML 解析与纠错、网页导航和网络资源体系、完整 DOM API、全部 CSS 布局与级联兼容性、打印分页，以及没有真实编辑器需求支撑的任意混合/滤镜合成组合。应保留可用的样式、文本塑形、输入和一致布局；不能借“无需浏览器”删掉中文编辑、缺字 fallback 或 DPI 正确性。

## 结论与尚未验证项

最值得学习的是：**样式变化有类别；布局结果决定进一步失效；绘制内容可缓存；经常变化的视觉参数可以独立更新；合批服从覆盖顺序；资源由 panel 生命周期统一退出。** 这些机制适合 Windows 游戏编辑器，也与统一入口的目标一致。

尚未验证：FrostFoxAtelier 当前图形后端的纹理槽与 GPU 上传策略、文本后端、panel/窗口对应关系、实际编辑器 UI 负载，以及 Unity 各预览功能最终稳定版契约。公开 C# 能解释链路，但不能证明原生 shader、驱动行为或最终性能；任何性能收益都需在项目真实端到端场景中测量。
