# RHI 决策记录

2026-09-25；Main已选入窄离屏RHI。具体C++名称可调整，行为以[contract](contract.md)为准；此处记录选择理由与重开条件，而非生产实现已完成。

## 已选实施基线

| 决策 | 理由与证据 | 重开条件 |
| --- | --- | --- |
| Windows x64、现有SDK原生D3D12、单DIRECT queue | 符合当前产品与工具链，Main真实硬件debug路径和最小独立链接通过 | 目标设备或近期工具功能使D3D11更合适时重审；不暗建fallback |
| 完整资源提交子集，不称完整RHI覆盖 | Clear/copy/readback不需shader/window，仍能覆盖GPU使用期 | 产品必须Present/draw时单独批准增量，不扩大现有PASS含义 |
| 一个owner thread、最多一个未提交Recording | 资源状态、取消和提交次序直接可验证 | 确有并行录制/多queue负载后再设计完成关系 |
| Device拥有在飞记录；Ticket不拥有资源 | 丢查询凭证不撤GPU持有，首引用Recording持有覆盖未提交期间 | 内部容器可变，但可观察所有权不变 |
| 关停显式有界；timeout保留Device | 析构无限wait拖垮宿主/GC，timeout不是完成证明 | 如需故障转交，必须引入明确新owner，不能永久孤儿泄漏 |
| Device lost与正常完成分开 | fence最大值表示设备丢失，不能用简单大于比较判成功 | runtime/后端更换时重新核对 |
| presentation为独立Adapter | 离屏不需要HWND；swapchain与窗口生命另有协议 | 窗口增量入选时定义resize/close/surface世代 |
| C++核心独立、未来绑定共享Runtime | GPU资源不是业务对象，当前语言子集未覆盖一般RHI | 实际绑定需求出现后独立实现，不倒置依赖 |

## 未同时建设的真实备选

**D3D11**：传统工具UI/首个绘制可能更小。基础immediate context、staging Map/query可正确回读；较新Device5/Context4有fence接口，不能说它“没有fence”。本轮选D3D12为显式提交/退休闭环，不是性能或通用优越性结论。若改选，仅保一个后端、重验相同契约。

**Vulkan**：核心device/queue与WSI分离，offscreen不需surface/swapchain；Windows-only目标下新增headers/loader/ICD/validation的固定和验证成本，暂无相应产品收益。没有本机Vulkan可用性证据，不默认timeline semaphore/扩展支持。

**NVRHI**：固定提交`d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`有真实资源追踪、barrier与延后销毁；应用仍建native device/queue。其DX12 CMake要求DirectX-Headers/Guids，默认`v1.717.0-preview`；Vulkan-Headers默认`v1.4.352`。不是现有SDK零新增依赖；本轮不接入、不升级。以后若受控pin可显著减少维护可重审，不据此否定所有历史提交。[来源](research.md#成熟实现与依赖)

## 支线对抗的实际修正

- WaveModuleMap接受“RHI资源提交首里程碑”的命名，补上未提交Recording持有、取消、丢Ticket不丢在飞持有；本席接受其反问：SDK probe不验证未来wrapper。
- WaveUi接受双DPI/离屏反例：布局输出未舍入逻辑值，composition按target/DPI/policy派生像素视图。Viewport FLOAT与scissor整数各遵原生规则，RHI不round child.width或保持Node/业务root。
- 两模块不互为前置；文字shaping、glyph/clip/hit-test留到composition/text实际需求。

完整争议、证据和证伪条件见[会议报告](../../research/wave1-meeting/parallel-modules/rhi.md#13-已完成支线质询与修正)。

## 实现者可决定但不阻塞的细节

C++类型/方法拼写、局部错误表示、资源表/空槽管理和批次容器可选最直接方案；无跨语言ABI冻结。首版关停采用contract中明确生命周期与违规销毁诊断，不引入通用孤儿回收框架。内部若选共享control也必须维持相同所有权和有界关停契约。

正式目录/target以handoff为建议，Main完成CMake集成后成为权威。硬件支持列表、最低驱动、性能预算尚无全面证据；第二后端、suballocation、并行、自动设备恢复和shader路线均另需需求与验收授权。

## 集成与版本权威

准确工具链由现有toolchain-lock.json唯一维护；系统graphics runtime/OS/驱动另记录实测。新增源码依赖只由Main更新dependencies-lock.json：固定版本/commit、下载URL、实际归档SHA-256、构建选项、license/notice与patch；禁止虚构hash或moving main。当前SDK路线不增加第三方源码依赖。

Module生产target沿atelier_target接入默认native分配；GPU内存仍由D3D管理。升级或换后端后重跑同一Interface行为矩阵，不能以编译/CreateDevice成功代替。

