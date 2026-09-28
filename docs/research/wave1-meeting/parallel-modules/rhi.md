# 支线候选 RHI：单队列无窗口 GPU 资源闭环

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

作者 WaveRhi；2026-09-25。状态：独立初稿与两方质询完成后，Main已运行硬件debug SDK探针并裁决入选窄离屏RHI。原初稿的候选比较保留为决策过程；下一session文档见[模块目录](../../../modules/rhi/README.md)。本席未执行构建、测试或GPU实验；Main本机证据与尚未完成的wrapper验收明确分开，详见第15节。

## 1. 当前裁决建议

有条件推荐 Windows x64、单 DIRECT queue、无窗口 D3D12 资源闭环；不是 renderer/frame graph/shader 系统。首里程碑必须含 offscreen RGBA8 clear→barrier→copy/readback→queue Signal/fence→逐像素校验→安全退休，不以 CreateDevice 成功作为打通。D3D11 对传统工具 UI 的首个可见绘制更小；Vulkan 当前无跨平台产品收益且新增 loader/headers/ICD 约束。NVRHI 可复用生命周期/状态追踪，但当前固定提交 d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076 的 DX12 CMake 需要 DirectX-Headers/Guids，默认 v1.717.0-preview，不能称现有 SDK 零依赖接入。

## 2. 当前证据

已读会议章程、36项固定baseline、engineering、CONTEXT、runtime-model、language-contract。toolchain-lock.json 的 SDK 10.0.26100.0、Clang 23.1.1、MSVC 14.50.35717；本机 d3d12.h、shared/dxgi1_6.h、wrl/client.h、d3d12.lib/dxgi.lib 存在。仅证明开发文件，不证明设备或驱动能力。CMakeLists.txt 和 src 定向搜索未见 RHI 实现。runtime.hpp:322-333 的根不防显式销毁；runtime.cpp:817-826/1259-1272 的析构/destroy 立即释放对象；CPU GC 不是 GPU 完成协议。

## 3. 所有权要点

Device owns queue/fence/submission records。Recording 从首次引用开始持有资源；取消未提交录制立即撤销其持有；提交将这些持有移交 device 的 in-flight 记录，ticket 只是完成标识，丢 ticket 不释放 GPU 所用资源。CPU owner 消失请求退休，完成值覆盖最后使用才释放/复用。Queue::Signal 的完成不同于 CPU Fence::Signal；UINT64_MAX 是 device lost，不得当全部完成。资源不是业务对象，未来 Runtime 包装析构只撤 owner，不等 GPU、不把业务节点作为 fence root。

## 4. 关键质询结果

模块地图接受“资源提交首里程碑”而非完整RHI覆盖，并补上未提交recording持有、取消和丢ticket的device所有权；我接受其反向质询，明确SDK probe不验证未来wrapper接口。UI接受同逻辑树两个DPI/离屏目标的反例：保留未舍入逻辑快照，由composition按surface/DPI/policy派生像素视图，RHI不反向持Node/业务root或再次round。完整证据、回应、修正和证伪条件见第13节。

## 5. 一手来源

访问日期2026-09-25，Microsoft为持续文档；适用基础D3D12/对应DXGI接口，不承诺可选功能已安装。

- [M1 command list生命周期](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)：command list不持有传入资源引用，allocator复用必须等GPU完成。
- [M2 GPU readback](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps)：readback用buffer，Map不等待GPU。
- [M3 fence完成值](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue)：UINT64_MAX表示device removed。
- [N1 NVRHI生命周期](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/ProgrammingGuide.md)：应用负责原生device，内部ref持有GPU资源，runGarbageCollection处理完成退休。
- [N2 NVRHI依赖选项](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/CMakeLists.txt)。
- [N3 NVRHI许可](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/LICENSE.txt)：MIT。

## 6. 后端比较与失效条件

