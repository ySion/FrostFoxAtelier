# Scene Lit

本页维护当前已实现的 Scene Lit 光照、阴影、全局光照、反射、大气与后处理契约。视口与几何执行链见 [Renderer 合同](contract.md)，OpenPBR 材质与纹理编码见 [材质系统](materials.md)，验收覆盖与历史性能记录见 [验收记录](acceptance.md)。

Scene Lit 是 `Renderer::render(Frame)` 的物理光照着色模式，消费同一 `World` 的 `EnvironmentComponent`（`rendering::SceneLightingSettings` 作者值，随 Scene v10 保存并参与撤销）、`LightComponent` 与可编程材质快照，经线性 Arena `atelier_rdg` 在同一 `rhi::Device` 上单次编译提交。

## 材质和真实灯光

- 表面由 `VisBuffer → VisUnpackAndClassify CS → MaterialShade Bin CS` 解耦求值为 `3×8B` OpenPBR v1.1 Canonical Slab（支持标准 F82 金属/电介质、清漆层 Coat、天鹅绒 Fuzz、薄叶片双面透射 Foliage、薄膜干涉 Iridescent 与次表面散射 Subsurface 六类闭包，及 HDR 自发光）。
- 解析光源单位保持物理一致：点光/球光与聚光灯（`Spot`，平滑内外锥角衰减 + 单视图透视 VSM）使用 candela（带米制球半径 `source_radius` 与 `range`），矩形面光源使用 $\text{cd/m}^2$（米制 `width × height` 与单/双面设置），方向光使用 lux（带弧度角半径）。
- 三态环境天空由 `SceneLightingSettings::sky_mode` 控制：`none` 为零环境与纯黑背景；`physical_atmosphere` 采用 Hillaire 2020 预计算透射率与多重散射 LUT 物理大气模型；`hdri_sky_map` 使用 `Frame.environment`（`sky_color × sky_intensity`）乘等距柱状 `rendering::SkyMap`（离线/导入预计算的 `.atx` v4 `BC6H_UF16` GGX 粗糙度预滤波 mip 链、Ramamoorthi 9 系数旋转 `SH3` 漫射辐照度与 `256×128` 重要性采样 CDF，峰值超 `65504` 时按 $2^{-k}$ 无损缩放并在着色时乘回），`sky_rotation` 绕 `+Y` 顺时针旋转且与方向光正旋转严格一致。

## 直接光与三级阴影闭环

屏幕直接光无随机选灯、无 reservoir、无时间滞后历史，灯光、材质与遮挡编辑当帧确定生效：

