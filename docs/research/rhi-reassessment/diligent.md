# DiligentCore：底层抽象、生命周期与 Windows D3D12 接入边界

## 1. 结论摘要

**研究结论，不是后端选择。** DiligentCore 可以作为独立的低层图形依赖进行评估；不能把 DiligentTools、DiligentFX、样例框架或场景系统一起算作必需接入物。但它也不是仅把 D3D12 函数改名的薄层：资源引用计数、PSO/资源签名/SRB、动态上传和描述符分配、状态跟踪、延迟回收、设备上下文与交换链都有明确的库级语义。[S1][S3][S5][S9]

本轮最重要的边界：

1. **CPU 对象寿命和 GPU 完成不是同一个条件。** 引用计数控制接口对象；D3D12 原生资源通过提交序号、队列掩码和 fence 延后释放。调用者仍须保证 deferred command list 使用的资源在被 immediate context 执行前有效，不能把“自动回收”解释成“录制时传入裸指针后随时可丢弃”。[S2][S6][S7]
2. **自动资源状态管理不是线程安全的调度器。** `TRANSITION` 会写资源的状态元数据；共享资源并发录制需要调用者安排显式状态、提交顺序和跨队列同步。[S3][S4]
3. **Mutable SRB 的覆盖是 GPU 同步责任。** `ALLOW_OVERWRITE` 不是自动安全开关；D3D12 下覆盖正在被 GPU 使用的 mutable binding 必须由应用防护。动态绑定类别与 `USAGE_DYNAMIC` 上传内存是两套概念。[S8][S3][S6]
4. **`FinishFrame()`、`Flush()`、`IdleGPU()` 不可互换。** 前者回收上下文的动态分配且使其逻辑内容失效，但不提交；设备级 `IdleGPU()` 只等待已经提交的命令；无主交换链的离屏消费者需要自己驱动提交与回收周期。[S3][S4][S6]
5. **最小 D3D12 路线仍有真实工具链成本。** 该版本 D3D12 target 链接 Core 内的 `Diligent-ShaderTools`；源码构建涉及 Python3、`fxc`、Windows SDK 和 ATL 检查。这不同于要求应用采用完整 Shader 资产工具或 DiligentTools。[S9][S10][S11]
6. **业务类型不必迁入另一套对象世界——这是有依据的设计推断，而不是已验证兼容性。** 已核对入口接受资源描述、字节数据和低层接口；可以由项目外层 GPU 句柄持有这些对象，而不把实体、业务组件或自有语言对象继承自 Diligent 类型。具体 FFI、GC/finalizer、结构布局和线程语义仍须路径实验。[S2][S4][S5][S8]

未进行配置、编译、运行、GPU 测量或设备丢失实验；未修改文件。没有性能排名，也不宣称此版本为最新版本。

## 2. 来源快照与证据等级

### 2.1 固定范围

- 研究主快照：**DiligentCore `v2.5.6`**。官方 release 页记录发布于 **2024-09-02**；这是本轮实际取得并交叉核对的版本，不代表最新生产建议。[S0]
- 官方 GitHub contents API 对 `ThirdParty/xxHash?ref=v2.5.6` 的实际返回，将 self URL 的 ref 解析为 `b036337d68be2353c9950a85929acf796b9a6d50`，并给出 xxHash gitlink `a57f6cce2698049863af8c25787084ae0489d849`。下文源码采用实际抓取的 tag URL；没有另行取得签名验证或本地 checkout。[S17]
- 外部检索与原文获取均使用 Exa；源码定位以**仓库路径、tag、符号**表达。抓取文本会折叠部分 C++ 模板和空白，不能把抓取文本行号伪装成 GitHub 原始行号。
- `README.md`、头文件注释和旧官网页面存在年代差异：例如 README 仍有旧字段名，官网生命周期文中的单队列/旧类名也不等于此 tag 实现。冲突时本报告以版本化头文件和实现为依据。

### 2.2 主要一手来源

