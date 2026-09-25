# UI 布局核心

状态：**Main 已选中该独立模块；生产实现尚未开始。** 2026-09-25 的真实候选探针通过，证明 Yoga + 真实文字测量的窄路径可行，不等于本文描述的引擎 Module 已实现。规则状态、现有证据和后续验收严格分开。

## 目的

把有序、有限的几何输入、父级约束与真实叶测量转换为可重复读取的逻辑布局快照。一个原生拥有者管理布局节点、失效和结果发布；调用者不负责修补缓存、第三方树或像素共边。

第一实现方向为 **C++ 窄 Interface + 内部固定 Yoga 求解器**。布局核心不依赖 Runtime、LLVM、RHI、窗口或文字库；真实文本通过测量 Adapter 进入。依赖版本和本轮来源统一见 [research.md](research.md)，行为合同唯一见 [contract.md](contract.md)。当前没有通用语言 UI 绑定，不以此模块推动业务语言新增语法。

## 读哪些文件

| 任务 | 先读 |
|---|---|
| 理解依赖、所有权、约束、错误和快照 | [contract.md](contract.md) |
| 判断为何用 Yoga、为何不先自写/Taffy、何时重开决策 | [decisions.md](decisions.md) |
| 区分本机已证和正式实现尚需验收的行为 | [acceptance.md](acceptance.md) |
| 取上游源码、核对固定版本、许可证和原始实验 | [research.md](research.md) |
| 在下一 session 开始实现、分配文件、申请集成和完成交付 | [session-handoff.md](session-handoff.md) |

## 模块关系

```text
业务/工具的原生 Adapter --复制纯布局输入--> LayoutDocument
真实文本/图片测量 Adapter --内容尺寸----> LayoutDocument
LayoutDocument --未舍入逻辑快照--> UI composition
UI composition --按目标/DPI/政策派生的绘制数据--> renderer / RHI
平台层 --viewport、DPI、输入--> UI composition / 工具控制器
```

- 树节点是私有几何资源身份，不是第二套业务对象。共享 Runtime 继续拥有业务类型、对象、根与执行规则。
- Yoga 是内部唯一可变 style/children/测量缓存权威；外层只维护必要的 owner/代际解析、内容 revision 与已发布几何。
- 核心输出保持逻辑精度；不同目标使用同一逻辑快照。像素化由 composition 单点决定，RHI 不反向拥有布局树。
- 首版同步、单写者；测量回调只读内容快照。平台输入、任务线程和 GPU completion 不进入布局求解过程。

## 第一里程碑

交付一个可独立消费的原生静态库与无窗口命令行验收入口，完成：

1. 有序树的创建、编辑、移除、释放，以及跨 Document/旧代际句柄拒绝。
2. [contract.md](contract.md) 定义的 row/column、fixed/intrinsic/fill_equal、padding/gap、min/max 与明确溢出。
3. 真实叶测量、内容/字体/父约束失效，warm 与 fresh 结果一致。
4. 有版本的完整未舍入快照；失败不发布半结果，快照寿命与下一次布局解耦。
5. 正式 CMake/依赖/许可证接入及开发/发布配置的真实验收，证明核心不反向依赖 Windows/RHI/Runtime。

这里只定义交付终点，未把这些行为写成“已经通过”。候选探针覆盖和正式完成门槛见 [acceptance.md](acceptance.md)。

## 非目标

不交付控件皮肤、编辑器、DOM/虚拟DOM、完整 CSS/Flex/Grid、文字 shaping 或字体管理、glyph atlas、绘制、焦点/输入路由、滚动/虚拟列表、动画/transform、窗口、RHI、异步布局、一般语言绑定。简单 Rect/Size/约束值归本模块，不借机建设公共 math/job/asset 框架。

这不是用少量占位函数模拟 UI：承诺的窄规则、所有权、失败和失效都须完整实现；范围外值明确拒绝，不静默忽略。

## 本轮证据摘要

Main 运行真实 Yoga 与 Windows SDK DirectWrite 的无窗口探针，原始与移除 Runtime/JIT 库后的精简链接均退出码 0：固定盒/填充、resize、reorder、remove，DPI 热/冷对照，真实文字内容/字号/父约束及 dirty 路径全部通过。精简路径仍保留当前 global new/mimalloc、yogacore、DWrite 与 Windows 系统库，不是 Windows-free。嵌套相对偏移舍入观察支持保留逻辑精度再由 composition 输出，**不是 Yoga 错误结论**。

[原始输出](../../../out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json) 与 [依赖来源](../../../out/review/wave1-meeting/yoga-provenance.json) 位于本机 out；若该目录未随交接保留，使用 [acceptance.md](acceptance.md) 的证据摘要并按固定来源重建，不假称本地已有日志。没有布局性能基线，也没有完整模块实现通过证据。

## 工作范围与共享文件

本目录是下一 session 的模块启动包，不授权修改 Wave 1 核心语义。未来生产文件建议限定 `src/ui_layout/` 和 `acceptance/ui_layout/`；这些路径是规划，不表示现已存在。根 CMake、cmake 下接入、依赖锁、工具链锁、全局 README/正式验收由 Main 或指定集成者独占；详细所有权见 [session-handoff.md](session-handoff.md)。
