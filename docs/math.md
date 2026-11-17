# 数学值与双精度

数学值使用同一份 C++ 存储、Runtime 类型身份和源码类型定义。纯计算库位于 [`math/math.hpp`](../modules/core/math/include/math/math.hpp)，不依赖 Runtime；Runtime 启动时发布内建数学记录，源码编译器从已验证的类型元数据取得数学分类。名字用于查找，不能使任意同名记录获得编译器特权。

## 原生模块与来源

`math/math.hpp` 是正式聚合入口，定义按职责各存一份，也可直接包含对应头：

- [`type.hpp`](../modules/core/math/include/math/type.hpp)：实数精度约束和数学分类；不创建第二套 Runtime 类型身份。
- [`vector.hpp`](../modules/core/math/include/math/vector.hpp)：紧凑 2/3/4 分量存储、同形状显式精度转换和向量运算。
- [`quaternion.hpp`](../modules/core/math/include/math/quaternion.hpp)：四元数存储、同形状显式精度转换、乘积、共轭、轴角构造、归一化和球面插值/旋转。
- [`matrix.hpp`](../modules/core/math/include/math/matrix.hpp)：4×4 存储、同形状显式精度转换、矩阵运算、平移/缩放/TRS/左手视图构造、求逆及法线变换。
- [`bounds.hpp`](../modules/core/math/include/math/bounds.hpp)：C++ 紧凑三维轴对齐包围盒（`BasicBounds<T>` / `Bounds` / `Boundsd`）、显式空盒单位元 `empty()`、包围扩展、包含/相交测试、点到盒平方距离与仿射包围盒变换 `transform_bounds`；仅供原生 C++ 使用，不注册为源码内建类型。

当前头文件是本项目既有实现的职责拆分，未移植第三方实现，也没有 SIMD 寄存器公共类型、Affine3 或 TRS 计算值。存储布局、默认值、函数名和求值顺序保持不变；`dot` 从第一个乘积开始累加，`length` 使用 `hypot`（四维嵌套），归一化先按最大分量缩放。不能用代数等价的新公式替换这些数值路径。

