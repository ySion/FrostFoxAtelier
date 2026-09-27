# RHI 重新评估：首轮调研与对抗结论

状态：**搜索调研与独立对抗评估已完成；路径验证和生产落地尚未开始。** 本文供讨论，不是选型批准或生产能力声明。记录日期：2026-09-25。

旧 RHI 是临时占位，旧接口、后端、单线程/单队列与离屏范围不构成设计权威。本轮全部仓库写入位于独立 worktree `E:/FrostFoxAtelier-rhi` 的本研究目录，未改生产源码、构建规则或依赖锁。

## 后续讨论增量：专用渲染设施、RDG 与 Mesh Shader

以下覆盖首轮报告中相应的未决前提，不代表实现或 GPU 验收已完成：

- **已确认**：首个消费者为游戏引擎 3D 视口；本 session 全权负责渲染部分。UI 布局由另一 session 完成，之后归并。
- **已同意的方向**：持久化 RenderScene、独立 RenderView、当次执行数据；具体 Interface 和类型尚未冻结。
- **新增需求**：支持 Mesh Shader，采用前沿技术、激进推进；RHI 为本引擎专供，不追求跨引擎的通用最低公共能力集合。
- **新增提议**：以 RDG（Render Dependency Graph）组织渲染工作。它不替代场景/View，而是将 pass 与资源访问编译为执行计划；图与 RHI 不应分别猜测同一份依赖/状态。
- **硬件与管线已确认**：Mesh Shader 是最低要求，不建设旧 GPU 场景回退；Vertex、Mesh、Compute 都是正式能力，不把 VS/PS 当兼容补丁。首后端、其他配套能力的确切范围及故障终态仍待讨论。

主代理已通过 Exa 补充核对：Microsoft [Mesh Shader 规范](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html) 描述 mesh pipeline、可选 amplification 阶段、DispatchMesh、间接 dispatch 及 OPTIONS7/MeshShaderTier 查询；[Shader Model 6.5 规范](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html) 定义 ms_6_5/as_6_5。需要真实 feature query、编译和 dispatch 验证，不按设备型号或既有清屏探针推定支持。

Epic [UE 5.6 RDG 文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine?application_version=5.6) 返回正文说明依赖声明、pass 裁剪、瞬态资源寿命/别名、异步 compute fence 和并行录制。这是可参考机制，不授权直接复制 UE 的宏、线程限制或类型体系；抓取标题仍带 403 包装，本轮不宣称实时匿名访问或 UE 源码已核验。

当前建议的验证方向是“GPU 场景数据 → 可见性工作 → 间接 Mesh Shader 绘制 → 颜色/深度输出与后续 pass → 视口输出”，由 RDG 管理声明的资源依赖，经专用 RHI 执行。队列拓扑、绑定布局和几何预处理需下一轮定向调研/对抗及真实实验，不把该建议写成已批准的完整生产规范。

### 程序化几何与 VS/PS 路径

用户新增类似 Houdini 的程序化模型生成功能，并询问保留普通 VS/PS 的必要性。当前设计建议：传统顶点路径与 Mesh Shader 路径都作为同一专用 RHI 的正式图形能力，共用 RDG、资源和生命周期机制；不因设备缺少 Mesh Shader 而切换成兼容渲染器。

Microsoft Mesh Shader 规范说明 MS/可选 AS 替代几何处理阶段，而不是替代 PS；MS 从资源读取数据并输出有限批次顶点/图元，不要求输入只能是预先烘焙的静态 meshlet。AMD [From vertex shader to mesh shader](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-from_vertex_shader_to_mesh_shader/) 与 [Procedural grass rendering](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/) 提供程序化几何与 shader 内生成草叶、动态风形变的具体机制，均经 Exa 取得。不能由此推定所有程序化算子用 MS 更快。

分析应分开：固定拓扑形变可复用几何分组，但剔除包围体等元数据仍需更新/保守处理；任意拓扑变化可能需要重新分组与更新相关缓存；规则参数化几何可直接在 MS 中生成。若生成结果本来就是顶点/索引缓冲，普通 VS/PS 是自然的正式消费路径；若需要碰撞、导出或其他持久化消费者，生成系统还需发布可复用的几何数据，不能只把光栅化临时输出当完整模型。

提议将“程序化生成/增量计算图”与“本次 GPU 工作的 RDG”分开：前者产生/更新几何版本，后者调度必要的生成、上传、读取与绘制工作。路径选择比较生成、分组、上传、同步与绘制的总成本，未测量前不宣称 VS 或 MS 普遍更快。