| 路线 | 可实现的最小闭环 | 工程成本与反证 | 本轮意见 |
| --- | --- | --- | --- |
| SDK 原生 D3D12 | 明确 adapter → device → DIRECT queue → RGBA8 offscreen clear/copy → readback buffer → queue fence → 安全释放 | 状态、录制持有、allocator/descriptor 复用由本模块负责；不能凭“更现代”宣称更快 | 条件首选；先关闭本机路径风险，再决定是否值得独立开工 |
| SDK 原生 D3D11 | device/immediate context → RTV clear → staging copy → query/GetData 或同步 Map → 像素核对 | 初个工具绘制更小；隐式状态/提交与显式 D3D12 模型不同，但不是不能做正确退休 | 若近期目标仅传统工具 UI/轻量渲染，应重审为首选，不同时做备用后端 |
| Vulkan 核心 1.0 机制 | instance/physical device/device/queue → image/memory → command buffer/barrier/copy → fence → host readback | 新 headers/loader/ICD 和 validation 适配；非 coherent host memory 还需 invalidate；新同步 API 需特征检测 | 当前无跨平台要求，不优先；需求转向特定 Vulkan 生态时再裁决 |
| NVRHI + D3D12 | 应用先创建 native device/queue；复用资源追踪、barrier、上传及内部 in-flight refs | 当前固定提交除 SDK 外要求 DirectX-Headers/Guids；还需 native bootstrap、lost-state/宿主边界；不是免费删除所有权问题 | 真实复用候选而非已集成；首个 SDK 探针无需安装它 |

D3D11 不能被简单描述为“无 fence”：`ID3D11Fence` 在 d3d11_3.h，`ID3D11Device5::CreateFence` 在 d3d11_4.h，`ID3D11DeviceContext4::Signal` 只用于 immediate context。是否可用须 QueryInterface/调用证实，不由头文件存在推定。基本事件查询通过 immediate-context `GetData` 的 S_OK/S_FALSE 判断完成；循环仍要有限等待并检查失败。[M4–M6]

Vulkan 首个 clear/copy/readback 不必用 timeline semaphore，更不需要 `VK_KHR_surface`/swapchain；后者是 presentation 扩展，不是 device 创建的前提。`vkCreateDevice` 必须拒绝不支持的显式请求；`vkDestroyBuffer` 要求所有引用它的已提交命令完成。对非 coherent 回读，等待 fence 不替代 host cache invalidate，invalidate 也不替代等待。[V1–V4]

NVRHI 在该提交将 `project(nvrhi VERSION 1.0.0)` 作为项目版本，但准确锁应使用 commit，而不是把此值当 release 身份。它不创建底层 GAPI device；DX12 graphics queue 为必需，compute/copy 可缺。内部 refs 与 `runGarbageCollection()` 负责延后销毁，不是本仓 tracing GC。CMake 默认同时启用 Windows DX11/DX12 和 Vulkan，DX12 默认获取 DirectX-Headers `v1.717.0-preview`，Vulkan-Headers `v1.4.352`；选用时需关闭不需要的后端及 NVAPI/Aftermath/RTXMU，先评估预览依赖是否可接受。不能为了“薄层”偷偷升级已锁工具链；也不能凭此反证所有旧 NVRHI 提交都不适合 SDK-only，旧稳定提交需要另做真实闭环比较。[N1–N3]

**推荐失效条件**：Main 的近期目标若是可交互工具界面、目标设备无 D3D12 支持、或者承担显式提交维护成本却没有对应负载，推荐可转 D3D11/窗口优先。若成熟薄层在同一支持范围显著降低代码/验收负担且依赖闭包可锁，原生窄实现可被替换；现在没有性能实测支持哪个更快。禁止预先设计全平台最低公分母或多后端工厂。[设计取舍]

## 7. Spec / Standards 与风险分级

### Spec

当前没有 RHI 生产代码，本报告没有已证实生产缺陷；不是对现有 Runtime/GC 的失败判定。已读现行契约以 README、language-contract 及当前入口为准，runtime-model 的早期阶段说明不覆盖后续已实现章节。

- **源码证实**：Runtime::destroy、Runtime 析构可终止业务实例，持有根也不阻止。Runtime::check_code_publication 只检查活动执行计数/准入（runtime.cpp:766–771），没有 GPU 完成含义。CPU GC epoch、对象代际、JIT 版本和 GPU submission serial 是四种不同身份/时钟。
- **可证伪风险**：native wrapper 析构若直接释放 GPU 最后引用，会让在飞命令访问已释放资源；改成“等 fence 再返回析构”又可能把 GC 单步软预算拖成长等待。runtime.cpp:663–668 直接调用 native destroy，collect_step:792–810 只在工作单位之间检查时间；正确边界是析构撤销 CPU owner、GPU退休服务保留原生资源，不在 GC 内等 GPU。[M1、M2]
- **设计取舍**：RHI 资源标识属于 device 域，不是第二套业务类型体系；未来 RenderComponent/业务包装的类型、身份仍由 Runtime 管理。RHI 不扫描业务对象、不建立业务 root、不接收可跨提交悬空的 ObjectId 所解析地址。
- **可证伪风险**：CPU调用结束或窗口关闭不代表 GPU 已完成；device removal 导致 fence 返回 UINT64_MAX，不能在 `completed >= ticket` 之前省略 lost 判定。[M3、M7]