1. **`16×16×32` 三维视锥 + `Z-Bitmask` 并行分簇（`LitLightCluster`）**：先在 CPU 侧将方向光与局部光源稳定分区，再以 256 线程并行对每个 `16×16` 屏幕瓦片统计视口内接收面的精确有符号 `ViewZ` 包络、Gribb-Hartmann 四平面瓦片视锥与 32 位对数深度占用掩码，为每瓦片按确定性升序紧凑构建保守局部光源列表与逐灯 32 位深度掩码（溢出 64 盏时自动回退全表精确求值，不丢弃任何光源贡献）；开启 `direct_cluster_inspect` 时额外统计有效接收面的局部光源覆盖直方图（`0..64` 及 `>64`），定时路径零诊断开销。
2. **三级全尺度直接光阴影闭环（`LitVsmUpdate*` + `LitDirectShadow*` + 内联微阴影）**：
   - **宏观级（`0.2m – ∞`）动静分离软件分页 VSM + 紧凑 `PenumbraQueue` 半影光追**：维护 `128 MiB` 硬件 `D32_SFLOAT` 物理页池（`1,024` 张 `128×128` 持久静态基底页 `vsm_static_pool` + `1,024` 张瞬态动态覆盖页 `vsm_dynamic_pool`，保留硬件 Early-Z 与 HTILE，由 Mesh Shader 四平面齐次 `SV_ClipDistance0` 严格裁剪页界并支持按原型特化的 `Coverage::mask` 镂空光栅；对无纹理且恒定全裁剪的透明开口槽位在 `VisBuffer`/`VSM` 剔除与 `BLAS` 几何构建期直接置空，球面/聚光灯按发光体半径裁剪 `near_z` 与射线终点弦长以消除灯芯内胆自遮挡）：主方向光采用 4 级（`16m / 48m / 128m / 384m`）环形滚动正交 Clipmap，局部投影点/球光与矩形光分配透视虚拟页；纯静态页按 128 位静态签名跨帧零重绘复用，动态实例仅触发 `16 KiB` `CmdCopyTexture` Copy-on-Write 并叠加动态 Meshlet。屏幕端 `LitDirectShadowClassify` 对非双面薄叶表面的几何背光地平线像素直接裁定为自遮挡本影（`0`），并以中心遮挡锥导引的正交/对角 12-tap 步长自适应平面斜率与半影锥可达性 VSM 区间测试（局部球面/聚光灯与太阳光按外圈采样点遮挡深度能否触及光源立体锥过滤假阳性斜率截断，双面透射薄片 `SLAB_PROFILE_FOLIAGE` 保持无偏单层自深度明区/实心本影提前裁定，矩形面光保留宽距环绕半影判定）在 $O(1)$ 内提前裁定明区与多层建筑实心本影像素（零 `RayQuery`），仅将真实半影过渡带、近距薄片自叠褶皱与近场面光视差区经不依赖 `Wave*` 的 `64-bit` LDS 位掩码保序紧凑入 `PenumbraQueue`；逐像素按实际几何受光辐照度选取主导投影局部面光/球光进入半影降噪通道（避免隔墙远灯抢占通道），由 `LitDirectShadowTrace`（`CmdDispatchIndirect`）执行蓝噪分层 `DXR 1.1 RayQuery`（`TAA` 下主导局部面光 2-spp / 太阳 1-spp 时域累积，`Off / SMAA` 下主导局部面光采用 8-spp 确定性空间分层采样以保持物理软半影一致且跨帧逐字节确定），再经带几何受光半球一致性守卫的 `LitDirectShadowTemporal` 与步长 `1, 2, 4` 的三趟三维切平面/几何法线边缘保持 À-Trous 双边滤波（`LitDirectShadowFilter1/2/4`）生成平滑无晶格噪点且不向室内倒角渗漏的半影可见度；大规模场景（`>256` 实例）下纯方向光环境的 `0.25m` 以下微碎草豁免 VSM 交由接触阴影与 `GT-VBAO` 处理，`<16m` 纯不透明多级网格在 `25m–80m`（`24m..26m` 双锚点迟滞）切换至 `LOD2` 代理 BLAS（`Coverage::mask` 镂空植被保持 `LOD0` 非不透明 BLAS 以避免树冠阴影过暗），`80m` 以外（`78m..82m` 迟滞）`<16m` 实例移出 TLAS 并平滑交接 VSM 远场 PCF。
   - **中观厘米级（`1 cm – 0.4 m`）屏幕空间接触阴影（`ContactShadow CS`，`R8_UNORM`，锚定 `Sony Bend Studio bend_sss_gpu.h`）**：仅在存在投影主方向光且 `contact_shadow_length > 0` 时分配与派发，沿屏幕短轴（Minor-Axis）执行边缘保持次像素线性 `ViewZ` 插值与 24 步 IGN 连续相位步进，采用三段式遮蔽估计器（前 4 步硬接触、中段 4 组子相位取平均、末 6 步距离衰减 + `k_contrast = 3.0`）与有限厚度双线性包络拒绝，单/双网格解析验收场景自动跳过。
   - **微观毫米级（`0.1 mm – 1 cm`）材质孔径微阴影（Chan 2018 Aperture `Micro-Shadows`）**：在 `DirectLight CS` 内联纯确定性地按材质烘培 `Slab0.micro_ao` 软裁剪 `N·L`（与随机屏幕空间 `SSILVB` 严格解耦，杜绝斜射日光墙面时间沸腾），无 `NaN` 边界守卫还原法线贴图孔隙自阴影。
