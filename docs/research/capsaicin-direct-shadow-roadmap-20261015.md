# Capsaicin、直接光与阴影路线评估

日期：2026-10-15。状态：研究与建议，尚未实施替换。目标硬件 RX 6800 XT；目标为原生 2560×1440、90 FPS，即整帧约 11.11 ms。此次没有运行新的 GPU 测试，也没有修改引擎、用户工程或渲染参数。Capsaicin 源码固定在 `914b91596cd119eda85fbc1d3c7ee6ac391b1452`。

用户已同意方向；实施批次、协作边界与验收门槛见[执行计划](direct-lighting-execution-plan-20261015.md)。本页保留研究证据，不将计划中的能力视为当前实现。

## 建议

**将常规分簇直接光照作为下一条默认主线，采用缓存阴影，保留有明确用途和预算的硬件光追；暂不直接建设完整 Virtual Shadow Map 系统。** 当前全屏 ReSTIR DI 的成本与本地地牢的灯光分布不匹配，不宜继续把局部 shader 优化当作达到 90 FPS 的主要办法。

这不是证明 ReSTIR 在所有负载中都慢。需要分开三个决策：

- 直接光积分：确定性累加有限局部灯，还是随机抽样大量光源。
- 阴影表示：RT 几何可见性、常规 shadow atlas、虚拟分页 shadow maps。
- 信号重建：何种结果确实含有随机噪声，需要何种滤波。

VSM 可以配合常规光照，也可以配合随机光源采样。更换阴影表示并不会自动删除 ReSTIR 的 reservoir 和 radiance denoiser。下一阶段应先消除大面积不必要的随机求解，再决定阴影图需要何种虚拟化。

## 我们已有的成本证据

证据：`out/di-reconstruction-20261015/final-repeat-summary.json`，360 个完成帧；正式 Release、GPU validation 关闭。该组实际 viewport 为约 2265×1246（2.82 Mpx），client 为 3840×2054。**它不是原生 1440p 测量。** 原生 1440p 为 3.6864 Mpx，像素数约多 31%，不能将下表直接换算成目标帧率。

| 同一组测量 | 平均 GPU ms |
|---|---:|
| DI 初始采样／时间复用 | 3.050 |
| DI 空间复用 | 1.837 |
| DI 可见性／最终着色 | 1.993 |
| DI 时域重建与三轮空间滤波 | 4.893 |
| DI 父项 | 11.774 |
| DDGI | 4.800 |
| AO | 2.472 |
| HDR／SSSR | 3.049 |
| 其余主项（几何、上传、HDR resolve、display） | 5.161 |
| 八个主 scope 合计 | 27.256 |

父子计时没有重复相加。Visibility/shading 包含的不止射线遍历；不能把其数值全部叫作 RT 耗时。八主 scope 合计也不是包含 CPU、UI、Present 的完整帧时间。该组 DI P95 为 17.314 ms；场景仍有与几何 pass 同时变化的快慢模式，之前的 ADLX 采样没有证明其根因。

关键判断：约 4.89 ms 的重采样与约 4.89 ms 的重建占据 DI 大部分成本。仅优化阴影射线无法解决全部问题；但确定性累加后如果给所有灯逐像素发射多条 RT 阴影，也可能把省下的成本花回去。

同组主项减去 DI 仍为 15.483 ms。即使将 DI 假设成零，剩余成本也超过 90 FPS 预算。因此需要同时调整 GI、AO、反射与全屏处理；不能承诺只替换 DI 就达到目标。

## 保存地牢的灯光分布

读取用户已保存的 `OfficialExamples/Examples/Dungeons/Scenes/Dungeon.scene`，未修改。SHA-256：`68657ad2dce3c039b4e5cb2d2552ba6a59f4f62c66cd8cf340cd8b16c9c36b38`。

- 启用 95 个 point 和 1 个 directional；另有 95 个禁用 point，不能将文件中的 191 个组件误当作活动光源数。
- 活动点光均开启阴影，source radius 约 0.05 m，变换 scale 均为 1。
- 点光 range 分布：5 m ×44、7 m ×18、8 m ×1、10 m ×14、15 m ×6、20 m ×9、30 m ×2、50 m ×1。
- 在 3381 个启用网格实体原点上，计算落入多少个活动点光的 range 球域：P50=1、P95=3、最大6；其中2791个被至少一灯覆盖的样本，P50=1、P95=4、最大6。