### Standards

深模块应让调用者不再重复编写状态转移、在飞持有、readback pitch、取消及错误后释放规则，而不是把每个 D3D12 方法换名称暴露。内部可以先使用 ComPtr、连续 submission 记录与明确单线程拥有者；没有证据需要虚拟设备继承树、通用任务系统或资源注册反射。真实 GPU 分配/释放遵循 D3D/COM；通用 native 容器沿现有 allocator/CMake 规则，不把 GPU 内存纳入 mimalloc。

不承诺驱动相关固定延迟、所有设备支持或 renderer 性能。已知成本是每次资源创建、命令录制持有和提交维护；性能未知，不用“zero-cost RHI”包装。生产首版应批量操作并避免命令热循环反复名称查找；进一步 suballocation/allocator 池需真实工作集，不先建框架。

## 8. 接口方向与资源所有权

以下是待 Main 冻结的语义方向，不是已发布 API。

| 接口/实体 | 明确责任 | 不能泄漏给调用者的负担 |
| --- | --- | --- |
| Device / DeviceSession | 自有 native device、唯一 DIRECT queue、fence、设备世代、资源登记与 in-flight records；显式 single-owner thread | 不依赖 Runtime、LLVM、UI；跨设备句柄直接拒绝 |
| Texture / ReadbackBuffer | move-only CPU owner，首版一种颜色格式/2D单mip单采样；清晰尺寸/usage验证 | 不让业务代码保存 COM pointer/GPU地址；slot generation 只防误身份，非保活 |
| Recording | 第一次引用即持有资源；记录 clear/copy，冻结资源使用集合；首版单个未提交录制、按序提交 | 不将资源生命仅绑到 CPU调用栈；资源状态留在该模块 |
| submit(recording) → Ticket | 提交前完成可失败的记录存储准备和 Close；移交持有给 device 自有 in-flight 记录 | ticket 是 `{device_epoch, serial}` 完成标识，不是资源 owner；丢 ticket 不丢持有 |
| poll(ticket) / wait(ticket, deadline) | 区分 pending、complete、timeout、device_lost、foreign/stale ticket | UINT64_MAX 不算 complete；timeout 不触发释放、不伪造完成 |
| map_completed(readback,ticket) | 只在真实完成且资源对应后交付有边界的行数据/RAII借用；映射不跨 owner生命 | 正确处理 footprint offset/row pitch；Map 不负责等 GPU |
| collect_retired / close | 驱动完成回收，拒绝新提交并协调显式关停 | native析构不隐式wait/分配退休节点；GC回调只撤owner |

生命周期必须覆盖完整消费者链，而非只查生产者：资源创建→record时借用转内部持有→Close后仍持有→submit移交→queue完成→device退休释放。取消未提交 recording 撤销其持有且不得改动已提交资源状态；首版限制一个待提交 recording、串行记录/提交，避免先做一般并行状态合并器。提交后的 list/allocator、shader-visible descriptor区间（将来存在时）、上传/临时buffer与 texture 都各自等最后 GPU 使用；不是只保 texture。

**提交失败边界**：`ExecuteCommandLists` 不返回 HRESULT，因此 Close 与输入验证在前，随后 queue Signal 等完成证明。若 Execute 已发生而 Signal 失败，不能当“完全未提交”回滚释放；冻结该 device 的新工作、保留在飞持有直到确定 device lost/完成处置。普通参数/能力失败不污染既有有效资源；信号序号不得回绕或使用 UINT64_MAX 作为正常值。多队列时需要每队列完成关系，本轮不把一个 serial 冒充跨队列屏障。[M1、M3、M8]