3. **OpenPBR 确定性直接光求值与次表面散射（`LitDirectBrdf` + `LitDirectSubsurfaceX/Y`）**：将漫反射与高光分离输出（使漫反射直接供给无高光污染的 `PrevDiffuseEmissiveRadiance` 与按需间接派发的 `Subsurface` Jiménez 可分离正交/透视双边滤波）：点光/球光与角半径太阳光采用统一球冠最近代表方向、基础层/清漆层独立 Karis 能量守恒归一化与基于 `ltc_lut.hpp` 方向反照率的 **Turquin 2019 微表面 GGX 多次散射能量补偿**（保持白炉单调有界 $\le 1$），矩形面光采用零动态数组的流式无 `NaN` Sutherland-Hodgman 地平线裁切积分与本工程 BRDF 专用拟合的 `64×64` LTC 矩阵/幅值表（同步覆盖清漆、薄膜干涉、天鹅绒与薄叶片闭包），并在法线贴图扰动处施加平滑几何地平线衰减防止背光渗漏。

## 三层全局光照与 `SHaRC + GI-1.2` 多跳表面辐射度缓存

三层 GI 体系覆盖近场折痕遮蔽、一阶屏幕探针收集与世界空间多跳表面辐射度缓存，全管线照明缓冲统一在光度预曝光（Pre-Exposure，`fcb.exposure_scale`）空间下累积并按帧间曝光比 `fcb.exposure_ratio` 无缝重定标历史缓冲：

