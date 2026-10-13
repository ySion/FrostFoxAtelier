# 控件比对与视口输入调整

## 观察范围

2026-10-12/13，使用桌面上的 Houdini Indie 22.0.429 比对参数页签、参数枚举、动作菜单、勾选项、数值输入、搜索展开和 Move 工具切换。打开菜单、切换勾选并复原、进入数值编辑，检查实际显示。用户提供的截图同时作为尺寸与层级参考。

本次没有测量 Houdini 的端到端输入延迟，也没有逐帧捕获它的全部按下状态。关于菜单性能的结论来自本编辑器的源码和验收记录，不推断 Houdini 的内部实现。

## 逐项处理

| 控件 | Houdini 中观察到的行为 | 本次处理 |
|---|---|---|
| 子区域页签 | 第一个页签从条带左边缘起排；选中页与内容区连接 | 去掉 Parameters、World 的 7 DIP 左内边距，继续使用共享 section-tab；不改 docking tab |
| 参数枚举 | 当前值可直接阅读，按钮右端是独立触发区，菜单紧接按钮 | 沿用 Combo 的统一测量和输入；参数菜单维持 26.667 DIP 高度及左右留白，使用单下箭头 |
| 动作菜单 | Modify Pre-Transform 下是动作列表，没有前置选择圆环 | 普通 command 不保留 radio/checkbox 标记槽；互斥、勾选仍按自身语义绘制 |
| 参数勾选项 | 勾选结果可辨认；文字与字段保持固定对齐 | 按用户明确要求保持 active/inactive 两态，不给整行添加 hover/press 底色 |
| 数值字段 | 三分量共行、标签右对齐、黑色字段和轴色底线；点击进入文字编辑 | 保留既有 Vector/Number 组合及输入链，避免为了视觉重新实现编辑器 |
| 搜索 | 放大镜展开标题下的查询行，关闭后收回占用 | 保留当前按需展开行为，不改成常驻按钮阵列 |
| 工具状态 | 切换 Move 后，图标、模式名与选项一起变化 | Fly 同步显示自己的图标、模式名、底部提示及被动中心准星，退出恢复原工具显示 |
| 工具指针 | 指针用于提示当前操作语义 | 同一 Cursor 入口区分环绕、手掌平移、移动、旋转、缩放；保留文字、数值调整及分隔条已有指针 |

菜单方框列连续点击保留同一原生窗口、行身份与高亮；文字点击继续激活并关闭。内容测量使用独立 CPU Session，避免测量尺寸临时重置 live Session。此次审查另外修复了 anchor 移动被幂等早退忽略的问题；父菜单的来源行滚出裁剪区后，其子菜单关闭。

## 图标来源与代价

