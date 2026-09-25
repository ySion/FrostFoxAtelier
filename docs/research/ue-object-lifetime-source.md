# UE 5.8.2 UObject 创建、引用与回收取证

范围：`5.8.2-release`，提交 `16d75d84714512edfb744e1fd0a59e9c74d57873`；只读 `E:\EngineReferences\UnrealEngine`。参考根无 `.codegraph`，以下由 `rg` 沿调用链定位。本文不覆盖类型注册或 BlueprintGeneratedClass。

## 已证实的事实

1. **`NewObject` 不会把返回对象自动变为 RAII 根。**模板入口调用 `StaticConstructObject_Internal`；后者先 `StaticAllocateObject`，再执行 `ClassConstructor`，解锁重建 GC guard 后直接返回裸 `UObject*`。新对象在 `UObjectBase` 构造期间已经 `AddObject` 进入全局表，但“可寻址/已发布”不等于“由根保活”。
   - [UObjectGlobals.h:1953,1968,1991](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/UObjectGlobals.h:1953)
   - [UObjectGlobals.cpp:4803-4854,4874](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:4803)
   - [UObjectGlobals.cpp:3764-3772](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:3764)
   - [UObjectBase.cpp:154-170](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectBase.cpp:154)

2. **构造期 GC 保护是特定重建分支的同步，不是一般 NewObject 的所有权。**`FGCReconstructionGuard` 仅在覆写旧对象且仍 async-loading 的条件下取得 GC lock；`StaticConstructObject_Internal` 将 guard 持有至类构造完成。普通新分配没有由此获得持久根；并且 GC 锁定 UObject hash table 时禁止创建。
   - [UObjectGlobals.cpp:3348-3395,3620-3645,4843-4854](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:3348)
   - [UObjectGlobals.cpp:3484-3485](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:3484)

3. **强保活有显式机制。**`TStrongObjectPtr` 构造/Reset 调用 `AddRef`，析构/Reset 调用 `ReleaseRef`；对象项 refcount 从 0 到 1 时设 dirty root，若增量可达性正进行会 `MarkAsReachable`。这是堆外 RAII 根，不是 `NewObject` 的返回契约。
   - [StrongObjectPtrTemplates.h:21-25,47-60,122-174](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/Core/Public/UObject/StrongObjectPtrTemplates.h:21)
   - [UObjectArray.h:347-365,375-395](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/UObjectArray.h:347)
   - [GarbageCollection.cpp:6644-6661](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp:6644)

4. **对象内强边由 GC schema/回调收集，`TObjectPtr` 本身不是脱离 schema 的“全局根”。**可达性访问器先 `AddPropertyReferences(Class,Object,...)`，再调对象的 `CallAddReferencedObjects`；`FObjectProperty::EmitReferenceInfo` 把属性写入 schema。非 UObject 宿主则须继承/注册 `FGCObject`，在其 `AddReferencedObjects` 向 collector 报告要保活的对象；该注册器本身由 root-set 中的 `UGCObjectReferencer` 转发。
   - [GarbageCollection.cpp:4008-4012,6837-6845](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp:4008)
   - [GCObject.h:127-146,175-195](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/GCObject.h:127)
   - [GarbageCollection.cpp:6560-6575](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp:6560)

5. **弱引用是索引+代际而非保活边。**从对象赋值时写入 global object array 的 index 与 serial；检查会比较当前 serial。`Pin` 在 `FGCScopeGuard` 下验证 object item 后才构造 `TStrongObjectPtr`；`TryPin` 的 try-lock 失败会返回“无法安全 pin”。`PinEvenIfUnreachable` 是特殊 API，不应混同普通提升。
   - [WeakObjectPtr.cpp:29-48,138-208](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/WeakObjectPtr.cpp:29)
   - [WeakObjectPtr.h:417-438,442-562](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/WeakObjectPtr.h:417)

6. **不可达、显式销毁阶段、索引失效、内存回收不同。**`RF_Unreachable`/内部 unreachable 先作为即将删除的全局状态；随后 `ConditionalBeginDestroy`、等待 `IsReadyForFinishDestroy`、`ConditionalFinishDestroy`。FinishDestroy 后重置 serial，因此弱引用不能再访问；最后 purge 在析构前 `FreeUObjectIndex`，才回收对象项/内存。
   - [GarbageCollection.cpp:6511-6516,4934-4980,5187-5225,877-929](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp:6511)
   - [Obj.cpp:1299-1370,1373-1389](/E:/EngineReferences/UnrealEngine/Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp:1299)

## 对 FrostFoxAtelier 最小闭环的推论

- 借鉴：将 `StrongRef`、`WeakRef`、`RootHandle` 与对象内 `Trace`/类型 schema 分开。UE `NewObject` 返回裸指针且不 root 是事实；本项目仍可有意加强为“创建入口交付已登记根”，这是本项目的交付保证，并非 UE 行为。当前单线程、显式回收下没有已存在的创建—root 窗口漏洞；根写入与强边写入仍应各有唯一入口，日后增量标记才能挂接 barrier。
- 借鉴：弱引用保存稳定 `ObjectId` 与 generation；提升必须在 GC/生命周期协调区中原子验证并取得强引用。项目要求“失效引用保留身份”，故可让失效 `WeakRef` 保留 `(ObjectId,generation)` 供诊断/比较，但 `TryUpgrade` 必须失败，不能复活对象。
- 不照搬：UE global UObject array、反射属性 schema、`FGCObject` 和 async-loading lock 服务其 UObject/编辑器生态。项目 nonmoving 可避免地址转发，但仍要明确标记完成、析构、释放与 identity tombstone 的顺序；未来 VM 应调用同一 runtime 的 Trace/Root API，而非另建脚本包装图。

## 未覆盖

- 未核查 BlueprintGeneratedClass 构造/类型注册，也未把配置分支（`UE_WITH_REMOTE_OBJECT_HANDLE`、编辑器重建）推广为一般运行时契约。
