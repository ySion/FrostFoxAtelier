**Flax 渲染管线源码核对，2026-10-17**

本次用 Exa 检索四个角度，`sources_reviewed=20`（四次 `numResults=5` 的累计，不是去重来源数），再直接读取官方文档与固定源码。源码基线为默认分支 [`0cbc4be11fb55201d0f934ab0c312ca2fca5b68a`][commit]，提交于 2026-10-15；最新正式发布为 [1.12.6912，2026-05-18][release]，两者不混用。下述默认值指该源码的 C++ 设置，项目与 PostFX Volume 可以覆盖。

**判断：Flax 最值得借鉴的是把可见性、批处理和昂贵结果复用接成完整执行链。它的光照算法并不全面领先于 FrostFox；特别不能把 SDF DDGI 的画面当成发光三角形直接光的等价替代。** 此判断由下面已核实的实现推出，不是性能实测结论。

| 已核实范围 | 源码事实与边界 |
| --- | --- |
| 主光照 | 不透明物体先写 GBuffer，主光照读取 GBuffer0–3 与深度。CPU 按灯类型维护列表，并按屏幕大小、亮度、ID 排序；点灯/聚光逐个绘制球体 light volume，可用时辅以 depth bounds；方向光逐个画全屏三角形。所查主路径不是 clustered/tiled 灯列表。[LightPass][light] |
| 灯与发光几何 | 原生主入口为 Point、Spot、Directional、Sky。所谓 area light 在这里是 `SourceLength` 线段、`SourceRadius` 球面代表方向和能量修正。该入口未发现 Rectangle 或 mesh-emitter NEE/发光三角形直接光采样。材质 emissive 会写入 Surface Atlas，供 DDGI 采样，语义不同。[灯光近似][area]、[发光写入][emissive] |
| 阴影 | 传统 shadow atlas：点灯六面、聚光一面、方向光 CSM；投影大小按需要分配。源码已有静态图集、静态内容拷贝后叠加动态绘制，以及失效检测与更新频率。它不是虚拟页 VSM。过滤可选 PCF/PCSS，contact shadow 是屏幕空间追踪；所查阴影路径不依赖 DXR。[图集与缓存][shadow]、[官方说明][shadowdoc] |
| 可见性与提交 | SceneRendering 在调用 Actor 前做包围球视锥筛选；LOD 按屏幕尺寸选择。遮挡可选 HZB 或硬件查询，配置默认空，不能写成默认开启。HZB 计算后缓冲读回 CPU，默认延迟配置为两帧。收集、对象数据构建、排序有 JobSystem 并行；主 pass 通过主 GPUContext 执行，不能据此称完整 GPU-driven 或多线程 GPU 命令录制。[场景筛选][scene]、[LOD][lod]、[配置][graphics]、[HZB][hzb]、[提交链][renderer] |
| 批处理与资源 | RenderList 对 64 位键做 radix sort，再由材质/几何兼容性形成实例批次；透明绘制包含反向距离排序，深度 pass 有自己的键。RenderBuffers 保存视图数据，临时 RT 按描述复用并在闲置后清理。Renderer 显式组织 GBuffer、AO、阴影、光照、GI、反射、透明与后处理。[排序/实例化][list]、[资源池][pool]、[pass 顺序][passes] |
| AO、反射与 AA | AO 是 ASSAO 路径，默认开，但正交投影直接跳过。SSR 默认 ScreenTracing、带 temporal 设置；SoftwareTracing 是可选的 Global SDF/Surface Atlas 回退。GI 默认 None，AA 默认 TAA，可选 Off/FXAA/SMAA；渲染缩放小于 1 时可用 TAA 上采样或常规/自定义 upscale。这些开关不能写成每个项目默认同时使用全部效果。[AO][ao]、[设置][settings]、[SSR][ssr]、[上采样][upscale] |

DDGI 的链路可以明确到调用：模型距离场合入 Global SDF；Surface Atlas 从物体侧面投影缓存材质、深度与光照；探针 `CS_TraceRays` 调 `RayTraceGlobalSDF` 找交点，再 `SampleGlobalSurfaceAtlas` 取辐射，未命中采样天空。它是软件追踪，不需要硬件 RT。图集具有静态/动态失效与更新节奏，也会把先前 DDGI 结果用于间接反弹。[SDF][sdf]、[表面图集][atlas]、[探针追踪][trace]

探针围绕相机滚动，按距离和间距自动选 1–4 级，间距比例为 1/3/6/10；有激活、重定位和无效探针邻居回退。正常视图每帧最多更新两个级联，配合 pending/dirty 与帧模数调度，不能把源码中的 2/3/5/7 简单解释成固定“每 N 帧更新”。质量控制每探针射线上限 96/128/192/256，实际数随 irradiance 不稳定度调整；仅 active probes 间接 dispatch，并分批复用 trace 纹理。[布局与预算][ddgicpu]、[自适应射线][attention]

接收表面每个级联 gather 八个探针，读取八面体距离一二阶矩，算 Chebyshev 可见性，叠加法线与三线性权重；这里没有逐像素硬件几何 LOS。开启平滑跨级混合时可能采样两个级联，因此“整个像素最多八探针”也不准确。它允许偏置、最小权重及无效探针回退，是近似遮挡，不能保证与我们当前 LOS 等价。[gather][gather]、[跨级混合][cascadeblend]

基于任务提供的 FrostFox 现状，建议按收益排序借鉴四项：

