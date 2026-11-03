# 布局核心真实路径验证结果

状态：**三条候选已完成本轮隔离实验。Yoga 与修正后的 Taffy 在开发/发布均通过 9 组已实现检查；Clay 为 6/9，真实失败保留。** 这不是完整产品合同验收，该实验当时没有创建或接入生产布局库；本页不定义当前Interface。

本页保留三候选在固定版本、同一消费路径中的比较证据；生产布局采用原生 LayoutDocument，当前设计见[设计决定](../../modules/ui-layout/decisions.md)。实验的通过次数不代表当前检出的验收结果。

## 1. 比较结论

- 游戏 HUD、矩形布局和复杂本地化表单不能仅凭同一个矩形榜单选型。
- Yoga 在本轮动态文本任务中成本更低，但饱和 max-width 的三项分配会随子项顺序改变，不能承诺未经修正的公平填满语义。
- Taffy 的纯几何、冷建树和重排路径有竞争力，原生 Grid 通过共同几何；动态 Grid 文字产生更多 intrinsic 测量/实际排版，不能因为矩形更快就宣布通用胜出。
- Clay 的矩形结果可以比较；其原生分词/换行没有完成同一项本地化文字工作，文本结果不参与等价性能排名。换成外部段落排版并重新量测是另一条尚未运行的候选，不声称已被否决。

## 2. 实验位置与复现

历史探针原位于acceptance/ui_layout_probe，使用真实DirectWrite和三个真实候选。现已由正式原生布局消费者替代并退役；其源码、runner和Cargo锁可从Git提交`c644ba5692fe2d67d1dda93953e227a50ba64f9f`恢复。当前运行入口见[正式验收](../../modules/ui-layout/acceptance.md)。

- 来源与哈希唯一清单：[sources.json](sources.json)。Yoga `042f501...`；Taffy `77f3856...`；Clay M `e6cc369...`，不是 Clay v0.14 的实测。
- 构建：Windows x64；Clang 23.1.1；Windows SDK 10.0.26100.0；VC tools 14.50.35717；Rust 1.97.1 / x86_64-pc-windows-msvc，panic=unwind；mimalloc 3.5.3。具体版本、CPU、源码哈希、字体和 DWrite 文件哈希见 `out/review/ui-layout-validation/environment.json`。
- 实机：Windows 10.0.26200、Ryzen 9 3950X，16 核/32 逻辑处理器。只用 Release 作性能对照；构建结束后串行运行。没有设置产品帧预算，没有将 1k 节点定义为产品上限。
- `Segoe UI`、en-US；字体缩放/版本变更通过真实提供者验证。记录可用字体文件哈希不等于已经跟踪每个 CJK glyph 的实际 fallback face。
- C++ 通用分配与 Rust GlobalAlloc 使用同一配置的 mimalloc DLL；入口验证 CRT redirect、头文件/DLL版本及 engine 指针来自共享 heap。DWrite 的内部资源仍遵循其系统拥有者。
- 原始许可未改动：`out/review/ui-layout-validation/vendor/yoga/LICENSE`、`out/review/ui-layout-validation/vendor/taffy/LICENSE`，以及 `out/review/ui-layout-validation/vendor/clay/clay.h`。这只是来源记录，不替代将来实际发行方案的许可核对。

最终批次 `out/review/ui-layout-validation/runs.json` 包含 **36 次子进程运行：27 次退出 0、9 次非零、0 次超时**。非零为两配置 Clay 行为组、Clay 文本基准，以及 6 次深度栈溢出；runner 返回 2 是诚实汇报已记录的失败，不是全绿。

`out/review/ui-layout-validation/final-validation-utf8.log` 从原始混合编码日志逐行无替换转码；原始 `final-validation.log` 保留。每次运行记录 EXE SHA-256；最终源码的 11 个哈希已核对一致。初始失败及修正前样本保留在 `runs-initial.json` / `environment-initial.json`，不混入下方最终表。

运行后的 EXE/DLL 指纹另见 `out/review/ui-layout-validation/binary-provenance.json`。out 是本机证据目录，不能假定其他 worktree 已带有这些原始文件；缺失时只能恢复对应历史源码重新生成实验批次，本页保留本轮摘要，不冒称日志已随源码提交。

