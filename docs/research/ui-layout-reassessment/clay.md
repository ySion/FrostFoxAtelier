# Clay：布局求解与即时 UI 命令模型的边界

## 1. 结论与版本边界

**判断〔推断〕：Clay 值得保留为游戏 HUD、菜单及即时声明式 UI 的候选；作为复杂编辑器的通用布局核心，则应先验证共享轨道、文字、虚拟化和结果所有权，而不是因“无 malloc”直接选中，或因全局状态直接淘汰。** 它提供 solver，但公开使用路径同时组织声明树、ID、交互状态和渲染命令；不是可直接套到任意自有树上的无状态 solver，也不是包含 docking、表格和文字编辑的完整控件框架。[M1][M2]

本报告严格区分两个 pin：

| 标记 | 可核实对象 | 边界 |
|---|---|---|
| **R** | 正式 release `v0.14`；commit `b25a31c1a152915cd7dd6796e6592273e5a10aac` | GitHub release API 的 `published_at` 为 **2025-06-06T01:16:01Z**；不是搜索结果中的前一日标签。[R1][R2] |
| **M** | 本次 GitHub `commits/main` 返回 `e6cc36941ab2af5d81107617039d6f527a1c660b` | commit 时间 **2026-05-20T02:17:58Z**，提交说明为更新 Odin bindings；这是本次可证快照，不把搜索抓取时间当发布日期。[M0] |

**以下机制审计、容量和复杂度结论全部针对 M，不自动适用于 R。** 两者头部都写 `VERSION: 0.14`，但 R 使用 `CLAY({ .id=… })`，M 已改为 `CLAY(id, {…})`，并出现 `CLAY_AUTO_ID`、transition 和 `Clay_EndLayout(float deltaTime)`；仅锁“0.14”不足以复现实验。[R3][M1] M 的许可为 **zlib/libpng**：允许商业使用、修改及再分发，但须保留许可声明、不得冒认原作、修改版须明确标示。[M3]

本轮只读资料与源码，**没有本机构建、运行或性能测量**。文中的 probe 均为待执行方案。

## 2. 所有权、状态和静态帧〔M〕

**〔事实〕** `Clay_BeginLayout()` 调用 `Clay__InitializeEphemeralMemory()`，复位 arena 临时区、数组长度并增加 generation；声明宏构建内部 `Clay_LayoutElement` 树，`Clay__ConfigureOpenElementPtr()` 按值复制配置。`Clay_EndLayout()` 求布局并返回内部 `Clay_RenderCommandArray`。持久区保存 ID 映射、文字缓存、滚动及 transition 状态；业务数据和控件状态仍由调用者拥有。[M1]

- **静态与失效：**没有公开“修改一个 retained 节点后只重算 dirty 子树”的入口。每次 Begin/End 均重新声明、执行布局及生成命令；文字缓存命中不等于几何缓存命中。静态帧可由宿主决定完全不调用 Clay、复用自有结果，但 hover、滚动、动画及外部内容失效须由宿主调度。README 的 retained rendering 指按命令 ID 更新持久图形对象，不是增量布局树。[M1][M2]
- **身份：**`Clay_ElementId.id` 为 32 位散列；有 `CLAY_IDI` 和父作用域 local ID。数组下标不能在排序、插入、虚拟化复用时充当业务稳定身份；父作用域 ID 跨父移动也会改变。`Clay__AddHashMapItem()` 按散列与 generation 判断相同元素，同帧重复触发错误，但不能提供项目级代际句柄或 foreign-ID 检查。[M1]
- **输出寿命：**返回数组借用 arena，下一次同 context 的 Begin 即开始复用临时存储。`Clay_String` 不复制字符；text slices、`imageData`、`customData`、`userData` 也不获得资源所有权。跨帧或异步消费必须保证整个消费期间数据有效且不被修改；拥有式快照、延迟复用或同步消费都是可能方案，不能预设必须逐帧深复制。只复制命令结构体不会自动延长外部资源寿命。M 的退出动画会克隆内部元素，外部文字/资源生命周期还须覆盖退出阶段。[M1] 这一表述按[性能对抗评估](perf-adversarial.md)修正；具体同步方案仍须真实验证。
- **solver 复用：**可忽略绘制类型、用 `Clay_GetElementData(id)` 取纯矩形，但仍需将自有树重新声明为 Clay 内部树。内部 `Clay__CalculateFinalLayout(..., generateRenderCommands)` 的开关不是稳定公开的 layout-only 协议；直接依赖双下划线内部函数意味着承担 fork/API 变动成本。[M1][M2]

