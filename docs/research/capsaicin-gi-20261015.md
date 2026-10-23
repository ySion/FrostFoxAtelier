# Capsaicin GI 源码核对

2026-10-15 的研究快照，供[GI 比较条件](gi-comparison-contract-20261016.md)参考。Capsaicin 源码固定在 `914b91596cd119eda85fbc1d3c7ee6ac391b1452`；本次只读源码和论文，没有运行 Capsaicin。

## Capsaicin 实际提供什么

官方[README](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/README.md#L20-L32)明确将 GI-1.2 定位于间接光，需要搭配已有直接光技术；经 probes 处理的直接贡献限于 emissive mesh、sky 等子集。这不是完整的解析点光／太阳 DI 替代品。

源码核对：

1. [GI renderer pass list](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/renderers/gi1/gi1_renderer.cpp#L60-L70)没有常规解析点光 DI 或 VSM pass。`DirectLighting` 目标的名字不等于已实现完整点光直接光。
2. 默认依赖 `LightSamplerGridStream`：按空间 cell 构造压缩的光源 proposal，二次命中点再做候选 RIS；不是保留所有影响灯的精确 cluster 列表。[配置](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/components/light_sampler_grid_stream/light_sampler_grid_stream.h#L58-L69)、[实际采样](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.comp#L2500-L2525)。
3. 世界空间 reservoir 重采样是可选功能，`gi1_use_resampling=false` 为默认值。RIS 使用 reservoir 数据结构，不代表默认执行完整的屏幕 ReSTIR DI 时间／空间链。[默认配置](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.h#L49-L84)。
4. 对存活样本压缩工作列表后，仍执行 RT shadow rays；GI 与 glossy reflections 仍需滤波和重建。[shadow 工作](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.comp#L2022-L2087)。该固定版本没有可直接移植的 VSM 系统。
5. 二次 hit 的材料被设置 metallic=0、roughness=1 来减少噪声。这改变了传输近似，不能称为保留原有材质语义的免费优化。[源码](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.comp#L2505-L2511)。

最值得借鉴的是：将预算投向可见接收表面；用历史 radiance 引导 primary 方向；复用 secondary hit radiance；按活动工作压缩 dispatch。它并不是通过完全排斥 stochastic sampling 获得效果。

## 论文性能口径

| 版本／原始来源 | 条件 | 文献报告值与边界 |
|---|---|---|
| [GI-1.0 表1](https://gpuopen.com/download/publications/GPUOpen2022_GI1_0.pdf) | RX 6900 XT、1920×1080、¼ spp | 所列四场景 GI 约1.932–3.124 ms；不是95点光的完整 DI+GI，更不是整帧。cache/sampling 约1.032–1.693 ms，不能只算射线成本 |
| [GI-1.1 表1](https://gpuopen.com/download/publications/SA2023_RealTimeReflection.pdf) | RX 7900 XTX、1080p、反射半分辨率 | 所列四场景 GI 加反射约4.11–5.08 ms；不同硬件、不同任务，不可用作6800XT原生1440p承诺 |
| GI-1.2 固定源码 | 本轮仅阅读 | 没有本机测量；不能将旧论文数字标成这个commit的性能 |

GI-1.2 默认两个 radiance value buffer 按[源码配置与分配](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin/blob/914b91596cd119eda85fbc1d3c7ee6ac391b1452/src/core/src/render_techniques/gi1/gi1.cpp#L440-L507)计算已约340 MiB，尚不含其它资源。这是逻辑分配推算，不是运行时显存驻留测量。不能把双层缓存看成低成本附加开关。
