# RHI 实施决策

本文件维护当前选型及理由；行为契约以 [contract](contract.md) 为准，运行结果以 [acceptance](acceptance.md) 为准。[来源记录](../../research/rhi-reassessment/README.md)保留固定版本的研究依据，不再提供并行实施计划。

## 已落地选择

| 选择 | 理由 | 维护入口 |
| --- | --- | --- |
| 固定 gfx 作为 RHI 主体 | 复用资源、反射、绑定、descriptor、barrier、命令和退休机制，不建立第二份状态与资源账 | [依赖锁](../../../cmake/dependencies-lock.json)、[可重放补丁](../../../third_party/gfx/atelier.patch) |
| 宿主保留 Context 与 Pipeline | 硬件/验证门禁、执行 owner、完整管线准备和执行域寿命有明确责任；无需逐函数代理、profile 对象或 Publisher | [公开入口](../../../src/rhi/rhi.hpp) |
| owner 串行驱动 owned context | 与固定 gfx 的帧槽、常量池和状态机制匹配，GPU 仍可异步在飞 | [执行域契约](contract.md#执行域与所有权) |
| Vertex、Mesh、Compute 共用主体 | 实际离屏消费者覆盖三类管线和 GPU 生成的间接参数；Mesh Shader 与 DXR 1.1 inline RayQuery 都是硬件门槛 | [正式消费者](../../../acceptance/rhi/main.cpp) |
| 完整候选成功后移动替换 | 失败保留旧 Pipeline；新 Program 的参数由消费者绑定，不建第二份发布/绑定表 | [准备与替换](contract.md#管线准备与替换) |
| 编译、反射、主字节码及 PSO 都参与成功判断 | 有效 handle 或某一阶段生成 PSO 不足以证明所请求管线完整可用 | [生产实现](../../../src/rhi/rhi.cpp) |
| 准备在 Context 工作线程，登记在 owner 线程 | 编辑器 UI 线程不等待编译、PSO 创建或 GBV 首次使用补丁；gfx context 仍只由 owner 线程改动。同步 `prepare` 内联同一作业，不维护两份准备代码 | [异步准备](contract.md#异步准备与-dxil-磁盘缓存) |
| GBV 补丁靠不执行的预热录制提前 | 2026-10-19 实测：创建 PSO 时就打补丁让 SDK Layers 1.619.1.0 崩溃；工作线程只录 `SetPipelineState` 可以吸收首次使用成本，执行时校验不变 | [Renderer 验收](../renderer/acceptance.md) |
| 引擎自有 DXIL 缓存 | 异步准备不经过 gfx 的编译函数，gfx 自带的 `EnableShaderCache` 覆盖不到它；自有格式把编译器身份、全部参数放进键，条目带校验和，损坏即重编 | [缓存契约](contract.md#异步准备与-dxil-磁盘缓存) |
| 保留提交事实和失败终态 | Execute 后的 Signal 等失败不能被当成未提交，清理必须区别排空与重新执行 | [失败与关停](contract.md#失败与有界关停) |
| 普通借用限定为资源、参数和录制 | 窗口同步、frame/finish/close 与执行域由 Context 管理；原生 interop 只在专门回归中使用 | [借用范围](contract.md#借用-gfx-的许可范围) |
| 显式同步绘制前窗口尺寸 | 隐藏弹窗可准备正确画布；复用现有排空、退休与 resize，消除为旧尺寸额外绘制和 Present 的需要 | [窗口合同](contract.md#窗口与读回) |
| 一套正式消费者与必要后端回归 | 验收经过真实 Interface；native fence gate 等诊断只验证对应原生路径 | [验收范围](acceptance.md) |

## 取舍理由

- 原生类型与业务语言共享权威类型，不要求自行重写全部 GPU 底层机制。gfx 承担设备资源与执行职责；早期原生 SDK 探针只证明硬件和链接可行性，不作为另一套生产入口。自行维护后端需要承担的责任见[原生 D3D12 比较](../../research/rhi-reassessment/native-baseline.md)。
- 整体复用仍需要履行实际契约。普通绑定、资源导入、外部队列和并发录制的责任不同，不能将某条路径的延迟退休扩展成所有使用方式都安全。
- 宿主选择 owned context；原生 interop 会把持久 queue/list、状态恢复和同步责任交回消费者。当前业务需求没有要求承担这套额外执行模式。
- gfx 的快速原型定位或第三方采用记录不能替代本项目验证；选型依据是所需语义、真实消费者和修正后的行为矩阵，没有性能胜出声明。
- 现有 Renderer 组织场景、视口与 UI 绘制，gfx 保持底层状态和资源退休的唯一权威；将来引入 RDG 也遵守这一边界。业务资源的未实现设计另见[资源连接提案](../resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)。

## 补丁与依赖维护

补丁修改 `gfx.cpp`、`gfx.h`、`gfx_core.h`、`gfx_core.cpp`，覆盖资源状态、管线失败传播、有限执行与关停、根参数预算、AS 屏障、原始时间戳、绘制前窗口同步，以及分步构建 kernel（`GfxKernelBuild`：任意线程用已编译的阶段创建 root signature 与 PSO，owner 线程登记）。补丁从锁定原始归档生成；配置先还原 pristine 副本再重放，不在旧修改上叠加。按 gfx 修订号隔离源码根，避免升级时替换父目录却保留嵌套 SDK 的旧下载记录；具体路径见[构建说明](README.md#构建与运行)。

已修正的问题继续由同一宿主和补丁维护：内部缓冲表变化后重新按句柄取 Buffer、纹理复制计入 range 偏移、DXGI 与参数数组配平、原生拥有数组初始化、完成后常量池复用，以及 Mesh 候选筛选、输入边界和公开 C++23 要求。对应运行与历史观察见[验收记录](acceptance.md)。

gfx 的 AVX2 baseline 编译选项已移除，保持项目 x64 基线；不因此宣称全部可选路径已验证。依赖版本和实际归档 hash 只维护于依赖锁。DXC、Agility、D3D12MA、PIX 的 license/notice 与项目补丁由 [模块构建](../../../cmake/rhi.cmake)保留到 `licenses/rhi`；patch 的 LF 由仓库属性固定。

## 当前范围

不同时维护原生自建后端、NVRHI 或 Diligent。OMM 专题提交没有本阶段需求，不自动升级。窗口输入由 Application 管理；Present/resize 已通过现有 Context 接入 Renderer，证据见[模块验收](acceptance.md)与[Renderer 验收](../renderer/acceptance.md)。这不扩展为 HDR 或同一执行域承载多个窗口的验收结论。

并发录制、多 queue、生产原生导入、设备恢复、通用 RDG、业务资源的语言接线和性能基线均不在当前交付中。将来改变执行模式或升级 gfx，应重新核对所有权、同步和现有行为矩阵；历史验收范围见 [acceptance](acceptance.md)，当前接口边界见 [contract](contract.md)。
