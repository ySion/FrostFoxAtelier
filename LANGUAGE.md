# FrostFoxAtelier 当前源码语言规范

状态：记录当前编译器已经实现的单入口子集；尚未冻结为稳定语言版本。编译器只有一种语言模式，没有版本选择开关。本文整合原 v0.0.1～v0.0.3 的有效规则，旧增量文本保留在 Git 历史中。

本文是现行词法、语法和源码执行语义的唯一入口。[语言设计契约](docs/language-contract.md)记录更长期的方向与待定问题，[Runtime 协议](docs/runtime-model.md)负责公共宿主、对象及代码生命周期，[README](README.md)负责构建和验收。长期设计目标不自动成为已实现语法。

## 能力范围

每份源码是一个包含枚举/结构体声明、顶层标量/枚举常量和一至多个函数的模块，由宿主选定一个公开入口。函数可接受 bool/float/double/uint/int 或已注册枚举参数，返回 void 或同范围值类型；同步函数还支持内建数学值和 plain record 的按值参数与结果、原生或描述型记录和 Array 的借用、只读 String，以及具备生命周期能力的拥有型局部。`async` 函数可以用 `yield;` 挂起。语句包括声明、赋值、同模块函数调用、原生成员调用、块、`if/else`、`while`、`for`、`break/continue` 和 `return`。

对象环境至多包含一个宿主已注册的绑定对象类型，目录包含其继承的公开成员。接收者可为该类型的派生实例；源码的声明视图仍是绑定类型。源码可使用该视图的持有/观察局部、`null`、`new`、身份比较和已绑定的标量/数学字段及原生方法。原生与描述型对象沿用同一 Runtime 类型和身份。

尚未支持：源码 class、源码结构体中的对象引用、以源码结构体为元素的新数组类型、跨文件声明收集、递归调用、内部 async 调用、多对象类型目录、对象引用字段、对象引用参数与返回、const 对象引用局部、String/Array/资源记录的按值参数与返回、async 聚合参数与结果、借用返回与借用局部声明、源码显式销毁、通用泛型、语言异常捕获、`await`、跨线程恢复、一般方法分派、类型或活动帧迁移，以及 `+=` 等复合赋值、`++/--`、`foreach`、`switch`、`?:` 和其他整数宽度。本文不把这些边界转成必须立即实施的计划。

## 源码与词法

编译接口接收源名和显式长度的源码字节。源名用于诊断；文件读取由宿主负责，`.source` 只是样例扩展名。推荐 UTF-8 无 BOM；当前不剥离 BOM，不做 Unicode 规范化，标识符仅接受 ASCII。

- 标识符：`[A-Za-z_][A-Za-z0-9_]*`，区分大小写，没有转义标识符。
- 空白：空格、TAB、CR、LF、VT、FF；空白或注释不能拼接一个记号。
- `//` 到 LF 或输入末尾结束；CR 本身不结束行注释。`/* ... */` 到第一个 `*/` 结束，可以跨行、不嵌套；缺少结束符报错。
- NUL 不结束输入；注释中的 NUL 按字节跳过，注释外拒绝。
- 行列从 1 开始、字节偏移从 0 开始；列按字节计数，TAB 增加一列。只有 LF 换行，CRLF 中由 LF 完成换行。

保留字不能用于函数、参数、局部、类型名段或成员路径段：

```text
void bool true false float double uint int const if else while for break continue return self weak null new observe try_acquire async yield enum struct
```

`class` 等不在上述集合中的名字没有内建含义；符合标识符规则时可作为普通名字，不代表已实现同名特性。

记号包括 `+ - * / % = == != < <= > >= << >> ! && || & | ^ ~ ( ) { } [ ] ; , . : ::`。`>>` 在表达式中表示右移，也可在类型上下文中结束嵌套 `Array<Array<T>>`。相邻 `++/--` 明确拒绝，不再解释为连续的一元运算；用空白或括号分开的 `+ +value`、`- -value` 仍按一元运算检查。复合赋值和自增自减没有语法。

## 类型与数值

| 类型 | 规则 |
| --- | --- |
| `bool` | true/false，对应 core.bool；布局大小及对齐为 1 字节，默认值为 false |
| `uint` | 32 位无符号整数，范围 0～4294967295 |
| `int` | 32 位有符号整数，对应 core.i32 和当前目标的 C++ int |
| `float` | 32 位浮点数 |
| `double` | 64 位浮点数，对应 core.f64 |
| 内建数学值 | float/double 向量、四元数、矩阵；支持同步按值参数与结果、借用、局部及整值赋值，详见[数学值](docs/math.md) |
| 已注册枚举 | 独立类型身份，底层为 i32/u32；使用完整 Runtime 类型名 |
| 记录/结构体 | 同一 Runtime 中的原生或描述型无 GC 引用值；同步借用，具备保持借用有效的复制与生命周期能力时可作拥有型局部 |
| `Array<T>` | 同一 Runtime 注册的具体原生数组；同步借用或拥有型局部 |
| `String` | 已注册的引擎 UTF-8 String；只读借用或拥有型局部，长度和索引按字节 |
| `void` | 函数/方法无结果，不能用于值上下文 |
| 宿主对象类型 | 持有引用，允许为空；`weak` 表达观察引用 |

