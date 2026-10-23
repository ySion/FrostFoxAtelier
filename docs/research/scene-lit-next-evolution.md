# Scene Lit 后续方向

本页列出 Scene Lit 尚未实现、已有设计依据的方向。已实现的合同见 [Scene Lit](../modules/renderer/scene-lit.md)，各验收入口证明的内容和当前成本见[验收记录](../modules/renderer/acceptance.md)。外部论文和 SDK 的能力不代表本引擎已经具备。

## 几何前端与剔除

现状：Lit、阴影、工作室光栅和 RT 命中共用一份索引几何（焊接顶点、索引缓冲和每三角形 16 B 记录），BLAS 按索引构建；记录按网格内的全局三角形下标寻址，可以直接作为 meshlet 的输入。实例按几何分组实例化，CPU 按实例做视锥剔除；没有遮挡剔除、LOD 或网格簇剔除。相机环绕时方向级联每帧重画整个场景。新 Sponza（573.6 万三角形）上的分项成本见[验收记录](../modules/renderer/acceptance.md)。

- 子网格剔除有两条候选：CPU 按 part 包围盒剔除并合并相邻区间；或 task/mesh shader 的 meshlet 路径，逐 meshlet 做视锥、级联和背面锥剔除。后者也可能承担程序化实例集合需要的逐实例 GPU 剔除与 LOD。选择前要比较两边的工作量和预计收益。

## 阴影

- 只有硬阴影：球光半径与太阳角半径只影响着色，不产生半影。
- 相机移动时方向级联整级重画，没有静态投影者的级联缓存（滚动级联或页式缓存）。

## DDGI 覆盖与调度

- 现在最多四个人工布置的固定或跟随体积，World Settings 提供 Fit Scene。可以考虑的方向是自动布点、room/portal、稀疏砖块，以及用 [Mesh SDF](mesh-sdf-ddgi-tradeoffs-20261010.md) 做实体内部判定和距离查询。SDF 不能自动解决开放表面与实体内部的语义问题。
- 调度可以按变化原因分开排队：几何维护与辐射更新各自排队，稳定区域降频，同时保留最低维护频率、年龄上限和新区域优先，不能让被冻结的探针自己去发现变化。[S1]
- 网格发光只经 DDGI 照亮其他表面，小发光体附近的照度会被探针插值抹平。屏幕直接光只做确定求值，不对发光面随机采样；要加发光面的直接采样，需要先定义它与 DDGI 发光记录的分工，并解决随机采样带来的降噪需求。

## 较远间接光

用户已决定保留 DDGI 主线，并在相同预算下比较两条路线：“改良 DDGI 加较远 SSGI”，以及“表面探针加世界缓存”。比较条件见 [GI 比较条件](gi-comparison-contract-20261016.md)，Capsaicin 的源码核对见 [Capsaicin GI](capsaicin-gi-20261015.md)。

## 反射

- SSSR 在屏外、被遮挡或追踪失败时回退到天空的立体角均值。可以比较反射探针，或者对缺失部分用硬件 RT 并复用命中材质与光照；AMD GI-1.1 按粗糙度分配反射工作，可作参考。[S2]
- 粗糙反射只保留一个来源见证。主要来源不变、次要来源改变时，时间响应还没有专门的验收。

## 天空

- HDRI 里的太阳不提取为方向光，只经 DDGI 和反射照亮场景，没有清晰阴影。
- 解码和上传都在一帧内同步完成，4K 贴图的上传帧 CPU 约 34 ms。分帧上传和异步解码一起做。

## 未实现

透明混合与折射、蒙皮 BLAS、超分辨率、逐实例属性流，以及大世界所需的相机相对渲染或原点重置。最后一项等它成为需求时整体设计，不为单个函数单独补精度。

## 来源

| 编号 | 来源 |
|---|---|
| S1 | [RTXGI-DDGI DDGIVolume 文档](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md)、[Production DDGI 论文](https://jcgt.org/published/0010/02/01/paper.pdf)：固定射线、分类、滚动、分层与更新策略 |
| S2 | [AMD GI-1.1](https://gpuopen.com/learn/gi-1-1-glossy-reflection-rendering/)：屏幕 probe 与世界 hash radiance cache、硬件 RT 与按粗糙度分配的反射求值 |
| S3 | [Flax 渲染管线源码核对](flax-rendering-comparison-20261017.md)：阴影静态与动态分离、失效与更新预算 |
