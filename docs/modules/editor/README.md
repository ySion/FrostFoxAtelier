# 首个场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。
本轮范围是[首个编辑器方案](../../first-editor-plan.md)的 M0 与 M1 核心操作：位置编辑、场景成员、历史、相机和窗口生命周期。
2026-09-28 在本地完整集成树的 dev/release 均通过 `editor_acceptance`；每次窗口链接受184帧，Scene消费者与正常关闭检查通过。
旋转缩放编辑、存档重开、JIT 单步、视口拾取、gizmo、节点画布和自由停靠仍在后续范围。

## 正式执行链

```text
Win32 窗口消息 → application::Window → UI 已接受帧命中/焦点/捕获
  → UI Action → editor::Session → scene::Scene → Runtime 字段
  → Workspace 控件描述 + 场景数值 → UI 帧 + renderer 方块实例
  → renderer 最终 accepted → UI 接受对应命中帧
```

[Scene](../../../src/scene/scene.hpp)批量注册 Vec3、Transform 和 SceneObject，缓存 `transform.position.x/y/z` 字段路径。
成员目录与历史只持 ObjectKey 和 ObjectRoot；位置唯一存于 Runtime，UI 与 renderer 的数值是呈现快照。
[Session](../../../src/editor/session.hpp)是业务编辑入口。一次拖调先保存目标根与起点，再连续修改真实字段；提交只记一次历史，取消写回起点。
删除先取得历史根，再移出成员目录；Undo 恢复同一 ObjectId，Redo 重放值和成员变化。历史最多128项，场景最多256个成员。
选择、渲染与编辑均验证成员资格，历史保活不表示仍属于场景。宿主在同一线程逐帧以32工作单位推进 GC。

## 参数和坐标

位置 X/Y/Z 默认0米，合法域为有限的 `[-10000, 10000]`。普通拖调每逻辑像素0.01米，细调0.001米。
范围、默认值、单位与拖调步长由[应用参数描述](../../../src/editor/session.hpp)绑定 Runtime 字段，布局与 Runtime 不承担编辑器政策。
每个数值控件 key 包含目标 ObjectKey 和轴；切换选择会更换控件身份，旧帧不能误改新对象。

- 左手坐标系，+Y向上，单位米；方块各轴范围 `[-0.5, 0.5]`，本轮只做实例平移。
- 默认相机 eye `(6,5,-8)`、target `(0,0,0)`、up `(0,1,0)`；使用 LookAtLH/PerspectiveFovLH。
- CPU 矩阵行主存储，行向量按 `p * world * view * projection` 计算，D3D 深度为 `[0,1]`。
- viewport 与 UI clip 使用窗口客户区左上为原点的物理像素。1280×720客户区内的 `{240,32,760,660}` 表示从 `(240,32)` 开始、宽760高660的视口。
- UI 根据实际 backbuffer 尺寸和 DPI 生成 draw/clip；renderer 不再次缩放。深度目标显式匹配实际 backbuffer。

## 首版操作

工具条提供新增、删除、撤销、重做、聚焦选择和重置相机；左侧列表负责单选，右侧参数区可拖动分隔条调整宽度。
数值控件支持拖调和文本输入；Esc 取消当前编辑。文本焦点下 Delete 只编辑文字，场景删除快捷键由未消费的命令事件处理。
右键拖动视口旋转观察，滚轮缩放；Focus 或 F 聚焦选中对象，Reset camera 恢复初始相机。Ctrl+Z/Ctrl+Y在UI未消费时进入场景历史。
调整窗口尺寸、DPI变化、失焦或外部捕获丢失都会取消当前未提交编辑。应用主动释放捕获产生的同步通知不会误取消刚进入的文本编辑。

## 帧与关停

应用在 renderer 的最终结果成功且确认 accepted 后才切换命中帧，skip 保留旧帧。
accepted extent 固定为录制/呈现时尺寸，frame末尾resize后的新尺寸仅用于下一帧。
尺寸/DPI过渡期取消press/capture并暂停指针命中，释放/失焦/捕获丢失继续处理；匹配帧接受后恢复。
上传也是录制；录制开始后的错误没有回滚，终态错误停止新交互与后续帧。普通帧不调用finish，诊断截图才显式同步。
关闭停止输入并取消编辑，释放renderer拥有的资源后关闭RHI，最后销毁窗口；成员/历史根先于Runtime析构。

## 验收入口

`editor_scene_acceptance`消费正式Scene/Session，覆盖跨场景根拒绝、别名拒绝、根释放、拖调历史、删除跨GC后身份恢复。
`atelier_editor --acceptance <输出目录>`运行同一正式窗口，用Win32消息和系统焦点/捕获操作驱动控件，再核对Runtime状态与GPU帧。
窗口链包含分隔、增删/历史、列表选择、实时拖调/一次undo/Esc、文字Delete、非法值、相机、失焦/捕获、resize、最小化和正常退出。
截图来自renderer实际颜色/深度readback，输出 `m0.png`、`editor.png` 与 `acceptance.txt`。
本轮用户预览另保存为release证据目录的 `preview.png`，是已验收 `editor.png` 的原像素副本。
DPI验收覆盖当前显示器上的WM_DPICHANGED过渡政策；不据此声称已验证跨不同DPI显示器迁移。完整IME文本编辑不属于本轮。
