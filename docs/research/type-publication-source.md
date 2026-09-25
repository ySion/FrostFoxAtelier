# 类型发布：自引用、互相引用与“可用”的源码证据

核查日期：2026-09-21。只读 Unreal Engine 5.8.2 与 Godot 4.7.2 的本地检出；“可借鉴”及 FrostFoxAtelier 提案不是两者的既有事实。

## 先给结论

- UE 和 Godot 都有“先建立某种描述，再完成后续步骤”的分层，但**没有证据表明它们为一组任意互引类型做全组原子发布**。当前 `register_type` 的“target 必须已注册”限制，不能靠宣称与它们一致来绕过。
- UE 原生 `UClass` 的继承依赖是严格先注册 base；可延后的是属性 Link、蓝图加载/默认对象等后续完成。Godot 原生 `ClassDB` 同样要求 base 已存在；GDScript 则把 parser class 的 inheritance / interface / body 分阶段解析，专门允许 script preload 环。
- FrostFoxAtelier 若要接受 `A → B → A` 的**引用**边，应增加自己的 batch 事务。它与 UE/Godot 的逐类注册或脚本解析分阶段有关联，但原子公开、内联布局环检查和 rollback 都是本项目新契约。

## Unreal Engine：注册、Link 与默认对象不是一个原子事务

1. `InitializePrivateStaticClass` 先设 `SuperStruct`/`ClassWithin`，调用 `RegisterDependencies()`，再 `Register()`；源码还明确注释“不允许递归 `StaticClass`”。因此原生类注册以父类先行，而非用占位符解任意互相继承。证据：[Class.cpp:127-155](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp:127)。
2. `UStruct::RegisterDependencies` 递归调用 `SuperStruct->RegisterDependencies()`；这证明继承链的顺序约束，但没有提供任意类型组的 prepare/commit API。证据：[Class.cpp:711-722](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp:711)。
3. `UStruct::Link` 在已有描述上计算继承后的 `PropertiesSize`、alignment 并逐 property `Link`；`FStructProperty::LinkInternal` 必须 preload 目标 struct 才取其 size/alignment。故“类型对象已存在”不等于内联布局已完备。证据：[Class.cpp:854-923](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp:854)、[PropertyStruct.cpp:94-114](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/PropertyStruct.cpp:94)。
4. UE 在 `UScriptStruct::Link` 中仅对“直接 `FArrayProperty`，其 `Inner` 是 `FStructProperty`，且该 `Struct` 就是当前 struct”直接 fatal，并写明“不支持”。这是反射 property 系统的实现限制；动态数组自引用本身不证明无限内联尺寸。互相嵌套的所有形式是否均在这一层统一拒绝，所读源码**未核实**，不能外推。证据：[Class.cpp:924-943](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp:924)。
5. 蓝图 CDO 创建前会 preload property chain 并 `StaticLink(true)`；遇到循环加载重入后还会重新检查 CDO 是否已被创建，随后 release fence 才存 `ClassDefaultObject`。这说明 class descriptor、linked layout 和可实例化默认对象可处于不同完成阶段；它不是跨类 batch commit。证据：[Class.cpp:5044-5102](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Class.cpp:5044)。
6. `UBlueprintGeneratedClass::Link` 在基类 Link 后才组装 reference token stream；其存在支持“动态 class 后续 link”的判断，但未提供蓝图组原子发布证据。证据：[BlueprintGeneratedClass.cpp:2335-2364](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/Engine/Private/BlueprintGeneratedClass.cpp:2335)。

**可借鉴。** 将“已分配 type identity / descriptor”“已解析继承与引用”“已完成 inline layout”“可创建对象”设为不可混淆的内部状态；任何实例化入口只接受最后一态。不要照抄 UE 的递归 `StaticClass`/加载模型，也不要把其 `DeferredRegister` 名称误读成全组事务。

## Godot：ClassDB 原生注册与 GDScript 解析是两套机制

