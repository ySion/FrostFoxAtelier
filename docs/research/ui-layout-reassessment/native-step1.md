# 自研布局首步：原生实验与落地建议

> 后续进展：用户已授权并完成生产首增量，当前实现与正式验收见[布局模块](../../modules/ui-layout/README.md)。下文保留首步探索结束时的阶段快照；其12项结果不代替后续生产17项验收，原先的合同提案已迁入[正式合同](../../modules/ui-layout/contract.md)。

状态：**首步研究与隔离原生实验已完成；开发/发布配置各 12/12，通过退出码均为 0。** 这是尺寸依赖、真实测量和结果发布的探索证据，不是生产布局库验收。本页记录阶段实验，不另立永久合同；当时的合同提案已进入[当前正式合同](../../modules/ui-layout/contract.md)。

## 1. 本次回答的问题

1. 带权扩展、min/max 和填满/对齐如何组合，才能在重排及极端权重下保持合理分配？
2. 文字能否先确定宽度、再请求真实自然高度，而不是受 viewport 剩余高度压缩？
3. 计算失败后能否同时保留旧几何和布局测量缓存，并安全重试？
4. 静态计算、局部叶变化、深链和延迟几何消费，分别已证明什么、仍缺什么？

延续既有 Exa 来源研究，不改变自研方向，不再比较生产后端。本轮的新增结论来自下述自有原生程序；工业背景仍见[游戏 UI](industry-game-ui.md)与[编辑器 UI](industry-editor-ui.md)报告，未用新的网络搜索代替实测。

## 2. 实际产物与依赖

实验位于 [`acceptance/ui_layout_step1_probe/`](../../../acceptance/ui_layout_step1_probe/)：

| 文件/目标 | 内容 |
|---|---|
| `kernel.hpp` / `kernel.cpp`；`layout_step1_kernel` 静态库 | 纯 C++ 值批次、树校验、行列/覆盖定位、尺寸依赖、测量缓存、失败发布 |
| `main.cpp`；`layout_step1_probe` 可执行程序 | 真实 DirectWrite Adapter、12 组无窗口场景及 JSON 行输出 |
| `CMakeLists.txt` | 独立实验构建，核心目标没有 Runtime/JIT/RHI/DirectWrite 链接依赖 |
| `run.ps1` | 准备现有工具链，构建开发/发布配置，记录输入源码哈希、锁文件内容、程序哈希与原始输出 |

核心没有 Windows/文字库头文件。实验程序才链接 DirectWrite、共享 mimalloc，并复用项目 `global_new_delete.cpp`；运行检查了 mimalloc 版本及重定向。未改根 CMake、正式依赖锁、工具链锁或生产目录。**没有验证非 Windows 编译，也没有渲染/输入验收。**

实验输入是全量 `set_nodes`，ID 是调用者给定的整数；它不是生产节点池、generation handle 或增量编辑接口。行列/覆盖使用同一原生内核，不是三份 solver Adapter。

## 3. 双配置结果

| 配置 | 实际 CMake 类型 | 原生场景 | 退出码 | stderr / 超时 |
|---|---|---:|---:|---|
| dev | Debug | 12 PASS / 0 FAIL | 0 | 空 / 否 |
| release | Release | 12 PASS / 0 FAIL | 0 | 空 / 否 |

运行字体指定 Segoe UI、locale 为 en-US，内容包含无空格中文、英文长词、显式换行、尾随空格和空字符串；Unicode 排版由真实 DirectWrite 完成，不用 ASCII 分词估算。中文可能使用系统字体回退；未记录整套字体文件哈希，不承诺跨机器几何 bitwise 一致。工具版本以正式锁文件和本次 `environment.json` 为准，不在本页另建版本清单。

