# 较大字段表缺失查询的归因

**已定位到自研候选对默认 `StringView` 内联表使用的指纹。** 在这份缺失轨迹里，矩阵字段名的两个末字节反复产生相同指纹。更换缺失字符能让候选和现有后端的快慢关系反转。修正指纹后，完整 Release 程序的配对差距从 +7.29% 降到 +2.02%。这次保留了现有产品后端。

## 复现与分层

上次报告的 +7.46% 来自 36 轮。此次在同一台 Ryzen 9 3950X、逻辑处理器 18 上，先用留存的两份可执行文件交错复跑 18 轮，现有后端为 47.62 ns，自研候选为 51.69 ns，18 轮全部变慢，配对中位数 +8.60%。重新编译的两个对照程序，其 PE `.text` 段 SHA-256 分别与对应旧程序相同。[留存程序复测](results/archived-summary.csv)

真实调用先由 `Runtime::Impl::lookup` 按上下文和类型下标取得类型，再查 `field_by_name`。失败时构造含 `Unknown field: ` 和查询名的 `Error`，返回 `invalid_member`。两份留存程序的 `find_field` 主体同为 802 字节，200 条指令的助记符顺序一致，所调用的探测函数不同。为了控制不同二进制的代码布局、目录对象和分配器状态，诊断程序还在同一份 Runtime 注册结果里同时填充三种索引和指纹修正版，共用同一调用点与错误构造代码。[Runtime 实现](../../../../modules/execution/runtime/src/runtime.cpp) · [诊断程序](profile.cpp)

测试重新生成了上次相同的 4,096 个请求，使用同一随机种子、字段顺序和末字节替换。所谓“较大”指大于 4 项。8 张表里有 7 张为 5–16 项，共 3,069 次请求，候选这时仍走内联指纹。只有 `world.EnvironmentComponent` 一张为 24 项，承担另外 1,027 次外部索引查询。所有构造出的缺失名均逐项验证，完整错误路径还核对了返回的消息正文。[逐表样本与指纹计数](results/catalog.csv)

## 指纹为什么碰撞

原式取 `31 × 长度 + 13 × 倒数第二个字节 + 最后一个字节` 的低 8 位，再将保留值重映射。矩阵字段 `m00…m33` 都长 3 字节。从 `m00` 到缺失键 `m1#`，倒数第二字节增加 1，末字节从 ASCII 48 降到 35，正好抵消。两者指纹都为 253。`m2#` 对 `m10`、`m3#` 对 `m20` 也有同样的关系。指纹匹配后必须比较完整字段名才能确认缺失。

两张矩阵表共收到 1,395 个请求，分别触发 506 和 541 次多余的完整键比较。合计 **1,047 次**，占全轨迹的 25.6%。其余五张内联表在原轨迹上合计为零。这些数字由建表键与实际查询键逐一比较真实指纹算得；外部索引不计入这个计数。

只把同一批矩阵请求的最后一个字节从 `#` 改成 `!`，保留类型、顺序、键长和错误格式，原候选的矩阵假匹配从 1,047 次降到零。下表是同一诊断程序中 20 轮的 ns/次中位数。矩阵两行使用同一查询缓冲，原地修改最后一个字符。[分层计时](results/controlled-summary.csv) · [原始样本](results/controlled-samples.csv.gz)

| 负载 | 当前后端 | 原自研候选 | 修改指纹 |
| --- | ---: | ---: | ---: |
| 全部 4,096 次 `#` 缺失，完整 Runtime | 49.00 | 51.20 | 48.86 |
| 同一批，仅查真实字段索引 | 11.28 | 11.64 | 9.80 |
| 同一批，返回空消息的错误 | 16.52 | 16.98 | 15.31 |
| 两张矩阵表，`#` 缺失，完整 Runtime | 39.68 | 45.70 | 41.89 |
| 两张矩阵表，`!` 缺失，完整 Runtime | 39.21 | 36.11 | 35.43 |
| 两张矩阵表，`#` / `!`，仅查原候选索引 | — | 11.56 / 5.23 | 9.20 / 5.30 |

原候选在矩阵 `#` 完整路径上 20/20 轮更慢，在同长度的 `!` 路径上 20/20 轮更快。单独保留类型检查与完整错误构造、令探测直接返回缺失，混合轨迹约为 39.18 ns/次。按内联和外部索引分组后，原候选相对当前后端的仅索引配对差从 +0.30 ns 变成 −0.09 ns。可见请求混排也影响测值。各对照的指令执行和存储访问可以重叠，不能把独立计时相减，当作某一条指令或分配的耗时。

## 修正的实际效果

[指纹补丁](fingerprint.patch)把长度和末两个字节放入不同位段，再乘以固定奇数，取高 8 位。索引仍做完整键比较。这一改动将两张矩阵表的假匹配从 1,047 次降到 330 次，另外在 BodySettings 和 ProbeVolume 共出现 166 次，七张内联表合计从 1,047 降到 496。修正版保留为可重放补丁，前一轮候选源码仍可原样复现。

用正常 Release 后端独立构建当前、原候选、修正候选，沿用此前的完整 Runtime 和网格程序。六种进程顺序各跑三次，成对比较，没有诊断分派或重复索引。表中三列是 18 轮中位数，百分比按同轮配对计算。[完整 Release 汇总](results/release-summary.csv) · [原始样本](results/release-samples.csv.gz)

