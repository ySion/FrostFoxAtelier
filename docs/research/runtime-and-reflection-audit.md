# Runtime 优化阶段落档与反射体系实证对标

状态：已完成（`Steps 1–4` 与反射补齐 `Phase P0–P2` 全部落地并通过 `atelier_style` 与双配置端到端验收）。基线提交为 `85c6127fb0c3775a81ad19d6fa13550134978cb1`，核对日期：2026-11-18。

本文保存本轮 `modules/execution/runtime` 性能优化与 C++ 反射体系（`tools/reflection`、`runtime/native.hpp`）对标审查的完整阶段状态、同机测量与设计取舍，防止上下文压缩丢失关键事实。已落地能力的正式契约以 [基础库](../foundation.md)、[运行时模型](../runtime-model.md)、[运行时结构](../runtime-structure.md) 与 [C++ 自动反射](../reflection.md) 为准。

---

## 一、Runtime 优化已完成进度（Steps 1–4）

### 1.1 同机 Release 基准进展

测量环境：Windows x64，AMD Ryzen 9 3950X，锁定 Clang 23.1.5（`/O2 /DNDEBUG`），共享 `mimalloc`，跨编译单元真实调用边界（基准驱动编译单元未开启跨库内联）。临时微基准目录已于测量收尾后清理。

| 场景 / 结构指标 | Baseline | Step 1 | Step 2 | Step 3 | Step 4（最终） | 累计收益 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `sizeof(ObjectRoot)` / `sizeof(ValueRoot)` | `24 B` | `16 B` | `16 B` | `16 B` | **`16 B`** | `-33.3%` |
| `sizeof(Runtime::Impl::Slot)` | `64 B` | `32 B` | `32 B` | `32 B` | **`32 B`** | `-50.0%` (`alignas(32)`) |
| `[S1]` `ObjectRoot` 同 Runtime 拷贝赋值 | `28.44 ns` | `5.45 ns` | `5.48 ns` | `5.49 ns` | **`5.58 ns`** | **`5.10x–5.18x`** |
| `[S1]` `ObjectRoot` 拷贝构造 + `reset` | `26.86 ns` | `12.64 ns` | `12.66 ns` | `12.66 ns` | **`12.74 ns`** | **`2.11x`** |
| `[S1]` `Runtime::hold` + `reset` | `39.47 ns` | `19.62 ns` | `19.57 ns` | `22.31 ns` | **`20.56 ns`** | **`1.92x`** |
| `[S1]` `Runtime::is_a`（深度 6 命中 / 深度 4 未命中） | `5.35 / 5.39 ns` | `4.08 / 4.09 ns` | `4.12 / 4.11 ns` | `4.10 / 4.11 ns` | **`4.24 / 4.30 ns`** | **`1.26x–1.31x`**（含跨 TU `Result<bool>` 返回） |
| `[S2]` 增量 GC 周期（16k 存活叶对象 + 32k 历史根空洞，0 死亡） | `424.50 us` (`2561` 步) | `366.50 us` | `147.90 us` | `147.90 us` | **`148.90 us` (`513` 步)** | **`2.85x`（步数 `-80%`）** |
| `[S2]` 增量 GC 周期（16k 存活叶对象 + 32k 历史根空洞，16 死亡） | `426.90 us` (`2561` 步) | `372.40 us` | `147.20 us` | `149.10 us` | **`148.80 us` (`513` 步)** | **`2.87x`（步数 `-80%`）** |
| `[S3]` 帧级 `changed_objects`（16 次写入 + 32k 槽位扫描） | `1.72 us` | `1.39 us` | `1.40 us` | `0.86 us` | **`0.77 us`** | **`2.23x`**（纯扫描约 `60 ns`；全干净帧 `9.57–10.46 ns`） |
| `[S3]` `MathEntity` 创建 + 销毁（`Vec3`/`Quat`/`Mat4`x2/`f32`/`Enum`） | `171.37 ns` | `146.54 ns` | `135.34 ns` | `58.44 ns` | **`62.39 ns`** | **`2.75x–2.93x`** |
| `[S4]` `find_field` + `find_method` | `127.45 ns` | `114.09 ns` | `121.85 ns` | `115.54 ns` | **`37.36 ns`** | **`3.41x`** |
| `[S4]` `Scalar`（带硬边界）/ `Vector3` 反射读写 | `50.56 / 62.18 ns` | `51.40 / 61.70 ns` | `51.59 / 61.35 ns` | `52.67 / 61.99 ns` | **`44.77 / 42.31 ns`** | **`1.13x / 1.47x`**（含 136B/144B `Result` 跨 TU 返回） |
| `[S4]` `Runtime::call`（标量 / 枚举 / `Vec3`） | `143.0 / 110.3 / 116.1 ns` | `130.2 / 112.0 / 116.1 ns` | `131.8 / 107.2 / 117.5 ns` | `134.4 / 103.4 / 107.8 ns` | **`40.9 / 42.5 / 50.6 ns`** | **`3.49x / 2.60x / 2.29x`** |

