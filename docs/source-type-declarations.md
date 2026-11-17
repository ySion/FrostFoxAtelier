# 源码类型声明

状态：已实现，并通过 dev/release 的相关端到端验收。

源码通过现有 `source::Program` 声明枚举、结构体和原生对象的派生类。结构体字段可组合已注册的 String、具体 Array 和无 GC 引用记录。Runtime 负责类型身份、布局、反射及值生命周期；JIT 使用同一份字段偏移和原有存储。语法以 [LANGUAGE.md](../LANGUAGE.md) 为准。

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

宿主已注册 String 和 `Array<float>` 后，也可编译以下值组合：

```cpp
struct gameplay.Batch { String name; Array<float> samples; };
uint process(const gameplay.Batch& input) {
    gameplay.Batch local = input;
    local.name = "temporary/batch";
    local.samples.add(1.0);
    return local.samples.size();
}
```

`input` 借用宿主存储；`local` 拥有独立副本，其字段在函数退出时销毁。新源码类型的发布遵守下面的时序。

## 准备与发布

1. 请求收集类型声明，捕获源名、正文、入口和类型元数据。声明扫描与完整编译共用词法和声明解析代码。
2. Runtime 的内部类型批次完成名称解析、循环检查、布局与生命周期能力计算。候选类型尚不可被 `find_type` 查询，也不能用于创建值或对象。原生 `register_types` 复用同一批次实现。
3. 完整函数编译、LLVM 校验和链接消费候选布局。声明或函数错误由 `prepare` 返回，已有类型与代码不变；候选和请求释放时直接丢弃未发布定义。
4. 发布检查请求顺序、程序身份、Runtime 静止状态及捕获的目录。新类型还要求类型安全点，即代码发布条件加 GC 空闲；忙状态保留候选。仍含新类型的候选在目录变化时被拒绝，防止预备身份被重新解释，宿主需重新请求；已发布程序的代码更新只复核其捕获依赖，其他模块新增无关类型不改变其目录。
5. 签名解析、元数据与代码版本分配、类型容器预留全部完成后，提交类型和代码；提交部分不执行用户回调，也不再分配。

创建对象后仍可发布包含新声明的版本。已发布的源码类型声明不可变：声明在文件中的顺序可以调整，字段名称、类型和顺序、枚举底层类型及枚举项必须保持一致。新版本可以追加 struct/enum，新类型可以按名称引用已发布类型；删除、改名或改变已发布定义返回错误，需要重建 Runtime。关闭 Program、退休代码或释放旧租约均不注销已发布类型。

## 值与借用

- 枚举底层为 `int`（默认）或 `uint`，可前置 `[Flags]` 声明为无符号位掩码枚举（要求 `: uint`，支持同类型 `&/|/^/~` 按位运算）；枚举项前可附加 `[Label = "...", DESC = "..."]` 展示元数据，枚举值可指定十进制整数，否则从零或上一项加一；溢出和重复项名拒绝。
- 结构体字段支持 `float/double/uint/int`、内建数学值、已注册或同模块枚举、同模块结构体，以及已注册的无 GC 引用记录、String 和具体 Array。支持前向引用，内联布局循环拒绝。字段、参数与局部共用类型名称解析；`String`、数学固定短名和 `Array<T>` 解析既有身份，数组类型仍由原生生产者注册。
- 结构体局部使用 Runtime 值操作进行构造、深复制、赋值和清理。自有结构体的嵌套记录、String/Array 字段及元素可整值赋值或修改容器结构，目标须满足已有安全值操作能力；借用结构体允许可写字段叶值和完整数学字段修改，不能替换一般聚合字段或改变容器结构。
- 默认构造递归将标量置零、String/Array 置空，并调用原生字段构造。字段随记录逆序销毁；部分构造失败时 Runtime 回收已构造字段，生成代码只负责已完成构造的局部，避免重复清理。正常块退出、提前返回和跨函数失败均沿既有作用域清理链。
- 原生与描述型记录通过同一目录投影和调用检查。只有具有保持借用有效的复制与生命周期能力的记录可成为拥有型局部；描述型记录的该能力由全部字段组合得出。
- `const T&` 和 `T&` 保持原有只读、别名和有效期契约；拥有型结构体沿用同步值执行区间，失败逐层清理。

