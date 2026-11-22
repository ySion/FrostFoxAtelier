# 原生值借用与源码直接访问

状态：已实现。C++ 持有 `Array<原生记录>`，源码通过引用处理同一缓冲，C++ 直接读取修改结果。所有权和析构仍由原生拥有者负责。

## 使用方式

宿主将原生记录、String 和具体 Array 类型提交给同一个 `Runtime::register_types`，然后使用既有 `source::Program` 编译、发布和调用。下面的源码操作宿主持有的原生 Particle 数组：

```source
float update(Array<borrow.Particle>& values, float delta) {
    uint index = 0;
    float total = 0.0;
    while (index < values.size()) {
        values[index].position.x = values[index].position.x + delta;
        total = total + values[index].position.x;
        index = index + 1;
    }
    return total;
}
```

原生入口使用 `program.entry().invoke`，实参为 `Argument::borrow(particles)` 与 `Argument{Scalar{delta}}`，由动态入口核对共享签名。该借用不属于同步按值标量的 Typed 白名单。`Entry::invoke` 返回后，原生 `particles[i].position.x` 即为更新后的值。

Array 泛型拼写解析到已发布具体类型的同一身份；没有独立容器注册表。只读记录、数组和字符串使用 `const Type& name`（深只读）；可写引用 `Type& name` 允许字段/元素修改及非定长序列的结构/整值修改（String 的字节索引仍为只读值）。确切语法、只读传播、SOP 定长列约束、调用点别名检查及同步区间限制由 [LANGUAGE.md](../LANGUAGE.md#原生值借用)维护。

## 一条布局与执行链

1. [反射生成器](../tools/reflection/main.cpp)从实际 C++ 声明与 `ASTRecordLayout` 生成既有 `NativeRegistration` 调用。自然对齐的字段与非虚基类携带真实字节偏移；[基础库适配器](../modules/execution/runtime/include/runtime/foundation.hpp)提供取得 Array/String 原缓冲与长度的只读原语。
2. [Runtime](../modules/execution/runtime/src/runtime.cpp)在原事务中验证并发布大小、对齐、元素身份、基类偏移和字段偏移。隐藏字段不进入公开 `TypeInfo::fields` 目录；紧凑布局或未提供偏移的手写绑定保留原访问器，不猜测地址。
3. [源码绑定](../modules/execution/source/src/detail/bridge.cpp)从同一 Runtime 捕获目录；[Program](../modules/execution/source/src/program.cpp)解析共享 `FunctionSignature`，将元数据依赖固定到代码版本，兼容发布再次核对。
4. [前端](../modules/execution/language/src/source.cpp)检查类型、可写性和存续期限制；[LLVM 后端](../modules/execution/jit/src/source_backend.cpp)在边界检查后按真实字段偏移和元素步长发出加载/写入。
5. 宿主进入源码仍经过 `Argument::borrow`、参数准备与 `ExecutionScope`。聚合引用参数传递原地址，不复制到 `Scalar`，也不构造镜像对象。源码调用原生方法则使用编译时绑定的定型 thunk，引用实参在全部实参求值后定位；聚合按值实参另用独立对齐活值，交由 C++ thunk 完成真实按值调用。同步返回后借用结束，原生拥有者继续管理缓冲。

源码目录只开放具备所需投影的值，数组须先有可用的元素投影；未开放的嵌套容器和以其为参数的方法保留为原生能力，不妨碍无关标量或对象函数编译。源码实际使用未开放类型或方法时报告编译错误。

不改变结构的顶层序列参数在调用入口取得视图并复用；可能改变序列结构的可写引用参数与嵌套序列按访问取得最新视图，循环中的暂存放在函数入口。只读路径和可写路径允许合法别名（若同一次调用中某一可写引用形参可能改变序列结构，编译期禁止另一引用形参同时借用同一根局部），生成代码不附加未验证的独占访问假设。

## 生命周期与当前边界

含聚合值借用的函数形成同步原生值区间，允许拥有型临时 String/Array、按值读取 `self` 的标量/枚举/数学字段和 `self` 上已声明 `BorrowSafe` 的原生方法；字段读取不盖写代号、不推进 GC；其他对象操作、未声明的原生业务调用及挂起仍被拒绝。GC 留给返回后的宿主，生成清理和结果提交仍沿原执行协议。原生调用方须保证存储已构造、存续，且在未授权结构变更（如 SOP 定长列或只读借用）时保持结构稳定；编译器不能验证任意 C++ 调用者提供地址的真实寿命。

[拥有型基础值](source-owned-values.md)已扩展 String/Array 和满足生命周期能力的记录局部；[源码结构体](source-type-declarations.md)可组合这些值字段并自动合成描述型 `Array<T>`。满足 plain record 与拥有资格的记录支持同步按值参数和结果。非定长 `Array<T>&`/`String&`/`Struct&` 支持结构修改与整值赋值，SOP `[InOut]`/`[Out]` 定长列与 `const Type&` 编译期禁止改变结构；资源记录/容器按值调用及跨挂起借用尚未实现。[内建数学值](math.md)支持同步按值调用、返回和整体赋值，包括可写借用中的数学字段或数组元素；数学引用参数仍使用本页的同步借用区间。Runtime 对象字段仍不能借用到源码执行入口或内部助手，受保护的原生成员调用可以接收其 plain record 值或引用；`with_native` 中重入源码继续被拒绝。

错误时已完成的写入保留，失败不提交结果。布局依赖和参数传递方式不兼容时拒绝发布，旧代码租约仍按原版本执行；没有迁移原生布局或活动存储。

取舍依据：UE-AS 的[数组引用失效诊断配置](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptManager.h#L19-L20)限定于编辑器，[原生调用适配](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/a7890667d1abb289bc2cec16b3da075faf836f2d/Engine/Source/Runtime/CoreUObject/Public/UObject/CoreNative.h#L36-L151)仍遵守真实 C++ 签名（源码需 Unreal 访问权限）。因此共享地址、const 引用或开发配置的检查都不是完整 lifetime 证明；本项目用所有配置一致的同步借用区间与 BorrowSafe 效果声明保护地址，不把原生按值调用宣传成零复制。

## 验收结果

原生记录数组借用目前没有专项端到端验收。[`jit_acceptance`](../acceptance/jit/business.cpp) 只保留一个 bool 的只读/可写借用别名代表：写入经别名可见，相邻字节不被覆盖。规模、复制次数性能观察、拒绝矩阵与重复热更新场景已移除。

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target jit_acceptance
cmake --build --preset release --target jit_acceptance
```

该代表不证明特定吞吐提升或零调用开销。