### 1.2 Steps 1–4 已落地机制与保留契约

1. **Step 1：热句柄/槽位布局瘦身、单线程侵入式计数与 `O(1)` 继承判别**
   - `ObjectRoot`、`ValueRoot`（[runtime.hpp](../../modules/execution/runtime/include/runtime/runtime.hpp)）与 `Coroutine`（[execution.hpp](../../modules/execution/runtime/include/runtime/execution.hpp)）移除 `std::shared_ptr` 原子控制块，改为符合单线程宿主契约的侵入式引用计数（`ObjectRoot`/`ValueRoot` 锁定为 `16 B`；`Coroutine` 保留 `Control::Pin` 重入期保活）。
   - `detail::RootState` 拆分为 `SparseArray<ObjectReference> object_entries` 与 `SparseArray<ValueEntry> value_entries`，仅在 `marking == true` 期间通知写屏障；非 GC 标记期根创建/释放零间接调用，同 Runtime `ObjectRoot::operator=` 原地改槽。
   - `Runtime::Impl::Slot` 从 `64 B` 缩至 `32 B`（`alignas(32)`），保存 `uint32_t type_index` 替代 16B `TypeId`；冷 GC 状态拆至 `BitVector live_slots`、`BitVector marked_slots` 与 8B `GrayLink`（双链表头判据 `gray_head == index || previous != no_slot`）。
   - `Impl::Type` 建立 **Cohen's Display**（`InlineArray<const Type*, 8> ancestors`、`depth`、`native_base_depth`），使 `derives_from`（`Runtime::is_a` / `NativeContext::try_resolve`）降为常数时间，同时兼容后续动态 `register_types` 追加派生类。
2. **Step 2：增量 GC 位图跳空、叶对象直黑与精确差集清扫**
   - `BitVector::find_from`（[bit_vector.hpp](../../modules/core/foundation/include/foundation/bit_vector.hpp)）与 `SparseArray::find_index_from`（[sparse_array.hpp](../../modules/core/foundation/include/foundation/sparse_array.hpp)）暴露基于 `std::countr_zero` 的 64 位字跳空扫描，消除 `roots` 阶段对历史根高水位空洞的线性空转。
   - 对标 CoreCLR `!ContainsPointers()`，`shade(ObjectId)` 对 `trace_fields.empty()` 的叶对象直接 **White -> Black**，不再压入灰链表。
   - `trace_one()` 通过 `retire_trace_frames()` 即时退栈已完成帧和空序列，保留逐字段挂起预算与 `ValueRoot` 取消追踪语义。
   - `mark -> sweep` 在 `pending == 0` 时零垃圾直退 `idle`；有死对象时预计算 `sweep_slots = live_slots ^ marked_slots` 精确跳转死槽，清零后立即结束周期。
3. **Step 3：双纪元分层写戳页与 CDO 原型字节镜像**
   - 放弃首版存在假阳性累积的滚动单掩码方案，采用两级精确结构：`write_super_pages`（每 `uint64_t` 覆盖 4,096 槽）+ `WritePage`（精确保存最新两个 `write_epoch` 的 64 位掩码 `newest_mask` / `previous_mask` 与页底 `floor_epoch`）。既满足 `Physics` 单纪元查询，也零假阳性覆盖 `Presentation` 跨游戏逻辑帧与物理回写的双纪元查询，`stamp()` 保持 `[[likely]]` 单分支快路径。
   - `NativeLayout`（[runtime.hpp](../../modules/execution/runtime/include/runtime/runtime.hpp)、[native.hpp](../../modules/execution/runtime/include/runtime/native.hpp)）增加 `trivially_destructible`；`needs_destroy` 消除平凡值/数学字段的递归析构空跑。
   - `prepare_types` 末尾为无构造副作用且通过类型校验的描述对象/记录（含 `Quaternion{0,0,0,1}`、`Matrix4::identity()`、`default_leaves`、`ReferenceSlot`）预构建 **CDO `prototype` 字节镜像**，`create` / `initialize_value` / `copy_construct_value` / `assign` 退化为单次 `std::memcpy`。
