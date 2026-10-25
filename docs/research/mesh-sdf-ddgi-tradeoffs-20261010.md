# Mesh SDF 与 DDGI 探针内部判定 — 2026-10-10

状态：未来方向研究，Mesh SDF 尚未实现。本文依据 2026-10-10 的闭合几何方案和下文第一手资料；闭合资格修复已进入正式实现并通过验收，见[当前 DDGI 合同](../modules/renderer/scene-lit.md#ddgi-探针场)与[验收记录](../modules/renderer/acceptance.md)。本文不是 SDF 的性能验收或替换承诺。

**结论：未来 Mesh SDF 可以接替部分探针 inside／clearance／relocation 查询，但不能消除实体内外的语义定义。** 当前保留闭合组件修复更合适。若未来已有服务多个消费者的 SDF 资产与场景更新链，可让可靠区域使用 SDF，薄片、近表面和未就绪资源沿现有三角形查询处理；是否更快必须测量总成本。

## 当前代码实际做了什么

- [solid_geometry.hpp](../../src/renderer/solid_geometry.hpp) 在 Scene Lit 首次为不可变 Mesh 建 BLAS 时派生每三角形闭合资格，朝向由自身的位置叉积与作者法线确定；精确位置邻接跨 normal／UV seam，保留作者朝向，开放、非流形、退化或零体积组件不作为内部证据。这是一次性资格分析，不是每帧全网格 inside 测试，也不证明网格无自相交。
- [scene_lit.cpp](../../src/renderer/scene_lit.cpp) 把结果写成每三角形 4 B 的闭合字，随对应 Mesh 的缓存退休，没有另一套业务 Mesh 身份。
- [lit_ddgi.hlsl](../../src/renderer/shaders/lit_ddgi.hlsl) 的 `ddgi_trace_ray` 仅将合格闭合组件的几何背面记为负距离。开放背面仍遮挡，距离为正，并保留既有着色侧。`DDGIUpdate` 每次更新用 32 条固定方向射线做几何决策，超过四分之一为负判为内部；位移仍受网格单元的 `.45` 椭球范围约束。

NVIDIA RTXGI 同样利用固定射线的背面比例及最近距离做分类和 relocation，并限制位移范围；固定几何射线与随机照明估计分工明确。这是采样式判断，并非一般网格的严格点内外证明。官方内容建议还要求墙体有厚度；不能把它的适用条件直接等同于本项目允许开放薄片的合同。[RTXGI DDGIVolume：Relocation、Classification、Fixed Rays、Geometry Configuration](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/DDGIVolume.md#probe-relocation)，[实际 relocation shader](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/ProbeRelocationCS.hlsl)。

## SDF 表示不能自行决定拓扑语义

SDF 要回答两个不同问题：到表面的距离，以及这一侧是否属于实体。距离可从开放三角形计算，但开放薄片没有由封闭边界唯一确定的实体内部。libigl 将 unsigned、pseudonormal、winding-number 等符号方法分开提供，说明“距离”与“符号规则”是独立选择。[libigl signed_distance.h](https://github.com/libigl/libigl/blob/main/include/igl/signed_distance.h)。

| 输入／变换 | 对本项目的含义 |
| --- | --- |
| 正常闭合壳 | 可靠 SDF 的负号可直接提供内部证据；离散分辨率和误差带仍影响边界。 |
| 开放面、薄片 | UDF／双面距离可用于接近检测和遮挡；不能把表面一侧无限半空间默认为实体。加厚、补洞或阈值封闭必须是明确规则。 |
| 一个 Mesh 同时有闭合体与开放片 | 整 Mesh 一个 signed/unsigned 标志不够；烘焙需保留哪些表面能提供内部证据，不能因混合输入把薄片重新变成实体。 |
| 内外嵌套壳、空腔 | 符号应保留作者定义的空腔，不应独立把每个壳都填成正体积实心块。 |
| 自相交／非流形 | 可选择 winding 等规则生成可用体积，但这是具体的体积解释，不是恢复唯一“原意”。 |
| 镜像、非均匀缩放 | 逆变换采样可保持已定义实体的内外关系；世界距离和 relocation 梯度需要另外正确变换。 |

广义 winding number 的原始研究可处理开放边界、自相交和非流形输入，仍要求方向大致一致，并用额外分割构造体积。因此可以采用它作为未来烘焙策略，但不能将“换 SDF”视作省掉该策略。[Jacobson 等，Robust Inside-Outside Segmentation using Generalized Winding Numbers](https://igl.ethz.ch/projects/winding-number/)。

Epic 的 Mesh DF 文档明确区分分辨率损失、薄结构负体素要求和双面生成的额外代价；Lumen 文档也指出极薄结构及单面背面有表示限制。文档中的墙厚建议属于其系统配置，不是 SDF 的普适物理阈值。[Epic Mesh Distance Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine)，[Lumen Technical Details：Software Ray Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine#softwareraytracing)。

## 性能与效果的可比范围

潜在收益是稳定地查询探针位置的距离／符号和推出方向，减少 32 个离散方向遗漏细小障碍或估计最近距离不准的情况。若用有限差分估计梯度，一次距离加六次偏移采样约为七次场采样；每次三线性采样又涉及邻近体素。多个实例仍需空间筛选或合成场，不能假定“全场景一次读取”。这是算法量级分析，不是 GPU 耗时结论。

当前默认值来自 [view_settings.hpp](../../src/renderer/view_settings.hpp)：每帧最多更新 64 探针，各有 128 条随机照明射线。假设本帧更新足额 64 个且均执行照明，则有 `64 × 32 = 2,048` 条固定几何射线，以及 `64 × 128 = 8,192` 条随机照明射线。仅替代前者最多去掉这组主射线数量的 20%，不等于节省 20% 帧时：固定射线不着色，其他射线还产生次级可见性工作，屏幕 receiver gather 也保留独立的 `ddgi_segment_visible`。SDF 点查询更不能代替探针向各方向记录的距离矩、材质命中或入射辐射。

以下仅为**稠密、单通道、每 Mesh 一份、无 mip／边界／压缩／atlas 对齐／工作区**的容量计算；列出 FP32 与 R16 对照，不代表决定降低精度：

| 分辨率 | FP32 单 Mesh 原始容量 | R16 对照 |
| --- | ---: | ---: |
| 32³ | 128 KiB | 64 KiB |
| 64³ | 1 MiB | 512 KiB |
| 128³ | 8 MiB | 4 MiB |
| 256³ | 64 MiB | 32 MiB |

SDF 体素的空间分辨率、单个距离数值的位宽、屏幕 D32／FP32 深度是三个不同维度；提高数值位宽不能恢复体素网格已经漏掉的薄结构。当前屏幕深度及历史的 FP32 合同保持不变。

边长分辨率翻倍意味着八倍容量。当前闭合信息只在 GPU 上保留每三角形 4 B，CPU 分类数组在建 BLAS 后即释放；10 万三角形约 0.38 MiB。因此 SDF 并非必然更占内存：稠密复杂网格可能更省，简单盒子通常更贵；还要算生成、驻留、梯度和场景加速结构。若只替换固定射线而保留照明光追，BLAS/TLAS 成本仍存在。

Mesh 本地场可随刚体实例复用；几何版本变化需要重建。Global DF 则额外承担合成和更新。Epic 的 GDF 是跟随视点的低分辨率 clipmap 缓存，局部更新受影响区域；Lumen 的细节追踪结合近处 Mesh DF 与远处 Global DF，并指出大量重叠实例会增加细节追踪成本。这提供了工程参考，不能据此推断本项目小型 DDGI 体积已有净收益。[Epic Mesh Distance Fields：Global Distance Field](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine#globaldistancefield)，[Lumen Software Ray Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine#softwareraytracing)。

## 必须保留的数值与更新约束

以下为数学推导及本项目设计判断：对实例变换 `x = A p + t`，查询本地 `d(A⁻¹(x-t))` 可保留实体符号，但只有刚体／统一缩放能以单个比例保持精确世界欧氏距离。非均匀缩放下，最小／最大奇异值给出距离幅值的上下界；不能直接按本地距离把探针推出世界空间。梯度方向按 `A⁻ᵀ` 变换。反射本身不需要翻转标量内外符号。Epic 也明确记录其实现对非均匀缩放有局限、镜像可处理；这是实现参考而非禁止改进。[Epic Mesh Distance Fields：Limitations](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine#limitations)。

距离最近点在棱角、中轴或多个重叠物体间可能切换；局部梯度推出一次不保证已经离开所有实体。离散 SDF 的近零带、窄带以外、资源未驻留及更新滞后必须表达为不确定，不能当成已知空处。候选新位置应再验证，并保留现有单元位移限幅、稳定死区、实际移动后历史失效以及移动障碍消失后的重新激活。低分辨率 GDF 不宜独自裁决贴墙探针与薄片。

## 后续取舍

本轮继续使用现有 Mesh 派生分类和真实三角形查询。未来确有共享 SDF 需求时，先把同一几何派生数据生产者的符号／误差／版本／驻留合同确定下来；DDGI 内部可选择可靠 SDF 查询或三角形回退，仍只保留一套探针状态和更新入口。闭合资格可转移到烘焙过程，不能凭表示名称删除语义判断。

候选值得保留的条件，是同一场景下 inside 误判、薄隔墙泄漏、relocation 收敛和运动重激活达到既有合同，同时固定几何阶段节省的时间足以覆盖 SDF 查询、场景更新、驻留及维护代价。比较静态复杂场景与频繁移动场景，记录 GPU 分阶段耗时、长尾和峰值内存；不要由查询次数宣称毫秒收益，也不要为本次探针定位顺带替换全部光追。
