# 可编程材质、OpenPBR 闭包与 `.atx` 压缩纹理

本页描述当前 MS/MI 材质系统、`.source` 四路编译切片、OpenPBR v1.1 `24B/px` Canonical Slab 编码及 `.atx` BCn 纹理管线。端到端验收证据以 [Renderer 验收](acceptance.md#--material-programs) 与 `out/acceptance-gate/` 日志为准。语言语法合同见 [LANGUAGE.md](../../../LANGUAGE.md#材质纯值编译与具名纹理输入)，工程文件与编辑历史见 [工程工作流](../editor/projects.md)。

## 数据与四路编译切片（`SliceProjector`）

- **MS（Material Source）** 定义 `.source` 源码、参数默认值、具名纹理输入及静态策略（`Coverage::{opaque, mask}`、`double_sided`、`emission_two_sided`）。
- `source::MaterialProgram` 从同一类型化 AST 前端经 `SliceProjector` 生成四路自包含目标，消除无关分支与死纹理采样：
  1. **CPU 纯值求值**：供接受帧精确点拾取、属性预览与离线验证调用。
  2. **`hlsl_coverage`**：仅保留计算 `Output.coverage` 所需的最小参数与纹理子集（LOD0 采样），专供 Masked VisBuffer 光栅 PS 与直接光 Masked RayQuery AnyHit 使用。
  3. **`hlsl_footprint`**：传播 `VisUnpackAndClassify CS` 解析求得的透视重心偏导 `(c_dx, c_dy)`，对常规 UV 通过单次 `SampleGrad` 执行硬件各向异性过滤并采集法线贴图 Toksvig 方差（链式纹理扰动 UV 自动切换差分偏导），输出完整 OpenPBR v1.1 `material.Output`。
  4. **`hlsl_gi_hit`**：自动剪除二次漫反射弹射不可见的切线空间细节法线贴图、TBN 重建指令及未使用的 `coverage` 纹理分支，并将 OpenPBR v1.1 §3.10 方向—半球有效反照率（含高光权重/IOR/着色、次表面反照率、清漆吸收、天鹅绒消光与薄膜均值）能量守恒折叠为 `effective_albedo` 与薄叶片透射项 `transmission`（满足白炉守恒 $\rho_{\text{eff}} + \tau_{\text{eff}} \le 1$），供 `SecondaryGiHit CS` 高吞吐求值；若极端材质图超出切片节点预算则自动回退完整材质包装求值。
- **MI（Material Instance）** 支持任意深度无环继承链（MS→MI→MI）、稀疏类型化覆盖与细粒度撤销历史；`ProgramSurface` 自持不可变编译产物、字节参数块与 `ImageRef` 集合，不持有 Runtime GC 根。

## OpenPBR v1.1 `24B/px` Canonical Slab（`3 × 8B`）

Scene Lit 将几何解包、材质求值与光照消费彻底解耦为三段：`MaterialShade Bin CS` 输出总计 **24 B/px** 的三张紧凑纹理，使后续所有光照/反射通道完全独立于 `.source` 代码复杂度：

| 目标纹理 | 格式 | 承载字段 |
| --- | --- | --- |
| `Slab0_SpecNormal` | `RG32_UINT`（8 B） | `SNORM16×2` 八面体着色法线、滤波后粗糙度（8b）、金属度（6b）、平方根编码电介质 `F0`（6b，覆盖 `[0..0.25]` 即 `IOR ∈ [1.0, 3.0]` 且零误差保持默认 `0.04`）、材质微遮蔽 `micro_ao`（6b）、按最大活跃闭包权重选出的 `ProfileID`（3b）与表面有效/双面标志（3b） |
| `Slab1_AlbedoClosure` | `RG32_UINT`（8 B） | 平方根 `10:10:10` 基础色 + 2b EON 漫反射粗糙度；第二字按 `ProfileID` 编码六类 OpenPBR 闭包副参数 |
| `Slab2_Emissive` | `RGBA16_SFLOAT`（8 B） | HDR 自发光辐亮度；仅发光像素写入并置位 `8×8` 瓦片位掩码 `TileEmissiveMask`，非发光瓦片在 `SceneCompose CS` 中直接跳过 `Slab2` 显存读取 |

当前纳入通用主循环的六类 `ProfileID` 为：`0` 标准金属/电介质（含 Lazanyi-Schlick `F82` 金属掠射边缘着色）、`1` 电介质清漆层（`DielectricCoat`）、`2` 天鹅绒微纤维（`Fuzz` Estevez & Kulla Charlie NDF + 能量守恒 LUT）、`3` 薄叶片双面漫透射（`ThinFoliage`）、`4` 解析光谱薄膜干涉（`Iridescent`，Belcour & Barla 2017 光程差相位干涉，`[100..1000nm]`）及 `5` 间接派发门控可分离次表面散射（`Subsurface`，仅当帧存在有效次表面像素时激活 `LitDirectSubsurfaceX/Y` Jiménez SSSSS，否则零工作组派发）；各向异性高光（`Anisotropy`）移出 v1 通用主循环，仅保留 `ProfileID` 扩展枚举位。

## 作者格式、编辑与历史

唯一材质持久化格式为 v2（`.ms.material` 与 `.material`，配合 Scene v10 场景内实例与槽映射），不保留旧版兼容加载器。参数 Undo 仅记录所触及路径的前后作者项；同名共享资产修改在目录已知依赖未新增时直接热发布材质与参数，免除全工程目录磁盘扫描。

## 资源发布与默认替代

缺失关联、休眠或类型失配项保留作者数据并生成结构化诊断；单项不可用回退父内容或声明的线性 `empty_value`，损坏材质回退真实 `.source` 编译的不透明品红程序，网格未绑槽使用内置 **Default World Grid**。

## `.atx` 压缩纹理与 GPU 批次（`gpu-批次与实际显示`）

### GPU 批次与实际显示

- `Renderer::prepare_materials(span<ProgramSurfaceRef>)` 按精确唯一 `MaterialProgram` 原型复用/增量编译基础计算分桶 PSO（`shade_bin_cs`、`studio_bin_cs`、`gi_hit_cs`），并为存在 `Mask` 表面的原型独立隔离编译 `masked_vis_ms` 与解耦 `MaskedCoverageSet` 的 `ray_shadow_masked_pso`（Masked 编译状态与失败隔离，绝不阻塞或污染共享同一原型的 Opaque 批次）。标量/向量参数、贴图绑定或 MI 顺序变更零着色器重编。
- 材质纹理与批次缓冲在 `preparation(batch)` 阶段先对未驻留的唯一 `(Image, AtxRole)` 组合并行加载/烤制 `.atx`，再于 owner 线程合并进单次非阻塞 `UploadBatch` 批量提交（由 `UploadBatch` 强引用保活所有暂存 `.atx` 载荷直至拷贝完成，上传完成后对唯一纹理并行执行 `Image::evict_cold_pixels()` 释放冷未压缩像素），纹理经 [atx_texture.cpp](../../../modules/engine/assets/src/atx_texture.cpp) 离线/磁盘缓存烤制为 `.atx` v4（魔数 `ATEX`，512B 切片偏移对齐 + 256B 行距对齐的全金字塔 mip）：
  - 无透明 `color_srgb` → `BC1_RGBA_SRGB`；带透明 `color_srgb` → `BC7_RGBA_SRGB`（预乘线性加权盒滤生成各层 sRGB mip 以消除镂空黑边）
  - 灰度无透明线性标量贴图 → `BC4_R_UNORM`（采样自动广播至 RGB，保持任意单通道/三通道读取语义一致）
  - 多通道或带透明线性贴图（如打包 ORM、透明度贴图） → `BC7_RGBA_UNORM`
  - 强类型切线法线（`material.NormalTextureInput` + `sample_normal`） → `BC5_RG_UNORM`（逐 mip 三维单位化并直接在 `.atx` 头部导出 `||N_avg||` 供 Toksvig 粗糙度补偿斜率计算，着色器采样后精确重建 `+Z`）
  - 等距柱状 HDRI 天空（`SkyMap` GGX 粗糙度预滤波 mip 链） → `BC6H_UF16`
  - 尺寸小于 `4×4` 或非 4 倍数的极小合成测试贴图（如 `1×1` 白图、`256×1` 渐变条）自动保持无损 `RGBA8`，确保逐纹素解析真值零压缩失真。
- `ViewportView.scene` 仅在 `FrameStatus::accepted` 时替换；宿主视口点选与框选优先消费 GPU `RG32_UINT` 拾取缓冲与硬件深度，无 GPU 目标的纯无头验证回退 CPU 精确几何与 `MaterialProgram` LOD0 覆盖求值。

## 采样与各通道

- 主表面（Scene Lit Slab 与工作室着色）通过 `VisUnpack` 解析重心偏导执行 `atelier_material_sample_grad`；非有限或极端溢出梯度安全回退至最粗 mip。
- 直接光阴影 RayQuery AnyHit、Masked VisBuffer 与 CPU 点拾取对 `Mask` 表面执行 `coverage >= 0` 精确裁剪；GI 内部主射线（`Screen Probes`）固定采用 `RAY_FLAG_FORCE_OPAQUE` 硬件不透明遍历并在命中后调用 `hlsl_gi_hit` 读取完整自发光与反照率（保留视锥外负 coverage 发光面经 GI 照亮场景的契约）。
- `sample_orientation` 根据实际绑定的 `Repeat / Clamp / Mirror` 寻址模式返回折叠方向符号。