| 场景名 | 输入/预期与实际结果 |
|---|---|
| `bounded_pool` | 池 300、等权、上限 `[120,无,30]` → 同身份 `[120,150,30]`，全部 6 种顺序通过。池 100、min `[80,0]`、无 max → `[80,20]`；给第二项 max=10 后 → `[90,10]`。池不足保留硬 min；全部饱和后留白。 |
| `weight_dynamic_range` | 权重 `[1e30,1,1e-30]`、上限 `[1,1,无]`、池 100 → `[1,1,98]`。既检分配函数，也检真实行容器全部 6 种顺序。 |
| `slots_and_fill` | 100 宽分成两个 50 槽；第一项不填满、min=20、居中 → 框宽20、x=15，第二项从x=50开始。列容器同样验证80/20分配。 |
| `known_width_natural_height` | 20号中文、宽100、viewport高40 → 自然高 **186.484375**，不截成40；仅改变 viewport 高度不触发叶重测，增宽后自然高降低；字体尺度/版本改变后重测，warm 与 fresh 一致。 |
| `independent_content_width` | 内容宽容器显式 max=180、父宽400 → 容器及文字宽180；仅一次独立宽度查询加一次最终宽度查询，真实高度匹配 fresh。 |
| `provider_metrics_reuse` | 换行、尾空格、空串、长词和中文在反复窄/宽变化后与新建排版对象一致；单独的3次宽度查询只创建1个排版对象。**没有测量内部 shaping 次数。** |
| `failed_publication_and_retry` | 三个真实文字叶更新，第三次回调失败：旧 publication、视图地址和几何不变；重试重新调用全部3个叶，总共6次，失败尝试的新缓存未泄漏。回调编辑/重入被拒绝；NaN返回、异常及恢复也通过。 |
| `leaf_measurement_locality` | 两个文字叶只改一个：provider仅重测1个；本次核心仍访问9次，**不是局部几何调度已经完成**。warm 与 fresh 一致。 |
| `parent_anchors` | 800×450父框、80×40子框、右下角偏移−10 → `(710,400)`；嵌套0.4+0.4保持逻辑0.8。不同DPI的纯值消费不回写几何；未验证渲染或完整共边政策。 |
| `invalid_inputs_and_limits` | 环/重复ID、非法min/max、NaN、未支持的Content覆盖依赖、节点/深度超限、缺失内容均拒绝；有限输入累加造成数值溢出时保留旧发布。 |
| `explicit_deep_schedule` | 深度4096、4097节点、底部真实文字：1次测量、12291次三阶段访问，显式遍历通过。**不是产品深度或工作量上限。** |
| `owned_geometry_lifetime` | 输入编辑不替换借用视图；拥有式几何副本经过下一次成功发布和内核析构后仍可读，值保持不变。**不证明旧文字/绘制包租期。** |

几何比较使用程序中的绝对＋相对容差 `0.0002 + 0.000002 × max(abs(a),abs(b))`；分配函数反例另用更紧容差。所有表中 PASS 均来自同一版源码的两种构建，不把源码推断写成已运行场景。

## 4. 本轮确实发现并修复的错误

第一轮两种配置均为 **10 PASS / 1 FAIL、退出码2**，失败项为 `weight_dynamic_range`；其他行为已通过。旧事件扫描把活动权重保存在一个累加变量中，大权重加入时较小项可能已被舍入丢失，随后减去大权重不能恢复那些项。因此无法得到应有的 `[1,1,98]`。

修正保留每项活动权重，用可复用数组组成树形和；事件只更新对应叶和祖先，不从已舍入的单个全局和反向扣除。断点排序和更新是有界过程，不引入“反复分配直到收敛”。随后追加真实行容器的6种排列，以及独立的拥有式几何寿命场景；最终两配置各12/12。该修复是正确性修复，没有进行性能收益声明。

下界不是永久冻结值：例如min `[80,0]`、第二项max=10、池100，正确值是 `[90,10]`，不能把两个端点同时锁死成 `[80,10]`。

## 5. 已支持的结论与不能外推的结论