**析构与关停**：CPU owner 的释放应可在不等待 GPU、不分配新内存的条件下完成（如资源记录预有退休链接或已由 in-flight 批次持有；不规定必须另起通用容器）。宿主正常关停顺序为停止提交→结束/取消 recordings→撤业务 wrapper owners→显式有限等待/退休→释放队列/设备；DeviceSession control block 必须覆盖仍在飞资源。Runtime 停止并不关闭 GPU。若等待超时仍未判定 device lost，不提前重用旧内存；可以保留到单独故障处理/进程退出，而非伪造成功。[设计方向]

### 未来自有语言绑定

C++ 核心先独立成立；未来业务包装通过共享 NativeRegistration/TypeDefinition 发布，包装保存 opaque owner token，不另立脚本对象表。跨语言参数只传固定宽度值、受检 opaque资源标识和明确拥有/借用范围；不把 std::expected/std::span/ComPtr 布局当永久 ABI。当前最小语言只有受限 scalar/引用局部等能力，不能声称已能表达通用 RHI。此里程碑不扩语法、不改 Invoker，不等待通用语言完成。

GPU 提交不能保留 JIT 裸函数地址或业务对象回调。未来读回完成事件应由宿主轮询后在现有执行准入下派发；如果需要代码租约，那是宿主事件桥的明确责任，GPU fence 本身不等于代码/Runtime所有者。

## 9. 窗口、renderer 与设备丢失边界

- **RHI不等于renderer**：renderer负责场景可见性、材质、draw/dispatch组织；frame graph负责pass依赖/生命周期计划；shader系统负责源码编译、反射、变体/缓存。RHI未来可消费已编译 shader/PSO描述，但当前首里程碑连这层也不包括，不预先选DXC/自有shader语言。
- **平台窗口不是离屏硬依赖**：probe不创建 HWND，不创建 swapchain。以后 Win32 presentation bridge 接 platform拥有的 HWND、目标尺寸和surface世代；D3D12的 CreateSwapChainForHwnd 接 DIRECT queue，而非 ID3D12Device。窗口事件/消息泵/DPI/输入仍由平台负责，resize/close由桥层协调 backbuffer最后使用与释放，不把Windows头文件扩散到纯值布局核心。[M9]
- **坐标政策**：逻辑布局快照不因GPU存在而舍入；composition按目标surface/DPI/策略生成像素视图。viewport的FLOAT与scissor的RECT整数按各API契约转换，RHI不逐child.width再次round，不反向拥有Node/字体/业务root。[M10]
- **device_lost是终态而非“全完成”**：停止新提交，所有未完成ticket返回明确device_lost，读取GetDeviceRemovedReason；把旧device域的句柄全部判无效，不把它们用于新device。DRED/breadcrumb/page-fault信息只在接口/硬件支持时可用；并非通用完备诊断，不启用也不能宣称验证过。未来恢复须由拥有资产/CPU重建数据的上层重建，新device_epoch不得复用旧ticket。透明恢复、保存整个世界、强制TDR不在首里程碑。[M3、M7、M11]

## 10. 完整首里程碑、非目标与准入门槛

**名称：Windows x64 单队列离屏 RHI 资源提交闭环。** 完整的是以下已声明子集，不是完整RHI API覆盖。

1. 在锁定工具链上形成独立C++模块，参数与错误契约、拥有线程、资源世代/句柄失效明确；真实枚举所选adapter并报告硬件或WARP，不静默软件回退；支持范围失败可诊断。
2. 支持有限正尺寸 RGBA8_UNORM、2D/单mip/单采样 render-target纹理及readback buffer；真实clear→状态转换→copy→queue signal→有限wait/poll→逐像素核对，处理非紧致row pitch。两次不同颜色/资源复用不读旧数据。
3. 资源CPU owner在record后撤销仍可提交；取消record撤内部持有、不执行命令；提交后丢ticket仍由device保留在飞资源；完成前不复用allocator/资源slot，完成后才退休；陈旧/跨device句柄与ticket拒绝，失败不将新内容标为完成。
4. 明确Close/Signal/创建/Map失败、timeout与device_lost；设备丢失不发成功完成事件，不读回未完成数据；支持正常关停和失效device清理边界；开发路径记录debug layer是否实际启用。
5. 以真实模块公开入口而非直接SDK旁路验收上述规则；当前一次性probe只能关闭平台入口风险。候选模块实现后另跑其入口，不能拿本probe替代所有权/取消/句柄规则验收。

