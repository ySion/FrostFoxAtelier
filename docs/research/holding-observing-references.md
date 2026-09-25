# UE 与 Godot：持有、观察与显式销毁的事实核查

核查日期：2026-09-21。仅查阅 UE 5.8 与 Godot stable 官方文档，未运行项目或检视源码；结论只覆盖各页面明示的对象类别和指针/引用形式。

1. **UE：`TObjectPtr` 是否保持 `UObject` 可达取决于被 GC 追踪。** Epic 文档说明，`TObjectPtr<T>` 只有标为 `UPROPERTY` 时才 GC-safe；该形式是影响 GC 的强引用，会阻止 GC 销毁目标。用于非 `UObject` 持有者的 `TStrongObjectPtr` 也会保持目标存活，但这不是“任意 C++ 指针都保活”。[Object Pointers](https://dev.epicgames.com/documentation/unreal-engine/object-pointers-in-unreal-engine)

2. **UE：`TWeakObjectPtr` 是不保活的观察指针。** 它不阻止 GC；官方文档把目标被回收或销毁后的表现描述为变为 null，并要求检查 `IsValid()` 或 `Get()` 的返回值。这是使用接口的可观察结果，本轮未核实句柄内部标识是否被清零。该页还建议在工作线程通过 `Pin()` 取得 `TStrongObjectPtr`，而非仅作一次有效性检查。[对象指针文档](https://dev.epicgames.com/documentation/unreal-engine/object-pointers-in-unreal-engine)

3. **UE：Actor 的逻辑销毁与内存回收分离，具体回收受版本与设置影响。** Actors 页说明 `Destroy()` 从 level 移除 Actor 并标记 pending kill，内存由后续 GC 清理。5.8 Objects 页同时区分 `gc.PendingKillEnabled` 启用时的标记后回收与禁用时的引用保活行为，并描述已移出关卡的 Actor 等待引用释放的情形。因此可以参考“显式移除与物理回收分开”，不能把强引用在任何设置下都延迟已销毁对象回收当作通则。[Actors](https://dev.epicgames.com/documentation/unreal-engine/actors-in-unreal-engine)；[Objects](https://dev.epicgames.com/documentation/unreal-engine/objects-in-unreal-engine)

4. **Godot：仅 `RefCounted` 系列由引用计数自动管理。** `RefCounted`（及其子类如 `Resource`）内部维护计数，无引用时自动释放；循环强引用不会自动释放。此规则不可推广到一般 `Object`。[RefCounted](https://docs.godotengine.org/en/stable/classes/class_refcounted.html)

5. **Godot：`WeakRef` 对 `RefCounted` 不增加计数，对普通 `Object` 不产生保活效果。** `weakref()` 创建的 `WeakRef` 在目标不再存在时，`get_ref()` 返回 `null`；目标若非引用计数对象，文档说明 weakref 仍可创建但不影响该对象。[WeakRef](https://docs.godotengine.org/en/stable/classes/class_weakref.html)

6. **Godot：一般 `Object` / `Node` 可显式销毁并使既有引用失效。** `Object.free()` 删除对象，既有引用失效，访问会报运行时错误；`Node.queue_free()` 则在当前帧末删除该节点及子节点，删除后相关引用也失效。Godot 文档未把普通 Object 变量描述为延长其生命周期的持有引用，反而要求以 `is_instance_valid()` 检查已删除对象。[Object.free](https://docs.godotengine.org/en/stable/classes/class_object.html)；[Node.queue_free](https://docs.godotengine.org/en/stable/classes/class_node.html)

## 静态核查边界

本页没有证明 UE 的所有 `UObject` 子类、所有 `UPROPERTY` 容器或 GC 设置的细节，也没有证明 Godot 每一种语言绑定、Variant 传递路径或 `RefCounted.free()` 的所有边界行为。尤其不能把“强引用保活”外推为必然阻止显式逻辑销毁，也不能把 Godot 的 `RefCounted` 规则外推至全部 `Object`。