1. 先完成当前保序主视锥筛选的真实地牢验收，再评估可见 draw 的准备和批次复用。3381 meshes、481 geometry、原 2652 相邻 draw 说明这里有明确工作量；预测 557 不能当作已测 draw/FPS。非相邻材质/几何重排须另行处理共面深度竞争、绘制顺序和原实例/材质索引契约；主视图可见集不能裁掉完整 RT、材质、发光与阴影域。
2. 优先研究 DDGI gather 的“缓存可见性＋必要时精确复核”。旧测 gather 约 8.1 ms，而 probe update 约 0.7 ms；继续压缩探针更新预算难以命中主要成本。先用薄墙、门洞、移动遮挡验证可靠性，再决定哪些接收点可省 LOS。
3. 对实验 clustered 路径借鉴阴影静态/动态分离、明确失效和更新预算；保持统一 RHI/gfx 入口。是否取代默认 ReSTIR，仍须同时验收解析灯、发光三角形及区域光表现。
4. 借鉴稳定 draw 数据、并行准备和 RT 复用的生命周期。只有实际发现反复分配或准备开销时扩展现有机制，不为比较引入另一套 render graph。

这些是设计推断，不是 Flax 在 RX 6800 XT、1440p 同场景中的成绩。没有可比 GPU 实测，不能推算它能跑多少 FPS，也不能把我们的旧测约 32 FPS 当成最新 CPU/剔除优化后的结果。90 FPS 对应约 11.1 ms，需以优化后的同场景拆项重新判断。

不宜直接迁移整套 Global SDF＋Surface Atlas：它新增模型 SDF 资产、图集容量、更新延迟和内容约束；官方明确大封闭网格的内部表面可能缺失，小发光体可能闪烁，大发光面镜面表现可能需要额外光源。对低多边形地牢、多灯和强发光目标，保留精确几何追踪并改进采样成本更值得先评估。Flax 默认逐灯 volume 也不自动证明比我们的 clustered 或 ReSTIR 更合适。[GI 限制][gidoc]

许可证方面，仓库 [LICENSE.md][license] 将源码和二进制使用交由 [Flax EULA][licensing] 管理，属于源码可读的商业许可体系，不能按 MIT/BSD 自由复制来理解。以上可用于理解机制；是否移植具体代码须另核适用许可。未查实插件、私有分支或整仓所有硬件 RT 能力；“未发现 Rectangle/mesh NEE”仅限定已审阅的原生直接光入口。

一手来源（源码链接全部固定到上述 SHA；文档为检索当日版本）：

- [LightPass][light]、[LightingCommon][area]：原生直接光与区域光近似。
- [ShadowsPass][shadow]、[官方 Shadows 文档][shadowdoc]：图集、静态缓存与过滤。
- [SceneRendering][scene]、[RenderList][list]、[HZB][hzb]、[Renderer][renderer]：可见性、批次和执行链。
- [DDGI CPU][ddgicpu]、[DDGI 追踪][trace]、[DDGI gather][gather]、[Surface Atlas][atlas]：GI 数据流与预算。
- [PostProcessSettings][settings]、[GraphicsSettings][graphics]：默认值与可选能力。
- [官方 GI 文档][gidoc]、[LICENSE][license]、[许可页][licensing]：内容约束与许可边界。

[commit]: https://github.com/FlaxEngine/FlaxEngine/commit/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a
[release]: https://github.com/FlaxEngine/FlaxEngine/releases/tag/1.12.6912
[light]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Lighting/LightPass.cpp#L153
[area]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Shaders/Lighting/LightingCommon.hlsl#L137
[emissive]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/GI/GlobalSurfaceAtlasPass.cpp#L1546
[shadow]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Lighting/ShadowsPass.cpp#L1728
[shadowdoc]: https://docs.flaxengine.com/manual/graphics/lighting/shadows.html
[scene]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Level/Scene/SceneRendering.cpp#L245
[lod]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Graphics/RenderTools.cpp#L439
[graphics]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Core/Config/GraphicsSettings.h#L102
[hzb]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Culling/HZBOcclusionCulling.cpp#L46
[renderer]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Renderer.cpp#L487
[list]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/RenderList.cpp#L636
[pool]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Graphics/RenderTargetPool.cpp#L26
[passes]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Renderer.cpp#L640
[ao]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Lighting/AmbientOcclusionPass.cpp#L220
[settings]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Graphics/PostProcessSettings.h
[ssr]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Reflections/ScreenSpaceReflectionsPass.cpp#L247
[upscale]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Renderer.cpp#L827
[sdf]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/Utils/GlobalSignDistanceFieldPass.cpp
[atlas]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/GI/GlobalSurfaceAtlasPass.cpp#L1445
[trace]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Shaders/GI/DDGI.shader#L490
[ddgicpu]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Engine/Renderer/GI/DynamicDiffuseGlobalIllumination.cpp#L415
[attention]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Shaders/GI/DDGI.shader#L819
[gather]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Shaders/GI/DDGI.hlsl#L202
[cascadeblend]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/Source/Shaders/GI/DDGI.hlsl#L485
[gidoc]: https://docs.flaxengine.com/manual/graphics/lighting/gi/realtime.html
[license]: https://github.com/FlaxEngine/FlaxEngine/blob/0cbc4be11fb55201d0f934ab0c312ca2fca5b68a/LICENSE.md
[licensing]: https://flaxengine.com/licensing/
