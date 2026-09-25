# Wave 1 本机证据与复现入口

日期：2026-09-25。所有新实验由Main串行执行；审查席位只读结果。基线正式dev/release验收复用已匹配源码哈希的历史结果，本轮没有重新运行开发配置，也没有修改生产实现。裁决见[总报告](decisions.md)。

## 1. 证据组织与解释规则

- [baseline.json](baseline.json)：36个当前源码/构建/语言规范文件的SHA-256，及既有正式验收来源。
- [完整实验索引](../../../out/review/wave1-meeting/evidence-index.json)：19个成功构建、46次运行的精确参数、退出码、build.json/log与原始JSON路径。
- [派生数字表](../../../out/review/wave1-meeting/measurement-summary.json)：下表从raw计算的描述统计，不替代raw。
- [实际机器记录](../../../out/review/wave1-meeting/machine-environment.json)：CIM采集的OS/CPU/显卡驱动信息；实验选用的RX6800XT驱动为32.0.21045.5002。它不证明测量期间机器无其他负载。
- 实验源码和产物都在out；实验目录不是第二套生产构建系统。build.json从现有CMake compile_commands和实际链接行派生。
- 每个*-build.json保存真实命令，*-build.log保存stdout/stderr；运行JSON包含name/args/exitCode/stdout/stderr。实际EXE名是out/release/wave1-<name>.exe，不是早期报告中的建议文件名。
- 基线取址探针退出2表示已观察到契约失败；诊断探针退出0表示测量完成；mixed分区探针退出0表示预期拒绝及清理边界得到确认。不能按退出码统一盖“产品通过”。
- elapsed_ms是整个子进程耗时，不作为热路径性能。性能结论只取探针明确标出的内部计时边界；无置信区间、跨机器或生产尾延迟保证。

## 2. A：成员地址与失败展开

| 原始记录 | 观察 | 判定 |
| --- | --- | --- |
| [types-counter-address-default.json](../../../out/review/wave1-meeting/types-counter-address-default.json) | exit2；读91而非实际17；写后actual17/sentinel73；operator&调用2；edges0/target_alive0；cleanup PASS | 当前默认适配缺陷已复现 |
| [types-addressof-probe-default.json](../../../out/review/wave1-meeting/types-addressof-probe-default.json) | exit0；读17、写后actual73/sentinel91；operator&调用0；edges1/target_alive1；cleanup PASS | 只改out头文件的最小修法已验证，不是生产修复 |
| [types-counter-unwind-default.json](../../../out/review/wave1-meeting/types-counter-unwind-default.json) | initialize/copy各callbacks3/inner_rejections3；逆序原址恰好一次清理与重试PASS；最终live0、后续注册PASS | 该失败×重入组合通过，不外推OOM |

两种取址构建共用正式Runtime；variant驱动只换头文件include，隔离头显式加入<memory>与std::addressof。精确配方分别为types-counter-address-build.json和types-addressof-probe-build.json。

## 3. B：GC计数、取消与预算

[gc-counter-lifetimes-cursor.json](../../../out/review/wave1-meeting/gc-counter-lifetimes-cursor.json)、[identity.json](../../../out/review/wave1-meeting/gc-counter-lifetimes-identity.json)、[sweep.json](../../../out/review/wave1-meeting/gc-counter-lifetimes-sweep.json)均exit0。取消的四组合中，无重叠根的after_cancel为0；有独立重叠根则继续扫描合法。旧身份/新代际、错误后槽保持和下一周期收敛通过。单次sweep work1/reclaimed1/destructors257；1200ns只是单次观察，不是上界。

七组规模结果每组三轮；固定图，无中途mutator增删：

| raw后缀（前缀gc-explore-probe-） | work/每轮 | poll/每轮 | 三轮总周期wall ns |
| --- | ---: | --- | --- |
| roots-10000-1-0.json | 10005 | 10005 | 1022300 / 987300 / 982300 |
| slots-10000-1-0.json | 20004 | 20004 | 1973400 / 1995800 / 1975000 |
| empty-slots-10000-1-0.json | 10004 | 10004 | 1004200 / 996800 / 1094600 |
| wide-1024-1-0.json | 1033 | 1033 | 116000 / 102700 / 102600 |
| deep-512-1-0.json | 1032 | 1032 | 112100 / 105200 / 105100 |
| slots-10000-512-0.json | 20004 | 40 | 59700 / 60400 / 60100 |
| slots-10000-512-1.json | 20004 | 6179 / 6186 / 5800 | 1187200 / 1179200 / 1213700 |

