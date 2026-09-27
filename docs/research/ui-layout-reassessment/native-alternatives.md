# 原生替代项：RmlUi、LVGL、Slint

> 当前筛选以文末「附录 A：当前 release 的定点复核与结论修正」为准。主体保留旧版机制样本；不得沿用旧 Slint 动态 Grid 限制排除当前版本。

## 结论与证据边界

**[推断] 三者不默认作为独立通用 solver 引入，但完整 toolkit 路线不能按裸 solver 的门槛排除。保留 LVGL 的有界机制 probe；若评估完整 UI 子树，则让 RmlUi 与 Slint 竞争一个系统级对照名额。** Slint 动态 Grid 的旧版排除理由已由附录 A 撤回。本轮仅阅读主源，没有运行构建、测试、基准、lint 或格式化，没有性能排名。

三个对象都是 **framework／UI toolkit**，不是只接收外部节点并返回矩形的独立 solver；内部有求解算法和测量协议，不等于对外提供稳定的独立布局库。是否接受第二套 UI 树、样式和生命周期，是比语言或单次求解速度更先决的选择。不再加入第四个轻量候选：本轮已有 Yoga、Taffy、Clay 专席，补充一个语义更窄的名字不会增加有效证据。

下文以固定 tag 为可复核基线，**不声称这些是当前最新版本，也不把版本限定的缺口外推到后续版本**。版本选择是为了锁定完整源码及对应文档，不是生产依赖建议。事实来自实际获取的官方页面／固定源码；上游性能描述不当作本机测量。发布页确认的日期均为过去日期，不使用搜索索引日期推定发布事实。[R1][L1][S1]

## 一、身份、版本与接入边界

| 对象 | 固定审查版本及发布日期 | 许可与接入事实 | 分类 |
|---|---|---|---|
| RmlUi | `6.1`；2025-04-20 | MIT；C++14 起；CMake；标准库，默认 FreeType 可替换；宿主提交输入并实现渲染接口 | 自有 DOM、RCSS、控件、文本及布局的 UI middleware |
| LVGL | `v9.3.0`；2025-06-03 | MIT；C 源码；需配置 `lv_conf.h` 及对象／显示运行时，布局不是独立 C 数组函数 | 完整 GUI toolkit；flex/grid 是对象系统插件 |
| Slint | `v1.12.1`；2025-06-25 | GPL-3.0-only、Royalty-free 2.0 或商业许可择一；源码构建需 Rust/Cargo、CMake≥3.21、C++20；该发布有 Windows x64 MSVC SDK | 声明式语言、编译器、属性运行时、控件与后端组成的 toolkit |

**[事实]** 上表分别来自固定 README／许可／发布页。[R1][R2][L1][L2][S1][S2] 发布记录证明这些基线存在、维护者发布过修复，不证明截至本轮仍有维护 SLA；最新维护节奏及升级差异属于待验证项。

**[事实] Slint 的 royalty-free 不等于 MIT。** 该许可允许集成进桌面／移动／Web 应用，但有署名条件；不允许脱离应用单独分发、不允许嵌入式用途，也不允许分发暴露其部分或全部 API 的应用。**[推断]** 因而“抽出 solver 再作为引擎 SDK 暴露”不能仅凭免费桌面使用权判定许可适配；需明确发行形态并选择合适许可，本文不代替法律判断。[S3]

## 二、RmlUi：适合作为完整系统对照，不是低成本 solver 抽取

### 所有权、布局与测量

**[事实]** `Element` 持有自己的盒信息、样式接口、父子关系及滚动信息；`FormattingContext::FormatIndependent` 按 computed style 进入 block、table、flex，或 replaced-element 路径。6.1 的分派没有 CSS Grid；table 可以在同一表内对齐列，不能据此宣称跨独立表／组件共享轨道。浮动、绝对／固定定位和独立格式化上下文属于自身布局语义。[R3][R4]

**[事实]** 自定义元素有 `GetIntrinsicDimensions(dimensions, ratio)`、`GetBaseline()`、`SetBox()`／`SetOffset()`。但 intrinsic 回调没有 available-width 参数；它不是任意控件通用的 `Measure(constraints) → Arrange(rect)` 协议。字体接口提供 `GetStringWidth`、字体度量、shaping context 和字形几何生成；测量与绘制都要对接，不能只塞一个字符串宽高回调。`GetVersion()` 明确负责使字体几何重新生成，并未承诺字体度量改变时自动重排所有相关布局。[R4][R5]

