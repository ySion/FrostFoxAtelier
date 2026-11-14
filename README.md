# FrostFoxAtelier

面向 Windows x64 游戏的精简引擎工具包。C++ 与自有业务语言共享 Runtime 的类型、对象身份和生命周期；业务语言经 JIT 编译成本机代码后执行。

硬件基线：CPU 为支持 FMA3 的 x64（源码 JIT 创建后端时检查，缺少时编译失败并写明原因）；GPU 需同时支持 Mesh Shader 与 DXR 1.1 inline RayQuery（DX12 Ultimate 一档，见 [RHI 图形验收](#rhi-图形验收)）。两者都不保留旧硬件回退路径。

项目协作入口为 [AGENTS.md](AGENTS.md)，工程约定见 [工程指南](docs/engineering.md)，共享术语见 [CONTEXT.md](CONTEXT.md)。以下概览描述当前源码树的能力；具体语法、合同和验收证据由对应模块文档维护。

## 当前能力与文档入口

[Editor Control](docs/modules/editor/control.md) 是编辑器指令化操作与调试的标准入口，UI、快捷键、CLI、自动化、验收和 LLM 都是调用方。当前外部接入使用本机管道与 JSON，场景、属性、相机、显示设置、复现导出和渲染对照沿现有业务与撤销链执行；调试面板与 `debug.*` 仍待接入。

| 模块 | 当前能力 | 详细说明 |
| --- | --- | --- |
| Runtime | 原生与描述型类型、对象单继承、持有/观察引用、引用序列、值生命周期、宿主推进的非移动分步 GC | [运行时协议](docs/runtime-model.md) |
| 源码与 JIT | bool、32 位整数运算与数值转换、const、for/break/continue、跨文件与同模块函数、enum/struct 和原生派生 class、值字段组合、值与引用调用、记录/String/Array 拥有型局部、显式 async/yield、兼容代码更新与旧版本保活 | [语言规范](LANGUAGE.md)、[执行结构](docs/runtime-structure.md)、[源码类型发布](docs/source-type-declarations.md) |
| 可编程材质 | 自有语言纯值计算、具名纹理与 HLSL/CPU；MS→MI→MI、稀疏作者历史；纹理批次贯通光栅/阴影/DDGI、正式编辑与接受帧拾取 | [材质合同与边界](docs/modules/renderer/materials.md)、[Renderer 验收](docs/modules/renderer/acceptance.md)、[后续资产路线](docs/modules/resources/asset-system-plan.md) |
| 数学基础值 | float/double 向量、四元数和矩阵；原生/反射/源码共享布局；直接数学 IR 与 O2 优化 | [数学合同](docs/math.md) |
| Runtime / JIT 诊断 | 按需调用计数与计时、Runtime 资源快照与拥有值统计、源码故障与挂起位置、版本身份、IR 与 COFF 反汇编；开发版并行任务体误用检查 | [诊断接口与边界](docs/source-diagnostics.md)、[任务体禁入范围](docs/runtime-model.md#并行任务体禁入诊断) |
| 进程日志与 Project Output | Quill 后台日志、结构化 JSONL、增量 Reader、独立健康/丢失状态、完整只读正文与原始源码定位；Editor / Runner 分别拥有 run | [日志合同与验收入口](docs/modules/logging/README.md) |
| 源码调试核心 | 精确版本断点、进入/跳过/跳出、只读调用栈与有界局部值、受保护暂停和受控终止 | [调试接口与寿命](docs/source-debugger.md) |
| Jolt 物理 | PrimitiveComponent 统一网格与碰撞配置、子 Entity 多刚体、查询/触发、Cook 资产与 9 个官方 Physic 示例 | [物理模块](docs/modules/physics/README.md)、[接入与更新](#jolt-物理依赖) |
| 程序化几何与节点图 | 分层属性的 Geometry 值（列写时复制）、撒点/变换/Promote/Copy to Points 等 C++ 操作、实例按块发布；由 Runtime 对象构成的节点图与按内容键缓存的增量求值；尚未接入场景与编辑器 | [程序化模块](docs/modules/procedural/README.md) |
| C++ 基础库与反射 | Array/String、共享不可变的 SharedArray、Span/StringView、原生 Map/Set 与 Hash/Equal；反射薄宏及生成代码接入同一 Runtime，Map/Set 暂未开放源码/反射；常开 CPU 帧阶段计时机制 | [基础库](docs/foundation.md)、[自动反射](docs/reflection.md) |
| UI 布局、控件与样式 | 原生 Flex/Grid、列表可见范围索引与虚拟表格、Unicode 编辑、参数行与延迟悬浮说明、菜单/下拉/数值阶梯、SS 样式及接受帧文字快照 | [布局模块](docs/modules/ui-layout/README.md)、[UI 模块](docs/modules/ui/README.md)、[SS](docs/modules/ui/style-sheets.md) |
| 场景编辑器与绘制 | Entity/Component/World 编辑、位置参数与撤销、独立菜单窗口、单宿主停靠及控件实验台；圆角、边框、RGBA 图片及相邻实例合批 | [编辑器模块](docs/modules/editor/README.md)、[RHI](docs/modules/rhi/README.md) |

源码类之间的继承、对象引用参数与返回、布局迁移，以及参数面板固定目标、可浮动的编辑器视图与布局文件保存等能力仍待后续增量。Runtime 中已有的对象或值能力不自动代表源码已开放对应语法，实际范围以 [LANGUAGE.md](LANGUAGE.md) 为准。

## 启动首个场景编辑器

在仓库根目录的 PowerShell 中执行唯一产品入口：

```powershell
git submodule update --init --recursive
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target atelier_editor
./out/dev/atelier_editor.exe
```

编辑器直接消费正式 Entity/Component/World，无参数启动为空文档。独立 [Examples 工程](examples/README.md) 包含 Sponza / Scene Lit Room / Physic / Dungeons，通过 `--project ./examples` 或 File → Open Project 打开，不随引擎部署。顶部 Create 快捷区创建空 Entity 或真正的 StaticMeshEntity；Asset Browser 浏览工程文件与内置网格，选择资源后 Add to Scene 一次创建完整 Entity，撤销/重做保留 Entity 子树与组件身份。Scene Tree 展示 World → Entity 组合层级与实际类型，组件组成由 Entity Structure 维护，属性由 Parameters 展示。菜单、快捷区和快捷键共用指令定义、可用性与应用执行入口。位置字段编辑 Entity 的内联空间数据，拖调实时预览，松手只记一次撤销；Esc 取消，文本输入焦点下 Delete 不删除 Entity。
视口支持真实网格拾取、多选、框选、移动/旋转/缩放手柄及 G/R/S 数值操作；Shift+D 复制后移动，X 在指针下确认删除，Z 打开支持点击与按住划动松键选择的渲染模式饼菜单；Shift+反引号进入 FPS 自由视角，WASD/QE 移动、鼠标看向、滚轮调速。左键放置 3D 游标、右键选择、W 打开操作菜单，中键环绕、Shift+中键平移、滚轮缩放。工具选项栏提供枢轴、网格吸附与步长；视口内显示当前模式及透视/正交视图菜单。Parameters 的位置、欧拉角和缩放使用三分量数值行，其他数学向量按实际类型组合；选择资源时显示资源元数据。操作、事务和边界见[编辑器模块](docs/modules/editor/README.md#视口编辑)。窗口 resize、最小化恢复与 DPI 过渡使用同一正式帧/输入路径。
World 提供 Entity 递归组合、有序组件、可选内联 TRS 和独立空间参考，以及 Entity/Component 共用的有序 Begin/End、分阶段 Tick 和任务取消；完整 Detached 子树经统一入口注册，网格、灯和物理消费所属 Entity 的空间。renderer 消费矩阵、共享网格与按槽解析的不可变材质快照。验证入口见[场景模型验收](docs/modules/editor/README.md#场景模型验收)，逐次结果由下方验收门禁记录；当前 World 接入遵循[World 当前合同](docs/modules/world/README.md)。编辑器内源码断点/单步交互、节点图、参数面板固定目标、可浮动的编辑器视图与布局文件保存仍未实施。

Scene View 在路径栏末尾提供两个紧凑图标菜单：显示菜单包含 **MatCap / Smooth / Flat / Scene Lit**、着色与边线组合、**Polygon / Triangle Edges** 三组互斥选项；**Guides** 菜单用独立勾选控制渐变背景、参考网格和原点轴线。普通新建场景使用中性陶土 MatCap、蓝灰渐变和 XZ 参考网格，打开工程不按示例名称改变显示模式；选中对象以边线高亮。辅助显示在空场景中也可见，不写入物体深度；所有显示切换不修改场景材质或撤销历史。内置网格保留原始边信息，OBJ 简单多边形经三角化后保留原边界。实现与限制见[视口显示合同](docs/modules/renderer/contract.md#视口显示策略)。渲染特性代表图由 `renderer_acceptance_*` 窗口目标检查，运行时会显示真实窗口，应预留桌面时段。各验收入口证明的内容见[Renderer 验收](docs/modules/renderer/acceptance.md)。

工作室 MatCap / Smooth / Flat 默认开启 Screen Cavity，通过几何凹凸增强表面分界；开关、Ridge、Valley 与像素半径属于视口显示设置，具体范围见[渲染合同](docs/modules/renderer/contract.md#工作室-cavity)。

Scene Lit 消费同一 World 的真实光源与标准金属度/粗糙度材质，支持基础色、粗糙度、金属度、切线空间法线、HDR 自发光因子和 Opaque/Mask；六个贴图输入贯通光栅、射线命中与编辑深度，见[材质系统](docs/modules/renderer/materials.md)。直接光按屏幕 tile 与深度分层收集局部解析灯，在一个通道里逐盏确定求值并与间接光、自发光合成 HDR，方向光独立评估；投影光源的可见性来自阴影 atlas，没有随机采样、时间历史或降噪。发光网格只经 DDGI 照亮场景：探针射线命中发光面时记录其自发光，DDGI 关闭时发光网格不照亮其他表面。DDGI、GTAO 和随机屏幕反射分别稳定自己的信号，最终抗锯齿可选 Off / TAA / SMAA / SMAA T2x，默认 SMAA。**Window → World Settings** 编辑探针命中点的光源候选预算、四个 DDGI 体积、传播预算、曝光和 Bloom；作者值随 Scene 保存并参与撤销。Diagnostics 提供 DDGI 命中点空间选灯的对照开关、真实探针球、Volume 边界、重定位、更新年龄与接收面覆盖诊断，偏好留在视口，与 Control 共用入口。Control 的 `editor.inspect` 支持 `detail: summary`，观察相机、设置和最近接受帧时无需枚举整个场景。Point、Rectangle、Directional Light 沿 Create、Entity Structure、Parameters 和场景保存的现有入口编辑；均匀天光是 World Settings 的 Sky 组，关闭时环境照明为零，Environment 还可以带一张天空贴图；Scene Lit 有天空贴图时背景显示天空，没有贴图、空场景或纯线框时为黑，工作室模式保留渐变偏好。[Scene Lit Room](examples/Content/Scene%20Lit%20Room/README.md) 是 Examples 工程中的普通场景，选择 Main Camera 后可用 View from Camera 对齐，并从显示菜单选择 Scene Lit。具体范围、默认值与边界见 [Scene Lit](docs/modules/renderer/scene-lit.md)；Renderer 端到端入口为 `renderer_acceptance_scene_lit`；各入口证明的内容与当前成本见[验收记录](docs/modules/renderer/acceptance.md)。
共享文本编辑已接入 Unicode、参数搜索、剪贴板与 IMM32。字段切换或目标失效时取消旧编辑上下文；本地文本历史和场景撤销按当前编辑状态路由。真实输入法的检查方式见[编辑器验收](docs/modules/editor/README.md#输入法验收)，当前输入合同见[UI 模块](docs/modules/ui/README.md#共享单行编辑)，当前作者层实现与范围见[声明式 UI](docs/modules/editor/declarative-ui.md)。普通 UI/窗口验收不替代真实系统 IME 验收。

样式/绘制基础已扩展为 [SS（Style Sheets）](docs/modules/ui/style-sheets.md)：类名、角色、交互状态、token、局部作用域及属性覆盖共用最终样式；接受帧保有文字命中快照。GPU 支持独立圆角、向内边框、RGBA 图片与相邻实例合批。编辑器启动读取可执行文件旁的 `editor.ss`，源文件为 [assets/editor.ss](assets/editor.ss)。Parameters 的查询、Position、组件列表和 Scene Tree 现由 [editor.ui.source](assets/editor.ui.source) 声明，支持有类型的组件组合、文字输入、稳定 key 列表、条件挂载，以及保存后的源码 patch。Dev / Release 开发工作树构建读取这份文件，复用未改变的组件代码，保留兼容的 Instance / Runtime Context，只增删对应子实例；失败更新保留原界面。语义、性能与限制见[原生声明式 UI](docs/modules/editor/declarative-ui.md)。

现有编辑器界面已统一使用声明式 UI，覆盖范围与尚缺能力见[区域、控件与手势评估](docs/modules/editor/declarative-ui.md#区域与手势边界)。完整工具架、现有 Parameters 和视口图标工具区已由 DSL 声明。参数查询在对象标题下按需展开，Parameters 连续展示实例属性，Entity Structure 独立维护组件组成。Button、ToggleButton、ChoiceButton、Combo 复用既有命令和输入链；命名子组件保持原字段身份，保存后同步更新布局。资产浏览器采用常驻目录树、同行路径/搜索和紧凑菜单；Details 使用共享 Table，支持列头排序、列宽拖动、单元格选择、键盘导航及双击/Enter 打开。资源浏览器的工具行、目录树、状态行和 List/Details/Tiles 全部结果视图已迁入 DSL；三种呈现共享数据、选择和虚拟绘制，菜单内容、Controls、停靠界面与总外壳也已迁入同一 DSL，旧 C++ 界面组装路径已移除；跨区域资源拖放尚未实现。

输入、交互取消与对象生命周期遵循[编辑器合同](docs/modules/editor/README.md#正式执行链)。剪贴板失败保留草稿、选区和历史；原生文件选择、About 与保存确认的模态捕获和取消恢复目前没有自动验收。

历史 Unicode 剪贴板及窗口间歇失败尚未定因，后续通过不等于这些问题已关闭；验收通过也不替代针对具体场景的光照质量与性能评估。光照合同和验收范围以 [Scene Lit](docs/modules/renderer/scene-lit.md)与[Renderer 验收](docs/modules/renderer/acceptance.md)为准。

顶部菜单栏通过 File / Edit / Create / Project / View / Window 组织现有命令，左侧窄收拢区可将它收成细条；再次点击或使用 F10/Alt 菜单快捷键恢复。Tab 栏直接从标签开始，“＋”将已有视图加入当前组。下拉、参数右键和 Pane 菜单共用独立原生弹出窗口，可超出编辑器边界，支持键盘导航、菜单切换与焦点恢复，见[独立菜单窗口](docs/modules/editor/README.md#独立菜单窗口)。

**Controls** 标签打开同宿主的控件实验台，可操作数值、Vector3/Vector4、Unicode 文本、下拉、勾选和折叠，检查禁用、长标签与窄面板。实际位置参数复用同一参数行和 Houdini 风格 SS：中英文统一使用 Sarasa UI SC，数字使用 Source Code Pro，配合黑色字段、细凹凸边缘、轻渐变按钮与灰色编辑边界；Tab 以灰色选中底面区分，不显示焦点描边。右键菜单提供复制、粘贴与默认值恢复，中键支持数值阶梯，整组恢复只记一次撤销。字体依据见[字体说明](assets/fonts/README.md)，实验台的样本历史独立于场景，详见[控件实验台](docs/modules/editor/README.md#控件实验台)。

工作区采用 Scene View、Parameters、Controls、Scene Tree、Entity Structure、Asset Browser、Code、World 和 Project Output 九个可移动标签；支持组内重排、跨组移动、边缘分栏、分隔条拖动、关闭重开及最大化。Ctrl+Tab 切换标签，Ctrl+B 最大化/还原，Ctrl+W 关闭，**Reset layout** 恢复默认布局。状态与历史仍由原视图持有；参照 Houdini UI Scale 1.75 校准的尺寸与边界见[工作区文档](docs/modules/editor/README.md#工作区与停靠)。

Scene Tree 展示 World → Entity 递归组合关系和实际 Entity 类型，具有类型图标、层级线、交替行、键盘导航和底部过滤；组件组成由 Entity Structure 展示，属性由 Parameters 检查，详见[编辑器模块](docs/modules/editor/README.md)。

参数面板使用类型图标＋对象标题及连续属性滚动区，名称右侧的放大镜按需展开搜索。Entity 名称参与场景撤销；Controls 的 Appearance 页展示共享颜色值的 RGB 元组、逐通道滑条及嵌套页签。滚动条支持拖动、按页和步进，Tab 悬停保持底色，鼠标指针随编辑/拖动用途变化。组合与边界见[参数面板](docs/modules/editor/README.md#参数面板组合)。

工程使用文件夹作为入口：File 菜单创建/打开包含 Config、Content、Source、Cache 的工程，Ctrl+S 保存当前工作区文档；也可运行 `atelier_editor --project <目录> [--scene <Content 下相对路径>]`。格式、资源身份、OBJ 导入和支持范围见[工程工作流](docs/modules/editor/projects.md)。独立 Examples 工程中的 [Sponza](examples/Content/Sponza/README.md) 使用三盏真实灯光与覆盖完整模型的 DDGI 探针。Scene Lit、Smooth/Flat 共用网格材质槽与贴图覆盖；MatCap 保留自己的表面配色，同时遵循 Mask。

编辑器的窗口验收只保留日志零错误冒烟 `editor_log_smoke_acceptance`，经验收脚本运行以取得窗口锁：

```powershell
pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Targets editor_log_smoke_acceptance
```

成功输出 `Editor log smoke: PASS`；无窗口文档链使用 `editor_document_acceptance`。覆盖范围见[编辑器验收](docs/modules/editor/README.md#验收入口)。
窗口验收启用真实硬件 D3D12 debug layer。可执行文件旁须保留构建部署的 mimalloc、DXC、Agility、PIX DLL 与许可证，以及 editor.ss、fonts/ 中的字体和许可证。Examples 是独立工程，不属于可执行文件的部署依赖。

编辑器的可捕获异常会在释放 Session 前，将未保存的已提交作者 Scene 原子写入独立恢复目录，并在错误日志中报告路径；原场景保持不变。范围、资源依赖与恢复步骤见[异常退出时的场景保全](docs/modules/editor/projects.md#异常退出时的场景保全)。

## 编辑器内编写源码

**File → Edit Editor UI Source** 打开正在使用的 UI DSL，编辑后 Ctrl+S 保存并进入原有同步热更新。**File → Open Source…** 或 Asset Browser 打开项目 Source 文件。Code 面板支持行号、选区、缩进、剪贴板、独立撤销、错误定位与外部修改冲突处理；工程 Source 已接入跨文件构建、真实组件和属性、场景保存、独立运行及兼容热更；项目 UI 可独立预览。F6 构建、F8 运行、Shift+F8 停止；类型变化使用 Project → Rebuild Types and Scene。Code 面板的补全和断点调试交互尚未接入，Runtime/JIT 调试核心已可通过正式接口使用。详见[源码工作流](docs/modules/editor/source-editor.md)。

## 构建与端到端验收

在项目根目录的 PowerShell 中执行：

```powershell
# 首次克隆或切换到更新了子模块的提交后执行。
git submodule update --init --recursive
# 每个新会话执行一次。缺少 LLVM 时，增加 -InstallToolchain 参数。
. ./tools/enter-dev.ps1

cmake --preset dev
cmake --preset release
# 快速门禁：两种配置都构建，只运行列出的目标；不带配置前缀的目标在 release 运行。
pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Targets project_scene_acceptance,renderer_acceptance_scene_lit
# 阶段收尾完整门禁：双配置 all 构建，release 全部无窗口与窗口叶目标各一次。
pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Full
```

**快速门禁**双配置 `all` 构建，只运行受影响模块的无窗口目标；改到界面或渲染时再列对应窗口目标，只在 release 运行。多片集成后在最终树上跑一次门禁即可推送。**完整门禁** `-Full` 用于阶段收尾：双配置 `all` 构建后，release 的全部已注册无窗口与窗口叶目标各运行一次。验收保留与删除的标准只见[工程指南](docs/engineering.md#验收保留标准)。

`-Targets` 中的每一项是 `目标`、`release:目标` 或 `dev:目标`，逗号分隔；名称必须来自 CMake 生成的目标清单，否则脚本失败。汇总目标 `renderer_acceptance` 会被拒绝：它让全部入口共用一个验证级别，应改列需要的 `renderer_acceptance_<入口>`。`-Configs` 默认 `dev,release`，脚本总是先构建这些配置的 `all`。`-Full` 要求两种配置都在场，不能与 `-Targets` 或 `-ProbeWindowTarget` 同用；它读取构建后 CMake 生成的两类清单，去重后执行叶目标，不另列全套名单，也不重复执行聚合目标。

**GPU 验证范围**：脚本为每个窗口目标显式设置 `ATELIER_GPU_VALIDATION`，RHI 在创建设备时读取（见 [RHI 合同](docs/modules/rhi/contract.md)）。快速门禁全部设置为 `debug`。只有 `-Full` 中的 `rhi_acceptance` 与 `renderer_acceptance_default`、`renderer_acceptance_scene_lit`、`renderer_acceptance_material_programs` 四项设置为 `gbv`（D3D12 debug layer + GPU-based validation），其余窗口仍为 `debug`，包括 `editor_log_smoke_acceptance` 及其启动的编辑器子进程。该变量只把请求的 GBV 降为 debug layer，入口自己请求的更低级别不变：`scene-lit-benchmark` 不开验证。降级时每个设备向 stderr 写一行 `RHI: ATELIER_GPU_VALIDATION=debug …`。不经脚本直接运行目标时变量未设置，各入口仍按原有验证级别运行。

debug layer 仍在 CPU 侧检查 API 参数、对象创建和命令列表上的 barrier；以下错误只有 GBV 能发现：着色器执行时访问越界、未初始化或已销毁的描述符（bindless 和动态索引尤其如此），描述符类型与着色器声明不符，以及着色器实际访问的资源与其当前状态不符。快速门禁不自动发现这些 GBV 专属错误；`-Full` 仅在上述四项覆盖的执行路径中启用 GBV，其余窗口仍不覆盖这些错误。改动涉及未覆盖路径的 GPU 资源绑定时，需安排该路径的 GBV 复查；不经脚本运行时仍须遵守窗口锁，并确认入口实际请求 GBV。

CMake 的 `acceptance_headless` 和 `acceptance_window` 是两类目标的清单，总入口 `acceptance` 依赖两者，只供手动排查，不是交付或集成要求。直接运行 CMake 目标不取得跨工作树锁，也没有脚本的逐目标超时保护和验证级别设置；必要测试应使用脚本。

- `acceptance_headless`：基础库、数学、Runtime、Source/JIT、World、程序化、物理、布局、生产 UI、材质程序资源、日志、编辑器文档、工程场景与工程集成。都不创建窗口或 GPU 设备、不需要输入焦点；工程集成的子进程使用 `--headless`。在 GPU/窗口锁外执行，目标之间由 Ninja 并行调度，例如 `cmake --build --preset dev --target acceptance_headless --parallel 4`。
- `acceptance_window`：RHI、Renderer 与编辑器日志冒烟。先 CPU 后 GPU/窗口的混合目标整体归此层；Ninja console pool 保持串行，脚本也逐目标调用构建命令。Renderer 每个入口是一个目标 `renderer_acceptance_<入口>`（`-` 换成 `_`，如 `renderer_acceptance_scene_lit`），各自一个进程和窗口，限时 1800 秒；`renderer_acceptance` 汇总全部入口，只供手动运行。

无窗口目标通过 `atelier_headless_acceptance` 注册，自有临时文件使用 `out/<配置>/acceptance-headless/<目标>/temp/`，日志为同目录 `output.log`。原有工程证据和 Cache 保留各自专用目录；只读字体等仍从构建目录读取。

**新增自动验收目标必须归入其中一层**：无窗口目标使用 `atelier_headless_acceptance`；窗口目标加入根 CMake 的 `_window_acceptance`，保留 `USES_TERMINAL`。不确定或含任何 GPU/窗口/原生输入的目标归窗口层。窗口清单由 CMake 生成到 `out/<配置>/acceptance-window-targets.txt`，无窗口清单为 `acceptance-headless-targets.txt`。脚本只读取这些清单，不维护第二份目标列表。

脚本先在锁外构建并运行列出的无窗口目标，再按主检出（Git common dir 的上一级）的 `.amp/in/locks/gpu-window.lock` 文件协议取得窗口独占权；锁中记录调用者、检出、配置与用途，在 `finally` 释放。申请前检查已有持有者；释放后再次申请至少间隔 60 秒。别人持锁期间不得启动任何可见窗口（包括控制台），`Start-Process` 必须指定 `-WindowStyle Hidden`，不能用 `cmd /c start`；仅命令行输出的构建和无窗口验收可以继续。

窗口目标的超时按配置取**包含冷编译的近期最长耗时的 2 倍**、最少 5 分钟，读取 CSV 中该配置/目标最近三次成功窗口运行。超时和 probe 不作为完整耗时样本，避免故障反复扩大预算。未测量目标默认 30 分钟。超时按目标计，Renderer 的每个入口是单独目标，各自也用默认 30 分钟，与运行脚本的单入口限时一致；ec-5 实测各入口整进程都在 6 分钟以内。`history.csv` 中加入验证级别之前的行，`Validation` 列为空。超时先用 `PrintWindow` 逐个截取当前 Windows Job 内进程的可见顶层窗口，不读取桌面或覆盖它的其他应用；没有窗口时只记录“没有可截的窗口”，不退回整桌面截图。每个截图辅助进程最多等待 4 秒，失败或卡住也继续终止包含编辑器子进程的 Job 进程树，记录 `TIMEOUT` 后继续下一项。激活器只在当前日志等待“click the editor title bar to begin”、之后尚无“activation acquired”或 `FAIL` 且编辑器仍响应时注入激活。
持锁执行窗口目标时，脚本自身设为 `AboveNormal`，Windows Job 同时把该优先级应用于 cmake、Ninja 和所有验收子进程（Windows 不自动继承父进程的 AboveNormal）；锁外构建与无窗口目标保持 `Normal`，退出时恢复脚本原优先级。

每次运行的日志、超时截图 `*.log.timeout.<窗口句柄>.png`、`summary.txt` 和 `summary.csv` 位于 `out/acceptance-gate/<时间戳-PID>/`；汇总首行为 `Run=targeted`、`Run=full` 或 `Run=probe`，并区分请求范围和实际执行的配置/层（`build`、`headless`、`window` 或 `probe`）。每个目标记录配置、层、`PASS/FAIL/TIMEOUT`、秒数和验证级别（窗口目标为 `gbv` 或 `debug`，构建和无窗口目标为 `none`），并追加至 `out/acceptance-gate/history.csv`。并行无窗口目标的秒数取自各自 `result.csv`；窗口目标的起止时刻见 `gate.log` 的 `Starting` 和结果行。任一失败或超时令脚本返回非 0。仅验证超时清理时，可在已完成构建后运行 `pwsh -NoProfile -File ./tools/acceptance-gate.ps1 -Configs release -ProbeWindowTarget editor_log_smoke_acceptance -ProbeTimeoutSeconds 30`，在冷管线准备阶段检查整个父子进程树的终止；probe 不启动激活器，不注入输入，不能与 `-Targets` 或 `-Full` 同用。该模式仍持锁，但明确记为 probe，不是测试通过。

测量入口统一以 `--profile-*` 命名（如 `atelier_editor_document_acceptance --profile-scene-cpu`、`atelier_editor --profile-render-performance`），只输出数值报告，不属于验收，也不进入总入口；其中的数值不作为任何通过条件。成功时输出各模块 `PASS`，包括 `UI acceptance: PASS`、`Editor document acceptance: PASS` 和 `Editor log smoke: PASS`，返回码为 0。失败返回非 0；发布构建也执行全部验收检查。真实输入法没有自动验收，见[编辑器验收](docs/modules/editor/README.md#输入法验收)。只构建时使用 `cmake --build --preset dev`；只运行源码公共消费者及 JIT 平台/业务贯通验收使用 `cmake --build --preset dev --target jit_acceptance`（发布配置相应替换为 `release`）。

首次配置新机器：先按工具链锁文件安装所需 MSVC 工具集、Windows SDK、CMake/Ninja 的 Visual Studio C++ 开发工具，再执行：

```powershell
. ./tools/enter-dev.ps1 -InstallToolchain
```

脚本只在项目 `.tools/` 内下载、校验和解压完整 LLVM SDK（编译工具、开发头文件、CMake 配置和库），并准备当前进程环境，不改系统 PATH 或 PowerShell 配置。已有仅含编译工具的旧目录需要重新执行安装命令补齐 SDK。脚本不负责安装或升级 Visual Studio。`.tools/`、`out/` 和本机用户 presets 均不提交。

唯一版本清单位于 [cmake/toolchain-lock.json](cmake/toolchain-lock.json)，版本升级同时更新锁文件并验证构建环境，不在文档中维护第二份版本表。

通过归档下载的第三方依赖由 [cmake/dependencies-lock.json](cmake/dependencies-lock.json) 锁定版本、下载地址和 SHA-256。首次 CMake 配置取得 mimalloc、Foundation 的 unordered_dense、LLVM SDK 所需的 zlib/zstd，以及 gfx 和它使用的 DXC、Agility、D3D12MA、PIX 与编辑器 JSON 库。压缩包缓存于 `.tools/downloads/`，源码和构建结果位于各配置的 `out/` 下。Jolt 源码通过 Git 子模块保存在 `third_party/jolt/`，版本由主仓库记录的子模块提交固定。不要求编译 LLVM 或 compiler-rt 全量源码。

原生代码使用 C++23，警告视为错误；编译使用 clang-cl，静态归档使用 llvm-lib，链接使用 lld-link。只维护 CMake 构建定义。IDE 应使用相同 preset 和准备好的工具链环境。

## Jolt 物理依赖

[Jolt Physics](https://github.com/jrouwe/JoltPhysics) 官方源码以 Git 子模块接入 [third_party/jolt](third_party/jolt)，来源记录在 [.gitmodules](.gitmodules)，主仓库的 gitlink 是其唯一版本锁。[cmake/jolt.cmake](cmake/jolt.cmake) 直接通过 `add_subdirectory` 编译该源码，提供 `Jolt::Jolt` 静态库目标；构建产物位于 `out/<配置>/third_party/jolt/`，输出目录保留 MIT 许可证 `LICENSE.jolt.txt`。

新克隆可使用 `git clone --recurse-submodules`。已有工作区执行以下命令，取得主仓库固定的源码提交；CMake 在子模块缺失时会提示初始化命令：

```powershell
git submodule update --init --recursive third_party/jolt
git submodule status third_party/jolt
```

当前采用单精度位置、Windows x64 的 SSE2 基线和与宿主一致的动态 CRT；关闭上游 LTO、配置编译参数覆盖、调试绘制、profiler、ObjectStream 与 Compute 后端。Jolt 的内存分配使用默认 CRT 路径，由项目现有 mimalloc 重定向接管。`atelier_physics` 已接入 World 刚体、触发/接触、场景查询、Cook 网格和角色/约束/布娃娃基础场景 API 及对应组件；[Physic](examples/Content/Physic/README.md) 提供 9 个可运行场景。创建、步进、资源和未实现边界见[物理模块](docs/modules/physics/README.md)。专用服务器可执行宿主尚未接入。

按上方准备工具链并配置后，无窗口物理验收入口如下：

```powershell
cmake --build --preset dev --target physics_acceptance
cmake --build --preset release --target physics_acceptance
```

`physics_acceptance` 也包含在总 `acceptance` 中，通过真实 Jolt 及 World 公开接口检查刚体形状、当前 Cook 资产模拟及失败生命周期三个场景；`--list` 列出可独立运行的场景，完成时输出逐项结果与总数。检查不依赖 `assert`，发布配置同样有效。场景清单、单场景命令与证据边界见[物理验收](docs/modules/physics/acceptance.md)。

升级 Jolt 时，先确保子模块内没有未提交修改，获取上游版本并显式选择目标：

```powershell
git -C third_party/jolt fetch origin --tags
$joltRevision = 'vX.Y.Z' # 替换为要升级的正式 tag 或提交 SHA
git -C third_party/jolt switch --detach $joltRevision
```

检查上游构建选项/API 变化，重新配置并执行 dev/release 的 `physics_acceptance`，然后将子模块指针与必要的适配改动一同记录到项目提交。合并其他项目增量不会自动跟随 Jolt 上游最新提交；切换或拉取包含子模块版本更新的项目提交后，再执行 `git submodule update --init --recursive` 同步源码。

## RHI 图形验收

RHI 采用固定 gfx 与局部补丁，提供 Compute、Mesh、Vertex 管线和有界执行域。单独构建并运行：`cmake --build --preset dev --target rhi_acceptance`，发布配置替换为 `release`；验收在 D3D12 debug/GBV 下用 Compute、Mesh、Vertex 管线各绘制并读回一次，确认管线保活其执行域且 debug 层零警告零错误。命令要求同时支持 Mesh Shader、DXR 1.1 inline RayQuery 和 SV_Barycentrics 的真实硬件（DX12 Ultimate 一档），不自动降级为软件或跳过场景。当前实测与边界见 [RHI 模块](docs/modules/rhi/README.md) 和 [验收证据](docs/modules/rhi/acceptance.md)。

构建输出同时部署锁定的 DXC、Agility 与 PIX DLL，并保留 `licenses/rhi`。`atelier_rhi` 不链接业务 Runtime/JIT；顶层 CMake 配置仍需要完整工具链。

## UI 布局核心

正式静态目标 `atelier_ui_layout` 的唯一公开入口是 `ui_layout/layout.hpp`。`LayoutDocument` 统一管理节点身份、结构编辑、测量缓存和事务几何发布；核心不链接 Runtime/JIT/RHI/DirectWrite。正式验收只保留一个代表：有上限的行分配与独立分区真值一致，几何快照在文档析构后仍保有结果。

- 尺寸与排布：Fixed/Content/Expand、数值 min/max、对齐/锚定、table 共享列、wrap 自然换行与自适应重复槽、collapsed；原生 Flex 支持 basis/grow/shrink、分行/分布/reverse 和首基线，二维 Grid 支持轨道、跨度及显式或 sparse 自动放置。
- `VirtualList` 管理逻辑行高、可见范围与滚动锚点；调用方只需实例化可见范围中的行。滚动控件本身仍由上层实现。
- `LayoutView::find` 按完整身份提供常数时间查询。编辑和失败保留当前借用，下次成功发布终止借用；跨发布使用 `LayoutSnapshot` 保有几何。
- 几何快照不拥有内容资源。完整帧须固定相应内容版本，包括未参与测量的固定尺寸叶；生产段落与 Coverage 的寿命由[UI 验收](docs/modules/ui/README.md#限额与验收)负责。

环境准备和配置仍使用上方正式 presets；仅运行布局验收：

```powershell
cmake --build --preset dev --target ui_layout_acceptance
cmake --build --preset release --target ui_layout_acceptance
```

总体 `acceptance` 也包含这个消费者。几何变化仍求解参与布局的实例树；不承诺 CSS 全量、一般循环约束、通用局部调度、异步布局或硬帧预算。窗口输入、生产控件与 GPU 绘制由[UI 消费模块](docs/modules/ui/README.md)和编辑器接入；RGBA 图片、SS 样式及声明式参数/树组件已接入，图片解码和 Runtime 资源 composition 仍未实施。现行布局行为和测量边界见[合同](docs/modules/ui-layout/contract.md)、[验收](docs/modules/ui-layout/acceptance.md)和[审计与性能](docs/modules/ui-layout/audit.md)。

## 引擎资源连接方案

引擎已有不可变 CPU Mesh/材质图片、UI 图片快照、Renderer 上传与退休，以及 Runtime 的受保护原生载荷访问。三维材质已沿同一 Runtime 引入原生 Texture/Material 资源，见[材质系统](docs/modules/renderer/materials.md)。[共享 Runtime Texture 与 UI 连接方案](docs/modules/resources/asset-system-plan.md#texture-到-ui-的共同消费待接线)的剩余缺口是语言的跨类型对象目录/引用字段，以及固定内容版本到 UI 测量和绘制的共同接线；这条业务链尚未通过贯通验收，继续复用已有源码宿主、布局和渲染入口。

## 默认内存分配

原生进程使用 C 模式构建的共享 mimalloc DLL 与官方 Windows CRT 重定向，普通代码继续调用默认分配函数。`atelier_target` 为每个可执行目标及自有 DLL 直接编译 [global_new_delete.cpp](modules/core/memory/src/global_new_delete.cpp)，覆盖普通、数组、nothrow、对齐及 sized delete，链接同一 mimalloc DLL；LLVM DLL 在自身 `/MT` 域编译，不再由支持对象重复提供覆盖。仅 CRT 重定向对照 `atelier_allocator_client` 保留不直接链接 mimalloc 的特例。自有全局 new 的分配失败直接进入 fatal，包括 nothrow 形式，不调用 new_handler 或尝试异常恢复。静态库内的代码随最终链接模块使用该分配器。

基础层的 [memory/fatal.hpp](modules/core/memory/include/memory/fatal.hpp) 提供 `atelier::fatal(std::string_view) noexcept` 与 `atelier::install_fatal_handler() noexcept`。`atelier_memory_headers` 只传播头文件；`atelier_target` 在每个 exe/shared DLL 中按其自身 CRT 编译同一份 fatal 实现，不把宿主 CRT 对象链接进 LLVM 域。全部 exe 入口已安装处理器；Source 后端、JIT 平台验收 DLL 与两个 LLVM 工具在各自 CRT 域安装，LLVM 致命回调使用固定原因。错误政策与覆盖限制以[唯一 fatal 出口](docs/engineering.md#唯一-fatal-出口)为准；其他可空分配调用、线程启动及业务错误路径的去异常迁移仍在进行，本片不翻转全仓编译开关。

fatal 只向成功设为非阻塞的 stderr 管道尽力写出至多 1024 字节原因；控制台（包括 QuickEdit）、文件、设备、无效句柄或不能切换为非阻塞的管道均跳过文本，直接 fail-fast。mimalloc 的[最小补丁](third_party/mimalloc/atelier.patch)关闭其默认 stderr 诊断，避免分配返回 null **之前**被满管道或控制台阻塞；该策略在初始化早期、dev 默认诊断及环境变量要求 verbose/show-errors 时均生效，不改分配或调试检查。需要诊断时使用本方 fatal，不依赖分配器的默认输出。

本项目的可执行文件和 DLL 集中输出到各配置目录。运行验收程序时，需要保留同目录的 `mimalloc-debug.dll`（开发配置）或 `mimalloc.dll`（发布配置）、`mimalloc-redirect.dll`，以及用于验证跨 DLL CRT 分配的 `atelier_allocator_client.dll`。重定向器保持上游实现，构建同时保留 `LICENSE.mimalloc.txt`。

实际覆盖包括全局 C++ 分配、采用默认分配器的标准容器，以及经过动态 CRT 的 C 分配。独立堆、直接操作系统分配、GPU/JIT 专用内存和第三方自定义分配不因 CRT 重定向而自动改用 mimalloc。原有析构、所有权和指定释放接口的契约保持不变。

验收会检查 CRT 重定向状态，关闭重定向时返回失败；仅链接 mimalloc 或仅覆盖 `new/delete` 不算通过。开发配置保留上游调试检查，当前尚未测量分配器带来的性能收益。引擎基础容器通过相同分配入口取得存储，具体契约见基础库文档，演进约定见工程指南。

## Windows JIT 平台验收

[acceptance/jit](acceptance/jit) 同时包含真实本机平台实验和最小源码业务验收；两者范围分别说明。构建接入见 [cmake/jit.cmake](cmake/jit.cmake)。

- 宿主 EXE → JIT → 独立 DLL 的混合寄存器/栈参数、整型与浮点、64 位值及对齐记录互调；原生失败返回明确状态，不覆盖结果，清理恰好一次。
- Windows 动态函数表登记及 `RtlLookupFunctionEntry` / `RtlVirtualUnwind` 回溯，跨过四层递归生成帧并确认回到原生调用者，生成栈存储保持 32 字节对齐。当前机器上的 `CaptureStackBackTrace` 在这些动态帧处停止，故验收不依赖它。
- 在实际展开表登记之后注入 finalize 失败，检查撤表且对应地址回到 `MEM_FREE`。

采用显式 `ObjectLinkingLayer` / JITLink 与原生 COFF。LLVM 生成并保留 `.pdata` / `.xdata`；一个进程内插件通过 LLVM allocation actions 在链接完成后登记 Windows 展开表、在释放对应内存前撤销，失败回退和所有权转移沿用同一分配生命周期，不另建一套代码资源表。撤表失败会终止进程，不能继续释放仍被 Windows 引用的页。生成代码使用 Large code model，不假设宿主 DLL 位于 JIT 内存的 ±2 GiB 内。

锁定 LLVM 的 finalization 错误出口存在已复现的代码页泄漏，使用 [JITLinkMemoryManager.cpp](third_party/llvm/JITLinkMemoryManager.cpp) 的最小源级修复：失败时清理分配，已执行的动作先逆向撤销，正常分配策略不变。该完整翻译单元保留上游许可证、原始 SHA-256 与编译期版本检查，通过正式目标链接，不手改 SDK 二进制；升级 LLVM 时必须重新核查此修复。展开表安装/撤销的参数包分别为 24/8 字节，均留在 SDK 的内联缓冲内。

预编译 LLVM 的 Release 静态 CRT ABI 分别隔离在正式源码后端 `atelier_source_backend.dll` 和平台验收 `atelier_jit_probe.dll`；宿主与 `atelier_jit_native_client.dll` 保留原有开发/发布 CRT 配置。两个 LLVM DLL 共用构建中的展开支持与修复对象，LLVM/STL 对象、异常和分配所有权不跨隔离接口。运行完整 JIT 验收需保留上述 DLL 及本配置的 mimalloc/重定向 DLL；输出目录保留 LLVM、zlib、zstd 的许可证副本。

平台的代码拥有、展开表登记与退休取舍见[现行执行结构](docs/runtime-structure.md#当前取舍)；功能验收不代表真实业务负载的编译延迟或稳态吞吐结论。

**平台范围限制**：只验证受控、已定型 LLVM IR 的进程内同步执行，不支持任意 C++ 目标文件、C++ 静态构造/TLS 初始化或 C++ 异常跨 JIT 帧。平台探针只证明机器码执行、展开和退休路径。源码位置、断点/变量查看及兼容方法体更新由正式 Source 模块提供，分别见[源码调试](docs/source-debugger.md)与下节业务闭环；PDB 和原生调试器集成尚未接入。

## JIT 业务执行闭环

正式消费者包含 [source/program.hpp](modules/execution/source/include/source/program.hpp)，链接 `atelier_source`，使用同一个 `source::Program` 完成请求、编译、发布、绑定、同步调用及协程启动。标量/枚举值参数和返回值使用 Runtime 的 `Scalar`，借用参数使用 `Argument`，不会暴露 LLVM 或私有 DLL 参数布局。原生或描述型记录、Array 和只读 String 也通过 `Argument::borrow` 进入同一接口；布局来自 Runtime，生成代码访问调用方原有缓冲。[公共验收入口](acceptance/source/main.cpp)只运行项目快照与关停两个消费者。[关停消费者](acceptance/source/shutdown.cpp)覆盖关闭入口、取消任务、等待执行静止及最后释放 Runtime 的完整顺序；具体契约见[宿主关停](docs/runtime-model.md#宿主关停闭环)。

原生值借用的共享布局、只读/可写别名、调用链及边界见[原生值借用](docs/native-value-borrows.md)。

类型与调用效果声明沿同一 Runtime 发布，`AFUNCTION(BorrowSafe)` 开放受控原生算法。实际语法与范围见[拥有型基础值](docs/source-owned-values.md)。

宿主通过 `Program::request(name, source, entry_name)` 选择公开入口；整模块随代码版本一起发布和退休。当前内部调用图无环，不支持递归或调用 async 函数。详见[同模块多函数](docs/source-function-modules.md)。

[项目快照消费者](acceptance/source/projects.cpp)保留当前 Source 文件格式往返，以及跨文件助手函数驱动真实 World owner 的一次 tick 与停止/关闭顺序。类型与整组代码一次发布；兼容更新保留对象状态，旧租约保有旧模块。宿主 API、诊断定位和重建边界见[项目快照](docs/source-function-modules.md#项目快照与多个公开入口)。

源码 `enum/struct` 与 C++ 独立布局共用存储。新类型可在创建对象后随代码更新追加，已发布类型的定义与身份保持不变；原生方法可按引用直接消费这些描述型记录。执行链与边界见[源码类型声明](docs/source-type-declarations.md)。

原生与 Source 字段共用不可变的命名空间 `Meta("key", "value")` 载体；Source class 可声明真实 String 字段、空或字符串字面量默认值，沿宿主反射及既有场景格式保存、加载和重建。Meta 内容或文本默认值改变要求重建，兼容更新保留实例覆盖；本片不开放源码对对象 String 字段的访问或借用。语法与边界见[原生派生类](LANGUAGE.md#项目快照与原生派生类)，载体见[字段扩展元数据](docs/runtime-model.md#字段扩展元数据)。

同步 plain struct 的按值参数与返回使用同一 `Runtime::call`、`Program::Entry/Lease::invoke`，结果通过 `ValueOutput` 提交；记录可递归包含标量、枚举、数学值和 plain struct。原生成员调用由定型 C++ thunk 处理平台 ABI。含 String/Array、GC 引用或其他资源的记录及异步聚合边界仍未开放。

单独运行该消费者：`cmake --build --preset dev --target source_acceptance`（release 同理），成功输出 `Source public consumer: PASS`。

同步标量通过 `Lease::bind<R, Args...>()` 创建固定版本 Typed，或通过 `Entry::bind<R, Args...>()` 创建跟随发布 Typed；声明式 UI 的 `void(uint32_t)` 分派固定版本，项目 World 的 tick/post_physics_tick/late_tick/begin/end 跟随发布且不逐帧重新绑定。两者均不构造通用实参数组，执行区间与延迟退休保护回调中的代码寿命；借用和协程仍使用动态 Entry/Lease。支持范围与版本寿命见[源码调用合同](docs/runtime-model.md#单入口兼容方法体发布)，私有 ABI 与共用清理见[执行结构](docs/runtime-structure.md#同步调用)。混合寄存器/栈参数（K5）由 `jit_acceptance` 保留，关闭序列（K6）由 Source 关停消费者保留，不新增窗口目标或未经测量的性能承诺。

正式接口的现行成本边界见[执行结构](docs/runtime-structure.md#已知成本与能力边界)。

[业务验收](acceptance/jit/business.cpp)内嵌最小源码，经过[词法/语法与类型检查](modules/execution/language/src/source.cpp)、定型语法树及[LLVM 本机编译](modules/execution/jit/src/source_backend.cpp)，保留四个代表：定型标量的寄存器/栈参数位置，bool 借用别名不覆盖相邻字节，强/弱引用跨挂起和 GC 后保持身份且完成帧释放接收者，以及原生 Vector3d 聚合结果。

- [ExecutionFrame](modules/execution/runtime/include/runtime/execution.hpp)复用现有定型身份、`ObjectRoot` 及共享屏障/撤销机制。同步局部与临时引用由执行区间保护，不逐个登记持久根；协程持有槽跨挂起保活，观察槽不保活。生成帧只预留接收者槽，机器验收自行持有快照根；循环/早退/错误均显式清理。
- 顶层以 `ExecutionScope` 覆盖参数准备至结果交接、帧清理，内部准入检查拒绝原生借用或遍历内重入；同一 Runtime 的最外层执行期间，整个 GC 状态机禁止开始或推进。轮询只观察和控制，调用不再接受收集预算；最外层退出不自动收集，宿主继续在已有帧/阶段边界推进同一增量 GC。长执行会延迟回收并累积不可达对象；原生调用、借用和显式销毁的准入保持不变。
- 错误通过定型 thunk 的状态与固定诊断返回；IR 显式清根，宿主在 RAII 兜底之前验证清理完成。跨 Runtime、错类型、失效代际与槽位复用均按原身份规则拒绝，已发生的业务修改不回滚。
- LLVM 与 C++ 所有权继续隔离。活动调用在 DLL 内持有代码；已实际验证原生回调销毁外层代码句柄后继续执行，返回后释放代码页和展开表。两条生成路径共用 [Windows 展开机制](modules/execution/jit/src/windows_unwind.cpp)。
- 源码入口接受零个或多个 bool/float/double/uint/int/枚举参数，支持值、只读引用及可写引用，同步入口另支持已注册原生记录/Array 的引用和只读 String 引用，同步入口另支持内建数学值及 plain struct 的按值参数与结果，返回为 void、标量/枚举值、数学值或 plain struct；已定型数值不隐式转换。调用先校验数量、类型和结果存储；失败状态与成功标量分开，只有原生调用、结束回调和生成清理全部成功才提交结果，失败不覆盖调用方结果。
- 成员名称、字段路径和原生方法签名来自同一 Runtime 的元数据；前端/后端只使用宿主绑定表索引，不知道机器字段名字。机器快照的创建、轮询后刷新与结果根交接归宿主验收适配器；不访问对象的标量或原生值借用入口无需接收者、快照或登记对象根。
- 原生注册从同一 C++ 签名生成动态 Invoker 与定型 thunk。源码编译捕获不可变入口和字段定位，生成代码直接传定型参数；聚合按值实参使用独立对齐活值地址，由 C++ thunk 完成真实按值调用。引用实参在全部实参求值后定位，保留 const 与合法别名；失败只写固定诊断，不提交暂存结果。动态 Runtime 调用仍检查结果缺失、种类及存储合同。

开发/发布的正式目标均运行上述[真实端到端验收](acceptance/jit/business.cpp)。当前支持单源码选定入口、项目快照的多个同步入口与模块内函数、标量、同步值借用、String/Array/记录拥有型局部、源码 enum/struct、受控原生算法调用、既有成员、`if/while/for/break/continue/return`，以及单绑定对象类型的持有/观察局部、弱引用直接成员访问、`null/new/observe/try_acquire`、身份比较与词法引用清理。完整现行词法、数值、引用与协程规则见 [LANGUAGE.md](LANGUAGE.md)；长期设计方向见[语言契约](docs/language-contract.md)，执行协议见[运行时说明](docs/runtime-model.md#业务执行协议)。验收不重复原生/描述型和分支矩阵，期望值来自固定算术真值。

语言基础沿同一 Runtime/source/JIT 入口支持 `core.bool`、短路逻辑、受检查的整数除余/移位、四种数值类型的显式转换、局部只读 const 与编译期模块常量，以及带作用域清理的 for/break/continue。`GetX`／`SetX` 自动派生 [Property 调用糖](LANGUAGE.md#计算属性-property)，强引用可隐式赋给弱目标，反向仍需显式 `try_acquire`；不增加新的 Runtime 存储或引用体系。固定 ABI 不提供可替换的结果地址或动态结果标签。运行入口为 `cmake --build --preset dev --target source_acceptance jit_acceptance`，release 使用同名目标；语义以 [LANGUAGE.md](LANGUAGE.md) 为准。

业务验收的源码夹具内嵌于 C++，不再部署额外机器源码文件。编译诊断带源名/行/列；热更新请求另附单调请求号。超长源名仅在诊断与 COFF 文件标签中按首尾缩略，避免吞掉请求号、位置或原因，并保持 UTF-8 边界；请求快照与编译元数据仍保留完整名字，短名诊断格式不变。编译错误的位置与运行停点分别维护：实际断点、单步和局部值通过[调试接口](docs/source-debugger.md)使用；编辑器磁盘检测沿[源码工作流](docs/modules/editor/source-editor.md)执行，PDB 和任意机器指令源码映射尚未接入。

当前仍不是完整语言、任意引用值执行帧、永久通用 ABI、跨发布单元热重载/布局迁移的交付。前端不链接 LLVM/Runtime，只共享参数模式与序列操作的轻量声明，降低位于 `modules/execution/jit`，发布与执行服务位于 `modules/execution/source`；机器快照、统计与故障模式留在验收消费者。没有解释执行回退。生成代码使用已绑定的原生入口、真实字段偏移或固定取址操作，不逐次遍历字段路径。共享签名与原生结果的完整约束见[函数契约](docs/runtime-model.md#共享函数签名与结果)，数学值与递归 plain struct 已支持同步按值参数与结果；资源记录/容器的按值参数与结果、对象引用参数与结果及借用返回尚未开放。

早退路径共用按需发射的用户槽清理块，每个 `return` 不重复生成整份清理 IR，并在快照复制与发布之前结束用户引用。槽复用尚未加入，现行调用与清理链见[执行结构](docs/runtime-structure.md#从入口到执行清理)。

### 单入口兼容方法体热更新

正式 [source::Program](modules/execution/source/include/source/program.hpp) 先绑定 Runtime 与对象目录，首次成功发布从真实编译产物解析并固定函数声明，不重复手写源码签名。`request` 保存不可变源码和声明/依赖快照，并准备源码类型布局；`prepare` 完成编译、链接和物化，`try_publish` 在 Runtime 静止点提交。每次发布都可一并提交新的源码类型；后续更新固定已发布的类型定义、函数名、参数/结果、接收者要求、成员目录及实际服务地址。共享发布与类型生命周期合同见[运行时说明](docs/runtime-model.md#单入口兼容方法体发布)。

- 同一 Runtime 任一语言调用仍活动，或正处于原生借用、遍历/GC 回调时，发布返回 `busy`，候选保留供外层返回后重试。暂停的 GC 周期本身允许兼容发布；安全点不暗中切换版本。
- 最新请求一经接受，旧候选不能再提交；最新源码的解析、类型检查或真实链接失败，均保留上次已发布版本，不回退发布过期候选。重复提交及不兼容入口/签名/接收者/服务表明确拒绝。
- 模拟 Tick 与长期工具事件通过稳定弱入口选择当前版本，不保活接收者；显式 `Lease` 固定旧版本并保留其代码和展开信息。程序所有者在回调中销毁也不损坏本次调用，但后续稳定入口立即关闭；最后一个代码持有者释放后才能退休。
- 首次声明固定接收者要求：无 self 的入口不能在兼容替换中新增 self；已有接收者即使新版不再读取，也仍需提供。`describe()` 返回共同签名；Entry/Lease 的 `bind<R,Args...>()` 冷端核对同步按值标量合同并创建 Typed，借用仍由动态 `invoke()` 准备，协程通过 `start()` 创建实例。
- Version 保存自己的已验证元数据和实际调用/引用槽需求；发布后的调用不比较整份成员目录，原始未绑定入口仍完整检查。每次 Invocation 独占按本版本需求准备的暂存空间；效率与需求规模观察不再作为验收。
- 代码寿命的验收保留 Source 关停（回调内关闭与取消后代码页经 `VirtualQuery` 确认退休）和 JIT 平台 finalize 失败撤表两条序列；热更新与失败保旧没有独立验收。

没有加入后台编译线程、文件监视、活动帧迁移、跨发布单元源码调用/内联失效或对象布局迁移；Runtime、宿主回调与 DLL 仍须活得比使用它们的调用/租约长。具体发布与退休协议见[运行时说明](docs/runtime-model.md#单入口兼容方法体发布)。

## 数学基础值

每个 Runtime 自动发布 `core.f32/core.f64` 和 `math.vector2/3/4`、`math.quaternion`、`math.matrix4` 及对应 `d` 后缀双精度类型。C++、反射和源码使用同一原生类型身份；World 与相机直接消费共享数学库。源码数学局部和运算直接生成 LLVM IR，模块经过 O2 优化；没有开启 fast-math。完整语义、迁移边界和示例见[数学合同](docs/math.md)。

源码数学由 `jit_acceptance` 保留一个原生 Vector3d 聚合结果与 sret 代表；原生数学由 `math_acceptance` 检查解析变换及其逆。超越函数的独立真值对照不再作为验收。

已移除的数组计时、超越函数计时与大规模扫描不再提供运行入口；历史结果见[数学研究记录](docs/research/engine-math-design.md)与[超越函数](docs/math.md#超越函数)。

## 执行链路

源码请求、编译、发布、同步调用、协程恢复及代码退役的完整链路见[结构与执行链](docs/runtime-structure.md#从源码到发布)；下图说明同一类型与对象体系的定义来源和访问入口。

协程使用[现行规范](LANGUAGE.md#显式协程) 的 `async` 入口和 `yield` 语句。源码仍经 LLVM/LLJIT，新增显式协程转换；核心 [Coroutine](modules/execution/runtime/include/runtime/execution.hpp) 管理帧状态、恢复准入、合作取消和结果。挂起帧复用共享 GC 根并持有原代码版本；宿主决定恢复时机。真实源码验收由 `jit_acceptance` 覆盖挂起期间 GC 后强/弱引用保持身份、完成帧释放接收者，由 `source_acceptance` 覆盖回调内关闭与取消。

Runtime、宿主回调和 DLL 须活到所有非终态帧清理完成。当前没有 `await` 组合、跨线程恢复、原生深层挂起或挂起帧迁移；LLVM 降低与宿主绑定通过正式源码模块提供。设计取舍见[执行结构](docs/runtime-structure.md#当前取舍)，当前行为见[核心协议](docs/runtime-model.md#协程核心协议)。

```text
原生 NativeRegistration<T>::build() ─┐
直接构造 TypeDefinition ────────────┼─ Runtime::register_types(一组定义)
单条 register_type(TypeDefinition) ─┘
                                    │
                          唯一类型表与不可变元数据
                                    ├─ 对象：create → ObjectRoot → id() → read / write / call / destroy
                                    └─ 值：initialize_value / copy_construct_value / assign_value / destroy_value
                                           使用调用方提供的存储，无独立对象身份
```

- [modules/execution/runtime/include/runtime/runtime.hpp](modules/execution/runtime/include/runtime/runtime.hpp)：运行时公开契约、定义输入和元数据。
- [modules/execution/runtime/src/runtime.cpp](modules/execution/runtime/src/runtime.cpp)：注册验证、布局、存储、生命周期和动态调用。
- [modules/execution/runtime/include/runtime/native.hpp](modules/execution/runtime/include/runtime/native.hpp)：从 C++ 成员指针生成类型定义与调用适配，不拥有第二份类型表。
- [acceptance/runtime/main.cpp](acceptance/runtime/main.cpp)：完整验收入口，同时运行反射与 mimalloc 代表。

原生 C++ 类型通过成员指针访问字段，默认适配使用 `std::addressof` 取得真实子对象地址，不调用成员类型重载的 `operator&`；同一定位用于标量访问、引用枚举和 GC 强边追踪。注册顺序不需要匹配内存布局。描述型记录/对象由运行时计算布局，不需要对应的 C++ 类。值记录内联存储，没有独立对象身份，可以嵌套；原生值记录也可以嵌入描述型对象。

定义输入与已发布元数据的区别见[成组类型发布](docs/runtime-model.md#已实现的成组类型发布)，引用存储、外部根与观察取得见[定型引用与外部根](docs/runtime-model.md#已实现的定型引用与外部根)。

## 当前契约与范围

- 每个 `Runtime` 显式拥有自己的类型和对象实例，单线程使用，不可复制或移动。值操作使用的外部存储及其生命周期由调用方管理。
- 类型先注册后使用；创建对象后仍可在类型安全点（无回调、借用、遍历或活动生成代码调用，且 GC 空闲）追加类型，已发布定义不可变。支持原生与描述型定义成组发布，前向依赖和引用环不要求按依赖顺序提交；整组验证与布局计算成功后才公开，失败不会留下半注册类型。单条注册复用同一路径。
- `TypeId`、`FieldId`、`MethodId` 和 `ObjectId` 都限于所属运行时；限定类型名是声明名称，进程内标识不是存档 ID。
- `create` 交付已登记的 `ObjectRoot`，`ObjectId` 仍只标识身份。根复制增加独立登记，移动转移登记；释放根不立即销毁目标，也不延长 `Runtime` 自身的生命。显式销毁及运行时退出仍释放对象；不可达对象由宿主显式推进 GC 回收。
- 当前支持 bool/float/double/u32/i32、i32/u32 底层枚举、含引用内联记录与单继承对象；原生成员方法支持标量/枚举的值、只读引用及可写引用参数；原生方法与源码入口还支持无 GC 引用的原生记录/Array/String 借用，源码入口的 String 参数限于只读。对象强引用序列见上方合同；资源记录按值调用、对象引用参数与结果、借用返回、通用值容器及记录方法尚未实现；内建数学记录与 plain struct 支持同步按值调用。
- 标量和记录支持调用方存储上的初始化、复制构造、赋值和销毁。含引用记录的复制保持共享目标，不克隆对象；原生记录执行 C++ 操作，描述型记录按字段执行。`TypeInfo` 分别提供复制构造和赋值能力，缺少能力时在操作开始前拒绝。
- `root_value` 用可移动的 `ValueRoot` 登记调用方的活值存储，不接管或延长存储寿命。登记撤销前禁止重建或销毁该存储；赋值仍可更新其中的引用。值接口检查类型、容量、对齐和重叠，调用方负责实际表示、构造状态和存续期。这仍是可信原生接口，不是任意字节的安全验证器，见[值操作入口](docs/runtime-model.md#已实现的值操作入口)。
- `Scalar` 是保存 bool/float/double/u32/i32 或带类型身份枚举值的无堆分配值载体，不承担语言对象或通用动态值的存储。bool 使用独立的 `core.bool` 身份，值存储大小与对齐为 1 字节。
- 通过名称查找成员后可缓存成员标识。类型/成员名称查询目前是线性扫描，定位于注册和发现阶段；缓存后的字段路径访问与方法调用不重新查名字，不分配堆内存（错误报告除外）。C++ 表示到类型的查询（`native_type`、`with_native`/`borrow_native`）使用发布时维护的哈希索引，不随已注册类型数量线性增长。
- `read/write` 的 `Scalar` 重载要求终字段是标量，`ValueOutput/ValueInput` 重载整值读写 plain 值以及不含 GC 引用的 String、Array 和由它们组成的记录；非 plain 写入在夹值后的副本上交换提交，失败时字段保持原值。四个重载另有对调用方持有的记录存储 `(TypeId, storage, path)` 的版本，与对象目标共用路径解析和硬边界，详见[整值读写与调用方存储](docs/runtime-model.md#整值读写与调用方存储)。`restore(object, path, ValueOutput)` 用调用方写入前读出的值与字段交换，撤销调用方自己刚做的写入，不经硬边界和校验器，失败时两边都不变。`read_reference/write_reference` 操作引用字段；字段路径穿过内联记录和数组元素，不隐式解引用对象。引用写入检查目标类型和形式，弱引用不能直接写入持有字段；`try_acquire` 对活的观察目标交付根句柄，空或失效目标返回空结果。已定型的失效引用仍可复制，旧身份不自动清空，也不绑定复用槽位的新对象。
- 字段可声明 UI 范围与硬边界（`FieldRange`）。Runtime 的标量、数学向量、数组元素和记录整值写入会把值夹到硬边界内，NaN/Inf 在改动任何字节之前被拒绝；`constrain` 用于无对象预演。受约束字段拒绝可写借用，可信的 C++ 直接访问不做检查。详见[字段范围与写入约束](docs/runtime-model.md#字段范围与写入约束)。
- 记录和对象类型可以声明纯函数校验器。`validate` / `validate_value` 先检查硬边界，再按后序执行内联记录、数组元素和继承链上的校验器，返回第一个失败及其结构化字段路径；它从不在写入时自动运行。详见[类型校验](docs/runtime-model.md#类型校验)。
- 每个对象带写代号：Runtime 的写入、元素和引用修改、`restore`、可变 `with_native`、可写接收者和可写字段实参的方法调用以及创建都会盖章，`with_native<const T>` 和读取不盖章。`change_cursor` 在宿主安全点取游标，`changed_since` 和 `changed_objects` 查询游标之后被写过的对象。详见[写代号与变化游标](docs/runtime-model.md#写代号与变化游标)。
- `FieldId` 是成员选择子：记录和对象的成员是字段，Array 的成员是元素。`element` 构造元素选择子，`insert_element/remove_elements/move_element` 原子地改变内联 Array 字段的结构，回调和借用期间返回 busy。元素选择子是位置性的，结构变化后可能失效。详见[成员选择子与数组元素](docs/runtime-model.md#成员选择子与数组元素)。
- `visit_roots` 枚举外部根入口，`visit_references` 枚举一个对象或外部值的非空引用边，包括持有、观察和失效身份；它们不执行可达性收集。原生 `hidden_field` 不暴露公共字段标识，但参与布局、值能力与引用遍历。访问者执行期间拒绝 Runtime 修改操作和新增根，异常转换为调用错误。
- `begin_collection` 开始一轮非移动增量标记—清扫；宿主在同一运行时线程的安全点调用 `collect_step`，按根处理、标记、清扫分步推进。分配、普通值操作和撤根不隐式触发收集；编辑器与项目运行宿主在各自帧安全点推进收集。
- 标记完成时统一提交死亡判定，`live_objects` 立即排除待回收对象，`pending_reclamation` 单独报告待析构数量。即使内存尚未释放，观察取得和对象访问也不能复活已判死目标；旧引用保留身份。
- 收集步支持工作量上限和可选的软时间上限，返回阶段、实际工作量、回收数量与待回收数量。单次原生析构不可中断；写入屏障与值操作不计入 GC 步预算，不承诺硬帧时间。具体计量与存活规则见[分步回收](docs/runtime-model.md#已实现的分步回收)。
- 方法参数或非 void 输出存储检查失败不会进入用户代码。成功值先在 `CallContext::result` 暂存，核对声明后才写 output；缺值、错类型、未声明结果或异常不覆盖调用方原值。已发生的业务修改不自动回滚。
- 初始化/复制构造失败会清理已构造部分，不留下半成品；赋值失败保证可销毁、基本不变量成立，允许部分更新，不自动回滚。原生操作必须遵守这些保证，方法异常转换为调用错误。回调中抛出的 `std::bad_alloc` 同样由这些异常边界转换；运行时自身存储、元数据、根登记和错误信息的分配失败可能继续向外传播分配异常，当前不提供完整的内存耗尽恢复保证。
- 原生绑定属于可信代码，必须提供正确的布局和生命周期操作，并声明全部对象引用，包括不公开的原生成员；析构不得抛出异常。原生引用槽通过 Runtime 的引用接口修改，整条原生记录复制走值操作；不能绕过这些入口任意覆盖引用存储。C++ 表示标记只验证原生字段兼容性，不是语言类型身份。
- 普通回调（包括原生字段定位器）期间禁止创建、销毁对象、注册类型或启动/推进 GC；允许访问有效字段、同步方法调用，以及合法的引用写入和根登记。`CallContext::storage` 只在本次同步调用期间有效，不能缓存；只读引用遍历另行禁止这些修改操作。
- JIT 业务代码直接调用定型原生 thunk，并按发布布局或固定取址操作访问字段；不把 C++ `Scalar`、`Result` 或私有身份布局暴露给生成代码。接收者存储只在同身份的有效执行段内复用，策略回调与协程恢复使缓存失效；字段值不缓存，写代号、硬边界、原生生命周期/重入保护和异常转换继续有效。动态 Runtime 反射接口仍保留。

## 已验收场景

验收从公开接口执行，未引入单元测试框架。当前 `runtime_acceptance` 每种行为只保留一份必要代表：

- 有根对象经完整 GC 周期保留，撤根后被回收。
- GC 游标正在访问原生值记录的引用存储时撤销值根并释放存储，周期结束后不追踪已释放存储，也不保留目标。
- 反射生成的原生类型构造/析构各执行一次，字段元数据在 Runtime 析构后仍完整。
- mimalloc：独立 DLL 在 main 之前的 CRT 分配、calloc 零初始化，以及跨 DLL 扩容保留值 123 后释放，均落在 mimalloc 堆。独立验收 DLL 不链接 mimalloc，用于确认 CRT 路径的实际重定向。

执行合同与验收范围见[运行时协议](docs/runtime-model.md#业务执行协议)，尚未实现的范围见[当前能力边界](docs/runtime-structure.md#已知成本与能力边界)。

现行源码语法见 [LANGUAGE.md](LANGUAGE.md)，长期方向与待定问题见[业务语言设计契约](docs/language-contract.md)；待定方案不代表已实现能力。

World 配置通过 **Window → World Settings** 打开，光照、天光、DDGI 与效果写在 Environment 组件上，随当前 Scene 格式保存并支持同一场景撤销；诊断留在视口。Parameters 支持 AFIELD Label / Category 元数据和 Component / Category 分组；详情见[编辑器文档](docs/modules/editor/README.md)。
