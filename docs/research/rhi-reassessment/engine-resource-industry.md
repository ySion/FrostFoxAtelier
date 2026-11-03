# 引擎资产与 GPU 资源：工业实现参考

研究时间：2026-09-27。状态：分析参考，未采纳架构；未修改现行契约，未运行验收。资料为实际打开的官方文档和源码；Godot `master`、O3DE `development` 链接会继续变化。工业参考与本项目事实、设计建议分节记录。本地分析基于 `c644ba5692fe2d67d1dda93953e227a50ba64f9f`。

## Unreal：业务资产、渲染表示和释放进度分开

Asset Manager 通过 `FPrimaryAssetId` 发现、加载资产，Streamable Handle 保持对象存活。这个 ID 默认包含类型和名称，不能直接当作永不变化的 GUID。[资产管理](https://dev.epicgames.com/documentation/unreal-engine/asset-management-in-unreal-engine)

官方骨骼网格示例给出执行链：`USkeletalMesh::PostLoad → InitResources → BeginInitResource → FRenderResource::InitResource`；销毁走 `BeginDestroy → BeginReleaseResource → fence → IsReadyForFinishDestroy`。GC 判断“不再需要”之后，渲染线程仍可能使用数据。这里的 render-command fence 跟踪渲染线程进度，不能直接等同 GPU 完成证明。页面含旧命令宏，此处仅引用所有权原则。[线程与生命周期](https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine)

`FStaticMeshRenderData` 同时容纳 LOD、Nanite、光追等表示；纹理 streamer 根据视点和预算选择 mip，再生成加载/卸载请求。因此网格资产不等于一个 GPU Buffer，资产加载也不等于全部显存驻留。[网格表示](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshRenderData)、[流送策略](https://dev.epicgames.com/documentation/en-us/unreal-engine/texture-streaming-overview-for-unreal-engine)

**可借鉴**：逻辑存活、渲染所有权、GPU 完成分别判定。**不宜照抄**：UObject GC、全局 Asset Manager 和线程体系不是实现这些边界的前提。

## O3DE Atom：资产系统和 RHI 之间有具体资源实现

产物 ID 是源文件 UUID 加稳定 sub-ID；同一源文件可以产出多个运行时产物，资产依赖用产物 ID 表达。[产物与身份](https://www.docs.o3de.org/docs/user-guide/assets/pipeline/product-assets/)

纹理链为 `StreamingImageAsset → StreamingImage::FindOrCreate → RHI::StreamingImagePool / Image`。RPI controller 根据优先级和预算请求磁盘 mip，CPU 到货后在指定阶段上传；CPU mip 缓存释放不必删除 GPU 内容。该类特意保持不可变资产和 runtime instance 一对一，不能误称所有引擎都要求“一资产多实例”。[类契约与实现](https://raw.githubusercontent.com/o3de/o3de/development/Gems/Atom/RPI/Code/Include/Atom/RPI.Public/Image/StreamingImage.h)

热重载由 RPI 的 `OnAssetReloaded` 接收资产事件，然后 `Shutdown/Init`；RHI `Image` 处理描述符、视图和设备资源。RHI 的 `ObjectCollector` 另外处理 GPU 时间线上的延迟释放，其调用方必须满足同步条件。[重载实现](https://raw.githubusercontent.com/o3de/o3de/development/Gems/Atom/RPI/Code/Source/RPI.Public/Image/StreamingImage.cpp)、[Image](https://raw.githubusercontent.com/o3de/o3de/development/Gems/Atom/RHI/Code/Include/Atom/RHI/Image.h)、[退休机制](https://raw.githubusercontent.com/o3de/o3de/development/Gems/Atom/RHI/Code/Include/Atom/RHI/ObjectCollector.h)

**可借鉴**：流送策略需要同时看见资产与显存预算，放在拥有这些信息的资源实现中。**不宜照抄**：整套 RPI/pool/controller；其重载路径也不能证明“失败保留旧版本”的事务语义。

## Godot：可持久引用与设备句柄明确不同

`ResourceUID` 维持文件改名后的引用；`ResourceLoader` 管路径、格式、依赖和缓存，`CACHE_MODE_REPLACE` 可刷新同类型的已有实例。这些身份和操作不等同服务器的 RID。[UID](https://docs.godotengine.org/en/stable/classes/class_resourceuid.html)、[加载与缓存](https://docs.godotengine.org/en/stable/classes/class_resourceloader.html)

`ImageTexture::set_image → RenderingServer::texture_2d_create/texture_replace` 可替换底层纹理；网格 storage 则把一个 surface 展开为顶点、属性、蒙皮、索引及 LOD buffers。RenderingDevice 的 `_begin_frame` 先等待 frame fence，再 `_free_pending_resources`，资源对象的释放请求与驱动内存真正释放有明确间隔。[纹理替换](https://raw.githubusercontent.com/godotengine/godot/master/scene/resources/image_texture.cpp)、[网格展开](https://raw.githubusercontent.com/godotengine/godot/master/servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp)、[设备退休](https://raw.githubusercontent.com/godotengine/godot/master/servers/rendering/rendering_device.cpp)

**可借鉴**：上层引用保持稳定，设备表示允许更新。**不宜照抄**：再造一套所有业务对象都必须进入的 RID 世界；缓存原地刷新也不能证明跨资源热更原子性。

## 对设计问题的推断

上述实现支持以下边界判断，但不指定本项目应采用多少类或管理器：

- 资产身份回答“引用什么内容”；运行时对象回答“本执行上下文中的对象”；GPU 句柄回答“此设备上怎样执行”。三者可以关联，不能互换。
- 资产到设备表示应允许零个、一个或多个；网格可以展开多个 buffer，尚未加载的资产没有 GPU 表示。具体不可变纹理仍可选一对一共享。
- 内容依赖、质量选择和热更发布由掌握资产语义的一侧决定；RHI 提供分配、上传、绑定、完成与安全回收。观察到的创建接口无需资产路径或业务 GC；调试名、shader cache 不改变这个责任边界。
- “业务仍引用”“CPU 已加载”“GPU 驻留”“旧版本仍在飞”是不同事实。把它们压成一个 `loaded` 或一份引用计数，会使缓存、流送和替换难以正确协同。

## 2026-09-27 的项目状态

本节保留当时的源码判断；此后引擎已增加 CPU Mesh/材质图片、UI 图片消费与窗口渲染。当前资源连接状态见[资源连接提案](../../modules/resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)，RHI 行为以[现行契约](../../modules/rhi/contract.md)为准。

正式消费者已经创建 `GfxBuffer`、`GfxTexture`，用于计算输入、间接参数、颜色/深度目标、上传与回读；`GfxProgram/GfxKernel` 表达着色程序与执行管线。资源由 gfx 跟踪分配、状态、绑定与延迟退休。`atelier::rhi::Context` 管执行域，`Pipeline` 管 Program/Kernel 与执行域的拥有关系。[正式消费者](../../../acceptance/rhi/main.cpp)、[公开入口](../../../src/rhi/rhi.hpp)、[当前契约](../../modules/rhi/contract.md)

有两个容易误读的地方：

1. `PipelineKind::mesh` 表达 Mesh Shader 管线。上游另有 `gfx_scene.h::GfxMesh`，但本项目明确设置 `GFX_ENABLE_SCENE OFF`；它没有进入当前生产模块。引擎 Mesh 资产、材质槽、LOD 和碰撞表示尚未由这个枚举提供。[构建入口](../../../cmake/rhi.cmake)
2. 普通 gfx 句柄的复制不增加资源拥有关系。消费者必须显式销毁；`Context::close` 的 busy 检查只覆盖 Pipeline。Pipeline 保持执行域寿命的规则不能自动推广到 texture/buffer 缓存。[关停实现](../../../src/rhi/rhi.cpp)

当前接口公开依赖 gfx，且存在 D3D12/DXGI 类型。这是现行固定后端的选择；本文不提出先增加另一层后端抽象。shader 的文件输入也是编译便利入口，不承担资产目录、持久身份或跨资源重载依赖管理。

## 建议：按含义、表示与使用期组织资源

下表是职责划分，初期可以集中在少量文件中实现。名称用于讨论，尚未确定公共类型。

| 概念 | 负责的事实 | 例子 |
| --- | --- | --- |
| 资产与内容 | 持久引用、来源、导入设置、依赖、内容版本 | 图像源及压缩产物、几何源及碰撞产物 |
| 引擎资源 | 业务类型、共享使用、可观察内容与引用语义 | Texture、Mesh、Material |
| 设备表示 | 在特定执行域如何执行该内容；拥有具体 gfx 资源 | texture、vertex/index buffers、绑定数据 |
| GPU 使用期 | 编码、提交、完成、资源可安全释放的时机 | 现有 gfx 状态与退休机制 |

引擎资源若作为业务对象暴露给原生代码和自有语言，应复用同一个权威类型和对象身份。设备表示可以是该对象的私有原生存储，也可以由 renderer 拥有；不需要逐个把 buffer、mip 或 descriptor 建成 GC 对象。持久资产标识、运行时对象身份和 GPU 句柄属于不同作用域，与项目的统一类型世界兼容。[现有术语](../../../CONTEXT.md)

首增量允许资源的私有实现直接组合 GPU 表示。只有出现多设备、多个派生表示或独立缓存策略时，才按真实需要外置映射。资源发现和版本解析可以放在冷路径；录制绘制时使用已经选定并被持有的表示，避免每个 draw 都按文件名查全局管理器。

资源设施与 renderer 在格式选择、上传安排、LOD/mip 需求和预算方面需要明确协作。RHI 则继续提供创建、录制与退休机制。这种依赖方向允许专用服务器只使用 Mesh 的碰撞产物，也允许 renderer 创建没有文件来源的阴影贴图、相机目标和程序化纹理。

因此，暂不建议从覆盖所有类型的 `Resource` 继承树和万能 `ResourceManager` 开始。先承载内容查找、完整候选发布与设备表示所有权这三项真实责任；出现重复需求后再提取共用机制。

## 最关键的衔接：CPU 消费、owner 与 GPU 退休

建议将一次纹理更新理解为以下链路：

1. 导入或生成新内容，准备完整候选；失败保留正在使用的版本。
2. 在 RHI owner 上创建设备表示并安排上传；后续 GPU 消费与上传建立正确顺序。
3. 在明确的引擎阶段切换供新工作使用的表示。单 queue 中，正确排序后可继续编码消费，无需为每次发布调用 `finish()`。
4. 已经选定旧表示但尚未编码的 draw packet、异步任务继续持有其所需版本及 CPU 数据。gfx 无法保护尚未交给它的未来使用。
5. 这些上层使用结束后，由 owner 释放旧 gfx 句柄；已编码 GPU 工作的实际使用期继续交给 gfx 管理。

`GC 不可达`、`CPU 任务不再使用` 和 `GPU 已完成` 是三个判定。若 GC 运行于 RHI owner 的安全阶段，可直接履行释放；若未来跨线程，就需要把必要原生拥有状态交给 owner，避免延后消息借用已被 GC 销毁的对象。当前尚无这样的业务资源桥接契约，不把它当成现有能力。

新增缓存时也必须明确退出顺序：停止新请求，结束或取消尚未编码的任务，释放资源拥有者与 Pipeline，再关闭 Context。可以由 renderer 保证整体嵌套寿命；只有无法用现有拥有结构表达时，才考虑扩展执行域持有原语。两种方案都应沿用现有入口，不复制 GPU 资源表或退休队列。

Filament 提供了一个直接的参照：AssetLoader 解析资产，ResourceLoader 准备并上传数据；销毁资产时，解码或上传尚未结束的数据不一定立即释放。它还要求 ResourceLoader 在渲染线程销毁，以接收数据释放回调。这说明连接职责本身有实际行为，不能省略为一句“资源引用归零就释放”。[AssetLoader](https://raw.githubusercontent.com/google/filament/main/libs/gltfio/include/gltfio/AssetLoader.h)、[ResourceLoader](https://raw.githubusercontent.com/google/filament/main/libs/gltfio/include/gltfio/ResourceLoader.h)

## 研究方向与建议的验证实例

Nanite 的官方说明描述了导入时的层次化三角形簇，以及运行时按视点选择、按需流送的表示。对本项目有价值的推断是：引擎 Mesh 的语义应允许底层表示演进，不能永久等同某一对 vertex/index buffers。当前没有引入虚拟几何系统的必要。[Nanite 概览](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)、[作者 SIGGRAPH 2021 课程说明](https://www.wihlidal.com/projects/nanite-deepdive/)

适合验证上述关系的首例是“一份被两个使用者共享的纹理或 Mesh → 真实绘制 → 更新失败保留旧内容 → 成功替换 → 撤引用并安全退出”，仍可先在现有离屏路径完成。增加一个无文件来源的纹理，就能同时检查设计是否把所有设备资源错误绑定到资产加载流程。本文只提出验证实例，没有开始实施。

实施前值得共同明确三项语义：业务引用默认跟随最新内容还是固定版本；渲染和碰撞派生产物是否必须同时切换；CPU 内容在上传后允许释放还是要求保留。它们属于引擎资源政策，应由真实业务需求决定。
