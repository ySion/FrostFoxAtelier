# Taffy：布局求解器与宿主所有权评估

## 1. 结论与版本边界

**[推断] Taffy 应进入真实路径验证短名单，但不能据此冻结生产选型。** 最强支持理由是同一求解器提供 Flex、Grid、Block，且允许宿主保留自己的树和样式存储；最强反对理由是跨容器共享轨道、虚拟化、测量失败事务和 C++ 接入仍需宿主解决，不能算作现成功能。

**[事实] 本次可证发布版为 `taffy 0.14.0`：** crates.io 发布时间为 **2026-08-24 19:04:54 UTC**，GitHub release 为同日 19:05:31 UTC；tag 为 `v0.14.0`，发布包 `.cargo_vcs_info.json` 固定到 **`77f385683c1d698c91a23a259f87fdddf26925fb`**。许可证 MIT；Rust edition 2021，声明 MSRV 1.71，启用 `strict_provenance` 则要求 Rust 1.84 以上。旧 `0.9.1` 发布于 2025-08-20，不能作为当前 API 的替身。[1–3]

**[事实] 0.9.1→0.14.0 不是小修补：** 0.10 加入 RTL `direction`、float/clear、可选样式解析；0.11 加入 safe alignment；0.12 改变缓存键；0.13 增加 flow-root 等；0.14 改变叶测量签名、baseline/overflow 输出及 min/max 类型，加入 sizing keywords、balanced flex wrap 和部分 containment，修复删除节点未使原父节点失效等问题。`contain:size` 仍未支持。[2,4]

**[上游自报]** 0.12 发布说明称更严格缓存曾带来常见约 10%、病态约 60% 的性能损失；这不是 0.14 的本机结论。README 的 Yoga 比较仍是 M1 Pro、layout-only、不计建树、Yoga 经 Rust binding，比较提交为 Yoga `ba27f9d` 与 Taffy `71027a8`。**本报告未运行构建、测试、lint、格式化或计时，不宣布性能胜负。**[4,5]

## 2. 身份与所有权

**[事实] 身份是 solver，不是控件 framework，也不是完整 UI 系统。** traits 构成可适配的求解接口，但不等同于完整布局协议；绘制、事件、字体 shaping、docking、滚动交互与虚拟化不由这些算法提供。[5,6]

| 路径 | 已有机制 | 宿主责任 |
|---|---|---|
| `TaffyTree` | SlotMap 节点/父子关系；节点保存 Style、Cache、未取整与最终 Layout；独立 context map；自动算法分派及向祖先失效 | 同业务树同步，维护业务身份到 NodeId 映射，测量与外部资源版本 |
| 低层 traits | `TraversePartialTree` 只要求容器及直接孩子访问；`LayoutPartialTree` 配合 Flex/Grid/Block 专用 traits；CacheTree、RoundTree 分离 | 自有存储、缓存、失效、算法分派、输出提交及树合法性 |

以上依据实际 trait 声明和实现；部分源码顶部示例仍展示旧接口，应以声明为准。样式通过关联类型与 getter 暴露，**不要求宿主复制完整 `Style`，也不要求 Rust 拥有业务树**。`compute_child_layout` 可分派自定义布局或组合标准算法；`TaffyTree` 默认按 Display 和有无孩子分派，并非任意容器 custom-layout 插件系统。[6–8]

## 3. S1–S9 适配

下表是功能事实与宿主组合判断，不是场景已经通过验证。[2,6–12]

| 场景 | 映射与缺口 |
|---|---|
| S1 HUD | absolute/inset、百分比及普通容器可表达角落定位；安全区、缩放、叠层绘制和像素转换由宿主定义。不是任意锚点约束求解器。 |
| S2 格子/技能条/标签 | Grid 固定、内在尺寸、fr、repeat/auto-fit/auto-fill 和 Flex wrap 可用；0.14 的 balance 需单列成本。 |
| S3 Inspector | 同一 Grid 的标签和编辑器可共享列轨道；独立嵌套 Grid 不自动共享自然列宽。Subgrid issue 仍开放，0.14 类型仅有 Single/Repeat，不能假定支持。 |
| S4 splitter/docking | 拖拽转成尺寸/约束再求解；popup 可 absolute 或另建根。拖放、吸附、焦点和层级重排属于宿主。 |
| S5 树/表格/日志 | 只布局已实例化节点；可变行高索引、估高、滚动锚定、离屏测量与回收由宿主完成。Grid 并非无限逻辑行数据库。 |
| S6 本地化/混合字号 | 宽度依赖高度由测量提供者负责；0.14 回调可返回 first/last baseline，但默认叶 helper 输出无 baseline，不能宣称真实字体对齐已接通。 |
| S7 嵌套滚动 | overflow、scrollbar gutter、scrollable overflow rect 参与几何；偏移、裁剪与命中测试在宿主。仅改变滚动偏移时是否跳过布局由宿主调度。 |
| S8 高频变化 | 热缓存、叶失效、祖先 resize、结构变化分别测；rounding 与结果消费可能掩盖 solver 热命中收益。 |
| S9 失败/边界 | 非法 ID、循环树、Grid line 0、极端轨道、深树及测量失败需要边界合同；Result 不代表所有异常都返回 Err。 |