4. **Step 4：成员哈希索引、无中间 `Result<Address>` 单趟反射读写与预绑定动态调用**
   - `Impl::Type` 在布局定型后为公开字段与方法构建 `Map<StringView, FieldId>` / `Map<StringView, MethodId>`（零字符串复制，直接借用不可变 `TypeInfo` 名称），`find_field` + `find_method` 从 `127.45 ns` 降至 **`37.36 ns`（`3.41x`）**。
   - 引入 `scalar_variant_index` 与 `try_resolve_inline_field`，使常规直接偏移路径上的 `Runtime::read` / `Runtime::write`（含 `Scalar` 与 plain value `ValueOutput` / `ValueInput`）跳过 152B `Result<Address>` 中转、`CallbackScope` 计数与二次 `Type*` 查找，同时完整保留 `stamp` 写戳与硬边界失败诊断契约。
   - `Impl::Type::CompiledMethod` 在发布期预绑定返回值/参数 `Type*`、`scalar_variant` 与 128B 栈暂存偏移，`CallContext::expected_result_type` 消除 `native_method` 枚举返回的动态 `native_type<Return>()` 哈希查找，`TypeReference::native<T>()` 借用实参直接对比 `native.representation` 指针，使 `Runtime::call` 提速 **`2.29x–3.49x`**。
5. **已执行验证**：`atelier_style` 42 TU 0 告警；`dev` 与 `release` 双配置通过 `foundation_acceptance`、`runtime_acceptance`、`jit_acceptance`、`source_acceptance`、`world_acceptance`、`physics_acceptance`、`procedural_acceptance`、`project_scene_acceptance`、`editor_document_acceptance`、`project_integration_acceptance`。

---

## 二、反射体系实证比对（`FrostFoxAtelier` vs `ExtremeEngine`）

本节全部结论基于两边真实源码与规则库逐项核对，不依赖猜测：
- **FrostFoxAtelier 核对文件**：[reflection.md](../reflection.md)、[tools/reflection/main.cpp](../../tools/reflection/main.cpp)、[reflection.hpp](../../modules/execution/runtime/include/runtime/reflection.hpp)、[native.hpp](../../modules/execution/runtime/include/runtime/native.hpp)、[foundation.hpp](../../modules/execution/runtime/include/runtime/foundation.hpp)、[runtime.hpp](../../modules/execution/runtime/include/runtime/runtime.hpp)、[bridge.cpp](../../modules/execution/source/src/detail/bridge.cpp)、[editor/fields.hpp](../../modules/editor/core/include/editor/fields.hpp)。
- **ExtremeEngine 核对文件**：`.codex/skills/sattr-reflection/references/反射标记数据库.md`、`反射语法数据库.md`、`engine/tools/LLVMTools/meta/src/{main.cpp, ASTConsumer.cpp, meta_db.h, IgnoreGeneratedFileSystem.h}`、`engine/modules/core/core/include/SkrRTTR/{rttr_type.hpp, rttr_interface.hpp, export/export_data.hpp, export/editor_attributes.hpp, generic/generic_base.hpp, type_signature/type_signature.hpp}`、`SkrObject/{object.hpp, object_scan.hpp, gc.hpp}`。

### 2.1 全维度能力对照表