文件均在[实验目录](../../../out/review/wave1-meeting/)，精确链接由完整索引提供。总周期包含外层poll与采样开销，不能把全部差额归因collector或读钟；1ns不是硬时限。宽/深赋值分别有idle/mark三样本，其时间包含赋值及同步屏障，不是collect_step.elapsed。小样本分位数不作稳定尾延迟结论；未量测元数据字节或证明全进程无泄漏。

## 4. C：前端、宿主诊断与隔离索引

- [原41项性质](../../../out/review/wave1-meeting/source-explore-probe-properties.json)与[候选41项性质](../../../out/review/wave1-meeting/source-active-probe-properties.json)均PASS。
- [source-counter-semantics-diagnostics.json](../../../out/review/wave1-meeting/source-counter-semantics-diagnostics.json)：2100字节源名，请求2/3各2047字节错误且完全相同；identity/position/cause=false，DEGRADATION_OBSERVED；旧Entry正常。
- [source-counter-semantics-semantics.json](../../../out/review/wave1-meeting/source-counter-semantics-semantics.json)：Native/Described各4例，共8例PASS；验证指定求值顺序、失败停止和显式清理，不证明所有GC交错。
- [额外反证A](../../../out/review/wave1-meeting/source-active-counter-a-counter.json)与[B](../../../out/review/wave1-meeting/source-active-counter-b-counter.json)：6例全部通过，完整输出逐字一致。覆盖重复参数、重复名优先于坏RHS、退出局部不可自见、兄弟块同名异型、内块退出不删外层、短源码key跨locals增长。未合并进原41项计数。

### 4.1 scale：只计compile_source，不含返回Program销毁

每点一个预热后五样本；源码生成和树验证在计时外。A为当前source.cpp，B为out/source-active-names.cpp；同driver/编译参数，绝不同时链接两份compile_source。原始数据：[A](../../../out/review/wave1-meeting/source-explore-probe-scale.json)、[B](../../../out/review/wave1-meeting/source-active-probe-scale.json)。

| 工作负载 | N | A median ms | B median ms | A/B |
| --- | ---: | ---: | ---: | ---: |
| wide_unique | 64 | 0.0254 | 0.0177 | 1.44 |
| wide_unique | 256 | 0.1866 | 0.0554 | 3.37 |
| wide_unique | 1024 | 3.3532 | 0.2322 | 14.44 |
| wide_unique | 4096 | 40.8693 | 1.0115 | 40.40 |
| sibling_scope | 64 | 0.0174 | 0.0206 | 0.84 |
| sibling_scope | 256 | 0.0725 | 0.0791 | 0.92 |
| sibling_scope | 1024 | 0.2575 | 0.2864 | 0.90 |
| sibling_scope | 4096 | 1.0851 | 1.1514 | 0.94 |
| old_parameter_lookup | 64 | 0.0415 | 0.0295 | 1.41 |
| old_parameter_lookup | 256 | 0.3042 | 0.1140 | 2.67 |
| old_parameter_lookup | 1024 | 4.3812 | 0.4579 | 9.57 |
| old_parameter_lookup | 4096 | 57.0574 | 1.9374 | 29.45 |
| newest_local_lookup | 64 | 0.0400 | 0.0334 | 1.20 |
| newest_local_lookup | 256 | 0.2441 | 0.1291 | 1.89 |
| newest_local_lookup | 1024 | 3.5143 | 0.5092 | 6.90 |
| newest_local_lookup | 4096 | 39.0045 | 2.2114 | 17.64 |
| sequential_reference_blocks | 64 | 0.0256 | 0.0298 | 0.86 |
| sequential_reference_blocks | 256 | 0.0971 | 0.1138 | 0.85 |
| sequential_reference_blocks | 1024 | 0.3624 | 0.4268 | 0.85 |
| sequential_reference_blocks | 4096 | 1.5667 | 1.7020 | 0.92 |

全部节点数量对应。宽场景改善与独立块回退同时保留；不构成JIT端到端提速或容器分配占比测量。

### 4.2 small：批量编译并销毁，不能与scale绝对时间混算

每点一个预热批次后九批，每批1000次；每次含返回Program销毁、成功校验及计数累加。原始数据：[A](../../../out/review/wave1-meeting/source-active-counter-a-small.json)、[B](../../../out/review/wave1-meeting/source-active-counter-b-small.json)。