**[推断] 共享轨道和可变行高存在成本耦合：** 一个更长标签可能拓宽整列，使全部编辑器文本重新换行；虚拟化若只收集可见标签，列宽可能随滚动漂移；要求离屏最大标签决定列宽，则需要全数据度量或宿主维护最大值。低层 traits 允许专用列表/表格布局，但不会消除这些依赖。

## 4. 测量、缓存与失效

**[事实] 第一项关键 API 升级：0.14 的 `compute_layout_with_measure` 回调为 `FnMut(LayoutInput, NodeId, Option<&mut NodeContext>, &Style) -> LayoutOutput`。** 旧版直接返回 Size 的接法应显式包入 `compute_leaf_layout`，否则 padding、border、min/max 等叶规则不能假定仍自动应用。**不得拿旧 0.9 文档生成 0.14 适配代码。** 固定 pin 的实际定义见 [7,9]。[2]

`known_dimensions` 是已确定尺寸，不是 available-space 上限；`available_space` 有 Definite、MinContent、MaxContent。叶 helper 解析父尺寸、box sizing、边距、滚动条及内容区域，并可能在已知尺寸时提前返回；其 PerformLayout 分支传给内部内容测量函数的 known dimensions 是 NONE，不能混淆外层与内层回调。字体 shaping 必须区分内在尺寸查询与确定宽度换行。[9]

**[事实] 第二项关键 API 升级是缓存：** 0.9.1 使用一个 final entry、九个按已知轴/约束类别分槽的 measurement entries；0.14 仍为 1+9，但 measurement 改为 second-chance/clock 淘汰，`Cache::get` 和 `CacheTree::cache_get` 接收可变 self 及 `&LayoutInput`。固定 pin 源码见 [8,10]。[2]

新键编码 known-dimension/available-space、父尺寸、requested axis 与尺寸 definiteness；final layout 要完整键相等，measurement 匹配只比较父宽度部分，并要求缓存轴覆盖请求轴。带 margin-collapse 元数据的测量结果不缓存。它不是对任意自定义回调都成立的通用 memoization。[10]

**[事实] 内容、字体版本不在上述键中。** `set_style`、`set_node_context` 及结构修改会失效；`get_node_context_mut` 本身不标脏。重复写相同 Style 仍会 mark_dirty。失效沿父链清缓存，遇已空缓存提前结束；新叶失效的父链工作可按高度 h 分析，但整次重排不因此成为 O(h)。父宽变化可能重测多行，共享轨道可能扩大影响集合。[7,10,11]

**[推断]** 宿主只提交真正变化的输入，在字体/内容/测量策略变化时显式失效；自定义布局依赖额外状态时，由自身版本键或 CacheTree 实现补足。

## 5. 热帧、分配、rounding 与边界

**必须区分三种工作，不能全部称作“缓存命中”：**

- **[事实] Solver/cache：** `compute_cached_layout` 命中后直接返回；未变约束的根命中不要求遍历全部孩子。`compute_root_layout` 仍读取根样式并写根结果，不是整个入口完全无工作。[14]
- **[事实] Rounding：** TaffyTree 默认开启。`compute_layout_with_measure` 在 root compute 后，仅当 `use_rounding` 为真才调用递归全子树的 `round_layout`，不管 root 是否命中。`disable_rounding()` 置 false，确实删除这次 rounding 全遍历；`layout(node)` 随之读取 `unrounded_layout`，不会偷偷再导出全树。固定 pin 入口见 [7]。[14]
- **[推断] Host 消费：** C++ 若逐节点读取/复制所有 Layout，仍为 O(N) 结果消费；这是 adapter 的策略，不是关闭 rounding 后 solver 必然执行的工作。增量导出也不能未经实现就声称现成存在。

**[事实]** Rounding 分开保存未取整/最终结果，以累计坐标的边界差计算宽高；API 不接收设备缩放系数。std 的 round 为 `(x+0.5).floor()`，no_std polyfill 对负半整数处理不同，应验证负坐标 popup 与 DPI 策略。[13,14]

