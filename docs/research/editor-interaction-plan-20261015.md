# 编辑器交互补全规划 — 2026-10-15

状态：规划，首批已落地（见末尾）。参照：用户本机 Houdini 22.0.429 与 Blender 4.5.14 LTS 的同屏截图（只读观察，不发送输入），及用户提供的 Blender 弹出面板截图。

## 1. 视口三条功能栏（Houdini）

Houdini 场景视图有三条互不重复的栏：

| 栏 | Houdini 内容 | 本编辑器现状 | 规划 |
| --- | --- | --- | --- |
| 顶部操作栏 | 当前工具的选项：选择模式、拾取类型、安全/锁定、相机/视图链接、显示选项入口 | 视口头部（Blender 语义菜单）+ 28 DIP 操作栏 | 保持：头部放语义菜单与显示，操作栏放当前工具选项 |
| 左侧工具栏 | 选择 / 安全选择 / 变换手柄 / 吸附 / 相机 / 渲染区域 | 选择、通用、移动、旋转、缩放、吸附 | 已对应；后续加相机工具与长按弹出组 |
| 右侧显示栏 | 显示选项：参考平面、线框/着色、可见性（隐藏/显示）、灯光开关（无/头灯/普通/高质量）、背面、点/法线/编号显示 | 无 | **首批落地**：右侧 32 DIP 显示栏，参考网格、原点轴、线框叠加、三角边、Cavity、四种着色、FPS 与光照单独切换，全部复用既有 ViewOption 与命令 |

右侧栏只承载“看”的状态，绝不修改场景历史；它与头部着色段、Z 饼菜单、Guides 菜单共用同一 ViewOption 路由，任一入口修改后其余入口同步。

## 2. Blender 弹出面板（popover）

Blender 的吸附、变换朝向与视口着色均为“标题 + 分组 + 分段按钮 / 图标列表 + 勾选”的富面板，而不是线性菜单。本编辑器已有 PopupHost 与 DSL Popup，规划：

- **变换朝向**（Global / Local / Normal / View / Cursor / Parent）：图标列表，单选。首批先接入 Global 与 Local（缩放手柄已沿局部轴），其余作为后续语义扩展，未实现项不显示。
- **吸附面板**：Snap Base（Closest / Center / Median / Active）、Snap Target（Increment / Grid / Face …）、Affect（Move / Rotate / Scale）、Rotation Increment。首批落地：Snap Target = Increment/Grid 已有；增加 **旋转吸附步长**（5° / 15° / 45°）与 **Affect** 三项开关，接入 ViewportTool 的旋转与缩放手势。Vertex / Edge / Face 吸附依赖网格查询，列为后续。
- **视口着色面板**：Lighting（Studio / MatCap / Flat）、Wireframe Color、Background、Options（Backface Culling、Outline、Cavity、X-Ray）。现有 display 菜单内容按此分组重排为富面板是第二批；首批先由右侧显示栏承载开关。

富面板统一实现方式：一个 `PopoverPanel` DSL 组件（标题、组标题、分段 ChoiceButton 行、图标列表行、勾选行），由 PopupHost 承载，与菜单共享焦点、Esc、外部点击关闭规则。

## 3. 场景树与资源管理器

Houdini Tree View / Blender Outliner 的必要操作，按优先级：

| 操作 | 规划 | 首批 |
| --- | --- | --- |
| 拖动行到另一行 = 组合换父，拖到 World = 解除父级 | 复用 `Session::reparent(keep_world)`，一次历史 | ✅ |
| F2 / 双击重命名 | 复用 `Session::rename` 与对象头的名称字段聚焦 | ✅ F2 聚焦名称 |
| Delete / X 删除，Ctrl+D / Shift+D 复制 | 复用 erase_selected / duplicate_selected | ✅ 树获得焦点时生效 |
| 右键菜单：Rename、Duplicate、Delete、Focus、Unparent、Select Children | 复用命令模型 | ✅ |
| 可见性 / 可选性开关（眼睛、箭头） | 需要 World 层的显示与可选状态，不只是 UI | 后续 |
| 资源：重命名、删除、在资源管理器中显示、复制路径、新建文件夹 | 需要 Project 文件操作 API 与引用检查 | 后续（先补 Show in Explorer / Copy Path，无破坏性） |
| 资源拖入视口放置 | 依赖跨面板拖放协议 | 后续 |

## 4. 参数控件

| 控件 | 规划 |
| --- | --- |
| 颜色编辑器 | 点击色块弹出：HSV 色盘（SV 方块 + H 条）、RGB/HSV 数值、Hex、原色/新色对比；拖动为一次预览事务，确认一次历史，Esc 恢复。**首批落地 HSV + RGB + Hex 弹出编辑器** |
| Ramp | 后续 |
| 角度/方向球 | 后续 |
| 文件/资源选择器 | 已有资源下拉，后续加缩略图 |

## 5. 响应速度

UI 帧只在输入或数据变化时重建；后续对 Source/JIT 绑定求值、菜单测量与树行重建做实测（`atelier_editor --snapshot` 与 World Performance 表），只对测得的热点优化，不预设结论。

## 首批落地（本轮，2026-10-15）

- ✅ 视口右侧显示栏（8 个视图开关）。
- ✅ 场景树拖动换父（keep world、防环、一次历史、拖放高亮）、F2 / Delete / Ctrl+D / Alt+P 与右键行菜单。
- ✅ 颜色编辑器：RGB + HSV + Hex，Session 组编辑一次历史；HSV 色盘方块与 H 条需要二维拾取控件，留到下一批。
- ⏳ 吸附面板（旋转步长、Affect）与变换朝向富面板：下一批，与 `PopoverPanel` 组件一起实现。
- ⏳ 资源管理器的重命名/删除/新建文件夹：需要 Project 文件操作与引用检查，下一批。
