# 首个场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。

完整 DSL 迁移的当前评估、区域需求、控件语义、手势边界和实施次序见[迁移计划](dsl-migration.md)。完整工具架、现有参数区结构和 Scene Tree 主体已使用当前[声明式 UI](declarative-ui.md)，现有外壳、菜单、停靠、Controls 和资源浏览器的装配也已迁入同一声明。
启动日志和 Help → About 使用构建时生成的同一提交标识及配置；未提交改动标为 `local changes`。`atelier_editor --version` 可在不创建窗口的情况下读取该二进制的标识。工程打开成功后记录实际根目录，About 同时显示当前工程目录和场景路径。标识在构建时刷新，运行时不读取当前目录的 Git 状态。
当前消费者将正式 Actor/Component/World 接入同一编辑器，并使用自动反射、共享调用合同与基础值类型。已收口的 S1/S2 输入与视觉合同保留；当前基础控件增量及其验收入口见下文。World 使用 SceneComponent、StaticMeshComponent 和真实 StaticMeshActor；网格使用中立的不可变 `geometry::MeshRef`，遵循[World 合同](../world/README.md)。
模型与渲染消费完整 TRS；参数面板展示实例的公开标量和内联记录。位置、欧拉角和缩放使用三分量行，规范数学类型按元数据组合 Vector2/3/4，材质颜色按 RGB 展示；选择 Material 时编辑共享材质参数，其他资源显示元数据。已接入真实网格拾取、多选/框选、变换手柄及 G/R/S 操作。已声明默认值、修改状态、折叠分组、独立菜单、中键数值阶梯继续复用同一控件链；编辑器内 JIT 单步交互、节点画布、可浮动编辑器视图、布局文件保存、参数面板固定目标仍未实施。

UI 作者层已将 Parameters 的查询、Position、属性组、Actor Structure 和 Scene Tree 迁入 [editor.ui.source](../../../assets/editor.ui.source)，使用有类型的组件组合、动态项和原单行编辑器。在 Dev / Release 开发工作树保存声明文件即可更新面板；代码按组件复用，兼容的 Instance / Context 保留，只增删对应子实例，源码失败保留旧定义与交互。布局、样式、输入与业务事务继续沿用现有设施；作者语法、生命周期和更新边界见[原生声明式 UI](declarative-ui.md)。

本波次审查、对象模型判断和验收见[工程工作流审查](../../research/editor-project-review-20261006.md)。

## 工程与文件

[Editor Control](control.md) 是指令化操作与调试的标准入口，供 UI、快捷键、CLI、自动化、验收和 LLM 共用。现有外部指令支持实例发现、场景操作、内存快照导出和渲染对照；UI 直接调用业务层，本机管道与 CLI 适配到相同的 Application/Session 入口。正常编辑器默认启用外部接入，`--no-control` 可关闭；源码调试面板和 `debug.*` 仍待接入。

工程直接选择文件夹，包含 Config、Content、Source、Cache。已接入创建/打开、场景保存重开、资源目录、三种浏览视图和 OBJ 几何导入；Source 文件已接入编辑器内创建、跨文件编译、真实组件类型、属性持久化、独立运行与兼容热更；项目 UI 采用同一工程配置并支持独立预览。格式、操作、资源身份和实际支持范围见[文件夹工程](projects.md)。

## 源码编辑

File → Edit Editor UI Source 可直接打开并修改当前 UI 声明；Code 面板支持持久文档、独立撤销、保存、编译诊断定位和失败恢复。用法、边界与验收见[源码工作流](source-editor.md)。

## 当前 UI 作者入口

现有编辑器界面统一由 [editor.ui.source](../../../assets/editor.ui.source) 描述，包含总外壳、顶部菜单栏、工具架、状态栏、停靠栏/分隔条、Scene View、Parameters、World、Scene Tree、Actor Structure、Asset Browser、Controls、Code、Project Output 和原生窗口内的弹出菜单。C++ 提供数据与身份、业务命令和原生机制；各区域不再保留另一条 C++ 界面组装路径。支持范围、动态内容槽和更新事务见[声明式 UI](declarative-ui.md)。

## 正式执行链

```text
Win32 窗口消息 → Window 复制拥有的事件 → 每条消息后顺序排空
  → UI 已接受帧命中/焦点/捕获 → Reply.effects
  → Action → editor::Session → world::World → Runtime 的真实 Actor/Component 字段
           → Workspace.query → 参数查看投影
  → ViewportTool（已接受的视口/相机）→ Session 的选择与整组 TRS 事务
  → PlatformRequest → Window 剪贴板/IMM32 → UI 结果事件
  → Workspace 控件描述 + World RenderItem 矩阵/资源/材质值 → UI 帧 + renderer 网格实例
 → renderer 最终 accepted → UI 接受对应命中帧
```

WndProc 的输入事件只复制和排队，不重入 UI/业务；同步输入回调也在同一泵中依序消费。UTF-16 代理项在顺序投递时组装，组合流绑定独立 composition_id 与原编辑身份。候选位置只使用与当前 token 相匹配的 accepted caret。同步 IMR_QUERYCHARPOSITION 只读对应帧的插入/组合起点快照，将客户区物理坐标转为屏幕坐标；仅支持偏移 0，无有效快照或其他偏移明确拒绝。快照在原生组合/失焦/Surface 变化时立即撤下，不用旧显示 caret 冒充组合起点。IME 终止键的首次按下、重复和抬起保持同一归属，避免确认组合后又提交字段。
系统模态窗口的 `WM_CANCELMODE` 在 WndProc 内立即交给 `DefWindowProcW` 释放原生鼠标捕获，同时排队保留业务取消事件。文件选择和保存确认会暂停应用主循环，不能把原生捕获释放也延后到该循环；否则已禁用的主窗口会继续截获对话框的鼠标输入。业务取消仍在原有事件泵中执行，不从系统回调重入 UI、场景或渲染。该时序遵循 [Windows WM_CANCELMODE 合同](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-cancelmode)。
剪贴板失败保留草稿/选区/历史供重试；业务 Action 失败停止同组剩余普通 effects，仍执行 UI 取消、业务回滚、IME 与捕获清理，保留首错并报告清理失败。

[World](../../../src/world/world.hpp)和 Runtime 管理唯一 Actor/Component 身份、组成与成员资格。编辑 World 保持 stopped；Actor 的 AttachedInactive 状态仍可呈现和编辑，renderer 不按 Active 状态筛除编辑内容。
World 的原生类型与字段元数据由[自动反射](../../reflection.md)生成，经 `world::register_types` 发布到 Session 拥有的 Runtime；项目 Source 派生类型在 Session::Prepare 中由同一 Runtime 类型系统发布。编辑器使用该入口返回的类型和字段路径；生成头、World 库及正式编辑器均由当前工作树、当前配置的 CMake 依赖链构建。
[Session](../../../src/editor/session.hpp)是文档唯一写入口，保存 `{actor, object}` 选择和统一的 Actor、组件、字段历史。字段事务捕获具体对象 ObjectId、正式 FieldPath 和控制会话，不再从当前选择或类型顺序猜目标。一次拖调连续改真实字段，提交只记一次历史。SceneComponent 字段可能使四元数其他分量规范化，所以记录完整 TRS 前后值，经 World 原子恢复；不是删除时的对象图快照。属性 undo/redo 恢复该字段所属对象作为检查目标，并保留其 Actor 上下文。同一目标重复选择不取消草稿；换目标先取消事务。
Actor 删除记录保存普通 Actor 根及 World 成员位置，组件独立移除记录保存组件根及 owner/顺序/空间父节点/根身份恢复资料。Undo 恢复同一 ObjectId 的当前组成与成员资格，重新生成 UI 绑定。裁剪历史/redo 与关闭文档只释放持有，不隐式 destroy。历史最多128项，编辑器最多256个 Actor；只有 World 成员可编辑，没有离线写入或多写者冲突机制。
World 操作的 Result 失败表示提交未完成；成功 Commit 中的 notification_error 表示关系已改变。Session 先记录变化并更新选择，应用再报告通知错误并停止后续动作，不把它当成未提交或重做一次。成员 span 只在结构变化前借用，UI/renderer 不持有它。
宿主同线程逐帧以32工作单位推进 GC；普通根保活不表示对象仍参与 World。文档关闭先结束活动编辑，再关闭 World、释放历史；World close 撤成员/参与和根，Runtime 生命周期随后由宿主管理。

## 参数和坐标

