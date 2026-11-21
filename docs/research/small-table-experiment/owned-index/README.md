# 自有大表索引对照

这轮实验保留了当时的四指纹 + ankerl 默认实现。[最终接入结果](../retirement/README.md)。候选由项目自己管理连续条目和分组索引，公开 Map/Set 与 Hash/Equal 沿用原入口。这里保存通过端到端验收的候选，便于在独立工作树复现。[候选实现](dense_hash_table.hpp) · [Map/Set 接线](map-set.patch)

## 设计

默认对象仍是 Release 64 B、dev 72 B。候选利用对象内 16 个指纹字节，每个位置直接对应连续条目区的同一位置。第 17 个不同键或显式预留超过 16 项时才分配外部索引。外部每组有 15 个条目下标，SSE2 同时比较 16 个字节。第 16 字节记录有无条目越过这一组，完整的越组计数随插入和删除维护。删除中间元素时用末条目填洞，修正一个索引下标。遍历直接读取连续条目。

保留版本把各组指纹连续存放，查询未命中通常只读指纹。对使用默认 Hash/Equal 的 `StringView` 小表，根据字节长度和最后两个字节计算指纹，命中后仍比较完整字节。自定义判等和所有外部索引继续使用原有完整哈希。另一次按键类型将指纹与下标相邻存放的尝试，较大字段表的同长度缺失仍慢 5.15%，打散整数 64K Map 点查慢 4.59%，未保留该布局。

## 实测

同一台 Ryzen 9 3950X，Windows x64、clang-cl 23.1.5 Release、mimalloc、逻辑处理器 18。对照是已接入的 64 B 四指纹版本。两批各 18 轮，每批内两端交替先后执行，复用各自二进制。下表为 36 轮中位数，变化率按同轮配对再取中位数。[全部指标](summary.csv) · [原始记录](samples.csv.gz)

| 工作负载 | 现有后端 | 自有候选 | 配对变化 |
| --- | ---: | ---: | ---: |
| 注册 46 个真实类型，µs | 121.57 | 120.21 | −1.10% |
| 注册后存活分配，块 / B | 776 / 240,336 | 772 / 237,296 | −4 块 / −3,040 B |
| 小字段表命中 / 同长度缺失，ns | 19.04 / 42.38 | 17.95 / 41.15 | −5.66% / −2.98% |
| 5 项及以上字段表命中 / 同长度缺失，ns | 22.39 / 48.82 | 21.90 / 52.42 | −2.01% / **+7.46%** |
| 小方法表命中 / 同长度缺失，ns | 20.79 / 38.53 | 18.01 / 39.05 | −13.48% / +1.41% |
| TerrainGrid 网格焊点，µs | 139.26 | 136.74 | −1.84% |
| 4,096 活跃键的删插回放，ns/组 | 25.25 | 20.85 | −17.38% |
| 连续 / 打散整数 64K Map 点查，ns | 7.87 / 8.49 | 7.40 / 7.94 | −6.02% / −6.62% |
| 连续 / 打散整数 64K Set 点查，ns | 7.59 / 7.99 | 7.06 / 7.51 | −6.95% / −5.81% |
| Map 第 5 / 第 17 项插入，P95，TSC tick | 315 / 140 | 140 / 350 | −55.56% / +150% |

字段缺失只替换实际成员名的末字节，Runtime 必须返回预期错误码。较大字段表的这个请求在 36/36 轮都更慢，相差约 3.6 ns。这个分类含 5 项及以上的表，计时覆盖完整 Runtime 查询，无法单独归因于外部索引探测。网格计时调用真实 `procedural::import_mesh`，文件读取在计时外。6,144 个顶点生成 1,089 个唯一位置，每次与本端参考逐项比较位置和映射，两端坐标与映射的摘要一致。整数删插是合成负载，每组包括删除、确认缺失、补入一个键，并摊入初始装表与每轮遍历的成本。它与实际求值器验收分别运行。

## 64K 表内存与增长

独立程序记录同一线程上经项目全局 new/delete 分配的 mimalloc 可用块字节，每次销毁后计数必须回零，峰值包含新旧缓冲同时存活的时段。在 12 轮交错对照中，内存数字每轮相同。随后关闭分配记录，分别以连续和打散的键自然增长到 65,536 项。每个进程对 64 次完整建表各取一次最慢插入，再取这 64 次的 P95。这是整次建表最坏停顿的分布，含计时屏障与系统调度干扰。[内存与增长汇总](growth-summary.csv) · [12 轮原始记录](growth-samples.csv.gz)

| 65,536 项 Map | 现有后端 | 自有候选 |
| --- | ---: | ---: |
| 预留后存活 / 峰值，B | 1,900,544 / 1,900,544 | 1,769,472 / 1,769,472 |
| 自然增长存活 / 峰值，B | 2,293,760 / 3,342,336 | 2,162,688 / 3,211,264 |
| 连续键，最慢插入 P95，µs | 153.6 | 148.6 |
| 打散键，最慢插入 P95，µs | 248.0 | 224.4 |

耗时以本机约 3.493 TSC tick/ns 折算。连续键最坏停顿的 P95 候选在 7/12 轮更低，P50 的配对中位数反而增加 10.09%，连续增长的普遍加速尚未证实。一次有界升格也从第 5 项移到了第 17 项，第 17 项的 P95 增加约 60 ns。

## 复现

在包含本工作树改动的独立副本中执行。先保存默认实现的程序，再复制候选头并应用两行入口所需的补丁。旧程序保留其构建时的运行库。两端均需通过 dev 和 release 的 `foundation_acceptance`、`runtime_acceptance`、`source_acceptance`、`world_acceptance`、`procedural_acceptance`。

```powershell
$study = 'docs/research/small-table-experiment'
$output = 'out/owned-index'
$mesh = 'examples/Content/Procedural/Meshes/TerrainGrid.obj'
& "$study/build.ps1" -Label tag4 -OutputDirectory "$output/previous"
& "$study/build.ps1" -Label tag4 -Allocations -OutputDirectory "$output/previous"
foreach ($dll in 'atelier_source_backend.dll','mimalloc.dll','tbb12.dll') {
    Copy-Item "out/release/$dll" "$output/previous"
}
Copy-Item "$study/owned-index/dense_hash_table.hpp" 'modules/core/foundation/include/foundation/detail/'
git apply "$study/owned-index/map-set.patch"
& "$study/build.ps1" -Label owned -OutputDirectory "$output/current"
& "$study/build.ps1" -Label owned -Allocations -OutputDirectory "$output/current"
& "$study/compare.ps1" -OutputDirectory $output -Reference previous/tag4 -FirstCandidate current/owned -SecondCandidate '' -LogicalProcessor 18 -Rounds 18 -Mesh $mesh
$growth = New-Item -ItemType Directory -Force "$output/growth64k"
Copy-Item "$output/previous/tag4-allocations.exe","$output/current/owned-allocations.exe" $growth
& "$study/compare.ps1" -OutputDirectory $growth -Reference tag4-allocations -FirstCandidate owned-allocations -SecondCandidate '' -LogicalProcessor 18 -Rounds 12
```