- **第 0 层：全分辨率时域亚探针 `ONB-GT-VBAO`（`LitSSILVB` + `LitSSILVBFilter`，锚定 `Therrien et al. 2023` + `Intel XeGTAO` + `UE5 Lumen ShortRangeAO`）**：融合 `XeGTAO` 精确球面余弦积分度量与 32 扇区（`[-π/2, +π/2]`）有限厚度可见性位掩码拓扑：绕视线极轴 $\mathbf{V}_v$ 构建正交基（消除离轴透视方位角畸变），在原生全分辨率下执行二次接触加密步进（热历史稳态 `1 slice × 4 steps/side = 8 taps/px`，冷启动首帧 `2 slices × 4 steps/side = 16 taps/px`；近距读取全分辨率 `D32_SFLOAT` 深度、远步切换 `CurrHZB` Mip 级联）与距离成比例有限厚度判定；开启屏幕探针 GI 时自动将作用半径收敛于亚探针残差窗口（`min(ao_radius, ~4.5 × ProbeSpacing)`，与探针 `SH2` 可见度分母保持同尺度对齐以杜绝重复压暗）；前/后表面仰角先在 $\mathbf{V}_v$ 极轴切片半平面内经无分支切割的 `GTAOFastAcos` 求值，再于实数轴平移减去法线倾角并以中点无偏量化映射至 32 位扇区（根除掠射角背面 `atan2` 模回绕黑斑），以零反三角函数的精确球面雅可比权重 $|\sin(n + \alpha_b)|\cos\alpha_b$ 自归一化求值可见性与世界空间弯曲法线锥（白炉恒等为 `1.0`），并在近场残差窗内采样 `PrevDiffuseEmissiveRadiance`；随后经全分辨率 `3×3` 三维切平面边缘保持滤波与 `1:1` 像素对齐的切平面子像素时间累积（`ssilvb_ao_history` / `ssilvb_gi_history`，上限 12 帧），无需半分辨率上采样折损即可保留 `1–2 px` 建筑石缝细节；关闭 `ssilvb` 时 RDG 直接跳过两道计算派发与瞬态纹理分配。
- **第 1 层：一阶主表面收集（`AMD Capsaicin v1.3 GI-1.2 ScreenProbes`：`LitGiPrimaryTrace` + `LitGiProbeBlend / MaskMip / FilterX / FilterY / Project`）**：每 `8×8` 像素瓦片维护持久等面积半球八面体（Clarberg Hemi-Octahedron，`8×8` 单元，`RGBA16_SFLOAT` 存辐射度与命中距离 `<= 500 m`）图集 `probe_atlas` 与多级种子掩码 `probe_mask`（`R32_UINT` 存精确种子像素坐标）；每帧对全量探针执行基于前帧解抖动真实深度与前帧几何法线的三维视差重投影（`ReprojectScreenProbes`，带反向方向查表填补离散针孔），稳态探针按防御性 `75% (Radiance × Cosine) + 25% Uniform` 并行前缀和 CDF 执行重要性引导采样（保留未采样单元的重投影估计以消除 `RadianceBackup` 能量爆炸），冷启动/不遮挡覆盖（`override_tiles`）探针执行均匀分层填充；射线先沿精确透视插值 `CurrHZB` 步进查询 `PrevDiffuseEmissiveRadiance`，屏外射线经 `64-bit` LDS 保序紧凑发射 `RAY_FLAG_FORCE_OPAQUE` DXR 射线并按原型分桶着色；在异步计算 `LitGiWrcResolve` 交汇后，`LitGiProbeBlend` 从已收敛 `SHaRC Slot[0]`（近角点 `< 1.25 × VoxelSize` 自动 `bypass_cache` 防漏光）解析单元辐射度并做防萤火虫时间融合，随后经 `7×1 + 1×7` 命中点视差/夹角回退可分离共面滤波（`FilterProbes`）与保持离散 $8\times 8$ 白炉精确恒等（$\hat{E}(\mathbf{N})/\pi \equiv 1$）的正交 `SH2` 投影生成平滑探针场。
- **第 2 层：二次命中着色、`SHaRC + GI-1.2` 多跳表面缓存与时空降噪（`LitGiHitShade` + 异步计算 `LitGiWrcResolve` + `LitGiGather` + `LitGiDenoiseX/Y`）**：按原型 `CmdDispatchIndirect` 执行剪除细节法线贴图后的 `ArchGi::ms_shade_gi_hit`（解析灯 NEE 阴影查询 + `manifold_closed` 闭合流形背面防漏光）；基于 `NVIDIA SHaRC v1.8.3`（`HashGridCommon.h` / `SharcCommon.h`）与 `AMD Capsaicin v1.3 GI-1.2` 维护 `262,144` 槽位（`16,384` 个 64B 缓存行对齐的 16 槽线性探测桶 + 32 位 `xxHash32` 无锁 `InterlockedCompareExchange` 校验和 + 完整 64 位 `(x:17, y:17, z:17, level:7, oct_normal:6)` 对数多级空间键）世界空间表面缓存，双通道分离存储**总出射漫射辐射度 $L_o(\mathbf{x})$**（供一阶探针命中解析、二次反弹与屏外反射查询）与**解调多跳间接辐照度 $E_{\text{ind}}(\mathbf{x})/\pi$**（由 `1/4` 分层余弦次级射线 $\mathbf{x}_1 \to \mathbf{x}_2$ 无偏传播）；异步计算 `LitGiWrcResolve` 与图形队列直接光零资源冲突并行完成防萤火虫、32 帧滚动窗口消退加速、相机移动跨级父子体素预热与 64 帧陈旧槽位回收；`LitGiGather` 以 `±6 px` 共面切平面蓝噪抖动与分层 `FindClosestProbe` 插值 `SH2` 探针，结合三维切平面距离/前帧几何法线子像素重投影与有符号亮度变化率 `gi_color_delta` 自适应帧数（`4..32` 帧）时间累积，最后由 `LitGiDenoiseX/Y` 对解调漫射辐照度执行着色法线与三维切平面边缘保持可分离滤波，耦合全分辨率弯曲法线锥与逐像素着色法线方向对齐度及切平面微裂缝可见度后传入 `LitDirectCompose`，按 **Jiménez 2016 `GTAOMultiBounce`** 多重弹射反照率保持多项式与弯曲锥高光遮蔽融合输出。