## 3. 行为矩阵

以下只记录当时实际执行的fixture；未运行设想不计入通过。几何比较使用 `0.08 + 0.000002 × max(abs(a),abs(b))` 容差，非逐 bit 相同；大虚拟坐标的相对容差也不能当作像素级 clip/hit-test 证明。

| 检查 | Yoga Debug / Release | Taffy Debug / Release | Clay Debug / Release |
|---|---|---|---|
| P1 英文共享列、resize、内容/字体、删除/重排 | PASS / PASS | PASS / PASS | FAIL / FAIL |
| P1 无空格中文、窄列回流 | PASS / PASS | PASS / PASS | FAIL / FAIL |
| P2 HUD、嵌套小数坐标、DPI 纯值消费 | PASS / PASS | PASS / PASS | PASS / PASS |
| P3 固定格换行阈值与 warm/fresh | PASS / PASS | PASS / PASS | PASS / PASS |
| P4 变高虚拟列表、离屏最长标签、字体/宽度更新 | PASS / PASS | PASS / PASS | FAIL / FAIL |
| P5 固定外框/自然高度、宿主反馈环拒绝 | PASS / PASS | PASS / PASS | PASS / PASS |
| P6 无效 UTF-8、第三次测量失败/重入、旧结果/重试 | PASS / PASS | PASS / PASS | PASS / PASS |
| P6 环/非有限值/重复 ID 的批事务拒绝 | PASS / PASS | PASS / PASS | PASS / PASS |
| P6 40,000 字节文本容量增长、warm/fresh、缩回短词 | PASS / PASS | PASS / PASS | PASS / PASS |

P4 为 10,000 个逻辑项，三轮分别只实例化 24/20/18 个布局输入节点。共享列宽采用**全数据精确扫描**，高度/prefix 索引由宿主拥有；每次刷新执行 10,000 次标签自然宽度查询及 20,000 次受约束高度查询。不是免费的全局信息，也不是已经实现增量索引；这部分扫描不在后面的可见树性能表内。滚动锚点固定逻辑项 5000，检查 prefix、完整 content extent 和 warm/fresh，不声称已经实现完整树表控件、横向列虚拟化或 renderer。

## 4. 必须保留的反例

### Yoga：饱和分配依赖顺序

300 宽的 row，三个同权 grow 子项，B 最大 120、A 最大 30、C 无最大值。按本场景的剩余空间继续分配语义，按身份读取应为 `[B,C,A]=[120,150,30]`。

- Yoga 初始顺序实际 `[120,135,30]`，总宽 285；交换受限项顺序后为 `[120,150,30]`，总宽 300。
- Taffy、Clay 两个顺序均为 `[120,150,30]`。
- Debug/Release 一致，见各候选 `*-saturation.json`。这是与本产品分配 profile 的不一致，不拿一次例子宣布全部 CSS/Flex 行为错误；生产若选择 Yoga，必须明确并处理此语义。

### Clay：原生文字不是同一项工作

- 英文某行预期高度 148.968750，实际 170.250000；中文预期 53.339844，实际 26.669922。
- 固定源码 `clay.h` 的 `Clay__MeasureTextCached`（1731–1764）只按 ASCII 空格/换行切词；无空格中文成为完整 word。布局的最小词宽/整词溢出路径使该输入不产生 DirectWrite 的两行。
- 英文差一个行高；原生词段包含尾随空格，再在断行后移除末尾空格（1740–1743、2607–2617）。这支持“断行位置不同”的**推断**，没有逐词 trace，不能把特定触发词写成已证事实。
- P4 的最终 extent 不一致。没有独立分离“原生 FIT 压缩”和“词段断行”的全部贡献，不把未隔离原因写成定论。
- Clay 128 文本基准在初始 golden 就退出 3，**没有产生可排名的文字计时记录**。矩形快不抵扣这项语义差异。

### Taffy：适配修复而非改 golden

最初 64 行 Inspector 的首行预期 79.804688，实际 26.364586。适配器把 cell 最小高度置零、Grid 使用 auto 行，又将可用高度转成文本 AT_MOST，导致有限 viewport 高度参与压缩本应自然增长的行。

