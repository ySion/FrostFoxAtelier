# RHI 历史 SDK 路线依据

访问/记录日期：2026-09-25。外部新检索使用Exa，事实回到Microsoft/Khronos/NVRHI原文。以下区分源码、本机运行和设计判断；不把外部文档当本机设备能力证明。

本页是 gfx 路线确定之前的研究快照，不是当前实现契约。现行入口见 [README](README.md)、[contract](contract.md) 与 [现行决策](decisions.md)。本页“当前”“未来 Module”等用语仅指记录当时；正式目标与运行结果见 [acceptance](acceptance.md)。

## 2026-09-25 的项目与本机证据

- [工程指南](../../engineering.md)：以所有权/生命周期组织Module，C++23、CMake权威、native通用分配沿现有规则，GPU/第三方专用资源按其提供方释放。
- [术语](../../../CONTEXT.md)、[语言契约](../../language-contract.md)：业务对象共用Runtime；私有GPU资源标识不是另一套业务类型世界。
- [runtime.hpp](../../../src/runtime/runtime.hpp):322–333：根不阻止显式销毁；[runtime.cpp](../../../src/runtime/runtime.cpp):663–668、766–771、792–826、1259–1272：native析构、发布准入、GC预算与显式销毁均没有GPU完成含义。
- [工具链锁](../../../cmake/toolchain-lock.json)是准确版本唯一权威。会议时SDK为10.0.26100.0、LLVM23.1.1、MSVC14.50.35717；这是日期化研究快照，不是第二份升级清单。
- 首次硬件debug结果（`out/review/wave1-meeting/rhi-path-probe-hardware-debug.json`）、最小链接命令（`out/review/wave1-meeting/rhi-minimal-build.json`）、最小链接硬件执行（`out/review/wave1-meeting/rhi-minimal-hardware-debug.json`）：Main实测RX6800XT、两次像素一致、debug0错误、GPU-based validation关闭。精确数值与不覆盖项只维护于[acceptance](acceptance.md)。
- 会议[固定 baseline](../../research/wave1-meeting/baseline.json)记录当时的源码依据；当时尚无生产 target。如今已有 gfx 正式目标，不再用旧最小链接证据替代模块验收。

## D3D12 资源与提交

| 来源 | 支撑的契约 | 适用限制 |
| --- | --- | --- |
| [创建/录制command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles) | GraphicsCommandList不持有传入资源引用；allocator复用要等其GPU使用完成；Close集中报告部分录制错误 | command-list Reset与allocator Reset不是同一安全条件；不因CPU对象仍存在推断命令正确 |
| [执行/同步command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists) | queue提交、合法command list与状态限制；多队列资源访问需同步 | 本Module只采用DIRECT queue，未声明多队列支持 |
| [Multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization) | queue Signal在GPU到达位置后发生；Fence::Signal为CPU立即操作；一条fence保持单一时间线 | 不能CPU Signal生产completion fence；不能从单queue值推定其他queue完成 |
| [Readback](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps) | READBACK堆使用buffer；Map不等待GPU，先完成再读取 | 必须仍处理footprint/row pitch，文档示例不等于本机运行 |
| [GetCompletedValue](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue) | UINT64_MAX意味着device removed | 最大值不是正常完成，比较serial前先判断 |
| [GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason) | lost可唤醒fence等待，返回故障原因 | event唤醒不证明命令成功 |
| [D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice) | 真实adapter/feature-level创建；同adapter进程内device单例行为 | 创建成功不代替queue/clear/copy/readback；实验隔离进程避免其他device持有影响 |

上述Microsoft Learn为持续文档。录制与readback页当次附文档提交`6f41993721f0d764311159caa092421e354a8d70`；若干API页附`9267262487657894a8af112d7165006fed5035a7`。这是文档版本，不是系统D3D12 DLL版本或驱动版本。接口以本项目锁定SDK可声明且当前设备真实支持为准。

## 设备丢失、窗口和坐标

