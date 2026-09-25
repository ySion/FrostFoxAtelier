# RHI：Windows x64 单队列离屏资源提交

状态：2026-09-25，Main 已选入独立推进模块。**SDK 平台路径已验证；生产 RHI Module 尚未实现。** 本目录定义下一 session 的可实施首里程碑，不宣称完整 renderer 或未来 wrapper 已通过验收。

## 目的

提供小而完整的 C++ Interface，封装资源创建、录制持有、提交、完成、回读和安全退休。调用者不重复处理 D3D12 状态、行间距、allocator 复用或在飞生命。

首里程碑称为 **RHI 资源提交闭环**：Windows x64、单 device/owner thread、单 DIRECT queue、RGBA8_UNORM 2D 单采样/单 mip 纹理、readback buffer、clear/copy 和有界等待。无需窗口、swapchain 或 shader 即可验收该子集。

## 阅读入口

- 实现或消费 Interface：先读 [contract.md](contract.md)，它是所有权、错误、失效、关停与线程规则的权威。
- 后端、依赖或范围变更：读 [decisions.md](decisions.md)，了解裁决与重开条件。
- 编写/执行验收：读 [acceptance.md](acceptance.md)，区分已有路径证据与尚需通过的模块行为。
- 核对机制、版本或许可：读 [research.md](research.md)，回到固定一手来源。
- 下一 session 开工：读 [session-handoff.md](session-handoff.md)，落实实施顺序、文件所有权与集成门槛。

## 当前证据

Main 的真实硬件探针使用 AMD Radeon RX 6800 XT，实际启用 D3D12 debug layer；两次 37×19 clear/copy/readback，票据/完成值 1/2，mismatches=0，debug error/warning=0。[原始结果](../../../out/review/wave1-meeting/rhi-path-probe-hardware-debug.json)

Main 另报告去除 Runtime/JIT 链接依赖的最小独立链接与同一路径通过，证据见 acceptance。它们只证明指定机器的 SDK 路径与路径级链接独立，不证明其他 GPU、WARP、设备恢复、性能或未来 wrapper。GPU-based validation 未启用。正式 Module 必须通过自己的 Interface 重新验收。

## 首里程碑完成条件

1. 真实 offscreen clear→copy→queue completion→逐像素回读，正确处理非紧致 row pitch及两次不同颜色。
2. 未提交录制持有资源；取消撤销持有；提交移交给 device；释放 CPU owner 或丢弃 ticket 不导致 GPU 资源提前死亡。
3. 完成前不复用 allocator/slot，完成后退休；陈旧/跨设备标识拒绝；timeout/device_lost 不伪装成功。
4. single-owner thread、有限等待、正常关停与 lost-device 清理明确，通过 acceptance 的模块矩阵后才宣布完成。

## 依赖与非目标

源码层依赖 C++ 标准设施及现有 Windows SDK 的 D3D12/DXGI/Win32/WRL；正式目标沿现有 native 分配和 CMake 规则。版本权威为 [toolchain-lock.json](../../../cmake/toolchain-lock.json)。GPU 专用内存不由 mimalloc/GC 管理。

RHI 不依赖业务 Runtime、GC、LLVM、语言、UI 树、字体、窗口/输入或 scheduler；现有顶层 CMake 仍含 JIT 验收，不能把源码隔离误说成顶层配置无需 LLVM。根 CMake/依赖锁由 Main 串行集成。

非目标：renderer、scene/material、frame graph、shader编译/PSO/draw/compute、swapchain/Present/HDR/多窗口、第二后端/其他平台、多queue/multi-GPU、bindless/ray tracing、GPU内存别名/驻留、render线程、自动设备恢复、文字/UI绘制、通用语言绑定或GC重构。

UI composition 以后按目标/DPI处理逻辑布局，RHI 不持 Node/业务根、不规定像素snap。未来语言包装沿既有共享类型体系，析构只撤 CPU owner，不在 GC 内等 GPU。

