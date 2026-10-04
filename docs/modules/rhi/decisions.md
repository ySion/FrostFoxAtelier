# RHI 实施决策

本文件维护当前选型及理由；行为契约以 [contract](contract.md) 为准，运行结果以 [acceptance](acceptance.md) 为准。[来源记录](../../research/rhi-reassessment/README.md)保留固定版本的研究依据，不再提供并行实施计划。

## 已落地选择

| 选择 | 理由 | 维护入口 |
| --- | --- | --- |
| 固定 gfx 作为 RHI 主体 | 复用资源、反射、绑定、descriptor、barrier、命令和退休机制，不建立第二份状态与资源账 | [依赖锁](../../../cmake/dependencies-lock.json)、[可重放补丁](../../../third_party/gfx/atelier.patch) |
| 宿主保留 Context 与 Pipeline | 硬件/验证门禁、执行 owner、完整管线准备和执行域寿命有明确责任；无需逐函数代理、profile 对象或 Publisher | [公开入口](../../../src/rhi/rhi.hpp) |
| owner 串行驱动 owned context | 与固定 gfx 的帧槽、常量池和状态机制匹配，GPU 仍可异步在飞 | [执行域契约](contract.md#执行域与所有权) |
| Vertex、Mesh、Compute 共用主体 | 实际离屏消费者覆盖三类管线和 GPU 生成的间接参数；Mesh Shader 是硬件门槛 | [正式消费者](../../../acceptance/rhi/main.cpp) |
| 完整候选成功后移动替换 | 失败保留旧 Pipeline；新 Program 的参数由消费者绑定，不建第二份发布/绑定表 | [准备与替换](contract.md#管线准备与替换) |
| 编译、反射、主字节码及 PSO 都参与成功判断 | 有效 handle 或某一阶段生成 PSO 不足以证明所请求管线完整可用 | [生产实现](../../../src/rhi/rhi.cpp) |
| 保留提交事实和失败终态 | Execute 后的 Signal 等失败不能被当成未提交，清理必须区别排空与重新执行 | [失败与关停](contract.md#失败与有界关停) |
| 普通借用限定为资源、参数和录制 | frame/finish/close 与执行域仍由 Context 管理；原生 interop 只在专门回归中使用 | [借用范围](contract.md#借用-gfx-的许可范围) |
| 一套正式消费者与必要后端回归 | 验收经过真实 Interface；native fence gate 等诊断只验证对应原生路径 | [验收范围](acceptance.md) |

## 取舍理由

- 原生类型与业务语言共享权威类型，不要求自行重写全部 GPU 底层机制。现有 gfx 能承担正式消费者的资源与执行职责；SDK clear/copy 路径仅留下[历史可行性证据](research.md)，自建 Recording/Ticket/ResourceId 方案不再是实施入口。
- 整体复用仍需要履行实际契约。普通绑定、资源导入、外部队列和并发录制的责任不同，不能将某条路径的延迟退休扩展成所有使用方式都安全。
- 宿主选择 owned context；原生 interop 会把持久 queue/list、状态恢复和同步责任交回消费者。当前业务需求没有要求承担这套额外执行模式。
- gfx 的快速原型定位或第三方采用记录不能替代本项目验证；选型依据是所需语义、真实消费者和修正后的行为矩阵，没有性能胜出声明。
- 现有 Renderer 组织场景、视口与 UI 绘制，gfx 保持底层状态和资源退休的唯一权威；将来引入 RDG 也遵守这一边界。业务资源的未实现设计另见[资源连接提案](../resources/design-proposal.md)。

## 补丁与依赖维护

补丁修改 `gfx.cpp`、`gfx.h`、`gfx_core.h`、`gfx_core.cpp`：Mesh PS 可见资源状态、shader/主阶段失败传播、null-list 清理、有限等待、Recording/Closed/Submitted 状态、失败录制门禁，以及间接 finish/sync 调用的错误传播。补丁从锁定原始归档生成；配置先还原 pristine 副本再重放，不在旧修改上叠加。

2026-09-27 的九项[审计修正](audit-2026-09-27.md)已进入同一生产宿主或补丁：间接命令按有效句柄重新取得 Buffer、纹理复制计入 range 偏移、DXGI 和参数数组存储配平、原生拥有数组初始化、成功 finish 后常量池复用，以及宿主 Mesh 候选筛选、输入边界和公开 C++23 要求。细项结果只维护于验收文档。

gfx 的 AVX2 baseline 编译选项已移除，保持项目 x64 基线；不因此宣称全部可选路径已验证。依赖版本和实际归档 hash 只维护于依赖锁。DXC、Agility、D3D12MA、PIX 的 license/notice 与项目补丁由 [模块构建](../../../cmake/rhi.cmake)保留到 `licenses/rhi`；patch 的 LF 由仓库属性固定。

## 当前范围

不同时维护原生自建后端、NVRHI 或 Diligent。OMM 专题提交没有本阶段需求，不自动升级。窗口输入由 Application 管理；Present/resize 已通过现有 Context 接入 Renderer，证据见[模块验收](acceptance.md)与[Renderer 验收](../renderer/acceptance.md)。这不扩展为 HDR 或同一执行域承载多个窗口的验收结论。

并发录制、多 queue、生产原生导入、设备恢复、通用 RDG、业务资源的语言接线和性能基线均不在当前交付中。将来改变执行模式或升级 gfx，应重新核对所有权、同步和现有行为矩阵；历史验收范围见 [acceptance](acceptance.md)，当前接口边界见 [contract](contract.md)。
