# 验收：已证路径与正式里程碑

状态：2026-09-25。Main已统一运行候选探针；下面分开记录本机事实与尚未实现的Module验收。规则唯一来源为 [contract.md](contract.md)，来源pin/hash见 [research.md](research.md)。本页不把未执行表格当通过清单。

## 1. 已执行的两条真实路径

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

## 2. 本机已证的可观察行为

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

## 3. 这些PASS没有证明什么

- LayoutDocument、公共NodeId代际、编辑验证、失败快照发布、测量错误桥接尚未实现。
- 精简链接证明该源路径不需要Runtime/JIT库；仍保留allocator与Windows系统库，不声称Windows-free二进制或全平台可用。
- 没有正式模块CMake target，也没有该模块dev/release完整验收。此前Wave1生产PASS不能替代它。
- 未执行column、min/max全组合、非法输入、超深树/大规模、重入/错误恢复或所有文字脚本/字体fallback验收。
- 原始进程elapsed约47.78ms、精简约48.21ms含启动/字体等，不是布局性能比较；没有吞吐、尾延迟或内存收益结论。
- 真实DirectWrite是叶Adapter示范，不代表核心需要DWrite，更不是文字引擎完成。

## 4. 正式第一里程碑必须覆盖

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

## 5. 复现入口

优先使用保留的精简build.json中的真实命令与当前正式开发环境；该JSON含本机绝对路径，换机器时从同一CMake/toolchain重新产生相同契约的命令，不复制他机工具绝对路径。输出exe为 `out/release/wave1-ui-layout-minimal.exe`；它仍需要本配置mimalloc相关DLL。普通probe不是已经注册的正式cmake target。

```powershell
. ./tools/enter-dev.ps1
./out/release/wave1-ui-layout-minimal.exe
if ($LASTEXITCODE -ne 0) { throw 'Layout candidate probe failed' }
```

此命令只用于已保留且与证据匹配的二进制；缺失/源码改变时先按固定pin和真实build命令重建，不运行陈旧exe。首次取包步骤见[原研究报告第9节](../../research/wave1-meeting/parallel-modules/ui-layout.md)，该节示例命令与Main实际命令的区别以research说明为准。正式模块实现后改从其真实target和consumer验收，不能永久依赖研究out脚本。

## 6. 完成定义

正式交付须同时具备实现、唯一CMake/依赖/许可接入、表4全部合同项的可观察证据，以及清晰仍不支持的范围。测量失败、过期ID或错误发布任何一项空缺，均不能称第一里程碑完成。成功交付更新本页“待实现”状态时附实际构建/运行来源，不沿用本轮candidate PASS填充。