## 3. 尺寸、文字与扩展〔M〕

**〔事实〕** `Clay_LayoutConfig` 只有横向/纵向排布、padding、gap、两轴对齐和 sizing；提供 `FIT/GROW/FIXED/PERCENT`，FIT/GROW 带 min/max，GROW 在可用空间内分配尺寸，不能等同 CSS 的全部 flex 规则。普通流中的百分比扣除父 padding 和主轴 gaps；floating 的百分比路径直接用挂靠元素尺寸，不能用同一公式概括全部情况。[M1：`Clay__SizeContainersAlongAxis`]

公开配置**没有容器子项自动换行、grid/shared tracks 或 baseline 对齐字段**。文字 wrap 不等于标签控件 wrap。固定物品格可由宿主分行；跨行共享列宽需宿主先求轨道，再传 FIXED/PERCENT。`Clay_CustomElementConfig.customData` 只是生成 CUSTOM 绘制命令的透传指针，**不是自定义 measure/arrange 回调**；自定义二维布局需在外部计算或修改实现。[M1][M2]

文字路径的关键合同：[M1：`Clay__HashStringContentsWithConfig`、`Clay__MeasureTextCached`、`Clay__CalculateFinalLayout`]

1. **〔事实〕** `Clay_SetMeasureTextFunction()` 回调签名是 `(Clay_StringSlice, Clay_TextElementConfig*, void*) -> Clay_Dimensions`，没有 available-width、baseline 或错误结果通道。Clay 先测量词段，再定横向尺寸、换行、向祖先传播高度、处理纵轴；并非将整段排版问题交给文字引擎。
2. **〔事实〕** 文本枚举声明 WORDS、NEWLINES、NONE 三种模式；源码分词识别 ASCII 空格和换行，超宽单词分支直接输出该词。不能据此声称支持 Unicode 断行、CJK 字间换行、双向段落或完整 shaping；真实字体提供者仍负责度量和绘制一致性。
3. **〔事实〕** M 对动态文字散列内容；`isStaticallyAllocated=true` 则采用地址及长度。缓存键加入 `fontId/fontSize/letterSpacing`，没有字体修订号、DPI 或回调 userData。字体热替换及外部度量变化须 `Clay_ResetMeasureTextCache()`；把可变缓冲误标为 static 会有陈旧结果风险。
4. **〔事实〕** 缓存按 generation 回收部分旧条目；查找命中只比较散列 ID。热帧仍可能散列动态字节，并在换行路径再次调用测量回调取得空格宽度。“缓存热了就没有测量回调”不是成立的合同。

## 4. Floating、滚动、线程与错误〔M〕

**〔事实〕** Floating 支持挂靠父、指定 ID 或根，九点对齐、offset、zIndex、指针捕获与 `clipTo`。`.clip` 提供两轴裁剪和 `childOffset`；可自行维护偏移，也可用 `Clay_GetScrollOffset()`、`Clay_UpdateScrollContainers()` 跟踪拖动/惯性，`Clay_GetScrollContainerData()` 返回 viewport/content dimensions。最终裁剪由 renderer 执行 SCISSOR 命令。[M1][M2]