- **多上下文 4 路 LRU 历史隔离**：`hzb_pyramid`、`prev_diffuse_emissive`、`ssilvb_ao_history` / `ssilvb_gi_history`、`probe_atlas` / `probe_mask`、`geom_normal_history`、`gi_history` / `gi_color_delta`、`sharc_slots` / `sharc_hash_keys`、`refl_history` / `refl_moments_history` 与 `taa_history` 按 `frame.scene_identity` 归属独立的 4 路温驻留 `LitContextState` 槽位，使编辑器在主世界视口与 `.procgraph` SOP 节点预览上下文间往返切换时不冲毁主场景 GI、反射与 HZB 收敛历史。

## 三级反射、大气雾效、时间超分抗锯齿与后处理链

1. **三级粗糙度分流清漆感知反射与 `GI-1.2` 时空方差降噪（`LitReflectionsTrace` + `LitReflectionsFilter*`）**：清漆材质读取 `Slab1` 真实清漆粗糙度与曲面安全清漆法线（在平滑网格上退化为顶点平滑法线而非棱面法线）；`r <= 0.12` 走精确镜面追踪，`0.12 < r < rough_cut`（默认受 `reflection_roughness` 控制，上限 `0.80`）走有界 VNDF（Dupuy & Benyoub 2023）采样，先执行带近平面裁剪与透视精确射线命中距离恢复的多级 `CurrHZB` 屏幕步进，屏外射线经 LDS 位掩码紧凑后发射 `RAY_FLAG_FORCE_OPAQUE` DXR 射线并查询当帧已解析的 `SHaRC` 表面总出射辐射度缓存（未预热体素回退低频天空漫射基底以消除纯黑斑块）；锚定 `AMD Capsaicin v1.3 GI-1.2` 在空间滤波前执行**虚拟反射命中点视差（$\mathbf{P}_{\text{virt}} = \mathbf{E} + \mathbf{I}(\|\mathbf{P}-\mathbf{E}\| + d_{\text{hit}})$，带前帧反射面共面校验）与表面运动矢量双源时间重投影**及保色相 Luma-Karis 二阶矩方差追踪（`refl_moments_history`），随后执行步长 `1, 2, 4` 的**接触硬化反射锥足迹（$d_{\text{hit}}\alpha/\text{px\_ws}$）+ 时间方差引导保色相 Luma-Karis 准算术均值**三步边缘保持 A-Trous 滤波；`r >= rough_cut` 或关闭 `reflections` 时零额外射线沿 Frostbite GGX 主导反射瓣方向带视差插值采样已滤波的 `8×8 ScreenProbes` 半球八面体辐射度图集（`[rough_cut - 0.12, rough_cut]` 区间与追踪反射平滑交接；无 GI 时回退弯曲锥预滤波天空），保留粗糙石材与织物的窗户/天光方向性高光层次。
2. **三态天空、体积雾、视口裁剪派发与 HDR 合成（`LitFroxel*` + `LitDirectCompose`）**：内置 Hillaire 2020 预计算透射率与多重散射 LUT 物理大气求值器及按需启用的 `160×90×64` 近场视锥体积雾（`fog_enabled && fog_density > 0` 时激活）；在编辑器子视口模式下，全分辨率阴影分类/滤波、直接光求值、`ONB-GT-VBAO`、GI 收集/降噪、反射追踪/滤波及屏幕探针组均按 `scissor_rect`（含 1 瓦片双边安全边界）裁剪线程组或组级提前退出，避免视口外 UI 区域的无效着色计算；`LitDirectCompose` 融合直接漫反射/高光、**Turquin 2019 微表面 GGX 多次散射能量补偿**、区分精确光追反射与探针回退反射的 **Lagarde 2014 高光 AO**（不重复压暗光追反射且不截断掠射菲涅尔微光）与 OpenPBR 清漆多次内反射暗化（Coat Darkening）调制的间接光照及瓦片门控自发光，同时全屏写出下一帧 FP16 安全无高光 `PrevDiffuseEmissiveRadiance` 与解抖动深度历史。
3. **抗锯齿与色调映射（`LitTemporalResolve` → `LitBloom*` → `LitTonemap` → `LitSmaa*` → `LitPresentBlit`）**：
   - 支持 `Off`、`SMAA`（基于固定 `AreaTex` / `SearchTex` 正交与对角模式搜索及转角保留权重，单帧零抖动逐字节确定）与默认的混合高清晰度 `Atelier TAAU`（`Antialiasing::taa`：8 相位 Halton 抖动 + 材质纹理 `-0.65` 负 Mip 偏置 + 原生分辨率零邻域预模糊的 **Blackman-Harris 置信度子像素时域积分**（超分态启用子像素核重建） + **5-tap 防振铃 Catmull-Rom 双三次历史重采样** + 仅在真实像素运动下激活的同像素 `2×2` 前帧深度遮挡检测 + 速度自适应稳态织物细节包络保护 + 末端内联无振铃 **AMD FidelityFX `RCAS`** 微对比度恢复；直接光半影、GI 与反射历史重投影统一按 `pxc - jitter_pixels` 对齐去抖深度）。
   - Bloom 采用 RDG 逐 Mip 子资源屏障追踪的两张多级纹理（`bloom_down_tex` + `bloom_up_tex`），首层执行二次软膝阈值（Soft-Knee）+ 逐 4 纹素盒 Karis 防萤火虫 13-tap 下采样，再以单位增益归一化帐篷插值上采样，避免全屏灰雾抬升。
   - 色调映射默认采用 **AgX**（`16.5` 档对数动态范围，保留暗部多跳间接光层次且高光自然去饱和，可选切换 **Khronos PBR Neutral** 资产检视曲线），输出经静态屏幕空间 `0.99`-LSB IGN 抖动量化后编码至 `RGBA8_SRGB` 后台缓冲，消除 8 位渐变条带且静止帧逐字节确定。
   - 存在参考网格、选中边或显式 `Capture` 且启用时间抖动时，`LitUnjitterDepth` 在 `3×3` `VisBuffer` 邻域内对去重后的可见三角形做带 `NaN`/近平面守卫且禁止 `Coverage::mask` 镂空膨胀 1px 的精确射线—三角形齐次求交，零重绘恢复公开无抖动 `D32_SFLOAT` 深度。
   - **GPU `VisBuffer` 拾取（`Renderer::pick_pixel` / `Renderer::pick_box`）**：`VisUnpackAndClassify`（及纯线框模式）将每个屏幕像素的 `(instance_index + 1, global_triangle_id)` 写出至持久 `RG32_UINT` 拾取缓冲，支持 $O(1)$ 单像素精确实例/图元/深度拾取与矩形框选实例集合读回。