| 指标 | 当前后端 | 原自研候选 | 修正候选 |
| --- | ---: | ---: | ---: |
| 5 项以上字段表同长度缺失，ns | 46.52 | 50.03 | 47.37 |
| 5 项以上字段表命中，ns | 21.92 | 21.51 | 21.08 |
| 小字段表同长度缺失，ns | 41.75 | 40.49 | 40.27 |
| 小方法表命中，ns | 20.69 | 17.74 | 18.29 |
| 小方法表同长度缺失，ns | 37.99 | 38.78 | 38.93 |
| 64K 整数 Map 混合点查，ns | 7.75 | 7.28 | 7.29 |

目标缺失路径上，修正候选相对原候选 18/18 轮更快，配对中位数 −5.48%；相对当前后端仍有 15/18 轮更慢，配对中位数 +2.02%。没有证据将剩余约 0.85 ns 全部归到同一个原因。修正候选以及恢复后的默认版本都通过了 dev、release 的 Foundation、Runtime、Source、World、Procedural 端到端验收。

另做了一组同程序对照，只把内联名字指纹改为复用完整的 `Hash<StringView>`，阈值仍为 16。它把矩阵的假匹配降至 343 次，但每次内联查询都要算完整哈希。该组完整 Runtime 缺失为当前 46.14 ns、原候选 50.12 ns、完整哈希 51.40 ns；按同一字段分布回放正常命中分别为 22.23、21.04、23.72 ns。直接改用完整哈希没有取得更好的折中。此组只和本组的对照比较，汇总中的 `fixed` 指完整哈希。[完整哈希补丁](full-hash.patch) · [补充对照](results/full-hash-summary.csv) · [原始样本](results/full-hash-samples.csv.gz)

## 字段诊断时的依赖清单

下表记录字段诊断当时的源码。后续已迁移 Foundation 哈希与 Renderer 焊接，并移除 JIT 的直接链接。该阶段的默认大表和构建下载仍使用 ankerl。[最终切换](../retirement/README.md) · [迁移结果](../../foundation-hash/README.md)

| 位置 | 仍需迁移的内容 |
| --- | --- |
| Foundation 哈希 | `Hash<T>` 的整数、枚举、指针混合，`String/StringView` 的按长度字节哈希，`hash_fields` 的多字段混合，以及上游 `hash_is_avalanching_v` 特征。候选自己的 `mixed_hash` 也直接使用该特征，未声明充分混合的策略会经整数哈希再次混合。迁移时维持透明键之间的哈希与相等一致性。复合哈希的消费者包括 Runtime 的 TypeId/ObjectId 和 Procedural 精确布尔运算的规范化投影点。[公共哈希](../../../../modules/core/foundation/include/foundation/hash.hpp) · [候选索引](../owned-index/dense_hash_table.hpp) · [精确几何](../../../../modules/engine/procedural/src/detail/boolean_exact.hpp) |
| Renderer 顶点焊接 | `weld` 直接构造上游 Set，同时用上游字节哈希处理顶点的完整对象表示。集合保存 32 位下标，拿顶点做异构查询。Hash/Equal 的实例都借用同一个唯一顶点数组，比较保留 `memcmp` 的逐字节语义。现有 Set 只默认构造策略。迁移需要接通策略实例、复用统一字节哈希，并验收唯一顶点和索引输出。研究中的网格性能数据来自 Procedural 的位置焊点，Renderer 这条路径要单独验收。[Renderer 焊接](../../../../modules/render/renderer/src/renderer.cpp) · [Set](../../../../modules/core/foundation/include/foundation/set.hpp) |
| 构建、验收与交付 | Foundation 头目标和 JIT 的 `atelier_source_backend` 都显式链接 `unordered_dense::unordered_dense`。Foundation 验收也直接实例化上游 Map/Set 做尺寸对照，应换成项目自己的布局断言。生产使用点清空后才能移除下载锁项、FetchContent、补丁重放、对应许可证复制步骤并从空构建目录验收。研究目录的历史对照可单独保留。[项目构建](../../../../CMakeLists.txt) · [JIT 构建](../../../../cmake/jit.cmake) · [尺寸验收](../../../../acceptance/foundation/main.cpp) · [当时的构建接入](../retirement/ankerl-reference.patch) |

## 复现

在默认 worktree 的仓库根目录运行以下命令。脚本只在 `out` 生成诊断用的 Runtime 翻译单元，链接项目 Release 依赖。运行程序会固定测量线程到指定核。目录文件、指纹碰撞计数、逐轮输出和汇总都保存在输出目录。

```powershell
$study = 'docs/research/small-table-experiment/field-miss'
& "$study/build.ps1"
python "$study/run.py" --rounds 20 --cpu 18
& "$study/build.ps1" -HashPatch full-hash -OutputDirectory out/field-miss-study/uniform-control
python "$study/run.py" --out out/field-miss-study/uniform-control --rounds 20 --cpu 18
```

正常 Release 三端对照沿用[候选复现步骤](../owned-index/README.md)。在独立副本先构建原候选，随后对归档头应用本目录的 `fingerprint.patch`，重新复制该头到候选的 Foundation 私有路径，再构建修正候选。三份程序放入不同输出目录，交给同一个 [compare.ps1](../compare.ps1)。
