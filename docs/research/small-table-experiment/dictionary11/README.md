# .NET 11 Dictionary 与原生 Map 点查对照

三种实现读取同一份带预期返回值的二进制轨迹。原生端分别使用 `atelier::Map::find` 和 `ankerl::unordered_dense::map::find`，两者使用相同的项目 Hash/Equal。托管端使用 `Dictionary<ulong,uint>.TryGetValue` 和 `Dictionary<string,uint>.TryGetValue`。三个实现都预留最终元素数，使用非零的 32 位值，缺失返回零。每个程序先逐项核对结果，每次回放再核对返回值之和。

## 输入与计时

- 整数表含 4、1,024 或 65,536 个键。连续键为 `[0, N)`，缺失键为 `[N, 2N)`。打散组对这两段键应用同一可逆 64 位混合函数。两组共用相同的随机下标和命中顺序。每组 32,768 个请求，分别测全部命中、全部缺失和恰好一半命中。
- 原生程序实际注册 world 与 presentation 类型并导出成员。选出 19 个非空小字段表的 48 个名字和 12 个非空小方法表的 33 个名字。随机抽取 4,096 个请求，缺失请求替换最后一个字符且保持长度。原生端使用借用键 `StringView`，托管端使用 `StringComparer.Ordinal`。本样本名字均为 ASCII，两端分别使用 UTF-8 和 UTF-16，从轨迹分别读取入表和查询用的名字。
- 每个进程把查询线程固定到逻辑处理器 18，后台线程使用系统调度。三端先完整回放全部场景，再在同一进程重新建表并采样。每个场景先预热至少 150 ms，再计 32 遍查询。计时覆盖键读取、选择成员表、查询和返回值累加。进程启动、轨迹读取、建表和预热在计时区间之外。C# 使用 Release 与默认分层 JIT，记录计时期间的 GC 次数并检查查询线程分配为零。
- 每轮三个进程串行执行，六种先后排列各用两次。汇总给出各自的 12 轮中位数，耗时变化率另按轮次配对取中位数。独立的容量对照只比较 .NET 的两种预留量，六轮内先后手各占三次。

## 两种键分布对 .NET 意味着什么

.NET 11 的 `Dictionary` 用一个整数数组保存桶头，另一个连续数组保存条目，每个条目含哈希值、下一个条目的下标、键和值。64 位平台使用乘法辅助的 `FastMod` 定位质数桶。默认的 `ulong` 哈希将高低两个 32 位字异或。对本实验的连续键，它等于原键，65,536 个已插入键各占一个桶。打散后的键保留唯一性，但可能落在同一个桶。[Dictionary 源码](https://github.com/dotnet/runtime/blob/v11.0.0/src/libraries/System.Private.CoreLib/src/System/Collections/Generic/Dictionary.cs) · [UInt64 哈希](https://github.com/dotnet/runtime/blob/v11.0.0/src/libraries/System.Private.CoreLib/src/System/UInt64.cs) · [桶容量选择](https://github.com/dotnet/runtime/blob/v11.0.0/src/libraries/System.Private.CoreLib/src/System/Collections/HashHelpers.cs)

按源码的入桶与链首插入规则离线重建索引，计算每次查询实际需要查看的条目数。下列数字是从同一轨迹计算的访问次数，不是硬件性能计数器。容量变化还会改变缓存占用，因此条目数只能解释部分耗时变化。

| 65,536 项，每次查询平均查看的条目数 | 75,431 桶 | 130,363 桶 |
| --- | ---: | ---: |
| 连续整数，命中 / 缺失 | 1.000 / 0.848 | 1.000 / 0.011 |
| 打散整数，命中 / 缺失 | 1.436 / 0.873 | 1.253 / 0.512 |

## 复现

在仓库根目录用 PowerShell 运行。前两条编译实验，后两条是三端对照和托管容量对照。需要项目锁定的原生工具链、Python 3、.NET 11 SDK 和 x64 运行时 11.0.0。本目录只用于研究，不参与引擎构建。

```powershell
$study = 'docs/research/small-table-experiment'
$output = 'out/dictionary11-steady'
& "$study/build.ps1" -Label dictionary-lookup -OutputDirectory $output
dotnet build "$study/dictionary11/Lookup.csproj" -c Release --artifacts-path "$output/artifacts" -o "$output/managed"
python "$study/dictionary11/compare.py" --out $output --cpu 18 --rounds 12
python "$study/dictionary11/compare.py" --out $output --cpu 18 --capacity-control --rounds 6
```