**〔推断〕** 这足以组合 HUD 锚定、splitter 的外部尺寸约束及 popup，但不是 docking 状态机。关闭屏外绘制只减少命令；节点此前已参与声明和尺寸计算，不能冒充虚拟化。固定行高可用可见项与上下占位尺寸；可变行高还需要宿主维护高度索引、估计修正和滚动锚定。[M1]

**〔事实〕** 多 context 有公开 `Clay_SetCurrentContext()`，每个 context 使用独立 arena；README 给出同线程顺序切换示例，同时明确不支持多线程同时处理实例。M 中 `Clay__currentContext` 和 `Clay__MeasureText` 都是普通全局变量，测量 userData 才存于 context：为 B 设置不同回调，再切 A，并不会自动恢复 A 的回调函数。[M1][M2]

**〔推断〕** 同步串行、多视口或多个文档是可用路径；统一测量函数按 context userData 分派较自然。并行求解或回调重入不可由“独立 arena”推出安全，必须外部串行化或另行隔离实现；是否构成产品阻碍取决于真实线程合同，不直接判为不适用。

**〔事实〕** `Clay_ErrorHandler` 是回调，不是事务性返回值；缺失测量、容量、重复 ID、floating 父缺失、未配平声明等有错误枚举，部分内部错误返回默认对象/NULL 后继续，默认 handler 不处理错误。测量回调只有尺寸返回，不能自然传递 shaping 失败。宿主若要求“失败不发布半快照”，需自行收集错误并控制发布；NaN、负尺寸、回调失败及再次使用的恢复能力均待实测。[M1]

## 5. 分配、复杂度与 C++ 接入〔M〕

**〔事实〕** 调用者先配置容量，用 `Clay_MinMemorySize()` 获取字节数，再交给 `Clay_CreateArenaWithCapacityAndMemory()` 和 `Clay_Initialize()`；内部数组由 arena 切分，没有热路径 malloc/free。默认节点上限 **8192**、word cache **16384**；扩大这两者需要相应存储和重新初始化，不能只改整数。[M1]

更细的容量不能遗漏：`Clay__InitializePersistentMemory()` 将 **scrollContainerDatas 固定为 100、transitionDatas 固定为 200**；即使只设 `.clip`、使用外部偏移，也会登记 scroll 数据。临时 render commands、wrapped lines 等分别按 maxElementCount 分配，因此“一节点一容量单位”不足以预测多行文本、多绘制类型、debug 和退出动画的需求。[M1]

**〔事实及条件推导〕** BFS/DFS 使用 arena 内显式工作数组，核心遍历不必按树深消耗 C 递归栈；应用自己递归声明仍会消耗栈，容量检查也不证明任意深度安全。无堆分配仍有按值写配置、散列、缓存查询、多遍树访问和命令输出成本。[M1]

- `EndLayout` 清理 ID 映射遍历全部桶：成本受配置容量 **C** 而非只有活跃节点 **N** 影响。
- `Clay__CalculateFinalLayout` 对 **R 个 roots** 作无提前退出的冒泡排序，比较次数为 `R(R−1)/2`；普通树只有少量 roots，不可误写成所有 UI 都平方，但大量 floating 是具体反例。
- GROW/FIT 分配内部会重复扫描可调整兄弟；不能不加前提声称全输入 O(N)。动态文本总字节数、word 数、clip 数和退出动画也必须单独计量。
- M 有 transition 数据时可先求目标布局再求最终布局，并克隆退出子树；不把这一成本回灌到 R 或未启用 transition 的负载。[M1]

**〔上游自报〕** README 宣称微秒级布局、8192 元素约 3.5MB；本报告没有复测其 ABI、容量组合、文本提供者或端到端成本，不能据此宣布胜过 Yoga/Taffy。[M2]