### 当前推进范围与 gfx 增量

用户进一步明确近期只聚焦 RHI 本体；大多数游戏场景物体无需形变，隔壁语言未来会服务程序化生成，但这些不扩展当前实现范围。下一步建议先收敛资源绑定、录制/提交/完成、在飞寿命与 RDG 状态交接契约，再进入三类管线共用资源的真实路径验证。

新增候选 [gfx 源码核对](gfx.md)：固定快照已具备真正的 Mesh Shader、直接/间接 Mesh 分派及原生 interop，并非只能 Vertex/Compute。是否整体复用取决于状态、帧推进、资源退休和错误语义是否匹配；快速原型定位与 AMD Capsaicin 的依赖关系都不能替代这项判断。本轮没有新增构建/GPU/PASS 或性能结论。

## 1. 本轮做了什么

- 五名研究代理分别调查 NVRHI、DiligentCore、bgfx/SDL GPU/wgpu 家族、工业引擎实践、论文与工程资料。
- 两名独立评估者分别挑战“必须自研”的必要性与“成熟库加薄适配足够”的充分性。对抗不是投票；主流程按证据和适用条件合成。
- 主流程补充原生 D3D12 责任基线，复核 SDL command buffer/窗口线程契约，并从作者书目确认 Frostbite FrameGraph 为 GDC 2017 演讲。
- 两路研究的连接中断后已恢复并取得完整报告。Exa 搜索一度受限，已知原文继续通过 Exa fetch 取得。所有纳入结论的 RHI 外部证据来自 Exa 检索/获取；一次误触发的非 Exa 空查询返回无关内容，已弃用，未进入证据集。
- 没有构建、运行 GPU、故障注入、基准测试或新增依赖；没有将旧平台探针结果升级为本轮 Module 验收。

## 2. 最重要的结论

**先决定项目必须拥有的语义，再决定哪些底层机制自己实现。** 统一业务类型不自动要求统一所有 GPU 底层对象；第三方资源引用也不自动形成另一套业务对象世界。反过来，普通路径的自动保活、自动 barrier 或一个 native handle，也不保证任意更新、线程、取消和互操作都安全。[自研对抗 §一至三](adversarial-native.md)；[复用对抗 §一、异议1–3](adversarial-reuse.md)。

本轮已经把原先笼统的“轻不轻、可不可控”缩小成可核查的问题：

1. **资源活着和旧数据有效是两件事。** CPU 引用、录制持有、在飞持有、descriptor 内容、上传数据版本各有期限，不能以一个 handle 或“延迟 N 帧”替代。
2. **自动状态管理不等于调度器。** 资源进入/退出状态、跨命令列表和队列的执行依赖，以及 native 交接，仍要明确归属。
3. **提交、完成、回收和 present 不是同一事件。** 正常渲染、纯离屏、暂停呈现与关停都需能够推进到可解释的终态。
4. **图、着色系统和 GPU 内任务调度不是同一层。** 工业实现和论文不能直接为“首版低层 RHI 必须内建全部机制”背书。
5. **差异是适用条件，不一定是库缺陷。** 只有违反已确认产品需求或真实验证失败，才构成淘汰理由；未取得源码、版本较旧和没有某项文档都不是“不支持”的证明。

事实依据及限制分别见六份证据报告与两份对抗报告，不将这些综合判断描述为本项目实测。

## 3. 条件化候选矩阵：不是总排名

| 路线 | 值得保留的理由 | 最先需要检验的边界 | 当前定位 |
| --- | --- | --- | --- |
| 原生后端自研 | 可直接实现已经确认、候选无法表达的资源/绑定/调度/故障语义 | 必须承担上传、录制保活、同步、退休、绑定和错误出口；小 demo 不是等语义替代 | 保留为必要机制的直接实现或针对性对照，不预先造完整通用 RHI |
| NVRHI | 普通受追踪路径承担录制保活、提交实例退休、上传与可选状态追踪 | descriptor table/native escape 的保护例外、自定义 tracker GC、边界状态、跨后端错误返回 | 接受其低层契约时是强复用候选；平台/Shader/呈现仍有引擎职责 |
| DiligentCore | 可独立采用 Core 的资源、binding、动态页、fence 延迟回收和可选呈现 | deferred 执行前持有、mutable SRB 覆盖、FinishFrame/Flush 区别、实际构建与诊断闭包 | 不应因整个引擎家族体量被排除，也不能按“全自动寿命”采用 |
| bgfx | 接受 view、排序和 frame 汇合时，可以委托相当部分执行组织 | frame/view 时序、上传数据期限、shaderc 资产约定、fatal 策略 | 以是否接受其调度语义判断，不伪装成任意提交器 |
| SDL GPU | 应用级 graphics/compute 工作流、资源 cycling 与可移植接口 | command buffer 同线程获取/使用/提交；窗口线程获取 swapchain；binding schema 与 cycling 后内容 | 接受其线程、资源版本和能力边界时进入验证 |
| wgpu / wgpu-native | 明确验证、安全资源模型与 feature/limit 契约 | Rust 能力不能直接认证 C ABI；映射互斥、回调推进、固定原生扩展/二进制组合 | 保留具体接入路线，不能把整个家族混成一个能力表 |

