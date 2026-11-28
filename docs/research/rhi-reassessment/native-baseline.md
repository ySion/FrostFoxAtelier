# 原生 D3D12 路线：比较基线

历史来源记录：2026-09-25 的选型研究快照，保留固定版本的源码事实、许可证依据、比较理由和证据缺口。文中的“当前”“未确认”均指该次研究；生产 RHI 与 RDG 现已采用 NVIDIA NRI + `atelier_rhi` + `atelier_rdg`，现行选择见[模块决策](../../modules/rhi/decisions.md)，运行证据见[模块验收](../../modules/rhi/acceptance.md)。本文不再提供实施计划或待执行实验。

状态：一手文档核对与设计分析；本轮未构建、未运行 GPU、未改变生产 RHI。检索/核对日期：2026-09-25。

本报告不把 Windows 项目背景推导为后端定案，也不继承旧占位实现的线程、队列、句柄或离屏范围。

## 来源快照

- [M1] Microsoft，Important Changes from Direct3D 11 to Direct3D 12：https://learn.microsoft.com/en-us/windows/win32/direct3d12/important-changes-from-directx-11-to-directx-12 。滚动文档，非 SDK/驱动版本。
- [M2] Microsoft，Creating and recording command lists and bundles：https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles 。本次 Exa 返回文档提交 `6f41993721f0d764311159caa092421e354a8d70`，该提交仅描述文档快照。
- [M3] Microsoft，Non Shader Visible Descriptor Heaps：https://learn.microsoft.com/en-us/windows/win32/direct3d12/non-shader-visible-descriptor-heaps 。滚动文档。
- [M4] Microsoft，ID3D12Fence::GetCompletedValue：https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue 。滚动文档。
- [L1] 本仓 `cmake/toolchain-lock.json`、`CMakeLists.txt`、`cmake/windows-clang.cmake`、`tools/enter-dev.ps1`、`README.md`；只用于描述当前集成条件，不授权把 RHI 设计固定成旧方案。

外部来源通过 Exa 搜索/获取。Microsoft 原文还含一般性性能表述；本报告不将其作为本项目性能结果。

## 已核对事实

| 问题 | 原生责任 | 依据 |
| --- | --- | --- |
| 动态数据 | 更新所需存储、缓冲和 CPU/GPU 同步由应用安排；不能仅依赖 CPU 对象作用域 | M1 Explicit Synchronization |
| 录制引用 | GraphicsCommandList 调用不替应用保持被引用对象；提交引用已销毁资源的命令是调用方错误 | M2 Reference counting |
| 复用 | command list Reset 和 allocator Reset 的条件不同；后者必须等待相关 GPU 工作结束，不能统一理解成一个“帧结束”动作 | M2 Recording command lists |
| 描述符 | RTV/DSV 属于非 shader-visible 描述符，内容在录制时消费，对应槽位可在相关调用结束后复用；不能把所有 descriptor 都机械地按 GPU 使用期退休 | M3 Non-visible views |
| 失效 | fence 完成值为 `UINT64_MAX` 表示 device removed，不可先按普通完成值比较 | M4 Return value |

## 设计推断：自研实际要承担什么

这些不是新增生产契约，而是评估自研工作量时不能省略的责任：

1. 资源和视图创建、用途/格式验证、CPU 持有与录制/在飞持有的组合。
2. 资源状态的权威归属，以及录制顺序与最终提交顺序不同情况下的解释。
3. 上传/动态常量/回读存储的版本与复用条件，不能只实现静态资源创建。
4. Shader 产物、binding layout、原生 root signature/descriptor 与 pipeline 之间的一致性。
5. command allocator/list、描述符及存储各自的复用规则，而非一套粗糙“延迟 N 帧销毁”。
6. 可诊断的提交/等待/设备失效和退出协议。
7. 若消费者需要窗口显示，还需明确呈现和窗口事件的职责；若需要并行录制/多队列，还需对应同步关系。

GPU 使用期不变量不强迫公开 Interface 采用 move-only owner、可复制强句柄或非拥有 ID 中的某一种。应按真实消费者与失效语义选择，不能从旧占位代码倒推。

## 采用条件与反对条件

**支持原生自研的条件（设计判断）**：真实需求需要直接控制绑定、调度、内存或故障语义，成熟候选无法用小且清楚的适配表达；团队愿意长期维护这些机制和验收。

**反对直接自研的条件（设计判断）**：主要收益只是给原生调用换名字，或者成熟库能表达消费者且无需第二套生命/状态管理；此时自研会增加职责，却未证明项目收益。

**不能由本轮推出**：原生一定更快、更轻、开发更快，或某个库的动态派发/引用计数必然成为瓶颈。需要先有同等语义的真实负载。