### 失效、分配与错误

**[事实]** `ElementDocument::UpdateLayout()` 在文档 `layout_dirty` 时从文档根调用 `LayoutEngine::FormatElement`，随后清 dirty；代码明确忽略格式化期间产生的文档 dirty，滚动条迭代由布局内部处理。因此静态帧可以跳过这一布局入口，但不能把叶更新解释为仅重算祖先链，更不能把整个 `Context::Update` 说成零成本。[R6]

**[事实]** 格式化创建 `UniquePtr` 布局盒；布局结束调用递归的 `ClampScrollOffsetRecursive`。格式化失败记录日志，入口没有结构化错误返回；公开 FAQ 不保证线程安全，多线程访问须同步。[R3][R7] **[待验证]** 临时分配次数、缓存复用、深树上限、回调期间修改树的可恢复性，以及失败后旧几何是否仍可使用，均未获足以作保证的证据。

**最强支持 [推断]：** 不要求完整浏览器，且 renderer／字体可替换，适合真实游戏 HUD、文本和滚动对照；可以直接检验“借用成熟 UI 系统”是否比自建控件体系划算。**最强反对 [推断]：** 若只需要几何求解，就必须同时适配 DOM、computed style、盒模型与字体几何，抽取代价高；6.1 的文档级失效入口和非 Grid 语义也不支持先验宣称复杂编辑器最合适。[R2–R7]

## 三、LVGL：值得研究的 grid，但不能把嵌入式定位等同高性能

### 轨道与自定义协议

**[事实]** grid 支持固定像素、`LV_GRID_CONTENT`、FR 轨道和 span；子对象须显式指定单元格。style 保存轨道描述符数组；**共用描述符不是共用已求出的自然列宽**。9.3 文档明确：subgrid 只解析一层，且父网格的 CONTENT 轨道不在 subgrid 中处理。因此 S3 的多层分组 Inspector 不能直接套用“支持 subgrid”结论。[L3][L4]

**[事实]** `lv_layout_register(cb, user_data)` 注册容器布局回调，回调操作真实 `lv_obj_t`；`LV_EVENT_GET_SELF_SIZE` 返回对象自然尺寸，`lv_obj_refresh_self_size()` 触发布局失效。label 在样式或尺寸变动时刷新文本，并使用当前内容宽度或 CONTENT 条件计算 `lv_text_get_size`；另有 `invalid_size_cache` 缓存。这是可借鉴的 custom arrange＋自然尺寸协议，但不是绕过对象／style系统的纯函数接口，也不自动提供项目所需的 shaping。[L5][L6][L7]

### 增量不等于局部遍历

**[事实]** dirty 同时标记对象和其 screen；`lv_obj_update_layout` 只要 screen 仍 dirty 就重复 `layout_update_core(screen)`。后者先递归访问所有子对象，再检查本节点 `layout_inv`。**[推断]** 单轮访问量随已实例化 screen 树增长，而不是只随脏节点数增长；多轮固定点收敛必须另记次数。没有 screen dirty 时可跳过这些轮次。[L6]

**[事实]** `lv_grid.c` 为 x/y/w/h 轨道数组分配并释放内存，subgrid 还可创建临时模板；每个 CONTENT 列会扫描子对象，且忽略 `col_span != 1` 的项目。**[推断]** 该部分存在“CONTENT 轨道数×子对象数”的工作量，不能宣称 grid 是无分配的线性单遍算法。长文本绘制 hint 只是文本绘制辅助，并不等于列表行实例虚拟化。[L4][L7]

**[事实]** LVGL 不默认线程安全；其布局入口的布尔重入保护不是跨线程 mutex。布局更新循环未在该入口设置收敛次数上限；API／分配路径含断言和日志，不是事务式 `Result`。**[待验证]** OOM、非法轨道及回调不断重新置脏时的行为必须隔离验证。[L5][L6][L8]

**最强支持 [推断]：** MIT、C 实现、显式轨道和可注册布局回调，适合用很小场景验证哪些机制应借鉴。**最强反对 [推断]：** 框架对象树、screen、事件、字体／绘制和 style 都进入实际路径；仅取 grid 仍需重新定义自然尺寸、失效和坐标写回边界，而其共享轨道限制正撞 S3。[L3–L8]

## 四、Slint：机制有价值，独立抽取与 SDK 分发不成立为默认路径

