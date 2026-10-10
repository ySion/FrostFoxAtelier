# 编辑器 renderer 验收

## SMAA 下移动物体的光照拖尾（2026-10-10）

证据根目录 `out/moving-history-20261010/`。通过 Control 导出用户当前未保存场景，原进程和 Undo 保持不变。新增 `scene_lit_motion.inl` 沿既有 `--scene-lit-temporal` 入口，用真实 Renderer 分别隔离移动面光阴影、红色／等亮灰色发光面反射、Local GI。固定相机使用 SMAA，移动 16 个接受帧后观察停止响应；清除图像历史并收敛得到同一终点参考。比较仅包含移动前后仍为同一接收面的像素，误差为 RGB8 最大通道绝对差之和，除以初始／终点图像变化之和。它不是残留光能百分比或主观质量分数。

| 停止后 16 帧的相对图像误差 | 修改前 | 最终 |
|---|---:|---:|
| 面光阴影 | .6072 | .1035 |
| 红色反射 | .5990 | .0693 |
| 等亮灰色反射 | .5575 | .0370 |
| Local GI | .3377 | .0658 |

原始 `baseline.log` 和 `baseline/` 确实失败。只全局降低几何变动时的反射历史支持会使远处无关物体的运动将静态反射帧间变化从 .6792 放大到 6.2605，`fix.log` 是失败候选，不作为交付。最终采用局部来源几何验证、跨随机 miss 的贡献见证和 signed RGB／矩的同步仿射收缩；纯红场景中未发光的绿蓝通道最大值为 0，修前可见的白色污染消失。

原移动门限保持不变：停止 8／16 帧的误差分别不超过初始变化的 .25／.15，或实测静态采样噪声的三倍。负对照中的远处物体持续移动，SMAA 反射均值变化 .29%，帧间最大通道差均值为 .5390／.5605；T2x 对应 .69%、1.1313／1.0198。T2x 同一采样相位在第 256／512 帧的反射区域均值变化 .15%，防止来源坐标递归取整漂移。不同 AA 的随机采样和重建结果不要求逐像素／平均亮度一致，跨 AA 差异不作为来源漂移的判据。

Dev `final-dev-temporal.log` 在 GPU validation 下通过新增移动／无关运动／512 帧来源检查和原时间隔离场景。Release `release-quality.log` 在 GPU validation 下通过完整质量入口，包含四 AA、反射／半影稳定性、光源与几何变化、SSIL、DDGI 接缝／薄隔墙、生命周期和 FP32 深度；T2x 静止／停止运动／新身份轮廓最大相邻帧 RGB8 差仍为 0／1／2。Astra 对来源坐标、贡献权重、矩、格式绑定和失效路径复审通过。

Release 编辑器为 `out/release/atelier_editor.motion-history.exe`，SHA256 `fa8ab4ea0b57342b823daeb51a3f99e6caf737a8858505684e34959b891c9d5c`，沿正式 Ninja 链接命令仅改输出名。`release-build.log`、`release-link.log` 记录构建，`preview.result.json` 记录用户场景副本的 Control 对照及最终状态。实际产品捕获不启用 GPU validation，管线验证由上述两个真实 Renderer 入口提供。

没有新增纹理、dispatch 或射线；既有三张 hit 图仍各为 8B／像素，前帧逆投影为新增常量。局部验证增加了 shader 计算，未给出独占 GPU 计时或 FPS 提升结论。反射仍是有限屏幕空间随机估计，移动后保留短暂收敛，不能据本轮结果宣称所有场景无噪声或无拖尾。

## SMAA T2x 静态抖动与反射对照（2026-10-10）

证据根目录 `out/t2x-investigation-20261010/`。原实现把含 jitter 的 surface motion 用于 T2x 独立空间帧颜色重投影，并用深度／背景硬拒绝互补覆盖；即使静止且 SSSR 关闭，两个采样相位仍来回跳动。修复仅为 T2x 派生去 jitter 的速度，沿上游邻域核计算抗锯齿速度，按原两帧 point resolve 和速度幅值权重组合前景／背景。`motion.z` 使用原保留位记录覆盖，`xy/w`、其他信号降噪及 FP32 深度保持原合同。1/256 显示像素的 T2x 速度噪声下限防止 FP16 motion／FP32 投影残差被平方根权重放大；没有增加纹理或 dispatch。

Astra 窄范围复审通过。新增真实 Renderer 端到端 `quality_smaa_t2x_coverage` 关闭环境、解析灯、DDGI、AO、SSSR、SSIL、Bloom，仅观察静止自发光轮廓；同一场景三帧及公开无抖动深度直接比较，门限始终为最大 RGB8 色差不超过 2。旧代码为 188（`dev-red.log`）；只修坐标与覆盖的中间候选为 7（`dev-quality.log`，FAIL，不作通过证据）；最终为静止 0、移动停止 1、新对象身份 2。最终 Dev `--scene-lit-quality` 在 GPU validation 下通过，包含四 AA、灯光／几何／相机变化、反射／阴影收敛、SSIL、FP32 深度及原 DDGI／生命周期 contracts，见 `dev-quality-v2.log`。

Release 正式编辑器使用已保存柱子现场的独立副本，通过 Control 比较 SMAA／T2x × SSSR 开／关，各采集接受帧 256、257、258。反射对照仅在副本将地板 roughness 设为 .18、metallic 设为 .65，原哑光地板 roughness .82 超过 .6 反射阈值，不能作为反射质量证据。新旧基线及修复数据为 `glossy-baseline.result.json`、`glossy-fixed.result.json` 和对应 `.metrics.json`；所有测量来自固定相机，没有模拟输入或修改原场景文件。

| 指标 | 修前 | 修后 |
|---|---:|---:|
| T2x，SSSR 关闭，135 行发光板斜边相邻帧横向位移中位数 | .9645 px | 0 px |
| T2x，SSSR 开启，同一区域边缘位移 | .9645 / .9639 px | 0 / 0 px |
| T2x 反射 ROI 相邻帧 RGB8 平均差 | .5194 | .1460 |
| T2x 反射 ROI 相邻帧 RGB8 差 p99 | 5 | 2 |
| SMAA 反射 ROI 相邻帧 RGB8 平均差 | .1269 | .1282 |

反射 ROI 为完整 1920×1260 客户区的 x∈[800,940)、y∈[775,870)，均为左闭右开。三帧只提供两个相邻帧样本，说明该复现的相位噪声改善，不代表所有材质／运动场景或长期方差。采样期间其他编辑器可能共用 GPU，未据此给出性能提升。现有 SSSR 仍有独立时域矩历史和 step 1／2／4 空间降噪；本轮未升级该降噪器，不能宣称消除所有随机反射噪声。T2x 仍为非递归两帧组合，新显露／增删可能保留一帧旧空间颜色。

修正版为 `out/release/atelier_editor.t2x-fix.exe`，SHA256 `860db923aaa4f5a8335838e2292908d90f3684887daefe23ef97d5f6df5ed63b`。沿正式 CMake/Ninja 构建和链接，只改最终输出名；构建记录为 `dev-build-v2.log`、`release-build-v2.log`、`editor-link-v2.log`。Release 产品捕获为 `glossy-fixed-control/artifacts/2aa5d92b1eb880428010fb19752245b0/91b8ed726059ce49aa62d3335ef35ae6/`。未来 SDF 取舍见 [调研](../../research/mesh-sdf-ddgi-tradeoffs-20261010.md)，本轮未引入 SDF。

## Review 修复与复审（2026-10-10）

证据根目录为 `out/lit-review-fixes-20261010/`，修改前源码保存在其 `baseline-source/`。本批修复普通成员变化清空 DDGI、开放薄片背面误判 inside、World/Renderer 灯值域不一致、点光 source_radius 剔除遗漏、探针小预算失真、空场景资源滞留、被拒绝模式候选污染历史；同时复用静态实例/灯光上传与排序/法线缓存，合并直接光材质恢复和 Compose，关闭 Bloom 时跳过生成通道。仍使用现有 RHI、FP32 深度/距离和原生分辨率 AA。

三位 Astra 分别复审光照语义、生命周期、性能与缓存失效。复审发现的低预算空半球写黑已修正为保留无观测方向历史；验收也从探针分类色改为真实间接光，并补上均匀天空能量与删空重入。最终源码复核未发现剩余阻断项，以下 GPU 验收由主执行方独立完成：