| 维度 | ExtremeEngine (`sattr` + `SkrRTTR` + `SkrObject`) | FrostFoxAtelier (`atelier_reflect` + `Runtime`) | 差距性质与中肯判定 |
| --- | --- | --- | --- |
| **核心类型架构** | 进程全局静态注册表（明确声明不支持动态/脚本建类）；强制手工维护 128-bit `guid("...")`；`skr::Object` 侵入式继承 `RTTRBasic`（含虚表、`_object_id`、`_object_array_index`） | 多实例独立 `Runtime`；C++ `NativeRegistration` 与 `.source` 编译器共用同一 `TypeDefinition` 事务发布，支持 `.source` 派生原生 `ACLASS`；业务类零虚表/零侵入头，32B 外部 `Slot` | **我方架构领先**。不引入全局静态表、全量手工 GUID 或侵入式基类 |
| **字段与基类字节偏移** | `ASTConsumer.cpp:896` 对所有非位域字段无条件提取 `ASTRecordLayout::getFieldOffset`，`ASTConsumer.cpp:408` 对所有非虚基类提取固定字节偏移并扁平化，非虚继承转型与字段寻址均为纯指针加法 | `main.cpp:883` 将 `direct_layout` 绑死在 `isStandardLayout() && !hidden`，导致**引擎现有 `ACLASS` 对象字段 100% 输出 `offset = nullopt`**；且 `runtime.cpp:2258` 对原生字段忽略 `native_offset`，导致 Runtime 与 `.source` JIT 访问对象字段全部退化为间接函数指针调用 | **[P0 必补 / `Step 4-prev`]** 真实性能与统一类型底层断层，必须立即打通 |
| **跨模块/跨目标类型引用** | 通过 Clang `AnnotateAttr("__reflect__")` + `_collect_type_facts` 跨头文件识别上游模块反射 `struct` / `enum` / `class`，任意模块可内联组合上游反射值类型 | 跨生成组仅支持 `ACLASS` 基类（靠 `AGENERATED_BODY` 友元识别）与 `ReferenceSlot`/`Sequence` 名称解析；**跨模块内联 `ASTRUCT` 与 `AENUM` 字段、方法参数/返回值全部报错拒绝**（`main.cpp:664, 797` 只查当前组 `state.types`） | **[P1 关键短板]** 模块拆分后（如 `world` 的枚举/结构体被 `physics`/`gameplay` 内联使用）会直接卡死 |
| **类型级跨字段校验** | 无内建统一校验钩子（靠 `ed_dirty` 或 `serde_custom_reader` 分散处理） | `Runtime::validate` 与 `NativeRegistration::validate<&F>()` **已实现**，但 `atelier_reflect` 缺少对应宏选项（[reflection.md](../reflection.md) 第 89 行已注明，如 `CameraComponent` 需注册后手工补挂） | **[P1 易补短板]** 机制已在 Runtime 就绪，生成器补 `Validate` 即可闭环 |
| **GC 引用漏标防护** | `ObjScan` 默认全扫，允许手写 `obj_scan_off` / `obj_scan_gc_off`，靠人工纪律保证不漏扫真实引用 | `main.cpp:857` `contains_reference_storage` 递归检查字段与基类，**漏标 `ReferenceSlot` / `ReferenceSequence` 或内嵌含引用结构直接在构建期报错** | **我方更安全**。保持编译期静态防漏标契约 |
| **标量与枚举能力** | 支持 `i8/u8/i16/u16/i32/u32/i64/u64/f32/f64/bool`；枚举支持任意底层整数、`flags` 位掩码标记、枚举项独立属性与 `rttr_off` 排除 | 标量固定为 `bool/i32/u32/f32/f64`；`AENUM` 仅限 `i32/u32` `enum class`；**无 `Flags` 位掩码标记，无枚举项级 `Label` / `DESC` 元数据** | **枚举元数据属实缺口（P2）**：编辑器下拉框和碰撞/渲染层位掩码需要 `Flags` 与枚举项说明；窄整数/`i64` 牵动 `.source` 与 `Scalar` 契约，按需再开 |
| **容器与泛型擦除** | 基于引用计数虚接口 `RC<IGenericBase>` 支持 `Vector`、`SparseVector`、`SparseHashMap`、`SparseHashSet`、`Optional`、定长数组 `T[N]` | 原生无虚表直连 `String`、无引用 `Array<T>`、对象专属 `ReferenceSequence`；**不支持反射 `Map<K,V>`、`Optional<T>`、定长数组，也不支持 `ASTRUCT` 内持有引用列表** | **中期按需扩展（P2）**：需要 `Optional`/`Map` 时应沿 `TypeKind`/`NativeLayout` 零虚表扩展，**不照搬**堆分配 `RC<IGenericBase>` |
| **编辑器与持久化属性** | 细粒度区分：`ed_readonly`、`ed_drag_speed`、`ed_as_color`、`serde_off`（运行期可见但不存档）、`serde_alias("old")`、`getter/setter` 属性绑定、`ed_dirty`/`ed_signal` | 已有 `Label`、`Category`、`DESC`、`Min/Max` + 运行时强制 `Lock/Hard` 边界、开放 `Meta("k","v")`；但**可见性与存档强耦合在单一 `Hidden` 上**（无法声明“面板只读字段”或“公开但不进 Scene 存档的运行期字段”），步长/单位/颜色语义仍靠 `editor` 硬编码字段身份 | **存在真实工具链缺口（P1/P2）**：`Transient`（不序列化）与 `ReadOnly`（面板只读）及标准 UI 提示可通过轻量标志或规范 `Meta` 命名空间解决；**不引入** `getter/setter` 虚拟属性与逐字段 dirty 回调（保持纯数据 CDO 与写戳模型） |
| **方法、静态函数与构造** | 支持成员方法、**`static` 方法**、**命名空间自由函数**、**多参数构造函数**、**同名重载（按签名区分）**、**C++ 默认参数值**、`in/out/inout`；代价是注册期大量 `SkrNew<RTTRParamData>` 堆分配与 `DynamicStack` 封送 | 支持 `const`/`noexcept`/`Result<R>`/`BorrowSafe` 成员方法、零分配 `PreparedArguments` + JIT 定型 `NativeThunk`；但 **`atelier_reflect` 不支持：(1) 上下文方法 `(Runtime&, ObjectId, Self&, ...)`（`native.hpp:98` 已支持但生成器未接）、(2) `static`/自由函数、(3) C++ 默认参数值、(4) `String`/`Array` 返回值或 `StringView`/`Span` 借用参数** | **(1) 上下文方法生成器接入简单且实用**；**(2)(3)(4)** 属语言/脚本绑定增强项，应在扩展 `.source` 调用能力时协同推进；多构造函数与同名无别名重载故意不做 |
| **构建期前端工具性能** | `meta.exe` 显式开启 `SkipFunctionBodies = true` 跳过内联函数体解析；但后端需再跑一趟 `.NET SB` C# 进程消费 JSON 生成代码，且可顺带提取源码注释作为默认说明 | `atelier_reflect.exe` 单 C++ 进程直出 `.gen.hpp`/`.gen.cpp`/`.d` 且带内存虚拟头与内容比对保时间戳（链路更干净）；但**未开启 `SkipFunctionBodies = true`**，每次扫描全量解析并类型检查头文件内所有内联/模板函数体 | **[P0 必补 / `Step 4-prev`]** 在确认不影响常量求值与 `noexcept` 解析的前提下开启 `SkipFunctionBodies`，白拿构建期提速 |