**[事实]** 固定版本 `GridLayout` 的 row/col/span 绑定要求编译期常量。内部 `GridLayoutCellData` 带轨道索引、span、`LayoutInfo`；`solve_grid_layout` 将输入转换为轨道临时 `Vec`，处理 min/max/preferred/stretch，再返回 `SharedVector`。C++ 包装位于 `private_api`，而不是独立稳定 solver API。两个 Grid 不会仅因列号相同而共享自然轨道。[S4][S5]

**[事实]** 文本 `layout_info` 横向先获取无换行自然宽度；纵向换行测量传入当前 width；测量经 `WindowAdapter.renderer().text_size`。属性引擎记录依赖、dirty 后重求值，并包含堆分配；模型允许业务数据自有，但 UI 元素／布局缓存属于运行时，模型变更须通知。`ListView` 文档承诺仅实例化可见元素，不能推广为任意 Grid、Tree 或 ScrollView 都自动虚拟化。[S6–S9]

**[事实]** 属性递归访问会断言报 `Recursion detected`；C++ API 提供 `invoke_from_event_loop` 把跨线程操作送回事件线程。文档化的 `ListView` 可见实例化是**上游自报**，本次未测固定／可变行高及 C++ 路径。**[待验证]** 独立自定义 measure/arrange 的稳定公开扩展点、超深树和错误回滚边界尚未建立。[S8–S10]

**最强支持 [推断]：** 约束汇总与安排分离、依赖驱动缓存、model 通知和可见实例化值得设计借鉴。**最强反对 [推断]：** 采用它不只是链接 Rust 算法，还带入语言编译、属性运行时、文本 renderer 路径及许可约束；private solver 和固定版动态 Grid 限制削弱“低代价替换布局模块”的主张。[S2–S10]

## 五、S1–S9 映射

下表是基于上述主源的**[推断] 能力映射**，不是验收通过；`需上层` 不表示不可能。

| 场景 | RmlUi 6.1 | LVGL 9.3 | Slint 1.12.1 |
|---|---|---|---|
| S1 HUD | 定位、叠层、dp 可用；安全区由宿主 | 对象对齐／浮动；缩放与安全区需约定 | 显式几何与绑定；游戏宿主边界需适配 |
| S2 格子／换行 | flex；规则格可组合；非 CSS Grid | 显式 grid＋flex；自动布格策略另定 | Grid／线性布局；固定版动态轨道不能先验通过 |
| S3 Inspector | 同一 table 列对齐；跨分组需设计 | CONTENT／subgrid 限制是主要风险 | 同一 Grid 有约束汇总；跨 Grid 共享需上层 |
| S4 splitter/docking | 可改变约束及定位；docking 归上层 | 可注册布局及外部改尺寸；docking 归上层 | 属性驱动；docking 交互与树编辑归上层 |
| S5 大列表 | 未建立通用自动行虚拟化证据 | 未建立通用自动行虚拟化证据 | ListView 有可见实例化；复杂树表仍待验证 |
| S6 本地化 | 字体接口、基线、换行；真实 shaping 必测 | 宽度相关 label；混排基线／shaping 必测 | 纵向依赖 width；测量依赖 renderer |
| S7 嵌套滚动 | 有 overflow 与滚动条；最终递归 clamp | 坐标与滚动修正进入对象路径 | ScrollView/ListView；需区分裁剪与实例化 |
| S8 更新 | 文档 dirty 入口，不能推定局部求解 | screen 级递归扫描＋dirty 对象求解 | 属性依赖缓存；模型通知须正确 |
| S9 边界 | 日志、递归、无事务返回保证 | 递归、分配、重入与收敛风险 | 属性递归断言、private ABI、运行时边界 |

## 六、筛选决定与重开条件

| 对象 | 本轮决定 | 主源依据与可推翻条件 |
|---|---|---|
| LVGL | **probe ①：轨道及失效机制**；暂不采用完整框架或抽取生产 solver | 一层 subgrid／CONTENT 限制、screen 遍历和临时分配已确认。[L3–L6] 若真实 Inspector 不需跨嵌套自然宽度，或小型桥接能保留对象语义且端到端成本合格，可重开 |
| RmlUi | **probe ②：完整游戏 UI 基线**；筛掉“低代价独立 solver”路径 | 自有 DOM／字体几何／格式化上下文耦合。[R2–R7] 若项目愿意让它拥有完整 UI 子树，而非只求矩形，可重开系统集成；若必须保留自有树，应先证明可维护的抽取边界 |
| Slint | 不列本轮 probe 短名单；保留机制研究 | solver 属 private API，royalty-free 有 SDK/API 暴露限制，固定版本 Grid 存在编译期结构约束。[S3–S5] 重开需发行许可明确，且证明稳定嵌入路径或接受整体 toolkit；后续版本若解决结构限制，须固定新 tag 重审 |