7. `ClassDB::_add_class` 在 write lock 内插入 `ClassInfo`，但若有父类会立即断言父类已经在表；`register_class<T>` 再在同一类注册路径初始化并写 creator/exposed/class pointer。它是原生类的顺序注册，并非互相继承或跨类原子发布。证据：[class_db.cpp:935-953](/E:/EngineReferences/Godot/core/object/class_db.cpp:935)、[class_db.h:245-257](/E:/EngineReferences/Godot/core/object/class_db.h:245)。
8. GDScript 的 analyzer 明确分 `resolve_inheritance`、`resolve_interface`、`resolve_body`、`resolve_dependencies`；interface 先置 `resolved_interface`，再解继承、base interface 和成员，body 有独立 `resolved_body` 防重入。这里“名称/AST 类型可先解析、body 后完成”确实成立，但只适用于脚本编译流程。证据：[gdscript_analyzer.cpp:1268-1405](/E:/EngineReferences/Godot/modules/gdscript/gdscript_analyzer.cpp:1268)、[gdscript_analyzer.cpp:6623-6669](/E:/EngineReferences/Godot/modules/gdscript/gdscript_analyzer.cpp:6623)。
9. GDScript 对 script preload 特意浅载入以允许 cyclic references，但继承环会报错；测试也覆盖两个脚本互相 preload 的引用环。故“引用依赖环可暂存”与“继承/须求定值的解析环可接受”是不同规则。证据：[gdscript_analyzer.cpp:4755-4764](/E:/EngineReferences/Godot/modules/gdscript/gdscript_analyzer.cpp:4755)、[gdscript_analyzer.cpp:609-617](/E:/EngineReferences/Godot/modules/gdscript/gdscript_analyzer.cpp:609)、[preload_cyclic_reference_a.notest.gd:1](/E:/EngineReferences/Godot/modules/gdscript/tests/scripts/analyzer/features/preload_cyclic_reference_a.notest.gd:1)。
10. `GDScript::reload` 先置 `valid = false`，parse/analyze 成功后才 compile；失败直接返回。它说明 script resource identity 与可运行脚本可分离，但**未读出**可直接对应 C++ 原生类型表的公开原子性。证据：[gdscript.cpp:737-850](/E:/EngineReferences/Godot/modules/gdscript/gdscript.cpp:737)。

**可借鉴。** 把“声明壳可互指”和“有可执行/可布局 body”分开；用显式 resolved flags 和 DFS/SCC 诊断环，而非在注册时要求每个 target 已公开。GDScript 没有与本项目 record inline layout 相同的来源证据，不能用它证明何种 value-layout 环合法。

## FrostFoxAtelier：建议的 batch 原子发布（本项目提案）

现状是 `register_type` 为每个 field 立即 `lookup(field.type)`，缺失或 object target 都失败，因此只能引用已注册 scalar/record。[runtime.cpp:198-252](/E:/FrostFoxAtelier/src/runtime/runtime.cpp:198)。

1. `begin_type_batch` 收集 definitions 并建立**私有** staging descriptor；外部 `lookup`、对象创建和 native callback 都看不到它。
2. 先验重名、native representation、field/method 名称和类型 kind；随后解析 target 于“已公开表 ∪ staging 表”。引用/观察边允许成环；仅沿当前支持的 inline-value edge 建图，拒绝自环或包含多个节点的强连通分量，再拓扑计算 size/alignment。
3. 全部 Link、native layout 校验与方法表构建成功后，在注册封闭前统一提交到同一权威类型表；失败销毁 staging，不留下对外可见的半成品。提交后对象创建才可见该批每个 type。

这借鉴 UE 的“descriptor 与 Link/实例化分离”和 Godot 的“interface/body 分阶段及环分类”，但**batch 原子性、inline-SCC 规则、失败无可见半成品**均不是上述源码的既成机制，须作为 FrostFoxAtelier 的明确新契约实现与验收。