**〔事实〕** 头文件要求 C99、C++20 或 MSVC，带 `extern "C"`，一个翻译单元定义 `CLAY_IMPLEMENTATION`；官方 C++ 示例设 C++20。Windows x64/C++23 可据此设计薄接入，但本轮未编译验证；C/C++ struct/packed-enum ABI、编译宏及 DLL 边界须统一。根 CMake 默认开启全部 examples，且 interface-library 代码被注释；不能直接当作干净 core target 引入。字体、窗口、renderer 是应用提供，不由布局核心强制绑定。[M1][M4][M5]

## 6. S1—S9 场景映射〔M；判断为推断〕

| 场景 | 适配点与待补边界 |
|---|---|
| S1 HUD | floating、九点挂靠、zIndex 可用；安全区、DPI/像素共边由宿主定义。 |
| S2 物品格/标签 | 固定格可外部分行；无原生容器 wrap 或自适应共享轨道。 |
| S3 Inspector | FIT/GROW 与多行文字可用；跨行标签列宽及 baseline 需外部协议。 |
| S4 docking/splitter | 外部交互更新尺寸，popup 用 floating；不提供 docking 控件状态。 |
| S5 树/表格/日志 | culling 不是虚拟化；可见项声明、稳定 ID、变量行高索引由上层实现。 |
| S6 本地化 | 真实测量可接入；Unicode 断行、混合字号基线和跨词 shaping 是关键缺口。 |
| S7 嵌套滚动 | clip/offset/extents 可组合；renderer clip-stack、嵌套事件与 100 容量需探针。 |
| S8 局部变更 | 每次调用重新声明求解；没有内建 retained dirty-tree，静态跳帧由宿主负责。 |
| S9 边界失败 | 有错误回调及容量；没有自带事务快照、代际句柄、重入安全合同。 |

依据为 M 的公开结构与上述源码路径。[M1][M2]

## 7. 最强支持、最强反对与可推翻条件

**最强支持〔推断〕：** 对主要由嵌套行列、文字、覆盖层组成且原本就逐帧声明的游戏 UI，Clay 将容量规划、布局和 renderer-neutral 命令组合得直接，能保留宿主业务状态与图形系统；串行多 context、外部滚动和自定义绘制也提供了实际组合点。[M1][M2]

**最强反对〔推断〕：** 若目标核心要原生支持共享轨道、复杂文本段落、静态大树增量更新、可独立发布的纯值快照，那么采用 Clay 不是“替换 solver”这么窄：需要额外排版/虚拟化/快照层，并付出重复建树、容量扫描及命令适配成本。是否接受必须与真实产品工作集比较，而非用单个 demo 判胜负。[M1]

下列 probe 足以推翻过强支持或反对，**全部待执行**，不得写成已有 PASS：

1. **静态大编辑器与 root 放大（S4/S8）：** 取真实 Inspector 结构，分别跑冷帧、热静态、单叶文字更新、祖先 resize、重排；固定活跃 N，改变容量 C；另将普通节点改为 32/128/512 个 floating roots。记录声明、测量、布局、ID 桶扫描、命令复制分别耗时与访问次数。若实际工作集端到端已满足预算，不能单凭全树重算淘汰；若 R² 项主导则否定无条件线性宣传。
2. **共享列与标签换行（S2/S3）：** 两组 Inspector 行中放短标签和本地化长标签，展开隐藏行；窗口逐步缩窄。比较“外部轨道预计算+Clay”与目标共享列/标签换行规则，记录新增遍历和反复测量。只有固定列数 demo 正确不算通过。
3. **真实文字与 cache（S6）：** 使用真实 DirectWrite 字体，输入中文无空格段、阿拉伯文、长 URL、显式换行和混合字号；宽度收窄后比对行边界、高度及绘制基线。保持 fontId 不变替换字体，先不 reset 再 reset；另改同地址动态文字，比较 static 标志真假。记录热帧空格测量调用，并逐项核对 WORDS/NEWLINES/NONE，而非只看枚举名。
4. **虚拟化与身份（S5/S7）：** 10 万逻辑行、约 100 可见行，分别用全量声明+culling、固定行高窗口、可变行高窗口；行首插入、排序、折叠后验证滚动锚及 hover 对象。分别使用位置 ID 与业务 ID，禁止把“显示一样”当作身份正确。
5. **多 context 与快照（S9）：** A/B 两 arena 顺序切换，先用不同测量函数再用统一函数+独立 userData，对比测量路由。保存 A 的命令后再次 Begin A，验证浅拷贝不能作为快照；对退出动画移除其文字源，检查生命周期合同。禁止用未定义的并发竞态作为唯一淘汰证据。
6. **独立容量与失败（S7/S9）：** 声明 101 个 clip、201 个 transition，再测少节点大量 wrapped lines；增加 maxElementCount 后确认独立限额是否仍触发。注入缺失字体、NaN 测量、缺失 floating anchor、深链及宏块提前退出；错误时不得发布带新版本的半成品快照，旧拥有式快照必须仍可读。