## 七、可执行反例与真实路径 probe 方案

以下均为**[待验证] 后续实验设计，本次未执行**。必须经过项目真实输入树→候选适配→真实文本测量→几何写回／消费，不能只计 solver。记录逻辑项数、实例节点数、可见数、访问节点、测量次数、分配字节、收敛轮数、几何差异及端到端时间；实验规模不是产品上限。

1. **P1：跨组共享自然列宽（S3，LVGL 优先）。** 建三个可折叠属性组，每组两列；标签分别使用短英文、长德文和中文；中间组再嵌两层 wrapper。根列设 CONTENT＋FR，子组尝试 NULL 描述符 subgrid。依次折叠最长标签组、改变文本、修改容器宽度。检查所有编辑器左边缘、换行高度和隐藏组是否影响列宽。若必须平铺破坏组件边界或每帧手工扫描所有标签才能成立，即反驳“subgrid 自然满足 Inspector”。共享同一描述符数组不能算通过。

2. **P2：叶变更是否真增量（S8，LVGL／RmlUi）。** 构造宽树并同时保留实际可见规模，分别运行静态帧、只改一个数字标签、祖先 resize、增删子树。LVGL 计 `layout_update_core` 访问及外层 while 次数；RmlUi 计文档格式化入口、文本宽度查询和实际访问。若单叶改动仍扫描全 screen／重排文档，它可仍满足预算，但必须撤回 O(脏路径) 或静态零成本说法。

3. **P3：宽度—高度—滚动条反馈（S6/S7，两候选）。** 多语文本加入 fallback 字体及混合字号；把容器宽度设在恰好换行边界，开启嵌套滚动，再增加一行错误提示。另在同字体标识下替换字体度量。检查行数、baseline、content extent、滚动条出现后是否稳定、是否再次测量以及旧行高是否残留。RmlUi 单独测试仅变更字体 `GetVersion()`：不得把几何刷新误当布局失效证据；测量与绘制须使用同一真实字体路径。

4. **P4：边界与恢复（S9，两候选）。** 独立进程内逐级增加树深；LVGL 自定义布局回调故意交替设置宽度，使后续事件重新置脏；RmlUi 自定义元素／字体提供者测试资源加载失败与约束异常。设置实验外部超时，记录断言、日志、栈／内存峰值、是否保留有效旧结果。超时或崩溃只能证明该输入风险，不能推广成候选全面不可靠。

5. **P5：虚拟化不是裁剪（S5，公共验收夹具）。** 用一万逻辑行、约几十可见行，分别固定和可变行高，执行滚动、跳转末尾、离屏插入和语言切换。候选仅裁剪却仍实例化全部行不能宣称虚拟化。LVGL／RmlUi 若由宿主实施窗口化，单列宿主成本；Slint 仅在重开后用 C++ ListView 同夹具核验，不能引用文档代替测试。

## 八、仍未知及移植建议

**[待验证]** 三者在目标 Windows x64/C++23 配置下的 ABI／CRT、实际二进制依赖、OOM 策略、最大有效深度、复杂字体质量、可变行高滚动稳定性与失败后恢复，均无本机证据。也未审查后续版本是否改变上述局限。

**[推断]** 值得借鉴的是 LVGL 的显式轨道和可插拔容器回调、RmlUi 的宿主接口与文本／布局／滚动协作、Slint 的约束信息与依赖通知分离；不应直接照搬它们的整套对象所有权。移植成本必须同时计算节点映射、style 翻译、自然尺寸协议、失效桥接、资源寿命、文本绘制一致性和输出复制。旧布局／旧 Yoga 接口不构成保留理由，但“可替换旧接口”也不等于获得免费框架迁移。

## 直接主源

所有源码地址固定 tag；非版本 FAQ 仅支撑其明确陈述，不用于推定新版本算法。

