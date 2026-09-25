# FrostFoxAtelier 业务语言 v0.0.2：对象引用局部增量（暂定）

> 状态：源码引用局部已实现，开发与发布配置的正式端到端验收均通过；语言规范仍为暂定，不代表完整语言或热更新已交付。
>
> 本文规定在 [v0.0.1](LANGUAGE-v0.0.1.md) 之上的引用增量。未被本文件明确替换的词法、数值、标量作用域、控制流及错误规则继续适用。v0.0.1 文件保留原样作为历史基线；编译器没有双版本选择开关，也不承诺新增保留字完全向后兼容。

## 1. 本次范围

本次贯通的是源码对象引用，而不是手写 AST 或固定宿主槽的替代演示：

- 显式类型的持有局部与 `weak` 观察局部；二者可空、默认空。
- `self` 可作为不可赋值的持有引用值。
- `null`、引用赋值与身份相等/不等比较。
- `new T()`、`observe(holding)`、`try_acquire(observation)`。
- 通过持有引用访问既有绑定字段及调用既有原生方法。
- 块退出、循环、早退、失败与完整表达式临时值的根生命周期。

仍只有一个返回 `void`、接收 `float`/`uint` 的宿主绑定入口；未增加一般函数、对象返回、源码类型声明、引用字段、任意多类型局部、源码销毁、记录值帧、热更新或并发执行。普通业务对象、身份、类型和 GC 仍属于共享 Runtime，不另建脚本对象世界。

## 2. 对象类型绑定

当前每个编译输入至多使用**一个宿主已绑定对象类型**。宿主提供真实、完整的类型名，例如 `business.NativeMachine` 或 `business.DescribedMachine`，以及已验证的原有字段/方法目录。

- 声明和 `new` 必须使用该完整类型名，不引入 `import`、类型别名或隐式类型推导。
- 类型名是由点连接的合法 ASCII 标识符；各段不能是保留字。
- 名字选择已有权威类型，不创建另一份类型定义。两个对象布局相似，不意味着其源码类型名可互换。
- 编译产物拥有类型名字节；源码和临时绑定字符串可在编译后释放。
- 宿主在执行前检查 Runtime、接收者身份、绑定成员形状及编译产物要求的类型名。不匹配的类型在进入生成代码前拒绝。
- 没有显式对象类型名的原有标量程序仍可通过既有机器适配器执行；这不授权任意类型、Runtime 或持久机器码缓存复用。

## 3. 新增词法与语法

### 3.1 保留字变化

新增 `weak`、`null`、`new`、`observe`、`try_acquire`。它们不再可用作普通标识符；这是相对 v0.0.1 的明确词法变化，不伪装成完全兼容的扩展。数值字面量、浮点行为和原有运算优先级不变。

### 3.2 引用声明

```text
business.NativeMachine owner = self;
business.NativeMachine empty;
weak business.NativeMachine watched = observe(owner);
weak business.NativeMachine empty_watch;
```

普通对象类型声明为持有引用，`weak` 明确选择观察引用。省略初始化器只适用于这两种引用局部，等价于初始化为该类型/持有形式的空引用；标量局部仍必须显式初始化。

沿用原有名称规则：初始化表达式不能读取尚未声明完成的本变量，不能重复声明或遮蔽当前仍可见的局部/参数；退出内层块后可按原规则再次使用该名字。`self` 仍不可赋值。

### 3.3 增量文法

以下是对 v0.0.1 相应产生式的增量，必须与静态规则共同使用：

```ebnf
qualified_type      = identifier, { ".", identifier } ;
reference_type      = [ "weak" ], qualified_type ;
reference_local     = reference_type, identifier, [ "=", expression ], ";" ;
reference_primary   = "self"
                    | "null"
                    | "new", qualified_type, "(", ")"
                    | "observe", "(", expression, ")"
                    | "try_acquire", "(", expression, ")" ;
member_suffix       = ".", identifier, { ".", identifier },
                      [ "(", expression, ")" ] ;
```

`reference_local` 加入语句选择；`reference_primary` 加入主表达式。主表达式可以使用括号，其后可接成员后缀；接收者必须是持有引用，成员路径和参数仍按既有绑定目录检查。`new` 没有构造参数，不是泛用构造函数解析。