已定型值严格匹配类型：bool 与数值、整数与浮点、float 与 double、不同数学精度、不同枚举、枚举与底层整数不隐式转换。`int(value)`、`uint(value)`、`float(value)`、`double(value)` 显式转换 float/double/int/uint；bool 和枚举不参与数值转换，也没有通用类型转换语法。非负十进制整数常量默认保持既有 uint 语义；int 上下文可将所有叶字面量均不超过 2147483647 的未定型纯整数常量表达式定型为 int。具有显式类型的模块常量保留声明类型，不参与这一定型便利规则。

数值转整数先向零截断，再检查目标范围；NaN、无穷或截断结果不可表示时报告执行错误。`int → uint` 拒绝负数，`uint → int` 拒绝大于 2147483647 的值，不按位重解释；同类型转换保持值。整数转浮点允许舍入，float 转 double 精确扩展，double 转 float 可舍入为同符号零或无穷，并保留 NaN 的数值语义。

### 参数传递与枚举

标量（含 bool）/枚举、内建数学及 plain record 参数在同步函数和原生成员中支持 `T`、`const T&`、`T&`。按值参数有独立存储；只读引用可绑定同类型左值或临时值；可写引用必须绑定可写局部、参数或对象内联字段。调用语法不增加 ref/out 标记。只读参数和 const 局部不可赋值或转交可写引用，但只读参数能观察其他别名对同一存储的更新。

同一存储重复传参保持别名，包括只读与可写参数混用，以及参数与接收者字段重合。借用只覆盖同步调用；保存借用、借用返回、借用局部声明和 async 引用参数尚不支持。async 体可在一次同步原生调用期间借用帧内局部，借用不跨 yield。

枚举使用 `namespace.Mode::item` 常量，可声明局部、传参、返回、读写字段及比较同类型枚举的 `==/!=`。重复数值的名字保留为别名；宿主提供的未命名底层值仍有效。源码没有整数与枚举转换或 flags 运算。

### 原生值借用

同步源码入口可声明 `Type& name` 或 `const Type& name`；`const` 必须位于类型前。Type 可为已注册、不含 GC 引用的原生/描述型记录或原生数组。`String` 只支持 `const String&`。这些参数使用调用方已有的存储，不复制元素，也不延长拥有者的生命周期。