## 诊断与性能边界

`FrameStats` 通过 4 槽非阻塞 `ReadbackSlotRing` 与 RDG 校准硬件时间戳共同发布上一已完成帧的时间轴并集耗时 `scene_lit_gpu_union_ms`、8 个顶层 GPU 阶段（`GBuffer`、`AS`、`direct`、`GI`、`SSILVB`、`Reflections`、`temporal`、`composite`）、子阶段细分、VSM 页池缓存统计、15 位 `scene_lit_gpu_pass_mask`（稳态帧置齐 `0x5fffu`）、分簇计数及硬件光追阴影/GI 主射线/反射光线计数；`scene_lit_resource_bytes` 统计当前逻辑 GPU 资源与加速结构字节数。

## 后续按需扩展方向

以下能力不预埋空壳占位，待实际场景需求驱动时独立立项：

- **延迟 Slab 贴花**：`World` / `Presentation` 贴花组件与 `2.5D Clustered DecalModifySlab CS`。
- **前向半透明与单层水面**：`LitDirectCompose` 之后的 `SingleLayerWater` / 前向半透明通道及 `Atelier TAAU` `ReactiveMask`。
- **外部超分 SDK 接入**：基于 `NRIUpscaler` 的 `FSR / DLSS / XeSS` 可选路径（内置超分抗锯齿由 `Atelier TAAU` 承担）。
