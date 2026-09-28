# 首个场景编辑器

唯一产品入口为 `atelier_editor`；启动和构建命令见[根 README](../../../README.md)。
当前包含[首个编辑器方案](../../first-editor-plan.md)的 M0/M1 核心操作，以及[原生 UI 方案](../../ui-foundation-plan.md)的 S1：统一样式、真实字体度量失效与屏外内容剪除。
2026-09-28 在本地 S1 集成树的 dev/release 均通过 UI 8/8 与 `editor_acceptance`；每次窗口链接受597帧，Scene消费者与正常关闭检查通过。
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

## UI 样式与诊断

Workspace 只声明 `normal/panel/title/caption` 角色及 enabled/selected 等语义状态，样式值由唯一 [UI Style](../../../src/ui/style.hpp) 提供。工具条与参数标题共用 title 层级，说明和状态文字使用 caption；容器结构 padding 留在 NodeInput，按钮、列表项和数值文字的内容内距由 Style 提供。工具条按钮宽度按文字测量并保留原最小宽度，字号改变时不再靠固定宽度裁掉末字。
`Application::set_style` 直接进入 UI 的整体校验/应用路径，应用不保存另一份主题。颜色复用文字 Coverage，字体族、字重和字号经同一 TextStore 更新度量、caret 和栅格；已接受帧仍保有旧几何与不可变内容。具体样式、版本及状态合同见 [UI 模块](../ui/README.md)。
`Application::sample_frames` 显式开启 CPU prepare/render 计时，默认关闭；Capture 帧不计入。窗口验收从同一应用按钮添加真实场景成员至 256，再采样列表滚动与位置拖动。UI 累计计数按段求差，renderer 的 `FrameResult::stats` 按实际采样帧合计；CPU Coverage 和 renderer cache 的持有峰值分别记录。
render 计时包括 Present/等待，R8 上传字节只表示有效 payload，缓存 texel 字节不表示物理显存。验收使用硬件 D3D12 debug/GBV 与 vsync，含 release 配置；数据用于说明该验收负载，不能直接当作关闭验证的产品帧耗时，也没有同负载改前基线可用于百分比提升结论。

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
S1 覆盖正常/hover/pressed/focus/invalid/disabled、颜色改变的同Coverage/零raster/零上传、非法Style整体拒绝、字体family/weight/size及caret同步失效、旧帧几何/像素寿命。256真实成员由工具条加入，验证屏外控件几何仍在、文字不生成、滚回恢复，以及连续拖调历史。
数值焦点 Ctrl+Z 仍取消本地草稿；Esc 退出后同一 Win32 modifier 链驱动场景 undo/redo。最初新增验收把数字焦点内 Ctrl+Z 错当场景命令，现已分别验收两个上下文，生产输入政策未改。S2 的快捷键状态裁决留在当前方案。
截图来自 renderer 实际颜色/深度 readback，输出 `m0.png`、`editor.png`、`s1-pressed/focused/invalid/disabled/color/font/list-256.png` 与 `acceptance.txt`。release 的 `preview.png` 是当前 `editor.png` 的原像素副本；首版证据另保存在各配置的 `editor-evidence-v1/`。
DPI验收覆盖当前显示器上的WM_DPICHANGED过渡政策；不据此声称已验证跨不同DPI显示器迁移。完整IME文本编辑不属于本轮。

### S1 真实负载采样

以下是同一次 release 窗口验收在 AMD Radeon RX 6800 XT、debug/GBV 与 vsync 开启时的结果。原始数据为 `out/release/editor-evidence/s1-performance.csv` 和 `s1-resources.csv`；dev 目录保留相同格式。没有同负载改前数据，不声称性能提升比例。

| 256成员负载 | 样本帧 | prepare CPU 均值 / p95（ms） | render CPU 均值 / p95（ms） | 栅格次数 / R8字节 | 上传次数 / 有效R8字节 |
| --- | --- | --- | --- | --- | --- |
| 列表滚动 | 48 | 0.235 / 0.342 | 15.346 / 17.422 | 232 / 370890 | 490 / 780588 |
| 连续位置拖动 | 64 | 0.228 / 0.326 | 16.080 / 16.944 | 64 / 143427 | 64 / 143427 |

滚动/拖动分别累计 UI draw 4245/5632、场景 draw 48/64。采样时 UI CPU Coverage 持有峰值分别为277项/451706字节、277项/457032字节；renderer cache峰值分别为33项/65354 texel字节、32项/64488 texel字节。CPU像素统计不含DWrite对象等开销，cache统计不含gfx待退休资源或物理显存。
滚动中累计剪除11716个屏外文字控件生成，拖动中剪除15616个；滚回可能重新上传是现有帧末GPU缓存淘汰行为。真实业务参数仍只有XYZ，数据不代表高密度参数面板已经验证。
