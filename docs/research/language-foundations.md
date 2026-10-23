# 第 1 轮语言基础：运算边界调研

2026-10-04；实现基线 `bba1595`。本记录核对官方 LLVM LangRef 与 Microsoft C# 规范，作为现行运算规则的依据；实现行为以 [LANGUAGE.md](../../LANGUAGE.md) 为准。范围为 `bool`、32 位 `int/uint` 运算、四种数值类型的显式转换、标量常量及 `for/break/continue`。

## 布尔表示与短路

LLVM 比较产生 `i1`；Microsoft C++ 的 `bool` 对象占 1 字节（C++ 标准本身不保证这个大小）。建议保留一套语言 `bool` 类型，SSA 用 `i1`，对象、字段和宿主值槽用 `i8`：写入时 `zext` 得到 `0/1`，读取时与 `0` 比较恢复 `i1`；布局仍服从权威类型元数据。[LLVM icmp](https://llvm.org/docs/LangRef.html#icmp-instruction)、[LLVM zext](https://llvm.org/docs/LangRef.html#zext-to-instruction)、[Microsoft C++ 类型大小](https://learn.microsoft.com/en-us/cpp/cpp/fundamental-types-cpp#sizes-of-built-in-types)

C# 将 `bool` 与数值类型分开，没有标准的相互转换；`&&/||` 只在需要时求值右侧。建议本轮沿用该类型约束，用分支和 `phi i1` 实现短路，右侧的调用和执行错误也必须被跳过；`!` 只接受 `bool`。[C# §8.3.9](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/types#839-the-bool-type)、[C# §12.17.2](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#12172-boolean-conditional-logical-operators)

## 整数运算：必须在指令前保护

| 边界 | LLVM 契约 | C# 规范 | 本轮建议 |
| --- | --- | --- | --- |
| 整数 `/` 或 `%` 的右侧为 `0` | UB | 除零异常 | 执行错误 |
| `INT_MIN / -1` | `sdiv` 为 UB | checked 报算术异常；unchecked 可报错或返回 `INT_MIN` | 执行错误 |
| `INT_MIN % -1` | `srem` 同样为 UB，尽管数学余数为 `0` | 当且仅当对应除法会报错时抛溢出异常 | 与除法统一报执行错误 |
| 移位量不在 `0..31` | 三种移位都产生 poison；移位量按无符号解释，所以负数也越界 | `int/uint` 使用 `count & 31`，负数同样被掩码 | 执行错误；这是项目主动选择的语义 |

来源：[LLVM udiv/sdiv](https://llvm.org/docs/LangRef.html#sdiv-instruction)、[LLVM urem/srem](https://llvm.org/docs/LangRef.html#srem-instruction)、[LLVM shifts](https://llvm.org/docs/LangRef.html#shl-instruction)、[C# §12.13.3–4](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#12133-division-operator)、[C# §12.14](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#1214-shift-operators)

建议通过条件分支进入错误路径，只有合法路径生成除余或移位。`int >>` 用算术右移，`uint >>` 用逻辑右移；左移保留 32 位回绕，不添加 `nsw/nuw/exact`。`&/|/^/~` 没有额外数值范围保护；C# 也将整数位操作定义为不溢出。[C# §12.16.2](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#12162-integer-logical-operators)

## 显式数值转换

LLVM `fptosi/fptoui` 向零截断；截断结果不可表示时产生 poison，包括 NaN 和无穷。C# checked 转换会报错，unchecked 则允许不可指定结果。建议本轮统一采用可表示性检查及执行错误。[LLVM fptosi/fptoui](https://llvm.org/docs/LangRef.html#fptosi-to-instruction)、[C# §10.3.2](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/conversions#1032-explicit-numeric-conversions)

对浮点输入 `x`，先按 `double` 精度进行 ordered 范围比较（`float` 可先 `fpext`），再在合法分支转换：

- 到 `int`：`x > -2147483649.0 && x < 2147483648.0`。
- 到 `uint`：`x > -1.0 && x < 4294967296.0`。

这两个条件同时排除 NaN 和无穷，且允许 `-2147483648.5 → int` 和 `-0.5 → uint`。不要用原始值的 `[min,max]` 检查，也不要先转换再检查。以上边界由向零截断规则推导；float 下直接构造 `-2147483649` 会发生边界舍入。

按“数值转整数不可表示则报错”的建议，`int → uint` 检查非负，`uint → int` 检查不超过 `INT_MAX`；两者在 LLVM 中都为 `i32`，范围检查仍然有意义。C# unchecked 的同宽整数转换则按位解释，因此项目若采用该行为，需要作为明确例外记入语言契约。[C# §10.3.2](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/conversions#1032-explicit-numeric-conversions)

`int/uint → float/double` 按源类型选择 signed/unsigned 转换；精度损失允许。`float → double` 可精确扩展，`double → float` 舍入并保留 NaN、无穷语义，过大有限数可变成无穷；本轮无需增加错误保护。[C# §10.2.3](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/conversions#1023-implicit-numeric-conversions)、[C# §10.3.2](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/conversions#1032-explicit-numeric-conversions)

## 常量与循环的最小语义约束

实际采纳时，只有顶层标量/枚举常量必须在编译期求值；局部 `const` 是运行期初始化的只读值，可以接收调用结果及拥有型值，不要求常量表达式。顶层求值复用运行期的运算、转换与短路语义，并把需要执行的非法运算报告为编译错误。现行规则统一见 [LANGUAGE.md](../../LANGUAGE.md#声明与作用域)。C# 常量表达式同样复用运行期规则，但默认整数常量溢出检查更严格；项目保留已选定的 32 位加减乘回绕。[C# §12.26](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#1226-constant-expressions)

`for` 的初始化执行一次，随后依次执行条件、循环体、迭代表达式；省略条件等于持续成立。`continue` 必须经过迭代表达式再回到条件，`break` 进入最近循环的出口。循环外 `break/continue` 应在编译期拒绝。[C# §13.9.4](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/statements#1394-the-for-statement)、[C# §13.10.2–3](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/statements#13102-the-break-statement)
