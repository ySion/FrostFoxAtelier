# 源码拥有型基础值与受控原生调用

状态：已实现。同步源码可创建临时 String/Array、借用原生存储调用受控算法，并在正常退出或错误时结束拥有值生命周期。现行语法统一由 [LANGUAGE.md](../LANGUAGE.md#拥有型-stringarray-与原生算法)维护。

## 使用方式

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

宿主预先在同一 Runtime 注册 String、具体 Array 和元素类型，并通过已有 Program 编译、发布和调用。示例中的原生方法可声明为：

```cpp
AFUNCTION(BorrowSafe)
std::uint32_t process(Array<float>& values, const String& label);
```

方法体可以读取标签、原地处理元素并返回数量；必须保持调用期间已有借用有效。当前阶段不允许此方法重分配、清空或替换所借容器，因为其他参数可能是它的元素或字段别名。

## 统一执行链

1. `runtime/foundation.hpp` 生产既有 TypeDefinition，提供序列视图、修改原语与值操作能力。原生宏生成器发布同一 MethodDefinition 的 `preserves_borrows`。
2. Runtime 在原类型事务中完成大小、对齐、元素、引用闭包和方法签名验证，再发布 TypeInfo/MethodInfo。数组的值操作能力取决于元素能力；前向方法参数在全部布局完成后再验证，失败不部分发布。
3. `source::Program` 捕获同一目录并固定到代码版本。前端检查类型、只读性、拥有者和调用效果；没有第二套类型身份或调用入口。
4. LLVM 生成函数使用按原生大小及对齐分配的栈存储，并为可能构造的值记录是否存活。构造、复制、赋值与销毁调用既有 Runtime 值操作；序列修改通过 `Runtime::mutate_sequence` 校验后分派到基础库。
5. 原生算法仍使用 `Runtime::call`、共享 FunctionSignature 和 Argument 借用。记录、数组元素和字段可直接传递原地址；String 索引是扩展后的 uint 值，传给 const 引用时使用标量临时存储。
6. 块退出、循环迭代、提前返回和错误清理按逆序销毁已成功构造的值。字面量临时值在语句或条件结束时清理。公共调用确认存活计数归零后才提交成功结果，错误保留已发生的修改。

String 字面量直接用于声明、赋值或追加时，生成代码直接传递文本字节，不建立未使用的临时拥有值及清理分支；真正作为值使用的字面量仍提前分配清理槽，覆盖全部返回路径。

拥有型对象放在生成栈上，String 的短文本仍使用既有 SSO，长文本和 Array 缓冲仍由基础库分配。复制使用真实值语义；这里不把深复制描述为零拷贝。外部借用及传给原生方法的引用继续访问原存储。没有测量本阶段的业务吞吐收益。

## 借用与值操作契约

`AFUNCTION(BorrowSafe)` 或 `NativeRegistration::method(name, true)` 是可信 C++ 生产者的声明：整个调用链不保存输入借用、不使仍使用的借用失效、不重入源码或挂起。Runtime 继续执行原生回调的 GC、生命周期与执行准入保护；编译器不能证明任意 C++ 实现遵守该声明。

类型的 `NativeLayout::value_operations_preserve_borrows` 同样要求构造、复制、赋值、移动及析构不保存输入地址、不重入源码、不使其他存储的借用失效；目标值自身的结构修改由拥有者控制。NativeRegistration 为默认构造、复制构造、复制赋值、析构以及 Array 重定位实际选中的移动/复制构造均平凡的类型自动提供保证。基础 String 显式提供该保证，Array 在发布时根据元素类型递归组合。

非平凡记录元素由生产者明确设置此能力，例如验收中的带计数 Particle；仅仅不含 GC 引用不足以开放源码拥有操作。未具备能力的记录和 Array 仍可按现有规则同步借用。

聚合借用、一般记录/String/Array 拥有型局部和 String 字面量使整个函数成为同步值区间。仅开放 `self` 上的 BorrowSafe 方法，GC 和 `begin/poll/finish` 策略回调留到区间结束后的宿主。不提供执行超时保证；原生调用方仍负责外部存储的真实寿命和结构稳定。[内建数学值](math.md)的按值参数与局部不调用这些生命周期服务，也不产生同步值区间；数学引用参数仍保留借用限制。

## 当前边界

- 局部拥有值包括 String/Array，以及具备相同生命周期能力的原生或描述型记录；支持默认构造、深复制初始化和整值赋值。Array 支持 reserve/resize/clear/add/append，String 支持 reserve/clear/append 和字面量赋值；这些操作也可沿拥有局部访问满足同样能力的嵌套字段或元素。[源码结构体](source-type-declarations.md)复用 Runtime 递归组合字段的布局和生命周期。
- 外部借用容器禁止结构修改；源码入口的 String 参数仍只允许 `const String&`，字符索引只读。BorrowSafe 原生方法须遵守全部实参的只读性与别名约束。
- 字面量支持 UTF-8、反斜线、引号、换行/回车/tab/零字节转义；未知转义在编译时拒绝，非法 UTF-8 在基础 String 构造或修改时报告执行错误。
- String/Array 和一般拥有型记录暂不跨 async/yield；数学局部可以跨挂起。现有标量（含 double）与对象协程沿原协议执行。
- 一般记录/容器按值参数与结果、借用逃逸与返回和源码 class 仍属于后续增量；内建数学值已支持同步按值参数与结果。[源码 enum/struct](source-type-declarations.md)已接通记录拥有型局部；[同模块多函数](source-function-modules.md)已沿用这些值生命周期与借用契约。

## 端到端验收

[正式消费者](../acceptance/source/owned_values.cpp)通过生成反射、公共 Program 和真实原生存储覆盖：

- 临时 String、Array<String>、嵌套 Array 与 64 字节对齐非平凡记录；深复制后独立修改。
- 原生批处理、记录/数组元素/字段的直接可写引用与 const 别名、String 字节作为标量引用实参。
- 128 次循环、嵌套块、提前返回、原生异常、越界、负容量、非法 UTF-8，以及部分复制失败后的清理。
- 字面量临时值和转义、兼容方法体替换、旧租约执行。
- 未声明安全值操作、未知原生调用、借用结构修改、跨挂起、String 字节写入的拒绝；同批前向 GC 记录参数不能绕过共享签名验证。

本阶段 dev/release 均已通过上述消费者及下列相关验收；阶段复核日志为 `out/stage-review-source-dev.log`、`out/stage-review-dev.log` 和 `out/stage-review-release.log`。

入口仍是 `source_acceptance`，也包含在 `jit_acceptance` 中。开发与发布配置执行同一套检查，不依赖 assert。相关基础库、反射 CLI、JIT、Runtime、World 与无窗口编辑器文档验收一并覆盖；图形窗口不在本次修改范围内。
