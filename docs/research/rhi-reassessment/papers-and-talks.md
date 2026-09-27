# RHI 相关学术与工业资料：机制证据、分层边界与可证伪实验

## 1. 结论摘要

本报告不推荐具体后端，也不以旧 RHI 或旧交接文件为设计权威。

- **已取得可分析原文的核心研究资料只有三项**：Slang 同行评审论文、Whippletree 同行评审论文、NVIDIA 的 OpenGL/Vulkan 工程会议讲稿。另保留 Frostbite FrameGraph 正式会议条目，以及 Filament 官方 FrameGraph 工程说明作为补充；不把它们包装成另外两篇论文。
- **绑定契约的直接研究证据最充分**：Slang 讨论了 shader 代码模块化、参数块布局与特化之间的关系。可以迁移的问题是“RHI 如何接收并验证编译产物的资源布局、如何支持参数块复用”，不是“把 Slang 编译器当作 RHI”。[S1，§2.6、§3.5、§5–7](https://doi.org/10.1145/3197517.3201380)
- **必须区分三种工作调度**：CPU 命令录制与提交、帧内 pass/resource 调度、GPU kernel 内细粒度任务调度。Whippletree 主要研究第三种；FrameGraph 讨论第二种；NVIDIA 讲稿涉及第一种。它们不能互相替代。[S2，§3–5](https://schmalstieg.github.io/pdf/Schmalstieg_286.pdf)；[S3，Vulkan 后半部分](https://developer.download.nvidia.com/gameworks/events/GDC2016/mschott_lbishop_gl_vulkan.pdf)；[S4，会议简介](https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in)
- **本轮没有取得足以证明“通用瞬态堆别名算法应内建于低层 RHI”的论文证据**。Filament 官方说明支持的是图层声明依赖、分析资源生命周期、推导使用及 load/store 属性；这与堆子分配、跨资源物理内存别名、GPU 完成后的安全回收不是同一件事。[S5，What does it do? / Additional details](https://google.github.io/filament/notes/framegraph.html)
- **设计推断**：先明确资源有效期、绑定布局、录制隔离、提交完成与外部资源交接等低层契约，再根据真实消费者决定是否加入图编译、异步计算或 GPU 任务调度。该推断不是作者对 FrostFoxAtelier 的结论，也不能据此判定自研或复用路线胜出。

## 2. 来源快照与证据等级

外部检索及原文取得使用 Exa。作者 PDF、DOI 返回正文、会议正式条目与官方工程文档分开记录；未核验任何相关实现的提交 SHA，未运行示例、GPU、构建或测试，未写文件。

### 可引用书目表

| 编号 | 书目信息 | 类型与证据强度 | 本轮原文状态及定位 |
|---|---|---|---|
| S1 | **Slang: Language Mechanisms for Extensible Real-Time Shading Systems**；Yong He、Theresa Foley、Kayvon Fatahalian；2018；*ACM Transactions on Graphics* 37(4)，Article 141；SIGGRAPH 2018；DOI **10.1145/3197517.3201380** | 同行评审论文；可支持其机制设计与报告中的实验条件，不代表当前编译器全部行为 | 已取得 DOI 返回的论文正文并核对关键章节。[DOI](https://doi.org/10.1145/3197517.3201380)；[CMU 作者项目页](http://graphics.cs.cmu.edu/projects/slang/)。项目页与 DOI 索引的作者列序不一致，正文页眉为 He, Foley, and Fatahalian；引用时不采用检索包装中的引用量或作者指标。 |
| S2 | **Whippletree: Task-based Scheduling of Dynamic Workloads on the GPU**；Markus Steinberger、Michael Kenzel、Pedro Boechat、Bernhard Kerbl、Mark Dokter、Dieter Schmalstieg；2014；*ACM Transactions on Graphics* 33(6)，Article 228，1–11；SIGGRAPH Asia 2014；DOI **10.1145/2661229.2661250** | 同行评审论文；CUDA 特定环境中的任务调度研究，不是通用图形 API 规范 | 已取得作者托管 PDF 并核对 §3–7。[作者 PDF](https://schmalstieg.github.io/pdf/Schmalstieg_286.pdf)；[ACM 书目](https://dl.acm.org/doi/10.1145/2661229.2661250)。PDF 未提供本轮可固定的实现提交。 |
| S3 | **High-performance, Low-Overhead Rendering with OpenGL and Vulkan**；NVIDIA；GDC 2016。正文可核实 Lars Bishop 和合讲者 Mathias；官方文件名为 `mschott_lbishop_gl_vulkan.pdf`，Mathias 的完整姓氏仍需题名页复核 | 工业工程演讲，不是同行评审论文，也不是当代 Vulkan 规范；未发现 DOI | 已取得官方 PDF 的演讲文字。[官方 PDF](https://developer.download.nvidia.com/gameworks/events/GDC2016/mschott_lbishop_gl_vulkan.pdf)。以正文开场标题及 Vulkan 相关段落定位；文本提取未保留可靠幻灯片编号，不能编造页码。 |
| S4 | **FrameGraph: Extensible Rendering Architecture in Frostbite**；Yuriy O'Donnell；Frostbite / Electronic Arts；**GDC 2017**，年份由主代理补充核对的[作者 Publications 页面](http://kayru.org/publications/)明确标示 | 工业工程演讲；不能因常被称为“FrameGraph paper”而升级为同行评审论文 | 已取得会议条目、简介与作者书目，未取得可核验的完整官方 slides/video。[条目一](https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in)；[条目二](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)。具体 allocator/同步算法与实验条件保留待核。 |
| S5 | **FrameGraph**；Filament 官方工程说明；页面未署具体个人作者、发表年份或版本 | 工程文档补充，**不是论文、预印本，也不是已核验的正式技术报告** | 已取得页面全文。[官方说明](https://google.github.io/filament/notes/framegraph.html)。滚动来源，不能将访问到的文字绑定到未经本代理核查的发布标签或提交。 |

**证据强度说明**：同行评审不自动意味着跨硬件、跨 API 或生产适用；会议演讲可以提供真实工程问题与机制，但不能替代实验或规范。技术文档可以说明该项目声称承担的职责，但本报告未据此声称实际源码逐项验证。没有把预印本冒充同行评审来源。

## 3. 逐项分析

### S1 — Slang：资源绑定布局是编译器与运行时之间的契约

**事实：问题与机制。** 论文指出，如果完整入口特化后才能确定全局 shader 参数布局，同一光照环境在不同材质变体下可能无法复用同一参数块。其机制包括显式 `ParameterBlock<T>`、带接口约束的泛型、关联类型，以及查询类型布局和特化入口的编译器 API。`ParameterBlock<T>` 的底层表示可以随目标平台变化；论文不要求统一成固定字节布局。运行时 API 名称也不意味着必须在每帧运行：作者明确允许离线资产处理、加载期或按需调用。[S1，§2.6、§3.5、§5](https://doi.org/10.1145/3197517.3201380)

**事实：实验条件。** 作者基于 **Falcor 2.0.2** 重构材质和光照系统，对比原始 HLSL 分支、增加光照特化的 HLSL 分支、Slang 重构分支；场景为 ORCA 的 temple、bistro-interior、bistro-exterior；分辨率 **1920×1080**，处理器 **Intel i7-5820K**，GPU **NVIDIA Titan V**。CPU 指标是每帧生成 GPU 命令的时间，GPU 指标是执行这些命令的时间。[S1，§6、§7.1、Fig. 4](https://doi.org/10.1145/3197517.3201380)

**限制。** 这不是“仅换编译器”的消融实验：参数块复用、变体查找、材质和光照组织一并改变。论文还报告了当时 Slang 输出 HLSL 后再调用 HLSL 编译器的额外编译开销。§8 明确说明评估在光栅化、前向渲染背景中进行；不能把后续 ray tracing 展示当成相同强度的系统对比，更不能将 2018 年实现描述为当前 Slang 行为。[S1，§6–8](https://doi.org/10.1145/3197517.3201380)

**设计推断／可迁移结论。** RHI 需要清晰区分 shader/管线标识与资源绑定布局标识，允许上层按更新频率构建并复用参数集合；布局改变时必须可识别不兼容旧绑定。材质组合、变体策略、shader 热重载策略应由着色系统决定，低层负责接收、验证和执行有效绑定。

**匹配条件。** 如果项目将包含多材质变体、自有语言与原生之间的共享类型或工具热重载，布局反射与版本化是直接问题；如果首个消费者仅有少数固定 shader，手工生成并验证固定布局也可能足够。论文不能证明此时引入完整泛型着色系统更划算。

### S2 — Whippletree：GPU 内任务模型不能冒充 RHI 提交模型

**事实：问题与机制。** 论文针对动态、不规则 GPU 工作，将任务按可使用的执行层级能力区分；参考实现使用 CUDA C++ 模板库、持续运行的 worker blocks、按 procedure 划分的队列、共享内存与全局内存两级排队。不同任务可以动态生成后续任务。作者同时提供 time-sliced kernels、hybrid dynamic parallelism 和 Whippletree megakernel 等调度实现，以比较同一程序模型的不同映射。[S2，§3、§4](https://schmalstieg.github.io/pdf/Schmalstieg_286.pdf)

**事实：同步与限制。** 某些任务内部同步使用 PTX named barriers；共享内存需求和寄存器压力会限制融合 kernel 的 occupancy。其队列、工作项和 GPU 内同步不是 Vulkan queue、D3D command queue 或 CPU frame graph 的同义词。论文也展示了某些场景下替代调度器更有竞争力，不能把摘要中的积极总结当成所有工作负载上的支配关系。[S2，§4.1、§4.4、§5、§6.2、§7](https://schmalstieg.github.io/pdf/Schmalstieg_286.pdf)

**事实：实验条件。** 测试环境为 **Windows 8、Intel i7 3.4GHz、CUDA 5.5、NVIDIA GTX 580／680／TITAN**。包括递归、树展开、变化的多阶段流水线等合成工作负载，以及 Reyes、过程几何生成、带 irradiance cache 的体渲染示例。此处引用这些应用是为了研究其任务组织，不是将画质算法作为 RHI 架构依据。[S2，§5–6](https://schmalstieg.github.io/pdf/Schmalstieg_286.pdf)

**设计推断／可迁移结论。** RHI 不应把所有 GPU 工作都假设成 CPU 已知、固定 draw 列表；但是否需要 indirect dispatch、GPU 生成任务或特殊扩展，必须由真实消费者决定。任务优先级、局部队列和 megakernel 融合属于算法／GPU 执行层，不应无条件加入通用 RHI。

**匹配条件。** 若项目确有递归过程生成、不规则 compute 或 GPU 驱动工作，可将其作为反例与实验设计参考。对主要是常规 draw 的小型 Windows 引擎，本文不足以支持引入 CUDA 依赖、持续 kernel 调度器或厂商特定同步原语。论文的旧式 warp 执行假设也不能未经当代规范核查直接重用。

### S3 — NVIDIA GDC 2016：显式 API 改变的是责任边界

**事实：问题与机制。** 演讲比较传统 OpenGL、扩展命令列表和 Vulkan，讨论把管线配置、资源布局、内存分配、命令录制及提交责任显式交给应用。Vulkan 部分提出按线程组织 command pools、在提交完成后复用 command buffers、按生命周期组织 descriptor pools，以及批量提交线程录制的工作。它也讨论多个资源使用同一分配、buffer 加 offset 的组织方式。[S3，Vulkan objects、resource management、threading、reduce by reuse 段落](https://developer.download.nvidia.com/gameworks/events/GDC2016/mschott_lbishop_gl_vulkan.pdf)

**事实：演示条件。** 讲稿提到 NVIDIA Fish demo，展示资源更新与命令录制的多线程路径；本轮取得文本未提供足以复现、覆盖所有建议的统一硬件、驱动、场景和测量协议。因此这是工程机制证据，不是该项目的性能基准。[S3，Fish demo 与结尾 trade-offs 段落](https://developer.download.nvidia.com/gameworks/events/GDC2016/mschott_lbishop_gl_vulkan.pdf)

**限制。** 这是 Vulkan 初期资料；有关 event、semaphore、command buffer 继承和管线状态的概括不能代替后续目标版本的规范。官方 `gdc-2016` 活动入口本次返回了另一年份活动内容，故不将那个滚动入口用作 2016 书目依据；采用年份固定在路径中的原始 PDF。未声称 PDF 中所有同步表述适用于当前 API。

**设计推断／可迁移结论。** RHI 应说明哪些对象可以独立录制、谁串行化提交、哪个完成凭证允许回收，而不是仅给 `Draw` 等命令更换名字。允许多线程录制与要求多个 GPU 队列应分开建模；后者不是前者的前提。

**匹配条件。** 当实际 CPU 工作分配需要并行录制或频繁资源上传时，这些责任直接相关。若尚无真实 workload，不能据此预设线程数、每帧 command list 数或多队列拓扑。

### S4 — Frostbite FrameGraph：确认是工程演讲，原文不足以承载细节结论

**已核实事实。** GDC 正式简介描述了一种以所有 render passes 与 resources 的图为基础的渲染架构，目的包括渲染功能的解耦和模块化。[S4，Overview](https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in)

**未核实。** 未取得完整官方演讲原文，不能在本报告中断言该实现具体采用何种瞬态别名分配器、如何做异步 compute 调度、如何生成 barriers、支持何种队列模型，或获得多少内存／性能收益。演讲年份已由主代理通过 Exa 补充核对的[作者 Publications 页面](http://kayru.org/publications/)确认为 GDC 2017；该书目信息不补足全文缺口。检索中发现的第三方 FrameGraph 实现或未验证上传者的镜像不作为补全官方演讲细节的替代物。

**设计推断。** 该条目足以证明“图组织渲染功能”是一项工业架构实践线索，不足以证明“这是低层 RHI 必须遵循的论文建议”。对小型引擎是否适用，只能取决于实际 pass 依赖、资源生命周期和调试需求。

### S5 — Filament 工程说明：用来校准 graph 与 RHI 的边界

**事实：问题与机制。** 页面区分 resource 与 pass 节点、读写依赖及父子资源关系；`addPass` 的 setup lambda 立即同步执行，执行 lambda 在图构建完成后执行。文档列举图层职责：资源生命周期、纹理 usage 属性以及 render target 的 load/store 属性，并解释跨帧导入／导出资源的复杂性。[S5，Dependency Graph、Example code、What does it do?、Additional details](https://google.github.io/filament/notes/framegraph.html)

**条件与限制。** 这是带生产代码片段的工程说明，不是带受控实验的论文；没有可引用的性能测量或显存收益。本页关于 DAG 可得到依赖顺序的描述，不证明实际实现执行通用拓扑重排，更不证明 GPU 并行调度。页面将 CPU-only passes 的多线程与重排写在 Future Work 中；该文字本身也不能代表未经源码核查的当前实现状态。

**设计推断／可迁移结论。** 可以将声明依赖、裁剪无用工作、推导资源需求作为图层候选职责；低层 RHI 则保留对实际资源、命令和完成条件的明确表达。若项目只有少量固定 passes，这些需求可以先通过显式代码表达，而不必立即实现通用图框架。

## 4. 共同机制矩阵

表中的“未覆盖”表示本轮来源未给出可依赖的结论，不表示机制一定不存在。

| 契约维度 | S1 Slang | S2 Whippletree | S3 显式 API 演讲 | S4／S5 图组织资料 | 对项目的设计推断或未知项 |
|---|---|---|---|---|---|
| 资源所有权与寿命 | 类型布局、参数块分配与复用；不是完整 GPU 资源所有权模型 | GPU 工作项、队列、shared/global memory；不是图形资源所有权模型 | 应用负责资源寿命、分配组织与完成后复用 | S5 分析图内资源寿命，并讨论导入／导出；S4 仅简介 | 区分 CPU wrapper 生命周期、GPU 使用完成、图中最后一次逻辑使用；尚未选定句柄或引用计数策略 |
| 同步／状态 | 未解决 GPU barrier 或队列同步 | kernel 内任务和线程协作；部分机制依赖 CUDA/PTX | 说明显式同步及完成后回收责任；规范细节需另核 | S5 描述依赖与 usage 推导，不足以证明完整 barrier 生成或别名同步 | 资源状态、执行依赖、内存可见性与完成凭证需要分别约定；不能压成单一“已用完”布尔值 |
| 绑定／Shader | 直接研究参数块、反射、布局与特化 | CUDA procedure/context 接口，不是图形 descriptor ABI | 讨论 descriptor set layout、绑定与动态 offset | 图负责资源读写声明，不替代 shader ABI | 着色系统产生布局，RHI 消费可验证的布局与绑定；不能推定 CPU 原生类型字节布局等同 GPU 布局 |
| 并发／提交 | 编译／特化时机由引擎决定，不规定 GPU 提交策略 | GPU 内调度，不是 CPU command list 调度 | 独立录制、池隔离、完成后复用、批量提交 | S5 的概念图不证明并行或重排实现 | 首先验证单队列上的并行录制需求；多队列仍是待证需求，而非既定限制或必选能力 |
| 窗口／呈现 | 未覆盖 | 未覆盖 | 已取得讲稿未提供可据以设计完整 swapchain 的契约 | 跨帧导入／导出不等于窗口呈现协议 | resize、backbuffer 获取／交还、最小化、呈现失败仍需后续规范与候选实现证据 |
| 故障／诊断 | 编译期接口检查；并非 GPU device-loss 处理 | 队列保护与部分实验未完成的报告；不等于健壮故障恢复 | 不足以确定完整错误传播／设备丢失设计 | 本轮无完整错误／设备丢失模型 | 不应从正常渲染实验推出设备丢失后可恢复；这是后续独立调研和验收项 |
| 依赖／许可 | 历史实验使用 Slang、Falcor 及 HLSL 编译链；未审计当前依赖许可 | CUDA/PTX 与论文参考实现；“开源”不等于已核定许可证 | 工程讲稿，不直接提供可纳入项目的实现许可 | 会议资料与工程文档不是复用源码的许可证明 | 本报告不做许可结论；采用任何代码前必须核对选定版本 LICENSE、NOTICE 与传递依赖 |

上述事实定位分别见 S1 §3.5／§5–7、S2 §3–5、S3 的 Vulkan 段落及 S5 的职责列表；表中项目建议均为设计推断。

## 5. Render graph、瞬态内存与低层 RHI：不得合并的责任

### 5.1 可以由本轮证据支持的区分

1. **图层的逻辑寿命分析**：根据 passes 对资源的声明，判断某个中间结果何时第一次需要、最后一次使用。S5 明确描述该职责。
2. **分配器的空间子分配**：让多个对象或 buffer 区间来自较大的分配。S3 讨论这种组织；它不自动意味着两个活跃资源能够占同一物理区间。
3. **时间别名与安全回收**：复用一段物理内存需要证明有关 GPU 使用不再重叠，并满足选定 API 的绑定／别名／同步要求。**本轮未取得足够一手原文建立该完整协议**；不能把 S4 的图简介当作证明。

### 5.2 待验证的项目分层假设

- Renderer／着色系统：决定画什么、选择何种材质和变体、确定持久与历史资源。
- 可选图层：声明 passes 与依赖、分析图内寿命、提出暂存资源需求及执行计划。
- 分配服务：实现物理分配、子分配和在有充分条件时的别名策略；其接口位置有待项目验证，不预定必须包含在 RHI 内或外。
- 低层 RHI：表达合法资源与视图、管线和绑定、命令录制、队列提交、同步及完成条件，并向上层提供足够的能力／限制信息。
- 平台呈现层：处理窗口、swapchain 和呈现结果的衔接；本轮资料不足以裁定与 RHI 的具体接口划分。

这是供对抗评估检验的责任划分，不是从某篇论文逐字推出的最终架构。

## 6. 不足、未取得原文与不可迁移结论

- Exa 搜索出现一次明确的额度限制；后续继续使用 Exa fetch 获取已知官方资料，没有把搜索摘要升级为源码核验。
- **Spark: Modular, Composable Shaders for Graphics Hardware**，Tim Foley、Pat Hanrahan，2011，*ACM TOG* 30(4)，DOI [10.1145/2010324.1965002](https://dl.acm.org/doi/10.1145/2010324.1965002)：核实了[Stanford 作者页](https://graphics.stanford.edu/papers/spark/)与摘要；出版社 PDF fetch 超时，未取得实验全文，因此不纳入核心机制或性能证据。
- **Shader Components: Modular and High Performance Shader Development**，Yong He、Tim Foley、Teguh Hofstee、Haomin Long、Kayvon Fatahalian，2017，*ACM TOG* 36(4)，Article 100，DOI [10.1145/3072959.3073648](https://doi.org/10.1145/3072959.3073648)：DOI 返回摘要，出版社 PDF fetch 超时。本次机制分析以已经取得全文的 Slang 论文为准，不把前作重复计为完整核验来源。作者名采用 Slang 正文参考文献中的书目信息；检索索引存在姓名更新差异。
- **The Direct3D 10 System** 与 **An Overview of Next-Generation Graphics APIs** 仅取得摘要／书目级内容，未纳入架构结论，避免为覆盖“显式 API 抽象”凑来源数。
- Frostbite FrameGraph 正式会议简介可取，EA 新闻原文 fetch 返回 `CRAWL_NOT_FOUND`，完整 slides 未取得。未核实演讲中的瞬态内存、异步计算、别名安全或性能数字。
- 未获得现代跨 API GPU 内存模型的专门论文全文、跨队列 ownership/state 的完整对照、设备丢失恢复的研究证据或本项目工作负载数据。不能据此声称这些领域“没有研究”；只能说本轮证据不足。
- **没有将作者的性能百分比、速度倍率或显存收益迁移到本项目**。本文保留的硬件、版本与分辨率仅用于界定原实验适用域，不构成本项目最低需求。
- 未审计任何当前实现的许可证、传递依赖、编译器二进制或兼容硬件范围；所有“可接入”“可移植”“生产可用”判断仍待路径验证。

## 7. 三个可推翻上述推断的真实路径实验

以下均为下一阶段建议，**本轮未执行**。先由主流程确定实际消费者与批准的候选，再固定源码版本、编译器、驱动和 GPU；不为实验反向预定生产后端。

### 实验 A：参数块复用是否真的脱离 shader 变体

**待证假设。** 同一材质／光照参数块能够在不同有效 shader 变体之间复用，且布局变化能够被显式识别。

**真实路径。** 用批准的 shader 编译链建立包含数值字段、纹理、sampler 与数组的参数类型；构造不同材质／光照特化以及会裁掉未使用字段的变体。经过真实布局反射、RHI 绑定、GPU 读取与 readback 验证字段值；加入一次改变布局的热重载，在旧提交仍在飞行时切换到新版本。无需先建完整材质编辑器。

**记录。** 布局标识及 offset、编译产物、绑定更新次数、失效原因、旧资源释放依据、验证层输出；若比较成本，分离首次编译、缓存命中、CPU 参数准备和 GPU 执行。

**反证。** 入口特化令拟复用布局不兼容且无法检测，或热重载后旧绑定被提前回收，则“参数块可安全独立复用”的契约不成立，需要调整布局粒度／版本化／寿命方案。若真实小型消费者使用固定布局更简单且没有可观察损失，则 Slang 式完整系统的引入理由减弱。依据：S1 §2.6、§5–7。

### 实验 B：逻辑最后使用是否足以支持瞬态内存复用

**待证假设。** 图层能提供足够信息，而不需要把整个 renderer 固化到低层 RHI，来实现正确的资源回收及可选别名。

**真实路径。** 建立真实 upload → compute 写入临时资源 → graphics 采样 → readback／呈现链，并加入独立临时资源及跨帧历史资源。对比禁止别名与批准的别名分配路径；只在目标 API 明确允许时启用后者。保留多个在飞行帧并改变 CPU 录制完成顺序；如候选确实支持且需求成立，再加跨队列版本。通过可预测像素／buffer 内容校验，而非仅看程序没有崩溃。

**记录。** 每项资源的图内使用区间、真实提交及完成值、物理分配区间、barrier／ownership 记录、导入导出状态、峰值实际分配、验证错误与输出差异。

**反证。** CPU 最后一次录制后便回收导致内容损坏，或者图层无法表达历史／外部资源交接，说明逻辑寿命信息不足；必须增加明确的执行完成／资源交接契约。若别名节约不足以抵消复杂性，或候选能力限制使其不安全，就推翻“初期必须做通用瞬态别名”的假设，而不隐蔽禁用验证。依据：S3 的完成后复用、S5 的寿命与导入导出职责；完整别名协议须另行核对目标规范。

### 实验 C：并行录制与 GPU 内调度分别解决了什么瓶颈

**待证假设。** 低层先提供独立录制与批量提交即可满足首个消费者，不必预设多 GPU 队列或 GPU 内任务调度器。

**真实路径。** 对同一确定性画面使用单线程录制／单队列作为基线，再使用多线程独立录制、同一队列批量提交；校验输出一致性和回收正确性。只有当真实消费者存在不规则 GPU 工作时，再比较分阶段 compute dispatch 与其所需的 GPU 驱动任务路径；不为了复现 Whippletree 引入 CUDA 或搭建无关调度框架。

**记录。** CPU 录制时间、提交次数及时间、分配和同步等待、GPU 时间线、队列／kernel 工作量、输出校验、峰值内存。使用实际 API 支持的完成证据，不能把多个提交对象误认为并发执行。

**反证。** 多线程录制没有改善实际瓶颈却增加分配和锁成本，说明默认并行化不适合该消费者；若不规则任务必须依赖当前抽象无法表达的间接参数或能力，说明候选低层契约过窄。反之，传统分阶段路径足够则削弱 GPU 内通用调度器的必要性。依据：S2 §4–6；S3 的 threading 与 trade-offs 段落。

## 8. 交付边界

本轮形成可追踪的研究事实、明确标注的设计推断与未覆盖清单；没有选定后端，没有修改生产契约，没有运行性能实验或声称集成成功。后续对抗评估应优先挑战参数布局／寿命边界与图层责任，而不是依据论文数量或作者报告中的加速数字评选赢家。