选择 [Tabler Outline](https://github.com/tabler/tabler-icons) 的十个符号，采用其 [MIT 许可](https://github.com/tabler/tabler-icons/blob/main/LICENSE)，按许可随程序保留声明。没有使用 Houdini 图稿。具体上游提交、映射、SVG、PNG、生成工具与生成命令见 [图标说明](../../assets/icons/README.md)。

SVG 离线生成透明 PNG 和压缩 alpha，运行时复用 `ControlIcon → Symbol → Coverage` 及现有缓存。原生工具指针消费相同 alpha，浅色笔画带暗描边，以意图与 DPI 缓存。移除了被替换的工具数学图案及双色 cube 叠画；箭头、勾选等简单基础符号仍由原入口提供。

此方式没有新增运行时图片解码、资源路径查找或独立 atlas。只在图标尺寸/指针缓存未命中时生成像素或原生句柄；窗口销毁回收句柄。没有把这些结构性节省描述成未经测量的帧率提升。

## 输入与验收边界

- Z 打开着色饼菜单；S 保留缩放，反引号保留视角菜单。
- Shift+反引号进入 FPS 自由视角。WASD 移动、Q/E 升降、滚轮调速、Shift 加速；Enter/左键确认，Esc/右键取消。行为参照 [Blender Walk Navigation](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/walk_fly.html) 的无重力方式。
- 飞行复用 Camera；不会建立 Scene 编辑事务或修改 3D 游标。原始鼠标注册、隐藏、约束与捕获只在前台活动手势期间持有。
- 失焦、捕获丢失、窗口/视口变化、菜单接管、文档更换和关闭均清理手势；每帧时间上限避免暂停后突然跳远。
- 真实原始鼠标验收放在既有前台窗口入口；后台 shortcuts 保留不注入桌面输入的合同，并验证后台飞行请求无副作用。

当前快捷键和输入合同以 [编辑器模块](../modules/editor/README.md#视口编辑) 为准；此记录只说明本轮比对与取舍。

## 集成前 UI 验收（703b89e）

主目录 Debug（`out/dev`）与独立 Release（`out/dialog-release`）均完成构建；这轮 UI 验收未关闭或替换当时运行的、带未保存场景的 Release 实例。

| 正式入口 | Debug / Release 结果 |
|---|---|
| `atelier_editor_document_acceptance --scene-lit` | PASS：World 配置、持久化、撤销、DSL 发布与过期动作边界 |
| `atelier_editor --acceptance-viewport <输出目录>` | PASS：真实原始鼠标、六方向移动、Fly 生命周期、被动 HUD，以及原有视口显示验收 |
| `atelier_editor --acceptance-shortcuts <输出目录>` | PASS：Z 饼菜单、快捷键隔离、六向视角、吸附、连续勾选与分隔条输入 |
| `atelier_editor --acceptance-surfaces <输出目录>` | PASS：菜单高亮保留、按钮尺寸、参数状态、World 编辑和布局/捕获生命周期 |

两配置均记录：分隔条拖动中 8 个检查帧的 RT 重建为 0；5 次菜单重复打开的多余呈现为 0。这些计数不等于端到端输入延迟或通用性能基准。日志和原始 PNG 保存在主目录 `out/ui-input-{dev,release}-*`，其中 `viewport-fly.png`、`parameters-component.png` 用于视觉检查。

Astra 分别审查了 UI/DSL 菜单机制、视口输入生命周期及图标/指针资源路径。落地的结构修正包括子菜单定位早退、原生输入仅由前台持有、工具图标共用源与缓存、原生指针资源回收；最终代码未保留被替换的工具图案或 Shift+S 着色入口。

## 与渲染分支的主目录集成

将 UI 提交 `703b89e` 与渲染提交 `c868ef9` 合入主目录。共享文件保留精简的视口预览菜单和 World 的九输入 DSL 契约，World 不枚举灯光；直接光候选数经正式数值字段进入作者配置、撤销和 Scene v5。场景保全只有同一份实现。示例和模块文档同步更新已退役的阴影预算与菜单入口。

主目录 `out/dev`、`out/release` 均构建成功，并通过以下端到端入口：

| 正式入口 | 两配置结果 |
|---|---|
| `atelier_editor_document_acceptance --recovery` | PASS：已提交状态保全、原子写入与重开、缺失资产、幂等、IO 失败及原文件保护 |
| `atelier_editor_document_acceptance --scene-lit <官方示例目录>` | PASS：World 编辑、持久化、撤销、DSL、过期动作及 Scene v4/v5 兼容边界 |
| `atelier_editor --acceptance-surfaces <输出目录>` | PASS：菜单、参数、World、停靠与资源目录拖动、相机手势和 Control 生命周期 |
| `atelier_editor --acceptance-shortcuts <输出目录>` | PASS：Z 饼菜单、快捷键隔离、导航、吸附、FPS 显示与分隔条 |
| `atelier_editor_control_acceptance <editor.exe> <输出目录> --repro-lighting` | PASS：旧复现格式校验、迁移、拒绝后状态保留及新格式导出重开 |

首次 Debug surfaces 验收遇到两次管道调用之间状态令牌变化：客户端把合法的 `stale_state` 当作手势失效。验收客户端现仅对这一错误重新读取概要并有限重试，仍严格断言新鲜请求的 busy/idle 状态。生产 Control 的令牌与拒绝规则不变；修正后两配置 surfaces 均通过。初次日志保留为 `out/integrated-dev-surfaces.log`，最终结果为 `out/integrated-{dev,release}-surfaces-final.log`；其他构建、验收日志与 PNG 使用 `out/integrated-{dev,release}-*`。已检查合并后的 World Lighting 与 Parameters 实际截图。

Renderer、共享渲染契约与渲染构建定义与 `c868ef9` 一致，完整 GPU 组合验收依据该提交的双配置记录。本次没有再次注入设备故障，也未重跑前台真实鼠标 Fly 验收；后者沿用上一节的 UI 提交证据。地牢能量和部分地板 GI 支持缺口、Control 完整检查的 4 MiB 边界仍按对应模块记录保留，不计为本轮已解决问题。
