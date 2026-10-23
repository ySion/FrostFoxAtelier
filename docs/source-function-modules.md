# 同模块多函数

状态：已实现。单源码请求选择一个公开入口；项目快照发布一组具名同步入口。内部函数与所属入口共同编译和退休。现行语法与限制统一见 [LANGUAGE.md](../LANGUAGE.md#同模块函数与入口)。

## 使用方式

```cpp
float process(const Array<float>& input) {
    Array<float> working = input;
    working.add(1.0);
    return sum(working);
}

float sum(const Array<float>& values) {
    float result = 0.0;
    uint index = 0;
    while (index < values.size()) {
        result = result + values[index];
        index = index + 1;
    }
    return result;
}
```

宿主仍使用已有入口：

```cpp
auto request = program.request("algorithm.source", source_text, "process");
```

首次发布前，多函数源码必须明确选择入口；单函数源码可省略。发布后省略名称沿用已冻结的入口。声明顺序和私有助手可以在新版本中调整，公开入口继续接受共享签名与接收者兼容性检查。

## 执行链

1. 前端收集类型、顶层常量声明和完整函数签名，解析常量依赖后再检查各函数体，因此可以前向调用或引用模块常量。函数名唯一，模块最多 1024 个函数和 1024 个常量；未知目标、参数不匹配、常量依赖环、直接/间接递归和内部 async 调用均在编译时报告带位置的错误。常量的类型、纯表达式与局部 const 规则以 [LANGUAGE.md](../LANGUAGE.md#声明与作用域) 为准。
2. 调用图从叶函数向上传播原生值使用、接收者需求和保持借用有效的效果。普通 plain record 按值参数/结果、一般拥有值或聚合借用的调用链只能使用符合 BorrowSafe 契约的原生方法；数学按值参数与局部不产生该限制，数学引用参数仍产生。未知效果无法通过助手绕过。未调用的函数不改变所选入口的执行要求。
3. 一个 LLVM module 包含选定入口和全部可达助手。助手以定型标量、数学值或普通记录存储地址及借用地址直接传参，不构造通用参数描述；公开入口和原生调用沿既有边界协议。编译期间完成各调用签名核对，普通 O2 可内联助手并消除中间值。
4. 每个函数的局部、拥有值、引用实参临时值和失败暂存使用栈存储。普通记录按值实参在调用方创建独立快照，被调函数直接使用快照，不进行二次复制；快照在调用方完整表达式结束时释放。存储分配位置不改变按值语义，也不允许借用逃逸。只为原生调用准备通用实参缓冲，其需求取可达函数中最大的实际原生调用参数数。
5. 单个 Invocation 持有全部根槽，各可达函数有固定且互不重叠的区间；接收者槽由公开入口保留。函数只清理自己的根和拥有值，内部同步调用只在自己的栈上建立帧。无环调用图保证同一函数不会同时有两个活动帧。
6. 失败状态与内部标量/数学/普通记录结果分开，原始错误逐层传播；每层先完成清理再返回。只有公开入口执行 begin/finish 策略与最终接收者清理，成功结果仍须通过整次执行资源检查后才交付宿主。
7. Version 拥有完整编译模块及编译期求值后固定的模块常量。活动调用、固定租约和挂起入口始终使用同一版助手与常量；修改常量只影响新版本，私有签名可随模块整体替换；退休仍由最后一个代码持有者负责。

## 生命周期边界

值、const 引用、可写引用和聚合借用沿用既有契约。内部 const 标量/数学/plain record 引用可绑定临时值，普通记录结果及其字段不能绑定可写引用，临时存活至完整表达式结束，String 字面量存活到当前语句结束；内部助手不能保存或返回借用，也不能改变借用容器的结构。double 及内建数学值的定型运算、公共结果存储和默认值见[数学值规范](math.md)。

Runtime 对象字段可按值传给助手。对象字段借用需要 Runtime 原生调用的存储保护，当前仍仅开放给原生成员调用；源函数间可以传递调用方自有标量、记录/数组叶值及聚合引用。

async 公开入口可调用同步助手，助手退出后才允许 yield。String/Array 和一般拥有型记录仍限定同步执行，不能通过内部调用进入 async 入口；数学局部可跨 yield，数学借用保持同步限制。取消挂起入口时，所有助手帧均已退出，只需清理持久入口帧及其保留的对象根。

当前没有递归、跨模块调用、内部 async 调用、函数值、跨类动态分派、String/Array/资源记录按值返回或 async 聚合参数/结果；内建数学值与 plain record 支持同步按值返回。普通记录按值签名同时要求 Runtime `plain_value` 与保持借用安全的 `can_own` 资格；两者独立，编译时检查。公开结果沿同一 `invoke` 的 `ValueOutput` 交付，并要求准确 Runtime 类型身份。源码 enum/struct 见[类型声明](source-type-declarations.md)。递归需要按实际激活管理 GC 根帧，跨模块调用需要跨版本依赖契约；这些能力不由当前固定槽区间隐式授权。

## 项目快照与多个公开入口

```cpp
source::Program program(runtime);
source::SourceSnapshot snapshot{"game", 1, {
    {"Source/Math.source", 3, math_text},
    {"Source/Move.source", 8, move_text}
}};
auto request = program.request(std::move(snapshot));
if (!request) return std::unexpected(request.error());
auto candidate = source::Program::prepare(*request);
if (!candidate) return std::unexpected(candidate.error());
auto published = program.try_publish(*candidate);
if (!published) return published;
auto tick = program.entry("game.Move.tick");
```

`classes()` 返回 `{name,type,base}`，`entries()` 返回 `{name,signature,parameter_names,code}`，`parameter_names` 按顺序给出源码声明的参数名，不含 `self`；`code` 是入口的代码身份：一个编译单元（某个 class 的全部方法，或全部全局函数）的编译输入——公共声明与该单元方法的文本、快照名、`CompileOptions` 和成员目录——与上次发布逐字节相同时沿用上次的值，否则取进程内从未用过的新值。它保守而不精确：改全局助手、struct、enum 或常量，以及 Runtime 新增值类型，会更换全部 code；改某个 class 的任一方法会更换该 class 的全部 code；撤销改动也得到新值，不跨进程稳定。`revision()` 是成功发布次数，首次发布前为 0，失败或被拒绝的发布不改变它；宿主可先比较它，再重读 `entries()`。视图在下一次成功发布或 close 后失效。`Candidate` 提供同名只读视图，供宿主在提交前检查业务约束；其预览类身份不可用于正式 Runtime 查询，base 已是注册的原生类型，视图在候选移动、销毁或成功发布后失效。请求拥有不可变快照，`Request::snapshot()` 可用于把诊断关联回原文件版本。`Error::source` 保存 UTF-8 字节定位。运行时错误继续由原执行协议返回。

类型布局、默认值和所有入口共同准备，实际 TypeId 与代码只在无回调提交时一起可见。某个编译单元失败会释放整组候选，旧代码继续使用。`busy` 不消费候选；旧请求、重复提交和关闭后的提交被拒绝。已发布模块只增长：新增 struct/enum、派生原生对象的 class、全局函数，以及已有或新增 class 上的方法（包括 begin/tick/post_physics_tick/late_tick/end）都可兼容发布，新类型在类型安全点（代码发布条件加 GC 空闲）与代码一起提交，已经创建的对象保留。删除或改名已发布的类型、字段、函数或方法，已有类型的字段、基类、默认值或定义变化，以及已有入口的签名、执行种类或接收者变化，返回 `ErrorCode::rebuild_required`，不会改写存量对象。参数名不属于签名：只改参数名是兼容发布，发布后 `entries()` 给出新名字。

全局助手和每个类的接收者环境分别形成 LLVM 模块；同一环境的多个入口拥有共享 LLJIT 和展开表。所有入口复用既有 `Version → Invocation → Runtime` 执行与清理链。借用保护通过内部调用参数传播，同一个助手既可在值区间内运行，也可在普通入口中推进 GC。

具名 `Entry` 只在绑定时解析名称，调用时直接按稳定槽位取得当前 Version；候选在提交前对齐已发布槽位，新入口追加在其后。代码租约保留实际使用的旧模块，关闭 Program 后仍可完成调用。Runtime、原生服务和后端 DLL 必须存活到所有租约/任务释放。

语法与限制以 [LANGUAGE.md](../LANGUAGE.md#项目快照与原生派生类) 为准。项目文件、World 生命周期调度和上下文重建由宿主组织；当前没有后台编译或对象布局迁移。

## 端到端验收

[正式消费者](../acceptance/source/modules.cpp)通过公共 Program 接口覆盖：

- 前向、多层与重复助手调用，嵌套实参求值、float/int/uint 值传递、const 标量临时、引用别名及 String 字节临时。
- 原生 Array 的深复制、跨函数借用、BorrowSafe 原生算法、深层原生异常/越界与部分复制失败后的逐层清理；逐项核对返回宿主的原始错误码和原因。
- 调用者与助手对象根的隔离、暂停 GC 中的调用、退出回收，以及挂起和取消时保留/释放正确根。
- 入口显式选择、缺失/歧义入口拒绝、冻结入口拒绝切换、只读/结构修改/递归/async 调用拒绝、传递效果拒绝及未调用助手的隔离。
- 私有助手名称/顺序/签名变化的整模块替换、旧租约执行，以及挂起入口恢复时继续调用旧助手。

模块场景随 `source_acceptance` 和 `jit_acceptance` 运行。

项目快照消费者见 [projects.cpp](../acceptance/source/projects.cpp)。它覆盖跨文件 helper、原生派生类与标量默认值、具名多入口及其参数名、原子更新、只改参数名的兼容发布、旧租约、入口重排、错误文件定位、无入口语义检查，混合借用入口对 GC 的影响，入口 code 与 revision 随编译输入的变化，以及值区间内只读 `self` 字段且不改变借用与 GC 规则。可单独运行 `atelier_source_minimal.exe --projects`；完整覆盖仍在 `source_acceptance` / `jit_acceptance`。