- [DRED](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred)：breadcrumb/page-fault诊断有硬件、runtime与配置条件；不保证精确指出故障命令，也不是免费开销。本轮未采集DRED，不宣称其通过。
- [ID3D12Device5::RemoveDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice)：可供隔离真实负向验证，fence置UINT64_MAX；页面列最低Windows Build20348，仍需QueryInterface。当前probe未调用它，设备恢复未测。
- [CreateSwapChainForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)：D3D12传DIRECT queue与有效HWND。它属于presentation，未被首里程碑调用，不能因device工作推断窗口/Present工作。
- [D3D12_VIEWPORT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_viewport)、[RSSetScissorRects](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-rssetscissorrects)、[RECT](https://learn.microsoft.com/en-us/windows/win32/api/windef/ns-windef-rect)：viewport字段为FLOAT、scissor矩形为LONG坐标；支持composition与原生坐标职责分开，不构成一切UI几何都必须snap的依据。

## D3D11 与 Vulkan 对照

- [D3D11 GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)：immediate-context查询S_OK/S_FALSE区分完成；staging Map同步语义与D3D12 Map不同，不把D3D12 fence代码直接移植。
- [ID3D11Fence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nn-d3d11_3-id3d11fence)、[Device5::CreateFence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence)、[Context4::Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal)：较新D3D11接口也支持fence，后者只用于immediate context。头存在不证明设备QueryInterface成功；本轮未跑D3D11。
- [VK_KHR_surface revision25](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_surface.html)：presentation instance extension；[vkCreateDevice](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateDevice.html)是核心1.0，设备创建单独验证请求特征/扩展。offscreen方案不需surface/swapchain。
- [vkDestroyBuffer，VUID-00922](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyBuffer.html)：引用buffer的已提交工作必须完成；[vkInvalidateMappedMemoryRanges](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html)：非coherent回读还需wait和cache可见性链，invalidate不代替同步。两者为核心1.0机制，引用当日滚动规范，不表示选中最新Vulkan版本。

## 成熟实现与依赖

NVRHI研究固定commit：`d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`，上游日期2026-09-23；不以其CMake project VERSION 1.0.0充当精确源码版本。

- [ProgrammingGuide](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/ProgrammingGuide.md)：应用负责native device/queue；内部资源refs、状态追踪与runGarbageCollection负责GPU完成后的回收。名称中的GC不等于本仓业务tracing GC；命令资源管理仍有具体调用条件。
- [CMakeLists](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/CMakeLists.txt)：DX12要求DirectX-Headers/Guids targets，默认fetch `v1.717.0-preview`；Vulkan-Headers默认`v1.4.352`；DX11/DX12/Vulkan、validation等开关应按选择显式锁定。NVAPI/Aftermath/RTXMU不随本里程碑引入。
- [NVRHI LICENSE](https://github.com/NVIDIA-RTX/NVRHI/blob/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/LICENSE.txt)：MIT；保留版权和许可证。不声称预览header依赖已获准接入或历史其他pin不可用。
- Vulkan许可研究样本固定为v1.3.280：[Headers LICENSE](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.280/LICENSE.md)声明Apache-2.0/MIT，部分双许可、parse_dependency.py例外；[Loader LICENSE](https://github.com/KhronosGroup/Vulkan-Loader/blob/v1.3.280/LICENSE.txt)说明多数Apache-2.0、部分更宽松许可。样本只服务对照，不是采纳或最新版声明；新增依赖需逐包保留license/notice。

SDK路线不新增以上源码依赖。本机SDK许可文件位于`C:/Program Files (x86)/Windows Kits/10/Licenses/10.0.26100.0/sdk_license.rtf`和`sdk_third_party_notices.rtf`；已确认文件存在，不据此推定任意SDK/系统DLL可再分发。部署使用系统图形runtime；如未来新增可再分发组件，另核其具体许可和锁定闭包。

## 证据等级

- **本机实测**：Main执行的两份硬件run及精简链接路径，详见acceptance。
- **源码/文件证据**：本仓Runtime/构建链、已安装SDK头库、固定NVRHI源码及许可。
- **外部机制支持**：Microsoft/Khronos接口文档，不替代本机feature/debug/driver检测。
- **历史取舍**：SDK 路径只证明当时的硬件与链接可行性；资源和执行模型已由[现行决策](decisions.md)确定。
- **未证实**：性能、其他平台/设备、WARP、透明device recovery、尚未实现wrapper的行为。历史探针结果不能升级为当前生产能力。