- **支持进入生产实现的语义提案**：整池带权分配；槽与实际框分离；按需独立宽度＋最终 `H(W)`；失败后不提交新几何或布局缓存；默认借用结果、跨期显式复制。
- **当前仍是全树几何重算**：变化输入会重建/遍历值批次并执行三阶段；缓存减少 provider 查询，不等于增量布局调度。静态输入直接复用结果的路径已验证，但没有分配计数和端到端性能基线。
- **测量对象只是 metrics scratch**：改变可变 DirectWrite 对象的宽度不会自动保留旧绘制版本；布局失败不会回滚 provider 自己的对象。正式 composition 必须管理绘制结果租期，不能凭旧几何继续使用新文字对象。
- **还没有生产身份与生命周期实现**：失效 handle、跨 Document ID、slot复用/代际耗尽、真实创建/移除/重排、回调销毁及并发均未验收。回调期间必须保持 Document 和 Adapter 存活，不假称能够安全拒绝已经发生的 C++ 销毁。
- **资源证据有限**：验证显式节点/深度拒绝与若干数值错误；没有 OOM 注入、分配器故障恢复、工作预算或任意数值范围证明。depth4096只代表这一确定场景。
- 共享列 Inspector、换行/物品格、基线、多向文字全覆盖、虚拟集合与滚动稳定策略、RHI/窗口/输入、业务语言绑定均未由本次程序实现或证明。

## 6. 复现与原始材料

在布局专属 worktree 的 PowerShell 中运行：

```powershell
./acceptance/ui_layout_step1_probe/run.ps1 -Configuration both
```

本次为避免重复下载，实际使用已有 mimalloc 源目录：

```powershell
./acceptance/ui_layout_step1_probe/run.ps1 -Configuration both `
  -ReuseMimallocSource E:/FrostFoxAtelier/out/release/_deps/mimalloc-src
```

可选本地源覆盖要求该目录已有正确依赖来源；运行中的版本检查不等于重新审计整棵依赖源码。省略覆盖参数时走项目正式 FetchContent 锁定路径。两个构建分别生成 `layout_step1_kernel.lib`、`layout_step1_probe.exe`，程序由 runner 捕获并等待退出，不以编译成功代替执行。

原始材料在 `out/review/ui-layout-step1/`：

- [environment.json](../../../out/review/ui-layout-step1/environment.json)：实验源码 SHA-256、正式工具链/依赖锁内容、运行环境和启动时间。
- [runs.json](../../../out/review/ui-layout-step1/runs.json)：最终双配置原始 stdout/stderr、退出码、超时标志和程序哈希；单配置副本为 `dev-run.json`、`release-run.json`。
- [initial-failure.json](../../../out/review/ui-layout-step1/initial-failure.json)：修复前双配置失败输出及对应源码环境记录；没有被最终 PASS 覆盖。
- `dev-configure.log` / `release-configure.log`、`dev-build.log` / `release-build.log`：实际配置和构建输出；初次完整反馈记录保留在 `initial-console.log`。

`out/` 是本地生成证据，不保证随源码交付。缺失时以本页摘要和实验源码重新运行，不声称对方已有本机二进制/日志；源码未提交时以记录的文件哈希对应证据，不虚构提交 revision。

## 7. 首个生产增量的实施清单

1. 审查[首增量合同提案](../../modules/ui-layout/decisions.md)，把确定语义迁入正式 `contract.md` / `acceptance.md`，将旧 Yoga 合同保留为明确历史材料。
2. 在 `src/ui_layout/` 实现原生拥有者、稳定局部身份、结构编辑/校验；不直接导出实验的整数ID与全量batch接口。
3. 用同一内部测量/安排机制落地行列、覆盖/父框锚定、整池带权分配、真实叶测量、缓存及原子发布；将本次反例迁入正式消费者。
4. 在 `acceptance/ui_layout/` 验证身份失效、结构编辑、资源/失败、快照和真实文字；沿既有工程规则完成最小目标接入及双配置运行。
5. 共享列、换行/重复格、实际增量及虚拟视图继续按 S08 分增量实施，不由本次12个场景抵扣。

结论：首步已经有可运行证据支持上述窄合同提案；目前交付的是探索程序和可审查的生产入口，**不是“生产布局库已完成”，也没有“比第三方更快”的结论。**