---

## 三、分阶段推进路线落档（`P0` → `P1` → `P2` → `Step 4`）

按优先级与契约依赖，将反射补齐（`Step 4-prev`）拆为 **`Phase P0`、`Phase P1`、`Phase P2`** 三个独立可验收阶段依次推进，并与 **`Step 4`（Runtime 反射查找/路径/调用快路径）** 衔接。每完成一阶段即在本节更新状态与实测证据。

### 3.1 `Phase P0`：统一类型直接字节偏移全链路打通 + `atelier_reflect` 前端提速
- **当前状态**：**`已完成（atelier_style 0 findings，dev/release 验收通过）`**
- **落地结果与实测数据**：
  1. **直接字节偏移全覆盖**：`atelier_reflect`（[tools/reflection/main.cpp](../../tools/reflection/main.cpp)）解除 `isStandardLayout()` 与 `!field.hidden` 限制，基于 `ASTRecordLayout` 为所有非虚继承的自然对齐字段与基类子对象输出精确字节偏移（如 `world::Entity`、`Component`、`PrimitiveComponent`、`presentation`、`procedural` 等全部对象类型现均发布非空 `FieldInfo::offset` 与 `TypeInfo::base_offset`；`Hidden` 字段仍不进入公开 `TypeInfo::fields`，仅在 Runtime 内部消除间接取址）。
  2. **Runtime 与 JIT 消费打通**：`Impl::Type` 预计算 `base_root_offset` 使常规单继承 `base_storage` 退化为 `O(1)` 常量偏移计算（零循环、零 `CallbackScope`、零间接调用，保留无 `base_offset` 手写绑定的回调回退）；`FieldStorage` 直接使用 `native_offset`（保留 `native_offset == std::nullopt` 时的自定义 `native_address` 兼容 `Links` GC 测试桩）；`source/src/detail/bridge.cpp` 优先消费 `base_offset` 与 `FieldInfo::offset`，彻底消除 `.source` JIT 访问原生对象字段时的 `callbacks` 计数读写与间接函数调用（避免每次字段读取破坏 x64 调用者保存寄存器）。（注：C++ 宿主侧 `Runtime::read/write` 仍经 144B `Result<Address>` 中转，留待 `Step 4` 单趟快路径消除。）
  3. **`atelier_reflect` 开启 `SkipFunctionBodies = true` 同机 Release 实测（5 次取中位数）**：

     | 反射目标 | 优化前中位数 | `Phase P0` 后中位数 | 加速比 / 降幅 |
     | --- | ---: | ---: | --- |
     | `atelier_procedural`（25 类型） | `3,273.7 ms` | **`1,590.1 ms`** | **`2.06x`（`-51.4%`）** |
     | `atelier_world`（10 类型） | `2,803.0 ms` | **`1,546.6 ms`** | **`1.81x`（`-44.8%`）** |
     | `atelier_editor_core` | `2,246.5 ms` | **`1,516.3 ms`** | **`1.48x`（`-32.5%`）** |

