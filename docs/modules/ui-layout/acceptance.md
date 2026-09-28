# 布局正式验收

状态：**dev/release各39项正式场景及最小消费者PASS，正式ui_layout_acceptance入口均exit 0。** 首次审计发现的基线与Grid问题已复现修复；交叉二审发现的极端权重触界问题也已关闭。新增4项审计场景并扩展真实基线验收，见[审计与同场景性能](audit.md)。实际合同见[Flex/Grid](flex-grid.md)。未改共享构建或其他线源码，没有复跑Runtime/JIT/RHI全仓验收。

## 当前入口与证据范围

- 生产：`src/ui_layout/layout.hpp`、`layout.cpp`及其私有`flex.inl`、`grid.inl`。
- 正式消费者：`acceptance/ui_layout/main.cpp`、`practical.inl`、`scaling.inl`、`editor.inl`、`flex_grid.inl`、`audit.inl`和`minimal.cpp`；inl仅由现有正式消费者包含，没有第二个目标。
- 构建与运行仍使用README中的`cmake --build --preset dev --target ui_layout_acceptance`及release对应命令，先按正式工具脚本准备环境和配置。
- 本轮验收源码以`c644ba5692fe2d67d1dda93953e227a50ba64f9f`为基线。最终源码/构建输入、四个程序及保留的修改前程序SHA-256记录于`out/review/ui-layout-audit/environment.json`，不沿用历史二进制证明新代码。
- 日志：`out/review/ui-layout-audit/dev-acceptance.log`、`out/review/ui-layout-audit/release-acceptance.log`。检查使用运行时require，Release仍有效；核心和消费者沿用/W4 /WX。
- 退役两套旧探针和整理文档后，两配置再次各39/39及minimal通过，exit 0：`out/review/ui-layout-audit/cleanup-dev-acceptance.log`、`out/review/ui-layout-audit/cleanup-release-acceptance.log`。正式源码与上述哈希记录一致。
- retained_text_frames和editor_session_end_to_end覆盖同内容不同宽度、固定文字的几何复用、旧资源保有/回收、批量编辑、稳定业务key、ghost取消/提交、字体失效及多面板失败保旧。消费链见[composition](composition.md)，最新成本分项见[审计](audit.md)。

## 容器与消费覆盖

| 场景 | 可观察行为 |
|---|---|
| publication_lookup_lifetime | 完整身份查询、跨Document拒绝、删除/slot复用/失败计算保留旧查询、新发布切换、快照复制/移动/析构独立寿命 |
| descriptors_and_collapsed_subtrees | 字段级错误、空表格和非法重复格拒绝、常用描述、折叠不测量且保留身份、恢复缺失内容失败后重试 |
| shared_inspector_dynamic | 真实中文/英文共享列、自然行高、窄宽变化、最长标签变化、行/单元折叠、字体版本、增删/重排，逐轮warm=fresh |
| shared_columns_structure_and_intrinsic | 跨行最大需求构成独立宽度、超列与非法行描述拒绝并保留发布、折叠单元不挪动后续列、Fixed/Expand轨道 |
| wrap_flow_and_repeated_grid | 自然宽度打包、每行对齐、等宽重复格、200到199跨列数阈值、零宽溢出、重排和折叠 |
| nested_wrap_and_shared_columns | 共享列先定宽再计算内部换行高度、宽度增大后自然高度回缩、确定重复槽不请求未使用内容 |
| grid_text_and_failure_recovery | 实际格宽下的DirectWrite自然高度、宽度/内容同时更新时测量失败、旧几何与索引保留、重试重测全部新条目、warm=fresh |

新增的规模与失效场景：

| 场景 | 可观察行为 |
|---|---|
| content_invalidation_scope | 无度量依赖的内容连续更新复用几何；其他节点的依赖跨复用发布保留；自然宽度、折叠恢复与失败重试仍正确 |
| virtual_range_and_anchor | 10万逻辑行、半开范围、增删/行高更新/统一重估保持锚点、错误不变更revision、约9×10^11偏移下的局部小数、空集合与末端夹紧 |
| virtual_inspector_end_to_end | 真实文字、只实例化窗口、回收离屏文字缓存、行内滚动不测量/遍历、离屏编辑、跳转、提交宽度、字体更新失败与恢复 |

本轮审计新增：

| 场景 | 可观察行为 |
|---|---|
| allocation_rounded_saturation | 大权重触及max时，剩余空间继续分给小权重，不被比例舍入吞掉 |
| grid_sparse_spans | 行向、列向跨轨自动项目推进全部覆盖轨的游标 |
| grid_collision_boundaries | 大跨度、64位边界、固定主轴放置、精确容量边界、失败保旧及恢复 |
| collapsed_publication_indices | 连续折叠/展开、失败和重试、删除/槽复用、旧快照查询均保持完整身份 |

基础17项继续纳入当前39项，矩阵如下。

## 性能与未覆盖项