**[事实] 预留 TaffyTree 容量不等于零分配。** 普通配置的 children 是 Vec；Grid 未命中路径构造 items、tracks、occupancy，默认 detailed-layout-info 保存额外 Grid 结果。0.14 occupancy 使用行/列两方向稀疏区间与 SmallVec，不应按旧实现推定为稠密二维矩阵；跨多轨 item 仍遍历覆盖行列，碎片区间仍可能分配。[3,7,11,12]

**[事实] `no_std` 不等于无堆：** `grid` 强制 alloc，TaffyTree 的 slotmap 是分配型存储。无 std/alloc 的 sys 后备使用 ArrayVec，通用 Vec 容量 256、ChildrenVec 容量 16；这是特定内部容器限制，不是低层 API 的统一节点上限。Grid 显式起点两侧各最多 10,000 tracks，单 item span 最大 10,000；超大网格会 clamp。[3,12,13]

**[事实/推断]** 默认求解、rounding、祖先失效存在递归路径，未发现可作安全合同的最大深度参数。入口持有可变树引用，测量同步调用；不能据此声称内部自动并行。单 context 应不重入、不并发修改；独立根能否并行取决于宿主存储及字体资源。安全深度、每节点字节数和 scratch 分配均待测。[7,8,14]

## 6. C++23 接 Rust 的最小 FFI

**[事实]** 上游 C bindings PR #404 仍 open/draft，未完成项包括 Grid 样式和树操作等，不能当作与 0.14 同步维护的成品 ABI。Windows 可选 `x86_64-pc-windows-msvc`；需要固定 Rust/Cargo、MSVC 链接配置，增加 staticlib 或 cdylib 适配 crate，不能假定裸 taffy crate 已导出 C 函数。这是工具链/维护成本，不是运行时慢的证据。[3,15,16]

**[设计推断] 验证两条路径，不预设 Rust 拥有树：**

1. **Rust 布局镜像树：** C++ 保留业务树；一批提交节点/样式/结构 delta，一次 layout，一批写回宿主结果缓冲区。适配较短，但镜像内存、同步及输出复制必须计费。
2. **C++ 存储 + Rust 低层 adapter：** 调用期间借用稳定的 POD 节点/样式/邻接数组，由 Rust traits 读取；Rust 可只拥有 cache/scratch 句柄，结果写 C++ 缓冲区。避免逐字段跨语言 getter；snapshot 转换如有复制，仍计入成本。缓存内存归属与业务树所有权是两回事。[6,8]

ABI 使用固定宽度整数、显式 tag 的尺寸/约束、指针+长度、版本号和状态码；不直传 Rust Style、Vec、Option 或 trait object。每块内存由分配侧释放，Rust handle 配对 destroy；回调 user-data 仅借用。借用期间 C++ 不得扩容/删除数组，整次成功后才发布输出。[16]

测量先采用同步 callback，以状态码+out-result 返回，携带内容/字体版本。宽度依赖性意味着一次预批测量不一定足够：批量输入/导出减少边界调用，不能保证测量本身只有一次往返。

**[事实/设计推断]** 现有测量返回类型不是 Result，adapter 必须设计失败传播：记录错误、不发布本次结果并清除受污染缓存。若用内部受控 unwind 中止求解，必须始终留在 Rust 且采用 unwind 配置；C++ 异常在 C++ callback 内捕获。`catch_unwind` 不能兜住 abort/OOM/栈溢出，不能替代外国异常处理。上游仓库 release profile 的 panic=abort 不自动决定最终适配工程 profile。[3,16]

## 7. 可否决选型的 probes

以下是**未执行**的验证规格；性能否决阈值由主方案预先规定，不临时编造毫秒预算。