**非目标**：第二后端、其他OS/架构、swapchain/present/HDR/多窗口、renderer/scene/material、frame graph、shader编译/PSO/draw/compute、bindless/ray tracing、多queue/multi-GPU、placed-resource内存别名/驻留系统、后台render线程、自动设备恢复、文本/UI绘制、通用语言绑定与GC重构。随后是否扩哪一项由真实产品目标决定，不由此列表自动排期。

若Main要求“当轮就产生可交互工具窗口”，本里程碑不满足该目标，不能靠改称“已完成RHI”掩盖。若仅WARP通过，结论只涵盖该软件实现；硬件adapter缺失/失败必须保留原错误，不外推支持平台或GPU型号。

## 11. 依赖、许可与固定方式

### 本轮SDK路线

只使用现有 Windows SDK 10.0.26100.0 的D3D12/DXGI/Win32/WRL头与d3d12.lib、dxgi.lib；运行使用系统D3D12/DXGI及实际显示驱动，不下载Agility、Vulkan、DXC或新allocator。SDK头库由现有toolchain-lock.json唯一锁定；驱动/OS runtime不因SDK版本锁定，验收证据必须记录实际OS、adapter/LUID、驱动版本及debug状态（probe打印adapter/LUID；OS/CPU/实际GPU驱动记录见Main的`out/review/wave1-meeting/machine-environment.json`）。

SDK非本仓开源依赖包；本机 `C:/Program Files (x86)/Windows Kits/10/Licenses/10.0.26100.0/` 提供 sdk_license.rtf / sdk_third_party_notices.rtf。这里仅确认许可文件所在，不作额外再分发许可推断；不把系统DLL拷进产物冒充vendor bundle。任何新增可再分发组件须按其具体许可审查。

### 若改成熟实现或Vulkan

NVRHI准确commit见N1–N3，MIT文本需随源码/二进制分发保留；连同所需DirectX-Headers/Guids及许可证闭包，由Main在现有dependencies-lock.json登记固定URL、commit/版本、SHA-256、build options与局部patch。未取得归档不填虚构hash；禁止只固定moving main或让默认FetchContent自行漂移。当前pin的preview依赖是选型成本，不是已批准升级。

Vulkan比较机制采用核心1.0 API；研究许可证样本固定Vulkan-Headers/Loader v1.3.280，不宣称最新或选中：Headers声明Apache-2.0/MIT（部分文件双许可，parse_dependency.py例外），Loader大部分Apache-2.0、个别更宽松许可。未来若选用，逐文件/包保留license/notice，并分别锁headers、loader、validation与SDK/工具版本；驱动ICD实测能力独立记录。没有本机Vulkan loader/device可用性的验证。[V5]

根CMake/依赖锁/许可证打包由Main串行接入；候选session只拥有自身目录与明确局部源，不能在src/runtime、acceptance/jit或cmake中暗建依赖。当前根CMake无条件含JIT验收，故源码隔离不等于顶层配置已摆脱LLVM；独立target接入由Main统一处理（采纳WaveModuleMap提醒）。

## 12. 本轮至多三项优先动作及Main入口

### A. 本机SDK真实闭环（源码已交，未由本席运行）

路径：`out/review/wave1-meeting/rhi-path-probe.cpp`。核心直接调用真实SDK，非mock、非复制生产RHI（目前不存在）。Main在已准备好的锁定开发环境中编译该一次性文件；以下命令只服务实验，不新增永久构建权威：

- 编译：`clang-cl /nologo /std:c++latest /W4 /WX /EHsc /MD /DWIN32_LEAN_AND_MEAN /DNOMINMAX out/review/wave1-meeting/rhi-path-probe.cpp /Fe:out/review/wave1-meeting/rhi-path-probe.exe /Fo:out/review/wave1-meeting/rhi-path-probe.obj /link d3d12.lib dxgi.lib`
- 硬件：`./out/review/wave1-meeting/rhi-path-probe.exe --debug`。
- 明确软件对照：`./out/review/wave1-meeting/rhi-path-probe.exe --warp --debug`。
- 若要强制debug证据，用`--require-debug`代替`--debug`；缺层必须失败，不安装新组件来改写当前环境。`--debug`缺层会明报并继续functional路径，不能算debug验证通过。

