# Carbon 优化提交的模块复审

审查基线为 `f21f9099`。本记录保留 `e25747ac`、`85c6127f`、`ba89fcc5` 的审查发现和处理状态。以下状态截至 2026-11-18。

## 待处理

| 模块 | 证据与业务影响 | 处理时机 |
| --- | --- | --- |
| 声明式 UI 分派 | 通过 `Module::compile` 复现。组件含 63 个按钮属性绑定和一个带 60 层代码块的事件。替换为 `e25747ac` 的 UI 生成器可以编译，当前生成器把事件放在平衡分派树叶节点，触及 Source 的 128 层嵌套上限。增加兄弟绑定会改变同一事件能否编译。[UI 分派生成](../../modules/gui/ui_language/src/declaration.cpp) · [Source 嵌套检查](../../modules/execution/language/src/source.cpp) | UI 深审时保留此复现。优先看实际生成的界面是否接近上限。 |

## 已修复

| 模块 | 审查基线上的复现 | 当前行为 |
| --- | --- | --- |
| 场景加载与自定义校验器 | 临时游戏组件 `SpawnWindow` 声明 `minimum <= maximum`。从默认 `(1,5)` 将下限改成 8，编辑器拒绝并恢复 1。同样的 `(8,5)` 写入场景后，严格和容错加载都成功、诊断数均为 0，重新调用 `Runtime::validate` 才报告失败。场景加载按内建组件类别调用校验器。该分支早于 Carbon，`ba89fcc5` 又向业务类型开放了声明式校验。[场景恢复](../../modules/editor/core/src/scene.cpp) · [World 编辑](../../modules/engine/world/src/world.cpp) | 已修复。严格加载拒绝，容错加载按失败路径恢复实例默认值。保存写入修复值，随后可严格重开。 |
| 原生反射校验器签名 | Release 生成注册和手写注册都接受 `static Result<void> check(Self copy)`。用记录的复制构造函数计数，两种入口均为校验通过时复制 1 次，失败时复制 2 次。失败会重走一次以生成字段路径。现有物理、灯光和相机类型使用无参 `const` 成员校验器。[生成器](../../tools/reflection/main.cpp) · [手写注册](../../modules/execution/runtime/include/runtime/native.hpp) · [签名约定](../reflection.md) | 已修复。生成器与手写模板均在接入时拒绝按值校验。 |
| 紧凑派生类的基类偏移 | `ba89fcc5` 的生成器为对齐为 1 的派生类和对齐为 8 的非虚基类也输出 `.base<Base>(0u)`。Runtime 按直接偏移的对齐契约拒绝整类注册。省略该可选偏移后，同一绑定可以注册；另在已确认基址按 8 字节对齐的堆对象上，C++、Runtime 与 JIT 读写一致。仓内业务类型未发现采用该紧凑布局。[偏移生成](../../tools/reflection/main.cpp) · [发布检查](../../modules/execution/runtime/src/runtime.cpp) | 已修复。生成时按实际基类对齐和偏移诊断，Runtime 对手写注册的回调与直接偏移路径也检查基类对齐。 |
| UI 与 CPU 材质的冗余状态 | UI 编译器内部的 `Evaluation::operation` 和 CPU 材质的 `MaterialProgram::State::resources` 均只写无读。UI 运行时使用的操作编号、材质使用的资源值仍有读者。[UI 编译器](../../modules/gui/ui_language/src/declaration.cpp) · [CPU 材质](../../modules/execution/source/src/material_program.cpp) | 已删除两个只写成员及其初始化，事件与绑定仍保留实际分派所需的操作编号。 |
| JIT 写标记文档 | 文档称多次写入各自盖章。同步 JIT 会合并同一执行段内对同一 `self` 的重复标记。执行期间 `Runtime::change_cursor()` 返回 `busy`，消费者无法在两次写入之间推进游标，观察到的变更集合一致。[标记生成](../../modules/execution/jit/src/source_backend.cpp) · [游标实现](../../modules/execution/runtime/src/runtime.cpp) · [写代号契约](../runtime-model.md) | 已按执行段内可观察的写代号更新运行时文档。 |

### 紧凑继承的取舍

