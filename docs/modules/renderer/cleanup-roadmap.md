# 渲染重做：单轨契约收口状态与远期扩展边界

状态：**§1 与 §2 已在 Batch 7 全链路收口落地；§3 记录按需推进的独立渲染特性扩展项**。

本文件记录 `atelier_rhi` / `atelier_rdg` / `atelier_renderer` 重做中已完成的单轨契约清扫与跨模块双轨收敛结果，以及明确延后的独立特性边界。当前权威契约见 [Renderer 合同](contract.md)、[Scene Lit](scene-lit.md) 与 [材质系统](materials.md)。

---

## 一、已完成（Batch 7）— 单轨光照/GI/后处理契约收口

1. **作者态组件与 `.scene` 持久化契约**：`SceneLightingSettings` 与 `world.EnvironmentComponent` 已移除全部旧探针体积、旧阶段开关与 `smaa_t2x`，统一为三层 GI（`indirect_lighting`、`gi_quality`、`indirect_gain`、`ssilvb`、`ao_radius`，剪除冗余第 3 层 `ZH3` 体积剪辑图并将表面辐射度缓存升级为 `SHaRC + GI-1.2` 多跳缓存）、三级反射（`reflections`、`reflection_distance`、`reflection_roughness`）、三级阴影（`shadows`、`contact_shadow_length`：方向光 4 级正交 Clipmap VSM + 半影光追、聚光灯单视图透视软件分页 VSM + 半影光追、点光与矩形面光 DXR 1.1 inline RayQuery）、三态天空（`none` / `physical_atmosphere` / `hdri_sky_map`）、体积雾、光度预曝光与色调映射（`pbr_neutral` / `agx`）及抗锯齿（`off` / `smaa` / `taa`）；`LightKind::spot` 与 `examples/` 下全部 `.scene` 已同步迁移。
2. **编辑器 UI、视口控制与性能观测**：`WorldPanel` 已更新为 `Lighting | GI & Refl | Post | Diagnostics | Performance` 五页签；已移除全部无效旧探针诊断项与拟合操作符；已新增无网格实体定位符显隐开关 `show_locators`；`Performance` 与 `EditorControl`（`view_schema = 8`）统一报告真实 RDG Pass 名称、VSM 页池计数及 `rdg::FrameGpuProfile::union_gpu_ms` 时间轴并集耗时；`render.trace` 支持 `"full"` 与 `"scene_lit_post"` 范围。
3. **渲染器统计与端到端真值门禁**：已删除全部伪统计与旧调试分支，`acceptance/renderer` 已补齐聚光灯 VSM、物理大气天空、AgX / PBR Neutral 调色、解耦镂空阴影及 GPU `pick_pixel` / `pick_box` 的独立真值验收。

---

## 二、已完成（Batch 7）— 跨模块双轨消除与宿主/流送收敛

1. **纯 UI 表面通道与宿主自适应休眠**：`PopupHost` 启用 `Options::ui_only` 并通过进程级弱引用 `SharedUiTextureHub` 共享 `rhi::Device` 上的 UI 贴图池；`--preview-ui` 跳过 3D 场景准备；编辑器在窗口最小化、Play 后台失焦与 3D 视口隐藏时释放 RDG 瞬态池并降载（`L1–L3`）；进入/退出 SOP 上下文自动保全与恢复世界相机，并在 `build_project` / `run_project` 前同步保存脏工程 `.procgraph`。
2. **增量场景同步与多上下文历史隔离**：`presentation::Scene::update_meshes` 在可渲染能力桶成员不变时保持槽位与材质缓存增量更新（仅标记被重挂父链的槽位）；`SceneSnapshot` 在纯选区高亮变化时零拷贝复用 `ViewportScene`；`RenderScene` 将实例与材质槽表常驻 `DEVICE` 堆并按脏区间增量上传（静止帧零字节重传）；`Renderer` 维护按 `scene_identity` 索引的 4 路温驻留 `LitContextState`，隔离主场景与 SOP 预览的 GI / HZB / TAA 历史。
3. **GPU `VisBuffer` 拾取与材质法线强类型切片**：`ViewportTool::trace` 与 `pick_box` 在宿主视口下直接消费 GPU `RG32_UINT` 拾取缓冲与硬件深度；材质语言引入强类型 `material.NormalTextureInput` 与 `sample_normal` 内建函数；光追镂空阴影解耦为独立 `MaskedCoverageSet`。
4. **压缩纹理 `.atx` v4、冷像素回收与 RDG 逐 Mip 屏障**：`.atx` v4 支持 `BC1_RGBA_SRGB`、`BC5_RG_UNORM`、`BC7_RGBA(_SRGB)` 与 `BC6H_UF16`（含 `SkyMap` GGX 预滤波 Mip 链、旋转 `SH3` 与重要性 CDF）；`Image::evict_cold_pixels()` 在上传后释放冷未压缩像素；`PlayLink` 改为传递无损 `.png` 而非裸 `.rgba`；`UploadBatch` 与 BLAS 构建采用非阻塞提交与延迟回收；HZB 与双纹理多级 Bloom 全程纳入 RDG 逐 Mip `SubresourceRange` 自动屏障。

---

## 三、待后续按需推进的独立特性扩展（遵循精悍原则不留空壳占位）

1. **大规模程序化森林（`>80m`）已完成基础（Batch 6）**：已落地 `128 MiB` 动静分离软件分页 VSM（双 `4096×4096` `D32_SFLOAT` 物理页池 + Copy-on-Write 动态覆盖页 + `PenumbraQueue` 半影光追）与三区段森林 TLAS 准入（`Zone 0` 微碎草豁免 VSM、`Zone 1` `LOD0 BLAS`、`Zone 2` `25..80m` 代理 BLAS、`>80m` 迟滞移出 TLAS）。
2. **按业务需求独立立项的扩展项**：
   - **延迟 Slab 贴花**：`World` / `Presentation` 贴花组件与 `2.5D Clustered DecalModifySlab CS`（带 `TileEmissiveMask` 脏内存守卫）。
   - **前向半透明与单层水面**：`SceneCompose` 之后的 `SingleLayerWater` / 前向半透明通道及 `Atelier TAAU` `ReactiveMask`。
   - **外部超分 SDK 接入**：基于 `NRIUpscaler` 的 `FSR / DLSS / XeSS` 可选运行路径（当前内置超分抗锯齿由 `Atelier TAAU` 承担）。