- [R1] RmlUi 6.1 发布：https://github.com/mikke89/RmlUi/releases/tag/6.1
- [R2] 定位、工具链、宿主／字体边界：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/readme.md
- [R3] 格式化分派及 MIT 源码许可：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/Source/Core/Layout/FormattingContext.cpp
- [R4] 元素、盒、intrinsic、baseline：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/Include/RmlUi/Core/Element.h
- [R5] 字体测量、几何和版本合同：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/Include/RmlUi/Core/FontEngineInterface.h
- [R6] 文档布局失效：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/Source/Core/ElementDocument.cpp
- [R7] 布局结束递归与错误：https://raw.githubusercontent.com/mikke89/RmlUi/6.1/Source/Core/Layout/LayoutEngine.cpp ；线程 FAQ：https://mikke89.github.io/RmlUiDoc/pages/faq.html
- [L1] LVGL 9.3.0 发布：https://github.com/lvgl/lvgl/releases/tag/v9.3.0
- [L2] MIT 许可：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/LICENCE.txt
- [L3] 固定版 grid／subgrid 文档：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/docs/src/details/common-widget-features/layouts/grid.rst
- [L4] 轨道分配、CONTENT 扫描、span：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/src/layouts/grid/lv_grid.c
- [L5] 自定义布局注册：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/src/layouts/lv_layout.c
- [L6] dirty、递归、自然尺寸与收敛：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/src/core/lv_obj_pos.c
- [L7] label 宽度约束、缓存与刷新：https://raw.githubusercontent.com/lvgl/lvgl/v9.3.0/src/widgets/label/lv_label.c
- [L8] 官方线程合同：https://lvgl.io/docs/open/9.3/details/integration/adding-lvgl-to-your-project/threading.html
- [S1] Slint 1.12.1 发布及 SDK：https://github.com/slint-ui/slint/releases/tag/v1.12.1
- [S2] 构建依赖：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/api/cpp/README.md ；许可选择：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/LICENSE.md
- [S3] Royalty-free 2.0 完整条款：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md
- [S4] 固定版 GridLayout 文档：https://docs.slint.dev/1.12.1/docs/slint/reference/layouts/gridlayout/
- [S5] solver 数据、分配及约束算法：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/internal/core/layout.rs
- [S6] 文本方向测量及 renderer 耦合：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/internal/core/items/text.rs
- [S7] 固定版 ListView 文档：https://docs.slint.dev/1.12.1/docs/slint/reference/std-widgets/views/listview/
- [S8] 属性依赖、分配和递归错误：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/internal/core/properties.rs
- [S9] 模型通知合同：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/internal/core/model.rs
- [S10] C++ private layout 包装及事件线程：https://raw.githubusercontent.com/slint-ui/slint/v1.12.1/api/cpp/include/slint.h

## 附录 A：当前 release 的定点复核与结论修正

本附录保留正文的历史固定样本，只复核足以改变排除理由的机制，**不是对新版本的完整重审**。本席未修改文件，未运行构建、测试或性能测量。对应发布页确认：RmlUi `6.2` 发布于 **2026-01-11**，LVGL `v9.5.0` 发布于 **2026-02-18**，Slint `v1.17.1` 发布于 **2026-07-07**。[A1–A3] 来源固定至上述 tag；不把 release API 的 `target_commitish` 当作已经解析验证的 tag commit。

### A.1 RmlUi 6.2：Grid 缺口仍在，但存在自定义布局钩子

**[事实]** `Style::Display` 仍没有 Grid；`FormattingContext::FormatIndependent` 仍分派 block、table、flex 或 replaced-element。因此正文“无原生 CSS Grid”的判断在 6.2 上仍成立。[A4][A5]

**[事实]** `Element` 仍提供 `GetIntrinsicDimensions(Vector2f&, float&)`、`GetBaseline()`、`SetBox()`、`SetOffset()`，并有 protected `OnLayout()`、`OnResize()`、`DirtyLayout()`。**不能将其概括成“不允许 custom layout”。** 可以派生元素并参与布局通知、几何设置；但 intrinsic 回调仍不接收显式 available-size 约束，`OnLayout()` 也不是标准化的 `Measure(constraints) → Arrange(rect)` 双阶段接口。[A6]

**[推断]** 保留完整游戏 UI 集成基线，仍不默认采用低代价独立 solver 抽取路径。6.2 probe 应检验自定义元素中的宽度依赖高度、重新置脏、几何写入时序；不能仅因没有统一 measure/arrange 形式就先判自定义控件不可能。