- **已同步文档与验收**：已更新 [reflection.md](../reflection.md)、[native-value-borrows.md](../native-value-borrows.md)、[runtime-model.md](../runtime-model.md)，并在 [acceptance/reflection/main.cpp](../../acceptance/reflection/main.cpp) 增加非标准布局继承对象（`DerivedCounter`）直接偏移往返断言；`dev` 与 `release` 双配置通过 `runtime_acceptance`、`jit_acceptance`、`source_acceptance`、`world_acceptance`、`physics_acceptance`、`procedural_acceptance`。

### 3.2 `Phase P1`：模块化反射组合、声明式校验、上下文方法与字段可见性/存档解耦
- **当前状态**：**`已完成（atelier_style 0 findings，dev/release 验收通过）`**
- **落地结果与契约收口**：
  1. **跨生成组 / 跨模块 `ASTRUCT`、`AENUM`、`ACLASS` 识别**：
     - [reflection.hpp](../../modules/execution/runtime/include/runtime/reflection.hpp) 增加零开销编译期特征模板 `atelier::reflection::Reflected<T, TypeKind>`；`atelier_reflect`（[tools/reflection/main.cpp](../../tools/reflection/main.cpp)）在 `.gen.hpp` 输出对应特化，并通过 `collect_external_reflected` 在下游 AST 中自动识别上游反射值/枚举/对象。
     - [cmake/reflection.cmake](../../cmake/reflection.cmake) 为每个反射目标注册 `${target}_reflect` 并沿 `LINK_LIBRARIES` / `INTERFACE_LINK_LIBRARIES` 递归建立生成依赖，确保上游 `.gen.hpp` 始终先于下游 `atelier_reflect` 生成。
  2. **类型级跨字段校验宏 `Validate = "..."` 与业务迁移**：
     - `ACLASS` / `ASTRUCT` 支持声明 `const` 成员方法 `Result<void>() const`、`static` 成员方法 `Result<void>(const Self&)` 或命名空间自由函数，生成器输出 `.validate<&F>()`。
     - 已将引擎内全部 4 处注册后手写补挂校验（[world.hpp](../../modules/engine/world/include/world/world.hpp) `BodySettings`，[presentation.hpp](../../modules/engine/presentation/include/presentation/presentation.hpp) `LightSettings`、`CameraComponent`、`EnvironmentComponent`）迁移至声明处闭环。
  3. **上下文感知静态 `AFUNCTION` 与业务迁移**：
     - `ACLASS` 内标记 `AFUNCTION` 的 `static Result<R>(Runtime&, ObjectId, [const] Self&, ...)` 自动对接 `native.hpp` 上下文特化并剔除前三个宿主上下文形参名，同步生成动态调用入口与 JIT `NativeThunk`。
     - 已将 `world::Entity`（`destroy`、`set_enabled`）与 `world::Component`（`translate_owner`、`destroy`、`set_enabled`）共 5 个手写上下文方法迁移至自动生成。
  4. **字段 `Transient` 与 `ReadOnly`（方案 A）全链路解耦**：
     - `FieldDefinition` / `FieldInfo` 与 `NativeRegistration` 增加 `transient`、`read_only`（禁止与 `Hidden` 混用）；`.source` 类字段属性同步支持 `[Transient, ReadOnly]` 并在热重载 `same_field` 中参与等价性比较。
     - 采用确认的**方案 A 契约**：`Transient` 字段保留在 `TypeInfo::fields` 与编辑器参数面板中，但由 `editor::detail::author_fields` 默认排除出 `Scene v10` 与程序化图持久化编解码；`ReadOnly` 为编辑器交互只读契约（面板置灰不可编辑，`Session::{begin_edit, begin_group_edit, set_properties}` 拒绝交互写入，`editor_control` 导出 `"transient"` / `"read_only"` 标记，而 C++ 宿主、`Runtime::write`、`.source` 脚本与场景反序列化仍可正常写入）。
