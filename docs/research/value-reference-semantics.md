# AngelScript 与 C#：值、句柄、赋值与身份的事实核查

核查日期：2026-09-21。范围仅限 AngelScript 官方手册与 Microsoft Learn；未安装或运行任何运行时。这里的“复制”只描述赋值语义，不推断具体 ABI、分配器、GC 配置或对象布局。

## AngelScript（官方手册）

1. **脚本 `class` 是引用类型，但变量形式可不同。** 所有脚本声明的类都是引用类型；普通 `T value` 表示一个对象变量，`T@ handle` 是可为 `null`、可指向对象的句柄。并非所有宿主注册对象类型都允许句柄，是否允许由宿主注册决定。[对象与句柄](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_datatypes_obj.html)；[对象句柄](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_handle.html)

2. **`=` 操作对象内容，句柄重绑定须显式操作句柄。** 对 `T@` 使用普通 `=` 时，已注册的 `=`（或脚本类的 `opAssign`）作用于句柄所指向的对象；它不是把左句柄改指向右对象。手册给出的重绑定句法是 `@left = @right`（常被简称为“`@=` handle assignment”）；它让两个句柄指向同一对象。若脚本类未自定义 `opAssign`，编译器生成的版本逐成员复制，因此不能把值赋值概括为“完整深拷贝”：成员自身的赋值/句柄语义仍然决定结果。[对象句柄的两种赋值](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_handle.html)；[自动生成 `opAssign`](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_class_ops.html)

3. **身份和生命周期是独立概念。** `is` / `!is` 用于引用类型的地址（同一对象）比较，区别于 `==` / `!=` 的值比较/对象比较运算。强对象句柄会使对象存活到所有句柄释放为止；手册也允许宿主把引用类型注册为 `asOBJ_NOCOUNT`，因此“引用”本身不能推出必用 GC、必用引用计数或每个对象都由语言独立堆分配。[身份与句柄生命周期](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_handle.html)；[身份运算符](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_expressions.html)；[无引用计数的引用类型](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_reg_basicref.html)

## C#（Microsoft Learn）

1. **`struct` 与 `class` 的变量表示和 `=` 不同。** C# 规范规定值类型变量直接包含数据，赋值创建该值的副本；引用类型变量保存对象引用，赋值复制引用而不复制被标识的对象。`struct` 是值类型，`class` 是引用类型。[C# 规范：类型](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/types)；[C# 类](https://learn.microsoft.com/en-us/dotnet/csharp/fundamentals/types/classes)

2. **struct 的“复制”不是递归深拷贝。** `struct` 赋值会复制整个结构体值；若其字段本身是引用类型，字段值就是引用，因而复制后的两个 struct 仍可指向同一个字段对象。这是“值类型赋值复制值”与“引用类型赋值复制引用”的直接组合，不是复制被引用对象的保证。[C# 规范：值类型赋值](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/types)

3. **身份比较另有明确 API，`==` 不能脱离类型的定义解释。** 默认普通 class 的 `==` / `Equals` 采用引用相等，但类型可以重写/重载等值语义；`object.ReferenceEquals` 始终检验对象身份。普通 struct 没有预定义的 `==`，而 `Equals` 表达其值相等规则。[C# 相等比较](https://learn.microsoft.com/en-us/dotnet/csharp/fundamentals/expressions/equality)

## 对照与未证实边界

两者都将“变量中的值”和“对象身份”分开：AngelScript 通过显式 `@` 句柄与 `is` 把重绑定、对象内容赋值、身份比较分开；C# 由 `struct`/`class` 的类型类别决定普通赋值复制值还是复制引用，并提供 `ReferenceEquals` 作身份检查。

本核查未证实 AngelScript 在任意宿主配置下对脚本类值赋值的全部递归复制细节、循环回收的触发时机或内存布局；也未讨论 C# 的装箱、`ref` 参数、record、互操作封送或运行时 GC 实现。它们不应由上述赋值规则外推。
