# Foundation 哈希选型与 Renderer 顶点焊接

Foundation 持有进程内哈希实现，沿用 unordered_dense v5.3.0 的数值映射。Renderer 用同一个字节入口计算 48 B 顶点的哈希，使用 `Set<uint32_t, Hash, Equal>` 存唯一顶点下标，并按顶点内容异构查询。此阶段的大表索引由 ankerl 提供。[后续退役对照](../small-table-experiment/retirement/README.md) · [Foundation 实现](../../../modules/core/foundation/include/foundation/detail/byte_hash.hpp) · [公共入口](../../../modules/core/foundation/include/foundation/hash.hpp) · [Renderer 焊接](../../../modules/render/renderer/src/renderer.cpp)

## 成熟实现中值得沿用的部分

| 来源 | 与本工程直接相关的设计 |
| --- | --- |
| wyhash final4 | 64×64 位乘积折叠高低半部，短键只做长度允许的读取，长键分道混合。上游以 Unlicense 发布。对照使用固定提交、默认常量和 seed 0。[固定版本](https://github.com/wangyi-fudan/wyhash/tree/ea3b25e1aef55d90f707c3a292eeb9162e2615d8) |
| XXH3-64 | 按输入长度选择路径，是比较短名和定长块的独立参考。对照使用 v0.8.3 的 `XXH3_64bits`，库代码为 BSD-2-Clause。[上游版本](https://github.com/Cyan4973/xxHash/tree/v0.8.3) |
| unordered_dense v5.3.0 | 字节算法派生自 wyhash。17–144 B 的块独立乘混后合并，覆盖 Renderer 的 48 B 顶点。沿用其映射可以保持既有分桶和指纹分布。已把所需逻辑适配到项目的 Windows x64 实现，并保存原 MIT 声明。[上游源文件](https://github.com/martinus/unordered_dense/blob/v5.3.0/include/ankerl/unordered_dense.h) · [保留的许可](../../../third_party/unordered_dense/LICENSE) |
| Boost.ContainerHash | 借鉴哈希是否已充分混合的类型标记。Foundation 识别 `void`、`std::true_type` 和 `std::false_type`。TypeId、ObjectId 和精确几何复合键继续按既有字段顺序混合。[Boost 的标记契约](https://www.boost.org/doc/libs/latest/libs/container_hash/doc/html/hash.html#refhash_is_avalanching) |

## Ryzen 9 3950X 上的 Release 对照

固定逻辑处理器 18、clang-cl 23.1.5。名字来自 world 与 presentation 注册的 176 条字段/方法记录，固定种子抽 4,096 次。顶点来自 TerrainGrid.obj 导入的 6,144 个顶点。单独算哈希的数字包含读取键与累加返回值，按六种测量项轮换进程顺序并交替反序，取 24 轮中位数。

| 字节哈希，ns/次 | 成员名 | 48 B 顶点 |
| --- | ---: | ---: |
| 原 ankerl | 4.10 | 2.14 |
| wyhash final4 上游实现 | 2.16 | 2.04 |
| XXH3-64 v0.8.3 | 4.44 | 4.35 |
| Foundation 兼容实现 | 3.08 | 2.11 |

另外先将逐项对齐上游的 wyhash final4 接入 Foundation，在相同 Map/Set 后端与已迁移的 Renderer 上运行。与当轮原版基线相比，小字段表命中耗时的配对中位数增加 24.40%，Renderer 焊接增加 19.41%。独立计算的吞吐包含不同于实际查表的指令依赖；这组对照本身不能把退化分摊到计算延迟、指纹分布等单个原因。[final4 字节与焊接汇总](final4-hash-summary.csv) · [final4 Runtime 汇总](final4-runtime-summary.csv)

采用兼容映射后，实际 Renderer 焊接中位数从 107.02 µs 变为 106.57 µs，配对变化 −0.38%，按持平解读。24 轮中两端的顶点、索引和面数据 SHA-256 始终相同。在独立的 18 轮完整 Runtime 对照中，注册从 120.28 变为 119.18 µs，注册后存活分配同为 240,336 B。小字段表命中从 18.71 变为 17.95 ns，较大字段表同长度缺失从 48.07 变为 46.96 ns，小方法表命中从 20.87 变为 19.50 ns；小方法表缺失约 38.3 ns。这里测量的是当前四指纹后端。此前自研大表的 +2.02% 是另一组索引实验。[哈希与 Renderer 汇总](hash-summary.csv) · [Runtime 汇总](runtime-summary.csv) · [原始采样](samples.csv.gz) · [Runtime 原始采样](runtime-samples.csv.gz)

## 兼容与验收

- 对 0–4,096 字节、16 种起始偏移的 65,552 个输入，核对 `hash_bytes` 和 `Hash<StringView>` 的 64 位结果与固定原版相等，包含空视图及含零字节数据。用 66,560 组有符号整数和复合字段核对整数与有序字段混合结果。哈希值继续只用于进程内索引。
- Renderer 对每个真实输入顶点，按完整对象字节独立计算首次出现的下标，逐项验证焊接结果。另用只差 UV 有符号零、切线手性或完全相同的顶点验收，期望映射为 `0,1,2,1,2,0`，并验证取消处理。带绑定的 Set 覆盖复制、移动、移动源复用和 release 后复用。
- 移除 ankerl 的 include 路径后，Foundation 哈希和研究中的自有索引可独立编译、链接、运行。JIT 的重复直接链接已经移除；当时的 Foundation 大表仍使用该包。后续切换保留了字节哈希的原作者许可。

## 复现

在迁移前后各自的工作树中用 `build.ps1 -Label before` 和 `build.ps1 -Label after` 构建同一研究程序，汇集两份可执行文件到 `out/foundation-hash`。构建脚本只将上游对照头下载到实验输出目录。成员列表用先前 [Runtime 导出程序](../small-table-experiment/dictionary11/lookup.cpp) 的 `members` 模式生成，保存为该输出目录下的 `members.tsv`。从仓库根目录执行以下命令。

```powershell
$env:PATH = "$(Resolve-Path out/release);$env:PATH"
python docs/research/foundation-hash/compare.py
& out/foundation-hash/after.exe examples/Content/Procedural/Meshes/TerrainGrid.obj out/foundation-hash/members.tsv verify
```

完整 Runtime 的两个构建都使用 [现有 profile 构建脚本](../small-table-experiment/build.ps1) 的 `-Label tag4`，分别输出到 `out/foundation-hash/baseline` 和 `out/foundation-hash/current` 并保留对应运行库。用 [同轮对照脚本](../small-table-experiment/compare.ps1) 指定这两份程序、逻辑处理器 18 和 TerrainGrid 输入复测。
