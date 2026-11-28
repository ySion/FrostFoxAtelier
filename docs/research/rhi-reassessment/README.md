# RHI 来源与选型依据

当前生产路线已升级为固定 NVIDIA NRI + SM 6.6 Bindless `atelier_rhi` + Enhanced Barriers `atelier_rdg`（早期过渡 `gfx` 已退役移除）。现行行为、选型理由与验收分别维护于 [模块契约](../../modules/rhi/contract.md)、[模块决策](../../modules/rhi/decisions.md)、[验收记录](../../modules/rhi/acceptance.md)。本目录保留外部图形库、工业引擎与学术论文的一手选型对比依据，不承担实施计划。

## 当前资源设计研究

[引擎资产与 GPU 资源](engine-resource-industry.md)记录 Unreal、O3DE、Godot、Filament 的资源衔接机制，以及本项目的设计推断。可审查的未实施首增量集中在 [资源连接方案](../../modules/resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)；负责人确认的职责不等于新增 API 已实现或贯通验收通过。

## 固定来源记录

以下材料来自 2026-09-25 至 2026-09-27 的选型阶段。版本、日期和取证限制属于当时快照，不能解释为当前最新版或未选库的能力上限。报告中保留的未知项是研究证据边界，不是待实现任务。

| 文件 | 保留用途 |
| --- | --- |
| [原生 D3D12](native-baseline.md) | 原生 API 的同步、描述符和资源责任；说明自行承担的实际工作 |
| [NVRHI](nvrhi.md) | 固定源码的录制持有、状态、上传、提交和依赖闭包比较 |
| [DiligentCore](diligent.md) | 资源寿命、绑定、执行和构建目标边界 |
| [bgfx / SDL GPU / wgpu](portable-libraries.md) | 不同调度与资源模型的适用条件，避免将各库混作同一种能力 |
| [工业渲染实现](industrial-engines.md) | UE、Godot、Filament 中图层和设备执行职责的分工 |
| [论文与工程资料](papers-and-talks.md) | Shader 参数布局、GPU 任务和 FrameGraph 的原始来源及证据限制 |

这些记录没有将作者性能数字迁移为本项目结果。有效的比较理由已归入[模块决策](../../modules/rhi/decisions.md)。

## 历史运行证据

运行结果统一见[模块验收](../../modules/rhi/acceptance.md)，包括当前正式场景、已完成修正的历史观察和证据边界。早期 SDK 探针不是生产验收入口。本机 `out/` 日志与临时观察脚本不随仓库克隆交付。
