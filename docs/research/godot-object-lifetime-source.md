# Godot 对象、GDScript 实例与生命周期：本地源码事实

核查日期：2026-09-21。只读检出
`E:\EngineReferences\Godot`，`HEAD = ed1daf0bf001b61586d9930840f2f1394092c079`
（Godot 4.7.2-stable）。未发现 `.codegraph/`，按项目规则使用 `rg` 定位。范围仅为
`core/` 与 `modules/gdscript/` 的运行时对象/持有链；不是 FrostFoxAtelier 的既定设计。

## 已核实事实（9 项）

1. **GDScript 资源的 `new()` 不是独立脚本堆对象。** `GDScript::_bind_methods()` 把
   `new` 绑定到 `GDScript::_new`；后者沿脚本继承链取最顶层 native 基类，调用其
   `instantiate()`；无 native base 时默认 `memnew(RefCounted)`。
   [gdscript.cpp:210-241,1077-1079](/E:/EngineReferences/Godot/modules/gdscript/gdscript.cpp:210)。

2. **native 创建仍回到 ClassDB，随后才附接解释器侧实例。**
   `GDScriptNativeClass::instantiate()` 调用 `ClassDB::instantiate_no_placeholders(name)`；
   `_new()` 对 `RefCounted` 转成 `Ref<RefCounted>`，否则保留 `Object*`，再调用
   `_create_instance()`，最后返回相应 `Variant`。
   [gdscript.cpp:94-108,227-253](/E:/EngineReferences/Godot/modules/gdscript/gdscript.cpp:94)。

3. **`GDScriptInstance` 是 owner 上的独立 sidecar。** `_create_instance()` 分配该对象，
   保存 `owner` 原始指针、`owner_id` 与 `Ref<GDScript>`，调整 `members` 大小，并以
   `owner->set_script_instance(instance)` 附接；随后运行隐式初始化与 `_init`。
   [gdscript.cpp:154-207](/E:/EngineReferences/Godot/modules/gdscript/gdscript.cpp:154)。替换/销毁 owner
   时 `Object` 用 `memdelete` 删除 sidecar，且 `_predelete()` 明确先删脚本实例。
   [object.cpp:171-200,973-1008](/E:/EngineReferences/Godot/core/object/object.cpp:171)。

4. **脚本实例字段不是 ClassDB 的原生字段布局。** `GDScriptInstance` 的实际存储是
   `Vector<Variant> members`；以脚本的 `member_indices` 查到 index 后直接读写该 vector
   （除非该成员已有 GDScript getter/setter）。而 `Object::set/get` 先派发
   `script_instance`，失败后才进入 ClassDB 的 native property 路径。
   [gdscript.h:347-408](/E:/EngineReferences/Godot/modules/gdscript/gdscript.h:347)；
   [gdscript.cpp:1536-1558,1609-1621](/E:/EngineReferences/Godot/modules/gdscript/gdscript.cpp:1536)；
   [object.cpp:233-261,316-344](/E:/EngineReferences/Godot/core/object/object.cpp:233)。

5. **VM 局部值遵循 `Variant` 的即时持有规则。** 调用栈为 `alloca` 的 `Variant` 数组，
   参数以 `Variant(*p_args[i])` 构造，退出时逐项析构；`await` 会把这些 `Variant` 复制到
   `GDScriptFunctionState`。所以临时/局部值不是由一次脚本 GC 扫描发现。
   [gdscript_vm.cpp:535-588,2603-2642,4023-4036](/E:/EngineReferences/Godot/modules/gdscript/gdscript_vm.cpp:535)。

6. **同为 `Variant::OBJECT`，`RefCounted` 与普通 `Object` 的保持语义不同。** ObjData
   同时存 `ObjectID` 与 `Object*`；复制/构造时只对 `id.is_ref_counted()` 的对象执行
   `reference()/init_ref()`，清理时对应 `unreference()` 并在归零后 `memdelete`。普通
   `Object` 不递增引用计数，清理 Variant 也不会删除它。
   [variant.h:169-186](/E:/EngineReferences/Godot/core/variant/variant.h:169)；
   [variant.cpp:1074-1127,1417-1419](/E:/EngineReferences/Godot/core/variant/variant.cpp:1074)。

7. **普通 Object 的 Variant 可保留失效身份，并另有原生取指针快路径。**
   `get_validated_object[_with_check]()` 通过 `ObjectDB::get_instance(id)` 返回 null 并能报告
   previously-freed；`operator Object*()` 直接返回保存的 raw pointer。这是须由调用方满足
   生命周期条件的原生取指针路径；本轮未核查所有调用方，不能据此评判 GDScript 解引用安全。
   [variant.cpp:2024-2048](/E:/EngineReferences/Godot/core/variant/variant.cpp:2024)。

8. **ObjectDB 的 ID 防止槽复用误认同一对象。** 创建把 slot、递增 validator 与
   ref-counted bit 编入 ObjectID；删除清空 slot validator/object；查询须匹配 validator。
   旧 ID 因而解析为 null，即使同一 slot 已被另一对象复用。
   [object.cpp:2284-2318,2451-2492,2495-2525](/E:/EngineReferences/Godot/core/object/object.cpp:2284)；
   [object.h:908-928](/E:/EngineReferences/Godot/core/object/object.h:908)。

9. **回收模型是 RC 加显式销毁，不是 tracing GC。** `Ref<T>` 和 `Variant` 在
   `unreference()` 返回真时立即 `memdelete`；`Object.callp("free")` 对非 RefCounted
   直接 `memdelete(this)`（调试配置拒绝 free RefCounted）。已检查的 RefCounted 路径只做
   计数/零计数销毁，不追踪对象图；互相强持有的 RefCounted 循环因此没有在此路径中被打破。
   [ref_counted.h:199-228](/E:/EngineReferences/Godot/core/object/ref_counted.h:199)；
   [ref_counted.cpp:74-126](/E:/EngineReferences/Godot/core/object/ref_counted.cpp:74)；
   [object.cpp:768-795](/E:/EngineReferences/Godot/core/object/object.cpp:768)。

## 对 FrostFoxAtelier 的影响

- 可借：将稳定身份做成 `(slot, generation)`，让失效观察引用仍保留旧身份、公开/跨边界
  解析走验证表；这符合 nonmoving 对象与“失效引用保留身份”。项目也可保留可信 native
  快路径，但应把其生命周期前提写成契约，不能让公开或脚本侧引用隐式依赖它。
- 不应借：Godot 把 native ClassDB 对象、解释器 `GDScriptInstance` sidecar、`Variant` 的
  RC/普通 Object 两套保持语义并置。它不能证明“脚本字段已与 native ClassDB 统一布局”，
  也不能作为共享类型体系或 nonmoving tracing GC 的实现蓝本。
- 若采用追踪 GC，脚本 VM locals、await 保存状态、脚本成员与原生强句柄都应成为同一准确根/
  字段契约；不能仅仿照 Godot 的 Variant 析构来维持可达性。RC 可作为资源/外部所有权工具，
  但循环与跨语言图仍须追踪方案处理。

## 本轮未核实

- GDScript 编译器如何为每一个成员产生 index/字节码访问，以及 native 类型声明、解析、
  编译缓存与注册发布链（已按分工留给另一项核查）。
- `Node.queue_free()` 的延迟销毁调度和扩展绑定的完整线程竞态；本文只确认 `Object.free`
  与 `RefCounted` 的核心路径。
- `ObjectDB` 查询返回 raw pointer 后，调用者跨线程持有该 pointer 的完整同步协议；不能把
  validator 校验误读为自动的并发生命期保护。