现在 `GRID_TWO` 使用原生 `max_content()` 行轨道；无 known/fixed height 的 AUTO 文本按已解析宽度查询自然无界高度，确定高度仍保留。修复仅在 Rust adapter；预期几何没有改动。最终两个配置 9/9，Release 128 文本的全部 7 个模式通过前置、首个变化状态和最终 golden/warm-fresh。

## 5. Release 成本

单位：**微秒，median / p95**。每模式 20 次预热 + 100 次记录，计数统计排除预热。每个有效模式保存 total/prepare+apply/layout-inclusive/callback/export+consume 的 100 条原始记录；35 个有效模式共 3,500 条样本，时间分段核对一致。p99 与全部分项留在 JSON。

### 1,000 固定盒（1,001 个输入节点）

| 模式 | Yoga | Taffy | Clay |
|---|---:|---:|---:|
| 冷 solver／热字体 | 592.7 / 604.7 | 344.6 / 352.3 | 305.2 / 328.2 |
| clean refresh | 6.8 / 6.8 | 6.2 / 6.2 | 0.6 / 0.6 |
| 单叶更新 | 296.5 / 347.2 | 262.2 / 270.0 | 277.0 / 353.7 |
| 根宽度交替 | 252.5 / 258.8 | 186.4 / 190.6 | 120.5 / 159.9 |
| 结构重排 | 587.5 / 601.8 | 333.3 / 342.4 | 316.8 / 451.1 |
| 高变化 | 519.8 / 534.0 | 307.6 / 316.2 | 282.6 / 385.7 |

### 128 文字 Inspector（129 个输入节点）

| 模式 | Yoga | Taffy |
|---|---:|---:|
| 冷 solver／热字体 | 255.8 / 263.0 | 233.3 / 297.5 |
| clean refresh | 1.3 / 1.4 | 1.1 / 1.3 |
| 单叶更新 | 37.6 / 45.1 | 100.5 / 131.5 |
| 根宽度交替 | 103.6 / 107.8 | 101.3 / 136.2 |
| 结构重排 | 257.2 / 273.8 | 237.4 / 329.8 |
| 高变化 | 1671.8 / 1788.4 | 7374.5 / 7709.7 |

这不是相同原生节点数：Yoga 增加投影行；Clay 增加行、内部 text 和隐式 root。比较对象是同一产品几何任务，不计算“每个 solver 节点”的排行榜。

### 动态文字差距来自哪里

高变化帧为 64 个值字符串每帧全部改变。100 个样本中：

| 指标 | Yoga | Taffy |
|---|---:|---:|
| 端到端 median（µs） | 1671.8 | 7374.5 |
| layout 包含回调 median（µs） | 1649.5 | 7327.0 |
| 每样本先扣回调后的 layout median（µs） | 226.3 | 182.7 |
| 测量回调总次数 | 6400 | 32000 |
| 真实 DWrite layout 创建/排版次数 | 6400 | 25600 |
| 真实排版累计时间（µs） | 137772.3 | 691117.3 |
| 定时区间 API 调用总次数 | 300 | 300 |
| 输入结构体可读字节 | 1444800 | 1444800 |
| 输出 memcpy 字节 | 412800 | 412800 |

**Taffy 此任务端到端约慢 4.4 倍，但扣除回调后的中位时间反而略低。** 不能把差距统称 Rust/FFI 开销：其 Grid 路径触发更多 intrinsic 查询，公共提供者为不同约束键创建更多真实段落布局。后续优先验证排版对象/测量结果复用与容器协议，而不是未经测量就建立逐节点零拷贝 FFI。尚未验证该优化收益，也不删除必要 intrinsic 查询来伪造同等工作。

静态 `static_skip` 三者都不调用布局/导出，复用已有纯值；观测在 0.0–0.1µs 的计时底噪附近，不作性能胜负。`clean_refresh` 是各 adapter 自然的干净请求：Clay 直接复用 publication，Yoga/Taffy 仍进入 native compute/collect；不是三个 native solver warm-compute 的同工比较。

## 6. 深度与安全边界

单链后接真实文字叶，独立子进程运行，避免一次崩溃中止整个实验：