材质资源、槽菜单、共享编辑、独立资产历史和实例参数覆盖使用同一 DSL/Runtime/Session 链。Actor 参数区的覆盖复选框明确表达继承意图；资产绑定后隐藏无效的旧整体材质控件。文件入口、Scene v4、保存与实际限制见[材质资源工作流](projects.md#材质与贴图资源)。


位置 X/Y/Z 默认0米，合法域为有限的 `[-10000, 10000]`。普通拖调每逻辑像素0.01米，细调0.001米。
范围、默认值、单位与拖调步长由[应用参数描述](../../../src/editor/session.hpp)绑定 Runtime 字段，布局与 Runtime 不承担编辑器政策。
控件 key 独立单调分配，绑定具体 ObjectId 和 FieldId 路径；稳定控件复用其键，失效后退役且不再分配。Actor/组件恢复保留业务身份，但不会恢复旧控件会话、IME 或剪贴板请求。

位置、旋转、缩放和数学向量复用 DSL `NumberRow` 与共享 `append_number_row`。标题和字段随宽度换行，分量共用一行。根旋转使用 XYZ 顺序的欧拉角（度），实际存储仍为规范化四元数；一次输入始终基于事务起始姿态计算，避免跨过 90° 后将中间转换结果反馈到下一次预览。显示角为四元数的规范欧拉表示，不保存额外的圈数状态。缩放范围为 `[0.0001, 10000]`，不提供负缩放或 shear；位置范围与上方参数定义一致，手柄和字段沿 Session 执行相同范围检查。

数值显示按底层 float/double 生成可往返的最短十进制，不把 float 扩成 double 的尾差展示为有效精度。欧拉角与四元数转换使用 double 中间计算，最后写入 float，避免额外的中间舍入误差。数学元组根据 Runtime 数学类型元数据识别；SceneComponent 的旋转与 World 材质颜色由实际字段身份识别。标题和菜单可恢复完整位置、旋转、缩放；位置恢复包含搜索隐藏的分量。无变化不增加历史，undo/redo 恢复具体组件及 Actor 上下文。

- 左手坐标系，+Y向上，单位米；方块各轴范围 `[-0.5, 0.5]`。World 组合局部 `S * R * T` 与父世界矩阵；renderer 不自行重算 TRS。空间换父 API 在 World 提供，当前面板编辑根位置。
- 默认相机 eye `(6,5,-8)`、target `(0,0,0)`、up `(0,1,0)`；使用 LookAtLH/PerspectiveFovLH。
- CPU 矩阵行主存储，行向量按 `p * world * view * projection` 计算，D3D 深度为 `[0,1]`。
- viewport 与 UI clip 使用窗口客户区左上为原点的物理像素。1280×720客户区内的 `{240,32,760,660}` 表示从 `(240,32)` 开始、宽760高660的视口。
- UI 根据实际 backbuffer 尺寸和 DPI 生成 draw/clip；renderer 不再次缩放。深度目标显式匹配实际 backbuffer。

## 视口编辑

Scene View 按 Houdini 22 的场景视图校准（UI Scale 1.75 同机像素采样，2026-10-08）。左侧 38 DIP 工具栏以 32 DIP 单元排列选择、通用、移动、旋转、缩放，选择与手柄工具之间及底部吸附开关前各有 `#4d4d4d` 细分隔；手柄工具使用中性立方体加 `#e04a1c` 手柄的双色图标，选中态 `#5d7289`。路径栏下方是可折叠的 32 DIP 操作控制栏（`#2e2e2e`，黑色上边与 `#424242` 内沿），包含当前操作图标、轴空间、枢轴、吸附开关/步长和表面游标；G/R/S 期间整条换成 Blender 头部读数（如 `D: 0.9499 m (0.9499 m) along global Z`、`Rotation: 22.90 along global X`，键入值显示为 `[2|]`）。视口左上是带阴影的白色操作名与图标，右上是半透明圆角视图胶囊（`Persp ▾`），左下为 Houdini 彩色圆片坐标轴，底边一行为当前工具的鼠标/按键提示，右、下两边绘制 `#424242/#666666` 凹边。背景渐变为 `#657f8c → #bec6c6`，选中描边为 Houdini 黄 `#ffd900`。图标沿原生符号栅格化和缓存链绘制，不复制 Houdini 专有图标素材。

| 操作 | 行为 |
| --- | --- |
| 右键单击物体 | 对实际网格三角形求最近命中；无可渲染网格的空间根以十字定位点命中 |
| Shift / Ctrl + 右键 | Shift 切换该 Actor 的选择，Ctrl 移除；Scene Tree 中 Ctrl 切换、Shift 选择可见范围 |
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
| Shift+D | 复制所选 Actor 及其组件并进入移动；左键或 Enter 确认，Esc/右键只取消位移、保留原位副本。复制和初次移动合为一次撤销 |
| X | 在指针下打开删除确认项；取消不修改场景。视口始终删除 Actor，检查某个 Component 时也不会误删成单个组件 |
| Shift+A | 在指针处打开添加菜单（Create 组命令，于 3D 游标处创建） |
| Z | 渲染模式饼菜单：左 Wireframe、右 Solid、下 MatCap、上 Scene Lit。轻按后点击，或按住、划向选项、松开 Z 确认；中心死区内长按松开取消。Esc/右键取消 |
| Shift+反引号 | FPS 自由视角：鼠标看向，WASD 移动，Q/E 下降/上升，滚轮调速，Shift 加速。Enter/左键确认，Esc/右键恢复进入前的相机；显示 Fly、中心准星和操作提示 |
| 吸附开关 / Shift+Tab | 开关移动与游标的网格吸附；步长为 0.1、0.2、0.5 或 1 米，与参考网格共用设置。移动时对齐活动 Actor 原点，保留组内偏移与被约束的坐标 |
| Ctrl / Shift 拖动 | Ctrl 临时反转移动网格吸附开关；旋转为 15°、缩放为 0.1 倍率吸附。Shift 使用十分之一步长；键入数值仍按精确值执行 |
| 左键单击空白或物体 | 放置 3D 游标，不修改选择或场景历史。游标是带朝向的三维坐标系：表面定位时局部 Z 对齐命中面法线，X 尽量对齐视图右方向；绘制为 Blender 4.x 样式的 16 段红白虚线圆环，四根黑色刻度沿游标自身 X/Y 轴投影，因此贴面放置时可见其倾斜。无命中时使用当前视图的构造平面，射线平行时使用穿过旧游标的视平面；关闭 Surface Cursor 后只使用平面定位，重置游标同时清除朝向 |
| W | 打开模式、Snap、聚焦、创建和删除菜单；Snap 提供选择到网格、选择中心到游标、游标到选择/原点/网格 |
| 中键 / Alt+左键 / Shift+中键拖动 | 前两者环绕相机，水平拖动按抓取场景的方向旋转，Shift+中键平移；滚轮缩放，F 聚焦当前 Actor。正交视图开始环绕后转为透视 |

Alt+A/G/R/S 以 `WM_SYSKEYDOWN` 到达，窗口把这四个键与菜单助记键一样交给应用，不进入系统菜单。快捷键仅在视口内且文本输入未占用键盘时启动。视口上方的交互控件先参与统一 UI 命中，点击视图菜单不会向后穿透为游标定位。多选变换只处理有空间根的 Actor；Parameters 展示活动 Actor，视口手柄修改整组。移动/旋转使用世界轴；缩放手柄沿活动根的局部轴，改变各根自身的 scale 分量，并在活动根基底中缩放组内位置偏移。枢轴可选组中心或游标，Static Mesh 创建使用游标位置。此阶段没有局部/世界坐标切换、物体变换的表面/顶点吸附、跨面板资源放置或程序外拖入。

`ViewportTool` 只持有手势，`Session` 是业务修改入口；一个手势记录一次历史，组内任一目标拒绝时恢复前一有效预览。非法或未完成的数值不能确认；Esc、右键、失焦、捕获丢失、尺寸/DPI 改变、菜单转移、文档更换都取消草稿。Ctrl+Z/Y 先结束视口草稿，再执行同一场景历史。活动变换期间拒绝保存快照与启动 World。

复制沿用场景可编辑属性的同一字段规则，创建独立 Actor/Component 身份，重建组件父子、根、启用状态，复制业务属性、材质与物理配置，复用不可变网格/碰撞资源。整组选中先在脱离 World 的状态下准备，成员发布失败时回退；不复制运行期句柄、速度或外部业务引用。初次定位只允许合入刚建立且未保存的创建记录，之后的普通移动照常单独记历史。交互参照 [Blender Duplicate](https://docs.blender.org/manual/en/4.3/scene_layout/object/editing/duplicate.html) 和 [Pie Menus](https://docs.blender.org/manual/en/latest/interface/controls/buttons/menus.html)；渲染模式使用 Z，吸附保留在磁铁菜单和 Shift+Tab。

自由视角采用 [Blender Walk Navigation](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/walk_fly.html) 的无重力移动方式。相对鼠标只在前台视口手势期间注册、隐藏并约束；失焦、捕获丢失、尺寸变化、菜单转移或文档更换时取消并释放。它复用同一 Camera，不修改 Scene、3D 游标或撤销栈；Control 在飞行期间报告 `camera_fly` 忙状态。真实原始鼠标输入由需要前台的窗口验收覆盖，后台快捷键验收不会注入桌面鼠标。

视角与渲染模式饼菜单共用 DSL 的 `ViewportPie` / `PieEntry` 和稳定的根节点身份，只更新标题与条目；叠在同一接受帧内，复用菜单项模型与命令执行入口。普通列表菜单继续使用可跨窗边界的原生 PopupHost。方向选择基于接受帧的槽位，显示位置受客户区约束；边缘位置可直接点击可见项。快速划动松键在首次呈现后结算；菜单打开期间屏蔽场景输入，关闭、失焦和 surface 变化撤销模态状态。六向相机的投影、平移、拾取与游标构造平面使用同一相机基底；切换视角不进入场景历史。快捷键参照 [Blender 默认键位](https://docs.blender.org/manual/en/5.0/interface/keymap/blender_default.html)。

可勾选菜单项的方框列允许连续修改：按下和松开都在该列内时，切换后保留同一菜单窗口与焦点，并从权威视图设置刷新勾选状态。点击文字或用 Enter/Space 激活则切换后关闭；互斥项和普通命令维持原有关闭行为。

绘制和命中遍历同一手柄几何；坐标和相机只在 renderer 接受帧后发布。线段/方框复用 renderer 固色实例管线，在场景后、UI 前绘制为一个裁剪批次，不写场景深度，不建立第二个视口窗口或渲染循环。

### 工作室 Cavity

Scene View 导航行末尾的 **Cavity** 菜单控制屏幕空间凹凸增强：`Screen Cavity` 开关、Ridge 凸边亮度、Valley 凹边暗度及 Radius 采样半径。默认开启，两个强度默认 1、范围 0–2，半径默认 1 物理像素、范围 0.5–4；菜单提供常用档位，Control 接口支持范围内的数值。效果用于 MatCap、Smooth 和 Flat；Scene Lit 和纯线框时入口禁用并保留偏好。

增强使用几何深度和法线，在网格、选中轮廓、手柄和 UI 之前合成；不改场景、材质和撤销历史。此项对应 Blender 的 [Screen Cavity](https://docs.blender.org/manual/en/3.6/editors/3dview/display/shading.html)，随屏幕像素工作；渲染和资源寿命合同见[渲染模块](../renderer/contract.md)。

### FPS 显示

通过视口 **Guides → Show FPS** 或顶部 **View → Show FPS** 开关，默认关闭。启用后在左上角模式名称下方显示帧率及平均帧间隔（ms），每 0.5 秒更新一次。统计来自主窗口已接受帧之间的实际时间，包含正常 UI、场景绘制及呈现等待；它不是 GPU 单独耗时。文字不占布局空间、不拦截视口输入，不修改场景历史。视口隐藏、窗口最小化、提交失败或诊断截图时清空采样，恢复后重新累计，避免将暂停和读回耗时混入正常读数。开关由公共 `show_fps` 命令和同一布尔 Action 入口处理。

### 场景光照

Scene View 的显示菜单只提供着色预览与边线模式：`Scene Lit`、Smooth、Flat、MatCap 共用互斥组。场景光照与效果参数统一在 World 页编辑，随 Scene 保存并进入同一撤销栈；显示模式、相机及诊断开关仍属于视口。Local GI 与 GTAO 互斥。实际请求超阴影预算时，状态栏显示上一个 accepted frame 的 `Shadows 实际/请求`；关闭 shadows 的灯不计请求数，创建灯数量不受该质量预算限制。

**Window → World Settings** 打开可停靠的 World 页。页面由 `editor.ui.source` 声明，子页为 Lighting、DDGI、Effects、Diagnostics。Lighting 只提供全局曝光与阴影配置。具体灯光仍通过 Scene Tree / Actor Structure 选择，并在 Parameters 编辑；World 页不枚举场景灯光。Effects 提供 GTAO / Local GI、SSSR、Bloom、AA 与半径/距离。DDGI 可选择四个固定槽位，编辑启用、Fixed / Follow Focus、边界、三轴数量、混合边界、共享更新预算、射线数、反馈和间接光增益。Follow Focus 持久化模式和相对边界；实际焦点由宿主提供，编辑器使用观察目标。Diagnostics 包含探针球体、接收面指标、体积、X-Ray、反馈可见性和 CPU 布局读数，不进入 Scene。

**Coverage → Fit Scene** 按当前可渲染几何的世界边界建立固定覆盖；**Scene + Follow Focus** 保留粗层场景覆盖，并用高优先级体积跟随观察目标。两者保留传播与诊断值，按一次完整 World 修改进入场景历史；重复应用不产生新历史。示例初始化可显式拟合其场景覆盖；普通场景读取自身配置，旧格式使用默认值，不继承上一场景。数值输入和拖动只更新字段草稿，提交后才通过 Session 发布 World 配置与渲染快照；取消、关闭页面和替换文档会结束未提交编辑。无效配置整体拒绝，规则与 Renderer / Control 共用。

**Current layout (CPU)** 显示最近接受帧的实际世界边界、间距、数量、驻留槽数及本帧调度/重置数。这些数值描述 CPU 布局和调度，不表示 GPU 已完成更新或有效光照支持数。切换文档清除旧读数；具体容量、传播和诊断语义以 [Scene Lit](../renderer/scene-lit.md) 为准。

覆盖预设根据场景几何边界和相机观察目标确定范围；楼层高度、薄墙和空腔会影响有效支持。多层室内场景可在现有槽位中配置局部楼层体积，用 Receiver Coverage 的 Status / Volume 检查实际贡献。观察时应等待多个完整更新轮次，分别判断覆盖范围、遮挡、有效支持和照明强度。

Create 菜单与工具架沿同一命令创建 Point、Rectangle、Directional Light 和 Sky Light。场景已有 Sky Light 时，其创建入口禁用；天光的颜色和 Radiance (cd/m²) 强度由同一属性链编辑，隐藏局部范围、光源尺寸和直接阴影控件。Parameters 来自真实 LightComponent.settings 反射字段：颜色为 RGB 数值行，强度标注 cd、cd/m² 或 lux；局部灯显示米制范围，矩形灯显示宽高及双面选项，方向灯显示弧度制角半径。材质 roughness/metallic 为 `[0,1]`，emissive 使用非负 HDR RGB。编辑、取消、撤销、组件移除与恢复均沿普通 Session 入口；场景编码见[项目场景](../world/project-scenes.md)。

`application::SceneSnapshot` 在一个地方把 World 的 mesh/light 值快照转换为 Renderer 数据。完整 ObjectId 仅用作映射键，Renderer 使用该场景内单调分配的非零身份；移出投影的条目释放映射，重新出现时分配新键。替换文档重置映射并改变 scene_identity。几何资源、材质、灯参数和呈现成员变化递增 scene_revision；TRS、选择和相机连续移动不递增。显式聚焦、重置、切换视图及正交/透视转换提交 camera_cut，直到 Renderer 接受帧才清除。没有独立的灯生命周期或另一份可编辑场景。

光照及快照的当前合同见 [Scene Lit](../renderer/scene-lit.md)，应用创建、撤销、保存重开和正式窗口结果统一见 [Renderer 验收](../renderer/acceptance.md)。早期只完成语法检查的交接记录已被这些集成结果替代。

## 工作区与停靠

主窗体只组织四个区域：顶部菜单栏、工具架、中间 DockSpace 内容区和底部状态栏。业务功能以可停靠的 Tab 子页组成，不将页面业务提升为固定外壳控件。

默认布局参照 Houdini 的分区：Scene View/Code 在左上，Asset Browser/Project Output 在左下，Parameters/Controls/World 共用右上标签组，Scene Tree/Actor Structure 在右下。各视图只保留一份业务状态，可以移到其他标签组或分割区域。

固定视图标签统一使用正体，包括 Scene View；选中状态仅通过灰色底面区分，不使用斜体或焦点描边。标签只绘制右侧分隔边，首标签直接衔接分隔条的收边，不叠加左边框。

- 拖标签到另一个标签栏可合并/重排；拖到内容的左、右、上、下边缘可分栏，松开前有预览，Esc 取消。分隔条拖动只移动预览线，两侧面板和 viewport 保留已接受的尺寸，松手一次提交比例，避免连续重建视口渲染目标。已有 UI 指针手势始终由原控件处理，跨入视口时也不交给视口悬停处理。Esc、失焦和丢失捕获取消预览；已取消手势的迟到松键不提交。两侧箭头收起对应面板为 8 DIP 细条，重按恢复原比例；Tab 栏直接从标签开始。中央单排方点单击交换两侧，Alt+单击切换横竖方向，拖动仍调整比例。
- 分隔条的三个按钮沿条方向各占 36 DIP，连续居中排列，窄小宿主内等比例缩短。背景为 `#494949`，靠前边为黑色、靠后边为 `#333333`；横竖方向共用 SS 边框规则，悬停和按下反馈限制在按钮内部，保持外侧细边连续。
- “＋”紧跟最后一个标签，打开原生菜单，将已有视图移到当前组，或重新打开已关闭的视图；视图的值和历史保留。右端下拉菜单的 Show 则跳转到视图当前所在组。
- 标签的关闭按钮移除其显示；右侧菜单可以重新打开已注册视图，并把当前标签移到右方/下方的新区域。空间不足或组中只有一个标签时，对应分割菜单不可用。
- 面板右上角切换最大化/还原；Ctrl+B 对最近操作的面板执行同一动作。Ctrl+Tab / Ctrl+Shift+Tab 切换组内标签，Ctrl+W 关闭当前标签（Code 中先关闭源码文档）；菜单打开期间由菜单独占键盘。
- **Window → Reset layout** 恢复默认位置及标签；场景、相机、筛选、样本值和历史不因恢复布局而重置。最后一个视图关闭后也可恢复。
- 样本与场景历史按照最近操作的视图选择，允许 Controls 与 Parameters 同时显示。视图隐藏/移动时，未提交交互仍通过 Session 的取消/失焦合同结束；业务字段 key 依对象实际成员生命周期退役，隐藏面板不退役对象绑定。
- Scene View 隐藏时提交空场景矩形，不生成场景实例；其他 UI 继续走原窗口、renderer 和接受帧。参数面板标题与可选搜索固定，实验台另外保留示例内容页签；下方内容滚动以适应较矮的区域；窄参数行仍使用公共 Flex 换行。

以 Houdini UI Scale 1.75 为同机参照：数值字段最低 23 DIP、标签列 160 DIP、行间 7 DIP。菜单栏为 32 DIP，Tab 栏为 22 DIP；菜单栏与工具架的最新收起把手和整行恢复条尺寸见下方[首版操作](#首版操作)。当前中英文正文与 Tab 使用 12.5 DIP Sarasa UI SC，菜单栏文字为 13.5 DIP。Tab 选中后使用灰色底面，键盘焦点不额外画框；文字字段保留输入焦点提示。F10/Alt 菜单快捷键会展开收起的菜单栏；恢复布局一并展开菜单栏和工具架。系统 DPI 仍只有 Window → Surface 的一个换算入口。早期观察见[工作区研究](../../research/houdini-workspace-reference-20261001.md)与[栏结构校准](../../research/houdini-chrome-20261002.md)，当前行为以本页为准。独立原生浮窗、多份同类视图和持久布局属于后续增量。

布局与参数的本轮参照、原生观察及验收记录见 [Pane 细节](../../research/houdini-pane-details-20261001.md)。

## 控件实验台

**Controls** 标签打开实验台；选择 **Parameters** 回到实际场景参数，也可将两者分栏同时显示。实验台使用同一个 `ui::Session`、SS、布局、输入法和 renderer，没有独立绘制循环。

- 可编辑 Position Vector3、Exposure、Unicode 名称、Enabled 勾选、Vector4 和 Projection 下拉样本。Position / Vector4 使用显式轴提示色线；Exposure 同时提供数值框和滑条，合法数值为 0–32、滑条显示 0–16，支持拖动、键盘细调、撤销及取消。数值与下拉共用右对齐标签列，名称位于固定的 Sample 标题行；数值字段从 23 DIP 起，共用黑色输入框、凹入边缘、轻渐变按钮；默认灰白文字、非默认数值加粗、灰色编辑边界和菜单定位；勾选使用勾号，折叠使用三角。参数勾选项与折叠标题没有悬停或按下底色。参数下拉按钮高 26.667 DIP（150% 缩放下约 40 px），文字两侧保留 14 / 12 DIP，尾部使用单向下箭头。普通命令菜单不绘制勾选或互斥圆环，也不预留空的标记列；只有布尔项和单选项使用对应标记。
- 右键菜单提供分量/整组默认值、复制和粘贴；Ctrl+中键直接恢复默认值，中键按住调出数值阶梯。整组恢复仍只记一次撤销，全部样本保留统一 Reset。Projection 和 Vector4 均为独立样本，不向 World 添加虚构字段。
- Disable sample fields 检查禁用传播，Show long labels 检查换行，Transform and display 与 Color components 标题支持鼠标与 Enter/Space 折叠。
- 分隔条由视图最小尺寸和当前工作区空间共同约束。窄面板保持字段可达，超出可用高度的样本通过同一 list 滚动。
- 样本最多保留 64 次提交历史；UI 活动草稿仍优先使用共享文本历史。实验台打开时，Edit 菜单 Undo/Redo 与无本地事务的 Ctrl+Z/Y 进入样本历史，Delete 不穿透到场景删除。样本值不写入 World。

真实窗口验收在既有 `editor_acceptance` 中覆盖整向量单次撤销、搜索隐藏修改分量后的整组恢复、修改状态、折叠与稳定字段身份、勾选键鼠行为、最窄面板中的 Vector3/Vector4 与禁用、样本历史隔离及返回业务面板。新增验收覆盖菜单不穿透外部点击/视口滚轮/Delete、Menu/Shift+F10/Esc、下拉键盘选择、菜单剪贴板及非法粘贴取消、中键阶梯/细调/默认值/取消/缓存复用。截图为 `out/<配置>/editor-evidence/controls-*.png`；真实搜狗输入、Space 确认、折叠/切换面板取消组合由同一 `editor_ime_acceptance` 覆盖。

原生菜单的统一开启入口先取消主 UI 当前抓取手势，经原 effects 链回滚业务编辑后再转移鼠标捕获。窗口验收覆盖数值拖动和中键阶梯期间按 F10，随后释放鼠标、关闭菜单、再次移动，确认不恢复旧手势、不新增历史。

本阶段最终验收、review 修复和已知边界见 [2026-10-02 阶段收口](../../research/editor-ui-20261002-closeout.md)。

## 首版操作

顶部菜单栏提供 File / Edit / Create / Project / View / Window / Help。Create 快捷区创建 Actor 或 Static Mesh Actor，View 快捷区聚焦选择和重置相机；Window 打开视图、切换/关闭页签、最大化和恢复布局。无参数启动加载自带 Sponza 场景，启用 Scene Lit 与 DDGI；Project → Examples 提供 Sponza 和 Scene Lit Room，保存与副本规则见[官方 Examples](../../../assets/examples/README.md)。Parameters 通过真实类型显示检查目标及其连续属性区。Actor Structure 展示 Actor 归属下的组件，负责选择、添加和删除；Scene Tree 保持 Actor 列表。没有根组件时不显示根位置，Focus 停用；没有检查目标时显示空状态。

`commands.hpp` 是编辑器命令描述入口，`Workspace::command_enabled` 判定当前可用性，`Application::execute` 执行。菜单和顶部快捷区读取同一描述，快捷键与资源点击进入同一个执行函数。执行时再次检查可用性；UI 只产生通用 Action，不直接修改 World。输入字段和拖动仍通过 Session 事务入口。

菜单栏通过 `EditorShell`、工具架通过 `ToolShelf` DSL 描述同一套收起交互，共用 `stow_*` 尺寸和 `bar-stow` 样式。展开把手宽 12 DIP，当前 150% 缩放下为 18px；箭头位于顶部 12 DIP 方形区。收起时同一个按钮铺满整条横栏，高 12 DIP，任何位置均可点击恢复，箭头保持左对齐。展开把手和收起横条的常态、悬停、焦点均为灰色渐变；仅按住时采用灰蓝渐变 `#5D7389 → #748CA5`，松开即恢复灰色，箭头始终为黑色。两栏分别使用自身实际占用高度分配剩余工作区；窗口验收覆盖单独/同时收起、横条中部与末端点击和键盘恢复。

顶部工具架使用 18 DIP 分类行与紧凑图标区，总高 56 DIP；收起后为上述整行恢复条。分类选择跨收起/展开保留，收起不会改变场景历史，Reset Layout 恢复展开。工具按钮统一宽 64 DIP、18 DIP 图标与 10.5 DIP 短标签，菜单保留完整指令名。图标和文字由同一按钮绘制/命中，`icon-placement: above` 由共享 SS 测量与绘制处理。

Asset Browser 是独立可停靠视图，浏览工程目录与引擎资源；单击选择，Add to Scene 执行共享创建命令，支持名称/类型过滤与 List/Details/Tiles。工具行、目录树、状态行和全部结果视图由 DSL 声明；List/Details/Tiles 通过 TableBinding 接入共享 Table，提供排序、列宽拖动、当前单元格、键盘导航和双击/Enter Open。三种呈现复用同一数据快照和选择状态，详见[工程与资源浏览](projects.md)。顶部快捷创建继续引用相同内置资源。World/renderer 不按 Cube 名字分支；拖放、吸附和模型缩略图尚未实施。
数值控件支持普通拖动和中键七档数值阶梯；上下移动选步长，左右移动调值，Shift 细调，释放提交一次、Esc 取消。右键或 Menu/Shift+F10 打开参数菜单；Ctrl+中键恢复分量或标题对应整组默认值。菜单期间视口输入暂停，外部点击只关闭菜单，Esc/Tab 返回原焦点。普通拖动用于调值，单击进入文字模式后拖动用于选字。首次实际文字修改才建立事务；合法 Enter 提交，非法 Enter 保留草稿，Esc 恢复整次编辑的原值。结束后保留字段焦点，数值退出文字模式。字段焦点下 Delete/Backspace 只编辑文字。
文本与数值共用 Unicode 字素边界、视觉光标、双向选区、横滚及本地撤销/重做。Ctrl+A/C/X/V 提供全选、复制、剪切、粘贴。存在本地事务时 Ctrl+Z/Y 只操作本地历史，即使历史耗尽也不穿透；仅聚焦无事务时进入场景历史，Ctrl+Shift+Z 同样重做。历史合组、容量和 Unicode 覆盖见 [UI 模块](../ui/README.md#共享单行编辑)。
Parameters 的 Component / Category 子页切换组织方式。`AFIELD(Label="Position (m)", Category="Transform")` 提供显示名与分类；`Category` 是标准拼写。内联记录的分类向下继承，子字段的非空分类可覆盖；向量分量沿用聚合字段的标签。无标签时按字段名生成标题，无分类时归入 General。Component 模式保留根 TRS 的整组编辑与默认值恢复；Category 模式按元数据汇集属性，字段仍绑定同一 ObjectId/FieldPath，分组不改变持久身份或场景历史。Source 生成的未声明元数据保持空值，本轮没有添加 Source 注解语法。

子区域使用共享 `section-tabs` / `section-tab`：26 DIP 页签、0.667 DIP 细边、未选中 #555555→#303030 渐变、选中 #4F4F4F 平面与 #3A3A3A 条带。Parameters 和 World 的第一个页签从条带左边缘起排，不预留空槽；停靠页签继续使用自己的 `dock-tab` 样式。

参数搜索按实际属性、类型名，以及位置/旋转/缩放行的名称与别名过滤。确认文字实时过滤，IME 预编辑不改变查询；Enter 或普通换焦点提交查询，Esc/OS 失焦/Surface 失效恢复原查询。查询属于 Workspace 查看状态，不写场景历史。相机与变换操作见上方[视口编辑](#视口编辑)。
调整窗口尺寸、DPI变化、失焦或外部捕获丢失都会取消当前未提交编辑。应用主动释放捕获产生的同步通知不会误取消刚进入的文本编辑。

### 独立菜单窗口

菜单栏、参数右键、Projection 下拉和 Pane 菜单全部通过同一个 PopupHost，使用独立 owned HWND，主窗口保留输入焦点。F10 或 Alt+F/E/C/V/W 打开顶部菜单；打开后左右键或悬停切换菜单，上下/Home/End 导航，Enter/Space 执行，Esc/Tab 关闭并返回原焦点。快捷键列与分隔线来自同一菜单模型。首项始终不绘制组间分隔线。菜单尺寸通过独立的 CPU 测量会话计算；勾选或可用性更新在尺寸不变时直接更新当前输入表面，保留悬停、焦点、滚动和行身份。窗口外点击只关闭，不执行背后的编辑器命令；视口和场景快捷键在菜单期间暂停。

顶部菜单在未展开时不因 hover 改色；按下和展开的标题，以及展开菜单中当前指向的条目，共用 `#b36600` 橙色底面。菜单背景为 `#3a3a3a`，细框上/左为 `#949494`、下/右为 `#262626`。条目最低高 25⅓ DIP，快捷键为右对齐的灰色斜体，分隔线位于组间留白中央。PopupHost 用同一个 SS/文字测量入口计算标题与快捷键两列的宽度；短菜单不留空滚动槽，超出屏幕高度时预留实际滚动条空间。

菜单关闭后，标题恢复透明底面和普通文字颜色；保留的逻辑焦点不产生灰色 active 底面。标题的橙色状态仅由按下和实际展开驱动。

共享滚动范围计算抑制小数行高累计产生的浮点尾差，防止刚好容纳内容的菜单出现几乎不可滚动的滚动条；真实溢出仍保留。UI 端到端验收覆盖四项菜单的初始测量、像素尺寸窗口、滚轮和末项导航，以及 100%/125%/150%/175%/200% DPI。

弹出窗口基于锚点显示器的工作区定位，可超出编辑器边界；下拉空间不足时向上翻转，超宽/超高按可用空间限制并滚动。主窗口移动、尺寸/DPI 改变、失焦、系统取消或捕获丢失关闭菜单；关闭编辑器时先关闭弹出窗口的 GPU 资源，再销毁 HWND。它不创建第二套字段编辑或场景历史。

当前 renderer/RHI 的所有权仍是一窗口一上下文，主菜单及首层子菜单宿主随应用初始化，其余层首次使用后复用，隐藏时不渲染，静止时不重新提交，主窗口负责帧节奏。跨窗口共享 GPU 设备和纹理缓存尚未实现；多显示器真实 DPI 迁移需要额外设备验证。级联子菜单复用保留的原生菜单窗口，详见下方[页签菜单与弹窗资源](#页签菜单与弹窗资源)。定位依据见 [Microsoft 多显示器指南](https://learn.microsoft.com/en-us/windows/win32/gdi/positioning-objects-on-multiple-display-monitors)，焦点与捕获遵循 [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture) 的前台窗口限制。

主窗口统一消费线程消息队列，每次分发后处理自身事件记录；弹出窗口只处理已由该消息泵或同步回调排入的自身记录和回调错误，业务消费者始终在 WndProc 返回后执行。

## UI 样式与诊断

### 参数面板组合

参数标签沿 Houdini 习惯由字段名生成：取最后一段，下划线分词并首字母大写（`material.base_color` → `Base Color`，项目源码字段 `moving` → `Moving`）；字段路径和绑定身份不变。颜色元组（基础色、自发光、灯光颜色）末尾带当前值色块，HDR 值仅在色块上截断到 `[0,1]`。组件分组与变换分组共用整宽折叠条。

路径栏显示 World 上下文；固定对象标题用类型图标、类型和名称输入框表达当前 Actor，右侧放大镜控制参数搜索。名称是 editor::Session 的文档标签，通过对象身份索引，重命名、删除恢复和撤销重做共用场景历史；标签本身不持有运行时对象，GC 完成后清理失效索引。名称随场景 JSON 持久化，不是新增的 World 反射字段。

路径栏的下拉通过同一原生菜单选择 World 或 Actor；前后箭头浏览最近的选择。导航历史属于 Workspace，最多保存 64 个不保活的身份，保留 World 入口并清理失效对象；导航调用现有 Session::select，不增加场景撤销条目。路径框、普通输入框、内容页签和滚动条的像素色值与共享样式扩展见[细节校准](../../research/houdini-chrome-pixels-20261002.md)。

Parameters 展示所选对象的连续属性区，不预设 Transform／Components 内容页签。选择 Actor 时展示 Actor 自身字段和所拥有组件的属性分组；选择组件时只展示该组件，标题中的组件类型只读。Actor 名称仍可编辑。搜索默认收起，点击名称右侧的放大镜在标题下方展开，并在首个接受帧后聚焦；收起停止筛选、归还行高并保留查询文字。查询筛选实际字段与类型分组，不改变组成或撤销历史。结构增删只在 Actor Structure 提供。布局使用 UI DSL 的 PropertyGroup／PropertyRow，数值、勾选、枚举复用共享参数行；枚举只显示元数据中的命名选项，不接受任意整数。图标来源见[参数面板与图标研究](../../research/parameter-header-icons-20261006.md)。

Actor Structure 是可移动、关闭、重开的独立子页：Actor 作为父行，组件按拥有顺序列为子行，根组件标为 Root。它表达组成归属，不冒充空间树或 Houdini SOP 网络。选择组件保留 Actor 上下文，视口和 Scene Tree 仍指向其拥有者；Delete 删除检查目标，删除组件后回到 Actor，Undo 恢复组件身份与选择。树行、属性分组和字段分别持有稳定 UI key，避免两个可见页对同一对象复用控件身份。当前没有重排、空间换父或跨面板拖放手势。

### 类定义、实例与默认值

目前支持原生及 Source 类定义，实例通过同一个 Runtime 创建，字段声明默认值参与初始化。Scene 持久化限定类型名和实例值。当前没有 Class 资产、组件模板或显式 override 集，也没有模板默认值编辑器。选择场景对象时 Parameters 编辑当前实例；选择 Asset Browser 条目时显示资源类型、资源键、位置、文件大小及已知网格统计，资源不会冒充 Actor 或可编辑类模板。返回 Scene Tree、路径导航或视口操作后恢复实例检查。两类查看共享参数页和 DSL 控件身份分配入口，不增加场景历史。

现有值与默认值的比较仅用于显示修改状态。没有声明默认值的原生字段不假定为零，也不伪造恢复默认值能力。修改 Source 字段默认值需重建类型，已保存实例值会恢复，不自动随新默认值传播。未来 Class 资产必须先明确持久身份、组件模板、覆盖和传播规则，再复用同一个检查入口。


Controls 的 Parameters / Appearance 使用同一组合，Appearance 内的 Color / Channels 是独立嵌套组：RGB 元组、三条逐通道滑条和色块消费同一组样本值，支持折叠、禁用、默认值及样本撤销。所有行、组和伸展继续使用现有 row/column/Flex，未引入第二套布局器。

折叠标题以常规字重显示，鼠标悬停使用 `#4f4f4f` 灰底，按下使用 `#464646`；展开状态由箭头方向表示。焦点仅轻微提亮文字，不增加边框或黄色下划线，Enter/Space 仍可切换展开状态。

全局交互样式区分按下、选中与焦点：普通按钮和列表用灰色底面反馈，菜单栏展开标题及当前菜单条目用橙色底面反馈；普通控件获得焦点不增加外框。焦点通过文字或控件标记的亮度表达；文本和数值编辑器保留原宽度的细灰色边界，非法输入仍使用红色。鼠标点击保留键盘焦点，Tab、方向键、Enter/Space 和菜单返回继续使用同一焦点状态。

Tab 标签悬停不改变背景，统一使用正体。列表有带握柄和双步进按钮的滚动条，支持拖动、翻页和滚轮；字段不会伸到滚动条下方。数字调整、文本、分隔条、停靠拖动与视口观察使用对应鼠标指针。研究依据和未覆盖能力见[参数组合研究](../../research/houdini-parameter-composition-20261002.md)；完整 Ramp、色轮弹窗、动画/表达式编辑仍未实现。

Workspace 声明 `normal/panel/title/caption` 角色、enabled/selected 等语义状态及菜单栏、参数区域等类名。基础主题和 [editor.ss](../../../assets/editor.ss) 通过同一 UI 解析得到最终样式；应用启动时从可执行文件旁读取 SS，非法内容报告行列。对象类型使用 object-type 样式，说明和状态文字使用 caption；容器结构 padding 留在 NodeInput，文字内容内距由最终样式提供。菜单栏标题宽度按文字测量并保留最小宽度。样式覆盖、图片/圆角和相邻合批合同见 [SS 文档](../ui/style-sheets.md)。
主窗口和原生菜单通过同一 `editor_fonts()` 加载 Sarasa UI SC，数字使用 Source Code Pro；额外缺字及 emoji 沿系统回退，应用不修改系统字体设置。构建部署 `fonts/` 和两份 SIL OFL 许可证；选择依据、字重、体积与来源见[字体说明](../../../assets/fonts/README.md)。窄面板继续同一 Flex 换行。
`Application::set_style` 直接进入 UI 的整体校验/应用路径，应用不保存另一份主题。颜色复用文字 Coverage，字体族、字重和字号经同一 TextStore 更新度量、caret 和栅格；已接受帧仍保有旧几何与不可变内容。具体样式、版本及状态合同见 [UI 模块](../ui/README.md)。
`Application::sample_frames` 显式开启 CPU prepare/render 计时，默认关闭；Capture 帧不计入。窗口验收从同一 Create 菜单添加真实场景成员至 256，再采样列表滚动与位置拖动。主画布 UI 累计计数按段求差，主 renderer 的 `FrameResult::stats` 按实际采样帧合计；弹出窗口的独立上下文未计入这些既有计数。CPU Coverage 和 renderer cache 的持有峰值分别记录。
render 计时包括 Present/等待，R8 上传字节只表示有效 payload，缓存 texel 字节不表示物理显存。验收使用硬件 D3D12 debug/GBV 与 vsync，含 release 配置；数据用于说明该验收负载，不能直接当作关闭验证的产品帧耗时，也没有同负载改前基线可用于百分比提升结论。

2026-10-01 独立菜单增量：dev/release UI 15/15 通过；正式窗口完整链分别接受 2632 / 2631 帧并 PASS。新增断言真实 owned WS_POPUP（非子窗口）、跨编辑器边界点击、显示器工作区约束、F10/Alt 快捷入口、左右键/悬停切换、宿主复用、焦点/捕获清理，以及原参数菜单和下拉业务操作。报告及独立 GPU 截图归档于 `out/native-menu-evidence-20261001/{dev,release}/`。开发版通过后仅调整了发布验收中完整拖动的消息投递时序和接口注释；最终两种配置均重新编译。本轮未运行真实搜狗验收。

## 帧与关停

应用在 renderer 的最终结果成功且确认 accepted 后才切换命中帧，skip 保留旧帧。
accepted extent 固定为录制/呈现时尺寸，frame末尾resize后的新尺寸仅用于下一帧。
尺寸/DPI过渡期取消press/capture并暂停指针命中，释放/失焦/捕获丢失继续处理；匹配帧接受后恢复。
上传也是录制；录制开始后的错误没有回滚，终态错误停止新交互与后续帧。普通帧不调用finish，诊断截图才显式同步。
关闭停止输入并取消编辑，释放renderer拥有的资源后关闭RHI，最后销毁窗口；成员/历史根先于Runtime析构。

## 验收入口

`editor_dialogs_acceptance` 使用正式编辑器菜单打开 About、文件选择、工程目录选择，以及 Examples 子菜单触发的未保存场景确认。它检查原生窗口响应、模态期间主窗口不再持有鼠标、取消后输入与接受帧恢复、场景和撤销历史保留；看门狗对无响应给出有界失败。单独入口为 `atelier_editor --acceptance-dialogs <输出目录>`，不注入桌面键鼠，报告写入 `dialog-evidence/dialogs.txt`。仅检查 WM_NULL 响应不足以识别输入被主窗口捕获的问题，因此同时读取线程的原生捕获状态。

`editor_document_acceptance`消费正式 World/Session，覆盖空 Actor、实际组件增删、单次字段历史/控制会话、完整 TRS undo、成员资格、删除跨 GC 的同身份恢复、历史裁剪普通根保组/最终收集及关闭释放。旧 Scene 库和位置专用入口已删除。
`atelier_editor --acceptance <输出目录>`运行同一正式窗口，用Win32消息和系统焦点/捕获操作驱动控件，再核对Runtime状态与GPU帧。
`atelier_editor_document_acceptance --viewport-edit` 覆盖真实网格求交、多选/框选、G/R/S、非法输入、整组回滚、撤销、数学元组布局及资源检查。`atelier_editor --acceptance-edit <输出目录>` 驱动独立原生窗口，覆盖手柄拖动、快捷键、参数联动、捕获取消、数值显示、欧拉角连续输入及资源/实例切换，导出 GPU 截图。组合构建目标为 `editor_edit_acceptance`；窗口部分需要前台焦点。
2026-10-07 的范围、修正及证据见[视口编辑验收记录](../../research/editor-viewport-edit-20261007.md)。
`--viewport-edit` 另覆盖 A/Alt+A/Ctrl+I、Alt+G/R/S 单次历史、表面游标朝向、G/R/S 中键选轴、添加/饼菜单内容和框选相机距离；复制验收比较完整场景序列化结果、组件关系、物理配置和共享网格，检查组合撤销、取消、空选择和容量拒绝。窗口验收覆盖经 `WM_SYSKEYDOWN` 的 Alt 组合键与模态读数栏截图 `modal-readout.png`。

`editor_shortcuts_acceptance` 先执行上述文档链，再通过独立真实编辑器的统一输入入口验收视口两轴分隔条的连续缩小/放大、跨入视口内容时的捕获保持、松手提交与取消，以及六向视图、视角饼菜单、Ctrl+Numpad 相反视图、Shift+D、X、Z、快速松键、文本焦点隔离、原生菜单复用和 Persp 状态。菜单链检查方框列连续勾选后保留同一原生窗口并刷新状态、文字点击关闭，以及 Cavity 开关和参数子菜单；这些操作不改变场景历史和游标。它不注入桌面键鼠、不要求前台；GPU 截图、菜单 CPU 耗时与逐移动的 `splitter-drag.csv` 写入构建目录的 `shortcut-evidence/`。真实操作手感仍以交互使用为准。

本轮集成的 Dev / Release 编译、World、场景/JIT、材质及原生窗口快捷键/弹窗尺寸端到端均通过；包含 FPS 开关、采样、重复设置、被动文字绘制与关闭清理。发布版完整编辑器文档链及地牢的 2332 个模型、30 张共享图集、三个场景保存重开也通过。日志为 `out/stage-{dev,release}-{world,scene,materials,shortcuts,surfaces}.log`，发布版完整链为 `out/stage-release-{documents,dungeons}.log`；FPS 截图与逐移动记录在 `out/stage-{dev,release}-shortcuts/`。渲染效果与 GPU 资源验收见[渲染验收](../renderer/acceptance.md)。
`editor_surfaces_acceptance` 沿正式 Application、UI 输入和 GPU 帧验证不同尺寸菜单复用同一窗口且仅需一次 Present、两轴分隔条的延迟尺寸提交、Esc/失焦/丢失捕获及迟到松键；也覆盖 Asset Browser 目录分隔条的扩大/缩小、边界、取消和场景历史隔离，确认目录调整不改变 viewport 尺寸。它不注入桌面键鼠；`surface-evidence/surfaces.csv` 保存每次打开的提交次数、CPU 耗时与整个 step 耗时，PNG 保存拖动预览和提交后的画面。耗时包含当前机器负载，不代表输入到显示器的端到端延迟。

2026-10-11 主目录 `72843f7` 加本轮修改：目录分隔条 Dev/Release 端到端通过，日志为 `out/browser-splitter-{dev,release}.log`，对应目录中的 `asset-directory-width.png` 保存调整后的画面；最终 Release 原生对话框复验通过，记录在 `out/native-dialogs-final-release.log`。渲染边线的专门复现与验证见[渲染验收](../renderer/acceptance.md#选中边线深度一致性2026-10-11)。

2026-10-11 的 DDGI 整合验收通过当时的 Scene Lighting → DDGI Debug 菜单点击 Probe Spheres 互斥项与 Volume Bounds 勾选项（当前入口已迁至 World → Diagnostics），确认显示设置生效且不写场景历史。Dev 与独立 Release 均通过，菜单截图和日志在 `out/ddgi-integration-20261011/surfaces-{dev,release}/` 及对应 `.log`。

2026-10-12 相机手势与 Control 联合验收：通过真实 Application 输入及命名管道复现松手后残留 `camera_pan` 的拒绝，失败证据为 `out/camera-control-red.log`。相机状态合并后，Dev/Release 的同一入口均通过，覆盖 Shift+中键平移、中键旋转、Alt+左键旋转，以及正常松手、失焦、捕获丢失、转入菜单四种结束路径；还检查活动期间拒绝请求、非所属按键释放、迟到移动、原生捕获释放及场景历史隔离。证据在 `out/camera-control-{dev,release}.log` 和对应目录的 `surfaces.csv`。标准 `out/release/atelier_editor.exe` 已重新构建，包含 DDGI 整合与相机修复；旧进程继续使用其启动时加载的程序。
`atelier_editor --snapshot <输出目录>` 是设计评审用的截图入口，不是验收：窗口以不激活方式显示，不抢占前台、不发送系统输入，直接调用文档与视口工具入口，导出各工具手柄、表面游标、模态 G/R 的 GPU readback PNG。可在他人使用桌面时运行。
`atelier_editor --acceptance-project <输出目录> [源工程目录]`在同一个正式窗口内先执行共享视口模式／TRS 验收，再接续工程操作链，输出目录下每次新建独立的 `run-*` 证据目录。提供源工程时，仅复制 Config／Content／Source 到验收目录；跳过链接和 Cache，所有编辑、导入、保存和目录搬移发生在副本。输出目录必须位于源工程之外。省略源工程时创建空工程 fixture；CMake 的 `editor_project_acceptance` 使用这一形式。窗口输入仍要求编辑器保持前台，外部焦点丢失会停止输入。
窗口链包含分隔、增删/历史、列表选择、实时拖调/一次undo/Esc、文字Delete、非法值、相机、失焦/捕获、resize、最小化和正常退出。
S1 覆盖正常/hover/pressed/focus/invalid/disabled、颜色改变的同Coverage/零raster/零上传、非法Style整体拒绝、字体family/weight/size及caret同步失效、旧帧几何/像素寿命。256真实成员由 Create 菜单加入，验证屏外控件几何仍在、文字不生成、滚回恢复，以及连续拖调历史。
S2 扩展覆盖本地/场景历史归属、非法 Enter/保留焦点、文本模式拖选、真实参数过滤、代理对/组合字符/ZWJ 删除、双向选区、CF_UNICODETEXT 读写。用真实剪贴板占用验证失败保留草稿；预先建立真实 Session 事务，使随后原生拖动的 begin 失败，验证后续 preview 停止、回滚/捕获释放和首错保留。
截图来自 renderer 实际颜色/深度 readback，输出 `m0.png`、`editor.png`、`s1-pressed/focused/invalid/disabled/color/font/list-256.png`、`s2-search-unicode/bidi-selection/number-selection/failed-edit.png` 与 `acceptance.txt`。首版和已收口 S1 证据分别保存在各配置的 `editor-evidence-v1/`、`editor-evidence-s1/`；被后续运行覆盖的工作目录不作为 S1 原始证据。
DPI 验收覆盖当前显示器上的 WM_DPICHANGED 过渡政策，不据此声称已验证跨不同 DPI 显示器迁移。Unicode WM_CHAR 和 UI 组合协议注入不代表真实系统输入法通过。

### S2 集成状态与输入法验收

本节是 World 迁移前的已验收记录。当前 `Window`/共享文本后端未修改，本轮不重复未受影响的真实 IME 验收；组件字段接入仍通过普通窗口的 Unicode、剪贴板和目标失效链检查。

2026-09-29：共享 UI 提交 `60472ed` 已集成为 `0e45c6a`；当时 dev/release 的 UI 12/12 通过，实际文字服务报告 ICU 72.1 / Unicode 15.1。`afb2286` 的应用平台接线与验收驱动在 dev/release 编译通过，顺序运行 `editor_acceptance` 均通过：Scene PASS、窗口各接受 709 帧，含原有场景/窗口链及 S2 非 IME 操作。该版本原始报告和截图另存于 `out/{dev,release}/editor-evidence-afb2286/`，与后续 IME 工作目录分开保留。

组合起点快照 `4de055a` 已集成为 `7cd9c28`，同一集成树 dev/release UI **13/13** 通过。最终 Window 包含 IMR_QUERYCHARPOSITION 单起点应答、原生边界立即失效及输入绑定更换的上下文生命周期。目标失效/更换时依序取消、解绑旧上下文，发布新绑定，再恢复原默认上下文；原生事件捕获时保存 token，取消/解绑的同步回调仍归旧目标。正常确认的 revision 推进不解绑。最终产品与验收驱动已完成双配置编译和以下独立实测，证据归档于 `out/{dev,release}/editor-evidence-s2/`，不复用旧版本报告。

| 最终验收 | dev | release |
| --- | --- | --- |
| 本机搜狗真实输入 | PASS；starts/results 各3，candidate changes 57 | PASS；starts/results 各3，candidate changes 57 |
| 原跨字段 reading 回归 | query=`位置 `，X=0，business editing=0 | query=`位置 `，X=0，business editing=0 |
| 原线程 profile 恢复 | verified=1 | verified=1 |
| 普通窗口全部链路 | PASS；709 accepted frames | PASS；709 accepted frames |

消息验收 helper 显式设置本次 Ctrl/Shift 并清除未请求修饰位，覆盖修饰位继承和意外失焦中断；生产失焦取消语义保留。最终双配置在稳定前台时段顺序完成，没有干预其他进程。

本机搜狗真实输入使用同一 `atelier_editor --acceptance-ime <输出目录>`，CMake 入口为：

```powershell
cmake --build --preset dev --target editor_ime_acceptance
cmake --build --preset release --target editor_ime_acceptance
```

这两个目标须依次运行，并保持编辑器前台且无物理修饰键按住。驱动保存原线程 profile，必要时仅在本线程选择已启用搜狗，退出时恢复并核对原文本服务或键盘布局；不修改用户默认、输入列表或启用状态。实际输入使用 SendInput 扫描码，系统生成的 IMM32 快照进入正式共享编辑路径。GPU 截图保存应用预编辑文字，`s2-ime-native-candidate.png` 保存编辑器所在桌面区域的原生候选窗；`ime-acceptance.txt` 记录系统组合/结果/候选通知及结果。

以下是 2026-09-29 的固定环境和历史证据，不表示当前机器配置。该次按用户指定使用搜狗，Windows build 为 26200.9457 / 25H2，`C:\Windows\System32\SogouTSF.ime` 的 FileVersion/ProductVersion 均为 16.8.0.5415；TIP 为 `{E7EA138E-69F8-11D7-A6EA-00065B844310}`，profile 为 `{E7EA138F-69F8-11D7-A6EA-00065B844311}`，语言为 `0804`。当时输入列表是 zh-Hans-CN 搜狗与 en-US 美式键盘，默认覆盖为搜狗。后续实机验收须重新记录实际环境。

此前为微软拼音临时设置的用户 enable 位及追加可选项已通过原受支持 API 撤回，未覆盖整份用户设置；`out/pinyin-profile-reverted.txt` 与 `out/pinyin-environment-reverted.json` 记录 enable=0、列表顺序及搜狗默认与操作前一致。这不是微软拼音验收通过的证据。当前输入生命周期合同见[共享单行编辑](../ui/README.md#共享单行编辑)与[平台交接](../ui/README.md#剪贴板组合与平台位置)。以下结果须分别看待：

- 真实 dev 运行已通过位置应答的屏幕坐标/非法请求检查。原生截图 `out/dev/editor-evidence/s2-ime-native-candidate.png` 确认 WEIZHI 的首候选为“位置”；物理 Space 已通过真实 START → GCS_RESULTSTR → END 将“位置”插入一次，`s2-ime-confirmed.png` 与 `out/dev/sogou-real-result.log` 保存确认结果。候选阶段可只有 VK_PROCESSKEY、IMN_CHANGECANDIDATE 和起点查询，没有 GCS 组合；不以 Session::composing 判断搜狗 reading 是否存在。
- 加强生命周期检查后，在 X=0 的数值字段输入 WEIZHI，切到原查询为“位置”的搜索末端，再按 Space，实际变成“位置位置”。同一搜索字段的干净状态 Space 基准是“位置 ”。最新有界诊断存于 `out/dev/sogou-rebind-diagnostic-20260929/`：旧/新 token 转换正确、同一 HIMC/open=1，CPS_CANCEL 确实调用并返回成功；切换前后 starts/results 均为3，Space 后才变4。Space 前原生候选仍可见；最终 expected UTF16 `4f4d 7f6e 20`、actual `4f4d 7f6e 4f4d 7f6e`，X=0、business editing=0。该次证据排除取消分支未执行及取消期间新 start 排队错绑，表明 pre-START reading 仍存活。
- 加入解绑/恢复后的版本在 dev/release 均通过原真实回归：切字段/失焦/Surface/目标变化后，Space 与同一 IME 的干净状态基准一致。字段内容、旧对象值和业务 editing 分别断言并记录；不以非空候选缓存或结果消息计数增加判失败。候选文本枚举已移除，仅用候选数量观察准备阶段、按本机已观察首候选送 Space。最终 `ime-platform.txt` 的跨字段 expected/actual 均为 `4f4d 7f6e 20`；`ime-acceptance.txt` 与 `acceptance.txt` 分别记录真实搜狗和普通窗口 PASS。最终归档不包含旧失败截图；诊断原始证据单独保留。

驱动只请求一次前台并等待用户切换，随后失焦即停止，不反复抢焦点。临时生产 DEBUG、逐键跟踪、有界取消快照及诊断分支均已移除，保留必要原生计数、失败分类和正式回归。`out/sogou-environment-final.json` 与实测前记录核对，输入法列表/顺序和搜狗默认未变。系统 IME、协议注入、静态环境检查分别记录；此次仅证明当时搜狗 16.8.0.5415 及已观察候选顺序，不代表其他 IME。S2 已在该范围内收口；后续编辑器能力以本模块和当前声明式 UI 文档为准。

### S1 真实负载采样

以下是已收口 S1 的同一次 release 窗口验收在 AMD Radeon RX 6800 XT、debug/GBV 与 vsync 开启时的结果。原始数据为 `out/release/editor-evidence-s1/s1-performance.csv` 和 `s1-resources.csv`；dev 归档目录保留相同格式。没有同负载改前数据，不声称性能提升比例，也不把这组数据当作 S2 测量结果。

| 256成员负载 | 样本帧 | prepare CPU 均值 / p95（ms） | render CPU 均值 / p95（ms） | 栅格次数 / R8字节 | 上传次数 / 有效R8字节 |
| --- | --- | --- | --- | --- | --- |
| 列表滚动 | 48 | 0.235 / 0.342 | 15.346 / 17.422 | 232 / 370890 | 490 / 780588 |
| 连续位置拖动 | 64 | 0.228 / 0.326 | 16.080 / 16.944 | 64 / 143427 | 64 / 143427 |

滚动/拖动分别累计 UI draw 4245/5632、场景 draw 48/64。采样时 UI CPU Coverage 持有峰值分别为277项/451706字节、277项/457032字节；renderer cache峰值分别为33项/65354 texel字节、32项/64488 texel字节。CPU像素统计不含DWrite对象等开销，cache统计不含gfx待退休资源或物理显存。
滚动中累计剪除11716个屏外文字控件生成，拖动中剪除15616个；滚回可能重新上传是现有帧末GPU缓存淘汰行为。真实业务参数仍只有XYZ，数据不代表高密度参数面板已经验证。

## Scene Tree

Scene Tree 使用共享 `tree/tree_item`，投影 World → Actor。行标签同时显示实例名称和 Runtime 发布的实际类型；World、Actor、StaticMeshActor 使用不同的原生矢量符号和颜色。组件组成在 Actor Structure 中显示，不进入 Scene Tree。当前 World 没有跨 Actor 父子合同，因此成员平铺在 World 下，不用组件空间关系伪造 Actor 层级。行绑定实际 ObjectId，视图状态不持有业务根，不增加场景撤销。

参照 Houdini 22.0.429 的 Tree View：20 DIP 等高行、18 DIP 逐层缩进、空白区连续交替底色、细层级连接线、圆形加减按钮、类型图标和底部 Filter。全行选择采用灰色，遵循当前编辑器无黄色焦点框的约定。树节点的展开状态跨停靠、隐藏及重开恢复；每份接受帧只保存一次，绑定退休时同步删除其展开记录，避免旧帧重放和逐条反查绑定。底部过滤保留祖先并临时展开结果，清空后恢复手动折叠。

上下/Home/End 导航并显露选中行，左/右键回父层、折叠和展开。Ctrl 切换 Actor 选择，Shift 根据当前接受帧中的 Actor 行选择范围；选择与视口共用 Session 的有序 Actor 集及活动 Actor。复用当前路径栏、字号、滚动条、输入捕获与接受帧；没有添加虚构父子或拖拽重挂。

共享 UI 端到端包含四档 DPI、过滤、长树滚动、禁用祖先、退休和失效命中；真实窗口验收新增 `tree-component-selection.png`、`tree-filtered.png`、`tree-view.png`，随完整窗口链执行。2026-10-03 开发版与发布版完整窗口链均通过，最终清理候选报告分别为 `out/dev/ui-stage-evidence/acceptance.txt`、`out/release/ui-stage-evidence/acceptance.txt`；共享 UI 16/16 与文档消费者同样通过。普通窗口验收仅在启动时等待一次前台激活，开始输入后失焦仍立即中断。本轮未运行搜狗验收。

## 页签菜单与弹窗资源

页签下拉菜单提供 Set Pane Tab Type 子菜单、前后页签、关闭页签、恢复最近关闭、最大化、关闭整个 pane 和左右/上下分割。已注册视图保持单实例；更换类型交换已有视图的位置，关闭后重开保留该视图状态。加号负责向当前组加入视图。浮动窗口、复制视图尚未实现，菜单不提供无效动作。

菜单描述可嵌套。每层是保留的原生无激活窗口，主窗口保留键盘焦点与 capture；点击或 Right 打开子菜单，Left/Escape 回到父菜单。选择叶命令关闭整条菜单链；点击可勾选叶项的方框列则保留当前链，便于连续设置。改变根来源、失焦、关闭和改变 surface 时清理子菜单路径。每层按实际文字、DPI、样式和屏幕工作区测量，短菜单不保留滚动槽。

应用初始化主菜单及首层子菜单宿主；Renderer 仅在实际网格绘制时创建场景管线和深度资源。隐藏菜单折叠已有控件，保留文字缓存；相同描述重开复用尺寸，不再先缩到 32×32。验收输出 `popup-open.csv` 记录当前首次打开和暖重开在更新/呈现路径上的 CPU 耗时，不包含启动期 Context 初始化，也不包含截图读回；不据此声称已经测量旧版本加速比。

菜单栏和 Combo 在鼠标按下时请求展开，弹窗接管后续松键，不再等待一次完整点击。列表弹窗在主视口提交/Present 前更新与呈现，避免再等待一轮场景渲染；仍只接受与原生客户区匹配的 GPU 帧。PopupHost 在窗口呈现前通过 `Renderer::sync_window_target` 同步实际客户区尺寸，沿 RHI 原有排空和资源退休机制调整交换链；菜单直接准备并呈现正确尺寸的帧，移除了先呈现旧画布再重试的循环。同尺寸为空操作，隐藏窗口仍可准备尺寸，菜单输入继续只使用已接受的帧。这个调度调整不改变渲染器本身的重场景耗时。