`if`/`while` 仍要求原有块体。比较只增加引用 `==`、`!=`；没有布尔局部、引用真值转换、引用排序、指针算术或赋值表达式。

## 4. 引用值与操作

| 操作 | 规则 |
| --- | --- |
| 持有赋给持有 | 复制身份与持有关系，不克隆对象 |
| 观察赋给观察 | 复制观察身份，不保活 |
| `null` 赋给引用 | 保留目标类型/持有形式，去掉目标身份 |
| 持有与观察互赋 | 拒绝；必须显式 `observe` 或 `try_acquire` |
| `observe(holding)` | 产生同类型观察引用，不访问目标数据；允许持有值为空或已失效 |
| `try_acquire(observation)` | 存活目标产生临时持有；空或已失效目标产生同类型空持有；不改写原观察值 |
| `new T()` | 经既有 Runtime 创建同绑定类型对象，返回立即受保护的持有临时值；不隐式克隆 `self` |
| `==` / `!=` | 比较对象身份而非内容或存活性；同目标类型的持有/观察可交叉比较，也可与 `null` 比较 |
| 通过持有引用访问成员 | 每次实际访问仍遵守 Runtime 的类型、身份、存活与原生借用规则 |
| 通过观察引用访问成员 | 静态拒绝；先尝试取得持有引用 |

`observe` 的实参必须已具有持有引用类型，`try_acquire` 的实参必须已具有观察引用类型。直接传入无持有形式的 `null` 字面量拒绝；先声明一个相应形式的空局部即可。`null == null` 为真，但 `null` 不是可声明类型，也不能赋给数值。

### 4.1 空与失效不同

空引用没有目标身份。对象被显式销毁或回收后，既有引用保留原身份和代际，不自动变为空，也不能指向复用位置上的新对象。

因此，复制、观察转换和身份比较不要求目标仍存活；实际成员访问才要求有效目标。`try_acquire` 对失效观察返回空持有，原观察仍不等于 `null`。成功取得不阻止稍后的获准显式销毁，也不等于数据加锁。

### 4.2 失败结果

本增量用**可空持有引用**表达观察取得失败，不新增 `Option`、元组、`out` 参数或一般错误值类型：

```source
void inspect(float amount, uint steps) {
    weak business.NativeMachine watched = observe(self);
    business.NativeMachine held = try_acquire(watched);
    if (held != null) {
        held.advance(amount);
    }
}
```

只有“没有可取得目标”转为空值。Runtime 准入、类型、容量或原生服务错误仍按既有执行失败路径传播并清理，不能吞错并伪装为空。源码仍没有异常捕获，已经发生的业务修改不回滚。

## 5. 求值与生命周期

### 5.1 局部

- 初始化完成后才使局部值可见；未初始化槽不是已初始化空引用。
- 持有局部活到其词法块退出，不先实现“最后一次使用后提前撤根”。观察局部不登记为持有根。
- 替换持有值时先取得/保护新值，再撤旧关系；失败不擅自丢掉原局部值。
- 块退出必须撤销已初始化引用。循环体的局部在该次回边安全点之前退出，下一次进入该声明开始新生命周期。
- 早退撤销所有已初始化用户局部和临时值，再走原有宿主结果路径；错误路径也显式清理，宿主 RAII 只作兜底。
- 槽位结束/复用必须沿既有根撤销机制取消排队或暂停借用，不能因地址/槽号相同而延续旧生命周期。

### 5.2 临时值和求值顺序

表达式按既有左到右方向求值。成员接收者先于方法参数；字段赋值的接收者先于右侧表达式，并保持到写入完成。

`new`、`observe`、`try_acquire` 的结果可以作为临时值继续参与表达式。持有临时值在后续可能观察或收集的操作前已被保护，不能靠未报告的寄存器值保活。赋值/声明消费结果后结束相应临时值；条件求值完成后，在进入分支/循环体前清理条件临时值。

普通读取一个引用局部不意味着新增业务对象，也不承诺本轮已完成根放置或寄存器级优化。

