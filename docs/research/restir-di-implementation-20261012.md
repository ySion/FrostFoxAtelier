# 原生 ReSTIR DI 与 DDGI 采样合同 — 2026-10-12

状态：基于 viewport-rendering 工作树 `30ca640` 的本批实现说明；产品迁移与 Dev/Release 完整 GPU 组合通过，另有 1440p 固定房间的正常配置成本采样。当前接口以 [Scene Lit](../modules/renderer/scene-lit.md) 为准，具体条件、数值和证据以[验收记录](../modules/renderer/acceptance.md)为准。本文不把论文帧时、验证层运行时长或旧窗口捕获当新候选性能。

## 问题与本次边界

旧主视口为少量解析灯保留阴影槽，额外灯仍照明却不查询遮挡；DDGI 在多灯时均匀抽两个解析灯。少量强方向光混入大量弱局部灯时，探针命中照明常为零、偶有大值，发光三角形的独立采样又有不同的方差表现。

旧 DDGI 把照度变为五次根后做历史混合，再还原五次方，并限制编码域正冲击。**代码与数学推断**：非负输入的该幂均值不大于算术均值；高方差和正冲击夹取可把稀有强样本持续压暗。增加探针、最终 gain 或 SSGI 不能修复此统计合同。该链路是可检验的原因，不等同于现场全部昏暗已归因。

本批统一光源身份、proposal、贡献与可见性入口，屏幕使用原生 ReSTIR DI，探针命中使用独立新 RIS；DDGI 的目标和显示历史在线性照度域累积。空间布局、距离矩、反馈 LOS 与发光能量归属继续保留。

## 一手依据与采用范围

