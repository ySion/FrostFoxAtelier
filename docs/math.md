# 数学值与双精度

数学值使用同一份 C++ 存储、Runtime 类型身份和源码类型定义。纯计算库位于 [`math/math.hpp`](../src/math/math.hpp)，不依赖 Runtime；Runtime 启动时发布内建数学记录，源码编译器从已验证的类型元数据取得数学分类。名字用于查找，不能使任意同名记录获得编译器特权。

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
- 源码使用严格浮点语义和普通 LLVM O2 优化，不开启全面 fast-math。结果允许 IEEE 无穷和 NaN；不承诺跨 CPU、平台或编译器版本逐位一致。精度策略与 SIMD 使用不能替代确定性协议。

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
| `math.length(v)`、`math.normalize_or_zero(v)` | 向量或四元数；归一化对零或非有限输入返回全零值，四元数全零不是单位旋转 |
| `math.conjugate(q)`、`math.rotate(q, v)` | 共轭；单位四元数旋转同精度 vector3 |
| `math.transpose(m)` | 矩阵转置 |
| `math.transform_point(v, m)`、`math.transform_direction(v, m)` | 同精度 vector3、matrix4，参数顺序符合 `v * M` |
| `math.sqrt(value)` | float/double 平方根；负数沿 IEEE 规则产生 NaN |

字段可从变量、数组元素、借用记录及数学临时值读取，例如 `(a + b).x`。临时值不可赋值；修改需要真正的可写存储。矩阵源码使用 `.m00` 等字段，当前没有矩阵下标语法。对象目录同时提供完整数学字段与标量分量路径，支持 `self.position = self.position + self.velocity * delta;`；对象读写经过 Runtime 生命周期检查，整值读取不会暴露对象内存地址。

原生 C++ 库还提供向量复合赋值、`lerp`、`is_finite`、返回 optional 的 `try_normalize`，以及矩阵加减、标量缩放、`vector4 * matrix4`、`rotation_matrix`、`trs_matrix`、`try_inverse`。这些能力不自动成为源码语法；源码当前以本节表格为准。原生 `try_normalize` 对零或非有限输入返回空值；`try_inverse` 先按行缩放再做带选主元的消元，拒绝非有限、奇异、超出工作精度或不能表示的结果，不隐式回退到单位矩阵。

## 调用、生命周期与优化边界

数学值可作为同步源码及原生方法的按值参数、结果或 `const T&` / `T&` 参数。按值参数是独立副本，数学左值可整体赋值；引用保持只读性和别名关系。同模块辅助函数直接使用定型内部调用，常用数学操作降低为 LLVM 指令；先展开分量，再由 O2 选择跨元素向量化、内联及消除中间值。普通反射原生方法仍沿 Runtime 调用边界，不保证跨原生机器码边界内联。

公共 `Program::Entry` 与 `Lease` 继续使用同一个 `invoke` 入口。标量结果沿原 `Scalar` 返回，其中包含 double；数学结果选择带 `ValueOutput{type_id, &value}` 的重载。宿主提供已构造、大小和对齐正确、类型准确的活存储；调用成功后提交完整结果，失败不提交结果；已经发生的业务修改不回滚，输出与可变参数别名时同样适用。数学按值参数沿共享 `Argument` 准备过程取得快照，不能把传入地址当成延长生命周期的所有权。

数学局部和按值参数无需 String/Array 的构造、赋值或析构服务。数学局部可以保存在 async 帧中跨 `yield` 存活，double 参数及结果也支持 async。当前 async 数学参数与数学结果被明确拒绝，借用参数同样不能跨挂起。

数学**引用参数**仍形成[同步借用区间](native-value-borrows.md)，原生调用必须满足 BorrowSafe 契约；String/Array 和一般拥有型记录仍遵循[作用域清理](source-owned-values.md)。引入可内联数学值没有解除外部存储、容器重分配或对象字段借用的生命周期限制。

常见标量与数学参数快照共用 128 字节内联暂存，超出容量才扩展。真实数组计算的场景、测量结果和目标 ISA 限制见[研究与测量记录](research/engine-math-design.md#真实数组计算测量)；这些结果不代表所有数学操作或整引擎帧时间。