这是 CPU 的**实体原点几何检查**，不是实际表面、屏幕像素、法线剔除、阴影可见性或 GPU cluster 统计。它只支持“灯光较分散，保守空间剔除值得优先验证”的判断，不预测新路径耗时。当前点光衰减有明确 range 截止；精确剔除必须继续与这个真实支持域一致。体积大、原点偏置的 mesh 尤其不能按原点代替表面覆盖。

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

## 为什么不直接把 VSM 当作第一步

VSM 的收益是按需提供高分辨率阴影与页级复用。代价包含页需求、分配与回收、投影者分配、失效、深度绘制和阴影过滤。对点光仍需覆盖多个方向，不能将“虚拟化”理解为取消几何工作。

Epic 的[官方文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)指出其实现针对 Nanite 优化，非 Nanite 几何的多灯提交更昂贵；移动灯与投影者会作废缓存；SMRT 软阴影仍有采样噪声。这是对 Epic 实现及其依赖的证据，**不是说所有自研 VSM 都必须有 Nanite**。

尤其值得参考的是 [Fortnite 工程记录](https://www.unrealengine.com/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4)：太阳持续移动使缓存策略失效，他们改为让不缓存的太阳阴影本身满足预算；小型远处局部灯反而简化成近似单页的普通缓存路径。该记录说明完整虚拟化不一定对每盏小灯都划算，不能把它的总体效果归功于一个算法名称。

同一记录也有反证：当时的 clustered shading 在 Fortnite 中产生性能回退，后来将 one-pass shadow projection 与它解耦。不能以“工业常用”证明分簇一定胜过分块或普通 light-volume 绘制。本项目应先用轻量保守列表验证收益，不把复杂 cluster 构建本身当作目标；现有实体原点统计不足以作最终选择。

我们尚无已确认的 GPU-driven 分页 caster 管线。直接引入整套 VSM 会让页系统与现有提交成本同时成为新问题。地牢中的固定小灯、固定墙体适合先验证普通缓存 atlas；若后续实测表明整灯缓存的分辨率／重绘浪费主导成本，再增加页级虚拟化。

## 建议的统一执行模型

```text
World / Material / Geometry
          ↓
       LightFrame  ← 同一身份、强度、范围、阴影与生命周期
       ├─ 可见表面：保守 cluster 灯列表 → 直接光积分
       │                               ↑
       │                 缓存 shadow atlas / 方向阴影
       │                 以及明确预算的必要 RT visibility
       └─ GI 二次命中：共享空间 proposal → RIS / RT visibility
                                             ↓
                                       DDGI / radiance cache
```

这是不同求解域共享权威数据，不是两套灯光世界。当前 top32 空间 proposal 有全局兜底，是随机选灯机制；不能直接拿它截断确定性 DI 的灯列表。

建议各光型：

| 对象 | 下一阶段建议 | 必须保留的边界 |
|---|---|---|
| 太阳 | 独立确定性着色；稳定方向阴影／CSM作为首个缓存后端候选 | 窗缝分辨率、级联过渡、移动太阳时的失效成本；保持D32深度要求 |
| 点光 | 分簇剔除后累加真实局部灯；只更新需要的阴影资源 | 支持超过256灯，列表溢出不能丢光；相机外投影者仍需参与阴影 |
| 面光 | 研究LTC等确定性面积积分；阴影另算 | [LTC](https://eheitzresearch.wordpress.com/415-2/)近似BRDF积分，不自动解遮挡；不能把面光无声改成点光 |
| 发光网格 | 继续作为真实发光源；复杂mesh选灯可保留RIS／分层采样 | 全局灯数与三角形数不同；不能把所有三角形放进逐像素全量求和，也不能悄悄删掉直接贡献 |
| DDGI／屏幕外查询 | 初期继续使用完整RT场景可见性 | 屏幕需求生成的shadow页不保证覆盖secondary hit；缺页不能视为无遮挡 |

稳定的确定性直接光不需要为随机选灯去噪，但软阴影、GI、反射仍可能需要重建。当前95点光都有非零光源半径；普通PCF不是面积光软阴影的严格等价物，必须单独评估PCSS／有限RT等近似及其预算。AMD [Hybrid Shadows](https://gpuopen.com/fidelityfx-hybrid-shadows/)展示了用shadow maps分类、减少需要RT的区域与射线区间的方向；该太阳级联示例并不能直接证明95盏点光同样便宜。

### 缓存与生命周期合同

- 光源颜色／强度变化应直接改变着色，并在可见性没有改变时复用深度；位置、方向、投影范围或阴影语义变化按实际依赖使缓存失效。
- 投影者移动、增加、删除、材质alpha变化要覆盖旧／新bounds；静态／动态层可分开，避免角色移动重绘整座静态地牢。
- 不以主相机可见集代替shadow caster集合；不开全灯全方向全分辨率逐帧重绘。
- 资源回收、光源删除／重建、scene close/open、viewport变化保持稳定ID与generation规则；无效历史不能被错误复用。
- 确定性灯列表overflow须完整求值或采取等价保守退路；shadow缓存未就绪／超预算不能偷偷退成无阴影。退路本身需要计费，不能自动对所有灯触发全量RT。
- DDGI和主表面共享灯光物理参数、材质和几何权威，但保持直接／间接路径的计能边界，避免重复贡献。

## 实施顺序与验收终点

1. **直接光主链**：用真实GBuffer记录局部灯重叠分布、受影响屏幕面积及shadow工作量；建立精确分簇累加，复用现有RT作有界质量对照，确认省掉reservoir／radiance重建后的收益。该对照不是长期保留第二套默认入口的理由。
2. **缓存阴影**：点光atlas与方向阴影优先，加入静态／动态失效和完整成本统计；软阴影单独验收。若页面级细分有充分收益证据，再扩展同一缓存机制为VSM，不重建灯光语义。
3. **GI稳定与成本**：独立验证primary方向引导与secondary radiance复用；屏幕probe布局是可能的后续架构演进，不把整套Capsaicin直接叠在DDGI之上。方向分布变化必须改相应PDF／卷积权重，不能只替换随机方向；缓存必须处理表面身份、光源变化与显隐切换。
4. **整帧收敛**：原生1440p、SMAA、既有深度精度；统一计算DI（含shadow update/filter）、GI、AO、反射、后处理与UI，避免只给某一个shader报优秀时间。约2–3ms可作为直接光含阴影的初始设计预算，属于目标，尚非实测承诺。

仅做必要端到端场景：静止暖缓存、首次进入／camera cut冷缓存、角色和门运动、移动／开关灯、薄墙／小窗、复杂发光体、超过256灯与密集局部灯。分别看稳定帧和失效帧的尾延迟；不能只展示静态平均帧率。新路径通过后退役被替代的全屏ReSTIR DI生产链；保留复用的数据契约与有真实用途的GI采样原语。

## 复审与研究边界

两位Astra分别核对固定源码与GI论文／工业阴影资料；随后对建议路线作了反证复审。共同结论是优先验证常规分簇直接光与缓存阴影，暂不直接立项完整VSM。补充重点为精确支持域、列表overflow、相机外投影者、非零半径软阴影及DI/DDGI计能合同。

Exa检索量：主代理25条请求结果＋GI审查代理25条，合计 `sum(numResults)=50`，含重试与重复域；不是阅读了50篇独立文献。主要结论来自固定Capsaicin代码、GI-1.0/1.1原文、[clustered shading原论文](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)、Epic官方VSM文档及Fortnite工程记录、AMD Hybrid Shadows与LTC作者说明。没有引用无条件FPS横比，也没有在本轮运行Capsaicin。

当前仍未知：真实可见表面灯重叠分布、缓存阴影在本GPU的冷／暖成本、密集动态灯尾延迟，以及新路线的完整1440p性能。上述建议是有现有测量与源码证据支持的下一阶段技术选择，不是已经完成的90FPS结果。