矩阵来源：[原生](native-baseline.md)、[NVRHI](nvrhi.md) §3–10、[Diligent](diligent.md) §3–9、[应用级库](portable-libraries.md) §2–6。候选不需和原生采用同样的类/调用数量，只需在确认的语义上等价。必须按合法用法比较，不能故意违规释放或覆盖后指控候选失败。

## 4. 对抗后保留与否定的主张

| 主张 | 合成判断 | 后续证据门槛 |
| --- | --- | --- |
| 自有语言/统一类型，所以必须全部自研 RHI | **不成立。** GPU 资源投影和业务类型权威可以分离；真实 FFI/借用冲突另论 | 用真实共享数据/释放路径验证一次投影，而不是复制业务身份 |
| 成熟库有引用计数，所以薄适配天然足够 | **不成立。** 普通 binding、未提交录制、原生 import 与可变 descriptor 的保证不同 | 验证合法持有链、取消点、数据/绑定版本和错误出口 |
| 加少量提交前持有或维护调用，就等于又造一套 RHI | **不成立。** 明确职责的适配可以合理存在 | 判断是否出现两份相互猜测的权威状态/退休机制，而不是数包装类 |
| 自研拥有源码，因此天然更快或更可控 | **没有本项目证据。** 控制收益也可以是必要语义或可诊断性，但必须具体 | 同语义负载和可归因的边界实验；不删除一方安全保证来制造优势 |
| UE/Godot/Filament/论文采用图与并行，所以首版必须如此 | **不能跨层推出。** 三者图所在层次和并发模型本来就不同 | 用真实 pass/资源链证明新增图、线程或队列值得承担 |
| 库不包窗口/Shader工具，或没透明恢复，所以不合格 | **取决于产品要求。** 合法职责划分不等于不足 | 确定需要诊断退出还是恢复，验证实际平台与错误协议 |
| 滚动资料、旧版本或缺原文可以直接降低候选能力评价 | **不成立。** 它们降低证据确定性，不证明当前产品不支持 | 对参与实验的准确版本补齐决定性证据 |

详见 [自研必要性对抗](adversarial-native.md) 与 [复用充分性对抗](adversarial-reuse.md)。评估者是独立只读审查已保存证据，没有重新审计所有上游源码；主流程没有将评审判断包装成额外的运行证据。

## 5. 工业实践和论文实际支持了什么

### 工业引擎

- UE RDG、Godot RDGraph 和 Filament FrameGraph 不是同一抽象：分别以不同方式组织 pass、资源与底层状态责任。
- 公共接口线程安全、CPU 并行录制、后端消费线程、GPU 多队列应分别讨论。工作线程存在不代表接口可任意并发。
- 跨帧 history、图内 last-use 与 GPU 完成回收不应合并。

这些是样本版本的机制参照，**不是可直接抽出的三套稳定 RHI 库**。UE 只核对公开文档；没有声称访问授权受限源码。详见 [工业实践](industrial-engines.md)。

### 研究材料

| 材料 | 已取得的证据 | 对 RHI 有用的边界 |
| --- | --- | --- |
| Slang，2018，同行评审论文 | 论文正文与实验条件 | Shader 特化、参数块布局和绑定之间需要可验证契约；不等于必须采用 Slang，也不替代低层同步 |
| Whippletree，2014，同行评审论文 | 作者全文及 CUDA/硬件条件 | GPU 内细粒度任务调度与 CPU 提交/帧图不同；不能据此给首版添加通用 GPU scheduler |
| NVIDIA OpenGL/Vulkan，GDC 2016 | 官方工程讲稿 | 显式 API 把生命周期、存储、录制与同步责任交给应用；不要求应用亲自从零实现每项机制 |
| Frostbite FrameGraph，GDC 2017 | 正式会议简介与作者书目 | 确认工业图组织线索；未取得可核验完整官方 slides，不借镜像补写 allocator/同步/性能结论 |
| Filament FrameGraph notes | 官方滚动工程说明 | 校准图逻辑寿命、资源使用和 driver 执行职责；不是另外一篇论文 |

