# 引擎数学类型：研究与方案

状态：2026-10-02 调研起点，2026-10-03 按用户授权实施。当前合同见 [math.md](../math.md)；下文保留设计依据和迁移前事实。

2026-10-03 补充：用户已授权 float 与 double 的完整基础数学实现、`math.vector3` 等规范名称。下面的实现约束是基于第一方资料的建议；实际落地与验收结果以当前代码和 README 为准。

## 迁移前代码事实（基线 c626817）

本节描述迁移前的代码；不代表当前能力。`ASTRUCT(Name="world.Vec3")` 发布类型的规范名称；省略 `Name` 会把 C++ 完整命名空间转换成点分名称。名字不规定存储、调用或优化方式。当前 `Vec3` 是三个公开 `float` 字段及相等比较的普通 record；数学声明依赖 Runtime/reflection，实现归属 `atelier_world`。

当前 JIT 能直接生成部分标量算术与比较 IR，但未实现 record 算术、record 按值参数/结果，也未配置通常的 O2、SLP 或循环向量化 IR 管线。反射原生调用仍经过 ABI 打包和 `Runtime::call`。宿主已有的 C++ 机器码不会因注册到 LLJIT 而自动成为可内联 IR。

当前相机已经直接使用 DirectXMath 的 look-at 与 perspective 运算（`src/editor/camera.cpp`），World 则在 `src/world/math.cpp` 实现 TRS 校验和矩阵转换。通用数学模块应统一这些调用所遵守的表示与约定；无需重新编写已有成熟底层算法。World 独立工作树还存在矩阵乘法与相对变换增量，实际迁移须同步保留，本任务只修改所属工作树；后续与独立 World 增量同步时仍须保留其功能。

## 第一方资料提供的事实