### 5.3 `self`、快照与安全点

入口和每条实际循环回边的安全点位置不变；提前返回不产生额外回边。新增引用局部没有引入另一套 GC、隐式 Tick 或每条语句轮询。

`self` 仍由受保护的宿主接收者提供。验收快照仍不是源码返回值，也不是源码隐藏局部；其同步次序和结果交接不因新增引用而改变。对象局部的词法活期不能被快照保活冒充。

## 6. 完整示例与应拒绝情况

以下函数需要宿主绑定真实的 `business.NativeMachine` 类型：

```source
void references(float amount, uint steps) {
    business.NativeMachine owner = self;
    weak business.NativeMachine watched = observe(owner);
    business.NativeMachine empty;
    if (empty == null) {
        business.NativeMachine held = try_acquire(watched);
        if (held != null) {
            held.advance(amount);
            held.state.cycles = held.state.cycles + 1;
        }
    }
    uint i = 0;
    while (i < steps) {
        business.NativeMachine temporary = new business.NativeMachine();
        temporary.state.heat = amount;
        i = i + 1;
    }
}
```

语义边界反例：

| 写法 | 拒绝原因 |
| --- | --- |
| `weak business.NativeMachine w = self;` | 不隐式从持有转换到观察 |
| `business.NativeMachine h = observe(self);` | 观察不能直接赋给持有 |
| `try_acquire(self)` | 需要观察引用 |
| `observe(null)` / `try_acquire(null)` | 实参没有所需的持有形式 |
| 观察局部 `w.advance(amount)` / `w.progress` | 观察不能直接解引用 |
| `self = null;` | 接收者不可赋值 |
| 引用 `a < b`、`a + b` 或 `if (a)` | 不支持排序、算术或真值转换 |
| `new business.NativeMachine(amount)` | 当前创建不接受构造参数 |
| 绑定 NativeMachine 却声明另一对象类型 | 当前编译输入只绑定一种对象类型 |
| `float x = null;` | 空引用不能转换为数值 |

## 7. 实现职责与验收终点

- [前端](src/language/source.hpp)：只持有编译分类、类型名字节、绑定成员选择器、接收者表达式与块局部信息，不拥有 Runtime 或 LLVM 对象。
- [降低与私有接口](acceptance/jit/business_backend.hpp)：报告调用所需强/弱槽，生成完整表达式及控制流清理。槽编号只是调用存储位置，不是第二套对象身份或永久语言 ABI。
- [执行帧](src/runtime/execution.hpp)：经共享 Runtime 处理带类型的空/失效持有值；既有要求存活的入场和快照交接操作保留各自契约。
- [真实业务验收](acceptance/jit/business.cpp)：在原生/描述型对象上从源码编译执行；显式清理在 RAII 兜底之前检查。

验收必须观察：默认空、强弱转换、身份比较、创建与别名访问；局部独自跨完整新 GC 周期存活；退出作用域后只有观察则可回收；循环槽复用；失效身份复制/比较与真正解引用的区别；早退与原生错误清理；临时接收者及嵌套取得的生命周期；错误源码拒绝及旧代码保留；旧标量、平台展开和代码退休行为仍通过。

开发和发布配置均执行 `cmake --build --preset <dev|release> --target acceptance`，退出码为 0，Runtime、Business execution、JIT 三项均 PASS。新增引用路径在原生/描述型对象上各运行 11 个独立场景，每场景结束均核对对象、根与原生值实例已清空；拒绝矩阵也通过真实编译检查，并重新执行保留下来的旧代码。旧标量、对象图 GC、原生借用、Windows 展开和活动代码退休验收继续通过。

本文件两个完整 `source` 示例另经 Release 真实编译并执行，核对接收者/快照结果、安全点数和最终零根/零对象；记录在 `out/review/reference-doc-examples.log`，一次性驱动与构建命令也保留在 `out/review/`，不加入常驻基准或测试框架。

本机完整日志为 `out/review/reference-dev-acceptance.log` 和 `out/review/reference-release-acceptance.log`。本次没有重新做吞吐测量，不声称性能提升、完整故障覆盖、安全沙箱、热更新或通用记录值帧已经完成。
