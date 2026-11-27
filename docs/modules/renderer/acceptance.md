# Renderer 验收

本页说明重写后的 Renderer 端到端验收入口各自证明什么、如何运行、当前门禁验证证据以及仍存在的运行边界。行为与参数契约见 [显示合同](contract.md)、[Scene Lit](scene-lit.md) 与 [材质系统](materials.md)。

## 运行

- `renderer_acceptance` 聚合三个真实窗口/GPU 入口：`renderer_acceptance_default`、`renderer_acceptance_scene_lit`、`renderer_acceptance_material_programs`。每个入口独立进程，限时 1800 s，任一失败即整体失败。也可以经正式门禁脚本运行：`pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Targets renderer_acceptance_default,renderer_acceptance_material_programs,renderer_acceptance_scene_lit` 或收尾完整门禁 `-Full`。
- 验证层与跨工作树窗口锁遵循 [README 的构建与端到端验收规则](../../../README.md#构建与端到端验收)：快速门禁设置 `ATELIER_GPU_VALIDATION=debug`（D3D12 Debug Layer + Enhanced Barriers）；`-Full` 对 `rhi_acceptance` 与三个 `renderer_acceptance_*` 目标启用 `gbv`（GPU-Based Validation）。`--scene-lit-benchmark` 关闭验证层，计时采样期间不读回图像。
- **活动输入桌面要求**：四个窗口验收目标（三个 `renderer_acceptance_*` 与 `editor_log_smoke_acceptance`）均通过真实 `IDXGISwapChain4::Present` 验证完整呈现契约（`hr != DXGI_STATUS_OCCLUDED` 才判定 `FrameStatus::accepted`）。必须在连接活动物理显示器的交互式 `Default` 输入桌面中运行；若在隔离的非输入 Win32 沙盒桌面（如代理工具箱 `exebox-*`）或显示器休眠状态下执行，DWM 对后台桌面交换链固定返回 `DXGI_STATUS_OCCLUDED`，渲染器会按契约将帧记为 `FrameStatus::skipped` 并拒绝伪造接受帧。

Renderer 消费者共用可执行文件目录下的 `renderer-shader-cache/` 与同级 `renderer-texture-cache/`（`.atx` 烤制缓存），不污染编辑器的用户缓存目录。

## 默认入口

- **K5**：非对角左手系 row-major `view / projection` 矩阵下，读回 `VisBuffer` 光栅深度与解析公式 `1 / (9.9 × 3.5) − 0.1 / 9.9` 逐位核对（容差 `< 1e-4`）。
- **K4**：
  - `ui-shapes.ppm`：独立非对称圆角、向内边框、带行填充的预乘 RGBA 图像、逐实例裁剪、字形覆盖图与单次 draw 内 `A/B/A` 画家序透明混合。
  - `selection-far-perspective-studio.ppm`：跨 `far = 4` 远裁剪平面的三角形 `SV_ClipDistance0` 精确裁剪与保留部分的橙色选中边线。
  - `viewport-hidden-lines.ppm`：`Wireframe::only` 隐藏线消隐模式下保留背景、完整表面深度 `0.8` 与左边界多边形线框。

## `--scene-lit`

每个特性保存一张代表图，并与独立解析或几何真值比对（不以旧帧或另一渲染后端输出作 golden）：

| 图像 | 独立真值 |
| --- | --- |
| `area-dielectric.png` | CPU 双精度 96×96 矩形面积微面元 BRDF 数值积分（不调用 LTC），经当前默认 **Khronos PBR Neutral** 色调映射与 sRGB 编码后逐像素核对粗糙电介质平面响应。 |
| `shadow-point-settled.png` | 作者接收平面 `z = 4`、遮挡盒 AABB 与点光源的精确线段—包围盒相交几何；轮廓外几何遮挡区像素严控 `<= 2`，非遮挡亮区显著受光。 |
| `sky-marker-yaw-35.png` | 2048×1024 等距柱状天空标记纹素经 `35°` 顺时针 yaw 旋转后的双精度投影质心，实测图像亮度质心误差 `<= 1 px`；并附带两阶段 HZB 第二阶段遮挡解除（`hzb-phase2-disocclusion.png`）深度与像素恢复验证。 |
| `spot-vsm-cone.png` / `spot-vsm-umbra.png` | 聚光灯内外锥角平滑衰减、锥外截止、单视图透视软件分页 VSM 静态页缓存复用与几何遮挡体本影遮挡。 |
| `physical-atmosphere-sky.png` / `tonemap-agx.png` | Hillaire 物理大气瑞利蓝移天空辐射、**AgX** 与 **Khronos PBR Neutral** 高光色度压缩对比，以及 GPU `pick_pixel` / `pick_box` 精确可见实例与硬件深度读回验证。 |

## `--material-programs`

- **K4**：五张原生批次代表图——`native-smooth`（作者红 Mask 前表面 `0.8` 深度与绿 Opaque 后表面 `0.4` 深度）、`native-default-smooth`（引擎默认不透明品红）、`native-final-uv-mips`（256×256 红蓝棋盘在高频 UV 下经 `.atx` sRGB 线性均值 mip 过滤为均匀紫色均值且保持 `0.8` 深度）、`native-gi-masked`（无灯无天光房间内，视锥外负 coverage 发光面经 `Screen Probes` `RAY_FLAG_FORCE_OPAQUE` 遍历与 `hlsl_gi_hit` 照亮地面接收区）与 `native-masked-ray-shadow`（解耦 `MaskedCoverageSet` AnyHit 着色器在正 coverage 半区投下本影、负 coverage 镂空半区透光）。
- **K6**：验证已准备但未采用的 `MaterialBatch` 纹理保活、批次切换、最后一次使用不同步等待的安全延迟退休、空场景切换、管线集合缓存命中及正常 `close`。

## `--scene-lit-benchmark`

独立测量入口（不纳入 `-Full` 门禁通过条件）：原生 `2560×1440` 固定房间场景，关闭验证层，预热后采集异步完成的 RDG 硬件时间戳样本与射线计数，校验父子区间包含关系、`scene_lit_gpu_pass_mask`（`0x5fffu`）及 `kloppenheim_05_4k.hdr` 天空贴图上传与稳态开销。

## 当前验证状态与历史基线说明

- **2026-11-26 重写分支当前已验证证据**：
  - `dev` 与 `release` 全量 558 个构建目标在 `/W4 /WX /EHs-c-`（`_HAS_EXCEPTIONS=0`）下零错误、零警告编译通过；变更的渲染、材质、编译器与编辑器宿主 C++ 翻译单元经 `atelier_style` 静态检查零新增违规。
  - 全部 15 个无窗口端到端验收目标在 `dev` 与 `release` 下 `15 / 15 PASS`。
  - 真实硬件 RX 6800 XT 上 `dev:rhi_acceptance` 与 `release:rhi_acceptance` 通过，且四个窗口目标在 D3D12 Debug Layer + Enhanced Barriers 下完成首帧全管线编译、加速结构构建、RDG 执行与读回录制，零 GPU 页故障、零验证层警告/错误（沙盒会话处于非输入桌面 `exebox-*`，最终桌面交换链呈现验收需在宿主 `Default` 桌面执行 `-Full` 确认）。
- **性能测量边界**：本轮重写尚未在交互式桌面重跑 `atelier_editor --profile-render-performance`（Sponza / Dungeon）与 `--scene-lit-benchmark` 实测采样；设计文档中的阶段毫秒预算属于架构目标推断，不替代实机测量结果，历史 `gfx` 基线数据请查阅 Git 历史记录。

## 未覆盖

- 非 RX 6800 XT 硬件（NVIDIA / Intel）及不同驱动版本的实机运行矩阵。
- 透明混合/折射、骨骼蒙皮 BLAS 动态变形、流式虚拟纹理与 Vulkan 后端尚未实现，无对应验收。