**精确预期而非实测值**：退出0；打印selected adapter、vendor/device、software、LUID与debug状态；两次37×19图像，分别RGBA=(255,0,255,255)及(0,255,0,255)，每次703像素/2812颜色字节均吻合（mismatches=0）；row_bytes=148，row_pitch由GetCopyableFootprints报告，不假设148紧致；ticket依次1/2，completed≥对应ticket且非UINT64_MAX；第二次只在第一次完成后Reset allocator/复用RTV；输出caller owner提交前撤销、submission_hold完成后释放。debug实际启用时检查InfoQueue error/corruption计数为0并保留warnings；未启用打印not_performed。

**负向观测**：无硬件D3D12时明确失败，不自动WARP；required-debug缺层失败；任何创建/Close/Signal/Map失败报stage与HRESULT/removed reason；10秒等待超时报timeout，不将其当完成。失败使用隔离进程退出而不提前stack-unwind释放尚可能在飞资源，这不是生产RHI错误恢复方案。

**不覆盖**：本探针未注入device loss、未验证自动恢复/Shader/draw/Present、未实现未来wrapper的record取消/丢ticket/陈旧slot/泄漏统计；COM hold是API存活演示，不是生产retirement-manager实现。平台探针通过≠RHI模块里程碑通过，也不证明更多GPU/驱动可用或有性能优势。

### B. 只在入选后实现并验收真正所有权接口

以第8/10节为边界，在模块自身接口验证未提交取消、caller owner撤销、提交后ticket丢弃、完成前后slot/allocator复用与关停；必要反例是record后caller owner消失但未submit，以及Execute后Signal失败。每个反例必须观察明确状态/释放计数与真实GPU完成，不用“程序没崩”代替。Main若不接受这份维护成本，本轮可只批准布局，不需要凑第二模块。

### C. 设备丢失为受控独立负向门槛

后续使用支持的隔离进程/设备，通过真实`ID3D12Device5::RemoveDevice()`（先QueryInterface；官方最低Windows Build 20348）触发lost；预期fence为UINT64_MAX、GetDeviceRemovedReason失败、未完成ticket归lost、禁止normal-complete/readback，并能有界退出。该实验不要求真实GPU挂死、不修改TDR，也不能代表驱动所有故障或自动恢复。当前probe没有此模式，本席没有执行；若接口/环境不支持，记录unsupported并以发布范围约束，不填“已通过”。[M7、M12]

## 13. 已完成支线质询与修正

### WaveModuleMap：完整目标与未提交寿命

已读`parallel-modules/map.md`独立初稿。对方主张纯布局+条件离屏RHI，窗口非硬依赖、单后端要隐藏真实不变量；要求真实提交/完成/读回/退休。

我的质询：第一，“完整离屏首里程碑”应明确只是资源提交子集，不能外推PSO/shader/draw/Present；第二，仅写GPU提交后寿命漏掉录制但尚未提交和丢ticket。反例：CreateResource→record Clear/Copy→caller owner销毁→稍后submit；M1明确command-list不AddRef，尚无last-use fence的资源也必须被recording保留。

对方已回应并修正map：改称“RHI资源提交首里程碑”；增加首引用record持有→device in-flight移交→完成退休、取消撤持有与丢ticket不丢持有。本人接受其反向质询：现有SDK探针不能证明不存在的wrapper取消/丢ticket；已在第10/12节明确分开platform evidence与module acceptance。未决：Module入选与正式API冻结由Main；若真实产品必须立即Present，当前子集不足即证伪入选理由。

### WaveUi：DPI舍入与GPU生命

已读`parallel-modules/ui-layout.md`独立初稿4.4。对方主张DPI/共边舍入由布局快照输出唯一完成，RHI不能逐child.width round；GPU fence不应保活Node/业务对象，并要求给反例。

我的回应：赞同不持Node/Runtime root，未发现RHI必须反向拥有布局节点的理由；异步需要保留的是输出命令快照/上传/原生GPU资源，而非布局树。质询“舍入归布局核心”的边界：同一逻辑树用于两个DPI/离屏目标是反例；D3D12_VIEWPORT为FLOAT、scissor输入RECT整数，不代表一切GPU几何都该snap。[M10]

对方接受并明确修正：核心输出未舍入逻辑snapshot；composition adapter按surface、DPI、rounding policy派生像素snapshot；不snap也是合法策略；Yoga可设scale=0，嵌套局部舍入另跑真实探针。我的初稿因此把“布局输出纯值”细化为“逻辑值与目标像素视图分离”，并保留RHI不做二次round。未决：snap/clip/hit-test一致性与文本hinting cache key归composition/text边界，不在RHI私定；两个target共享一个被提前舍入的几何缓存即构成反例。