| 工作负载 | N | A median μs/次 | B median μs/次 | B/A |
| --- | ---: | ---: | ---: | ---: |
| wide_unique | 0 | 0.4245 | 0.5113 | 1.204 |
| wide_unique | 1 | 0.7077 | 0.8324 | 1.176 |
| wide_unique | 8 | 2.8704 | 2.8561 | 0.995 |
| sibling_scope | 0 | 0.4092 | 0.5002 | 1.222 |
| sibling_scope | 1 | 0.8376 | 0.9212 | 1.100 |
| sibling_scope | 8 | 3.1390 | 3.4701 | 1.105 |
| sequential_reference_blocks | 0 | 0.4095 | 0.4992 | 1.219 |
| sequential_reference_blocks | 1 | 0.9045 | 1.0593 | 1.171 |
| sequential_reference_blocks | 8 | 4.1426 | 4.6701 | 1.127 |

本轮没有测量分配次数/字节，也不新增阈值快路或紧凑哈希依赖。候选留在out，需真实源码分布与端到端收益后再决定。

## 5. D：编译、调用、回收、退休分开

[execution-probe-measure.json](../../../out/review/wave1-meeting/execution-probe-measure.json)含16场景、80次编译/退休及每场景9个128调用批次；全部内置值/根/清理/退休断言PASS。编译aggregate包含前端+LLJIT建立+降低+物化；没有内部stage拆分。warm_execute含checked host execute、根结果转移和driver loop；reclaim另计root reset+现有collect helper。调用内未请求GC。第一个native empty compile为进程首编译，原始标志保留，不当作纯暖样本。

| 类型 | 场景 | 强/弱槽 | compile median ms（5次） | execute median μs/128次 | reclaim median μs/128次 |
| --- | --- | --- | ---: | ---: | ---: |
| native | empty | 2/0 | 3.4775 | 86.6 | 14.5 |
| native | default_loop | 2/0 | 4.5256 | 731.7 | 14.5 |
| native | strong_weak | 5/2 | 4.3912 | 130.7 | 14.5 |
| native | sequential_null_blocks | 3/0 | 3.1953 | 98.4 | 14.5 |
| native | sequential_null_blocks | 18/0 | 5.7069 | 238.0 | 14.6 |
| native | sequential_null_blocks | 66/0 | 13.7238 | 693.7 | 14.5 |
| native | early_return_blocks | 18/0 | 11.5649 | 128.3 | 14.6 |
| native | early_return_blocks | 66/0 | 110.0510 | 230.3 | 14.5 |
| described | empty | 2/0 | 3.3574 | 90.4 | 14.3 |
| described | default_loop | 2/0 | 4.4799 | 840.8 | 14.4 |
| described | strong_weak | 5/2 | 4.4160 | 134.4 | 14.5 |
| described | sequential_null_blocks | 3/0 | 3.1512 | 101.6 | 14.5 |
| described | sequential_null_blocks | 18/0 | 5.8335 | 244.1 | 14.5 |
| described | sequential_null_blocks | 66/0 | 13.4701 | 696.3 | 14.6 |
| described | early_return_blocks | 18/0 | 11.4115 | 132.2 | 14.5 |
| described | early_return_blocks | 66/0 | 109.7566 | 240.8 | 14.6 |

全部80个退休样本的合并中位56.85μs，仅描述此混合场景集合。9个暖批次的P95只是这9点的经验分位数，不是服务SLO。

额外四模式：

- [layout](../../../out/review/wave1-meeting/execution-counter-semantics-layout.json)：不可达3块把2/0变成5/6槽；扩展元素120/288字节不是完整分配占用。
- [semantics](../../../out/review/wave1-meeting/execution-counter-semantics-semantics.json)：第二poll失败，失败前cycles1、返回后仅宿主receiver根，显式清理通过；同时调用既有词法根/循环槽消费者。
- [owner](../../../out/review/wave1-meeting/execution-counter-semantics-owner.json)：最后owner活动销毁，正常与native bad_alloc路径通过，不外推真实OOM。
- [finalize](../../../out/review/wave1-meeting/execution-counter-finalize-default.json)：Standard-only/Finalize-only成功；mixed同reservation自然拒绝，回补abandon后两地址MEM_FREE。配方沿正式/MT、当前backport.obj和LLVM静态依赖，不链接宿主/MD对象。两条invalid-address是首次释放和abandon再次尝试，不是“两次泄漏”。不含unwind action，不证明业务COFF可达或业务泄漏。

