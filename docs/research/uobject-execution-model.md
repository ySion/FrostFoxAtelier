# UE UObject 反射类型与执行路径：静态事实核查

核查日期：2026-09-21。这里的“UObject World”仅按口语理解为 UE 的 UObject/反射/脚本体系；[UWorld](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UWorld) 则是表示地图或场景的具体类。资料限于 Epic 官方 UE 5.8 文档与官方技术博客；没有运行引擎、也没有解读整个引擎源码。

对象体系的职责不限于调用：引擎可依据已登记的属性和类型信息提供编辑器属性访问、序列化、运行时类型查询与引用管理。原生 C++ 成员不必全部进入反射；这些服务有各自的规则，不能仅凭有一份类型描述就推定所有功能自动完备。[Unreal Object Handling](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-object-handling-in-unreal-engine)；[Objects](https://dev.epicgames.com/documentation/en-us/unreal-engine/objects-in-unreal-engine)

1. **`UClass` / `UBlueprintGeneratedClass` 是共享的类型描述层，不等于一个 VM。** 蓝图编译会把 Blueprint 的变量和函数建立为 `UClass` 作用域上的属性与 `UFunction`，再填充属性链、函数映射、大小和元数据；`UBlueprintGeneratedClass` 是该类的编译产物并带有 UFunction/uber graph 等数据。[Blueprint Compiler Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/compiler-overview-for-blueprints-visual-scripting-in-unreal-engine)；[UBlueprintGeneratedClass API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UBlueprintGeneratedClass)

2. **蓝图函数使用同一套函数描述，但其后端是字节码。** 编译器为函数创建 `UFunction` 及其属性，随后 VM backend 将编译语句转为字节码并序列化到该函数的 script array；Epic 也明确说明 C++ 编译为 CPU 直接执行的机器码，而 Blueprint 字节码在 VM 上执行。因此“共同类型/函数描述”和“共同执行后端”是两件事。[Blueprint 编译后端](https://dev.epicgames.com/documentation/en-us/unreal-engine/compiler-overview-for-blueprints-visual-scripting-in-unreal-engine)；[Blueprint vs. C++](https://dev.epicgames.com/documentation/en-us/unreal-engine/coding-in-unreal-engine-blueprint-vs-cplusplus)

3. **`FFrame` 是脚本调用的一层执行栈帧。** API 将其定义为“一层 script execution”的信息；它携带当前 `UObject`、`UFunction`、局部变量、上一帧、当前字节码指针和 Kismet 控制流栈，并提供读 opcode/推进执行的操作。这是蓝图 VM 所需的执行状态，不是对象实例或类型元数据本身。[FFrame API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FFrame)

4. **`ProcessEvent` / `UFunction::Invoke` 是反射式调用路径上的接入与调用步骤。** Epic 官方调试文章展示 Blueprint/Scripting 进入 native 调用时包含二者的调用栈；该栈片段中 `ProcessEvent` 是 `Invoke` 的调用方，调试器列出的栈顺序不是执行先后顺序。原生 C++ 仍可包含未注册为 `UFunction` 的普通成员，并直接执行普通成员调用；不能把反射调用路径推广为每次 C++ 调用的必经路线。本次未穷举生成包装函数与事件覆写的具体分支。[Debugging UFunction::Invoke](https://www.unrealengine.com/tech-blog/debugging-ufunction-invoke)；[原生成员与反射描述](https://dev.epicgames.com/documentation/en-us/unreal-engine/objects-in-unreal-engine)；[原生调用示例](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-object-handling-in-unreal-engine)

5. **朴素结论（推断，非 UE 官方术语）：它更像“统一的可描述、可查找、可调度对象系统，加上多种执行实现”，不是“UObject 自己就是一台 VM”。** 前四项直接证据显示：蓝图扩展 C++ 并通过暴露的 `UPROPERTY`/`UFUNCTION` 接入同一反射面；只有 Blueprint 后端明确产出并运行 VM 字节码。这个分层是从官方编译与执行资料作出的架构性概括，不是官方对 UObject 的定义。[C++/Blueprint 连接方式](https://dev.epicgames.com/documentation/en-us/unreal-engine/coding-in-unreal-engine-blueprint-vs-cplusplus)

## 未证实边界

本页未验证 `UFunction::Invoke` 在每一种 native、Blueprint、网络 RPC、委托或生成代码调用中的精确分支；也不主张 `ProcessEvent` 是所有反射入口的唯一实现。没有由这些资料推出自有语言的语法、调度成本或对象生命周期规则。