这是 RAII 作用域拥有值：局部本体使用生成栈存储，String/Array 的动态缓冲仍由基础库管理。它不要求缓冲也在栈上，和 C# `stackalloc` 的区别见[拥有值执行链](source-owned-values.md#统一执行链)。

同步函数允许普通源码或原生结构体按值参数与返回，资格由共享 Runtime 类型身份上的 `plain_value` 判定：递归字段只包含标量、枚举、内建数学值与其他 plain record，且复制平凡。普通记录还必须具备 `can_own`，保证构造、复制和销毁保持借用安全；两项资格独立，前端在签名编译时检查。含 String、Array、引用或资源的记录仍只支持既有拥有局部与借用路径，不能按值跨调用边界。

按值参数拥有独立副本，可以整体重赋值或修改叶字段。返回临时支持赋值、字段读取、传值和只读借用，生命周期覆盖完整表达式，不能绑定可写引用或逃逸。源码与原生记录都使用准确 Runtime 类型身份；相同字段或布局不能替代类型身份，也不会把普通记录归类为内建数学值。公开记录结果使用统一 `invoke` 的 `ValueOutput`，输出类型必须匹配已绑定的 Runtime 类型。

当前不包含结构体中的对象引用、自动实例化 `Array<源码结构体>`、自定义构造析构、命名空间块和布局迁移。跨文件与原生派生类通过项目快照收集。[内建数学值](math.md)具有独立的定型计算支持，可同步按值传递与返回，但不使任意同布局记录取得相同编译器分类。用户类型名使用准确的声明名，可以写成 `combat.DamageInfo`；同作用域短名推导尚未开放。声明不能占用 `core.*`、`math.*` 或内建数学短名。

## 原生对象的源码派生类

项目快照中的 class 使用同一 `TypePublication` 事务准备，继承检查只查询事务类型目录；预览 TypeId 不进入正式 Runtime 查询或对象操作。发布后 `Runtime::create` 直接构造原生基类和描述型字段，不需要宿主调用第二个初始化入口。`FieldInfo::default_leaves` 描述标量、向量和枚举默认值，`default_text` 描述真实 String 字段默认文本，`range/label/category/description/metadata/transient/read_only` 来自字段属性；原生字段仍由 C++ 构造函数负责，规则见[字段默认值](runtime-model.md#字段默认值)与[扩展元数据](runtime-model.md#字段扩展元数据)。String 字段仅供宿主反射、保存与重建使用，不向源码开放 `self` 上的 String 访问或借用。

已发布 class 的布局、字段默认值、范围、展示文本、`Transient`/`ReadOnly` 标记和 Meta 内容冻结；后续版本可以追加 class 和已有 class 的新方法。热更新保留已创建对象的字段值、地址和身份；关闭源码 Program 不注销类型。语法、规范化兼容比较、重建限制和具名生命周期入口见 [LANGUAGE.md](../LANGUAGE.md#项目快照与原生派生类)，宿主发布示例见[项目快照](source-function-modules.md#项目快照与多个公开入口)。

## 验收

源码 enum/struct 与 C++ 共享存储、值字段组合的故障清理目前没有专项端到端验收。

[项目消费者](../acceptance/source/projects.cpp)随 `source_acceptance` 无窗口运行，保留当前磁盘格式的保存读回与跨文件 World tick；[工程集成](../acceptance/editor/project_execution.cpp)随 `project_integration_acceptance` 检查一次跨文件 Source 组件 tick。它们不改变持久格式。
