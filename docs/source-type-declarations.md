# 源码枚举与结构体

状态：已实现，并通过 dev/release 的相关端到端验收。

源码通过现有 `source::Program` 声明枚举和简单结构体。Runtime 负责类型身份、布局、反射及值生命周期；JIT 使用同一份字段偏移和原有存储。语法以 [LANGUAGE.md](../LANGUAGE.md) 为准。

## 使用方式

```cpp
enum combat.Kind : uint { Physical, Fire = 7, Ice }
struct combat.DamageInfo {
    combat.Point point;
    combat.Kind kind;
    uint amount;
};
struct combat.Point { float x; int count; };

uint process(combat.DamageInfo& damage) {
    combat.DamageInfo before = damage;
    damage.amount = damage.amount + 4;
    damage.kind = combat.Kind::Fire;
    return before.amount;
}
```

宿主仍调用 `Program::request`、`Program::prepare` 和 `try_publish`。成功发布后，通过 `Runtime::find_type/describe/find_field` 取得同一类型及字段信息；按 `TypeInfo::size/alignment` 提供存储，使用 `initialize_value/copy_construct_value/assign_value/destroy_value` 管理活值。`Argument::borrow(type, data, writable)` 将该存储传入源码或原生调用，引用写入直接对宿主可见。

这些描述型结构体没有对应的 C++ 静态类型。宿主依据正式元数据访问存储；布局相似不授权将它强转成另一个已注册的 C++ 类型。枚举通过 `EnumValue` 保留 Runtime 类型身份。

## 准备与发布

1. 请求收集类型声明，捕获源名、正文、入口和类型元数据。声明扫描与完整编译共用词法和声明解析代码。
2. Runtime 的内部类型批次完成名称解析、循环检查、布局与生命周期能力计算。候选类型尚不可被 `find_type` 查询，也不能用于创建值或对象。原生 `register_types` 复用同一批次实现。
3. 完整函数编译、LLVM 校验和链接消费候选布局。声明或函数错误由 `prepare` 返回，已有类型与代码不变；候选和请求释放时直接丢弃未发布定义。
4. 发布检查请求顺序、程序身份、Runtime 静止状态及捕获的目录。新类型还要求注册未封闭、GC 空闲；忙状态保留候选。仍含新类型的候选在目录变化时被拒绝，防止预备身份被重新解释；已发布程序的代码更新只复核其捕获依赖，其他模块新增无关类型不改变其目录。
5. 签名解析、元数据与代码版本分配、类型容器预留全部完成后，提交类型和代码；提交部分不执行用户回调，也不再分配。

类型注册仍在首次成功创建对象后封闭。包含新声明的程序应在该点之前完成首次发布。现有类型声明和函数体可在封闭后重新编译；代码更新继续遵守既有静止点协议。

首次成功发布固定该程序的源码类型声明集合。类型声明在文件中的顺序可以调整；字段名称、类型和顺序、枚举底层类型及枚举项必须保持一致。增删类型或改变定义需要重建 Runtime。关闭 Program、退休代码或释放旧租约均不注销已发布类型。

## 值与借用

- 枚举底层为 `int`（默认）或 `uint`，枚举项可指定十进制整数，否则从零或上一项加一；溢出和重复项名拒绝。
- 结构体字段支持 `float/uint/int`、已注册或同模块枚举，以及同模块结构体。支持前向引用，内联布局循环拒绝。字段由 Runtime 默认构造；原有标量默认值为零。
- 结构体局部使用 Runtime 值操作进行构造、复制、赋值和清理。自有结构体的嵌套记录字段也可整值赋值；对借用结构体仍只开放字段叶值修改，不能整值替换调用者的聚合值。
- 原生与描述型记录通过同一目录投影和调用检查。只有具有保持借用有效的复制与生命周期能力的记录可成为拥有型局部；描述型记录的该能力由全部字段组合得出。
- `const T&` 和 `T&` 保持原有只读、别名和有效期契约；拥有型结构体沿用同步值执行区间，失败逐层清理。

当前不包含源码 class、结构体中的 String/Array 或对象引用、结构体按值参数/返回、自定义构造析构、命名空间块、跨文件声明收集和布局迁移。类型名使用准确的声明名，可以写成 `combat.DamageInfo`；同作用域短名推导尚未开放。

## 验收

[公共消费者](../acceptance/source/types.cpp)覆盖正式 Program、Runtime 和原生 Invoker：

- 前向嵌套结构体、枚举显式/隐式值和整数边界、反射布局及身份。
- C++ 构造存储、源码引用原地写入、局部复制与嵌套赋值、原生方法读取、枚举参数与结果。
- 准备时不发布类型、GC 忙状态重试、函数或定义失败、布局循环、只读拒绝、过期候选与外部目录变化。
- 类型定义冻结、声明重排、多个独立模块追加类型、注册封闭后的代码更新、旧租约以及关闭代码后类型存活。

运行入口为 `source_acceptance`；核心改动同时运行 dev/release 的 Runtime、基础库、反射、JIT、World 与无窗口编辑器文档消费者。没有增加性能基准或宣称吞吐收益。

本阶段 dev/release 的上述消费者均已通过，日志分别为 `out/source-types-final-dev.log` 与 `out/source-types-final-release.log`。图形窗口和 GPU 验收不在本次核心类型改动范围内。