| 编号 | 精确官方定位 | 实际核对内容 |
|---|---|---|
| S0 | [v2.5.6 release](https://github.com/DiligentGraphics/DiligentCore/releases/tag/v2.5.6) | 发布日期、版本及异步 shader/PSO 状态 API 的版本背景 |
| S1 | [README.md](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/README.md) | Core 可独立构建、初始化、资源绑定分类、静态/DLL 接入；示例不作当前签名权威 |
| S2 | [Primitives/interface/Object.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Primitives/interface/Object.h) | `QueryInterface`、`AddRef`、`Release`、C/C++ interface 声明 |
| S3 | [Graphics/GraphicsEngine/interface/DeviceContext.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/DeviceContext.h) | 状态模式、绑定引用、录制/提交、fence、map、flush、finish-frame 契约 |
| S4 | [Graphics/GraphicsEngine/interface/RenderDevice.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/RenderDevice.h)；[Fence.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/Fence.h) | 资源创建、格式查询、`ReleaseStaleResources`、`IdleGPU`、CPU/GPU fence 能力 |
| S5 | [Texture.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/Texture.h)；[TextureView.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/TextureView.h) | 资源/视图引用、默认视图借用、初始数据、context mask、native handle |
| S6 | [Graphics/GraphicsEngineD3D12/src/DeviceContextD3D12Impl.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/src/DeviceContextD3D12Impl.cpp)；[include/CommandListD3D12Impl.hpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/include/CommandListD3D12Impl.hpp) | `MapBuffer`、`FinishFrame`、`FinishCommandList`、`ExecuteCommandLists` 和 command-list 持有关系 |
| S7 | [Graphics/GraphicsEngineNextGenBase/include/RenderDeviceNextGenBase.hpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineNextGenBase/include/RenderDeviceNextGenBase.hpp)；[RenderDeviceD3D12Impl.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/src/RenderDeviceD3D12Impl.cpp)；[BufferD3D12Impl.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/src/BufferD3D12Impl.cpp) | `SafeReleaseDeviceObject`、`SubmitCommandBuffer`、`PurgeReleaseQueue`、提交后 allocator 回收、buffer 析构 |
| S8 | [ShaderResourceVariable.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/ShaderResourceVariable.h)；[ShaderResourceBinding.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/ShaderResourceBinding.h)；[Shader.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/interface/Shader.h) | static/mutable/dynamic、覆盖约束、SRB 查询、源代码/字节码/反射输入 |
| S9 | [根 CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/CMakeLists.txt)；[Graphics/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/CMakeLists.txt)；[GraphicsEngineD3D12/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/CMakeLists.txt) | 后端选项、工具链检查、目标依赖、`fxc`、系统库、可选 PIX |
| S10 | [ShaderTools/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/ShaderTools/CMakeLists.txt)；[ShaderTools/src/DXCompilerLibraryWin32.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/ShaderTools/src/DXCompilerLibraryWin32.cpp) | Core ShaderTools、Python3、C++17、DXC headers、动态加载 dxcompiler |
| S11 | [GraphicsEngineD3DBase/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3DBase/CMakeLists.txt)；[GraphicsEngineNextGenBase/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineNextGenBase/CMakeLists.txt)；[GraphicsEngine/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/CMakeLists.txt)；[Common/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Common/CMakeLists.txt) | 低层 target 闭包及 NVAPI 条件 |
| S12 | [ThirdParty/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/ThirdParty/CMakeLists.txt)；[.gitmodules](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/.gitmodules)；[GraphicsTools/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsTools/CMakeLists.txt) | 非 D3D12 工具链条件、xxHash 配置及 GraphicsTools 链接关系 |
| S13 | [GraphicsEngineD3D12/interface/EngineFactoryD3D12.h](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h)；[src/SwapChainD3D12Impl.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/src/SwapChainD3D12Impl.cpp) | 创建设备/contexts、接入外部原生设备和队列、独立交换链、Present/Resize |
| S14 | [GraphicsEngine/include/RenderDeviceBase.hpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngine/include/RenderDeviceBase.hpp)；[GraphicsEngineD3D12/src/CommandQueueD3D12Impl.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsEngineD3D12/src/CommandQueueD3D12Impl.cpp) | 创建失败出口、队列序列化、fence 和 device-removed 检查 |
| S15 | [License.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/License.txt)；[ThirdParty/DirectXShaderCompiler/LICENSE.TXT](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/ThirdParty/DirectXShaderCompiler/LICENSE.TXT)；[ThirdParty/GPUOpenShaderUtils/DXBCChecksum.cpp](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/ThirdParty/GPUOpenShaderUtils/DXBCChecksum.cpp) | Core Apache-2.0 文本、随树 DXC 许可文本、DXBCChecksum 实际文件头 |
| S16 | [旧官网 Managing Resource Lifetimes](https://diligentgraphics.com/diligent-engine/architecture/d3d12/managing-resource-lifetimes/) | 仅历史解释线索；滚动页面，抓取含无关广告污染，未将其旧实现描述当成 tag 契约 |
| S17 | [官方 contents API：xxHash gitlink](https://api.github.com/repos/DiligentGraphics/DiligentCore/contents/ThirdParty/xxHash?ref=v2.5.6)；[固定 xxHash LICENSE](https://raw.githubusercontent.com/DiligentGraphics/xxHash/a57f6cce2698049863af8c25787084ae0489d849/LICENSE) | Core 解析 ref、子模块 SHA、BSD-2-Clause 文本 |
| S18 | [Platforms/Win32/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Platforms/Win32/CMakeLists.txt)；[GraphicsAccessories/CMakeLists.txt](https://raw.githubusercontent.com/DiligentGraphics/DiligentCore/v2.5.6/Graphics/GraphicsAccessories/CMakeLists.txt) | Win32 platform、BasicPlatform、Shlwapi 和 GraphicsAccessories 依赖 |

## 3. 候选／机制矩阵：库承担什么，调用者还承担什么

| 维度 | 已核对事实 | 调用者责任／限制 | 本项目判断 |
|---|---|---|---|
| 公开抽象 | `IRenderDevice` 创建 buffer、texture、shader、PSO、signature、fence 等；`IDeviceContext` 负责状态、draw/dispatch/copy、map、录制/提交。创建与窗口不是同一个调用。[S3][S4][S13] | 提供真实资源描述、绑定布局、shader、执行顺序和窗口生命周期。 | **推断：** 可替代低层实现，无须替代业务层或 Renderer。 |
| CPU 所有权 | `IObject` 引用计数；`AddRef/Release` 本身线程安全。`CreateTexture/CreateView` 返回拥有引用；`GetDefaultView` 不加引用；显式 texture view 保持 texture，view 可保持 sampler。context 对当前绑定资源持有强引用。[S2][S3][S5] | 区分 borrowed/owned；不能由“引用计数线程安全”推出整个资源、context 或 SRB 并发修改安全。 | **推断：** 外层句柄表可以独立存在，但必须正确管理引用而非复制业务对象。 |
| GPU 寿命 | buffer 析构把原生资源送入 `SafeReleaseDeviceObject`；按 queue mask 加入 stale/release queues，再按 completed fence purge；提交序号与 fence value 被分开处理。[S7] | 正确声明 `ImmediateContextMask`；deferred 录制依赖不能在执行前失效。跨队列可用范围不等于跨队列自动同步。 | 自动回收减少工作，不消除提交前保活和调度责任。 |
| 状态／同步 | `NONE` 不读取/写入状态；`TRANSITION` 自动转换已知状态；`VERIFY` 仅检查且 Release 构建不做该验证；unknown 状态跳过自动管理。手动 `TransitionResourceStates` 支持显式 barrier。[S3] | `TRANSITION` 状态读写需排他；共享资源的并行手动管理须先禁用相应自动状态跟踪，并按实际执行顺序提供 old/new state。 | 不能将此层当自动 render graph 或资源依赖分析器。 |
| 绑定／Shader | PSO/资源签名描述布局，SRB 承载实例绑定；static、mutable、dynamic 分类表达绑定改变频率，而不是资源内容是否可更新；shader 可来自源、文件流或 bytecode。[S1][S8] | 规划 layout、反射、绑定类别、SRB 更新与 fence；跨语言数据布局仍由应用保证。 | 可以保留自有材质/实体类型，但 GPU 投影要适配 SRB/signature。 |
| 并发／提交 | 初始化可请求 immediate/deferred contexts；`Begin(ImmediateContextId)` 为 deferred 指定执行目标；`FinishCommandList` 得到列表；`ExecuteCommandLists` 由 immediate 执行，执行后的列表不再有效。D3D12 queue `Submit` 加锁并同时安排 fence。[S1][S3][S6][S14] | 分配 context 的使用权；共享资源状态、跨队列 fence、deferred 保活和 FinishFrame 时机由应用组织。 | 不是“只能单队列”，也不是“任意调用可自由多线程”。 |
| 动态映射／回收 | D3D12 dynamic buffer 按 context 保存动态地址；DISCARD 分配新空间；NO_OVERWRITE 可复用，存在原生 backing resource 的分支拒绝 NO_OVERWRITE。FinishFrame 回收动态页及描述符分配。[S6] | 不覆盖在途数据；不跨 FinishFrame 使用旧映射内容；staging read 必须先自行同步 GPU 完成。 | 需映射到自有运行时的批次／帧边界，不能让 GC 任意延长映射地址使用期。 |
| 呈现 | 工厂单独接受 `NativeWindow/HWND` 创建 swap chain。D3D12 Present 转 PRESENT 状态、Flush、等待帧、调用 DXGI；主 swap chain 额外 FinishFrame 与 ReleaseStaleResources。[S13] | 窗口创建/消息泵、应用 resize 策略、外部 backbuffer 引用、无 swap chain 时的维护周期。 | 既可用内建呈现，也可研究原生 interop；自定义呈现闭环尚未验证。 |
| 故障 | `CreateDeviceObject` 捕获异常、记录日志并保留 null 输出；Shader 有 `GetStatus`。Present 路径检查 HRESULT；queue 完成值对 device-removed sentinel 做 VERIFY。[S8][S13][S14] | 检查 null/status；定义可观察的错误出口、停止提交与重建规则。 | **未知：** 不能由日志/VERIFY 推出生产级可恢复 device-loss 状态机。 |
| 依赖／许可 | Core 独立；D3D12 本体需要 ShaderTools、D3DBase、NextGenBase 等。Core 主许可是 Apache-2.0；第三方需要逐项归因。[S1][S9–S12][S15][S17] | 锁定 target、子模块和分发工件；区分配置依赖、链接依赖、运行时依赖。 | 接入面小于完整 Engine，但不是零依赖或只有一个 DLL。 |

## 4. 生命周期与同步的关键细节

### 4.1 三层寿命必须分别建模

**事实：**

- CPU 接口对象：引用计数归零触发对象销毁。[S2]
- 原生 GPU 对象：如 `BufferD3D12Impl::~BufferD3D12Impl`，析构调用 `SafeReleaseDeviceObject(std::move(m_pd3d12Resource), ImmediateContextMask)`，由 device release queues 延迟释放。[S7]
- 动态页／描述符分配：`DeviceContextD3D12Impl::FinishFrame` 向对应管理器交还动态分配，使用已提交 command buffers 的 queue mask；GPU 仍在使用时不会因为逻辑“结束帧”就立即物理重用。[S6][S7]

**重要修正：** 不能笼统复述旧官网的“deferred context 完全不持有资源引用”。当前公开 context 契约确实保持**当前绑定**对象的强引用；`CommandListD3D12Impl` 也保持 deferred context 和 pooled command context。但这不等于保存了该 command list 曾引用的完整资源集合。`FinishCommandList` 会使上下文状态失效，NextGenBase 的提交回收注释仍明确要求 deferred 使用资源在执行前不被释放。安全适配应以后一约束为准，而不是依赖偶然剩余绑定保活。[S3][S6][S7]

**设计推断：** 项目的提交批次可持有最小 GPU 资源引用集合，直到 deferred lists 已执行；之后让库处理原生资源的 fence 延迟回收。这个集合属于 GPU 命令生命周期，不是第二套业务实体世界。是否需要显式引用集合、何时可归还引用，必须通过实际路径确认。

### 4.2 提交、结束帧与等待

- `Flush()` 只适用于 immediate；提交后 PSO 与 shader resource bindings 需应用重新设置。[S3]
- `EnqueueSignal()` 本身不 flush；如果随后 CPU 等待该值，必须确保实际提交，否则可能一直等待未入队的信号。[S3]
- `IDeviceContext::WaitForIdle()` 会提交并等待；`IRenderDevice::IdleGPU()` 不自动 flush 各 immediate contexts，仅等待已提交工作。[S3][S4]
- `FinishFrame()` 不 flush，之后动态内容和已提交的绑定状态需要重建；deferred 上须等相关 lists 被 immediate 执行后调用。[S3]
- `ReleaseStaleResources(false)` 依 completed fence 清理；`true` 是调用者保证资源不再被 GPU 使用时才可采用的强制路径。[S4][S7]

**设计推断：** 离屏或不使用 primary swap chain 的消费者需明确调用顺序，例如提交相关 immediate 工作，再结束对应 contexts 的动态分配周期，并周期性 purge。不能把 `Present` 隐含维护动作遗漏后归因于“引用计数泄漏”。具体多 context 顺序须依真实任务依赖安排，而非硬编码单帧全局等待。

### 4.3 显式状态不等于自动并发正确

`TransitionResourceStates` 即使手动调用仍可能读取内部状态；若多线程并行管理同一资源，头文件要求先置 unknown 以禁用自动管理。设置 `SetState` 只是更新记录，不发送 GPU barrier。正确性还需要把 barrier 放在合适 context/队列、给 producer/consumer 安排 fence。[S3][S5]

纹理初始化数据中的 `pContext` 也有明确约束：初始化后纹理处于写状态，随后在别的 context 使用需以 fence 同步。[S5]

### 4.4 SRB 更新的两个常见误判

1. “Static buffer 不能更新内容”不成立。Static 描述绑定保持不变；同一个 buffer 的内容可以按 usage 更新。[S1][S8]
2. “`ALLOW_OVERWRITE` 会复制描述符并自动保护旧绑定”不成立。mutable binding 的覆盖在 D3D12/Vulkan 要求 GPU 不再访问该 SRB；static binding 的覆盖不会追溯修改已创建 SRB 的静态资源。[S8]

因此，材质热更新或大纹理数组槽位复用必须选择经过验证的 SRB 版本化、dynamic binding 或 fence 后覆盖策略，不能仅用引用计数替代 descriptor 在途保护。

## 5. Shader、呈现及原生互操作边界

### 5.1 Shader 接入

**已核对事实：** `ShaderCreateInfo` 支持 `Source`、`FilePath + pShaderSourceStreamFactory`、`ByteCode` 三种入口；文件/include 可接自有流工厂。默认反射路径要求 bytecode 保留反射信息；`SHADER_COMPILE_FLAG_SKIP_REFLECTION` 是不同路径，不可随意 strip 后假设名称绑定仍能工作。`Shader.h` 列出 FXC（至 SM5.1）和 DXC（SM6.x）选项。[S8]

**最小路线含义：** 应用可以保留自己的资产系统与离线 shader pipeline。Core 的 `Diligent-ShaderTools` 是 D3D12 的实现依赖，却不要求应用使用完整渲染框架或把所有 shader 在运行时从源码编译。[S9][S10]

**未核对项：** 本轮没有跟踪所有 DXIL/反射剥离组合，也没有证明“输入预编译字节码即可完全不分发 dxcompiler.dll”。Win32 DXC loader 明确通过 `LoadLibraryA` 加载指定库或 `dxcompiler`；实际所需 DLL 要由确定的 shader 格式、反射路径和 target 工件验证。[S10]

### 5.2 呈现

`CreateSwapChainD3D12` 是独立于设备创建的 API；只支持 graphics context 并接受平台 native window。Win32 HWND 不由此调用创建。[S13]

D3D12 `Present` 自带 backbuffer 状态转换和提交；primary swap chain 驱动 immediate 的动态分配结束与 device purge。`Resize` 实现会清空内部 backbuffer views、flush、idle，然后重建视图；应用自己保存的 backbuffer 引用不在这段内部清理范围内。[S13]

**推断：** 没有呈现需求的消费者不必引入应用样例框架，但需要补上显式维护节奏。若项目要完全自己管理 DXGI，不能只因存在 native handle 就宣称接入完成。

### 5.3 原生互操作

工厂支持 `CreateCommandQueueD3D12` 包装原生队列并 `AttachToD3D12Device`；device 实现提供原生 D3D12 资源包装路径，texture 也能查询 native handle。[S5][S7][S13]

这说明原生 interop 是公开设计的一部分。**它不证明**外部提交能绕过 Diligent 的 fence、状态和 resource mask 协议后仍安全。混合原生/Diligent 提交的所有权、状态交接和故障处理属于未验证接入门槛。

## 6. 最小 Windows x64 / D3D12 依赖闭包

本节给出源码证据导出的候选闭包，**不是成功构建记录**。

### 6.1 目标、工具和默认行为分开看

| 层次 | 已核对的最小路线内容 | 不能扩大解释为 |
|---|---|---|
| 入口 target | 选择 `Diligent-GraphicsEngineD3D12-static` 或 `-shared`；shared 包装并链接 static，公开 interface target。[S9] | 必须整套 Samples 或 DiligentFX |
| 核心 target 闭包 | D3D12 → Common、D3DBase、NextGenBase、TargetPlatform、ShaderTools、接口；D3DBase/NextGenBase → GraphicsEngine；后者及 ShaderTools → GraphicsAccessories/Common。[S9–S11][S18] | 只复制 D3D12 一个目录就能构建 |
| Win32 平台 | Win32Platform 引用 BasicPlatform/PlatformInterface 和 `Shlwapi.lib`。[S18] | 必须采用窗口样例框架 |
| 系统 API | D3D12 target 链接 `dxgi.lib`、`d3dcompiler.lib`；Win32 分支使用 D3D12 loader，非 loader 分支才链接 `d3d12.lib`。[S9] | Win32 运行时无需 D3D12 系统能力 |
| 编译配置 | 根最低 CMake 3.19；检测 `d3d12.h` 与 `atlbase.h`，两者通过才启用 D3D12。ShaderTools 明确 C++17。[S9][S10] | 任意 Windows C++ 编译器无配置即可构建；最低 VS/SDK 版本尚未完整认证 |
| 构建期生成 | D3D12 内建 GenerateMips shader 使用 `fxc /T cs_5_0` 生成头；ShaderTools 无条件 `find_package(Python3 REQUIRED)`，运行 FILE2STRING 生成 HLSL definitions include。[S9][S10] | 用户选择预编译自己的 shader 后，可以省掉本体源码构建的这些步骤 |
| SIMD 默认值 | 根 CMake 的 x86_64 MSVC release 默认附 `/arch:AVX2`，通过配置变量提供选项。[S9] | Windows x64 天然等于项目所有 CPU 支持 AVX2；硬件范围未确认 |
| Core GraphicsTools | `Graphics/CMakeLists.txt` 总会加入该子目录；它链接 xxHash 等。但所核对 D3D12 及其低层闭包没有反向依赖 GraphicsTools。[S9][S12] | 默认 ALL 会构建某目标，就代表应用必须链接并采用其资源注册表、缓存和 uploader |

**可供后续路径验证的配置取向：** 仅启用 D3D12，关闭 D3D11、OpenGL、Vulkan、Metal、WebGPU、Archiver，并保持 tests 关闭；对应根选项为 `DILIGENT_NO_DIRECT3D11`、`DILIGENT_NO_OPENGL`、`DILIGENT_NO_VULKAN`、`DILIGENT_NO_METAL`、`DILIGENT_NO_WEBGPU`、`DILIGENT_NO_ARCHIVER`。本轮没有执行这些选项，也没有确认所有安装目标、默认 ALL 与选定单 target 的工件差异。[S9]

### 6.2 哪些外部依赖不是 D3D12 路线不可避免项

- `ThirdParty/CMakeLists.txt` 仅在 Vulkan/Metal/WebGPU，或 Archiver 与 GL/GLES 组合开启时启用 SPIR-V toolchain；纯 D3D12 配置不因 Core 名称就必须编译 glslang、SPIRV-Tools、SPIRV-Cross。[S12]
- Vulkan-Headers/volk、GLEW、Dawn/abseil、GoogleTest 分别受后端或测试条件控制。[S12]
- WinPixEventRuntime 由 `DILIGENT_LOAD_PIX_EVENT_RUNTIME` 控制；NVAPI 由显式路径及头文件存在条件控制，不是最小要求。[S9][S11]
- **xxHash 是特殊情况：** ThirdParty 的 add_subdirectory 在没有外部 `xxHash::xxhash` target 时无后端条件；因此未修改的 Core 顶层配置仍需要相应源码。GraphicsTools 链接它，但这不证明仅链接 D3D12 本体也需要它进入最终程序。[S12]
- Core 的 ShaderTools 在 Win32 编译 DXC 相关代码并引用随树 DXC headers；这不等于从源码构建 LLVM/DXC 整个编译器工程。[S10]

### 6.3 许可证核对，而非一揽子许可结论

| 项目 | 已核对文本／事实 | 尚需交付前核实 |
|---|---|---|
| DiligentCore | 固定 tag `License.txt` 是 Apache License 2.0。[S15] | 最终分发源码、二进制及 notice 集合；本报告不是法律合规意见 |
| xxHash | 已固定子模块 SHA，LICENSE 是 BSD 2-Clause。[S17] | 是否进入最终二进制；即使不链接也需区分源码分发/构建缓存与产品分发 |
| 随树 DXC 文件 | 本 tag `ThirdParty/DirectXShaderCompiler/LICENSE.TXT` 为 University of Illinois/NCSA（LLVM Release License）文本；ThirdParty CMake 同时安排安装 ThirdPartyNotices。[S12][S15] | 所选 dxcompiler/dxil 二进制的版本及其真实 notices，不能从随树旧 header 许可推断运行时 DLL 的全部条款 |
| DXBCChecksum | ShaderTools 的 Direct3D 分支直接加入该源文件。README 概述称 MIT，但文件头实际可见 AMD 与 RSA MD5 署名/保留声明。[S10][S15] | 需追踪文件及相邻许可材料，不能只写“所有第三方都是 MIT/Apache” |
| Windows SDK / ATL / FXC / 系统或编译器 DLL | 构建依赖关系已核对。[S9–S11] | 开发工具与可再分发工件的许可/分发范围未逐项获取，暂不作批准结论 |
| PIX/NVAPI/非 D3D12 后端依赖 | 当前拟议最小路线可不启用。[S9][S11][S12] | 如果路径验证启用，再追加许可证审查，不能预先算进不可避免许可负担 |

## 7. 自有业务类型与语言运行时：可分离的是什么

### 7.1 由接口证据支持的设计推断

可以形成如下依赖方向：

`共享业务对象 → 自有渲染数据/任务 → GPU 资源句柄与绑定投影 → DiligentCore 接口 → D3D12`

Diligent 的 `TextureDesc`、`ShaderCreateInfo`、PSO/SRB 等不是要求整个业务对象继承的场景节点。创建接口接受明确描述和数据；C interface 与 `IObject` 引用计数提供可包装边界。因此外层可只保存 GPU 资源、PSO/SRB 及必要的提交保活信息，不必复制实体、组件、语言类型注册或业务身份。[S2][S4][S5][S8]

这是**结构上可行的候选**，尚不是 FrostFox 运行时兼容证明。也不意味着“无需任何适配对象”：GPU 绑定布局、资源版本和提交寿命本来就需要表示。

### 7.2 外层至少必须明确的约束

- Borrowed view、owned resource、SRB 内部引用与提交批次引用如何转换；不能对 `GetDefaultView` 的借用引用无条件 `Release`。[S2][S5]
- GC/finalizer 何时释放 GPU wrapper；线程安全的引用计数不能替代 deferred 提交前保活。[S2][S7]
- 语言对象移动后，映射指针或异步 shader 输入是否仍有效；动态映射内容尤其不能越过 FinishFrame。[S3][S6]
- 业务结构到 constant/structured buffer 的大小、对齐、矩阵约定、数组 stride 等如何验证；统一业务类型不代表 GPU ABI 自动相同。[S5][S8]
- 包装层如何把 null 创建结果、shader status、验证回调转换为自有错误体系；不能只把 `void` 方法当“必定成功”。[S4][S8][S14]
- 专用服务器是否完全不链接图形依赖，必须由自己的 target 分层保证；本轮没有检查或改变项目生产构建。

## 8. 与原生自研／NVRHI 竞争的具体条件

以下是对抗评估条件，不是对未分配候选作未经核对的功能断言。

| 条件 | DiligentCore 进入路径验证的理由 | 更有利于原生自研／NVRHI 对照路线的可观察证据 |
|---|---|---|
| 项目希望低层库同时承担资源延迟回收、动态上传、绑定布局与可选呈现 | 所核对实现已经覆盖这些机制，可检验是否减少项目需要维护的代码。[S6][S7][S13] | 对照路线在同一真实消费者中明显减少集成代码，而不是把工作隐藏进另一个框架 |
| 业务对象保持自有，允许 GPU binding 投影 | DiligentCore 公共入口不要求采用场景/业务对象模型。[S2][S4][S8] | 实际 shader/材质需求迫使大量同步的第二套材质状态、冗余反射或无法局部更新的 SRB 重建 |
| 并发模型适合按 context 录制并明确提交批次 | deferred/immediate、多队列 masks 与 fences 有公开契约。[S3][S6][S7] | 所需长寿命录制、跨帧缓存命令、共享可变 descriptor 或特殊队列协议与其 FinishFrame/保活机制实质冲突 |
| 近期需要特定 D3D12 原生能力或精确错误出口 | native interop 提供考察基础，但完整路径未验证。[S13][S14] | 必需能力只能大量绕过库，且原生状态/同步桥比直接原生实现更复杂，或失败无法可靠转换成项目错误契约 |
| 构建和分发成本可接受 | 不必引入完整 DiligentEngine 家族，且非 D3D12 大部分工具链可关闭。[S9–S12] | 实际闭包、AVX2 默认、构建工具或许可清单违反已确定产品约束，而对照路线没有同类障碍 |

**不得直接比较的项：** 没有真实 workload、统一 shader/binding 方案及测量协议前，不比较“更快”“更薄”“更现代”的主观分数。NVRHI 的确切所有权、presentation、binding 和 shader 工具责任应由该主题报告独立核对后，再逐项对照。

## 9. 未知项与反证缺口

1. **生产版本选择**：已核对 v2.5.6，不知道本项目应采用哪个维护版本；没有评估此后修复和 API 改动。路径验证应重新锁定选定版本，而不是把本报告视为 HEAD 认证。
2. **工作负载与能力**：真实第一消费者、shader model、硬件最低线、显存预算、并发规模和跨帧缓存需求尚未确认。
3. **实际依赖工件**：CMake 目标闭包已读，但没有配置/link map/install manifest；最小配置中 xxHash、DXC、GraphicsTools 的实际产物应实证区分。
4. **设备丢失恢复**：已见 `Present` 的 HRESULT VERIFY、queue `GetCompletedFenceValue` 的 `UINT64_MAX` 检查，以及 `WaitForIdle` 的无限等待路径；未追踪完整 DRED、removed-reason、超时、错误回调与重建设备协议。不能因此断言“完全不支持”，也不能宣称库已安全托管恢复。[S13][S14]
5. **状态精度和内存策略**：没有完整审核 aliasing、子资源跟踪、residency、显存预算和所有 descriptor allocator 实现；不由存在资源状态枚举推导全部需求已覆盖。
6. **FFI/运行时契约**：没有核对自有语言 ABI、GC 释放线程、异步数据借用与 shader 内存布局；接口名字相似不构成兼容证据。
7. **许可闭包**：随树 header/source 许可与实际分发 DXC/SDK 二进制许可未完全闭合；DXBCChecksum 的 README 概述与文件头应进一步对齐。
8. **资料质量**：旧官网页面抓取有明显无关广告，且代码示例与固定源码代际不同，故仅保留作历史线索；核心结论均来自版本化头文件/实现。

## 10. 三个可推翻结论的真实路径实验

以下均为**待批准后的路径验证设计，本轮未执行**。每个实验先锁定源码 SHA、子模块、工具链、硬件/驱动、命令及退出码，不把规划当结果。

### 实验 A：仅 GPU wrapper，不复制业务对象；离屏生命周期闭环

- 使用项目真实共享业务类型产生一组最小真实渲染/计算输入，外层仅增加 GPU 资源句柄、数据投影和提交保活；不接入样例框架或第二套实体注册。
- 从两个 deferred contexts 录制实际使用资源的任务；在不同位置释放业务侧引用，保留符合契约的 GPU 提交引用，执行后交还；无 swap chain，显式安排 Flush、FinishFrame、ReleaseStaleResources。
- 加入 dynamic buffer 更新和 staging readback，只有 fence 完成后读取；记录验证层、回读结果与连续多轮后资源/上传分配是否稳定。
- **推翻条件：** 合法保活仍产生 use-after-free/错数据；回收无法闭合；或为了使用低层 API 被迫引入一套与原业务身份保持同步的场景/业务对象世界。单纯存在 GPU wrapper 或 bytes 投影不算推翻。

### 实验 B：真实绑定变更与多队列状态责任

- 选取第一消费者真实的材质更新或 buffer 写读依赖：并行录制，共享只读资源；显式转换及 producer/consumer fence；配置准确的 ImmediateContextMask。
- 对需频繁更新的 binding，分别实现 dynamic binding、SRB 版本化、完成 fence 后 mutable overwrite；不要在 GPU 访问 SRB 时违规覆盖。
- 同一输入、shader 和结果校验下，对照原生自研/NVRHI 路线，记录必需 barrier、额外投影/版本数量、CPU 录制成本与 descriptor/上传峰值；只报告测得值，不预设优势。
- **推翻条件：** 所需语义无法合法表达；正确实现需要持续全 GPU idle；或额外 SRB/状态桥复杂度、内存与耗时超过事先商定产品限额，使复用收益消失。

### 实验 C：最小构建—Shader—呈现—故障出口的端到端闭包

- 在获准环境中仅构建选定 D3D12 target，关闭其他后端/Archiver/tests；记录 Python/fxc/ATL/SDK、实际依赖 DLL/库与许可证 manifest，检查 AVX2 配置是否匹配硬件范围。
- 加载项目离线 shader 字节码和最小真实 PSO/SRB；再用独立 HWND 创建 swap chain，完成 Present、resize、释放外部 backbuffer 引用与 shutdown。
- 验证无效 shader/资源参数的可观察错误；在专门允许故障注入的后续环境才验证 device loss，不在调研工作站随意触发 GPU 故障。
- **推翻条件：** 声称可裁剪的工具实际不可去除且违反产品约束；合法离线 shader 路线要求无法接受的运行时编译器闭包；错误只剩不可捕获日志/无限等待且无法形成项目要求的失败协议；或关闭顺序需要未公开的内部侵入。

## 11. 调研阶段结论

DiligentCore 值得作为**“低层资源与执行机制的复用候选”**进入独立对抗评估，不应被整体渲染框架的体量先行淘汰，也不应因为有自动状态和引用计数就被视为无需生命周期设计的安全壳。下一阶段真正应竞争的是：在同一个真实消费者上，能否以可控的 target/许可证闭包、明确的 context/fence/FinishFrame 契约及足够好的错误出口，减少项目必须长期维护的底层机制。当前证据不足以选定它，也不足以因 Windows x64 背景直接排除它。