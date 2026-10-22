# Renderer 验收记录

本页记录真实执行过的验收，不以文档更新代替新测量。当前行为和参数见[显示合同](contract.md)与 [Scene Lit](scene-lit.md)。每批结果只对应其注明的源码、配置和场景；早期帧时、前向深度或选中染色行为不代表当前实现。

## 近期证据索引

| 范围 | 实际覆盖与记录 |
|---|---|
| D1 2.1–2.3 审查修复 | 2026-10-22 rendering-d1-compose：Oracle 审查 2.1–2.3 的修复。删除无读者的 `history_base_metallic`；零辐亮度光不请求 face；延后的光保留绘制原点所需的 face，换原点时作废未一起重绘的旧 face；无键光不受预算；掠射接收面改用自身平面斜率并把比较深度钳在 0。Release 全部入口通过；九个变异都失败，见[本批记录](#d1-21-23-审查修复2026-10-22) |
| D1 2.3 阴影层级上限与重绘预算 | 2026-10-22 rendering-d1-compose：只有重要性前 16 的点光保留直径层级；每帧最多重绘 24 个 face，按整盏打包、记录绘制原点；face 没画齐的光不贡献直接光，画齐后 8 帧淡入。Release 默认与 `--scene-lit`（GBV）及其余 Scene Lit、Control 入口通过；五个变异都失败，见[本批记录](#d1-23-阴影层级上限与重绘预算2026-10-22) |
| D1 2.2 阴影过滤与法线偏移 | 2026-10-22 rendering-d1-compose：PCSS 换成 GatherCmp 3×3 帐篷和一个纹素的法线偏移，不再读 `shadow_bias`；新增 70° 掠射、近距遮挡的真值用例。Release 默认与 `--scene-lit`（GBV）及其余 Scene Lit、Control 入口通过；法线偏移 ×10、偏移置零、去掉接收平面梯度、探针命中不计 emission 四个变异都失败，见[本批记录](#d1-22-阴影过滤与法线偏移2026-10-22) |
| D1 2.1 融合 compose | 2026-10-22 rendering-d1-compose，基线 `691ae0ea`：cluster 列表确定性求值加单批阴影 atlas 取代 ReSTIR DI、A 路径和重建链；网格发光只经 DDGI 探针命中。Release 默认与 `--scene-lit`（GBV）及 shadows、quality、stability、clustered、cost、temporal、Control、编辑器文档入口通过；shadows 两条变异失败，法线偏移 ×10 未被发现，见[本批记录](#d1-21-融合-compose2026-10-22) |
| 显示抖动 | 2026-10-21 rendering 工作树，基线 `a743ac13`：Lit 显示 pass 加静态 IGN 抖动，room 的 moving−settled 差异图不再成环；必要的 6 个 Release 入口在调试层下通过；Composite p50 0.423→0.424 ms，在噪声范围内，见[本批记录](#显示抖动2026-10-21) |
| 异步准备与 GBV 预热 | 2026-10-19 rendering 工作树，基线 `80de8156`：Scene Lit 管线按组在 RHI 工作线程上准备，首帧 2.0 ms 返回 not_ready；GBV 下基础组 141 s 就绪。Release 全部 10 个入口通过；与 ec-3 门禁相比，9 个 `--scene-lit*` 入口的整进程时长合计从 59.7 min 降到 34.9 min，见[本批记录](#异步准备与-gbv-预热2026-10-19) |
| 默认直接光切到 A | 2026-10-18 rendering 工作树：`direct_clustered` 默认 true，ReSTIR 保留为诊断回退；切换前 Release 九个入口通过，Edit/Play 收敛 1.228/4，Play 宿主走 A；合并后 ec-3 门禁（`5911be69`）在 Dev 和 Release 下跑完全部入口并通过，见[本批记录](#默认直接光切到-a2026-10-18) |
| Renderer 验收入口盘点 | 2026-10-18 rendering 工作树：此前 `renderer_acceptance` 只运行无参数默认入口，14 个 `--scene-lit*` 入口都不在任何常规目标里；现在该目标依次运行默认入口和 9 个有独有检查的入口，删除 9 个完全被覆盖的子集入口，见[本批记录](#renderer-验收入口盘点2026-10-18) |
| temporal isolation 参考配置 | 2026-10-18 rendering 工作树：`--scene-lit-temporal` 自 10-13 起一直失败（main 基线 `d2b6da2b` 同样失败），原因是参考配置从确定性 64 阴影样本换成逐帧独立的随机参考；改为比较两段 16 帧均值后为 1.032/3，通过，见[本批记录](#temporal-isolation-参考配置2026-10-18) |
| DI 辐射响应窗口 | 2026-10-18 rendering 工作树：DI 降噪器按距最后一次辐射变化的帧数缩短支持，编辑后恢复的残留在 64 帧内回到基线（A 路径 R−B 从 +.70 降到 +.04）；持续动画光源的半影噪声不变；去掉窗口的变体使新验收失败，见[本批记录](#di-辐射响应窗口2026-10-18) |
| Edit/Play 收敛修复 | 2026-10-18 rendering 工作树：DDGI 命中点改为完全粗糙求值；`--scene-lit` 的 Edit/Play 收敛检查从 mean 4.212、p95 14 降到 2.659、8，通过；随后改为比较两域各 64 帧的平均，为 2.224、7，见[本批记录](#editplay-收敛修复2026-10-18) |
| Lit 帧率批次 | 2026-10-18 rendering 工作树：原生 1440p 静止 A 路径 61.50 接受帧/秒（GPU 15.88 ms），默认 ReSTIR 44.70；相机持续环绕 53.77。半分辨率 DDGI、GTAO 深度层级、A 路径半影分类；Edit/Play 收敛检查在基线上同样失败，见[本批记录](#lit-帧率批次2026-10-18) |
| 基础链路与精确工作复用 | 2026-10-17：CPU、三项正式 GPU 专项与地牢原生 1440p 采样通过；默认 Lit 40.81、A 路径 46.32 接受帧/秒，静止 DDGI gather 约 2.48 ms；正常停靠布局仍有 CPU 瓶颈，见[本批记录](#基础链路与精确工作复用2026-10-17) |
| 编辑器整帧诊断 | 2026-10-16：空闲、无 VSync、原生 1440p 最大化视口的 App/UI 对照已完成，非 Lit 提速且画面一致；关闭 DDGI 的默认 Lit 约 45 接受帧/秒。正常停靠布局与 RHI 候选性能仍待验，见[本批记录](#编辑器整帧诊断2026-10-16) |
| 精确局部光照与缓存阴影候选 | 2026-10-16 主目录工作区：小 ROI 通过；原生 1440p 实测 A 精确枚举＋RT 的 DI 下降约 20.5%，B 缓存阴影成本及真实地砖质量未过默认迁移门槛，见[本批记录](#精确局部光照与缓存阴影候选2026-10-16) |
| DI 重建缓存与目标复用 | 2026-10-15 后续：分项计时、step 1 协作缓存、同帧 target 复用、显式探针端点归因，见[本批记录](#di-重建缓存与目标复用2026-10-15) |
| DI 精确优化与共享空间提议 | 2026-10-15 主目录：双配置构建、地牢 Control A/B、Dev 稳定性、Release ReSTIR／质量／合同通过，见[本批记录](#di-精确优化与共享空间提议2026-10-15) |
| DDGI 访问采样与有效响应 | 2026-10-15 主目录第二批：Dev/Release响应阶段、hold进度、1～256射线与原质量/滚动验收通过，见[本批采样与响应](#ddgi-访问采样与有效响应2026-10-15) |
| DDGI 运输/呈现分离 | 2026-10-15 主目录：Dev/Release 异步 E/w/transport 与时间生命周期通过；Release 隔墙、发光面、多体积及滚动回归通过，见[本批稳定性](#ddgi-运输与呈现分离2026-10-15) |
| 多 Astra 审查与工作剔除 | 2026-10-13～14 主目录：历史身份、DDGI 响应、ROI、目标/深度副本、编辑绘制和数值域；双配置构建与 World 数据链、Dev GPU 合同、Release 光照组合及 Renderer 全套通过，空闲 GPU 对照已完成，见[本轮审计](#多-astra-审查与工作剔除2026-10-13) |
| Scene Lit Pass 消耗统计 | 2026-10-13 主目录：16 个同帧计时区间、完成样本启用掩码、双配置构建、Dev GPU 合同与两次正常配置成本采样，见[分项统计](#scene-lit-pass-消耗统计2026-10-13) |
| 原生 ReSTIR DI 与 DDGI 线性能量 | 2026-10-12～13 viewport-rendering 工作树：产品迁移、Dev/Release 完整 GPU 组合与 1440p 成本采样通过，见[本批合同与证据](#原生-restir-di-与-ddgi-线性能量2026-10-13) |
| DDGI 多体积与反馈 | 2026-10-12 主目录工作树：Dev/Release 联合 GPU 回归、双配置编辑器/Control 与地牢同预算布局对照，见[覆盖演进](#ddgi-覆盖与反馈演进2026-10-12) |
| 选中边线深度一致性 | `72843f7` 主目录工作树：Dev 专项与 Release 全套 GPU 验收、真实 Dungeons 桶模型，见[深度一致性](#选中边线深度一致性2026-10-11) |
| DDGI 诊断 | 2026-10-11 工作树：真实探针/Volume、八邻接收覆盖、年龄、深度与历史隔离，见[DDGI 诊断](#ddgi-诊断2026-10-11) |
| 工作室 Cavity | 2026-10-11 工作树：屏幕凹凸增强、显示隔离与生命周期，见[工作室 Cavity](#工作室-cavity2026-10-11) |
| 标准材质 | 2026-10-11 工作树：Dev/Release 材质与原 Renderer 全套 GPU 验收，见[标准材质](#标准材质2026-10-11) |
| 选中边线 | `a7ab99f`：Dev／Release 原生 Renderer 裁剪矩阵；另有独立 Release 官方 Sponza 编辑器集成捕获，见[选中修复](#跨相机平面的选中边线铺黄2026-10-10) |
| 材质／设置变更和无效工作 | `ca4af33`：Dev 时间响应／contracts、Release 质量矩阵、双配置场景文档链，见[Review 补强](#提交后-review-补强2026-10-10) |
| 移动物体与 T2x | `c8293e9`：移动阴影、反射、Local GI、无关运动与 T2x 来源稳定性，见[移动历史](#smaa-下移动物体的光照拖尾2026-10-10)和 [T2x 对照](#smaa-t2x-静态抖动与反射对照2026-10-10) |
| 发光面／DDGI | 发光主动采样、成本拆分、墙角／薄隔墙与大场景接入，见[主动采样](#发光面主动采样与-ddgi-重建成本2026-10-10)和 [Sponza DDGI](#sponza-ddgi-场景接入2026-10-10) |

这些入口验证的范围不同，不合并称为“所有场景已经验收”。本机 `out/` 证据不随源码提交；不同工作树的来源在对应段落标明。当前重复运行命令沿 README 与各专项入口，不使用旧交接文档中的临时头、补丁注入或待验安排。

## D1 2.1–2.3 审查修复（2026-10-22）

状态：rendering-d1-compose 分支，接在 2.3 `f03776ec` 之后，本地提交，未推送。行为见 [Scene Lit](scene-lit.md#直接光与阴影-atlas)，无键光的预算例外写进了[显示合同](contract.md)。

Release 入口，RX 6800 XT，一个入口一个进程，退出码均为 0：默认入口（GBV）103 s，`--scene-lit`（GBV）104 s；shadows、quality、stability、clustered、cost、temporal（debug layer）13 / 16 / 22 / 4 / 7 / 12 s；编辑器 Control 全套与 `--repro-lighting` 81 / 52 s。Dev 也构建通过。

新用例：

- 30 盏无键灯：每帧全部重绘，不受预算，画面与关阴影时逐通道差 ≤1，本影仍暗。
- 绘制原点滞后：30 盏只需 +Z 的小灯每帧移动 1 mm 占满预算；探针灯 P 每 3 帧在 P0、P1 之间来回，接收面在 P0 处跨 +X/+Z 对角线、在 P1 处只需 +Z；探针灯 Q 同时右移 1.2 m 并把 range 从 1 缩到 0.7，接收面 R2 只在移动后进入范围，从 Q0 看在 +X。两盏都排在最后，移动的那一帧被延后。36 帧里每个完成样本 missing_taps 都为 0。
- 掠射平面：点光离未遮挡平面 13 mm、4 mm，横向 1 m，N·L 约 0.013、0.004，低于 128 tile 的偏移比例 0.0161（用透视相机，断言 atlas_cells==faces，确认是 128 tile）。阴影开、关两帧的读数都是 173.667 / 175.222。

变异（各自单独构建、运行后撤回）：

| 变异 | 结果 |
|---|---|
| 不并入绘制原点需要的 face | 原点滞后用例 missing_taps 407 |
| 绘制原点的候选接收面按旧原点的支持范围筛 | 原点滞后用例 missing_taps 121 |
| 换原点时不作废未一起重绘的 face | 原点滞后用例 missing_taps 121 |
| 无键光也受预算 | 30 盏无键灯在 ⌈faces/19⌉+8 帧内没有全部画齐 |
| 比较深度改回偏移后平面的斜率 | 13 mm 掠射平面 136.8，关阴影为 173.7 |
| 比较深度不钳在 0 | 13 mm 掠射平面 167.2，关阴影为 173.7 |
| 法线偏移 ×10 / 置零 / 去掉接收平面梯度 | 点光真值分别 202 / 3996 / 192 处失败，和 2.2 时相同 |

## D1 2.3 阴影层级上限与重绘预算（2026-10-22）

状态：rendering-d1-compose 分支，接在 2.2 `cf9cd1d0` 之后，本地提交，未推送。行为见 [Scene Lit](scene-lit.md#直接光与阴影-atlas)。FrameStats 和 Control 的 `shadow_cache` 新增 `deferred_faces`、`deferral_frames`、`atlas_cells`、`pending_lights`。

Release 入口，RX 6800 XT，一个入口一个进程，最终二进制上退出码均为 0：默认入口（GBV）83 s，`--scene-lit`（GBV）95 s；shadows、quality、stability、clustered、cost、temporal（debug layer）7 / 16 / 17 / 4 / 8 / 12 s；编辑器 Control 全套与 `--repro-lighting` 68 / 51 s。Dev 也构建通过。

首轮运行发现两处问题，都改实现，没有放宽判据：

- 预算轮转用例里灯每帧移动 1 mm，被延后的 face 沿用旧位置的投影，取样却按灯的当前位置选 face、做投影，cube 接缝附近的接收点落到旧 face 外，`missing_taps` 489。改为一盏光需要重绘的 face 整盏一起画，GPU 光源记录带上这些 face 的绘制原点，取样按它选 face、投影。
- light_set 的 272 盏灯里有位置、颜色都相同的灯，atlas 放不下时按重要性加下标决定谁驻留。光源顺序反转后换成另一批同重要性的灯驻留，在预算下要几帧才画齐，"反转顺序画面完全不变"失败。改为同重要性时优先保留已有 face 的灯。

新用例的读数（`shadow-cache-completed.csv`）：

| 用例 | 结果 |
|---|---|
| 16 盏灯 | 80 个 face 全为 256 层级，atlas_cells 320 |
| 增加到 20 盏 | 新增 20 个 128 face，原 80 个 face 复用不重画，atlas_cells 340 |
| 20 盏灯每帧移动 1 mm（每盏 5 face） | 每帧整盏画 4 盏（20 个 face），延后 80；最长等待 4 帧（一轮 5 帧）；missing_taps 0。停下后延后数 60→40→20→0 |
| 一次新增 30 盏投影点光（120 face） | 每帧 24 个 face，pending_lights 24→18→12→6→0；全过程本影均值 0，画齐后受光 90.3，未遮挡区与关阴影画面逐通道差 ≤1 |
| 272 盏重合灯（atlas 压力） | 放弃 102 盏，驻留 1020 个 face；首帧只画 24 个，166 盏 pending；之后在上限内画齐 |

`Fixture::settle_shadows` 断言 pending_lights 在 ⌈faces/19⌉+8 帧内归零：只要有光被延后，本帧至少已画 25 − k 个 face（k ≤ 6），且有未画 face 的光排在前面。light_set、clustered 的阴影用例和 272 灯压力用例都用它等待，判据不变。

变异（各自单独构建；跑的是同重要性驻留优先修正之前的二进制，该修正只影响同重要性光的驻留顺序，不涉及下列检查）：

| 变异 | 结果 |
|---|---|
| 所有点光都保留直径层级 | 失败于层级用例：atlas_cells 400，应为 340 |
| 排队去掉等待帧数，只按重要性 | 失败于预算轮转：最长等待 5 帧，达到一整轮 |
| 忽略预算 | 失败于预算轮转：一帧重画 100 个 face |
| face 没画齐的光照常着色 | 失败于 30 盏新增：pending 期间本影均值 134.5，画齐后为 0 |
| 取样按光源当前位置选 face、投影 | 失败于预算轮转：missing_taps 489 |

## D1 2.2 阴影过滤与法线偏移（2026-10-22）

状态：rendering-d1-compose 分支，接在 2.1 `8d4335ce` 之后，本地提交，未推送。行为见 [Scene Lit](scene-lit.md#直接光与阴影-atlas)。FrameStats 和 Control 的 `shadow_cache` 删除 `clamped_taps`。

Release 入口，RX 6800 XT，一个入口一个进程，退出码均为 0：默认入口（GBV）103 s，`--scene-lit`（GBV）123 s；shadows、quality、stability、clustered、cost、temporal（debug layer）13 / 31 / 33 / 8 / 14 / 20 s；编辑器 Control 全套与 `--repro-lighting` 81 / 68 s。Dev 也构建通过。

新用例 `shadow-grazing-contact`：阳光与接收面法线成 70°，4 cm 厚的鳍离接收面 1 cm，全分辨率视口，真值 margin 1.5 cm。判定 118260 个遮挡像素、277020 个受光像素，0 处失败。鳍原先只有 2 cm 厚：半厚小于 margin，收缩后的盒子为空，遮挡像素为 0，这一点在首轮运行中发现并修正。

变异（各自单独构建、运行后撤回）：

| 变异 | 结果 |
|---|---|
| 法线偏移 ×10（点光与太阳） | 失败于第一个点光真值，202 处 |
| 只对太阳做法线偏移 ×10 | CSM 各用例照常通过；`shadow-grazing-contact` 失败，27540 个应遮挡像素被照亮（最亮 105） |
| 法线偏移置零 | 点光真值出现自遮挡，3996 处，最大差 40 |
| 去掉接收平面梯度（各纹素都比较投影点深度） | 点光真值失败，192 处，最大差 5 |
| 比较深度不加半纹素斜率 | 全部通过 |

最后一条说明这个斜率偏置是多余的：一个纹素的法线偏移在入射角 θ 下提供 w/cosθ 的深度余量，块中心比较最多需要 0.71·w·tanθ（w 为纹素宽度），前者总是更大。因此删掉了这个偏置，块中心的平面深度加法线偏移就够了，法线偏移和平面梯度各由上面的变异证明必要。

材质传输的"离屏"发光面实际不在屏外：投影的垂直半视场正切为 0.556，发光面远边在 0.486，截图顶部能看到被裁掉的一块。2.1 删掉主可见性里那条"视口外发光体"检查时，依赖的正是这条用例，所以把发光面移到 z −2…2，远边的正切为 0.61，并新增几何前提检查（四个角都在视锥上方）。探针命中不计 emission 的变异下，该用例失败于 "offscreen textured emission and content-only replacement illuminate real receivers"；`--scene-lit-quality` 的网格发光用例同样失败（接收面 red=0）。

## D1 2.1 融合 compose（2026-10-22）

状态：rendering-d1-compose 分支，基线 integration/ec-7 `691ae0ea`，本地提交，未推送；D1 的 2.1–2.5 一起交付。行为见 [Scene Lit](scene-lit.md)。Control 的 view 键升到 schema 4，删除的键返回 invalid_request，见[编辑器 Control](../editor/control.md)。

Release 入口，RX 6800 XT，一个入口一个进程，退出码均为 0：

| 入口 | 验证 | 时长 |
|---|---|---|
| 无参数默认入口 | GBV | 185 s |
| `--scene-lit` | GBV | 259 s |
| `--scene-lit-shadows` | debug layer | 8 s |
| `--scene-lit-quality` | debug layer | 25 s |
| `--scene-lit-stability` | debug layer | 32 s |
| `--scene-lit-clustered` | debug layer | 7 s |
| `--scene-lit-cost` | debug layer | 10 s |
| `--scene-lit-temporal` | debug layer | 14 s（刚性停止的运动向量 2835 对 0） |
| 编辑器 Control 全套、`--repro-lighting`、场景文档、viewport 编辑 | debug layer | 73 / 58 / 48 / 8 s |

temporal、cost 和 `--repro-lighting` 跑的是最后一处渲染修正之前的二进制。那处修正只把"没有投射阴影的光源"时的阴影参数改成绑定空句柄：GPU 看到的同样是空描述符，只是不再触发 gfx 的失效句柄报错，所以没有重跑。Dev 也构建通过。`--acceptance-surfaces` 失败（"Asset directory splitter did not preview shrinking"），不含本批改动的构建同样失败；修复在 ec-7 的 `2c0eb730` 里，变基后自然解决。需要焦点的 `--acceptance-scene-lit` 编辑器入口还没跑，留到桌面空闲时段。

改写的测试场景及理由：

- CSM 真值：阳光原来向 x 偏，13 号接收列的影子真实地落在 14 号列左缘；真值模型只把阳光遮挡物当作遮挡体，于是报了 589 个"未遮挡却变暗"的像素。改成只向 y 偏（`(0,.2,1)`，移动后 `(.03,.2,1)`），接收列之间不再互相遮挡。真值 margin 从 .04 改为 .03：.04 正好等于遮挡物的 z 半厚，收缩后盒子为空，occluded 恒为 0。末级 cascade 的探测点移到正交相机下影子的实际位置 `{2.24,.24,2}`。
- 网格发光经 DDGI：原探针体积的 z 上限 3.75 不含 z=4 的接收面，体积外的 gather 返回 0。扩到 4.6、7 层、预算 441。
- 可见性：删掉"视口外发光体使 `scene_lit_light_sources>0`"。网格发光不再生成光源；视口外发光体照亮接收面由材质传输的"offscreen textured emission"覆盖。
- 材质传输：单面背面、双面、Mask 三次采集前各换一次 `scene_identity`，因为探针历史会保留上一个发光体很多帧。
- 271 盏点光的 atlas 压力：原平面 AABB 加 PCSS 切向扩张后每盏光要约 4 个面，271×4 超过 1024 格。平面缩到 `1/6×.25`，遮挡物移到每盏光只需 +Z/+X/−Y 三个面的位置。

`--scene-lit-shadows` 变异（各自单独构建、运行后撤回）：

- compose 忽略全部阴影：失败于 "off-camera-ray caster produces an actual dark receiver shadow"。
- 只让方向光忽略阴影：失败于 shadow-csm-truth，6 个应遮挡像素全亮（178）。
- 阴影采样的法线偏移 ×10（0.02 m）：未被发现。推断：接收面几乎正对阳光，影子横向只移约 4 mm，小于真值的 0.03 m 边带；点光用例的距离同样让这个偏移看不出来。CSM 真值只有 6 个遮挡像素，只能发现整片阴影丢失。2.2 补了掠射角、近距遮挡用例，同一变异在该用例上失败，见 [2.2 记录](#d1-22-阴影过滤与法线偏移2026-10-22)；2.5 放大 CSM 遮挡物。

## 显示抖动（2026-10-21）

状态：rendering-dither 分支，基线 `a743ac13`，本地提交，未推送。只改 `lit_post.hlsl` 的 `lit_display_ps`，行为见 [Scene Lit](scene-lit.md) 第 8 步；工作室着色、Frame/FrameStats 接口和 GPU scope 都没有变化。

moving−settled 对照（实测，Release，空闲机器，`out/dither-20261019/{base,dither}-{room,sponza}`）：两个编辑器从同一棵源码构建，只差 `lit_post.hlsl`，沿 10-18 环纹记录的同一相机移动再静止。room 的后墙与地面在基线差异图里是同心环和细线，加抖动后成为无结构的细颗粒；金属箱子上的运动滞后差值两者相同，与显示编码无关。Sponza 两者都看不出环纹，纹理本身已经打散等照度线。量化统计只取两帧相差不超过 1 级的像素（room 90%、Sponza 91%），排除反射滞后：

| 场景 | 变体 | ±1 翻转占比 | 右邻/下邻同翻转倍率 | 16 px 块均值标准差 |
|---|---|---|---|---|
| room | 基线 | 49.9% | 1.83 / 1.87 | .286 |
| room | 抖动 | 50.6% | 1.62 / 1.64 | .273 |
| Sponza | 基线 | 11.0% | 4.77 / 4.97 | .100 |
| Sponza | 抖动 | 11.2% | 4.58 / 4.80 | .100 |

翻转占比和低频功率基本不变，说明亚级运动过渡本身仍在；下降的是相邻翻转的相关性，也就是成环的结构。

Renderer 入口：Release、`ATELIER_GPU_VALIDATION=debug`，只跑断言容差不超过 1 级或要求逐字节/精确为零的入口：默认、`--scene-lit`（含天光移除 ≤1 和精确零）、`--scene-lit-quality`（SSIL p95≤1）、`--scene-lit-cost`（mean<.05/.01、p95≤1）、`--scene-lit-clustered`（删灯后精确零）、`--scene-lit-benchmark`，全部通过。其余入口的阈值在 3 级以上，抖动最多改变 1 级，不跑。与 `9a451cc5` 的同入口输出比较，112 条指标 76 条不变，36 条只有小幅变化；未断言的 “Inactive receiver realtime” 均值从 0 变成 .0048，推断是抖动让原本低于半级的 DI 尾部显出 1 级。

成本：同一会话、空闲机器（CPU 8–15%）各跑两次 benchmark，Composite scope p50 基线 .4230/.4230、抖动 .4241/.4227 ms，没有可测的变化。p95 基线 .553/.558、抖动 .614/.715；Composite 本身呈双峰（240 个样本里超过 .5 ms 的慢峰，基线 53/38 个、抖动 61/59 个），两次运行不足以判断尾部差别是否来自抖动。

## 异步准备与 GBV 预热（2026-10-19）

状态：rendering 分支，基线 integration/ec-4 `80de8156`，本地提交，未推送。合同见 [Renderer 合同](contract.md#场景和-ui) 与 [RHI 异步准备](../rhi/contract.md#异步准备与-dxil-磁盘缓存)。编辑器的 Smooth 回退、进度显示和验收中的就绪等待由编辑器负责人另行提交，两者同轮集成；单独合入本提交时，编辑器窗口验收切到 Lit 后会超时。

改动：Scene Lit 的 50 条管线分为基础组（38 条）和 7 个按设置启用的组，在 RHI Context 的工作线程上编译、创建 PSO，并在 GBV 下预热。`render` 遇到所需组未就绪的 Lit 帧返回 not_ready，不录制、不 Present。draw 内的懒建管线全部去掉。Renderer 验收中除契约用例外，所有 render 调用先等该帧视图所需的组就绪，因此使用 ReSTIR、B 缓存阴影、inspect、DDGI 诊断的入口各自等到自己的组。

Release `renderer_acceptance` 的 10 个入口，RX 6800 XT，验证方式与以前相同（benchmark 不开验证，stability 只开 debug layer，其余为 GPU-based validation），每个入口一个进程，退出码均为 0，证据在 `out/release/renderer-evidence/<入口>/`。

默认入口新增的契约用例（GBV）：

- 首次 render 返回 not_ready，用时 2.0 ms，活动计数为 0，capture 未被改写；此时进度 0/38。
- 准备期间同一窗口的 Smooth 帧照常 accepted。
- 基础组 141.1 s 后就绪（10-19 预热试验为 134.4 s），之后 Lit 帧 accepted。
- 打开 `probe_debug` 后进度变为 38/39，render 重新返回 not_ready；新组就绪后 accepted。

整进程时长（实测；本批为取锁到进程退出，含约 1 s 的启动开销；ec-3 为同一台机器上该门禁的逐进程记录，基线 `5911be69`）：

| 入口 | 验证 | ec-3 门禁 Release | 本批 Release |
|---|---|---|---|
| 无参数默认入口 | GBV | 未单独计时 | 183 s |
| `--scene-lit` | GBV | 565 s | 270 s |
| `--scene-lit-benchmark` | 无 | 11 s | 5 s |
| `--scene-lit-temporal` | GBV | 516 s | 270 s |
| `--scene-lit-quality` | GBV | 641 s | 351 s |
| `--scene-lit-stability` | debug layer | 47 s | 37 s |
| `--scene-lit-restir-suite` | GBV | 667 s | 353 s |
| `--scene-lit-clustered` | GBV | 297 s | 220 s |
| `--scene-lit-shadows` | GBV | 347 s | 249 s |
| `--scene-lit-cost` | GBV | 492 s | 341 s |
| 9 个 `--scene-lit*` 合计 | | 3583 s（59.7 min） | 2096 s（34.9 min） |

推断（未单独拆分测量）：

- 节省主要来自 GBV 首次使用补丁从所属线程的第一次录制移到 8 个工作线程上并行完成（单进程约 297 s → 约 135～141 s），其次是 DXC 并行编译；不开验证的 benchmark 少的 6 s 与 10-19 离线测得的 DXC 串行 7 s→8 线程约 1.8 s 一致。
- 按需分组让默认 A 路径不再为 ReSTIR、B 缓存阴影和诊断管线付预热成本；用到它们的入口在第一次打开设置时另付该组的成本。
- 验收没有设置 `shader_cache`，每个进程都重新编译；缓存对门禁的收益约为每进程 1～2 s，不影响上述结论。

未覆盖：Dev 的 rhi/renderer 由下一轮集成门禁带 Dev 窗口层运行；编辑器窗口验收和 Control 的预热前后对比等编辑器改动合入后，在正常窗口时段测量。

## 默认直接光切到 A（2026-10-18）

状态：基于 `f01139f1` 的单独本地提交，未推送、未并入 main。C 批第一步：`direct_clustered` 默认改为 true，ReSTIR 保留为诊断回退；回退只需把 `view_settings.hpp` 中这一项改回 `{}`。没有持久数据：该开关不在作者选项目录里，不写 Scene 或 Undo。Play 宿主（`project_runner.cpp`）默认构造 ViewSettings，也随之走 A。Frame/FrameStats 接口和 GPU scope 没有变化。

改动：

- `view_settings.hpp` 的默认值。
- 断言 ReSTIR 行为的 renderer 验收显式设 `direct_clustered=false`：`--scene-lit-restir-suite` 的两个 ReSTIR 夹具（reservoir 统计与 MIS）、`quality_work_elision`（reservoir scope 与每接收点一条选中可见性射线）、`--scene-lit-benchmark`（CSV 要求所有 reservoir 子 scope 都执行）。其余验收不设置，即运行在 A 上。
- 编辑器 `view_options.cpp` 的说明与编辑器文档（编辑器负责人审阅），renderer contract、Scene Lit 与 README。编辑器 Performance 基线仍显式 false，之后由编辑器负责人改为测默认路径。

`renderer_acceptance` 的 10 个入口，RX 6800 XT，GPU validation，证据在 `out/accept-20261018/<配置>-<入口>-a1/`：

| 入口 | Release | Dev |
|---|---|---|
| 无参数默认入口 | PASS | ec-3 门禁 PASS |
| `--scene-lit` | PASS：Edit/Play 收敛 1.228/4（阈值 3/8；ReSTIR 下为 2.262/8），temporal-stable .294/1 | ec-3 门禁 PASS |
| `--scene-lit-temporal` | PASS：direct-reference 1.032/3 | ec-3 门禁 PASS |
| `--scene-lit-quality` | PASS：辐射恢复红移 1–16 帧 +2.511、49–64 帧 +.050；动画光源半影 .462（静止 .194，单帧 3.559） | ec-3 门禁 PASS |
| `--scene-lit-stability` | PASS | ec-3 门禁 PASS |
| `--scene-lit-restir-suite` | PASS，夹具显式 ReSTIR | ec-3 门禁 PASS |
| `--scene-lit-benchmark` | ec-3 门禁 PASS | ec-3 门禁 PASS |
| `--scene-lit-clustered` | PASS：单参考／最小范围／透视参考均为 0 | ec-3 门禁 PASS |
| `--scene-lit-shadows` | PASS：有限点光参考 .681/1，CSM 与移动太阳 ≤.106 | ec-3 门禁 PASS |
| `--scene-lit-cost` | PASS：五组确定性对照均为 0 | ec-3 门禁 PASS |

Dev 列与 Release 的 benchmark 由合并 main 之后的 ec-3 集成门禁（`5911be69`）运行，各入口退出码为 0；数值没有逐项摘录。

Edit/Play 收敛余量：A 下 p95 为 4，距阈值 8 有 4 级，mean 余量 1.77；ReSTIR 下 p95 等于阈值。

Play：Release Play 宿主打开 Scene Lit Room，约 25 秒后用 PrintWindow 截取客户区，`out/accept-20261018/play-{before,after}-room/play.png` 分别为切换前（ReSTIR）与切换后（A）。两图构图与平均亮度一致（场景区 76.95／76.99），A 的后墙与地面没有 ReSTIR 的斑驳噪声。

运动差异图的环纹（实测，`out/ab-20261018/room`，后墙与地面）：A 的 moving−settled 差值 98.4% 为 0 或 1 级，整体偏移 +.27；差 1 级的像素 51% 落在静止图的 1 级等照度线上，未变化的像素只有 3.8%；静止图 99.7% 的像素与相邻像素相差不超过 1 级。ReSTIR 的同一差值也有 +.3～+.65 的偏移，但逐像素噪声把等照度线打散。结论：环纹是两条路径共有的亚级运动过渡落在 8 位量化台阶上的显现，A 没有噪声所以显出来，不是 A 的结构误差。当时显示编码没有抖动；加入静态显示抖动后的对照见[显示抖动](#显示抖动2026-10-21)。

## Edit/Play 收敛修复（2026-10-18）

状态：基于 `f8202f25` 的单独本地提交，未推送、未并入 main。只改 DDGI 命中着色；renderer contract、Frame/FrameStats 接口和 GPU scope 都没有变化。

失败的检查是 `--scene-lit` 的 “identical Edit and Play snapshots converge”。夹具把旧执行域（一串编辑之后约 104 帧）与只递增 `scene_identity` 的新域（96 帧）比较，两边提交的 Frame 除身份外完全相同，所以差异来自渲染器内部，与编辑器或 World 交出的快照无关。

根因（实测）：新域的 DDGI 运输场按域确定，种子随 `probe_sequence` 一起重置，每个新域都重放同一组样本。立柱接收点几乎只依赖一个探针（空间权重 .43，主导度约 1）。临时逐射线检查显示，该探针的 64 条射线中，典型辐射约 0.3～2.5，个别射线落在粗糙度 0.18 的光泽面上，并恰好对准光源的镜面方向，GGX 镜面项达到 23～73。新场前 12 次更新的历史权重只有 .5～.8（成熟后 .93），这三条样本出现在第 7、9、10 次更新，各以约 20% 的权重进入（成熟场为 7%），使该探针在第 96 帧附近约为成熟值的 1.6 倍，约百帧后才回落。旧域已成熟，于是两张图在立柱、墙面和地面上出现大片低频差异。同一二进制重复运行结果完全一致（4.212/14），所以这不是偶发噪声。

修复：探针命中点按完全粗糙（roughness 1）求值材质，颜色、金属度和 F0 不变，详见 [Scene Lit](scene-lit.md#ddgi-探针场)。这是路径正则化，与 Lumen 表面缓存的 fully rough 折算、RTXGI 和 Flax 的纯漫反射命中属同类取舍。另外两个候选没有采用：限幅单条射线会丢失能量，而且原注释已明确拒绝；新场改用纯运行平均，会与多次反弹预热的非平稳反馈冲突。阈值未改。

测量（Release，同一 `--scene-lit` 序列）：

| 指标 | 修复前 | 修复后 |
|---|---:|---:|
| Edit/Play 收敛 mean / p95（阈值 3 / 8） | 4.212 / 14 | 2.659 / 8 |
| temporal-stable mean / p95 | .800 / 3 | .579 / 2 |
| 主导探针单次更新射线均值（12 次） | .26～2.05 | .29～.36 |
| 主导探针 200 帧内读数 | .33 → 1.48 → 回落 | .37～.46 平稳 |

代价（实测）：成熟画面（`temporal-stable-1`）修复前后对比 mean 3.40、p95 14，有符号 −2.42，整体约暗 1.6%，主要分布在后墙、立柱和右墙，并排观察看不出差别。原因是光泽地面朝立柱的集中镜面反射被摊平，该立柱探针的成熟读数从约 .88 降到约 .38。材质、金属、发光和 SSSR 截图逐像素不变，颜色反弹图 mean .80。

余量：p95 正好等于阈值 8。剩余差异主要来自屏幕随机序列：屏幕 RNG 使用全局 `sequence` 且从不重置，同龄的两个新域相互比较也有 mean 1.6、p95 6。比较方法的调整见[下节](#改为比较收敛信号第二个单独提交)。

另记：DI 降噪器只在编辑帧置 `radiance_changed`，颜色或材质编辑后再恢复，会残留偏色，约 200 帧才消失。本检查路径中 DI 历史被 `direct_reference` 切换重置，所以它不是这次失败的原因。该问题已由 [DI 辐射响应窗口](#di-辐射响应窗口2026-10-18) 修复。

验收：Release 与 Dev 各自全量构建 acceptance 后，持锁串行运行 `--scene-lit`、`--scene-lit-stability`、`--scene-lit-quality`、`--scene-lit-ddgi`、`--scene-lit-cost`，均为默认 GPU validation，十次全部退出 0，stderr 为空（`out/accept-20261018/{release,dev}-*-ep1`）。两配置的收敛指标一致（2.659/8），`--scene-lit` 中该检查之后的删光、正交、MatCap 独立性和 resize 检查也全部通过。cost 的五组 DI 图像对差异仍为零。

证据：`out/accept-20261018/editplay-diag-06`（修复前逐射线）、`editplay-fix-01`（修复后逐射线）及各验收目录。

### 改为比较收敛信号（第二个单独提交）

总负责人决定：检查名为“收敛”，就比较收敛后的信号，阈值 3/8 不变。现在两个域各取 64 帧逐像素平均再比较：旧域取 `temporal-stable` 之后的连续 64 帧，新域预热 96 帧后再取 64 帧（域龄约 97–160）。

K 的选择（Release 实测，`editplay-k-01`、`editplay-age-01`）：

- 只把一边换成 K≤32 帧平均没有用。新域单帧对旧域 K 帧平均，K 从 1 到 32 时 mean 从 2.507 变为 2.657，p95 始终为 8。剩余差异不是逐帧白噪声：DI 历史最长 47 帧，DDGI 历史权重 .93 且每个探针约每 8 帧才更新一次，估计噪声在几十到上百帧内相关。
- 新域还有缓慢的系统性多次反弹预热亏量。相对旧域 64 帧平均的有符号差，域龄 96 的单帧为 −.50，域龄 705–768 的 64 帧平均为 −.18。
- 原单帧比较很脆弱：只让旧域在相机切换前多跑 64 帧，单帧结果就变成 2.697/9，检查失败。通过与否取决于两域恰好落在相关噪声的哪个相位。
- 两边各取 64 帧平均：同一序列下为 2.224/7。域龄 193–256、321–384、705–768 的 64 帧平均分别为 2.096/7、1.797/5、2.060/7，延长预热不再稳定改善。

结果：Release 与 Dev（`{release,dev}-scene-lit-avg1`）均为 2.224/7，`--scene-lit` 全部通过，stderr 为空。p95 余量从 0 级增加到 1 级，mean 余量为 .78。剩余差异由两部分组成：几十到上百帧内相关的估计噪声，64 帧平均只能削弱、不能消除；以及新域缓慢的多次反弹预热亏量（域龄 96 为 −.50，705–768 为 −.18）。后者是 DDGI 多次反弹反馈的预热速率，属于渲染器性质，不是测试假象。余量增幅不大，所以没有收紧阈值。代价是该用例多渲染 128 帧。

## Renderer 验收入口盘点（2026-10-18）

状态：基于 `4be48d33` 的单独本地提交，未推送。只改验收入口、`renderer_acceptance` 的运行脚本和文档，渲染代码不变。

盘点前：`renderer_acceptance` 目标只以无参数方式运行 `atelier_renderer_acceptance` 一次；顶层 `acceptance` 目标不依赖 `renderer_acceptance`。所有 `--scene-lit*` 入口都只能手动运行，`--scene-lit-temporal` 因此从 10-13 起失败而无人发现。

| 入口 | 盘点前在常规目标中 | 独有检查 | 处理 |
|---|---|---|---|
| 无参数默认入口 | `renderer_acceptance` | cavity、材质、UI、变换、网格、可见性、选中、视口、参考辅助、关闭 | 保留 |
| `--materials`、`--cavity`、`--selection`、`--visibility` | 否 | 无，均为默认入口的子集 | 删除 |
| `--scene-lit` | 否 | 环境与逐项图像 | 并入 |
| `--scene-lit-benchmark` | 否 | 原生 2560×1440 完成样本与 GPU 分段计时合同 | 并入 |
| `--scene-lit-temporal` | 否 | 信号编辑、移动历史、时间隔离 | 并入 |
| `--scene-lit-quality` | 否 | 四种 AA、收敛、direct64、DI 辐射响应、SSIL、深度 | 并入 |
| `--scene-lit-stability` | 否 | 稳定性 | 并入 |
| `--scene-lit-restir-suite` | 否 | ReSTIR、probe debug、audit | 并入 |
| `--scene-lit-clustered`、`--scene-lit-shadows`、`--scene-lit-cost` | 否 | A 路径、缓存阴影 B、DI 成本 | 并入 |
| `--scene-lit-restir` | 否 | 无，`restir-suite` 已含 | 删除 |
| `--scene-lit-volumes`、`--scene-lit-debug` | 否 | 无，`restir-suite` 已含多体积、DDGI 质量与 probe debug | 删除 |
| `--scene-lit-contracts` | 否 | 无，`quality` 与 `restir-suite` 已含 audit/contracts | 删除 |
| `--scene-lit-ddgi` | 否 | 无，`quality`、`stability`、`restir-suite` 已含 | 删除 |

现在 `renderer_acceptance` 在各自的进程中依次运行默认入口和 9 个保留的 `--scene-lit*` 入口，每个入口限时 1800 秒，任一失败即整体失败；日志为 `renderer-acceptance-<入口>.log`，证据在构建目录的 `renderer-evidence/<入口>`。入口清单维护在 [Scene Lit](scene-lit.md#诊断与性能边界)。

验证（Release 与 Dev，持锁串行）：
- 运行脚本：只保留默认入口和 `--scene-lit-shadows` 的脚本副本退出 0，两个入口的日志都写出（`{release,dev}-runner-inv1`）；不存在的入口名使脚本以 FATAL_ERROR 退出 1，并写出该入口的日志。
- 此前没有近期结果的入口：默认入口、`--scene-lit-shadows`、`--scene-lit-benchmark`、`--scene-lit-restir-suite` 均退出 0，stderr 为空（`{release,dev}-scene-lit-{benchmark,restir-suite}-inv1`）。
- 其余入口在本提交前一轮已通过：`--scene-lit`、`--scene-lit-quality`、`--scene-lit-stability`、`--scene-lit-clustered`、`--scene-lit-cost`（`*-rb1`），`--scene-lit-temporal`（`*-tf1`）。本提交只删除分派分支，没有改变这些入口执行的用例。
- 为缩短单次持锁时间，没有一次性运行完整目标；完整目标在 Release 下约需一小时。

## temporal isolation 参考配置（2026-10-18）

状态：基于 `53f20601` 的单独本地提交，未推送。只改 `--scene-lit-temporal` 的 temporal isolation 检查，渲染代码不变，阈值 3/8 不变。

现象：`--scene-lit-temporal` 在 “all SceneLit temporal isolation configurations meet the unchanged stability bounds” 失败。五种配置中只有 `direct-reference` 越界，相隔 8 帧的两帧 mean 3.729、p95 11。main 基线 `d2b6da2b`（`temporal-d2b`）、`53f20601` 去掉 B 的构建和 `53f20601` 本身数值都相同。

根因（代码历史）：该检查写于 `fbd2cad7`（10-09），第五种配置当时是 `shadow_samples=64`，即确定性的 64 次阴影采样参考，逐帧稳定。`c868ef99`（10-13，引入 ReSTIR）把它机械替换为 `direct_reference=true`。后者每帧重新抽取 64 次独立 RIS 估计，没有降噪，逐帧差异就是估计量的白噪声；差异图在所有受光面上都是均匀的空间白噪声。文档中最后一次 `--scene-lit-temporal` 通过记录是 10-11，此后各批验收都没有运行这个入口，所以失败一直没有被发现。

修复：参考配置的稳定性按收敛信号比较，即两段相邻 16 帧均值之间的差异；四种降噪配置仍比较实际显示的单帧，因为闪烁正是用户能看到的。参考帧之间相互独立，16 帧均值应把噪声降到约 1/4。

结果：`direct-reference` 为 mean 1.032、p95 3，与 3.729/11 按 √16 缩小的预测（约 .93/3）一致；其余四种配置不变（all .617/2，no-sssr .542/2，no-ddgi .616/2，direct-only .591/2）。Release 与 Dev 的 `--scene-lit-temporal` 均退出 0，stderr 为空（`{release,dev}-scene-lit-temporal-tf1`）。

## DI 辐射响应窗口（2026-10-18）

状态：基于 `35b07c17` 的单独本地提交，未推送、未并入 main。改动 `lit_direct.hlsl`、`scene_lit.cpp` 与 quality 验收；renderer contract、Frame/FrameStats 接口和 GPU scope 都没有变化。新增的 `lit_direct_response` 只是内部 shader 常量。

问题：DI 降噪器只在辐射变化那一帧（`lit_temporal.z`）把漫反射/镜面的历史支持上限压到 11/7 帧并夹取颜色。此后计数每帧加一，旧内容按调和级数衰减，第 k 帧仍保留约 12/(12+k)。所以颜色或材质编辑后再恢复，会留下偏色，约 200 帧才消失。

修复（总负责人选定方案 B）：CPU 记录距最后一次辐射变化的帧数。判定沿用原 `radiance_changed`，即灯光、材质、发光体或环境变化。48 帧窗口内 `lit_direct_response=1`，支持上限保持 11/7 帧，并持续按当前邻域夹取颜色。旧辐射每帧保留 11/12，48 帧后为 (11/12)^48≈1/65。持续变化会让窗口一直打开。改动只作用于 DI 历史：不全局重置，`history_invalid` 不变，SSIL 与最终颜色仍使用单帧的 `temporal.z`。

新增验收 `quality_direct_radiance_response`（`--scene-lit-quality`）：

- 场景：只开直接光，AA 关闭。矩形面光（光源 3）改为橙色、强度 ×4，中央遮挡体改为红色，8 帧后恢复。比较恢复后各 16 帧平均与编辑前 32 帧平均之间的 R−B 有符号均值。编辑是有色的，所以残留表现为偏红，而中性光照的估计噪声在 R−B 中大体抵消。
- 门限：恢复后第 1–16 帧 R−B ≥ 1，证明编辑确实进入了历史；第 49–64 帧 |R−B| ≤ 第 1–16 帧的 10%。动画用 0.1% 的交替强度，使每帧都标记辐射变化，半影 ROI 的帧间差不得超过单帧估计（每帧 camera cut）的 25%。
- 使用 A 路径（`direct_clustered=true`）：两条路径共用被测的 resolve，而 ReSTIR 的时空复用会留下低频斑驳，两次 32 帧平均之间的 R−B 噪声底为 −.35～−.39，波动约 ±.5，与残留同量级；A 路径的噪声底约 ±.02。

恢复轨迹（R−B 有符号均值，每格为 16 帧平均；修复前为 Release `item3-base-02`，修复后为 Dev `item3-b-01`，诊断构建）：

| 恢复后帧 | A 修复前 | A 修复后 | ReSTIR 修复前 | ReSTIR 修复后 |
|---|---:|---:|---:|---:|
| 1–16 | +2.807 | +2.521 | +3.131 | +2.911 |
| 17–32 | +1.479 | +.548 | +1.881 | +1.117 |
| 33–48 | +.998 | +.120 | +1.386 | +.855 |
| 49–64 | +.699 | +.043 | +1.034 | +.554 |
| 81–96 | +.355 | +.017 | +.725 | +.407 |
| 113–128 | +.188 | −.003 | +.359 | +.166 |
| 177–192 | +.064 | −.014 | −.127 | −.211 |
| 241–256 | +.030 | −.014 | −.080 | −.132 |
| 噪声底（编辑前两次 32 帧平均） | −.012 | −.020 | −.350 | −.391 |

N 的实测：A 路径修复后在 64 帧内回到基线（第 49–64 帧 +.043，噪声底约 ±.02）；修复前到第 241–256 帧才降到 +.03。ReSTIR 路径修复后在第 49–128 帧的残留约为修复前的一半，但剩余部分与它的噪声底同量级，这一测量无法区分剩余是相关噪声还是另一来源。ReSTIR 的时间 reservoir 复用按前帧光源快照求前域目标（`lit_restir.hlsl` 的 GRIS MIS），按设计不携带旧辐射；该推断没有单独验证。

持续动画（半影 ROI 平均帧间差，32 帧）：A 路径静止 .209、动画 .425、单帧 3.744；ReSTIR 路径静止 .461、动画 1.474、单帧 12.245。修复前后数值相同：每帧都变化时，旧代码同样每帧压低上限，B 不改变这一情形。

变异检查：只把共享 resolve 的两处 `lit_direct_response` 改回 `lit_temporal.z`，CPU 窗口保留，用 Release 运行最终验收（`item3-mut-01`）。结果在 “restored direct radiance returns to its baseline within 64 frames” 处失败：第 1–16 帧 +2.785，第 49–64 帧 +.692，门限 .279。可见该验收确实覆盖共享代码。

代价：每次辐射变化后的 48 帧内，随机直接光像素的历史支持为 11/7 帧而不是最多 47 帧，噪声略高。持续动画的情形与修复前相同。

验收：Release 与 Dev 各自全量构建后，持锁串行运行 `--scene-lit`、`--scene-lit-quality`、`--scene-lit-stability`、`--scene-lit-temporal`、`--scene-lit-clustered`、`--scene-lit-ddgi`、`--scene-lit-cost`，结果在 `out/accept-20261018/{release,dev}-*-rb1`。除 temporal 外全部退出 0，stderr 为空。新检查在两种配置下数值相同：恢复后第 1–16 帧 +2.511、第 49–64 帧 +.050；动画 .462，单帧 3.559。默认路径仍是 ReSTIR，所以 quality、stability 及其余用例原样通过，即构成回退路径的无回归检查。

Edit/Play 收敛检查在两种配置下为 2.262/8（本提交前为 2.224/7），仍通过，但 p95 又回到阈值。B 只在辐射变化后 48 帧内改变 DI 历史，两域的平均窗口都在此之后；差异来自该序列开头恢复场景时旧域历史的衰减方式不同，改变了相关噪声的实现。默认切到 A 后噪声结构会明显改变，届时重新评估这一余量。

`--scene-lit-temporal` 在两种配置下都在 “all SceneLit temporal isolation configurations meet the unchanged stability bounds” 失败。`direct-reference` 配置相隔 8 帧的两帧为 mean 3.729、p95 11，阈值 3/8。同一提交点去掉本提交渲染改动的构建（`release-temporal-base`）数值完全相同，且参考模式本身不经过 DI 历史，所以这是既有失败，与本提交无关，根因与修复见 [temporal isolation 参考配置](#temporal-isolation-参考配置2026-10-18)。同一入口中的信号编辑、移动历史及其余四种配置均通过。

## Lit 帧率批次（2026-10-18）

状态：rendering 工作树基于 `d2b6da2b`，只提交到本地分支，未推送、未并入 main。默认 ReSTIR 路径和 `direct_clustered` 的默认值不变。最大收益在 A 路径（`direct_clustered=true`）；切换默认路径仍属[直接光计划](../../research/direct-lighting-execution-plan-20261015.md)的 C 批。renderer contract、Frame/FrameStats 接口和 GPU scope 索引都没有改动。DDGI receiver gather 区间现在包含代表点 gather、上采样和回退精确 gather，见 [Scene Lit](scene-lit.md#诊断与性能边界)。

- A 路径：紧凑光源（距离不小于四倍半径的有限点光，以及全部太阳）的辐射改为确定求值。软阴影可见性写入逐像素掩码，5×5 邻域判定半影，并保持 8 帧迟滞。空间滤波只间接派发含半影的 8×8 tile。
- DDGI：每个 2×2 块只对一个代表点执行完整 gather，再做联合双边上采样。找不到匹配代表点的像素进入回退列表，做精确 gather。LOS 缓存改为按块存储，1440p 下从 28.125 MiB 降到 7.03 MiB。
- GTAO：一次派发写出 R32F 视空间深度和四级缩减层；缩减偏向最远深度（XeGTAO 思路），远处 tap 按距离选层。resolve 用组共享内存载入 12×12 邻域。
- 编辑器性能入口新增 `clustered_orbit`：每帧 yaw 改变 .2°，在 ±12° 内往返，测相机持续运动时缓存失效的成本，见[编辑器说明](../editor/README.md)。
- 验收中发现并修复一个问题：半分辨率代表点 gather 漏掉了零漫反射材质的工作剔除，导致 `work-ddgi-metal` 出现非零 receiver 查询。修复后，2×2 块内没有漫反射消费者时跳过 gather。代表点仍只按几何选择，保证 LOS 缓存仍然有效。修复前后，地牢三组截图 changed>4 均为 0.00%。

测量条件与 10 月 17 日一致：RX 6800 XT；同一 Dungeon 场景（SHA-256 `c3360e5f…`）与相机；最大化视口原生 2560×1440；Release，无 GPU validation，关闭 VSync；SMAA、GTAO、SSSR、Bloom 开启；单个 8³ DDGI，256 updates，128 rays。每个 case 预热 192 帧，采集 240 个已完成样本；FPS 为接受帧数除以墙钟时间。`final-01` 在 GPU 锁内运行。`baseline-01` 运行于锁规则出台之前，可能有桌面并发负载，FPS 受影响比 GPU 毫秒大。

| 证据 | 路径 | 接受帧/秒 | 八主 GPU scope 均值 | DI | DDGI gather | GTAO |
|---|---|---:|---:|---:|---:|---:|
| `baseline-01`（`d2b6da2b`） | 默认 ReSTIR | 41.05 | 23.98 ms | 12.93 ms | 2.46 ms | 3.07 ms |
| 同上 | A | 46.57 | 21.12 ms | 10.22 ms | 2.53 ms | 3.14 ms |
| `final-01`（本批） | 默认 ReSTIR | 44.70 | 22.03 ms | 13.43 ms | 1.82 ms | 1.90 ms |
| 同上 | A | 61.50 | 15.88 ms | 7.29 ms | 1.63 ms | 1.87 ms |
| 同上 | A 持续环绕 | 53.77 | 18.23 ms | 8.29 ms | 3.14 ms | 1.90 ms |

- A 静止：直接光重建从 6.59 降到 3.91 ms。filter 1/2/4 从 2.08/1.58/.53 降到 1.14/.78/.17 ms，temporal resolve 从 2.39 降到 1.82 ms；可见性从 3.29 降到 3.04 ms。
- 环绕：中间版本 `phase1-03` 只有 A 分类，DDGI 仍为全分辨率，环绕 gather 8.17 ms、总计 24.97 ms、39.34 帧/秒。本批 gather 为 3.14 ms；环绕时 receiver LOS 从 2,776 万条/帧降到 714 万条/帧。静止画面的 receiver LOS 从约 120 条/帧升到约 20.1 万条/帧，因为回退精确 gather 不读写缓存；静止 gather 仍从 2.53 降到 1.63 ms。
- 默认 ReSTIR：AO 和 DDGI 合计省下 1.95 ms，但 direct temporal resolve 从 2.31 升到 2.56 ms（本批三次采样都在 2.56 左右）。新的分类与列表结构在不分类路径上约多花 .25 ms。推断原因是组共享掩码载入和额外的组同步，尚未优化。
- 图像：`final-01` 对比 `baseline-01`，默认路径 changed>4 为 0.81%（mean .205，max 81）；A 为 0.89%（mean .696，max 122），平均亮度 66.08→66.28，来自紧凑光源的确定求值。差异集中在几何边缘和接触处的 1 像素线，来自半分辨率 DDGI 上采样，没有大片色块。同一二进制两次运行的噪声为 0。

Renderer GPU 验收逐项持锁串行，均为默认 GPU validation：

- Release `--scene-lit-quality`：修复前在 `work-ddgi-metal` 失败，修复后 `release-scene-lit-quality-03` 通过。`--scene-lit-ddgi`（`-03`）、`--scene-lit-cost`（`-03`）在修复后通过；cost 的五组 DI 图像对差异为零，DDGI 缓存生命周期与重追踪图像对通过。`--scene-lit-clustered`（`-02`）在修复前的构建上通过，该修复只涉及 DDGI receiver gather。
- Release `--scene-lit`（`-02`）只在“identical Edit and Play snapshots converge…”处失败，mean 4.212、p95 14，阈值为 3/8。同机 `d2b6da2b` 基线（`lit-baseline`）同样失败，mean 4.222、p95 14；此前各项指标与基线的 mean 差都不超过 .03。因此这是既有缺陷，不由本批引入。差异是墙面和地面上大片低频斑驳；推断是两套独立收敛的随机历史在预热帧数内未达到阈值，根因与修复见 [Edit/Play 收敛修复](#editplay-收敛修复2026-10-18)。该检查之后的删光、正交、MatCap 独立性、resize 等检查在基线和本批都被它挡住。为覆盖这些检查，用临时构建只跳过这一项断言（指标照常输出，未提交），Release 与 Dev 的 `--scene-lit` 其余检查全部通过（`{release,dev}-scene-lit-coverage`），跳过项仍为 mean 4.212、p95 14。
- Dev（`out/dev` 全量构建）：`--scene-lit-cost`、`--scene-lit-clustered`、`--scene-lit-ddgi`、`--scene-lit-quality`（均为 `dev-*-03`）在修复后的构建上全部通过，stderr 为空。

证据：性能、图像与场景哈希见 `out/perf-20261018/{baseline-01,phase1-03,final-01}/`；验收输出见 `out/accept-20261018/` 下各同名目录。

## 基础链路与精确工作复用（2026-10-17）

状态：Dev/Release Editor 与 Renderer acceptance 构建完成；两配置 Layout 46/46、UI 27/27、World 16/16、minimal、Editor viewport-edit / Entity composition 和 materials / color transactions 端到端通过。用户随后提供空闲时段，串行完成 Release `--scene-lit-cost`、Dev `--visibility`、Release `--scene-lit-clustered` 的 GPU validation 验收，以及正常 Release 地牢性能采样。该通过范围不包含所有旧专项重跑，也不表示达到 90 FPS。

- DI 保守随机支持直通、严格零方差空间跳过；`direct_clustered=true` 默认回到 A 的精确枚举＋RT，`direct_shadow_cache=true` 才选择 B。二者仍为默认关闭的视口诊断；已知缓存阴影暗楔未在本批宣称修复。
- DDGI 复用未变化端点/几何的精确 LOS；光照、矩权重和反馈照常更新。jitter 模式禁用该缓存。新增缓冲的逻辑内存计入已有资源统计，未改 D32/R32 精度或薄墙遮挡标准。
- `out/render-cost-20261017/compile-shaders.ps1` 按实际 CMake 拼接顺序调用当前 DXC，`cs_6_5 / HV 2021 / O3 / enable-16bit-types / all_resources_bound / res_may_alias`。17 个修改相关 entry 全部成功；DXIL 的 feedback/inspection resource bindings 不包含新缓存资源。此过程未创建 GPU 设备，不能替代 PSO、GBV、图像与性能验收。
- 两位 Astra 分别复审 DI 与 DDGI 主链，未发现生产逻辑阻断；其 jitter 缓存成本意见已处理。另两位 Astra 复审应用/World/Workspace 和 UI/Layout 基础修复，未发现 P1/P2。窗口退避和拓扑分配失败的证据边界保留在对应模块验收说明中。
- Dev World 首轮零分配断言捕获 MSVC checked-iterator 的返回容器固定开销，修正采样区间后两配置复验通过；没有改变生产 API 或关闭 Debug 检查。Dev 空结果基线为 48 bytes、稀疏场景加入 256 个逻辑对象前后均 8592 bytes；Release 对应 0 / 8064 / 8064 bytes，无网格结果容量均为零。Astra 复核确认固定基线仍能识别原全成员预留回归。第一次 Renderer 编译的验收局部变量残留也已清理，最终两配置通过。
- 新增 `--scene-lit-cost --output <绝对目录>` 沿正式 Renderer 小 ROI 消费者检验确定直接光当前帧、缓存生命周期和独立持续重追踪图像对照。光源编辑、finite→delta、移动遮挡物、B→A 首帧和旧路径 delta sun 五组 RGB8 差异为零，FP32 深度一致。DDGI 的 13 类生命周期产生 245 对已完成样本，包含相机、几何增删移动、Mask 恢复、体积布局/滚动、开关和新场景；图像/深度逐帧通过独立参考检查，光照改变时缓存仍可复用。TAA／SMAA T2x／无抖动切换也通过。实际小 ROI 为 127.5×95.25，不外推其绝对耗时。
- Dev `--visibility` 通过连续区间、像素/深度参考、重入、非法输入、六面剔除、反向缩放、跨眼平面、离屏光照域和空场景；Release `--scene-lit-clustered` 通过 272 灯溢出、有限源/矩形边界、投影、小数/尾 tile、参考优先级与空场景重入。有限球光 ROI 均值为 182.348，对独立参考 184.673；矩形光为 189.576 对 188.118，仍属于有误差的采样重建，不称为严格积分等价。三次退出均为 0、stderr 为空。
- 首轮 cost 的 240 秒 watchdog 在冷管线准备阶段截断；第二轮已完成五项 DI 对照，但第二个 Fixture 因窗口类重复注册而失败。验收 Window 改为共享的函数局部静态 WindowClass RAII，单窗口销毁不提前注销共享类；Astra 独立复核无阻断。两配置 acceptance 重编后，最终 cost 完整通过（含冷准备 418 秒）。原失败和超时记录保留，不能当通过或性能基线。
- 性能测量结束并释放 GPU 后，应用负责人用同一冻结 Release Editor 串行完成正式 `--acceptance-surfaces`，退出 0、stderr 为空。`editor-surfaces-release/surfaces.csv` 包含 `render-retry,validation-control-recovery-close,passed`，实际覆盖 F1 新 deadline 自然到期恢复、Control 可达、成功清零和原生关闭；完整菜单/拖动验收及其内嵌性能检查也通过。此窗口证据不替代下列地牢计时。

正式性能使用 RX 6800 XT / 驱动 `32.0.21045.5002`、相同保存地牢和相机、原生 viewport 2560×1440、Release、关闭验证层和 VSync。保留 SMAA、GTAO、SSSR、Bloom，以及单个 8³ DDGI volume、256 updates、128 lighting rays；不修改作者参数。每个 Lit 路径预热 192 个接受帧，采集 240 个唯一已完成 GPU 样本。FPS 是真实 Application 接受帧数除以墙钟时间，包含 CPU/UI/Present 与验收开销，不是 GPU 耗时取倒数。

| 最大化 Scene View 路径 | 接受帧/秒 | 八主 GPU scope 均值 / P95 | DI 均值 / P95 | DDGI 均值 / P95 | 其中 DDGI gather |
|---|---:|---:|---:|---:|---:|
| 当前默认 ReSTIR DI | 40.81 | 24.127 / 24.510 ms | 13.036 / 13.077 ms | 3.432 / 3.812 ms | 2.484 ms |
| A：精确局部枚举＋RT | 46.32 | 21.230 / 21.694 ms | 10.297 / 10.658 ms | 3.183 / 3.582 ms | 2.495 ms |

DI 重建仍分别为 6.595 / 6.614 ms；本轮旁路未在该地牢显著改善 DI，总耗时仍是后续重点。DDGI update 为 .948 / .688 ms，不能将两路径间这项差异归因 DI 切换。静止 gather 的实际 LOS 均值约 118 / 122 条/帧；同场景旧默认基线约 2,767 万条/帧、gather 8.221 ms，现在下降到约 2.48 ms。没有降低 FP32 深度、probe 更新预算或取消遮挡；相机持续运动时缓存失效的 1440p 吞吐尚未测量，不能以静止结果证明动态 90 FPS。

最大化视口 Smooth / MatCap 分别为 160.65 / 148.77 接受帧/秒；同相机旧基线为 77.36 / 72.80，两种模式的 2560×1440 RGB8 截图逐像素一致。主场景 draw 从 2,652 降至 557，3,381 实例中剔除 2,713 个。默认 Lit 从 32.24 提升到 40.81；单帧 RGB8 平均绝对差 .00348、P95 0、最大 34，保留随机采样差异，不将单帧近似一致当作全部时域质量证明。

正常停靠布局同为 viewport 2560×1440，完整 target 从最大化的 2668×1730 变为 4315×2319。Smooth / MatCap 仅 56.81 / 54.09 接受帧/秒：controls 7.86 / 8.25 ms，UI prepare 3.07 / 3.27 ms，scene snapshot 3.06 / 3.22 ms，frame-slot wait 近零。此项仍受编辑器 CPU 工作影响，已将现有证据交应用负责人分析；本次没有测正常停靠布局的 Lit，不能拿最大化 Lit 数字代替它。

`cost-final-20261017-01` 正式采集退出 0，原 Scene/UI authoring hash 不变。GPU 监控保留桌面 DWM 和 Chrome 视频/合成活动，未观察到游戏负载；数据按这类桌面后台环境解释，不称为完全无后台负载。未开启诊断计数直方图参与计时。GPU 父项与子项不可重复相加，非 Lit 没有 Scene Lit GPU scope，其缺失值不是零 GPU 成本。

证据：CPU/构建仍见 `out/foundation-review-20261017/evidence.json`、`out/world-w1-20261017/`、`out/render-cost-20261017/{shader-build,final-build}.json`。GPU 见 `out/render-cost-20261017/gpu-acceptance/acceptance-summary.json`、对应三项目录及 `fixture-{dev,release}-build.log`。性能、图像、采样和场景哈希见 `out/pipeline-baseline-20261016/cost-final-20261017-01/` 的 `render-performance.json`、`runtime-result.json`、`image-comparison.json` 和 `desktop-gpu.jsonl`。Release Editor SHA256 保持 `C356AABD33DDA115F12D050C965A2DFD59B5E7B7C55FE7FCEAAC74923B6C9BB4`，Dev 保持 `4B06DEE825C84D9C6D9CC6D9A8A0193813F2BB893F87AEFE138E762D96A4A7DD`；修复验收窗口后的 acceptance Release 为 `21D97CAB44BA622604D0179C9F8EC6D91EA730A6F0711E2D70B5EE82790DA703`，Dev 为 `B62CFEB397C68874CA74154935B6464E7286823135C01696BDF001318F1106A3`。本批未提交推送。

## 编辑器整帧诊断（2026-10-16）

用户要求暂缓光照路径优化，优先排查非 Lit 同样缓慢的问题。正式编辑器既有 `--acceptance-render-performance` 入口现使用实际 VSync=false，并加入同场景、同相机、原生 2560×1440 的 Smooth/MatCap 各 192 帧预热与 240 个接受帧；正常产品默认 VSync 不变。CPU 记录 Application 控件、UI prepare、场景快照、整步、Renderer 准备/命令录制及 RHI 提交/Present/帧槽等待/回收。CPU 当前提交序号和异步 GPU 已完成序号分开，非 Lit 的 Scene Lit GPU 值明确为 null，不读取旧样本。

Release 计时版 SHA256 `d09018e63fe81167e4dbe9c7ca6810393a90382e25af9a97aae527ef60563338` 构建通过。初次 `out/pipeline-baseline-20261016/uncapped-full-01` 因 Nivalis Nights 竞争负载不具性能基线资格，状态保留于 `qualification.json`。随后空闲对照 `idle-app-before-01` / `idle-app-after-01` 均退出 0，CPU 分段和、唯一提交/接受帧身份及原场景/UI 作者 hash 通过。两项 App/UI 重复工作清理的具体前后数据统一见[编辑器 CPU 记录](../../research/editor-cpu-pipeline-20261016.md)：Smooth 59.05 → 77.36 接受步/秒，MatCap 59.39 → 72.80；同相机 viewport PNG 的 RGB8 最大差均为 0。默认 Lit 仍约 32 接受步/秒，GPU 约 30 ms，不把 CPU 省时说成已改善 GPU 受限场景。监控中修改前启动/预热阶段出现过其他进程 GPU 活动，最后一次高活动记录早于计时段；保留完整原始时间戳，不将整段进程运行描述为始终无其他活动。

`idle-no-ddgi-01` 使用原地牢的独立 Scene 副本，仅将 `world.ddgi` 改为 false，原相机、灯、阴影、GTAO、SSSR、SMAA 与曝光保持一致。默认 ReSTIR 路径为 45.02 接受步/秒，GPU 主 scope 合计 21.773 ms，其中 direct 13.102 ms、其重建子项 6.705 ms；DDGI 各 scope 为 inactive。它不等于传统无光追管线。临时副本前后 hash 一致，运行后移除，原 Scene 未改。汇总见 `out/pipeline-baseline-20261016/idle-results.json`。

RHI 自动 Globals root-CBV 候选已经在其工作树通过 Dev/Release 真实 GPU RHI 端到端验收，日志为同目录 `rhi-root-cbv-{dev,release}.log`，两次退出 0、debug warnings/errors=0。但 `idle-rhi-root-cbv-01` 期间新启动的 PioneerGame 占用最高约 95% GPU，且后续正常停靠布局的精确 1440p 校准失败，不能作为性能通过或退化证据。已冻结该候选并从主目录撤回其增量，计时及已实测 App/UI 优化保留。正常布局补测和 RHI 独立性能对照仍待用户空闲时进行，最大化结果不替代完整停靠布局验收。

同日继续排查的 CPU-only 证据为 `scene-cpu-before.json` / `scene-cpu-after.json`：通过正式 Project/Session 加载同一保存地牢，不创建图形设备或窗口，不修改源 Scene。3,381 个实例共 481 种几何、2,652 个相邻批次、1,853,242 个实例三角形。用上次已接受的 2560×1440 相机矩阵计算，新增保守主视图剔除保留 668 个实例、557 个连续几何区间、373,180 个三角形；2,713 个实例完全在视锥外。这是当前 CPU 可见性计算的工作量，尚不是 GPU 实测 draw/FPS，也不是遮挡剔除。完整 TLAS、材质与光源域继续包含屏幕外物体。静态图片表按 Image 与颜色空间去重为 12 项，另有 1 项默认白图。

World/Workspace 的同次提取临时分配清理，将 SceneSnapshot 中位数从 3.984 ms 降至 3.062 ms、最大化 Workspace 描述从 1.749 ms 降至 0.232 ms。独立调用的 render_items 为 3.690 → 2.186 ms，不能和包含它的 SceneSnapshot 相加。正常停靠 Workspace 为 11.578 → 7.671 ms，但该 CPU 入口没有 accepted UI 帧、UI prepare 或 Present，只用于同条件归因，不代表正式编辑器稳态帧时间。代码细节与文档端到端验收由[编辑器 CPU 记录](../../research/editor-cpu-pipeline-20261016.md)维护。

主视图剔除新增的 `atelier_renderer_acceptance --visibility` 沿同一真实 Renderer 入口覆盖原索引区间、图像/深度一致、重新入镜、视锥外非法矩阵仍拒绝、六裁剪面、镜像非均匀缩放、跨眼点和屏幕外发光域；该 GPU 验收目前待执行。既有 `--materials` 继续检验屏幕外纹理发光对可见接收面的真实照明。保持用户“先代码收尾，稍后再测”的安排，本批尚无新的 GPU 画面或 FPS 通过结论。新改动前的完整编辑器基线已留存为 `application-before-d09018/atelier_editor.calibration-baseline.exe`（SHA256 `709499f891e0dd080068dddc72c4a6f42541a019707588a16bca64ec8ea71743`）。

最终集成的 Dev/Release `atelier_editor` 和 `atelier_renderer_acceptance` 均构建成功，日志 `scene-visibility-{dev,release}-build.log`，四份产物完整哈希见 `scene-visibility-build.json`；Release Editor 为 `9bbb4b46b359c7f6af49f4a0f1ac725081b27d3665da6f8a886cc093cbd4b45d`。Release CPU 端到端 World（15/15）、视口编辑与 Entity composition、材质/颜色事务均退出 0；新增验证包含同一 Session revision 下直接 Runtime 位置修改可见，以及非有限变换拒绝后恢复。构建与 CPU 验收不替代前述待执行的 GPU 画面和整帧对照。

## 精确局部光照与缓存阴影候选（2026-10-16）

基于主目录 `5e5d73c8` 的未提交工作区，沿[直接光执行计划](../../research/direct-lighting-execution-plan-20261015.md)推进 A/B 批。`direct_clustered` 仍是默认关闭的视口诊断；没有宣称完成默认迁移、旧 ReSTIR 退役或 90 FPS 目标。RHI 的矩形 clear/PSO bias 由负责人独立 Dev/Release GPU 验收后提供，本线程只通过现有 RHI/gfx 接入。

A 阶段 `--scene-lit-clustered` 在 Dev/Release 启用 GPU validation 通过，输出保留于 `out/direct-clustered-20261016/dev-fixed` 与 `release`。最终可选接收面直方图又经 Dev `dev-inspect` 通过：272 灯溢出与等功率单灯图像 mean/p95 均为 0；delta 单灯与独立参考 mean .121、p95 1；有限点光范围、矩形面积边界、共享最小 range、小数/尾 tile、两种投影、参考优先级与空场景重入均通过。大球光范围边缘的平均显示值 167.483 对参考 184.673，约低 9.3%，仍是重建/低样本近似的限制，不能凭宽容差宣称最终画质达标。

B 的 `--scene-lit-shadows` 在 Dev `shadow-dev-final` 与 Release `shadow-release-final` 启用 GPU validation 完整通过，退出 0、stderr 为空。Release 已包含独立 Astra 复审指出并修复的无限远 Reversed-Z 相机与冷分配全表复制问题；前述 Dev GPU 运行发生在这两项 CPU 补强之前，最终 CPU 版另在 Dev debug layer 的 `shadow-dev-cpu-final-debug` 完整通过。临时诊断构建后的验收入口已恢复默认 GPU validation。双配置 Editor/Renderer 构建通过。证据均位于 `out/direct-clustered-20261016/`，实际 159.5×111.25 或 31×23 小 ROI，不能外推地牢成本。

覆盖精确完成帧 cold/warm、静转动、后续动态更新、删除/重建、Mask、光强/材质不触发深度重画、移灯、离屏 caster、空 World 重入、有限源与独立 RT 参考、四级 CSM、相机移动、atlas 压力及混合光源。静止点光 5/5 面复用、更新/绘制均 0；272 灯产生 1,632 面需求并分为两批，等功率单灯 ROI mean .0196/P95 0，无 missing tap、意外 RT 或重复计能。point/sun/rectangle/emitter 混合 ROI mean .6327/P95 1；球内 RT 计数 713，半径缩小后归零且原 5 面全部复用；无阴影球内点光仍有贡献且无 RT。Release 无限远相机同时验证冷启动、实际亮面、独立参考与暖缓存。

修复期间的失败保留：混合 PCSS/RT kernel 首次 Bind 出现异常 CPU 内存增长；拆分编译后通过上述 GBV 运行，不能把等待位置直接解释成已证明的驱动/GBV 根因或 CPU 缓存泄漏。最终独立 DXIL：cached point 26,688 B、cached sun 35,784 B，均无 RayQuery；endpoint RT 42,420 B，见 `out/shadow-final-dxil-20261016/`。斜面条纹的修复同时覆盖 texel 中心的接收平面深度与固定 DXC 的矩阵访问差异；GPU float-bit 读回和 DXIL 偏移确认了问题，统一 `mul` 提取后 point mean/P95 从 5.862/37 降至 .323/1，有限点光 .799/1，CSM .010/0。未提高 bias 或放宽门槛，临时颜色/读回诊断已移除。压力完成样本改为有界等候真实序号，不再假设固定两帧延迟。

PCSS 仍是中心深度图近似：本例局部半影与 RT 的 mean 9.716/P95 33，通过预先声明的 12/40 门槛，不代表所有大球光、薄遮挡与掠射条件等价。独立 Astra 当时确认齐次远方向和分配撤销记录的修复路径，未发现该范围的新阻断项；随后真实地牢暴露了下述质量和性能问题，默认迁移与旧 ReSTIR 退役仍未完成。

正式编辑器验收入口已校准真实 pixel-centre 覆盖为 2560×1440：当时完整 RT 为 2668×1730、DPI 1.5、逻辑物理 rect 为 `[57,255,2560,1440.5]`。保存地牢场景与 UI authoring hash 前后不变，场景 SHA256 为 `68657ad2dce3c039b4e5cb2d2552ba6a59f4f62c66cd8cf340cd8b16c9c36b38`。有效接收面 3,686,395 个、保守解析局部支持最大 3；该观测只属于此固定相机与场景。

`dungeon-a` 仅取得 legacy 半组 240 个连续唯一完成样本，随后 PNG COM 初始化遗漏导致停止；编辑器负责人已修复初始化。事后发现同机 Nioh 3/Houdini 与显存压力，GPU 专用占用约 15.29 GB、共享约 3.36 GB。这些内存值不是逐帧测量；原数据、失败报告和 `assessment.json` 保留，明确标记 `usable_as_performance_baseline:false`。不能使用这组被竞争的异常数据计算路径提速。

用户随后提供空闲时段，正常 Release 在同一保存配置（单个 8³ volume、256 updates、128 lighting rays、SMAA、GTAO、SSSR）完成原生 1440p 对照。每路径额外预热 192 个接受帧，保存 240 个唯一完成 GPU 样本；VSync=true。下表吞吐由真实 Application 接受帧数/墙钟时间计算，包含 CPU、Present 等待和验收驱动开销，不是 GPU 毫秒取倒数，也不是独立测量的显示器扫描率。工具输出 PASS 表示采集合同完整，不表示达到性能或画质门槛。

| 证据/路径 | Direct 均值 | 其中着色 | 其中重建 | 八主 GPU scope 合计 | 接受帧吞吐 |
|---|---:|---:|---:|---:|---:|
| `dungeon-a-idle-05` / legacy | 13.026 ms | 2.492 ms | 6.672 ms | 29.508 ms | 32.74/s |
| 同进程 / A 精确枚举＋RT | 10.349 ms | 3.375 ms | 6.649 ms | 26.839 ms | 36.75/s |
| `dungeon-b-gather-idle-06` / legacy | 13.144 ms | 2.517 ms | 6.732 ms | 29.873 ms | 33.04/s |
| 同进程 / B＋Gather 实验（已撤回） | 16.139 ms | 9.341 ms | 6.478 ms | 32.610 ms | 28.35/s |
| `dungeon-b-restored-idle-07` / legacy | 13.082 ms | 2.502 ms | 6.705 ms | 29.760 ms | 33.18/s |
| 同进程 / 最终回退后的 B | 15.305 ms | 8.529 ms | 6.457 ms | 31.711 ms | 28.63/s |

A 的 DI 降低约 20.5%，收益来自省去 reservoir 选灯/复用；实际 visibility/shading 比原路径更贵。A 仅用于有哈希记录的隔离实验可执行文件，关闭缓存选择的临时代码已恢复，不增加产品兼容入口。B 在 304 个 face 全暖、零重绘、零 missing tap/endpoint RT 的条件下仍较慢，不能把成本解释为冷缓存。`dungeon-b-idle-03` 的原始 B 为 DI 15.627 ms、着色 8.603 ms；该轮全程监控出现 Chrome/DWM 活动，不能据此单独证明跨运行的微小收益。

两项指令级候选已撤回：`dungeon-b-idle-04` 的每 invocation 局部计数＋wave 汇总使着色增至 11.449 ms，虽保留事件计数语义，却没有实测收益；Gather 也未证明优于原四次 Load。最终两个缓存 HLSL 文件去除注释/空白后与此前 GBV 通过的拼接源码对应部分相同，没有留下这些实验实现。DI 重建约 6.7 ms，DDGI receiver gather 约 8.2 ms，是后续仍须处理的独立成本，不能由 A 的局部收益推算 90 FPS。

07 为最终正常 Release、退出 0、stderr 为空、场景/UI authoring hash 不变；全程 GPU 引擎采样没有观察到显著的其他应用竞争。主 GPU scope 合计的 legacy/B P50 为 29.712/31.509 ms，P95 为 30.013/33.260 ms，P99 为 30.088/34.663 ms。每个同序号完成样本新增记录既有射线计数：probe primary 每帧 40,960；receiver gather LOS 的 legacy/B 均值分别为 27,675,032/27,674,972 条，约 7.51 条/有效接收像素，已接近八邻域理论上限。对应 gather 均值 8.106/7.930 ms，probe update 约 .72/.69 ms。这里的 LOS 是真实查询数量，既不是阴影光源数，也不由 probe 更新预算直接决定。

真实地牢质量归因位于 `dungeon-shadow-inspection` 与 `dungeon-shadow-inspection-02`。关闭 DDGI/GTAO/SSSR/Bloom 后，B 的日照地砖/台阶仍有三角暗楔；太阳 angular radius 从 .05 改为 0 后仍存在；只关闭太阳 `casts_shadow`、保留局部灯投影后，相关暗楔消失。归因指向 B 的太阳缓存阴影链，尚不能确定为投影、级联、法线或深度比较中的哪一项，也不能仅归咎于 PCSS 软过滤。太阳两项均经正式 Control 编辑/Undo 恢复，最终 dirty=false、undo=0，源场景哈希不变。Astra 复审支持把此项列为 B 的默认迁移阻断，继续 A 与重建信号分离；有限源 RIS/太阳角度采样仍是随机信号，不能因“精确枚举”就直接删除其重建。

与编辑器负责人补齐同一 `editor.inspect` 的有界对象目录和单对象详情，在 3,572 个 Entity 的地牢中实际完成按名称找太阳、读取精确属性、临时编辑与恢复。性能入口也提前锁定作者配置和 Undo/Redo/revision/dirty，在准备及每个接受帧拒绝作者状态变化，并保存源 World/加载后 World。普通 `--scene-lit-camera` 会自动 fit DDGI；本轮性能入口明确关闭这一行为，交互诊断则使用正常工程打开与 `camera.look_at`，没有把自动 fit 的 16³/1024 配置混入上述基准。

最终 Dev/Release 的 Editor、Renderer acceptance 与 Control acceptance 构建通过。两配置 `--descriptions` 真实进程验收均退出 0，覆盖新增有界目录、分页/过滤、对象详情、非法请求、旧游标/句柄和查询不改变文档；日志为 `control-inspection-dev.log`、`control-inspection-release.log`。不重复把这些 Control 验收写成渲染质量通过。所有本轮启动的验收/诊断编辑器已结束，保存地牢哈希保持不变。

## DI 重建缓存与目标复用（2026-10-15）

基于主目录 `94d92f0e` 与上一批未提交增量。新增20个同帧计时区间中的16～19项及性能表四个重建子行；step 1 使用6912 B FP32组共享几何引导；reservoir 原预留DWORD保存同帧target，仍为24 B，时间复用显式固定两输入上界。第二轮滤波缓存实测退化，已撤回。具体算法与合同见 [Scene Lit](scene-lit.md)，逐候选性能、真实视口大小和运行态边界见[研究记录](../../research/di-dungeon-diagnosis-20261015.md#重建与-reservoir-热点实施2026-10-15-后续)。

Dev/Release Editor、Renderer acceptance、Control acceptance构建通过。缓存阶段 Dev `--scene-lit-stability` 退出0，覆盖新增指定probe、active范围外请求、auto恢复、关闭后的迟到读回、96 B ray端点距离/有效性，以及原运输、响应、隔墙、多体积和滚动生命周期。Release `--scene-lit-quality` 退出0、stderr为空，覆盖四种AA、反射/半影、动态编辑、FP32深度、薄墙/零通道、work elision与contracts；这两次发生在随后target缓存加入之前，不能冒充最终target版验收。

最终target版 Release `--scene-lit-restir-suite` 开启GPU validation，退出0、stderr为空。覆盖材质/输运、272个投影光源、空间top32溢出与全局兜底、独立参考能量、源重排/增删/移动、DDGI质量/调试/多体积、64位历史身份、小数/单像素视口、关闭重入与重复close。最终Dev `--scene-lit-restir` 同样开启GPU validation，退出0、stderr为空，独立通过272光源、提议溢出/兜底/滚动/生命周期、参考能量与源变化。

正常 Release 地牢副本（client3840×2054、viewport2265.36×1246.12）360个唯一完成样本的起始DI均值16.247 ms；最终两次为11.514/11.774 ms，P95为16.976/17.314 ms。step 1约2.61→1.62 ms的收益在快慢档均可见。共同慢档仍在，整段差异不能全部归因单项优化；八主阶段合计不含完整编辑器CPU/UI/Present，不报告为60 fps或功耗改善。

最终标准Release Editor副本的正式Control验收已通过：21行层级/八父项总计，四个重建子项不重复累加；`probe_inspect_source`重复patch幂等、4096原子拒绝、300实际端点、4095有效但不在驻留池时无旧rays；关闭inspection并切MatCap后新计时数组归零。相机、view、Undo、redo、dirty全部恢复。两位Astra独立只读复审未发现阻断项；GPU结论仍以真实运行结果为准。

指定probe 372/300/310各768个地牢样本抓到probe300接收方向运输照度+53.25%、receiver+6.70%的事件；相关点光距离.192 m、q .394、W1.186。未触及.05m衰减下限，既不能判为低q爆炸，也不足以证明所有闪光已归因。研究已记录近场方向与secondary GGX的方差边界，本批没有改运输模型、增加亮度clamp或以变暗作为修复。

证据根目录 `out/di-reconstruction-20261015/`：`dev-stability`、`release-quality`、`release-final-suite`、`dev-final-restir`及日志/状态，`final-control-*.json`，各候选`*-samples.json`/`*-summary.json`、`dxil/summary.json`、`rhi/`与端点归因；源码和二进制身份在`manifest.json`。`cache2-editor.log`保留第一次实验的HLSL结构体条件表达式编译失败；修正后第二轮缓存虽能运行但性能更差，最终产品移除了该候选。基线ADLX与GPU记录不重叠的那组明确排除，未用其推断频率原因。

## DI 精确优化与共享空间提议（2026-10-15）

基于主目录 `99111cab` 的本批工作区；期间主线前进到编辑器 UI 提交 `1bbf6cef`，最终源码与二进制清单记录当前 HEAD 和未提交增量。新增成对直接光时间重建、MIS 候选准备复用与便宜邻域拒绝；同一 LightFrame 的有界空间提议供 DI / DDGI hit 消费，使用实际整数技术配额与完整混合 q。当前行为见 [Scene Lit](scene-lit.md)，研究、性能中间候选与测量边界见[地牢 DI 诊断](../../research/di-dungeon-diagnosis-20261015.md#本批实施与实测2026-10-15)。没有改变 FP32 深度、内部像素数或最终 AA 策略。

Dev / Release Renderer acceptance 与正式 Release Editor 构建通过。Dev `--scene-lit-stability` 已通过异步 E/w/transport、有效响应、hold、合法射线预算、源时间曲线、材质发光传输、整墙／薄隔墙和多体积／滚动生命周期。800 三角形发光面接收红通道 50.0450，遮挡/移除后 0；整墙静态 max mean .1446、p95 1，慢速发光体 max mean 2.3615、p95 7，薄隔墙被遮挡区域红通道为 0。Release 组合入口中的材质、材质光输运、新增 ReSTIR 空间提议、DDGI 质量／多体积／调试及 audit 均已 PASS；随后在既有 work-elision 用例处退出 1。修正其信号隔离后，独立 Release `--scene-lit-contracts` 开启 GPU validation，于 05:22:46–05:29:11 完成，退出 0，audit / work elision / contracts 均 PASS。两次运行共同覆盖本批相关范围，不把原组合进程的退出 1 改写为成功。

首次新增 lifecycle 验收在空场景统计断言处失败：GPU 表已退休，但 Renderer 手动清零链未包含四个新增统计字段，继续报告上一帧的 36 B sentinel。已补齐每帧清零，保留原空场景 0 B 断言；这不是通过放宽阈值隐藏失败。新增用例还覆盖 top32 溢出、全局独立参考、近灯无贡献而远灯主照明、M=1、焦点离开／返回、跨格移灯和源删除／重建。

既有 inactive-probe 用例把“插入封闭遮挡后总颜色 ≤1 RGB8”当成 DDGI scratch 退休断言。保留的原版二进制与新版都得到均匀 RGB 3；新版实测关闭 DDGI 仍为 3，直接光独立参考为 0，定位为原有 DI 降噪历史的低亮度尾巴。该用例现保留暖探针场，只切换直接光参考，并断言没有 DDGI history/reset-probe 重置，再检查原有 ≤1 阈值和 DDGI 关闭对照；最终 GPU 实测这两张参考图均为 RGB 0，所有断言通过。不改生产历史策略或阈值；原版与诊断失败日志保留，低亮度 DI 残留仍是已知限制，不能把隔离后的 DDGI 通过解释为它已修复。

普通 Release 固定 1440p、96 帧预热、240 唯一完成样本的首次最终 GPU 路径测得 DI p50 **4.328 ms**，原版两次 **4.999 / 4.964 ms**；八主阶段合计 p50 **8.976 ms**。地牢保存副本恢复用户相机和参数，实际 viewport 约 1126×666；原版／新版空间提议各 240 样本的 DI 均值 **12.214 / 11.046 ms**，八主阶段合计均值 **21.544 / 21.483 ms**。这支持 DI 优化，不足以证明地牢整帧提速、1440p 地牢帧率或功耗下降。新版同二进制 global-only DI 均值 10.598 ms，说明空间提议也有额外成本。

同一地牢、同一二进制两个提议模式各预热128帧后捕获12张静止图，间隔8 accepted frames。后墙／右墙 sRGB 亮度时间标准差均值从 .598 / 1.081 降为 .485 / .546，地面亮度均值 146.155 / 146.124；全取样视口标准差均值 .660 / .481。短窗与非线性显示统计不是无偏能量证明，仍存在局部高亮突变，不能宣布 DDGI 根治。测试运行在独立保存副本，没有保存、替换或关闭用户原窗口；用户随后说明误关了原实例，并允许自行重开验收窗口，因此无需恢复已退出实例的显示模式。

三位 Astra 最终复审通过，分别核对估计器/历史、生命周期/幂等/边界和 view/World/Control 接线。新开关不污染 World 保存，关闭诊断只禁用提议，表构建和资源仍保留。应用与编辑器负责人已接收统一 metadata 表的合并说明。RHI 提供只读 PresentMon 旁证，本批无需新增低层能力。

最终标准 `out/release/atelier_editor.exe` 打开独立地牢副本，经正式 Control 验证空间提议开关、重复 patch 的 state token 幂等、World Undo/dirty 隔离及真实捕获，通过并退出验收副本。Lit 静态帧报告 60 retained entries / 54 非空格 / 34108 B / uploaded=false，切回 MatCap 后四项当前帧统计归零。没有向原用户工程写回相机、Scene 或作者参数。

证据目录：`out/di-evolution-20261015/`，包含 `manifest.json`、`performance-summary.json`、固定场景各候选 CSV、地牢原始唯一样本和质量捕获 manifest、`dungeon-quality-analysis.json`、`rhi-runtime/` 及各验收日志。`release-restir.log` 保留首次统计失败，`release-suite.log`、`baseline-contracts.log`、`current-contracts-diagnosis.log` 保留信号隔离前的失败；最终合同结果为 `release-contracts-final.log` / `.status.json`，正式编辑器结果为 `standard-editor-verification.json`、`standard-editor-capture.json` 和 `standard-editor.status.json`。不能只查看文件存在就视为通过。

## DDGI 访问采样与有效响应（2026-10-15）

主目录 `9263010` 上未提交的第二批，RX 6800 XT；Dev/Release正式编辑器与Renderer acceptance已构建。原实例本轮开始时已不在Control列表，未由本任务保存或关闭；验收均使用独立复现工程，保存Scene SHA256仍为 `B4CE5D42A074F843288F4A213B2C5F821C7B8058E675C78CE83FB29AB0D410B6`。

- 同一 `--scene-lit-stability` 真实GPU、D3D12 debug layer通过：逐方向有效响应、未成熟hold计数冻结、source-frame关联、coverage保持、新场/空场/暂停、1/3/127/128/255/256 rays切换及原quality/多体积滚动验收。最终日志 `out/ddgi-sampling-20261015/delivery-{dev,release}.log`。
- 175 probes /128 rays /固定1/60s的B16用例：feedback 0/.85，运输关灯t90从4.23/4.92s降至.95/1.13s，显示从4.42/5.12s降至1.15/1.35s；开灯运输t50从.75s降至.383s。没有增加射线预算。t90要求连续8个完成样本满足阈值，不能解读为任意反馈场景的永久收敛时限。
- 最终Release静态整墙max mean=.1624 RGB8/P95=1；连续移动发光体max mean=2.4136/P95=7，停止后的原稳定门槛保留且通过；薄与亚像素隔板遮挡区红通道0。
- 地牢固定后墙点各512样本：Y std .0037543→.00082994，P99 .0518553→.0356172，均值下降约4%；不同序列轨迹，不能外推全场或宣称严格能量等价。关诊断后B64/1024的DDGI median约3.35/4.16ms；实际viewport约1126×666，不是1440p帧率。证据 `delivery*.json` 与 `paired-reference*.json`，完整限制见第二批研究记录。
- Astra分别审查变化身份/资源生命周期、R3整数相位与Trace/Update对应、实际GPU验收覆盖。修复纯对象重排导致全场epoch误升，退役全局4N响应计数；未保留失败Sobol候选或第二套光源/渲染入口。详细方法、对照数据及限制见[第二批记录](../../research/ddgi-stability-and-lighting-roadmap-20261015.md#11-第二批采样顺序与有效响应)。

## DDGI 运输与呈现分离（2026-10-15）

源码为主目录 `9263010` 上本批未提交增量；RX 6800 XT。受影响 Renderer、Application 和正式编辑器的 Dev/Release 对象/库已编译。原用户 Release 编辑器保持打开，因此 Release 候选沿 CMake/Ninja 生成的相同链接命令，仅改产物名为 `out/release/atelier_editor_ddgi_preview.exe`；未覆盖正在使用的正式 exe，非另一套源代码或构建配置。

- `--scene-lit-stability`：Dev/Release 实际 GPU + D3D12 debug layer 通过。三路 E/w 归因各 32 个唯一完成样本；holdE 同时保持运输/显示 RGB，holdD 保持同接收点距离权重。零 delta 时 transport 继续变化而有效显示精确保留，恢复后响应；新黑场立即撤销旧亮度，空场/关闭返回 null，背景/越界无旧邻居数据。
- Release 同一准备好的 Renderer 继续复用原 `quality_ddgi` 和 `probe_volume_cases`：发光面 NEE、无重复能量、移除/遮挡、接缝、动态发光体、薄墙和亚像素挡板、多体积有效黑色、共享预算、滚动/负坐标/跨轴/teleport、资源生命周期均通过。静态整墙 max mean=0.154 RGB8、P95=1；移动发光体 max mean=2.3724、P95=7；被挡区域红通道为 0。
- 独立地牢副本打开 3599 entities / 96 sources，恢复原相机及布局。在后墙选点以 48 个完成样本分别测 live / holdE / holdD，支持 E 输入为此点主要波动来源。此轮无独占 GPU 性能对照，不作帧率或功耗结论。
- 另一次原 `--scene-lit-volumes` 的 GPU-based validation 尝试在窗口被最小化后因不可接受帧中断，**不记为通过**；上述补跑使用 debug layer，不宣称该轮 GBV 通过。

本机证据在 `out/ddgi-stability-20261015/`：`final-{dev,release}/ddgi-stability.csv`、`final-combined.log` 和对应 PNG、`dungeon-before.json`、Release暖机后的`dungeon-after-settled.json`及`final-editor-capture.json`。Release副本通过正式Control检查与第128/192帧截图，仍有可测照度输入波动，不宣称全场根治或跨配置改善倍率。完整归因、短采样统计边界及尚未解决的运输输入方差见[研究记录第 10 节](../../research/ddgi-stability-and-lighting-roadmap-20261015.md#10-第一批落地与证据)。Astra 最终代码复审通过；修复的两项时钟接线和 GPU 浮点端点问题均已并入，旧 budget/total 呈现说明已删除。没有保存原用户 Scene，没有提交推送。

## 多 Astra 审查与工作剔除（2026-10-13）

来源为 `b0ede07d` 加 Pass Table 与本轮工作区改动。工业依据、审计发现、已实施与未实施边界见[研究记录](../../research/render-pipeline-audit-20261013.md)。本批不修改 RHI 原生实现，没有新测试框架或单元测试。

数值收尾后的 Dev `atelier_world_acceptance atelier_renderer_acceptance`、Release `atelier_world_acceptance atelier_renderer_acceptance atelier_editor` 构建通过；36 个受影响计算/顶点/像素入口用项目固定 DXC 离线编译通过。最新日志为 `out/render-audit-numeric-dev-build.log`、`out/render-audit-numeric-release-build.log`、`out/render-audit-numeric-shaders/compile.log`，后者逐项记录编译结果并保留 DXIL。此前 `out/render-audit-*-final-build.log` 与 `out/render-audit-shaders/` 对应数值收尾之前的代码。编译不等同于最终 GPU 验收。

World 的真实 Runtime / Source / JIT / 数据提取验收在 Dev、Release 均 **PASS（9/9）**，日志为 `out/render-audit-numeric-world-dev.log`、`out/render-audit-numeric-world-release.log`。新增覆盖解析光 `2^32` 乘积边界及相邻越界值、矩形面积乘积、高强度乘低颜色、65504 自发光边界、拒绝编辑的原子性和原生绕过 setter 后的提取检查。Astra 复审确认解析光计算域及 MIS 极大 target 的有序缩放；新增 Renderer 超界拒绝后保留 Capture / 历史的用例随后通过 Dev GPU 验收，见下节收尾证据。

新用例接入 `--scene-lit-contracts`：高/低身份字、SSSR 来源替换、选中连续区间与实际 draw 数、奇数 Hi-Z 与单像素视口、小数视口移动及效果重入。第一次 Dev GPU validation 执行已通过 DI/SSSR 身份、selected run 和 337×191 真实反射，随后严格小数视口检查失败。DI 揭露后的蓝通道与 reset/reference64 都为 237；SSSR 8152 个旧反射像素在首帧旧红色最大值为 0。证据在 `out/render-audit-dev-contracts/` 与 `out/render-audit-dev-contracts-diagnosis/` 及同名日志。

失败精确位于 viewport `{.25,.25,336.5,190.5}` 的 `(336,0)`：中心虽在逻辑视口内，深度和颜色仍为零。RHI 负责人回到 Direct3D §15.7 确认了硬件隐式向下取整裁剪；此后补入统一 `RasterViewport`、最终 SV_Position 补偿及 pixel-center scissor，未放宽原断言。两位 Astra 交叉复审覆盖补偿/历史坐标和 DDGI 单 target UAV 顺序依赖，未发现新可行动遗漏。上述为首错与定位记录；修复后的实际 GPU 结果见下节，不以这些部分通过替代最终验收。

性能使用原 1440p、96 预热、240 唯一完成样本、关闭 validation 且测量中不 Capture 的固定场景。退出游戏后的旧二进制基线 `out/render-audit-before-idle/` 总计 p50/p95 为 **9.47060/9.62108 ms**，直接光 4.80104 ms（resolve/filter 2.43232），DDGI 1.06304 ms（update .13728、gather .92440）。首次 `out/render-audit-baseline/` 在 ARC Raiders 占用约 97% GPU 时测得总计 p50 179.762 ms；后测 `out/render-audit-after-idle/` 和旧版复测 `out/render-audit-before-confirm/` 又遇到重新启动的游戏进程，结果均保留并标记竞争污染，**不构成有效提速证据**。

同一固定场景逻辑资源从 **1,758,393,304 B** 减至 **1,728,050,136 B**，减少 **28.9375 MiB**，对应删除全尺寸 Hi-Z 复制及当前探针池的第二份 target。该字节差是明确资源删除的结果，既不是物理显存峰值，也不证明毫秒或电耗改善。用户当时要求先完成其他工作、稍后再测性能，因此 2026-10-13 的数值收尾只执行构建、离线编译与 CPU 数据链验收；后续无竞争 A/B 和 GPU 收尾记录在下节。

### 2026-10-14 空闲 GPU 性能对照

用户确认空闲后，在同一 RX 6800 XT 上以旧→当前→当前→旧顺序执行 Release 固定场景，每次 96 帧预热、240 个唯一完成 GPU 样本，关闭 validation，计时期间不 Capture。源码、二进制哈希及日志保留在 `out/render-audit-20261014/`；`before-1/2` 为保留的旧二进制，`after-1/2` 为本轮审计候选。下表合并每组的 480 个样本，沿原 benchmark 的排序取下标规则计算分位数；没有平均单次 p95，也不把各 Pass 的 p50 相加当成整帧 p50。

| GPU 阶段 | 旧版 p50 ms | 当前 p50 ms | 差值 |
|---|---:|---:|---:|
| 已计时阶段合计 | 9.29928 | 9.33224 | +0.35% |
| 直接光与降噪 | 4.79392 | 4.87492 | +1.69% |
| └ 初始/时间复用 | 1.30132 | 1.43624 | +10.37% |
| └ 空间复用 | 0.80388 | 0.84596 | +5.23% |
| └ 可见性/着色 | 0.25524 | 0.25764 | +0.94% |
| └ 颜色 resolve/filter | 2.43200 | 2.33404 | −4.03% |
| DDGI | 1.05672 | 1.03116 | −2.42% |
| └ update/transfer | 0.13624 | 0.13176 | −3.29% |
| └ receiver gather | 0.91964 | 0.89900 | −2.24% |
| GTAO | 0.91080 | 0.96792 | +6.27% |
| SSSR/HDR | 1.42636 | 1.36528 | −4.28% |
| HDR resolve/表面历史 | 0.56428 | 0.55200 | −2.18% |
| 最终显示合成 | 0.41708 | 0.41452 | −0.61% |

合计 p95 为 **9.91382 → 9.93357 ms**。整体持平，不能把局部阶段的改善宣称为整帧提速。逻辑资源减少仍为 **28.9375 MiB**。该场景为 8 个共享 cube、1 方向光、2 点光、1 矩形光与网格自发光；结果不是地牢地图的性能，也不是 CPU 帧时、物理显存或电功率测量。独立 Pass 的尾部时长会受提交和系统调度影响，不能相加推断整帧尾部。

针对新增 MIS 保护的实测成本尝试过两个等价候选：极端分支重算 target 的 `optimized-1` 合计 p50 **10.1245 ms**，显式加权分量的 `final-1` 为 **9.36016 ms**，均未优于原审计候选，已完整撤回。`final-1` 目录名只是当时的候选标签，不表示最终采用。最终 ReSTIR shader 的 SHA256 与本次测试初始清单完全一致；拒绝的测量继续保留作归因。原有数值保护与历史身份检查没有为提速而删除。

首次本轮 Dev GPU 合同已通过小数视口覆盖、DI/SSSR 身份及奇数 Hi-Z，随后发现验收窗口在 150% DPI 下把请求的 1×1 客户区扩成 180×1。独立真实 Win32 窗口复现确认是装饰窗口的最小宽度；无装饰客户区可达到 1×1，但下一轮进一步确认 gfx 的窗口同步与逐帧 resize 路径将每轴保持在至少 8，即本例实际为 **8×8**。RHI 负责人确认首次创建和 headless 初始化没有这一相同限制，故不把它概括为硬件或所有 RHI 目标的下限。验收现使用无装饰 8×8 真实目标中的 **1×1 视口**，检查唯一接收像素、其余 63 个像素深度为零，以及反射回退保持；之后恢复窗口样式。`resize` 按实际样式计算客户区。没有修改 RHI 合同或绕过其入口；物理 1×1 后缓冲和单层 Hi-Z 特例不声称已经实机覆盖。两次失败记录保留在 `dev-contracts` 与 `dev-contracts-final`，用于说明夹具修正。新的 audit 已并入现有 `--scene-lit-restir-suite`，复用其 Renderer 并在关闭检查前运行。

最终 Dev `--scene-lit-contracts` 开启 GPU validation，**退出 0、stderr 为空**；audit、work elision、contracts 均 PASS。包括新增数值拒绝的 Capture/历史隔离、高低 64 位身份、SSSR 来源替换、选中连续区间、奇数 Hi-Z、小数视口及单像素视口、原光源范围/支撑、Mask/闭合几何、精确射线预算、DDGI scratch 大小变化、空场景退休与重复关闭。证据为 `dev-contracts-accepted/` 及同名 `.stdout.log`、`.stderr.log`、`.status.json`。

Release `--scene-lit-restir-suite` 开启 GPU validation，**退出 0、stderr 为空**，材质、材质光输运、272 个投影光源、独立参考能量、DDGI 多体积/滚动/反馈、发光网格质量、探针调试、上述 audit 和完整 contracts 均 PASS。证据为 `release-suite-complete/` 及同名日志与状态，运行于 16:27:41–16:45:54。此前 `release-restir-suite/` 虽产生了后段用例图像，但进程结束后没有最终退出记录且日志未落盘，不记为完整通过。收尾运行改用独立进程和原生日志文件句柄保存结果；它仅是 `out/` 中的本次执行辅助，不是另一套产品或验收入口。

随后 Release 原 `atelier_renderer_acceptance` 无参数全套开启 GPU validation，**退出 0、stderr 为空**。Studio Cavity、标准材质、跨近面/眼平面的选择裁剪、各着色与 AA 模式、线框/边线、参考网格、UI 覆盖/混合/裁剪、资源 revision、resize、最小化/恢复和生命周期均 PASS，覆盖统一 `RasterViewport` 对既有 Studio 路径的影响。证据为 `release-renderer-full/` 及同名日志与状态，运行于 16:45:54–16:50:41。两组串行完成后已通知应用与编辑器、RHI 负责人恢复 GPU 工作。

本次最终夹具的双配置构建日志为 `dev-min-target-build.log`、`release-min-target-build.log`；此前的 `dev-suite-build.log`、`release-suite-build.log` 对应组合入口接入 audit，`dev-accepted-build.log`、`release-accepted-build.log` 对应恢复实测采用的 shader。最终源码和二进制清单为 `accepted-source-sha256.csv`、`accepted-binaries-sha256.csv`，来源与完成状态汇总于 `completion-context.json`，均位于 `out/render-audit-20261014/`。审计起点为 `b0ede07d`；2026-10-14 采样清单记录的主线已为编辑器 UI 提交 `a0daaba`。原始源码清单对比确认渲染生产代码未变化，本轮最终变化是验收夹具、组合入口与记录。没有以本次 Renderer 验收替代新 UI 提交的编辑器交互验收，未自动提交或推送。

## Scene Lit Pass 消耗统计（2026-10-13）

来源为 `E:/FrostFoxAtelier` 的 `b0ede07` 加本批统计/UI 改动，RX 6800 XT。Dev/Release 的 `atelier_renderer_acceptance` 构建通过。新增的 4 个直接光和 2 个 DDGI update 时间区间、执行掩码与既有 8+2 区间/射线读回按同一完成序号一起发布；没有新增等待、同步读回或光照算法修改。父子时间的定义见[统计合同](scene-lit.md#诊断与性能边界)。

Dev `--scene-lit-contracts` 开启 GPU validation，退出 0、stderr 为空。现有真实 Renderer 用例新增检查：候选/参考切换后的执行掩码属于当前完成帧，关闭 DDGI 的所有相关区间跳过，直接光和 DDGI update 的细分时长有限且非负。该入口同时覆盖原参考、预算、空场重入、scratch 大小变化与重复关闭。证据为 `out/pass-cost-dev-contracts/` 及同名 `.stdout.log`、`.stderr.log`、`.status.json`。

Release 两次执行原 `--scene-lit-benchmark`，均为原生 2560×1440、8 个共享 cube、1 方向光/2 点光/1 矩形光及网格发光，关闭验证、预热 96 帧后记录 240 个唯一完成样本，测量期间没有 Capture。两次均通过全部区间非负、子区间之和不超过父区间和固定场景执行掩码 `0xffff` 的检查。

| GPU 区间 | 首轮 p50 ms | 同条件复测 p50 ms |
|---|---:|---:|
| ReSTIR 初始/时间复用 | 1.30104 | 1.28488 |
| ReSTIR 空间复用 | .80160 | .80364 |
| 当前可见性/着色，含显式太阳 | .25324 | .25204 |
| 以上三项逐样本合计 | **2.36524** | **2.33976** |
| 直接光辐射 resolve/filter | 2.43460 | 2.43648 |
| 直接光父阶段 | 4.80576 | 4.77360 |
| DDGI update/transfer | .14336 | .14488 |
| 其中：主射线/分类/重定位 | .03484 | .04152 |
| 其中：命中照明/反馈/卷积 | .08140 | .07908 |
| DDGI receiver gather | .90468 | .90348 |
| DDGI 父阶段 | **1.05836** | **1.04992** |

首轮八阶段合计 p50/p95 为 9.75076/66.226 ms，前 60 个测量样本存在明显长尾；复测为 11.5761/17.4206 ms，合成阶段仍有波动。保留全部样本，未事后裁剪首段、延长预热或宣称稳定帧时/提速。上述表格只描述本机该固定场景，不代表地牢；额外 timestamp 的净开销也没有独立 A/B 结论。两次逻辑资源峰值均为 1,758,393,304 字节，不是物理显存。

原始 CSV、摘要与图像分别在 `out/pass-cost-release-benchmark/`、`out/pass-cost-release-benchmark-repeat/`，逐样本 ReSTIR 合计摘要在 `out/pass-cost-measurement-summary.json`。编辑器表格消费同一完成样本；应用/窗口验收证据由[编辑器文档](../editor/README.md)维护。

主目录 Dev/Release 应用与 Control 构建、Scene Lit 文档链和最终 `--acceptance-surfaces` 均通过。性能表真实 E2E 收满 120 个完成样本，验证 DDGI 开启子项、关闭时 Skipped、父阶段独立合计、列宽刷新保持、Control 概要空值、选择不清窗，以及配置/目标尺寸/文档换域后拒绝旧样本。日志为 `out/performance-{dev,release}-verified.log`；对应目录保留 `world-performance-ddgi.png`、禁用/缩放/不可用截图和 `performance.json`。最终 Release 开启与缩放截图已人工核对，Target/View 与各列可读；这些小场景 UI 数字不冒充上述 1440p 房间成本。

## 原生 ReSTIR DI 与 DDGI 线性能量（2026-10-13）

来源为 `C:/Users/Arzuo/.codex/worktrees/viewport-rendering/FrostFoxAtelier`，基于 `30ca640` 的本批未提交候选，硬件为 AMD Radeon RX 6800 XT。Dev/Release 完整 GPU 组合均通过，实际地牢另作场景对照；固定房间基准不代表地牢帧时，也没有等价旧实现的提速结论。

组合入口为 `atelier_renderer_acceptance --scene-lit-restir-suite --output <绝对目录>`，在同一 Renderer 上依次运行材质/传输、ReSTIR、多体积、DDGI 质量/调试及 contracts，减少重复管线准备。组间恢复相机、视口、场景与显示状态，帧序号持续推进；含关闭检查的 contracts 最后执行，各独立入口保留。参考模式关闭全部相关历史时，材质用例只捕获所需的一帧，保留每帧 64 次独立估计及原 ROI/判据；DDGI 收敛用例仍保留原预热。

两配置均开启 GPU validation，退出 0、stderr 为空，完整证据为 `out/restir-fixed-{dev,release}-suite/` 与同名 `.stdout.log`、`.stderr.log`、`.status.json`。本批统一源身份与 PMF、显式方向光、reservoir 初始/时间与空间阶段、最终 fresh visibility，以及 DDGI 独立 RIS 和 FP32 线性照度历史；旧阴影槽、256 灯固定上限与 gamma EMA 已退役，Scene v5 保存候选预算，参考模式属于视口诊断。算法与边界见[实现说明](../../research/restir-di-implementation-20261012.md)。

| 验收面 | Dev/Release 实际结果 |
|---|---|
| 统一采样与能量 | 272 个投影源实时／参考 RGB8 均值为 `(208.105,196.241,189.926)`／`(208.828,196.939,190.768)`；重排、增删、接收面与光源运动后的均值判据通过。方向光 DDGI 混入 96 个无关局部灯后各通道差小于 .01。见 `restir-energy.csv`。 |
| 可见性与材质 | 超过旧限制的解析灯与 800 个发光三角形保持遮挡；不同剖分、镜像/非均匀变换、Mask、单双面发光及离屏纹理 NEE 通过。纹理修改与混合开放/闭合几何通过。 |
| DDGI 传播与质量 | 一跳发光无重复注入；开放面探针辐照度 81.2，均匀天光 64/1 射线均为 78。整墙静态差最大均值 .1599、p95 为 1；移动发光体最大均值 2.4484、p95 为 7；薄与亚像素隔墙暗侧红通道为 0。未放宽原图像判据。 |
| 调试与深度 | 体积滚动/传送、接收覆盖、球体、X-Ray、重定位、年龄、照度/距离、四种 AA 和正交视图通过；FP32 拾取深度逐值不变。调试只使用显示颜色与私有深度。 |
| 分块与寿命 | 实际 337×191 尾部覆盖的三个独立完成样本各有 4,119,488 次参考可见性查询，切回实时后各为 64,367 次，最右列/最下行均有光照。DDGI scratch `1×1 → 64×256 → 3×7 → 64×64`、全探针失活、空场重入、无效帧隔离、重复关闭通过。见 `work-elision-gpu.csv`。 |
| 产品与恢复 | Dev/Release `atelier_editor_document_acceptance --scene-lit`、Control `--repro-lighting` 通过旧格式严格迁移和新格式保存。应急作者 Scene 保全的双配置 CPU `--recovery` 通过；它不代替真实设备失败后的 Application 全链验收，边界见[场景保全](../editor/projects.md#异常退出时的场景保全)。 |

本轮修正了两个实现边界：历史源在时间入口归一到当前索引域，空间入口不再消费旧域资源；参考着色按 64×64 像素分派，全局坐标、随机序列与 64 次估计保持不变。DDGI 主射线与命中着色分为两个阶段，并修正空间调试误声明第七个 GBuffer 颜色附件的问题。Astra 复审覆盖索引/空实现的 MIS 责任、分块尾部、scratch 生命周期和完整身份附件链，未留新的实质发现。

早期候选的高负荷参考诊断曾 DEVICE_HUNG，另一候选曾报告映射表 stride 错误。描述符/寿命检查和 RHI 工具链 A/B 未建立驱动根因；没有修改生产 RHI、原生接口边界或工具链锁定。当前上述两配置完整组合及 Dev 独立 `--materials` 均未复现，不能把早期失败日志或任意一次分项通过改写为普遍设备稳定性保证。旧排障证据在 `out/restir-map-diagnostic-conclusion.txt`。

### 1440p 固定房间成本

Release `--scene-lit-benchmark`：2560×1440、8 个共享 cube 实例、1 个方向光/2 个点光/1 个矩形光及网格发光。关闭 GPU validation，预热 96 帧后采集 240 个去重的完成 GPU 样本；采样期间不 Capture，之后单独捕获证据图。结果为 GPU 阶段合计 p50 **9.40748 ms**、p95 **9.50392 ms**、p99 **9.52848 ms**；不包括 CPU Present、显示扫描或整个应用帧时。

各阶段 p50：直接光/降噪 4.79604 ms，DDGI 1.06496 ms（更新 .14084、接收 gather .92252），GTAO .90996 ms，SSSR 1.43260 ms，历史解析 .56840 ms，显示合成 .41972 ms。逻辑当前资源峰值 1,758,393,304 字节（约 1.64 GiB），不等同于驱动物理显存峰值。证据为 `out/restir-current-release-benchmark/benchmark-gpu.csv`、`benchmark-summary.txt` 和 `benchmark-1440p.png`。

### 已保存地牢的实景观察

本批 Release 编辑器另打开独立 Dungeons 副本，3617 Actor、96 个有效解析源，1920×1260 客户区中的视口约 1126×648。普通 Lit、关闭 DDGI、接收支持与探针球均通过 Control 捕获，stderr 为空。原保存文件未修改；此版本保留屋顶，不能冒充用户后来未保存的场景。

室内图仍暗。墙面取样区约 99.93% 有非零 GI 支持，但开启 DDGI 的 RGB8 均值增量只有 `(.5283,.3638,.0007)`；地板取样区约 36.97% 为贡献候选全遮挡。该结果显示低能量和局部支持缺口同时存在，没有证明全场景传播质量已经解决。实际太阳强度 .2、多数点光强度 1/范围 5 米、无天光；本轮不通过修改这些作者值或曝光来隐藏问题。

完整相机、布局、取样区与归因边界见[地牢支持研究](../../research/ddgi-dungeon-support-20261012.md#5-当前实景证据与后续归因边界)。本机证据为 `out/restir-dungeon-compare.json`、`out/restir-dungeon-interior-compare.json` 和 `out/restir-dungeon-observations.json`，结果文件指向原始图像与 FP32 深度。捕获中的单帧 GPU 时间不作为地牢稳定帧时。

下文的四灯预算、`shadow_samples` 和旧照度历史结果属于各自注明的历史候选；它们是回归来源，不代表当前管线。既有用户窗口的地牢捕获也不是本批新候选证据。

## DDGI 覆盖与反馈演进（2026-10-12）

来源为主目录 `E:/FrostFoxAtelier` 的本批工作树，AMD Radeon RX 6800 XT。本批没有调整 RHI 或建立另一套图形 API 入口；保留 FP32 深度与距离矩，重定位 offset/state 改为 RGBA32F，在 4096 个探针的双缓冲下增加 64 KiB 逻辑字节。

- 最终 Dev/Release `atelier_renderer_acceptance --scene-lit-volumes --output <绝对目录>` 均退出 0，启用 GPU validation。覆盖共享驻留/更新预算、真实接收面来源/贡献数/主导程度、亚格历史保留、正负多轴滚动、传送、重复配置、有效暗部、最终增益与反馈运输分离、全关重开、非法配置拒绝后继续渲染。
- 小体积/大密闭房间对照通过：缓存之外约 20 米处的墙仍遮挡非零天光，真实有效暗部显示为青色。缓存 bounds 不再充当场景照明射线的终点。
- 同一 Renderer 继续通过原 mesh emission NEE、整墙/墙脚、连续移动发光体、薄与亚像素隔墙，以及完整空间诊断回归。两配置的整墙静态差异最大均值均为 .0831、p95 为 1；移动发光体最大均值 1.3538、p95 为 4；薄/亚像素隔墙暗侧红通道均为 0。以上均为既有图像判据，未放宽门限。空间诊断后 FP32 拾取深度逐值不变，覆盖四种 AA、正交、空场、最大探针池和资源寿命。
- 日志为 `out/ddgi-evolution-final-{dev,release}.log`，完整图像在对应 `out/ddgi-evolution-final-{dev,release}/`。已人工核对多体积来源和亚像素隔墙图。该最终候选包含 Astra 提出的全部生产修正，早期 Dev 多体积单项结果不代替此联合回归。

Astra 分别审阅空间身份/生命周期、反馈传播和成本。已修正跟随体积缺少完整内部区域、世界关注点精度、局部缓存截断远墙、新切片抢占导致远层响应期过早结束等问题；应用发布前校验与旧版 repro 默认基底由应用负责人接齐。报告位于 `out/ddgi-astra-20261012/`。代码审查不替代 GPU 与真实编辑器验收。

应用接线的 Dev/Release 文档、正式 surfaces 窗口与 Control `--ddgi-debug` 均 PASS，日志为 `out/ddgi-panel-{document,surfaces,control}-{dev,release}.log`。覆盖四槽 UI 编辑与取消、配置幂等、非法世界关注点精度的原子拒绝、规范字段/旧别名冲突、四槽导出恢复及真正缺少所有新增字段的旧版 repro。Astra 最终复核确认发布链、相机 target 入口和旧文件默认基底的问题已关闭。

### Dungeons 同预算布局对照

正式 Release 编辑器在独立 Dungeons 副本中对照，未关闭或写入用户原窗口。两布局均为 **2911 个探针、每帧更新 256 个、每次 32 条照明射线**，每个 Lit / status / volume case 在第 72 个接受帧捕获（六个完整预算轮转），未借增加预算获得覆盖改善。

- Scene + Follow Focus：关注点约 `(3.609,-1.381,33.788)`。细层实际 bounds 为 `(-3,-4,27)..(9,2,39)`，`13×7×13`、间距 1 米；粗层为 `12³`，bounds `(-105,-18,-163)..(63,32,92)`，间距约 `(15.27,4.55,23.18)` 米。画面地板约在 y=-4.9，低于细层下界，只能依赖粗层；近景地板和部分远景出现遮挡支持缺口。
- 保持总数与预算，将同一个 slot 0 改为局部固定楼层 `(-6,-5.5,28)..(12,-2.5,52)`、仍为 `13×7×13`，间距 `(1.5,.5,2)` 米；slot 1 粗层保持。地板诊断从大量遮挡/零辐照度转为连续有效贡献，来源也从粗层转为局部层。
- 同相机固定地板 ROI `(300,630)..(850,745)` 共 63,250 像素，按正式 status 色分类：全部候选被遮挡从 **38.417% 降至 .114%**，有效零辐照度从 61.554% 降至 .996%，有非零 GI 支持从 .028% 升至 **98.890%**。没有未分类像素。此结果描述该区域的支持状态，不是全关卡照明质量、真实辐照度或收敛速度的保证；不同进程/配置不保证相同 GPU 历史和随机序列。

原近/远视图证据在 `out/ddgi-panel-dungeons/89db6b1086d64de3bd395acfde432a3c/`，局部楼层在 `out/ddgi-panel-dungeons/4b782452264c419888e9986fbb34a162/`，各 `result.json` 指向实际图像、相机、FP32 深度、有效配置和统计。分类脚本与结果为 `out/ddgi-layout-compare.py`、`out/ddgi-layout-comparison.json`。

普通 Lit 仍整体偏暗，上方墙面和远处仍有粗层遮挡缺口。这次实景证明“将密度投向实际楼层”能在相同预算下改善支持，不能把 Scene + Follow Focus 预设称为全建筑自动覆盖方案。捕获中的单个 GPU 时间样本不作为稳定帧时或提速结论。

## 选中边线深度一致性（2026-10-11）

`E:/FrostFoxAtelier` 的 `72843f7` 加本轮修改，RX 6800 XT：Dev 的 `atelier_renderer_acceptance --selection --output <绝对目录>` 和 Release 完整 `atelier_renderer_acceptance` 均返回 0，启用 GPU validation。日志分别为 `out/selection-final-dev.log`、`out/selection-renderer-release-full.log`。Release 同时覆盖原材质、Cavity、参考网格、UI、尺寸变化与资源寿命检查。

改前用倾斜表面、非均匀变换与远离原点的相机复现：同一参考图的 902 个强边线像素中，Off/SMAA 仅保留 864 个；TAA/T2x 为 895 个，记录在 `out/selection-red-translated.log`。统一位置计算后，八个相机位置的四种 AA 均达到参考边线至少 98% 覆盖；在原失败位置均为 895/902。另验证相距 2 毫米的前方表面完全遮住选中边线，选择不改变 FP32 深度。既有 15 组近面/相机平面裁剪、极端尺度和镜像场景继续通过，没有表面铺黄。

正式 Dev 编辑器通过 Editor Control 加载官方 `SM_Prop_Barrel_02` OBJ、纹理与真实光源，捕获 Off/SMAA/TAA/T2x 各第 1、4 个接受帧。约 31914 个强黄色像素中，各捕获与首帧的掩码差异为 0～4 个，没有截图所示的大片断线。可见线条的截图已人工检查；这不代替任意相机轨迹或任意重叠几何的保证。独立工程、请求记录和捕获索引保存在 `out/selection-barrel/`，其中 `capture-result.json` 指向完整颜色、深度与元数据。

## DDGI 诊断（2026-10-11）

来源为 `codex/viewport-rendering` 工作树，基线 `72843f7a24d51542620f5bebedb83433cfca914b` 加本批未提交改动。硬件为 AMD Radeon RX 6800 XT。Dev/Release 受影响 Renderer、Editor、Control 验收目标均编译通过；没有调整 RHI 或调用另一套图形 API。

主目录 `E:/FrostFoxAtelier` 已整合这批功能与选中边线、目录分隔条、原生对话框修复。整合后的 Dev 与独立 Release（`out/dialog-release`）均通过 Renderer `--scene-lit-debug`、`--selection`、Editor `--acceptance-surfaces` 和 Control `--ddgi-debug`；日志位于 `out/ddgi-integration-20261011/{renderer,selection,surfaces,control}-{dev,release}.log`。新增交集检查覆盖四种 AA 下探针叠层之后选中边线仍完整、FP32 深度不变，以及实际 DDGI 子菜单的互斥/勾选操作与场景历史隔离。这里的 Release 证据来自独立发布构建；标准 `out/release/atelier_editor.exe` 在本轮链接时被用户未保存窗口占用，未将其关闭或覆盖。

| 实际入口 | 结果与范围 | 本机证据 |
| --- | --- | --- |
| Dev `--scene-lit-debug`，GPU validation | PASS：真实八邻 pending/有效零照度/体积外诊断、重定位后的 inside 球、Volume、X-Ray、年龄、方向辐照度/距离、TAA/T2x/正交、空场景、深度不变和历史隔离 | `out/ddgi-debug-dev.log`、`out/ddgi-debug-dev/` |
| Release `--scene-lit-debug`，GPU validation | PASS：上述范围，另覆盖首次空场显示、4096 探针显示、窗口缩放与非法半径后继续渲染 | `out/ddgi-debug-release.log`、`out/ddgi-debug-release/` |
| Dev 正式编辑器 `--ddgi-debug`，正常启动 | PASS：共享 Control schema、重复 patch 不改变 state token、非法 patch 原子拒绝、四组真实 render.compare、基础 view/Actor/Undo 恢复、原场景文件不变、干净关闭 | `out/ddgi-control-dev.log`、`out/ddgi-control-dev/fbd82ae72930994ea0e62b134e11061b/` |
| Release 正式编辑器 `--ddgi-debug`，正常启动 | PASS：与 Dev 相同的完整 Control 链，独立编辑器干净关闭 | `out/ddgi-control-release.log`、`out/ddgi-control-release/73d10115395a3744ae891708069a5f6e/` |

图像检查包括 `debug-02-coverage-dark-outside.png`（青色为有支持但零照度，蓝色为超出体积）、`debug-05-xray.png` 和 `debug-12-inside.png`（红球位于真实 offset 位置，紫线连接原格点）。原接收面伪彩进入光照路径的实现已退役；生产 gather 与诊断共用查询代码，空间绘制使用私有 D32_FLOAT，真实场景 Capture 深度逐值保持一致。

首轮 inside 断言误把原格点作为重定位后球心，已根据实际显示和探针合同修正为验证实体内的实际红球；未修改生产分类或放宽照明质量门限。Control 的非法数值沿既有 `operation_failed` 合同拒绝，验收修正了错误码预期；没有修改协议。GPU validation 的首次 SMAA 驱动 PSO 准备较慢，调用栈定位在既有 SMAA dispatch；本批未改 SMAA。该准备耗时、同步截图耗时均不作为 GPU 帧时或性能提升证据。

这些用例验证诊断设施，不代表已完成用户未保存 Dungeons 黑区的逐像素归因、滚动 DDGI 或 ReSTIR DI。

## 工作室 Cavity（2026-10-11）

本轮沿用 `a7ab99f` 基线的 `codex/viewport-rendering` 工作树、RX 6800 XT 与真实 GPU validation。`cavity.inl` 加入现有无参数 `atelier_renderer_acceptance`；`--cavity --output <绝对目录>` 可单独运行同一用例并导出图像，不建立另一套 Renderer。

最终 Dev/Release 全量均退出 0，包含 Cavity、材质/AA、贴图 DDGI 与离屏发光传输、原 Renderer 生命周期和 15 组选中裁剪。日志为 `out/cavity-dev-full.log`、`out/cavity-release-full.log`；两份均未报告 D3D12 warning/error。Dev 直接执行已构建程序，用时 359.2 秒；Release 通过正式 `renderer_acceptance` 目标，用时 333.8 秒（含增量构建）。这些是本次命令耗时，不作两配置性能对比。最终折面、方块/球体的开关对照在 `out/cavity-evidence/dev/` 与 `release/`。

应用负责人完成同一工作树的 Dev/Release `--viewport-edit` 与 `--acceptance-shortcuts`，覆盖 Cavity 控件、视角导航/饼菜单、菜单勾选更新、停靠缩放及原快捷键链；证据为 `out/viewport-document-<配置>.log` 和 `out/viewport-navigation-<配置>.log`。本轮没有操作或关闭用户的未保存编辑器。

完整入口 `renderer_acceptance` 的进程预算更新为 600 秒，保留全部 GPU validation 与断言。原 120 秒在本机首个 Lit 帧阶段截断了正常推进的验证；该次超时保留在 `out/cavity-dev-initial-timeout.log`，不记为通过。首帧初始化和整套验收时间不用于推断稳态帧率。

用例覆盖 MatCap/Smooth/Flat 的凸折面提亮、凹折面压暗、ridge/valley 独立控制、零强度旁路、0.5/1/4 像素半径和静态连续帧一致性。另有真实方块、球体与地板图像，检查效果变化及 FP32 深度逐值不变；屏幕凹凸增强不产生世界空间接触阴影。

背景、视口外边距和 UI 与关闭效果时逐字节一致；纯线框与 Scene Lit 跳过效果。Lit 对照使用相同 `camera_cut` 起点，避免把光照自身的历史变化误归因于 Cavity，并要求颜色、深度与 pass 数完全一致。窗口缩放覆盖接受旧尺寸、跳过过期尺寸、接受新尺寸的完整路径；另检查透视/正交、非法参数的编码前拒绝、清空场景和重复关闭。

默认 Cavity 在表面合成之后绘制选中边线，因此该状态比未选中时多一次 scene draw；旧 UI 缓存用例已同步实际绘制计数，上传次数、字节数和退休检查保持不变。临时颜色/法线目标为每窗口像素 12 字节，另加一次全屏合成；未选中且没有 Overlay 的批次跳过后置边线绘制。没有新增深度目标、随机采样或时间历史，也没有此次 GPU 毫秒或 FPS 收益的测量。

## 标准材质（2026-10-11）

本轮基于 `a7ab99f` 的 `codex/viewport-rendering` 工作树，RX 6800 XT。当前 Dev/Release 的 `atelier_renderer_acceptance` 均返回 0，启用真实 D3D12 GPU validation。证据为 `out/material-renderer-dev-full.log`、`out/material-renderer-release-full.log`，图片分别位于 `out/material-evidence/dev-full/` 和 `release-full/`；后续对渲染代码的更改须重新注明对应证据，不能沿用本段结论。

材质消费入口在原 `materials.inl` 中扩展；`--materials --output <绝对目录>` 可只运行材质与光照传播用例。检查包含：

- 材质分区、负 UV、sRGB 与独立线性 opacity、MatCap/Smooth/Lit、四种 AA 的 Mask 和公开 FP32 深度一致性。
- 镂空在相机与真实阴影射线中一致；粗糙度/金属度打包图与相同常量响应匹配；MikkTSpace 法线图改变着色。
- 贴图内容替换更新可见 emission；单面/双面 emission 的相机与 mesh NEE 方向一致，含镜像变换。
- 离屏贴图发光仍照亮真实接收面；发光图从红换蓝后对应照明变化，完全裁弃的发光面不产生照明。
- DDGI 命中读取贴图颜色；中性接收区相对关闭 DDGI 的 8-bit RGB 均值增量，红墙为 `(6.140, 0.950, 1.099)`，蓝墙为 `(5.240, 3.025, 12.587)`。这证明颜色传播，不是物理真值或性能测量。
- 同一 Mesh 加入不相连的 Mask 三角形不改变原封闭墙体的 DDGI 支持；随后只换贴图内容，GI 跟随新颜色。
- 原有共享网格、UI、invalid-frame 恢复、resize/minimize、资源退休及 15 组选中裁剪场景继续通过。各组选中表面内部改变/铺黄仍为 `0 / 0`。

Release 还通过 `--scene-lit-temporal`（`out/material-temporal-release.log`）：材质/发光变化、移动阴影、红/灰反射、Local GI、T2x 来源稳定性与分通道隔离均通过；红色反射的非受光绿色/蓝色最大值保持 0。该检查覆盖本轮着色法线/几何法线分离对既有历史链的影响。

Dev/Release 的完整 `atelier_editor_document_acceptance`、`--materials` 及 `atelier_project_scene_acceptance` 均通过，分别记录在 `out/material-editor-document-<配置>.log`、`material-resources-<配置>.log` 和 `material-project-scene-<配置>.log`。除共享/实例参数外，还检查了取消草稿、无效值回退、幂等绑定、复制实例、换网格槽对齐、旧场景兼容与关闭 Runtime 后的快照所有权。点拾取专项验证透明孔洞、负 UV 镜像和单面正/背面。

真实 Release 编辑器的独立工程位于 `out/material-evidence/editor-preview/Project`。Control 实际完成图片导入、资产创建、两物体共享绑定、实例粗糙度覆盖、共享颜色传播、两条撤销历史、无效值回退、保存/重开及旧句柄拒绝。请求和响应在 `control-transcript.jsonl`；64 个接受帧后的 Scene Lit 全窗口截图由 `capture-result.json` 指向。截图中左侧粗糙度覆盖为 0.85，共享资产为 0.3，棋盘纹理使用 2 倍 UV 重复。场景使用显式方向光与 Sky Light，没有操作用户原编辑器。

收尾审查同时覆盖资源发布、失败回滚、复制与保存、缓存命中和混合实例覆盖。Dev/Release 的材质资源、Textured OBJ/Sponza、Project Scene 定向验收已通过（`out/material-review-{resources,textured,scene}-<配置>.log`）：含 `./` 与合法 `..` 的创建路径、重复创建保护、越界拒绝、持久材质拒绝匿名纹理且保留旧状态、内联材质复制后的独立编辑/Undo/Redo/保存重开，以及退化 UV 与 +X 作者法线的稳定正交切线。限定代码交叉复审未发现新的阻断问题；没有模拟文件系统或 GPU 失败。

收尾 Dev/Release `--materials` GPU 验收均通过（`out/material-review-renderer-<配置>.log`，图片在 `out/material-review-evidence/<配置>/`）。新增同一 Mesh 的 Mask/Opaque 实例共享、交换顺序、移除 Mask 后仅保留 Opaque、重新恢复 Mask 的连续帧检查，光栅深度和阴影透明孔洞均正确；原材质、DDGI 和离屏发光用例继续通过，颜色传播均值与本节前述值一致。正式 Editor 的双配置构建及停靠/快捷键验收由应用负责人完成，另见[编辑器记录](../editor/README.md)。

无绑定材质的稳定捕获不再查询引用字段；RT 不透明性聚合复用几何缓存，消除每帧临时哈希表。旧的逐材质绑定通路已退役，重复的 Lit 标量验证由 Renderer 边界统一承担，未使用的 shader 中间字段已删除。上述减少工作量是代码事实，尚未换算为受控帧率收益。

本段不宣称纹理流送、大规模材质压力测试或已测得 FPS 提升。原始无贴图光照质量/性能记录的场景条件保持在对应旧章节，不与新材质工作量直接比较。

## 跨相机平面的选中边线铺黄（2026-10-10）

基线为 `5d24e4a6ab10b954c3bda03ffdfea674e939b12d`，修复提交 `a7ab99f9c0356e2b31aa96b6317e760b3e834c8e`，证据根目录 `out/selection-clipping-20261010/`。用户 Sponza 截图的近处整面铺黄，用同一真实 Renderer、RX 6800 XT 和 GPU validation 缩减为一个两三角形地板复现；未操作或重启用户编辑器，也未修改场景数据。

原 shader 把无透视插值重心坐标当作非负距离。顶点跨到相机背后时，屏幕重心坐标可变负，`smoothstep` 把该区域当成完全覆盖的边线。实际失败 `red.log`：前端顶点 z=-1 时，远离全部原始边的 22,528 个内部像素全部改变并成为黄色；顶点位于 z=0、.04 和 .11 时，该内部区域没有铺黄，近裁剪面为 .1。对应原始捕获为 `red/selection-clipping-0.ppm`，无损 PNG 转换为同名 `.png`。

最终保留原始网格边高亮与显式 wireframe 的共同语义。VS 将原始三角形重心坐标和 clip w 放入同一透视插值字段；PS 使用 `barycentric / clip_w` 恢复屏幕仿射边函数，再按梯度求绝对像素距离。梯度先按最大分量归一化，避免极大／极小投影尺度在平方时溢出／下溢。不新增 RT、draw、CPU 缓冲或公共 API；增加一个插值标量和局部 shader 计算，未测量 FPS 收益。对象 mask/ID 轮廓与边线的取舍见[显示合同](contract.md#视口显示策略)。

`selection.inl` 加入现有无参数 `atelier_renderer_acceptance`，没有另建测试入口。15 组真实 GPU 场景覆盖跨眼平面／恰好 w=0／近裁剪两侧、Smooth／Flat／MatCap／Scene Lit、Scene Lit 的四种 AA、1e-20／1e20 同构场景尺度、镜像和 8 像素宽线。每组检查选中内部颜色保持、原始边仍可见且宽度有界、显式三角形模式恢复对角线、Overlay／Lines Visible／Lines All 均不铺面，以及 FP32 捕获深度逐值保持。现有材质／Alpha Test、遮挡、正交与透视线宽、参考辅助显示、resize／最小化恢复和资源生命周期验收也继续执行。

| 最终真实 GPU 结果 | Dev | Release |
|---|---:|---:|
| 各场景内部改变／铺黄像素 | 0 / 0 | 0 / 0 |
| 默认选中远边可见像素（表面内侧） | 1～2 | 1～2 |
| 8 像素宽线在表面内侧的可见像素 | 4 | 4 |
| 新裁剪场景与原 Renderer 全部验收 | PASS | PASS |

最终构建／验收为 `dev-final-build.log`、`dev-final.log`、`release-final-build.log`、`release-final.log`，配对选中／未选中捕获在 `dev-final/` 和 `release-final/`。最终源码不含临时诊断输出，构建及 GPU 日志没有编译诊断或 D3D12 warning/error。本轮验证原生 Renderer 场景，未声称已经操作用户未保存的 Sponza 现场。

编辑器集成复验使用独立 Release 进程打开仓库内的 Official Examples，通过 Control 恢复从原窗口只读取得的相机状态，设置 Smooth 并选中 Sponza Atrium。实际 GPU 捕获中，近处地板保留材质，原始边继续可见，未出现整面铺黄；它使用官方示例的场景数据，不包含用户未保存的其他对象。原窗口和未保存内容保持不变。Dev／Release 的 `editor_surfaces_acceptance` 与 `editor_shortcuts_acceptance` 同时验证停靠、菜单与视口输入集成。来源是应用集成工作树 `C:/Users/Arzuo/.codex/worktrees/2a7e/FrostFoxAtelier` 的 `out/release/surface-evidence/selection-sponza.png`，不是渲染专项目录中的捕获。

中间结果保留用于归因：`fix.log` 的首版消除了铺黄，但宽线检查发现把系统位置 w 当作倒数的假设导致宽度失真；显式插值 clip w 后修正。`dev.log` 还暴露验收颜色判据错误：浅色 MatCap 的边线主要降低蓝通道，不能要求红绿同时提高 20；最终比较任意 RGB 通道的实际变化，并保持线宽约束。`diagnostic*.log` 属于临时 GPU 数值探针的预期失败，不作为通过证据。

## 跨相机平面的选中边线铺黄（2026-10-10）

基线为 `5d24e4a6ab10b954c3bda03ffdfea674e939b12d`，证据根目录 `out/selection-clipping-20261010/`。用户 Sponza 截图的近处整面铺黄，用同一真实 Renderer、RX 6800 XT 和 GPU validation 缩减为一个两三角形地板复现；未操作或重启用户编辑器，也未修改场景数据。

原 shader 把无透视插值重心坐标当作非负距离。顶点跨到相机背后时，屏幕重心坐标可变负，`smoothstep` 把该区域当成完全覆盖的边线。实际失败 `red.log`：前端顶点 z=-1 时，远离全部原始边的 22,528 个内部像素全部改变并成为黄色；顶点位于 z=0、.04 和 .11 时，该内部区域没有铺黄，近裁剪面为 .1。对应原始捕获为 `red/selection-clipping-0.ppm`，无损 PNG 转换为同名 `.png`。

最终保留原始网格边高亮与显式 wireframe 的共同语义。VS 将原始三角形重心坐标和 clip w 放入同一透视插值字段；PS 使用 `barycentric / clip_w` 恢复屏幕仿射边函数，再按梯度求绝对像素距离。梯度先按最大分量归一化，避免极大／极小投影尺度在平方时溢出／下溢。不新增 RT、draw、CPU 缓冲或公共 API；增加一个插值标量和局部 shader 计算，未测量 FPS 收益。对象 mask/ID 轮廓与边线的取舍见[显示合同](contract.md#视口显示策略)。

`selection.inl` 加入现有无参数 `atelier_renderer_acceptance`，没有另建测试入口。15 组真实 GPU 场景覆盖跨眼平面／恰好 w=0／近裁剪两侧、Smooth／Flat／MatCap／Scene Lit、Scene Lit 的四种 AA、1e-20／1e20 同构场景尺度、镜像和 8 像素宽线。每组检查选中内部颜色保持、原始边仍可见且宽度有界、显式三角形模式恢复对角线、Overlay／Lines Visible／Lines All 均不铺面，以及 FP32 捕获深度逐值保持。现有材质／Alpha Test、遮挡、正交与透视线宽、参考辅助显示、resize／最小化恢复和资源生命周期验收也继续执行。

| 最终真实 GPU 结果 | Dev | Release |
|---|---:|---:|
| 各场景内部改变／铺黄像素 | 0 / 0 | 0 / 0 |
| 默认选中远边可见像素（表面内侧） | 1～2 | 1～2 |
| 8 像素宽线在表面内侧的可见像素 | 4 | 4 |
| 新裁剪场景与原 Renderer 全部验收 | PASS | PASS |

最终构建／验收为 `dev-final-build.log`、`dev-final.log`、`release-final-build.log`、`release-final.log`，配对选中／未选中捕获在 `dev-final/` 和 `release-final/`。最终源码不含临时诊断输出，构建及 GPU 日志没有编译诊断或 D3D12 warning/error。本轮验证原生 Renderer 场景，未声称已经操作用户未保存的 Sponza 现场。

编辑器集成复验使用独立 Release 进程打开仓库内的 Official Examples，通过 Control 恢复从原窗口只读取得的相机状态，设置 Smooth 并选中 Sponza Atrium。实际 GPU 捕获 `out/release/surface-evidence/selection-sponza.png` 中，近处地板保留材质，原始边继续可见，未出现整面铺黄；它使用官方示例的场景数据，不包含用户未保存的其他对象。原窗口和未保存内容保持不变。Dev／Release 的 `editor_surfaces_acceptance` 与 `editor_shortcuts_acceptance` 同时验证本批停靠、菜单与视口输入集成。

中间结果保留用于归因：`fix.log` 的首版消除了铺黄，但宽线检查发现把系统位置 w 当作倒数的假设导致宽度失真；显式插值 clip w 后修正。`dev.log` 还暴露验收颜色判据错误：浅色 MatCap 的边线主要降低蓝通道，不能要求红绿同时提高 20；最终比较任意 RGB 通道的实际变化，并保持线宽约束。`diagnostic*.log` 属于临时 GPU 数值探针的预期失败，不作为通过证据。

## 提交后 Review 补强（2026-10-10）

基线提交为 `c8293e9`（移动光照历史与此前 Scene Lit 演进），补强提交为 `ca4af3344f09a3fc2cfd45b11bf6fdf895d412d0`，已进入 `5d24e4a6` 集成基线。证据根目录 `out/lit-review-hardening-20261010/`。Astra 分别审查需求符合度、生命周期／规范与性能路径；主执行方负责集成和真实端到端验收。审核补强不改变 RHI 边界、FP32 深度或原生分辨率。

需求审查确认：原位关闭 emission、等亮度换色、材质粗糙度跨过 SSSR 上限和修改反射上限，都会保留不再有效的旧反射。新增 `scene_lit_signal_edits.inl` 沿原 `--scene-lit-temporal` 入口隔离这些信号，使用同一终点清历史后收敛的参考图；比较只包含实际存在反射的接收像素。相对误差定义与下节移动验收相同，不表示残留能量比例。

| 编辑后 16 帧的相对图像误差 | 修改前 `red.log` | 修复后 `fix-v2.log` |
|---|---:|---:|
| 关闭自发光 | .8879 | 0 |
| 等亮度红色改蓝色 | .8094 | .1401 |
| 材质 roughness .58 → .65，SSSR 上限 .6 | .7723 | 0 |
| SSSR roughness 上限 .6 → .1，材质 .18 | .9445 | 0 |

原位材质编辑使该对象的着色对应失效一帧，保留几何运动；反射旧来源验证也消费此标记。分类、解析、空间重建共用反射资格判断，超出范围即时回退环境并清除来源。两个 roughness 用例在编辑后的 0／1／2／4／8／16 帧均与 SSSR Off 参考一致，RGB8 最大误差为 0，FP32 深度逐值相同。等亮度换色仍需短暂收敛，实测静态噪声为 .0329；原 .25／.15 或三倍静态噪声的 8／16 帧门限未放宽。

规范与生命周期审查修正了两处 CPU 链路：非 Sky 组件 attach 无须为天光唯一性额外扫描所有对象；新增解析灯时显式标记 SceneSnapshot 变化。正式文档链现在验证灯光创建、禁用、重新启用各使 scene revision 增加一次，重复 refresh 保持幂等，且不改变天光环境。原有其他 presentation refresh 成本仍在，不能据此次局部扫描消除宣称整个 attach 流程降为线性。

性能补强包括跳过纯复制的三个直接光滤波通道、SSSR 关闭时省去 HiZ／工作列表准备／反射滤波、确定性光源样本复用、无漫反射贡献时跳过 DDGI 接收查询。`scene_lit_work.inl` 加入既有 contracts，按完成序号去重，每种条件采集三个实际 GPU 查询样本；Dev 与 Release 计数一致：

| 固定接收面场景 | 每帧接收／可见性查询 | 每帧探针主查询 |
|---|---:|---:|
| 零半径点光，1／8／64 样本 | 均为 441,408 | 0 |
| 零角半径方向光，1／8／64 样本 | 均为 441,408 | 0 |
| 有限半径点光／方向光，8 样本 | 3,531,264 | 0 |
| DDGI，纯金属或纯黑漫反射，Local GI Off | 0 | 6,144 |
| DDGI，纯金属但 Local GI On | 3,531,264 | 6,144 |
| DDGI，基础色仅 1e-5 的非零漫反射 | 3,531,264 | 6,144 |

同时验证确定性光源的亮度与深度、有限面积样本、探针调试显示和世界空间探针继续更新。逻辑 dispatch／读写节省的计算见 [Scene Lit](scene-lit.md#诊断与性能边界)，本轮没有独占 GPU 帧耗时或 FPS 的前后对照。保留可复用纹理也不等于释放显存。

| 配置／入口 | 结果 | 日志 |
|---|---|---|
| Dev `--scene-lit-temporal`，GPU validation | PASS：以上四项编辑、移动拖尾、无关运动、T2x 长时来源与原时间隔离 | `fix-v2.log` |
| Dev `--scene-lit-contracts`，GPU validation | PASS：查询计数、几何／灯边界、成员变化、无效帧隔离、空场景退休／重入、重复关闭 | `dev-contracts.log` |
| Release `--scene-lit-quality`，GPU validation | PASS：四种 AA、反射／半影、连续编辑、SSIL、DDGI 接缝／隔墙、上述 contracts | `release-quality.log` |
| Dev／Release `atelier_editor_document_acceptance --scene-lit examples/scene-lit` | PASS：真实 World／Project／Scene、快照版本、Sky Light 唯一性、撤销、临时保存重开 | `dev-document.log`／`release-document.log` |

Dev 时间验收中移动后 16 帧误差为：面光阴影 .1015、红色反射 .0663、等亮灰色反射 .0367、Local GI .0638；纯红反射的未发光绿蓝通道最大值为 0。最终构建及 GPU 日志没有编译诊断或 D3D12 warning/error。`red.log` 是实际失败证据；`fix-build.log` 是中间 C++ 缩进诊断导致的构建失败，最终构建证据为 `fix-v2-build.log` 与 `release-build.log`。

Astra 已复审最终材质／几何对应、来源验证、资格边界、设置依赖失效及跳过通道的输入有效性，未发现本批阻断项。仍需独立演进的边界：单个来源见证不能完整表示粗糙波瓣中的多个光源，次要来源改变而主要来源不变尚缺专门验收；解析灯仍上限 256，满分块的全灯回退与动态实例／发光分布重建成本值得在更大场景中采样。本轮没有实现多来源历史模型或大规模灯光调度。

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

## Sponza DDGI 场景接入（2026-10-10）

基于 `17996cee07bcb234860848e31d6717fac3dec335` 加本轮工作树修改，在 Windows x64 / AMD Radeon RX 6800 XT 验证。复用现有 Scene Lit/DDGI shader，新增场景灯光、启动配置及真实编辑器验收。该轮的 `Open-Sponza.ps1` 默认进入 Scene Lit（该历史入口现已由编辑器默认启动及 Project → Examples 替代），方向光、暖色可移动点光和冷色点光均来自实际 World；建筑使用线性基础色 .55、粗糙度 .85。

启动时按完整建筑拟合的探针体积为 `[-20,-2,-13]`～`[19,15,12]` 米，`16×10×14 = 2240` 个探针，每帧更新 560 个，每次更新 128 条照明射线加 32 条固定几何射线。验收逐顶点检查覆盖范围；普通相机配置保留已有体积。整个模型仍为 262,267 个三角形、103 个材质组，24 个漫反射贴图材质的基础显示与场景序列化往返继续通过。

Dev / Release 的正式编辑器及文档消费者构建、完整 Sponza/OBJ 文档链、正式窗口验收均通过；两个窗口运行都启用 D3D12 GPU validation，日志无 warning/error/fail。两份最终 `ddgi.txt` 指标逐字一致。最终图像分别保留默认完整 Scene Lit、关闭 AO/SSSR/Bloom/环境后的 DDGI 开关、暖灯从 x=-4 到 x=4 的 32 步真实字段编辑、移动停止后的开关对照，以及所有灯强度归零后的消退。移动步骤的 `history_resets` 全为 0；完成后恢复原始场景值，不保存验收编辑。

| 最终中性材质场景的图像指标，0～255 | Dev / Release |
| --- | ---: |
| 原位 DDGI 开关平均 RGB 绝对差 | 49.8709 |
| 静态两次捕获的平均 RGB 绝对差 | .371414 |
| 移动后 DDGI 开关平均 RGB 绝对差 | 35.4786 |
| 移动前后开关差的再差分 | 26.4249 |
| 关灯初期 / 389 个接受帧后的平均显示亮度 | 123.362 / 2.47939 |

指标取相同视口内有几何深度的像素，属于色调映射后的显示空间差异，包含其上的编辑辅助叠层；不是线性间接辐照度真值，也不是帧率测量。静态噪声对照的两次捕获之间推进 32 个预热帧及一次捕获帧；关灯后的最终捕获包含三段各 128 个预热帧和中间捕获。固定检查要求 DDGI 开关差大于 .5 且超过静态差两倍、移动后的再差分大于同一门槛、关灯末期亮度低于初期的 30% 或 2；没有随实测放宽阈值。

2026-10-18 补注：上表是中性 .55 无贴图材质的值。自 `60edfab0` 把 DDGI/GTAO/SSSR/Bloom 改为 World 作者设置后，验收直接写视口设置，每帧被 World 值覆盖，开关从未到达渲染器，ec-2 门禁因此在开关差 .94、静态差 .64 上失败。编辑器验收改走 World 作者路径后（编辑器负责人的修复链），rendering 工作树（A 直接光）在贴图材质下实测：Dev／Release 开关差 3.686／3.684、静态差 .144／.141，移动后开关差 3.63、再差分 2.18。下降幅度是推断，没有做中性材质对照：有效反照率为组件 .55 乘贴图线性均值（建筑约 .20～.39，布料 .07～.14），相机所见间接光近似与接收面和反弹面反照率之积成比例，估计为 10-10 的 3%～9%，即 1.5～4.5。关灯消退原判据把编辑器叠层亮度（3.680）计入，现扣除同相机关灯且关 DDGI 的底数：Dev 初期 7.343、稳定 0.000，稳定帧与无光底数逐值相同。证据在 `out/accept-20261018/sponza-{dev-base,dev-fix,release-fix,dev-floor}/`。

首次沿用基础显示的纯白实例色 1 时，DDGI 使画面偏亮；关灯初期 / 末期亮度为 218.154 / 107.381，未通过消退检查。最终默认改成 .55 是场景材质设定，本轮没有修正这个高反射率下的慢消退问题，不能将最终通过外推到所有材质。该轮 Scene Lit 尚不读取 Sponza 漫反射/opacity 图，植物与铁链按不透明处理；此历史限制已由 2026-10-11 的标准材质接入替代。

本轮证据位于 `out/sponza-ddgi-20261010/`：`{dev,release}-final/` 保存最终截图、`ddgi.txt`、`ddgi-motion.csv` 和 `acceptance.txt`，`{dev,release}-window-final.log` 为窗口运行记录；`{dev,release}-document-final.log` 保存最终场景往返结果。首次纯白失败的图像与指标保留在 `dev/` 和 `dev-window.log`。构建使用锁文件版本；本机环境脚本选中的已安装 CMake 版本不同，实际使用既有 `.tools/setup/cmake18.5/` 内与锁文件相符的 CMake/Ninja，未修改锁文件或全局安装。

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

该次历史合并尚未让 Scene Lit 采样 Mesh 材质图，其辅助深度与叠层保持和不透明光栅/射线相同的覆盖；这一限制已由上面的标准材质接入替代。纯线框和基础着色继续裁剪透明材质。下面独立分支各节保留各自的历史证据；阶段合并的验证单独记录。

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

源资产逐文件 SHA-256 验证通过：57 个原始文件、95,882,276 字节，与 [source.json](../../../assets/examples/Examples/Sponza/Sponza/source.json) 一致。解析结果为 262,267 个三角形、103 个连续材质组、25 个命名材质及 24 个漫反射贴图材质；源文件未改动，Scene 保持相对资源引用与 0.01 实例比例。

真实导入链验证凹多边形面积、负索引/负 UV、共享 WIC 图像、重复导入拒绝、刷新幂等、图片/MTL 改写和删除、旧 Mesh/图像 owner 保持，以及缺依赖和目录逃逸拒绝。GPU 像素读回区分两组几何，验证顶点起点、sRGB 漫反射、UV 重复与左上原点、漫反射 alpha 和独立线性透明遮罩、背景深度、线框/MatCap 轮廓、暖帧零上传、替换与移除后的 owner 释放。完整窗口开启 D3D12 debug/GBV，捕获实际场景颜色与深度；最终渲染器和窗口日志未报告 D3D12 warning/error。

可重复入口为 `editor_sponza_acceptance` 与 `renderer_acceptance`，使用现有 dev/release presets。证据为 `out/<配置>/sponza-{document,import,renderer,window,physics}.log`；图片和窗口报告在 `out/<配置>/sponza-evidence/`，包括 `sponza-interior.png`、`sponza-overview.png`、`sponza-matcap.png` 和 `acceptance.txt`。正式打开方式见 [Sponza 默认场景](../../../assets/examples/Examples/Sponza/README.md)。

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
cmake --build --preset dev --target editor_project_acceptance
# release 使用相同目标，对应 --preset release。
```

Renderer 消费者检查 MatCap 对基础色的覆盖与视空间响应、Flat/顶点法线差异、多边形对角线过滤、全场景遮挡、纯线模式深度、线宽与透视距离、无效参数的编码前拒绝、异步模式切换；沿原消费者继续执行矩阵、网格版本、UI 与关停验收。正式编辑器专项仍使用 `atelier_editor`，在 `--acceptance-project` 开头从空场景通过真实资源浏览器创建 sphere、点击共享原生下拉菜单切换各显示模式，检查几何/材质引用、矩阵、选中对象与撤销历史保持不变，并复验 World TRS 到 GPU 的投影。专项图片保存在 `out/<配置>/project-evidence/`。

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

以下保留 2026-09-28 至 S1 的早期记录，包含后来已经替换的前向深度、整面选中色和构建接线。它们用于追溯当时结果，不能覆盖上方当前 Reversed-Z、边线高亮及正式 presets 合同。

2026-09-28，Windows x64、AMD Radeon RX 6800 XT，dev 与 release 的正式 `renderer_acceptance` 均通过。fixture 使用真实 HWND、正式 Renderer、D3D12 debug layer 与 GPU validation；不使用 mock。最终程序输出必须包含 `Renderer acceptance: PASS` 且退出 0，runner 保留日志并限制进程为 120 秒。

### 可重复入口

应用根构建先创建 RHI 与 `atelier_ui_protocol`，再 include `cmake/renderer.cmake` 并调用 `atelier_add_renderer()`。然后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target renderer_acceptance
cmake --preset release
cmake --build --preset release --target renderer_acceptance
```

早期独立 RHI worktree 曾使用临时 CMake DEFER 注入完成接线，该辅助方式已废弃。当前正式根构建直接包含 Renderer，使用上面的普通 presets 命令。

### 已核对的结果

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

### S1 诊断计数增量

从 `fcb9ddf` 增加 `FrameResult::stats` 后，dev/release 分别再次运行现有 `renderer_acceptance`，均 PASS。只扩展同一真实窗口消费者的检查，没有另建测试或 benchmark 工程，没有重跑无改动的完整 RHI/Runtime/JIT 矩阵。

- 首帧 3 次 UI draw、1 次包含两个方块的 scene draw；一次覆盖图上传为 6 个紧密 R8 字节，缓存为 1 项/6 个 texel 字节/10 个含行 padding 的 CPU 字节。
- 仅改变选中状态时，绘制数保持，上传量为 0；同 identity 的新 revision 只在首帧上传，旧版本退出缓存后 CPU 字节由 10 变为 6。连续 12 帧计数未累计漂移。
- 非空输入数组中额外加入空 clip 图元，实际 UI draw 与上传数不增加，未使用的覆盖图不进入 GPU 缓存。
- extent 不匹配的 skipped 帧活动计数全为 0，当前缓存量保留；删除最后一个可见覆盖图并清空场景后，缓存三项归零、scene draw 为 0。
- 原有颜色/深度、矩阵、UI 混合、resize、最小化恢复和关停检查仍通过。原生遮挡分类未另行强制制造，旧的驱动故障与硬件证据边界保持不变。

该结果证明计数对应已编码操作和现有缓存持有量，不是 256 对象的性能结论；真实交互 prepare/render 耗时与上传分布由应用侧在同一正式入口采样。日志为本机 `out/<preset>/renderer-stats-run.log` 和正式 runner 生成的 `renderer-acceptance.log`。

### 已有 RHI 回归

同一 gfx 补丁在 dev/release 分别运行 `rhi_acceptance`，两种配置均通过：Compute/Mesh/Vertex 场景颜色 24,576 像素、深度 12,288 像素、90 个参数字匹配；管线准备失败、资源使用期、owner/关停、null-list、mesh-pixel-state 和真实 timeout 矩阵通过。timeout 检查继续报告 `poisoned-admission=PASS drain-only=PASS increments=1`。

对应日志为 `out/<preset>/rhi-acceptance.log`、`rhi-regression-*.log`。没有重复运行无改动的 Runtime/JIT 矩阵。

### 当时未覆盖

没有动态注入 Present 后的 ResizeBuffers、Signal、Reset、device removal 或 native OOM 故障；该处失败继续沿原 RHI 终态返回，代码仅在整个 frame 成功后接受。原 timeout 证明不能替代这些驱动故障的证明。未强制制造 `DXGI_STATUS_OCCLUDED`；其分类有实现，隐藏/最小化是编码前可见性检查，不能记作 native occlusion 的验收。

没有扩展到其他显卡/驱动、不支持 Mesh 的硬件、HDR、多个窗口、并行录制或设备恢复。真实产品交互的完成以应用侧共同验收为准。