### A.2 LVGL 9.5.0：共享轨道限制保留，扩展与错误机制需要更新

**[事实]** 固定版 Grid 文档仍明确：subgrid 只解析一层，父 grid 的 `LV_GRID_CONTENT` 轨道不在 subgrid 中处理。实现的缺失描述符路径仍只读取直接父对象，CONTENT 尺寸扫描当前容器的子对象。因此深层 wrapper 下跨组共享自然列宽仍未获得原生保证。文档新增的 content／percent min/max 支持，不等于解决了这个问题。[A7][A8]

**[事实]** 当前布局扩展入口是 `lv_layout_create(lv_layout_callbacks_t, void*)`，回调集合同时包含 `layout_update_cb` 与 `get_min_size_cb`。后者签名为 `bool (*)(lv_obj_t*, int32_t *req_size, bool width, void *user_data)`。旧 `lv_layout_register` 已标为 deprecated，并转接新入口。[A9][A10]

**修正：** 正文 9.3 样本中“容器提供安排回调、自然尺寸通过对象事件”的机制描述，不能直接当成当前版本全部能力。9.5 已允许布局回调提供最小尺寸；但仍依赖真实 `lv_obj_t`，不是独立二维约束求解协议。

**[事实]** grid 的 `calc`、`calc_rows`、`calc_cols` 已返回 `lv_result_t`；缺失父描述符可返回 `LV_RESULT_INVALID`，`grid_update` 遇失败停止该次更新。不能把 9.3 错误路径原样外推。**[待验证]** 这不证明公开事务式失败返回、OOM 安全、全部中间资源释放或旧几何恢复均已具备。[A8]

**[推断]** LVGL 有界 probe 价值增强：应使用新 minimum-size＋arrange 回调，原跨组共享自然列宽反例仍有效。当前 S8 遍历／收敛路径没有在本次定点复核中重新审查，不能自动继承正文 9.3 的复杂度结论。

### A.3 Slint 1.17.1：必须撤回动态 Grid 旧排除理由

**[事实]** 固定 `v1.17.1/CHANGELOG.md` 明确记录：[A11]

| 版本与日期 | 足以影响本轮判断的变更 |
|---|---|
| 1.15.0，2026-02-04 | Grid 的 row、col、colspan、rowspan 可运行时改变，支持 if／for |
| 1.15.1，2026-02-12 | 修复 repeated rows 中 colspan／rowspan |
| 1.16.0，2026-04-16 | C++ ListView 与 Rust 共用实现，仅实例化可见项 |
| 1.17.0，2026-06-24 | 修复不同行高 ListView 拖动 |
| 1.17.1，2026-07-07 | 修复 Grid 越界行列 panic，以及 conditional cells 的 colspan |

动态 Grid 不仅有发布说明：**[事实]** 固定 tag 编译器 `lower_layout.rs` 处理运行时行列表达式；C++ 包装含 `organize_grid_layout`、repeater indices／steps 和 grid repeater cache；运行时 `GridLayoutInputData` 携带行列及 span，`GridLayoutOrganizedData` 存储运行时组织结果。[A12–A14]

**仍有具体表达边界：** 编译器对 auto-numbering 与 runtime expression 混用保留错误／警告诊断。因此“支持动态”不等于任意自动编号与运行时坐标组合均可通过。[A13]

**来源冲突：** 实际获取的 `docs.slint.dev/1.17.1/.../positioning-and-layouts/` 网页仍写“行列必须编译期确定，Grid 不支持 for／if”，与固定 tag 的 changelog、编译器、运行时、C++ 包装相冲突。**[推断]** 网页存在滞后或版本服务问题；本次未定位其根因。当前判断采用多项固定源码交叉证据，不能用这份网页继续排除 Slint。[A11–A15]

#### 精确保留的限制

1. **[事实] 独立 API 边界仍在。** `organize_grid_layout`、`solve_grid_layout` 仍在 C++ `private_api`。新增 `solve_flexbox_layout_with_measure` 已提供 known-width／known-height 测量桥接，但也在 private namespace。必须区分“已有内部测量机制”与“已有稳定独立公开 solver API”，不能把后者未成立写成前者不存在。[A12]
2. **[事实] 框架许可边界仍在。** 当前 `LICENSE.md` 区分三许可证框架与 MIT 文档／示例；`layout.rs` 文件头仍为框架三许可证。Royalty-free 2.0 仍有署名条件、不得脱离应用单独分发、不得暴露部分或全部 API、嵌入式用途排除。不能因文档／示例许可宽松，把 solver 整体视为 MIT。[A14][A16][A17]
3. **[待验证] 真实运行验收仍缺。** 上游 C++ ListView 可见实例化及可变行高修复，不能代替目标平台的节点数量、测量开销、跳转／插入／本地化重排及生命周期测试；但也不能再把旧版本的 C++ 路径疑虑当当前已知缺陷。