详细书目、作者、DOI、历史实验条件、未取得全文的 Spark/Shader Components 等见 [论文与工程资料](papers-and-talks.md)。本轮是两篇全文论文，不用演讲/摘要充数，也不迁移作者性能数字。

## 6. 来源快照与剩余缺口

- NVRHI 核心证据固定为 `d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`；部分补充来源仍是滚动文档。曾发现滚动头文件缓存与固定快照版本不同，报告已分开。
- DiligentCore 研究样本为 `v2.5.6`，不是最新版本声明；Godot `4.4-stable`、Filament `v1.56.0` 也只是本轮机制样本。不能用这些快照直接判定其他版本。
- bgfx/SDL 与部分 wgpu-native 来源尚未形成完整不可变版本闭包；Rust 文档、native manifest、C 头和实际二进制不能相互替代。
- 根许可证和部分依赖已核对，不代表完成最终产品工件的完整许可/notice 审计。
- 重要取消/丢弃、device loss、native interop 与 C ABI 分支仍有证据缺口。它们进入验证门槛，不自动写成不支持。
- **当前没有任何候选在本 worktree 中构建或运行过；没有性能赢家，没有已验证的正式 RHI。**

## 7. 下一阶段建议与讨论入口

建议先选定一个真实消费者，用最匹配的成熟候选检验是否可以清楚委托低层机制。若有两个候选的语义取舍无法靠资料区分，再对照第二个；只有出现具体必需能力冲突或可归因瓶颈，才为相应机制增加小范围原生对照。**不先实现两个完整 RHI，也不为了反对复用而制造产品需求。**

这不是预先决定复用：如果已确认需求本身就是候选无法承接的低层控制，原生路线可直接进入针对性验证。实验必须对双方维持相同正确性、输出与必需行为。

优先讨论三项：

1. **首个真实消费者**：要显示或计算什么，怎样核对结果？不是先选库名。
2. **必须自有的控制权**：是否有不可改变的录制/提交、在途更新、外部互操作或 Shader 资产语义？未确定者仍开放，不沿用旧占位选择。
3. **故障终态**：允许明确诊断并结束渲染会话/进程，还是必须在同进程恢复？这会直接改变候选和验收门槛。

硬件能力、后端时间表及依赖/分发边界随所选消费者补齐。tracker 容器、句柄编码、helper 拆分等实现细节由实施者承担，不提前甩给用户。

[路径验证方案](validation-plan.md) 已给出 V0–V5 的输入、真实实验、记录格式和停止条件；**尚未执行**。按用户允许的分阶段流程，本轮停在证据与对抗结果，等待讨论确认后再开展验证和生产落地。

## 8. 阅读导航

| 文件 | 用途 |
| --- | --- |
| [research-brief.md](research-brief.md) | 用户前提、调查范围、证据和阶段约定 |
| [evaluation-framework.md](evaluation-framework.md) | 统一比较维度与证伪规则 |
| [native-baseline.md](native-baseline.md) | 原生责任与本地集成条件 |
| [nvrhi.md](nvrhi.md) | NVRHI 固定源码与公开契约 |
| [diligent.md](diligent.md) | DiligentCore 生命周期、绑定和目标闭包 |
| [portable-libraries.md](portable-libraries.md) | bgfx、SDL GPU、wgpu/native 的不同模型 |
| [industrial-engines.md](industrial-engines.md) | UE、Godot、Filament 的分层与机制 |
| [papers-and-talks.md](papers-and-talks.md) | 论文、演讲及证据限制 |
| [adversarial-native.md](adversarial-native.md) | 对“必须自研”的独立挑战 |
| [adversarial-reuse.md](adversarial-reuse.md) | 对“薄适配足够”的独立挑战 |
| [validation-plan.md](validation-plan.md) | 下一阶段真实验证路径，未运行 |

研究/评估代理原文中的“未写文件”描述其只读执行范围；这些报告已由主代理统一保存。补充校正以本总览与原报告的明确增补为准，未将新证据伪装成代理原先已经取得。