- DirectXMath 区分存储和计算：`XMFLOAT3` 有三个 float 字段，经 load/store 与 `XMVECTOR` 转换；`XMVECTOR` 是需要 16 字节对齐的 SIMD 寄存器代理。官方建议结构成员可采用紧凑存储类型，计算期间保持 SIMD 形式，也提供数组 stream 操作。其 C++ 实现是头文件中的 inline intrinsic routines。[设计说明](https://learn.microsoft.com/en-us/windows/win32/dxmath/pg-xnamath-getting-started)、[XMFLOAT3](https://learn.microsoft.com/en-us/windows/win32/api/directxmath/ns-directxmath-xmfloat3)
- `XMLoadFloat3` 加载 xyz，并令临时计算向量的 w 为零。这不意味着业务 Vec3 需要一个可观察的第四字段。[API 说明](https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmloadfloat3)
- GLM 支持 packed 与 aligned 类型；默认 aligned 需要配置宏，其示例展示额外 padding。GLM 自身的 SIMD 优化要求 aligned 类型。这是该库的实现策略，不是所有 Vec3 的硬件必然要求。[官方手册 2.9–2.11](https://github.com/g-truc/glm/blob/master/manual.md)
- .NET 的 Vector2/3/4 保持与相应数量 float 结构一致的布局与互操作表示，同时由 JIT 优化。Vector3 源码明确说明其大小为 12 字节、自然对齐为 4 字节，并在操作上使用 intrinsic 与 AggressiveInlining。这是紧凑存储与计算特化能并存的实际例子。[运行时设计](https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/botr/vectors-and-intrinsics.md)、[Vector3 源码](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/System/Numerics/Vector3.cs)
- UE5 为大世界引入双精度变体，`FVector` 转为 double，另保留 `FVector3f` 等浮点变体。精度由世界需求决定，不能从 SIMD 宽度直接推出。[Epic LWC 文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-in-unreal-engine-5)、[Niagara LWC 说明](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-in-niagara-for-unreal-engine)
- LLVM 提供 vector IR；fast-math 标志允许本来不安全的浮点变换，因此并非免费性能开关。[Vector types](https://llvm.org/docs/LangRef.html#vector-type)、[Fast-math flags](https://llvm.org/docs/LangRef.html#fast-math-flags)

## 调研时的建议

数学基础应归属引擎通用 math 模块，World 消费它。采用同一份权威值类型、布局和操作契约；原生注册为语言和工具发布这些定义。C++ 可以直接使用类型和可见函数体，语言 JIT 可以将相同操作降为标量或 vector IR。语义统一不要求每次执行都进入反射调用。

优先保留 Vec3 的紧凑三分量存储；计算临时值可放在 SIMD 寄存器中，数组内核可依据访问方式采用批处理或 SoA。16 字节 Vec3 会把每元素存储从 12 增至 16 字节，增加三分之一；这不自动换来更快的完整场景。GPU buffer 及原生跨语言调用分别遵守各自 ABI，不把某个 shader 布局直接当作整个引擎的值布局。

像基础类型一样的使用体验，可由编译器的运算符解析和 IR 降低提供，不必把 Vec3 另建成第二套基本类型世界。若编译器特化，需要绑定权威类型身份和已验证布局，避免靠字符串拼写识别任意同名 record。首片可以限定为受支持数学值的构造、运算、内部按值传递与优化，不要求先完成任意 record 的原生按值 ABI 或通用运算符重载。简单数学运算直接降低到 IR；复杂函数可复用原生实现，进一步特化由实际收益决定。

纯数学类型与计算保持不依赖 Runtime；薄反射接线复用现有注册体系，由引擎执行上下文的标准初始化统一注册。源码短名可以由标准环境解析到同一类型身份。通用数学值不应要求先建立 World，别名也不能重复注册成第二种 Vec3。

小型库的边界是游戏实际需要的 Vec2/3/4、Quat、Mat4，以及构造、逐分量算术、标量缩放、dot/cross、长度、明确的安全归一化、插值、旋转、矩阵组合和点/方向变换。AABB、Ray 等在真实调用出现时加入。通用数学处理负数及零等数学值；World 的有限值、正 scale、四元数规范化等业务约束继续由 World 负责。

还需共同确定：float/double 的世界位置需求；零长度归一化及 NaN/Inf 的结果；相等与近似相等的区别；FMA/重排及服务器确定性要求；矩阵与四元数组合顺序；负 scale、shear 和逆变换失败策略；record 按值调用 ABI。当前 row-major、row-vector、S*R*T 及 World 规范化行为应在未获准更改前保持。选 DirectXMath 或其他后端属于后续实现判断，本研究没有新增依赖。

## 本轮实现约束

- **存储与计算分离。** 建议三分量 float 保持 12 字节、double 保持 24 字节，按标量自然对齐。LLVM 中可用三个标量字段的 struct 存储，再以 `insertelement` 形成计算 vector；若暂用四 lane，初始化无业务意义的第四 lane 为零。不要把紧凑 record 地址当成四分量 vector 宽加载，也不要用 vector 的 ABI allocation size 推导 record 数组步长。LLVM DataLayout 区分 ABI 对齐和存储位宽；`.NET Vector3` 的 12 字节实例说明紧凑存储可以与 JIT 特化并存。[LLVM DataLayout](https://llvm.org/docs/LangRef.html#data-layout)、[.NET Vector3](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/System/Numerics/Vector3.cs)
- **精度是类型契约。** float 与 double 的向量、四元数和矩阵具有平行功能，混合精度由显式转换表达。DirectXMath 官方明确不支持 double 运算，因此只能作为 float 路径参考或后端；double 路径不能先降到 float。模板化纯数学公式可共享一套算法，使用匹配标量精度的常量和 `sqrt/sin/cos/atan2`。[DirectXMath 内部说明](https://learn.microsoft.com/en-us/windows/win32/dxmath/pg-xnamath-internals)
- **普通浮点先行。** O2 优化与浮点 `fast` 不是同一开关。建议通过 PassBuilder 的标准 O2 module pipeline 接入内联、简化、SLP 和循环向量化，保持普通 FP 指令且不加 `fast`。逐分量运算可向量化；浮点 reduction 重排常要求放松语义。普通 FP 不等于完整的动态舍入模式/异常严格语义；如果未来要求这些，需要 constrained FP。也不能仅凭无 fast 标志承诺跨 CPU、编译器和数学库逐位确定性。[新 PassManager](https://llvm.org/docs/NewPassManager.html)、[LLVM 向量化](https://llvm.org/docs/Vectorizers.html)、[FP 标志与 constrained FP](https://llvm.org/docs/LangRef.html#fast-math-flags)
- **稳定归一化。** 对有限且非零的输入，先算 `m=max(abs(component))`，再算 `u=v/m` 与 `u/sqrt(dot(u,u))`；避免 `dot(v,v)` 对有限的大数溢出、对很小的数下溢。这里必须直接除以 m，不能先算 `1/m`，因为极小 m 的倒数可能溢出。建议安全 API 对零、NaN、Inf 返回失败，并明确失败时输出是否保持不变；普通长度仍可能在真实长度不可表示时返回 Inf。相同策略覆盖四元数。DirectXMath 提供最大分量缩放的 robust 示例，Eigen 的 stable norm 同样使用缩放并处理极端倒数。[robust normalize](https://learn.microsoft.com/en-us/windows/win32/api/directxmath/nf-directxmath-xmvector3normalize)、[Eigen StableNorm 源码](https://libeigen.gitlab.io/eigen/docs-nightly/StableNorm_8h_source.html)
- **旋转与组合有一个约定。** 保持 row-major 存储、row-vector、`p*(S*R*T)`；四元数明确 xyzw 分量、单位旋转 `(0,0,0,1)`、角度为弧度。Hamilton 乘积与 row-vector 的组合顺序需要由 `rotate(rotate(v,a),b)` 和对应矩阵乘法的一致性验收锁定，不能只靠名称。slerp 对负 dot 选择等价的反号四元数，对近同向输入用线性分支，避免除以接近零的 sin；输入单位化前提必须明确。Eigen 的通用 Scalar 实现提供可核对的 float/double 算法和矩阵到四元数的 trace/最大对角分支。[Eigen Quaternion 源码](https://libeigen.gitlab.io/eigen/docs-nightly/Quaternion_8h_source.html)
- **逆矩阵与别名。** 通用逆矩阵应通过失败可观察的 API 暴露奇异或非有限输入；不要用固定绝对 determinant 阈值判断不同尺度矩阵，因为 determinant 不是可靠的条件数。可选择固定大小公式或带 pivot 的消元，但计算结果须有限，阈值语义明确。`out == input`、`a *= a` 等操作先完成局部结果再写回；除非契约保证不重叠，不能加 `noalias/restrict`。Eigen 官方说明小型固定矩阵可用直接公式，也说明表达式别名会影响结果。[Eigen 逆矩阵指导](https://libeigen.gitlab.io/eigen/docs-nightly/group__TutorialLinearAlgebra.html)、[Eigen aliasing](https://libeigen.gitlab.io/eigen/docs-nightly/group__TopicAliasing.html)

本轮端到端验收建议集中覆盖：规范类型身份及 float/double 布局；紧凑三分量数组的末尾元素；双精度无法被 float 精确表示的计算；大值/极小值/零/非有限归一化；原地矩阵乘法；四元数旋转与矩阵组合一致；奇异逆矩阵的失败结果；C++ 和 JIT 在约定的容差内一致。SIMD、寄存器分配与性能收益需要检查实际优化 IR 或机器码及真实场景测量，不能由 vector IR 或 inline 声明推定。

## 已实施的路径

规范名称统一为 `math.vector2/3/4`、`math.quaternion`、`math.matrix4` 与 `d` 后缀双精度类型。Runtime 自动登记同一 C++ 类型，World 不再发布重复数学定义。double 已接入 Scalar、原生反射、64 位私有 ABI、源码常量/算术/字段/调用及标量协程结果。

数学局部与内建运算直接降低为向量 IR，读取/写回采用真实分量布局；模块开启 O2，内部助手可内联。同步数学值参数与返回已经贯通原生和源码，结果使用调用方类型化缓冲，避免扩大每个 Scalar。一般聚合类型、async 数学参数/结果以及跨模块内联未随此增量开放。

审查修复包括缺失字段服务的链接拒绝、原生回调数学结果地址及标量位宽检查，以及矩阵求逆的逐行尺度均衡。求逆先计算 `B = D^-1 A`，再将 `B^-1` 的列按原始行尺度还原，避免有限大数在消元中直接相加溢出，同时保留独立小尺度行。

## 真实数组计算测量

2026-10-03，Windows x64，AMD Ryzen 9 3950X；固定 Clang/LLVM 23.1.1 Release。`atelier_source_minimal --math-measure` 对 65,536 个紧凑位置/速度向量执行 32 次 `position += velocity * dt`。每条路径预热，5 组交错样本取中位数；编译、分配和数据重置在计时外，全部结果逐元素核对一致。

| 32 次更新的中位耗时 | float | double |
| --- | ---: | ---: |
| 向量源码，初始 O2 | 2.0083 ms | 3.0162 ms |
| 向量源码，先 Scalarizer 再 O2 | 0.5577 ms | 1.0378 ms |
| 同批逐分量源码 | 0.6495 ms | 1.0362 ms |
| 同批原生 C++ 循环 | 4.5081 ms | 4.0496 ms |

这项实验保留数学的直接 IR 表达，在 O2 前使用锁定 LLVM 的 `ScalarizerPass` 展开分量，再由循环向量化和 SLP 选择实际宽度。LLVM 明确将重新选择向量宽度列为该 pass 的用途；默认不开启额外 load/store 拆分，与当前紧凑分量访存一致。[Scalarizer 源码](https://llvm.org/doxygen/Scalarizer_8cpp_source.html)、[向量化说明](https://llvm.org/docs/Vectorizers.html)

向量路径在本场景改善约 3.60 倍（float）和 2.91 倍（double），与逐分量源码处于相近水平。不能据此推断所有矩阵、四元数或引擎工作负载都会同幅提速。JIT 使用 detectHost 针对本机 CPU 特化；C++ 对照沿项目默认 x64 编译选项，所以这里只比较当前两条部署路径，不声称等 ISA 的语言性能优劣。原始证据：`out/math-measure-before-scalarize.jsonl` 与 `out/math-measure-release.jsonl`。


参数准备还将标量与数学值统一到按实际大小、对齐排布的 128 字节内联区；引用不占复制空间，超出容量才分配可复用的原始对齐缓冲。这个改动消除了常见单向量/单矩阵调用新增的参数堆分配；上表批量数组场景本来使用借用参数，未用它来推断该改动的吞吐收益。


最终实现复测（相同场景、5 组中位数）：向量源码 float 0.5620 ms、double 1.0446 ms；逐分量源码为 0.5560 / 1.0277 ms，C++ 对照为 3.7995 / 4.9613 ms。与初始向量 lowering 相比约为 3.57 / 2.89 倍，原始数据见 `out/math-measure-final-release.jsonl`。

## 验收与审查

开发与发布配置都通过原生数学、Runtime、反射生成消费者及 CLI、源码公共消费者、JIT 平台/业务、World 和编辑器文档端到端验收；编辑器目标编译通过。完整证据在 `out/math-verified-dev.log` 与 `out/math-verified-release.log`。新增场景包含三分量值紧贴不可访问页面的实际 JIT 读写、数学局部跨 yield、double 位模式/精度、混合参数对齐与扩展暂存、失败不提交结果、原生 ABI 故障与清根、整块对象字段及旧代码租约。

工程规范与需求两路审查共识别三类实际问题，均已修复并定向复核；参数暂存收敛另经一次限定复核，未发现遗留问题。此记录不是对任意后续变更的保证。

发布配置编辑器窗口验收创建了 AMD Radeon RX 6800 XT 的 D3D12 设备，但因外部焦点切换触发保护而中断，未继续发送模拟输入；该窗口验收不计为通过。证据为 `out/math-editor-window-release.log`。没有将窗口交互限制改成忽略焦点，也没有据此声称完成本轮窗口/GPU画面验收。
