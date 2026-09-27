# gfx：三类管线已具备，下一步核对执行契约

状态：源码核对与验证提案，不是生产选型或 GPU 验收结果。当前交付只聚焦 RHI；语言、程序化建模与完整 Renderer 不在本轮实现范围。

## 1. 快照、定位与 AMD 关联

本轮固定 `gboisse/gfx` 提交 `e039b255efb327994d30b187caee400ca60110e0`；[官方提交记录][S0]日期为 2026-09-21。公开接口、实现、构建和许可均按此快照核对，不以旧 README 的遗漏判断当前功能不存在。

AMD Capsaicin 的官方 [.gitmodules][S6] 明确依赖 `gboisse/gfx`，其 [README][S7] 将 gfx 列为 D3D12 抽象库。因此 AMD 关联有直接证据；但不能把 Capsaicin、gfx 及其支持承诺视作同一产品。gfx 自身 [README][S4] 定位快速原型，根 [LICENSE][S5] 为 MIT、版权人为 Guillaume Boissé。定位不等于性能测量，也不是淘汰条件。

源码通过 Exa 抓取并作定向核对；大文件取得至末尾，但清洗文本会丢失部分 C++ 模板标记，不能将其用作可编译源码。定位以固定文件与符号为准，构建须另用可校验的源码快照。

## 2. 会改变判断的源码证据

| 项目 | 已核对事实 | 对本项目的含义 |
| --- | --- | --- |
| 三类管线 | `gfx.h` 有 Graphics、Mesh、Compute kernel 与对应命令；`GfxProgramDesc` 含 AS/MS。`createMeshPipelineState`、`encodeDrawMesh` 和 `encodeDrawMeshIndirect` 具备真实原生路径。[S1] [S2] | Mesh 不是模型加载器里的同名概念，不能以“不支持 MS”排除它。实际硬件运行仍未验证。 |
| 间接 Mesh | 已读实现调用 `ExecuteIndirect`，单次最多一条 Mesh 参数记录、无 count buffer。[S2] | 一条记录也能启动许多工作组，并不否定 GPU-driven；是否需要多记录/count buffer 必须由工作负载决定。 |
| 绑定 | 公开 program 参数按名称设置；反射、root signature、descriptor 管理由库承担。[S1] [S4] | 不因字符串入口就断言热路径慢。需验证参数准备时机、绑定实例隔离和布局变化，不能先假定能直接替换为自有绑定 ABI。 |
| 原生交接 | 可导入 device/resource、替换 command list、获取 native 对象与查询/设置资源状态。[S1] 其中 state setter 会调用 transition 并可能提交 barrier，不是仅修改追踪元数据。[S2] | 有 interop 是积极条件；但还需确定 RDG 与库各自负责哪些 barrier，避免两个追踪者相互覆盖。 |
| 提交与退休 | 自有路径围绕当前 command list、DIRECT queue、帧槽 fence 运行；垃圾回收按帧推进。interop 的 `frame` 路径不执行原生路径的 fence 等待，`finish` 拒绝 interop；`execute` 仅提交，没有 signal/wait/GC，也不返回完成 ticket。[S2] | 帧推进、外部提交与资源保活必须形成明确协议。不能把普通路径的自动保护推广到任意外部队列或长期未提交录制。 |
| 完成与错误 | 已读 `frame`/`sync` 路径有无限等待；Mesh 命令在 root signature/PSO 缺失时存在返回成功但跳过下发的分支。[S2] | 不据此宣布库有 bug；它与我们将要定义的完成/失败契约是否相容，需要实测。API 返回成功不能替代 GPU 输出证据。 |

以上是局部源码语义，不是完整线程安全、设备恢复或所有资源类型审计。并行录制、多队列、bindless 的具体近期需求尚未冻结，不能为淘汰库反向制造硬要求。

## 3. 构建闭包不是两个头文件

固定 [CMakeLists][S3] 包含 C++20 与 clang-cl 分支。关闭示例、GUI 和 scene 后，仍有 DXC、Agility SDK、D3D12MA 和 PIX 运行库：脚本指定 DXC `v1.9.2602`、Agility `1.619.1`、D3D12MA `v3.0.1`、WinPixEventRuntime `1.0.240308001`。scene/GUI 默认关闭，示例默认开启；window 源文件包含在基础静态库目标中，不等于调用者必须创建窗口。