| 一手资料 | 可据此确认的事实 | 本批采用与边界 |
|---|---|---|
| [Bitterli 等，ReSTIR DI，2020](https://benedikt-bitterli.me/restir/bitterli20restir.pdf) | 原始方法通过 reservoir 在时间和空间重采样光源样本；采样复用不同于混合最终颜色。论文分别讨论无偏与有偏变体。 | 采用直接光样本复用的结构，不把论文速度或无偏声明自动归给本实现。 |
| [Lin 等，GRIS，2022](https://cwyman.org/papers/sig22_foundationsOfReSTIR.pdf) | 相关样本、不同域与未知 PDF 需要正确的贡献权重和 MIS；新鲜 canonical 样本、相关性约束与 M 上限影响收敛条件。 | 使用 generalized balance MIS 交叉求输入目标，每帧保留新鲜样本；历史 confidence 不冒充独立样本数。 |
| [作者课程，A Gentle Introduction to ReSTIR](https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Notes.pdf) | 作者建议先建立正确的基本实现，并用 generalized balance heuristic 处理复用；support 与域处理是容易静默引入偏差的地方。 | 固定统一采样域、完整前帧输入与独立空间读写，先验收均值与生命周期，再考虑更复杂 proposal。 |

这是依论文设计的原生实现。RTXDI v3.1 只作为此前研究参考，没有引入 SDK，也没有复制其自定义许可 shader；项目不宣称已集成 RTXDI。

## 统一源与概率合同

- `LightFrame` 从 Frame / MaterialFrame 建立点光、矩形、发光三角形和方向光的共同快照；局部源在前，方向光在后。太阳逐盏显式评估并排除局部候选池，避免漏选和重算。
- 局部 proposal 是功率估计加 5% 均匀混合的 alias 表。CPU 用 double 构造，再按 float alias 阈值还原实际 PMF；全局功率只用于选择提议，不替代接收点的贡献估计。
- 采样域为 `(光源身份, u, v)`，后两维在单位正方形均匀采样。面积变换的 Jacobian、距离、余弦、BRDF 与材质在贡献中求值；源 PMF 和面积 PDF 不能混用。
- 发光采样读取真实 UV 的 emission / Mask。材质控制单双面发光，镜像后的正面尊重作者法线；开放几何和闭合分类不另建一套发光侧向语义。
- 源身份使用完整稳定键与种类；三角源还包含持有 owner 的不可变 Geometry 和 primitive。保有 owner 防止地址复用；零稳定键不建立时间映射。
- previous/current 分别保存实际位置、方向和面积，并提供双向映射。新增、删除、几何 generation 或材质 Description 变化断开映射；只有描述相同时才重映射前帧材质表索引。
- 每个真实 Renderer sequence 推进快照；静止帧先比较对象级几何、变换和材质描述，避免无变化时重扫全部发光三角形。UI 帧号不承担 Renderer 生命周期。

## 屏幕与探针的各自执行链

1. 屏幕初始 RIS 使用 `direct_candidates` 个局部候选（1～32，默认 8），与通过表面/光源身份验证的前帧 reservoir 合并后写 scratch。
2. 空间通道只读 scratch，最多接收两个通过几何/材质支持检查的邻居，写 current；不在同一 dispatch 中读写邻居状态。
3. generalized balance MIS 对同一候选在各输入接收面重新求目标。零实现仍代表分布；不能依据本次光源亮度或遮挡删除它在分母中的责任。
4. 最终着色用当前表面和当前源重新查询 visibility；reservoir 不存旧遮挡。全部被选中且要求投影的点光、矩形和三角源都经过同一可见性入口，再加入显式方向光。
5. 漫反射与镜面颜色进入既有独立降噪，和 reservoir 历史分开。前帧目标保有表面整数身份、基础色/金属度、法线与 FP32 深度，不拿当前移动位置代替旧表面。
6. DDGI hit 共享源域、PMF、贡献和遮挡 API，但独立生成局部 RIS 与方向光样本，不复制屏幕 reservoir；其离屏照明仍按探针场调度。
7. DDGI 的 target / display 均为双缓冲 RGBA32F 在线性域累积，移除 gamma EMA 和单侧冲击夹取。距离矩仍为 RG32F，FP32 深度和现有反馈遮挡合同不变。

DDGI 将主射线与复杂命中照明分开执行：`DDGITrace` 写每条 64 字节的临时命中记录，`DDGIUpdate` 读取后完成光照与卷积。记录显式携带可着色状态及历史丢弃决策；使用同一方向函数和采样 seed，保持背面、miss、分类和重定位语义。此前单一 kernel 的 GPU 验证报错已检查相关五个描述符槽的完整提交寿命，未发现覆盖或提前释放；这不足以归因于驱动。阶段拆分的通过状态以实际验收为准，不以代码规模下降推断帧时收益。

`direct_reference` 是视口诊断：64 次独立局部 RIS 估计取平均，每次保留当前候选预算，方向光也执行独立多样本估计；无 reservoir 时空复用、无直接光颜色降噪。着色分为最多 64×64 像素的 dispatch，沿全局像素坐标生成随机序列；整帧样本总数与估计保持不变。该分块限制单次提交规模，不提供任意负载下的执行时限保证。它不是纯路径追踪真值，且仍有有限样本方差。

## 能量归属、持久化与成本

屏幕 mesh NEE 负责发光面的一跳漫反射；DDGI hit 注入经过命中表面散射的直接照明，探针直接命中的 emission 不重复注入。相机直接看见的 emission 保留；相机处发光面的镜面仍由 SSSR 表达，本批没有 specular MIS，离屏镜面缺失仍存在。

`direct_candidates` 是 World 作者值，随 Scene v5 保存/撤销；`direct_reference` 是视口偏好。旧预算/样本字段只在 v4 读取边界严格验证后丢弃。四阴影槽、屏幕 tile 灯表和 256 灯固定上限退役；源身份与实际 buffer 大小仍须合法。

三份每像素 24 字节 reservoir 共 `72P` 逻辑字节，另有表面历史、源快照与映射，P 为窗口像素数。局部候选求值、MIS 交叉求值、fresh visibility 和独立方向光都是成本；取消固定灯循环不自动等于更低帧时。资源逻辑字节不等同于驱动实际分配或显存峰值。

全局功率 alias 尚未表达房间、距离和门洞；遮挡重或远处高功率源可能浪费候选。ReSTIR 的屏幕复用不补充墙内或另一楼层缺失的 DDGI 支持，也不增加稀疏光源的真实能量，空间问题见[地牢支持研究](ddgi-dungeon-support-20261012.md)。

## 已完成验收与仍有的边界

Dev/Release 的完整 `--scene-lit-restir-suite` 已通过真实 GPU validation，包含 272 个投影源、重排/增删/运动、Mask/单双面/镜像发光、800 个三角发光源、空场重入、非法帧隔离、FP32 深度、参考分块尾部和 DDGI scratch 伸缩。方向光 DDGI 单独运行与混入 96 个无关局部源的 RGB8 均值差小于 .01；这是指定接收区的显示结果，不能替代任意场景的原始线性照度真值。

关闭验证和 Capture 后，1440p 固定房间收集 240 个独立完成 GPU 样本，阶段合计 p50 9.40748 ms、p95 9.50392 ms。它是当前成本记录，没有同条件旧实现基线，也没有独立冷启动性能结论。详细分项、资源口径和先前失败候选的范围统一记录在[验收记录](../modules/renderer/acceptance.md#原生-restir-di-与-ddgi-线性能量2026-10-13)。

已保存 Dungeons 的独立副本另做普通 Lit、关闭 DDGI、接收支持和探针显示对照。当前 96 个有效解析光源能参与照明，但旧场景仍暗：室内墙面有非零 GI 支持却能量很低，部分地板的候选全部被遮挡。该结果不支持“ReSTIR 已解决全部地牢画质问题”，也不能代替用户后来未保存现场的精确复现。按房间/楼层的布局、原始 HDR 能量和实际作者灯光的后续归因仍沿[地牢支持研究](ddgi-dungeon-support-20261012.md)处理。

本次来源核对使用 Exa：三次 search 的 `numResults` 为 3、2、2，`sources_reviewed=7`；提取并阅读正文/关键段落的唯一 URL 为上表 **3 个**，搜索摘要只作入口。没有以厂商 SDK 性能或论文效果图代替本项目验收。
