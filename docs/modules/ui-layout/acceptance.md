# 生产首增量：正式验收证据

状态：**自研生产库的首增量已通过正式开发/发布配置；每个配置17个场景PASS，最小消费者亦PASS，四个原生进程退出码均为0。** 不是研究探针结果的转用。当前行为合同见[contract.md](contract.md)，后续未完成项见第5节；页尾旧Yoga资料只作历史。

## 1. 实际构建与运行入口

生产源码是 [`src/ui_layout/layout.hpp`](../../../src/ui_layout/layout.hpp) / [`layout.cpp`](../../../src/ui_layout/layout.cpp)，消费者是 [`acceptance/ui_layout/main.cpp`](../../../acceptance/ui_layout/main.cpp) 与 [`minimal.cpp`](../../../acceptance/ui_layout/minimal.cpp)。全部由仓库根CMake正式目标和现有presets产生，没有独立编译参数表或第三方solver路径。

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target ui_layout_acceptance
cmake --preset release
cmake --build --preset release --target ui_layout_acceptance
```

本次先通过正式presets构建两个原生目标、分别捕获进程输出，再实际运行上述`ui_layout_acceptance`聚合入口；两个聚合入口也均返回0。总体`acceptance`已经接入布局消费者，**本轮未重跑不受布局源码影响的Runtime/JIT全项目验收**，不把局部通过写成全仓通过。

配置时实际复用了`E:/FrostFoxAtelier/out/release/_deps/`中的mimalloc、zlib、zstd源码，使用对应的`FETCHCONTENT_SOURCE_DIR_MIMALLOC/ZLIB/ZSTD`覆盖；未改正式锁文件。普通复现可省略覆盖，走既有锁定下载路径。本次构建/运行的工具版本、锁文件内容和源码哈希在环境记录中，源码未提交时不虚构git revision。

| 配置 | CMake类型 | 最小消费者 | 正式17场景消费者 | stderr / 超时 |
|---|---|---|---|---|
| dev | Debug | exit 0 / PASS | exit 0 / 17 PASS、0 FAIL | 空 / 否 |
| release | Release | exit 0 / PASS | exit 0 / 17 PASS、0 FAIL | 空 / 否 |

检查通过普通运行时`require`执行，Release不会因NDEBUG去掉。新核心和消费者均按既有`/W4 /WX`规则编译。两个配置的源码哈希一致。

## 2. 覆盖矩阵

| 场景名 | 实际执行的可观察合同 |
|---|---|
| `identity_and_reuse` | 不同Document身份、跨Document/空/失效ID拒绝、根保护、子树移除、slot复用不复活旧ID、销毁后ID拒绝；编辑仍保留旧发布 |
| `atomic_ordered_edits` | 指定位置插入、同父移位的移除后位置语义、跨父移动保留ID、先序结果；非法位置/叶父/环/带孩子容器改叶拒绝；no-op不推进revision |
| `capacity_and_depth` | 非法构造容量、节点上限、移除后容量恢复、创建深度及移动后代深度检查；失败不改变输入revision |
| `bounded_allocation` | 池300的`[120,150,30]`全部6种顺序；min80/0得到80/20，第二项max10时为90/10；硬min溢出、上限余白及1:2权重 |
| `weight_dynamic_range` | `[1e30,1,1e-30]`权重、`[1,1,无]`上限得到`[1,1,98]`，真实行容器6种排列通过 |
| `slots_padding_and_columns` | padding/gap扣除、50槽中20宽框居中、交叉轴对齐，以及纵向80/20分配 |
| `known_width_natural_height` | 真实中文宽100自然高186.484375大于viewport高40；静态计算复用同一发布、不访问节点；只改viewport高不重测，增宽及字体/provider版本改变后重测，warm=fresh |
| `independent_content_width` | Content容器独立宽度被max180约束，再以最终180测真实高度；一次独立查询＋一次确定宽度查询，后续命中相互独立的缓存 |
| `demand_driven_measurement` | 确定且填满的Expand文字框不询问未使用的自然高；移动到Content祖先后按真实依赖询问；Fixed高度不因宽度变化重测未使用的自然高 |
| `provider_metrics_reuse` | 换行、尾空格、空串、英文长词、中文在反复窄/宽查询下与新建DirectWrite对象一致；专门的3次查询仅创建1个metrics对象，不声称内部shaping只发生一次 |
| `transactional_failure_and_reentry` | 三叶变更后第三次测量失败，保留旧publication、input revision、视图地址和全部几何；provider错误码77保留；重试重新测3叶；所有编辑/布局重入busy；NaN/普通异常拒绝，provider bad_alloc传播并可恢复 |
| `geometry_failure_cache_rollback` | 真实度量已成功、随后锚定坐标溢出失败：错误指出具体节点，保留旧发布；修正定位后必须重测，证明几何阶段失败也不提交本次测量缓存 |
| `local_cache_and_slot_reuse` | 只改一个文字叶只调用1次provider；等宽兄弟重排不重测且保留ID，结果匹配fresh；slot复用后清空旧节点缓存 |
| `anchors_and_logical_precision` | 800×450父框右下80×40框得到710/400；嵌套0.4+0.4保留逻辑0.8；不同DPI纯值消费不回写；out-of-flow子项不消耗普通行分配 |
| `invalid_inputs_and_dependencies` | min/max、padding、零权重、NaN、非法根、负/Inf viewport拒绝；未支持的overlay自然需求组合不发布半树；缺失真实内容保留provider错误码91，不伪装为零框成功 |
| `owned_geometry_lifetime` | 编辑不替换借用结果，下一次成功发布才结束旧借用；拥有式几何副本经过发布和Document析构仍保持原值 |
| `explicit_deep_tree` | 真实深度4096、4097节点、底部DirectWrite叶：1次度量、12291次尺寸/定位访问；深子树删除不靠递归，旧拥有式快照继续保留4097个框 |

几何比较容差为`0.0002 + 0.000002 × max(abs(a),abs(b))`；某些精确可表达几何另做相等检查。本机固定输入、字体环境下通过不等于任意字体/构建的bitwise保证。

## 3. 最小依赖与边界证据

`atelier_ui_layout_minimal`只包含生产头文件和标准C++输出/异常处理，使用真实节点创建、计算、快照和移除；验证两项1:2分配、padding/gap、身份及旧快照，然后输出`UI layout minimal consumer: PASS`。

两个配置均检查了Ninja生成的实际链接命令及`llvm-readobj --coff-imports`导入表：

- 最小消费者链接自研`atelier_ui_layout`及项目默认分配器；无DirectWrite、Runtime/JIT、LLVM库或第三方solver链接项。
- 最小消费者直接导入mimalloc、C++/CRT与Windows系统DLL；**不是Windows-free可执行程序**。
- `DWrite.dll`只出现在真实文字消费者的直接导入表。真实消费者也不链接Runtime/JIT/RHI。
- 核心源文件/头文件没有Windows或字体头文件，输出仍是纯逻辑几何；没有窗口、绘制或输入交互验证。

## 4. 原始证据与复现限制

本机输出位于`out/review/ui-layout-production/`：

- [runs.json](../../../out/review/ui-layout-production/runs.json)：四个原生进程的完整stdout/stderr、退出码、超时与程序SHA-256。
- [dev-environment.json](../../../out/review/ui-layout-production/dev-environment.json) / [release-environment.json](../../../out/review/ui-layout-production/release-environment.json)：各配置运行源码哈希、正式锁文件内容、实际本地依赖覆盖和环境时间。
- [imports.json](../../../out/review/ui-layout-production/imports.json)：四个二进制的直接DLL导入表及工具退出码。
- [aggregate-targets.log](../../../out/review/ui-layout-production/aggregate-targets.log)：实际执行两个正式`ui_layout_acceptance`入口的输出。
- `dev/release-configure.log`、`dev/release-build.log`及`dev/release-minimal-commands.txt`、`dev/release-acceptance-commands.txt`：实际配置、编译和链接命令；同目录`run.ps1`只是本机证据捕获脚本，构建权威仍是根CMake/presets。

DirectWrite指定Segoe UI、en-US；中文可能使用系统字体回退，未对所有字体文件取哈希，不承诺跨机器复现完全相同小数。工作区生成的out不会自动随源码交付；缺失时按正式命令重建并运行，不声称另一机器已有本机日志。

## 5. 未测项与后续范围

- 没有用数十亿次操作实测generation/Document identity/revision耗尽；代码按不回绕/退役处理，但不把分支检查写成实际极限运行。
- 只通过真实provider seam注入了bad_alloc及失败；没有覆盖core每一个分配点的故障注入、字节级内存预算或任意极端数值组合。
- depth4096是输入场景，不是产品支持上限；没有硬帧预算、分配计数或端到端性能胜出结论。
- 当前全树几何计算，稳定叶缓存不是局部调度；共享列Inspector、换行/重复格、基线、虚拟集合、滚动稳定策略尚未实现。
- 没有并发编辑、异步测量、回调销毁安全、跨工具链/热卸载DLL稳定ABI、旧文字绘制租期或非Windows构建证明。
- 像素消费只证明不回写逻辑几何，不代表完整共边/clip/hit-test政策、RHI渲染或业务语言UI绑定通过。

## 历史附录：旧候选验收，非当前通过清单

以下原有证据和旧里程碑设想保留，不能用于抵扣上方自研正式验收。

### 验收：已证路径与正式里程碑

> **历史验收资料：旧候选探针及旧 Yoga 里程碑设想。** 当前已选择 [自研方向](decisions.md)；三候选真实运行结果见 [验证报告](../../research/ui-layout-reassessment/validation-results.md)。下列表格不是自研通过清单。

状态：以下保留当时的运行事实与未完成验收。自研正式验收须从当前方案重写，并由自己的消费者在开发/发布配置执行；不复用旧 PASS，也不因旧 `contract.md` 中的要求限制新设计。

#### 1. 已执行的两条真实路径

| 路径 | 实际构建/链接 | 结果 |
|---|---|---|
| 原始候选probe | 独立yogacore，probe复用当前release host参数/库列表（含Runtime/JIT） | exitCode=0，signal=null，stderr空 |
| 精简链接probe | 相同源码；移除Runtime/JIT静态及导入库，保留当前global new/mimalloc、yogacore、dwrite和Windows系统库 | exitCode=0，signal=null，stderr空 |

原始：[build.json](../../../out/review/wave1-meeting/ui-layout-path-probe-build.json)、[run.json](../../../out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json)。精简：[build.json](../../../out/review/wave1-meeting/ui-layout-minimal-build.json)、[run.json](../../../out/review/wave1-meeting/ui-layout-minimal-headless-layout.json)。[probe源码](../../../out/review/wave1-meeting/ui-layout-path-probe.cpp)已验证后冻结，生产模块并未从它生成。

两次stdout一致：

```text
CASE row/constraints/order/removal: PASS
CASE DPI/shared-edges/cold-equivalence: PASS
OBS nested native local-sum=0.000000; unrounded absolute=0.800000; desired single absolute-edge snap=1
CASE DirectWrite/dirty/font/constraint: PASS; measure_calls=5; final=(97.548828,53.203125); font=Segoe UI/20/en-US
UI layout path probe: PASS
```

这是实际输出，不是预测。三组CASE内部使用明确require检查，Release下不会因NDEBUG去掉断言。下面几何数值是这些真实入口所检查的具体fixture，不是全CSS规则说明。

#### 2. 本机已证的可观察行为

| 真实输入/动作 | 通过的观察 |
|---|---|
| root300×100，row，padding10，gap5，fixed50/fixed100/fill | A=(10,10,50,80)，B=(65,10,100,80)，C=(170,10,120,80) |
| 同约束重算；root宽改320 | warm不改几何；C=(170,10,140,80)，与fresh树一致 |
| children重排A,C,B | C=(65,10,140,80)，B=(210,10,100,80) |
| 移除并释放B | C=(65,10,245,80)，未释放孩子继续合法布局 |
| root宽101、三个equal fill、scale0 | 逻辑宽约101/3 |
| scale1 | 物理共边0,34,67,101，不是三个各round(width) |
| scale1.5，再回1 | 1.5时物理共边0,51,101,152；每个scale的warm=fresh |
| 嵌套0.4+0.4 | native rounded-local sum=0；unrounded absolute=.8；对绝对边界一次最近像素应为1 |
| DirectWrite真实Segoe UI文字，同输入第二次 | 测量计数不增加 |
| 从FrostFox改为FrostFoxAtelier layout，不dirty | 刻意负对照：仍旧几何/计数，证明content不会隐式通知求解器 |
| 显式MarkDirty | 向根传播dirty，重新measure，与fresh树和直接DWrite度量一致 |
| 字号16→20并dirty；父宽400→100 | 新真实度量生效；最终box约97.548828×53.203125 DIP |

几何比较容差0.001 DIP只适用于此小坐标fixture。measure_calls=5是该程序全部路径总数，不是每叶/每次布局的承诺。文本宽高是本机真实字体结果，不作跨OS/字体文件golden。

**嵌套OBS没有判Yoga缺陷**：它显示局部舍入后相加与保留逻辑绝对坐标再舍入是不同政策，支持核心scale0、composition单点派生结果。DPI CASE用于核对上游机制，不意味着生产核心要把DPI存进树。

#### 3. 这些PASS没有证明什么

- LayoutDocument、公共NodeId代际、编辑验证、失败快照发布、测量错误桥接尚未实现。
- 精简链接证明该源路径不需要Runtime/JIT库；仍保留allocator与Windows系统库，不声称Windows-free二进制或全平台可用。
- 没有正式模块CMake target，也没有该模块dev/release完整验收。此前Wave1生产PASS不能替代它。
- 未执行column、min/max全组合、非法输入、超深树/大规模、重入/错误恢复或所有文字脚本/字体fallback验收。
- 原始进程elapsed约47.78ms、精简约48.21ms含启动/字体等，不是布局性能比较；没有吞吐、尾延迟或内存收益结论。
- 真实DirectWrite是叶Adapter示范，不代表核心需要DWrite，更不是文字引擎完成。

#### 4. 正式第一里程碑必须覆盖

以下是**待实现验收要求**。每项必须从未来公开Interface进入，不绕过封装直改Yoga；只用真实求解器和实际叶来源，避免用假solver制造通过。

| 类别 | 场景 | 必须观察 |
|---|---|---|
| 生命周期 | create child→layout→remove subtree→槽复用→Document结束 | 正确几何；子树全释放；旧/foreign ID明确拒绝；旧快照纯值仍可读 |
| 有序树 | 同父重排、跨父移动、非法共享/循环、测量叶加孩子 | 合法变更更新几何；非法批次在变更前拒绝、无部分树提交 |
| 行/列 | 对称row/column，空容器、单孩子、多孩子 | padding只加一次，gap只在相邻项之间，轴交换后的几何正确 |
| 固定/填充 | 本轮fixture通过公开Interface重复 | 与表2一致；消费者不访问Yoga节点 |
| min/max | fill被max限制、被min撑大，fixed冲突，父空间不足 | 符合contract分配规则；冲突拒绝，合法溢出明确、不负尺寸、不隐式裁剪 |
| intrinsic | 真实文本叶、非测量固定叶、含自然子树的容器 | 内容尺寸与外框尺寸区分；最终输出不是临时measure缓存 |
| 失效 | style、children、content、font/measure revision、父约束逐项改变 | warm结果与同输入fresh一致，无过时cache；无操作不冒充新版本 |
| 非法尺寸 | 负数、NaN、Infinity、min>max、padding冲突、未知模式 | 操作诊断清楚；不交上游fatal、不返回零值成功 |
| 测量失败 | 真实内容键查找失败或实际文字Adapter错误 | 不发布半快照；旧成功结果仍带旧版本；修正后按已定义恢复策略工作 |
| 重入 | provider同步期间尝试layout/编辑 | busy/reentrant，外层拥有者/内容借用不被破坏 |
| 快照发布 | 两次输入版本，第二次失败或未布局 | 旧快照仍可读；不能把新revision贴旧结果；成功后完整替换 |
| 有界资源 | 选定容量/深度边界内与超限输入 | 边界内工作，超限求解前拒绝；不是用小树证明任意深度 |
| 逻辑精度 | 一份snapshot被两个DPI消费，嵌套fractional offset | 核心值未被任何一次像素化回写；输出政策由独立composition消费者决定 |
| 依赖/分配 | 单独core target + 最小consumer；真实Adapter另链接 | core无Runtime/LLVM/RHI/Windows头/库反向依赖；沿正式allocator/许可证约定 |
| 开发/发布 | 两种正式配置均运行真实消费者验收 | 检查不被优化/NDEBUG删掉；代码、target与运行证据对应 |

不得为“全绿”缩窄输出或改golden去迎合错误。失败先保留输入、pin、构建、实际与预期和根因，再修实现或明确裁决尚未冻结的合同。

#### 5. 复现入口

优先使用保留的精简build.json中的真实命令与当前正式开发环境；该JSON含本机绝对路径，换机器时从同一CMake/toolchain重新产生相同契约的命令，不复制他机工具绝对路径。输出exe为 `out/release/wave1-ui-layout-minimal.exe`；它仍需要本配置mimalloc相关DLL。普通probe不是已经注册的正式cmake target。

```powershell
. ./tools/enter-dev.ps1
./out/release/wave1-ui-layout-minimal.exe
if ($LASTEXITCODE -ne 0) { throw 'Layout candidate probe failed' }
```

此命令只用于已保留且与证据匹配的二进制；缺失/源码改变时先按固定pin和真实build命令重建，不运行陈旧exe。首次取包步骤见[原研究报告第9节](../../research/wave1-meeting/parallel-modules/ui-layout.md)，该节示例命令与Main实际命令的区别以research说明为准。正式模块实现后改从其真实target和consumer验收，不能永久依赖研究out脚本。

#### 6. 完成定义

正式交付须同时具备实现、唯一CMake/依赖/许可接入、表4全部合同项的可观察证据，以及清晰仍不支持的范围。测量失败、过期ID或错误发布任何一项空缺，均不能称第一里程碑完成。成功交付更新本页“待实现”状态时附实际构建/运行来源，不沿用本轮candidate PASS填充。