| 配置 / 入口 | 结果 | 证据 |
|---|---|---|
| Dev `--scene-lit-contracts`，GPU validation | PASS | `dev-contracts-v2.log`、`dev-contracts-v2/` |
| Dev `--scene-lit-ddgi`，debug layer | PASS | `dev-ddgi.log`、`dev-ddgi/` |
| Release `--scene-lit-quality`，GPU validation | PASS，包含四种 AA 和上述 contracts | `release-quality.log`、`release-quality/` |
| Release 正式编辑器 / Control | PASS，接受帧 96 捕获及独立复现导出 | `editor-fixed.result.json`、`editor-capture.log` |

开放薄片与闭合立方体共用同一个 Mesh 的场景中，水平传播方向光只照亮墙，地板直接项为零：DDGI Off 接收面不超过 1，开启 DDGI 后接收面红通道均值为 46.6528。反绕序、镜像、连续 24 帧成员增删和快照重排均通过；后者同时检查真实光照与没有全局 history reset。均匀天空从独立初始化比较 64 条和 1 条照明射线，接收面均值均为 78；实际完成查询计数包含每探针独立的 32 条固定几何射线。上述数值是捕获的 sRGB 通道值，不是线性辐照度单位。首轮 `dev-contracts/` 使用了分类色，不作为 GI 能量证据；正式结论采用 v2。

原质量阈值没有放宽：Release 墙/地面比例 p10 为 .9986，静态整墙最大相邻帧 mean .0804 / p95 1，移动发光体 mean .9786 / p95 3；12 cm 与亚像素隔墙的暗侧均为 0，direct64 参考也为 0。SSIL 关闭恢复的画面差为 0；四种 AA 的收敛及连续灯光、几何和相机变化检查通过。

正式编辑器沿上轮已保存的柱子复现建立独立 `PreviewProject`，没有修改原未保存窗口。新可执行文件为 `out/release/atelier_editor.lit-review-fixes.exe`（SHA256 `1041f6ab692f0f5dca1c2d2343880fc913f7a23a16d783e06da8093e939effe5`），由 CMake/Ninja 的正式链接命令仅改输出名生成，以保留正在运行的旧二进制。捕获为 `editor-fixed-control/artifacts/05bcf62dee008344b96fd7ba222e84d4/9d0a85965e22ca499fa5e668d0bbfe6b/sample-1.png`；临时验收实例已关闭，原两个实例保留。

同为 1920×1260、SMAA 的柱子捕获，逻辑资源从 873571020 降至 854217612 字节，减少 19353408 字节（约 18.46 MiB），见 `editor-resource-comparison.json`。它等于移除 `8P` 中转纹理减去本场景 12 个缓存三角形新增的 `16T` 数据；不包含待退休/驱动开销，不作为 FPS 或实际显存结论。视口尺寸资源裁剪、空间多灯 proposal 和超过 256 解析灯的扩展仍属后续演进。

## 发光面主动采样与 DDGI 重建成本（2026-10-10）

证据根目录为 `out/lit-sampling-4f514c5d/`。用户调整柱子后，通过 Control 读取内存属性和相机，在独立工程恢复现场；逐 Actor/Component 属性比较一致，原工程 Config/Content/Source 的四个输入文件与开始时副本的 SHA256 一致。固定视角的旧版对照中，柱子旁尖锐亮斑随 DDGI 关闭而消失，单独关闭 GTAO、SSSR 或解析阴影仍保留。旧窗口存在 Control busy 误报，未被关闭、保存或取消编辑。

本批增加 world-space 发光三角 alias 分布与四候选 streaming RIS；屏幕漫反射和 probe-hit shading 共享采样/PDF、真实终点遮挡。DDGI 与 SSIL 不再重复注入已由直接光负责的一跳 emission。移除 DDGI 5×5 表面重建、对应 pipeline 和中间纹理，保留实际 probe→receiver LOS、连续方向权重以及 FP32 深度/距离。1440p 少一张 28.125 MiB 逻辑纹理；新增发光表每三角 64 字节，仅发光数据变化时重建。

新能量场景关闭 DDGI、SSIL、所有解析灯与环境光，以 64 样本检查真实 GPU 接收面；观察的是 sRGB 红通道均值，不将其比例当作线性能量比例：

| 条件 | 红通道均值 |
| --- | --- |
| 无发光面 | 0 |
| 两三角发光面 | 50.0138 |
| 不等面积四三角 | 50.0459 |
| 镜像与非均匀实例变换后同一表面 | 50.0138 |
| 同一表面细分为 800 个发光三角 | 49.9458 |
| 反向绕序 | 50.0441 |
| 辐射加倍 | 85.9669 |
| 恢复原辐射并开启 DDGI | 50.0138 |
| 加入遮挡 / 删除发光面 | 0 / 0 |

最初新增直接发光暴露了降噪器的跨薄墙混色：RIS 候选的亚像素隔墙暗侧为 34，而只读直接光时间结果为 1、未滤波 direct64 为 0。修正空间重建，使当前接收点的估计方差决定混色范围，避免明亮邻点的噪声扩大稳定暗区的支持。原门槛没有放宽，最终 Dev/Release 专项结果相同或仅采样相位有微差：墙脚/墙面比 p10 约 .9983；静态整墙最大帧差均值 .0815、p95=1；移动自发光最大均值 .9659、p95=3；12 cm 隔墙暗侧 0、亮侧约 229.1；亚像素隔墙 0 / 230，direct64 暗侧也为 0。

`final-dev-ddgi.log`、`final-release-ddgi.log` 均退出 0。`final-release-quality.log` 开启 GPU validation，四种 AA、反射/半影稳定性、直接光参考、连续灯光编辑、几何/相机恢复、Local GI 和 FP32 Reversed-Z 均通过；随后仅增加镜像/800 三角验收，生产代码不变，并在两种配置的专项入口补验。日志未报告 GPU warning/error。

DDGI 计时与可见性射线已按 update/transfer、receiver gather 拆分，与主阶段使用同一个完成样本。原三阶段诊断证明 5×5 重建在该柱子视角额外执行约 1727 万查询；移除后该项为零，gather 约 583 万查询保留。`benchmark-{stats-baseline,final}` 记录 1440p 原始样本及资源统计，但原用户 Lit 窗口仍在并行渲染，`benchmark-condition.json` 明确标记未隔离；这些计时不能用于宣称稳定帧率或无干扰性能提升。

该固定场景的 240 个唯一完成样本中，可见性查询中位数从 37,065,305 降至 15,250,712，减少约 58.9%；主探针查询仍为 9,216。gather 查询保持 8,442,020，移除的表面滤波为 25,147,940；新发光直接采样及 probe-hit shading 增加的查询已计入最终总量。逻辑资源从 1,361,695,756 降至 1,332,205,060 字节。射线与资源计数不受同时渲染导致的计时伸长影响；它们也不等同于帧率收益。

补充配置对照中，当前 Dev 也出现了长延迟，因此没有归因为 Release 独有回归。临时 Dev 副本完成并关闭后，同一 Release 预览进程在不重建、不重启的情况下恢复到 SSSR 约 1.289 ms、DDGI 约 .476 ms 的单帧样本，见 `config-release-capture.response.json`。Windows adapter counters 同时观察到 dedicated 约 14.63 GiB、shared 约 2.21 GiB、committed 约 18.61 GiB；这是资源压力线索，不是已证实的因果，也不能把进程 counters 简单相加解释为物理驻留。原始记录已交 RHI 负责人核对，未为猜测修改 RHI 或绕过它。

最终 Release 预览为 `out/release/atelier_editor.lit-sampling.exe`，由当前官方 Ninja 链接命令仅改输出名称生成，避免覆盖正在运行且有未保存数据的旧程序；命令保存在 `release-preview-link.txt`。窗口打开独立 `DeliveryProject`，完成 SMAA、AA Off、direct64 的 32/33 接受帧对照和复现导出，返回后恢复基础 SMAA/单样本设置；这组 GUI 观察不替代上面的长收敛质量验收。记录为 `delivery-final-*.response.json`、`delivery.result.json` 与 `delivery-evidence.json`，前后图为 `stats-baseline-viewport.png`、`delivery-viewport.png`。新版 Control 提供具体 busy_reasons 并区分被动文本焦点，消费者的实际 compare/export 成功；用户旧窗口仍保持原进程和未保存状态。

