# Foundation Map/Set 后端接入

默认 Map/Set 使用自有分组索引。对照双方调用同一份 Foundation Hash/Equal。46 个真实 Runtime 类型的注册基本持平，原先慢约 2% 的较大字段表同长度缺失在两批回放中分别快 2.57% 和 2.05%。[生产实现](../../../../modules/core/foundation/include/foundation/detail/dense_hash_table.hpp) · [使用合同](../../../foundation.md#mapset-与哈希)

## 字段与注册

Ryzen 9 3950X、Windows x64、clang-cl 23.1.5 Release、mimalloc。两个后端分别编译 Runtime、Procedural 和 Renderer，保存各自的运行库。进程固定在逻辑处理器 18，轮内轮换执行顺序。注册 World 与 Presentation 的真实类型，再通过公共 Runtime 接口反复回放 4,096 个请求，核对成员或错误码。查询分布来自注册目录和固定随机种子。下表列各端中位数，百分比先在同轮配对再取中位数。[全部汇总](summary.csv) · [逐轮样本](samples.csv.gz)

| 完整负载 | ankerl | Foundation | 配对变化 |
| --- | ---: | ---: | ---: |
| 注册 46 个类型，µs | 118.85 | 119.10 | +0.16% |
| 5 项以上字段表，命中，ns | 21.71 | 20.58 | −5.38% |
| 同长度缺失，改末字节，ns | 48.20 | 46.55 | −2.57% |
| 同长度缺失，改首字节，ns | 45.11 | 44.89 | −0.45% |
| 查询来自其他类型的真实成员名，ns | 46.94 | 46.87 | −0.13% |
| 小方法表，同长度缺失，ns | 38.27 | 38.36 | +0.33% |
| 64K 整数 Map / Set 混合点查，ns | 7.88 / 7.57 | 7.39 / 6.96 | −5.93% / −7.97% |
| Procedural 导入 TerrainGrid，µs | 137.26 | 135.34 | −1.31% |

表中来自 24 轮的 `length` 批次。再用增加首字节反例之前的原始驱动独立跑 30 对进程，注册 +0.12%，较大字段命中 −4.78%，末字节缺失 −2.05%（28/30 轮更快），异表真实名字缺失 +0.37%（46.82 → 46.91 ns）。小方法表同长度缺失有可测代价，这一批为 38.05 → 38.90 ns，+2.21%。两批注册完成后都少用 4 块、2,240 B。Renderer 的实际顶点焊接另跑 18 对进程，105.90 → 99.86 µs，−5.70%。逐顶点校验和输出 SHA-256 均相同。[Renderer 样本](renderer.csv)

内联表仍容纳 16 项。仅使用默认 Hash/Equal 的借用名字表把空闲的组掩码用于记录长度模 32 的位集合，可在读取字符前排除长度不符的查询。指纹用 Foundation 整数混合器处理首字节、末两字节和长度，完整判等决定命中。第 17 项或提前预留后使用完整 Foundation 哈希。同一个掩码字恢复为目录掩码。自定义策略沿用调用方定义的哈希与比较。

单取末字节在最初的末字节缺失工况中接近持平，但较大字段命中慢 30.87%。只混合长度和末两字节时，改首字节的缺失慢 10.31%。将借用名字表提前到第 5 项建立索引也未解决问题，在该批中异表名字缺失慢 2.39%。最终同轮的消融对照为 `length/owned` 与 `mixed/owned`，后者只去掉长度位集合。两个后端都对真实异表名字和两个方向的单字节变更做了回放。

## 自然增长

外部每组有 15 个下标，平均每组达到 10 项后翻倍。专项用连续键和固定奇数打散的 64 位键自然插入至 65,536 项。每个进程建 64 张表，逐次取 TSC 差，先取每张表最慢的一次，再取这 64 个最大值的 P95。下表是 30 对独立进程的中位数，本机约 3.493 TSC tick/ns。固定扩容位次也单独留样，以区分普通插入、索引重建及其他停顿。

| 自然插入 | ankerl | Foundation | 配对变化 |
| --- | ---: | ---: | ---: |
| 第 5 项，P95 | 315 tick | 140 tick | −55.56% |
| 第 17 项，P95 | 140 tick | 350 tick | +150%，约多 60 ns |
| 连续键，64K 最慢插入 P95 | 152.1 µs | 129.8 µs | −11.94% |
| 打散键，64K 最慢插入 P95 | 234.0 µs | 150.9 µs | −35.25% |

最后一次索引重建分别发生在第 52,429 项和第 40,961 项。该次连续键插入的 P95 分别约 147.8 和 129.8 µs，打散键分别约 233.7 和 150.9 µs。在 65,536 项时，自然增长后的存活堆内存为 2,240 → 2,112 KiB，峰值为 3,264 → 3,136 KiB，预留后为 1,856 → 1,728 KiB。在 81,921 项提前扩容的边界，预留占用为 2,112 → 2,624 KiB，多 512 KiB。数值来自分配器可用块大小，不含对象本身。默认对象 Release 64 B、dev 72 B。

## 干净构建与最终复测

移开已有两个预设目录，从空目录执行 dev 和 release 配置、全量编译。在两种配置内分别执行全部 15 项无窗口和 5 项窗口验收，40/40 项通过。Release 的 RHI 和三个 Renderer 场景使用 GPU-based validation。两个生成目录的 CMake 缓存、Ninja 规则和编译命令均已脱离 ankerl 包。随构建分发的字节哈希许可与仓库中的 MIT 原文一致。

最后将相同驱动链接到干净 release 的产物，和保留原运行库的 ankerl 程序再交替跑 30 轮。较大字段表末字节缺失 48.65 → 46.80 ns（配对 −3.63%），首字节缺失 45.44 → 45.28 ns（−0.25%），异表成员名 47.30 → 47.14 ns（−0.26%），命中 21.85 → 20.67 ns（−5.41%）。注册 120.00 → 120.54 µs（+0.36%）。64K 连续键最慢插入 P95 为 156.5 → 137.9 µs（−11.67%），打散键 255.0 → 160.9 µs（−35.40%）。单独的 Renderer 焊接 18 轮配对 −5.18%，输出摘要与切换前一致。逐轮数据对应 `clean-runtime` 和 `clean-growth`，Renderer 样本对应 `clean`。

## 复现

在两个包含本次改动的独立源码副本中配置 Release。在对照副本先应用[历史后端补丁](ankerl-reference.patch)，再配置构建。补丁恢复 v5.3.0 的下载锁、局部补丁和原默认容器接线。两个副本使用同一份 Foundation 哈希。分别使用[基准构建脚本](../build.ps1) 的 `-Label tag4` 和 `-Label owned`，再各自带 `-Allocations` 构建增长程序。将程序与各自配置内的 `atelier_source_backend.dll`、`mimalloc.dll`、`tbb12.dll` 放在 `out/replay/base` 与 `out/replay/native`。以下命令从源码根目录运行。

```powershell
$driver = 'docs/research/small-table-experiment/compare.ps1'
$mesh = 'examples/Content/Procedural/Meshes/TerrainGrid.obj'
& $driver -OutputDirectory out/replay -Reference base/tag4 -FirstCandidate native/owned -SecondCandidate '' -LogicalProcessor 18 -Rounds 30 -Mesh $mesh
& $driver -OutputDirectory out/replay -Reference base/tag4-allocations -FirstCandidate native/owned-allocations -SecondCandidate '' -LogicalProcessor 18 -Rounds 30
```

脚本为每次调用写入同名汇总，保留第一组结果后再跑第二组。生产构建随附 Foundation 字节哈希使用的 [MIT 许可](../../../../third_party/unordered_dense/LICENSE)。