## 6. E：发布、持有与退休

[reload-counter-interleavings-default.json](../../../out/review/wave1-meeting/reload-counter-interleavings-default.json)：E1=8、E2=4，全部PASS。实际入口包括Entry/Lease唯一活动持有、正常/原生失败退出，以及busy候选遇到更新请求parse/link失败后的陈旧拒绝和caller-owned退休。

E-reload-probe八个0/1组合代表Native/Described、history关闭/保留、旧pin关闭/保留，每组16版本，运行参数和文件名见完整索引。每组还有16个真实错误候选与每版本32次busy拒绝；共128次成功更新、128次失败、4096次busy路径均被断言覆盖。版本2–16的成本分项见[探索报告的实测表](reload/explore.md)：

- 无pin publish中位58.7–62.9μs；有pin publish1.7–1.9μs，最后旧Lease释放54.7–61.8μs，逐行合计中位56.7–63.7μs。不能把退休延后当作消除成本。
- 保留32 Request+16诊断对应9611/9731字节文本，不阻止代码退休；代码页/展开元数据通过指定退休检查。
- 所测后段private_done不增长，但全退/清历史后commit仍高于start约24.27MiB，未完成归因。样本不支持全进程无泄漏结论；working set/private commit不同于活对象或代码页计数。

## 7. 两条并行模块路线

| 路径 | 当前实测 | 限制 |
| --- | --- | --- |
| [原布局路径](../../../out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json) | Yoga行布局/约束/顺序/删除、DPI共享边/冷树一致、真实DirectWrite文字失效通过；measure_calls5 | 原配方复用正式host依赖，不单凭它宣称独立target |
| [精简布局链接](../../../out/review/wave1-meeting/ui-layout-minimal-headless-layout.json) | 同源码同结果；[build.json](../../../out/review/wave1-meeting/ui-layout-minimal-build.json)删除Runtime/JIT库，保留mimalloc/new+yogacore+dwrite/系统库 | 仍复用既有global_new编译对象/环境；不是干净目录CMake目标；无文字像素渲染验收 |
| [原GPU路径](../../../out/review/wave1-meeting/rhi-path-probe-hardware-debug.json) | RX6800XT硬件、debug开、两次37×19读回零差异，fence后释放，debug消息/错误/警告0 | gpu_validation=0，device loss未测 |
| [精简GPU链接](../../../out/review/wave1-meeting/rhi-minimal-hardware-debug.json) | 同源码同结果；[build.json](../../../out/review/wave1-meeting/rhi-minimal-build.json)删除Runtime/JIT库，保留mimalloc/new+D3D12/DXGI/系统库 | SDK路径，不是未来Module取消/丢ticket/资源复用/关停的实现证明 |

Yoga固定v3.2.1提交与压缩包SHA见[provenance](../../../out/review/wave1-meeting/yoga-provenance.json)，只在out研究目录构建；生产依赖锁没有添加它。嵌套舍入观测local-sum0、未舍入绝对0.8、目标一次snap1，只支持后续单点像素转换的设计选择，不是上游缺陷或完整文本渲染结论。

## 8. 复现与校验

从仓库根按README准备工具链。以下示例使用真实保存的配方与实际EXE：

```powershell
. ./tools/enter-dev.ps1
./out/review/wave1-meeting/build-probe.ps1 -Plan ./out/review/wave1-meeting/types-counter-address-build.json
& ./out/release/wave1-types-counter-address.exe
# 当前基线预期退出2：这个命令用于复现缺陷，不应吞掉该退出码。
./out/review/wave1-meeting/build-probe.ps1 -Plan ./out/review/wave1-meeting/source-active-probe-build.json
& ./out/release/wave1-source-active-probe.exe properties
& ./out/release/wave1-source-active-probe.exe scale
```

其余参数全部在完整索引。LLVM低层实验必须用自己的/MT配方，不能复用host链接或用未回补archive成员替代正式对象。

生产基线匹配、报告/模块文档内部链接、占位符和原始证据存在性由[最终校验记录](../../../out/review/wave1-meeting/validation.json)保存；它仅是本次文档/证据校验，不是额外生产功能验收。研究阶段的未来验收门槛明确保留，不伪装成已经实现。