同一个正式消费者提供`--text-costs`、`--virtual-performance`和`--editor-performance`。最新复合场景、完整文字资源和成本拆分见[编辑器消费](composition.md)；文字API/字体对照及早期消费者结果见[历史性能证据](../../research/ui-layout-reassessment/native-performance-history.md)。这些不是帧预算承诺。

仍未提供通用局部调度、完整滚动控件、绘制/输入、业务语言UI绑定、并发/异步或非Windows构建证明。没有所有core分配点失败注入、代际/版本极限循环实测、完整字体文件固定或全进程内存分析。几何快照不保活旧文字/绘制资源。


## 基础行为覆盖


| 场景名 | 实际执行的可观察合同 |
|---|---|
| `identity_and_reuse` | 不同Document身份、跨Document/空/失效ID拒绝、根保护、子树移除、slot复用不复活旧ID、销毁后ID拒绝；编辑仍保留旧发布 |
| `atomic_ordered_edits` | 指定位置插入、同父移位的移除后位置语义、跨父移动保留ID、先序结果；非法位置/叶父/环/带孩子容器改叶拒绝；no-op不推进revision |
| `capacity_and_depth` | 非法构造容量、节点上限、移除后容量恢复、创建深度及移动后代深度检查；失败不改变输入revision |
| `bounded_allocation` | 池300的`[120,150,30]`全部6种顺序；min80/0得到80/20，第二项max10时为90/10；硬min溢出、上限余白及1:2权重 |
| `weight_dynamic_range` | `[1e30,1,1e-30]`权重、`[1,1,无]`上限得到`[1,1,98]`，真实行容器6种排列通过 |
| `slots_padding_and_columns` | padding/gap扣除、50槽中20宽框居中、交叉轴对齐，以及纵向80/20分配 |
| `known_width_natural_height` | 真实中文宽100自然高186.484375大于viewport高40；静态计算复用同一发布、不访问节点；只改viewport高不重测，增宽及字体/provider版本改变后重测，warm=fresh |
| `independent_content_width` | Content容器独立宽度被max180约束，再以最终180测真实高度；一次独立查询＋一次确定宽度查询，后续命中相互独立的缓存 |
| `demand_driven_measurement` | 确定且填满的Expand文字框不询问未使用的自然高；移动到Content祖先后按真实依赖询问；Fixed高度不因宽度变化重测未使用的自然高 |
| `provider_metrics_reuse` | 换行、尾空格、空串、英文长词、中文在反复窄/宽查询下与新建DirectWrite对象一致；专门的3次查询仅创建1个metrics对象，不声称内部shaping只发生一次 |
| `transactional_failure_and_reentry` | 三叶变更后第三次测量失败，保留旧publication、input revision、视图地址和全部几何；provider错误码77保留；重试重新测3叶；所有编辑/布局重入busy；NaN/普通异常拒绝，provider bad_alloc传播并可恢复 |
| `geometry_failure_cache_rollback` | 真实度量已成功、随后锚定坐标溢出失败：错误指出具体节点，保留旧发布；修正定位后必须重测，证明几何阶段失败也不提交本次测量缓存 |
| `local_cache_and_slot_reuse` | 只改一个文字叶只调用1次provider；等宽兄弟重排不重测且保留ID，结果匹配fresh；slot复用后清空旧节点缓存 |
| `anchors_and_logical_precision` | 800×450父框右下80×40框得到710/400；嵌套0.4+0.4保留逻辑0.8；不同DPI纯值消费不回写；out-of-flow子项不消耗普通行分配 |
| `invalid_inputs_and_dependencies` | min/max、padding、零权重、NaN、非法根、负/Inf viewport拒绝；未支持的overlay自然需求组合不发布半树；缺失真实内容保留provider错误码91，不伪装为零框成功 |
| `owned_geometry_lifetime` | 编辑不替换借用结果，下一次成功发布才结束旧借用；拥有式几何副本经过发布和Document析构仍保持原值 |
| `explicit_deep_tree` | 真实深度4096、4097节点、底部DirectWrite叶：1次度量、12291次尺寸/定位访问；深子树删除不靠递归，旧拥有式快照继续保留4097个框 |

几何比较容差为`0.0002 + 0.000002 × max(abs(a),abs(b))`；某些精确可表达几何另做相等检查。本机固定输入、字体环境下通过不等于任意字体/构建的bitwise保证。


## Flex/Grid覆盖

flex_allocation_and_distribution、flex_wrapping_and_alignment、flex_real_text_baseline、grid_tracks_spans_and_placement、grid_limits_and_nested_content、flex_grid_editor_end_to_end覆盖有界伸缩、按需测量、全部分布、换行/reverse、真实基线及最终几何传播、跨行跨列、同跨度顺序无关、自动放置、容量和失败保旧。

## 运行

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target ui_layout_acceptance
cmake --build --preset release --target ui_layout_acceptance
```

同一消费者支持--performance、--text-costs、--virtual-performance、--editor-performance、--flex-grid-performance、--audit-performance。历史采样按版本保存在[历史性能](../../research/ui-layout-reassessment/native-performance-history.md)；它们不替代当前构建结果。out目录是本机生成证据，不随源码提交；新检出按正式入口生成自己的验收记录。