后续设计参考锁定为 [nfrechette/rtm v2.4.0，d333385e302686529fc2e981e8ba64249554040d](https://github.com/nfrechette/rtm/commit/d333385e302686529fc2e981e8ba64249554040d)（MIT），不构成当前运行期依赖。若实际移植其实现，衍生文件须记录原文件、commit、本项目修改并保留版权通知，同时随源码和部署保留完整 MIT 声明。ExtremeEngine 的本地 SkrBase/math 快照只用于设计参考，未确认来源链，不复制实现。

## 类型、名称与存储

| C++ `atelier::math` 类型 | Runtime 规范名称 | 源码短名 | 大小 / 对齐（字节） |
| --- | --- | --- | --- |
| `Vector2` / `Vector2d` | `math.vector2` / `math.vector2d` | `vector2` / `vector2d` | 8 / 4；16 / 8 |
| `Vector3` / `Vector3d` | `math.vector3` / `math.vector3d` | `vector3` / `vector3d` | 12 / 4；24 / 8 |
| `Vector4` / `Vector4d` | `math.vector4` / `math.vector4d` | `vector4` / `vector4d` | 16 / 4；32 / 8 |
| `Quaternion` / `Quaterniond` | `math.quaternion` / `math.quaterniond` | `quaternion` / `quaterniond` | 16 / 4；32 / 8 |
| `Matrix4` / `Matrix4d` | `math.matrix4` / `math.matrix4d` | `matrix4` / `matrix4d` | 64 / 4；128 / 8 |

无后缀类型使用 float，`d` 后缀使用 double。C++ 还提供 `BasicVector<T, N>`、`BasicQuaternion<T>`、`BasicMatrix4<T>`；`T` 限于 float/double，向量维数为 2、3、4。源码没有通用模板实例化。

源码短名解析到同一个规范类型，不产生第二份注册。`math.*` 为内建名称空间；内建短名不能被类型、函数或局部遮蔽。用户记录仍按声明的完整名称解析。`Array<vector3d>` 只解析已注册的对应原生数组，不自动注册数组类型。

这些类型都是无 GC 引用的平凡值。向量与四元数按 `x/y/z/w` 顺序紧凑存储；矩阵按行连续保存 16 个元素，反射字段为 `m00`～`m33`。JIT 可以使用向量寄存器计算，但读写按真实大小与对齐进行；12 字节 Vector3 的尾部没有可额外读取的第四个分量。GPU buffer 布局仍遵守图形接口自己的契约。

### 名称迁移

原 `world.Vec3`、`world.Quaternion` 已由规范数学类型取代；不额外保留同布局的旧 Runtime 类型或隐式名称回退。引用旧名称的源码须更新。持久格式若保存类型名称或布局，必须通过明确的格式版本执行迁移，或拒绝不兼容数据，不能依赖 C++ namespace 或 Runtime ID 猜测类型。本文不声明已有通用磁盘格式迁移器。

## 默认值、坐标与浮点规则

- 向量默认零；四元数默认 `(0,0,0,1)`；矩阵默认零。C++ 单位矩阵使用 `Matrix4::identity()`，源码使用 `matrix4(1.0)`；单参数矩阵构造把参数放到四个对角元素，其余元素为零。
- 源码构造还允许无参数或完整分量列表：向量 2/3/4 项、四元数 4 项、矩阵按行 16 项。每个参数必须精确匹配分量精度。
- 矩阵使用行向量约定 `v * M`，变换组合为 `S * R * T`，平移位于第 4 行。`transform_point(v, m)` 按齐次分量 1 计算，`transform_direction(v, m)` 按 0 计算；它们用于仿射变换，不做透视除法。
- 四元数乘法使用 Hamilton 乘积。`rotate(q, v)` 要求调用者提供单位四元数，不在每次旋转中隐式归一化。四元数相等比较分量，因此 `q` 与 `-q` 可以表示相同旋转但比较不相等。
- World 对缩放、有效变换和生命周期的限制由 World 维护，通用数学值本身不禁止负缩放或非有限计算结果。
- 源码使用严格浮点语义和普通 LLVM O2 优化，不开启全面 fast-math。结果允许 IEEE 无穷和 NaN；除[超越函数](#超越函数)在支持 FMA3 的主机之间逐位相同外，不承诺跨 CPU、平台或编译器版本逐位一致。精度策略与 SIMD 使用不能替代确定性协议。

原生 C++ 保持 Windows x64 的 SSE2 编译基线，不启用 AVX2/FMA 或 fast-math。依赖分步乘加舍入误差证明的几何与体素路径（`atelier_procedural` 及其验收目标）通过 PRIVATE 编译选项 `/clang:-ffp-contract=off`（clang-cl 透传给 Clang 的 `-ffp-contract=off`）禁止翻译单元内的隐式乘加融合，并保留 BVH 与三角形距离核的局部严格浮点 pragma；`atelier_math` 不再向无关模块传递该选项。这不改变舍入模式、FTZ/DAZ 或显式 FMA，也不改变 JIT 生成 IR 的规则或其 FMA3 主机要求。

`float` 对应 `core.f32`，`double` 对应 `core.f64`。无后缀小数/指数仍是 float，例如 `1.0`、`1e2`；`1f`、`1.0F` 显式表示 float，`1d`、`1.0D` 表示 double。浮点字面量必须有限且在转换范围内。已定型 float/double 不隐式混合；`float(value)` 与 `double(value)` 显式转换 float、double、int 或 uint，窄化由调用者选择。

## 源码运算

```cpp
vector3d advance(vector3d position, vector3d velocity, double delta) {
    return position + velocity * delta;
}

double energy(const Array<vector3d>& values) {
    double total = 0d;
    uint index = 0;
    while (index < values.size()) {
        total = total + math.dot(values[index], values[index]);
        index = index + 1;
    }
    return total;
}
```

| 源码表达式 | 当前契约 |
| --- | --- |
| 向量 `+`、`-` | 同一向量类型逐分量运算 |
| 向量 `*` 标量、标量 `*` 向量；向量 `/` 标量 | 标量与向量精度一致；除零沿 IEEE 规则 |
| 四元数 `*` 四元数；矩阵 `*` 矩阵 | 同类型乘积；矩阵运算为真正矩阵乘法 |
| 数学值一元 `+/-`、`==/!=` | 逐分量正负与精确比较；没有排序或近似相等运算符 |
| `math.dot(a, b)`、`math.cross(a, b)` | 同类型向量；cross 限 vector3，dot 返回对应标量 |
| `math.length(v)`、`math.normalize_or_zero(v)`、`math.fast_normalize_or_zero(v)` | 向量或四元数；归一化对零或非有限输入返回全零值（`fast_normalize_or_zero` 省去分量缩放，见[近似变体](#近似变体)），四元数全零不是单位旋转 |
| `math.conjugate(q)`、`math.rotate(q, v)` | 共轭；单位四元数旋转同精度 vector3 |
| `math.slerp01(a, b, t)`、`math.fast_slerp01(a, b, t)` | 同精度单位四元数与同精度插值标量的 `[0, 1]` 最短弧球面插值；`t` 按 `math.clamp(t, 0, 1)` 钳制（`NaN -> 0`），`dot(a, b) < 0` 时翻转第二操作数（`t = 1` 得 `-b`），`slerp01` 在近共线处采用归一化线性插值，`fast_slerp01` 见[近似变体](#近似变体) |
| `math.transpose(m)` | 矩阵转置 |
| `math.transform_point(v, m)`、`math.transform_direction(v, m)` | 同精度 vector3、matrix4，参数顺序符合 `v * M` |
| `math.sqrt`、`fast_rsqrt`、`floor`、`ceil`、`trunc`、`fract`、`abs`、`sign`、`min`、`max`、`clamp`、`lerp`、`step`、`smoothstep` | float/double 标量与向量的逐分量函数，见下节与[近似变体](#近似变体) |
| `math.sin`、`cos`、`atan2`、`exp`、`log`、`pow` | float/double 标量与向量的逐分量超越函数，见[超越函数](#超越函数) |

### 逐分量函数

参数和结果是 float、double 标量或它们的 vector2/3/4。结果类型取参数中的向量类型，没有向量时为标量；每个参数必须是结果类型或其分量标量，分量标量作用于每个分量，例如 `math.clamp(v, 0.0, 1.0)`、`math.lerp(a, b, t)`、`math.step(0.5, v)`。精度不隐式转换：不带后缀的浮点字面量是 float，double 向量要写 `0.0d`。四元数、矩阵、整数和 bool 编译期拒绝。以下定义按分量给出，x、y 为同类型分量：

| 函数 | 定义与特殊值 |
| --- | --- |
| `sqrt(x)` | IEEE 平方根；负数得 NaN，`sqrt(-0) = -0` |
| `floor(x)`、`ceil(x)`、`trunc(x)` | IEEE 取整；保留 ±0，NaN 与 ±∞ 原样返回 |
| `fract(x)` | `min(x − floor(x), 1 的前一个浮点数)`，结果在 [0, 1)：极小负数不会得到 1.0，`fract(x) * n` 可直接作下标；NaN 和 ±∞ 得 NaN |
| `abs(x)` | 清除符号位，含 −0 与 NaN |
| `sign(x)` | `x > 0 ? 1 : (x < 0 ? −1 : x)`；±0 原样返回，NaN 得 NaN |
| `min(x, y)`、`max(x, y)` | GLSL 定义：`y < x ? y : x`、`x < y ? y : x`；只有第一个参数为 NaN 时结果为 NaN |
| `clamp(x, lo, hi)` | `max(lo, min(x, hi))`；x 为 NaN 得 lo，lo > hi 时得 lo |
| `lerp(a, b, t)` | `a·(1 − t) + b·t`（GLSL mix）；有限值下 t = 0 精确得 a、t = 1 精确得 b；不保证单调，`lerp(a, a, t)` 可能差 1 ULP |
| `step(edge, x)` | `x < edge ? 0 : 1`；NaN 得 1 |
| `smoothstep(e0, e1, x)` | `t = clamp((x − e0) / (e1 − e0), 0, 1)`，返回 `t·t·(3 − 2t)`；e0、e1 是同一个有限值时为阶跃（x ≤ e0 得 0，x > e0 得 1；+0 与 −0 按公式算，不视为同一个值），e0 > e1 时方向相反；e0 ≠ e1 时 x 为 NaN 得 0 |

这些函数在调用点内联为 LLVM 指令，不调用任何函数，也不加 fast-math，编译器不会自动把乘加融合；数组循环由 O2 的循环向量化按主机 SIMD 宽度打包。x64 上 min/max/clamp 是 `minps`/`maxps`，floor 类是 `roundps`。源码 JIT 要求主机 CPU 支持 FMA3（这类主机都有 floor 类所需的 SSE4.1）：创建 JIT 后端时检查，缺少时编译失败并写明原因。

字段可从变量、数组元素、借用记录及数学临时值读取，例如 `(a + b).x`。临时值不可赋值；修改需要真正的可写存储。矩阵源码使用 `.m00` 等字段，当前没有矩阵下标语法。对象目录同时提供完整数学字段与标量分量路径，支持 `self.position = self.position + self.velocity * delta;`；对象读写经过 Runtime 生命周期检查，整值读取不会暴露对象内存地址。

原生 C++ 库还提供同形状数学值的显式 `float` ↔ `double` 转换、向量复合赋值、`cwise_mul`、`cwise_div`、`cwise_min`、`cwise_max`（同源码 `min`/`max` 的 GLSL 顺序，仅首操作数为 NaN 时保留 NaN）、`cwise_abs`、`clamp`（支持向量或标量上下界，语义同源码 `math.clamp`）、`lerp01`（先将参数按 `math.clamp(t, 0, 1)` 钳制到 `[0, 1]` 再调用 `lerp`，`t` 为 NaN 时归零）、`min_component`、`max_component`（按左折叠顺序调用 `min`/`max`，不传播后续分量的 NaN，校验未信任输入时须配合 `is_finite`）、`length_squared`、`distance_squared`、`distance`、`is_finite`、返回 optional 的 `try_normalize`、标量与向量的 `fast_rsqrt`、向量与四元数的 `fast_normalize_or_zero`，四元数 `dot`、`from_axis_angle`（要求单位轴与弧度角）、精确 `slerp01` 与近似 `fast_slerp01`（要求单位四元数，先按 `math.clamp(t, 0, 1)` 将插值参数钳制到 `[0, 1]`，`dot < 0` 时翻转第二操作数以取最短弧故 `t = 1` 得 `-unit_b`）、三维轴对齐包围盒 `BasicBounds<T>`（默认零盒，`empty()` 为 `{+∞, -∞}`，是 `encapsulate` 与 `distance_squared` 唯一合法的空盒单位元；任意有限反转盒虽满足 `is_empty` 但不可作为归约单位元；另提供 `is_finite`、`center`、`size`、`extents`、`corner`、`contains`、`intersects` 及按列极值精确传播的 `transform_bounds`），以及矩阵加减、标量缩放、`vector4 * matrix4`、`translation_matrix`、`scale_matrix`、`rotation_matrix`、`trs_matrix`、左手行主序视图矩阵 `try_look_to_lh` / `try_look_at_lh`（对零、非有限、与上方向共线的视线或平移点积溢出的输入返回空值）、`try_inverse`。这些能力不自动成为源码语法；源码当前以本节表格为准。原生向量 `lerp` 与源码 `math.lerp` 使用同一公式。原生 `try_normalize` 对零或非有限输入返回空值；`try_inverse` 先按行缩放再做带选主元的消元，拒绝非有限、奇异、超出工作精度或不能表示的结果，不隐式回退到单位矩阵。

### 近似变体

原生精确 `slerp01` 在近共线处直接返回归一化线性插值，省去三角函数调用。以精度换取吞吐的变体在 C++ `atelier::math` 与源码 `math.*` 中统一使用显式 `fast_` 前缀，fast-math 许可仅作用于变体内的指定指令。当前提供的近似变体及其契约如下（可编程材质 HLSL 子集拒绝近似变体与四元数插值）：

| 函数 | 支持类型 | 误差上界与定义域 | 端点与特殊值 |
| --- | --- | --- | --- |
| `fast_slerp01(a, b, t)` | 同精度单位四元数 + 同精度标量 | 全弧段最短弧无分支多项式反翘曲（零反三角/正弦/开方/除法）；相对精确 `slerp01` 的最大旋转角误差 float ≤ `1e-4` rad、double ≤ `1e-7` rad，输出范数误差同阶 | `t` 按 `math.clamp(t, 0, 1)` 钳制（`NaN -> 0`）；`t = 0` 精确返回 `a`，`t = 1` 精确返回最短弧目标 `±b` |
| `fast_rsqrt(x)` | float/double 标量与 vector2/3/4 | 正的有限正规数相对 `1 / sqrt(x)` 的误差 float ≤ `1.5e-4`（源码 JIT 允许后端使用硬件倒数平方根估计与 1 步牛顿迭代，C++ 为位初值 + 2 步迭代）、double ≤ `1e-10`（位初值 + 3 步迭代） | 正次正规数返回 `+∞`，`±0 -> ±∞`（保符号），`+∞ -> +0`，负数与 `NaN -> NaN` |
| `fast_normalize_or_zero(v)` | float/double 向量与四元数 | 省去精确版的最大分量预缩放除法，直接对 `dot(v, v)` 调用快速倒数平方根；在 `dot(v, v)` 为正规正数的区间内方向与单位长度误差同 `fast_rsqrt` | 平方长度为零、次正规数、无穷或 `NaN` 时返回全零值 |

### 超越函数

`math.sin(x)`、`math.cos(x)`、`math.exp(x)`、`math.log(x)`、`math.atan2(y, x)`、`math.pow(x, y)` 接受 float、double 标量和它们的 vector2/3/4，按参数类型选择 float 或 double 实现；标量参数作用于每个分量，其余类型规则同上，float 与 double 不混合。整数、四元数和矩阵在编译期拒绝。

精度契约：与精确值相差不超过结果类型的 4 ULP。sin、cos 的准确区间 float 是 |x| ≤ 2^20，double 是 |x| ≤ 2^40；超出后结果仍有限且绝对值不超过 1，但不再近似正弦、余弦。

下表保留实现阶段的 release 测量，原 `atelier_source_measure --math-sweep` 入口已移除，不代表当前验收重跑过大规模扫描。float：sin、cos、exp、log 穷举全部 2^32 个 float，atan2、pow 各约 10.4 亿组随机参数，另加 111 万组边界网格，误差相对精确值。double 的[原分支测量](https://ampcode.com/threads/T-01a149c9-15b7-75a9-9a24-80c450cd4aed)每函数 1,002,438,656 组参数，覆盖归约边界附近、溢出与次正规区间和随机位模式；sin/cos 的随机输入限于准确区间。double 列记录与 [CORE-MATH](https://core-math.gitlabpages.inria.fr/) 正确舍入结果的相邻浮点数步长距离，是抽样结果，不是全域误差证明或跨 CPU 实测。

| 函数 | float | double |
| --- | --- | --- |
| `sin` | 1.669 ULP（\|x\| ≤ 1e5 内 1.570） | 1 ULP（99.43% 正确舍入） |
| `cos` | 1.807 ULP（\|x\| ≤ 1e5 内 1.572） | 1 ULP（99.43% 正确舍入） |
| `exp` | 1.009 ULP | 1 ULP（93.90% 正确舍入） |
| `log` | 1.162 ULP | 1 ULP（97.43% 正确舍入） |
| `atan2` | 1.867 ULP | 1 ULP（99.97% 正确舍入） |
| `pow` | 0.565 ULP | 1 ULP（90.95% 正确舍入） |

特殊值按 IEEE 约定，pow 的负底数例外：

- `sin(±0) = ±0`，`cos(±0) = 1`；±∞ 与 NaN 得 NaN。
- `exp(−∞) = +0`，`exp(+∞) = +∞`；x 大于约 88.72（double 约 709.78）时溢出为 +∞，下溢先变为次正规数再到 +0。
- `log(±0) = −∞`；负数与 −∞ 得 NaN；`log(+∞) = +∞`，`log(1) = +0`。
- `atan2` 按 C 标准附录 F，例如 `atan2(±0, −0) = ±π`、`atan2(±0, +0) = ±0`、`atan2(±∞, −∞) = ±3π/4`、`atan2(±∞, +∞) = ±π/4`；float 结果在 [−float(π), float(π)] 内，float(π) 比 π 略大；double 结果在 [−double(π), double(π)] 内，double(π) 比 π 略小。任一参数为 NaN 得 NaN。
- `pow(x, ±0) = 1` 与 `pow(1, y) = 1` 对任意 x、y 成立，含 NaN。x < 0（含 −∞）时得 NaN，即使 y 是整数。±0 都按 +0 处理：`pow(0, y > 0) = +0`，`pow(0, y < 0) = +∞`。底数或指数为 ±∞ 时取极限值；其余情况 NaN 原样传播。

实现全部是调用点上没有函数调用的 IR，使用 FMA，与其他逐分量函数一样随循环向量化，AVX2 主机上一次处理 8 个 float 或 4 个 double。float 版：

- sin、cos：用 FMA 的三段 Cody-Waite 归约 r = x − q·π/2，在 |r| ≤ 0.83 上用 minimax 多项式，按象限选择正弦或余弦多项式及符号。
- exp：归约为 2^k·e^r，2^k 分两次相乘，次正规结果只舍入一次。
- log：x = 2^e·m，m ∈ [√½, √2)，对 log(1 + f) 用 minimax 多项式。
- atan2：交换参数使比值 u/v ≤ 1；比值大于 1/2 时用 π/4 + atan((u − v)/(u + v))，此时 u − v 是精确的。
- pow：在 double 中计算 exp2(y·log2|x|)，最后舍入到 float 一次。

double 版把归约后的参数和主要项保留为双字，即精确和等于该值的两个 double，由 FMA 的无误差乘法与加法得到：

- sin、cos：π/2 拆成三个 double 常数，x − q·C1 由 FMA 精确得到，q·C2 整个保留为双字，第三段之后的截断远小于 |x| ≤ 2^40 内最小的 |r|（约 2^-60.5，离线用格归约逐个二进制段搜出最难的参数）；在 |r| ≤ 0.786 上分别用正弦、余弦 minimax 多项式。
- exp：ln2 的高位只有 32 位有效数字，n 乘它是精确的；r 保留为双字，12 项多项式用 Estrin 求值以缩短依赖链；2^k 同样分两次相乘。
- log：归约同 float，按 fdlibm 的 f − (f²/2 − s·(f²/2 + R)) 形式，s = f/(2 + f)，R 为 7 项多项式。
- atan2：从最近的 atan(c)（c ∈ {0, 1/4, 1/2, 1}）出发，约化比值 (u − c·v)/(v + c·u) 不超过 0.163，u − c·v 是精确的，除法保留余数；最后以双字加上 π/2 或 π。
- pow：log|x| 以双字算到约 2^-68 的相对误差，y·log|x| 也保留为双字，交给 exp 的核心；特殊值规则与 float 版相同。

精确超越函数与普通数学运算的结果只取决于 IEEE 基本运算与 FMA，不使用查表或随 CPU 变化的近似指令（仅显式 `fast_rsqrt` / `fast_normalize_or_zero` 单精度路径在开方及取倒数指令上局部标记 `afn`、`arcp`、`reassoc`、`contract`，允许后端选择硬件倒数平方根估计与 1 步牛顿迭代，不影响同函数内其他运算）。在相同的默认浮点环境（nearest-even 舍入、FTZ/DAZ 关闭）下，且使用同一套保留既定运算顺序与显式 FMA 的严格 IR/后端时，精确运算在支持 FMA3 的主机上逐位相同，标量调用与向量化循环也逐位相同；修改浮点环境或放宽编译规则不在此保证内，入口不替宿主隔离或恢复浮点环境。前端不向精确运算赋予 fast-math 许可，优化后仍禁止重排、隐式融合、近似或次正规数清零；优化器证明成立的 `nnan/ninf/nsz` 不在此禁令内。NaN 结果只保证是 NaN，符号与载荷不作保证。原生 C++ 代码使用 `<cmath>`，与源码的超越函数结果不保证逐位相同，两边各自满足自己的精度。

超越函数目前没有自动验收；上表是历史测量，不由验收重跑。

## 调用、生命周期与优化边界

数学值可作为同步源码及原生方法的按值参数、结果或 `const T&` / `T&` 参数；同步 `const T&` / `T&` 数学引用参数还可通过 `Program::Entry/Lease::bind` 绑定定型调用句柄。按值参数是独立副本，数学左值可整体赋值；引用保持只读性和别名关系。同模块辅助函数直接使用定型内部调用，常用数学操作降低为 LLVM 向量指令：四元数与四阶矩阵保持原生 SIMD 向量读写和置换（单精度转置在 O2 之后展开为四行道内交织，双精度转置交给后端选择置换指令），含三维或双分量单精度向量（含单精度四元数球面插值辅助对）的函数先展开分量再由 O2 执行跨元素满宽向量化、内联及消除中间值。普通反射原生方法仍沿 Runtime 调用边界，不保证跨原生机器码边界内联。

公共 `Program::Entry` 与 `Lease` 继续使用同一个 `invoke` 入口。标量结果沿原 `Scalar` 返回，其中包含 double；数学结果选择带 `ValueOutput{type_id, &value}` 的重载。宿主提供已构造、大小和对齐正确、类型准确的活存储；调用成功后提交完整结果，失败不提交结果；已经发生的业务修改不回滚，输出与可变参数别名时同样适用。数学按值参数沿共享 `Argument` 准备过程取得快照，不能把传入地址当成延长生命周期的所有权。

数学局部和按值参数无需 String/Array 的构造、赋值或析构服务。数学局部可以保存在 async 帧中跨 `yield` 存活，double 参数及结果也支持 async。当前 async 数学参数与数学结果被明确拒绝，借用参数同样不能跨挂起。

数学**引用参数**仍形成[同步借用区间](native-value-borrows.md)，原生调用必须满足 BorrowSafe 契约；String/Array 和一般拥有型记录仍遵循[作用域清理](source-owned-values.md)。引入可内联数学值没有解除外部存储、容器重分配或对象字段借用的生命周期限制。

常见标量与数学参数快照共用 128 字节内联暂存，超出容量才扩展。真实数组计算的场景、测量结果和目标 ISA 限制见[研究与测量记录](research/engine-math-design.md#真实数组计算测量)；这些结果不代表所有数学操作或整引擎帧时间。
