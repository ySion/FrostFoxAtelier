# RHI 实施决策

当前架构依据是 [gfx 实施裁决](../../research/rhi-reassessment/gfx-decision.md)。原先的 SDK-only clear/copy 与自建 Recording/Ticket 设计已被替代；历史研究保留为取舍证据，不再约束当前实现形状。

## 已落地选择

| 选择 | 理由 | 维护入口 |
| --- | --- | --- |
| 固定 gfx 作为 RHI 主体 | 复用完整资源、反射、绑定、descriptor、barrier、命令、退休实现，不建设第二套浅包装或状态世界 | 依赖锁与 `third_party/gfx/atelier.patch` |
| 宿主只保留 Context 与 Pipeline | 集中真实硬件/验证门禁、execution owner、完整准备与存活规则；不增加 profile 对象、Publisher、ticket 或逐函数代理 | `src/rhi/rhi.hpp` |
| 一个 owner 串行驱动 owned context | 与固定 gfx 的实际帧槽、常量池及状态模型一致；内部 GPU 仍异步在飞 | 当前 contract |
| 候选成功后移动替换 | 准备失败不伤及已使用的 Pipeline；不自动继承参数或引入双份绑定表 | `Context::prepare`、Pipeline 移动赋值 |
| 不靠 PSO 非空掩盖阶段错误 | 任一明确阶段编译失败阻止 Ready；缺失主阶段在原生 PSO 创建前拒绝 | gfx 编译补丁与准备矩阵 |
| 保留提交事实与错误终态 | 超时、Signal 等执行失败不能被当成未提交，destroy 只 drain 已提交工作 | gfx execution 补丁 |
| 真实 GPU 场景验收 | 同一正式 Interface 测试业务链；后端专门回归仅用于 native failure seam | `acceptance/rhi` |

## 补丁范围

补丁修改 `gfx.cpp`、`gfx.h`、`gfx_core.h`、`gfx_core.cpp`：Mesh PS 资源状态、shader/主阶段失败传播、null-list 清理、有限等待与错误字符串、Recording/Closed/Submitted 状态、失败录制门禁、间接 finish/sync 错误传播。补丁从锁定原始归档生成；CMake 保留生成源码的 pristine 副本，补丁更新先还原再重放，不在旧修改上叠加。

2026-09-27 审计修正继续进入同一补丁：间接命令在 shader 安装后按句柄取 Buffer，纹理上传只保留由活句柄保护的原生资源及 range 基址，纹理复制计入 range 偏移；配平 DXGI 和 buffer 参数数组的拥有存储，原生拥有指针数组先清零；成功 finish 后复用当前常量池。宿主同时落实 Mesh 候选筛选和 PipelineDesc 输入边界，公开 target 传递 C++23。没有增加资源镜像、发布器或第二套执行入口。

gfx 的库编译选项取消 AVX2 baseline 要求，保持项目的 x64 基线。没有因此宣称 gfx 全部可选能力在该基线上得到验证。DXC、Agility、D3D12MA、PIX 均固定实际归档 hash；license/notice 与补丁随构建输出保留。

## 明确排除

不同时维护原生自建后端、NVRHI 或 Diligent；不自动加入 OMM 新提交。暂不做并发录制、多 queue、外部原生互操作生产契约、设备自动恢复、通用 RDG、renderer、语言绑定或 UI。

Present/resize 路线只保留 gfx 能力并贯通失败传播，本轮证据是离屏，不把源码存在当验收。未来加入这些能力、升级 gfx 或变更执行模式时，须重新核对原生所有权与同步，并运行现有行为矩阵。

历史原生路线研究见 [research.md](research.md)；实际支持与未覆盖项以 [acceptance.md](acceptance.md) 为准。