- 一般记录使用完整 Runtime 名称；内建数学类型同时接受[固定短名](docs/math.md#类型名称与存储)。`Array<T>` 递归解析为同一 Runtime 中已注册的具体数组类型，也可直接使用该数组的注册名；它不自动实例化或注册新类型。`String` 是已注册引擎字符串的便利拼写，也接受其注册名。
- 数组支持 `values.size()` 与 `values[index]`；元素可以是已支持的标量、枚举、无 GC 引用记录、String 或嵌套数组。记录通过 `value.field` 访问已发布直接偏移的公开字段；只有访问器的字段不能直接使用。
- String 支持 `text.size()` 与只读 `text[index]`。长度不含末尾零字符，索引返回 0～255 的 `uint` 字节，不解码 Unicode 码点，也不提供可写字节地址。
- `.size()` 返回 `uint`；索引接受 `uint` 或 `int`，在实际访问前检查负数和越界。长度超过 uint 范围或缓冲布局不合法时执行失败，不截断。空序列的长度查询合法，任何元素访问均越界。
- 只读性沿记录字段与数组元素向下传播。可写借用允许修改标量/枚举叶值和整体数学值，不允许替换一般记录/容器、改变序列结构或修改 String。只读和可写参数可以指向同一存储，读取会观察已经发生的别名写入。

含聚合借用参数、普通 plain record 按值参数/结果、一般记录/String/Array 拥有型局部或 String 字面量的函数形成**同步值区间**：允许标量/数学计算、局部、控制流和上述值访问；原生方法仅允许调用 `self` 上声明 `BorrowSafe` 的方法。其他对象操作、async 与 yield 被拒绝。数学按值参数和局部自身不产生该限制，数学引用参数仍产生。函数使用这些值存储的效果沿调用图传播；区间内包括所有被调用的同步辅助函数，不运行入口/回边/结束 GC 回调；返回宿主后由宿主继续推进收集。调用方须保证存储已构造且持续有效，不销毁、重分配或并发修改其结构。没有执行超时保证，无限循环也会推迟宿主重新取得控制。

plain record 可以在同步源码与原生调用中按值传入和返回，借用不能保存或逃逸。Runtime 的 `plain_value` 证明记录只递归包含标量、枚举、数学值和 plain record，并具备平凡复制资格；`can_own` 独立证明现有构造、复制和销毁操作保持借用安全。普通记录跨源码按值边界必须同时满足两项，签名不满足时在编译期拒绝。String、Array、对象引用和资源记录不能按值跨越这些边界。

普通记录按值参数是可重赋值、可修改叶字段的独立副本。内部 JIT 在调用方创建独立参数快照，被调函数直接操作该存储，无二次复制；快照在调用方完整表达式结束时释放，不能逃逸。返回临时支持复制赋值、字段读取、按值传递与 `const T&` 借用，存活到完整表达式结束；临时及其字段不能绑定可写引用。内建数学值继续使用既有定型计算路径。String/Array 拥有型局部遵循[拥有值规则](#拥有型-stringarray-与原生算法)。错误保留此前完成的写入，成功结果仍只在整次执行完成后提交。公共调用链、布局来源和验收见[原生值借用](docs/native-value-borrows.md)。

### 字面量

- `true/false` 为 bool 字面量，可以用于局部、参数、返回和条件；它们不是整数 1/0。
- `uint` 只接受无后缀十进制数字；`007` 为十进制 7。超范围字面量拒绝，不先回绕。没有进制前缀或分隔符。`-1` 定型为 int；`-2147483648` 合法，超出有符号范围的负字面量拒绝。uint 变量仍不能应用一元负号。
- 浮点字面量必须以数字开头。无后缀且包含小数点或指数时为 float；`f/F` 显式指定 float，`d/D` 指定 double。`1.0`、`1.`、`1e2`、`1E-2`、`1.e+2`、`1f`、`1.0d` 合法；`.5` 和缺少指数数字的 `1e+` 不合法。
- 浮点字面量由 `std::from_chars` 完整转换，结果必须有限；超范围或下溢范围错误拒绝。`NaN/Infinity/inf` 没有字面量含义，宿主输入和运行中的运算仍可携带非有限值。

### 运算和求值

运算符优先级与 C 的对应运算一致，从高到低为：

| 优先级 | 运算 |
| --- | --- |
| 最高 | 主表达式、成员与索引后缀 |
| 一元 | `+ - ! ~` |
| 乘除余 | `* / %` |
| 加减 | `+ -` |
| 移位 | `<< >>` |
| 关系 | `< <= > >=` |
| 相等 | `== !=` |
| 按位与 | `&` |
| 按位异或 | `^` |
| 按位或 | `\|` |
| 逻辑与 | `&&` |
| 最低 | `\|\|` |

一元右结合，二元左结合。括号改变组合，不改变类型。

- `uint` 的加减乘按模 `2^32` 回绕；比较按无符号值，一元 `+` 保持值，没有一元负号。`4294967295 + 1` 合法并为 0，`4294967296` 是错误字面量。
- `int` 加减乘和取负按 32 位补码回绕，比较按有符号值；生成代码不附加 signed-overflow 未定义行为假设。
- `int/uint` 支持 `/`、`%`；有符号除法向零截断，非零余数与被除数同号。除数为零以及 `INT_MIN / -1`、`INT_MIN % -1` 均报告执行错误，未定义的机器运算不会执行。
- `int/uint` 支持 `&/|/^/~`；二元按位运算要求两侧同类型。移位量可为 int 或 uint，必须在 0～31，负数或超宽计数报告执行错误，不掩码。左移按 32 位回绕；int 右移补符号位，uint 右移补零。
- bool 只支持 `!`、短路 `&&/||` 和同类型 `==/!=`，没有排序、位操作或算术。`&&` 仅在左侧为 true 时求右侧，`||` 仅在左侧为 false 时求右侧；被跳过的表达式不调用函数，也不产生执行错误。
- `float/double` 加减乘除及取负不启用 fast-math 假设。一元负号翻转符号，保留负零；正负零数值相等。`==/< /<=/>/>=` 遇 NaN 为假，`!=` 遇 NaN 为真。运算可产生无穷或 NaN；没有饱和或浮点异常处理语法，不承诺额外的跨平台 NaN payload/次正规数行为。
- 向量、四元数、矩阵构造与运算，以及 `math.*` 内建函数，以[数学值规范](docs/math.md#源码运算)为准；源码不提供通用运算符重载。
- 比较结果是 bool，可以存储、传参、返回、做逻辑运算及比较相等；`(a < b) == (c < d)` 合法，`a < b < c` 仍因 bool 不支持排序而拒绝。条件只接受 bool，不做数值或引用真值转换。
- 二元操作从左到右求值；成员接收者先于实参，实参从左到右求值。字段或元素赋值先求目标地址（包括接收者和索引），再求右侧，并保持所需存储到写入完成。嵌套调用不能覆盖外层已求出的参数。

### 拥有型 String/Array 与原生算法

```cpp
uint process(const Array<float>& input) {
    Array<float> values = input;
    values.reserve(128);
    values.add(1.0);
    String label = "batch/火花";
    label.append("/ready");
    return self.process(values, label);
}
```

- 类型须已注册，且其值操作已声明保持其他借用有效。String、平凡值元素与满足此条件的嵌套 Array 自动组合；非平凡记录元素须由可信原生生产者明确提供保证。
- 局部对象按真实大小和对齐放在生成函数的栈上，缓冲由同一基础库持有。初始化/赋值使用 Runtime 值操作和深复制语义。不提供隐式移动或聚合值返回。
- Array 支持 `reserve(count)`、`resize(count)`、`clear()`、`add(element)`、`append(array)`；String 支持 `reserve(count)`、`clear()`、`append(string)`。修改目标必须从当前函数拥有的局部出发，可以是其嵌套序列；外部借用参数不能改变结构。容量参数为 int/uint，负数报告执行错误；这些修改操作返回 void。整值赋值开放给拥有型局部及其嵌套记录、String/Array 字段或元素，目标类型须具备相同的安全值操作能力；复制后的缓冲独立。
- 字符串用双引号，支持 UTF-8 及 `\\`、`\"`、`\n`、`\r`、`\t`、`\0`；拒绝未知转义、未闭合字面量和直接换行。构造/修改沿基础 String 验证 UTF-8，非法字节在执行时失败。字面量可作为 `const String&` 实参，也可读取长度和字节。
- 正常块退出、循环迭代、break/continue、提前返回和错误路径逆序销毁已构造的值；字面量临时对象在当前语句或条件结束时清理。复制部分失败由原生构造操作撤销已构造元素，生成代码不重复析构未完成对象。
- `AFUNCTION(BorrowSafe)` 或 `NativeRegistration::method(name, true)` 是可信 C++ 契约：调用及其间接调用不得使仍使用的借用失效、保存借用、重入源码或挂起。它可以修改可写标量/记录叶值，不能通过容器重分配、清空或替换破坏既存别名。编译器读取同一 MethodInfo 的声明，Runtime 继续保护 GC 和对象生命周期；该标记不是对任意 C++ 函数的自动证明。

这些方法沿既有 Runtime 调用入口接受无 GC 引用的原生/描述型记录、Array 和 String 引用，仍需遵守实参只读性和 BorrowSafe 契约，保留原地址与别名。完整执行链和验收见[拥有型基础值](docs/source-owned-values.md)。

## 源码枚举与结构体

```cpp
enum combat.Kind : uint { Physical, Fire = 7, Ice }
struct combat.DamageInfo { combat.Point point; combat.Kind kind; uint amount; };
struct combat.Point { float x; int count; };

uint process(combat.DamageInfo& damage) {
    combat.DamageInfo before = damage;
    damage.amount = damage.amount + 4;
    return before.amount;
}
```

- 类型声明位于顶层，可与函数交错；结构体字段可前向引用同模块类型。名称为准确声明名，支持 `combat.Point` 形式；没有命名空间块或用户类型的相对短名查找。类型不能重名，不能覆盖已注册类型、内建数学短名或 `String/Array/core.*/math.*`；同名类型与函数也拒绝。
- `enum Name [ : int | uint ] { ... }` 默认底层为 int，末尾分号可省略。枚举项可显式指定十进制整数（含负号），否则为零或上一项加一；底层范围溢出、重复项名拒绝，重复值允许。可使用尾随逗号。
- `struct Name { Type field; ... };` 字段支持 bool/float/double/uint/int、内建数学值、已注册或同模块枚举、同模块结构体，以及已注册的无 GC 引用记录、String 和具体 Array。`Array<T>`（含嵌套数组）按元素身份解析现有注册类型，不自动实例化 `Array<源码结构体>`。字段顺序决定布局，偏移和对齐由 Runtime 计算；内联循环、未知类型、重复字段和对象引用字段拒绝。没有字段初始化器、方法、继承或自定义构造析构。
- 结构体的拥有能力由字段生命周期能力递归组合；满足条件的局部可默认构造、深复制初始化和整值赋值。默认构造递归将标量置零、String/Array 置空，并调用原生记录的构造。字段随拥有者清理；部分构造失败时仅销毁已完成的字段。自有局部中的字段及元素遵守上述拥有值修改规则；引用参数允许修改可写标量/枚举叶值和数学字段，不能替换一般聚合字段或改变容器结构。满足 plain record 与拥有资格的源码结构体支持同步按值参数与结果；源码返回临时由返回值复制形成，不额外调用默认构造。没有初始化器的局部仍执行默认构造。
- 类型声明经 `Program::request` 收集并取得未发布的 Runtime 布局，函数由 `prepare` 编译；成功 `try_publish` 后类型和代码一起可用。任何候选失败均不发布新类型。新类型须在 Runtime 注册封闭前发布。
- 首次发布固定本程序的类型声明集合与定义；声明出现顺序可调整，增删类型或改动字段/枚举定义须重建 Runtime。代码更新、旧租约和程序关闭不替换或注销类型身份。

执行链、C++ 同存储访问及验收见[源码类型声明](docs/source-type-declarations.md)。

## 语法与静态规则

下列文法与本页类型、名称和能力限制共同适用；句法可解析不等于通过静态检查。

```ebnf
program        = { type_declaration | module_constant | function }, end_of_input ; (* 至少一个函数 *)
module_constant = "const", scalar_type, identifier, "=", expression, ";" ;
type_declaration = struct_declaration | enum_declaration ;
struct_declaration = "struct", qualified_name, "{", { field_declaration }, "}", ";" ;
field_declaration = ( scalar_type | aggregate_type ), identifier, ";" ;
enum_declaration = "enum", qualified_name, [ ":", ( "int" | "uint" ) ], "{",
                   [ enum_item, { ",", enum_item }, [ "," ] ], "}", [ ";" ] ;
enum_item      = identifier, [ "=", [ "-" ], decimal_integer ] ;
function       = [ "async" ], result_type, identifier, "(", [ parameters ], ")", block ;
result_type    = "void" | scalar_type | math_type | plain_record_type ;
scalar_type    = "bool" | "float" | "double" | "uint" | "int" | qualified_enum_name ;
math_type      = qualified_name ; (* 内建数学规范名称或固定短名 *)
plain_record_type = qualified_name ; (* Runtime plain_value 且 can_own 的普通记录 *)
math_name      = qualified_name ; (* 数学构造名称或已支持的 math.* 操作 *)
qualified_enum_name = qualified_name ; (* 必须解析为已注册枚举 *)
parameters     = parameter, { ",", parameter } ;
parameter      = ( ( scalar_type | math_type | plain_record_type ), [ "&" ]
                 | "const", ( scalar_type | math_type | plain_record_type ), "&"
                 | aggregate_type, "&" | "const", aggregate_type, "&" ), identifier ;
aggregate_type    = qualified_name | "String" | "Array", "<", ( scalar_type | aggregate_type ), ">" ;
                 (* 须解析为已准备或发布的原生/描述型记录、原生数组/String；String 参数只允许 const 引用 *)
qualified_name = identifier, { ".", identifier } ;
block          = "{", { statement }, "}" ;
statement      = block | value_local | reference_local | assignment | call_statement
               | if_statement | while_statement | for_statement | break_statement | continue_statement
               | return_statement | yield_statement ;
value_declaration = [ "const" ], ( scalar_type | math_type | aggregate_type ), identifier, [ "=", expression ] ;
                   (* 标量及所有 const 局部必须初始化；聚合值须满足拥有能力 *)
reference_declaration = [ "weak" ], qualified_name, identifier, [ "=", expression ] ;
value_local    = value_declaration, ";" ;
reference_local = reference_declaration, ";" ;
assignment_clause = expression, "=", expression ;
call_clause    = expression ; (* 必须是同模块调用、原生调用或序列修改 *)
assignment     = assignment_clause, ";" ;
call_statement = call_clause, ";" ;
if_statement   = "if", "(", expression, ")", block, [ "else", ( block | if_statement ) ] ;
while_statement = "while", "(", expression, ")", block ;
for_statement  = "for", "(", [ for_initializer ], ";", [ expression ], ";", [ for_step ], ")", block ;
for_initializer = value_declaration | reference_declaration | assignment_clause | call_clause ;
for_step       = assignment_clause | call_clause ;
break_statement = "break", ";" ;
continue_statement = "continue", ";" ;
return_statement = "return", [ expression ], ";" ;
yield_statement = "yield", ";" ;
expression     = logical_or ;
logical_or     = logical_and, { "||", logical_and } ;
logical_and    = bitwise_or, { "&&", bitwise_or } ;
bitwise_or     = bitwise_xor, { "|", bitwise_xor } ;
bitwise_xor    = bitwise_and, { "^", bitwise_and } ;
bitwise_and    = equality, { "&", equality } ;
equality       = relation, { ( "==" | "!=" ), relation } ;
relation       = shift, { ( "<" | "<=" | ">" | ">=" ), shift } ;
shift          = addition, { ( "<<" | ">>" ), addition } ;
addition       = product, { ( "+" | "-" ), product } ;
product        = unary, { ( "*" | "/" | "%" ), unary } ;
unary          = ( "+" | "-" | "!" | "~" ), unary | postfix ;
postfix        = primary, { ".", qualified_name, [ "(", [ arguments ], ")" ] | "[", expression, "]" } ;
arguments      = expression, { ",", expression } ;
primary        = qualified_enum_name, "::", identifier | "true" | "false" | numeric_literal | string_literal | identifier, [ "(", [ arguments ], ")" ] | "(", expression, ")" | "self" | "null"
               | math_name, "(", [ arguments ], ")" (* 内建数学构造或 math.* 运算 *)
               | ( "float" | "double" | "int" | "uint" ), "(", expression, ")"
               | "new", qualified_name, "(", ")"
               | "observe", "(", expression, ")"
               | "try_acquire", "(", expression, ")" ;
```

### 声明与作用域

局部在初始化表达式检查完成后才可见；参数从函数体开始可见。每对花括号建立词法块，不允许重复声明或遮蔽仍可见的参数/局部；退出后，后续独立块可复用名字和不同类型。每条声明只声明一个变量。

`const T name = expression;` 局部支持标量、枚举、数学值及具备拥有能力的记录/String/Array；必须初始化，初始化可以在运行期求值。只读性沿字段与元素传播，禁止赋值、传给可写引用或修改容器结构；该规则不改变复制和销毁责任。对象持有/观察引用的 const 局部暂不支持。

顶层 `const T name = expression;` 只允许标量或枚举，初始化必须是编译期纯表达式：字面量、同模块常量、枚举项、括号、已支持的一元/二元运算及显式数值转换；不能调用函数、读取对象字段或创建对象/容器。允许前向引用，依赖环拒绝。常量求值遵守运行期运算、转换和短路语义：需执行的除零、溢出除余、非法移位或整数转换在编译期报告错误，被短路跳过的计算不执行；未执行部分仍须通过语法和类型检查。常量保持显式声明类型，不被 int 上下文重新定型。模块常量不能与类型、函数、参数或局部同名。

标量局部必须显式初始化；持有/观察局部可以省略初始化器，等价于该类型和持有形式的空引用。String/Array 局部省略初始化器时构造为空值；一般记录局部调用 Runtime 默认构造；数学值遵循[数学默认值](docs/math.md#默认值坐标与浮点规则)。复制初始化产生独立拥有存储。未初始化槽不能当成已初始化空值。

赋值目标允许可写标量/枚举/数学参数、局部、绑定字段、值中的对应叶值、当前函数拥有的记录/String/Array 局部及其满足拥有能力的嵌套字段或元素，可以外包括号。字面量、方法调用、比较和运算结果不可赋值；`(steps) = 1;` 合法，`+steps = 1;` 不合法。`=` 不是表达式，不允许链式或条件内赋值。`self` 不可赋值。

### 控制流与返回

声明、赋值、调用、`break/continue`、`return` 和 `yield` 以分号结束；空语句 `;` 拒绝，空块合法。同模块函数调用、原生成员调用和序列修改可以作为表达式语句；普通数值、字段读取或 `new` 表达式不能单独作为语句。

`if/while` 必须有括号条件和花括号体；`else` 可跟块或完整 `if`。条件必须是 bool，可以来自字面量、变量、比较、逻辑运算或调用。循环可能执行零次；不提供循环终止证明、执行次数或超时上限。

`for (init; condition; step) { ... }` 的三个子句均可省略，省略条件等价于 true。init 可为一条声明、赋值或调用，只执行一次；声明的变量仅在该循环的条件、迭代和体内可见，循环退出时清理。condition 必须是 bool；step 为一条赋值或调用，不允许声明或逗号列表。正常迭代和 continue 都先清理退出的体内作用域，再执行 step，之后经过循环回边安全点重新检查条件。break 清理退出的作用域并直接退出循环，跳过 step。break/continue 只作用于最近的 while/for，在循环外拒绝。

`void` 函数允许 `return;` 或自然落尾。非 void 函数必须返回同类型表达式；顺序块中确定返回、完整 `if/else` 两侧返回可证明不落尾，循环自身不能证明必定返回。不可达代码仍要通过语法和类型检查。

## 同模块函数与入口

函数使用 `sum(values)` 形式直接调用同一模块中的函数，允许调用在后面声明的函数。函数名唯一，当前没有重载或函数值。所有函数体均须通过类型检查；机器码只包含选定入口可达的函数。

宿主使用 `program.request(source_name, source, "process")` 选定入口。首次发布前只有一个函数时可省略名称；多个函数必须明确选择。首次发布后，省略名称沿用已冻结的入口，不依赖声明顺序。入口名称、参数/结果、接收者要求和同步/async 种类沿原发布契约固定；私有助手可随整模块版本改变名称、顺序及签名。

- 内部同步调用支持标量/枚举/数学值、合格 plain record 的按值参数，以及现有同步引用；返回 void 或上述按值类型。引用直接传递地址，临时值的绑定与存活期遵循[参数传递与枚举](#参数传递与枚举)，String 字面量保持到当前语句结束。
- Runtime 对象字段可以按值传给内部函数；字段引用仅能用于受 Runtime 保护的原生成员调用，不能跨进源码助手。调用方拥有的标量、原生记录/数组的叶值和聚合借用沿已有规则传递。
- 借用保护与接收者需求沿调用图传播，内部函数不能绕过只读性、结构修改限制或 BorrowSafe 契约。未调用的函数不改变所选入口的接收者和执行区间要求。
- 调用图必须无环，直接和间接递归均在编译时拒绝；内部调用目标必须同步。函数中的对象根与拥有值归该次调用，逐层清理；助手不执行整次调用的 begin/finish 回调，也不清理调用者或公开接收者的根。
- 活动调用、固定租约和挂起入口保留整个模块版本。更新后，旧执行链中的所有助手仍来自旧模块。
- 顶层常量在编译时求值，随其模块代码版本固定。更新常量只影响新版本；旧租约、活动调用与挂起帧继续使用旧值。

具体执行结构、示例与端到端证据见[同模块多函数](docs/source-function-modules.md)。

## 对象与成员

宿主提供完整的已注册对象类型名，例如 `business.NativeMachine`；声明与 `new` 必须使用该名字，不能用布局相似的另一个名字替代。对象类型没有别名或短名回退，语言没有 `import/include`；内建数学短名是固定便利拼写，类型声明见[源码枚举与结构体](#源码枚举与结构体)。

字段路径及原生方法签名来自同一 Runtime 元数据，继承成员保留原声明身份。标量与数学字段可位于内联记录中；数学字段同时开放整值与分量路径，例如 `self.position`、`self.position.x`，均沿 Runtime 对象校验访问。目录名由非保留标识符组成，同类目录不允许重复。源码可通过 `self`、持有局部或持有临时值访问符合绑定类型的对象，尚不能遍历引用字段或使用第二种声明类型的目录。基类绑定看不到派生专有成员，原生基段地址调整由 Runtime 完成。

`self` 是受保护的接收者持有值。目录含对象类型不表示入口必须提供 self：纯标量或仅 `new` 的函数可以不使用接收者。首次发布固定接收者要求；已有接收者声明即使新方法体不再读取 self，调用方仍须提供正确活对象，无接收者声明不能在兼容更新中新增 self。

原生成员方法接受[参数传递与枚举](#参数传递与枚举)及[拥有值与原生算法](#拥有型-stringarray-与原生算法)规定的按值参数与同步借用，返回 void 或标量/枚举/数学值/合格 plain record。非 void 调用可按结果类型参与表达式、实参和 return，也可作为语句丢弃成功值；失败仍传播。void 不能用于值上下文。原生 const 接收者信息与参数传递方式分别保留在共享签名中。

### 引用操作

| 操作 | 行为 |
| --- | --- |
| 持有赋给持有 | 复制身份与持有关系，不克隆对象 |
| 观察赋给观察 | 复制身份，不保活目标 |
| `null` 赋给引用 | 保留目标类型/持有形式，清除目标身份 |
| 持有与观察互赋 | 拒绝，须显式转换 |
| `observe(holding)` | 返回同类型观察引用；允许空或已失效持有值，不访问对象数据 |
| `try_acquire(observation)` | 存活目标返回临时持有；空或已失效目标返回同类型空持有，不改写原观察 |
| `new T()` | 通过共享 Runtime 默认构造绑定类型 T，立即保护返回的持有临时值；不改为接收者的实际派生类型，没有构造参数 |
| 引用 `==/!=` | 比较身份，不比较内容或存活性；持有/观察可交叉比较，也可与 null 比较 |
| 观察引用成员访问 | 静态拒绝，须先取得持有引用 |

`observe/try_acquire` 的输入须分别已有持有/观察类型，不能直接传无类型的 `null`。`null == null` 为真；null 不是可声明类型，不能赋给标量。引用不支持排序或指针运算。

### 空值、失效与失败结果

空引用没有目标身份。显式销毁或 GC 后，旧引用保留身份和代际，不自动变空，也不指向复用槽位的新对象。复制、观察转换和身份比较不要求目标存活；实际成员访问检查活身份。

只有观察取得的“没有可取得目标”转换为空持有。准入、类型、容量、原生服务错误仍走执行失败与清理路径，不伪装为空。成功取得不阻止稍后的获准显式销毁，也不等于数据加锁。

### 词法生命周期

持有局部活到词法块退出；观察局部不登记强根。替换持有值先保护新值，再撤销旧关系，失败保留原值。循环体局部在回边安全点之前清理，下一次进入重新初始化。break/continue 清理其退出的体内作用域；for 初始化变量在 continue 时保留到下一轮，在循环结束时清理。

持有临时值在后续可能收集的操作前已受保护；声明/赋值消费后结束临时值，条件临时值在进入分支或循环体前结束。成员赋值的临时接收者保持到写入完成。早退和错误显式清理已初始化局部及临时值，RAII 只兜底；槽位复用沿既有根撤销和暂停游标取消机制。

## 显式协程

```source
async uint accumulate(uint count) {
    uint total = 0;
    uint index = 0;
    while (index < count) {
        total = total + index;
        yield;
        index = index + 1;
    }
    return total;
}
```

`async` 位于结果类型前，执行种类属于函数契约。`yield;` 只能位于协程体，可出现在块、分支或循环中；它不产生值，不结束作用域或函数。

创建复制标量/枚举值参数并固定代码版本，不执行源码体；bool 和 double 同样支持参数和结果，async 聚合参数/结果及引用参数拒绝。首次恢复从开头执行，其后从上次 yield 后继续。标量（含 bool）和数学局部（含各自的 const 形式）由持久帧保存，对象引用沿同一 Runtime 根机制保活；挂起不缩短词法生命周期。for 和 break/continue 遵守相同控制流与清理规则。显式销毁仍可能使目标失效，恢复后访问继续校验。

return 完成原有清理后发布结果；挂起、失败、取消不发布完成结果。宿主在同一 Runtime 线程显式恢复：ready/suspended 取消立即清理，running 只登记请求，在本次恢复返回时处理。执行错误优先，否则取消压过挂起或成功结果；既有副作用不回滚。取消不抢占原生函数或无挂起长循环，也不运行用户续体。

协程入口可以调用同步辅助函数，辅助调用必须在 yield 前结束；源码不能直接调用 async 函数。每个实例始终恢复和清理创建时的整模块版本，包括各同步辅助函数。旧实例挂起时可发布兼容新版本，新实例使用新版本；没有帧迁移。完整状态、准入及关停责任见 [Runtime 协程协议](docs/runtime-model.md#协程核心协议)。

## 执行、诊断与保护

代码必须经过类型检查、LLVM/JIT 编译、链接和发布校验才执行；失败保留已有发布版本。没有解释执行回退。当前每份源码的编译产物拥有所需名称、目录数据和入口可达的全部内部函数；原生对象生命周期、错误首因与代码退休沿[正式执行协议](docs/runtime-model.md#已实现的最小业务执行协议)。

编译诊断包含源名、从 1 开始的行列及原因，通常在首个词法/语法/类型错误停止；当前位置可能是解析器发现问题的位置。过长源名的显示有界缩略，仍保留请求号、位置和原因，完整名字不因此变成截断身份。

原生 C++ 异常转换为执行错误，不穿越生成帧；参数/类型等调用边界错误在业务副作用前拒绝。成功标量、数学或 plain record 结果先暂存，原生调用、结束回调与生成清理均成功后才提交；失败不覆盖调用方结果，不回滚已发生的修改。数学和 plain record 公共结果沿同一 `invoke` 的 `ValueOutput` 重载交付，见[数学调用边界](docs/math.md#调用生命周期与优化边界)。成功回调若漏写或返回错类型结果，报告明确的结果契约违约，保留更早的宿主首因。

普通函数在入口、实际循环回边及受控原生调用后沿执行协议推进已有 GC；含聚合借用参数、普通 plain record 按值参数/结果、一般记录/String/Array 拥有型局部或 String 字面量的函数按上述同步值区间规则推迟收集。单独的数学局部与按值参数不推迟收集。不隐式创建 Tick 调度器。机器验收的快照是消费者策略，不是语言返回值、隐式克隆或编译器固定根槽。

| 实现保护 | 当前边界 |
| --- | --- |
| 解析递归和表达式高度 | 各受 128 的实现预算约束，不等于可接受 128 层括号或语句块 |
| 模块声明数量 | 函数、类型、顶层常量各最多 1024 个；模块内名称不冲突，函数调用图、常量依赖和结构体内联布局均无环 |
| 局部/目录索引 | `uint32_t` 范围检查，不承诺能编译数十亿项 |
| DLL 源码长度 | 使用 `uint32_t` 字节长度；前端接收 string_view，不承诺近 4 GiB 输入 |
| 执行平台 | 当前 Windows x64，同一 Runtime 线程 |
| 编译资源 | 同步编译，尚无硬内存配额、超时中断或完整 OOM 恢复 |

这些实现保护不是永久语言规则，也不构成任意不可信程序的安全沙箱。语法实例与拒绝场景由[源码端到端验收](acceptance/jit/business.cpp)、[标量调用验收](acceptance/jit/typed_business.inl)、[协程验收](acceptance/jit/coroutine_business.inl)、[生成反射调用链](acceptance/reflection/calls.cpp)、[原生值借用验收](acceptance/source/native_values.cpp)、[拥有值验收](acceptance/source/owned_values.cpp)、[模块函数验收](acceptance/source/modules.cpp)、[类型声明验收](acceptance/source/types.cpp)、[语言基础验收](acceptance/source/foundations.cpp)、[bool 调用边界验收](acceptance/jit/bool_abi.cpp)及[公共消费者](acceptance/source/main.cpp)覆盖；命令以 README 为准。