当前限制：发光体镜面仍依赖 SSSR，离屏镜面路径未补全；解析灯仍有 256 上限和既有 tile 溢出路径，800 三角验收只证明派生发光面不受该上限截断。局部高密度解析灯、空间重要性采样和 Adaptive DDGI 调度仍待后续批次，未以本轮结果宣称完成。

## DDGI 墙角残留暗斑与探针过渡（2026-10-10）

前一轮逐 tap 距离矩修正缓解了尖斑，但没有覆盖用户新的贴近墙角视图。通过 Editor Control 固定当前相机、保留发光网格与仅剩的点光/天光后对照：关闭 GTAO 只能减轻接触暗部，残留不均匀暗斑仍来自 DDGI。最终让实际 probe→receiver 射线剔除被阻挡的贡献，同时保留距离矩的连续方向权重。纯二值可见性候选虽然通过墙脚检查，却在柱子旁暴露探针成员切换的三角轮廓，因此未作为最终交付。小范围接收面重建另用真实 LOS 约束，避免薄墙两侧同平面的像素混光；全部追踪和提交沿现有 RHI 入口，D32/R32 深度与 RG32 距离矩保持不变。

证据根目录为 `out/ddgi-contact-7d839dce/`。新增亚像素隔墙场景，并将原墙脚比值下限从 .75 收紧至 .95；原实现重新编译后在同一验收入口确实失败，记录在 `regression-baseline-verified.log`。最终 Dev `--scene-lit-ddgi` 通过：

| 指标 | 本轮原实现 | 最终组合 |
| --- | --- | --- |
| 墙脚/墙面红通道亮度比 p10 | .8338 | .9971 |
| 静态整墙最大帧差均值 / p95 | .2093 / 1 | .2027 / 1 |
| 移动自发光整墙最大帧差均值 / p95 | .7291 / 3 | .7052 / 3 |
| 12 cm 隔墙暗侧 / 亮侧红通道均值 | .8182 / 236.1240 | 0 / 236.2231 |
| 亚像素隔墙暗侧 / 亮侧红通道 | 220 / 224 | 1 / 239 |

最终 Dev 专项与 Release 完整 `--scene-lit-quality` 均退出 0，日志未报告 GPU warning/error；Release 覆盖四种 AA、直接光/反射/半影稳定性、连续光照编辑与收敛、Local GI、几何/相机恢复、Reversed-Z 和上述 DDGI 场景。最终日志和图像分别在 `gated-final-dev-ddgi`、`gated-final-release-quality` 及相邻 `.log`；本轮 Dev 未重复完整质量入口。

最终 Release 正式编辑器在用户原工程、相同相机和显示设置下完成 512 接受帧对照与独立复现导出；窗口留给用户继续观察。`before-viewport.png` 与 `gated-final-viewport.png` 为修复前后图像，`gated-final-editor-4.response.json`、`gated-final-editor-6.response.json` 分别记录对照与导出。应用负责人提供的 Control 长路径修复已合入消费树并通过此次导出，原工程四个输入文件的 SHA256 未变化。当前实例、源码/二进制指纹与相机/显示恢复证据见 `gated-final-manifest.json`。

RX 6800 XT、Release、原生 2560×1440、验证关闭，沿既有固定 benchmark 场景（8 个网格、4 盏灯、SMAA、`11×7×10` 探针、96 更新/帧、64 照明射线）串行比较。运行中的编辑器临时切至 Flat，测量后恢复；每个版本预热 96 帧并收集 240 个唯一完成样本，测量阶段不 Capture。最终数据在 `gated-controlled-{moment-baseline,gated-final}/benchmark-{gpu.csv,summary.txt}`：

| GPU 阶段 | 原实现 p50 / p95（ms） | 最终组合 p50 / p95（ms） |
| --- | --- | --- |
| DDGI | .450 / .454 | 1.567 / 1.574 |
| 已计时通道总和 | 6.618 / 6.939 | 7.743 / 8.059 |

额外逻辑纹理字节为 29,491,200（28.125 MiB），不是物理显存峰值。该 benchmark 的 DDGI 开销增加约 1.12 ms，不能推广为完整编辑器帧率或复杂场景上限。方向缓存与有限探针密度仍会影响有效贡献之间的插值；本轮结论限于已复现的墙角、成员过渡、运动与隔墙场景，不宣称所有 GI 伪影消失。

## DDGI 方向可见性尖斑（2026-10-10，以上墙角修正前）

在 `codex/viewport-rendering` 的 `17996cee` 加既有天光增量上，通过应用负责人提供的 Editor Control 增量读取现场、独立导出和固定相机对照。首次场景关闭 DDGI 后尖斑消失，单独关闭 GTAO 或 SSSR 后仍在；增加照明射线不能消除其空间形状。逐项诊断将问题定位到方向距离矩混合后的可见性：改为四个 tap 各自计算并压缩 Chebyshev 可见性，再双线性插值。最终未保留诊断用的可见性旁路或更高分辨率距离图。

用户随后删除三盏解析灯并移动、放大发光板。保存后的 12 Actor 场景保留 Cool point 和 Sky Light；在修复前后的正式 Release 编辑器中各等待 256 个接受帧，复现并消除了沿墙排列的三角亮斑。两边场景文件 SHA-256、相机矩阵、显示设置、环境和 viewport 均一致；证据为 `out/lighting-spikes-4a8e3e16/adjusted-scene-comparison.json` 及其指向的两组 PNG、FP32 深度和 manifest。这是固定场景的视觉端到端对照，不是所有场景无伪影的声明。

Dev / Release 的 editor、control、Renderer 验收目标均构建通过；两种配置的 `--scene-lit-ddgi` 均通过，既有阈值未改变：

| DDGI 专项指标 | Dev / Release |
| --- | ---: |
| 墙脚 / 稍高墙面红通道比值 p10（下限 .75） | .8338 |
| 静态整墙最大帧差均值 / p95（上限 3 / 8） | .2093 / 1 |
| 发光物连续运动最大帧差均值 / p95（上限 3 / 8） | .7291 / 3 |
| 12 cm 隔墙暗侧 / 亮侧红通道均值 | .8182 / 236.1240 |

Release 完整 `--scene-lit-quality` 通过 GPU validation，覆盖四种 AA、直接光与反射稳定性、连续光照修改、Local GI、几何/相机恢复、Reversed-Z 和上述 DDGI 场景；本轮 Dev 执行 DDGI 专项，未重跑完整质量入口。日志与图像在同一证据根目录的 `release-quality`、`dev-ddgi`、`visibility-filter-release` 及相邻 `.log`；正常退出且未报告 D3D12 warning/error。