| 候选/配置 | depth 64 | depth 256 | depth 1024 |
|---|---|---|---|
| Yoga Debug | PASS | PASS | 栈溢出 |
| Yoga Release | PASS | PASS | 栈溢出 |
| Taffy Debug | PASS | 栈溢出 | 栈溢出 |
| Taffy Release | PASS | PASS | 栈溢出 |
| Clay Debug | PASS | PASS | 栈溢出 |
| Clay Release | PASS | PASS | PASS |

失败码为 `0xC00000FD`（有符号 -1073741571）。直接检查 Taffy 两配置及 Yoga/Clay Release 的 PE header，栈 reserve 1,048,576 字节、初始 commit 4,096 字节；例如 `out/review/ui-layout-validation/dev-layout_probe_taffy.exe-headers.txt`。结果属于**本探针的完整适配调用链和此构建/线程栈**；Clay adapter 自身也有递归 emit，不归因于纯库内核。不修改栈大小掩盖失败，不承诺任意深度，也不把通过的 depth64 当作所有树形都安全的产品上限。

本轮失败恢复、旧 publication 与 callback 重入是返回错误路径；栈溢出/分配器 abort 不属于可恢复错误。生产必须在进入不可中断求解前规定并检查资源限额。

Clay adapter 还修正了本 snapshot 的容量耦合：text hash 桶索引空间为 wordCapacity/32，而桶数组按 elementCapacity 分配，显式保证后者不小于前者；40k 文本检查覆盖扩容和缩回。保留 main 的 uint16 padding/gap/font-size、零 FIXED 不支持及固定盒 wrap 等适配限制，不把此探针包装成通用正式接口。

## 7. 测量口径与尚未证明的内容

- `prepare_apply` 包含场景编辑、bind、验证、输入同步；cold 模式还含释放/新建。字体提供者保持热，不是冷进程启动。
- `layout_inclusive` 包含 native 求解、adapter 处理、回调、collect/publish；它不是 solver-only。扣回调值由逐样本相减后取 median，不是不同集合 median 相减。
- 输入/回调元数据的“字节”是 ABI 可读范围；字符串借用不是已发生 memcpy。输出字节才是明确的导出复制量。统计读取在计时区间外。
- 成功路径检查不创建临时诊断字符串；失败动态模式先报错，只有通过首态/终态 golden 和 warm/fresh 才发 `validated:true` 的 benchmark 记录。
- 结构重排在这三份原型里触发重建；此成本已计入，不将其宣称各库的最佳增量重排能力。cold rebuilds 按每个新实例累计，100 样本为100，而非只读最后一个实例的1。
- 字体提供者按文本/字体大小/约束模式与值/字体版本缓存；不同模式顺序固定，缓存跨模式保留。单叶在两种内容间交替，根宽在两个值间交替，预热后实际 shaping 可为零；不是连续拖动任意宽度的成本证明。
- `process_lifetime_peak_rss/commit` 是进程生命周期峰值，包含 DWrite、precondition/fresh 对照时并存的两个 engine、跨模式缓存，不当作单帧/单 solver 内存。未可靠记录逐分配次数、字节或独立 FFI 延迟；不填估计零。
- Clay 固定盒基准：输入浮动节点0、文字字节/词数0，element capacity4096、word capacity4096；HUD 的4个浮动输入另有几何验收。没有采集一般负载的实际 command 数和 native 访问节点量。
- P5 只证明已实现的固定/自然容器依赖及宿主180/220反馈环拒绝，不证明通用用户定义 measure/arrange ABI或 sibling 失效隔离性能。
- P6 的回调重入实测调用是 `lp_layout`；未实测回调中 destroy/结构删除。旧几何以调用者拥有的值副本验证，未测试生产级跨线程/借用租期协议。
- 几何高度相符不代表已经逐 glyph/逐行验证 baseline、ink、bi-di 或所有 Unicode 换行；没有 renderer、完整 docking、输入/焦点、GPU在飞寿命验收。
- 没有最终生产 NodeId/代际、发布租期或内存限额合同；实验全量拥有输出只是本轮等价消费选择，不强制未来每帧全量深拷贝。
- 未锁定进程亲和性/电源策略，表为一次串行本机样本，不能转成跨机保证或硬帧预算；主项目全仓验收未运行，因为没有接入生产 target。