在 Ryzen 9 3950X 上用项目锁定的 Clang 23.1.5、Release 优化和 mimalloc 对照了同一组字段（先 `double` 后单字节标记）。自然布局为 16 字节、对齐 8，紧凑布局为 9 字节、对齐 1。419 万条连续记录顺序读取 `double` 时，占用分别为 64 和 36 MiB，15 轮交错测量的中位数为 0.689 和 0.476 ns/条。用 Runtime 对象所用的分配器分别独立申请 104 万个数据块，两者的平均可用块大小同为 16 字节，打乱指针顺序后的读取中位数为 3.604 和 3.507 ns/条，四分位区间重叠。测量覆盖数据布局与分配器，不包含反射调用和 GC。[对象分配](../../modules/execution/runtime/src/runtime.cpp) · [分配器接入](../../modules/core/memory/src/global_new_delete.cpp)

活的反射对象及其基类按各自 C++ 类型要求的对齐访问。当前 JIT 会按字段类型的自然对齐生成读写，原生引用和方法也会消费实际地址。若今后引入紧凑外部格式，可在格式边界按字节解码。如果连续数据确有可测收益，再针对其访问方式决定是否支持直接计算。[JIT 字段读写](../../modules/execution/jit/src/source_backend.cpp) · [原生借用](../native-value-borrows.md)

## 等待业务信号

- **Mask 材质的 CPU 拾取。** 每个候选交点调用材质求值时，会为每个已声明纹理槽扫描全部绑定。StandardPBR 的六槽对应 36 次槽号比较，旧实现同样是平方复杂度。目前只有静态工作量，若拾取耗时成为问题，再用有遮罩的场景采样。[拾取入口](../../modules/editor/core/src/viewport_tool.cpp) · [纹理绑定匹配](../../modules/execution/source/src/material_program.cpp)
- **Flags 操作符参数。** 默认组合值 `A | B` 可以编码为整数 `3`，参数描述的 `enum` 却只列出成员名。场景字段的 Flags 掩码已另外处理。首个实际使用 Flags 参数的操作符接入时，再统一参数描述和解码器接受的值。[操作符参数描述](../../modules/editor/application/src/editor/operators.cpp)

## 首轮深审结果

**原生反射的生成与注册**，从 `ACLASS`、`ASTRUCT`、`AFUNCTION` 声明，经 `atelier_reflect`、`NativeRegistration`，追到 Runtime 发布类型后第一次按元数据读字段、校验对象或调用方法。

| 业务样本 | Release 公开入口验证 |
| --- | --- |
| `world.BodySettings` | 默认值通过。`material.restitution = 2` 在值入口报告 `material.restitution`，在派生的 `CollisionComponent` 上报告 `body.material.restitution`。碰撞忽略位和重叠位相交时报告整条 `body` 规则。两类错误同时存在时先报告字段硬边界。[业务校验器](../../modules/engine/world/src/objects.cpp) |
| `CameraComponent` | 两段真实基类偏移均为 0，`vertical_fov` 偏移为 32。Runtime 写 60，JIT 增至 67，C++ 和 Runtime 均读回 67。另用非虚继承、实际基类偏移为 8 的类型验证直接偏移和回调寻址，两条路径均读写了正确字段。[相机声明](../../modules/engine/presentation/include/presentation/presentation.hpp) |
| `world.Component::translate_owner` | 在 12 次 World tick 中分别走 C++、Runtime 标量调用、Runtime 字段实参和 Source JIT。传 1、2、3 个实参，四条路径提交后的位移分别为 `(3,0,0)`、`(3,4,0)`、`(3,4,5)`。方法是私有的，直接 C++ 对照仅在临时调用方关闭访问检查。另以生成类型核对 `const float&`、`const enum&` 默认实参，四条路径的省略值和显式覆盖结果一致。[方法声明](../../modules/engine/world/include/world/world.hpp) |

审查使用临时目录和工程的 Release 产物复现上述问题。修复后的 dev 与 release 均通过 Runtime、World、Source、JIT、Physics、工程场景、编辑器文档、工程集成、材质资源及 UI 端到端验收。自定义实体、嵌套组件记录和内建相机均已验证容错恢复后的保存与严格重开。两种配置也分别验证了生成器与手写入口拒绝按值校验器、紧凑基类拒绝、自然对齐为 16 的基类可生成和编译，以及 UI 事件分派和绑定状态更新。[反射契约](../reflection.md) · [运行时优化的历史记录](runtime-and-reflection-audit.md)
