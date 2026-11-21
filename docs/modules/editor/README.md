# 场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。

现有外壳、菜单、工具架、停靠与业务面板由同一 DSL 装配；区域职责、控件语义和未实施的手势见[声明式 UI](declarative-ui.md#区域与手势边界)。
启动日志和 Help → About 使用构建时生成的同一提交标识及配置；未提交改动标为 `local changes`。`atelier_editor --version` 可在不创建窗口的情况下读取该二进制的标识。工程打开成功后记录实际根目录，About 同时显示当前工程目录和场景路径。标识在构建时刷新，运行时不读取当前目录的 Git 状态。
当前消费者将正式 Entity/Component/World 接入同一编辑器，并使用自动反射、共享调用合同与基础值类型。输入、视觉合同及验收入口见下文。World 使用可选内联空间的 Entity、StaticMeshComponent 和真实 StaticMeshEntity；网格使用中立的不可变 `geometry::MeshRef`，遵循[World 合同](../world/README.md)。
模型与渲染消费完整 TRS；参数面板展示实例的公开标量和内联记录。位置、欧拉角和缩放使用三分量行，规范数学类型按元数据组合 Vector2/3/4，材质颜色按 RGB 展示；选择 Material 时编辑共享材质参数，其他资源显示元数据。已接入真实网格拾取、多选/框选、变换手柄及 G/R/S 操作。已声明默认值、修改状态、折叠分组、独立菜单、中键数值阶梯继续复用同一控件链；编辑器内 JIT 单步交互、可浮动编辑器视图、布局文件保存、参数面板固定目标仍未实施。

UI 作者层已将 Parameters 的查询、Position、属性组、Entity Structure 和 Scene Tree 迁入 [editor.ui.source](../../../assets/editor.ui.source)，使用有类型的组件组合、动态项和原单行编辑器。在 Dev / Release 开发工作树保存声明文件即可更新面板；代码按组件复用，兼容的 Instance / Context 保留，只增删对应子实例，源码失败保留旧定义与交互。布局、样式、输入与业务事务继续沿用现有设施；作者语法、生命周期和更新边界见[原生声明式 UI](declarative-ui.md)。

## 工程与文件

[Editor Control](control.md) 是指令化操作与调试的标准入口，供 UI、快捷键、CLI、自动化、验收和 LLM 共用。现有外部指令支持实例发现、场景操作、内存快照导出和渲染对照；UI 直接调用业务层，本机管道与 CLI 适配到相同的 Application/Session 入口。正常编辑器默认启用外部接入，`--no-control` 可关闭；源码调试面板和 `debug.*` 仍待接入。

场景和视口命令通过同一操作符入口供菜单、快捷键与 Control 调用；参数 schema、可用性、历史和后续语言接入边界见[操作符](operators.md)。

工程直接选择文件夹，包含 Config、Content、Source、Cache。已接入创建/打开、场景保存重开、资源目录、三种浏览视图和 OBJ/glTF 几何导入；Source 文件已接入编辑器内创建、跨文件编译、真实组件类型、属性持久化、独立运行与兼容热更；项目 UI 采用同一工程配置并支持独立预览。格式、操作、资源身份和实际支持范围见[文件夹工程](projects.md)。

## 源码编辑

File → Edit Editor UI Source 可直接打开并修改当前 UI 声明；Code 面板支持持久文档、独立撤销、保存、编译诊断定位和失败恢复。用法、边界与验收见[源码工作流](source-editor.md)。

## 当前 UI 作者入口

现有编辑器界面统一由 [editor.ui.source](../../../assets/editor.ui.source) 描述，包含总外壳、顶部菜单栏、工具架、状态栏、停靠栏/分隔条、Scene View、Parameters、World、Scene Tree、Entity Structure、Network、Geometry Spreadsheet、Asset Browser、Controls、Code、Project Output 和原生窗口内的弹出菜单。C++ 提供数据与身份、业务命令和原生机制；各区域不再保留另一条 C++ 界面组装路径。支持范围、视觉层级规范、动态内容槽和更新事务见[声明式 UI](declarative-ui.md)。

## 正式执行链

```text
Win32 窗口消息 → Window 复制拥有的事件 → 每条消息后顺序排空
  → UI 已接受帧命中/焦点/捕获 → Reply.effects
  → Action → editor::Session → world::World → Runtime 的真实 Entity/Component 字段
           → Workspace.query → 参数查看投影
  → ViewportTool（已接受的视口/相机）→ Session 的选择与整组 TRS 事务
  → PlatformRequest → Window 剪贴板/IMM32 → UI 结果事件
  → Workspace 控件描述 + World RenderItem 矩阵/资源/材质值 → UI 帧 + renderer 网格实例
 → renderer 最终 accepted → UI 接受对应命中帧
```

WndProc 的输入事件只复制和排队，不重入 UI/业务；同步输入回调也在同一泵中依序消费。UTF-16 代理项在顺序投递时组装，组合流绑定独立 composition_id 与原编辑身份。候选位置只使用与当前 token 相匹配的 accepted caret。同步 IMR_QUERYCHARPOSITION 只读对应帧的插入/组合起点快照，将客户区物理坐标转为屏幕坐标；仅支持偏移 0，无有效快照或其他偏移明确拒绝。快照在原生组合/失焦/Surface 变化时立即撤下，不用旧显示 caret 冒充组合起点。IME 终止键的首次按下、重复和抬起保持同一归属，避免确认组合后又提交字段。
`Window::poll` 是线程消息泵的唯一入口：只有主窗口取线程消息并处理 WM_QUIT；弹窗只消费自己的排队记录与回调错误，不在捕获期间另外泵走退出消息。
系统模态窗口的 `WM_CANCELMODE` 在 WndProc 内立即交给 `DefWindowProcW` 释放原生鼠标捕获，同时排队保留业务取消事件。文件选择和保存确认会暂停应用主循环，不能把原生捕获释放也延后到该循环；否则已禁用的主窗口会继续截获对话框的鼠标输入。业务取消仍在原有事件泵中执行，不从系统回调重入 UI、场景或渲染。该时序遵循 [Windows WM_CANCELMODE 合同](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-cancelmode)。
剪贴板失败保留草稿/选区/历史供重试；业务 Action 失败停止同组剩余普通 effects，仍执行 UI 取消、业务回滚、IME 与捕获清理，保留首错并报告清理失败。

[World](../../../modules/engine/world/include/world/world.hpp)和 Runtime 管理唯一 Entity/Component 身份、组成与成员资格。编辑 World 保持 stopped；Entity 的 AttachedInactive 状态仍可呈现和编辑，renderer 不按 Active 状态筛除编辑内容。
World 的原生类型与字段元数据由[自动反射](../../reflection.md)生成，经 `world::register_types` 发布到 Session 拥有的 Runtime；项目 Source 派生类型在 Session::Prepare 中由同一 Runtime 类型系统发布。编辑器使用该入口返回的类型和字段路径；生成头、World 库及正式编辑器均由当前工作树、当前配置的 CMake 依赖链构建。
[Session](../../../modules/editor/core/include/editor/session.hpp)是文档唯一写入口，保存 `{entity, object}` 选择和统一的 Entity、组件、字段历史。字段事务捕获具体对象 ObjectId、正式 FieldPath 和控制会话，不再从当前选择或类型顺序猜目标。一次拖调连续改真实字段，提交只记一次历史。Entity 的空间字段可能使四元数其他分量规范化，所以记录完整 TRS 前后值，经 World 原子恢复；不是删除时的对象图快照。属性 undo/redo 恢复该字段所属对象作为检查目标，并保留其 Entity 上下文。同一目标重复选择不取消草稿；换目标先取消事务。
Entity 删除记录保存完整子树根、原组合父与成员位置；组件独立移除记录保存组件根及 owner/组成顺序。Undo 恢复同一 ObjectId 的当前组成与成员资格，重新生成 UI 绑定。裁剪历史/redo 与关闭文档只释放持有，不隐式 destroy。历史最多128项，编辑器最多256个 Entity；只有 World 成员可编辑，没有离线写入或多写者冲突机制。
World 操作的 Result 失败表示提交未完成；成功 Commit 中的 notification_error 表示关系已改变。Session 先记录变化并更新选择，应用再报告通知错误并停止后续动作，不把它当成未提交或重做一次。成员 span 只在结构变化前借用，UI/renderer 不持有它。
Session 的 `set_transform`、`remove_transform`、`set_space_reference`、多项 `reparent`、`erase_entities` 和 `properties` 直接传播 Runtime/World 的 Result 错误，保留 code、message 和 source，不经异常中转。多项 reparent 仍按输入顺序发布：后项拒绝时先将已提交前缀记为一次历史，再返回原拒绝；不回滚前缀、不执行或重试后缀。已提交项的 notification_error 同样在记录历史后交付，并停止后续对象，不降为成功或替换首因。
宿主同线程逐帧以32工作单位推进 GC；普通根保活不表示对象仍参与 World。文档关闭先结束活动编辑，再关闭 World、释放历史；World close 撤成员/参与和根，Runtime 生命周期随后由宿主管理。

Session 默认构造只建立空壳，在稳定地址上调用一次 `initialize(Prepare, SessionRole)`，以 `Result<void>` 传递引擎/编辑器类型注册、World、呈现与 Prepare 的完整错误。只有成功后才能使用文档方法；失败后只允许析构，不承诺重试，因为类型注册可能已部分改变 Runtime。这与初始化失败后可重试的 World 不同。析构在呈现已建立时先关闭 World，再按原成员顺序退役物理、呈现、World、Prepare 返回的代码所有者和 Runtime；更早失败由已构造成员清理。
Application 的稳定空壳通过 `initialize` 接收已初始化的独占 Session；两个宿主创建入口都先初始化 Session，再创建 Window，文档初始化失败不会打开窗口。Window、UI、Renderer、主/子 Popup 的顺序不变，全部成功后才显示主窗口，部分初始化失败只允许析构。Win32 初始化错误保存当时的错误码，UI 诊断保留 code/key/native/message，Renderer 诊断保留 cause code/native_result/message；宿主 Error 无原生码字段，这些值写入诊断文本。工程修复用的空文档同样先准备成功，再退役旧文档；`deserialize` 保持先解析 JSON、后执行 Prepare，`Project::create` 对初始化错误仍只返回 `invalid_storage` 与原 message，不带 source。

部署文件 `editor.ss` 的打开、大小（上限 1 MiB）和读取失败由 `editor_styles()` 返回 `invalid_storage`，解析失败返回 `invalid_definition` 并保留行列和 parser 原文；不再通过异常传递。Runner 在这些启动失败时写 `runner.failed` 后正常返回，World 和 Renderer 沿正常作用域清理。

## 参数和坐标

MS/MI、槽菜单、共享编辑、独立资产历史和实例覆盖使用同一 DSL/Runtime/Session 链。参数按 MS 声明生成，覆盖复选框明确表达继承意图；组件不再有固定整体材质控件。文件入口、Scene v10、保存与实际限制见[材质资源工作流](projects.md#材质与贴图资源)。


位置 X/Y/Z 默认0米，合法域为有限的 `[-10000, 10000]`。普通拖调每逻辑像素0.01米，细调0.001米。
范围、默认值、单位与拖调步长由[应用参数描述](../../../modules/editor/core/include/editor/session.hpp)绑定 Runtime 字段，布局与 Runtime 不承担编辑器政策。
控件 key 独立单调分配，绑定具体 ObjectId 和 FieldId 路径；稳定控件复用其键，失效后退役且不再分配。Entity/组件恢复保留业务身份，但不会恢复旧控件会话、IME 或剪贴板请求。

位置、旋转、缩放和数学向量复用 DSL `NumberRow` 与共享 `append_number_row`。标题和字段随宽度换行，分量共用一行。根旋转使用 XYZ 顺序的欧拉角（度），实际存储仍为规范化四元数；一次输入始终基于事务起始姿态计算，避免跨过 90° 后将中间转换结果反馈到下一次预览。显示角为四元数的规范欧拉表示，不保存额外的圈数状态。缩放按绝对值限制在 10000 以内：负值为镜像，0 可以保存（呈现与物理按退化处理），不提供 shear；镜像 Entity 在移动、旋转、缩放后保留自身的镜像轴，缩放手势可以越过 0 进入镜像；含零轴的 Entity 同样可以移动、旋转和缩放，零轴保持为 0；位置范围与上方参数定义一致，手柄和字段沿 Session 执行相同范围检查。

数值显示按底层 float/double 生成可往返的最短十进制，不把 float 扩成 double 的尾差展示为有效精度。欧拉角与四元数转换使用 double 中间计算，最后写入 float，避免额外的中间舍入误差。数学元组根据 Runtime 数学类型元数据识别；Entity 的旋转与 World 材质颜色由实际字段身份识别。标题和菜单可恢复完整位置、旋转、缩放；位置恢复包含搜索隐藏的分量。无变化不增加历史，undo/redo 恢复实际检查对象及 Entity 上下文。

- 左手坐标系，+Y向上，单位米；方块各轴范围 `[-0.5, 0.5]`。World 组合局部 `S * R * T` 与父世界矩阵；renderer 不自行重算 TRS。空间换父 API 在 World 提供，当前面板编辑根位置。
- 默认相机 eye `(6,5,-8)`、target `(0,0,0)`、up `(0,1,0)`；使用 LookAtLH/PerspectiveFovLH。
- CPU 矩阵行主存储，行向量按 `p * world * view * projection` 计算，D3D 深度为 `[0,1]`。
- viewport 与 UI clip 使用窗口客户区左上为原点的物理像素。1280×720客户区内的 `{240,32,760,660}` 表示从 `(240,32)` 开始、宽760高660的视口。
- UI 根据实际 backbuffer 尺寸和 DPI 生成 draw/clip；renderer 不再次缩放。深度目标显式匹配实际 backbuffer。

## 视口编辑

停靠区支持 Blender 风格的悬浮跟随焦点（`DockSpace::focus_point`）：在无模态手势、弹出菜单或活动单行文本/数值输入时，鼠标移入可见停靠面板即切换活动区域焦点（例如光标悬浮在 `Network` 时按 `F` 聚焦节点网络，悬浮在 `Scene View` 时按 `F` 聚焦选中对象或全场景）；`Code` 缓冲区在光标移出该面板时自动释放键盘占用。`Network` 面板可见或正在检视节点时，`SceneSnapshot` 不对 `ProceduralGeneratorEntity` 叠加整网面选中内线框，保证 MatCap 等模式下未开启 Wireframe 时仅显示干净曲面。

`Network` 画布支持 `Shift+D`（复制选中/悬停节点并进入跟随光标放置，左键或 `Enter` 确认并合为一步撤销，`Esc`/右键保留偏移副本）、`Ctrl+D`（偏移复制）、`G`（抓取移动节点）、`Tab` / `Shift+A`（节点搜索创建）、`A` / `Alt+A` / `Ctrl+I`（全选切换、清空选择、反选）、`X` / `Del`（删除节点或注释框）、`F` / `H`（聚焦选中或全图）及 `D` / `T` / `B` / `Z` / `C` / `I` / `L` / `Y`。

G／R／S 通过同一 ViewportTool 与 Session 事务预览、提交和撤销 TRS。仿射矩阵求逆保持精确的齐次末列，避免带旋转和平移的有效对象因消元舍入而被误判为透视变换；真正的剪切、反射、非有限值仍沿 World 规则拒绝。

父子多选时，继承去重只减少实际写入对象；活动 Entity 的世界原点和朝向独立保留，用于 Local 基底、吸附、手柄拾取、中键择轴、约束线与读数。缩放统一使用 Local。框选与变换互斥；约束切换、反选和清除变换只处理首次按下，修饰键释放会立即更新当前预览。相机拖动转入全局命令时，先结束旧拖动再释放捕获，迟到的鼠标释放不会恢复旧手势。

Scene View 左侧 38 DIP 工具栏以 32 DIP 单元排列选择、通用、移动、旋转、缩放，选择与手柄工具之间及底部吸附开关前各有 `#4d4d4d` 细分隔，选中态 `#5d7289`。工具图标使用 [Tabler 掩码](../../../assets/icons/README.md) 与原生超采样矢量图标栅格化，沿原生符号栅格化、主题着色和缓存链绘制，不复制 Houdini 专有图标素材。视口右侧显示栏（34 DIP）包含参考网格、原点轴、线框叠加、纯线框、三角边、背面绕序青蓝高亮（Backface Winding Tint，工作室着色下启用）、Cavity、Scene Lit、FPS；在 SOP 图上下文或世界态选中 `ProceduralGeneratorEntity` 时，右侧显示栏与 Guides 菜单同步展开 6 项程序化几何诊断开关：点标记（纯点云节点默认自动开启，点击后转为显式开关，支持 `Cd` 点色与半球深度消隐）、点法线（`N`）、图元面法线、姿态与缩放环（`orient` + `pscale`）、包围盒与实例空间分块（AABB + Chunks）以及右上角几何信息 HUD 卡片（汇总点数/顶点数/图元数/实例数/尺寸/属性列表与求值耗时）。全工作区工具栏、停靠栏、显示栏与节点检索面板按钮统一挂载“英文控件标题（含快捷键）+ 中文功能说明”悬停提示。每个显示栏按钮只改其对应的一项视图或诊断设置，与头部着色段、Guides 菜单和 Z 饼菜单同源同步，不进入场景历史。

视口头部（32 DIP 一行）融合 Blender 的语义分组：路径之后依次为交互模式图标菜单、**View / Select / Add / Object** 平铺文字菜单、枢轴下拉、吸附开关+步长、叠加层（Guides）下拉、Cavity、四段着色按钮（Wireframe / Solid / MatCap / Scene Lit，与 Z 饼菜单同一组预设）及完整显示菜单箭头。View 菜单含 Focus Selection、Frame All、Viewpoint 子菜单、工具选项栏与 FPS 开关；Select 菜单含 Select All / None / Invert、Box Select 和选择模式子菜单；Add 列出全部创建命令（同 Shift+A）；Object 菜单含 Duplicate、Delete、Clear（Location/Rotation/Scale，同 Alt+G/R/S）、Snap 子菜单与 Transform 增删。面板宽度不足 660 DIP 时文字菜单与 Cavity 折叠，W 菜单和主菜单 View 仍可达。

吸附菜单按 Blender 吸附面板分节（带灰色节标题）：总开关、步长（0.1/0.2/0.5/1 m）、Affect（Move/Rotate/Scale 各自是否随吸附开关吸附）、旋转步长（1°/5°/15°/45°）、选择/游标对齐；G/R/S 的 Ctrl 仍临时反转吸附，缩放步长 0.1。操作栏的 Global/Local 下拉即 Blender 变换朝向：Local 时移动/旋转手柄与 X/Y/Z 约束沿活动 Entity 的自身轴，网格吸附只在 Global 下生效（网格是世界格）；缩放始终沿局部轴。Normal/View/Cursor/Parent 朝向尚未实现，不在菜单中显示。

头部下方是可折叠的 28 DIP 操作控制栏（`#2e2e2e`，黑色上边与 `#424242` 内沿），左侧为 Blender 五段选择模式（Set / Extend / Subtract / Invert / Intersect，决定右键点选和框选如何与当前选择合并；Shift 仍为切换、Ctrl 仍为减选），随后是轴空间、枢轴名称和表面游标；G/R/S 期间整条换成 Blender 头部读数（如 `D: 0.9499 m (0.9499 m) along global Z`、`Rotation: 22.90 along global X`，键入值显示为 `[2|]`）。视口左上是带阴影的白色操作名与图标，右上是半透明圆角视图胶囊（`Persp ▾`），左下为 Houdini 彩色圆片坐标轴，底边一行为当前工具的鼠标/按键提示，右、下两边绘制 `#424242/#666666` 凹边。背景渐变为 `#657f8c → #bec6c6`，选中描边为 Houdini 黄 `#ffd900`。

| 操作 | 行为 |
| --- | --- |
| 右键单击物体 | 对实际网格三角形求最近命中；无可渲染网格的空间根以十字定位点命中 |
| Shift / Ctrl + 右键 | Shift 切换该 Entity 的选择，Ctrl 移除；Scene Tree 中 Ctrl 切换、Shift 选择可见范围 |
| 右键拖动 / B 后左键拖动 | 以投影包围范围框选，包含遮挡后的对象；Shift 添加、Ctrl 减选，Esc 取消 |
| Q | 切换纯选择工具，便于框选和选中被手柄覆盖的物体 |
| 左键拖手柄 | 箭头平移、平面菱形约束平移、圆环旋转、轴端方块缩放；中心方块进行视平面移动或等比缩放。手柄比例与配色取自 Houdini：`#ff1414/#14ff14/#1414ff`，拾取高亮 `#ffd900`；旋转球的背面弧段淡化，外圈为紫色视平面环 |
| G / R / S | 开始移动/旋转/缩放；X/Y/Z 约束轴，Shift+轴约束平移/缩放平面；再次按同一轴解除约束 |
| 输入数字 | 移动为米、旋转为度、缩放为倍率；Backspace 修改，Enter 或左键确认，Esc 或右键取消 |
| G/R/S 期间中键 | 按指针移动方向选取最接近的世界轴作为约束，与 Blender 相同；约束轴以整条世界轴线显示 |
| A / Alt+A / Ctrl+I | A 全选，已全选时再按 A 取消全选；Alt+A 取消全选；Ctrl+I 反选 |
| Alt+G / Alt+R / Alt+S | 分别清除所选根的位置、旋转、缩放，各记一次历史 |
| 小键盘 1/3/7，Ctrl+1/3/7 | Front / Right / Top 正交视图；加 Ctrl 切换 Back / Left / Bottom。小键盘 5 或 0 回到透视，`.` 框选所选，Home 框选全部 |
| `·` / \` 键（Esc 下方，AccentGrave） | 六向视图饼菜单，另含 Perspective 和 Focus；轻按后点击，或按住、划向选项、松键确认。Esc/右键取消 |
| Shift+D | 复制所选 Entity 及其组件并进入移动；左键或 Enter 确认，Esc/右键只取消位移、保留原位副本。复制和初次移动合为一次撤销 |
| X | 在指针下打开删除确认项；取消不修改场景。视口始终删除 Entity，检查某个 Component 时也不会误删成单个组件 |
| Shift+A | 在指针处打开添加菜单（Create 组命令，于 3D 游标处创建） |
| Z | 渲染模式饼菜单：左 Wireframe、右 Solid、下 MatCap、上 Scene Lit。轻按后点击，或按住、划向选项、松开 Z 确认；中心死区内长按松开取消。Esc/右键取消 |
| Shift+反引号 | FPS 自由视角：鼠标看向，WASD 移动，Q/E 下降/上升，滚轮调速，Shift 加速。Enter/左键确认，Esc/右键恢复进入前的相机；显示 Fly、中心准星和操作提示 |
| 吸附开关 / Shift+Tab | 开关移动与游标的网格吸附；步长为 0.1、0.2、0.5 或 1 米，与参考网格共用设置。移动时对齐活动 Entity 原点，保留组内偏移与被约束的坐标 |
| Ctrl / Shift 拖动 | Ctrl 临时反转移动网格吸附开关；旋转为 15°、缩放为 0.1 倍率吸附。Shift 使用十分之一步长；键入数值仍按精确值执行 |
| 左键单击空白或物体 | 放置 3D 游标，不修改选择或场景历史。游标是带朝向的三维坐标系：表面定位时局部 Z 对齐命中面法线，X 尽量对齐视图右方向；绘制为 Blender 4.x 样式的 16 段红白虚线圆环，四根黑色刻度沿游标自身 X/Y 轴投影，因此贴面放置时可见其倾斜。无命中时使用当前视图的构造平面，射线平行时使用穿过旧游标的视平面；关闭 Surface Cursor 后只使用平面定位，重置游标同时清除朝向 |
| W | 打开模式、Snap、聚焦、创建和删除菜单；Snap 提供选择到网格、选择中心到游标、游标到选择/原点/网格 |
| 中键 / Alt+左键 / Shift+中键拖动 | 前两者环绕相机，水平拖动按抓取场景的方向旋转，Shift+中键平移；滚轮缩放，F 聚焦当前 Entity。正交视图开始环绕后转为透视 |

Alt+A/G/R/S 以 `WM_SYSKEYDOWN` 到达，窗口把这四个键与菜单助记键一样交给应用，不进入系统菜单。快捷键仅在视口内且文本输入未占用键盘时启动。视口上方的交互控件先参与统一 UI 命中，点击视图菜单不会向后穿透为游标定位。多选变换只处理具有内联空间的 Entity；Parameters 展示活动 Entity，视口手柄修改整组。移动/旋转遵循所选 Global/Local 朝向；缩放手柄沿活动 Entity 的局部轴，改变各 Entity 自身的 scale 分量，并在活动 Entity 基底中缩放组内位置偏移。枢轴可选组中心或游标，Static Mesh 创建使用游标位置。物体变换的表面/顶点吸附、跨面板资源放置和程序外拖入尚未实现。

`ViewportTool` 只持有手势，`Session` 是业务修改入口；一个手势记录一次历史，组内任一目标拒绝时恢复前一有效预览。非法或未完成的数值不能确认；Esc、右键、失焦、捕获丢失、尺寸/DPI 改变、菜单转移、文档更换都取消草稿。Ctrl+Z/Y 先结束视口草稿，再执行同一场景历史。活动变换期间拒绝保存快照与启动 World。

复制沿用场景可编辑属性的同一字段规则，创建独立 Entity 子树与 Component 身份，重建组合关系、空间参考与启用状态，复制业务属性、材质与物理配置，复用不可变网格/碰撞资源。整组选中先在脱离 World 的状态下准备，成员发布失败时回退；不复制运行期句柄或速度。同 Runtime 复制将子树内部的强/弱引用和持有序列重定向到新对象，图外引用保留原目标及其强弱语义；独立子树存档拒绝图外引用。初次定位只允许合入刚建立且未保存的创建记录，之后的普通移动照常单独记历史。交互参照 [Blender Duplicate](https://docs.blender.org/manual/en/4.3/scene_layout/object/editing/duplicate.html) 和 [Pie Menus](https://docs.blender.org/manual/en/latest/interface/controls/buttons/menus.html)；渲染模式使用 Z，吸附保留在磁铁菜单和 Shift+Tab。

自由视角采用 [Blender Walk Navigation](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/walk_fly.html) 的无重力移动方式。相对鼠标只在前台视口手势期间注册、隐藏并约束；失焦、捕获丢失、尺寸变化、菜单转移或文档更换时取消并释放。它复用同一 Camera，不修改 Scene、3D 游标或撤销栈；Control 在飞行期间报告 `camera_fly` 忙状态。真实原始鼠标输入目前没有自动验收。

视角与渲染模式饼菜单共用 DSL 的 `ViewportPie` / `PieEntry` 和稳定的根节点身份，只更新标题与条目；叠在同一接受帧内，复用菜单项模型与命令执行入口。普通列表菜单继续使用可跨窗边界的原生 PopupHost。方向选择基于接受帧的槽位，显示位置受客户区约束；边缘位置可直接点击可见项。快速划动松键在首次呈现后结算；菜单打开期间屏蔽场景输入，关闭、失焦和 surface 变化撤销模态状态。六向相机的投影、平移、拾取与游标构造平面使用同一相机基底；切换视角不进入场景历史。快捷键参照 [Blender 默认键位](https://docs.blender.org/manual/en/5.0/interface/keymap/blender_default.html)。

可勾选菜单项的方框列允许连续修改：按下和松开都在该列内时，切换后保留同一菜单窗口与焦点，并从权威视图设置刷新勾选状态。点击文字或用 Enter/Space 激活则切换后关闭；互斥项和普通命令维持原有关闭行为。

绘制和命中遍历同一手柄几何；坐标和相机只在 renderer 接受帧后发布。线段/方框复用 renderer 固色实例管线，在场景后、UI 前绘制为一个裁剪批次，不写场景深度，不建立第二个视口窗口或渲染循环。

### 工作室 Cavity

Scene View 导航行末尾的 **Cavity** 菜单控制屏幕空间凹凸增强：`Screen Cavity` 开关、Ridge 凸边亮度、Valley 凹边暗度及 Radius 采样半径。默认开启，两个强度默认 1、范围 0–2，半径默认 1 物理像素、范围 0.5–4；菜单提供常用档位，Control 接口支持范围内的数值。效果用于 MatCap、Smooth 和 Flat；Scene Lit 和纯线框时入口禁用并保留偏好。

增强使用几何深度和法线，在网格、选中轮廓、手柄和 UI 之前合成；不改场景、材质和撤销历史。此项对应 Blender 的 [Screen Cavity](https://docs.blender.org/manual/en/3.6/editors/3dview/display/shading.html)，随屏幕像素工作；渲染和资源寿命合同见[渲染模块](../renderer/contract.md)。

### FPS 显示

通过视口 **Guides → Show FPS** 或顶部 **View → Show FPS** 开关，默认关闭。启用后在左上角模式名称下方显示帧率及平均帧间隔（ms），随[帧性能](#帧性能)快照约每 0.25 秒更新。数值取 CPU 段最近 0.5 秒内（最多 120 个）相邻采样步的帧间隔均值，包含正常 UI、场景绘制及呈现等待；它不是 GPU 单独耗时。诊断截图、提交失败、最小化及没有宿主提交的服务轮不计为采样步；它们或 Recorder 序号断档会立即清除快照中的旧间隔与 FPS，直到第二个连续采样步才恢复，避免将暂停、服务和读回耗时混成帧率；显示域变化后的首个间隔也只在新域内形成。文字不占布局空间、不拦截视口输入，不修改场景历史；视口隐藏时不显示。开关由公共 `show_fps` 命令和同一布尔 Action 入口处理。

### 场景光照

Scene View 的显示菜单只提供着色预览与边线模式，`Scene Lit` 与 Smooth、Flat、MatCap 放在同一互斥组。场景作者参数沿 World 页和同一 ViewOption / Session 路由编辑，修改进入场景撤销栈，随 Scene 保存。直接光对保守灯列表中的解析灯逐盏确定求值，投影光源的遮挡来自阴影 atlas；`Direct candidates`（1～32，默认 8）只控制 DDGI 探针命中点的局部光候选数。AA 默认 SMAA；显示模式、相机和诊断开关仍属于视口。Scene Lit 管线由 Renderer 在工作线程上按设置分组准备，首次使用某组时不阻塞编辑器：视口先继续显示上一个已画出的 Scene Lit 视图，还没画过时显示 Smooth，状态栏显示 `Compiling shaders n/m`（n/m 按当前视图需要的组计算，打开新组时总数会变大），就绪后自动切回。准备失败时状态栏显示错误，视口保持 Smooth。编辑器和 Play 进程把编译结果缓存到环境变量 `ATELIER_SHADER_CACHE` 指定的目录，未设置时为 `%LOCALAPPDATA%\FrostFoxAtelier\ShaderCache`。快照、性能采样模式和 Control 验收在宿主未设置该变量时改用编辑器可执行文件旁的 `shader-cache`（即 `out\dev\shader-cache` 与 `out\release\shader-cache`，两种配置不共用），并由它们启动的 Play 进程继承，不写用户缓存。验收需要冷启动（强制出现未就绪和回退）时，宿主在启动编辑器或 `atelier_editor_control_acceptance` 前把 `ATELIER_SHADER_CACHE` 设为一个新建的空目录；已设置的值不会被覆盖。

**Window → World Settings** 打开可停靠的 World 页。页面由 `editor.ui.source` 声明，子页为 Lighting、DDGI、Effects、Diagnostics、Performance。Lighting 提供曝光、阴影、直接光候选数与 Sky 组（均匀天光的开关、强度和颜色）；这些作者值写在 Environment 组件上，第一次编辑时创建它，与这次写入是同一条历史。具体灯光通过 Scene Tree / Entity Structure 选择，并在 Parameters 编辑，World 页不枚举场景灯光。Effects 提供 GTAO、SSSR、Bloom、AA 与半径/距离。DDGI 可选择四个固定槽位，编辑启用、Fixed / Follow Focus、边界、三轴数量、混合边界、共享更新预算、射线数、反馈和间接光增益。Follow Focus 持久化模式和相对边界；实际焦点由宿主提供，编辑器使用观察目标。Diagnostics 包含探针命中点的空间选灯开关、探针球体、接收面指标、体积、X-Ray、反馈可见性和 CPU 布局读数，不进入 Scene。

World 字段的 label、category、description 与类型、范围、枚举值统一声明在 `ViewOptionInfo` 原定义表中，面板不再维护另一份逐项名称或帮助表。同一描述供共享属性行与 Control 查询使用；Probe Volume 的四个槽位和 XYZ 分量共用说明，Follow Focus 只按真实语义把边界标题改为 Min / Max offset。World 与 Entity 参数共用属性行和悬浮说明的呈现契约；World 仍是场景持有的设置值，通过完整候选值验证与 Session 撤销提交，不为显示参数创建 Entity。视口诊断保持独立生命周期，不进入 Scene 数据。

**Coverage → Fit Scene** 按当前可渲染几何的世界边界建立固定覆盖；**Scene + Follow Focus** 保留粗层场景覆盖，并用高优先级体积跟随观察目标。两者保留传播与诊断值，按一次完整 World 修改进入场景历史；重复应用不产生新历史。打开工程或场景、设置 `--scene-lit-camera` 均不自动拟合 DDGI；覆盖拟合须显式调用上述操作。场景读取自身配置，已保存的体积及其更新预算原样保留，没有 Environment 时使用默认值，不继承上一场景。数值输入和拖动只更新字段草稿，提交后才通过 Session 发布 World 配置与渲染快照；取消、关闭页面和替换文档会结束未提交编辑。无效配置整体拒绝，规则与 Renderer / Control 共用。

**Current layout (CPU)** 显示最近接受帧的实际世界边界、间距、数量、驻留槽数及本帧调度/重置数。这些数值描述 CPU 布局和调度，不表示 GPU 已完成更新或有效光照支持数。切换文档清除旧读数；具体容量、传播和诊断语义以 [Scene Lit](../renderer/scene-lit.md) 为准。

覆盖预设根据场景几何边界和相机观察目标确定范围；楼层高度、薄墙和空腔会影响有效支持。多层室内场景可在现有槽位中配置局部楼层体积，用 Receiver Coverage 的 Status / Volume 检查实际贡献。观察时应等待多个完整更新轮次，分别判断覆盖范围、遮挡、有效支持和照明强度。

Create 菜单与工具架沿同一命令创建 Point、Rectangle 和 Directional Light。天光不是灯，而是 World Settings 的 Lighting 页中 Sky 组（开关、Intensity (cd/m²) 和 RGB 颜色），写在 Environment 组件上。Parameters 来自真实 LightComponent.settings 反射字段：颜色为 RGB 数值行，强度标注 cd、cd/m² 或 lux；局部灯显示米制范围，矩形灯显示宽高及双面选项，方向灯显示弧度制角半径。材质 roughness/metallic 锁定在 `[0,1]`，emissive 为非负 HDR RGB（常用范围 0–10，硬上限 65504）。编辑、取消、撤销、组件移除与恢复均沿普通 Session 入口；场景编码见[项目场景](../world/project-scenes.md)。

`application::SceneSnapshot` 在一个地方把 World 的 mesh/light 值快照转换为 Renderer 数据。完整 ObjectId 仅用作映射键，Renderer 使用该场景内单调分配的非零身份；移出投影的条目释放映射，重新出现时分配新键。替换文档重置映射并改变 scene_identity；快照不另记内容修订号，内容变化由 Renderer 逐帧比较得知。显式聚焦、重置、切换视图及正交/透视转换提交 camera_cut，直到 Renderer 接受帧才清除。没有独立的灯生命周期或另一份可编辑场景。

`SceneSnapshot::refresh` 跟随 [presentation 的提取缓存](../presentation/README.md#提取缓存)：先调用 `Scene::update()`，版本号和选择都与上一次相同时保留整份快照，所以只转相机或只做 UI 操作的帧不重建任何条目。否则只重建变化的部分：网格槽位不变、只是内容变化时原位改写对应的 MeshInstance，保留它的稳定键和选中状态；网格布局变化、或有槽位开始或停止绘制时，按缓存重排全部网格并重新计算选中；灯光变化时重建灯列表；只有选择变化时只重新计算选中。`built` 累计实际重建的 MeshInstance 与灯数量。替换文档时调用 `reset()`，它改变 scene_identity 并清空映射，下一次 refresh 完整重建；没有 reset 就用同一份快照刷新另一个文档时，refresh 按 `Scene::identity()` 发现并返回错误。

光照及快照的当前合同见 [Scene Lit](../renderer/scene-lit.md)，渲染特性的验收代表见 [Renderer 验收](../renderer/acceptance.md)。

### 帧性能

**World → Performance** 用一张 Table 展示 CPU 与 GPU 帧性能：先 CPU 段，后 GPU 段，列为 Current / Mean / P95 / Max（毫秒）与比例。列宽可拖动，行可选择和滚动；层级顺序固定，子阶段缩进，不按格式化字符串排序。顶部给出 CPU 与 GPU 样本数，并区分实际 Target 纹理尺寸与 Scene View 范围；底部显示直接光/AA/DDGI 配置与 Scene Lit 当前逻辑资源大小，不表示物理显存驻留量。两段由同一个 `editor::FramePerformance` 统计，窗口各最多 120 个样本，表格、Control 与 Play 报告都读取它约 4 Hz 发布的同一不可变快照，不各自持有历史。

编辑器宿主启用 [DXGI 呈现准入](../rhi/contract.md#呈现准入)，宿主 VSync=1、原生弹窗 VSync=0 及三个资源帧槽保持不变；弹窗、Play 和其他默认 RHI 消费者不启用此策略。可绘制宿主在每一步外层安全点最多等待 20 ms，输入可以唤醒，但等待本身不分发消息或重入业务。连续鼠标移动已占用本轮消息泵 2 ms 时，宿主把队列中剩余消息留给下一轮按序处理。每次唤醒都处理输入、Control、后台服务和 GC。主窗口取得呈现许可后才描述工作区、准备 UI 并提取宿主场景。已显示的原生菜单在权威文档、视图、选区与控制状态均未变化时，可在服务轮沿用已接受的锚点和菜单模型独立刷新；首次打开、切换原生菜单，或发生连续勾选（`keep_menu_open`）及后台状态变更时，仍会重新投影并验证其命令模型；输入唤醒不能冒充许可，accepted 几何也不会被服务轮替换。隐藏/最小化不叠加这次等待，沿原有服务与有限等待路径；许可保留不妨碍已有的准备中回退与恢复。

CPU 段：

- 样本是宿主每一步的[帧阶段计时](../../foundation.md#帧阶段计时)记录，编辑器为一次 `Application::step`，Play 为一次运行循环。只有接受了画面且没有 Capture 的步进入窗口；Capture 读回、失败、跳过及无宿主提交的服务轮不计入，但每一步的结构错误都累计，状态行和 Control 报告其数量与第一个出错的阶段。此窗口不代表所有服务轮的 CPU 成本；输入延迟专项须读取全部原始 Recorder 轮，不能只读取接受帧快照。
- 行按阶段目录深度优先排列：`CPU frame`（整步）之下是宿主阶段 `app.admission`（呈现准入）、`app.input`（输入与窗口事件）、`app.project`（工程、Play 与代码轮询）、`app.control`（Control；Play 中为变更链路）、`app.gc`（GC 步）、`app.ui`（UI 描述）、`app.layout`（UI 布局）、`app.popup`（弹出菜单）、`app.scene`（场景快照，子阶段 `app.scene.extract` 为呈现提取），以及 World 的 `world.*`、物理的 `physics.*` 和渲染的 `render.*` 阶段，最后是 `Unattributed`（整步减去直接子阶段之和）。阶段是包含式的，同一步多次进入时累加，父子不能相加；比例以 CPU frame 均值为分母。
- 一行从本域第一次进入起保留，直到清空；未进入的步 Current 显示“—”，行不会随偶发阶段出现又消失。编辑器不运行 World，World 与物理阶段只在 Play 报告中有值。行 key 固定：CPU frame 为 1000，阶段为 1000 加目录下标，Unattributed 为 1064。
- 弹出菜单的独立表面在 `timing::Suspend` 内渲染，整体计入打开或更新它的宿主阶段。
- 下文 GPU 域变化或文档替换时，CPU 窗口一并清空。

GPU 段：

- 只接收已完成且有效的 GPU 样本，按 sequence 去重。统计窗口最多 120 个样本，表格与 Control 共用约 4 Hz 发布的不可变快照，不发起 GPU 等待或同步读回。
- 场景身份、RT/视口范围或显示配置变化后清空窗口，并用当前域第一接受帧 ID 拒绝迟到的旧域样本。普通相机运动和选择变化继续统计；文档替换清空历史。此边界依赖正式 Application 单调分配的 UI 帧身份，不扩展成对任意低层 Frame 调用方的承诺。
- 非 Scene Lit、视口无有效几何或尚无有效样本时显示 Unavailable / Waiting 和空值。mask 未启用的 scope 显示 Skipped；mask 表示进入计时范围，不保证内部存在非零工作。整个窗口均未启用的项不显示伪造的零成本。
- Mean / P95 使用该窗口中同源的完成帧区间；p95 取最近秩。Measured total 对每个样本先累加八个主阶段，再计算统计量，不重复累加子阶段，也不累加各行 p95。子阶段比重也以主阶段合计为分母。
- GPU 段共 16 行；Direct lighting 下展开 Light list construction、Shadow atlas updates 与 Shading / HDR composition，DDGI 下展开 update/gather 及 update 的主射线与命中照明。原始 GPU 掩码使用 bit 0～14，其中 12～14 依次是灯列表、阴影 atlas 更新和着色；阴影 atlas 只在本帧有面重绘时置位。表格另以 bit 15 表示计算出的合计，子项不重复加进合计。数值与质量归因统一见 [Renderer 验收记录](../renderer/acceptance.md)。
- 合计不含 UI、参考辅助线、探针显示叠层、Capture 或 CPU Present；父阶段可能包含传输、调度及计时边界开销，子阶段之和不保证等于父阶段。分组与原始计时范围见 [Scene Lit](../renderer/scene-lit.md)。

统计与表格状态不写 Scene、不产生撤销项。`editor.inspect` 的 full / summary 都返回相同 `performance` 快照，概要检查不需要遍历对象。

### 真实工程的原生 1440p 渲染对照

直接光只有一条路径（灯列表、阴影 atlas 与融合 compose）。下面原生性能入口的 `clustered` 与 `clustered_orbit` 沿用原 case 名；2026-10-22 之前同名 case 测的是 RT 可见性的 A 路径，旧 `legacy` case 测的是已删除的 ReSTIR，前后对比时必须同时核对报告中的构建版本。

`atelier_editor --project <工程目录> --profile-render-performance <绝对输出目录>` 是测量入口，不属于验收，也不在总 `acceptance` 目标中：它只输出数值报告，其中的耗时与帧率不作为任何通过条件，入口内的检查只拒绝无效样本和对作者状态的改动。它使用该工程的启动 Scene；也可组合已有的 `--scene <相对场景路径> --scene-lit-camera eye-x eye-y eye-z target-x target-y target-z`。它运行同一个产品 Application、Scene 和 Renderer，不生成替代场景；此测量模式不会因 camera 参数自动拟合或改写 DDGI Volume。项目必须完整可加载，禁止进入修复后继续的流程。

在首次产品帧与任何视图修改之前，入口锁定已加载 Scene 的序列化内容、World 作者值、undo/redo、revision、dirty 和源文件哈希；入口要求刚加载的干净文档。首个真实接受帧同步 World 后才读取原始 view。后续仅允许改变视口诊断项，候选作者值与基线不同会在写入前拒绝。`authoring_baseline` 记录经过正常加载器解析、再序列化的 `loaded_environment`（Environment 组件的属性，没有 Environment 时为 null）；每帧检查作者状态，准备完成和每个 case 后再检查完整场景与文件哈希。失败恢复不会覆盖已经变化的作者值。

普通启动和测量中的 `--scene-lit-camera` 都只设置视口，不自动拟合 DDGI。交互诊断也可在打开工程和 Scene 后通过 Control `camera.look_at` 设置相机；这些相机入口均保留场景的 World 配置。

测量先调用既有 Scene pane maximize，再调整原生客户区。实际 Scene View 范围通过 Renderer 共用的 `raster_viewport` 计算：只在像素中心覆盖恰为 **2560×1440** 时继续。JSON 同时记录含小数的物理逻辑 rect、DIP rect、像素范围、DPI 和完整 Frame.extent；完整窗口 RT 仍包含编辑器 chrome，不能把它与视口像素数混为一谈。无法精确校准时失败，不接受近似尺寸。

正常停靠布局可能需要超出桌面的客户区。仅此测量的校准使用 `SWP_NOSENDCHANGING` 避免 Windows 默认拖动尺寸上限截断，仍沿原生 `WM_SIZE` 和产品 resize 路径更新；产品窗口规则不变。报告的 `calibration` 按布局保存每步原窗口/客户区、实际 RT、DPI、viewport 像素误差、请求与实际调整尺寸及 Win32 错误。尺寸被截断、重复状态、超出 16384 像素窗口上限或耗尽校准步数均明确失败，不改变 Dock 比例或降低采样分辨率。

完成冷资源准备后，独立采集接收面灯数 histogram。clustered case 关闭该诊断，保留同一相机、Scene 和 World 设置；额外 warmup 192 个接受帧，再收集 240 个按 sequence 去重、且来自 warmup 之后的完成 GPU 样本。之后追加 clustered_orbit：配置与 clustered 相同，预热和采样期间每个接受帧把 yaw 改变 .2°，在原相机 ±12° 内三角波往返，没有镜头切换；case 的 `camera_motion` 记录这些参数，结束后恢复原相机。它测量相机持续运动导致缓存失效时的成本，静止 case 的结果不能代表这种情况。随后使用同一场景、相机与原生视口测量最大化布局的 Smooth、MatCap，各 warmup 192 帧并记录 240 个真实接受步。最后通过 Dock 原有恢复入口解除最大化，保留原树和分隔比例，重新将实际 viewport 校准为 2560×1440，追加 docked_smooth、docked_matcap 两组；相机状态和 DPI 不变。非 Lit 的 gpu_passes 和 measured_main_total_ms 明确为 null，不读取上一组的 Scene Lit GPU 样本。统计含八个主区间、全部直接光/DDGI 子区间、cluster 计数、资源，以及接受帧呈现间隔的 P50/P95；`probe_rays`、`shadow_rays`、`ddgi_visibility_rays`、`reflection_work_items`（SSSR 分类出、送去层级追踪的像素数）与耗时共用同一完成帧。CPU 观察帧和完成 GPU 帧分开标记。截图和 JSON 写盘在计时区间外。

输出为 `render-performance.json`（schema v7）和六组同名 PNG：clustered、clustered_orbit、smooth、matcap、docked_smooth、docked_matcap。每个 case 明确记录 layout（maximized / docked）；layouts 目录记录各布局的实际 viewport、完整 RT、相机矩阵与活动 Dock 页。根级 viewport/camera 保留最初最大化布局，布局对照须读取 case 自身数据。正常布局可能需要更大的客户区；校准只接受像素覆盖精确相等，不以完整窗口尺寸代替 viewport。仅此测量入口在 Application 创建主 Renderer 时关闭 VSync，正常编辑器保持默认开启；报告读取本次实际配置。默认 Release 不启用 GPU validation。测量读取 Application 每一步自身的[帧阶段计时](../../foundation.md#帧阶段计时)记录（`Application::frame_record()`），呈现间隔取相邻两个接受步的帧记录结束时刻之差，包含被跳过的步进、等待以及步与步之间的验收工作；GPU 时间倒数不代表实际 FPS。结束恢复原 view，禁止保存 Scene。`--gpu-validation` 可用于诊断，其结果须与正常性能数据区分。

每个 case 的 cpu.samples 记录接受帧身份及该步的帧计时序号 `timing_sequence`；cpu.stages 按目录顺序给出该 case 中进入过的每个阶段的分布：`frame` 为整步，`app.*` 为宿主阶段（`app.scene` 含 `app.scene.extract`），`render.*` 为渲染阶段（prepare/encode/frame，frame 下为 submit/present/wait/recycle），`frame.unattributed` 为整步减去直接子阶段之和。父子阶段不能相加；帧计时序号不能和异步完成的 GPU 样本身份混用。accepted_steps_per_second 来自接受步数与帧记录覆盖的墙钟时间。

### 不创建 GPU 的场景 CPU 定位

`atelier_editor_document_acceptance --profile-scene-cpu <工程目录> <相对Scene路径> <新建绝对输出.json> [render-performance.json]` 通过正式 Project/Session 加载同一工程，分别测量：World 几何/灯光提取（`render_items`、`light_items`）、材质 Surface；SceneSnapshot 的静止帧（`scene_snapshot`，不重建条目）与变化帧（`scene_snapshot_changed` 与 `scene_snapshot_changed_50`，每个样本经 Runtime 写入交替移动 1 个或 50 个叶网格 Entity，计时包含这些写入）；成对还原的 World 编辑（`enabled_toggle_pair`、`detach_restore_leaf`）与逐 Entity 世界矩阵（`world_matrix_each_entity`）；Play 的 `play_start`、`play_update`、`play_stop`；两种布局的 Workspace 描述与 DSL 提交（`workspace_maximized`、`workspace_docked`），以及无头 Session 上停靠布局的描述、布局与接受（`ui_docked`）。`play_start`/`play_stop` 各测一次，其余每项预热 20 次、采样 121 次；每个样本是一个帧阶段计时 Recorder 帧，包含临时结果析构；进程安装与产品相同的阶段目录，SceneSnapshot 的样本像宿主一样在 `app.scene` 下调用 refresh，其中的 `app.scene.extract` 因此不算结构错误。`play_update` 的每个样本都检查 World 与物理阶段的结构和进入次数，报告其中各 `world.*`、`physics.*` 阶段的中位数与 P95；与 Play 进程一样，Play 在从同一源文件另行加载的副本上运行，含动态刚体的场景也不改变被测文档。报告（schema `atelier.editor.scene-cpu.v4`）的 `timing_scope_ns` 记录同机标定的单个计时作用域开销。结束检查序列化内容、源文件与文档历史不变。

`atelier_editor_document_acceptance --profile-physics-cpu <工程目录> <相对Scene路径> <新建绝对输出.json>` 在 Play 副本上以 1/60 s 的帧逐帧推进（每帧一个固定步），直到连续 120 帧没有活动刚体或满 1800 帧，按帧结束时是否仍有活动刚体把帧分为 active / sleeping 两组，分别报告 `physics.step`、`physics.reconcile`、`physics.solve`、`physics.write_back` 和每帧更新之后在 `app.scene` 下调用的 `SceneSnapshot::refresh`（`snapshot.refresh`）的中位数与 P95；另报 Play 启动耗时 `start_ms`（单独一帧），以及前 5 帧各自的 step 与 solve（`first_frames`），开局建树这类只出现在前几帧的开销在中位数里看不到。进程安装与产品相同的阶段目录，所有耗时都取自帧阶段计时 Recorder，每帧都要求没有结构错误。它只是测量入口，数值不作为通过条件。

报告同时保存实例/唯一网格/相邻几何批次数、实例三角形总量，以及按 ProgramSurface 输入的 Image 身份和颜色空间计数的纹理数；空输入使用类型化值，只有整张图片表为空时才保留一个 sentinel 槽，不额外添加默认白图。可选参数复用已有性能报告的实际相机矩阵和 viewport，调用 Renderer 相同的保守视锥判定，仅输出可见实例、连续几何段和三角形数。当前帧的 `scene_instances` / `scene_culled_instances` 也沿既有原生性能 JSON CPU rows 与 Control `last_frame.frame_stats` 输出。

此 CPU 定位不创建窗口或图形设备，也不提供接受 UI 帧；因此不包含 UI prepare、实际裁剪、Present 或完整帧耗时。尤其正常停靠布局的数值只能用于相同测量条件的前后比较，不能转换为产品 FPS。

### 可恢复渲染错误

主 Renderer 在编码前返回可恢复错误时，Application 以 20 ms 起步、最多 250 ms 的退避间隔重试。门控位于事件、Project/Control 与 GC 服务之后，控件描述/场景提取/渲染之前；输入唤醒不会绕过重试时刻。每次消息等待最长 20 ms，关闭请求仍走正常退出链。成功接受帧或切换文档清除退避；终止性设备错误仍退出。正常帧没有增加帧率上限。

## 工作区与停靠

主窗体只组织四个区域：顶部菜单栏、工具架、中间 DockSpace 内容区和底部状态栏。业务功能以可停靠的 Tab 子页组成，不将页面业务提升为固定外壳控件。

默认布局参照 Houdini 的分区：Scene View/Code 在左上，Asset Browser/Project Output/Geometry Spreadsheet 在左下，Parameters/Controls/World 共用右上标签组，Scene Tree/Entity Structure/Network 在右下。各视图只保留一份业务状态，可以移到其他标签组或分割区域。

### 程序化 SOP 视口上下文、Network 与 Geometry Spreadsheet

Network 在 CPU 上保留视口及周边连线的曲线采样点。同一拓扑和缩放级别下，平移直接从采样时的原点投影到当前画布并裁剪。视口移出缓存区域、缩放或更换拓扑会重新采样。拖动节点或包含节点的 Network Box 时，连线跟随实时端口位置；选中、错误和剪线高亮使用当前帧状态。

在 Asset Browser 打开 `Content/**/*.procgraph`（或通过 Network 面板 / Control 新建临时图 `/scratch/procedural`）后，Scene View 按 Houdini `/obj` ↔ SOP 上下文方式切换：

- **路径栏、视口隔离与程序化可视化栏**：Scene View 与 Parameters 的路径栏从 `World` 切换为图路径（如 `/Content/Graphs/Terrain.procgraph`），下拉菜单可返回 `World` 或在已打开的图文档间切换。3D 视口隔离渲染当前图的 **Display Flag**（蓝色 `D` 标记，`settings.display`）输出，并支持同时叠加 **Template Flag**（紫红色 `T` 标记，`settings.template`）的半透明参考线框；纯点云输出在无表面与实例时自动显示点标记（`Auto Points`），视口左下角常驻 3D XYZ 相机坐标轴指示器（Orientation Gnomon），Studio 模式默认启用背面绕序洋红染色（`Backface Winding Tint`）辅助排查顺逆时针绕序问题。视口右侧竖向程序化显示栏（全部控件采用“英文标题 + 中文说明”悬停提示）提供 **Display Points**、**Display Point Normals (`N`)**、**Display Primitive Normals**、**Display Orient & pscale**、**Display Bounds & Chunks** 与 **Geometry Info HUD (`i`)** 六项可视化开关；在 `World` 上下文中选中 `ProceduralGeneratorEntity` 时同样联动该显示栏与 Geometry Spreadsheet。
- **选中节点与 Display / Template / Bypass / Freeze 标记**：**Selected Node**（金色外框，Workspace 视图状态）驱动 Parameters 与默认 `Follow Selected` 的 Geometry Spreadsheet，左键直接拖拽未选中节点仅移动其坐标而不改变当前选中激活目标（左键单击不拖动才切换选择）；**Display (`D`)**、**Template (`T`)** 与 **Bypass (`B`)** 持久化于 `.procgraph` v2 文档，**Freeze**（冰蓝雪花标记）在当前编辑会话中锁定节点已求值的 `Geometry` 快照并支持单步撤销/重做（持久磁盘冻结格式另见程序化路线图）。
- **紧凑型 Houdini 式二维 Network 连线画布与 Geometry Spreadsheet**：Network 面板顶栏对齐 Scene View 的空气态层级：首行提供路径下拉与空气态 `Pin` / `Isolate` 开关（World 下点击 `ProceduralGeneratorEntity` 即自动跟随呈现并可就地编辑其 `.procgraph`），次行左侧提供与视口同构的 `Add / Edit / View / Layout` 空气态菜单（支持展开后横向悬停切换），右侧提供无外框空气态矢量图标工具（`ghost-tool` / `ghost-button`，含 `Color Palette [C]` 与 `Shape Palette [Z]` 常驻浮动调色/造型入口）。画布采用硬件抗锯齿解析 SDF 绘制 `88 × 28` DIP 紧凑节点主体、居中语义图标、上下外置脱离式端口圆点（支持宽范围磁吸靠近高亮与连线吸附）、精细三段式连线（首尾圆滑贝塞尔弯折 + 中段水平/垂直轴对齐直线）、右侧双行外部标签（节点名 + 类型/耗时/备注/冻结状态）与 Display/Template 同心背景光环；节点内部保留贴合各造型侧边斜率的纵向状态条（左 `Bypass`、冻结时左内冰蓝条、右内 `Template`、右外 `Display`），鼠标悬停时在节点包络外圈展开互不重叠的 100% 不透明扇形空气环（Air-Ring：`r = 54..72` DIP，对齐 Houdini 排布提供左上黄箭头 `Bypass`、左中白 `i` 明细、左下冰蓝雪花 `Freeze`、右上蓝眼 `Display`、右下紫 T 标 `Template` 五个矢量图标扇区）。画布支持 8 种节点形状预设（`Z` 键或顶栏/菜单切换，浮窗内支持 `Shapes [Z]` ⇄ `Colors [C]` 一键互切）、24 色校准色板（`C` 键）、节点信息与布尔诊断/备注浮窗（`I` 键）、分类 + 驼峰首字母缩写的 `Tab` / `Shift+A` 快速创建（如 `ctp` → `CopyToPoints`）、`Shift+D` 交互复制放置（合为单步撤销）、拖未连线节点到连线上自动串接（Auto-Splice）、`Y` 划断连线、**Sticky Notes 便签**（`N` 键，支持缩放与双击多行编辑）及 **Network Boxes 分组框**（`O` 键，拖动标题栏将框内节点与便签一步整组移动）。Geometry Spreadsheet 页按 `Points | Vertices | Primitives | Detail | Instances` 五层实时展示元素计数，支持空气态 `Follow Selected`、列名过滤与 512 行零拷贝分页切片。
- **参数、独立撤销与保存路由**：`PropertyRow` 支持 `String` 字段单行文本提交（供 `MeshImport.mesh`、`Noise.attribute`、`GroupBox.group` 等节点参数编辑）及既有数值、向量、布尔、枚举参数。每个已打开图在 `Session` 内拥有独立的 128 步增量历史（只记录作者增删改与节点 `ObjectRoot`，绝不把重算 `Geometry` 缓冲塞入撤销栈；撤销回旧参数直接命中 `Evaluator` 缓存），SOP 上下文下 `Ctrl+Z` / `Ctrl+Y` 与 `Ctrl+S` 自动路由到活动图文档，不污染场景 `World` 的 dirty 标记。

固定视图标签统一使用正体，包括 Scene View；选中状态仅通过灰色底面区分，不使用斜体或焦点描边。标签只绘制右侧分隔边，首标签直接衔接分隔条的收边，不叠加左边框。

- 拖标签到另一个标签栏可合并/重排；拖到内容的左、右、上、下边缘可分栏，松开前有预览，Esc 取消。分隔条拖动只移动预览线，两侧面板和 viewport 保留已接受的尺寸，松手一次提交比例，避免连续重建视口渲染目标。已有 UI 指针手势始终由原控件处理，跨入视口时也不交给视口悬停处理。Esc、失焦和丢失捕获取消预览；已取消手势的迟到松键不提交。两侧箭头收起对应面板为 8 DIP 细条，重按恢复原比例；Tab 栏直接从标签开始。中央单排方点单击交换两侧，Alt+单击切换横竖方向，拖动仍调整比例。
- 分隔条的三个按钮沿条方向各占 36 DIP，连续居中排列，窄小宿主内等比例缩短。背景为 `#494949`，靠前边为黑色、靠后边为 `#333333`；横竖方向共用 SS 边框规则，悬停和按下反馈限制在按钮内部，保持外侧细边连续。
- “＋”紧跟最后一个标签，打开原生菜单，将已有视图移到当前组，或重新打开已关闭的视图；视图的值和历史保留。右端下拉菜单的 Show 则跳转到视图当前所在组。
- 标签的关闭按钮移除其显示；右侧菜单可以重新打开已注册视图，并把当前标签移到右方/下方的新区域。空间不足或组中只有一个标签时，对应分割菜单不可用。
- 面板右上角切换最大化/还原；Ctrl+B 对最近操作的面板执行同一动作。Ctrl+Tab / Ctrl+Shift+Tab 切换组内标签，Ctrl+W 关闭当前标签（Code 中先关闭源码文档）；菜单打开期间由菜单独占键盘。
- **Window → Reset layout** 恢复默认位置及标签；场景、相机、筛选、样本值和历史不因恢复布局而重置。最后一个视图关闭后也可恢复。
- 样本与场景历史按照最近操作的视图选择，允许 Controls 与 Parameters 同时显示。视图隐藏/移动时，未提交交互仍通过 Session 的取消/失焦合同结束；业务字段 key 依对象实际成员生命周期退役，隐藏面板不退役对象绑定。
- Scene View 隐藏时提交空场景矩形，不生成场景实例；其他 UI 继续走原窗口、renderer 和接受帧。参数面板标题与可选搜索固定，实验台另外保留示例内容页签；下方内容滚动以适应较矮的区域；窄参数行仍使用公共 Flex 换行。

以 Houdini UI Scale 1.75 为同机参照：数值字段最低 23 DIP、标签列 160 DIP、行间 7 DIP。菜单栏为 32 DIP，Tab 栏为 22 DIP；菜单栏与工具架的收起把手和整行恢复条尺寸见下方[首版操作](#首版操作)。当前中英文正文与 Tab 使用 12.5 DIP Sarasa UI SC，菜单栏文字为 13.5 DIP。Tab 选中后使用灰色底面，键盘焦点不额外画框；文字字段保留输入焦点提示。F10/Alt 菜单快捷键会展开收起的菜单栏；恢复布局一并展开菜单栏和工具架。系统 DPI 仍只有 Window → Surface 的一个换算入口。外部观察见[工作区研究](../../research/houdini-workspace-reference-20261001.md)，当前行为以本页为准。独立原生浮窗、多份同类视图和持久布局尚未实现。

布局与参数的 SideFX 来源及固定版本观察见 [Pane 细节](../../research/houdini-pane-details-20261001.md)，不作为本项目的验收结果。

## 控件实验台

**Controls** 标签打开实验台；选择 **Parameters** 回到实际场景参数，也可将两者分栏同时显示。实验台使用同一个 `ui::Session`、SS、布局、输入法和 renderer，没有独立绘制循环。

- 可编辑 Position Vector3、Exposure、Unicode 名称、Enabled 勾选、Vector4 和 Projection 下拉样本。Position / Vector4 使用显式轴提示色线；Exposure 同时提供数值框和滑条，合法数值为 0–32、滑条显示 0–16，支持拖动、键盘细调、撤销及取消。数值与下拉共用右对齐标签列，名称位于固定的 Sample 标题行；数值字段从 23 DIP 起，共用黑色输入框、凹入边缘、轻渐变按钮；默认灰白文字、非默认数值加粗、灰色编辑边界和菜单定位；勾选使用勾号，折叠使用三角。参数勾选项与折叠标题没有悬停或按下底色。参数下拉按钮高 26.667 DIP（150% 缩放下约 40 px），文字两侧保留 14 / 12 DIP，尾部使用单向下箭头。普通命令菜单不绘制勾选或互斥圆环，也不预留空的标记列；只有布尔项和单选项使用对应标记。
- 右键菜单提供分量/整组默认值、复制和粘贴；Ctrl+中键直接恢复默认值，中键按住调出数值阶梯。整组恢复仍只记一次撤销，全部样本保留统一 Reset。Projection 和 Vector4 均为独立样本，不向 World 添加虚构字段。
- Disable sample fields 检查禁用传播，Show long labels 检查换行，Transform and display 与 Color components 标题支持鼠标与 Enter/Space 折叠。
- 分隔条由视图最小尺寸和当前工作区空间共同约束。窄面板保持字段可达，超出可用高度的样本通过同一 list 滚动。
- 样本最多保留 64 次提交历史；UI 活动草稿仍优先使用共享文本历史。实验台打开时，Edit 菜单 Undo/Redo 与无本地事务的 Ctrl+Z/Y 进入样本历史，Delete 不穿透到场景删除。样本值不写入 World。

原生菜单的统一开启入口先取消主 UI 当前抓取手势，经原 effects 链回滚业务编辑后再转移鼠标捕获。

## 首版操作

顶部菜单栏提供 File / Edit / Create / Project / View / Window / Help。Create 快捷区创建 Entity 或 Static Mesh Entity，View 快捷区聚焦选择和重置相机；Window 打开视图、切换/关闭页签、最大化和恢复布局。无参数启动为空文档；[Examples](../../../examples/README.md) 是通过 File → Open Project 或 `--project` 打开的独立工程，没有专用菜单或隐式复制。Parameters 通过真实类型显示检查目标及其连续属性区。Entity Structure 递归展示 Entity 子树与所属组件，负责选择、添加和删除；Scene Tree 递归展示 Entity 组合层级。没有空间时不显示实体 TRS；Edit 和组成菜单的 Add Transform / Remove Transform 操作同一内联空间；没有检查目标时显示空状态。

`OperatorRegistry` 是编辑器动作的权威入口，poll 判定当前可用性，`call` 严格解码参数并执行。菜单和顶部快捷区读取 `menu_bindings.hpp` 的 `OperatorCall`；非 modal 快捷键从 keymap 解析同一调用，菜单快捷键文字也来自 keymap。执行时再次检查可用性；UI 只产生通用 Action，不直接修改 World。输入字段和拖动仍通过 Session 事务入口，详见[操作符](operators.md)。

菜单栏通过 `EditorShell`、工具架通过 `ToolShelf` DSL 描述同一套收起交互，共用 `stow_*` 尺寸和 `bar-stow` 样式。展开把手宽 12 DIP，当前 150% 缩放下为 18px；箭头位于顶部 12 DIP 方形区。收起时同一个按钮铺满整条横栏，高 12 DIP，任何位置均可点击恢复，箭头保持左对齐。展开把手和收起横条的常态、悬停、焦点均为灰色渐变；仅按住时采用灰蓝渐变 `#5D7389 → #748CA5`，松开即恢复灰色，箭头始终为黑色。两栏分别使用自身实际占用高度分配剩余工作区。

顶部工具架使用 18 DIP 分类行与紧凑图标区，总高 56 DIP；收起后为上述整行恢复条。分类选择跨收起/展开保留，收起不会改变场景历史，Reset Layout 恢复展开。工具按钮统一宽 64 DIP、18 DIP 图标与 10.5 DIP 短标签，菜单保留完整指令名。图标和文字由同一按钮绘制/命中，`icon-placement: above` 由共享 SS 测量与绘制处理。

Asset Browser 是独立可停靠视图，浏览工程目录与引擎资源；单击选择，Add to Scene 执行共享创建命令，支持名称/类型过滤与 List/Details/Tiles。工具行、目录树、状态行和全部结果视图由 DSL 声明；List/Details/Tiles 通过 TableBinding 接入共享 Table，提供排序、列宽拖动、当前单元格、键盘导航和双击/Enter Open。三种呈现复用同一数据快照和选择状态，详见[工程与资源浏览](projects.md)。顶部快捷创建继续引用相同内置资源。World/renderer 不按 Cube 名字分支；拖放、吸附和模型缩略图尚未实施。
数值控件支持普通拖动和中键七档数值阶梯；上下移动选步长，左右移动调值，Shift 细调，释放提交一次、Esc 取消。右键或 Menu/Shift+F10 打开参数菜单；Ctrl+中键恢复分量或标题对应整组默认值。菜单期间视口输入暂停，外部点击只关闭菜单，Esc/Tab 返回原焦点。普通拖动用于调值，单击进入文字模式后拖动用于选字。首次实际文字修改才建立事务；合法 Enter 提交，非法 Enter 保留草稿，Esc 恢复整次编辑的原值。结束后保留字段焦点，数值退出文字模式。字段焦点下 Delete/Backspace 只编辑文字。
文本与数值共用 Unicode 字素边界、视觉光标、双向选区、横滚及本地撤销/重做。Ctrl+A/C/X/V 提供全选、复制、剪切、粘贴。存在本地事务时 Ctrl+Z/Y 只操作本地历史，即使历史耗尽也不穿透；仅聚焦无事务时进入场景历史，Ctrl+Shift+Z 同样重做。历史合组、容量和 Unicode 覆盖见 [UI 模块](../ui/README.md#共享单行编辑)。
Parameters 的 Component / Category 子页切换组织方式。`AFIELD(Label="Position (m)", Category="Transform")` 提供显示名与分类；`Category` 是标准拼写。内联记录的分类向下继承，子字段的非空分类可覆盖；向量分量沿用聚合字段的标签。无标签时按字段名生成标题，无分类时归入 General。Component 模式保留 Entity 内联 TRS 的整组编辑与默认值恢复；Category 模式按元数据汇集属性，字段仍绑定同一 ObjectId/FieldPath，分组不改变持久身份或场景历史。Source 生成的未声明元数据保持空值，本轮没有添加 Source 注解语法。

子区域使用共享 `section-tabs` / `section-tab`：26 DIP 页签、0.667 DIP 细边、未选中 #555555→#303030 渐变、选中 #4F4F4F 平面与 #3A3A3A 条带。Parameters 和 World 的第一个页签从条带左边缘起排，不预留空槽；停靠页签继续使用自己的 `dock-tab` 样式。

参数搜索按实际属性、类型名，以及位置/旋转/缩放行的名称与别名过滤。确认文字实时过滤，IME 预编辑不改变查询；Enter 或普通换焦点提交查询，Esc/OS 失焦/Surface 失效恢复原查询。查询属于 Workspace 查看状态，不写场景历史。相机与变换操作见上方[视口编辑](#视口编辑)。
调整窗口尺寸、DPI变化、失焦或外部捕获丢失都会取消当前未提交编辑。应用主动释放捕获产生的同步通知不会误取消刚进入的文本编辑。

### 独立菜单窗口

菜单栏、参数右键、Projection 下拉和 Pane 菜单全部通过同一个 PopupHost，使用独立 owned HWND，主窗口保留输入焦点。F10 或 Alt+F/E/C/P/V/W/H 打开顶部菜单；Scene Tree 非文本焦点下，Alt+P 优先解除父级。打开后左右键或悬停切换菜单，Right 优先展开当前可用分支；上下/Home/End 导航，Enter/Space 执行，Esc/Tab 关闭并返回原焦点。F10、面板最大化/关闭等一次性命令忽略自动重复，文本删除和历史导航保留重复。快捷键列与分隔线来自同一菜单模型。首项始终不绘制组间分隔线。菜单尺寸通过独立的 CPU 测量会话计算；勾选或可用性更新在尺寸不变时直接更新当前输入表面，保留悬停、焦点、滚动和行身份。窗口外点击只关闭，不执行背后的编辑器命令；视口和场景快捷键在菜单期间暂停。

菜单切换请求立即退役旧菜单的输入身份；新菜单首帧接受前只消费过渡输入，不能执行旧条目。普通命令先关闭旧菜单，再接管输入或捕获；checkbox 的 keep-open 路径仍保留当前菜单。

Popup 的文字、样式、显示器查询、定位/尺寸与 GPU 失败通过 Result 返回；GPU terminal 在转换诊断前关闭该宿主。hide 先隐藏原生窗口并停止输入，再收集取消和 suspend 的错误；reset 释放 JIT scope，close 在 HWND 析构前退休 GPU。主菜单/子菜单清理收齐首错并记录后续错误，不因首个失败跳过必要清理。prepare→render→accept 不允许重入修改，UI 拒绝已经被 GPU accepted 的帧是 contract-fatal，不能声称画面可回滚。

菜单生命周期沿统一进程日志的 `menu` 分类记录，不以逐帧日志代替业务状态：

| event_code | 级别与时机 | 字段 |
|---|---|---|
| `menu.requested` | info，采用菜单请求 | `source_control`、`group`、`frame`、`incarnation` |
| `menu.frame_accepted` | info，弹窗首帧接受 | `source_control`、`surface_revision`、`frame`、`present_attempts` |
| `menu.closed` | info，已有逻辑或可见菜单进入关闭路径；无菜单的重复关闭不记录 | `source_control`、`incarnation`、`restore_focus`、`caller_line`、`caller_function`；后两项来自直接调用方的 `std::source_location` |
| `menu.rejected` | 请求源失效或与活动编辑冲突时为 info；弹窗 update 后仍不可见时为 warning | 请求拒绝携带请求身份；更新拒绝携带 `source_control`、`incarnation`、原始 `diagnostic` |

关闭来源用于区分 `cancel_interaction`、菜单 dismiss、命令执行及其他路径；它是直接调用位置，不是原生事件轨迹，单凭 `cancel_interaction` 不能进一步区分失焦、捕获丢失或其他取消。`PopupHost::hide` 不重复记录关闭事件，日志不改变关闭、回滚或恢复焦点的顺序。

顶部菜单在未展开时不因 hover 改色；按下和展开的标题，以及展开菜单中当前指向的条目，共用 `#b36600` 橙色底面。菜单背景为 `#3a3a3a`，细框上/左为 `#949494`、下/右为 `#262626`。条目最低高 25⅓ DIP，快捷键为右对齐的灰色斜体，分隔线位于组间留白中央。PopupHost 用同一个 SS/文字测量入口计算标题与快捷键两列的宽度；短菜单不留空滚动槽，超出屏幕高度时预留实际滚动条空间。

菜单关闭后，标题恢复透明底面和普通文字颜色；保留的逻辑焦点不产生灰色 active 底面。标题的橙色状态仅由按下和实际展开驱动。

共享滚动范围计算抑制小数行高累计产生的浮点尾差，防止刚好容纳内容的菜单出现几乎不可滚动的滚动条；真实溢出仍保留。

弹出窗口基于锚点显示器的工作区定位，可超出编辑器边界；下拉空间不足时向上翻转，超宽/超高按可用空间限制并滚动。主窗口移动、尺寸/DPI 改变、失焦、系统取消或捕获丢失关闭菜单；关闭编辑器时先关闭弹出窗口的 GPU 资源，再销毁 HWND。它不创建第二套字段编辑或场景历史。

PopupHost 的测量 Session 也遵循 Surface 变化取消模态状态的规则。DPI／工作区失效时，先退役测量控件树，再设置新 Surface 并重新投影；保留实际弹窗 HWND、接受帧和输入身份。

当前 renderer/RHI 的所有权仍是一窗口一上下文，主菜单及首层子菜单宿主随应用初始化，其余层首次使用后复用，隐藏时不渲染，静止时不重新提交，主窗口负责帧节奏。跨窗口共享 GPU 设备和纹理缓存尚未实现；多显示器真实 DPI 迁移需要额外设备验证。级联子菜单复用保留的原生菜单窗口，详见下方[页签菜单与弹窗资源](#页签菜单与弹窗资源)。定位依据见 [Microsoft 多显示器指南](https://learn.microsoft.com/en-us/windows/win32/gdi/positioning-objects-on-multiple-display-monitors)，焦点与捕获遵循 [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture) 的前台窗口限制。

主窗口统一消费线程消息队列，每次分发后处理自身事件记录；弹出窗口只处理已由该消息泵或同步回调排入的自身记录和回调错误，业务消费者始终在 WndProc 返回后执行。

## UI 样式与诊断

### 参数面板组合

颜色色片打开同一 DSL 的 ColorEditor。RGB 与 HSV 的 V 沿用原属性范围，灯光与 emissive 可编辑 HDR；Hex 和色块展示按 0–1 范围表示，不能把它们当作 HDR 原值。弹窗内可键入、粘贴、按 Tab 切换字段，键盘导航不离开弹窗。颜色分组复用 Session 的 World 或材质事务：成功预览才更新显示，拒绝时还原所有通道与 override；Done、普通 Escape 或点击外部只提交一次历史，Revert 恢复打开时的颜色。失焦、外部捕获丢失、尺寸/DPI 改变和业务失败先回滚整组颜色，再消费字段与弹窗的清理通知；重复通知不提交历史。关闭确认和保存会先完成活动颜色事务；弹窗挂载失败取消事务，避免留下看不见的编辑。


参数说明沿同一展示链路进入界面：原生类型与字段的 `DESC` 经 Runtime 进入 `Session::Property`；World 使用已有 `ViewOptionInfo` 的 label/category/description。两种生产者都适配成 `PropertyRow`，经同一 DSL 参数行和原生 Tooltip 显示，不建立另一份帮助登记表。World 保留整组候选校验、场景保存与撤销；诊断项保持视口归属。

悬停参数标签、工具按钮或节点 180ms（在已展开 Tooltip 的相邻控件间滑移时 50ms 快速切换），可看到标题、完整字段身份及实际类型、中文/English 混合说明；向量共享聚合字段的说明，Rotation 明确区分 Quaternion 存储与 Euler 角度编辑。对象标题提供实际类型说明。提示只持有文字快照，切换对象或热更后旧提示失效，不改变 dirty、撤销或场景数据。`editor.inspect`、`material.inspect` 与 `capabilities.display_options` 返回同源描述，`AFUNCTION(DESC=...)` 保存到 Runtime 方法元数据，暂不自动创建函数调用按钮。

参数标签沿 Houdini 习惯由字段名生成：取最后一段，下划线分词并首字母大写（`material.base_color` → `Base Color`，项目源码字段 `moving` → `Moving`）；字段路径和绑定身份不变。颜色元组（基础色、自发光、灯光颜色）末尾带当前值色块，HDR 值仅在色块上截断到 `[0,1]`。组件分组与变换分组共用整宽折叠条。

控件宽度按 Houdini 取内容而非整行：枚举菜单按标签收缩（最小 96 DIP，选项显示首字母大写的名称），单个数值字段宽度为一个元组列（140 DIP，可在窄面板中缩小），勾选框只占其文字宽度；向量元组和带滑条的数值仍填满数值列。

类型图标来自与工具图标相同的 Tabler 线框集（[assets/icons](../../../assets/icons/README.md)），以 1.5 笔宽栅格化，尺寸按 Houdini 同屏比例收小（路径导航 20、路径图标 18、标签栏工具 11～13、树行 13、头部 16 DIP；组合框下拉箭头统一不超过 9 DIP）：点光 `bulb`、矩形光 `rectangle`、方向光 `sun`、天空光 `cloud`、网格 `box`、普通 Entity `cube-3d-sphere`、组件 `packages`，以及 World、文件夹、搜索。工具架、Scene Tree、Entity Structure 和参数标题使用同一判定：StaticMeshEntity 为网格，含灯光组件的 Entity 按灯光类型，其余为普通 Entity。Scene Tree 行只显示名称，类型由图标表达。状态栏左侧为当前消息，右侧以灰色显示场景计数，二者以间距而非竖线分隔，文字在 23 DIP 栏内垂直居中。

路径栏显示 World 上下文；固定对象标题用类型图标、类型和名称输入框表达当前 Entity，右侧放大镜控制参数搜索。名称是 editor::Session 的文档标签，通过对象身份索引，重命名、删除恢复和撤销重做共用场景历史；标签本身不持有运行时对象，GC 完成后清理失效索引。名称随场景 JSON 持久化，不是新增的 World 反射字段。

路径栏的下拉通过同一原生菜单选择 World 或 Entity；前后箭头浏览最近的选择。导航历史属于 Workspace，最多保存 64 个不保活的身份，保留 World 入口并清理失效对象；导航调用现有 Session::select，不增加场景撤销条目。路径框、普通输入框、内容页签和滚动条使用同一 [SS 样式](../ui/style-sheets.md)，当前色值由 [editor.ss](../../../assets/editor.ss)定义。

Parameters 展示所选对象的连续属性区，不预设 Transform／Components 内容页签。类型、名称、Object/Category 分组页签与搜索按钮同在一行标题内（Houdini 单行参数头），不再单独占一行。颜色元组末尾的色块可点击，打开颜色编辑器：新色/原色对比（点原色恢复）、RGB 与 HSV 两行数值编辑同一颜色、六位十六进制输入；从打开到 Done 或点外部关闭为一次 Session 组编辑（`begin_group_edit`），只记一条历史，Esc 同样保留当前值，撤销一次恢复三个通道。选择 Entity 时展示 Entity 自身字段和所拥有组件的属性分组；选择组件时只展示该组件，标题中的组件类型只读。Entity 名称仍可编辑。搜索默认收起，点击名称右侧的放大镜在标题下方展开，并在首个接受帧后聚焦；收起停止筛选、归还行高并保留查询文字。查询筛选实际字段与类型分组，不改变组成或撤销历史。结构增删只在 Entity Structure 提供。布局使用 UI DSL 的 PropertyGroup／PropertyRow，数值、勾选、枚举复用共享参数行；枚举只显示元数据中的命名选项，不接受任意整数。数值的合法范围取字段声明的硬边界（`HardMin`/`HardMax`，或带 `LockMin`/`LockMax` 的 `Min`/`Max`），没有时取类型范围；当前值已在范围外（可信原生写入）时放宽到包含它。键入或拖调的越界值夹到合法范围，与 Runtime 写入一致。声明了 UI 范围的浮点标量在数值框后显示滑条，端点为 UI 范围，单侧缺失时用硬边界；整数、布尔、枚举和向量分量不显示滑条。悬浮说明在字段描述后附范围，例如“范围 0 – 1，上下限锁定”“硬下限 0”。Position、旋转和缩放仍按上方应用参数描述。图标来源与许可见[图标说明](../../../assets/icons/README.md)。搜索入口参照 [SideFX Parameters](https://www.sidefx.com/docs/houdini/network/parms.html#searching-and-filtering-parameters)；收起保留查询是本编辑器的合同，不据此推断外部软件的隐藏状态。

Scene Tree 与 Asset Browser 支持就地重命名：树行 F2 或双击已选中行，资源 F2 或右键 Rename，在名称文字位置覆盖一个输入框（不遮图标），Enter 提交、Esc 或失焦取消；Entity 重命名进入场景历史，资源重命名通过 `Project::rename` 同时移动 `.asset.json` 身份文件，资源键与场景引用不变，拒绝非法字符、同名覆盖、顶层文件夹及配置中的启动场景/UI 源。Scene Tree 行可拖动：拖到另一行使其成为该 Entity 的子级，拖到 World 行或下方空白解除父级；目标行高亮，不能拖到自身或其后代。换父保持世界变换（keep world），一次拖放一条历史，完成后选中被移动的 Entity 并展开到它。右键行先选中该行，再弹出 Rename（F2，聚焦参数头名称）、Duplicate、Delete、Clear Parent（Alt+P）与 Focus；树获得焦点时 Delete、F2、Ctrl+D/Shift+D、Alt+P 生效。

Entity Structure 是可移动、关闭、重开的独立子页：Entity 递归显示组合子实体和有序组件。它表达真实组成归属；空间参考是独立关系，组件不再标记 Root。选择组件保留 Entity 上下文，视口和 Scene Tree 仍指向其拥有者；Delete 删除检查目标，删除组件后回到 Entity，Undo 恢复组件身份与选择。树行、属性分组和字段分别持有稳定 UI key，避免两个可见页对同一对象复用控件身份。Control 已提供组合换父与空间参考设置；当前没有对应的跨面板拖放手势。

### 类定义、实例与默认值

目前支持原生及 Source 类定义，实例通过同一个 Runtime 创建，字段声明默认值参与初始化。Scene 持久化限定类型名和实例值。当前没有 Class 资产、组件模板或显式 override 集，也没有模板默认值编辑器。选择场景对象时 Parameters 编辑当前实例；选择 Asset Browser 条目时显示资源类型、资源键、位置、文件大小及已知网格统计，资源不会冒充 Entity 或可编辑类模板。返回 Scene Tree、路径导航或视口操作后恢复实例检查。两类查看共享参数页和 DSL 控件身份分配入口，不增加场景历史。

现有值与默认值的比较仅用于显示修改状态。没有声明默认值的原生字段不假定为零，也不伪造恢复默认值能力。修改 Source 字段默认值需重建类型，已保存实例值会恢复，不自动随新默认值传播。未来 Class 资产必须先明确持久身份、组件模板、覆盖和传播规则，再复用同一个检查入口。

外部参照：[UE UClass](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass) 与 [Blueprint Class Defaults](https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprint-editor-defaults-tab)区分类型默认对象与实例；[SideFX Parameters](https://www.sidefx.com/docs/houdini/network/parms.html)和[参数界面](https://www.sidefx.com/docs/houdini/ref/windows/edit_parameter_interface.html)区分节点实例参数与类型作者界面。它们不表示本项目已提供 Class 资产，也不把 SOP 网络等同于 Entity 组成。

Controls 的 Parameters / Appearance 使用同一组合，Appearance 内的 Color / Channels 是独立嵌套组：RGB 元组、三条逐通道滑条和色块消费同一组样本值，支持折叠、禁用、默认值及样本撤销。所有行、组和伸展继续使用现有 row/column/Flex，未引入第二套布局器。

折叠标题以常规字重显示，鼠标悬停使用 `#4f4f4f` 灰底，按下使用 `#464646`；展开状态由箭头方向表示。焦点仅轻微提亮文字，不增加边框或黄色下划线，Enter/Space 仍可切换展开状态。

全局交互样式区分按下、选中与焦点：普通按钮和列表用灰色底面反馈，菜单栏展开标题及当前菜单条目用橙色底面反馈；普通控件获得焦点不增加外框。焦点通过文字或控件标记的亮度表达；文本和数值编辑器保留原宽度的细灰色边界，非法输入仍使用红色。鼠标点击保留键盘焦点，Tab、方向键、Enter/Space 和菜单返回继续使用同一焦点状态。

Tab 标签悬停不改变背景，统一使用正体。列表有带握柄和双步进按钮的滚动条，支持拖动、翻页和滚轮；字段不会伸到滚动条下方。数字调整、文本、分隔条、停靠拖动与视口观察使用对应鼠标指针。组合行为与来源见[UI 参数合同](../ui/README.md#可组合参数控件)；完整 Ramp、二维色盘、动画/表达式编辑仍未实现。

Workspace 声明 `normal/panel/title/caption` 角色、enabled/selected 等语义状态及菜单栏、参数区域等类名。基础主题和 [editor.ss](../../../assets/editor.ss) 通过同一 UI 解析得到最终样式；应用启动时从可执行文件旁读取 SS，非法内容报告行列。对象类型使用 object-type 样式，说明和状态文字使用 caption；容器结构 padding 留在 NodeInput，文字内容内距由最终样式提供。菜单栏标题宽度按文字测量并保留最小宽度。样式覆盖、图片/圆角和相邻合批合同见 [SS 文档](../ui/style-sheets.md)。
主窗口和原生菜单通过同一 `editor_fonts()` 加载 Sarasa UI SC，数字使用 Source Code Pro；额外缺字及 emoji 沿系统回退，应用不修改系统字体设置。构建部署 `fonts/` 和两份 SIL OFL 许可证；选择依据、字重、体积与来源见[字体说明](../../../assets/fonts/README.md)。窄面板继续同一 Flex 换行。
`Application::set_style` 直接进入 UI 的整体校验/应用路径，应用不保存另一份主题。颜色复用文字 Coverage，字体族、字重和字号经同一 TextStore 更新度量、caret 和栅格；已接受帧仍保有旧几何与不可变内容。具体样式、版本及状态合同见 [UI 模块](../ui/README.md)。

视口 locator 与拾取读取 Renderer 已接受帧的 SceneSnapshot CPU owner，不重新提取活 World；Workspace 仅在已有 UI 引用需要时补查材质成员。完整采用与快照寿命见[材质系统](../renderer/materials.md#gpu-批次与实际显示)。

## 帧与关停

应用在 renderer 的最终结果成功且确认 accepted 后才切换命中帧，skip 保留旧帧。
accepted extent 固定为录制/呈现时尺寸，frame末尾resize后的新尺寸仅用于下一帧。
尺寸/DPI过渡期取消press/capture并暂停指针命中，释放/失焦/捕获丢失继续处理；匹配帧接受后恢复。
上传也是录制；录制开始后的错误没有回滚，终态错误停止新交互与后续帧。普通帧不调用finish，诊断截图才显式同步。
关闭停止输入并取消编辑，释放renderer拥有的资源后关闭RHI，最后销毁窗口；成员/历史根先于Runtime析构。

`Application::step` 返回 `Result<bool>`：成功 false 是正常退出，unexpected 是失败。Window.poll 的回调只在该调用栈保存首错，之后不派发业务输入；帧失败先关闭菜单、报告原错并执行 preserve_failure，再由入口正常关闭。析构中的外部清理错误记录诊断，不能安全完成 World/Renderer 退休时由资源拥有者 fatal。作者源码错误仍可恢复旧定义；系统文字服务失败不当成坏源码重试。

Output 新记录正文的字符/长度转换后置条件破坏是 fatal，ICU 失败则返回错误并保留旧缓存，不把旧正文冒充新选择的成功结果。`Workspace::describe` 返回 Result，失败拒绝候选并结束 property-help 轮次；不发布半份控件。它本来会改绑定缓存，不保证整个 Workspace 回滚，因此错误继续传给 step 并结束运行。替换文档失败时仍拥有旧 document，不能提前销毁其 Runtime 或宣称新文档已采用。

本轮三族迁移不表示整个编辑器已满足 W5：Workspace::describe 内原有 take、DSL 拒绝及树路径留给 UI-BASE-B/WORKSPACE；Workspace/ProjectUI 注册、Application 其余业务/主 GPU 桥、Window 非初始化异常运输、Output::capture_raw 等仍待后续迁移。

### 后续调度与成本边界

空闲/后台节流与事件驱动停帧仍需设计，不是现有能力。方向是在唯一 Application 宿主入口协调服务、渲染需求和提交节奏，不在 RHI/Renderer 再建限帧循环。前台输入应尽快处理；渐进光照、反射、AA 或动画必须有明确持续需求，不猜固定帧数后收敛；最小化/隐藏停止 GPU 提交仍须推进服务。独立菜单与主窗口属于同一活跃组。

长时间等待之前须接好 Control/后台结果的就绪通知，以及源码检查、DSL 去抖、Tooltip 等拥有者的最近期限；通知只安排安全阶段，不能在编码、发布或 World 事务内任意派发业务消息。原生模态框须明确可响应的取消/只读边界，不重入文档写入。flip-model 的普通遮挡不能仅靠 `DXGI_STATUS_OCCLUDED` 判定，见 [Microsoft DXGI_STATUS](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-status)与 [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)。

通知句柄的拥有者须先停止等待者，再销毁通知句柄；等待尚未结束时关闭句柄的行为未定义，见 [Microsoft MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)。

Play 的 World Tick/协程按宿主调用推进，物理累积器不代表整个游戏已经固定步进。后台渲染预算不得改变游戏时间；现有 dt 截断和丢弃须按 [Physics 合同](../physics/README.md)核对。渲染时间沿 RenderClock，暂停后重建基线，普通等待不制造 camera_cut，也不重置所有 GI 历史。

性能改动先区分面板投影、DSL prepare/append、Session 全量差分与 UI 帧构造；等值快照/片段复用或稀疏提交必须仍观察直接 Runtime 写入、选择、展开、过滤、热更和 DPI。仅 Document revision 不能覆盖这些变化。Scene Tree 当前范围见下文；普通 List 的绘制裁剪不等于前段虚拟化，范围化须同时维护键盘、范围选择、过滤、scroll extent、anchor 和稳定身份，复用现有 VirtualList/Session，不另建输入后端。

### 未实现的交互

当前吸附和着色设置使用已有菜单；带分段按钮、图标列表和勾选组合的富弹出面板仍待建设，须复用 PopupHost 的焦点、Esc 和外部点击政策。Normal/View/Cursor/Parent 变换朝向、顶点/边/面吸附、视口独立可见性/可选性状态、长按工具组、Ramp、方向球和 HSV 二维色盘尚未实现；现有 RGB/HSV/Hex 数值颜色编辑不是二维色盘。

多份同类面板、固定检查目标、浮动视图和布局持久化沿现有 DockSpace 与稳定业务身份扩展，不复制场景状态。资源缩略图及跨区域拖放的责任见[声明式 UI 边界](declarative-ui.md#区域与手势边界)；源码调试接入复用[Runtime/JIT 核心](../../source-debugger.md)和[Control 方案](control.md#源码调试接入待实现)，不是另建源码执行宿主。SOP Network 二维节点连线画布已将节点选择（含多选）、作者图对象与持久化 Display 输出严格分开，并通过同一 `Session` 图事务维护单步撤销。

## 验收入口

编辑器的自动验收只保留下列入口，均进入总 `acceptance` 目标；构建配置、窗口锁和运行方式见[验收门禁](../../../README.md#构建与端到端验收)。`--profile-*` 是测量入口，不进入总入口。

`atelier_editor --project <工程目录> --profile-ui-reload <新的绝对输出目录>` 在真实编辑器窗口中保留 VSync，预热后用编辑器的原子保存函数写作者文件副本。每个进程交错执行 12 组修改，每组分别测一次开启细分计时的高度修改、一次仅测外层时间的高度修改，以及一次在同一函数内插入或移除空白与注释。三种条件的先后顺序均衡。

- `reloads.csv` 给出保存返回到新版首次进入接受帧的各段耗时。后台版本分别记录主线程捕获（`stage_ms`）、工作线程执行、结果就绪到回收的等待，以及核对文件并发布。编译窗口内的最大输入轮询间隔为 `compile_input_gap_ms`，同时统计工作线程执行期间的输入轮询数和接受旧帧数。以 `height_coarse` 标记关闭逐组件计时的高度对照。
- `input.csv` 记录额外一次高度保存后，从向真实窗口投递折叠点击到折叠帧被接受的耗时，并标明输入是否落在工作线程执行区间、是否由旧版 UI 呈现。随后展开工具架验证新高度生效。
- `components.csv` 按试次、文档版本和组件名给出复用决定、请求、Source 前端、依赖比较、LLVM/ORC 和 Runtime 发布时间，单位为纳秒。每个组件的 `total_ns` 累加其捕获、编译和发布阶段的工作时间。`native_compile_ns` 包含 `ir_ns` 和 `first_lookup_ns`；`ir_ns` 包含 `optimize_ns`；`first_lookup_ns` 包含 `object_ns`。
- `dispatch.csv` 测已接受版本的执行成本。保存计时结束后，编辑器修改并还原 PropertyGroup 事件，等待新帧接受，再用同一 Runtime 中的 ToolShelf 和 PropertyGroup 分批执行折叠更新与激活事件。每批记录操作数、实际求值数与总纳秒数。属性更新包含控件提交，事件计时止于处理函数返回。[机器码级别对照](../../research/ui-helper-incremental-compilation.md#ui-机器码生成级别对照)
- `steps.csv` 提供逐步输入轮询、接受帧间隔及现有帧记录器的阶段时间。常规启动不采集这些报表。[真实编辑器计时](../../research/ui-helper-incremental-compilation.md#真实编辑器从保存到接受帧) · [单组件编译分解](../../research/ui-helper-incremental-compilation.md#单组件编译耗时)

- `editor_log_smoke_acceptance`（窗口）运行 `atelier_editor_control_acceptance <atelier_editor.exe> <输出目录> --smoke-logs`：按 `Config/Project.json` 复制 Examples 的 `Config`、`Source` 与启动场景所在的 `Content/<场景包>`，以 `--no-activate --gpu-validation` 启动真实编辑器并等待启动场景的接受帧；经 Control 执行一次 `scene.add` → `scene.save` → `edit.undo`，确认撤销后有新的接受帧；随后启动 Play，在 Play 运行时关闭编辑器，检查编辑器与 runner 日志零错误且 Play 正常结束，通过后自动删除临时 `Project/` 副本。日志判定见 [Control 端到端验收](control.md#端到端验收)。
- `editor_document_acceptance`（无窗口）的默认运行覆盖 StaticMeshEntity 创建的单条历史及 undo/redo 后的身份与网格资源、glTF 节点变换数学、一次网格导入→重新导入、`.procgraph` 图格式往返与宽容加载，以及 SOP 编辑器上下文与 Houdini 式二维 Network 连线画布（工程自定义 `.source` `parallel_for` 节点、World 场景选中 `ProceduralGeneratorEntity` 自动跟随呈现并支持就地编辑 Network 与 Pin/Isolate 切换、Display 与选中节点解耦、`String`/标量参数撤销缓存命中、`file.save`、`graph.layout` 拓扑自动排版、Bezier 连线与解析轮廓节点绘制、锚点滚轮缩放、框选与多选单步撤销拖动、`Y` 划断连线单步撤销、`Tab` 浮动搜索创建并自动连线、Geometry Spreadsheet 分页过滤、多边形线框 `edge_mask` 与完整 Workspace UI 帧接受）。
- `project_scene_acceptance`（无窗口）覆盖一次作者字段值的 Scene 保存/重开往返，以及失败 Tick 中嵌套 stop 时 End 返回后才释放 PhysicsScene 的停止/关闭顺序。
- `project_integration_acceptance`（无窗口）覆盖真实 Source → Runtime 的一次 tick，以及停止时卡死的 Source 子进程在期限内被终止。

Control 验收夹具关闭编辑器时，投递 `WM_CLOSE` 后最多等待 300 秒，必须确认进程结束且退出码为 0；超时仍失败，并沿原 RAII 路径清理子进程。该期限包含关闭时等待当前不可中断的管线准备步骤，是验收上限，不是产品关闭时延承诺；[RHI 关闭合同](../rhi/contract.md#异步准备与-dxil-磁盘缓存)不变。析构中强制终止后的等待及 CLI 等待期限不变。

`atelier_editor --snapshot <输出目录>` 是显式设计评审工具，不进入默认验收或渲染特性对照：窗口以不激活方式显示，不抢占前台、不发送系统输入，直接调用文档与视口工具入口，导出各工具手柄、表面游标、模态 G/R 的 GPU readback PNG。运行仍遵守机器锁和窗口资源排期。

`--snapshot` 在 UI 线程上挂一个 WH_CBT 钩子：只要创建顶层原生对话框（`#32770`，包括 MessageBoxW 和文件选择框），进程就立即以退出码 1 结束，并输出 `unexpected native dialog "<标题>"`，不会停在那里等人点击。正常启动不挂钩子；由 Control 驱动的编辑器子进程和 `--profile-*` 测量入口不在覆盖范围内。

### 输入法验收

目前没有真实系统输入法的自动验收。改到输入法、组合或候选位置链路时，按[工程指南](../../engineering.md#迁移验收)在本机手动检查真实 IME。

## Scene Tree

Scene Tree 使用共享 `tree/tree_item`，投影 World → Entity 递归组合关系。行标签同时显示实例名称和 Runtime 发布的实际类型；World、Entity、StaticMeshEntity 使用不同的原生矢量符号和颜色。组件组成在 Entity Structure 中显示，不进入 Scene Tree。Entity 组合 parent 决定树缩进；无空间的组织 Entity 仍显示，组件保留在 Entity Structure，不另建组件空间树。行绑定实际 ObjectId，视图状态不持有业务根，不增加场景撤销。

参照 Houdini 22.0.429 的 Tree View：20 DIP 等高行、18 DIP 逐层缩进、空白区连续交替底色、细层级连接线、圆形加减按钮、类型图标和底部 Filter。全行选择采用灰色，遵循当前编辑器无黄色焦点框的约定。Workspace 持有 Scene Tree 与 Entity Structure 的展开状态（按 ObjectId，两棵树各自一份），资源目录树由 AssetBrowser 按目录路径持有，目录行首次出现时当前目录及其祖先展开、其余目录折叠；状态跨停靠、隐藏及重开保留，随对象退役清理，不进入撤销历史。每帧宿主按组合先序展平全部成员（O(n)），只为视口附近约三屏的行、首末行以及选中、焦点、重命名行和它们的祖先生成 TreeItem 描述，Dungeons（3617 个 Entity）停靠布局下 Scene Tree 每帧描述约 35 行。展开请求经 `Workspace::set_tree_expanded`（Entity 身份、绝对值、整体校验）或 `AssetBrowser::set_expanded`（目录路径）进入；选中行或其父变化时展开祖先一次，F2 通过 `reveal_in_tree` 展开祖先后开始内联重命名。底部过滤保留祖先并展开结果，不改写保存的展开状态。

上下/Home/End 导航并显露选中行，左/右键回父层、折叠和展开。Ctrl 切换 Entity 选择，Shift 按宿主展平的行选择范围（含尚未描述的行）；选择与视口共用 Session 的有序 Entity 集及活动 Entity。复用当前路径栏、字号、滚动条、输入捕获与接受帧；树层级来自真实 Entity 组合关系。

## 页签菜单与弹窗资源

页签下拉菜单提供 Set Pane Tab Type 子菜单、前后页签、关闭页签、恢复最近关闭、最大化、关闭整个 pane 和左右/上下分割。已注册视图保持单实例；更换类型交换已有视图的位置，关闭后重开保留该视图状态。加号负责向当前组加入视图。浮动窗口、复制视图尚未实现，菜单不提供无效动作。

菜单描述可嵌套。每层是保留的原生无激活窗口，主窗口保留键盘焦点与 capture；点击或 Right 打开子菜单，Left/Escape 回到父菜单。选择叶命令关闭整条菜单链；点击可勾选叶项的方框列则保留当前链，便于连续设置。改变根来源、失焦、关闭和改变 surface 时清理子菜单路径。每层按实际文字、DPI、样式和屏幕工作区测量，短菜单不保留滚动槽。

应用初始化主菜单及首层子菜单宿主；Renderer 仅在实际网格绘制时创建场景管线和深度资源。隐藏菜单折叠已有控件，保留文字缓存；相同描述重开复用尺寸，不再先缩到 32×32。菜单的更新与呈现耗时计入打开它的宿主阶段（见[帧性能](#帧性能)）。

菜单栏和 Combo 在鼠标按下时请求展开，弹窗接管后续松键，不再等待一次完整点击。列表弹窗在主视口提交/Present 前更新与呈现，避免再等待一轮场景渲染；仍只接受与原生客户区匹配的 GPU 帧。PopupHost 在窗口呈现前通过 `Renderer::sync_window_target` 同步实际客户区尺寸，沿 RHI 原有排空和资源退休机制调整交换链；菜单直接准备并呈现正确尺寸的帧，移除了先呈现旧画布再重试的循环。同尺寸为空操作，隐藏窗口仍可准备尺寸，菜单输入继续只使用已接受的帧。这个调度调整不改变渲染器本身的重场景耗时。

## 场景模型验收

`world_acceptance` 覆盖编辑与撤销/重做历史、层级变换组合和 Begin 失败后的清理；`project_scene_acceptance` 覆盖一次 Scene 保存/重开往返和物理停止顺序。场景只接受当前版本，不包含旧格式迁移验收。物理代表由 `physics_acceptance` 覆盖。

`project_integration_acceptance` 使用真实 Source 和独立 headless Play，覆盖一次 Source tick 与卡死子进程的停止。真实 Source 控制器切镜头仍依赖尚未接入的通用引用能力。

窗口与 Control 的入口和覆盖范围见[验收入口](#验收入口)。构建配置、必要性选择、机器锁和逐次报告由[验收门禁](../../../README.md#构建与端到端验收)统一规定；通过结果必须来自对应工作树的实际执行，不复用旧版本的通过表。