- **已同步文档与验收**：已更新 [reflection.md](../reflection.md)、[LANGUAGE.md](../../LANGUAGE.md)、[source-type-declarations.md](../source-type-declarations.md)、[project-scenes.md](../modules/world/project-scenes.md)；在 [acceptance/reflection](../../acceptance/reflection/main.cpp)、[acceptance/source/projects.cpp](../../acceptance/source/projects.cpp)、[acceptance/editor/project_scene.cpp](../../acceptance/editor/project_scene.cpp) 增加端到端验收；`atelier_style` 31 TU 0 告警，`dev` 与 `release` 双配置通过 `runtime_acceptance`、`jit_acceptance`、`source_acceptance`、`world_acceptance`、`physics_acceptance`、`procedural_acceptance`、`project_scene_acceptance`、`editor_document_acceptance`、`project_integration_acceptance`。

### 3.3 `Phase P2`：枚举 `Flags` 位掩码与 `AMETA` 元数据、`AFUNCTION` 尾部默认实参
- **当前状态**：**`已完成（atelier_style 0 findings，dev/release 验收通过）`**
- **落地结果与裁剪结论**：
  1. **枚举 `Flags` 位掩码与枚举项 `AMETA(Label, DESC)` 全链路打通**：
     - `TypeDefinition::Enumeration` / `TypeInfo::Enumeration` 新增 `bool flags`（限定 `core.u32` 底层）与等长 `EnumeratorMetadata`（`label`、`description`），兼容全部既有 `.values` 消费者。
     - C++ 反射支持 `AENUM(Flags, ...)` 与紧邻枚举项的 `AMETA(Label = "...", DESC = "...")`；`.source` 支持 `[Flags] enum ... : uint { [Label = "...", DESC = "..."] ... }`、同类型 `&/|/^/~` 按位运算及类字段常量位组合默认值。
     - 编辑器参数面板显示枚举项 `Label` 与说明提示，`Flags` 枚举采用保持弹窗常驻的 `MenuBehavior::checkbox` 多选位开关；`Scene v10` 对无单一名称匹配的合法组合位掩码按无符号整数无损编解码往返，拒绝未声明位。
  2. **`AFUNCTION` 尾部标量 / 枚举默认参数（`parameter_defaults`）**：
     - `atelier_reflect` 对 `AFUNCTION` 参数上的 C++ 编译期常量默认表达式直接求值（浮点采用十六进制位精确字面量，枚举输出底层整数支持同批前向引用），发布至 `MethodInfo::parameter_defaults`（已接入 `world::Component::translate_owner` 等）。
     - 动态 `Runtime::call` 自动补齐省略的尾部默认实参；`.source` 调用原生方法省略尾部实参时在前端编译期直接静态补齐常量节点，零运行期开销且不改变 `NativeThunk` ABI。
  3. **按工程原则暂缓项（待真实业务工作负载驱动）**：全局无接收者自由函数表、违反窄 ABI 合同的拥有型容器按值返回，以及会撑大 `Scalar` 的窄整数/`i64` 与 `Optional`/`Map` 容器均不预建空泛透传层。

### 3.4 `Step 4`：Runtime 反射查找、路径读写与动态调用快路径
- **当前状态**：**`已完成（atelier_style 0 findings，dev/release 验收通过）`**
- **落地结果**：见第 1.1 节基准表与第 1.2 节第 4 项（`find_field + find_method` **`3.41x`**，`Vector3` 读写 **`1.47x`**，`Runtime::call` **`2.29x–3.49x`**）。