#### 修正后的筛选决定

**[推断]** Slint 作为“独立通用 solver／引擎 SDK 再分发”的默认路径，仍有当前主源支持的 API 与许可边界；**作为“允许完整 toolkit 子树的编辑器 UI 对照”，已满足重新进入候选讨论的条件**。撤回基于旧动态 Grid 限制决定当前不 probe 的理由。

若允许整体 toolkit 路线，应让 Slint 与 RmlUi 竞争同一个系统级 probe 名额，而不是机械增加第三项；整体仍最多两个。这里没有断言 Slint 胜出，也没有把动态 Grid 支持误写成跨任意组件共享自然轨道已经通过。

**重开后的最小 probe [待验证]：** 使用 1.17.1 C++ 路径，Grid 重复行含运行时 row／col／span 与条件错误提示，执行增删、折叠、resize；另测 auto／runtime 编号混用诊断。将同一业务模型接入可变行高 ListView，执行快速滚动、跳转末尾、离屏插入和语言切换，分别记录逻辑项数、实例节点数、可见数、测量次数、几何稳定性及端到端成本。该方案本次未执行。

### A.4 决策差异摘要

| 对象 | 当前仍成立 | 必须修正 |
|---|---|---|
| RmlUi 6.2 | 无原生 CSS Grid；仍为框架对象路径 | 明确存在 OnLayout／OnResize 等钩子，不宣称无法自定义布局 |
| LVGL 9.5.0 | 一层 subgrid／CONTENT 共享限制 | 纳入新 minimum-size＋arrange 回调及内部错误传播改进 |
| Slint 1.17.1 | private solver 与框架再分发许可边界 | 撤回动态 Grid 旧排除；承认 C++ 可见 ListView 已有版本证据；完整 toolkit 路线重开 |

**本附录不把新 tag 查询当成完整重审。** 除明确核对的机制外，新版本线程、深度、分配、缓存、失败恢复及性能仍须单独验证。

### 附录直接主源

- [A1] RmlUi 6.2 发布：https://github.com/mikke89/RmlUi/releases/tag/6.2
- [A2] LVGL 9.5.0 发布：https://github.com/lvgl/lvgl/releases/tag/v9.5.0
- [A3] Slint 1.17.1 发布：https://github.com/slint-ui/slint/releases/tag/v1.17.1
- [A4] RmlUi 格式化分派：https://raw.githubusercontent.com/mikke89/RmlUi/6.2/Source/Core/Layout/FormattingContext.cpp
- [A5] RmlUi Display 类型：https://raw.githubusercontent.com/mikke89/RmlUi/6.2/Include/RmlUi/Core/StyleTypes.h
- [A6] RmlUi 元素与扩展钩子：https://raw.githubusercontent.com/mikke89/RmlUi/6.2/Include/RmlUi/Core/Element.h
- [A7] LVGL Grid／subgrid 文档：https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/docs/src/common-widget-features/layouts/grid.rst
- [A8] LVGL Grid 实现：https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/src/layouts/grid/lv_grid.c
- [A9] LVGL 新布局回调声明：https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/src/layouts/lv_layout.h
- [A10] LVGL 新布局回调实现：https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/src/layouts/lv_layout.c
- [A11] Slint 固定版变更记录；1.16.0／C++ 段含可见 ListView 证据：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/CHANGELOG.md
- [A12] Slint C++ private layout API：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/api/cpp/include/slint.h
- [A13] Slint 动态布局编译与诊断：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/internal/compiler/passes/lower_layout.rs
- [A14] Slint 运行时布局与文件许可：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/internal/core/layout.rs
- [A15] 与源码冲突的版本化网页：https://docs.slint.dev/1.17.1/docs/slint/guide/language/coding/positioning-and-layouts/
- [A16] Slint 当前许可分类：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/LICENSE.md
- [A17] Slint 当前 Royalty-free 2.0 全文：https://raw.githubusercontent.com/slint-ui/slint/v1.17.1/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md