需在 V0 中显式核对：SDK 选择不随本机安装集合漂移、源码/归档 hash、运行库分发、PIX 头文件补丁、AVX2 编译基线，以及项目的编译/分配规则。存在 clang-cl 分支只证明有相关构建配置，不证明本工程已编译、链接或启动成功。未经统一集成，不直接修改根 CMake 或依赖锁。

## 4. 下一步：先收敛绑定与执行契约

建议将下一项产出定位为可评审的 **RHI 资源绑定与执行契约**，不是马上编写 Renderer，也不是先铺大量类和工厂。需要回答：

1. **管线与绑定**：Vertex/PS、Mesh/可选 AS/PS、Compute 共用怎样的资源视图、布局与参数数据；反射在准备阶段承担什么，录制看到哪个不可变绑定版本。
2. **资源与描述符寿命**：CPU owner、未提交录制、在飞提交分别持有什么；数据更新、取消录制和描述符复用如何保证旧工作仍有效。
3. **命令与完成**：录制上下文和提交职责如何分离；何时能重用 allocator、上传区域和临时资源；成功、未完成、超时与失效如何观察。并行录制和多队列需求在此明确，不从旧占位继承限制，也不凭“复杂”二字默认全部实现。
4. **RDG 交接**：资源访问声明、导入/导出状态和完成依赖由谁解释；RHI 接受怎样的执行计划。RDG 本体是否在近期实现另行明确，不让其与 RHI 各自推断一套状态。
5. **硬件与错误边界**：Mesh Shader 为必需能力、无旧 GPU 回退；其他特性各自查询。明确失败是否结束渲染会话、是否要求设备重建，不把日志或静默跳过当错误协议。

语言未来可以生成参数与几何，但本轮不冻结语言 ABI，也不假设业务对象布局等于 shader 数据布局。多数物体静态只影响代表性负载，不取消正确的更新与在飞版本语义。

## 5. 一个能决定路线的真实实验

沿用 [验证方案][V] 的 V0–V5，不增加另一套永久验收体系：

- **V0**：固定 gfx 与依赖，用项目工具链构建最小消费者；先确认实际 Mesh Shader 能力与 shader 编译，不以 GPU 型号代替查询。
- **V1/V3**：Compute 写数据和间接参数；Vertex 与 Mesh 两条路径消费数据并输出颜色/深度；回读核对结果。数据与参数必须真的来自 GPU，不以 CPU 常量冒充 GPU-driven。无需建场景系统或程序化建模系统。
- **V2/V4**：重点比较普通 gfx 路径与受支持的 interop 路径；按合法协议测试旧工作在飞时发布新数据、释放调用方 owner、完成后回收、无需 Present 的推进。并行/多队列只在契约要求时加入。
- **V5**：验证 shader/管线准备失败是否可观察、提交完成是否可判断、停止工作后能否有界收尾。真实 device-loss 实验仍需单独批准，不运行故意挂死 GPU 的负载。

**判断规则**：若 gfx/interop 能在不维护冲突状态或侵入内部的情况下满足契约，就保留整体复用路线；若关键语义必须重写其提交、状态或退休核心，则如实计入长期分叉成本，再比较更低层机制复用。不要先选自研再寻找否决理由，也不要因 AMD 关联忽略语义差异。

本轮未配置/编译 gfx、未安装依赖、未运行 GPU、未改生产 RHI。源码存在相关实现不等于本项目 PASS，更没有性能优劣结论。

[S0]: https://api.github.com/repos/gboisse/gfx/git/commits/e039b255efb327994d30b187caee400ca60110e0
[S1]: https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.h
[S2]: https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/gfx.cpp
[S3]: https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/CMakeLists.txt
[S4]: https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/README.md
[S5]: https://github.com/gboisse/gfx/blob/e039b255efb327994d30b187caee400ca60110e0/LICENSE.md
[S6]: https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/Capsaicin/master/.gitmodules
[S7]: https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/Capsaicin/master/README.md
[V]: validation-plan.md