真实路径应从项目现有 `docs/modules/ui-layout/acceptance.md` 记录的 **Windows/C++、正式 allocator、DirectWrite 真实字体** 路径重新制作独立 Clay consumer，不链接无关 Runtime/JIT，不改冻结的旧 Yoga probe。旧数值、窄合同与 PASS 只可作历史参照；新的断行、身份和失败合同应按本轮场景另定。本轮未运行这些路径。

## 8. 未知项与证据限制

- **〔待验证〕** M 的容量、退出树复制、wrap 枚举行为与错误恢复在目标编译器上的实际表现；本报告不因静态阅读把潜在问题宣布为已复现漏洞。
- **〔待验证〕** 业务是否真正需要并行求解、共享轨道由核心还是上层提供，以及多数帧的真实 N/C/文本字节数；这些决定适用性。
- **〔待验证〕** R 没有完成与 M 相同深度的机制审计，尤其不能沿用 M 的 transition、100/200 限额或全局实现结论。若短名单选择正式 release，需在 R 独立重跑全部 probe。
- **〔事实〕** M 的 README 有残留旧签名/示例，且指定 floating 父的声明顺序说明与即时源码查找存在张力；probe 实作以固定头文件为准，首帧“先 popup 后 anchor”应单列验证。[M1][M2]
- **〔待验证〕** Exa 返回的 main SHA 是本次可追溯快照；不能把它包装成未来仍不变化的最新版本。

## 9. 直接主源

- [M0] main commit 元数据：https://api.github.com/repos/nicbarker/clay/commits/main
- [M1] **M 固定源码**：https://github.com/nicbarker/clay/blob/e6cc36941ab2af5d81107617039d6f527a1c660b/clay.h
- [M2] **M 固定 README**：https://github.com/nicbarker/clay/blob/e6cc36941ab2af5d81107617039d6f527a1c660b/README.md
- [M3] **M 固定许可**：https://github.com/nicbarker/clay/blob/e6cc36941ab2af5d81107617039d6f527a1c660b/LICENSE.md
- [M4] **M 根 CMake**：https://github.com/nicbarker/clay/blob/e6cc36941ab2af5d81107617039d6f527a1c660b/CMakeLists.txt
- [M5] **M C++ 示例构建**：https://github.com/nicbarker/clay/blob/e6cc36941ab2af5d81107617039d6f527a1c660b/examples/cpp-project-example/CMakeLists.txt
- [R1] 正式发布与时间：https://github.com/nicbarker/clay/releases/tag/v0.14 ；核对 API：https://api.github.com/repos/nicbarker/clay/releases/latest
- [R2] tag → commit：https://api.github.com/repos/nicbarker/clay/git/ref/tags/v0.14
- [R3] **R 固定源码**：https://github.com/nicbarker/clay/blob/b25a31c1a152915cd7dd6796e6592273e5a10aac/clay.h

以上关键主源已获取正文；源码正文通过对应 `raw.githubusercontent.com` 固定 SHA 地址获取。未用搜索摘要代替机制审计。