## 14. 补充一手来源及证据等级

所有外部访问日期为2026-09-25。新检索经Exa，内容回到下列一手API/源码；Microsoft滚动文档只证明所列机制，Windows/驱动可用性以Main实测为准。M1/M2页面所附文档提交为`6f41993721f0d764311159caa092421e354a8d70`，多数API页附`9267262487657894a8af112d7165006fed5035a7`；它们是文档修订，不是Windows runtime版本。

- [M4 D3D11 GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)。
- [M5 ID3D11Fence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nn-d3d11_3-id3d11fence)及[CreateFence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence)。
- [M6 Context4 Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal)。
- [M7 GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason)。
- [M8 D3D12 queue/fence同步](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)及[执行限制](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists)。
- [M9 HWND swapchain边界](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)与[真实device创建](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice)。
- [M10 Viewport FLOAT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_viewport)、[Scissor输入](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-rssetscissorrects)、[RECT LONG成员](https://learn.microsoft.com/en-us/windows/win32/api/windef/ns-windef-rect)。
- [M11 DRED范围与硬件限制](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred)。
- [M12 RemoveDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice)：该页要求Windows Build20348，不向较早版本推定。
- [V1 VK_KHR_surface revision25](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_surface.html)：instance presentation扩展。
- [V2 vkCreateDevice，核心1.0](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateDevice.html)。
- [V3 vkDestroyBuffer，核心1.0，VUID-00922](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyBuffer.html)。
- [V4 noncoherent回读](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html)：wait与invalidate职责不同。
- [V5 Headers v1.3.280许可](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.280/LICENSE.md)、[Loader同tag许可](https://github.com/KhronosGroup/Vulkan-Loader/blob/v1.3.280/LICENSE.txt)。这是固定研究样本，不是选型版本或最新版声明。

证据分级：仓库契约/工具链锁/本地头库存在/Runtime源链为源码与文件证据；后端机制与许可为一手文档/源码支持；推荐排序、未来接口与GPU性能收益为设计取舍或可证伪预期。本席未执行实验；Main随后交付的本机证据单列于第15节，不从平台成功推定wrapper或设备恢复成功。生产文件与已验证探针均未再修改。

## 15. Main裁决、实测与独立模块交接

当轮裁决选择了 Windows x64 离屏 RHI，以下保留实际实验范围。后续已经完成正式模块；现行入口为 [RHI README](../../../modules/rhi/README.md)，旧交接清单不再指导开工。

### Main本机证据，不是本席运行

- `out/review/wave1-meeting/rhi-path-probe-hardware-debug.json`：参数`--require-debug`，exitCode=0；AMD Radeon RX6800XT，vendor0x1002/device0x73BF、software=0；debug_enabled=1、gpu_validation=0。
- 两轮37×19颜色回读，row_bytes=148、row_pitch=256、footprint_bytes=4756；ticket/completed分别1/1与2/2，mismatches=0；caller owner提交前释放，submission_hold于fence后释放；debug messages/errors/warnings均0。
- `out/review/wave1-meeting/rhi-minimal-build.json`及`out/review/wave1-meeting/rhi-minimal-build.log`：使用正式release host标志和Clang C++23，删除Runtime/JIT静态与导入库，仅保留global_new_delete对象、共享mimalloc及Windows/D3D12/DXGI库。
- `out/review/wave1-meeting/rhi-minimal-hardware-debug.json`：最小链接结果在同硬件上`--require-debug`运行exitCode=0，两轮mismatches=0/debug0错误。证明路径级独立链接可行，不等于已经建立正式RHI target。

### 证据限制与交接结果

没有WARP或其他设备支持证据；没有GPU-based validation、device-loss注入/恢复、性能验证。进程elapsed不是稳态GPU/CPU性能基线。本机SDK hold示例不证明尚未实现wrapper的未提交取消、丢ticket、slot代际、Mapping与显式关停规则。

六文档已定义下一session可独立实施的真实依赖、ResourceOwner/ResourceId与Ticket区别、record→device in-flight→fence退休链、取消/失败/timeout/lost、无分配/无wait资源析构、明确Device关停、验收矩阵和Main独占集成锁。全程未更改src/acceptance/cmake/语言规范，未改Main已验证探针，未运行构建、测试或GPU工作。