最终修改没有新增 GPU 资源或射线，保留 8×8 方向图与 RG32_FLOAT 距离矩，只增加逐 tap 的可见性算术；本轮未测量常规帧的性能差值。当前实现与有限角分辨率边界见 [DDGI 探针场](scene-lit.md#ddgi-探针场)。原工程没有被本轮诊断写回；修正版编辑器打开独立的 `UserAdjustedProject`，可通过正常场景命令继续编辑、保存。

## 单一天光、Lit 黑背景与中键方向（2026-10-09）

在阶段集成基线 `17996cee` 上修正中键水平环绕方向。Scene Lit 始终使用黑背景，工作室模式保留渐变偏好，网格与轴线仍独立；黑背景上的底部操作提示改用浅色。均匀环境辐射从 ViewSettings 移到场景快照，只由 World 中唯一的 Sky Light 提供，缺失或停用时为零。示例房间显式包含一个 Sky Light，替代此前的隐式环境值。

| 端到端结果 | Dev | Release |
| --- | --- | --- |
| 最终 editor、文档消费者与 Renderer 编译链接 | PASS | PASS |
| World → SceneSnapshot：天光创建、属性、停用、删除/撤销、保存重开、重复天光和类型切换拒绝 | PASS | PASS |
| Renderer `--scene-lit`：黑场、显式天光、移除后收敛、Lit 空场景黑背景及原有光照/阴影/DDGI/GTAO/反射/深度 | PASS | PASS |
| 正式窗口：四类灯菜单、天光属性与保存重开、中键右拖及反向拖回 | PASS | PASS |

无天光、直接灯和自发光的真实几何像素为黑。移除已收敛天光后，探针仍按既有分帧历史衰减：首次 192 帧读回有 78 个像素残留 2/255；最终验收保持每通道 ≤1/255 的终点要求，在 384 帧后检查通过，没有改动 DDGI 滤波、射线预算或放宽阈值。该帧数是此验收场景的收敛等待，不是运行帧率测量。首版天光为均匀辐射，尚不提供 HDRI、方向性天空或可见天空绘制。

证据在 `out/sky-light-20261009/`：`{dev,release}-final-build.log`、`{dev,release}-document-final.log`、`{dev,release}-renderer-final.log` 和 `{dev,release}-editor.log`；GPU 图像在 `{dev,release}-renderer-final/`，正式窗口图像及临时工程在 `{dev,release}-editor/`。Dev 核心交互验收在提示文字颜色调整前执行；最后的提示文字显示由双配置文档/布局验收及 Release 正式窗口确认。所有保存/重开检查均使用临时工程副本，源示例未被验收修改。

## 阶段集成边界（2026-10-09）

本轮将 Claude/Sponza 与 Scene Lit 质量分支在共同内容快照 `f967165` 上逐文件合并。保留基础模式的 MeshPart、漫反射/opacity 图、Alpha Test 和编辑叠层，以及 Scene Lit 的 Reversed-Z、独立信号重建与 DDGI 修正。Scene Lit 的 GPU 顶点读取同步为含 UV 的 32 字节布局，三角形为 112 字节；材质像素验收采用近 .8 / 远 .4 的反向设备深度。

Scene Lit 仍不采样 Mesh 材质图，其辅助深度与叠层保持和不透明光栅/射线相同的覆盖。纯线框和基础着色继续裁剪透明材质。下面独立分支各节保留各自的历史证据；阶段合并的验证单独记录。

阶段 GPU 验收首次失败于材质图像检查。读回四个采样点均为 `(0,216,0,255)`、设备深度约 .6：默认 Camera 已将世界 z 映射为 `1-z`，合并时又把验收几何的近 .2 / 远 .6 反置成 .8 / .4，背景因此覆盖贴图。修正恢复原几何，仅将设备深度预期保持为近 .8 / 远 .4；全部颜色与深度阈值不变。修正后的四点依次为红 `(216,0,0)`/.8、透孔露出的绿 `(0,216,0)`/.4、蓝 `(0,0,216)`/.8、黄 `(216,216,0)`/.8。两个配置的基础验收均已通过，并保留 `material-coverage.png` 与实际像素日志。

阶段集成最终使用各配置重新构建的正式 `atelier_renderer_acceptance` 完成以下真实 GPU 验收：

| 验证 | Dev | Release |
| --- | --- | --- |
| 无参数：基础模式、材质、参考网格、UI、深度和资源寿命 | PASS | PASS |
| `--scene-lit-quality`：四种 AA、直接光、反射、连续灯光编辑、Local GI、几何/相机恢复、Reversed-Z、DDGI 接缝与自发光更新 | PASS | PASS |

两个配置的 `image-metrics.csv` 均为 323 行且逐字节一致，也与合并前 Renderer 分支的完整质量记录一致。DDGI 墙脚比值 p10 为 .8068；静态整墙最大帧差均值 / p95 为 .2089 / 1，自发光移动为 .7222 / 4；隔墙暗侧 / 亮侧红通道均值为 0 / 236.2149。四份最终运行日志均无 warning、error 或 failed 记录。

证据根目录为 `out/stage-sync-20261009/`：构建记录为 `{dev,release}-renderer-final-build.log`，运行记录为 `{dev,release}-renderer-final.log` 与 `{dev,release}-quality-final.log`；材质图像在 `{dev,release}-renderer-final/`，完整质量图像和指标在 `{dev,release}-quality-final/`。这些结果覆盖阶段合并后的 Renderer，不代替集成负责人执行的其他模块或编辑器交互验收。

## Sponza 完整模型与材质（2026-10-08）

本次增量基于 `402fab075bb2aa6605b51fdb767387ebd0e08869`，在 Windows x64 / AMD Radeon RX 6800 XT 完成以下真实端到端验收；基线 SHA 不包含本次 Sponza 实现。

| 验证 | Dev | Release |
| --- | --- | --- |
| 正式 editor、document、renderer、physics、World 构建 | PASS | PASS |
| 完整 editor_document 链，含原工程导入回滚和物理示例 | PASS | PASS |
| Textured OBJ 与完整 Sponza 资源、场景序列化往返 | PASS | PASS |
| renderer_acceptance：材质像素/深度及全部既有 GPU 场景 | PASS | PASS |
| 正式 editor 的 Sponza 内庭、外观全景与 MatCap 捕获 | PASS | PASS |
| Physics 15 个场景 / World 8 个场景 | PASS | PASS |

源资产逐文件 SHA-256 验证通过：57 个原始文件、95,882,276 字节，与 [source.json](../../../examples/physics-test/Content/Sponza/source.json) 一致。解析结果为 262,267 个三角形、103 个连续材质组、25 个命名材质及 24 个漫反射贴图材质；源文件未改动，Scene 保持相对资源引用与 0.01 实例比例。

真实导入链验证凹多边形面积、负索引/负 UV、共享 WIC 图像、重复导入拒绝、刷新幂等、图片/MTL 改写和删除、旧 Mesh/图像 owner 保持，以及缺依赖和目录逃逸拒绝。GPU 像素读回区分两组几何，验证顶点起点、sRGB 漫反射、UV 重复与左上原点、漫反射 alpha 和独立线性透明遮罩、背景深度、线框/MatCap 轮廓、暖帧零上传、替换与移除后的 owner 释放。完整窗口开启 D3D12 debug/GBV，捕获实际场景颜色与深度；最终渲染器和窗口日志未报告 D3D12 warning/error。

可重复入口为 `editor_sponza_acceptance` 与 `renderer_acceptance`，使用现有 dev/release presets。证据为 `out/<配置>/sponza-{document,import,renderer,window,physics}.log`；图片和窗口报告在 `out/<配置>/sponza-evidence/`，包括 `sponza-interior.png`、`sponza-overview.png`、`sponza-matcap.png` 和 `acceptance.txt`。正式打开方式见 [Physics Test](../../../examples/physics-test/README.md#sponza-场景)。

该次 Sponza 验收使用完整几何、漫反射/遮罩、mip 和基础方向光，不覆盖 Scene Lit 的贴图、DDGI、阴影或 PBR，也没有附加 Sponza 物理 Body。未测量 GPU 帧耗时或显存性能收益。OBJ/MTL 来源贴图中暂未使用的凹凸/高光数据仍保留。

## DDGI 接缝与自发光连续更新（2026-10-09）

在同一工作树修正用户发现的墙脚脏边和发光网格移动时的区域跳变。原质量场景主要移动解析矩形光、测量局部接收区域，本次补充默认 `8×4×8` 探针、64 个/帧、128 条照明射线下的强自发光网格与整墙场景。正式 Renderer 的 `--scene-lit-ddgi` 已复现原错误并返回失败，修改前图像和指标保留在 `out/ddgi-wall-20261009/baseline/`。

逐项对照确认：先乘空间权重再进行非线性压缩，会锐化探针之间的过渡；原 .04 倍间距的表面偏移不足以覆盖墙角的方向距离滤波范围。分离线性空间权重并将法线偏移对齐到 .12 倍间距后，接缝检查通过。单独改变归一化分母或加入可见性下限没有改善该场景；最终保留正确的归一化语义，未加入可见性下限。空间权重顺序也与 [NVIDIA RTXGI 的查询实现](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl) 交叉核对。

移动记录还复现四帧轮转造成的停顿与跳变。现在分批更新采样目标、逐帧推进有效探针的照明历史，复用现有传递通道；没有增加射线、屏幕颜色模糊、原生 API 或提交入口。当前机制见 [Scene Lit 探针场](scene-lit.md#ddgi-探针场)。

| 最小复现场景指标 | 修正前 | 修正后 Dev / Release |
| --- | ---: | ---: |
| 墙脚 / 稍高墙面的红通道比值 p10 | .1093 | .8068 |
| 静态 16 对整墙连续帧的最大均值 / p95 | .7344 / 6 | .2089 / 1 |
| 32 帧发光网格移动及强度编辑的最大均值 / p95 | 2.4311 / 19 | .7222 / 4 |

比值在墙上 61 个位置比较高度 .035 m 与 .35 m 的小区域；帧差使用 sRGB 8 位最大通道差，表中结果不是物理辐照度真值。空间比值下限 .75、帧差均值 3 / p95 8 均在修正前确定，未随结果放宽。运动始终保持物体与场景身份，停止后照明继续收敛；最终照明必须与运动前有可见差异，避免冻结旧结果也能通过稳定性检查。

额外的 12 cm 隔墙延伸越过探针体积，强自发光位于另一侧；关闭环境后，暗侧红通道均值为 0，亮侧为 236.215，证明本场景没有以穿墙漏光换取接缝平滑。默认体积新增的两张目标历史纹理共 256 KiB 逻辑字节；此数值是资源尺寸计算，不是 GPU 帧耗时或物理显存测量。

Dev / Release 的完整 `--scene-lit-quality` 均通过 GPU validation，覆盖四种 AA、直接光与反射、连续灯光编辑、Local GI、几何/相机恢复、Reversed-Z 和以上新增场景，日志没有 D3D12 warning/error。完整证据为 `out/ddgi-wall-20261009/{dev,release}-quality/` 与对应 `.log`；补充最终照明变化约束的快速入口结果为 `{dev,release}-ddgi-final/`，同样使用真实 GPU，但验证级别为 debug layer。本节记录的是 `codex/viewport-rendering` 的工作分支验证，主线集成另行进行。

本轮没有重新执行编辑器原生菜单及保存/重开输入链，已有产品输入验收仍归属下一节的候选。有限体积、低频空间近似与强小自发光源的随机采样限制仍在；本节证明给定场景的修正，不将先前或当前的 PASS 扩展成所有场景无伪影。

## Scene Lit 质量演进（2026-10-09，以上 DDGI 修正前）

本轮仍在 `codex/viewport-rendering`，基础提交为 `402fab075bb2aa6605b51fdb767387ebd0e08869`，加同一工作树中的集成源码。当前合同见 [Scene Lit](scene-lit.md)。下方 10 月 8 日首批数据是历史结果，不能代替新版结果。

| 真实端到端结果 | Dev | Release |
| --- | --- | --- |
| Renderer、editor、文档消费者编译链接 | PASS | PASS |
| 原有 Renderer：工作室显示、线框、参考辅助、UI、深度与寿命 | PASS | PASS |
| Scene Lit 完整 GPU 功能与执行域重建 | PASS | PASS |
| 四种 AA、独立光照稳定性、移动灯光、Local GI、远距离深度 | PASS | PASS |
| Scene Lit 工程、参数、持久化、四种 AA 菜单与近场互斥 | PASS | PASS |
| 正式窗口原生菜单、AA 切换、三类灯操作、保存与重开 | PASS | PASS |

### 图像与合同证据

`--scene-lit-quality` 使用正式 Renderer、真实窗口和 GPU validation。每种 AA 在初始状态和连续移动灯光后的固定终点各预热 96 帧，再逐帧检查 16 对图像；反射与半影分别测量。差异为 sRGB 8 位最大通道差，原有上限均值 3 / p95 8 未放宽。

| AA | 64 个区域对比中的最大均值 | 最大 p95 |
| --- | ---: | ---: |
| Off | 0.413 | 1 |
| TAA | 0.835 | 2 |
| SMAA | 0.388 | 1 |
| SMAA T2x | 0.727 | 1 |

上述为 Dev / Release 实测，两个配置的质量指标一致。每种 AA 的 32 帧灯位/强度编辑均没有全局历史重置，终点照明发生可见变化。静态 Capture 深度逐值一致；jitter 留在内部光照采样中，公开深度和编辑辅助使用未抖动相机。确定性 64 样本直接光两次捕获完全一致，单样本重建相对它的半影区域均值 1.818、p95 3；有限 64 样本对照不等于无偏真值。

Local GI 对照关闭 DDGI，使用固定直接光与均匀环境；邻近红色自发光源的地面差异均值 1.008、p95 2，远处区域差异为 0，连续帧局部稳定性与关闭后恢复基线均通过。该结果证明有界近场修正，不表示全场景额外 GI。

深度验收覆盖透视/正交近远裁剪、900 米处相差 2 厘米的表面顺序和有限 Reversed-Z。零键兼容物体与 `UINT64_MAX-4` 真键同时存在时，直接光颜色及深度与原场景逐值相等，证明光栅和 TLAS 都保留两个物体。完整功能验收的静态帧差均值 0.554、p95 1；重建相同执行域后均值 2.744、p95 7，仍在原有上限内。

### 审计与失败归因

两组 Astra 只读审计分别检查光照算法与资源/深度/公共合同。已落实反射材质边界、有符号控制变量重建、逐信号依赖失效、TAA 逐 tap 历史判断，以及辅助显示深度、稳健法线、私有物体键域和环境变化响应。所有 GPU 操作继续经过现有 RHI/gfx。

首轮 TAA 半影采样的异常大跳变集中在 ROI 最左列 10 个像素。几何投影和失败帧证明该列距遮挡物轮廓仅 0.008～0.138 像素，jitter 会把地面替换成方块。测量点由 `(.75,.01,3.6)` 移到同一半影中的 `(.85,.01,3.6)`，整块 ROI 留在地面且距轮廓超过 4 像素；没有扩大阈值或排除正常抖动帧。原始失败 PNG/逐像素 CSV 保留在证据目录。

正式窗口首次检查还发现空背景深度的旧断言仍为 1，已迁移到 Reversed-Z 的 0。Dev 窗口通过后，视觉复核发现参考网格与真实地板共面时产生斑点；浅视角的新增 GPU 场景复现了 2,213/3,045 个实体像素被辅助网格改变。网格改为直接解齐次平面，并在辅助比较中覆盖子像素光栅舍入误差，主 D32 深度不变。修正后 Dev / Release Renderer 均通过三个透视角度的逐像素检查，全部实体像素与关闭网格时相等；同时重跑了既有 Renderer 的完整回归。

### 本轮证据

本机目录为 `out/scene-lit-quality-20261008/`（沿用开始工作时的日期）。`{dev,release}-quality-final/` 包含逐帧区域指标、四种 AA 的灯光轨迹和 PNG；`{dev,release}-functional-final/` 为完整功能图像；`dev-quality-audited/` 保留旧 ROI 的失败诊断。各配置日志位于同目录；最终来源和文件指纹在 `final-manifest.json`。

正式产品窗口分别在 `dev-product-reversed/` 与 `release-product-coplanar/` 通过：包括此前的 MatCap/线框/参考网格流程、四种 AA 原生菜单、三类灯创建及键入参数、`Shadows 4/5` 提示、临时工程保存/重开。源示例 Config/Content 保持不变，重开后有 10 个网格及 7 盏作者灯。Dev 窗口之后仅修正辅助网格和对应验收；该改动由双配置 `*-grid-final.log` 和最后的 Release 产品窗口验证，未改已测量的 Scene Lit 光照信号或无辅助网格的 benchmark。所有通过的 GPU 日志没有 D3D12 warning/error。原生窗口启用 GPU validation 和 VSync，不能作为性能测量。

Release 最终截图已复核，真实地板上的共面参考网格斑点消失。SSSR 仍受屏幕可见信息限制，DDGI 仍为有限体积的低频近似；本轮指标证明指定场景的稳定性与合同，不宣称所有材质、场景和运动都不存在伪影。

### RX 6800 XT 新版性能

Release、验证与 VSync 关闭、原生 2560×1440；场景仍为八个共享 cube、一盏方向光、两个点光、一盏矩形光，四盏均投影。默认 SMAA，DDGI / GTAO / SSSR / Bloom 开启、Local GI 关闭；探针 `11×7×10`，每帧更新 96 个，每次更新 32 条固定几何射线与 64 条照明射线。预热 96 帧后收集 240 个唯一完成样本，期间不 Capture。

| GPU 阶段 | p50 ms | p95 ms |
| --- | ---: | ---: |
| GBuffer | 0.062 | 0.065 |
| AS/上传 | 0.0004 | 0.0006 |
| 直接光与独立降噪 | 2.806 | 3.087 |
| DDGI | 0.424 | 0.430 |
| GTAO 与独立历史 | 0.782 | 0.794 |
| HDR/SSSR 与独立降噪 | 2.315 | 2.836 |
| HDR 合并/表面历史 | 0.229 | 0.238 |
| Bloom/色调映射/SMAA/显示 | 0.460 | 1.263 |
| 每样本阶段合计 | **7.167** | **7.887** |

合计 p99 为 8.138 ms；相对首批 4.249 ms 的 p95，独立降噪与新 AA 增加了真实成本，不宣称免费提升。逻辑资源峰值为 1,331,414,708 字节（约 1,269.7 MiB），不等于物理显存峰值；Local GI 首次开启另需四张 RGBA16F 纹理，1440p 约 112.5 MiB。阶段计时不包含完整编辑器 CPU、Present 等待、参考辅助与 UI，不代表完整游戏帧率。

数据在 `release-benchmark-final/benchmark-{gpu.csv,summary.txt}`。启用 GPU validation 的进程首次准备曾观察到较长 CPU 等待；关闭验证的本次性能运行没有相同等待现象，不能把前者作为普通产品启动或 GPU 帧耗时。

## Scene Lit 首批（2026-10-08）

集成位置为 `codex/viewport-rendering` 工作树，基础提交 `402fab075bb2aa6605b51fdb767387ebd0e08869` 加本次未提交源码。RHI、World、Application 的拥有者交付均在同一工作树构建、链接和验收；没有绕过 RHI 的设备、队列或图形原生接口调用。功能范围和历史归属见 [Scene Lit 合同](scene-lit.md)。

| 真实端到端结果 | Dev | Release |
| --- | --- | --- |
| Renderer、正式 editor、文档消费者编译链接 | PASS | PASS |
| 原有 Renderer：MatCap、Smooth/Flat、线框、背景/网格、UI、变换及寿命 | PASS | PASS |
| Scene Lit GPU：三类灯、真实阴影、HDR/材质、DDGI、GTAO、SSSR、时域、正交、清空与 resize | PASS | PASS |
| 完整编辑文档、Project、原生 UI/Source 消费链 | PASS | PASS |
| Scene Lit World/Project、灯参数与持久化、独立运行 World 快照 | PASS | PASS |
| 正式窗口：原生菜单、三类灯创建、键入参数、预算提示、保存与重新打开 | PASS | PASS |

GPU 图像验收启用 D3D12 debug/GBV。方向光、两个点光及矩形光的阴影开关分别产生 6,432、4,211、11,503、6,687 个变暗像素；面积和单面朝向也改变真实受光。第五盏投影灯在预算 4 下仍正常接受帧，统计为 4 shadowed / 1 unshadowed，正式状态栏显示 `Shadows 4/5`。

DDGI 验证红/蓝墙面向可见地板反弹以及墙面改色后更新；接收点核对投影深度，避免将遮挡它的金属面当作漫反射接收面。GTAO 的直接光/自发光对照使用确定性光源，RGB 差为 0；接触遮蔽使用固定环境隔离其他随机项。SSSR 对照关闭 DDGI 并使用固定 64 样本直接光，检测到 22,815 个变化像素；另行确认屏外环境回退。

同一静止场景 96 帧预热后隔 8 帧比较，完整链 RGB 最大通道差均值 0.543、p95 为 1；场景域重建后对照均值 1.118、p95 为 3。验收上限始终为均值 3、p95 8，没有放宽。Dev 的五组时域隔离均通过；追加 16 帧连续刚体移动，保持稳定组件键和 scene revision，历史重置数每帧为 0，实际受光变化 46,780 像素。

正式编辑器验收使用临时工程副本，源示例保持不变。原生菜单创建三类灯并键入 `79.25 cd`、`22.5 cd/m²`、`3.25 lux`，重开后保留 10 个网格和 7 盏灯。静态标签不属于可编辑 `TextGeometry`：精确单位文本由文档链核对，窗口检查其真实绘制并保留截图；点光参数及预算提示已人工查看实际 PNG。当前没有 Simulate UI，运行状态独立性由独立 World 的正式 `start/update/stop → SceneSnapshot` 和 Renderer 的场景域对照验证，不将编辑文档强行切入运行状态。

### RX 6800 XT 本机性能

Release、验证关闭、VSync 关闭、原生 2560×1440；固定场景为 8 个共享 cube 实例、一盏方向光、两个点光、一盏矩形光，4 盏均投影。DDGI 使用 `11×7×10` 探针，每帧更新 96 个、每探针 64 条主射线；其余效果开启，阴影 1 样本。预热 96 帧后收集 240 个唯一、原始、已完成 GPU 样本，采样循环没有 Capture。

| GPU 阶段 | p50 ms | p95 ms |
| --- | ---: | ---: |
| GBuffer | 0.056 | 0.057 |
| AS/上传 | 0.006 | 0.008 |
| 直接光/阴影 | 1.395 | 1.576 |
| DDGI | 0.395 | 0.400 |
| GTAO | 0.295 | 0.298 |
| HDR/SSSR | 1.130 | 1.421 |
| TAA | 0.476 | 0.483 |
| Bloom/色调映射 | 0.179 | 0.183 |
| 每样本阶段合计 | **3.932** | **4.249** |

合计 p99 为 4.265 ms；分位数来自每个样本的合计，不是逐阶段分位数相加。当前资源逻辑峰值为 689,800,900 字节（约 658 MiB），包含 AS 数据量，不是物理显存峰值。GPU 阶段和不包含编辑器 CPU、Present 等待、显示扫描、首次 shader 准备、参考辅助线或 UI 的完整成本；该小场景结果不能外推为完整游戏帧率。

### 复现与证据

先按 README 准备固定工具链，并构建 `atelier_renderer_acceptance atelier_editor atelier_editor_document_acceptance`。在相应 `out/<preset>` 目录运行：

```powershell
./atelier_renderer_acceptance.exe --scene-lit --output <绝对证据目录>
./atelier_renderer_acceptance.exe --scene-lit-temporal --output <绝对证据目录>
./atelier_editor_document_acceptance.exe --scene-lit <绝对示例工程目录>
./atelier_editor.exe --acceptance-scene-lit <绝对证据目录> <绝对示例工程目录>
# 仅 Release，单独运行，测量期间不运行其他 GPU 验收：
./atelier_renderer_acceptance.exe --scene-lit-benchmark --output <绝对证据目录>
```

本机证据位于 `out/scene-lit-20261008/`：`dev-domain-run.log`、`release-final-run.log` 与对应图片；`temporal-motion-final-run.log`；`dev-product/`、`release-product/` 的原生编辑器图片和产品记录；`{dev,release}-world-lifecycle.log`；`profile-final/benchmark-{gpu.csv,summary.txt}`。最终通过的 GPU 日志未报告 D3D12 warning/error。原 RHI 与 World 补丁的双配置专项结果另见各自模块验收。

中间失败保留原日志。修复包括全屏 tone 三角剔除、运动深度精度、斜面历史重投影、静态 DDGI 噪声误判和探针采样时钟归属；验收也修正了随机光对照、被遮挡测点、同命令菜单/工具架坐标以及静态文字查询方式。两次未取得初始前台焦点的窗口运行未发送合成输入，不能记作通过。早期窗口报告曾保留过时的“displayed World simulation”说明，最终范围以上述独立 World/Renderer 验证为准。

完整文档验收从构建目录读取字体和样式；本机深层 worktree 的 Release 物理缓存临时路径达到传统 Windows 路径上限，因此使用 `E:/FrostFoxAtelier/out/sl-doc-release-1008` 短工作目录及同一部署资源通过，日志为 `release-document-full-short.log`。这不是 Scene Lit 生产代码改动，也没有宣称解决项目既有的任意长路径支持。

## 背景与参考网格（2026-10-05）

Guides 生产者提交为 `d2dafe49298b2e67617e270df788c050badc246d`。它与原生 UI 语言 M1、Session 更新及 RHI 文档在 `acae7b90784c7a1ad6a76e077a9ee5a3b6592530` 完成组合，开发/发布版 Renderer、UI、文档消费者与正式项目窗口均通过，见[组合验收](../../research/native-ui-language-20261005.md#正式组合验收)。

以下保留 Guides 独立生产者的首次验收：当时工作树为 `viewport-rendering/FrostFoxAtelier`，基线 `855433a840584b414073758d6587f5ce6ae66703`，源码指纹在 `out/reference-guides-20261005/source-manifest.json`；该基线 SHA 本身不包含 Guides 代码。

| 验证 | Dev | Release |
| --- | --- | --- |
| 正式 Renderer / editor 构建 | PASS | PASS |
| 既有 editor_document_acceptance，含 Guides 栏三档 DPI 布局 | PASS | PASS |
| renderer_acceptance：既有模式、真实 GPU 背景/网格/轴线、遮挡和深度 | PASS | PASS |
| editor_viewport_acceptance：正式原生菜单、实际画面、显示独立性和 World TRS | PASS | PASS |

GPU 专项覆盖空 World 渐变/网格、独立轴线、实体与 Visible/All 线框遮挡、共面真实地面优先、删除最后物体后深度全 1、有限奇异相机仅跳过网格、相机在平面/上下方及远处、零 viewport、非法设置编码前拒绝。新增 `viewport_draw_calls` 单独计算辅助显示，不改变原物体 draw 统计。既有 UI-only、矩阵、网格版本、图像、resize、最小化与释放检查继续通过。

正式窗口先捕获空场景，再通过真实 Guides 菜单开关网格、轴线和渐变，检查颜色变化及场景深度、geometry/material/transform、选择和 undo 不变；继续切换 MatCap/Smooth/Flat 与全部线框模式。Release 截图场景区域独立 RGB 复核，网格开关有 96,766 像素通道差异大于 3；比较区域不含显示栏文字。两配置 `out/<preset>/viewport-evidence/acceptance.txt` 均为 PASS，图片包括 `empty-reference-grid.png`、`reference-grid-{off,on}.png`、`origin-axes-off.png`、`gradient-off.png` 和既有模式/TRS 图片。

最终 GPU 日志为 `out/reference-guides-20261005/{dev,release}-renderer-coplanar.log`，窗口日志为 `{dev,release}-viewport.log`，最终日志无 D3D12 warning/error。开发过程中修正了 HLSL 保留标识符，并将空场景 shader 的深度输出移除，避免无深度附件时的调试层警告；使用相同网格算法的有/无深度变体，无新增 RHI 能力或原生图形 API。

本轮没有实现 Play 或真实场景光照，但已明确两者独立：Edit/Play 不决定 MatCap/未来 Scene Lit 的选择；背景与参考辅助显示也不绑定着色模式。用户视口偏好尚不跨进程保存，没有新增工作平面编辑、刻度文字或角落方向标。没有声称已测得 GPU 帧耗时/显存改进；未重跑无关 Project 全窗口、Unicode 剪贴板或系统对话框专项。

提交前的 Standards/Spec 双轴审查及 RHI 负责人接点检查未发现确认的生产代码结构、幂等或生命周期缺陷；修正了前代“待验/等待激活”状态、显示菜单数量与研究文档的过时链接。审查后 15 份源代码、构建定义与验收文件的 SHA256 均与上述双配置 PASS 快照一致，改动仅为文档，因此沿用原端到端证据，没有重复启动窗口。低视角/远距检查证明深度与数值边界保持，未对这些角度的视觉质量做完整量化评估。

## 视口显示增量

2026-10-04，`codex/viewport-rendering` 基于 `bba15954` 实现 MatCap、Smooth/Flat、实体叠线、隐藏线与全线框、原始多边形边/三角形边选择。前代功能最终已随 PR #11 发布到主线 `855433a840584b414073758d6587f5ce6ae66703`；实际完整窗口验收代码为 `aa9b310c619beec1cc3c21a1b000f047ce2f3233`，见[最终集成记录](../../research/editor-product-integration-20261005.md)。以下列出各项当时的真实来源，首错及中间中断证据保留在后面的历史轨迹中。

| 验证 | Dev | Release |
| --- | --- | --- |
| 正式 editor、renderer、专项验收消费者编译 | PASS | PASS |
| Project/Workspace/editor 文档端到端链 | PASS | PASS |
| World 生命周期、空间树与 StaticMeshActor | PASS（8/8） | PASS（8/8） |
| 视口显示栏 1×/1.5×/2× DPI 布局 | PASS | PASS |
| 新场景 VS/PS，DXC Shader Model 6.5 编译及 GPU 结构布局核对 | PASS（两配置共享同一 HLSL） | 同左 |
| Renderer 真实 GPU 颜色/深度与模式矩阵 | PASS（2026-10-05，5c1f21a） | PASS（2026-10-05，5c1f21a） |
| 正式编辑器原生菜单切换、MatCap/线框画面与 World TRS 投影 | PASS（aa9b310，2a7e 集成树） | PASS（aa9b310，2a7e 集成树） |

窗口 fixture 会显示/恢复窗口，编辑器消费者还会执行窗口内输入。原后台日志在视口工作树 `out/viewport-integration/`，Renderer GPU 日志在下述 `math-project-sync` 集成树；最终正式窗口证据来自应用 `2a7e` 集成树，来源及固定 SHA 详见最终集成记录，不以编译或 CPU 布局结果替代。

最初阶段同步合入 `a57cb0ca5cd15aef61e79cef42513105b1c9bd3e`，后台验证代码为 `f70d4e5e5f1059764a6ea35c6f2c1f3205a97d36`。执行 `atelier_renderer_acceptance atelier_editor editor_document_acceptance world_acceptance`：前两项仅编译，后两项编译并运行，双配置均退出 0；日志为 `stage-sync-dev.log`、`stage-sync-release.log`。该阶段未修改 RHI，后续真实 GPU 执行发现的根签名缺口由 RHI 负责人修复，见下。World 已确认边标记与资源来源键恢复兼容，没有更改 Scene 格式。

已接好的可重复入口（会显示真实窗口，需依次执行）：

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target renderer_acceptance
cmake --build --preset dev --target editor_viewport_acceptance
# release 使用相同目标，对应 --preset release。
```

Renderer 消费者检查 MatCap 对基础色的覆盖与视空间响应、Flat/顶点法线差异、多边形对角线过滤、全场景遮挡、纯线模式深度、线宽与透视距离、无效参数的编码前拒绝、异步模式切换；沿原消费者继续执行矩阵、网格版本、UI 与关停验收。正式编辑器专项仍使用 `atelier_editor`，通过真实资源浏览器创建 sphere、点击共享原生下拉菜单切换各显示模式，检查几何/材质引用、矩阵、选中对象与撤销历史保持不变，并复验 World TRS 到 GPU 的投影。专项图片将保存在 `out/<配置>/viewport-evidence/`。

该阶段只内置一张程序生成的中性陶土 MatCap；当时没有新增自定义图片导入、Ghost 淡显、AO 或 PBR，OBJ 输入仍限定三角形。2026-10-08 的导入器已支持简单多边形与材质贴图，见上方 Sponza 验收；仍不能恢复源文件未保留的多边形边。没有该历史阶段的帧耗时或显存性能测量结论。

### 2026-10-05 集成历史：首错、修复与最终收口

最初在 `math-project-sync/FrostFoxAtelier` 的固定 `495bf513c4f3cb7e7a2a0ab9506559e34ee3f3c7` 开始执行；本小节前半段的程序与日志来自该树。中间“未完成”均为当时状态，最终收口见本小节末。

首次 Dev Renderer 的纯 UI 部分通过，场景管线创建因根签名超过 64 DWORD 失败。RHI 负责人修复了各 shader stage 根常量与描述符表成本没有聚合的缺口，复用既有 CBV 常量池回退；修复提交为 `fb77b6a04063cb3e1df7f6d6e1872a9804890cc2`，直接父为上述固定候选。集成树正常快进该提交后双配置重新编译，Dev `renderer_acceptance` 完整 PASS，真实硬件为 AMD Radeon RX 6800 XT，启用 D3D12 debug/GBV。覆盖矩阵、任意网格、MatCap、线框、隐藏线、选择、线宽及资源寿命；日志未报告 D3D12 warning/error。

随后 Dev `editor_viewport_acceptance` 因前台焦点检查退出 1，未生成模式 PNG。原错误共用于初始激活失败和运行中失焦，因此该次日志不能断言用户曾切走。验收诊断提交 `e9c2210010fcf9d8f66d671e92c6898deab7132e` 仅区分初始 20 秒未获前台与运行中失焦，原输入保护不变。

用户确认时段后，重跑成功执行原生菜单并生成 MatCap、Smooth、Flat PNG，在 Flat/Smooth 画面断言失败。截图中的球体约 70 像素宽，选中边线覆盖大量表面；实际 RGB 各通道差异大于 3 的像素分别为 33、180、366，而原判据只统计红色通道，要求超过 100。提交 `5c1f21a29fd281cca6e9b87545d46e7c318dd48d` 通过既有 Focus 命令和滚轮放大验收球体，比较全部 RGB 通道，保留差异阈值与文档/深度不变检查；没有修改生产渲染行为。两配置编译通过。其后一次启动明确报告初始 20 秒未获前台，未发送合成输入；该结果与前一次画面断言失败分别记录。

同一固定 `5c1f21a` 上 Dev/Release `renderer_acceptance` 均完整 PASS，日志未报告 D3D12 warning/error；当时正式编辑器专项尚未完成，因此该阶段没有将 Renderer 结果记作编辑器输入链 PASS。

为避免手动激活窗口短暂出现即退出，随后将同一验收入口的初始等待延长到 120 秒，并显式记录 `WAITING FOR ACTIVATION` 与 `RUNNING`。等待期间继续 `app.step()` 泵消息，直接检查 Win32 当前 foreground/focus，激活后在同一进程继续；超时记录 editor/foreground/focus HWND 与 foreground PID。只尝试一次前台激活，运行中失焦保护不变。诊断改动的双配置编译日志为 `{dev,release}-activation-state-build.log`，随后由应用接手统一窗口验收。

证据目录为该集成树 `out/viewport-validation-20261005/`：首次失败 `dev-renderer-initial.log`、修复后 `dev-renderer.log`、最终双配置 `dev-renderer-final.log` / `release-renderer.log`、首次编辑器中断 `dev-viewport.log`、真实画面断言失败 `dev-viewport-retry.log` 与 `small-sphere-evidence/`、放大 fixture 后初始前台拒绝 `dev-viewport-framed.log`，以及两配置重建 `{dev,release}-build-rhi-fix.log` 和 `{dev,release}-activation-diagnostic-build.log`。RHI 自有树的双配置离屏回归是底层补丁证据，不代替本集成树的窗口结果。旧 Project 普通窗口和 Unicode 剪贴板问题未在本专项修复或验收。

最终应用 `2a7e` 树在 `aa9b310` 上完成 Dev/Release 统一 `--acceptance-project` 全链 PASS，包含同一 `verify_viewport_display` 与 World TRS。Project 叠线判据改为比较未选中网格，避免把选中对象 Off/Overlay 原本相同的轮廓误判为缺陷；两配置观察到 1,126 个 RGB 变化像素。最终文档提交 `1d79598` 没有生产代码变化，PR #11 已发布到 `855433a`。证据路径和仍未覆盖的 Unicode/普通系统对话框边界见[正式集成收口](../../research/editor-product-integration-20261005.md)。

## 历史验收

2026-09-28，Windows x64、AMD Radeon RX 6800 XT，dev 与 release 的正式 `renderer_acceptance` 均通过。fixture 使用真实 HWND、正式 Renderer、D3D12 debug layer 与 GPU validation；不使用 mock。最终程序输出必须包含 `Renderer acceptance: PASS` 且退出 0，runner 保留日志并限制进程为 120 秒。

## 可重复入口

应用根构建先创建 RHI 与 `atelier_ui_protocol`，再 include `cmake/renderer.cmake` 并调用 `atelier_add_renderer()`。然后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target renderer_acceptance
cmake --preset release
cmake --build --preset release --target renderer_acceptance
```

本次独立 RHI worktree 未修改由应用拥有的根 CMake；通过忽略目录 `out/renderer-development.cmake` 的 CMake DEFER 在既有根构建末尾调用同一 `atelier_add_renderer()`，用 `CMAKE_PROJECT_FrostFoxAtelier_INCLUDE` 注入。没有复制生产实现、建立另一个产品入口或引用其他 worktree；应用集成后使用上面的普通 presets 命令。

## 已核对的结果

| 行为 | 真实观察 |
| --- | --- |
| 窗口格式 | renderer 显式请求 RGBA8，打开时检查实际 RTV 为 `R8G8B8A8_UNORM_SRGB`；当前 10 位显示器环境下打开成功 |
| 颜色与独立深度同时绑定 | 两个相同屏幕位置的实例，先画近红块再画远蓝块；中心颜色保持红，深度为 0.15，空白深度为 1 |
| 相机矩阵 | 非对角 view 平移与左手透视矩阵，中心读回深度符合 `10/9.9 - 1/(9.9*3.5)` |
| 选中与 UI 覆盖 | 选中后颜色变为金色；半透明 UI 覆盖场景，场景深度不变 |
| 线性颜色与混合 | 绿色上覆盖半透明红色，sRGB 读回 R/G 约为 188；半开 scissor 外保持背景 |
| R8 内容 | CPU 行距 5、有效宽度 3，验证 padding 没进入纹理；白色覆盖与透明部分可区分 |
| 内容版本与退休 | 相同 identity 的新 revision 改变像素；12 帧普通异步绘制后旧 CPU owner 已释放，新像素正确 |
| 任意窗口宽度 | 641×385 与 703×427 的 RGBA8、float32 深度经对齐 GPU 行距读回为紧密 CPU 行，指定像素均正确 |
| resize 接受 | resize 当帧 accepted/capture 仍为 641×385，next extent 为 703×427；旧 extent 再提交 skipped，新帧重建深度后 accepted |
| 窗口生命周期 | 最小化与隐藏均 skipped；恢复后继续 accepted；关闭 renderer 后才销毁 HWND，重复 close 成功 |
| 输入与 owner | 非有限矩阵在编码前拒绝且可恢复；异线程 target 查询拒绝 |
| 调试层 | renderer 检查编码/完成/关停后的 InfoQueue；本次两种配置日志无 D3D12 warning/error |

证据在各配置 `out/<preset>/renderer-acceptance.log` 与 `renderer-acceptance.ppm`；这些本机文件不随仓库提交。窗口是端到端验收 fixture，正式产品窗口由 application 唯一拥有。fixture 没有文字排版与用户控件；真实文字、Reset Camera、场景对象操作、撤销/取消和 DPI 输入路由由应用与 UI 的共同验收证明。

## S1 诊断计数增量

从 `fcb9ddf` 增加 `FrameResult::stats` 后，dev/release 分别再次运行现有 `renderer_acceptance`，均 PASS。只扩展同一真实窗口消费者的检查，没有另建测试或 benchmark 工程，没有重跑无改动的完整 RHI/Runtime/JIT 矩阵。

- 首帧 3 次 UI draw、1 次包含两个方块的 scene draw；一次覆盖图上传为 6 个紧密 R8 字节，缓存为 1 项/6 个 texel 字节/10 个含行 padding 的 CPU 字节。
- 仅改变选中状态时，绘制数保持，上传量为 0；同 identity 的新 revision 只在首帧上传，旧版本退出缓存后 CPU 字节由 10 变为 6。连续 12 帧计数未累计漂移。
- 非空输入数组中额外加入空 clip 图元，实际 UI draw 与上传数不增加，未使用的覆盖图不进入 GPU 缓存。
- extent 不匹配的 skipped 帧活动计数全为 0，当前缓存量保留；删除最后一个可见覆盖图并清空场景后，缓存三项归零、scene draw 为 0。
- 原有颜色/深度、矩阵、UI 混合、resize、最小化恢复和关停检查仍通过。原生遮挡分类未另行强制制造，旧的驱动故障与硬件证据边界保持不变。

该结果证明计数对应已编码操作和现有缓存持有量，不是 256 对象的性能结论；真实交互 prepare/render 耗时与上传分布由应用侧在同一正式入口采样。日志为本机 `out/<preset>/renderer-stats-run.log` 和正式 runner 生成的 `renderer-acceptance.log`。

## 已有 RHI 回归

同一 gfx 补丁在 dev/release 分别运行 `rhi_acceptance`，两种配置均通过：Compute/Mesh/Vertex 场景颜色 24,576 像素、深度 12,288 像素、90 个参数字匹配；管线准备失败、资源使用期、owner/关停、null-list、mesh-pixel-state 和真实 timeout 矩阵通过。timeout 检查继续报告 `poisoned-admission=PASS drain-only=PASS increments=1`。

对应日志为 `out/<preset>/rhi-acceptance.log`、`rhi-regression-*.log`。没有重复运行无改动的 Runtime/JIT 矩阵。

## 未覆盖

没有动态注入 Present 后的 ResizeBuffers、Signal、Reset、device removal 或 native OOM 故障；该处失败继续沿原 RHI 终态返回，代码仅在整个 frame 成功后接受。原 timeout 证明不能替代这些驱动故障的证明。未强制制造 `DXGI_STATUS_OCCLUDED`；其分类有实现，隐藏/最小化是编码前可见性检查，不能记作 native occlusion 的验收。

没有扩展到其他显卡/驱动、不支持 Mesh 的硬件、HDR、多个窗口、并行录制或设备恢复。真实产品交互的完成以应用侧共同验收为准。