1. **共享列（S3/S5）：** 同一数据分别构造单 Grid 和每行嵌套 Grid，加入跨列错误提示、折叠组、离屏最长本地化标签；改变标签、滚动、展开顺序。若要求保留嵌套容器并自动共享内在列宽，又不接受宿主轨道协调，缺失 Subgrid 足以否决原生直用。
2. **缓存差分（S6/S8）：** 同树依次查询 MinContent、MaxContent、换行边界两侧的 Definite 宽度，再仅更新字体/内容版本；每步与清缓存冷算的几何/baseline 比较，记录回调输入、次数、axis。旧高度或 baseline 即否决该缓存/失效接法；0.9.1 与 0.14 必须各用正确适配。
3. **热帧与 resize（S8）：** 比较不调用布局、默认 rounding 热 compute、关闭 rounding 热 compute、单叶更新、逐帧父宽变化；分别记录 solver、round、FFI、导出、分配、测量。如果全命中仍因 round/导出超预算，否决每帧无条件 compute 架构，不泛化为所有 Taffy 接法都失败。
4. **可变行高虚拟化（S5/S7）：** 大逻辑集只实例化 viewport+overscan，执行快速滚动、字体到达、宽度拖拽及展开；检查锚定项、总高估计、回收身份。若必须全部逻辑项常驻求解树或无法避免跳滚，否决该列表集成。
5. **失败/深度/像素（S1/S9）：** 输入循环/重复孩子、失效 ID、Grid line 0、超大 span、非有限测量、callback 失败/重入；逐级增深链树；以 -0.5 等坐标和非整数 DPI 比较 snap。要求边界拒绝非法输入、失败不发布半更新结果；不能合理隔离的进程终止路径否决生产边界设计。
6. **生命周期（S9）：** context 带析构计数器，反复创建→remove→clear。固定源码的 remove/clear 未清对应 `node_context_data`，而 `set_node_context(None)` 有清理；验证是否延迟至重用/整树析构才释放。业务要求删除即释放时，应显式清 context 或不用其资源所有权；持续滞留则否决未经修正的镜像树接法。[7]

## 8. 未知项与决策条件

**[待验证]** Windows Release 端到端时间、测量跨界成本、热帧分配、缓存命中率、安全深度、真实字体 baseline、context 删除生命周期、低层 adapter 别名安全及失败恢复均无本机证据。上游消费项目名单不替代 FrostFoxAtelier 场景验收。[5]

**[推断]** 若硬要求无额外分配、内置虚拟化、独立容器自动共享轨道或开箱即用 C ABI，当前证据不足以直接采用；允许宿主承担这些边界时，多算法和可替换存储值得与其他候选进行真实路径对比。

## 直接主源

原页通过 Exa 获取；`v0.14.0` 对应上述发布包 commit。关键 API 链接固定到 commit，其余源码固定版本 tag；动态 issue 状态为本次获取结果。

1. 发布与 pin：https://crates.io/crates/taffy/0.14.0 ；https://docs.rs/crate/taffy/0.14.0/source/.cargo_vcs_info.json
2. 0.14 release：https://github.com/DioxusLabs/taffy/releases/tag/v0.14.0
3. 许可/features/MSRV/profile：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/Cargo.toml
4. 历次 release：https://github.com/DioxusLabs/taffy/releases/tag/v0.10.0 ；https://github.com/DioxusLabs/taffy/releases/tag/v0.11.0 ；https://github.com/DioxusLabs/taffy/releases/tag/v0.12.0 ；https://github.com/DioxusLabs/taffy/releases/tag/v0.13.0
5. README/历史 benchmark：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/README.md
6. 双 API 架构：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/lib.rs
7. `compute_layout_with_measure`、`disable_rounding`、`layout`、生命周期及失效：https://github.com/DioxusLabs/taffy/blob/77f385683c1d698c91a23a259f87fdddf26925fb/src/tree/taffy_tree.rs
8. traits/CacheTree 实际定义：https://github.com/DioxusLabs/taffy/blob/77f385683c1d698c91a23a259f87fdddf26925fb/src/tree/traits.rs
9. 叶测量与官方接法：https://github.com/DioxusLabs/taffy/blob/77f385683c1d698c91a23a259f87fdddf26925fb/src/compute/leaf.rs ；https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/examples/measure.rs
10. 新旧缓存：https://github.com/DioxusLabs/taffy/blob/77f385683c1d698c91a23a259f87fdddf26925fb/src/tree/cache.rs ；https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/src/tree/cache.rs
11. Grid 算法/style/Subgrid：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/compute/grid/mod.rs ；https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/style/grid.rs ；https://github.com/DioxusLabs/taffy/issues/468
12. Grid 稀疏占用/范围：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/compute/grid/types/cell_occupancy.rs ；https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/compute/grid/types/coordinates.rs
13. std/alloc/core 和 rounding：https://raw.githubusercontent.com/DioxusLabs/taffy/v0.14.0/src/util/sys.rs
14. root/cache/round 算法：https://github.com/DioxusLabs/taffy/blob/77f385683c1d698c91a23a259f87fdddf26925fb/src/compute/mod.rs
15. C bindings 状态：https://github.com/DioxusLabs/taffy/pull/404
16. Rust 官方 FFI/Windows ABI：https://doc.rust-lang.org/nomicon/ffi.html#ffi-and-unwinding ；https://doc.rust-lang.org/rustc/platform-support/windows-msvc.html
